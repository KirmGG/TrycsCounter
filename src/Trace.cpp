// Player traces: comparing runs with the baseline, checkpoint checks, convergence.

#include "App.hpp"

namespace tc {

namespace {

constexpr size_t kTraceMaxTicks = 2'000'000;

// The recorded attempt and its replay may differ by float noise; a different alignment differs by far more.
constexpr float kRecordedPathTolerance = 0.01f;

// A trial from a practice checkpoint runs the same replay code as the baseline replay from the level start.
constexpr float kCheckpointTolerance = 1e-4f;

constexpr float kCheckpointRotationTolerance = 0.01f;

} // namespace

PlayerTrace playerTrace(PlayerObject* player) {
    PlayerTrace t;
    if (!player) return t;
    auto const pos = player->getPosition();
    t.x = pos.x;
    t.y = pos.y;
    t.yVel = player->m_yVelocity;
    t.rotation = player->getRotation();
    t.size = player->m_vehicleSize;
    t.speed = player->m_playerSpeed;
    unsigned bit = 0;
    auto put = [&](bool value) {
        if (value) t.flags |= 1u << bit;
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
    put(player->m_isOnSlope);
    return t;
}

TraceState sampleTrace(PlayLayer* play) {
    TraceState t;
    if (!play || !play->m_player1) return t;
    t.valid = true;
    t.p1 = playerTrace(play->m_player1);
    t.dual = play->m_gameState.m_isDualMode && play->m_player2;
    if (t.dual) t.p2 = playerTrace(play->m_player2);
    return t;
}

float pathTolerance() {
    // The edge check accepts only an exact return onto the baseline path.
    if (Session::get().strictTrials) return 0.f;
    return std::max(0.f, cfg::real("path-tolerance"));
}

namespace {

bool samePlayer(PlayerTrace const& a, PlayerTrace const& b, TraceCheck check) {
    switch (check) {
        case TraceCheck::Recorded:
            return std::abs(a.x - b.x) <= kRecordedPathTolerance && std::abs(a.y - b.y) <= kRecordedPathTolerance &&
                std::abs(a.yVel - b.yVel) <= kRecordedPathTolerance;
        case TraceCheck::Checkpoint:
            return a.flags == b.flags && a.size == b.size && a.speed == b.speed &&
                std::abs(a.x - b.x) <= kCheckpointTolerance && std::abs(a.y - b.y) <= kCheckpointTolerance &&
                std::abs(a.yVel - b.yVel) <= kCheckpointTolerance &&
                std::abs(a.rotation - b.rotation) <= kCheckpointRotationTolerance;
        case TraceCheck::Exact:
            return a.flags == b.flags && a.size == b.size && a.speed == b.speed && a.x == b.x && a.y == b.y &&
                a.yVel == b.yVel && a.rotation == b.rotation;
        case TraceCheck::Near: {
            float const tolerance = pathTolerance();
            return a.flags == b.flags && a.size == b.size && a.speed == b.speed &&
                std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
                std::abs(a.yVel - b.yVel) <= 0.2 * tolerance + 1e-6;
        }
    }
    return false;
}

} // namespace

bool sameTrace(TraceState const& a, TraceState const& b, TraceCheck check) {
    if (a.positionsOnly || b.positionsOnly) {
        auto samePosition = [](PlayerTrace const& p, PlayerTrace const& q) {
            return std::abs(p.x - q.x) <= kRecordedPathTolerance && std::abs(p.y - q.y) <= kRecordedPathTolerance;
        };
        auto const& known = a.positionsOnly ? a : b;
        return samePosition(a.p1, b.p1) && (!known.dual || samePosition(a.p2, b.p2));
    }
    if (a.dual != b.dual) return false;
    return samePlayer(a.p1, b.p1, check) && (!a.dual || samePlayer(a.p2, b.p2, check));
}

TraceState const* traceAt(std::vector<TraceState> const& trace, int tick) {
    if (tick < 0 || static_cast<size_t>(tick) >= trace.size()) return nullptr;
    auto const& state = trace[static_cast<size_t>(tick)];
    return state.valid ? &state : nullptr;
}

void tracePlayer(PlayLayer* play, std::vector<TraceState>& trace, int tick) {
    if (tick < 0 || static_cast<size_t>(tick) >= kTraceMaxTicks || !play->m_player1) return;
    if (trace.size() <= static_cast<size_t>(tick)) trace.resize(static_cast<size_t>(tick) + 1);
    trace[static_cast<size_t>(tick)] = sampleTrace(play);
}

// Baseline replay: records its own trace and notes the first raw tick where it leaves the recorded attempt.
void traceBaseline(PlayLayer* play, Session& s, int tick) {
    tracePlayer(play, s.baselineTrace, tick);
    if (s.baselineDivergeTick >= 0 || s.lastInjectedTick < 0) return;
    // The last ticks before a recorded death are sampled while the player dies.
    if (tick > s.source.endTick - 8) return;
    auto const* want = traceAt(s.sourceTrace, tick);
    auto const* got = traceAt(s.baselineTrace, tick);
    if (!want || !got) return;
    if (!sameTrace(*got, *want, TraceCheck::Recorded)) {
        s.baselineDivergeTick = tick;
        log::warn("Baseline replay (offset {}) left the recorded attempt at raw tick {}: recorded ({:.4f}, {:.4f}, vy {:.5f}, "
            "flags {:#x}) replay ({:.4f}, {:.4f}, vy {:.5f}, flags {:#x})", s.activeOffset(), tick, want->p1.x, want->p1.y,
            want->p1.yVel, want->p1.flags, got->p1.x, got->p1.y, got->p1.yVel, got->p1.flags);
    }
}

// Ship, wave and swing.
std::vector<char> freeFlightInputs(Session const& s) {
    auto const& events = s.source.events;
    std::vector<char> result(events.size(), 0);
    if (!cfg::flag("free-flight") || s.baselineTrace.empty()) return result;
    // Bits of PlayerTrace::flags (see playerTrace): ship 2, wave 5, swing 8.
    unsigned const mask = (1u << 2) | (1u << 5) | (1u << 8);
    int count = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        auto const& e = events[i];
        TraceState const* state = nullptr;
        for (int t = e.tick; t >= std::max(0, e.tick - 8) && !state; --t) state = traceAt(s.baselineTrace, t);
        if (!state) continue;
        auto const& player = state->dual && !e.player1 ? state->p2 : state->p1;
        if (player.flags & mask) {
            result[i] = 1;
            ++count;
        }
    }
    log::info("Free flight: {} of {} inputs in ship, wave or swing (reaction {} frames)", count, events.size(),
        cfg::integer("free-flight-reaction"));
    return result;
}

} // namespace tc
