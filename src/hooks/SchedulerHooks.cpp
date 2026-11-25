// Hooks of the scheduler: counting faster than real time.

#include "../App.hpp"
#include <Geode/modify/CCScheduler.hpp>

namespace tc {

namespace {

// Wall-clock budget of one render frame while the analysis runs sped up.
// Long enough for fast counting, short enough that the game keeps drawing (about 12 frames a second).
constexpr std::chrono::milliseconds kSpeedUpFrameBudget{120};

// Largest slice of game time given to one scheduler update while counting.
constexpr double kSpeedUpChunkSeconds = 0.05;

// Live Counter: game time per scheduler update inside the synchronous simulation.
constexpr float kLiveChunkSeconds = 0.1f;

// Live Counter: the simulation is abandoned if it takes longer than this.
constexpr std::chrono::milliseconds kLiveMaxWall{3000};

} // namespace
} // namespace tc

using namespace tc;

// Runs a reset the analysis asked for, outside of any physics step.
void performPendingReset() {
    auto& s = Session::get();
    if (s.pendingReset == Session::PendingReset::None) return;
    auto const kind = s.pendingReset;
    s.pendingReset = Session::PendingReset::None;
    auto* play = PlayLayer::get();
    if (!play || !s.drivesInput()) return;
    if (kind == Session::PendingReset::FromStart) play->resetLevelFromStart();
    else play->resetLevel();
    if (s.stage == Stage::BaselineReplay && s.checkpoints.empty()) hideDecoration(play);
}

class $modify(TCScheduler, CCScheduler) {
    // Runs the live simulation to the end inside this single frame.
    void runLiveSim() {
        auto& s = Session::get();
        auto const started = std::chrono::steady_clock::now();
        while (s.liveSim && s.isAnalyzing()) {
            performPendingReset();
            CCScheduler::update(kLiveChunkSeconds);
            if (std::chrono::steady_clock::now() - started > kLiveMaxWall) {
                abortLive("the simulation took too long");
                break;
            }
        }
        if (s.liveSim) abortLive("the simulation stopped unexpectedly");
    }

    void update(float dt) {
        auto& s = Session::get();

        performPendingReset();
        if (s.stage == Stage::Recording && liveMode()) tryStartLive();
        if (s.liveSim) {
            this->runLiveSim();
            restoreLive(PlayLayer::get());
            CCScheduler::update(dt);
            return;
        }
        if (s.liveRestorePending) restoreLive(PlayLayer::get());

        if (!s.isAnalyzing()) {
            CCScheduler::update(dt);
            return;
        }

        // The game time of this rendered frame is dt * speed. It is split into
        // slices of at most kSpeedUpChunkSeconds: one huge dt would make the game
        // run all its steps in a single update, many tiny ones waste time on
        // rendering related updates. The wall-clock budget keeps the window alive.
        using Clock = std::chrono::steady_clock;
        auto const deadline = Clock::now() + kSpeedUpFrameBudget;

        double const speed = std::clamp(static_cast<double>(cfg::real("count-speed")), 1.0, 100000.0);
        double const total = static_cast<double>(dt) * speed;
        int const chunks = std::max(1, static_cast<int>(std::ceil(total / kSpeedUpChunkSeconds)));
        float const chunkDt = static_cast<float>(total / static_cast<double>(chunks));

        for (int i = 0; i < chunks; ++i) {
            performPendingReset();
            CCScheduler::update(chunkDt);
            if (!s.isAnalyzing() || Clock::now() >= deadline) break;
        }
    }
};
