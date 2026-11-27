// Helpers shared by the counting code: ticks, positions, planner settings, recording and live modes.

#include "App.hpp"

namespace tc {

// Raw tick of the level clock.
int currentTick(GJBaseGameLayer* layer) {
    if (!layer) return 0;
    int tick = layer->m_tickIndex;
    if (static_cast<GJBaseGameLayer*>(PlayLayer::get()) == layer) tick += Session::get().tickOffset;
    return tick;
}

// Puts the level clock back to the tick the checkpoint was created at.
void alignTickToCheckpoint(PlayLayer* play, CheckpointObject* checkpoint) {
    auto& s = Session::get();
    auto const it = s.checkpointTicks.find(checkpoint);
    if (it == s.checkpointTicks.end()) {
        log::debug("Loaded a checkpoint the mod did not see being created; tick clock left as is");
        return;
    }
    s.tickOffset = it->second - play->m_tickIndex;
}

// After any reset: level start means tick 0, a practice respawn means the tick of the last checkpoint.
void alignTickAfterReset(PlayLayer* play) {
    auto& s = Session::get();
    auto* array = play->m_checkpointArray;
    if (s.resettingFromStart || !play->m_isPracticeMode || !array || array->count() == 0) {
        s.tickOffset = 0;
        return;
    }
    alignTickToCheckpoint(play, static_cast<CheckpointObject*>(array->lastObject()));
}

// Keeps the checkpoint tick table small during long practice sessions.
void pruneCheckpointTicks(PlayLayer* play) {
    auto& s = Session::get();
    if (s.checkpointTicks.size() < 4096) return;
    std::unordered_map<CheckpointObject*, int> kept;
    auto keep = [&](CheckpointObject* object) {
        if (auto const it = s.checkpointTicks.find(object); it != s.checkpointTicks.end()) kept.insert(*it);
    };
    if (play->m_checkpointArray) {
        for (auto* object : CCArrayExt<CheckpointObject*>(play->m_checkpointArray)) keep(object);
    }
    for (auto const& snapshot : s.liveSnapshots) keep(snapshot.object.data());
    if (s.liveRestore) keep(s.liveRestore.data());
    s.checkpointTicks = std::move(kept);
    std::erase_if(s.checkpointExtras, [&](auto const& entry) { return !s.checkpointTicks.contains(entry.first); });
}

namespace {

// Orbs and pads of the level: the objects whose used state a checkpoint keeps.
std::vector<EnhancedGameObject*> const& usableObjects(PlayLayer* play) {
    auto& s = Session::get();
    if (s.usableObjectsFound || !play->m_objects) return s.usableObjects;
    s.usableObjectsFound = true;
    for (auto* object : CCArrayExt<GameObject*>(play->m_objects)) {
        switch (object->m_objectType) {
            case GameObjectType::YellowJumpPad: case GameObjectType::PinkJumpPad: case GameObjectType::RedJumpPad:
            case GameObjectType::GravityPad: case GameObjectType::SpiderPad: case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing: case GameObjectType::RedJumpRing: case GameObjectType::GravityRing:
            case GameObjectType::GreenRing: case GameObjectType::DropRing: case GameObjectType::CustomRing:
            case GameObjectType::DashRing: case GameObjectType::GravityDashRing: case GameObjectType::SpiderOrb:
            case GameObjectType::TeleportOrb:
                if (auto* usable = typeinfo_cast<EnhancedGameObject*>(object)) s.usableObjects.push_back(usable);
                break;
            default:
                break;
        }
    }
    return s.usableObjects;
}

} // namespace

void saveCheckpointExtra(PlayLayer* play, CheckpointObject* checkpoint) {
    if (!cfg::flag("exact-checkpoints") || !play->m_player1) return;
    CheckpointExtra extra;
    extra.p1 = PlayerState::of(play->m_player1);
    extra.dual = play->m_gameState.m_isDualMode && play->m_player2;
    if (extra.dual) extra.p2 = PlayerState::of(play->m_player2);
    for (auto* object : usableObjects(play))
        extra.activations.push_back({object, {object->m_activated, object->m_activatedByPlayer1, object->m_activatedByPlayer2}});
    Session::get().checkpointExtras[checkpoint] = std::move(extra);
}

// After a reset that loaded a checkpoint: what the game did not restore.
void applyCheckpointExtra(PlayLayer* play) {
    auto& s = Session::get();
    auto* checkpoint = std::exchange(s.loadingCheckpoint, nullptr);
    if (!checkpoint || !play->m_player1 || !cfg::flag("exact-checkpoints")) return;
    auto const it = s.checkpointExtras.find(checkpoint);
    if (it == s.checkpointExtras.end()) return;
    auto const& extra = it->second;
    bool const dual = play->m_gameState.m_isDualMode && play->m_player2;
    if (extra.dual != dual) return;
    if (extra.p1.sameMode(play->m_player1)) extra.p1.applyTo(play->m_player1);
    if (dual && extra.p2.sameMode(play->m_player2)) extra.p2.applyTo(play->m_player2);
    for (auto const& [object, flags] : extra.activations) {
        object->m_activated = flags[0];
        object->m_activatedByPlayer1 = flags[1];
        object->m_activatedByPlayer2 = flags[2];
    }
}

// Where attempts begin now: the X of the active Start Pos, -1 for the level start.
float startPosX(PlayLayer* play) {
    return play && play->m_startPosObject ? play->m_startPosObject->getPositionX() : -1.f;
}

std::string startPlace(float x) {
    return x < 0.f ? std::string("the start of the level") : fmt::format("the Start Pos at x {:.0f}", x);
}

// Empty when a run that began at `startX` can be replayed from where the level starts now.
std::string startMismatch(PlayLayer* play, float startX) {
    float const now = startPosX(play);
    if (std::abs(now - startX) < 0.5f) return {};
    return fmt::format("This run began at {}, but the level now starts at {}. Choose the same Start Pos.",
        startPlace(startX), startPlace(now));
}

int currentLevelID(PlayLayer* layer) {
    if (!layer || !layer->m_level) return 0;
    return static_cast<int>(layer->m_level->m_levelID.value());
}

// Position of a player in object layer space.
CCPoint playerPoint(GJBaseGameLayer* layer, bool player1) {
    if (!layer || !layer->m_objectLayer) return CCPointZero;
    auto player = player1 ? layer->m_player1 : layer->m_player2;
    if (!player || !player->getParent()) player = layer->m_player1;
    if (!player || !player->getParent()) return CCPointZero;
    auto const world = player->getParent()->convertToWorldSpace(player->getPosition());
    return layer->m_objectLayer->convertToNodeSpace(world);
}

// Where the marker of a click goes.
void placeEvent(GJBaseGameLayer* layer, InputEvent& event) {
    bool const dual = layer->m_gameState.m_isDualMode && layer->m_player2;
    bool const twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    if (!dual || twoPlayer) {
        auto const point = playerPoint(layer, event.player1);
        event.x = point.x;
        event.y = point.y;
        event.dual = false;
        return;
    }
    auto const p1 = playerPoint(layer, true);
    auto const p2 = playerPoint(layer, false);
    bool const p1Lower = p1.y <= p2.y;
    auto const& lower = p1Lower ? p1 : p2;
    auto const& other = p1Lower ? p2 : p1;
    event.x = lower.x;
    event.y = lower.y;
    event.dual = true;
    event.otherP2 = p1Lower;
    event.ox = other.x;
    event.oy = other.y;
}

// The click as its marker shows it (Deep Count may move a dual's marker).
InputEvent markerEvent(RunView const& run, size_t index) {
    auto event = run.events[index];
    if (event.dual && index < run.otherMarker.size() && run.otherMarker[index]) {
        event.x = event.ox;
        event.y = event.oy;
    }
    return event;
}

// Raw tick at which a recorded input is injected.
int injectTick(Session const& s, InputEvent const& event) {
    return std::max(0, event.tick + s.activeOffset());
}

// Index of the release that belongs to the press at `index`, or npos.
size_t partnerOf(std::vector<InputEvent> const& events, size_t index) {
    if (index >= events.size() || !events[index].down) return std::string::npos;
    for (size_t i = index + 1; i < events.size(); ++i) {
        if (events[i].button == events[index].button && events[i].player1 == events[index].player1)
            return events[i].down ? std::string::npos : i;
    }
    return std::string::npos;
}

bool moveHold() {
    return cfg::flag("move-release-with-press");
}

bool adaptClicks() {
    return cfg::flag("adapt-next-clicks");
}

namespace {

// Raw ticks a free-flight click must keep the player alive after the next click.
int reactionTicks() {
    return cfg::framesToTicks(cfg::integer("free-flight-reaction"));
}

} // namespace

// Survival horizon after the input `index` of the run: short in free flight.
int horizonTicks(Session const& s, size_t index) {
    if (index < s.freeFlight.size() && s.freeFlight[index]) return reactionTicks();
    return cfg::lookaheadTicks();
}

std::vector<PlannerInput> plannerInputs(std::vector<InputEvent> const& events, int units,
    std::vector<char> const* freeFlight) {
    std::vector<PlannerInput> inputs;
    inputs.reserve(events.size());
    bool const carry = moveHold();
    int const reaction = reactionTicks() * units;
    for (size_t i = 0; i < events.size(); ++i) {
        auto const& e = events[i];
        PlannerInput input{eventPos(e, units), e.button, e.player1, true,
            carry && partnerOf(events, i) != std::string::npos};
        if (freeFlight && i < freeFlight->size() && (*freeFlight)[i]) input.lookahead = reaction;
        inputs.push_back(input);
    }
    return inputs;
}

PlannerConfig plannerConfig(Session const& s, int endTick, RunEnd endKind) {
    PlannerConfig config;
    config.step = s.stepTicks;
    config.maxShift = cfg::maxShiftTicks() * s.units;
    config.lookahead = cfg::lookaheadTicks() * s.units;
    config.maxWindowTicks = cfg::maxWindowTicks() * s.units;
    config.endTick = endTick * s.units;
    config.endKind = endKind;
    config.unitsPerTick = s.units;
    config.relaxNeighbors = true;
    // Every physics frame of each side is tested outwards from the click.
    int const frameSteps = std::max(1, cfg::kRawTicksPerPhysicsFrame * s.units / std::max(1, s.stepTicks));
    config.coarseSteps = frameSteps;
    config.gallop = cfg::flag("fast-search");
    config.firstCandidate = frameSteps;
    return config;
}

// Recording is on after "Start Recording" in the pause menu, or in Live Counter mode.
bool recordingWanted() {
    return Session::get().recordingEnabled || cfg::flag("live-counter");
}

// Live Counter simulates in practice mode only.
bool liveMode() {
    return cfg::flag("live-counter") && !Session::get().liveDisabled;
}

// Live Counter simulates copies of the player unless the exact mode that reloads practice checkpoints is
// selected.
bool trajectoryLive() {
    return cfg::text("live-mode") != "Exact";
}

// Recording works in normal mode and in practice mode.
Stage idleStage(PlayLayer* play) {
    if (!play) return Stage::Idle;
    return recordingWanted() ? Stage::Recording : Stage::Idle;
}

void sortByTick(std::vector<InputEvent>& events) {
    std::stable_sort(events.begin(), events.end(), [](InputEvent const& a, InputEvent const& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.frac < b.frac;
    });
}

void forceClickOnSteps(GJBaseGameLayer* layer) {
    if (!layer || !cfg::flag("force-click-on-steps")) return;
    if (layer->m_clickBetweenSteps) log::info("Click Between Steps was on, switched off for exact replays.");
    layer->m_clickBetweenSteps = false;
}

void muteAudio(bool mute) {
    auto& s = Session::get();
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;
    if (mute) {
        if (s.audioMuted || !cfg::flag("mute-during-count")) return;
        s.savedMusicVolume = engine->m_musicVolume;
        s.savedSfxVolume = engine->m_sfxVolume;
        engine->setBackgroundMusicVolume(0.f);
        engine->setEffectsVolume(0.f);
        s.audioMuted = true;
    } else if (s.audioMuted) {
        engine->setBackgroundMusicVolume(s.savedMusicVolume);
        engine->setEffectsVolume(s.savedSfxVolume);
        s.audioMuted = false;
    }
}

// GameStatsManager keys: "1" jumps, "2" attempts.
void freezePlayerStats(PlayLayer* play, bool testRun) {
    auto& s = Session::get();
    if (!play || s.frozenStats) return;
    auto* stats = GameStatsManager::sharedState();
    Session::PlayerStats saved;
    saved.attempts = play->m_attempts;
    saved.jumps = play->m_jumps;
    saved.clicks = play->m_clicks;
    saved.attemptTime = play->m_attemptTime;
    if (auto* level = play->m_level) {
        saved.levelAttempts = level->m_attempts;
        saved.levelJumps = level->m_jumps;
        saved.levelClicks = level->m_clicks;
        saved.levelAttemptTime = level->m_attemptTime;
    }
    saved.totalJumps = stats->getStat("1");
    saved.totalAttempts = stats->getStat("2");
    saved.testMode = play->m_isTestMode;
    s.frozenStats = saved;
    if (testRun) play->m_isTestMode = true;
}

void restorePlayerStats(PlayLayer* play) {
    auto& s = Session::get();
    if (!s.frozenStats) return;
    auto const saved = *s.frozenStats;
    s.frozenStats.reset();
    if (!play) return;
    play->m_attempts = saved.attempts;
    play->m_jumps = saved.jumps;
    play->m_clicks = saved.clicks;
    play->m_attemptTime = saved.attemptTime;
    if (auto* level = play->m_level) {
        level->m_attempts = saved.levelAttempts;
        level->m_jumps = saved.levelJumps;
        level->m_clicks = saved.levelClicks;
        level->m_attemptTime = saved.levelAttemptTime;
    }
    auto* stats = GameStatsManager::sharedState();
    stats->setStat("1", saved.totalJumps);
    stats->setStat("2", saved.totalAttempts);
    play->m_isTestMode = saved.testMode;
}

} // namespace tc
