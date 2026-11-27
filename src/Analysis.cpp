// The count: baseline replay, trials from checkpoints, corrections, results.

#include "App.hpp"

namespace tc {

namespace {

// A checkpoint check replays this much further than the trial needs.
constexpr int kControlExtraTicks = 480;

} // namespace

void startProbing(Session& s) {
    s.pendingOutcome = ProbeOutcome::None;
    s.stage = Stage::Probing;
    if (s.deepMode) {
        s.skipVisuals = cfg::flag("fast-visuals");
        s.skipVisualsRejections = 0;
        s.freeFlight = freeFlightInputs(s);
        // The edges first (setting "Check edges"), then the Deep Count jobs.
        if (s.borderMode) prepareBorderJobs(s);
        else if (s.edgeCheckRan || !prepareEdgeChecks(s)) buildDeepJobs(s);
        s.trialBase.clear();
        // An empty planner: the first job starts in advanceProbing.
        s.planner.begin({}, plannerConfig(s, s.source.endTick, s.source.endKind));
        return;
    }
    s.skipVisuals = cfg::flag("fast-visuals");
    s.skipVisualsRejections = 0;
    s.trialBase = s.source.events;
    s.dependent.assign(s.source.events.size(), 0);
    s.deep.clear();
    s.exactTrials.clear();
    s.trialDeathP2.clear();
    s.otherMarker.clear();
    s.freeFlight = freeFlightInputs(s);
    s.planner.begin(plannerInputs(s.source.events, s.units, &s.freeFlight), plannerConfig(s, s.source.endTick, s.source.endKind));
}

// Removes checkpoints from the end of the game's list until `count` remain.
bool trimArrayTo(PlayLayer* play, size_t count, CCObject* owned) {
    auto* array = play->m_checkpointArray;
    if (!array) return count == 0;
    for (size_t guard = array->count() + 4; array->count() > count && guard > 0; --guard) {
        if (owned && array->lastObject() == owned) array->removeLastObject();
        else play->removeCheckpoint(false);
    }
    return array->count() == count;
}

namespace {

// Keeps the newest checkpoint that is still early enough for the current trial.
bool trimCheckpoints(PlayLayer* play, Session& s) {
    // Every input the trial moves (Deep Count also moves the previous click).
    int const earliest = s.trialEarliestMove;
    int const earliestNeeded = std::min(earliest, earliest + s.activeOffset()) - cfg::leadTicks();

    // A checkpoint whose runs leave the baseline before this trial ends is no use for it.
    int const needed = std::min(s.trialEndTick, s.trialHitLimit);
    int keepIndex = -1;
    for (size_t i = 0; i < s.checkpoints.size(); ++i) {
        auto const bad = s.checkpointBadFrom.find(s.checkpoints[i].tick);
        if (s.checkpoints[i].tick <= earliestNeeded && (bad == s.checkpointBadFrom.end() || bad->second > needed))
            keepIndex = static_cast<int>(i);
    }

    if (keepIndex < 0) {
        play->removeAllCheckpoints();
        s.checkpoints.clear();
        return true;
    }

    s.checkpoints.erase(s.checkpoints.begin() + keepIndex + 1, s.checkpoints.end());
    return trimArrayTo(play, s.checkpoints.back().arrayCount);
}

} // namespace

// Units of one physics frame in the current measurement.
int frameUnits(Session const& s) {
    return cfg::kRawTicksPerPhysicsFrame * s.units;
}

// Every button keeps its press / release order after inputs were moved.
bool keepsButtonOrder(std::vector<InputEvent> const& original, std::vector<InputEvent> moved) {
    sortByTick(moved);
    std::map<std::pair<int, bool>, std::vector<bool>> want;
    std::map<std::pair<int, bool>, std::vector<bool>> got;
    for (auto const& e : original) want[{e.button, e.player1}].push_back(e.down);
    for (auto const& e : moved) got[{e.button, e.player1}].push_back(e.down);
    return want == got;
}

namespace {

enum class TrialBuild { Ok, Skip, LevelExhausted, Exhausted };

// The deeper and wider corrections belong to the Deep Count jobs.
bool deepCorrections() {
    auto const& s = Session::get();
    return s.deepMode && !s.verifyMode;
}

// Deepest correction chain and the most trials one moved click may use for it.
int adaptDepth() {
    if (deepCorrections()) return std::clamp(cfg::integer("deep-adapt-depth"), 1, 8);
    return std::clamp(cfg::integer("adapt-depth"), 1, 6);
}

// Deep Count lets the player fix a moved click further.
int adaptFrames() {
    return deepCorrections() ? std::clamp(cfg::integer("deep-adapt-frames"), 2, 16) : 4;
}

int maxAdaptTrials() {
    return deepCorrections() ? 160 : 40;
}

// Builds the input list of a trial.
TrialBuild buildTrial(Session& s, Trial const& trial) {
    auto const& events = s.trialBase.size() == s.source.events.size() ? s.trialBase : s.source.events;
    s.activeTrial = trial;
    s.trialNear = false;
    for (auto& path : s.borderTrial) path.clear();
    s.trialEndTick = (trial.endTick + s.units - 1) / s.units;
    s.currentTrial = events;
    s.currentTrial[trial.index] = shiftedEvent(events[trial.index], trial.shift, s.units);
    // The whole tap moves: its release keeps the same distance to the press.
    std::vector<size_t> moved{trial.index};
    auto const partner = moveHold() ? partnerOf(events, trial.index) : std::string::npos;
    if (partner != std::string::npos) {
        s.currentTrial[partner] = shiftedEvent(events[partner], trial.shift, s.units);
        moved.push_back(partner);
    }
    auto following = [&](size_t index) {
        size_t i = index + 1;
        while (i < events.size() && std::find(moved.begin(), moved.end(), i) != moved.end()) ++i;
        return i;
    };

    s.adaptAfterTick = std::numeric_limits<int>::max();
    size_t const next = following(trial.index);
    s.adaptNext = next < events.size() ? next : std::string::npos;
    s.adaptAfterIndex = s.adaptNext;
    if (next < events.size()) {
        int const shiftTicks = shiftedEvent(events[next], trial.shift, s.units).tick - events[next].tick;
        s.adaptAfterTick = std::min(events[next].tick, events[next].tick + shiftTicks);
    }
    if (s.adaptChain.empty()) return TrialBuild::Ok;

    // Correction attempt: the clicks of the chain (with their releases) are re-timed.
    int groupEnd = std::numeric_limits<int>::min();
    size_t last = trial.index;
    for (auto& level : s.adaptChain) {
        if (level.index >= events.size()) return TrialBuild::Exhausted;
        auto const correction =
            correctionShift(level.baseShift, frameUnits(s), level.attempt, adaptFrames(), deepCorrections());
        if (!correction) return TrialBuild::LevelExhausted;
        level.correction = *correction;
        auto const corrected = shiftedEvent(events[level.index], *correction, s.units);
        // A click made after the moment the run died cannot have saved it.
        if (corrected.tick + s.activeOffset() > level.deathTick) return TrialBuild::Skip;
        s.currentTrial[level.index] = corrected;
        moved.push_back(level.index);
        groupEnd = std::max(groupEnd, corrected.tick);
        auto const levelPartner = moveHold() ? partnerOf(events, level.index) : std::string::npos;
        if (levelPartner != std::string::npos && std::find(moved.begin(), moved.end(), levelPartner) == moved.end()) {
            s.currentTrial[levelPartner] = shiftedEvent(events[levelPartner], *correction, s.units);
            moved.push_back(levelPartner);
            groupEnd = std::max(groupEnd, s.currentTrial[levelPartner].tick);
        }
        last = level.index;
    }
    if (!keepsButtonOrder(events, s.currentTrial)) return TrialBuild::Skip;

    // The last re-timed click must hold until the input after it and the lookahead.
    size_t const after = following(last);
    s.adaptAfterIndex = after < events.size() ? after : std::string::npos;
    s.adaptAfterTick = after < events.size() ? events[after].tick : std::numeric_limits<int>::max();
    int anchor = groupEnd;
    if (after < events.size()) anchor = std::max(anchor, events[after].tick);
    s.trialEndTick = std::max(s.trialEndTick, std::min(anchor + horizonTicks(s, last), s.source.endTick));
    return TrialBuild::Ok;
}

// The current trial is decided: tell the planner and forget the corrections.
void settleTrial(Session& s, bool survived) {
    // A moved click that only survives when the next click is re-timed.
    if (survived && !s.adaptChain.empty() && !s.liveSim && s.adaptChain.front().index < s.dependent.size())
        s.dependent[s.adaptChain.front().index] = 1;
    // A death, or a survival without "Back on path", comes out the same every time.
    if (!s.liveSim && !s.deepMode && !(survived && s.trialNear))
        s.exactTrials[{s.activeTrial.index, s.activeTrial.shift}] = survived;
    if (!s.liveSim && !s.deepMode && !survived) s.trialDeathP2[{s.activeTrial.index, s.activeTrial.shift}] = s.lastDeathP2;
    keepBorderTrial(s, survived);
    s.planner.report(survived);
    s.adaptChain.clear();
    s.adaptTrials = 0;
    ++s.trialsFinished;
}

} // namespace

// Settles the outcome of the previous trial and prepares the next one.
void advanceProbing(PlayLayer* play, Session& s) {
    if (s.verifyingCheckpoint && s.pendingOutcome == ProbeOutcome::Survived && !s.checkpoints.empty()) {
        // The checkpoint reproduced the baseline up to the end of the control run.
        int& good = s.checkpointGoodUntil[s.checkpoints.back().tick];
        good = std::max(good, s.trialEndTick);
        s.pendingOutcome = ProbeOutcome::None;
    }
    if (s.verifyingCheckpoint) s.pendingOutcome = ProbeOutcome::None;
    s.verifyingCheckpoint = false;
    if (s.pendingOutcome != ProbeOutcome::None) {
        bool const died = s.pendingOutcome == ProbeOutcome::Died;
        // Died only after the next click.
        bool handled = false;
        if (died && !s.liveSim && adaptClicks() && s.activeTrial.shift != 0 && s.adaptTrials < maxAdaptTrials()) {
            bool const later = s.lastProbeDeathTick >= s.adaptAfterTick && s.adaptAfterIndex != std::string::npos;
            if (s.adaptChain.empty()) {
                if (later) {
                    s.adaptChain.push_back({s.adaptAfterIndex, s.activeTrial.shift, 1, s.lastProbeDeathTick, 0});
                    handled = true;
                }
            } else {
                auto const& top = s.adaptChain.back();
                if (later && static_cast<int>(s.adaptChain.size()) < adaptDepth() && top.correction != 0) {
                    // The re-timed click worked, the one after it did not: fix that one too.
                    s.adaptChain.push_back({s.adaptAfterIndex, top.correction, 1, s.lastProbeDeathTick, 0});
                } else {
                    ++s.adaptChain.back().attempt;
                }
                handled = true;
            }
            if (handled) ++s.adaptTrials;
        }
        if (!handled) settleTrial(s, !died);
        s.pendingOutcome = ProbeOutcome::None;
    }

    while (true) {
        auto const trial = s.planner.next();
        if (!trial) {
            if (!s.liveSim) {
                if (s.deepMode && beginNextDeepJob(s)) continue;
                if (s.deepMode && s.verifyMode) {
                    finishEdgePhase(play, s);
                    return;
                }
            }
            s.stage = Stage::Complete;
            return;
        }
        auto const built = buildTrial(s, *trial);
        if (built == TrialBuild::Ok) break;
        if (built == TrialBuild::Skip && !s.adaptChain.empty()) {
            ++s.adaptChain.back().attempt;
            continue;
        }
        if (built == TrialBuild::LevelExhausted && s.adaptChain.size() > 1) {
            // Nothing at this depth works with the parent's candidate: the parent's next one.
            s.adaptChain.pop_back();
            ++s.adaptChain.back().attempt;
            continue;
        }
        // No correction saves this trial.
        s.adaptChain.clear();
        settleTrial(s, false);
    }
    auto const* trial = &s.activeTrial;

    // The trial can only come back onto the baseline path after every input it moved.
    s.trialConvergedSince = -1;
    s.trialConvergeAfter = std::numeric_limits<int>::min();
    s.trialEarliestMove = std::min(s.source.events[trial->index].tick, s.currentTrial[trial->index].tick);
    for (size_t i = 0; i < s.currentTrial.size(); ++i) {
        auto const& moved = s.currentTrial[i];
        auto const& original = s.source.events[i];
        if (moved.tick != original.tick || moved.frac != original.frac || moved.cbf != original.cbf) {
            s.trialConvergeAfter = std::max({s.trialConvergeAfter, moved.tick, original.tick});
            s.trialEarliestMove = std::min({s.trialEarliestMove, moved.tick, original.tick});
        }
    }
    if (s.trialConvergeAfter != std::numeric_limits<int>::min()) s.trialConvergeAfter += std::max(0, s.activeOffset());

    // Noclip run: until the next hazard the recorded attempt touched, a moved click must not touch one either.
    s.trialHitLimit = std::numeric_limits<int>::max();
    {
        int const earliest = s.trialEarliestMove;
        s.trialHitFrom = earliest;
        s.trialVerifyUntil = earliest + std::min(0, s.activeOffset());
        auto const it = std::lower_bound(s.source.hits.begin(), s.source.hits.end(), earliest);
        if (it != s.source.hits.end()) s.trialHitLimit = *it;
    }
    sortByTick(s.currentTrial);

    // A live simulation always starts from its single saved state.
    size_t expected = 0;
    bool trimmed = false;
    if (s.liveSim) {
        expected = s.liveArrayBase + 1;
        trimmed = trimArrayTo(play, expected, s.liveAnchor);
    } else {
        trimmed = trimCheckpoints(play, s);
        expected = s.checkpoints.empty() ? 0 : s.checkpoints.back().arrayCount;
    }
    if (!trimmed) {
        size_t const actual = play->m_checkpointArray ? play->m_checkpointArray->count() : 0;
        abortAnalysis(fmt::format(
            "The practice checkpoint list does not match: the game has {} checkpoints, expected {}. See the Geode log.",
            actual, expected));
        return;
    }

    // The checkpoint must reproduce the baseline for the whole trial, not only up to the moved click.
    if (!s.liveSim && !s.checkpoints.empty() && !s.baselineTrace.empty()) {
        int const checkpoint = s.checkpoints.back().tick;
        int const needed = std::min(s.trialEndTick, s.trialHitLimit);
        auto const good = s.checkpointGoodUntil.find(checkpoint);
        if (good == s.checkpointGoodUntil.end() || good->second < needed) {
            s.verifyingCheckpoint = true;
            ++s.checkpointControls;
            s.currentTrial = s.source.events;
            sortByTick(s.currentTrial);
            s.trialEndTick = std::min(s.source.endTick, needed + kControlExtraTicks);
            s.trialVerifyUntil = s.trialEndTick;
            s.trialHitFrom = std::numeric_limits<int>::max();
            s.trialHitLimit = std::numeric_limits<int>::max();
            s.trialConvergeAfter = std::numeric_limits<int>::max();
        }
    }
}

// Arms the replay of the current event list from the newest checkpoint and re-presses the buttons that are held
// at that point.
void beginSegment(PlayLayer* play, Session& s) {
    // A replay from the level start has no checkpoint boundary.
    bool const hasCheckpoint = !s.checkpoints.empty();
    int const startTick = hasCheckpoint ? s.checkpoints.back().tick : std::numeric_limits<int>::min();
    if (hasCheckpoint && currentTick(play) != startTick) {
        abortAnalysis(fmt::format(
            "After loading the checkpoint the level clock is at raw tick {}, expected {}. See the Geode log.",
            currentTick(play), startTick));
        return;
    }
    auto const& events = s.stage == Stage::Probing ? s.currentTrial : s.source.events;

    s.replayCursor = 0;
    if (hasCheckpoint) {
        while (s.replayCursor < events.size() && injectTick(s, events[s.replayCursor]) <= startTick)
            ++s.replayCursor;
    }
    s.lastInjectedTick = -1;
    if (s.stage == Stage::BaselineReplay) {
        // A new baseline run records a new reference trace and its own corrections.
        s.baselineTrace.clear();
        s.baselineDivergeTick = -1;
        s.source.fixes.clear();
        s.fixRun = 0;
        s.fixRunLastTick = -100;
        s.fixesGaveUp = false;
        s.fixLargest = 0.f;
    }

    // Buttons held at the start of the segment.
    std::map<std::pair<int, bool>, bool> held = s.liveSim ? s.liveAnchorHeld : std::map<std::pair<int, bool>, bool>{};
    for (size_t i = 0; i < s.replayCursor; ++i) held[{events[i].button, events[i].player1}] = events[i].down;

    s.injecting = true;
    for (auto const& [key, isDown] : held) {
        if (isDown) play->handleButton(true, key.first, key.second);
    }
    s.injecting = false;

    log::info("Replay segment armed: {}, checkpoint tick {}, skipped {} of {} inputs, next tick {}, offset {}",
        hasCheckpoint ? "checkpoint" : "level start", hasCheckpoint ? startTick : -1,
        s.replayCursor, events.size(),
        s.replayCursor < events.size() ? events[s.replayCursor].tick : -1, s.activeOffset());
}

// Click Between Frames inputs are replayed by the mod itself.
void warnAboutCbf() {
    static bool warned = false;
    if (warned) return;
    auto* cbf = cbfMod();
    if (!cbf || cbf->getSettingValue<bool>("soft-toggle")) return;
    if (!cbf->getSettingValue<bool>("physics-bypass")) return;
    warned = true;
    log::warn("Click Between Frames Physics Bypass is on: the step count depends on real frame times, replays may desync.");
    Notification::create("Turn off CBF Physics Bypass: replays may desync", NotificationIcon::Warning)->show();
}

namespace {

constexpr int kRandomTriggerID = 1912;
constexpr int kAdvancedRandomTriggerID = 2068;

// Random triggers pick differently in every attempt: the replay may not repeat the run.
void warnAboutRandomTriggers(PlayLayer* play) {
    if (!play->m_objects) return;
    int count = 0;
    for (auto* object : CCArrayExt<GameObject*>(play->m_objects)) {
        if (object->m_objectID == kRandomTriggerID || object->m_objectID == kAdvancedRandomTriggerID) ++count;
    }
    if (count == 0) return;
    log::warn("The level has {} random triggers", count);
    Notification::create(fmt::format("This level has {} random trigger{}: the count may fail", count, count == 1 ? "" : "s"),
        NotificationIcon::Warning)->show();
}

} // namespace

// Returns an error text, or an empty string when the analysis was started.
std::string startAnalysis(PlayLayer* play, RunRecord run, bool deep) {
    auto& s = Session::get();
    if (play->m_levelSettings && play->m_levelSettings->m_platformerMode)
        return "Platformer levels are not supported by this analyzer yet.";
    if (run.empty())
        return "No recorded input found. Press Start Recording in the pause menu, play an attempt in normal mode, then press Count Frame Windows.";

    if (auto const mismatch = startMismatch(play, run.startX); !mismatch.empty()) return mismatch;
    warnAboutRandomTriggers(play);

    sortByTick(run.events);
    if (run.endTick <= 0) run.endTick = run.events.back().tick + cfg::lookaheadTicks();

    s.resetProbeState();
    s.layoutOff = false;
    s.source = std::move(run);
    s.baseOffset = 0;
    configureUnits(s, play, s.source.events);
    warnAboutCbf();
    s.recording.clear();
    s.recordingHits.clear();
    s.recordingRespawns.clear();
    s.recordHeld.clear();
    s.recordingDeathTick = -1;
    s.deepMode = deep;
    s.edgeCheckRan = false;
    clearHitborders(play);
    if (!s.borderMode) s.borders = {};
    if (deep) {
        // The results stay: Deep Count only adds to them, and a stopped one changes nothing.
        s.deepBase = s.windows;
    } else {
        s.deepCounted = false;
        s.hasResults = false;
        s.windows.clear();
        s.dependent.clear();
        s.edgeUncertain.clear();
        s.deep.clear();
    }
    resetDisplay(play);

    s.sourceTrace = s.source.trace;
    s.wasPracticeMode = play->m_isPracticeMode;
    freezePlayerStats(play, false);
    s.stage = Stage::BaselineReplay;
    s.startFromBeginning = true;
    std::string respawns;
    for (auto const& r : s.source.respawns) respawns += fmt::format(" {}(after {})", r.tick, r.reached);
    log::info("Starting baseline replay: {} inputs, first raw tick {}, end raw tick {}, practice respawns:{}",
        s.source.events.size(), s.source.events.front().tick, s.source.endTick, respawns.empty() ? " none" : respawns);

    muteAudio(true);
    if (!play->m_isPracticeMode) play->togglePracticeMode(true);
    play->removeAllCheckpoints();
    return {};
}

// "Count Frame Windows" on the end screen: the game restarts the level a moment after its Retry button.
void queuePendingAnalysis() {
    auto& s = Session::get();
    if (!s.pendingAnalysis) return;
    auto run = std::move(*s.pendingAnalysis);
    s.pendingAnalysis.reset();
    Loader::get()->queueInMainThread([run = std::move(run)]() mutable {
        auto* play = PlayLayer::get();
        if (!play || Session::get().drivesInput()) return;
        auto const error = startAnalysis(play, std::move(run));
        if (!error.empty()) FLAlertLayer::create("TrycsCounter", error, "OK")->show();
    });
}

bool startReplay(PlayLayer* play) {
    auto& s = Session::get();
    if (!play || !s.hasResults || s.resultsKey != s.levelKey || s.source.empty()) return false;
    if (auto const mismatch = startMismatch(play, s.source.startX); !mismatch.empty()) {
        FLAlertLayer::create("TrycsCounter", mismatch, "OK")->show();
        return false;
    }

    // Keep the input alignment that the baseline replay settled on.
    auto const alignment = s.alignmentIndex;
    s.resetProbeState();
    s.alignmentIndex = alignment;
    if (play->m_isPracticeMode) play->togglePracticeMode(false);
    play->removeAllCheckpoints();
    s.checkpoints.clear();
    freezePlayerStats(play, true);
    s.stage = Stage::Replay;
    s.startFromBeginning = true;
    return true;
}

} // namespace tc
