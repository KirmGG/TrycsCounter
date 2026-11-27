#pragma once
// Counted runs saved to disk.

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include "Session.hpp"
#include "WindowMath.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tc::saved {

inline constexpr int kFormatVersion = 1;

// FNV-1a: stable across builds and game restarts (std::hash is not guaranteed to be).
inline uint64_t fnv1a(std::string_view data) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

// Online levels by their ID.
inline std::string levelKeyOf(GJGameLevel* level) {
    if (!level) return "unknown";
    int const id = static_cast<int>(level->m_levelID.value());
    if (id > 0) return std::to_string(id);
    std::string const name = level->m_levelName;
    std::string const data = level->m_levelString;
    return fmt::format("local-{:016x}", fnv1a(name + '\n' + data));
}

inline std::string levelKeyOf(PlayLayer* play) {
    return levelKeyOf(play ? play->m_level : nullptr);
}

inline std::string levelNameOf(PlayLayer* play) {
    if (!play || !play->m_level) return {};
    return std::string(play->m_level->m_levelName);
}

inline std::filesystem::path runsDir(std::string const& key) {
    return geode::Mod::get()->getSaveDir() / "runs" / key;
}

// Everything a saved run holds.
struct SavedRun {
    RunRecord run;
    std::vector<WindowResult> windows;
    int baseOffset = 0;
    size_t alignmentIndex = 0;
    int units = 1;
    int stepTicks = 2;
    bool precise = false;
    std::string levelName;
    float levelLength = 0.f;     // x of the level end, 0: not known (older files)
    std::string summary;
    std::string name;            // set by the player, empty: the date is shown
    int64_t created = 0;
    int mismatchTick = -1;       // the replay left the attempt here (-1: never)
    std::vector<char> dependent; // per input: "~", depends on the previous click
    std::vector<char> uncertain; // per input: the edge check disagreed ("?")
    std::vector<char> otherMarker; // per input: a dual's marker is on the other player
    bool deepCounted = false;
    Hitborders borders;
    // Deep Count, per input: smallest / largest window and whether the click only worked when re-timed.
    std::vector<DeepRange> deep;
};

struct SavedRunInfo {
    std::filesystem::path path;
    int64_t created = 0;
    std::string levelName;
    std::string name;
    std::string summary;
    bool deep = false;
    size_t inputs = 0;
    bool completed = false;
};

inline char const* endName(RunEnd end) {
    switch (end) {
        case RunEnd::Completed: return "completed";
        case RunEnd::Paused: return "paused";
        case RunEnd::Death: return "death";
    }
    return "death";
}

inline RunEnd endFromName(std::string const& name) {
    if (name == "completed") return RunEnd::Completed;
    if (name == "paused") return RunEnd::Paused;
    return RunEnd::Death;
}

// A path as one flat list: tick, x, y, tick, x, y, ... (positions to 0.1).
inline matjson::Value pathToJson(std::vector<PathPoint> const& path) {
    auto list = matjson::Value::array();
    auto round = [](float v) { return std::round(static_cast<double>(v) * 10.0) / 10.0; };
    for (auto const& p : path) {
        list.push(p.tick);
        list.push(round(p.x));
        list.push(round(p.y));
    }
    return list;
}

inline std::vector<PathPoint> pathFromJson(matjson::Value const& value) {
    std::vector<PathPoint> path;
    auto list = value.asArray();
    if (list.isErr()) return path;
    auto const& v = list.unwrap();
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        path.push_back({v[i].as<int>().unwrapOr(0), static_cast<float>(v[i + 1].asDouble().unwrapOr(0.0)),
            static_cast<float>(v[i + 2].asDouble().unwrapOr(0.0))});
    }
    return path;
}

inline matjson::Value bordersToJson(Hitborders const& b) {
    auto root = matjson::Value::object();
    root["path1"] = pathToJson(b.path1);
    root["path2"] = pathToJson(b.path2);
    // input, then early and late path of player 1 and of player 2
    auto clicks = matjson::Value::array();
    for (auto const& [index, sides] : b.clicks) {
        clicks.push(std::vector<matjson::Value>{static_cast<int>(index), pathToJson(sides[0]), pathToJson(sides[1]),
            pathToJson(sides[2]), pathToJson(sides[3])});
    }
    root["clicks"] = std::move(clicks);
    return root;
}

inline Hitborders bordersFromJson(matjson::Value const& root) {
    Hitborders b;
    b.path1 = pathFromJson(root["path1"]);
    b.path2 = pathFromJson(root["path2"]);
    if (auto list = root["clicks"].asArray(); list.isOk()) {
        for (auto const& item : list.unwrap()) {
            auto f = item.asArray();
            if (f.isErr() || f.unwrap().size() < 3) continue;
            auto const& v = f.unwrap();
            int const index = v[0].as<int>().unwrapOr(-1);
            if (index < 0) continue;
            auto& sides = b.clicks[static_cast<size_t>(index)];
            for (size_t i = 0; i < 4 && i + 1 < v.size(); ++i) sides[i] = pathFromJson(v[i + 1]);
        }
    }
    return b;
}

inline matjson::Value toJson(SavedRun const& data) {
    auto root = matjson::Value::object();
    root["version"] = kFormatVersion;
    root["created"] = data.created;
    root["levelName"] = data.levelName;
    root["levelLength"] = static_cast<double>(data.levelLength);
    root["summary"] = data.summary;
    root["endTick"] = data.run.endTick;
    root["endKind"] = endName(data.run.endKind);
    root["startX"] = static_cast<double>(data.run.startX);
    root["baseOffset"] = data.baseOffset;
    root["alignmentIndex"] = static_cast<int>(data.alignmentIndex);
    root["units"] = data.units;
    root["stepTicks"] = data.stepTicks;
    root["precise"] = data.precise;
    root["name"] = data.name;
    root["mismatchTick"] = data.mismatchTick;

    // tick, down, button, player1, x, y, betweenSteps, cbf, frac, then for a dual played by one player.
    auto events = matjson::Value::array();
    for (auto const& e : data.run.events) {
        std::vector<matjson::Value> f{e.tick, e.down, e.button, e.player1, static_cast<double>(e.x),
            static_cast<double>(e.y), e.betweenSteps, e.cbf, e.frac};
        if (e.dual) {
            f.push_back(e.otherP2);
            f.push_back(static_cast<double>(e.ox));
            f.push_back(static_cast<double>(e.oy));
        }
        events.push(std::move(f));
    }
    root["events"] = std::move(events);

    // early, late, step, unitsPerTick, earlyCapped, lateCapped, valid, exceeded
    auto windows = matjson::Value::array();
    for (auto const& w : data.windows) {
        windows.push(std::vector<matjson::Value>{w.earlyTicks, w.lateTicks, w.stepTicks, w.unitsPerTick, w.earlyCapped,
            w.lateCapped, w.valid, w.exceeded});
    }
    root["windows"] = std::move(windows);

    auto steps = matjson::Value::array();
    for (auto const& d : data.run.stepDts)
        steps.push(std::vector<matjson::Value>{d.tick, static_cast<double>(d.p1), static_cast<double>(d.p2)});
    root["stepDts"] = std::move(steps);

    auto hits = matjson::Value::array();
    for (int hit : data.run.hits) hits.push(hit);
    root["hits"] = std::move(hits);

    // tick, player 2, from x, from y, from vy, to x, to y, to vy
    auto fixes = matjson::Value::array();
    for (auto const& f : data.run.fixes) {
        fixes.push(std::vector<matjson::Value>{f.tick, f.player2, static_cast<double>(f.fromX), static_cast<double>(f.fromY),
            f.fromVy, static_cast<double>(f.toX), static_cast<double>(f.toY), f.toVy});
    }
    root["fixes"] = std::move(fixes);

    auto dependent = matjson::Value::array();
    for (size_t i = 0; i < data.dependent.size(); ++i) {
        if (data.dependent[i]) dependent.push(static_cast<int>(i));
    }
    root["dependent"] = std::move(dependent);
    auto uncertain = matjson::Value::array();
    for (size_t i = 0; i < data.uncertain.size(); ++i) {
        if (data.uncertain[i]) uncertain.push(static_cast<int>(i));
    }
    root["uncertain"] = std::move(uncertain);
    auto other = matjson::Value::array();
    for (size_t i = 0; i < data.otherMarker.size(); ++i) {
        if (data.otherMarker[i]) other.push(static_cast<int>(i));
    }
    root["otherMarker"] = std::move(other);

    // index, smallest, largest, re-timed, step, [from, to, from, to, ...] of a disconnected window
    auto deep = matjson::Value::array();
    for (size_t i = 0; i < data.deep.size(); ++i) {
        auto const& d = data.deep[i];
        if (!d.ranged() && !d.broken && !d.disconnected()) continue;
        std::vector<matjson::Value> entry{static_cast<int>(i), d.lo, d.hi, d.broken};
        if (d.disconnected()) {
            auto pieces = matjson::Value::array();
            for (auto const& span : d.spans) {
                pieces.push(span.from);
                pieces.push(span.to);
            }
            entry.push_back(d.step);
            entry.push_back(std::move(pieces));
        }
        deep.push(std::move(entry));
    }
    root["deep"] = std::move(deep);
    root["deepCounted"] = data.deepCounted;
    if (!data.borders.empty()) root["borders"] = bordersToJson(data.borders);
    // index, usual window of a click with a range
    auto typical = matjson::Value::array();
    for (size_t i = 0; i < data.deep.size(); ++i) {
        if (data.deep[i].typical > 0.0) typical.push(std::vector<matjson::Value>{static_cast<int>(i), data.deep[i].typical});
    }
    root["typical"] = std::move(typical);
    // index, easier window, how
    auto easier = matjson::Value::array();
    for (size_t i = 0; i < data.deep.size(); ++i) {
        if (data.deep[i].easier > 0.0)
            easier.push(std::vector<matjson::Value>{static_cast<int>(i), data.deep[i].easier, data.deep[i].easierHow});
    }
    root["easier"] = std::move(easier);
    return root;
}

inline geode::Result<SavedRun> fromJson(matjson::Value const& root) {
    SavedRun data;
    GEODE_UNWRAP_INTO(int version, root["version"].as<int>());
    if (version != kFormatVersion) return geode::Err(fmt::format("unknown file version {}", version));
    data.created = root["created"].as<int64_t>().unwrapOr(0);
    data.levelName = root["levelName"].asString().unwrapOr("");
    data.levelLength = static_cast<float>(root["levelLength"].asDouble().unwrapOr(0.0));
    data.summary = root["summary"].asString().unwrapOr("");
    GEODE_UNWRAP_INTO(data.run.endTick, root["endTick"].as<int>());
    data.run.endKind = endFromName(root["endKind"].asString().unwrapOr("death"));
    data.run.startX = static_cast<float>(root["startX"].asDouble().unwrapOr(-1.0));
    data.baseOffset = root["baseOffset"].as<int>().unwrapOr(0);
    data.alignmentIndex = static_cast<size_t>(std::clamp(root["alignmentIndex"].as<int>().unwrapOr(0), 0,
        static_cast<int>(kAlignmentCandidates.size()) - 1));
    data.units = std::max(1, root["units"].as<int>().unwrapOr(1));
    data.stepTicks = std::max(1, root["stepTicks"].as<int>().unwrapOr(2));
    data.precise = root["precise"].asBool().unwrapOr(false);
    data.name = root["name"].asString().unwrapOr("");
    data.mismatchTick = root["mismatchTick"].as<int>().unwrapOr(-1);

    GEODE_UNWRAP_INTO(auto const& events, root["events"].asArray());
    for (auto const& item : events) {
        GEODE_UNWRAP_INTO(auto const& f, item.asArray());
        if (f.size() < 9) return geode::Err("bad input entry");
        InputEvent e;
        GEODE_UNWRAP_INTO(e.tick, f[0].as<int>());
        GEODE_UNWRAP_INTO(e.down, f[1].asBool());
        GEODE_UNWRAP_INTO(e.button, f[2].as<int>());
        GEODE_UNWRAP_INTO(e.player1, f[3].asBool());
        e.x = static_cast<float>(f[4].asDouble().unwrapOr(0.0));
        e.y = static_cast<float>(f[5].asDouble().unwrapOr(0.0));
        e.betweenSteps = f[6].asBool().unwrapOr(false);
        e.cbf = f[7].asBool().unwrapOr(false);
        e.frac = f[8].asDouble().unwrapOr(0.0);
        if (f.size() >= 12) {
            e.dual = true;
            e.otherP2 = f[9].asBool().unwrapOr(false);
            e.ox = static_cast<float>(f[10].asDouble().unwrapOr(0.0));
            e.oy = static_cast<float>(f[11].asDouble().unwrapOr(0.0));
        }
        data.run.events.push_back(e);
    }

    GEODE_UNWRAP_INTO(auto const& windows, root["windows"].asArray());
    for (auto const& item : windows) {
        GEODE_UNWRAP_INTO(auto const& f, item.asArray());
        if (f.size() < 8) return geode::Err("bad window entry");
        WindowResult w;
        GEODE_UNWRAP_INTO(w.earlyTicks, f[0].as<int>());
        GEODE_UNWRAP_INTO(w.lateTicks, f[1].as<int>());
        GEODE_UNWRAP_INTO(w.stepTicks, f[2].as<int>());
        GEODE_UNWRAP_INTO(w.unitsPerTick, f[3].as<int>());
        w.earlyCapped = f[4].asBool().unwrapOr(false);
        w.lateCapped = f[5].asBool().unwrapOr(false);
        w.valid = f[6].asBool().unwrapOr(false);
        w.exceeded = f[7].asBool().unwrapOr(false);
        data.windows.push_back(w);
    }
    if (data.windows.size() != data.run.events.size()) return geode::Err("inputs and windows do not match");

    if (root.contains("stepDts")) {
        GEODE_UNWRAP_INTO(auto const& steps, root["stepDts"].asArray());
        for (auto const& item : steps) {
            GEODE_UNWRAP_INTO(auto const& f, item.asArray());
            if (f.size() < 3) continue;
            data.run.stepDts.push_back({f[0].as<int>().unwrapOr(0), static_cast<float>(f[1].asDouble().unwrapOr(0.0)),
                static_cast<float>(f[2].asDouble().unwrapOr(0.0))});
        }
    }
    if (root.contains("hits")) {
        GEODE_UNWRAP_INTO(auto const& hits, root["hits"].asArray());
        for (auto const& hit : hits) data.run.hits.push_back(hit.as<int>().unwrapOr(0));
    }
    if (data.run.events.empty()) return geode::Err("the run has no inputs");

    if (auto list = root["fixes"].asArray(); list.isOk()) {
        for (auto const& item : list.unwrap()) {
            auto f = item.asArray();
            if (f.isErr() || f.unwrap().size() < 8) continue;
            auto const& v = f.unwrap();
            StateFix fix;
            fix.tick = v[0].as<int>().unwrapOr(0);
            fix.player2 = v[1].asBool().unwrapOr(false);
            fix.fromX = static_cast<float>(v[2].asDouble().unwrapOr(0.0));
            fix.fromY = static_cast<float>(v[3].asDouble().unwrapOr(0.0));
            fix.fromVy = v[4].asDouble().unwrapOr(0.0);
            fix.toX = static_cast<float>(v[5].asDouble().unwrapOr(0.0));
            fix.toY = static_cast<float>(v[6].asDouble().unwrapOr(0.0));
            fix.toVy = v[7].asDouble().unwrapOr(0.0);
            data.run.fixes.push_back(fix);
        }
    }

    data.dependent.assign(data.run.events.size(), 0);
    data.uncertain.assign(data.run.events.size(), 0);
    data.otherMarker.assign(data.run.events.size(), 0);
    if (auto list = root["otherMarker"].asArray(); list.isOk()) {
        for (auto const& item : list.unwrap()) {
            int const i = item.as<int>().unwrapOr(-1);
            if (i >= 0 && static_cast<size_t>(i) < data.otherMarker.size()) data.otherMarker[static_cast<size_t>(i)] = 1;
        }
    }
    data.deep.assign(data.run.events.size(), DeepRange{});
    if (auto list = root["uncertain"].asArray(); list.isOk()) {
        for (auto const& item : list.unwrap()) {
            int const i = item.as<int>().unwrapOr(-1);
            if (i >= 0 && static_cast<size_t>(i) < data.uncertain.size()) data.uncertain[static_cast<size_t>(i)] = 1;
        }
    }
    if (auto list = root["dependent"].asArray(); list.isOk()) {
        for (auto const& item : list.unwrap()) {
            int const i = item.as<int>().unwrapOr(-1);
            if (i >= 0 && static_cast<size_t>(i) < data.dependent.size()) data.dependent[static_cast<size_t>(i)] = 1;
        }
    }
    if (auto list = root["easier"].asArray(); list.isOk()) {
        for (auto const& item : list.unwrap()) {
            auto f = item.asArray();
            if (f.isErr() || f.unwrap().size() < 3) continue;
            auto const& v = f.unwrap();
            int const i = v[0].as<int>().unwrapOr(-1);
            if (i < 0 || static_cast<size_t>(i) >= data.deep.size()) continue;
            data.deep[static_cast<size_t>(i)].easier = v[1].asDouble().unwrapOr(0.0);
            data.deep[static_cast<size_t>(i)].easierHow = v[2].asString().unwrapOr("");
        }
    }
    if (auto list = root["typical"].asArray(); list.isOk()) {
        for (auto const& item : list.unwrap()) {
            auto f = item.asArray();
            if (f.isErr() || f.unwrap().size() < 2) continue;
            int const i = f.unwrap()[0].as<int>().unwrapOr(-1);
            if (i >= 0 && static_cast<size_t>(i) < data.deep.size())
                data.deep[static_cast<size_t>(i)].typical = f.unwrap()[1].asDouble().unwrapOr(0.0);
        }
    }
    data.deepCounted = root["deepCounted"].asBool().unwrapOr(false);
    if (root.contains("borders")) data.borders = bordersFromJson(root["borders"]);
    if (auto list = root["deep"].asArray(); list.isOk()) {
        if (!list.unwrap().empty()) data.deepCounted = true;
        for (auto const& item : list.unwrap()) {
            auto f = item.asArray();
            if (f.isErr() || f.unwrap().size() < 4) continue;
            auto const& v = f.unwrap();
            int const i = v[0].as<int>().unwrapOr(-1);
            if (i < 0 || static_cast<size_t>(i) >= data.deep.size()) continue;
            auto& d = data.deep[static_cast<size_t>(i)];
            d.lo = v[1].asDouble().unwrapOr(0.0);
            d.hi = v[2].asDouble().unwrapOr(0.0);
            d.broken = v[3].asBool().unwrapOr(false);
            if (v.size() >= 6) {
                d.step = v[4].asDouble().unwrapOr(0.0);
                if (auto pieces = v[5].asArray(); pieces.isOk()) {
                    auto const& p = pieces.unwrap();
                    for (size_t j = 0; j + 1 < p.size(); j += 2)
                        d.spans.push_back({p[j].asDouble().unwrapOr(0.0), p[j + 1].asDouble().unwrapOr(0.0)});
                }
            }
        }
    }
    return geode::Ok(std::move(data));
}

inline int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// `unix` is a macro on Android and iOS, hence `seconds`.
inline std::string formatTime(int64_t seconds) {
    std::time_t const t = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M", &local);
    return buffer;
}

// The last count of a level, kept even when it was not saved.
inline std::filesystem::path lastCountPath(std::string const& key) {
    return runsDir(key) / "last-count.autosave";
}

inline geode::Result<> saveLastCount(std::string const& key, SavedRun data) {
    GEODE_UNWRAP(geode::utils::file::createDirectoryAll(runsDir(key)));
    if (data.created == 0) data.created = nowUnix();
    return geode::utils::file::writeStringSafe(lastCountPath(key), toJson(data).dump(matjson::NO_INDENTATION));
}

// Time the last count was made, 0 when there is none.
inline int64_t lastCountTime(std::string const& key) {
    auto text = geode::utils::file::readString(lastCountPath(key));
    if (text.isErr()) return 0;
    auto parsed = matjson::parse(text.unwrap());
    return parsed.isOk() ? parsed.unwrap()["created"].as<int64_t>().unwrapOr(0) : 0;
}

inline geode::Result<std::filesystem::path> save(std::string const& key, SavedRun data) {
    auto const dir = runsDir(key);
    GEODE_UNWRAP(geode::utils::file::createDirectoryAll(dir));
    if (data.created == 0) data.created = nowUnix();
    auto path = dir / fmt::format("{}.json", data.created);
    // Two saves within one second keep both files.
    for (int n = 2; std::filesystem::exists(path); ++n) path = dir / fmt::format("{}-{}.json", data.created, n);
    GEODE_UNWRAP(geode::utils::file::writeStringSafe(path, toJson(data).dump(matjson::NO_INDENTATION)));
    return geode::Ok(path);
}

inline geode::Result<SavedRun> load(std::filesystem::path const& path) {
    GEODE_UNWRAP_INTO(auto text, geode::utils::file::readString(path));
    auto parsed = matjson::parse(text);
    if (parsed.isErr()) return geode::Err(fmt::format("not valid JSON: {}", parsed.unwrapErr().message));
    return fromJson(parsed.unwrap());
}

// Gives a saved run a new name (empty: show its date again).
inline geode::Result<> rename(std::filesystem::path const& path, std::string const& name) {
    GEODE_UNWRAP_INTO(auto text, geode::utils::file::readString(path));
    auto parsed = matjson::parse(text);
    if (parsed.isErr()) return geode::Err(fmt::format("not valid JSON: {}", parsed.unwrapErr().message));
    auto root = parsed.unwrap();
    root["name"] = name;
    GEODE_UNWRAP(geode::utils::file::writeStringSafe(path, root.dump(matjson::NO_INDENTATION)));
    return geode::Ok();
}

// Title of a saved run: its name, or the time it was saved.
inline std::string titleOf(SavedRunInfo const& info) {
    return info.name.empty() ? formatTime(info.created) : info.name;
}

// The small header fields of a run file.
inline std::optional<SavedRunInfo> infoOf(std::filesystem::path const& path) {
    auto text = geode::utils::file::readString(path);
    if (text.isErr()) return std::nullopt;
    auto parsed = matjson::parse(text.unwrap());
    if (parsed.isErr()) return std::nullopt;
    auto const root = parsed.unwrap();
    SavedRunInfo info;
    info.path = path;
    info.created = root["created"].as<int64_t>().unwrapOr(0);
    info.levelName = root["levelName"].asString().unwrapOr("");
    info.summary = root["summary"].asString().unwrapOr("");
    info.name = root["name"].asString().unwrapOr("");
    if (auto deep = root["deep"].asArray(); deep.isOk()) info.deep = !deep.unwrap().empty();
    info.completed = root["endKind"].asString().unwrapOr("") == "completed";
    if (auto events = root["events"].asArray(); events.isOk()) info.inputs = events.unwrap().size();
    return info;
}

// Saved runs of a level, newest first.
inline std::vector<SavedRunInfo> list(std::string const& key) {
    std::vector<SavedRunInfo> runs;
    auto const dir = runsDir(key);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return runs;
    auto files = geode::utils::file::readDirectory(dir);
    if (files.isErr()) return runs;
    for (auto const& path : files.unwrap()) {
        if (path.extension() != ".json") continue;
        if (auto info = infoOf(path)) runs.push_back(std::move(*info));
    }
    std::sort(runs.begin(), runs.end(), [](SavedRunInfo const& a, SavedRunInfo const& b) { return a.created > b.created; });
    return runs;
}

// The last count of a level, if there is one.
inline std::optional<SavedRunInfo> lastCount(std::string const& key) {
    std::error_code ec;
    if (!std::filesystem::exists(lastCountPath(key), ec)) return std::nullopt;
    return infoOf(lastCountPath(key));
}

// Keys of every level that has saved runs or a last count.
inline std::vector<std::string> levelKeys() {
    std::vector<std::string> keys;
    std::error_code ec;
    auto const root = geode::Mod::get()->getSaveDir() / "runs";
    if (!std::filesystem::is_directory(root, ec)) return keys;
    for (auto const& entry : std::filesystem::directory_iterator(root, ec)) {
        if (entry.is_directory(ec)) keys.push_back(geode::utils::string::pathToString(entry.path().filename()));
    }
    return keys;
}

} // namespace tc::saved
