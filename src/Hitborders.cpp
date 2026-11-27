// Make Hitborders: where the players can be around every click, NaN GD style.

#include "App.hpp"
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/PlayLayer.hpp>

namespace tc {

namespace {

// Points closer than this to the previous one are not drawn.
constexpr float kMinStep = 1.f;

constexpr float kBorderWidth = 1.5f;
constexpr float kTrailRadius = 0.5f;
// A longer step of the trail is a teleport or a respawn.
constexpr float kTrailMaxStep = 60.f;
constexpr int kTrailTag = 1;

constexpr ccColor4F kMask{1.f, 1.f, 1.f, 1.f};
constexpr ccColor4F kBandFill{1.f, 0.5f, 0.f, 0.25f};
constexpr ccColor4F kBandBorder{1.f, 0.5f, 0.f, 1.f};
constexpr ccColor4F kTrail{1.f, 1.f, 1.f, 0.9f};

// Stencil passes: the bands of all clicks become one shape, filled once and outlined only along its edge.
constexpr GLint kStencilFree = 0x80;   // not drawn on yet
constexpr GLint kStencilUnion = 0x01;  // inside the bands
constexpr GLint kStencilInset = 0x02;  // inside the bands shrunk by the border width

// Sets GL state when visited, between the draw nodes of the passes.
class GLStateNode : public CCNode {
    std::function<void()> m_apply;

public:
    static GLStateNode* create(std::function<void()> apply) {
        auto* node = new GLStateNode();
        node->m_apply = std::move(apply);
        node->init();
        node->autorelease();
        return node;
    }

    void draw() override { m_apply(); }
};

void beginUnionPass() {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(kStencilFree);
    glClear(GL_STENCIL_BUFFER_BIT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilMask(kStencilUnion);
    glStencilFunc(GL_ALWAYS, kStencilUnion, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

void beginInsetPass() {
    glStencilMask(kStencilInset);
    glStencilFunc(GL_ALWAYS, kStencilInset, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

// The border is what is inside the bands but not inside their inset.
void beginBorderPass() {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(kStencilFree | kStencilUnion);
    glStencilFunc(GL_EQUAL, kStencilFree | kStencilUnion, kStencilFree | kStencilUnion | kStencilInset);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
}

void beginFillPass() {
    glStencilMask(kStencilFree | kStencilUnion | kStencilInset);
    glStencilFunc(GL_EQUAL, kStencilFree | kStencilUnion, kStencilFree | kStencilUnion);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
}

void endPasses() {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_STENCIL_TEST);
}

CCDrawNode* blendedDrawNode() {
    auto* node = CCDrawNode::create();
    node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    node->m_bUseArea = false;
    return node;
}

// A point moved towards `to` by the border width, or halfway for a thin band.
CCPoint inward(CCPoint from, CCPoint to) {
    auto const d = to - from;
    float const length = d.getLength();
    if (length <= 2.f * kBorderWidth) return from + d * 0.5f;
    return from + d * (kBorderWidth / length);
}

PathPoint pointOf(PlayLayer* play, bool player1, int tick) {
    auto const p = playerPoint(play, player1);
    return {tick, p.x, p.y};
}

PathPoint const* pointAt(std::vector<PathPoint> const& path, int tick) {
    auto it = std::lower_bound(path.begin(), path.end(), tick, [](PathPoint const& p, int t) { return p.tick < t; });
    return it != path.end() && it->tick == tick ? &*it : nullptr;
}

} // namespace

// The jobs of the pass: both edges of every counted click (Deep Count variant 6).
bool prepareBorderJobs(Session& s) {
    auto const& events = s.source.events;
    s.deepJobs.clear();
    s.borderShifts.clear();
    s.borderBest.clear();
    s.borders.clicks.clear();
    for (size_t k = 0; k < events.size() && k < s.deepBase.size(); ++k) {
        auto const& w = s.deepBase[k];
        if (!w.valid) continue;
        auto& shifts = s.borderShifts[k];
        if (w.earlyTicks > 0) shifts.push_back(-w.earlyTicks);
        if (w.lateTicks > 0) shifts.push_back(w.lateTicks);
        bool const wide = w.exceeded || w.earlyCapped || w.lateCapped;
        if (!shifts.empty() || wide) s.deepJobs.push_back({k, k, 0, 6, 1.0});
    }
    s.deepJobsTotal = s.deepJobs.size();
    s.deepJobActive = false;
    log::info("Hitborders: {} clicks", s.deepJobsTotal);
    return !s.deepJobs.empty();
}

// Baseline replay of the pass: the run's own path.
void recordBorderBaseline(PlayLayer* play, Session& s, int tick) {
    auto& b = s.borders;
    if (!b.path1.empty() && tick <= b.path1.back().tick) {
        // Another alignment starts the replay again.
        b.path1.clear();
        b.path2.clear();
    }
    if (!play->m_player1) return;
    b.path1.push_back(pointOf(play, true, tick));
    if (play->m_gameState.m_isDualMode && play->m_player2) b.path2.push_back(pointOf(play, false, tick));
}

// A trial of the pass: the paths of the players from the moved click on.
void recordBorderTrial(PlayLayer* play, Session& s, int tick) {
    if (!s.deepJobActive || s.deepCurrent.variant != 6) return;
    auto& [path1, path2] = s.borderTrial;
    // The trial runs again (a checkpoint it started from was rejected).
    if (!path1.empty() && tick <= path1.back().tick) {
        path1.clear();
        path2.clear();
    }
    size_t const k = s.deepCurrent.index;
    if (tick < s.source.events[k].tick + std::min(0, s.activeTrial.shift) - cfg::kRawTicksPerPhysicsFrame) return;
    path1.push_back(pointOf(play, true, tick));
    if (play->m_gameState.m_isDualMode && play->m_player2) path2.push_back(pointOf(play, false, tick));
}

// The trial is settled: the farthest one that lived on is the path at that edge.
void keepBorderTrial(Session& s, bool survived) {
    if (!s.borderMode || !s.deepJobActive || s.deepCurrent.variant != 6) return;
    int const shift = s.activeTrial.shift;
    size_t const k = s.deepCurrent.index;
    int const side = shift < 0 ? 0 : 1;
    if (survived && shift != 0) {
        auto& best = s.borderBest[{k, side}];
        if (std::abs(shift) >= best) {
            best = std::abs(shift);
            auto& sides = s.borders.clicks[k];
            sides[static_cast<size_t>(side)] = std::move(s.borderTrial[0]);
            sides[static_cast<size_t>(2 + side)] = std::move(s.borderTrial[1]);
        }
    }
    for (auto& path : s.borderTrial) path.clear();
}

void clearHitborders(PlayLayer* play) {
    if (auto* container = overlay::markerContainer(play)) container->removeChildByTag(overlay::kBorderTag, true);
}

namespace {

// The players are see-through in the replay of the hitborders.
constexpr GLubyte kPlayerOpacity = 110;

void setPlayersOpacity(PlayLayer* play, GLubyte opacity) {
    for (auto* player : {play->m_player1, play->m_player2}) {
        if (player && player->getOpacity() != opacity) player->setOpacity(opacity);
    }
}

bool bordersShown(Session const& s) {
    return s.borderReplay && s.stage == Stage::Replay && !s.borders.empty();
}

using Vertices = std::shared_ptr<std::vector<ccVertex2F>>;

// Triangles in one colour, drawn as they are.
class Triangles : public CCNode {
    Vertices m_vertices;
    ccColor4F m_color{};

public:
    static Triangles* create(Vertices vertices, ccColor4F color) {
        auto* node = new Triangles();
        node->m_vertices = std::move(vertices);
        node->m_color = color;
        node->init();
        node->autorelease();
        return node;
    }

    void draw() override {
        if (m_vertices->empty()) return;
        auto* shader = CCShaderCache::sharedShaderCache()->programForKey(kCCShader_Position_uColor);
        shader->use();
        shader->setUniformsForBuiltins();
        GLfloat color[4] = {m_color.r, m_color.g, m_color.b, m_color.a};
        shader->setUniformLocationWith4fv(glGetUniformLocation(shader->getProgram(), "u_color"), color, 1);
        ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // The vertices are read from memory.
#if CC_TEXTURE_ATLAS_USE_VAO
        ccGLBindVAO(0);
#endif
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position);
        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, m_vertices->data());
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertices->size()));
    }
};

void addQuad(std::vector<ccVertex2F>& v, CCPoint a, CCPoint b, CCPoint c, CCPoint d) {
    for (auto const& p : {a, b, c, a, c, d}) v.push_back({p.x, p.y});
}

// A line of the border's width, a little longer at both ends so the joints close.
void addLine(std::vector<ccVertex2F>& v, CCPoint from, CCPoint to) {
    auto const d = to - from;
    float const length = d.getLength();
    if (length < 1e-3f) return;
    auto const along = d * (kBorderWidth / 2.f / length);
    CCPoint const across{-along.y, along.x};
    auto const a = from - along;
    auto const b = to + along;
    addQuad(v, a + across, b + across, b - across, a - across);
}

} // namespace

// Draws the hitborders under the markers while their replay runs, or removes them.
void drawHitborders(PlayLayer* play) {
    auto& s = Session::get();
    auto* container = overlay::markerContainer(play);
    if (!container) return;
    container->removeChildByTag(overlay::kBorderTag, true);
    s.trailTick = -1;
    s.trailLast = {};
    if (!bordersShown(s)) return;
    auto const& b = s.borders;
    auto const& events = s.source.events;

    auto shape = std::make_shared<std::vector<ccVertex2F>>();
    auto shrunk = std::make_shared<std::vector<ccVertex2F>>();

    // The run's own path: a line of the border's width, part of the same shape.
    for (auto const* path : {&b.path1, &b.path2}) {
        PathPoint const* last = nullptr;
        for (auto const& p : *path) {
            if (last && std::abs(p.x - last->x) + std::abs(p.y - last->y) < kMinStep) continue;
            if (last && p.tick - last->tick < 40) addLine(*shape, {last->x, last->y}, {p.x, p.y});
            last = &p;
        }
    }

    // Every band from its earliest timing up to the next click that matters.
    auto const& windows = s.windows;
    for (auto const& [index, sides] : b.clicks) {
        int until = std::numeric_limits<int>::max();
        for (size_t j = index + 1; j < events.size() && j < windows.size(); ++j) {
            if (windows[j].valid && !windows[j].exceeded) {
                until = events[j].tick;
                break;
            }
        }
        for (size_t player = 0; player < 2; ++player) {
            auto const& base = player == 0 ? b.path1 : b.path2;
            auto const& earlySide = sides[player * 2];
            auto const& lateSide = sides[player * 2 + 1];
            if (earlySide.empty() && lateSide.empty()) continue;
            auto const& early = earlySide.empty() ? base : earlySide;
            auto const& late = lateSide.empty() ? base : lateSide;
            if (early.empty() || late.empty()) continue;
            // Along a trial's path (the run's own path is long), the other side found by tick.
            bool const alongEarly = !earlySide.empty();
            auto const& walk = alongEarly ? early : late;
            auto const& other = alongEarly ? late : early;
            int const from = std::max(early.front().tick, late.front().tick);
            PathPoint const* lastA = nullptr;
            PathPoint const* lastB = nullptr;
            for (auto const& a : walk) {
                if (a.tick < from) continue;
                if (a.tick > until) break;
                auto const* bPoint = pointAt(other, a.tick);
                if (!bPoint) continue;
                if (lastA && std::abs(a.x - lastA->x) + std::abs(a.y - lastA->y) < kMinStep &&
                    std::abs(bPoint->x - lastB->x) + std::abs(bPoint->y - lastB->y) < kMinStep)
                    continue;
                if (lastA) {
                    CCPoint const a0{lastA->x, lastA->y};
                    CCPoint const a1{a.x, a.y};
                    CCPoint const b1{bPoint->x, bPoint->y};
                    CCPoint const b0{lastB->x, lastB->y};
                    addQuad(*shape, a0, a1, b1, b0);
                    addQuad(*shrunk, inward(a0, b0), inward(a1, b1), inward(b1, a1), inward(b0, a0));
                }
                lastA = &a;
                lastB = bPoint;
            }
        }
    }

    auto* root = CCNode::create();
    root->setTag(overlay::kBorderTag);
    container->addChild(root, -1);
    int z = 0;
    auto state = [&](void (*apply)()) { root->addChild(GLStateNode::create(apply), z++); };
    auto add = [&](Vertices const& vertices, ccColor4F color) { root->addChild(Triangles::create(vertices, color), z++); };

    // Without a stencil buffer the bands are only filled, not merged.
    GLint stencilBits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    if (stencilBits > 0) {
        state(beginUnionPass);
        add(shape, kMask);
        state(beginInsetPass);
        add(shrunk, kMask);
        state(beginBorderPass);
        add(shape, kBandBorder);
        state(beginFillPass);
        add(shape, kBandFill);
        state(endPasses);
    } else {
        add(shape, kBandFill);
    }

    auto* trail = blendedDrawNode();
    trail->setTag(kTrailTag);
    root->addChild(trail, z++);
}

// Every frame: the hitborders come and go with their replay, and the trail grows behind the players.
void updateHitborders(PlayLayer* play) {
    auto& s = Session::get();
    if (s.borderReplay && s.stage == Stage::Replay) {
        s.borderReplayRan = true;
    } else if (s.borderReplayRan) {
        // The replay is over: the counter, markers and players as usual again.
        s.borderReplay = false;
        s.borderReplayRan = false;
        setPlayersOpacity(play, 255);
        refreshDisplay(play);
    }
    auto* container = overlay::markerContainer(play);
    if (!container) return;
    auto* root = container->getChildByTag(overlay::kBorderTag);
    bool const shown = bordersShown(s);
    if (shown) setPlayersOpacity(play, kPlayerOpacity);
    if (shown != (root != nullptr)) {
        drawHitborders(play);
        root = container->getChildByTag(overlay::kBorderTag);
    }
    auto* trail = root ? static_cast<CCDrawNode*>(root->getChildByTag(kTrailTag)) : nullptr;
    if (!trail) return;
    int const tick = currentTick(play);
    if (tick < s.trailTick) {
        trail->clear();
        s.trailLast = {};
    }
    s.trailTick = tick;
    bool const dual = play->m_gameState.m_isDualMode && play->m_player2;
    for (size_t p = 0; p < (dual ? 2u : 1u); ++p) {
        auto* player = p == 0 ? play->m_player1 : play->m_player2;
        auto& last = s.trailLast[p];
        if (!player || player->m_isDead) {
            last.reset();
            continue;
        }
        auto const now = playerPoint(play, p == 0);
        if (last && now.getDistance(*last) < 1.f) continue;
        if (last && now.getDistance(*last) < kTrailMaxStep) trail->drawSegment(*last, now, kTrailRadius, kTrail);
        last = now;
    }
}

namespace {

// Opens the level again once GD has left it, with the Play button of the screen it went back to.
class Reopen : public CCObject {
public:
    Ref<GJGameLevel> level;
    int tries = 0;

    void run(float) {
        auto* director = CCDirector::sharedDirector();
        auto* scene = director->getRunningScene();
        if (!scene || typeinfo_cast<CCTransitionScene*>(scene)) return;
        if (++tries < 3) return;   // the screen settles first
        director->getScheduler()->unscheduleSelector(schedule_selector(Reopen::run), this);
        if (auto* info = scene ? scene->getChildByType<LevelInfoLayer>(0) : nullptr) return info->onPlay(nullptr);
        if (auto* edit = scene ? scene->getChildByType<EditLevelLayer>(0) : nullptr) return edit->onPlay(nullptr);
        director->pushScene(CCTransitionFade::create(0.5f, PlayLayer::scene(level, false, false)));
    }
};

} // namespace

// The replay of the hitborders runs in layout mode.
namespace {

constexpr int kSetupScreenTag = 0x7F39;

// Black while the level is left and opened again in layout mode.
void showSetupScreen(bool visible) {
    auto* overlay = OverlayManager::get();
    overlay->removeChildByTag(kSetupScreenTag, true);
    if (!visible) return;
    auto const size = CCDirector::sharedDirector()->getWinSize();
    auto* screen = CCLayerColor::create({0, 0, 0, 255}, size.width, size.height);
    screen->setTag(kSetupScreenTag);
    if (auto* label = CCLabelBMFont::create("Setting up layout mode", "bigFont.fnt")) {
        label->setScale(0.6f);
        label->setPosition(size / 2.f);
        screen->addChild(label);
    }
    // Never stays if the level does not open again.
    screen->runAction(CCSequence::create(CCDelayTime::create(10.f), CCRemoveSelf::create(), nullptr));
    overlay->addChild(screen);
}

void startBorderReplay(PlayLayer* play) {
    auto& s = Session::get();
    showSetupScreen(false);
    if (!resultsForLevel(play) || s.borders.empty() || s.drivesInput()) return;
    s.borderReplay = true;
    s.borderReplayRan = false;
    refreshDisplay(play);
    if (!play->m_isPaused) play->pauseGame(false);
    beginReplay(play);
}

} // namespace

void playHitborders(PlayLayer* play) {
    auto& s = Session::get();
    if (!play || !play->m_level || s.borders.empty()) return;
    if (s.layoutLoad) {
        startBorderReplay(play);
        return;
    }
    s.pendingBorderReplay = true;
    showSetupScreen(true);
    Ref<GJGameLevel> level = play->m_level;
    Loader::get()->queueInMainThread([level] {
        auto* play = PlayLayer::get();
        if (!play) return;
        auto* reopen = new Reopen();
        reopen->autorelease();
        reopen->level = level;
        // Out through the pause menu, the way the game leaves a level.
        if (!play->m_isPaused) play->pauseGame(false);
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (auto* pause = scene ? scene->getChildByType<PauseLayer>(0) : nullptr) pause->onQuit(nullptr);
        else play->onQuit();
        CCDirector::sharedDirector()->getScheduler()->scheduleSelector(
            schedule_selector(Reopen::run), reopen, 0.1f, kCCRepeatForever, 0.f, false);
    });
}

// Once the level runs: the replay of the hitborders it was opened for.
void startPendingBorderReplay(PlayLayer* play) {
    auto& s = Session::get();
    if (!s.pendingBorderReplay || !play || CCDirector::sharedDirector()->getRunningScene() != play->getParent()) return;
    s.pendingBorderReplay = false;
    startBorderReplay(play);
}

} // namespace tc

using namespace tc;

class $modify(TCLayoutPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        auto& s = Session::get();
        s.layoutLoad = s.pendingBorderReplay;
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void addObject(GameObject* object) {
        if (!Session::get().layoutLoad) return PlayLayer::addObject(object);
        if (layoutSkipsTrigger(object->m_objectID)) {
            object->m_isHide = true;
            return;
        }
        object->m_hasNoGlow = true;
        object->m_activeMainColorID = -1;
        object->m_activeDetailColorID = -1;
        object->m_baseUsesHSV = false;
        object->m_detailUsesHSV = false;
        object->m_isHide = false;
        object->setOpacity(255);
        PlayLayer::addObject(object);
    }
};

class $modify(TCLayoutColors, PlayLayer) {
    void updateColor(ccColor3B& color, float fadeTime, int colorID, bool blending, float opacity, ccHSVValue& copyHSV,
        int colorIDToCopy, bool copyOpacity, EffectGameObject* callerObject, int unk1, int unk2) {
        if (Session::get().layoutLoad) {
            switch (colorID) {
                case 1000: case 1013: case 1014: color = {56, 60, 72}; break;   // background, middleground
                case 1001: case 1009: color = {40, 43, 52}; break;              // ground
                case 1002: color = {255, 255, 255}; break;                      // ground line
                default: break;
            }
        }
        PlayLayer::updateColor(color, fadeTime, colorID, blending, opacity, copyHSV, colorIDToCopy, copyOpacity,
            callerObject, unk1, unk2);
    }
};

class $modify(TCLayoutGameObject, GameObject) {
    void addGlow(gd::string frame) {
        GameObject::addGlow(frame);
        if (!Session::get().layoutLoad || !PlayLayer::get()) return;
        // 44 is the practice checkpoint.
        bool const decoration = m_objectType == GameObjectType::Decoration && m_objectID != 44 && m_objectID != 38 &&
            m_objectID != 747 && m_objectID != 749;
        m_isHide = decoration || m_isNoTouch;
    }
};
