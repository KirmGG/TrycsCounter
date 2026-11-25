#pragma once
// Everything the player sees and hears: click markers, the counter HUD and the ding.
// The look follows NaN GD's frame window counter videos: a black-outlined ring
// at the click with its window next to it, and a counter in the top-left corner
// whose numbers pulse and glow when a click is added.

#include <Geode/Geode.hpp>

#include "ModSettings.hpp"
#include "Session.hpp"
#include "WindowMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace tc::overlay {

inline constexpr int kMarkerTag = 0x7F31;
inline constexpr int kHudTag = 0x7F32;
inline constexpr int kCountingScreenTag = 0x7F33;
inline constexpr int kStatsTag = 0x7F34;
inline constexpr int kPulseTintAction = 0x52;
inline constexpr int kMarkerBodyTag = 0x53;
inline constexpr int kFadeAction = 0x54;
inline constexpr int kNoteTag = 0x7F35;
inline constexpr int kTitleTag = 0x7F36;

// One colour per counter bucket, from the hardest (0-1 / 1) to the widest.
inline constexpr std::array<cocos2d::ccColor3B, kBucketCount> kBucketColors = {{
    {255, 112, 160},   // 0-1   pink
    {255, 84, 72},     // 1-2   red
    {255, 166, 82},    // 2-3   orange
    {255, 236, 112},   // 3-4   yellow
    {132, 236, 120},   // 4-5   green
    {112, 236, 236},   // 5-6   cyan
    {104, 178, 255},   // 6-8   light blue
    {122, 124, 255},   // <10   blue
    {186, 140, 255},   // <=max lavender
}};

inline constexpr float kHudScale = 0.5f;
inline constexpr float kHudLine = 16.5f;

inline cocos2d::ccColor4F toColor4F(cocos2d::ccColor3B c, float alpha = 1.f) {
    return {static_cast<float>(c.r) / 255.f, static_cast<float>(c.g) / 255.f, static_cast<float>(c.b) / 255.f, alpha};
}

inline cocos2d::ccColor3B lighten(cocos2d::ccColor3B c, float amount) {
    auto mix = [amount](GLubyte v) { return static_cast<GLubyte>(v + (255 - v) * amount); };
    return {mix(c.r), mix(c.g), mix(c.b)};
}

// A click is shown (and counted, and dinged) only when its window was measured
// and is not wider than the configured display limit.
inline bool isShown(InputEvent const& event, WindowResult const& result) {
    if (!result.valid || result.exceeded) return false;
    if (!event.down && !cfg::flag("show-releases")) return false;
    return cfg::frames(result) <= static_cast<double>(cfg::maxDisplayFrames()) + 1e-9;
}

// Counter bucket of a window in the scheme the HUD currently uses.
inline size_t bucketOf(double frames) {
    return static_cast<size_t>(tc::bucketIndex(frames, Session::get().hudPrecise));
}

// ---- Sound -----------------------------------------------------------------

inline std::string const& dingPath() {
    static std::string const path = [] {
        auto const* mod = geode::Mod::get();
        auto const dir = mod->getResourcesDir();
        for (auto const& candidate : {dir / "proper-ding.mp3", dir / mod->getID() / "proper-ding.mp3"}) {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) return geode::utils::string::pathToString(candidate);
        }
        geode::log::error("Ding sound proper-ding.mp3 was not found in {}", geode::utils::string::pathToString(dir));
        return std::string{};
    }();
    return path;
}

// The sound of a click: "Hardest click sound" up to its window size, "Hard
// click sound" up to its own, otherwise "Click sound". One that is not set
// falls back to the next wider one, and in the end to the built-in ding.
inline std::string soundFor(double frames) {
    std::vector<char const*> keys;
    if (frames <= cfg::real("sound-hardest-below") + 1e-6) keys.push_back("sound-hardest");
    if (frames <= cfg::real("sound-hard-below") + 1e-6) keys.push_back("sound-hard");
    keys.push_back("sound");
    for (auto const* key : keys) {
        auto const file = geode::Mod::get()->getSettingValue<std::filesystem::path>(key);
        std::error_code ec;
        if (!file.empty() && std::filesystem::is_regular_file(file, ec))
            return geode::utils::string::pathToString(file);
    }
    return dingPath();
}

// The window size decides the pitch: smaller window, lower ding.
inline void playDing(double frames) {
    if (!cfg::flag("ding-enabled")) return;
    auto const path = soundFor(frames);
    if (path.empty()) return;
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;
    float const speed = static_cast<float>(tc::dingPitch(frames, static_cast<double>(cfg::real("ding-semitones-per-frame"))));
    engine->playEffect(path, speed, 0.f, cfg::real("ding-volume"));
}

// ---- Glow texture ------------------------------------------------------------

// Soft white blob (radial fall-off) used for the glow of a pulsing counter.
inline cocos2d::CCTexture2D* glowTexture() {
    static geode::Ref<cocos2d::CCTexture2D> texture;
    if (!texture) {
        constexpr int kSize = 64;
        std::vector<unsigned char> pixels(static_cast<size_t>(kSize * kSize * 4));
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                float const dx = (static_cast<float>(x) + 0.5f) / (kSize / 2.f) - 1.f;
                float const dy = (static_cast<float>(y) + 0.5f) / (kSize / 2.f) - 1.f;
                float a = std::clamp(1.f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
                // A wide soft halo with a brighter middle, not a dot.
                a = 0.55f * std::pow(a, 1.6f) + 0.45f * a * a * a * a;
                auto* px = &pixels[static_cast<size_t>((y * kSize + x) * 4)];
                px[0] = px[1] = px[2] = 255;
                px[3] = static_cast<unsigned char>(a * 255.f);
            }
        }
        auto* created = new cocos2d::CCTexture2D();
        if (created->initWithData(pixels.data(), cocos2d::kCCTexture2DPixelFormat_RGBA8888, kSize, kSize,
                cocos2d::CCSize{static_cast<float>(kSize), static_cast<float>(kSize)})) {
            created->setAntiAliasTexParameters();
            texture = created;
        }
        created->release();
    }
    return texture.data();
}

// ---- Click markers -----------------------------------------------------------

// The markers live in the UI layer, above every object, effect and shader of the
// level (in the object layer, decoration of some levels covered them), and
// follow the object layer through syncMarkers: their positions stay level
// coordinates.
inline cocos2d::CCNode* markerContainer(PlayLayer* play) {
    if (!play || !play->m_objectLayer || !play->m_uiLayer) return nullptr;
    auto* node = play->m_uiLayer->getChildByTag(kMarkerTag);
    if (!node) {
        node = cocos2d::CCNode::create();
        node->setTag(kMarkerTag);
        node->setID("markers"_spr);
        play->m_uiLayer->addChild(node, 99999);
    }
    return node;
}

// Gives the marker container the object layer's transform (camera move, zoom,
// rotation, mirror) as seen from the UI layer. Called every frame.
inline void syncMarkers(PlayLayer* play) {
    auto* container = markerContainer(play);
    if (!container) return;
    container->setAdditionalTransform(cocos2d::CCAffineTransformConcat(
        play->m_objectLayer->nodeToWorldTransform(), play->m_uiLayer->worldToNodeTransform()));
}

inline void clearMarkers(PlayLayer* play) {
    Session::get().markerBoxes.clear();
    if (auto* container = markerContainer(play)) container->removeAllChildrenWithCleanup(true);
    if (play && play->m_uiLayer) play->m_uiLayer->removeChildByTag(kNoteTag, true);
}

inline float overlapArea(cocos2d::CCRect const& a, cocos2d::CCRect const& b) {
    float const w = std::min(a.getMaxX(), b.getMaxX()) - std::max(a.getMinX(), b.getMinX());
    float const h = std::min(a.getMaxY(), b.getMaxY()) - std::max(a.getMinY(), b.getMinY());
    return w > 0.f && h > 0.f ? w * h : 0.f;
}

// Where the text of a marker goes: to the left of the ring like NaN GD, unless
// that covers the ring or text of a nearby marker. Then anywhere around the
// ring, turning from the left both ways in 15 degree steps, and if all of that
// is taken, the same a little further out. Failing that, the least covered place.
inline cocos2d::CCPoint placeText(cocos2d::CCPoint center, float radius, cocos2d::CCSize text) {
    auto& boxes = Session::get().markerBoxes;
    float const w = text.width;
    float const h = text.height;
    // Only markers placed recently can be close by (clicks go from left to right).
    size_t const first = boxes.size() > 160 ? boxes.size() - 160 : 0;
    cocos2d::CCPoint best{-radius - 3.f - w, -h / 2.f};
    float bestArea = -1.f;
    constexpr int kSteps = 12;   // 15 degrees each, left to right on either side
    constexpr float kStep = 3.14159265f / kSteps;
    for (float gap : {3.f, 9.f, 16.f}) {
        for (int step = 0; step <= kSteps; ++step) {
            for (int side : {1, -1}) {
                if (side < 0 && (step == 0 || step == kSteps)) continue;
                float const angle = 3.14159265f + static_cast<float>(side * step) * kStep;
                float const dx = std::cos(angle);
                float const dy = std::sin(angle);
                // The nearest corner or edge of the text just touches the gap around the ring.
                float const reach = radius + gap + std::abs(dx) * w / 2.f + std::abs(dy) * h / 2.f;
                cocos2d::CCPoint const offset{dx * reach - w / 2.f, dy * reach - h / 2.f};
                // A little inset: the font boxes are taller than the digits.
                cocos2d::CCRect const box{center.x + offset.x + 1.f, center.y + offset.y + 1.5f, w - 2.f, h - 3.f};
                float area = 0.f;
                for (size_t i = first; i < boxes.size(); ++i) area += overlapArea(box, boxes[i]);
                if (area <= 0.f) return offset;
                if (bestArea < 0.f || area < bestArea - 0.01f) {
                    bestArea = area;
                    best = offset;
                }
            }
        }
    }
    return best;
}

// The marker of one click: a black-outlined ring in the colour of its bucket
// (`frames`), the window `value` (with its marks) beside it and optionally the
// time in ms below.
inline void showMarkerText(cocos2d::CCNode* container, InputEvent const& event, double frames, bool capped,
    std::string const& value) {
    if (!container) return;

    auto const color = kBucketColors[bucketOf(frames)];
    float const scale = std::clamp(cfg::real("marker-scale"), 0.3f, 3.f);
    float const radius = (event.down ? 10.5f : 7.5f) * scale;

    auto* node = cocos2d::CCNode::create();
    node->setPosition({event.x, event.y});
    auto* body = cocos2d::CCNode::create();
    body->setTag(kMarkerBodyTag);
    node->addChild(body);

    auto* ring = cocos2d::CCDrawNode::create();
    float const outline = (event.down ? 4.f : 3.2f) * scale;
    float const line = (event.down ? 2.1f : 1.6f) * scale;
    ring->drawCircle({0.f, 0.f}, radius, {0.f, 0.f, 0.f, 0.f}, outline, {0.f, 0.f, 0.f, 0.85f}, 48);
    ring->drawCircle({0.f, 0.f}, radius, {0.f, 0.f, 0.f, 0.f}, line, toColor4F(color), 48);
    body->addChild(ring);

    // Text block: window and marks, with the time below it when enabled.
    auto* text = cocos2d::CCNode::create();
    auto* label = cocos2d::CCLabelBMFont::create(value.c_str(), "bigFont.fnt");
    float width = 0.f;
    float height = 0.f;
    if (label) {
        label->setScale((event.down ? 0.5f : 0.4f) * scale);
        label->setColor(color);
        label->setAnchorPoint({0.f, 0.f});
        auto const size = label->getScaledContentSize();
        width = size.width;
        height = size.height;
        text->addChild(label);
    }
    if (cfg::flag("show-ms")) {
        auto const ms = tc::formatMs(tc::windowMs(frames, cfg::windowFps()), capped);
        if (auto* msLabel = cocos2d::CCLabelBMFont::create(ms.c_str(), "bigFont.fnt")) {
            msLabel->setScale(0.26f * scale);
            msLabel->setColor({235, 235, 235});
            msLabel->setAnchorPoint({0.f, 0.f});
            auto const size = msLabel->getScaledContentSize();
            // Below the window, the block grows downwards.
            if (label) label->setPositionY(size.height - 1.f);
            height += size.height - 1.f;
            width = std::max(width, size.width);
            text->addChild(msLabel);
        }
    }
    text->setContentSize({width, height});
    auto const offset = placeText({event.x, event.y}, radius, {width, height});
    text->setPosition(offset);
    body->addChild(text);

    auto& boxes = Session::get().markerBoxes;
    boxes.push_back({event.x - radius - outline / 2.f, event.y - radius - outline / 2.f, 2.f * radius + outline,
        2.f * radius + outline});
    boxes.push_back({event.x + offset.x + 1.f, event.y + offset.y + 1.5f, width - 2.f, height - 3.f});

    container->addChild(node);
}

inline void showMarkerText(PlayLayer* play, InputEvent const& event, double frames, bool capped,
    std::string const& value) {
    showMarkerText(markerContainer(play), event, frames, capped, value);
}

inline void showMarker(PlayLayer* play, InputEvent const& event, WindowResult const& result, std::string const& marks) {
    double const frames = cfg::frames(result);
    auto const value = cfg::windowLabel(frames, cfg::isPrecise(result), result.capped()) + marks;
    showMarkerText(play, event, frames, result.capped(), value);
}

// ---- Notes (Deep Count) ----------------------------------------------------------

// A line of a note: plain text in white, "{2.20}" as "(2.20)" in the colour of
// its bucket, like NaN GD's explanations at the top right.
inline cocos2d::CCNode* noteLine(std::string const& line, float scale) {
    auto* row = cocos2d::CCNode::create();
    float x = 0.f;
    float height = 0.f;
    auto add = [&](std::string const& text, cocos2d::ccColor3B color) {
        if (text.empty()) return;
        auto* label = cocos2d::CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        if (!label) return;
        label->setScale(scale);
        label->setColor(color);
        label->setAnchorPoint({0.f, 0.f});
        label->setPosition({x, 0.f});
        row->addChild(label);
        auto const size = label->getScaledContentSize();
        x += size.width;
        height = std::max(height, size.height);
    };
    size_t pos = 0;
    while (pos < line.size()) {
        size_t const open = line.find('{', pos);
        size_t const close = open == std::string::npos ? std::string::npos : line.find('}', open);
        if (close == std::string::npos) {
            add(line.substr(pos), {240, 240, 240});
            break;
        }
        add(line.substr(pos, open - pos), {240, 240, 240});
        auto const value = line.substr(open + 1, close - open - 1);
        double const frames = geode::utils::numFromString<double>(value).unwrapOr(0.0);
        add("(" + value + ")", kBucketColors[bucketOf(frames)]);
        pos = close + 1;
    }
    row->setContentSize({x, height});
    return row;
}

// Shows the note of a click at the top right. It starts fading at once and is
// gone after "Note fade"; a new note replaces it.
inline void showNote(PlayLayer* play, std::vector<std::string> const& lines, bool precise) {
    (void)precise;
    if (!play || !play->m_uiLayer || lines.empty()) return;
    play->m_uiLayer->removeChildByTag(kNoteTag, true);
    auto const size = cocos2d::CCDirector::sharedDirector()->getWinSize();
    auto* note = cocos2d::CCNode::create();
    note->setTag(kNoteTag);
    note->setID("note"_spr);

    float const maxWidth = size.width * 0.66f;
    float y = size.height - 24.f;
    float const fade = std::max(0.5f, cfg::real("note-seconds"));
    for (auto const& text : lines) {
        auto* row = noteLine(text, 0.42f);
        float const width = row->getContentSize().width;
        float const fit = width > maxWidth ? maxWidth / width : 1.f;
        row->setScale(fit);
        float const x = cfg::hudRight() ? 8.f : size.width - 8.f - width * fit;
        row->setPosition({x, y - row->getContentSize().height * fit});
        y -= row->getContentSize().height * fit + 1.f;
        note->addChild(row);
        for (auto* label : geode::cocos::CCArrayExt<cocos2d::CCLabelBMFont*>(row->getChildren()))
            label->runAction(cocos2d::CCFadeOut::create(fade));
    }
    note->runAction(cocos2d::CCSequence::create(cocos2d::CCDelayTime::create(fade + 0.05f),
        cocos2d::CCRemoveSelf::create(), nullptr));
    play->m_uiLayer->addChild(note, 100001);
}

// Mirror portals (and other camera flips) mirror the object layer, which would
// mirror the text of the markers too: flip the markers back so they stay readable.
inline void orientMarkers(PlayLayer* play) {
    syncMarkers(play);
    auto* container = markerContainer(play);
    if (!container || !container->getChildrenCount()) return;
    auto const t = container->nodeToWorldTransform();
    float sx = 1.f;
    float sy = 1.f;
    if (t.a * t.d - t.b * t.c < 0.f) {
        if (t.a < 0.f) sx = -1.f;
        else sy = -1.f;
    }
    for (auto* node : geode::cocos::CCArrayExt<cocos2d::CCNode*>(container->getChildren())) {
        if (node->getScaleX() != sx) node->setScaleX(sx);
        if (node->getScaleY() != sy) node->setScaleY(sy);
    }
}

// ---- Counter HUD ---------------------------------------------------------------

inline void updateCounterLabels() {
    auto& s = Session::get();
    for (size_t i = 0; i < s.countLabels.size(); ++i) {
        if (!s.countLabels[i]) continue;
        auto const value = std::to_string(s.visibleCounts[i]);
        if (std::string(s.countLabels[i]->getString()) != value) s.countLabels[i]->setString(value.c_str());
    }
}

inline void setLabelText(cocos2d::CCLabelBMFont* label, std::string const& text) {
    if (label && std::string(label->getString()) != text) label->setString(text.c_str());
}

// The window of the last click (ms and Hz), L* so far and how much of the run's
// total L* that is (the level progress while no L* is known).
inline void updateHudStats(PlayLayer* play) {
    auto& s = Session::get();
    if (s.msLabel) {
        bool const any = s.lastShownMs > 0.0;
        setLabelText(s.msLabel, any ? tc::hudMs(s.lastShownMs) : "-MS");
        setLabelText(s.hzLabel, any ? tc::hudHz(s.lastShownMs) : "-Hz");
    }
    if (s.progressLabel && play) {
        double const percent = s.lstarReady && s.lstarTotal > 0.0 ? 100.0 * s.lastShownLStar / s.lstarTotal
                                                                  : play->getCurrentPercent();
        setLabelText(s.progressLabel, fmt::format("{:.2f}%", percent));
    }
    if (s.lstarLabel) setLabelText(s.lstarLabel, s.lstarReady ? fmt::format("{:.2f}", s.lastShownLStar) : "");
}

// The ms and Hz of the click just reached appear and fade out in about 0.75 s.
inline void flashLastClick() {
    auto& s = Session::get();
    for (auto* label : {s.msLabel.data(), s.hzLabel.data()}) {
        if (!label) continue;
        label->stopActionByTag(kFadeAction);
        label->setOpacity(255);
        auto* fade = cocos2d::CCSequence::create(cocos2d::CCDelayTime::create(0.05f),
            cocos2d::CCEaseSineIn::create(cocos2d::CCFadeOut::create(0.75f)), nullptr);
        fade->setTag(kFadeAction);
        label->runAction(fade);
    }
}

// A white line of `width` points around the digits of `label`, transparent
// inside: white copies of the text all around it, then the text itself cut
// out. The game fonts have a dark border in the glyphs, so the copies are drawn
// from the glyph shapes only (alpha), not their colours.
inline cocos2d::CCSprite* textOutline(cocos2d::CCLabelBMFont* label, float width) {
    auto* copy = cocos2d::CCLabelBMFont::create(label->getString(), label->getFntFile());
    if (!copy) return nullptr;
    copy->setScale(label->getScale());
    copy->setAnchorPoint({0.5f, 0.5f});
    copy->setColor({255, 255, 255});
    copy->setShaderProgram(cocos2d::CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTextureA8Color));

    auto const size = copy->getScaledContentSize();
    float const pad = width + 2.f;
    int const canvasWidth = static_cast<int>(std::ceil(size.width + 2.f * pad));
    int const canvasHeight = static_cast<int>(std::ceil(size.height + 2.f * pad));
    auto* canvas = cocos2d::CCRenderTexture::create(canvasWidth, canvasHeight);
    if (!canvas) return nullptr;
    // The middle of the canvas, so the outline sits centred on the number.
    cocos2d::CCPoint const middle{canvasWidth / 2.f, canvasHeight / 2.f};
    canvas->begin();
    copy->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    constexpr int kCopies = 16;
    for (int i = 0; i < kCopies; ++i) {
        float const angle = 6.2831853f * static_cast<float>(i) / kCopies;
        copy->setPosition({middle.x + std::cos(angle) * width, middle.y + std::sin(angle) * width});
        copy->visit();
    }
    copy->setPosition(middle);
    copy->setBlendFunc({GL_ZERO, GL_ONE_MINUS_SRC_ALPHA});
    copy->visit();
    canvas->end();

    auto* sprite = cocos2d::CCSprite::createWithTexture(canvas->getSprite()->getTexture());
    if (sprite) sprite->setFlipY(true);
    return sprite;
}

// A click was added to a bucket: its number keeps its size and flashes white,
// a white outline of it grows from its centre and fades, and a halo glows behind.
inline void pulseBucket(size_t index) {
    auto& s = Session::get();
    if (index >= s.countLabels.size() || !s.countLabels[index]) return;
    auto* label = s.countLabels[index].data();
    auto const color = kBucketColors[index];
    // The new count, so the outline below copies it rather than the old one.
    setLabelText(label, std::to_string(s.visibleCounts[index]));

    label->stopActionByTag(kPulseTintAction);
    label->setColor(color);
    auto* tint = cocos2d::CCSequence::create(
        cocos2d::CCTintTo::create(0.05f, 255, 255, 255),
        cocos2d::CCTintTo::create(0.3f, color.r, color.g, color.b), nullptr);
    tint->setTag(kPulseTintAction);
    label->runAction(tint);

    auto* parent = label->getParent();
    if (!parent) return;
    auto const size = label->getScaledContentSize();
    auto const anchor = label->getAnchorPoint();
    cocos2d::CCPoint const center{label->getPositionX() + (0.5f - anchor.x) * size.width,
        label->getPositionY() + (0.5f - anchor.y) * size.height};

    // The outline: a copy of the number drawn as a white line around its digits,
    // empty inside, growing from the number and fading out.
    if (auto* outline = textOutline(label, 1.5f)) {
        outline->setPosition(center);
        outline->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        parent->addChild(outline, label->getZOrder() - 1);
        outline->runAction(cocos2d::CCSequence::create(
            cocos2d::CCSpawn::create(
                cocos2d::CCEaseSineOut::create(cocos2d::CCScaleTo::create(0.53f, 1.3f)),
                cocos2d::CCFadeOut::create(0.53f), nullptr),
            cocos2d::CCRemoveSelf::create(), nullptr));
    }

    auto* texture = glowTexture();
    if (!texture) return;
    auto* glow = cocos2d::CCSprite::createWithTexture(texture);
    if (!glow) return;
    // A halo well around the number (NaN GD), not a dot behind it.
    float const w = (std::max(size.width, 12.f) * 2.f + 46.f) * 2.f / 1.25f;
    float const h = (size.height * 1.8f + 30.f) * 2.f / 1.25f;
    glow->setPosition(center);
    glow->setScaleX(w / 64.f);
    glow->setScaleY(h / 64.f);
    glow->setColor(lighten(color, 0.35f));
    glow->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
    glow->setOpacity(255);
    parent->addChild(glow, label->getZOrder() - 2);
    glow->runAction(cocos2d::CCSequence::create(
        cocos2d::CCDelayTime::create(0.08f),
        cocos2d::CCSpawn::create(
            cocos2d::CCEaseSineIn::create(cocos2d::CCFadeOut::create(0.47f)),
            cocos2d::CCScaleTo::create(0.47f, w * 1.25f / 64.f, h * 1.25f / 64.f), nullptr),
        cocos2d::CCRemoveSelf::create(), nullptr));
}

// Black screen with a notice while the frame windows are counted. The level
// itself is hidden, so the game draws nothing of it and the time goes to counting.
// The stage line, a progress bar (the level's own bar sprites), the time left
// and the trials done.
inline void setCountingScreen(PlayLayer* play, bool visible, std::string const& status = {},
                              double fraction = 0.0, std::string const& timeLeft = {},
                              std::string const& trials = {}) {
    if (!play) return;
    auto* scene = play->getParent();
    if (!scene) return;
    auto* screen = scene->getChildByTag(kCountingScreenTag);
    if (!visible) {
        if (screen && screen->isVisible()) {
            screen->setVisible(false);
            play->setVisible(true);
        }
        return;
    }
    auto const size = cocos2d::CCDirector::sharedDirector()->getWinSize();
    if (!screen) {
        auto* layer = cocos2d::CCLayerColor::create({0, 0, 0, 255}, size.width, size.height);
        if (!layer) return;
        layer->setTag(kCountingScreenTag);
        layer->setID("counting-screen"_spr);
        if (auto* title = cocos2d::CCLabelBMFont::create("COUNTING FRAME WINDOWS", "bigFont.fnt")) {
            title->setScale(std::min(0.9f, size.width * 0.85f / std::max(1.f, title->getContentSize().width)));
            title->setPosition({size.width / 2.f, size.height / 2.f + 44.f});
            title->setTag(1);
            layer->addChild(title);
        }
        if (auto* line = cocos2d::CCLabelBMFont::create("", "goldFont.fnt")) {
            line->setScale(0.7f);
            line->setPosition({size.width / 2.f, size.height / 2.f + 8.f});
            line->setTag(2);
            layer->addChild(line);
        }
        if (auto* groove = cocos2d::CCSprite::create("slidergroove2.png")) {
            groove->setScale(1.4f);
            groove->setPosition({size.width / 2.f, size.height / 2.f - 18.f});
            groove->setTag(3);
            if (auto* fill = cocos2d::CCSprite::create("sliderBar2.png")) {
                // Repeated along the bar, as in the level's progress bar.
                cocos2d::ccTexParams params{GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE};
                fill->getTexture()->setTexParameters(&params);
                fill->setAnchorPoint({0.f, 0.5f});
                fill->setPosition({2.f, groove->getContentSize().height / 2.f});
                fill->setTextureRect({0.f, 0.f, 0.f, 8.f});
                fill->setTag(1);
                groove->addChild(fill, -1);
            }
            layer->addChild(groove);
        }
        if (auto* eta = cocos2d::CCLabelBMFont::create("", "goldFont.fnt")) {
            eta->setScale(0.5f);
            eta->setPosition({size.width / 2.f, size.height / 2.f - 42.f});
            eta->setTag(4);
            layer->addChild(eta);
        }
        if (auto* trials = cocos2d::CCLabelBMFont::create("", "chatFont.fnt")) {
            trials->setScale(0.7f);
            trials->setColor({150, 150, 150});
            trials->setPosition({size.width / 2.f, size.height / 2.f - 60.f});
            trials->setTag(5);
            layer->addChild(trials);
        }
        if (auto* hint = cocos2d::CCLabelBMFont::create("Pause > TC > Stop to stop", "chatFont.fnt")) {
            hint->setScale(0.7f);
            hint->setOpacity(160);
            hint->setPosition({size.width / 2.f, size.height / 2.f - 84.f});
            layer->addChild(hint);
        }
        // Above the level, below the pause menu.
        scene->addChild(layer, 9);
        screen = layer;
    }
    if (!screen->isVisible()) screen->setVisible(true);
    if (play->isVisible()) play->setVisible(false);
    auto setText = [screen](int tag, std::string const& text) {
        if (auto* label = static_cast<cocos2d::CCLabelBMFont*>(screen->getChildByTag(tag))) {
            if (std::string(label->getString()) != text) label->setString(text.c_str());
        }
    };
    setText(2, status);
    setText(4, timeLeft);
    setText(5, trials);
    if (auto* groove = static_cast<cocos2d::CCSprite*>(screen->getChildByTag(3))) {
        if (auto* fill = static_cast<cocos2d::CCSprite*>(groove->getChildByTag(1))) {
            float const width = (groove->getContentSize().width - 4.f) * static_cast<float>(std::clamp(fraction, 0.0, 1.0));
            if (fill->getTextureRect().size.width != width) fill->setTextureRect({0.f, 0.f, width, 8.f});
        }
    }
}

// "TrycsCounter" under the level percentage at the top, half transparent, like
// NaN GD's "NaN Counter". Shown with the counter; follows the percentage label
// (it moves with the progress bar settings), or the top middle without one.
inline void placeCounterTitle(PlayLayer* play) {
    if (!play || !play->m_uiLayer) return;
    auto* title = static_cast<cocos2d::CCLabelBMFont*>(play->m_uiLayer->getChildByTag(kTitleTag));
    auto* hud = play->m_uiLayer->getChildByTag(kHudTag);
    bool const visible = hud && hud->isVisible() && cfg::flag("show-title");
    if (!visible) {
        if (title && title->isVisible()) title->setVisible(false);
        return;
    }
    if (!title) {
        title = cocos2d::CCLabelBMFont::create("TrycsCounter", "bigFont.fnt");
        if (!title) return;
        title->setTag(kTitleTag);
        title->setID("counter-title"_spr);
        title->setScale(0.3f);
        title->setOpacity(110);
        title->setColor({215, 215, 215});
        title->setAnchorPoint({0.5f, 1.f});
        play->m_uiLayer->addChild(title, 100000);
    }
    auto const size = cocos2d::CCDirector::sharedDirector()->getWinSize();
    cocos2d::CCPoint at{size.width / 2.f, size.height - 18.f};
    auto* percent = play->m_percentageLabel;
    if (percent && percent->isVisible() && percent->getParent()) {
        auto const box = percent->boundingBox();
        auto const center = percent->getParent()->convertToWorldSpace({box.getMidX(), box.getMinY()});
        at = play->m_uiLayer->convertToNodeSpace(center);
        at.y -= 1.f;
    }
    if (!title->getPosition().equals(at)) title->setPosition(at);
    if (!title->isVisible()) title->setVisible(true);
}

inline void setHudVisible(PlayLayer* play, bool visible) {
    if (!play || !play->m_uiLayer) return;
    for (int tag : {kHudTag, kStatsTag}) {
        if (auto* hud = play->m_uiLayer->getChildByTag(tag)) {
            if (hud->isVisible() != visible) hud->setVisible(visible);
        }
    }
}

// Bucket name with its colon. "<=12" is written "<12" and gets a bar under
// "<" (addLessEqualBar): the game fonts have no "≤" glyph.
inline bool isLessEqual(std::string const& name) {
    return name.rfind("<=", 0) == 0;
}

inline cocos2d::CCLabelBMFont* bucketTextLabel(std::string const& name, cocos2d::ccColor3B color) {
    std::string const text = (isLessEqual(name) ? "<" + name.substr(2) : name) + ":";
    auto* label = cocos2d::CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    if (label) label->setColor(color);
    return label;
}

// The bar of "≤": a sibling of the label (a font label only takes glyph
// sprites as children), right under its first glyph.
inline void addLessEqualBar(cocos2d::CCNode* parent, cocos2d::CCLabelBMFont* label, cocos2d::ccColor3B color) {
    if (!parent || !label || label->getChildrenCount() == 0) return;
    auto* glyph = geode::cast::typeinfo_cast<cocos2d::CCSprite*>(label->getChildren()->objectAtIndex(0));
    if (!glyph) return;
    auto const box = glyph->boundingBox();
    auto const t = label->nodeToParentTransform();
    auto const from = cocos2d::CCPointApplyAffineTransform({box.getMinX() + box.size.width * 0.12f, box.getMinY() + box.size.height * 0.14f}, t);
    auto const to = cocos2d::CCPointApplyAffineTransform({box.getMaxX() - box.size.width * 0.12f, box.getMinY() + box.size.height * 0.14f}, t);
    float const thickness = std::max(1.f, box.size.height * label->getScaleY() * 0.09f);
    auto* bar = cocos2d::CCLayerColor::create({color.r, color.g, color.b, 255}, std::max(1.f, to.x - from.x), thickness);
    if (!bar) return;
    bar->setPosition({from.x, from.y - thickness / 2.f});
    parent->addChild(bar, 2);
}

inline void createCounterHud(PlayLayer* play) {
    if (!play || !play->m_uiLayer) return;
    auto& s = Session::get();
    play->m_uiLayer->removeChildByTag(kHudTag, true);
    play->m_uiLayer->removeChildByTag(kStatsTag, true);
    s.resetHudPointers();

    auto const size = cocos2d::CCDirector::sharedDirector()->getWinSize();
    auto* panel = cocos2d::CCNode::create();
    panel->setTag(kHudTag);
    panel->setID("counter"_spr);
    play->m_uiLayer->addChild(panel, 100000);

    // Rows from the widest bucket at the top to the hardest one. On the left:
    // "name: count" from the edge; on the right the counts are right-aligned
    // at the edge and the names right-aligned before a column for them.
    auto const labels = tc::bucketLabels(cfg::maxDisplayFrames(), s.hudPrecise);
    bool const right = cfg::hudRight();
    float const edge = right ? size.width - 6.f : 6.f;
    float const ax = right ? 1.f : 0.f;
    float const top = size.height - 5.f;
    float textWidth = 0.f;
    float countColumn = 0.f;
    if (auto* probe = cocos2d::CCLabelBMFont::create("000", "bigFont.fnt")) {
        probe->setScale(kHudScale);
        countColumn = probe->getScaledContentSize().width;
    }
    std::array<cocos2d::CCLabelBMFont*, kBucketCount> texts{};
    for (size_t i = 0; i < kBucketCount; ++i) {
        auto* label = bucketTextLabel(labels[i], kBucketColors[i]);
        if (!label) continue;
        label->setScale(kHudScale);
        label->setAnchorPoint({ax, 1.f});
        textWidth = std::max(textWidth, label->getScaledContentSize().width);
        texts[i] = label;
    }
    float const textX = right ? edge - countColumn - 5.f : edge;
    float const countX = right ? edge : edge + textWidth + 5.f;
    for (size_t i = 0; i < kBucketCount; ++i) {
        float const y = top - static_cast<float>(kBucketCount - 1 - i) * kHudLine;
        if (auto* label = texts[i]) {
            label->setPosition({textX, y});
            panel->addChild(label, 2);
            if (isLessEqual(labels[i])) addLessEqualBar(panel, label, kBucketColors[i]);
            s.bucketTextLabels[i] = label;
        }
        if (auto* count = cocos2d::CCLabelBMFont::create("0", "bigFont.fnt")) {
            count->setScale(kHudScale);
            count->setAnchorPoint({ax, 1.f});
            count->setColor(kBucketColors[i]);
            count->setPosition({countX, y});
            panel->addChild(count, 2);
            s.countLabels[i] = count;
        }
    }

    // Window of the last click, in ms and as a rate.
    float y = top - static_cast<float>(kBucketCount) * kHudLine;
    for (auto* target : {&s.msLabel, &s.hzLabel}) {
        if (auto* label = cocos2d::CCLabelBMFont::create("", "bigFont.fnt")) {
            label->setScale(kHudScale);
            label->setAnchorPoint({ax, 1.f});
            label->setPosition({edge, y});
            label->setOpacity(0);   // shown by flashLastClick
            panel->addChild(label, 2);
            *target = label;
        }
        y -= kHudLine;
    }

    // Progress and L* in the bottom corner on the counter's side.
    auto* stats = cocos2d::CCNode::create();
    stats->setTag(kStatsTag);
    stats->setID("stats"_spr);
    play->m_uiLayer->addChild(stats, 100000);
    if (cfg::flag("show-lstar")) {
        float bottom = 5.f;
        for (auto* target : {&s.lstarLabel, &s.progressLabel}) {
            if (auto* label = cocos2d::CCLabelBMFont::create("", "bigFont.fnt")) {
                label->setScale(kHudScale);
                label->setAnchorPoint({ax, 0.f});
                label->setPosition({edge, bottom});
                stats->addChild(label);
                *target = label;
            }
            bottom += kHudLine + 2.f;
        }
    }

    bool const visible = s.stage == Stage::Replay || cfg::flag("live-counter");
    panel->setVisible(visible);
    stats->setVisible(visible);
    updateCounterLabels();
    updateHudStats(play);
}

} // namespace tc::overlay
