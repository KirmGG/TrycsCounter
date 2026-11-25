// Hooks of PlayLayer: level setup, resets, deaths, completion, display.

#include "../App.hpp"
#include <Geode/modify/PlayLayer.hpp>

#include <chrono>
#include <cmath>
#include <string>

namespace tc {

namespace {

// GD emits stale destroyPlayer callbacks while a level is being reset.
constexpr int kResetDeathIgnoreRawTicks = 32;

// Time left in a stage of the count, from how fast its progress moved so far.
// Only time the count runs is measured: a pause shows up as one long frame.
class StageClock {
    int m_stage = -1;
    double m_startFraction = 0.0;
    double m_elapsed = 0.0;
    double m_shownAt = -1.0;
    std::string m_shown;
    std::chrono::steady_clock::time_point m_last;

public:
    void reset() { m_stage = -1; }

    std::string timeLeft(int stage, double fraction) {
        auto const now = std::chrono::steady_clock::now();
        if (stage != m_stage || fraction < m_startFraction) {
            m_stage = stage;
            m_startFraction = fraction;
            m_elapsed = 0.0;
            m_shownAt = -1.0;
            m_shown = "Estimated time: ...";
        } else {
            m_elapsed += std::min(0.5, std::chrono::duration<double>(now - m_last).count());
        }
        m_last = now;

        // Twice a second, so the number does not flicker every frame.
        double const moved = fraction - m_startFraction;
        if (m_elapsed < 2.0 || moved <= 0.0 || m_elapsed - m_shownAt < 0.5) return m_shown;
        m_shownAt = m_elapsed;
        auto const left = static_cast<long long>(std::ceil(m_elapsed * (1.0 - fraction) / moved));
        if (left >= 3600) m_shown = fmt::format("Estimated time: {}h {:02}m", left / 3600, left / 60 % 60);
        else if (left >= 60) m_shown = fmt::format("Estimated time: {}m {:02}s", left / 60, left % 60);
        else m_shown = fmt::format("Estimated time: {}s", left);
        return m_shown;
    }
};

StageClock g_stageClock;

} // namespace
} // namespace tc

using namespace tc;

class $modify(TCPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* recorderHud = nullptr;
    };

    static void onModify(auto& self) {
        // First, so deaths of the trajectory copies never reach other mods.
        // Before noclip mods too: they swallow the call, and the analysis must see every hit.
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First - 500);
    }

    // GD keeps at most 50 practice checkpoints and drops the oldest when it stores
    // another one. A count needs them over the whole level, so the older ones sit
    // out of the list while the game stores the new one.
    void storeCheckpoint(CheckpointObject* checkpoint) {
        auto* array = m_checkpointArray;
        if (!Session::get().isAnalyzing() || !array || array->count() < 50) {
            PlayLayer::storeCheckpoint(checkpoint);
            return;
        }
        // Not createWithArray: it copies every object, and checkpoints cannot be copied.
        Ref<CCArray> older = CCArray::create();
        older->addObjectsFromArray(array);
        array->removeAllObjects();
        PlayLayer::storeCheckpoint(checkpoint);
        for (unsigned i = 0; i < older->count(); ++i) array->insertObject(older->objectAtIndex(i), i);
    }

    // The analysis resets by itself right after a death (much faster than the respawn delay).
    void delayedResetLevel() {
        auto& s = Session::get();
        if (s.isAnalyzing() && !s.liveSim) return;
        PlayLayer::delayedResetLevel();
    }

    // Counting: the level is not drawn, so its per-frame visibility work is
    // skipped (setting "Fast counting"; switched off if trials then differ).
    void updateVisibility(float dt) {
        auto const& s = Session::get();
        if (s.skipVisuals && s.isAnalyzing() && !s.liveSim) return;
        PlayLayer::updateVisibility(dt);
    }

    void playEndAnimationToPos(CCPoint position) {
        if (Session::get().trajSimulating) return;
        PlayLayer::playEndAnimationToPos(position);
    }

    void setupHasCompleted() {
        // The game resets the level inside setupHasCompleted: the counter labels
        // of the previous level must be forgotten before that.
        Session::get().resetHudPointers();
        PlayLayer::setupHasCompleted();
        auto& s = Session::get();
        s.layoutHidden.clear();
        s.layoutActive = false;
        s.resetProbeState();
        s.recording.clear();
        s.recordingHits.clear();
        s.recordingRespawns.clear();
        s.recordHeld.clear();
        s.recordingDeathTick = -1;
        s.resetHudPointers();
        s.recordingEnabled = false;
        s.checkpointTicks.clear();
        s.checkpointExtras.clear();
        s.loadingCheckpoint = nullptr;
        s.usableObjects.clear();
        s.usableObjectsFound = false;
        s.training = {};
        s.tickOffset = 0;
        s.resetCbfStep();
        s.resetTrajectory();
        s.pendingAnalysis.reset();
        s.resetLive();
        muteAudio(false);

        int const id = currentLevelID(this);
        std::string const key = saved::levelKeyOf(this);
        if (key != s.levelKey || id != s.levelID) {
            s.previousRun = {};
            s.previousCompletion = {};
            s.source = {};
            s.windows.clear();
            s.hasResults = false;
            s.resultsKey.clear();
            s.levelID = id;
            s.levelKey = key;
        }
        // The last count (or the newest saved run) shows its markers from the first attempt on.
        if (!s.hasResults) loadLevelResults(this);

        s.stage = idleStage(this);
        if (cfg::flag("live-counter") && !trajectoryLive() && !m_isPracticeMode)
            Notification::create("Live Counter measures clicks in practice mode", NotificationIcon::Warning)->show();
        // GD 2.2081 emits stale destroyPlayer callbacks during startup cleanup.
        s.deathIgnoreUntilTick = currentTick(this) + kResetDeathIgnoreRawTicks;
        forceClickOnSteps(this);
        log::debug("Level set up; death callbacks ignored until raw tick {}", s.deathIgnoreUntilTick);

        // Recording status in the bottom corner away from the counter.
        if (auto* label = CCLabelBMFont::create("", "bigFont.fnt")) {
            label->setScale(0.28f);
            bool const right = cfg::hudRight();
            label->setAnchorPoint({right ? 0.f : 1.f, 0.f});
            label->setOpacity(190);
            auto const size = CCDirector::sharedDirector()->getWinSize();
            label->setPosition({right ? 5.f : size.width - 5.f, 4.f});
            label->setZOrder(1000);
            if (m_uiLayer) m_uiLayer->addChild(label); else this->addChild(label);
            m_fields->recorderHud = label;
        }

        refreshDisplay(this);
    }

    void resetLevel() {
        auto& s = Session::get();
        int const tickBeforeReset = currentTick(this);
        noteStepDt(s);
        finalizeCbfStep(s);
        s.resetCbfStep();
        // The first reset of the player's own after a replay: the counters go
        // back to before it, and this reset counts as the next attempt.
        if (!s.drivesInput() && !s.liveSim) restorePlayerStats(this);

        // Loading the saved real attempt after a live simulation: nothing else may change.
        if (s.liveRestoring) {
            PlayLayer::resetLevel();
            applyCheckpointExtra(this);
            alignTickAfterReset(this);
            s.deathIgnoreUntilTick = currentTick(this) + kResetDeathIgnoreRawTicks;
            forceClickOnSteps(this);
            return;
        }

        if (s.stage == Stage::Recording && !s.recording.empty()) {
            bool const died = s.recordingDeathTick > 0;
            s.previousRun = recordedRun(this, died ? s.recordingDeathTick - 1 : tickBeforeReset,
                died ? RunEnd::Death : RunEnd::Paused);
        }

        if (s.stage == Stage::BaselineReplay && s.replayDiedAtEnd) {
            s.replayDiedAtEnd = false;
            if (acceptBaseline(s)) startProbing(s);
        }
        if (s.stage == Stage::BaselineReplay && s.baselineRetry) {
            s.baselineRetry = false;
            this->removeAllCheckpoints();
            s.checkpoints.clear();
            s.lastCheckpointTick = 0;
        }
        if (s.stage == Stage::Probing) advanceProbing(this, s);

        PlayLayer::resetLevel();
        applyCheckpointExtra(this);
        alignTickAfterReset(this);
        s.deathIgnoreUntilTick = currentTick(this) + kResetDeathIgnoreRawTicks;
        s.recordHeld.clear();
        forceClickOnSteps(this);
        // The level is at its start and has no checkpoints yet: switch off the decoration.
        if (s.stage == Stage::BaselineReplay && s.checkpoints.empty()) hideDecoration(this);
        // A live simulation must not wipe the markers and counters of the real attempt.
        if (!s.isAnalyzing() && !s.liveSim) {
            resetDisplay(this);
            // A practice respawn: clicks before the checkpoint are counted again, silently.
            s.displaySilentUntil = currentTick(this);
        }

        if (s.stage == Stage::Idle || s.stage == Stage::Recording) {
            if (m_isPracticeMode && s.stage == Stage::Recording) {
                // A practice respawn rewinds the attempt: keep the inputs before the checkpoint.
                int const tick = currentTick(this);
                std::erase_if(s.recording, [tick](InputEvent const& e) { return e.tick > tick; });
                std::erase_if(s.recordingHits, [tick](int hit) { return hit > tick; });
                std::erase_if(s.recordingRespawns, [tick](Respawn const& r) { return r.tick >= tick; });
                int const reached = s.recordingDeathTick > 0 ? s.recordingDeathTick : tickBeforeReset;
                if (!s.recording.empty()) s.recordingRespawns.push_back({tick, reached});
                std::erase_if(s.stepDts, [tick](StepDt const& e) { return e.tick > tick; });
                if (s.trace.size() > static_cast<size_t>(std::max(tick, 0))) s.trace.resize(static_cast<size_t>(std::max(tick, 0)));
                // The buttons as the kept inputs leave them; the game is asked
                // on the next step whether it still holds them.
                s.recordHeld.clear();
                for (auto const& e : s.recording) s.recordHeld[{e.button, e.player1}] = e.down;
                s.syncHeldAfterRespawn = !s.recording.empty();
                if (s.training.active) trainingRespawned();
                while (!s.liveJobs.empty() && s.liveJobs.back().index >= s.recording.size()) s.liveJobs.pop_back();
                while (!s.liveSnapshots.empty() && s.liveSnapshots.back().tick > tick) s.liveSnapshots.pop_back();
                s.dropTrajSnapshotsAfter(tick);
                dropLiveMeasures();
            } else {
                s.recording.clear();
                s.recordingHits.clear();
                s.recordingRespawns.clear();
                s.trace.clear();
                s.stepDts.clear();
                s.liveJobs.clear();
                s.liveSnapshots.clear();
                s.dropTrajSnapshotsAfter(std::numeric_limits<int>::min());
                dropLiveMeasures();
            }
            s.recordingDeathTick = -1;
            s.stage = idleStage(this);
            queuePendingAnalysis();
            return;
        }

        if (s.stage == Stage::BaselineReplay || s.stage == Stage::Probing || s.stage == Stage::Replay)
            beginSegment(this, s);
        if (s.stage == Stage::Complete) {
            if (s.liveSim) finishLive();
            else finishAnalysis(this);
        }
    }

    // Every checkpoint (the player's, the analyzer's, Live Counter snapshots) is
    // created here: remember the level tick it belongs to.
    CheckpointObject* createCheckpoint() {
        auto* checkpoint = PlayLayer::createCheckpoint();
        if (checkpoint) {
            Session::get().checkpointTicks[checkpoint] = currentTick(this);
            saveCheckpointExtra(this, checkpoint);
            pruneCheckpointTicks(this);
        }
        return checkpoint;
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        auto& s = Session::get();
        if (s.resettingFromStart) return;
        alignTickToCheckpoint(this, checkpoint);
        // Applied once the reset is done: it changes some of the state afterwards.
        s.loadingCheckpoint = checkpoint;
    }

    void resetLevelFromStart() {
        auto& s = Session::get();
        s.resettingFromStart = true;
        PlayLayer::resetLevelFromStart();
        s.resettingFromStart = false;
        s.tickOffset = 0;
        queuePendingAnalysis();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // Capture identity and time BEFORE the game's handler: it may replace
        // the active player pointers.
        auto& s = Session::get();
        // A trajectory copy hit a hazard: only the simulation needs to know.
        if (s.trajSimulating && player == s.trajSim.data() && object != m_anticheatSpike) {
            s.trajDead = true;
            return;
        }
        int const deathTick = currentTick(this);
        bool const wasPlayer1 = player && player == m_player1;
        bool const wasPlayer2 = player && player == m_player2;
        bool const wasCurrentPlayer = wasPlayer1 || wasPlayer2;
        bool const wasAnticheatSpike = object == m_anticheatSpike;

        PlayLayer::destroyPlayer(player, object);

        if (wasAnticheatSpike) return;
        if (deathTick < s.deathIgnoreUntilTick) {
            log::debug("Ignored reset-cleanup destroyPlayer callback at raw tick {} (ignore until {})",
                deathTick, s.deathIgnoreUntilTick);
            return;
        }
        // Delayed callbacks from an old, detached PlayerObject are not gameplay deaths.
        if (!wasCurrentPlayer) {
            log::debug("Ignored detached destroyPlayer callback at raw tick {}", deathTick);
            return;
        }

        if (s.isAnalyzing() && s.pendingReset != Session::PendingReset::None) {
            log::debug("Ignored death at raw tick {}: the trial is already decided", deathTick);
            return;
        }
        // Noclip (another mod) kept the player alive: a hit, not a death.
        bool const hit = player && !player->m_isDead;
        if (hit) {
            if (s.stage == Stage::Recording) {
                if (s.recordingHits.empty() || s.recordingHits.back() != deathTick) s.recordingHits.push_back(deathTick);
            } else if (s.stage == Stage::Probing && s.pendingOutcome == ProbeOutcome::None &&
                deathTick >= s.trialHitFrom && deathTick < s.trialHitLimit) {
                s.pendingOutcome = ProbeOutcome::Died;
                s.lastProbeDeathTick = deathTick;
                s.pendingReset = Session::PendingReset::Level;   // noclip: the game does not reset by itself
            }
            return;
        }
        if (s.stage == Stage::Recording) {
            // The first death ends the attempt; later callbacks belong to the dying player.
            if (s.recordingDeathTick < 0) s.recordingDeathTick = deathTick;
            log::debug("Captured real attempt death at raw tick {} ({})", deathTick, wasPlayer1 ? "P1" : "P2");
        } else if (s.stage == Stage::BaselineReplay) {
            onBaselineDeath(s, deathTick, wasPlayer1);
            // No waiting for the respawn: the next run starts at once.
            if (s.isAnalyzing() && !s.liveSim) s.pendingReset = Session::PendingReset::Level;
        } else if (s.stage == Stage::Probing) {
            if (!s.liveSim && s.pendingOutcome == ProbeOutcome::None && deathTick < s.trialVerifyUntil && !s.checkpoints.empty()) {
                rejectCheckpoint(s, deathTick, "died");
                s.pendingReset = Session::PendingReset::Level;
            } else if (s.pendingOutcome == ProbeOutcome::None) {
                s.pendingOutcome = ProbeOutcome::Died;
                s.lastProbeDeathTick = deathTick;
                if (!s.liveSim) s.pendingReset = Session::PendingReset::Level;
            }
        } else if (s.stage == Stage::Replay) {
            stopDriving(fmt::format("the replayed run died at raw tick {}", deathTick));
        }
    }

    void levelComplete() {
        auto& s = Session::get();
        noteStepDt(s);
        finalizeCbfStep(s);
        if (s.stage == Stage::Recording) {
            s.previousCompletion = recordedRun(this, currentTick(this), RunEnd::Completed);
            s.stage = Stage::Idle;
        } else if (s.isAnalyzing()) {
            if (s.stage == Stage::BaselineReplay) {
                if (acceptBaseline(s)) startProbing(s);
            } else if (s.pendingOutcome == ProbeOutcome::None) {
                s.pendingOutcome = ProbeOutcome::Survived;
            }
            // Resetting from inside levelComplete does not load the checkpoint: reset between frames.
            s.pendingReset = Session::PendingReset::Level;
            return;
        } else if (s.stage == Stage::Replay) {
            s.stage = Stage::Idle;
        }
        PlayLayer::levelComplete();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto& s = Session::get();
        startPendingReplay(this);

        // Checkpoints only once the replay segment runs: right after the analysis was
        // started the level still shows the old attempt.
        if (s.stage == Stage::BaselineReplay && !s.startFromBeginning && s.lastInjectedTick >= 0 &&
            m_player1 && !m_player1->m_isDead) {
            int const tick = currentTick(this);
            if (tick > 0 && tick - s.lastCheckpointTick >= cfg::checkpointTicks()) {
                size_t const before = m_checkpointArray ? m_checkpointArray->count() : 0;
                auto* checkpoint = this->markCheckpoint();
                size_t const after = m_checkpointArray ? m_checkpointArray->count() : 0;
                if (checkpoint && after > before) {
                    s.lastCheckpointTick = tick;
                    s.checkpoints.push_back({tick, checkpoint, after});
                } else {
                    log::warn("markCheckpoint did not add a checkpoint (list size {} -> {})", before, after);
                }
            }
        }

        // The counter: during the replay, in Live Counter mode, and in attempts on a
        // level with counted results (setting "Counter in attempts").
        bool const hud = !s.isAnalyzing() && (s.stage == Stage::Replay || cfg::flag("live-counter") ||
            (cfg::flag("hud-in-attempts") && resultsForLevel(this)));
        overlay::setHudVisible(this, hud);
        if (hud) overlay::updateHudStats(this);
        overlay::orientMarkers(this);
        {
            std::string status;
            double fraction = 0.0;
            int stage = 0;
            if (s.stage == Stage::BaselineReplay) {
                // A macro's end is not known yet: the level progress instead.
                fraction = s.source.openEnd ? this->getCurrentPercent() / 100.0
                                            : static_cast<double>(currentTick(this)) / std::max(1, s.source.endTick);
                // After the edge check the baseline runs again to place the checkpoints.
                bool const again = s.deepMode && s.edgeCheckRan;
                status = again ? "Placing checkpoints" : "Checking the replay";
                stage = again ? 1 : 0;
            } else if (s.stage == Stage::Probing && (s.deepMode || s.verifyMode)) {
                size_t const done = std::min(s.deepJobsTotal - s.deepJobs.size(), s.deepJobsTotal);
                status = fmt::format("{}: {} of {}", s.verifyMode ? "Checking edges" : "Deep Count", done, s.deepJobsTotal);
                fraction = static_cast<double>(done) / std::max<size_t>(1, s.deepJobsTotal);
                stage = s.verifyMode ? 2 : 3;
            } else if (s.stage == Stage::Probing) {
                size_t const done = s.planner.finishedInputs();
                status = fmt::format("Click {} of {}", std::min(done + 1, s.planner.inputCount()), s.planner.inputCount());
                fraction = static_cast<double>(done) / std::max<size_t>(1, s.planner.inputCount());
                stage = 4;
            }
            fraction = std::clamp(fraction, 0.0, 1.0);
            bool const counting = s.isAnalyzing() && !s.liveSim;
            std::string timeLeft;
            if (counting) timeLeft = g_stageClock.timeLeft(stage, fraction);
            else g_stageClock.reset();
            std::string const trials = s.stage == Stage::Probing ? fmt::format("{} trials", s.trialsFinished) : "";
            overlay::setCountingScreen(this, counting, status, fraction, timeLeft, trials);
        }
        revealPassedInputs(this);
        captureLiveSnapshot(this);
        trainingStep(this);

        if (auto* hud = m_fields->recorderHud) {
            std::string value;
            switch (s.stage) {
                case Stage::Recording:
                    value = liveMode() ? std::string("LIVE") : fmt::format("REC {} inputs", s.recording.size());
                    break;
                default:
                    break;
            }
            if (std::string(hud->getString()) != value) hud->setString(value.c_str());
        }
        overlay::placeCounterTitle(this);
    }

    void onQuit() {
        auto& s = Session::get();
        restorePlayerStats(this);
        s.layoutHidden.clear();
        s.layoutActive = false;
        s.resetProbeState();
        s.stage = Stage::Idle;
        s.resetLive();
        s.resetCbfStep();
        s.resetTrajectory();
        s.pendingAnalysis.reset();
        s.checkpointTicks.clear();
        s.checkpointExtras.clear();
        s.loadingCheckpoint = nullptr;
        s.usableObjects.clear();
        s.usableObjectsFound = false;
        s.training = {};
        s.tickOffset = 0;
        s.deathIgnoreUntilTick = 0;
        s.resetHudPointers();
        muteAudio(false);
        PlayLayer::onQuit();
    }
};
