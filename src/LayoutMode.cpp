// Layout mode while counting: decoration hidden, visual-only triggers off.

#include "App.hpp"

namespace tc {

namespace {

// Like Eclipse's layout mode: nothing is taken out of the level.

// Triggers that only change how the level looks.
bool isVisualTrigger(int id) {
    switch (id) {
    case 29: case 30: case 104: case 105: case 221: case 717: case 718: case 743: case 744:
    case 899: case 900: case 915:      // colour triggers (old and new)
    case 1006:                         // pulse
    case 1007:                         // alpha
    case 1520:                         // shake
    case 1818: case 1819:              // background effect on / off
    case 2903:                         // gradient
    case 2999:                         // middleground config
    case 3029: case 3030: case 3031:   // background / ground / middleground change
        return true;
    default:
        return false;
    }
}

} // namespace

bool layoutSkipsTrigger(int id) {
    return isVisualTrigger(id);
}

bool layoutBlocksTrigger(EffectGameObject* trigger) {
    auto const& s = Session::get();
    return s.layoutActive && s.isAnalyzing() && !s.liveSim && isVisualTrigger(trigger->m_objectID);
}

void hideDecoration(PlayLayer* play) {
    auto& s = Session::get();
    if (!play || !play->m_objects || s.liveSim || s.layoutOff || s.layoutActive || !cfg::flag("layout-mode")) return;
    s.layoutHidden.clear();
    for (auto* object : CCArrayExt<GameObject*>(play->m_objects)) {
        if (!object || object->m_objectType != GameObjectType::Decoration || object->m_isTrigger) continue;
        if (typeinfo_cast<EffectGameObject*>(object)) continue;
        s.layoutHidden.push_back({object, object->m_isHide});
        object->m_isHide = true;
    }
    s.layoutActive = true;
    log::info("Layout mode: {} decoration objects hidden, visual triggers off while counting", s.layoutHidden.size());
}

namespace {

void restoreDecoration(PlayLayer* play) {
    auto& s = Session::get();
    (void)play;
    if (!s.layoutActive) return;
    for (auto& [object, hidden] : s.layoutHidden) {
        if (object) object->m_isHide = hidden;
    }
    log::info("Layout mode: {} decoration objects shown again", s.layoutHidden.size());
    s.layoutHidden.clear();
    s.layoutActive = false;
}

} // namespace

// The baseline replay did not follow the attempt with the decoration switched off.
bool layoutFallback(Session& s) {
    if (!s.layoutActive) return false;
    restoreDecoration(PlayLayer::get());
    s.layoutOff = true;
    s.baselineRetry = true;
    log::warn("Layout mode: the replay did not follow the attempt without decoration; counting with the full level");
    return true;
}

// Undo everything the analysis changed in the game.
void endAnalysisEnvironment(PlayLayer* play) {
    auto& s = Session::get();
    overlay::setCountingScreen(play, false);
    s.injecting = false;
    s.pendingOutcome = ProbeOutcome::None;
    s.deepMode = false;
    s.immunePlayer = 0;
    s.borderMode = false;
    s.deepJobs.clear();
    s.deepJobActive = false;
    s.verifyMode = false;
    s.edgeCheckRan = false;
    s.strictTrials = false;
    s.approach.clear();
    s.deepRebase.clear();
    restoreDecoration(play);
    muteAudio(false);
    if (play && !s.wasPracticeMode && play->m_isPracticeMode) play->togglePracticeMode(false);
    restorePlayerStats(play);
}

} // namespace tc
