// The replay that counts: input injection, the baseline check, the end of a count.

#include "App.hpp"

namespace tc {

namespace {

// A baseline death this close to the recorded end counts as reaching it.
constexpr int kDeathToleranceTicks = 4;

// A replay that leaves the attempt this soon after a practice respawn left it
// because of the respawn (8 physics frames).
constexpr int kRespawnBlameTicks = 8 * cfg::kRawTicksPerPhysicsFrame;


// Describes the failed replay that followed the attempt the longest (not the
// last one tried: a worse offset usually breaks much earlier).
std::string describeDesync(Session const& s) {
    auto const& best = s.bestFailure;
    std::string nextInput = "none";
    if (best.cursor < s.source.events.size()) {
        auto const& next = s.source.events[best.cursor];
        nextInput = fmt::format("#{} at raw tick {} (button {}, {})",
            best.cursor, next.tick, next.button, next.down ? "press" : "release");
    }

    std::string offsets;
    for (size_t i = 0; i <= s.alignmentIndex; ++i) {
        if (i > 0) offsets += ", ";
        offsets += std::to_string(s.baseOffset + kAlignmentCandidates[i]);
    }

    auto message = fmt::format(
        "The replay left the recorded attempt at raw tick {} (frame {}) and died at raw tick {}. Best replay offset: "
        "{} of {} tried; replayed {}/{} inputs, next input: {}.",
        best.reach, cfg::ticksToFrames(best.reach), best.deathTick, best.offset, offsets, best.cursor,
        s.source.events.size(), nextInput);

    // A practice respawn before this place: right after it the checkpoint put
    // the player back a little differently than playing straight through; later,
    // the life before the respawn had already been here and could have left
    // orbs, pads or triggers used.
    for (auto it = s.source.respawns.rbegin(); it != s.source.respawns.rend(); ++it) {
        if (best.reach < it->tick) continue;
        if (best.reach - it->tick <= kRespawnBlameTicks) {
            message += fmt::format(" That is right after a practice respawn (raw tick {}): the checkpoint restored the "
                "player differently than playing straight through does.", it->tick);
            break;
        }
        if (best.reach <= it->reached) {
            message += fmt::format(" The attempt respawned at raw tick {} after a life that had already passed this "
                "place (it got to raw tick {}): practice mode may have kept an orb, pad or trigger of that life used, "
                "so the attempt went differently than playing straight through.", it->tick, it->reached);
            break;
        }
    }

    if (Loader::get()->getLoadedMod("zilko.xdbot") && !cfg::flag("state-fixes")) {
        message += " If xdBot played this attempt with 'Frame Fixes', turn on the setting 'Replay corrections': the "
            "replay then follows the positions the bot set.";
    } else if (s.fixesGaveUp) {
        message += fmt::format(" The replay needed corrections on many ticks in a row (largest {:.2f} units) and gave "
            "up: the run really went elsewhere (a missed or extra click, a random trigger), not just drifted.",
            s.fixLargest);
    }
    if (auto const between = s.betweenStepInputs(); between > 0) {
        message += fmt::format(
            " {} of {} inputs were registered between physics steps (Click Between Steps) and cannot be replayed exactly. "
            "Keep 'Force click on steps' enabled and record the attempt again.",
            between, s.source.events.size());
    }
    return message;
}

// Next replay alignment to try: the next candidate, or, once all were tried,
// the one that followed the recorded attempt the longest. False: nothing left.
bool pickNextAlignment(Session& s) {
    if (s.alignmentIndex + 1 < kAlignmentCandidates.size()) {
        ++s.alignmentIndex;
        s.baselineRetry = true;
        return true;
    }
    size_t best = s.alignmentIndex;
    int bestScore = kAlignmentDied;
    for (size_t i = 0; i < s.alignmentScores.size(); ++i) {
        if (s.alignmentScores[i] > bestScore) {
            bestScore = s.alignmentScores[i];
            best = i;
        }
    }
    if (bestScore < 0 || best == s.alignmentIndex) return false;
    s.alignmentIndex = best;
    s.baselineFinal = true;
    s.baselineRetry = true;
    return true;
}

} // namespace

void onBaselineDeath(Session& s, int deathTick, bool player1) {
    if (!s.source.hits.empty() && deathTick >= s.source.hits.front() - kDeathToleranceTicks) {
        abortAnalysis("This attempt was played with noclip and touched a hazard. Turn noclip on again while the frame windows are counted.");
        return;
    }
    // A macro ends where its run ends: a death after its last input is that end.
    if (s.source.openEnd && s.replayCursor >= s.source.events.size()) {
        log::info("Macro run ends with a death at raw tick {}", deathTick);
        s.source.endTick = deathTick - 1;
        s.source.endKind = RunEnd::Death;
        s.source.openEnd = false;
        s.replayDiedAtEnd = true;
        return;
    }
    log::error("Baseline death: tick={}, expected end={}, injected={}/{}, player={}, offset={}",
        deathTick, s.source.endTick, s.replayCursor, s.source.events.size(), player1 ? "P1" : "P2", s.activeOffset());

    if (deathTick >= s.source.endTick - kDeathToleranceTicks) {
        s.replayDiedAtEnd = true;
        return;
    }

    // Without decoration first: the same alignment with the full level.
    if (layoutFallback(s)) return;
    int const reach = s.baselineDivergeTick >= 0 ? s.baselineDivergeTick : deathTick;
    if (reach > s.bestFailure.reach) s.bestFailure = {reach, deathTick, s.activeOffset(), s.replayCursor};
    // The replay diverged from the recorded run. Try the next input alignment
    // before giving up: the recorder and the replay may differ by a half step.
    s.alignmentScores[s.alignmentIndex] = kAlignmentDied;
    if (!s.baselineFinal && pickNextAlignment(s)) {
        log::warn("Baseline replay died at raw tick {}; retrying with replay offset {}", deathTick, s.activeOffset());
        return;
    }
    abortAnalysis(describeDesync(s));
}

// The baseline replay reached the end of the run. It is used only if it also
// followed the recorded attempt: an alignment that survives on a different path
// would measure the windows of a different attempt. Returns false when another
// alignment is tried first (the level is reset from the start).
bool acceptBaseline(Session& s) {
    if (s.source.openEnd) {
        // A macro that completes the level: the run ends here.
        s.source.endTick = currentTick(PlayLayer::get());
        s.source.endKind = RunEnd::Completed;
        s.source.openEnd = false;
    }
    int const score = s.baselineDivergeTick < 0 ? kAlignmentMatched : s.baselineDivergeTick;
    if (score != kAlignmentMatched && layoutFallback(s)) return false;
    s.alignmentScores[s.alignmentIndex] = score;
    if (score != kAlignmentMatched && !s.baselineFinal && pickNextAlignment(s)) {
        log::warn("Baseline replay left the recorded attempt at raw tick {}; retrying with replay offset {}",
            score, s.activeOffset());
        return false;
    }
    s.baselineMismatchTick = s.baselineDivergeTick;
    if (!s.source.fixes.empty())
        log::info("Replay corrections: {} ticks, largest {:.4f} units{}", s.source.fixes.size(), s.fixLargest,
            s.fixesGaveUp ? " (stopped: the replay went elsewhere)" : "");
    if (s.baselineDivergeTick >= 0)
        log::warn("No replay offset follows the recorded attempt exactly; using offset {} (left it at raw tick {})",
            s.activeOffset(), s.baselineDivergeTick);
    else
        log::info("Baseline replay follows the recorded attempt (offset {})", s.activeOffset());
    return true;
}

namespace {

// "Frame Counting Complete" popup. The game is paused first so that nothing
// runs behind the popup; Replay resumes it and plays the counted run.
void showCountingComplete(std::string summary) {
    Loader::get()->queueInMainThread([summary = std::move(summary)] {
        auto* play = PlayLayer::get();
        if (!play) return;
        if (!play->m_isPaused) play->pauseGame(false);

        createQuickPopup("Frame Counting Complete", summary, "Close", "Replay", [](FLAlertLayer*, bool replay) {
            if (!replay) return;
            beginReplay(PlayLayer::get());
        });
    });
}

} // namespace

void finishAnalysis(PlayLayer* play) {
    auto& s = Session::get();
    bool const deep = s.deepMode;
    // Leave the Complete stage first: toggling practice mode below can reset the level.
    s.stage = Stage::Recording;
    if (deep) {
        s.windows = s.deepBase;
        finishDeep(s);
    } else {
        s.windows = s.planner.results();
        s.edgeUncertain.assign(s.source.events.size(), 0);
        s.resultsMismatchTick = s.baselineMismatchTick;
        s.resultsName.clear();
    }
    s.dependent.resize(s.source.events.size(), 0);
    s.edgeUncertain.resize(s.source.events.size(), 0);
    s.deep.resize(s.source.events.size(), DeepRange{});
    s.sourceSwift = swiftLevelsOf(s.source.events);
    s.hasResults = true;
    s.resultsLevelID = currentLevelID(play);
    s.resultsKey = s.levelKey;
    s.resultsSaved = false;
    s.resetDisplay();
    saveLastCount(play);
    if (cfg::flag("auto-save-runs")) saveCurrentRun(play, s.resultsName);
    computeLStar();
    endAnalysisEnvironment(play);
    s.stage = idleStage(play);
    refreshDisplay(play);

    int measured = 0;
    int shown = 0;
    int oneFrame = 0;
    int twoFrames = 0;
    for (size_t i = 0; i < s.windows.size() && i < s.source.events.size(); ++i) {
        auto const& result = s.windows[i];
        if (!result.valid) continue;
        ++measured;
        if (!overlay::isShown(s.source.events[i], result)) continue;
        ++shown;
        double const frames = cfg::frames(result);
        if (frames <= 1.0) ++oneFrame;
        if (frames <= 2.0) ++twoFrames;
    }

    int dependent = 0;
    int ranged = 0;
    int disconnected = 0;
    int easier = 0;
    int uncertain = 0;
    for (size_t i = 0; i < s.dependent.size(); ++i) {
        dependent += s.dependent[i] ? 1 : 0;
        if (i < s.deep.size() && s.deep[i].ranged()) ++ranged;
        if (i < s.deep.size() && s.deep[i].disconnected()) ++disconnected;
        if (hasEasierApproach(s, i)) ++easier;
    }
    for (char flag : s.edgeUncertain) uncertain += flag ? 1 : 0;
    auto summary = deep
        ? fmt::format("Deep Count: {} checks in {} trials.\n{} clicks depend on the previous click (~, {} with a range), "
              "{} have disconnected windows (-), {} an easier approach (^).\nThe replay explains them at the top right.",
              s.deepJobsTotal, s.trialsFinished, dependent, ranged, disconnected, easier)
        : fmt::format("Measured {} inputs in {} trials.\nShown {}: 1-frame {}, up to 2 frames {}. Marked ~: {}.",
              measured, s.trialsFinished, shown, oneFrame, twoFrames, dependent);
    if (deep && s.edgeCheckRan)
        summary += fmt::format("\nEdge check: {} clicks differed on a re-check (marked ?).", uncertain);
    if (s.source.endKind != RunEnd::Completed)
        summary += "\nThe counted run did not finish the level, so the replay stops where it ended.";
    if (!s.resultsSaved) summary += "\nKeep it: TC > Save Run in the pause menu.";
    if (s.baselineMismatchTick >= 0)
        summary += fmt::format("\nWarning: the replay left your attempt at frame {}; windows after it may be off.",
            cfg::ticksToFrames(s.baselineMismatchTick));
    log::info("Counting details: {} trials ended on the baseline path, {} checkpoint checks, {} checkpoint loads rejected, "
        "replay offset {}", s.trialsConverged, s.checkpointControls, s.checkpointRejections, s.activeOffset());

    log::info("{}", summary);
    if (queuedCountFinished(play, true, {})) return;
    showCountingComplete(std::move(summary));
}

// Injects the recorded inputs that are due at the current raw tick.
void injectDueInputs(PlayLayer* play, GJBaseGameLayer* layer, Session& s) {
    int const tick = currentTick(layer);
    if (tick == s.lastInjectedTick) return;
    if (s.lastInjectedTick < 0) log::info("First command tick of the replay segment: {}", tick);
    s.lastInjectedTick = tick;
    if (!layer->m_player1 || layer->m_player1->m_isDead || layer->m_levelEndAnimationStarted) return;

    auto const& events = s.stage == Stage::Probing ? s.currentTrial : s.source.events;
    while (s.replayCursor < events.size()) {
        auto const& event = events[s.replayCursor];
        // Click Between Frames inputs are applied inside the player update (replayCbfStep).
        if (event.cbf) break;
        int const target = injectTick(s, event);
        if (target > tick) break;
        if (tick - target > kInjectLateToleranceTicks) {
            // Injecting a badly missed input late would invalidate the replay.
            log::error("Missed input #{}: recorded raw tick {}, target {}, current raw tick {}, offset {}",
                s.replayCursor, event.tick, target, tick, s.activeOffset());
            stopDriving(fmt::format(
                "Replay missed input #{} (recorded at raw tick {}, the game is already at raw tick {}, offset {}). "
                "The replay clock does not match the recording; see the Geode log.",
                s.replayCursor, event.tick, tick, s.activeOffset()));
            break;
        }
        if (s.stage == Stage::BaselineReplay && s.source.fromMacro) {
            // A macro has no positions: its markers go where the replay makes the input.
            auto const point = playerPoint(layer, event.player1);
            s.source.events[s.replayCursor].x = point.x;
            s.source.events[s.replayCursor].y = point.y;
        }
        ++s.replayCursor;
        s.injecting = true;
        play->handleButton(event.down, event.button, event.player1);
        s.injecting = false;
        log::debug("Injected input #{} at raw tick {} (recorded {}): button={}, down={}, P1={}",
            s.replayCursor - 1, tick, event.tick, event.button, event.down, event.player1);
    }
}

void abortAnalysis(std::string const& reason) {
    auto& s = Session::get();
    if (s.liveSim) {
        abortLive(reason);
        return;
    }
    s.stage = Stage::Idle;
    endAnalysisEnvironment(PlayLayer::get());
    log::error("Frame-window analysis stopped: {}", reason);
    if (queuedCountFinished(PlayLayer::get(), false, reason)) return;
    FLAlertLayer::create("TrycsCounter", "Analysis stopped:\n" + reason, "OK")->show();
}

// Stops whatever drives the input: the analysis or the replay.
void stopDriving(std::string const& reason) {
    auto& s = Session::get();
    if (s.isAnalyzing()) {
        abortAnalysis(reason);
    } else if (s.stage == Stage::Replay) {
        s.stage = Stage::Idle;
        log::warn("Replay stopped: {}", reason);
        Notification::create("Replay stopped: " + reason, NotificationIcon::Warning)->show();
    }
}

} // namespace tc
