#pragma once
// What a practice checkpoint of GD does not bring back: part of the player's
// physics state and the used state of orbs and pads. Without it a respawn puts
// the player back a little differently than playing straight through, and a
// replay of the attempt (or a trial started from the checkpoint) drifts away.

#include <Geode/Geode.hpp>

#include <array>
#include <utility>
#include <vector>

namespace tc {

// Plain physics fields of PlayerObject. Game mode, size, speed and gravity are
// left to the game: it restores them together with their sprites.
#define TC_PLAYER_FIELDS(X) \
    X(m_wasTeleported) X(m_fixGravityBug) X(m_reverseSync) X(m_yVelocityBeforeSlope) X(m_slopeStartTime) \
    X(m_justPlacedStreak) X(m_maybeLastGroundObject) X(m_lastCollisionBottom) X(m_lastCollisionTop) \
    X(m_lastCollisionLeft) X(m_lastCollisionRight) X(m_currentSlope2) X(m_preLastGroundObject) X(m_slopeAngle) \
    X(m_slopeSlidingMaybeRotated) X(m_collidedObject) X(m_lastGroundObject) X(m_collidingWithLeft) \
    X(m_collidingWithRight) X(m_groundYVelocity) X(m_yVelocityRelated) \
    X(m_isCollidingWithSlope) X(m_isBallRotating) X(m_currentPotentialSlope) X(m_currentSlope) \
    X(m_collidingWithSlopeId) X(m_slopeFlipGravityRelated) X(m_slopeAngleRadians) X(m_rotationSpeed) \
    X(m_rotateSpeed) X(m_isRotating) X(m_isBallRotating2) X(m_speedMultiplier) X(m_yStart) X(m_gravity) \
    X(m_padRingRelated) X(m_maybeIsFalling) X(m_isOnGround3) X(m_lastJumpTime) X(m_lastFlipTime) \
    X(m_lastSpiderFlipTime) X(m_accelerationOrSpeed) X(m_snapDistance) X(m_ringJumpRelated) X(m_objectSnappedTo) \
    X(m_slopeRotation) X(m_currentSlopeYVelocity) X(m_blackOrbRelated) X(m_isAccelerating) X(m_isCurrentSlopeTop) \
    X(m_collidedTopMinY) X(m_collidedBottomMaxY) X(m_collidedLeftMaxX) X(m_collidedRightMinX) X(m_jumpBuffered) \
    X(m_stateRingJump) X(m_wasJumpBuffered) X(m_wasRobotJump) X(m_stateJumpBuffered) X(m_stateRingJump2) \
    X(m_touchedRing) X(m_touchedCustomRing) X(m_touchedGravityPortal) X(m_maybeTouchedBreakableBlock) \
    X(m_touchedPad) X(m_yVelocity) X(m_fallSpeed) X(m_isOnSlope) X(m_wasOnSlope) X(m_slopeVelocity) \
    X(m_maybeUpsideDownSlope) X(m_isOnGround) X(m_reverseRelated) X(m_maybeReverseSpeed) \
    X(m_maybeReverseAcceleration) X(m_xVelocityRelated2) X(m_groundObjectMaterial) X(m_isOnGround2) \
    X(m_lastLandTime) X(m_platformerVelocityRelated) X(m_maybeIsBoosted) X(m_scaleXRelatedTime) \
    X(m_decreaseBoostSlide) X(m_isLocked) X(m_lastActivatedPortal) X(m_hasEverJumped) X(m_hasEverHitRing) \
    X(m_totalTime) X(m_yVelocityRelated3) X(m_stateOnGround) X(m_stateUnk) X(m_stateNoStickX) X(m_stateNoStickY) \
    X(m_stateUnk2) X(m_stateBoostX) X(m_stateBoostY) X(m_maybeStateForce2) X(m_stateScale) \
    X(m_platformerXVelocity) X(m_scaleXRelated) X(m_maybeHasStopped) X(m_xVelocityRelated) \
    X(m_maybeGoingCorrectSlopeDirection) X(m_isSliding) X(m_maybeSlopeForce) X(m_isOnIce) X(m_physDeltaRelated) \
    X(m_isOnGround4) X(m_maybeSlidingTime) X(m_maybeSlidingStartTime) X(m_changedDirectionsTime) X(m_slopeEndTime) \
    X(m_isMoving) X(m_platformerMovingLeft) X(m_platformerMovingRight) X(m_isSlidingRight) \
    X(m_maybeChangedDirectionAngle) X(m_stateNoAutoJump) X(m_stateDartSlide) X(m_stateHitHead) \
    X(m_stateFlipGravity) X(m_stateForce) X(m_affectedByForces) X(m_lastMovedTime) X(m_fixRobotJump) \
    X(m_fallStartY) X(m_lastPosition) X(m_position)

struct PlayerState {
#define TC_DECLARE(name) decltype(PlayerObject::name) name{};
    TC_PLAYER_FIELDS(TC_DECLARE)
#undef TC_DECLARE
    float rotation = 0.f;
    // The game restores these; the state is only applied when they match.
    bool isShip = false, isBird = false, isBall = false, isDart = false, isRobot = false, isSpider = false,
         isSwing = false, isUpsideDown = false;
    float vehicleSize = 1.f;

    static PlayerState of(PlayerObject* player) {
        PlayerState state;
#define TC_SAVE(name) state.name = player->name;
        TC_PLAYER_FIELDS(TC_SAVE)
#undef TC_SAVE
        state.rotation = player->getRotation();
        state.isShip = player->m_isShip;
        state.isBird = player->m_isBird;
        state.isBall = player->m_isBall;
        state.isDart = player->m_isDart;
        state.isRobot = player->m_isRobot;
        state.isSpider = player->m_isSpider;
        state.isSwing = player->m_isSwing;
        state.isUpsideDown = player->m_isUpsideDown;
        state.vehicleSize = player->m_vehicleSize;
        return state;
    }

    bool sameMode(PlayerObject* player) const {
        return isShip == player->m_isShip && isBird == player->m_isBird && isBall == player->m_isBall &&
            isDart == player->m_isDart && isRobot == player->m_isRobot && isSpider == player->m_isSpider &&
            isSwing == player->m_isSwing && isUpsideDown == player->m_isUpsideDown && vehicleSize == player->m_vehicleSize;
    }

    void applyTo(PlayerObject* player) const {
#define TC_LOAD(name) player->name = name;
        TC_PLAYER_FIELDS(TC_LOAD)
#undef TC_LOAD
        player->setPosition(m_position);
        player->setRotation(rotation);
    }
};

#undef TC_PLAYER_FIELDS

// Everything kept for one checkpoint.
struct CheckpointExtra {
    PlayerState p1;
    PlayerState p2;
    bool dual = false;
    // Orbs and pads: used, used by player 1, used by player 2.
    std::vector<std::pair<EnhancedGameObject*, std::array<bool, 3>>> activations;
};

} // namespace tc
