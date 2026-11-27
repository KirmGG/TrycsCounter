// Bot macros in the GDR formats counted like a recorded attempt.

#include "App.hpp"
#include "Macro.hpp"
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

namespace tc {

namespace {

// Frame Fixes past this raw tick (about two hours of gameplay) are ignored.
constexpr int kMaxFixTick = 1 << 22;

macro::Node nodeFromJson(matjson::Value const& value) {
    macro::Node node;
    if (value.isBool()) {
        node.kind = macro::Node::Kind::Bool;
        node.b = value.asBool().unwrapOr(false);
    } else if (value.isNumber()) {
        node.kind = macro::Node::Kind::Float;
        node.f = value.asDouble().unwrapOr(0.0);
    } else if (value.isString()) {
        node.kind = macro::Node::Kind::Str;
        node.s = value.asString().unwrapOr("");
    } else if (value.isArray()) {
        node.kind = macro::Node::Kind::Array;
        for (auto const& item : value) node.items.push_back(nodeFromJson(item));
    } else if (value.isObject()) {
        node.kind = macro::Node::Kind::Map;
        for (auto const& [key, item] : value) node.fields.emplace_back(key, nodeFromJson(item));
    }
    return node;
}

// The macro in the file, or an error text.
std::pair<std::optional<macro::Macro>, std::string> readMacro(std::filesystem::path const& path) {
    auto data = utils::file::readBinary(path);
    if (!data) return {std::nullopt, "cannot read the file: " + data.unwrapErr()};
    auto const& bytes = data.unwrap();
    if (bytes.size() >= 3 && std::equal(bytes.begin(), bytes.begin() + 3, "GDR")) return macro::readGdr2(bytes);
    auto const first = std::find_if(bytes.begin(), bytes.end(), [](uint8_t b) { return !std::isspace(b); });
    if (first != bytes.end() && *first == '{') {
        auto json = matjson::parse(std::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
        if (!json) return {std::nullopt, "the JSON of the macro is broken"};
        return macro::macroFromNode(nodeFromJson(json.unwrap()));
    }
    auto node = macro::decodeMsgpack(bytes);
    if (!node) return {std::nullopt, "not a GDR macro (.gdr, .gdr.json and .gdr2 are read)"};
    return macro::macroFromNode(*node);
}

// The inputs of the macro on the level clock.
std::pair<std::optional<RunRecord>, std::string> runFromMacro(PlayLayer* play, macro::Macro const& m) {
    int const rawTps = cfg::rawTps();
    double const perFrame = rawTps / m.framerate;
    if (std::abs(perFrame - std::round(perFrame)) > 1e-6 || perFrame < 1.0) {
        return {std::nullopt, fmt::format("The macro runs at {:.0f} TPS, the count at {} TPS. Set 'Physics TPS' in the "
            "settings to {:.0f}.", m.framerate, cfg::tps(), m.framerate)};
    }
    int const ticks = static_cast<int>(std::lround(perFrame));
    bool const twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;

    RunRecord run;
    std::map<std::pair<int, bool>, bool> held;
    for (auto const& input : m.inputs) {
        bool const player1 = !twoPlayer || !input.player2;
        auto& state = held[{input.button, player1}];
        if (state == input.down) continue;
        state = input.down;
        run.events.push_back({input.frame * ticks, input.down, input.button, player1, 0.f, 0.f, false});
    }
    sortByTick(run.events);

    // Frame Fixes: the path the replay is checked against and kept on.
    for (auto const& fix : m.fixes) {
        int const tick = fix.frame * ticks;
        if (tick < 0 || tick >= kMaxFixTick) continue;
        if (run.trace.size() <= static_cast<size_t>(tick)) run.trace.resize(static_cast<size_t>(tick) + 1);
        auto& state = run.trace[static_cast<size_t>(tick)];
        state.valid = true;
        state.positionsOnly = true;
        state.p1.x = fix.x1;
        state.p1.y = fix.y1;
        state.dual = fix.hasP2;
        state.p2.x = fix.x2;
        state.p2.y = fix.y2;
    }

    run.endTick = std::numeric_limits<int>::max() / 2;
    run.endKind = RunEnd::Completed;
    run.fromMacro = true;
    run.startX = startPosX(play);
    run.openEnd = true;
    return {std::move(run), {}};
}

void startMacro(std::filesystem::path const& path) {
    auto* play = PlayLayer::get();
    if (!play || Session::get().drivesInput()) return;
    auto [m, readError] = readMacro(path);
    if (!m) {
        FLAlertLayer::create("TrycsCounter", "This macro cannot be counted: " + readError + ".", "OK")->show();
        return;
    }
    auto [run, runError] = runFromMacro(play, *m);
    if (!run) {
        FLAlertLayer::create("TrycsCounter", runError, "OK")->show();
        return;
    }
    log::info("Macro {}: {} inputs ({} kept), {:.0f} TPS, level {} '{}', bot '{}', {} Frame Fixes",
        path.filename(), m->inputs.size(), run->events.size(), m->framerate, m->levelID, m->levelName, m->bot,
        m->fixes.size());

    auto start = [run = std::move(*run)]() mutable {
        auto* play = PlayLayer::get();
        if (!play) return;
        auto const error = startAnalysis(play, std::move(run));
        if (!error.empty()) {
            FLAlertLayer::create("TrycsCounter", error, "OK")->show();
            return;
        }
        resumeFromPause();
    };
    int const level = currentLevelID(play);
    if (m->levelID > 0 && level > 0 && m->levelID != level) {
        createQuickPopup("Another level",
            fmt::format("This macro was made for <cy>{}</c> (ID {}), this level has ID {}. Count it here anyway?",
                m->levelName, m->levelID, level),
            "Cancel", "Count", [start = std::move(start)](FLAlertLayer*, bool yes) mutable {
                if (yes) start();
            });
        return;
    }
    start();
}

// Starts the next macro of the queue; files that cannot be counted here are skipped with a notification.
void startNextQueued() {
    auto& s = Session::get();
    auto* play = PlayLayer::get();
    if (!play || s.drivesInput()) return;
    while (!s.macroQueue.empty()) {
        auto const path = s.macroQueue.front();
        s.macroQueue.pop_front();
        auto const name = utils::string::pathToString(path.stem());
        auto skip = [&](std::string const& why) {
            log::warn("Macro queue: {} skipped: {}", name, why);
            Notification::create(fmt::format("{} skipped: {}", name, why), NotificationIcon::Warning)->show();
        };
        auto [m, readError] = readMacro(path);
        if (!m) {
            skip(readError);
            continue;
        }
        int const level = currentLevelID(play);
        if (m->levelID > 0 && level > 0 && m->levelID != level) {
            skip(fmt::format("made for {} (ID {})", m->levelName, m->levelID));
            continue;
        }
        auto [run, runError] = runFromMacro(play, *m);
        if (!run) {
            skip(runError);
            continue;
        }
        s.queuedMacro = name;
        if (auto const error = startAnalysis(play, std::move(*run)); !error.empty()) {
            s.queuedMacro.clear();
            skip(error);
            continue;
        }
        log::info("Macro queue: counting {}, {} left after it", name, s.macroQueue.size());
        resumeFromPause();
        return;
    }
}

} // namespace

// A count of the macro queue ended.
bool queuedCountFinished(PlayLayer* play, bool counted, std::string const& reason) {
    auto& s = Session::get();
    if (s.queuedMacro.empty()) return false;
    auto const name = std::exchange(s.queuedMacro, {});
    if (counted) {
        saveCurrentRun(play, name);
    } else {
        log::warn("Macro queue: {} could not be counted: {}", name, reason);
        Notification::create(fmt::format("{} could not be counted", name), NotificationIcon::Error)->show();
    }
    if (s.macroQueue.empty()) return false;
    Loader::get()->queueInMainThread([] { startNextQueued(); });
    return true;
}

void pickMacro() {
    utils::file::FilePickOptions options;
    if (auto* xdbot = Loader::get()->getLoadedMod("zilko.xdbot")) {
        auto const folder = xdbot->getSettingValue<std::filesystem::path>("macros_folder");
        if (!folder.empty()) options.defaultPath = folder;
    }
    options.filters.push_back({"GDR macros", {"*.gdr", "*.json", "*.gdr2"}});
    static std::optional<arc::TaskHandle<void>> task;
    // Several files: counted one after another, each saved under its name.
    task = async::spawn(utils::file::pickMany(std::move(options)), [](utils::file::PickManyResult result) {
        if (!result) {
            FLAlertLayer::create("TrycsCounter", "The file dialog failed: " + result.unwrapErr(), "OK")->show();
            return;
        }
        auto const& paths = result.unwrap();
        if (paths.size() == 1) {
            startMacro(paths.front());
        } else if (!paths.empty()) {
            Session::get().macroQueue.assign(paths.begin(), paths.end());
            Notification::create(fmt::format("Counting {} macros one after another", paths.size()),
                NotificationIcon::Info)->show();
            startNextQueued();
        }
    });
}

} // namespace tc
