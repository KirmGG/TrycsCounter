#pragma once
// Pure math helpers for frame windows. No Geometry Dash / Geode types here,
// so everything in this file is unit-testable on any platform.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace tc {

struct WindowResult {
    int earlyTicks = 0;
    int lateTicks = 0;
    // Smallest shift the measurement could make, in raw ticks: 2 = one physics
    // frame, 1 = half a frame (vanilla Click Between Steps).
    int stepTicks = 2;
    // Measurement units per raw tick: 1 normally, finer for Click Between Frames.
    // earlyTicks, lateTicks and stepTicks are in these units.
    int unitsPerTick = 1;
    bool earlyCapped = false;
    bool lateCapped = false;
    bool valid = false;
    // The window is wider than the display limit. Such a click is considered
    // free: it is neither labelled, nor counted, nor does it play a sound.
    bool exceeded = false;
    // A second window of the click beyond a gap (PlannerConfig::islandSteps):
    // shifts (units, counted away from the click) of its first and last passing
    // position on the early / late side; 0 when there is none.
    int earlyIslandFrom = 0;
    int earlyIslandTo = 0;
    int lateIslandFrom = 0;
    int lateIslandTo = 0;

    // Width of the whole window in raw ticks: every tested position covers one step.
    int widthTicks() const {
        return earlyTicks + lateTicks + stepTicks;
    }

    bool capped() const {
        return earlyCapped || lateCapped;
    }
};

inline int framesToTicks(int frames, int tps, int fps) {
    if (frames <= 0 || tps <= 0 || fps <= 0) return 0;
    return std::max(1, static_cast<int>(std::ceil(
        static_cast<double>(frames) * static_cast<double>(tps) / static_cast<double>(fps)
    )));
}

inline int ticksToFramesCeil(int ticks, int tps, int fps) {
    if (ticks <= 0 || tps <= 0 || fps <= 0) return 0;
    return std::max(1, static_cast<int>(std::ceil(
        static_cast<double>(ticks) * static_cast<double>(fps) / static_cast<double>(tps) - 1e-9
    )));
}

// Largest tick count that still maps to at most `frames` display frames.
inline int maxTicksForFrames(int frames, int tps, int fps) {
    if (frames <= 0 || tps <= 0 || fps <= 0) return 0;
    return static_cast<int>(std::floor(
        static_cast<double>(frames) * static_cast<double>(tps) / static_cast<double>(fps) + 1e-9
    ));
}

// Window width in display frames. Exact for any step size.
inline double windowFrames(int widthTicks, int tps, int fps) {
    if (widthTicks <= 0 || tps <= 0 || fps <= 0) return 0.0;
    return static_cast<double>(widthTicks) * static_cast<double>(fps) / static_cast<double>(tps);
}

// Windows are measured in fractions of a frame when the step is finer than one display frame.
inline bool isPrecise(int stepTicks, int tps, int fps) {
    return windowFrames(stepTicks, tps, fps) < 1.0 - 1e-9;
}

// Frames as shown to the player: whole frames, or exact fractions in precise mode.
inline double shownFrames(double frames, bool precise) {
    if (precise) return std::round(frames * 100.0) / 100.0;
    return std::max(1.0, std::ceil(frames - 1e-9));
}

// HUD buckets, NaN GD style (the counter lists them from the widest to the
// hardest). Precise mode (fractions of a frame, CBF / CBS):
//   0-1, 1-2, 2-3, 3-4, 4-5, 5-6, 6-8, <10 (8 up to 10), <=max (10 up to max).
// Whole frames: 1, 2, 3, 4, 5, 6, 7-8, 9-10, 11-max.
inline constexpr size_t kBucketCount = 9;

inline int bucketIndex(double frames, bool precise) {
    if (precise) {
        static constexpr double kEdges[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0};
        for (int i = 0; i < 8; ++i) {
            if (frames < kEdges[i] - 1e-9) return i;
        }
        return 8;
    }
    int const whole = static_cast<int>(std::lround(frames));
    if (whole <= 6) return std::max(whole, 1) - 1;
    if (whole <= 8) return 6;
    if (whole <= 10) return 7;
    return 8;
}

// "<=" at the start of a label is drawn as the single sign "≤" by the HUD
// (the game fonts have no such glyph).
inline std::array<std::string, kBucketCount> bucketLabels(int maxFrames, bool precise) {
    maxFrames = std::max(11, maxFrames);
    if (precise)
        return {"0-1", "1-2", "2-3", "3-4", "4-5", "5-6", "6-8", "<10", "<=" + std::to_string(maxFrames)};
    return {"1", "2", "3", "4", "5", "6", "7-8", "9-10", "11-" + std::to_string(maxFrames)};
}

// Smallest and largest window (frames) of a bucket, for the legend.
inline std::pair<double, double> bucketBounds(size_t index, bool precise, int maxFrames) {
    maxFrames = std::max(11, maxFrames);
    if (precise) {
        static constexpr double kLo[] = {0, 1, 2, 3, 4, 5, 6, 8, 10};
        static constexpr double kHi[] = {1, 2, 3, 4, 5, 6, 8, 10, 0};
        index = std::min<size_t>(index, 8);
        return {kLo[index], index == 8 ? static_cast<double>(maxFrames) : kHi[index]};
    }
    static constexpr double kLo[] = {1, 2, 3, 4, 5, 6, 7, 9, 11};
    static constexpr double kHi[] = {1, 2, 3, 4, 5, 6, 8, 10, 0};
    index = std::min<size_t>(index, 8);
    return {kLo[index], index == 8 ? static_cast<double>(maxFrames) : kHi[index]};
}

// "5" for a whole-frame window, "2.50" / "1.48" for a precise one (always two
// decimals, like NaN GD's CBF counters), with "+" when a side hit a limit.
inline std::string frameLabel(double frames, bool precise, bool capped) {
    std::string text;
    if (precise) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", frames);
        text = buffer;
    } else {
        text = std::to_string(static_cast<int>(std::lround(frames)));
    }
    return text + (capped ? "+" : "");
}

// A precise window with `decimals` digits (0-2), cut rather than rounded so
// that the text never shows a wider window than the one counted ("7.96" with
// one digit is "7.9"). Whole numbers lose their zeros ("8.0" is "8"). "+" when
// a side hit a limit.
inline std::string frameLabelDecimals(double frames, int decimals, bool capped) {
    decimals = std::clamp(decimals, 0, 2);
    double const scale = decimals == 0 ? 1.0 : decimals == 1 ? 10.0 : 100.0;
    double const cut = std::floor(frames * scale + 1e-6) / scale;
    // A whole number is written without its zeros: "7", not "7.0" or "7.00".
    if (std::abs(cut - std::round(cut)) < 1e-9) decimals = 0;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, cut);
    return std::string(buffer) + (capped ? "+" : "");
}

// Digits of a precise window: `hard` below `below` frames, `easy` from there on.
inline int windowDecimals(double frames, double below, int hard, int easy) {
    return frames < below - 1e-9 ? hard : easy;
}

// One window of a click measured with the previous click at some timing, and
// how likely the player is to make that timing.
struct WeightedWindow {
    double frames = 0.0;
    double weight = 1.0;
};

// How likely a player hits `shift` inside a window [-early, late] (any units):
// players aim at the middle, so the middle counts most, the edges least.
inline double timingWeight(double shift, double early, double late) {
    double const half = (early + late) / 2.0;
    if (half <= 0.0) return 1.0;
    double const off = (shift - (late - early) / 2.0) / half;
    return std::exp(-2.0 * off * off);
}

// The window the click usually has: the value with the most weight within
// `tolerance` of it (ties: the one closest to `prefer`), averaged over that
// group. Not the middle of the range: "almost always 2, can also be 1.5".
inline double typicalWindow(std::vector<WeightedWindow> const& samples, double tolerance, double prefer) {
    if (samples.empty()) return prefer;
    double bestScore = -1.0;
    double best = prefer;
    for (auto const& a : samples) {
        double score = 0.0;
        for (auto const& b : samples) {
            if (std::abs(a.frames - b.frames) <= tolerance + 1e-9) score += b.weight;
        }
        bool const better = score > bestScore + 1e-9 ||
            (std::abs(score - bestScore) <= 1e-9 && std::abs(a.frames - prefer) < std::abs(best - prefer));
        if (better) {
            bestScore = score;
            best = a.frames;
        }
    }
    double sum = 0.0;
    double weight = 0.0;
    for (auto const& b : samples) {
        if (std::abs(b.frames - best) > tolerance + 1e-9) continue;
        sum += b.frames * b.weight;
        weight += b.weight;
    }
    return weight > 0.0 ? sum / weight : best;
}

// One connected piece of a window on the shift axis, in frames: positions of
// its first and last passing test (each test covers one step).
struct WindowSpan {
    double from = 0.0;
    double to = 0.0;
    double width(double step) const { return to - from + step; }
};

// The pieces of a window, earliest first: the early island, the window around
// the click, the late island. `framesPerUnit` converts measurement units.
inline std::vector<WindowSpan> windowSpans(WindowResult const& r, double framesPerUnit) {
    std::vector<WindowSpan> spans;
    double const f = framesPerUnit;
    if (r.earlyIslandFrom > 0) spans.push_back({-r.earlyIslandTo * f, -r.earlyIslandFrom * f});
    spans.push_back({-r.earlyTicks * f, r.lateTicks * f});
    if (r.lateIslandFrom > 0) spans.push_back({r.lateIslandFrom * f, r.lateIslandTo * f});
    return spans;
}

// "(0.01, 1.85) U (2.09, 2.44) w/ 0.01": the pieces from the earliest passing
// test (each position written as where its step ends), like NaN GD's notes.
inline std::string spansLabel(std::vector<WindowSpan> const& spans, double step, bool precise) {
    if (spans.empty()) return {};
    double const origin = spans.front().from;
    std::string text;
    for (auto const& span : spans) {
        if (!text.empty()) text += " U ";
        text += "(" + frameLabel(span.from - origin + step, precise, false) + ", " +
            frameLabel(span.to - origin + step, precise, false) + ")";
    }
    if (precise) text += " w/ " + frameLabel(step, true, false);
    return text;
}

// Marks written after the window of a click:
//   ~    depends on the previous click; after Deep Count the number is the
//        window the click has most often
//   !    swift click: pressed right after the previous press of the same button
//   !!   double swift (third press of a swift chain), !!! triple swift and more
//   -    disconnected: the click also works beyond a gap (Deep Count); the
//        number is the piece around the click
//   ^    an easier approach exists: with earlier inputs timed differently the
//        click is clearly wider (Deep Count)
//   ?    the counting replay had already left the recorded attempt, or the
//        edge check got another outcome on a re-check: may be off
inline std::string clickMarks(bool dependent, int swiftLevel, bool uncertain = false, bool disconnected = false,
    bool easier = false) {
    std::string text;
    if (dependent) text += "~";
    if (disconnected) text += "-";
    if (easier) text += "^";
    if (swiftLevel > 0) text.append(static_cast<size_t>(std::min(swiftLevel, 3)), '!');
    if (uncertain) text += "?";
    return text;
}

// One input as the swift detection sees it: position on the measurement axis.
struct SwiftInput {
    int pos = 0;
    bool down = false;
    int key = 0;      // button and player together
};

// Swift level of every input: 0 for a normal click, 1 for a press that follows
// the previous press of the same key within `maxGap` units, 2 for the next
// press of such a chain, and so on. Releases are always 0.
inline std::vector<int> swiftLevels(std::vector<SwiftInput> const& inputs, int maxGap) {
    std::vector<int> levels(inputs.size(), 0);
    if (maxGap <= 0) return levels;
    std::vector<std::pair<int, size_t>> lastPress;   // key -> index of its last press
    for (size_t i = 0; i < inputs.size(); ++i) {
        auto const& input = inputs[i];
        if (!input.down) continue;
        auto it = std::find_if(lastPress.begin(), lastPress.end(), [&](auto const& e) { return e.first == input.key; });
        if (it != lastPress.end()) {
            if (input.pos - inputs[it->second].pos <= maxGap) levels[i] = levels[it->second] + 1;
            it->second = i;
        } else {
            lastPress.push_back({input.key, i});
        }
    }
    return levels;
}

// Time available for the click, in milliseconds, for a window of `frames`
// display frames at `fps` frames per second.
inline double windowMs(double frames, int fps) {
    if (frames <= 0.0 || fps <= 0) return 0.0;
    return frames * 1000.0 / static_cast<double>(fps);
}

inline std::string formatMs(double ms, bool capped) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1fms%s", ms, capped ? "+" : "");
    return buffer;
}

// HUD lines of a window: "6.17MS" and "162Hz" (the rate a click of that
// window corresponds to: one window per period).
inline std::string hudMs(double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2fMS", ms);
    return buffer;
}

inline std::string hudHz(double ms) {
    if (ms <= 0.0) return "-";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0fHz", 1000.0 / ms);
    return buffer;
}

// Playback-speed factor for the ding. A window of `baseFrames` (8) plays the
// sound as it is; every frame less lowers the pitch by `semitonesPerFrame`,
// every frame more raises it. Hard clicks sound low, free ones high.
inline double dingPitch(double frames, double semitonesPerFrame, double baseFrames = 8.0) {
    double const steps = std::max(frames, 1.0) - baseFrames;
    double const factor = std::exp2(steps * semitonesPerFrame / 12.0);
    return std::clamp(factor, 0.25, 4.0);
}

} // namespace tc
