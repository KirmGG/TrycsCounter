#include "../src/WindowMath.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    using namespace tc;

    // GD 2.2081 m_tickIndex uses two raw units per 240 Hz physics frame.
    assert(framesToTicks(1, 480, 240) == 2);
    assert(framesToTicks(1, 480, 60) == 8);
    assert(ticksToFramesCeil(8, 480, 60) == 1);
    assert(ticksToFramesCeil(9, 480, 60) == 2);

    // 16 displayed frames = 32 raw ticks of window width.
    assert(maxTicksForFrames(16, 480, 240) == 32);

    // Window widths: a step is one frame (2 raw) normally, half a frame (1 raw) with CBS.
    WindowResult whole;
    whole.earlyTicks = 4; whole.lateTicks = 2; whole.valid = true;  // step 2
    assert(whole.widthTicks() == 8);
    assert(std::abs(windowFrames(whole.widthTicks(), 480, 240) - 4.0) < 1e-9);
    assert(!isPrecise(whole.stepTicks, 480, 240));

    WindowResult half;
    half.stepTicks = 1; half.earlyTicks = 1; half.lateTicks = 2; half.valid = true;
    assert(half.widthTicks() == 4);
    assert(std::abs(windowFrames(half.widthTicks(), 480, 240) - 2.0) < 1e-9);
    assert(isPrecise(half.stepTicks, 480, 240));

    WindowResult tiny;
    tiny.stepTicks = 1; tiny.valid = true;   // only the original half step
    assert(std::abs(windowFrames(tiny.widthTicks(), 480, 240) - 0.5) < 1e-9);
    assert(bucketIndex(0.5, true) == 0);

    // Whole-frame buckets: 1, 2, 3, 4, 5-6, 7-8, <=max.
    assert(bucketIndex(1.0, false) == 0);
    assert(bucketIndex(2.0, false) == 1);
    assert(bucketIndex(4.0, false) == 3);
    assert(bucketIndex(5.0, false) == 4);
    assert(bucketIndex(6.0, false) == 4);
    assert(bucketIndex(8.0, false) == 5);
    assert(bucketIndex(9.0, false) == 6);
    assert(bucketIndex(12.0, false) == 6);
    assert(bucketLabels(12, false)[6] == "<=12");
    assert(bucketCount(false) == 7);

    // Precise buckets are ranges, NaN GD style: 0-1 ... 5-6, 6-8, <10, <=max.
    assert(bucketIndex(0.5, true) == 0);
    assert(bucketIndex(1.0, true) == 1);
    assert(bucketIndex(1.48, true) == 1);
    assert(bucketIndex(1.99, true) == 1);
    assert(bucketIndex(2.5, true) == 2);
    assert(bucketIndex(4.5, true) == 4);
    assert(bucketIndex(5.0, true) == 5);
    assert(bucketIndex(7.99, true) == 6);
    assert(bucketIndex(9.0, true) == 7);
    assert(bucketIndex(11.0, true) == 8);
    assert(bucketLabels(12, true)[0] == "0-1");
    assert(bucketLabels(12, true)[1] == "1-2");
    assert(bucketLabels(12, true)[7] == "<10");
    assert(bucketLabels(12, true)[8] == "<=12");

    // Labels: precise windows always show two decimals.
    assert(frameLabel(6.0, false, false) == "6");
    assert(frameLabel(6.0, false, true) == "6+");
    assert(frameLabel(2.5, true, false) == "2.50");
    assert(frameLabel(1.48, true, false) == "1.48");
    assert(frameLabel(0.25, true, true) == "0.25+");
    assert(clickMarks(false, 0).empty());
    assert(clickMarks(true, 0) == "~");
    assert(clickMarks(false, 1) == "!");
    assert(clickMarks(true, 2) == "~!!");
    assert(clickMarks(false, 7) == "!!!");
    assert(clickMarks(false, 1, true) == "!?");
    assert(hudMs(windowMs(1.48, 240)) == "6.17MS");
    assert(hudHz(windowMs(1.48, 240)) == "162Hz");

    // Swift chains: presses of one key closer than the gap.
    {
        std::vector<SwiftInput> in = {
            {0, true, 0}, {4, false, 0}, {10, true, 0}, {12, false, 0}, {18, true, 0},
            {100, true, 1}, {200, true, 0}, {205, true, 1}};
        auto const levels = swiftLevels(in, 10);
        assert(levels[0] == 0 && levels[1] == 0);
        assert(levels[2] == 1 && levels[3] == 0);
        assert(levels[4] == 2);
        assert(levels[5] == 0 && levels[6] == 0 && levels[7] == 0);
    }

    assert(shownFrames(2.004, true) == 2.0);
    assert(shownFrames(2.2, false) == 3.0);

    assert(formatMs(windowMs(3.0, 240), false) == "12.5ms");
    assert(formatMs(windowMs(1.0, 240), true) == "4.2ms+");
    assert(windowMs(0.0, 240) == 0.0);

    // Pitch: 1 frame is the highest, each frame goes down one semitone.
    assert(std::abs(dingPitch(8.0, 1.0) - 1.0) < 1e-9);
    assert(std::abs(dingPitch(20.0, 1.0) - 2.0) < 1e-9);
    assert(dingPitch(1.0, 1.0) < dingPitch(2.0, 1.0));
    assert(dingPitch(16.0, 1.0) > dingPitch(15.0, 1.0));
    assert(dingPitch(0.5, 1.0) == dingPitch(1.0, 1.0));
    assert(dingPitch(500.0, 1.0) <= 4.0);

    // Disconnected windows: NaN GD's "(0.01, 1.85) U (2.09, 2.44) w/ 0.01".
    {
        assert(tc::clickMarks(false, 0, false, true) == "-");
        assert(tc::clickMarks(true, 1, false, true) == "~-!");
        assert(tc::clickMarks(false, 0, true, false, true) == "^?");
        assert(tc::clickMarks(true, 2, false, true, true) == "~-^!!");
        tc::WindowResult r;
        r.earlyTicks = 100;   // units of 0.01 frame
        r.lateTicks = 84;
        r.stepTicks = 1;
        r.lateIslandFrom = 108;
        r.lateIslandTo = 143;
        auto const spans = tc::windowSpans(r, 0.01);
        assert(spans.size() == 2);
        assert(std::abs(spans[0].width(0.01) - 1.85) < 1e-9);
        assert(std::abs(spans[1].width(0.01) - 0.36) < 1e-9);
        assert(tc::spansLabel(spans, 0.01, true) == "(0.01, 1.85) U (2.09, 2.44) w/ 0.01");
    }

    // ~ shows the usual window, not the middle of the range.
    {
        // Previous window [-4, 4]: middle weighs most, edges least.
        assert(tc::timingWeight(0, 4, 4) > tc::timingWeight(2, 4, 4));
        assert(std::abs(tc::timingWeight(4, 4, 4) - tc::timingWeight(-4, 4, 4)) < 1e-12);
        // 2.0 at the recorded timing and two middle points, 1.5 and 2.5 at the edges.
        std::vector<tc::WeightedWindow> samples{{2.0, tc::timingWeight(0, 4, 4)}, {1.5, tc::timingWeight(-4, 4, 4)},
            {2.0, tc::timingWeight(-2, 4, 4)}, {2.0, tc::timingWeight(2, 4, 4)}, {2.5, tc::timingWeight(4, 4, 4)}};
        assert(std::abs(tc::typicalWindow(samples, 0.06, 2.0) - 2.0) < 1e-9);
        // Skewed: 1.2 at most timings, 3.0 only at one edge: the middle would be 2.1.
        std::vector<tc::WeightedWindow> skew{{1.2, 1.0}, {1.21, 0.6}, {1.2, 0.6}, {3.0, 0.14}};
        double const t = tc::typicalWindow(skew, 0.06, 1.2);
        assert(t > 1.19 && t < 1.22);
    }

    // Display digits: two below 7 frames, one from 7 on, cut not rounded.
    {
        assert(tc::windowDecimals(6.99, 7.0, 2, 1) == 2 && tc::windowDecimals(7.0, 7.0, 2, 1) == 1);
        assert(tc::frameLabelDecimals(6.47, 2, false) == "6.47");
        assert(tc::frameLabelDecimals(8.36, 1, false) == "8.3");
        assert(tc::frameLabelDecimals(7.96, 1, true) == "7.9+");
        assert(tc::frameLabelDecimals(12.0, 1, false) == "12");
        assert(tc::frameLabelDecimals(2.004, 2, false) == "2");
        assert(tc::frameLabelDecimals(8.04, 1, true) == "8+");
        assert(tc::frameLabelDecimals(2.30, 2, false) == "2.30");
        assert(tc::frameLabelDecimals(3.10, 2, false) == "3.10");
    }

    std::cout << "Window math tests passed\n";
}
