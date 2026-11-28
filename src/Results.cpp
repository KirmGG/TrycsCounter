// Counted runs kept on disk: saving, loading and naming.

#include "App.hpp"
#include <Geode/utils/file.hpp>

namespace tc {

// The results in memory belong to the level that is being played.
bool resultsForLevel(PlayLayer* play) {
    auto const& s = Session::get();
    return play && s.hasResults && !s.source.empty() && s.resultsKey == s.levelKey;
}

namespace {

std::string resultsSummary(Session const& s) {
    int shown = 0;
    int oneFrame = 0;
    for (size_t i = 0; i < s.windows.size() && i < s.source.events.size(); ++i) {
        auto const& result = s.windows[i];
        if (!overlay::isShown(s.source.events[i], result)) continue;
        ++shown;
        if (cfg::frames(result) <= 1.0) ++oneFrame;
    }
    return fmt::format("{} inputs, {} shown, {} of 1 frame{}", s.source.events.size(), shown, oneFrame,
        s.source.endKind == RunEnd::Completed ? ", completed" : "");
}

} // namespace

namespace {

saved::SavedRun currentRunData(PlayLayer* play, std::string const& name) {
    auto const& s = Session::get();
    saved::SavedRun data;
    data.run = s.source;
    data.run.trace.clear();   // only needed while counting
    data.windows = s.windows;
    data.baseOffset = s.baseOffset;
    data.alignmentIndex = s.alignmentIndex;
    data.units = s.units;
    data.stepTicks = s.stepTicks;
    data.precise = s.precise;
    data.levelName = saved::levelNameOf(play);
    data.levelLength = play ? play->m_levelLength : 0.f;
    data.summary = resultsSummary(s);
    data.name = name;
    data.mismatchTick = s.resultsMismatchTick;
    data.dependent = s.dependent;
    data.uncertain = s.edgeUncertain;
    data.otherMarker = s.otherMarker;
    data.deepCounted = s.deepCounted;
    data.borders = s.borders;
    data.deep = s.deep;
    return data;
}

} // namespace

bool saveCurrentRun(PlayLayer* play, std::string const& name) {
    auto& s = Session::get();
    if (!resultsForLevel(play)) return false;
    auto const result = saved::save(s.levelKey, currentRunData(play, name));
    if (result.isErr()) {
        log::error("Could not save the run: {}", result.unwrapErr());
        Notification::create("Could not save the run: " + result.unwrapErr(), NotificationIcon::Error)->show();
        return false;
    }
    log::info("Saved the counted run to {}", utils::string::pathToString(result.unwrap()));
    saved::removeLastCount(s.levelKey);
    s.resultsSaved = true;
    s.resultsName = name;
    Notification::create("Run saved for this level", NotificationIcon::Success)->show();
    return true;
}

// Keeps the finished count on disk, so quitting the level does not lose it.
void saveLastCount(PlayLayer* play) {
    auto const& s = Session::get();
    if (!resultsForLevel(play)) return;
    if (auto result = saved::saveLastCount(s.levelKey, currentRunData(play, s.resultsName)); result.isErr())
        log::error("Could not keep the last count: {}", result.unwrapErr());
}

// Forgets the counted run of the level; saved runs stay.
void clearResults(PlayLayer* play) {
    auto& s = Session::get();
    if (!s.resultsSaved) saved::removeLastCount(s.levelKey);
    s.hasResults = false;
    s.windows.clear();
    s.dependent.clear();
    s.edgeUncertain.clear();
    s.deep.clear();
    s.borders = {};
    s.resultsKey.clear();
    s.resultsName.clear();
    ++s.lstarGeneration;
    s.lstarReady = false;
    refreshDisplay(play);
}

// The results shown when a level opens.
void loadLevelResults(PlayLayer* play) {
    auto& s = Session::get();
    auto const runs = saved::list(s.levelKey);
    int64_t const last = saved::lastCountTime(s.levelKey);
    if (last > 0 && (runs.empty() || last > runs.front().created)) {
        if (loadSavedRun(play, saved::lastCountPath(s.levelKey), true)) {
            s.resultsSaved = false;
            s.resultsName.clear();
            return;
        }
    }
    if (!runs.empty()) loadSavedRun(play, runs.front().path, true);
}

namespace {

// Makes a saved run the current results of the level.
void applySavedRun(PlayLayer* play, saved::SavedRun data) {
    auto& s = Session::get();
    s.source = std::move(data.run);
    s.sourceTrace.clear();
    s.windows = std::move(data.windows);
    s.baseOffset = data.baseOffset;
    s.alignmentIndex = data.alignmentIndex;
    s.units = data.units;
    s.stepTicks = data.stepTicks;
    s.precise = data.precise;
    s.hasResults = true;
    s.resultsKey = s.levelKey;
    s.resultsLevelID = currentLevelID(play);
    s.resultsSaved = true;
    s.resultsName = data.name;
    s.resultsMismatchTick = data.mismatchTick;
    s.dependent = std::move(data.dependent);
    s.edgeUncertain = std::move(data.uncertain);
    s.otherMarker = std::move(data.otherMarker);
    s.deepCounted = data.deepCounted;
    s.borders = std::move(data.borders);
    s.deep = std::move(data.deep);
    s.exactTrials.clear();
    s.dependent.resize(s.source.events.size(), 0);
    s.edgeUncertain.resize(s.source.events.size(), 0);
    s.otherMarker.resize(s.source.events.size(), 0);
    s.deep.resize(s.source.events.size(), DeepRange{});
    s.sourceSwift = swiftLevelsOf(s.source.events);
    resetDisplay(play);
    computeLStar();
    refreshDisplay(play);
}

} // namespace

bool loadSavedRun(PlayLayer* play, std::filesystem::path const& path, bool quiet) {
    auto loaded = saved::load(path);
    if (loaded.isErr()) {
        log::error("Could not load saved run {}: {}", utils::string::pathToString(path), loaded.unwrapErr());
        if (!quiet) Notification::create("Could not load the run: " + loaded.unwrapErr(), NotificationIcon::Error)->show();
        return false;
    }
    applySavedRun(play, std::move(loaded).unwrap());
    log::info("Loaded saved run {}", utils::string::pathToString(path));
    return true;
}

size_t savedRunCount(std::string const& key) {
    std::error_code ec;
    auto const dir = saved::runsDir(key);
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    size_t count = 0;
    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") ++count;
    }
    return count;
}

} // namespace tc
