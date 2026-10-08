#include "wp/audio.h"

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#elif defined(WP_SDL_PLATFORM)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "wp/deterministic.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/settings.h"

namespace wp::audio {

namespace {

constexpr uint32_t kControl = 0xCD006C00;
constexpr uint32_t kSampleCounter = 0xCD006C08;
constexpr uint32_t kPlaying = 0x01;
constexpr uint32_t kSampleRate48k = 0x02;
constexpr uint32_t kCounterReset = 0x20;
constexpr double kRate32k = 32000.0;
constexpr double kRate48k = 48000.0;
constexpr double kTargetLatency = 0.06;
constexpr double kMaxLatency = 0.2;
#ifdef _WIN32
constexpr REFERENCE_TIME kBufferDuration = 400000;
#endif
constexpr double kMaxRateCorrection = 0.005;
constexpr double kFadeSeconds = 0.004;
constexpr int kTaps = 16;
constexpr int kPhases = 512;
constexpr double kCutoff = 0.95;
constexpr double kPi = 3.14159265358979323846;

using Clock = std::chrono::steady_clock;

Clock::time_point g_last{};
double g_samples = 0.0;

struct Frame {
    float left;
    float right;
};

std::mutex g_queue_mutex;
std::deque<Frame> g_queue;
uint32_t g_source_rate = 32000;
bool muted() {
    static const settings::LiveFlag value("audio.mute", "WP_MUTE");
    return value();
}
std::FILE* g_dump = nullptr;
uint32_t g_dump_frames = 0;

void write_wav_header(std::FILE* file, uint32_t frames, uint32_t rate) {
    uint32_t data_bytes = frames * 4;
    uint32_t values[] = {0x46464952, 36 + data_bytes, 0x45564157, 0x20746D66, 16, 0x00020001, rate, rate * 4, 0x00100004, 0x61746164, data_bytes};
    std::fseek(file, 0, SEEK_SET);
    std::fwrite(values, 4, 11, file);
    std::fseek(file, 0, SEEK_END);
    std::fflush(file);
}

struct Kernel {
    float weights[kPhases + 1][kTaps];

    Kernel() {
        for (int phase = 0; phase <= kPhases; phase++) {
            double fraction = static_cast<double>(phase) / kPhases;
            double sum = 0.0;
            for (int tap = 0; tap < kTaps; tap++) {
                double x = tap - (kTaps / 2 - 1) - fraction;
                double sinc = x == 0.0 ? kCutoff : std::sin(kPi * kCutoff * x) / (kPi * x);
                double w = (x + kTaps / 2.0) / kTaps;
                double window = 0.42 - 0.5 * std::cos(2.0 * kPi * w) + 0.08 * std::cos(4.0 * kPi * w);
                weights[phase][tap] = static_cast<float>(sinc * window);
                sum += sinc * window;
            }
            for (int tap = 0; tap < kTaps; tap++) {
                weights[phase][tap] = static_cast<float>(weights[phase][tap] / sum);
            }
        }
    }
};

const Kernel& kernel() {
    static const Kernel instance;
    return instance;
}

class Mixer {
public:
    explicit Mixer(double device_rate) : device_rate_(device_rate), filter_(kernel()) {
        if (const char* path = std::getenv("WP_DUMP_OUTPUT")) {
            dump_ = std::fopen(path, "wb");
            if (dump_) {
                write_wav_header(dump_, 0, static_cast<uint32_t>(device_rate_));
            }
        }
    }

    void fill(Frame* mixed, uint32_t available) {
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            size_t limit = static_cast<size_t>(kMaxLatency * g_source_rate);
            size_t target = static_cast<size_t>(kTargetLatency * g_source_rate);
            lowest_ = std::min(lowest_, g_queue.size());
            highest_ = std::max(highest_, g_queue.size());
            if (g_queue.size() > limit) {
                g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(g_queue.size() - target));
                gain_ = 0.0f;
                trims_++;
            }
            if (!primed_ && g_queue.size() >= target) {
                primed_ = true;
            }
            double error = (static_cast<double>(g_queue.size()) - static_cast<double>(target)) / static_cast<double>(target);
            double correction = std::clamp(error * kMaxRateCorrection, -kMaxRateCorrection, kMaxRateCorrection);
            double step = g_source_rate / device_rate_ * (1.0 + correction);
            float fade = static_cast<float>(1.0 / (kFadeSeconds * device_rate_));
            for (uint32_t i = 0; i < available; i++) {
                if (primed_) {
                    position_ += step;
                    while (position_ >= 1.0) {
                        position_ -= 1.0;
                        std::copy(history_ + 1, history_ + kTaps, history_);
                        if (!g_queue.empty()) {
                            history_[kTaps - 1] = g_queue.front();
                            g_queue.pop_front();
                        } else {
                            missing_++;
                            primed_ = false;
                        }
                    }
                }
                gain_ = primed_ ? std::min(1.0f, gain_ + fade) : std::max(0.0f, gain_ - fade);
                const float* weights = filter_.weights[static_cast<int>(position_ * kPhases)];
                Frame sample{0.0f, 0.0f};
                for (int tap = 0; tap < kTaps; tap++) {
                    sample.left += history_[tap].left * weights[tap];
                    sample.right += history_[tap].right * weights[tap];
                }
                mixed[i] = {sample.left * gain_, sample.right * gain_};
            }
        }
        if (dump_) {
            std::vector<int16_t> samples(available * 2);
            for (uint32_t i = 0; i < available; i++) {
                samples[2 * i] = static_cast<int16_t>(std::lround(std::clamp(mixed[i].left, -1.0f, 1.0f) * 32767.0f));
                samples[2 * i + 1] = static_cast<int16_t>(std::lround(std::clamp(mixed[i].right, -1.0f, 1.0f) * 32767.0f));
            }
            std::fwrite(samples.data(), 4, available, dump_);
            dump_frames_ += available;
            write_wav_header(dump_, dump_frames_, static_cast<uint32_t>(device_rate_));
        }
        if (muted()) {
            std::fill(mixed, mixed + available, Frame{0.0f, 0.0f});
        }
        Clock::time_point now = Clock::now();
        if (now - report_ >= std::chrono::seconds(1)) {
            log::write("output", "device %.0f Hz, queue %.1f-%.1f ms, %u frames missing, %u trims", device_rate_, lowest_ * 1000.0 / g_source_rate,
                       highest_ * 1000.0 / g_source_rate, missing_, trims_);
            missing_ = 0;
            trims_ = 0;
            lowest_ = SIZE_MAX;
            highest_ = 0;
            report_ = now;
        }
    }

private:
    double device_rate_;
    const Kernel& filter_;
    double position_ = 0.0;
    uint32_t missing_ = 0;
    uint32_t trims_ = 0;
    size_t lowest_ = SIZE_MAX;
    size_t highest_ = 0;
    Clock::time_point report_ = Clock::now();
    std::FILE* dump_ = nullptr;
    uint32_t dump_frames_ = 0;
    bool primed_ = false;
    float gain_ = 0.0f;
    Frame history_[kTaps] = {};
};

#ifdef _WIN32
template <typename T>
void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

bool is_float(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        static const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        return IsEqualGUID(extensible->SubFormat, kFloat) != 0;
    }
    return false;
}

void output_thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* format = nullptr;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    static const GUID kEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    static const GUID kEnumeratorInterface = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    static const GUID kClientInterface = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
    static const GUID kRenderInterface = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
    bool ok = SUCCEEDED(CoCreateInstance(kEnumerator, nullptr, CLSCTX_ALL, kEnumeratorInterface, reinterpret_cast<void**>(&enumerator))) &&
              SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
              SUCCEEDED(device->Activate(kClientInterface, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
              SUCCEEDED(client->GetMixFormat(&format)) &&
              SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0, format, nullptr)) &&
              SUCCEEDED(client->SetEventHandle(event)) &&
              SUCCEEDED(client->GetService(kRenderInterface, reinterpret_cast<void**>(&render)));
    UINT32 buffer_frames = 0;
    ok = ok && SUCCEEDED(client->GetBufferSize(&buffer_frames)) && SUCCEEDED(client->Start());
    if (!ok) {
        std::fprintf(stderr, "audio output could not start\n");
        release(render);
        release(client);
        release(device);
        release(enumerator);
        return;
    }
    const bool floats = is_float(format);
    const uint32_t channels = format->nChannels;
    Mixer mixer(format->nSamplesPerSec);
    while (true) {
        WaitForSingleObject(event, 100);
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            continue;
        }
        UINT32 available = buffer_frames - padding;
        if (available == 0) {
            continue;
        }
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(available, &data))) {
            continue;
        }
        std::vector<Frame> mixed(available);
        mixer.fill(mixed.data(), available);
        for (UINT32 i = 0; i < available; i++) {
            float left = mixed[i].left;
            float right = mixed[i].right;
            for (uint32_t channel = 0; channel < channels; channel++) {
                float value = channel == 0 ? left : channel == 1 ? right : 0.0f;
                if (floats) {
                    reinterpret_cast<float*>(data)[i * channels + channel] = value;
                } else {
                    reinterpret_cast<int16_t*>(data)[i * channels + channel] = static_cast<int16_t>(std::lround(value * 32767.0f));
                }
            }
        }
        render->ReleaseBuffer(available, 0);
    }
}
#elif defined(WP_SDL_PLATFORM)
void SDLCALL feed(void* data, SDL_AudioStream* stream, int additional, int) {
    Mixer& mixer = *static_cast<Mixer*>(data);
    uint32_t frames = static_cast<uint32_t>(additional) / sizeof(Frame);
    if (frames == 0) {
        return;
    }
    std::vector<Frame> mixed(frames);
    mixer.fill(mixed.data(), frames);
    SDL_PutAudioStreamData(stream, mixed.data(), static_cast<int>(frames * sizeof(Frame)));
}

void output_thread() {
    constexpr int kDeviceRate = 48000;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio output could not start: %s\n", SDL_GetError());
        return;
    }
    static Mixer mixer(kDeviceRate);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kDeviceRate};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, &mixer);
    if (!stream || !SDL_ResumeAudioStreamDevice(stream)) {
        std::fprintf(stderr, "audio output could not start: %s\n", SDL_GetError());
        return;
    }
    log::write("output", "SDL audio driver %s", SDL_GetCurrentAudioDriver());
}
#else
void output_thread() {
    Clock::time_point last = Clock::now();
    double owed = 0.0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Clock::time_point now = Clock::now();
        owed += std::chrono::duration<double>(now - last).count();
        last = now;
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        size_t take = static_cast<size_t>(owed * g_source_rate);
        owed -= static_cast<double>(take) / g_source_rate;
        size_t limit = static_cast<size_t>(kMaxLatency * g_source_rate);
        if (g_queue.size() > limit) {
            take = std::max(take, g_queue.size() - limit);
        }
        take = std::min(take, g_queue.size());
        g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(take));
    }
}
#endif

}

void update() {
    Clock::time_point now = det::now();
    if (g_last == Clock::time_point{}) {
        g_last = now;
    }
    double elapsed = std::chrono::duration<double>(now - g_last).count();
    g_last = now;
    uint32_t control = rd32(kControl);
    if (control & kCounterReset) {
        g_samples = 0.0;
        wr32(kControl, control & ~kCounterReset);
        control &= ~kCounterReset;
    }
    if (control & kPlaying) {
        g_samples += elapsed * ((control & kSampleRate48k) ? kRate48k : kRate32k);
    }
    wr32(kSampleCounter, static_cast<uint32_t>(g_samples) & 0x7FFFFFFF);
}

void start_output() {
    std::thread(output_thread).detach();
}

void push(const int16_t* frames, size_t count, uint32_t rate) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_source_rate = rate;
    static const char* dump_path = std::getenv("WP_DUMP_AUDIO");
    if (dump_path && !g_dump) {
        g_dump = std::fopen(dump_path, "wb");
        if (g_dump) {
            write_wav_header(g_dump, 0, rate);
        }
    }
    if (g_dump) {
        std::fwrite(frames, 4, count, g_dump);
        g_dump_frames += static_cast<uint32_t>(count);
        if (g_dump_frames % 16000 < count) {
            write_wav_header(g_dump, g_dump_frames, rate);
        }
    }
    for (size_t i = 0; i < count; i++) {
        g_queue.push_back({frames[2 * i] / 32768.0f, frames[2 * i + 1] / 32768.0f});
    }
}

}
