// Hooks of the scheduler: counting faster than real time.

#include "../App.hpp"
#include <Geode/modify/CCScheduler.hpp>

namespace tc {

namespace {

// Wall-clock budget of one render frame while the analysis runs sped up.
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

        bool const training = s.trainingFastForward();
        if (!s.isAnalyzing() && !training) {
            CCScheduler::update(dt);
            return;
        }

        // The game time of this rendered frame is dt * speed.
        using Clock = std::chrono::steady_clock;
        auto const deadline = Clock::now() + kSpeedUpFrameBudget;

        double const speed = std::clamp(static_cast<double>(cfg::real("count-speed")), 1.0, 100000.0);
        double total = static_cast<double>(dt) * speed;
        if (training) {
            // Up to the hand-over and no further: the player takes it from there.
            auto* play = PlayLayer::get();
            double const left = play ? static_cast<double>(s.training.stopTick - currentTick(play)) / cfg::rawTps() : 0.0;
            total = std::clamp(left, 1.0 / cfg::rawTps(), total);
        }
        int const chunks = std::max(1, static_cast<int>(std::ceil(total / kSpeedUpChunkSeconds)));
        float const chunkDt = static_cast<float>(total / static_cast<double>(chunks));

        for (int i = 0; i < chunks; ++i) {
            performPendingReset();
            CCScheduler::update(chunkDt);
            if (!(s.isAnalyzing() || s.trainingFastForward()) || Clock::now() >= deadline) break;
        }
    }
};
