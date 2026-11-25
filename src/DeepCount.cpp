// Deep Count: previous-click timings, holes, easier approach, deeper corrections.

#include "App.hpp"

namespace tc {

namespace {

// The normal count measures every click with all other clicks where they were.
// Deep Count, per shown press: measures it again with deeper corrections of the
// clicks after it, then with the previous press at several timings inside its
// window ("~": the window changes; shown is the one it has most often), past
// its own edges ("-": it works again beyond a gap) and, for hard clicks, with
// the inputs before it re-timed ("^": an easier approach).

// A value in the measurement units of `result`, converted to those of this count.
int toCurrentUnits(Session const& s, int value, WindowResult const& result) {
    int const from = std::max(1, result.unitsPerTick);
    return static_cast<int>(std::lround(static_cast<double>(value) * s.units / from));
}

// Moves input `index` of `list` by `shift` units, with its release when whole
// taps move, unless that release is `keep` (an input the job places itself).
void moveWithRelease(std::vector<InputEvent>& list, std::vector<InputEvent> const& events, size_t index, int shift,
    int units, size_t keep) {
    list[index] = shiftedEvent(events[index], shift, units);
    auto const partner = moveHold() ? partnerOf(events, index) : std::string::npos;
    if (partner != std::string::npos && partner != keep) list[partner] = shiftedEvent(events[partner], shift, units);
}

// Part of an input's window Deep Count moves it within (samples of the previous
// click, easier approach): at the very edge the run only just survives.
constexpr double kDeepSampleReach = 0.75;

// The two candidates (early and late edge, at most 3 frames) of the next input
// of an easier approach search. False when the search of that click is over.
bool pushApproachJobs(Session& s, size_t k) {
    auto it = s.approach.find(k);
    if (it == s.approach.end()) return false;
    auto& state = it->second;
    int const step = std::max(1, s.stepTicks);
    int const reachUnits = 3 * frameUnits(s);
    while (state.next < state.inputs.size()) {
        size_t const j = state.inputs[state.next];
        auto const& w = s.deepBase[j];
        int const early = static_cast<int>(std::min(toCurrentUnits(s, w.earlyTicks, w), reachUnits) * kDeepSampleReach) / step * step;
        int const late = static_cast<int>(std::min(toCurrentUnits(s, w.lateTicks, w), reachUnits) * kDeepSampleReach) / step * step;
        state.pending = 0;
        state.candidateBest = 0.0;
        state.candidateShift = 0;
        for (int shift : {-early, late}) {
            if (shift == 0) continue;
            s.deepJobs.push_back({k, j, shift, 4, 1.0});
            ++state.pending;
            ++s.deepJobsTotal;
        }
        if (state.pending > 0) return true;
        ++state.next;
    }
    return false;
}

// A candidate of the easier approach search is measured: once both candidates
// of an input are, the better one is kept if it widened the click.
void finishApproachJob(Session& s, size_t k, int shift, WindowResult const& result) {
    auto it = s.approach.find(k);
    if (it == s.approach.end()) return;
    auto& state = it->second;
    double const frames = result.valid ? cfg::framesOrLimit(result) : 0.0;
    if (frames > state.candidateBest) {
        state.candidateBest = frames;
        state.candidateShift = shift;
    }
    if (--state.pending > 0) return;
    if (state.candidateBest > state.best + 1e-6) {
        state.best = state.candidateBest;
        state.keep.push_back({state.inputs[state.next], state.candidateShift});
    }
    ++state.next;
    pushApproachJobs(s, k);
}

// "the press before it 1.50 frames later, the 2nd release before it 0.50 earlier"
std::string describeApproach(Session const& s, size_t k, std::vector<ApproachMove> const& moves) {
    auto const& events = s.source.events;
    std::string text;
    for (auto const& move : moves) {
        if (move.input >= events.size()) continue;
        bool const down = events[move.input].down;
        int nth = 0;
        for (size_t i = move.input; i < k; ++i) {
            if (events[i].down == down) ++nth;
        }
        double const frames = std::abs(move.shift) / static_cast<double>(frameUnits(s));
        std::string const which = nth <= 1 ? "" : nth == 2 ? "2nd " : nth == 3 ? "3rd " : fmt::format("{}th ", nth);
        if (!text.empty()) text += ", ";
        text += fmt::format("the {}{} before {:.2f} {}", which, down ? "press" : "release", frames,
            move.shift < 0 ? "earlier" : "later");
    }
    return text;
}

} // namespace

void buildDeepJobs(Session& s) {
    auto const& events = s.source.events;
    s.deepJobs.clear();
    s.deepSamples.assign(events.size(), {});
    s.deepHoles.assign(events.size(), std::nullopt);
    s.deepBaseWeight.assign(events.size(), 1.0);
    s.approach.clear();
    s.deepRebase.assign(events.size(), std::nullopt);
    s.deepJobsTotal = 0;
    // Timings of the previous click: spread evenly over the inner three quarters
    // of its window. At its very edge the run only just survives somewhere
    // later, so any shift of this click kills it: a "0.01" that says nothing.
    int const samples = std::clamp(static_cast<int>(cfg::integer("deep-samples")), 2, 9);
    // Only a click shortly before can change the outcome of this one.
    int const reach = 2 * cfg::lookaheadTicks() + cfg::maxShiftTicks();
    for (size_t k = 0; k < events.size() && k < s.deepBase.size(); ++k) {
        if (!events[k].down || !overlay::isShown(events[k], s.deepBase[k])) continue;
        size_t prev = std::string::npos;
        for (size_t i = k; i-- > 0;) {
            if (events[i].down) {
                prev = i;
                break;
            }
        }
        if (prev != std::string::npos && events[k].tick - events[prev].tick <= reach && s.deepBase[prev].valid) {
            auto const& w = s.deepBase[prev];
            int const early = toCurrentUnits(s, w.earlyTicks, w);
            int const late = toCurrentUnits(s, w.lateTicks, w);
            int const step = std::max(1, s.stepTicks);
            s.deepBaseWeight[k] = timingWeight(0.0, early, late);
            std::vector<int> shifts;
            for (int i = 0; i < samples; ++i) {
                double const from = -early * kDeepSampleReach;
                double const at = from + (late + early) * kDeepSampleReach * i / (samples - 1);
                int const shift = static_cast<int>(std::lround(at / step)) * step;
                // Inside the window, not the recorded timing (that one is the normal count).
                if (shift == 0 || shift < -early || shift > late) continue;
                if (std::find(shifts.begin(), shifts.end(), shift) != shifts.end()) continue;
                shifts.push_back(shift);
                s.deepJobs.push_back({k, prev, shift, 0, timingWeight(shift, early, late)});
            }
        }
        // Holes: does the click work again beyond the edges of its window?
        // Pushed after the samples, so it runs before them (it moves this click, they an earlier one).
        if (cfg::integer("island-frames") > 0) s.deepJobs.push_back({k, k, 0, 2, 1.0});
        // Re-measure with the Deep Count corrections: pushed after the jobs
        // below, it runs first and becomes their base.
        bool const rebase = true;
        // Easier approach of a hard click (at most a quarter of the display limit).
        double const base = cfg::frames(s.deepBase[k]);
        int const approachInputs = cfg::integer("approach-inputs");
        if (approachInputs > 0 && base <= cfg::maxDisplayFrames() / 4.0 + 1e-9) {
            ApproachState state;
            state.base = base;
            state.best = base;
            for (size_t i = k; i-- > 0 && static_cast<int>(state.inputs.size()) < approachInputs;) {
                if (events[k].tick - events[i].tick > reach) break;
                if (s.deepBase[i].valid) state.inputs.push_back(i);
            }
            if (!state.inputs.empty()) {
                s.approach[k] = std::move(state);
                pushApproachJobs(s, k);
            }
        }
        if (rebase) s.deepJobs.push_back({k, k, 0, 5, 1.0});
    }
    // (pushApproachJobs counted its jobs already.)
    s.deepJobsTotal = s.deepJobs.size();
    s.deepJobActive = false;
}

// Stores what the finished job found and starts the next one (from the back, so
// the checkpoints are only ever trimmed from the end). False once all are done.
bool beginNextDeepJob(Session& s) {
    if (s.deepJobActive) {
        auto const& results = s.planner.results();
        size_t const k = s.deepCurrent.index;
        s.deepJobActive = false;
        if (s.deepCurrent.variant == 5) {
            if (k < results.size() && k < s.deepRebase.size() && results[k].valid) {
                s.deepRebase[k] = results[k];
                // The easier approach starts from this window.
                if (auto it = s.approach.find(k); it != s.approach.end()) {
                    auto const& r = results[k];
                    double const frames = cfg::framesOrLimit(r);
                    it->second.base = frames;
                    it->second.best = frames;
                }
            }
        } else if (s.deepCurrent.variant == 3) {
            if (k < s.edgeOutcomes.size()) s.edgeOutcomes[k] = s.planner.fixedOutcomes();
        } else if (k < results.size() && s.deepCurrent.variant == 4) {
            finishApproachJob(s, k, s.deepCurrent.prevShift, results[k]);
        } else if (k < results.size() && k < s.deepSamples.size() && k < s.deepHoles.size()) {
            if (s.deepCurrent.variant == 2) {
                s.deepHoles[k] = results[k];
            } else if (!results[k].valid && !s.deepCurrent.follow) {
                // With the previous click earlier or later a player clicks this one
                // earlier or later too: measure it again moved along (same rhythm).
                auto again = s.deepCurrent;
                again.follow = true;
                s.deepJobs.push_back(again);
                ++s.deepJobsTotal;
            } else {
                s.deepSamples[k].push_back({s.deepCurrent.weight, results[k]});
            }
        }
    }
    if (s.deepJobs.empty()) return false;
    s.deepCurrent = s.deepJobs.back();
    s.deepJobs.pop_back();
    s.deepJobActive = true;

    // Strict trials (no "Back on path"): the edge check, and the re-measure of a
    // click whose edge it found approximate, which would otherwise widen again.
    size_t const index = s.deepCurrent.index;
    s.strictTrials = s.deepCurrent.variant == 3 ||
        (s.deepCurrent.variant == 5 && index < s.edgeUncertain.size() && s.edgeUncertain[index]);

    auto const& events = s.source.events;
    if (s.deepCurrent.variant == 3) {
        // Edge check: the last passing and first failing shifts once more, strictly.
        size_t const k = s.deepCurrent.index;
        s.trialBase = events;
        auto config = plannerConfig(s, s.source.endTick, s.source.endKind);
        config.onlyIndex = static_cast<int>(k);
        for (auto const& check : s.edgeChecks[k]) config.fixedShifts.push_back(check.shift);
        s.planner.begin(plannerInputs(s.trialBase, s.units, &s.freeFlight), config);
        return true;
    }
    if (s.deepCurrent.variant == 5) {
        // The click where it was, everything else as recorded: the deeper corrections only.
        s.trialBase = events;
        auto config = plannerConfig(s, s.source.endTick, s.source.endKind);
        config.onlyIndex = static_cast<int>(s.deepCurrent.index);
        s.planner.begin(plannerInputs(s.trialBase, s.units, &s.freeFlight), config);
        return true;
    }
    if (s.deepCurrent.variant == 4) {
        // Easier approach: the moves kept so far and the candidate.
        size_t const k = s.deepCurrent.index;
        s.trialBase = events;
        if (auto it = s.approach.find(k); it != s.approach.end()) {
            for (auto const& kept : it->second.keep) moveWithRelease(s.trialBase, events, kept.input, kept.shift, s.units, k);
        }
        moveWithRelease(s.trialBase, events, s.deepCurrent.prev, s.deepCurrent.prevShift, s.units, k);
        if (!keepsButtonOrder(events, s.trialBase)) {
            // Not a playable variant: counts as nothing found.
            finishApproachJob(s, k, s.deepCurrent.prevShift, WindowResult{});
            s.deepJobActive = false;
            return beginNextDeepJob(s);
        }
        auto config = plannerConfig(s, s.source.endTick, s.source.endKind);
        config.onlyIndex = static_cast<int>(k);
        config.controlTrial = true;
        s.planner.begin(plannerInputs(s.trialBase, s.units, &s.freeFlight), config);
        return true;
    }
    if (s.deepCurrent.variant == 2) {
        // The window is known: only the shifts past its edges are tried.
        size_t const k = s.deepCurrent.index;
        auto const& w = k < s.deepRebase.size() && s.deepRebase[k] ? *s.deepRebase[k] : s.deepBase[k];
        s.trialBase = events;
        auto config = plannerConfig(s, s.source.endTick, s.source.endKind);
        config.onlyIndex = static_cast<int>(s.deepCurrent.index);
        config.knownEarly = toCurrentUnits(s, w.earlyTicks, w);
        config.knownLate = toCurrentUnits(s, w.lateTicks, w);
        config.islandSteps = config.coarseSteps * cfg::integer("island-frames");
        s.planner.begin(plannerInputs(s.trialBase, s.units, &s.freeFlight), config);
        return true;
    }
    s.trialBase = events;
    moveWithRelease(s.trialBase, events, s.deepCurrent.prev, s.deepCurrent.prevShift, s.units, s.deepCurrent.index);
    if (s.deepCurrent.follow) {
        // This click (with its release) keeps its distance to the previous one.
        size_t const k = s.deepCurrent.index;
        moveWithRelease(s.trialBase, events, k, s.deepCurrent.prevShift, s.units, s.deepCurrent.prev);
        if (!keepsButtonOrder(events, s.trialBase)) {
            s.deepSamples[k].push_back({s.deepCurrent.weight, WindowResult{}});
            s.deepJobActive = false;
            return beginNextDeepJob(s);
        }
    }

    auto config = plannerConfig(s, s.source.endTick, s.source.endKind);
    config.onlyIndex = static_cast<int>(s.deepCurrent.index);
    // With the previous click moved, this click must still work where it is
    // (at its recorded time, or moved along with the previous one).
    config.controlTrial = true;
    s.planner.begin(plannerInputs(s.trialBase, s.units, &s.freeFlight), config);
    return true;
}

// Ranges of every click after Deep Count, from the normal windows and the jobs.
void finishDeep(Session& s) {
    auto const& events = s.source.events;
    s.deep.assign(events.size(), DeepRange{});
    if (s.dependent.size() != events.size()) s.dependent.assign(events.size(), 0);
    int changed = 0;
    int disconnected = 0;
    int marginal = 0;
    int widened = 0;
    double const framesPerUnit = static_cast<double>(cfg::windowFps()) / (static_cast<double>(cfg::rawTps()) * s.units);
    for (size_t k = 0; k < events.size() && k < s.deepSamples.size() && k < s.deepHoles.size() && k < s.deepBase.size(); ++k) {
        // The window with the deeper corrections replaces the normal one.
        if (k < s.deepRebase.size() && s.deepRebase[k] && k < s.windows.size()) {
            auto const& r = *s.deepRebase[k];
            if (cfg::frames(r) > cfg::frames(s.windows[k]) + 1e-6 || r.exceeded) {
                log::info("Deep Count: input #{} {:.2f} -> {:.2f} frames with the deeper corrections", k,
                    cfg::frames(s.windows[k]), cfg::framesOrLimit(r));
                ++widened;
            }
            s.windows[k] = r;
        }
        auto const& found = s.deepSamples[k];
        auto const& holes = s.deepHoles[k];
        if (found.empty() && !holes) continue;
        double const base = cfg::frames(k < s.windows.size() ? s.windows[k] : s.deepBase[k]);
        DeepRange range{base, base, false};
        bool const precise = cfg::isPrecise(s.deepBase[k]);
        std::vector<WeightedWindow> windows{{base, k < s.deepBaseWeight.size() ? s.deepBaseWeight[k] : 1.0}};
        if (holes && holes->valid) {
            auto spans = windowSpans(*holes, framesPerUnit);
            if (spans.size() > 1) {
                range.spans = std::move(spans);
                range.step = holes->stepTicks * framesPerUnit;
                ++disconnected;
            }
        }
        for (auto const& sample : found) {
            auto const& result = sample.result;
            if (!result.valid) {
                range.broken = true;
                continue;
            }
            // A single-step window (0.01) with the previous click moved: the run
            // only just survives somewhere, the click itself is not that tight.
            if (precise && result.earlyTicks == 0 && result.lateTicks == 0 && base > 0.5) {
                ++marginal;
                continue;
            }
            double const frames = cfg::framesOrLimit(result);
            range.lo = std::min(range.lo, frames);
            range.hi = std::max(range.hi, frames);
            windows.push_back({frames, sample.weight});
        }
        // The value a player usually gets, weighted by how likely each timing of
        // the previous click is; values within a hair count as the same.
        if (range.ranged()) range.typical = typicalWindow(windows, precise ? 0.06 : 0.5, base);
        s.deep[k] = range;
    }
    int easier = 0;
    for (auto const& [k, state] : s.approach) {
        if (k >= s.deep.size() || state.keep.empty() || state.best <= state.base + 1e-6) continue;
        if (s.deep[k].lo <= 0.0) s.deep[k].lo = s.deep[k].hi = state.base;
        s.deep[k].easier = state.best;
        s.deep[k].easierHow = describeApproach(s, k, state.keep);
        if (state.best - state.base >= 1.0 - 1e-9) ++easier;
    }
    log::info("Deep Count: {} hard clicks have an easier approach", easier);
    for (size_t k = 0; k < events.size() && k < s.deep.size(); ++k) {
        auto const& range = s.deep[k];
        if (range.broken || range.ranged()) {
            s.dependent[k] = 1;
            ++changed;
        }
    }
    log::info("Deep Count: {} of {} clicks depend on the previous click, {} have disconnected windows, "
        "{} single-step samples left out, {} wider with the deeper corrections", changed, events.size(), disconnected,
        marginal, widened);
}

} // namespace tc
