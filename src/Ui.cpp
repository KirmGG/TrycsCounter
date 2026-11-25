// Menus: the TC menu, saved runs, names, and the intro card of a replay.

#include "App.hpp"
#include <Geode/utils/file.hpp>

namespace tc {

// Closes the pause menu (if it is open) and lets the game run.
void resumeFromPause() {
    if (auto* scene = CCDirector::sharedDirector()->getRunningScene()) {
        if (auto* pause = scene->getChildByType<PauseLayer>(0)) pause->onResume(nullptr);
    }
}

namespace {

void restartFromPause() {
    if (auto* scene = CCDirector::sharedDirector()->getRunningScene()) {
        if (auto* pause = scene->getChildByType<PauseLayer>(0)) pause->onRestart(nullptr);
    }
}

// What the symbols after a window mean (menu legend and replay intro).
std::vector<std::string> legendLines() {
    return {
        "~  depends on the previous click",
        "-  disconnected    ^  easier approach (Deep)",
        "!  swift    !!  double    !!!  triple swift",
        "+  hit the limit    ?  may be off",
    };
}

// Name of the measurement, like NaN GD's title cards.
struct CounterName {
    std::string title;      // "CBF Window Counter"
    std::string rate;       // "@24,000SubFPS"
    std::string header;     // "240Hz "Frames" w/ Syzzi's CBF"
};

std::string groupThousands(long long value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), ",");
    return digits;
}

CounterName counterName(Session const& s) {
    int const fps = cfg::windowFps();
    bool const cbf = std::any_of(s.source.events.begin(), s.source.events.end(), [](InputEvent const& e) { return e.cbf; });
    double const subPerFrame = static_cast<double>(cfg::kRawTicksPerPhysicsFrame * std::max(1, s.units)) /
        std::max(1, s.stepTicks);
    long long const subFps = std::llround(subPerFrame * cfg::tps());
    if (cbf) return {"CBF Window Counter", "@" + groupThousands(subFps) + "SubFPS",
        fmt::format("{}Hz \"Frames\" w/ Syzzi's CBF", fps)};
    if (s.precise) return {"Frame Window Counter", "@" + groupThousands(subFps) + "SubFPS",
        fmt::format("{}Hz \"Frames\" w/ Click Between Steps", fps)};
    return {"Frame Window Counter", fmt::format("@{}FPS", fps), fmt::format("{}Hz Frames", fps)};
}

// Black title card shown before a replay: level, counter, the buckets with
// their rates and times on the right, the symbols and L* on the left.
// A tap (or Esc) skips it.
class ReplayIntro : public CCLayerColor {
    std::function<void()> m_done;
    bool m_finished = false;

public:
    static ReplayIntro* create(std::function<void()> done) {
        auto* layer = new ReplayIntro();
        if (layer->setup(std::move(done))) {
            layer->autorelease();
            return layer;
        }
        delete layer;
        return nullptr;
    }

    void registerWithTouchDispatcher() override {
        CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, -1000, true);
    }

    bool ccTouchBegan(CCTouch*, CCEvent*) override {
        this->finish();
        return true;
    }

    void keyBackClicked() override {
        this->finish();
    }

    void finish() {
        if (m_finished) return;
        m_finished = true;
        auto done = std::move(m_done);
        Ref<ReplayIntro> self = this;
        this->setTouchEnabled(false);
        this->setKeypadEnabled(false);
        this->removeFromParentAndCleanup(true);
        if (done) done();
    }

protected:
    static CCLabelBMFont* text(CCNode* parent, std::string const& value, CCPoint pos, CCPoint anchor, float scale,
        ccColor3B color = {255, 255, 255}) {
        auto* label = CCLabelBMFont::create(value.c_str(), "bigFont.fnt");
        if (!label) return nullptr;
        label->setAnchorPoint(anchor);
        label->setScale(scale);
        label->setColor(color);
        label->setPosition(pos);
        parent->addChild(label);
        return label;
    }

    static void fitWidth(CCLabelBMFont* label, float width) {
        if (!label) return;
        float const w = label->getScaledContentSize().width;
        if (w > width) label->setScale(label->getScale() * width / w);
    }

    bool setup(std::function<void()> done) {
        if (!CCLayerColor::initWithColor({0, 0, 0, 255})) return false;
        m_done = std::move(done);
        this->setID("replay-intro"_spr);
        auto const& s = Session::get();
        auto* play = PlayLayer::get();
        auto const size = CCDirector::sharedDirector()->getWinSize();
        float const w = size.width;
        float const h = size.height;
        auto const name = counterName(s);

        // Left: level, creator, saved run name.
        float const left = w * 0.045f;
        float const leftWidth = w * 0.47f;
        std::string level = play && play->m_level ? std::string(play->m_level->m_levelName) : "Level";
        std::string creator = play && play->m_level ? std::string(play->m_level->m_creatorName) : "";
        float y = h - 30.f;
        fitWidth(text(this, level, {left, y}, {0.f, 0.5f}, 1.0f), leftWidth);
        y -= 27.f;
        if (!creator.empty()) {
            fitWidth(text(this, "by " + creator, {left, y}, {0.f, 0.5f}, 0.6f), leftWidth);
            y -= 20.f;
        }
        if (!s.resultsName.empty()) {
            fitWidth(text(this, "(" + s.resultsName + ")", {left, y}, {0.f, 0.5f}, 0.55f), leftWidth);
            y -= 20.f;
        }

        // The symbols.
        if (cfg::flag("show-symbols")) {
            y -= 6.f;
            for (auto const& line : legendLines()) {
                fitWidth(text(this, line, {left, y}, {0.f, 0.5f}, 0.3f, {200, 200, 200}), leftWidth);
                y -= 12.f;
            }
        }

        // Counter name and rate.
        fitWidth(text(this, name.title, {left, 104.f}, {0.f, 0.5f}, 0.78f), leftWidth);
        fitWidth(text(this, name.rate, {left, 80.f}, {0.f, 0.5f}, 0.78f), leftWidth);

        // Bottom left: the precision line.
        fitWidth(text(this, "(Precision is not Difficulty)", {left * 0.5f, 30.f}, {0.f, 0.5f}, 0.42f, {140, 140, 140}),
            leftWidth);
        std::string const precision = s.lstarReady && s.lstarTotal > 0.0
            ? fmt::format("Raw Precision (24h): {:.2f} sigma/s", s.lstarTotal)
            : std::string("Raw Precision (24h) in sigma/s");
        fitWidth(text(this, precision, {left * 0.5f, 12.f}, {0.f, 0.5f}, 0.42f), leftWidth);

        // Right: the buckets from the widest to the hardest, with rate and time.
        float const right = w - w * 0.035f;
        float const rightWidth = w * 0.46f;
        int const fps = cfg::windowFps();
        float const line = (h - 14.f) / (1.f + 2.f * static_cast<float>(kBucketLines));
        float ry = h - 10.f;
        fitWidth(text(this, name.header, {right, ry}, {1.f, 0.5f}, 0.42f), rightWidth);
        ry -= line;
        for (size_t i = kBucketLines; i-- > 0;) {
            auto const [lo, hi] = bucketBounds(i, s.precise, cfg::maxDisplayFrames());
            auto const color = overlay::kBucketColors[i];
            std::string frames;
            std::string rate;
            std::string ms;
            auto num = [](double v) { return fmt::format("{}", static_cast<int>(std::lround(v))); };
            if (lo == hi) {
                frames = fmt::format("{} Frame{}", num(lo), lo == 1.0 ? "" : "s");
                rate = fmt::format("({:.0f}Hz FP)", fps / lo);
                ms = fmt::format("({:.1f}ms)", lo * 1000.0 / fps);
            } else {
                frames = fmt::format("{}-{} Frames", num(lo), num(hi));
                rate = lo <= 0.0 ? fmt::format("(>{}Hz FP)", fps) : fmt::format("(<{:.0f}Hz FP)", fps / lo);
                ms = fmt::format("({:.1f}-{:.1f}ms)", lo * 1000.0 / fps, hi * 1000.0 / fps);
            }
            fitWidth(text(this, frames + " " + rate, {right, ry}, {1.f, 0.5f}, 0.4f, color), rightWidth);
            ry -= line;
            fitWidth(text(this, ms, {right, ry}, {1.f, 0.5f}, 0.4f, color), rightWidth);
            ry -= line;
        }
        text(this, "tc.", {w - 4.f, 4.f}, {1.f, 0.f}, 0.3f, {230, 40, 40});

        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);
        float const seconds = std::max(0.5f, cfg::real("intro-seconds"));
        this->runAction(CCSequence::create(CCDelayTime::create(seconds),
            CCCallFunc::create(this, callfunc_selector(ReplayIntro::finish)), nullptr));
        return true;
    }

    static constexpr size_t kBucketLines = kBucketCount;
};

} // namespace

// Starts the replay of the counted run, after the intro when it is enabled.
void beginReplay(PlayLayer* play) {
    auto start = [] {
        auto* current = PlayLayer::get();
        if (!current || Session::get().drivesInput() || !startReplay(current)) return;
        resumeFromPause();
    };
    if (!play || !resultsForLevel(play)) return;
    if (cfg::real("intro-seconds") <= 0.f) {
        start();
        return;
    }
    auto* scene = CCDirector::sharedDirector()->getRunningScene();
    auto* intro = scene ? ReplayIntro::create(start) : nullptr;
    if (!intro) {
        start();
        return;
    }
    scene->addChild(intro, 100000);
}

namespace {

// Asks for a name (saving or renaming a run).
class NamePopup : public geode::Popup {
    TextInput* m_input = nullptr;
    std::function<void(std::string)> m_done;

public:
    static NamePopup* create(std::string const& title, std::string const& initial, std::function<void(std::string)> done) {
        auto* popup = new NamePopup();
        if (popup->init(title, initial, std::move(done))) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }

protected:
    bool init(std::string const& title, std::string const& initial, std::function<void(std::string)> done) {
        if (!Popup::init(300.f, 140.f)) return false;
        m_done = std::move(done);
        this->setTitle(title.c_str());
        m_input = TextInput::create(250.f, "Name", "bigFont.fnt");
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(48);
        m_input->setString(initial);
        m_input->setPosition({150.f, 74.f});
        m_mainLayer->addChild(m_input);

        auto* menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        auto* ok = CCMenuItemSpriteExtra::create(ButtonSprite::create("OK", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(NamePopup::onOK));
        ok->setPosition({150.f, 28.f});
        menu->addChild(ok);
        m_mainLayer->addChild(menu);
        return true;
    }

    void onOK(CCObject*) {
        std::string name = m_input ? std::string(m_input->getString()) : std::string{};
        auto const first = name.find_first_not_of(" \t");
        auto const last = name.find_last_not_of(" \t");
        name = first == std::string::npos ? std::string{} : name.substr(first, last - first + 1);
        auto done = std::move(m_done);
        this->onClose(nullptr);
        if (done) done(name);
    }
};

// Default name of a new saved run.
std::string defaultRunName() {
    return "Run " + saved::formatTime(saved::nowUnix());
}

void askAndSaveRun(std::function<void()> after = {}) {
    askName("Save run", defaultRunName(), [after](std::string name) {
        if (saveCurrentRun(PlayLayer::get(), name) && after) after();
    });
}

// The TC menu: everything the mod does, opened from the pause menu.
//   status card       what is recorded / counted / saved
//   actions           record, count, Deep Count / replay, save, saved runs
//   display           checkboxes for Live Counter and what is shown
//   icons             legend (top right), folder, settings, clear results
class TCMenuPopup : public geode::Popup {
    CCNode* m_content = nullptr;
    CCLabelBMFont* m_status = nullptr;

public:
    static TCMenuPopup* create() {
        auto* popup = new TCMenuPopup();
        if (popup->init()) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }

protected:
    static constexpr float kWidth = 420.f;
    static constexpr float kHeight = 290.f;
    static constexpr float kPanelWidth = 384.f;

    bool init() {
        if (!Popup::init(kWidth, kHeight)) return false;
        this->setTitle("TrycsCounter", "goldFont.fnt", 0.75f, 22.f);
        this->setID("menu"_spr);
        this->refresh();
        return true;
    }

    // Dark rounded card behind a group.
    CCNode* panel(float centerY, float height) {
        auto* card = NineSlice::create("square02b_001.png");
        card->setColor({0, 0, 0});
        card->setOpacity(70);
        card->setContentSize({kPanelWidth, height});
        card->setAnchorPoint({0.5f, 0.5f});
        card->setPosition({kWidth / 2.f, centerY});
        m_content->addChild(card);
        return card;
    }

    // A row of buttons that lays itself out and shrinks to fit.
    CCMenu* row(float centerY) {
        auto* menu = CCMenu::create();
        menu->ignoreAnchorPointForPosition(false);
        menu->setAnchorPoint({0.5f, 0.5f});
        menu->setContentSize({kPanelWidth - 16.f, 32.f});
        menu->setPosition({kWidth / 2.f, centerY});
        menu->setLayout(RowLayout::create()->setGap(7.f)->setAutoScale(true));
        m_content->addChild(menu);
        return menu;
    }

    void action(CCMenu* menu, std::string const& text, char const* bg, SEL_MenuHandler selector, bool enabled = true) {
        auto* sprite = ButtonSprite::create(text.c_str(), "goldFont.fnt", enabled ? bg : "GJ_button_04.png", 0.72f);
        sprite->setScale(0.82f);
        if (!enabled) sprite->setOpacity(140);
        auto* item = CCMenuItemSpriteExtra::create(sprite, this, selector);
        item->setEnabled(enabled);
        menu->addChild(item);
    }

    void checkbox(CCMenu* menu, float x, float y, char const* text, bool on, SEL_MenuHandler selector, bool enabled = true) {
        auto* toggler = CCMenuItemToggler::createWithStandardSprites(this, selector, 0.62f);
        toggler->toggle(on);
        toggler->setPosition({x, y});
        toggler->setEnabled(enabled);
        if (!enabled) {
            if (auto* sprite = typeinfo_cast<CCRGBAProtocol*>(toggler->m_onButton->getNormalImage())) sprite->setOpacity(110);
            if (auto* sprite = typeinfo_cast<CCRGBAProtocol*>(toggler->m_offButton->getNormalImage())) sprite->setOpacity(110);
        }
        menu->addChild(toggler);
        auto* label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({x + 15.f, y + 1.f});
        label->limitLabelWidth(92.f, 0.36f, 0.2f);
        if (!enabled) label->setOpacity(120);
        m_content->addChild(label);
    }

    // The game's own round icon buttons (info, folder, gear, trash).
    void icon(CCMenu* menu, char const* frame, float height, SEL_MenuHandler selector, CCPoint pos, bool enabled = true) {
        CCSprite* sprite = CCSprite::createWithSpriteFrameName(frame);
        if (!sprite) sprite = ButtonSprite::create("?", "goldFont.fnt", "GJ_button_04.png", 0.6f);
        sprite->setScale(height / std::max(1.f, sprite->getContentSize().height));
        if (!enabled) sprite->setOpacity(110);
        auto* item = CCMenuItemSpriteExtra::create(sprite, this, selector);
        item->setPosition(pos);
        item->setEnabled(enabled);
        menu->addChild(item);
    }

    std::string statusText() const {
        auto const& s = Session::get();
        auto* play = PlayLayer::get();
        std::string first;
        if (s.isAnalyzing()) first = s.deepMode ? "Deep Count running" : "Counting frame windows";
        else if (s.stage == Stage::Replay) first = "Replay running";
        else if (s.stage == Stage::Recording) first = cfg::flag("live-counter") ? std::string("Live Counter measuring") :
            fmt::format("Recording ({} inputs)", s.recording.size());
        else first = "Not recording";
        size_t const saved = savedRunCount(s.levelKey);
        first += fmt::format("   -   {} saved run{}", saved, saved == 1 ? "" : "s");

        std::string second = "No counted run for this level yet";
        if (resultsForLevel(play)) {
            int shown = 0;
            double hardest = 0.0;
            int dependent = 0;
            for (size_t i = 0; i < s.windows.size() && i < s.source.events.size(); ++i) {
                if (!overlay::isShown(s.source.events[i], s.windows[i])) continue;
                ++shown;
                double const f = cfg::frames(s.windows[i]);
                hardest = hardest <= 0.0 ? f : std::min(hardest, f);
                if (i < s.dependent.size() && s.dependent[i]) ++dependent;
            }
            second = fmt::format("{}: {} clicks   hardest {}   ~ {}{}",
                s.resultsName.empty() ? (s.resultsSaved ? "Saved run" : "Unsaved run") : s.resultsName, shown,
                shown ? cfg::windowLabel(hardest, s.precise, false) : "-", dependent,
                s.lstarReady && s.lstarTotal > 0.0 ? fmt::format("   L* {:.2f}", s.lstarTotal) : "");
        }
        return first + "\n" + second;
    }

    void updateStatus() {
        if (!m_status) return;
        m_status->setString(statusText().c_str());
        m_status->limitLabelWidth(kPanelWidth - 20.f, 0.62f, 0.3f);
    }

    // Rebuilt after the next frame: a button must not be removed while it is
    // still handling its own click.
    void refreshLater() {
        Ref<TCMenuPopup> self = this;
        Loader::get()->queueInMainThread([self] {
            if (self->getParent()) self->refresh();
        });
    }

    void refresh() {
        if (m_content) m_content->removeFromParentAndCleanup(true);
        m_content = CCNode::create();
        m_content->setContentSize({kWidth, kHeight});
        m_mainLayer->addChild(m_content);
        auto& s = Session::get();
        auto* play = PlayLayer::get();
        bool const busy = s.drivesInput();
        bool const results = resultsForLevel(play);

        // Status card.
        panel(226.f, 46.f);
        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setAlignment(kCCTextAlignmentCenter);
        m_status->setPosition({kWidth / 2.f, 226.f});
        m_content->addChild(m_status);
        this->updateStatus();

        // Actions.
        panel(158.f, 78.f);
        auto* top = row(177.f);
        if (s.training.active) {
            action(top, "Stop Training", "GJ_button_06.png", menu_selector(TCMenuPopup::onStopTraining));
        } else if (s.isAnalyzing()) {
            action(top, s.deepMode ? "Stop Deep Count" : "Stop Counting", "GJ_button_06.png", menu_selector(TCMenuPopup::onStop));
        } else if (s.stage == Stage::Replay) {
            action(top, "Stop Replay", "GJ_button_06.png", menu_selector(TCMenuPopup::onStop));
        } else {
            bool const recording = recordingWanted();
            bool const live = cfg::flag("live-counter");
            action(top, recording && !live ? "Stop Recording" : "Record", recording && !live ? "GJ_button_06.png" : "GJ_button_01.png",
                menu_selector(TCMenuPopup::onRecord), !live);
            action(top, "Count Windows", "GJ_button_01.png", menu_selector(TCMenuPopup::onCount));
            action(top, "Count Macro", "GJ_button_05.png", menu_selector(TCMenuPopup::onMacro));
            action(top, "Deep Count", "GJ_button_02.png", menu_selector(TCMenuPopup::onDeep), results);
        }
        top->updateLayout();
        auto* bottom = row(139.f);
        action(bottom, "Replay", "GJ_button_02.png", menu_selector(TCMenuPopup::onReplay), results && !busy);
        action(bottom, "Graph", "GJ_button_02.png", menu_selector(TCMenuPopup::onGraph), results);
        action(bottom, s.resultsSaved && results ? "Saved" : "Save Run", "GJ_button_01.png",
            menu_selector(TCMenuPopup::onSave), results && !busy && !s.resultsSaved);
        action(bottom, fmt::format("Saved Runs ({})", savedRunCount(s.levelKey)), "GJ_button_05.png",
            menu_selector(TCMenuPopup::onSavedRuns), !busy);
        bottom->updateLayout();

        // Display options.
        panel(80.f, 62.f);
        auto* checks = CCMenu::create();
        checks->setPosition({0.f, 0.f});
        m_content->addChild(checks);
        float const x1 = kWidth / 2.f - kPanelWidth / 2.f + 22.f;
        float const x2 = x1 + 128.f;
        float const x3 = x2 + 128.f;
        checkbox(checks, x1, 95.f, "Live Counter", cfg::flag("live-counter"), menu_selector(TCMenuPopup::onLive), !busy);
        checkbox(checks, x2, 95.f, "Exact live", cfg::text("live-mode") == "Exact", menu_selector(TCMenuPopup::onLiveMode), !busy);
        checkbox(checks, x3, 95.f, "Counter always", cfg::flag("hud-in-attempts"), menu_selector(TCMenuPopup::onHudMode));
        checkbox(checks, x1, 65.f, "Releases", cfg::flag("show-releases"), menu_selector(TCMenuPopup::onReleases), !busy);
        checkbox(checks, x2, 65.f, "Symbols", cfg::flag("show-symbols"), menu_selector(TCMenuPopup::onSymbols));
        checkbox(checks, x3, 65.f, "Ding", cfg::flag("ding-enabled"), menu_selector(TCMenuPopup::onDing));

        // Icons: legend in the corner, the rest along the bottom.
        auto* icons = CCMenu::create();
        icons->setPosition({0.f, 0.f});
        m_content->addChild(icons);
        icon(icons, "GJ_infoIcon_001.png", 24.f, menu_selector(TCMenuPopup::onLegend),
            {kWidth - 24.f, kHeight - 24.f});
        icon(icons, "gj_folderBtn_001.png", 30.f, menu_selector(TCMenuPopup::onFolder),
            {kWidth / 2.f - 42.f, 26.f});
        icon(icons, "GJ_optionsBtn_001.png", 30.f, menu_selector(TCMenuPopup::onSettings),
            {kWidth / 2.f, 26.f});
        icon(icons, "GJ_trashBtn_001.png", 30.f, menu_selector(TCMenuPopup::onClear),
            {kWidth / 2.f + 42.f, 26.f}, results && !busy);
    }

    void onStop(CCObject*) {
        auto& s = Session::get();
        s.macroQueue.clear();
        s.queuedMacro.clear();
        if (s.isAnalyzing()) {
            s.stage = Stage::Idle;
            endAnalysisEnvironment(PlayLayer::get());
        } else {
            s.stage = Stage::Idle;
        }
        this->onClose(nullptr);
        resumeFromPause();
    }

    void onRecord(CCObject*) {
        auto& s = Session::get();
        auto* play = PlayLayer::get();
        if (!play) return;
        if (recordingWanted()) {
            // Keeps what was recorded: it can still be counted.
            s.recordingEnabled = false;
            s.stage = idleStage(play);
            Notification::create("Recording stopped", NotificationIcon::Info)->show();
            this->refreshLater();
            return;
        }
        // Recording runs in practice mode, restarted at the start of the level.
        // The old checkpoints go: respawns must stay inside the recording.
        s.recordingEnabled = true;
        if (!play->m_isPracticeMode) play->togglePracticeMode(true);
        play->removeAllCheckpoints();
        Notification::create("Recording in practice: place checkpoints and respawn as you like, then TC > Count Windows.",
            NotificationIcon::Success)->show();
        this->onClose(nullptr);
        restartFromPause();
    }

    void onCount(CCObject*) {
        auto* play = PlayLayer::get();
        if (!play) return;
        auto const error = startAnalysis(play, selectRun(play));
        if (!error.empty()) {
            FLAlertLayer::create("TrycsCounter", error, "OK")->show();
            return;
        }
        this->onClose(nullptr);
        resumeFromPause();
    }

    void onStopTraining(CCObject*) {
        stopTraining();
        this->refreshLater();
    }

    void onGraph(CCObject*) {
        if (resultsForLevel(PlayLayer::get())) openGraph();
    }

    // Counts a bot macro (GDR) instead of a recorded attempt.
    void onMacro(CCObject*) {
        this->onClose(nullptr);
        pickMacro();
    }

    void onDeep(CCObject*) {
        if (!resultsForLevel(PlayLayer::get())) return;
        Ref<TCMenuPopup> self = this;
        createQuickPopup("Deep Count",
            "First checks the <cy>edges</c> of every window once more (unsure ones get <cy>?</c>). Then "
            "measures every click again with <cy>the previous click at other timings</c>, tries a few frames "
            "<cy>past its edges</c> and looks for <cy>an easier approach</c> to hard clicks. Clicks whose window "
            "changes get <cy>~</c> and the window they have most often, clicks that work again beyond a gap get "
            "<cg>-</c>, clicks with an easier approach get <cl>^</c>; the replay explains them at the top right.\n"
            "Takes several times as long as a normal count.",
            "Cancel", "Start", [self](FLAlertLayer*, bool start) {
                if (!start) return;
                auto& s = Session::get();
                auto* play = PlayLayer::get();
                if (!play || !resultsForLevel(play)) return;
                RunRecord run = s.source;
                auto const error = startAnalysis(play, std::move(run), true);
                if (!error.empty()) {
                    FLAlertLayer::create("TrycsCounter", error, "OK")->show();
                    return;
                }
                self->onClose(nullptr);
                resumeFromPause();
            });
    }

    void onReplay(CCObject*) {
        auto* play = PlayLayer::get();
        if (!resultsForLevel(play)) return;
        this->onClose(nullptr);
        beginReplay(play);
    }

    void onSave(CCObject*) {
        Ref<TCMenuPopup> self = this;
        askAndSaveRun([self] { self->refreshLater(); });
    }

    void onSavedRuns(CCObject*) {
        openSavedRuns(Session::get().levelKey, saved::levelNameOf(PlayLayer::get()));
    }

    // Checkboxes flip themselves; only the setting and what it shows change.
    void flip(char const* key) {
        Mod::get()->setSettingValue<bool>(key, !cfg::flag(key));
        refreshDisplay(PlayLayer::get());
        this->updateStatus();
    }

    void onLive(CCObject*) {
        this->flip("live-counter");
        auto& s = Session::get();
        if (auto* play = PlayLayer::get(); play && !s.drivesInput()) s.stage = idleStage(play);
        this->refreshLater();   // Record is not available with Live Counter
    }

    void onLiveMode(CCObject*) {
        bool const exact = cfg::text("live-mode") == "Exact";
        Mod::get()->setSettingValue<std::string>("live-mode", exact ? "Trajectory" : "Exact");
        if (!exact && !PlayLayer::get()->m_isPracticeMode)
            Notification::create("Exact Live Counter measures in practice mode", NotificationIcon::Info)->show();
    }

    void onHudMode(CCObject*) { this->flip("hud-in-attempts"); }
    void onReleases(CCObject*) {
        this->flip("show-releases");
        computeLStar();
    }
    void onSymbols(CCObject*) { this->flip("show-symbols"); }
    void onDing(CCObject*) { this->flip("ding-enabled"); }

    void onLegend(CCObject*) {
        std::string text = "<cy>~</c>  depends on the previous click: a shift of that click only worked out when this "
            "one was re-timed. After Deep Count: the window it usually has.\n<cg>-</c>  Deep Count: disconnected window, the "
            "click works again beyond a gap; the number is the piece around your click.\n<cl>^</c>  Deep Count: with the inputs before it timed differently the click is at least a frame wider (the note says how).\n<co>!</c>  swift click (pressed right after the previous press), <co>!!</c> double swift, "
            "<co>!!!</c> triple swift.\n<cr>+</c>  the window reached a limit and may be wider.\n<cp>?</c>  the counting "
            "replay had left your attempt before this click, or the edge check got another outcome on a re-check; the window may be off.";
        FLAlertLayer::create(nullptr, "Symbols", text, "OK", nullptr, 400.f)->show();
    }

    void onClear(CCObject*) {
        Ref<TCMenuPopup> self = this;
        createQuickPopup("Clear results", "Remove the counted run from this level? Saved runs stay on disk.", "Cancel",
            "Clear", [self](FLAlertLayer*, bool clear) {
                if (!clear) return;
                auto& s = Session::get();
                s.hasResults = false;
                s.windows.clear();
                s.dependent.clear();
                s.edgeUncertain.clear();
                s.deep.clear();
                s.resultsKey.clear();
                s.resultsName.clear();
                ++s.lstarGeneration;
                s.lstarReady = false;
                refreshDisplay(PlayLayer::get());
                self->refreshLater();
            });
    }

    void onFolder(CCObject*) {
        auto const dir = saved::runsDir(Session::get().levelKey);
        (void)utils::file::createDirectoryAll(dir);
        utils::file::openFolder(dir);
    }

    void onSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }
};

} // namespace

// Asks for a name; `done` gets it trimmed (empty when nothing was typed).
void askName(std::string const& title, std::string const& initial, std::function<void(std::string)> done) {
    if (auto* popup = NamePopup::create(title, initial, std::move(done))) popup->show();
}

// Opens the mod's menu (the TC button in the pause menu).
void openMenu() {
    if (auto* popup = TCMenuPopup::create()) popup->show();
}

} // namespace tc
