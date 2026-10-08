// Player motion: turns the Wii Remote and Nunchuk accelerometers into the poses the walls check.
//
// Lifted from fn_80041800 (called once per frame for each player from 0x800417AC). The two
// controllers are the two arms. Each frame:
//   1. the controller-state history shifts by one entry;
//   2. both accelerometers are scaled by the WPAD gravity calibration, clamped and normalized;
//   3. each arm's vertical position becomes a "tilt" value (half the normalized z component,
//      unwrapped to [-1, 1] when the controller faces away) and is classified as up, level or
//      down; its speed (tilt change per 1/60 s) and peak speed are tracked;
//   4. a pose is the pair (Remote up/down, Nunchuk up/down); it is accepted once both arms are
//      still for a short hold time and stored in pose(), which the wall check reads;
//   5. a confidence meter per pose rises while that pose is held and decays otherwise;
//   6. each controller's sideways tilt (-256 * y) drives a "tilted" flag with hysteresis.
//
// The float arithmetic mirrors the original instruction by instruction (lift::ppc helpers), and
// WP_VERIFY=1 compares this version with the translated one.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game_features.h"
#include "input/player_input.h"
#include "wp/lift.h"
#include "wp/threads.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

// Guest functions used here.
constexpr uint32_t kWPADProbe = 0x801DCB70;
constexpr uint32_t kWPADGetAccGravityUnit = 0x801DC930;
constexpr uint32_t kVECNormalize = 0x8021858C;           // (dst, src)
constexpr uint32_t kVECDot = 0x80218B44;                 // returns f1

constexpr uint32_t kDeviceNotFound = 0xFD;               // WPADProbe type: no controller
constexpr uint32_t kExtensionNunchuk = 2;

// Tilt classes.
constexpr uint32_t kLevel = 0;
constexpr uint32_t kUp = 1;
constexpr uint32_t kDown = 2;

// Poses (what the walls ask for, minus 3): 0 both up, 1 both down, 2 Remote up + Nunchuk down,
// 3 Remote down + Nunchuk up; 4 = no pose (an arm is level).
constexpr uint32_t kNoPose = 4;

// Tuning data in the game's .data/.sdata.
constexpr uint32_t kTiltThresholds = 0x8027EA98;  // {remote up, remote down, nunchuk up, nunchuk down}
constexpr uint32_t kUpAxis = 0x80270870;          // (0, 0, 1)
constexpr uint32_t kDefaultRemoteGravity = 0x804237B0;
constexpr uint32_t kDefaultNunchukGravity = 0x804237B8;

constexpr float kRemoteLimit = 3.4f;     // g
constexpr float kNunchukLimit = 2.1f;
constexpr float kFrameSeconds = 0.01666666753590107f;
constexpr float kMovingSpeed = 0.1f;     // tilt per frame above which an arm counts as moving
constexpr float kSwingPeak = 0.3f;       // a faster swing accepts the pose at once
constexpr float kMeterRate = 0.1f;       // meter change per frame

// The controller-state history: entries of 0x44 bytes at the object, newest first. Shifting moves
// the sampled fields (not the per-object ones at +0x0/+0x4/+0x10/+0x24/+0x26) one entry back.
void shift_history(uint32_t object) {
    for (int entry = 31; entry >= 0; entry--) {
        uint32_t from = object + 0x44 * static_cast<uint32_t>(entry);
        uint32_t to = from + 0x44;
        for (uint32_t offset : {0x0Cu, 0x08u, 0x14u, 0x18u, 0x1Cu, 0x20u}) {
            wr32(to + offset, rd32(from + offset));
        }
        for (uint32_t offset = 0x28; offset <= 0x42; offset += 2) {
            wr16(to + offset, rd16(from + offset));
        }
    }
}

// The original's stack frame (0xF0 bytes): its locals, at the same addresses, so everything the
// helpers read or leave behind matches.
struct Frame {
    uint32_t base;
    uint32_t remote_gravity() const { return base + 0x1C; }   // s16 x, y, z
    uint32_t nunchuk_gravity() const { return base + 0x14; }
    uint32_t probe_type() const { return base + 0x10; }
    uint32_t remote_scale() const { return base + 0x54; }     // float x, y, z
    uint32_t nunchuk_scale() const { return base + 0x48; }
    uint32_t remote_accel() const { return base + 0x6C; }     // float x, y, z (g)
    uint32_t nunchuk_accel() const { return base + 0x60; }
    uint32_t up_remote() const { return base + 0x3C; }
    uint32_t up_nunchuk() const { return base + 0x30; }
    uint32_t up_copy() const { return base + 0x24; }
};

// The int -> float conversions go through two stack slot pairs, {0x78, 0x7C} and {0x80, 0x84}
// (high words 0x43300000), alternating in a fixed pattern; later code can read what is left there.
constexpr uint32_t kSlotA = 0x7C;
constexpr uint32_t kSlotB = 0x84;

void slot_int(uint32_t frame, uint32_t slot, int32_t value) {
    wr32(frame + slot, static_cast<uint32_t>(value) ^ 0x80000000u);
}

// 1 / gravity unit per axis, or `fallback` when the calibration is missing. The conversions use
// `first` for x and y and `second` for z.
void gravity_scale(uint32_t gravity, uint32_t scale, float fallback, uint32_t frame, uint32_t first, uint32_t second) {
    int32_t x = static_cast<int16_t>(rd16(gravity + 0));
    int32_t y = static_cast<int16_t>(rd16(gravity + 2));
    int32_t z = static_cast<int16_t>(rd16(gravity + 4));
    if (static_cast<uint32_t>(z) * static_cast<uint32_t>(x) * static_cast<uint32_t>(y) == 0) {
        stf(scale + 0, fallback);
        stf(scale + 4, fallback);
        stf(scale + 8, fallback);
        return;
    }
    slot_int(frame, first, x);
    slot_int(frame, second, z);
    slot_int(frame, first, y);
    stf(scale + 0, fdivs(1.0f, from_int(x)));
    stf(scale + 4, fdivs(1.0f, from_int(y)));
    stf(scale + 8, fdivs(1.0f, from_int(z)));
}

// Each comparison converts the raw value again, alternating slots `first`, `second`, `first`.
float clamped(int16_t raw, float scale, float limit, uint32_t frame, uint32_t first, uint32_t second) {
    float value = fmuls(from_int(raw), scale);
    slot_int(frame, first, raw);
    if (!(limit >= value)) {
        return limit;
    }
    slot_int(frame, second, raw);
    if (!(value >= -limit)) {
        return -limit;
    }
    slot_int(frame, first, raw);
    return value;
}

// Tilt in [-1, 1] from the normalized acceleration: half the z component, unwrapped by the sign
// of y (after the axis swap below).
float tilt_of(Cpu& c, uint32_t accel, uint32_t up, uint32_t return_address) {
    gcall_from(c, return_address, kVECDot, {accel, up});
    float tilt = fmuls(static_cast<float>(c.f[1]), 0.5f);
    if (ldf(accel + 4) > 0.0f) {
        tilt = tilt > 0.0f ? fsubs(1.0f, tilt) : fsubs(-1.0f, tilt);
    }
    return tilt;
}

uint32_t classify(float tilt, float up, float down) {
    if (tilt > up) {
        return kUp;
    }
    if (tilt > down) {
        return kLevel;
    }
    return kDown;
}

// Speed and moving flag of one arm; updates its previous tilt and peak speed.
struct Arm {
    lift::Field<float> tilt, speed, before, peak;
    lift::Field<uint8_t> moving;
};

void track(const Arm& arm, float seconds, uint32_t scratch) {
    float speed = fdivs(fmuls(seconds, fsubs(arm.tilt, arm.before)), kFrameSeconds);
    arm.speed = speed;
    float magnitude = static_cast<float>(std::fabs(speed));
    stf(scratch, magnitude);
    if (!(arm.peak >= magnitude)) {
        arm.peak = magnitude;
    } else {
        arm.peak = arm.peak.get();
    }
    arm.before = arm.tilt.get();
    arm.moving = static_cast<float>(std::fabs(arm.speed.get())) > kMovingSpeed ? 1 : 0;
}

uint32_t pose_of(uint32_t remote, uint32_t nunchuk) {
    if (remote == kLevel || nunchuk == kLevel) {
        return kNoPose;
    }
    if (remote == kUp && nunchuk == kUp) {
        return 0;
    }
    if (remote == kUp && nunchuk == kDown) {
        return 2;
    }
    if (remote == kDown && nunchuk == kUp) {
        return 3;
    }
    return 1;
}

// Sideways tilt flag with hysteresis: on beyond +-170, off inside +-160.
void side_tilt(lift::Field<uint32_t> flag, float tilt) {
    if (tilt > 170.0f || !(tilt >= -170.0f)) {
        flag = 1;
    } else if (tilt < 160.0f && tilt > -160.0f) {
        flag = 0;
    }
}

}  // namespace

WP_LIFTED(kLifted, update_motion, "PlayerInput_UpdateMotion", Ret::None) {
    PlayerInput player;
    player.base = c.r[3];
    static bool printed = false;
    if (!printed && std::getenv("WP_MOTION_DEBUG")) {
        printed = true;
        std::fprintf(stderr, "motion object %08x\n", player.base);
    }
    OriginalFrame original(c, 0xF0);
    Frame frame{original.at(0)};
    original.save_fpr(31, 0xE0, 0xE8);
    original.save_fpr(30, 0xD0, 0xD8);
    original.save_fpr(29, 0xC0, 0xC8);
    original.save_fpr(28, 0xB0, 0xB8);
    original.save_fpr(27, 0xA0, 0xA8);
    original.save_gpr(31, 0x9C);
    original.save_gpr(30, 0x98);
    wr32(frame.base + 0x78, 0x43300000);
    wr32(frame.base + 0x80, 0x43300000);

    shift_history(player.base);

    float remote_side = 0.0f;   // -256 * remote y, before the axis swap
    float nunchuk_side = 0.0f;
    gcall_from(c, 0x800419B4, kWPADProbe, {player.channel(), frame.probe_type()});
    if (rd32(frame.probe_type()) != kDeviceNotFound) {
        wr32(frame.remote_gravity(), rd32(kDefaultRemoteGravity));
        wr16(frame.remote_gravity() + 4, rd16(kDefaultRemoteGravity + 4));
        wr32(frame.nunchuk_gravity(), rd32(kDefaultNunchukGravity));
        wr16(frame.nunchuk_gravity() + 4, rd16(kDefaultNunchukGravity + 4));
        if (player.read_state() == 0) {
            gcall_from(c, 0x800419FC, kWPADGetAccGravityUnit, {player.channel(), 0, frame.remote_gravity()});
            gravity_scale(frame.remote_gravity(), frame.remote_scale(), 0.01f, frame.base, kSlotA, kSlotB);
            gcall_from(c, 0x80041A88, kWPADGetAccGravityUnit, {player.channel(), 1, frame.nunchuk_gravity()});
            gravity_scale(frame.nunchuk_gravity(), frame.nunchuk_scale(), 0.005f, frame.base, kSlotB, kSlotA);
            uint32_t status = player.status();
            for (uint32_t axis = 0; axis < 3; axis++) {
                int16_t raw = static_cast<int16_t>(rd16(status + 0x2 + 2 * axis));
                stf(frame.remote_accel() + 4 * axis, clamped(raw, ldf(frame.remote_scale() + 4 * axis), kRemoteLimit, frame.base,
                                                                axis == 1 ? kSlotB : kSlotA, axis == 1 ? kSlotA : kSlotB));
            }
            if (player.extension() == kExtensionNunchuk) {
                for (uint32_t axis = 0; axis < 3; axis++) {
                    int16_t raw = static_cast<int16_t>(rd16(status + 0x2A + 2 * axis));
                    stf(frame.nunchuk_accel() + 4 * axis, clamped(raw, ldf(frame.nunchuk_scale() + 4 * axis), kNunchukLimit, frame.base,
                                                                     axis == 1 ? kSlotA : kSlotB, axis == 1 ? kSlotB : kSlotA));
                }
            } else {
                for (uint32_t axis = 0; axis < 3; axis++) {
                    stf(frame.nunchuk_accel() + 4 * axis, 0.0f);
                }
            }
        }
        gcall_from(c, 0x80041D98, kVECNormalize, {frame.remote_accel(), frame.remote_accel()});
        gcall_from(c, 0x80041DA4, kVECNormalize, {frame.nunchuk_accel(), frame.nunchuk_accel()});
        remote_side = fmuls(256.0f, -ldf(frame.remote_accel() + 4));
        nunchuk_side = fmuls(256.0f, -ldf(frame.nunchuk_accel() + 4));
        int32_t remote_value = original.float_to_int(0x88, remote_side);
        int32_t nunchuk_value = original.float_to_int(0x90, nunchuk_side);
        player.remote_side_tilt() = static_cast<int16_t>(remote_value);
        player.nunchuk_side_tilt() = static_cast<int16_t>(nunchuk_value);
    }
    player.pose_changed() = 0;

    // Swap y and z (negated): the game measures the arm against "up" = +z.
    for (uint32_t accel : {frame.remote_accel(), frame.nunchuk_accel()}) {
        float y = ldf(accel + 4);
        float z = ldf(accel + 8);
        stf(accel + 8, fmuls(y, -1.0f));
        stf(accel + 4, fmuls(z, -1.0f));
    }

    // Elapsed time since the last update, in whole milliseconds, as seconds.
    static const uint32_t os_get_tick = symbol_address("OSGetTick");
    uint32_t now = gcall_from(c, 0x80041E20, os_get_tick);
    uint32_t last = player.last_tick();
    uint32_t elapsed = now >= last ? now - last : (now - 1) - last;
    player.last_tick() = now;
    uint32_t ticks_per_ms = static_cast<uint32_t>((static_cast<uint64_t>(0x10624DD3) * (rd32(0x800000F8) >> 2)) >> 32) >> 6;
    uint32_t milliseconds = elapsed / ticks_per_ms;
    wr32(frame.base + 0x7C, milliseconds);
    float seconds = fdivs(from_unsigned(milliseconds), 1000.0f);

    for (uint32_t i = 0; i < 3; i++) {
        wr32(frame.up_copy() + 4 * i, rd32(kUpAxis + 4 * i));
        wr32(frame.up_remote() + 4 * i, rd32(kUpAxis + 4 * i));
    }

    // Remote arm.
    player.remote_tilt() = tilt_of(c, frame.remote_accel(), frame.up_remote(), 0x80041EC4);
    player.remote_class() = classify(player.remote_tilt(), ldf(kTiltThresholds + 0), ldf(kTiltThresholds + 4));
    track({player.remote_tilt(), player.remote_speed(), player.remote_tilt_before(), player.remote_peak_speed(), player.remote_moving()}, seconds,
          frame.base + 0xC);

    // Nunchuk arm.
    for (uint32_t i = 0; i < 3; i++) {
        wr32(frame.up_nunchuk() + 4 * i, rd32(frame.up_copy() + 4 * i));
    }
    player.nunchuk_tilt() = tilt_of(c, frame.nunchuk_accel(), frame.up_nunchuk(), 0x80041FD4);
    player.nunchuk_class() = classify(player.nunchuk_tilt(), ldf(kTiltThresholds + 8), ldf(kTiltThresholds + 12));
    track({player.nunchuk_tilt(), player.nunchuk_speed(), player.nunchuk_tilt_before(), player.nunchuk_peak_speed(), player.nunchuk_moving()},
          seconds, frame.base + 0x8);

    // Pose: accepted when both arms are still and either a fast swing just ended or the pose was
    // held long enough (2 frames, or 10 for "no pose").
    uint32_t remote_class = player.remote_class();
    uint32_t candidate = pose_of(remote_class, player.nunchuk_class());
    float hold = fmuls((remote_class == kLevel || player.nunchuk_class() == kLevel) ? 10.0f : 2.0f, kFrameSeconds);
    int32_t accepted = -1;
    if (player.remote_moving() == 0 && player.nunchuk_moving() == 0) {
        if (player.remote_peak_speed() > kSwingPeak || player.nunchuk_peak_speed() > kSwingPeak) {
            player.hold_time() = hold;
            accepted = static_cast<int32_t>(candidate);
        } else {
            float held = fadds(player.hold_time(), seconds);
            player.hold_time() = held;
            if (held >= hold) {
                player.hold_time() = hold;
                accepted = static_cast<int32_t>(candidate);
            }
        }
    } else {
        player.hold_time() = 0.0f;
    }
    // Feature dual_stick_poses: the pose the sticks ask for counts at once, without the hold or the
    // meters settling (and without the accelerometers, which shaking wobbles).
    bool instant = features::instant_poses();
    if (instant) {
        int remote = 0;
        int nunchuk = 0;
        features::stick_arms(remote, nunchuk);
        auto arm = [](int value) { return value > 0 ? kUp : value < 0 ? kDown : kLevel; };
        accepted = static_cast<int32_t>(pose_of(arm(remote), arm(nunchuk)));
    }
    if ((player.meters_moving() == 0 || instant) && accepted != -1 && player.pose() != static_cast<uint32_t>(accepted)) {
        player.pose() = static_cast<uint32_t>(accepted);
        player.pose_changed() = 1;
        static const bool debug = std::getenv("WP_MOTION_DEBUG") != nullptr;
        if (debug) {
            static const char* names[] = {"both up", "both down", "Remote up + Nunchuk down", "Remote down + Nunchuk up", "none"};
            std::fprintf(stderr, "pose: %s\n", names[accepted < 5 ? accepted : 4]);
        }
    }

    // Pose meters: the current pose fills, the others drain; any meter in between holds changes.
    // The meters also play the arms' pose animations (race/race_update.cpp): with instant poses
    // (feature dual_stick_poses) they jump, so the arms go straight into the pose and out of it.
    player.meters_moving() = 0;
    for (uint32_t pose = 0; pose < 4; pose++) {
        lift::Field<float> meter = player.meter(pose);
        float step = fdivs(seconds, kFrameSeconds);
        if (pose == player.pose()) {
            meter = fmadds(kMeterRate, step, meter);
        } else {
            meter = fnmsubs(kMeterRate, step, meter);
        }
        float value = meter;
        if (!(0.0f >= value) && !(value >= 1.0f)) {
            player.meters_moving() = 1;
        }
        float limited = (1.0f >= value) ? value : 1.0f;
        if (!(limited >= 0.0f)) {
            limited = 0.0f;
        }
        meter = limited;
        if (instant) {
            meter = pose == player.pose() ? 1.0f : 0.0f;
            player.meters_moving() = 0;
        }
    }
    if (player.remote_moving() == 0) {
        player.remote_peak_speed() = 0.0f;
    }
    if (player.nunchuk_moving() == 0) {
        player.nunchuk_peak_speed() = 0.0f;
    }

    player.remote_tilted_before() = player.remote_tilted().get();
    player.nunchuk_tilted_before() = player.nunchuk_tilted().get();
    side_tilt(player.remote_tilted(), remote_side);
    side_tilt(player.nunchuk_tilted(), nunchuk_side);

    original.restore_fpr(31, 0xE0, 0xE8);
    original.restore_fpr(30, 0xD0, 0xD8);
    original.restore_fpr(29, 0xC0, 0xC8);
    original.restore_fpr(28, 0xB0, 0xB8);
    original.restore_fpr(27, 0xA0, 0xA8);
    original.restore_gpr(31, 0x9C);
    original.restore_gpr(30, 0x98);
}

}  // namespace musclemarch
