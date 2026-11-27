// Hooks of GJBaseGameLayer: inputs and physics steps.

#include "../App.hpp"
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace tc;

class $modify(TCBaseLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        // Before Click Between Frames reads the queued real inputs.
        (void)self.setHookPriority("GJBaseGameLayer::getModifiedDelta", Priority::First);
    }

    // While the mod drives the input.
    double getModifiedDelta(float dt) {
        auto* play = PlayLayer::get();
        if (play && static_cast<GJBaseGameLayer*>(play) == this && Session::get().drivesInput())
            m_queuedButtons.clear();
        return GJBaseGameLayer::getModifiedDelta(dt);
    }

    // ---- Live Counter trajectory simulation: no side effects on the level ----
    // Gravity changes of a copy turn only the copy, without the level effects.
    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (Session::get().trajSimulating) {
            if (player) player->flipGravity(flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    // A copy never fires the level's event triggers.
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (Session::get().trajSimulating) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    // Copies collide with blocks, hazards, slopes, orbs, pads and gravity portals.
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        auto& s = Session::get();
        if (!s.trajSimulating || !objects) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
            return;
        }
        gd::vector<GameObject*> kept;
        kept.reserve(static_cast<size_t>(std::max(count, 0)));
        for (int i = 0; i < count && static_cast<size_t>(i) < objects->size(); ++i) {
            auto* object = objects->at(static_cast<size_t>(i));
            if (!object) continue;
            bool interactive = false;
            switch (object->m_objectType) {
                case GameObjectType::Solid:
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard:
                case GameObjectType::Slope:
                    break;
                case GameObjectType::YellowJumpPad:
                case GameObjectType::PinkJumpPad:
                case GameObjectType::RedJumpPad:
                case GameObjectType::GravityPad:
                case GameObjectType::YellowJumpRing:
                case GameObjectType::PinkJumpRing:
                case GameObjectType::RedJumpRing:
                case GameObjectType::GravityRing:
                case GameObjectType::GreenRing:
                case GameObjectType::DropRing:
                case GameObjectType::InverseGravityPortal:
                case GameObjectType::NormalGravityPortal:
                case GameObjectType::GravityTogglePortal:
                    interactive = true;
                    break;
                default:
                    continue;
            }
            if (interactive) {
                auto* enhanced = typeinfo_cast<EnhancedGameObject*>(object);
                if (!enhanced) continue;
                if (!s.simSavedFlags.contains(enhanced)) {
                    s.simSavedFlags[enhanced] = {enhanced->m_activated, enhanced->m_activatedByPlayer1, enhanced->m_activatedByPlayer2};
                    ActivationFlags flags{false, false, false};
                    if (s.simRunFlags) {
                        if (auto const it = s.simRunFlags->find(enhanced); it != s.simRunFlags->end()) flags = it->second;
                    }
                    enhanced->m_activated = flags[0];
                    enhanced->m_activatedByPlayer1 = flags[1];
                    enhanced->m_activatedByPlayer2 = flags[2];
                }
            }
            kept.push_back(object);
        }
        GJBaseGameLayer::collisionCheckObjects(player, &kept, static_cast<int>(kept.size()), dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = Session::get();
        auto* play = PlayLayer::get();
        bool const isPlayLayer = play && static_cast<GJBaseGameLayer*>(play) == this;

        if (isPlayLayer && s.drivesInput()) {
            // Real input is blocked, only the recorded one may pass.
            if (!s.injecting) return;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        if (isPlayLayer && s.stage == Stage::Recording) recordInput(this, down, button, isPlayer1);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = Session::get();
        auto* play = PlayLayer::get();
        bool const isPlayLayer = play && static_cast<GJBaseGameLayer*>(play) == this;
        bool const driving = isPlayLayer && s.drivesInput();

        // Resets wait for the end of the rendered frame (performPendingReset).
        if (driving && s.startFromBeginning) {
            s.startFromBeginning = false;
            s.pendingReset = Session::PendingReset::FromStart;
        }
        if (driving && s.pendingReset != Session::PendingReset::None) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        if (isPlayLayer && s.stage == Stage::Recording) {
            int const now = currentTick(this);
            rollCbfStep(s, now);
            syncHeldButtons(play, s, now);
            liveStep(play, now);
        }
        if (driving && !s.liveSim) applyStateFixes(play, s, currentTick(this));
        if (isPlayLayer) {
            int const now = currentTick(this);
            if (s.stage == Stage::Recording) {
                tracePlayer(play, s.trace, now);
            } else if (s.stage == Stage::BaselineReplay) {
                traceBaseline(play, s, now);
                if (s.borderMode) recordBorderBaseline(play, s, now);
            } else if (s.stage == Stage::Probing && !s.liveSim) {
                verifyTrialPrefix(play, s, now);
                checkConvergence(play, s, now);
                if (s.borderMode) recordBorderTrial(play, s, now);
            }
        }

        if (driving) injectDueInputs(play, this, s);

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (!driving) return;
        int const tick = currentTick(this);
        bool const alive = m_player1 && !m_player1->m_isDead;
        // Reaching the end portal counts as reaching the end of the run.
        bool const atEnd = m_levelEndAnimationStarted;

        if (s.stage == Stage::BaselineReplay) {
            if (alive && (tick >= s.source.endTick || atEnd)) {
                if (acceptBaseline(s)) startProbing(s);
                s.pendingReset = Session::PendingReset::Level;
            }
        } else if (s.stage == Stage::Probing) {
            if (alive && s.pendingOutcome == ProbeOutcome::None && (tick >= s.trialEndTick || tick >= s.trialHitLimit || atEnd)) {
                s.pendingOutcome = ProbeOutcome::Survived;
                s.pendingReset = Session::PendingReset::Level;
            }
        } else if (s.stage == Stage::Replay) {
            if (s.source.endKind != RunEnd::Completed && tick >= s.source.endTick) {
                s.stage = Stage::Idle;
                Notification::create("Replay finished", NotificationIcon::Success)->show();
            }
        }
    }
};
