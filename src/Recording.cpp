// Recording of the player's inputs.

#include "App.hpp"

namespace tc {

// Records an input only when it changes the held state of its button. Stale and
// duplicate callbacks that GD emits during resets therefore never reach the
// recording, while real clicks are kept from the very first tick.
void recordInput(GJBaseGameLayer* layer, bool down, int button, bool player1) {
    auto& s = Session::get();
    if (!layer->m_player1 || layer->m_player1->m_isDead || layer->m_levelEndAnimationStarted) return;

    auto& held = s.recordHeld[{button, player1}];
    if (held == down) return;
    held = down;

    auto const point = playerPoint(layer, player1);
    int const tick = currentTick(layer);
    InputEvent event{tick, down, button, player1, point.x, point.y, layer->m_isBetweenSteps};
    if (cbfSplitsSteps()) {
        rollCbfStep(s, tick);
        // Only an input that arrives after part of the step was simulated is a
        // between-steps input. Inputs at the start of a step (bots, CBF while the
        // player is airborne) are ordinary on-step inputs.
        float const done = std::max(s.cbfAcc1, s.cbfAcc2);
        if (done > 0.f) {
            event.cbf = true;
            s.cbfPending.push_back({s.recording.size(), static_cast<double>(done)});
        }
    }
    s.recording.push_back(event);
    log::debug("Recorded input #{} at raw tick {}: button={}, down={}, P1={}, betweenSteps={}, cbf={}",
        s.recording.size() - 1, tick, button, down, player1, layer->m_isBetweenSteps, event.cbf);

    // Live Counter: the window of this input is simulated once enough gameplay follows it.
    if (liveMode() && (down || cfg::flag("show-releases"))) {
        if (trajectoryLive()) s.liveStartQueue.push_back(s.recording.size() - 1);
        else if (layer->m_isPracticeMode) s.liveJobs.push_back({s.recording.size() - 1, tick + cfg::liveVerdictDelayTicks()});
    }
}

// The attempt recorded so far, ending at `endTick` with `kind`.
RunRecord recordedRun(PlayLayer* play, int endTick, RunEnd kind) {
    auto const& s = Session::get();
    RunRecord run;
    run.events = s.recording;
    run.endTick = endTick;
    run.endKind = kind;
    run.hits = s.recordingHits;
    run.trace = s.trace;
    run.stepDts = s.stepDts;
    run.respawns = s.recordingRespawns;
    run.startX = startPosX(play);
    return run;
}

// The run an analysis should start from: the attempt in progress, otherwise the
// last completion, otherwise the last attempt that ended in a death.
RunRecord selectRun(PlayLayer* play) {
    auto& s = Session::get();
    noteStepDt(s);
    finalizeCbfStep(s);
    if (!s.recording.empty()) {
        bool const died = s.recordingDeathTick > 0;
        return recordedRun(play, died ? s.recordingDeathTick - 1 : currentTick(play), died ? RunEnd::Death : RunEnd::Paused);
    }
    if (!s.previousCompletion.empty()) return s.previousCompletion;
    return s.previousRun;
}

} // namespace tc
