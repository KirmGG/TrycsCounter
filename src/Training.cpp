// Training of one click: the bot plays the counted run up to shortly before
// it, then a practice checkpoint is placed and the player tries the click again
// and again; each try shows how far it was from the counted timing and whether
// it was inside the window.

#include "App.hpp"

namespace tc {

namespace {

constexpr int kTrainingLabelTag = 0x7F37;

// The checkpoint is placed this long before the click (unless an earlier input
// is closer), and the player is sent back this long after it.
constexpr double kLeadSeconds = 2.0;
constexpr double kAfterSeconds = 1.0;

CCLabelBMFont* trainingLabel(PlayLayer* play) {
    if (!play->m_uiLayer) return nullptr;
    if (auto* label = typeinfo_cast<CCLabelBMFont*>(play->m_uiLayer->getChildByTag(kTrainingLabelTag))) return label;
    auto* label = CCLabelBMFont::create("", "bigFont.fnt");
    label->setTag(kTrainingLabelTag);
    label->setScale(0.4f);
    auto const size = CCDirector::sharedDirector()->getWinSize();
    label->setPosition({size.width / 2.f, size.height - 52.f});
    play->m_uiLayer->addChild(label, 100002);
    return label;
}

void showTraining(PlayLayer* play, std::string const& text, ccColor3B color = {255, 255, 255}) {
    if (auto* label = trainingLabel(play)) {
        label->setString(text.c_str());
        label->setColor(color);
    }
}

// Frames (at the window frame rate) of a distance in measurement units.
double unitsToFrames(int units, int perTick) {
    return static_cast<double>(units) / (static_cast<double>(cfg::rawTps()) * perTick) * cfg::windowFps();
}

void report(PlayLayer* play, InputEvent const* press) {
    auto& s = Session::get();
    auto& t = s.training;
    auto const& target = s.source.events[t.index];
    auto const& window = s.windows[t.index];
    int const units = std::max(1, window.unitsPerTick);
    ++t.tries;
    if (!press) {
        showTraining(play, fmt::format("Click #{}: no click   ({} of {} inside)", t.index + 1, t.inside, t.tries),
            {255, 90, 90});
        return;
    }
    int const offset = eventPos(*press, units) - eventPos(target, units);
    bool const inside = offset >= -window.earlyTicks && offset < window.lateTicks + window.stepTicks;
    if (inside) ++t.inside;
    double const frames = unitsToFrames(offset, units);
    std::string const side = offset == 0 ? "on time" : fmt::format("{:.2f} frames {}", std::abs(frames), offset < 0 ? "early" : "late");
    showTraining(play,
        fmt::format("Click #{}: {}, {} (window -{:.2f} .. +{:.2f})", t.index + 1, side, inside ? "inside" : "outside",
            unitsToFrames(window.earlyTicks, units), unitsToFrames(window.lateTicks + window.stepTicks, units)) +
            fmt::format("   ({} of {} inside)", t.inside, t.tries),
        inside ? ccColor3B{90, 255, 120} : ccColor3B{255, 90, 90});
}

} // namespace

std::string startTraining(size_t index) {
    auto* play = PlayLayer::get();
    auto& s = Session::get();
    if (!resultsForLevel(play)) return "Count the run first.";
    if (index >= s.source.events.size() || index >= s.windows.size() || !s.windows[index].valid)
        return "This click has no counted window.";
    auto const& target = s.source.events[index];
    if (!target.down) return "Only presses can be trained.";
    if (play->m_gameState.m_isDualMode && !target.player1) return "Clicks of player 2 cannot be trained.";

    int const rawTps = cfg::rawTps();
    int stop = std::max(cfg::kRawTicksPerPhysicsFrame, target.tick - static_cast<int>(kLeadSeconds * rawTps));
    // After the input before it: no button is held at the checkpoint.
    if (index > 0) stop = std::max(stop, s.source.events[index - 1].tick + cfg::kRawTicksPerPhysicsFrame);
    if (stop >= target.tick) return "The click comes right after the previous input: there is no time to place a checkpoint.";
    if (!startReplay(play)) return "The counted run cannot be replayed here.";
    s.training = {};
    s.training.active = true;
    s.training.index = index;
    s.training.stopTick = stop;
    log::info("Training click #{}: the bot plays up to raw tick {}, the click is at {}", index + 1, stop, target.tick);
    return {};
}

void stopTraining() {
    auto& s = Session::get();
    if (!s.training.active) return;
    s.training = {};
    s.recordingEnabled = false;
    s.recording.clear();
    s.recordHeld.clear();
    if (auto* play = PlayLayer::get()) {
        if (play->m_uiLayer) play->m_uiLayer->removeChildByTag(kTrainingLabelTag, true);
        s.stage = idleStage(play);
    }
}

// Every frame while training.
void trainingStep(PlayLayer* play) {
    auto& s = Session::get();
    auto& t = s.training;
    if (!t.active || !play->m_player1) return;
    int const tick = currentTick(play);

    if (s.stage == Stage::Replay) {
        if (tick < t.stopTick || play->m_player1->m_isDead) return;
        // The bot hands over: practice from a checkpoint here.
        s.stage = Stage::Idle;
        if (!play->m_isPracticeMode) play->togglePracticeMode(true);
        play->markCheckpoint();
        s.recording.clear();
        s.recordHeld.clear();
        s.recordingEnabled = true;
        s.stage = Stage::Recording;
        showTraining(play, fmt::format("Click #{}: your turn", t.index + 1));
        return;
    }
    if (s.stage != Stage::Recording) {
        stopTraining();
        return;
    }

    auto const& target = s.source.events[t.index];
    bool const dual = play->m_gameState.m_isDualMode && play->m_player2;
    InputEvent const* press = nullptr;
    for (auto const& event : s.recording) {
        if (event.down && event.button == target.button && (event.player1 || !dual)) {
            press = &event;
            break;
        }
    }
    // The press is judged once its step is over (Click Between Frames gives it
    // its place inside the step then).
    if (!t.judged && press && tick > press->tick + cfg::kRawTicksPerPhysicsFrame) {
        t.judged = true;
        report(play, press);
    }
    int const after = target.tick + static_cast<int>(kAfterSeconds * cfg::rawTps());
    if (!t.respawning && (tick > after || play->m_player1->m_isDead)) {
        if (!t.judged) {
            t.judged = true;
            report(play, press);
        }
        t.respawning = true;
        if (!play->m_player1->m_isDead) {
            Loader::get()->queueInMainThread([] {
                if (auto* current = PlayLayer::get(); current && Session::get().training.active) current->resetLevel();
            });
        }
    }
}

// A respawn at the training checkpoint: the next try.
void trainingRespawned() {
    auto& t = Session::get().training;
    t.judged = false;
    t.respawning = false;
}

} // namespace tc
