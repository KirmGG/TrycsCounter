// The library of counted runs.

#include "App.hpp"
#include <Geode/utils/file.hpp>
#include <memory>
#include <numeric>

namespace tc {

namespace {

// A run started from the library while no level was open: replayed once its level starts.
struct PendingReplay {
    std::string key;
    std::filesystem::path path;
};
std::optional<PendingReplay> g_pendingReplay;

// The level of a key on this device, nullptr when it is not here.
GJGameLevel* findLevel(std::string const& key) {
    if (key.starts_with("local-")) {
        auto* local = LocalLevelManager::sharedState();
        if (!local || !local->m_localLevels) return nullptr;
        for (auto* level : CCArrayExt<GJGameLevel*>(local->m_localLevels)) {
            if (saved::levelKeyOf(level) == key) return level;
        }
        return nullptr;
    }
    auto const id = utils::numFromString<int>(key);
    if (id.isErr()) return nullptr;
    auto* manager = GameLevelManager::sharedState();
    if (auto* level = manager->getSavedLevel(id.unwrap()); level && !std::string(level->m_levelString).empty())
        return level;
    // The official levels are not saved like online ones (online IDs start at 128).
    int const n = id.unwrap();
    if (n < 128 || n == 3001 || (n > 5000 && n < 5100)) return manager->getMainLevel(n, false);
    return nullptr;
}

void closeAllPopups() {
    std::vector<FLAlertLayer*> popups;
    if (auto* scene = CCDirector::sharedDirector()->getRunningScene()) {
        for (auto* node : CCArrayExt<CCNode*>(scene->getChildren())) {
            if (auto* popup = typeinfo_cast<FLAlertLayer*>(node)) popups.push_back(popup);
        }
    }
    for (auto* popup : popups) popup->keyBackClicked();
}

// Replays a saved run: in its level right away, from the main menu by opening the level.
void playRun(std::string const& key, std::filesystem::path const& path) {
    auto& s = Session::get();
    if (auto* play = PlayLayer::get()) {
        if (key != s.levelKey) {
            Notification::create("This run belongs to another level", NotificationIcon::Warning)->show();
            return;
        }
        if (s.drivesInput() || !loadSavedRun(play, path)) return;
        closeAllPopups();
        beginReplay(play);
        return;
    }
    auto* level = findLevel(key);
    if (!level) {
        FLAlertLayer::create("TrycsCounter",
            "The level of this run is not on this device. Download or open it once, then try again.", "OK")->show();
        return;
    }
    g_pendingReplay = PendingReplay{key, path};
    closeAllPopups();
    CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(0.5f, PlayLayer::scene(level, false, false)));
}

std::string levelLabel(std::string const& key) {
    return key.starts_with("local-") ? "Local level" : "ID " + key;
}

class RunStatsPopup;

// A run opened from a list, with what its stats and graph need.
struct OpenedRun {
    std::string key;
    std::filesystem::path path;
    std::string title;
    saved::SavedRun data;
    std::vector<int> swift;
    std::vector<double> lstar;   // per input, NaN for the clicks that are not shown
    double lstarTotal = 0.0;
    bool lstarReady = false;
    RunStatsPopup* popup = nullptr;   // the stats on screen, told when L* is ready

    RunView view() const { return RunView(data, swift); }
};

// L* of every click in the background, like computeLStar for the current results.
void computeRunLStar(std::shared_ptr<OpenedRun> const& run);

// Stats of one run: what it is, its hardest clicks, precision and marks; Graph and Play.
class RunStatsPopup : public geode::Popup {
    static constexpr float kWidth = 380.f;
    static constexpr float kHeight = 250.f;

    std::shared_ptr<OpenedRun> m_run;
    CCNode* m_lines = nullptr;

public:
    static RunStatsPopup* create(std::shared_ptr<OpenedRun> run) {
        auto* popup = new RunStatsPopup();
        if (popup->setup(std::move(run))) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }

    ~RunStatsPopup() override {
        if (m_run && m_run->popup == this) m_run->popup = nullptr;
    }

    void refresh() {
        if (m_lines) m_lines->removeFromParent();
        m_lines = CCNode::create();
        m_mainLayer->addChild(m_lines);
        float y = kHeight - 52.f;
        for (auto const& line : statsLines()) {
            auto* label = CCLabelBMFont::create(line.c_str(), "chatFont.fnt");
            label->setAnchorPoint({0.f, 0.5f});
            label->setPosition({20.f, y});
            label->limitLabelWidth(kWidth - 40.f, 0.7f, 0.3f);
            m_lines->addChild(label);
            y -= 18.f;
        }
    }

protected:
    bool setup(std::shared_ptr<OpenedRun> run) {
        if (!Popup::init(kWidth, kHeight)) return false;
        m_run = std::move(run);
        m_run->popup = this;
        this->setTitle(m_run->title.c_str());

        auto* menu = CCMenu::create();
        menu->setContentSize({kWidth - 40.f, 30.f});
        menu->ignoreAnchorPointForPosition(false);
        menu->setPosition({kWidth / 2.f, 24.f});
        menu->setLayout(RowLayout::create()->setGap(8.f));
        for (auto [text, bg, selector] : {
                 std::tuple{"Graph", "GJ_button_02.png", menu_selector(RunStatsPopup::onGraph)},
                 std::tuple{"Play", "GJ_button_01.png", menu_selector(RunStatsPopup::onPlay)},
             }) {
            auto* sprite = ButtonSprite::create(text, "goldFont.fnt", bg, 0.7f);
            sprite->setScale(0.7f);
            menu->addChild(CCMenuItemSpriteExtra::create(sprite, this, selector));
        }
        menu->updateLayout();
        m_mainLayer->addChild(menu);
        this->refresh();
        return true;
    }

    std::vector<std::string> statsLines() const {
        auto const& run = *m_run;
        auto const& data = run.data;
        auto const view = run.view();
        std::vector<std::string> lines;
        lines.push_back(fmt::format("{}  |  {}", data.levelName.empty() ? levelLabel(run.key) : data.levelName,
            levelLabel(run.key)));
        char const* end = data.run.endKind == RunEnd::Completed ? "completed"
            : data.run.endKind == RunEnd::Death                  ? "ended with a death"
                                                                 : "ended paused";
        lines.push_back(fmt::format("Counted {}, {}, {:.1f} s", saved::formatTime(data.created), end,
            data.run.endTick / static_cast<double>(cfg::rawTps())));

        std::vector<double> windows;
        size_t hardest = 0;
        double hardestFrames = 0.0;
        int oneFrame = 0;
        for (size_t i = 0; i < data.run.events.size() && i < data.windows.size(); ++i) {
            if (!overlay::isShown(data.run.events[i], data.windows[i])) continue;
            double const frames = shownWindow(view, i);
            if (windows.empty() || frames < hardestFrames) {
                hardest = i;
                hardestFrames = frames;
            }
            if (frames <= 1.0 + 1e-9) ++oneFrame;
            windows.push_back(frames);
        }
        bool const deep = std::any_of(data.deep.begin(), data.deep.end(),
            [](DeepRange const& d) { return d.ranged() || d.broken || d.disconnected() || d.easier > 0.0; });
        lines.push_back(fmt::format("{} inputs, {} counted clicks{}", data.run.events.size(), windows.size(),
            deep ? ", with Deep Count" : ""));
        if (windows.empty()) return lines;

        bool const precise = cfg::isPrecise(data.windows[hardest]);
        auto const& event = data.run.events[hardest];
        std::string where = fmt::format("{:.2f} s", event.tick / static_cast<double>(cfg::rawTps()));
        if (data.levelLength > 0.f) where = fmt::format("{:.1f}%, {}", 100.0 * event.x / data.levelLength, where);
        lines.push_back(fmt::format("Hardest: {}{} (click #{}, {})", cfg::windowLabel(hardestFrames, precise, false),
            resultMarks(view, hardest), hardest + 1, where));

        auto sorted = windows;
        std::sort(sorted.begin(), sorted.end());
        double const average = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
        lines.push_back(fmt::format("Average {:.2f} frames, median {:.2f}, {} of 1 frame or less", average,
            sorted[sorted.size() / 2], oneFrame));

        if (!run.lstarReady) lines.push_back("Precision L*: computing...");
        else if (run.lstarTotal > 0.0) lines.push_back(fmt::format("Precision L*: {:.2f} sigma/s (24 h)", run.lstarTotal));

        int dependent = 0, uncertain = 0, easier = 0, disconnected = 0, swift = 0;
        for (size_t i = 0; i < data.run.events.size() && i < data.windows.size(); ++i) {
            if (!overlay::isShown(data.run.events[i], data.windows[i])) continue;
            if (i < data.dependent.size() && data.dependent[i]) ++dependent;
            if (i < data.uncertain.size() && data.uncertain[i]) ++uncertain;
            if (hasEasierApproach(view, i)) ++easier;
            if (i < data.deep.size() && data.deep[i].disconnected()) ++disconnected;
            if (i < run.swift.size() && run.swift[i] > 0) ++swift;
        }
        std::string marks;
        auto add = [&](int count, char const* symbol) {
            if (count == 0) return;
            if (!marks.empty()) marks += ", ";
            marks += fmt::format("{} {}", symbol, count);
        };
        add(dependent, "~");
        add(disconnected, "-");
        add(easier, "^");
        add(swift, "!");
        add(uncertain, "?");
        if (!marks.empty()) lines.push_back("Marks: " + marks);
        return lines;
    }

    void onGraph(CCObject*) {
        auto const& run = *m_run;
        openGraph(run.view(), run.data.levelLength, run.lstarReady ? &run.lstar : nullptr, run.lstarTotal,
            run.data.precise);
    }

    void onPlay(CCObject*) {
        playRun(m_run->key, m_run->path);
    }
};

void computeRunLStar(std::shared_ptr<OpenedRun> const& run) {
    std::vector<size_t> owners;
    auto inputs = lstarInputs(run->view(), &owners);
    run->lstar.assign(run->data.run.events.size(), std::numeric_limits<double>::quiet_NaN());
    if (inputs.empty()) {
        run->lstarReady = true;
        return;
    }
    std::weak_ptr<OpenedRun> weak = run;
    std::thread([weak, inputs = std::move(inputs), owners = std::move(owners)]() mutable {
        auto values = precision::prefixLStar(inputs);
        Loader::get()->queueInMainThread([weak, values = std::move(values), owners = std::move(owners)] {
            auto run = weak.lock();
            if (!run) return;
            for (size_t j = 0; j < owners.size() && j < values.size(); ++j) {
                if (owners[j] < run->lstar.size()) run->lstar[owners[j]] = values[j];
            }
            run->lstarTotal = values.empty() ? 0.0 : values.back();
            run->lstarReady = true;
            if (run->popup) run->popup->refresh();
        });
    }).detach();
}

void openRunStats(std::string const& key, std::filesystem::path const& path, std::string const& title) {
    auto loaded = saved::load(path);
    if (loaded.isErr()) {
        Notification::create("Could not load the run: " + loaded.unwrapErr(), NotificationIcon::Error)->show();
        return;
    }
    auto run = std::make_shared<OpenedRun>();
    run->key = key;
    run->path = path;
    run->title = title;
    run->data = std::move(loaded).unwrap();
    run->swift = swiftLevelsOf(run->data.run.events);
    computeRunLStar(run);
    if (auto* popup = RunStatsPopup::create(run)) popup->show();
}

// A row of a list: title, detail line and buttons on the right.
CCMenu* listRow(CCNode* list, float width, std::string const& title, std::string const& detail) {
    auto* row = CCNode::create();
    row->setContentSize({width, 40.f});
    row->setAnchorPoint({0.f, 0.f});
    row->addChild(CCLayerColor::create({0, 0, 0, 70}, width, 40.f));

    auto* name = CCLabelBMFont::create(title.c_str(), "goldFont.fnt");
    name->setAnchorPoint({0.f, 0.5f});
    name->setPosition({8.f, 28.f});
    name->limitLabelWidth(width - 230.f, 0.5f, 0.2f);
    row->addChild(name);

    auto* info = CCLabelBMFont::create(detail.c_str(), "chatFont.fnt");
    info->setAnchorPoint({0.f, 0.5f});
    info->setPosition({8.f, 11.f});
    info->limitLabelWidth(width - 230.f, 0.5f, 0.2f);
    row->addChild(info);

    // Laid out (and shrunk if needed) by the menu.
    auto* menu = CCMenu::create();
    menu->ignoreAnchorPointForPosition(false);
    menu->setAnchorPoint({1.f, 0.5f});
    menu->setContentSize({214.f, 30.f});
    menu->setPosition({width - 6.f, 20.f});
    menu->setLayout(RowLayout::create()->setGap(5.f)->setAxisAlignment(AxisAlignment::End)->setAutoScale(true));
    row->addChild(menu);
    list->addChild(row);
    return menu;
}

void rowButton(CCMenu* menu, char const* text, char const* bg, CCObject* target, SEL_MenuHandler selector, int tag) {
    auto* sprite = ButtonSprite::create(text, "goldFont.fnt", bg, 0.7f);
    sprite->setScale(0.62f);
    auto* item = CCMenuItemSpriteExtra::create(sprite, target, selector);
    item->setTag(tag);
    menu->addChild(item);
}

ScrollLayer* createList(CCNode* parent, float popupWidth, float rowWidth, float height, float y) {
    auto* list = ScrollLayer::create(CCSize{rowWidth, height});
    list->setPosition({(popupWidth - rowWidth) / 2.f, y});
    list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(4.f));
    parent->addChild(list);
    return list;
}

// The runs of one level: the last count and the saved runs.
class SavedRunsPopup : public geode::Popup {
    static constexpr float kWidth = 440.f;
    static constexpr float kRowWidth = 400.f;
    // Tag of the last count row (saved runs are tagged by their index).
    static constexpr int kLastCountTag = -1;

    std::string m_key;
    std::string m_levelName;
    std::optional<saved::SavedRunInfo> m_last;
    std::vector<saved::SavedRunInfo> m_runs;
    ScrollLayer* m_list = nullptr;
    CCLabelBMFont* m_empty = nullptr;
    std::function<void()> m_changed;   // a run was saved, renamed or deleted

public:
    static SavedRunsPopup* create(std::string key, std::string levelName, std::function<void()> changed = {}) {
        auto* popup = new SavedRunsPopup();
        popup->m_changed = std::move(changed);
        if (popup->setup(std::move(key), std::move(levelName))) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }

protected:
    bool setup(std::string key, std::string levelName) {
        if (!Popup::init(kWidth, 260.f)) return false;
        m_key = std::move(key);
        m_levelName = std::move(levelName);
        this->setTitle(m_levelName.empty() ? "Saved Runs" : m_levelName.c_str());
        m_list = createList(m_mainLayer, kWidth, kRowWidth, 185.f, 25.f);
        m_empty = CCLabelBMFont::create("No runs for this level", "goldFont.fnt");
        m_empty->setScale(0.55f);
        m_empty->setPosition({kWidth / 2.f, 118.f});
        m_mainLayer->addChild(m_empty);
        this->refresh();
        return true;
    }

    // The results of this level are on screen: a run can be shown there.
    bool inLevel() const {
        return PlayLayer::get() && Session::get().levelKey == m_key;
    }

    // The files changed: the list and whoever opened it.
    void changed() {
        this->refresh();
        if (m_changed) m_changed();
    }

    void refresh() {
        m_last = saved::lastCount(m_key);
        m_runs = saved::list(m_key);
        auto* content = m_list->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);
        m_empty->setVisible(m_runs.empty() && !m_last);
        if (m_last) {
            auto* menu = listRow(content, kRowWidth, "Last count", detailOf(*m_last, false));
            rowButton(menu, "Stats", "GJ_button_02.png", this, menu_selector(SavedRunsPopup::onStats), kLastCountTag);
            rowButton(menu, "Play", "GJ_button_01.png", this, menu_selector(SavedRunsPopup::onPlay), kLastCountTag);
            rowButton(menu, "Save", "GJ_button_05.png", this, menu_selector(SavedRunsPopup::onKeep), kLastCountTag);
            menu->updateLayout();
        }
        for (size_t i = 0; i < m_runs.size(); ++i) {
            auto const& run = m_runs[i];
            auto* menu = listRow(content, kRowWidth, saved::titleOf(run), detailOf(run, !run.name.empty()));
            int const tag = static_cast<int>(i);
            if (inLevel()) rowButton(menu, "Show", "GJ_button_02.png", this, menu_selector(SavedRunsPopup::onShow), tag);
            rowButton(menu, "Stats", "GJ_button_02.png", this, menu_selector(SavedRunsPopup::onStats), tag);
            rowButton(menu, "Play", "GJ_button_01.png", this, menu_selector(SavedRunsPopup::onPlay), tag);
            rowButton(menu, "Name", "GJ_button_05.png", this, menu_selector(SavedRunsPopup::onRename), tag);
            rowButton(menu, "X", "GJ_button_06.png", this, menu_selector(SavedRunsPopup::onDelete), tag);
            menu->updateLayout();
        }
        content->updateLayout();
        m_list->scrollToTop();
    }

    static std::string detailOf(saved::SavedRunInfo const& run, bool withDate) {
        std::string detail = run.summary.empty() ? fmt::format("{} inputs", run.inputs) : run.summary;
        if (withDate || run.path.extension() != ".json") detail = saved::formatTime(run.created) + "  " + detail;
        if (run.deep) detail += ", deep";
        return detail;
    }

    saved::SavedRunInfo const* runOf(CCObject* sender) const {
        int const tag = static_cast<CCNode*>(sender)->getTag();
        if (tag == kLastCountTag) return m_last ? &*m_last : nullptr;
        auto const index = static_cast<size_t>(tag);
        return index < m_runs.size() ? &m_runs[index] : nullptr;
    }

    std::string titleOf(CCObject* sender) const {
        int const tag = static_cast<CCNode*>(sender)->getTag();
        auto const* run = runOf(sender);
        return tag == kLastCountTag || !run ? "Last count" : saved::titleOf(*run);
    }

    // Makes the run the current results: its markers show in attempts.
    void onShow(CCObject* sender) {
        auto const* run = runOf(sender);
        auto* play = PlayLayer::get();
        if (!run || !play || !inLevel() || Session::get().drivesInput()) return;
        if (loadSavedRun(play, run->path))
            Notification::create(fmt::format("Showing {}", saved::titleOf(*run)), NotificationIcon::Success)->show();
    }

    void onStats(CCObject* sender) {
        if (auto const* run = runOf(sender)) openRunStats(m_key, run->path, titleOf(sender));
    }

    void onPlay(CCObject* sender) {
        if (auto const* run = runOf(sender)) playRun(m_key, run->path);
    }

    // The last count becomes a saved run under a name.
    void onKeep(CCObject*) {
        if (!m_last) return;
        auto const path = m_last->path;
        auto const key = m_key;
        Ref<SavedRunsPopup> self = this;
        askName("Save run", "Run " + saved::formatTime(m_last->created), [self, path, key](std::string name) {
            auto loaded = saved::load(path);
            if (loaded.isErr()) {
                Notification::create("Could not load the run: " + loaded.unwrapErr(), NotificationIcon::Error)->show();
                return;
            }
            auto data = std::move(loaded).unwrap();
            data.name = name;
            data.created = 0;
            if (auto saved = saved::save(key, std::move(data)); saved.isErr()) {
                Notification::create("Could not save the run: " + saved.unwrapErr(), NotificationIcon::Error)->show();
                return;
            }
            Notification::create("Run saved", NotificationIcon::Success)->show();
            self->changed();
        });
    }

    void onRename(CCObject* sender) {
        auto const* run = runOf(sender);
        if (!run) return;
        auto const path = run->path;
        Ref<SavedRunsPopup> self = this;
        askName("Rename run", run->name, [self, path](std::string name) {
            auto const result = saved::rename(path, name);
            if (result.isErr()) {
                Notification::create("Could not rename the run: " + result.unwrapErr(), NotificationIcon::Error)->show();
                return;
            }
            // The shown results are this run: keep the name for the replay intro.
            auto& s = Session::get();
            if (s.resultsSaved && s.resultsKey == self->m_key) s.resultsName = name;
            self->changed();
        });
    }

    void onDelete(CCObject* sender) {
        auto const* run = runOf(sender);
        if (!run) return;
        auto const path = run->path;
        Ref<SavedRunsPopup> self = this;
        createQuickPopup("Delete run", fmt::format("Delete <cy>{}</c>? This cannot be undone.", saved::titleOf(*run)),
            "Cancel", "Delete", [self, path](FLAlertLayer*, bool remove) {
                if (!remove) return;
                std::error_code ec;
                std::filesystem::remove(path, ec);
                if (ec) {
                    Notification::create("Could not delete the run: " + ec.message(), NotificationIcon::Error)->show();
                    return;
                }
                self->changed();
            });
    }
};

// One level of the library.
struct LevelEntry {
    std::string key;
    std::string name;
    size_t saved = 0;
    bool lastCount = false;
    int64_t newest = 0;
    std::filesystem::path newestPath;   // the run shown when the level opens
};

std::vector<LevelEntry> libraryLevels() {
    std::vector<LevelEntry> levels;
    for (auto const& key : saved::levelKeys()) {
        auto const runs = saved::list(key);
        auto const last = saved::lastCount(key);
        if (runs.empty() && !last) continue;
        LevelEntry entry;
        entry.key = key;
        entry.saved = runs.size();
        entry.lastCount = last.has_value();
        // The same choice as loadLevelResults: the last count when it is newer.
        auto const& newest = last && (runs.empty() || last->created > runs.front().created) ? *last : runs.front();
        entry.newest = newest.created;
        entry.newestPath = newest.path;
        entry.name = newest.levelName;
        for (auto const& run : runs) {
            if (!entry.name.empty()) break;
            entry.name = run.levelName;
        }
        if (entry.name.empty()) entry.name = levelLabel(key);
        levels.push_back(std::move(entry));
    }
    std::sort(levels.begin(), levels.end(), [](LevelEntry const& a, LevelEntry const& b) { return a.newest > b.newest; });
    return levels;
}

// Every level with counted runs, newest first.
class LibraryPopup : public geode::Popup {
    static constexpr float kWidth = 440.f;
    static constexpr float kHeight = 280.f;
    static constexpr float kRowWidth = 400.f;

    std::vector<LevelEntry> m_levels;
    ScrollLayer* m_list = nullptr;
    CCLabelBMFont* m_summary = nullptr;
    CCLabelBMFont* m_empty = nullptr;

public:
    static LibraryPopup* create() {
        auto* popup = new LibraryPopup();
        if (popup->setup()) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }

protected:
    bool setup() {
        if (!Popup::init(kWidth, kHeight)) return false;
        this->setTitle("TrycsCounter");
        m_summary = CCLabelBMFont::create("", "chatFont.fnt");
        m_summary->setScale(0.6f);
        m_summary->setPosition({kWidth / 2.f, kHeight - 42.f});
        m_mainLayer->addChild(m_summary);
        m_list = createList(m_mainLayer, kWidth, kRowWidth, 190.f, 30.f);
        m_empty = CCLabelBMFont::create("No counted runs yet.\nCount a run in a level: pause > TC.", "goldFont.fnt");
        m_empty->setScale(0.5f);
        m_empty->setAlignment(kCCTextAlignmentCenter);
        m_empty->setPosition({kWidth / 2.f, 125.f});
        m_mainLayer->addChild(m_empty);

        // Settings and the folder with every run, bottom right.
        auto* icons = CCMenu::create();
        icons->setContentSize({70.f, 26.f});
        icons->ignoreAnchorPointForPosition(false);
        icons->setAnchorPoint({1.f, 0.5f});
        icons->setPosition({kWidth - 8.f, 16.f});
        icons->setLayout(RowLayout::create()->setGap(6.f)->setAxisAlignment(AxisAlignment::End));
        for (auto [frame, selector] : {
                 std::pair{"gj_folderBtn_001.png", menu_selector(LibraryPopup::onFolder)},
                 std::pair{"GJ_optionsBtn_001.png", menu_selector(LibraryPopup::onSettings)},
             }) {
            auto* sprite = CCSprite::createWithSpriteFrameName(frame);
            if (!sprite) continue;
            sprite->setScale(24.f / std::max(1.f, sprite->getContentHeight()));
            icons->addChild(CCMenuItemSpriteExtra::create(sprite, this, selector));
        }
        icons->updateLayout();
        m_mainLayer->addChild(icons);
        this->refresh();
        return true;
    }

    void refresh() {
        m_levels = libraryLevels();
        size_t saved = 0;
        for (auto const& level : m_levels) saved += level.saved;
        m_summary->setString(fmt::format("{} level{}, {} saved run{}", m_levels.size(), m_levels.size() == 1 ? "" : "s",
            saved, saved == 1 ? "" : "s").c_str());
        auto* content = m_list->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);
        m_empty->setVisible(m_levels.empty());
        for (size_t i = 0; i < m_levels.size(); ++i) {
            auto const& level = m_levels[i];
            std::string detail = fmt::format("{}  |  {} saved run{}{}  |  {}", levelLabel(level.key), level.saved,
                level.saved == 1 ? "" : "s", level.lastCount ? " + last count" : "", saved::formatTime(level.newest));
            auto* menu = listRow(content, kRowWidth, level.name, detail);
            rowButton(menu, "Runs", "GJ_button_02.png", this, menu_selector(LibraryPopup::onRuns), static_cast<int>(i));
            rowButton(menu, "Play", "GJ_button_01.png", this, menu_selector(LibraryPopup::onPlay), static_cast<int>(i));
            menu->updateLayout();
        }
        content->updateLayout();
        m_list->scrollToTop();
    }

    LevelEntry const* levelOf(CCObject* sender) const {
        auto const index = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        return index < m_levels.size() ? &m_levels[index] : nullptr;
    }

    void onRuns(CCObject* sender) {
        auto const* level = levelOf(sender);
        if (!level) return;
        Ref<LibraryPopup> self = this;
        if (auto* popup = SavedRunsPopup::create(level->key, level->name, [self] { self->refresh(); })) popup->show();
    }

    // Replays the run the level shows when it opens.
    void onPlay(CCObject* sender) {
        if (auto const* level = levelOf(sender)) playRun(level->key, level->newestPath);
    }

    void onFolder(CCObject*) {
        auto const dir = Mod::get()->getSaveDir() / "runs";
        (void)utils::file::createDirectoryAll(dir);
        utils::file::openFolder(dir);
    }

    void onSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }
};

} // namespace

void openLibrary() {
    if (auto* popup = LibraryPopup::create()) popup->show();
}

void openSavedRuns(std::string const& key, std::string const& levelName) {
    if (auto* popup = SavedRunsPopup::create(key, levelName)) popup->show();
}

// The level of a run started from the library runs: replay the run.
void startPendingReplay(PlayLayer* play) {
    if (!g_pendingReplay || !play) return;
    if (CCDirector::sharedDirector()->getRunningScene() != play->getParent()) return;
    auto const pending = std::move(*g_pendingReplay);
    g_pendingReplay.reset();
    auto& s = Session::get();
    if (pending.key != s.levelKey || s.drivesInput()) return;
    if (!loadSavedRun(play, pending.path)) return;
    play->pauseGame(false);
    beginReplay(play);
}

} // namespace tc
