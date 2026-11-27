// Live Counter: windows of the clicks of the attempt being played.

#include "App.hpp"

namespace tc {

namespace {

// Live Counter: raw ticks between saved game states (45 frames).
constexpr int kLiveSnapshotTicks = 90;

// Live Counter: clicks further than this from their checkpoint are skipped (60 s).
constexpr int kLiveMaxSimRawTicks = 60 * 480;

// Live Counter, trajectory mode: most trials one click may take.
constexpr int kTrajMaxTrials = 400;

} // namespace

// The window of a click is decided by simulating shifted copies of it.

void captureLiveSnapshot(PlayLayer* play) {
    auto& s = Session::get();
    if (!liveMode() || trajectoryLive() || !play->m_isPracticeMode || s.stage != Stage::Recording || s.liveSim) return;
    if (!play->m_player1 || play->m_player1->m_isDead || play->m_levelEndAnimationStarted || play->m_isPaused) return;

    int const tick = currentTick(play);
    if (!s.liveSnapshots.empty() && tick - s.liveSnapshots.back().tick < kLiveSnapshotTicks) return;

    auto* checkpoint = play->createCheckpoint();
    if (!checkpoint) return;
    s.liveSnapshots.push_back({tick, checkpoint});

    size_t const ring = static_cast<size_t>(
        std::clamp((cfg::liveVerdictDelayTicks() + cfg::maxShiftTicks()) / kLiveSnapshotTicks + 4, 4, 24));
    while (s.liveSnapshots.size() > ring) s.liveSnapshots.pop_front();
}

namespace {

void showLiveResult(PlayLayer* play, InputEvent const& event, WindowResult const& result, int swift = 0) {
    auto& s = Session::get();
    if (!overlay::isShown(event, result)) return;
    double const frames = cfg::frames(result);
    overlay::showMarker(play, event, result, showSymbols() ? clickMarks(false, swift) : std::string{});
    size_t const bucket = overlay::bucketOf(frames);
    ++s.visibleCounts[bucket];
    s.lastShownMs = windowMs(frames, cfg::windowFps());
    overlay::updateCounterLabels();
    overlay::pulseBucket(bucket);
    overlay::flashLastClick();
    overlay::playDing(frames);
}

} // namespace

// Called from the reset hook when the planner has measured the input.
void finishLive() {
    auto& s = Session::get();
    auto const& results = s.planner.results();
    s.liveHasResult = s.liveIndex < results.size() && results[s.liveIndex].valid;
    if (s.liveHasResult) s.liveResult = results[s.liveIndex];
    else log::debug("Live input #{} could not be measured", s.liveBase + s.liveIndex);
    s.liveSim = false;
    s.liveRestorePending = true;
    s.stage = Stage::Recording;
    s.pendingOutcome = ProbeOutcome::None;
    s.injecting = false;
}

// Puts the real attempt back where the simulation started and shows the result.
void restoreLive(PlayLayer* play) {
    auto& s = Session::get();
    s.liveRestorePending = false;
    s.stage = Stage::Recording;
    muteAudio(false);
    if (s.liveStash) {
        auto stash = std::move(*s.liveStash);
        s.liveStash.reset();
        s.source = std::move(stash.source);
        s.units = stash.units;
        s.stepTicks = stash.stepTicks;
        s.precise = stash.precise;
        s.hasResults = stash.hasResults;
        s.baseOffset = stash.baseOffset;
        s.alignmentIndex = stash.alignmentIndex;
    }

    if (play && s.liveRestore && play->m_checkpointArray) {
        auto* array = play->m_checkpointArray;
        // Back to the player's own checkpoints: drop the saved state and anything the game added meanwhile.
        if (!trimArrayTo(play, s.liveArrayBase, s.liveAnchor))
            log::warn("Live Counter: the checkpoint list has {} entries, expected {}", array->count(), s.liveArrayBase);

        // The game restores the last checkpoint on a reset: make the saved real state the last one.
        array->addObject(s.liveRestore.data());
        s.liveRestoring = true;
        play->resetLevel();
        s.liveRestoring = false;
        array->removeLastObject();
        play->m_attempts = s.liveSavedAttempts;

        s.recordHeld = s.liveHeld;
        s.injecting = true;
        for (auto const& [key, isDown] : s.liveHeld) {
            if (isDown) play->handleButton(true, key.first, key.second);
        }
        s.injecting = false;
    }
    s.liveRestore = nullptr;
    s.liveAnchor = nullptr;

    size_t const index = s.liveBase + s.liveIndex;
    if (play && s.liveHasResult && index < s.recording.size())
        showLiveResult(play, s.recording[index], s.liveResult, swiftLevelAt(s.recording, index));
    s.liveHasResult = false;
}

namespace {

void startLiveProbe(PlayLayer* play, LiveJob const& job) {
    auto& s = Session::get();
    auto* array = play->m_checkpointArray;
    if (!array) return;
    auto const& event = s.recording[job.index];

    // Newest saved state that is early enough for the earliest shift of this click.
    int const wantedTick = event.tick - cfg::maxShiftTicks();
    geode::Ref<CheckpointObject> anchor;
    int anchorTick = 0;
    for (auto it = s.liveSnapshots.rbegin(); it != s.liveSnapshots.rend(); ++it) {
        if (it->tick <= wantedTick) {
            anchor = it->object;
            anchorTick = it->tick;
            break;
        }
    }
    if (!anchor || job.verdictTick - anchorTick > kLiveMaxSimRawTicks) {
        log::debug("Live input #{} skipped: no saved state before raw tick {}", job.index, wantedTick);
        return;
    }

    // Only the inputs after the saved state are replayed; the buttons held at that point are re-pressed.
    size_t base = 0;
    std::map<std::pair<int, bool>, bool> anchorHeld;
    while (base < s.recording.size() && s.recording[base].tick <= anchorTick) {
        anchorHeld[{s.recording[base].button, s.recording[base].player1}] = s.recording[base].down;
        ++base;
    }
    if (job.index < base) return;

    s.liveRestore = play->createCheckpoint();
    if (!s.liveRestore) return;

    // The counted results of the level stay: they are put back in restoreLive.
    if (!s.liveStash)
        s.liveStash = Session::LiveStash{s.source, s.units, s.stepTicks, s.precise, s.hasResults, s.baseOffset, s.alignmentIndex};
    s.resetProbeState();
    s.baseOffset = 0;
    warnAboutCbf();
    s.hasResults = false;
    s.trialBase.clear();
    s.freeFlight.clear();

    s.source = {};
    s.source.events.assign(s.recording.begin() + static_cast<std::ptrdiff_t>(base), s.recording.end());
    s.source.endTick = job.verdictTick;
    s.source.endKind = RunEnd::Paused;
    configureUnits(s, play, s.source.events);

    auto config = plannerConfig(s, job.verdictTick, RunEnd::Paused);
    config.minInputTick = (anchorTick + 1) * s.units;
    config.anchorNext = false;
    config.controlTrial = true;
    config.onlyIndex = static_cast<int>(job.index - base);
    s.planner.begin(plannerInputs(s.source.events, s.units), config);

    s.liveBase = base;
    s.liveIndex = job.index - base;
    s.liveAnchorHeld = std::move(anchorHeld);
    s.liveHeld = s.recordHeld;
    s.liveArrayBase = array->count();
    s.liveAnchor = anchor.data();
    array->addObject(anchor.data());
    s.checkpoints.clear();
    s.checkpoints.push_back({anchorTick, anchor.data(), array->count()});

    s.liveSavedAttempts = play->m_attempts;
    s.liveStarted = std::chrono::steady_clock::now();
    s.wasPracticeMode = true;
    s.trialsFinished = 0;
    s.pendingOutcome = ProbeOutcome::None;

    muteAudio(true);
    s.liveSim = true;
    s.stage = Stage::Probing;
    play->resetLevel();   // loads the saved state and arms the control trial
}

// Like "show trajectory", but in step with the real attempt: a copy of the players is kept for every physics
// step.

PlayerObject* createTrajectoryPlayer(PlayLayer* play) {
    auto& s = Session::get();
    if (!s.trajPool.empty()) {
        auto player = s.trajPool.back();
        s.trajPool.pop_back();
        return player.data();
    }
    auto* player = PlayerObject::create(1, 1, play, play, true);
    if (!player) return nullptr;
    player->setVisible(false);
    player->setID("trajectory-player"_spr);
    if (play->m_objectLayer) play->m_objectLayer->addChild(player);
    return player;
}

void copyPlayerState(PlayerObject* dst, PlayerObject* src) {
    if (!dst || !src) return;
    dst->copyAttributes(src);
    dst->setPosition(src->getPosition());
    dst->m_lastPosition = src->m_lastPosition;
    dst->m_gravityMod = src->m_gravityMod;
    dst->m_isOnGround = src->m_isOnGround;
    dst->m_yVelocity = src->m_yVelocity;
    dst->m_playerSpeed = src->m_playerSpeed;
    dst->m_vehicleSize = src->m_vehicleSize;
    dst->m_isUpsideDown = src->m_isUpsideDown;
    dst->m_isShip = src->m_isShip;
    dst->m_isBird = src->m_isBird;
    dst->m_isBall = src->m_isBall;
    dst->m_isDart = src->m_isDart;
    dst->m_isRobot = src->m_isRobot;
    dst->m_isSpider = src->m_isSpider;
    dst->m_isSwing = src->m_isSwing;
    dst->m_isSideways = src->m_isSideways;
    dst->m_isGoingLeft = src->m_isGoingLeft;
    dst->m_isPlatformer = src->m_isPlatformer;
    dst->m_isDashing = false;
    dst->m_holdingButtons = src->m_holdingButtons;
    dst->m_holdingLeft = src->m_holdingLeft;
    dst->m_holdingRight = src->m_holdingRight;
    dst->m_jumpBuffered = src->m_jumpBuffered;
    dst->m_stateJumpBuffered = src->m_stateJumpBuffered;
    if (dst->m_touchingRings) dst->m_touchingRings->removeAllObjects();
    resetCollisionLog(dst);
    dst->setVisible(false);
}

// The player an input moves: player 2 only in dual mode.
bool targetsPlayer1(InputEvent const& event, bool dual) {
    return event.player1 || !dual;
}

constexpr size_t kLiveStateRing = 4096;

void giveTrajectoryPlayer(geode::Ref<PlayerObject>& player) {
    if (player) Session::get().trajPool.push_back(player);
    player = nullptr;
}

} // namespace

bool holdingJump(PlayerObject* player) {
    auto const it = player->m_holdingButtons.find(1);
    return it != player->m_holdingButtons.end() && it->second;
}

namespace {

LiveState liveStateOf(PlayerObject* player, int tick) {
    LiveState state;
    state.tick = tick;
    if (!player) return state;
    auto const pos = player->getPosition();
    state.x = pos.x;
    state.y = pos.y;
    state.vy = player->m_yVelocity;
    state.size = player->m_vehicleSize;
    unsigned bit = 0;
    auto put = [&](bool value) {
        if (value) state.flags |= 1u << bit;
        ++bit;
    };
    put(player->m_isOnGround);
    put(player->m_isUpsideDown);
    put(player->m_isShip);
    put(player->m_isBird);
    put(player->m_isBall);
    put(player->m_isDart);
    put(player->m_isRobot);
    put(player->m_isSpider);
    put(player->m_isSwing);
    put(player->m_isDashing);
    put(player->m_isSideways);
    put(player->m_isGoingLeft);
    put(holdingJump(player));
    put(player->m_isDead);
    return state;
}

bool sameState(LiveState const& a, LiveState const& b) {
    return a.flags == b.flags && std::abs(a.x - b.x) <= 1e-3f && std::abs(a.y - b.y) <= 1e-3f &&
        std::abs(a.vy - b.vy) <= 1e-3 && std::abs(a.size - b.size) <= 1e-4f;
}

LiveState const* realState(Session const& s, bool player1, int tick) {
    auto const& ring = s.liveStates[player1 ? 0 : 1];
    if (ring.empty() || tick < 0) return nullptr;
    auto const& state = ring[static_cast<size_t>(tick / 2) % ring.size()];
    return state.tick == tick ? &state : nullptr;
}

void pressOrRelease(PlayerObject* player, InputEvent const& event) {
    auto const button = static_cast<PlayerButton>(event.button);
    if (event.down) player->pushButton(button);
    else player->releaseButton(button);
}

// One physics step of a trajectory copy.
void simulateStep(PlayLayer* play, PlayerObject* sim, std::vector<InputEvent> const& due, float dt,
    std::map<EnhancedGameObject*, ActivationFlags>& used) {
    auto& s = Session::get();
    s.trajSimulating = true;
    s.trajSim = sim;
    s.simRunFlags = &used;

    // Orbs and pads the copy knows get the copy's flags for the whole step, before any input can activate them.
    auto sandbox = [&](EnhancedGameObject* object) {
        if (!object || s.simSavedFlags.contains(object)) return;
        s.simSavedFlags[object] = {object->m_activated, object->m_activatedByPlayer1, object->m_activatedByPlayer2};
        ActivationFlags flags{false, false, false};
        if (auto const it = used.find(object); it != used.end()) flags = it->second;
        object->m_activated = flags[0];
        object->m_activatedByPlayer1 = flags[1];
        object->m_activatedByPlayer2 = flags[2];
    };
    for (auto const& entry : used) sandbox(entry.first);
    if (sim->m_touchingRings) {
        for (auto* ring : CCArrayExt<RingObject*>(sim->m_touchingRings)) sandbox(ring);
    }

    std::vector<InputEvent const*> between;
    for (auto const& event : due) {
        if (event.cbf) between.push_back(&event);
        else pressOrRelease(sim, event);
    }

    bool split = false;
    float lastSub = dt;
    auto const startPos = sim->getPosition();
    if (between.empty()) {
        sim->update(dt);
    } else {
        bool const startedOnGround = sim->m_isOnGround;
        split = cbfCanSplit(sim);
        double elapsed = 0.0;
        for (size_t k = 0; k <= between.size() && !s.trajDead; ++k) {
            bool const endStep = k == between.size();
            if (k > 0) pressOrRelease(sim, *between[k - 1]);
            double factor = 0.0;
            if (endStep) {
                factor = std::max(kCbfSmallestFactor, 1.0 - elapsed);
            } else {
                factor = std::clamp(between[k]->frac - elapsed, kCbfSmallestFactor, 1.0);
                elapsed = between[k]->frac;
            }
            float const sub = static_cast<float>(dt * factor);
            lastSub = sub;
            if (split) {
                sim->update(sub);
                if (!endStep && !s.trajDead) {
                    if (k == 0 && ((sim->m_yVelocity < 0) ^ sim->m_isUpsideDown)) sim->m_isOnGround = startedOnGround;
                    if (!sim->m_isOnSlope || sim->m_isDart) play->checkCollisions(sim, 0.f, true);
                    else play->checkCollisions(sim, dt, true);
                    sim->updateRotation(sub);
                    resetCollisionLog(sim);
                }
            } else if (endStep) {
                sim->update(dt);
            }
        }
    }
    if (!s.trajDead) {
        resetCollisionLog(sim);
        play->checkCollisions(sim, dt, false);
    }
    if (!s.trajDead) {
        if (split) {
            // CBF finishes the rotation with the last part of the step only.
            sim->updateRotation(lastSub);
            sim->m_lastPosition = startPos;
        } else {
            sim->updateRotation(dt);
        }
    }
    // The copy keeps the orbs and pads it used; the level gets its own flags back.
    for (auto const& [object, real] : s.simSavedFlags) {
        used[object] = {object->m_activated, object->m_activatedByPlayer1, object->m_activatedByPlayer2};
        object->m_activated = real[0];
        object->m_activatedByPlayer1 = real[1];
        object->m_activatedByPlayer2 = real[2];
    }
    s.simSavedFlags.clear();
    s.simRunFlags = nullptr;
    s.trajSimulating = false;
}

void finishLiveMeasure(PlayLayer* play, LiveMeasure& m, bool shown) {
    auto& s = Session::get();
    giveTrajectoryPlayer(m.trial.sim);
    giveTrajectoryPlayer(m.control.sim);
    giveTrajectoryPlayer(m.start);
    m.done = true;
    if (!shown) {
        return;
    }
    auto const& results = m.planner.results();
    if (m.plannerIndex >= results.size() || !results[m.plannerIndex].valid) {
        return;
    }
    auto const& result = results[m.plannerIndex];
    showLiveResult(play, m.event, result, swiftLevelAt(Session::get().recording, m.index));
}

bool resetRun(PlayLayer* play, LiveMeasure& m, SimRun& run, InputEvent const& click) {
    auto& s = Session::get();
    if (!run.sim) run.sim = createTrajectoryPlayer(play);
    if (!run.sim) return false;
    copyPlayerState(run.sim.data(), m.start.data());
    run.tick = m.startTick;
    run.click = click;
    run.applied = 0;
    run.used.clear();
    run.firstIndex = 0;
    while (run.firstIndex < s.recording.size() && s.recording[run.firstIndex].tick < m.startTick) ++run.firstIndex;
    return true;
}

enum class RunState { Pending, Died, Survived, LeftPath };

// Advances one copy up to the newest real step.
RunState advanceRun(PlayLayer* play, LiveMeasure& m, SimRun& run, int nowTick, bool control) {
    auto& s = Session::get();
    bool const dual = play->m_gameState.m_isDualMode;
    float const dt = s.simStepDt > 0.f ? s.simStepDt : 0.25f;
    constexpr int kFar = std::numeric_limits<int>::max() / 4;

    if (m.event.down && moveHold() && m.partner == std::string::npos) m.partner = partnerOf(s.recording, m.index);
    bool const carry = !control && m.partner != std::string::npos;
    size_t nextIndex = m.index + 1;
    if (nextIndex == m.partner) ++nextIndex;
    bool const nextKnown = nextIndex < s.recording.size();
    size_t const nextPartner = nextKnown && moveHold() ? partnerOf(s.recording, nextIndex) : std::string::npos;

    struct Planned {
        InputEvent event;
        bool neighbour = false;   // an unmoved input on the same button after the click
    };
    std::vector<Planned> plan;
    InputEvent partnerMoved;
    if (carry) partnerMoved = shiftedEvent(s.recording[m.partner], run.shift, m.units);
    int const clickGroupEnd = carry ? std::max(run.click.tick, partnerMoved.tick) : run.click.tick;
    int nextMovedTick = kFar;
    for (size_t i = run.firstIndex; i < s.recording.size(); ++i) {
        auto const& event = s.recording[i];
        if (targetsPlayer1(event, dual) != m.player1) continue;
        if (i == m.index) {
            plan.push_back({run.click});
            continue;
        }
        bool move = false;
        int by = run.shift;
        if (!control && i > m.index) {
            if (carry && i == m.partner) {
                move = true;
            } else if (run.adaptMode >= 1 && (i == nextIndex || i == nextPartner)) {
                // Correction: the next click re-timed (see correctionShift).
                auto const correction = correctionShift(run.shift, cfg::kRawTicksPerPhysicsFrame * m.units, run.adaptMode);
                move = correction.has_value();
                by = correction.value_or(0);
            }
        }
        InputEvent const planned = move ? shiftedEvent(event, by, m.units) : event;
        if (i == nextIndex) nextMovedTick = planned.tick;
        bool const neighbour = !control && !move && i > m.index && event.button == m.event.button && planned.tick < clickGroupEnd;
        plan.push_back({planned, neighbour});
    }
    std::stable_sort(plan.begin(), plan.end(), [](Planned const& a, Planned const& b) {
        if (a.event.tick != b.event.tick) return a.event.tick < b.event.tick;
        return a.event.frac < b.event.frac;
    });

    // Inputs already applied must be exactly the plan before the current step.
    size_t applied = 0;
    while (applied < plan.size() && plan[applied].event.tick < run.tick) ++applied;
    if (applied != run.applied) {
        InputEvent const click = run.click;
        resetRun(play, m, run, click);
        applied = 0;
    }

    // Survive until the next input (moved or not) and the lookahead after it.
    int anchor = clickGroupEnd;
    if (nextKnown) anchor = std::max({anchor, s.recording[nextIndex].tick, nextMovedTick});
    else anchor = kFar;
    int const deadline = anchor + cfg::lookaheadTicks();
    int const afterTick = std::max({m.event.tick, run.click.tick, carry ? s.recording[m.partner].tick : 0,
        carry ? partnerMoved.tick : 0, run.adaptMode >= 1 ? (nextKnown ? std::max(s.recording[nextIndex].tick, nextMovedTick) : kFar) : 0});
    run.adaptAfterTick = nextKnown ? std::min(s.recording[nextIndex].tick, nextMovedTick) : kFar;

    std::vector<InputEvent> due;
    while (run.tick < nowTick) {
        int const tick = run.tick;
        due.clear();
        while (applied < plan.size() && plan[applied].event.tick <= tick) {
            if (plan[applied].neighbour) {
                // The next input on the same button came before the moved click.
                return RunState::Survived;
            }
            due.push_back(plan[applied].event);
            ++applied;
        }
        run.applied = applied;
        std::stable_sort(due.begin(), due.end(), [](InputEvent const& a, InputEvent const& b) { return a.frac < b.frac; });

        s.trajDead = false;
        simulateStep(play, run.sim.data(), due, dt, run.used);
        run.tick += cfg::kRawTicksPerPhysicsFrame;
        bool const died = s.trajDead;
        s.trajDead = false;

        auto const* real = realState(s, m.player1, run.tick);
        if (control) {
            if (died || (real && !sameState(liveStateOf(run.sim.data(), run.tick), *real))) {
                m.why = fmt::format("the copy left the real path at raw tick {}", run.tick);
                return RunState::LeftPath;
            }
            continue;
        }
        if (died) {
            run.deathTick = tick;
            return RunState::Died;
        }
        // Back on the real path after everything moved: from here it lives like the real attempt.
        if (run.tick > afterTick && real && sameState(liveStateOf(run.sim.data(), run.tick), *real))
            return RunState::Survived;
        if (run.tick >= deadline) return RunState::Survived;
    }
    return RunState::Pending;
}

// Starts the next trial of a live measurement, or finishes it.
void nextLiveTrial(PlayLayer* play, LiveMeasure& m) {
    auto const trial = m.planner.next();
    if (!trial) {
        finishLiveMeasure(play, m, true);
        return;
    }
    m.trial.shift = trial->shift;
    m.trial.adaptMode = 0;
    if (!resetRun(play, m, m.trial, shiftedEvent(m.event, trial->shift, m.units))) {
        m.why = "no player copy";
        finishLiveMeasure(play, m, false);
    }
}

void advanceLiveMeasures(PlayLayer* play, int nowTick) {
    auto& s = Session::get();
    for (auto& m : s.liveMeasures) {
        if (m->done) continue;
        // The control copy first: shifted copies are only trusted where it followed the real attempt.
        auto const control = advanceRun(play, *m, m->control, nowTick, true);
        if (control == RunState::LeftPath) {
            finishLiveMeasure(play, *m, false);
            continue;
        }
        int guard = 0;
        while (!m->done && ++guard < kTrajMaxTrials) {
            auto const state = advanceRun(play, *m, m->trial, nowTick, false);
            if (state == RunState::Pending) break;
            // Died only after the next click: try re-timing that click (correctionShift).
            if (state == RunState::Died && adaptClicks() && m->trial.shift != 0 &&
                (m->trial.adaptMode > 0 || m->trial.deathTick >= m->trial.adaptAfterTick) &&
                correctionShift(m->trial.shift, cfg::kRawTicksPerPhysicsFrame * m->units, m->trial.adaptMode + 1)) {
                ++m->trial.adaptMode;
                InputEvent const click = m->trial.click;
                resetRun(play, *m, m->trial, click);
                continue;
            }
            ++m->trials;
            m->planner.report(state == RunState::Survived);
            nextLiveTrial(play, *m);
        }
    }
    std::erase_if(s.liveMeasures, [](auto const& m) { return m->done; });
}

// A click of the attempt in progress: its measurement starts at once.
void startLiveMeasure(PlayLayer* play, size_t index) {
    auto& s = Session::get();
    if (index >= s.recording.size()) return;
    auto const& event = s.recording[index];
    if (!event.down && !cfg::flag("show-releases")) return;

    // The newest copy early enough for the earliest shift, or the oldest one there is.
    int const wanted = event.tick - cfg::maxShiftTicks();
    TrajSnapshot const* snapshot = nullptr;
    for (auto it = s.trajSnapshots.rbegin(); it != s.trajSnapshots.rend(); ++it) {
        if (it->tick <= event.tick) snapshot = &*it;
        if (it->tick <= wanted) break;
    }
    if (!snapshot || !snapshot->p1) return;

    auto m = std::make_unique<LiveMeasure>();
    m->index = index;
    m->event = event;
    m->player1 = targetsPlayer1(event, play->m_gameState.m_isDualMode);
    m->startTick = snapshot->tick;
    m->start = createTrajectoryPlayer(play);
    if (!m->start) return;
    copyPlayerState(m->start.data(), m->player1 ? snapshot->p1.data() : snapshot->p2.data());

    // The units of this click only: the counted results keep theirs.
    int const savedUnits = s.units;
    int const savedStep = s.stepTicks;
    bool const savedPrecise = s.precise;
    configureUnits(s, play, {event});
    m->units = s.units;

    // The previous input on the same button bounds the early side.
    std::vector<InputEvent> inputs;
    for (size_t i = index; i-- > 0;) {
        auto const& prev = s.recording[i];
        if (prev.button == event.button && prev.player1 == event.player1) {
            inputs.push_back(prev);
            break;
        }
    }
    m->plannerIndex = inputs.size();
    inputs.push_back(event);

    auto config = plannerConfig(s, std::numeric_limits<int>::max() / (4 * s.units), RunEnd::Completed);
    config.minInputTick = m->startTick * s.units;
    config.anchorNext = false;
    config.controlTrial = false;
    config.onlyIndex = static_cast<int>(m->plannerIndex);
    m->planner.begin(plannerInputs(inputs, s.units), config);
    s.units = savedUnits;
    s.stepTicks = savedStep;
    s.precise = savedPrecise;

    if (!resetRun(play, *m, m->control, event)) {
        giveTrajectoryPlayer(m->start);
        return;
    }
    nextLiveTrial(play, *m);
    if (!m->done) s.liveMeasures.push_back(std::move(m));
}

} // namespace

// Every command tick while Live Counter records.
void liveStep(PlayLayer* play, int tick) {
    auto& s = Session::get();
    if (!liveMode() || !trajectoryLive() || s.stage != Stage::Recording) return;
    if (!play->m_player1 || play->m_isPaused) return;

    for (int p = 0; p < 2; ++p) {
        auto& ring = s.liveStates[static_cast<size_t>(p)];
        if (ring.size() != kLiveStateRing) ring.assign(kLiveStateRing, LiveState{});
        auto* player = p == 0 ? play->m_player1 : play->m_player2;
        ring[static_cast<size_t>(tick / 2) % ring.size()] = liveStateOf(player ? player : play->m_player1, tick);
    }

    if (!play->m_player1->m_isDead && !play->m_levelEndAnimationStarted) {
        size_t const ring = static_cast<size_t>(cfg::maxShiftTicks() / cfg::kRawTicksPerPhysicsFrame + 3);
        TrajSnapshot snapshot;
        snapshot.tick = tick;
        if (s.trajSnapshots.size() >= ring) {
            snapshot.p1 = s.trajSnapshots.front().p1;
            snapshot.p2 = s.trajSnapshots.front().p2;
            s.trajSnapshots.pop_front();
        } else {
            snapshot.p1 = createTrajectoryPlayer(play);
            snapshot.p2 = createTrajectoryPlayer(play);
        }
        if (snapshot.p1 && snapshot.p2) {
            copyPlayerState(snapshot.p1.data(), play->m_player1);
            copyPlayerState(snapshot.p2.data(), play->m_player2 ? play->m_player2 : play->m_player1);
            s.trajSnapshots.push_back(std::move(snapshot));
        }
    }

    // Clicks of the previous step (their CBF fraction is known now).
    auto queue = std::move(s.liveStartQueue);
    s.liveStartQueue.clear();
    for (size_t index : queue) startLiveMeasure(play, index);

    if (play->m_player1->m_isDead) {
        for (auto& m : s.liveMeasures) {
            m->why = "the attempt ended first";
            finishLiveMeasure(play, *m, false);
        }
        s.liveMeasures.clear();
        return;
    }
    advanceLiveMeasures(play, tick);
}

// Practice respawn or restart: measurements of the old attempt are dropped.
void dropLiveMeasures() {
    auto& s = Session::get();
    for (auto& m : s.liveMeasures) {
        giveTrajectoryPlayer(m->trial.sim);
        giveTrajectoryPlayer(m->control.sim);
        giveTrajectoryPlayer(m->start);
    }
    s.liveMeasures.clear();
    s.liveStartQueue.clear();
    for (auto& ring : s.liveStates) ring.clear();
}

// Starts the live measurements whose verdict tick has been reached.
void tryStartLive() {
    auto& s = Session::get();
    auto* play = PlayLayer::get();
    if (!play || s.stage != Stage::Recording || s.liveSim || s.liveJobs.empty()) return;
    if (trajectoryLive() || !play->m_isPracticeMode) return;
    if (play->m_isPaused || !play->m_player1 || play->m_player1->m_isDead) return;
    if (play->m_levelEndAnimationStarted) return;

    int const tick = currentTick(play);
    while (!s.liveJobs.empty()) {
        // Jobs of inputs that were rewound by a practice respawn no longer exist.
        auto const& front = s.liveJobs.front();
        if (front.index >= s.recording.size() || s.recording[front.index].tick > tick) {
            s.liveJobs.pop_front();
            continue;
        }
        if (tick < front.verdictTick) return;

        auto const job = front;
        s.liveJobs.pop_front();
        startLiveProbe(play, job);
        return;
    }
}

// A failed live simulation.
void abortLive(std::string const& reason) {
    auto& s = Session::get();
    log::error("Live Counter stopped: {}", reason);
    s.liveDisabled = true;
    s.liveSim = false;
    s.liveJobs.clear();
    s.liveRestorePending = true;
    s.stage = Stage::Recording;
    s.injecting = false;
    s.pendingOutcome = ProbeOutcome::None;
    Notification::create("Live Counter stopped: " + reason, NotificationIcon::Warning)->show();
}

} // namespace tc
