// Click Between Frames and Click Between Steps: step timing, sub-step inputs, measurement units.

#include "App.hpp"

namespace tc {

namespace {

// CBF applies an input inside the update of player 1.

constexpr char const* kCbfModId = "syzzi.click_between_frames";

} // namespace

Mod* cbfMod() {
    return Loader::get()->getLoadedMod(kCbfModId);
}

// CBF is loaded and splits physics steps (not disabled, not in Click on Steps mode).
bool cbfSplitsSteps() {
    auto* cbf = cbfMod();
    if (!cbf) return false;
    return !cbf->getSettingValue<bool>("soft-toggle") && !cbf->getSettingValue<bool>("click-on-steps");
}

// Gives the inputs of the finished physics step their fraction of the step.
void finalizeCbfStep(Session& s) {
    float const total = std::max(s.cbfAcc1, s.cbfAcc2);
    for (auto const& [index, done] : s.cbfPending) {
        if (index >= s.recording.size() || !s.recording[index].cbf) continue;
        double const frac = total > 0.f ? done / static_cast<double>(total) : 0.0;
        s.recording[index].frac = std::clamp(frac, 0.0, 0.999999);
    }
    s.cbfPending.clear();
}

namespace {

// A new physics step starts whenever the level clock moves.
constexpr float kStepDtEpsilon = 1e-4f;

} // namespace

// Recording: remembers how long the player updates of the finished step were.
void noteStepDt(Session& s) {
    if (s.stage != Stage::Recording || s.cbfAccTick == std::numeric_limits<int>::min() || s.cbfAcc1 <= 0.f) return;
    if (!s.stepDts.empty()) {
        auto const& last = s.stepDts.back();
        if (last.tick >= s.cbfAccTick) return;
        if (std::abs(last.p1 - s.cbfAcc1) <= kStepDtEpsilon && std::abs(last.p2 - s.cbfAcc2) <= kStepDtEpsilon) return;
    }
    s.stepDts.push_back({s.cbfAccTick, s.cbfAcc1, s.cbfAcc2});
}

// Replay: the recorded length of this step for a player, or `dt` when the recording had the normal length.
float replayStepDt(Session const& s, int tick, bool player1, float dt) {
    auto const& dts = s.source.stepDts;
    auto it = std::upper_bound(dts.begin(), dts.end(), tick, [](int t, StepDt const& e) { return t < e.tick; });
    if (it == dts.begin()) return dt;
    --it;
    float const want = player1 ? it->p1 : it->p2;
    if (want <= 0.f || std::abs(want - dt) <= kStepDtEpsilon) return dt;
    return want;
}

void rollCbfStep(Session& s, int tick) {
    if (tick == s.cbfAccTick) return;
    noteStepDt(s);
    if (s.cbfAcc1 > 0.f && std::abs(s.cbfAcc1 - s.simStepDt) <= 0.5f * s.simStepDt) s.simStepDt = s.cbfAcc1;
    finalizeCbfStep(s);
    s.cbfAccTick = tick;
    s.cbfAcc1 = 0.f;
    s.cbfAcc2 = 0.f;
}

// Position of an input on the measurement axis (`units` per raw tick).
int eventPos(InputEvent const& event, int units) {
    int pos = event.tick * units;
    if (event.cbf) pos += static_cast<int>(std::lround(event.frac * cfg::kRawTicksPerPhysicsFrame * units));
    return pos;
}

// The input moved by `shift` measurement units.
InputEvent shiftedEvent(InputEvent event, int shift, int units) {
    if (units > 1) {
        // Sub-frame measurement: an input moved inside a physics step is applied the way CBF applies it.
        double const base = event.cbf ? event.frac : 0.0;
        double const total = base + static_cast<double>(shift) / (cfg::kRawTicksPerPhysicsFrame * units);
        double const carry = std::floor(total + 1e-9);
        event.tick += static_cast<int>(carry) * cfg::kRawTicksPerPhysicsFrame;
        event.frac = std::clamp(total - carry, 0.0, 0.999999);
        event.cbf = event.frac > 1e-6;
        if (!event.cbf) event.frac = 0.0;
    } else {
        event.tick += shift;
    }
    return event;
}

// CBF's own "buffering" rule: a player that is airborne and cannot act right now takes the input at the start
// of the step instead of splitting it.
bool cbfCanSplit(PlayerObject* player) {
    return player->m_isOnGround || (player->m_touchingRings && player->m_touchingRings->count()) || player->m_isDashing ||
        player->m_isDart || player->m_isBird || player->m_isShip || player->m_isSwing;
}

// PlayerObject::resetCollisionLog is inlined on Windows.
void resetCollisionLog(PlayerObject* player) {
    if (player->m_collisionLogTop) player->m_collisionLogTop->removeAllObjects();
    if (player->m_collisionLogBottom) player->m_collisionLogBottom->removeAllObjects();
    if (player->m_collisionLogLeft) player->m_collisionLogLeft->removeAllObjects();
    if (player->m_collisionLogRight) player->m_collisionLogRight->removeAllObjects();
    player->m_lastCollisionLeft = -1;
    player->m_lastCollisionRight = -1;
    player->m_lastCollisionBottom = -1;
    player->m_lastCollisionTop = -1;
}

// Measurement units and shift step for a run: CBF runs are measured in 1/100 frame.
void configureUnits(Session& s, PlayLayer* play, std::vector<InputEvent> const& events) {
    bool const anyCbf = std::any_of(events.begin(), events.end(), [](InputEvent const& e) { return e.cbf; });
    // Sub-frame steps only for runs with real between-steps (CBF) inputs.
    bool const fine = anyCbf;
    s.units = fine ? cfg::kCbfUnitsPerRawTick : 1;
    s.stepTicks = fine ? cfg::cbfStepUnits() : cfg::stepTicks(play->m_clickBetweenSteps);
    s.precise = cfg::isPrecise(s.stepTicks, s.units);
    if (fine) log::debug("Click Between Frames measurement: steps of {} / {} frame, {} CBF inputs",
        s.stepTicks, cfg::kRawTicksPerPhysicsFrame * s.units,
        std::count_if(events.begin(), events.end(), [](InputEvent const& e) { return e.cbf; }));
}

// Replays the Click Between Frames inputs of this physics step the way CBF applies them.
bool replayCbfStep(PlayLayer* play, PlayerObject* p1, float dt, int tick) {
    auto& s = Session::get();
    if (s.startFromBeginning || s.pendingReset != Session::PendingReset::None || s.lastInjectedTick < 0 || !p1 || p1->m_isDead)
        return false;
    if (play->m_levelEndAnimationStarted) return false;
    auto const& events = s.stage == Stage::Probing ? s.currentTrial : s.source.events;

    std::vector<InputEvent> due;
    while (s.replayCursor < events.size()) {
        auto const& event = events[s.replayCursor];
        if (!event.cbf) break;
        int const target = injectTick(s, event);
        if (target > tick) break;
        if (tick - target > kInjectLateToleranceTicks) {
            log::error("Missed CBF input #{}: recorded raw tick {}, target {}, current raw tick {}, offset {}",
                s.replayCursor, event.tick, target, tick, s.activeOffset());
            stopDriving(fmt::format(
                "Replay missed input #{} (recorded at raw tick {}, the game is already at raw tick {}, offset {}). "
                "The replay clock does not match the recording; see the Geode log.",
                s.replayCursor, event.tick, tick, s.activeOffset()));
            return false;
        }
        due.push_back(event);
        ++s.replayCursor;
    }
    if (due.empty()) return false;

    auto* p2 = play->m_player2;
    bool const dual = play->m_gameState.m_isDualMode;
    bool const p1StartedOnGround = p1->m_isOnGround;
    bool const p2StartedOnGround = p2 && p2->m_isOnGround;
    bool const p1Split = cbfCanSplit(p1);
    bool const p2Split = p2 && dual && cbfCanSplit(p2);
    // A step the recording made longer (see StepDt) is split over its recorded length.
    float const dt1 = replayStepDt(s, tick, true, dt);
    double const scale = dt1 == dt ? 1.0 : static_cast<double>(dt1) / dt;
    s.cbfP1Pos = p1->getPosition();
    if (p2) s.cbfP2Pos = p2->getPosition();

    auto splitExtras = [&](PlayerObject* player, bool firstLoop, bool startedOnGround, float sub) {
        if (firstLoop && ((player->m_yVelocity < 0) ^ player->m_isUpsideDown)) player->m_isOnGround = startedOnGround;
        if (!player->m_isOnSlope || player->m_isDart) play->checkCollisions(player, 0.f, true);
        else play->checkCollisions(player, dt, true);
        player->updateRotation(sub);
        resetCollisionLog(player);
    };

    s.cbfMidStep = true;
    double elapsed = 0.0;
    for (size_t k = 0; k <= due.size(); ++k) {
        bool const endStep = k == due.size();
        if (k > 0) {
            auto const& input = due[k - 1];
            s.injecting = true;
            play->handleButton(input.down, input.button, input.player1);
            s.injecting = false;
            log::debug("Injected CBF input at raw tick {} (recorded {}, frac {:.4f}): button={}, down={}, P1={}",
                tick, input.tick, input.frac, input.button, input.down, input.player1);
        }

        double factor = 0.0;
        if (endStep) {
            factor = std::max(kCbfSmallestFactor, scale - elapsed);
        } else {
            factor = std::clamp(due[k].frac * scale - elapsed, kCbfSmallestFactor, scale);
            elapsed = due[k].frac * scale;
        }
        float const sub = static_cast<float>(dt * factor);
        s.cbfRotDelta = sub;
        bool const firstLoop = k == 0;

        if (p1Split) {
            p1->update(sub);
            if (!endStep) splitExtras(p1, firstLoop, p1StartedOnGround, sub);
        } else if (endStep) {
            p1->update(dt1);
        }

        if (p2) {
            if (p2Split) {
                p2->update(sub);
                if (!endStep) splitExtras(p2, firstLoop, p2StartedOnGround, sub);
            } else if (endStep) {
                p2->update(replayStepDt(s, tick, false, dt));
            }
        }
    }
    s.cbfMidStep = false;

    s.cbfSplitP1 = p1Split;
    s.cbfSplitP2 = p2Split;
    s.cbfSplitTick = tick;
    return true;
}

} // namespace tc
