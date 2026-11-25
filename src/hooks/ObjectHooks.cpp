// Hooks of players and objects, and the reaction to display settings.

#include "../App.hpp"
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GameObject.hpp>

using namespace tc;

// Ours runs innermost, so while recording it sees every sub-step CBF makes, and
// while replaying CBF passes the whole step through to it.

class $modify(TCPlayer, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::update", Priority::VeryLate);
        (void)self.setHookPriority("PlayerObject::updateRotation", Priority::VeryLate);
    }

    void update(float dt) {
        auto& s = Session::get();
        auto* play = PlayLayer::get();
        if (!play || s.cbfMidStep || (this != play->m_player1 && this != play->m_player2)) {
            PlayerObject::update(dt);
            return;
        }

        // A decided trial waits for its reset: its players need not move any more.
        if (s.isAnalyzing() && !s.liveSim && s.pendingReset != Session::PendingReset::None) return;

        bool const isP1 = this == play->m_player1;
        int const tick = currentTick(play);
        if (isP1) {
            rollCbfStep(s, tick);
            s.cbfSplitP1 = false;
            s.cbfSplitP2 = false;
            s.cbfSplitTick = std::numeric_limits<int>::min();
            if (s.drivesInput() && replayCbfStep(play, this, dt, tick)) return;
        } else if (s.drivesInput() && s.cbfSplitTick == tick) {
            // Player 2 was already stepped inside the split update of player 1, as CBF does.
            return;
        }

        float const stepDt = s.drivesInput() ? replayStepDt(s, tick, isP1, dt) : dt;
        PlayerObject::update(stepDt);
        if (isP1) s.cbfAcc1 += stepDt;
        else s.cbfAcc2 += stepDt;
    }

    // ---- Live Counter trajectory simulation: no side effects ----
    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (Session::get().trajSimulating) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (Session::get().trajSimulating) return;
        PlayerObject::incrementJumps();
    }

    // After a split step the game rotates the player by the last sub-step only and
    // keeps the position from before the step (what CBF does for move triggers).
    void updateRotation(float dt) {
        auto& s = Session::get();
        auto* play = PlayLayer::get();
        if (play && !s.cbfMidStep && s.drivesInput()) {
            if (this == play->m_player1 && s.cbfSplitP1) {
                PlayerObject::updateRotation(s.cbfRotDelta);
                m_lastPosition = s.cbfP1Pos;
                return;
            }
            if (this == play->m_player2 && s.cbfSplitP2) {
                PlayerObject::updateRotation(s.cbfRotDelta);
                m_lastPosition = s.cbfP2Pos;
                return;
            }
        }
        PlayerObject::updateRotation(dt);
    }
};

class $modify(TCHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (Session::get().trajSimulating) return;
        HardStreak::addPoint(point);
    }
};

// Display settings changed in the middle of a level: the counter and the
// markers are built again (the clicks already passed are counted silently).
$on_mod(Loaded) {
    auto redraw = [] {
        Loader::get()->queueInMainThread([] {
            auto* play = PlayLayer::get();
            auto& s = Session::get();
            if (!play || s.isAnalyzing()) return;
            refreshDisplay(play);
        });
    };
    listenForSettingChanges<std::string>("hud-side", [redraw](std::string) { redraw(); });
    listenForSettingChanges<double>("decimals-below", [redraw](double) { redraw(); });
    listenForSettingChanges<int64_t>("hard-decimals", [redraw](int64_t) { redraw(); });
    listenForSettingChanges<int64_t>("easy-decimals", [redraw](int64_t) { redraw(); });
}

class $modify(TCEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (Session::get().trajSimulating) return;
        if (layoutBlocksTrigger(this)) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(TCGameObject, GameObject) {
    void playShineEffect() {
        if (Session::get().trajSimulating) return;
        GameObject::playShineEffect();
    }
};
