// Results in the level: markers passed, the counter, marks, notes and L*.

#include "App.hpp"

namespace tc {

namespace {

// A marker is announced with a ding only if the player has just reached it (raw ticks).
constexpr int kDingMaxLagTicks = 8;

} // namespace

// Shows the markers of the inputs the player has just reached, announces them with the ding.
void revealPassedInputs(PlayLayer* play) {
    auto& s = Session::get();
    if (!s.hasResults || s.isAnalyzing() || s.resultsKey != s.levelKey) return;
    if (liveMode() || !play->m_player1) return;

    int const tick = currentTick(play);
    bool changed = false;
    auto const& events = s.source.events;
    while (s.displayCursor < events.size() && s.displayCursor < s.windows.size()) {
        size_t const index = s.displayCursor;
        auto const& event = events[index];
        int const target = injectTick(s, event);
        if (target > tick) break;
        // A dead player reaches nothing new (the respawn recounts).
        if (play->m_player1->m_isDead && target > s.displaySilentUntil) break;

        auto const& result = s.windows[index];
        ++s.displayCursor;
        if (!overlay::isShown(event, result)) continue;

        bool const silent = target <= s.displaySilentUntil || s.trainingFastForward();
        double const frames = shownWindow(s, index);
        bool const ranged = index < s.deep.size() && s.deep[index].ranged();
        bool const capped = result.capped() && !ranged;
        // The replay of the hitborders counts and dings, but shows no rings.
        if (!s.borderReplay) {
            overlay::showMarkerText(play, markerEvent(s, index), frames, capped,
                cfg::windowLabel(frames, cfg::isPrecise(result), capped) + resultMarks(s, index));
        }
        size_t const bucket = overlay::bucketOf(frames);
        ++s.visibleCounts[bucket];
        s.lastShownMs = windowMs(frames, cfg::windowFps());
        if (index < s.lstar.size() && !std::isnan(s.lstar[index])) s.lastShownLStar = s.lstar[index];
        if (!silent) {
            overlay::pulseBucket(bucket);
            overlay::flashLastClick();
            if (tick - target <= kDingMaxLagTicks) overlay::playDing(frames);
            if (cfg::flag("show-notes") && !s.borderReplay) {
                auto const notes = deepNotes(s, index);
                if (!notes.empty()) overlay::showNote(play, notes, cfg::isPrecise(result));
            }
        }
        changed = true;
    }
    if (changed) overlay::updateCounterLabels();
}

void resetDisplay(PlayLayer* play) {
    Session::get().resetDisplay();
    overlay::clearMarkers(play);
    overlay::updateCounterLabels();
}

// Swift level of every input (see swiftLevels) with the configured gap.
std::vector<int> swiftLevelsOf(std::vector<InputEvent> const& events) {
    std::vector<SwiftInput> inputs;
    inputs.reserve(events.size());
    for (auto const& e : events)
        inputs.push_back({eventPos(e, 100), e.down, e.button * 2 + (e.player1 ? 0 : 1)});
    int const gap = cfg::integer("swift-frames") * cfg::kRawTicksPerPhysicsFrame * 100;
    return swiftLevels(inputs, gap);
}

// Swift level of the newest input of a list (Live Counter: only the past is known).
int swiftLevelAt(std::vector<InputEvent> const& events, size_t index) {
    if (index >= events.size()) return 0;
    size_t const from = index > 16 ? index - 16 : 0;
    std::vector<InputEvent> slice(events.begin() + static_cast<std::ptrdiff_t>(from),
        events.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    auto const levels = swiftLevelsOf(slice);
    return levels.empty() ? 0 : levels.back();
}

bool showSymbols() {
    return cfg::flag("show-symbols");
}

namespace {

// Bucket scheme of the counter: what Live Counter measures, else the results.
bool livePrecise(PlayLayer* play) {
    bool const cbf = cbfSplitsSteps();
    int const units = cbf ? cfg::kCbfUnitsPerRawTick : 1;
    int const step = cbf ? cfg::cbfStepUnits() : cfg::stepTicks(play && play->m_clickBetweenSteps);
    return cfg::isPrecise(step, units);
}

bool wantedHudPrecise(PlayLayer* play) {
    auto const& s = Session::get();
    if (liveMode()) return livePrecise(play);
    return s.hasResults ? s.precise : false;
}

} // namespace

// Rebuilds the counter, all markers and the counts up to now without dings or animations.
void refreshDisplay(PlayLayer* play) {
    auto& s = Session::get();
    if (!play) return;
    s.hudPrecise = wantedHudPrecise(play);
    overlay::clearMarkers(play);
    overlay::createCounterHud(play);
    drawHitborders(play);
    s.displaySilentUntil = currentTick(play);
}

// The shown clicks of a run as the L* model sees them; `owners` gets their input indices.
std::vector<precision::TimedWindow> lstarInputs(RunView const& run, std::vector<size_t>* owners) {
    std::vector<precision::TimedWindow> inputs;
    double const rawTps = static_cast<double>(cfg::rawTps());
    for (size_t i = 0; i < run.events.size() && i < run.windows.size(); ++i) {
        auto const& e = run.events[i];
        if (!overlay::isShown(e, run.windows[i])) continue;
        double const time = (e.tick + (e.cbf ? e.frac * cfg::kRawTicksPerPhysicsFrame : 0.0)) / rawTps;
        inputs.push_back({time, cfg::frames(run.windows[i]) / cfg::windowFps()});
        if (owners) owners->push_back(i);
    }
    return inputs;
}

// L* (sigma/s) after every counted click, in the background.
void computeLStar() {
    auto& s = Session::get();
    int const generation = ++s.lstarGeneration;
    s.lstarReady = false;
    s.lstar.assign(s.source.events.size(), std::numeric_limits<double>::quiet_NaN());
    s.lstarTotal = 0.0;
    if (!s.hasResults) return;

    std::vector<size_t> owners;
    auto inputs = lstarInputs(s, &owners);
    if (inputs.empty()) {
        s.lstarReady = true;
        return;
    }
    std::thread([generation, inputs = std::move(inputs), owners = std::move(owners)]() {
        auto values = precision::prefixLStar(inputs);
        Loader::get()->queueInMainThread([generation, values = std::move(values), owners]() {
            auto& s = Session::get();
            if (generation != s.lstarGeneration) return;
            for (size_t j = 0; j < owners.size() && j < values.size(); ++j) {
                if (owners[j] < s.lstar.size()) s.lstar[owners[j]] = values[j];
            }
            s.lstarTotal = values.empty() ? 0.0 : values.back();
            s.lstarReady = true;
            // The value of the clicks already passed.
            for (size_t i = std::min(s.displayCursor, s.lstar.size()); i-- > 0;) {
                if (!std::isnan(s.lstar[i])) {
                    s.lastShownLStar = s.lstar[i];
                    break;
                }
            }
            log::info("L* (24h, base model): {:.2f} sigma/s", s.lstarTotal);
        });
    }).detach();
}

// Deep Count found an approach that makes the click at least a frame wider than in this run.
bool hasEasierApproach(RunView const& run, size_t index) {
    if (index >= run.deep.size() || index >= run.windows.size()) return false;
    auto const& d = run.deep[index];
    return d.easier > 0.0 && d.easier - cfg::frames(run.windows[index]) >= 1.0 - 1e-9;
}

// Text after the window of a counted click.
std::string resultMarks(RunView const& run, size_t index) {
    if (!showSymbols()) return {};
    bool const dependent = index < run.dependent.size() && run.dependent[index];
    bool const disconnected = index < run.deep.size() && run.deep[index].disconnected();
    int const swift = index < run.swift.size() ? run.swift[index] : 0;
    bool const uncertain = (run.mismatchTick >= 0 && index < run.events.size() &&
        run.events[index].tick >= run.mismatchTick) ||
        (index < run.uncertain.size() && run.uncertain[index]);
    return clickMarks(dependent, swift, uncertain, disconnected, hasEasierApproach(run, index));
}

// The window shown for a counted click.
double shownWindow(RunView const& run, size_t index) {
    auto const& result = run.windows[index];
    double const frames = cfg::frames(result);
    if (index >= run.deep.size() || !run.deep[index].ranged() || run.deep[index].typical <= 0.0) return frames;
    double const typical = run.deep[index].typical;
    return cfg::isPrecise(result) ? std::round(typical * 100.0) / 100.0 : std::round(typical);
}

namespace {

// "{2.20}" in a note is a window: drawn "(2.20)" in the colour of its bucket.
std::string noteValue(double frames, bool precise) {
    return "{" + cfg::windowLabel(frames, precise, false) + "}";
}

} // namespace

// NaN GD style notes of a click, from Deep Count only: ranges, re-timing and disconnected windows.
std::vector<std::string> deepNotes(Session const& s, size_t index) {
    std::vector<std::string> lines;
    if (index >= s.deep.size() || index >= s.windows.size()) return lines;
    auto const& d = s.deep[index];
    bool const precise = cfg::isPrecise(s.windows[index]);
    // The range only matters for hard clicks: at most a quarter of the display limit at its smallest.
    if (d.ranged() && d.lo <= cfg::maxDisplayFrames() / 4.0 + 1e-9) {
        lines.push_back("Can be from " + noteValue(d.lo, precise) + " to " + noteValue(d.hi, precise));
        lines.push_back("depending on the previous click");
        lines.push_back("Mostly around " + noteValue(shownWindow(s, index), precise));
    }
    // Only for hard clicks, like the range.
    if (d.broken && cfg::frames(s.windows[index]) <= cfg::maxDisplayFrames() / 4.0 + 1e-9) {
        lines.push_back("With the previous click timed differently");
        lines.push_back("this one needs its own timing");
    }
    if (hasEasierApproach(s, index)) {
        lines.push_back("With an easier approach " + noteValue(d.easier, precise));
        if (!d.easierHow.empty()) lines.push_back(d.easierHow);
    }
    if (d.disconnected()) {
        auto const& spans = d.spans;
        lines.push_back(fmt::format("There are {} disconnected windows", spans.size()));
        std::string pieces;
        std::string gaps;
        for (size_t i = 0; i < spans.size(); ++i) {
            if (i > 0) {
                pieces += i + 1 == spans.size() ? " and " : ", ";
                if (!gaps.empty()) gaps += " and ";
                gaps += frameLabel(std::max(0.0, spans[i].from - spans[i - 1].to - d.step), precise, false);
            }
            pieces += noteValue(spans[i].width(d.step), precise);
        }
        lines.push_back(pieces + " separated by " + gaps);
        lines.push_back(spansLabel(spans, d.step, precise));
    }
    return lines;
}

} // namespace tc
