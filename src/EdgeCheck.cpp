// Edge check, the first part of Deep Count: the edges of every shown window tried again strictly.

#include "App.hpp"

namespace tc {

// For every shown click the last passing and the first failing shift of each side are tried again, strictly.
bool prepareEdgeChecks(Session& s) {
    if (!cfg::flag("verify-edges")) return false;
    auto const& events = s.source.events;
    s.deepJobs.clear();
    s.edgeChecks.assign(events.size(), {});
    s.edgeOutcomes.assign(events.size(), {});
    size_t skipped = 0;
    for (size_t k = 0; k < events.size() && k < s.deepBase.size(); ++k) {
        auto const& w = s.deepBase[k];
        if (!overlay::isShown(events[k], w)) continue;
        auto& checks = s.edgeChecks[k];
        int const step = std::max(1, w.stepTicks);
        auto add = [&](int shift, bool expectSurvive, bool early) {
            auto const known = s.exactTrials.find({k, shift});
            if (known != s.exactTrials.end() && known->second == expectSurvive) {
                ++skipped;
                return;
            }
            checks.push_back({shift, expectSurvive, early});
        };
        if (w.earlyTicks > 0) add(-w.earlyTicks, true, true);
        if (!w.earlyCapped) add(-(w.earlyTicks + step), false, true);
        if (w.lateTicks > 0) add(w.lateTicks, true, false);
        if (!w.lateCapped) add(w.lateTicks + step, false, false);
        if (!checks.empty()) s.deepJobs.push_back({k, k, 0, 3, 1.0});
    }
    log::info("Edge check: {} clicks, {} edges already exact", s.deepJobs.size(), skipped);
    s.edgeCheckRan = true;
    if (s.deepJobs.empty()) return false;
    s.verifyMode = true;
    s.strictTrials = true;
    s.deepJobsTotal = s.deepJobs.size();
    s.deepJobActive = false;
    return true;
}

// The windows after the edge check.
void applyEdgeChecks(Session& s) {
    auto const& events = s.source.events;
    s.edgeUncertain.assign(events.size(), 0);
    int differed = 0;
    int checked = 0;
    for (size_t k = 0; k < events.size() && k < s.windows.size() && k < s.edgeChecks.size() && k < s.edgeOutcomes.size(); ++k) {
        auto const& checks = s.edgeChecks[k];
        auto const& outcomes = s.edgeOutcomes[k];
        if (checks.empty() || outcomes.empty()) continue;
        ++checked;
        auto& w = s.windows[k];
        double const before = cfg::frames(w);
        bool off = false;
        for (size_t i = 0; i < checks.size() && i < outcomes.size(); ++i) {
            if (outcomes[i] < 0) continue;
            bool const survived = outcomes[i] == 1;
            if (survived == checks[i].expectSurvive) continue;
            off = true;
            if (checks[i].expectSurvive) {
                int& edge = checks[i].early ? w.earlyTicks : w.lateTicks;
                edge = std::max(0, edge - w.stepTicks);
            }
            log::warn("Edge check: input #{} {} shift {:+.2f} frames {} on the re-check (counted: {})", k,
                checks[i].early ? "early" : "late", checks[i].shift / static_cast<double>(frameUnits(s)),
                survived ? "survived" : "died", checks[i].expectSurvive ? "survived" : "died");
        }
        if (!off) continue;
        s.edgeUncertain[k] = 1;
        ++differed;
        log::warn("Edge check: input #{} window {:.2f} -> {:.2f} frames, marked ?", k, before, cfg::frames(w));
    }
    s.edgeChecks.clear();
    s.edgeOutcomes.clear();
    log::info("Edge check: {} of {} clicks differed on the re-check", differed, checked);
}

// The edge check is done: its corrections become the base of the Deep Count jobs.
void finishEdgePhase(PlayLayer* play, Session& s) {
    s.windows = s.deepBase;
    applyEdgeChecks(s);
    s.deepBase = s.windows;
    s.verifyMode = false;
    s.strictTrials = false;
    play->removeAllCheckpoints();
    s.checkpoints.clear();
    s.lastCheckpointTick = 0;
    s.checkpointGoodUntil.clear();
    s.checkpointBadFrom.clear();
    // Same alignment as the first baseline replay.
    s.baselineFinal = true;
    s.stage = Stage::BaselineReplay;
}

} // namespace tc
