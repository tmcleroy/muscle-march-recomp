// Course layout: which segment of the course a distance falls in.
//
// A course is a list of segments (walls, dash zones, ...), each with a type whose length is in a
// table. The tail of a course loops: once the runner passes `loop_end`, the stretch from
// `loop_begin` to `loop_end` repeats, so distances past the end map back into it.
//   fn_8003C204: the lap number in the loop for a distance (0 before the loop end).
//   fn_8003BA48: the segment index at a distance (the wall check asks it which wall is next).
//   fn_8003C280: whether a step forward enters another segment (the wall check runs then).
// The course object lives in the race object at +0x59CF0.

#include "wp/lift.h"
#include "wp/threads.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kReportStrings = 0x802707A0;  // debug message and file name
constexpr uint32_t kReportLine = 0x4F3;

struct Course : lift::View {
    lift::Field<int32_t> segment_type(uint32_t segment) const { return lift::Field<int32_t>(base + 0xED4 + 4 * segment); }
    WP_FIELD(int32_t, segment_count, 0x11A8)
    WP_FIELD(int32_t, loop_end, 0x11C0)
    lift::Field<float> type_length(int32_t type) const { return lift::Field<float>(base + 0x21D4 + 4 * static_cast<uint32_t>(type)); }
    WP_FIELD(float, loop_begin, 0xA2E0)
};

// (Course*, f1 distance) -> lap
WP_LIFTED(kLifted, course_lap, "Course_LapAt", Ret::Int) {
    Course course;
    course.base = c.r[3];
    double distance = c.f[1];
    OriginalFrame frame(c, 0x20, OriginalFrame::kLeaf);

    float loop_length = fsubs(frame.int_to_float(0x8, course.loop_end()), course.loop_begin());
    int32_t lap = 0;
    if (!(loop_length <= 0.0f)) {
        float past_end = static_cast<float>(distance - frame.int_to_float(0x8, course.loop_end()));
        if (!(past_end <= 0.0f)) {
            lap = frame.float_to_int(0x10, fadds(1.0f, fdivs(past_end, loop_length)));
        }
    }
    c.r[3] = static_cast<uint32_t>(lap);
}

// The segment lookup, as the original inlines it: the compiler's int <-> float conversions go
// through two stack slot pairs (`main` and `other`) and the lap through `lap_slot`; which slot is
// which differs between the inlined copies, and the bytes left there must match.
struct LookupSlots {
    uint32_t main;
    uint32_t other;
};
constexpr uint32_t kLapSlot = 0x18;

int32_t segment_at(Cpu& c, Course course, double distance, OriginalFrame& frame, LookupSlots slots, int counter_reg, uint32_t report_return) {
    // Past the loop end, start counting from where this lap of the loop begins.
    float start = 0.0f;
    float loop_end = frame.int_to_float(slots.main, course.loop_end());
    wr32(frame.at(slots.other), 0x43300000);
    if (!(distance <= loop_end)) {
        float loop_begin = course.loop_begin();
        float loop_length = fsubs(frame.int_to_float(slots.main, course.loop_end()), loop_begin);
        float end = frame.int_to_float(slots.other, course.loop_end());
        float span = fsubs(end, loop_begin);
        int32_t lap = 0;
        if (!(loop_length <= 0.0f)) {
            float past_end = static_cast<float>(distance - frame.int_to_float(slots.other, course.loop_end()));
            if (!(past_end <= 0.0f)) {
                lap = frame.float_to_int(kLapSlot, fadds(1.0f, fdivs(past_end, loop_length)));
            }
        }
        float laps_before = frame.int_to_float(slots.main, lap - 1);
        start = fadds(0.0f, fadds(-course.loop_begin(), fmadds(span, laps_before, end)));
    }

    int32_t segment = 0;
    while (true) {
        float length = course.type_length(course.segment_type(static_cast<uint32_t>(segment)));
        float segment_end = fadds(length, start);
        if (distance < segment_end || distance == segment_end) {
            break;
        }
        start = fadds(start, length);
        segment++;
    }

    c.r[counter_reg] = static_cast<uint32_t>(segment);  // the original counts in this register
    if (segment > course.segment_count()) {
        static const uint32_t os_report = symbol_address("OSReport");
        cr_set(c, 6, false);  // crclr cr1eq: no float arguments
        gcall_from(c, report_return, os_report, {kReportStrings + 0x4F, kReportStrings + 0x3D, kReportLine});
    }
    return segment;
}

// (Course*, f1 distance) -> segment index
WP_LIFTED(kLifted, course_segment, "Course_SegmentAt", Ret::Int) {
    Course course;
    course.base = c.r[3];
    double distance = c.f[1];
    OriginalFrame frame(c, 0x30);
    frame.save_gpr(31, 0x2C);
    int32_t segment = segment_at(c, course, distance, frame, {0x8, 0x10}, 31, 0x8003BB7C);
    frame.restore_gpr(31, 0x2C);
    c.r[3] = static_cast<uint32_t>(segment);
}

// (Course*, f1 distance, f2 step) -> 1 when moving by `step` enters another segment
WP_LIFTED(kLifted, course_crosses_segment, "Course_CrossesSegment", Ret::Int) {
    Course course;
    course.base = c.r[3];
    double distance = c.f[1];
    double step = c.f[2];
    OriginalFrame frame(c, 0x50);
    frame.save_fpr(31, 0x40, 0x48);
    c.f[31] = step;
    frame.save_fpr(30, 0x30, 0x38);
    c.f[30] = distance;
    frame.save_gpr(31, 0x2C);
    frame.save_gpr(30, 0x28);
    c.r[30] = course.base;
    frame.save_gpr(29, 0x24);

    int32_t here = segment_at(c, course, distance, frame, {0x8, 0x10}, 31, 0x8003C3D8);
    int32_t there = segment_at(c, course, fadds(static_cast<float>(distance), static_cast<float>(step)), frame, {0x10, 0x8}, 29, 0x8003C4F8);

    frame.restore_fpr(31, 0x40, 0x48);
    frame.restore_fpr(30, 0x30, 0x38);
    frame.restore_gpr(31, 0x2C);
    frame.restore_gpr(30, 0x28);
    frame.restore_gpr(29, 0x24);
    c.r[3] = here != there ? 1 : 0;
}

}  // namespace

}  // namespace musclemarch
