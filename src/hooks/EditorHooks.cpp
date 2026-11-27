// The editor: "Show frame windows" in its pause menu puts the markers of the level's counted run over the
// objects.

#include "../App.hpp"
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace tc {

namespace {

constexpr char const* kEditorMarkersKey = "editor-frame-windows";

bool editorMarkersOn() {
    return Mod::get()->getSavedValue<bool>(kEditorMarkersKey, false);
}

// Newest of the last count and the saved runs of a level.
std::optional<saved::SavedRunInfo> newestRun(std::string const& key) {
    auto runs = saved::list(key);
    auto last = saved::lastCount(key);
    if (last && (runs.empty() || last->created > runs.front().created)) return last;
    if (!runs.empty()) return runs.front();
    return std::nullopt;
}

// The run to show for an editor level.
std::optional<saved::SavedRunInfo> editorRun(GJGameLevel* level) {
    if (!level) return std::nullopt;
    if (auto run = newestRun(saved::levelKeyOf(level))) return run;
    std::string const name = level->m_levelName;
    std::optional<saved::SavedRunInfo> best;
    for (auto const& key : saved::levelKeys()) {
        if (!key.starts_with("local-")) continue;
        auto run = newestRun(key);
        if (run && run->levelName == name && (!best || run->created > best->created)) best = run;
    }
    return best;
}

CCNode* editorMarkerContainer(LevelEditorLayer* editor, bool create) {
    if (!editor || !editor->m_objectLayer) return nullptr;
    auto* node = editor->m_objectLayer->getChildByTag(overlay::kMarkerTag);
    if (!node && create) {
        node = CCNode::create();
        node->setTag(overlay::kMarkerTag);
        node->setID("editor-markers"_spr);
        editor->m_objectLayer->addChild(node, 99999);
    }
    return node;
}

// Removes the markers and, when the setting is on, draws them again.
void refreshEditorMarkers(LevelEditorLayer* editor, bool announce) {
    if (auto* old = editorMarkerContainer(editor, false)) old->removeFromParentAndCleanup(true);
    if (!editor || !editorMarkersOn()) return;

    auto const info = editorRun(editor->m_level);
    auto loaded = info ? saved::load(info->path) : geode::Err(std::string("no run"));
    if (loaded.isErr()) {
        if (announce) Notification::create("No counted run for this level", NotificationIcon::Info)->show();
        return;
    }
    auto const data = std::move(loaded).unwrap();
    auto const swift = swiftLevelsOf(data.run.events);
    RunView const run(data, swift);

    auto& s = Session::get();
    s.hudPrecise = data.precise;
    s.markerBoxes.clear();
    auto* container = editorMarkerContainer(editor, true);
    for (size_t i = 0; i < run.events.size() && i < run.windows.size(); ++i) {
        auto const& event = run.events[i];
        auto const& result = run.windows[i];
        if (!overlay::isShown(event, result)) continue;
        double const frames = shownWindow(run, i);
        bool const ranged = i < run.deep.size() && run.deep[i].ranged();
        bool const capped = result.capped() && !ranged;
        overlay::showMarkerText(container, markerEvent(run, i), frames, capped,
            cfg::windowLabel(frames, cfg::isPrecise(result), capped) + resultMarks(run, i));
    }
    s.markerBoxes.clear();
    if (announce) Notification::create(fmt::format("Showing {}", saved::titleOf(*info)), NotificationIcon::Success)->show();
}

} // namespace
} // namespace tc

using namespace tc;

class $modify(TCLevelEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        refreshEditorMarkers(this, false);
        return true;
    }
};

class $modify(TCEditorPauseLayer, EditorPauseLayer) {
    bool init(LevelEditorLayer* editor) {
        if (!EditorPauseLayer::init(editor)) return false;
        this->addFrameWindowsOption();
        return true;
    }

    // The last row of GD's column of options.
    void addFrameWindowsOption() {
        auto* menu = this->getChildByIDRecursive("options-menu");
        if (!menu) return;
        std::vector<CCMenuItemToggler*> toggles;
        std::vector<CCLabelBMFont*> labels;
        for (auto* child : CCArrayExt<CCNode*>(menu->getChildren())) {
            if (auto* toggle = typeinfo_cast<CCMenuItemToggler*>(child)) toggles.push_back(toggle);
            else if (auto* label = typeinfo_cast<CCLabelBMFont*>(child)) labels.push_back(label);
        }
        if (toggles.size() < 2 || labels.empty()) return;
        std::sort(toggles.begin(), toggles.end(), [](auto* a, auto* b) { return a->getPositionY() > b->getPositionY(); });

        float const top = toggles.front()->getPositionY();
        float const bottom = toggles.back()->getPositionY();
        float const step = (top - bottom) / static_cast<float>(toggles.size());
        for (size_t i = 0; i < toggles.size(); ++i) {
            float const oldY = toggles[i]->getPositionY();
            float const newY = top - step * static_cast<float>(i);
            for (auto* label : labels) {
                if (std::abs(label->getPositionY() - oldY) < 2.f) label->setPositionY(newY);
            }
            toggles[i]->setPositionY(newY);
        }

        auto* reference = toggles.back();
        auto* toggle = CCMenuItemToggler::createWithStandardSprites(this,
            menu_selector(TCEditorPauseLayer::onFrameWindows), 1.f);
        auto* label = CCLabelBMFont::create("Show frame windows", "bigFont.fnt");
        if (!toggle || !label) return;
        float const size = reference->getContentHeight() / std::max(1.f, toggle->getContentHeight());
        toggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(TCEditorPauseLayer::onFrameWindows), size);
        toggle->toggle(editorMarkersOn());
        toggle->setID("frame-windows-toggle"_spr);
        toggle->setPosition({reference->getPositionX(), bottom});
        menu->addChild(toggle);

        // Like the other labels: same x and scale, no wider than the widest.
        float labelScale = 0.f;
        float widest = 0.f;
        float labelX = 0.f;
        for (auto* other : labels) {
            labelScale = std::max(labelScale, other->getScale());
            widest = std::max(widest, other->getScaledContentWidth());
            labelX = other->getPositionX();
        }
        label->setAnchorPoint(labels.front()->getAnchorPoint());
        label->setScale(std::min(labelScale, widest / std::max(1.f, label->getContentWidth())));
        label->setPosition({labelX, bottom});
        label->setID("frame-windows-label"_spr);
        menu->addChild(label);
    }

    void onFrameWindows(CCObject* sender) {
        // The toggler flips after this call: its state here is the old one.
        bool const on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSavedValue(kEditorMarkersKey, on);
        refreshEditorMarkers(m_editorLayer, on);
    }
};
