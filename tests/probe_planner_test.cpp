#include "../src/ProbePlanner.hpp"

#include <cassert>
#include <functional>
#include <iostream>

using namespace tc;

namespace {

struct Truth {
    int early;  // raw ticks the input can move earlier and still survive
    int late;   // raw ticks the input can move later and still survive
};

// Runs the planner against a fake game where input i survives iff its shift
// stays inside [-early, +late].
std::vector<WindowResult> run(
    std::vector<PlannerInput> inputs, std::vector<Truth> truth, PlannerConfig config,
    size_t* trials = nullptr
) {
    ProbePlanner planner;
    planner.begin(std::move(inputs), config);
    size_t count = 0;
    while (auto trial = planner.next()) {
        auto const& t = truth[trial->index];
        planner.report(trial->shift >= -t.early && trial->shift <= t.late);
        assert(++count < 100000);
    }
    assert(planner.finished());
    if (trials) *trials = count;
    return planner.results();
}

PlannerConfig baseConfig() {
    PlannerConfig c;
    c.step = 2;
    c.maxShift = 32;
    c.lookahead = 180;
    c.maxWindowTicks = 32;
    c.endTick = 100000;
    c.endKind = RunEnd::Completed;
    return c;
}

} // namespace

int main() {
    // A release limited only by its neighbouring press is free, not a tiny window.
    {
        auto config = baseConfig();
        config.relaxNeighbors = true;
        PlannerInput press{1000, 1, true, false};
        PlannerInput release{1004, 1, true, true};
        PlannerInput press2{1008, 1, true, false};
        auto res = run({press, release, press2}, {{40, 40}, {40, 40}, {40, 40}}, config);
        assert(res[1].valid && res[1].exceeded);
        config.relaxNeighbors = false;
        res = run({press, release, press2}, {{40, 40}, {40, 40}, {40, 40}}, config);
        assert(res[1].valid && !res[1].exceeded && res[1].widthTicks() <= 8);
    }

    // Exact windows are found by bisection in both directions.
    {
        auto res = run({{1000, 0, true}}, {{4, 6}}, baseConfig());
        assert(res[0].valid);
        assert(res[0].earlyTicks == 4 && res[0].lateTicks == 6);
        assert(res[0].widthTicks() == 12);   // 6 frames
        assert(!res[0].capped() && !res[0].exceeded);
    }

    // Every possible window of a click is found, not only a few lucky ones.
    {
        for (int early = 0; early <= 14; early += 2) {
            for (int late = 0; late <= 14; late += 2) {
                auto res = run({{1000, 0, true}}, {{early, late}}, baseConfig());
                assert(res[0].valid && !res[0].exceeded);
                assert(res[0].earlyTicks == early && res[0].lateTicks == late);
            }
        }
    }

    // The fast search (gallop) needs far fewer trials than scanning every offset.
    {
        auto config = baseConfig();
        config.gallop = true;
        config.maxShift = 64;
        config.maxWindowTicks = 1000;
        size_t trials = 0;
        auto res = run({{1000, 0, true}}, {{40, 50}}, config, &trials);
        assert(res[0].earlyTicks == 40 && res[0].lateTicks == 50);
        assert(trials <= 22);   // linear scanning would need 45
    }

    // A hard click is cheap: one failed trial per side.
    {
        size_t trials = 0;
        auto res = run({{1000, 0, true}}, {{0, 0}}, baseConfig(), &trials);
        assert(res[0].valid && res[0].earlyTicks == 0 && res[0].lateTicks == 0);
        assert(trials == 2);
        trials = 0;
        res = run({{1000, 0, true}}, {{2, 0}}, baseConfig(), &trials);   // passes 1 step, fails 2 steps early
        assert(res[0].earlyTicks == 2 && trials == 3);
    }

    // A completely free click exceeds the display limit (17 frames > 16).
    {
        auto res = run({{1000, 0, true}}, {{1000, 1000}}, baseConfig());
        assert(res[0].valid && res[0].exceeded);
    }

    // Capped but still displayable: the shift limit is smaller than the display width.
    {
        auto config = baseConfig();
        config.maxShift = 8;           // 4 frames each way
        auto res = run({{1000, 0, true}}, {{1000, 1000}}, config);
        assert(res[0].valid && !res[0].exceeded);
        assert(res[0].earlyCapped && res[0].lateCapped);
        assert(res[0].widthTicks() == 18);   // 9 frames
    }

    // Early stop: a wide window ends the measurement quickly.
    {
        auto config = baseConfig();
        config.maxShift = 200;
        size_t trials = 0;
        auto res = run({{1000, 0, true}}, {{1000, 1000}}, config, &trials);
        assert(res[0].exceeded);
        assert(trials <= 17);   // 16 frames early already make the click free
    }

    // 15 and 16 frames are exact; one more frame on either side makes the click free.
    {
        auto res = run({{1000, 0, true}}, {{14, 14}}, baseConfig());
        assert(res[0].widthTicks() == 30 && !res[0].exceeded && !res[0].capped());

        res = run({{1000, 0, true}}, {{16, 14}}, baseConfig());
        assert(res[0].widthTicks() == 32 && !res[0].exceeded && !res[0].capped());

        res = run({{1000, 0, true}}, {{16, 16}}, baseConfig());
        assert(res[0].exceeded);
    }

    // Asymmetric windows: when one side dies early, the other side is checked
    // further, so the whole display width (16 shifts + the click) is covered.
    {
        auto res = run({{1000, 0, true}}, {{2, 1000}}, baseConfig());   // 1 frame early, free late
        assert(res[0].exceeded);
        res = run({{1000, 0, true}}, {{1000, 0}}, baseConfig());        // free early, 0 late
        assert(res[0].exceeded);
        res = run({{1000, 0, true}}, {{2, 26}}, baseConfig());          // 1 + 13 + 1 = 15 frames
        assert(!res[0].exceeded && res[0].earlyTicks == 2 && res[0].lateTicks == 26);
        res = run({{1000, 0, true}}, {{0, 30}}, baseConfig());          // 0 + 15 + 1 = 16 frames
        assert(!res[0].exceeded && res[0].widthTicks() == 32);
        res = run({{1000, 0, true}}, {{30, 0}}, baseConfig());
        assert(!res[0].exceeded && res[0].widthTicks() == 32);
    }

    // Half-frame precision (vanilla Click Between Steps): step of one raw tick.
    {
        auto config = baseConfig();
        config.step = 1;
        for (int early = 0; early <= 16; ++early) {
            for (int late = 0; late <= 16; late += 3) {
                auto res = run({{1000, 0, true}}, {{early, late}}, config);
                assert(res[0].valid);
                if (early + late + 1 > 32) { assert(res[0].exceeded); continue; }
                assert(res[0].earlyTicks == early && res[0].lateTicks == late);
                assert(res[0].stepTicks == 1);
            }
        }
        // Only the original half step works: 0.5 frame.
        auto res = run({{1000, 0, true}}, {{0, 0}}, config);
        assert(res[0].widthTicks() == 1);
    }

    // Neighbour on the same button limits the shift and is not reported as a cap.
    {
        auto res = run({{1000, 0, true}, {1006, 0, true}}, {{0, 1000}, {1000, 1000}}, baseConfig());
        assert(res[0].lateTicks == 4 && !res[0].lateCapped);
        assert(res[1].earlyTicks == 4 && !res[1].earlyCapped);
    }

    // An input at tick 0 cannot be moved earlier.
    {
        auto res = run({{0, 0, true}}, {{10, 4}}, baseConfig());
        assert(res[0].earlyTicks == 0 && res[0].lateTicks == 4);
    }

    // minInputTick keeps shifted inputs after a checkpoint.
    {
        auto config = baseConfig();
        config.minInputTick = 996;
        auto res = run({{1000, 0, true}}, {{100, 4}}, config);
        assert(res[0].earlyTicks == 4);
    }

    // A shifted input must survive until the next input plus the lookahead, so
    // inputs that are too close to a recorded death cannot be verified.
    {
        auto config = baseConfig();
        config.endKind = RunEnd::Death;
        config.endTick = 1300;
        auto res = run({{500, 0, true}, {700, 0, true}, {1200, 0, true}},
            {{4, 4}, {4, 4}, {4, 4}}, config);
        assert(res[0].valid);
        assert(!res[1].valid);
        assert(!res[2].valid);
    }

    // A completed level is a verified success: nothing has to be unmeasurable.
    {
        auto config = baseConfig();
        config.endKind = RunEnd::Completed;
        config.endTick = 1300;
        auto res = run({{500, 0, true}, {700, 0, true}, {1200, 0, true}},
            {{4, 4}, {4, 4}, {4, 4}}, config);
        for (auto const& r : res) assert(r.valid && r.earlyTicks == 4 && r.lateTicks == 4);
    }

    // Same button events keep their order.
    {
        auto res = run(
            {{1000, 0, true}, {1010, 0, true}, {1020, 0, true}},
            {{0, 100}, {100, 100}, {100, 100}}, baseConfig());
        assert(res[1].earlyTicks == 8 && res[1].lateTicks == 8);
        assert(res[0].lateTicks == 8);
        assert(res[2].earlyTicks == 8);
    }

    // Different buttons and players do not constrain each other.
    {
        auto res = run({{1000, 0, true}, {1002, 1, true}, {1004, 0, false}},
            {{4, 4}, {4, 4}, {4, 4}}, baseConfig());
        for (auto const& r : res) assert(r.earlyTicks == 4 && r.lateTicks == 4);
    }

    // Live mode: one input only, lookahead counted from the shifted input itself.
    {
        auto config = baseConfig();
        config.onlyIndex = 1;
        config.anchorNext = false;
        config.controlTrial = true;
        config.endKind = RunEnd::Paused;
        config.endTick = 1000 + 180 + 16;
        ProbePlanner planner;
        planner.begin({{500, 0, true}, {1000, 0, true}, {1050, 0, true}}, config);
        bool first = true;
        size_t trials = 0;
        while (auto trial = planner.next()) {
            assert(trial->index == 1);
            if (first) { assert(trial->shift == 0); first = false; }   // control trial
            planner.report(trial->shift >= -6 && trial->shift <= 4);
            ++trials;
        }
        auto const& res = planner.results();
        assert(res[1].valid && res[1].earlyTicks == 6 && res[1].lateTicks == 4);
        assert(!res[0].valid && !res[2].valid);   // untouched
        assert(trials < 14);
    }

    // Live mode: a control trial that dies means the simulation is unreliable.
    {
        auto config = baseConfig();
        config.onlyIndex = 0;
        config.controlTrial = true;
        ProbePlanner planner;
        planner.begin({{1000, 0, true}}, config);
        auto trial = planner.next();
        assert(trial && trial->shift == 0);
        planner.report(false);
        assert(!planner.next().has_value());
        assert(!planner.results()[0].valid);
    }

    // Holes: the window is the contiguous run of passing shifts around the click.
    // Shifts +4..+7 frames pass again after +3 died (e.g. a different orb is hit):
    // the late side must stop at +2 frames, the fast search would report +7.
    {
        auto pass = [](int shift) { return shift >= -2 && (shift <= 4 || (shift >= 8 && shift <= 14)); };
        auto measure = [&](PlannerConfig config) {
            ProbePlanner planner;
            planner.begin({{1000, 0, true}}, config);
            while (auto trial = planner.next()) planner.report(pass(trial->shift));
            return planner.results()[0];
        };
        auto exact = measure(baseConfig());
        assert(exact.valid && exact.earlyTicks == 2 && exact.lateTicks == 4);
        auto fast = baseConfig();
        fast.gallop = true;
        auto rough = measure(fast);
        assert(rough.lateTicks == 14);   // documents why the outward scan is the default
    }

    // Sub-frame steps (Click Between Frames, 100 units per frame): whole frames
    // are scanned outwards, the last frame is bisected down to one unit.
    {
        auto config = baseConfig();
        config.step = 1;
        config.coarseSteps = 100;
        config.maxShift = 1600;
        config.maxWindowTicks = 1600;
        for (int early : {0, 37, 100, 250, 499}) {
            for (int late : {0, 1, 99, 100, 101, 640}) {
                size_t trials = 0;
                auto res = run({{50000, 0, true}}, {{early, late}}, config, &trials);
                assert(res[0].valid && !res[0].exceeded);
                assert(res[0].earlyTicks == early && res[0].lateTicks == late);
                assert(trials <= static_cast<size_t>(early / 100 + late / 100 + 2 + 2 * 7));
            }
        }
        // A hole one whole frame away is not crossed.
        ProbePlanner planner;
        planner.begin({{50000, 0, true}}, config);
        while (auto trial = planner.next()) {
            int const s = trial->shift;
            planner.report(s >= -50 && (s <= 130 || (s >= 300 && s <= 900)));
        }
        assert(planner.results()[0].earlyTicks == 50 && planner.results()[0].lateTicks == 130);
    }

    // Corrections of the next click: same shift, opposite shift, then whole
    // frames nearest first (compensating direction first), never 0, no repeats.
    {
        std::vector<int> got;
        for (int i = 1; auto m = correctionShift(2, 2, i); ++i) got.push_back(*m);   // 1 frame late, frame = 2 units
        assert((got == std::vector<int>{2, -2, -4, 4}));
        got.clear();
        for (int i = 1; auto m = correctionShift(-250, 100, i); ++i) got.push_back(*m);   // CBF, 2.5 frames early
        assert((got == std::vector<int>{-250, 250, 100, -100, 200, -200, 300, -300, 400, -400}));
        got.clear();
        for (int i = 1; auto m = correctionShift(-2000, 100, i); ++i) got.push_back(*m);  // capped at 4 frames around
        assert(got.size() == 10 && got[0] == -2000 && got[1] == 2000 && got.back() == -400);
        assert(!correctionShift(0, 2, 1) && !correctionShift(2, 2, 0));
        // Deep Count: 8 frames around even for a 1-frame shift (+-1 to +-8 frames: 16 candidates).
        {
            std::vector<int> wide;
            for (int i = 1; auto m = correctionShift(2, 2, i, 8, true); ++i) wide.push_back(*m);
            assert(wide.size() == 16 && wide.front() == 2 && wide[1] == -2);
            assert(std::find(wide.begin(), wide.end(), -16) != wide.end() && std::find(wide.begin(), wide.end(), 16) != wide.end());
            assert(std::find(wide.begin(), wide.end(), 18) == wide.end());
        }
        for (int i = 1; auto m = correctionShift(6, 2, i); ++i) assert(*m != 0);
    }

    // Empty input list finishes immediately.
    {
        ProbePlanner planner;
        planner.begin({}, baseConfig());
        assert(!planner.next().has_value());
        assert(planner.finished());
    }

    // Per-input horizon: a free-flight input only has to live until the next input + its own lookahead.
    {
        tc::ProbePlanner planner;
        std::vector<tc::PlannerInput> inputs = {{100, 1, true}, {140, 1, true}};
        inputs[0].lookahead = 12;
        tc::PlannerConfig config;
        config.lookahead = 180;
        config.endTick = 1000;
        config.maxShift = 8;
        config.maxWindowTicks = 32;
        config.onlyIndex = 0;
        planner.begin(inputs, config);
        auto const trial = planner.next();
        assert(trial && trial->endTick == 140 + 12);
    }

    // Islands (Deep Count): a second window beyond a gap is found and bisected,
    // also when the main edges are given as known; none is invented.
    {
        auto islandRun = [](PlannerConfig config, std::function<bool(int)> passes, size_t* trials = nullptr) {
            ProbePlanner planner;
            planner.begin({{1000, 0, true}}, config);
            size_t count = 0;
            while (auto trial = planner.next()) {
                planner.report(passes(trial->shift));
                assert(++count < 10000);
            }
            if (trials) *trials = count;
            return planner.results()[0];
        };
        auto config = baseConfig();
        config.step = 1;
        config.coarseSteps = 2;
        config.islandSteps = 6;
        // Main window [-3, 4], late island [9, 12], early island [-14, -11] (too far: 6 steps past -4 reach -10).
        auto passes = [](int shift) { return (shift >= -3 && shift <= 4) || (shift >= 9 && shift <= 12) || (shift >= -14 && shift <= -11); };
        auto r = islandRun(config, passes);
        assert(r.valid && r.earlyTicks == 3 && r.lateTicks == 4);
        assert(r.lateIslandFrom == 9 && r.lateIslandTo == 12);
        assert(r.earlyIslandFrom == 0);
        config.islandSteps = 12;
        r = islandRun(config, passes);
        assert(r.earlyIslandFrom == 11 && r.earlyIslandTo == 14);

        size_t trials = 0;
        config.knownEarly = 3;
        config.knownLate = 4;
        r = islandRun(config, passes, &trials);
        assert(r.earlyTicks == 3 && r.lateTicks == 4 && r.lateIslandFrom == 9 && r.lateIslandTo == 12);
        assert(trials < 30);

        auto plain = [](int shift) { return shift >= -3 && shift <= 4; };
        r = islandRun(config, plain);
        assert(r.earlyIslandFrom == 0 && r.lateIslandFrom == 0 && r.lateTicks == 4);
    }

    // Edge check: fixed shifts are tried in order and their outcomes kept.
    {
        auto config = baseConfig();
        config.onlyIndex = 0;
        config.fixedShifts = {-4, -6, 6, 8};
        ProbePlanner planner;
        planner.begin({{1000, 0, true}}, config);
        std::vector<int> seen;
        while (auto trial = planner.next()) {
            seen.push_back(trial->shift);
            planner.report(trial->shift >= -4 && trial->shift <= 6);
        }
        assert((seen == std::vector<int>{-4, -6, 6, 8}));
        assert((planner.fixedOutcomes() == std::vector<int>{1, 0, 1, 0}));
        assert(planner.finished());
    }

    std::cout << "Probe planner tests passed\n";
}
