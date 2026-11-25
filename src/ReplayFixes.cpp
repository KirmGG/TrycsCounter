// Replay corrections ("frame fixes"): small state fixes that keep the replay on the recorded path.

#include "App.hpp"

namespace tc {

namespace {

// Practice checkpoints restore the player a little differently than a continuous
// run gets there, and xdBot's Frame Fixes set the player's position directly.
// A replay from the inputs alone then drifts away from the attempt. The baseline
// replay therefore puts the player back onto the recorded path whenever it is a
// little off (same mode, gravity, size and held buttons, at most a few units
// away) and remembers each correction. Trials, the Live replay and the replay of
// the counted run apply the same corrections: unchanged where the player is
// exactly where the baseline was before its correction, as an offset elsewhere
// (a moved click is not on the recorded path, the drift still happens).

// Smaller corrections never count as the replay going elsewhere.
constexpr float kFixTiny = 0.05f;

// Larger differences are a real desync, not a drift.
constexpr float kFixMaxDistance = 8.f;

constexpr double kFixMaxVelocity = 3.0;

// This many real corrections in a row: the replay went elsewhere.
constexpr int kFixMaxRun = 16;

void setPlayerState(PlayerObject* player, float x, float y, double vy) {
    CCPoint const point{x, y};
    player->m_position = point;
    player->setPosition(point);
    player->m_yVelocity = vy;
}

// A correction is real (not a rounding difference) and counts towards giving up.
// False once the replay has needed too many in a row: it went elsewhere.
bool noteCorrection(Session& s, int tick, float dx, float dy, double dvy) {
    bool const tiny = dx <= kFixTiny && dy <= kFixTiny && dvy <= kFixTiny;
    if (!tiny) {
        s.fixRun = tick - s.fixRunLastTick <= cfg::kRawTicksPerPhysicsFrame ? s.fixRun + 1 : 1;
        s.fixRunLastTick = tick;
        if (s.fixRun > kFixMaxRun) {
            s.fixesGaveUp = true;
            log::warn("Replay corrections stopped at raw tick {}: {} in a row", tick, s.fixRun);
            return false;
        }
    }
    s.fixLargest = std::max({s.fixLargest, dx, dy});
    return true;
}

// A bot macro's Frame Fixes: the players are put where the bot had them, like
// the bot does when it plays the macro. Only small differences: a large one is
// a desync, not a drift.
void fixPositions(PlayLayer* play, Session& s, int tick, TraceState const& want, bool dual) {
    for (int p = 0; p < (want.dual && dual ? 2 : 1); ++p) {
        auto* player = p == 0 ? play->m_player1 : play->m_player2;
        if (!player || player->m_isDead) continue;
        auto const got = playerTrace(player);
        auto const& w = p == 0 ? want.p1 : want.p2;
        float const dx = std::abs(w.x - got.x);
        float const dy = std::abs(w.y - got.y);
        if ((dx == 0.f && dy == 0.f) || dx > kFixMaxDistance || dy > kFixMaxDistance) continue;
        if (!noteCorrection(s, tick, dx, dy, 0.0)) return;
        s.source.fixes.push_back({tick, p == 1, got.x, got.y, got.yVel, w.x, w.y, got.yVel});
        setPlayerState(player, w.x, w.y, got.yVel);
    }
}

// Baseline replay: back onto the recorded path when only a little off.
void fixBaseline(PlayLayer* play, Session& s, int tick) {
    if (s.fixesGaveUp || s.sourceTrace.empty() || tick > s.source.endTick - 8) return;
    auto const* want = traceAt(s.sourceTrace, tick);
    if (!want) return;
    bool const dual = play->m_gameState.m_isDualMode && play->m_player2;
    if (want->positionsOnly) {
        fixPositions(play, s, tick, *want, dual);
        return;
    }
    if (want->dual != dual) return;
    for (int p = 0; p < (dual ? 2 : 1); ++p) {
        auto* player = p == 0 ? play->m_player1 : play->m_player2;
        if (!player || player->m_isDead) continue;
        auto const got = playerTrace(player);
        auto const& w = p == 0 ? want->p1 : want->p2;
        // Another mode, gravity, size or held button is a real difference.
        if (got.flags != w.flags || got.size != w.size || got.speed != w.speed) continue;
        float const dx = std::abs(w.x - got.x);
        float const dy = std::abs(w.y - got.y);
        double const dvy = std::abs(w.yVel - got.yVel);
        if (dx == 0.f && dy == 0.f && dvy == 0.0) continue;
        if (dx > kFixMaxDistance || dy > kFixMaxDistance || dvy > kFixMaxVelocity) continue;
        if (!noteCorrection(s, tick, dx, dy, dvy)) return;
        s.source.fixes.push_back({tick, p == 1, got.x, got.y, got.yVel, w.x, w.y, w.yVel});
        setPlayerState(player, w.x, w.y, w.yVel);
    }
}

// Trials and replays: the corrections the baseline made at this tick.
void applyRecordedFixes(PlayLayer* play, Session const& s, int tick) {
    auto const& fixes = s.source.fixes;
    auto it = std::lower_bound(fixes.begin(), fixes.end(), tick,
        [](StateFix const& f, int t) { return f.tick < t; });
    for (; it != fixes.end() && it->tick == tick; ++it) {
        auto* player = it->player2 ? play->m_player2 : play->m_player1;
        if (!player || player->m_isDead) continue;
        auto const pos = player->getPosition();
        if (pos.x == it->fromX && pos.y == it->fromY && player->m_yVelocity == it->fromVy) {
            setPlayerState(player, it->toX, it->toY, it->toVy);
        } else {
            setPlayerState(player, pos.x + (it->toX - it->fromX), pos.y + (it->toY - it->fromY),
                player->m_yVelocity + (it->toVy - it->fromVy));
        }
    }
}

} // namespace

void applyStateFixes(PlayLayer* play, Session& s, int tick) {
    // A bot macro's Frame Fixes are part of the macro: always applied.
    if (!play || !play->m_player1 || !(cfg::flag("state-fixes") || s.source.fromMacro)) return;
    if (s.startFromBeginning || s.pendingReset != Session::PendingReset::None) return;
    if (s.stage == Stage::BaselineReplay) fixBaseline(play, s, tick);
    else if (s.stage == Stage::Probing || s.stage == Stage::Replay) applyRecordedFixes(play, s, tick);
}

// Practice recording: right after a respawn the game may have released (or kept)
// the buttons without an input. The recording follows what the game holds now.
void syncHeldButtons(PlayLayer* play, Session& s, int tick) {
    if (!s.syncHeldAfterRespawn) return;
    s.syncHeldAfterRespawn = false;
    if (!play->m_player1 || play->m_player1->m_isDead) return;
    bool const dual = play->m_gameState.m_isDualMode && play->m_player2;
    for (int p = 0; p < (dual ? 2 : 1); ++p) {
        auto* player = p == 0 ? play->m_player1 : play->m_player2;
        bool const player1 = p == 0;
        bool const holding = holdingJump(player);
        auto& held = s.recordHeld[{1, player1}];
        if (held == holding) continue;
        held = holding;
        auto const point = playerPoint(play, player1);
        s.recording.push_back({tick, holding, 1, player1, point.x, point.y, false});
        log::debug("Practice respawn: button 1 of P{} is {} now, recorded at raw tick {}", p + 1,
            holding ? "held" : "released", tick);
    }
}

// The practice checkpoint of the current trial did not reproduce the baseline
// before the moved click: the trial is run again from an earlier one.
void rejectCheckpoint(Session& s, int tick, char const* what) {
    if (s.skipVisuals && ++s.skipVisualsRejections >= 3) {
        // Skipping the drawing work might change this level: count it fully.
        s.skipVisuals = false;
        log::warn("Counting draws the level again: trials left the baseline while its drawing work was skipped");
    }
    if (s.checkpoints.empty()) return;
    int const bad = s.checkpoints.back().tick;
    auto const known = s.checkpointBadFrom.find(bad);
    s.checkpointBadFrom[bad] = known == s.checkpointBadFrom.end() ? tick : std::min(known->second, tick);
    ++s.badCheckpointRetries;
    ++s.checkpointRejections;
    log::warn("{} for input #{} {} at raw tick {} (checked up to raw tick {}): checkpoint at raw tick {} "
        "does not restore the level exactly, running the trial again from an earlier one",
        s.verifyingCheckpoint ? "Checkpoint check" : "Trial", s.activeTrial.index, what, tick, s.trialVerifyUntil, bad);
    s.verifyingCheckpoint = false;
    s.pendingOutcome = ProbeOutcome::None;
    s.pendingReset = Session::PendingReset::Level;
}

// Before the moved click a trial must be identical to the baseline replay (the
// same replay from the level start). Without a baseline trace the recorded
// attempt is the reference, with the looser tolerance.
void verifyTrialPrefix(PlayLayer* play, Session& s, int tick) {
    if (s.checkpoints.empty() || s.pendingReset != Session::PendingReset::None || s.lastInjectedTick < 0) return;
    if (tick > s.trialVerifyUntil || tick < s.checkpoints.back().tick) return;
    auto const now = sampleTrace(play);
    if (!now.valid) return;
    if (auto const* want = traceAt(s.baselineTrace, tick)) {
        if (!sameTrace(now, *want, TraceCheck::Checkpoint)) rejectCheckpoint(s, tick, "left the baseline path");
        return;
    }
    if (auto const* want = traceAt(s.sourceTrace, tick)) {
        if (!sameTrace(now, *want, TraceCheck::Recorded)) rejectCheckpoint(s, tick, "left the recorded path");
    }
}

// After every moved input: a trial whose players are bit for bit in the
// baseline state (and stay there) continues exactly like the baseline replay,
// which reached the end of the run. It survives, no need to wait for the lookahead.
void checkConvergence(PlayLayer* play, Session& s, int tick) {
    if (s.verifyingCheckpoint) return;
    if (s.pendingOutcome != ProbeOutcome::None || s.pendingReset != Session::PendingReset::None || s.lastInjectedTick < 0) return;
    if (tick <= s.trialConvergeAfter) return;
    auto const* want = traceAt(s.baselineTrace, tick);
    // Bit for bit, or (setting "Back on path") within a small distance: the
    // player has fixed the mistake and goes on like the attempt.
    auto const check = pathTolerance() > 0.f ? TraceCheck::Near : TraceCheck::Exact;
    if (!want || !sameTrace(sampleTrace(play), *want, check)) {
        s.trialConvergedSince = -1;
        return;
    }
    if (s.trialConvergedSince < 0) s.trialConvergedSince = tick;
    if (tick - s.trialConvergedSince < kConvergeHoldTicks) return;
    ++s.trialsConverged;
    s.trialNear = check == TraceCheck::Near;
    s.pendingOutcome = ProbeOutcome::Survived;
    s.pendingReset = Session::PendingReset::Level;
}

} // namespace tc
