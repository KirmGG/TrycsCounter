#pragma once
// Typed access to the mod settings and the unit conversions that depend on them.

#include <Geode/Geode.hpp>

#include "WindowMath.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace tc::cfg {

// GD 2.2081 m_tickIndex advances two raw units per 240 Hz physics frame.
inline constexpr int kRawTicksPerPhysicsFrame = 2;
// Click Between Frames inputs are measured in 1/100 of a physics frame.
inline constexpr int kCbfUnitsPerRawTick = 50;

inline int integer(char const* key) {
    return static_cast<int>(geode::Mod::get()->getSettingValue<int64_t>(key));
}

inline float real(char const* key) {
    return static_cast<float>(geode::Mod::get()->getSettingValue<double>(key));
}

inline bool flag(char const* key) {
    return geode::Mod::get()->getSettingValue<bool>(key);
}

inline std::string text(char const* key) {
    return geode::Mod::get()->getSettingValue<std::string>(key);
}

inline int tps() { return integer("physics-tps"); }
inline int windowFps() { return integer("window-fps"); }
inline int rawTps() { return tps() * kRawTicksPerPhysicsFrame; }

inline int framesToTicks(int frames) { return tc::framesToTicks(frames, rawTps(), windowFps()); }
inline int ticksToFrames(int ticks) { return tc::ticksToFramesCeil(ticks, rawTps(), windowFps()); }

// Each side of a window is checked up to the display width, so the two sides together always cover it.
inline int maxWindowTicks();
inline int maxShiftTicks() { return maxWindowTicks(); }
inline int lookaheadTicks() { return framesToTicks(integer("lookahead-frames")); }
inline int checkpointTicks() { return framesToTicks(integer("checkpoint-interval")); }
inline int leadTicks() { return framesToTicks(integer("lead-frames")); }

// Smallest shift the measurement makes, in raw ticks.
inline int stepTicks(bool clickBetweenSteps) {
    auto const mode = text("precision");
    if (mode == "Frame") return kRawTicksPerPhysicsFrame;
    if (mode == "Half frame") return 1;
    return clickBetweenSteps ? 1 : kRawTicksPerPhysicsFrame;
}

// Shift step of a Click Between Frames measurement, in kCbfUnitsPerRawTick units.
inline int cbfStepUnits() {
    int const perFrame = kRawTicksPerPhysicsFrame * kCbfUnitsPerRawTick;
    auto const mode = text("precision");
    if (mode == "Frame") return perFrame;
    if (mode == "Half frame") return perFrame / 2;
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(real("cbf-step")) * perFrame)));
}

inline bool isPrecise(int stepUnits, int unitsPerTick = 1) {
    return tc::isPrecise(stepUnits, rawTps() * std::max(1, unitsPerTick), windowFps());
}
inline bool isPrecise(tc::WindowResult const& result) { return isPrecise(result.stepTicks, result.unitsPerTick); }

// Width of a measured window in display frames (whole frames unless precise).
inline double frames(tc::WindowResult const& result) {
    int const units = std::max(1, result.unitsPerTick);
    return tc::shownFrames(tc::windowFrames(result.widthTicks(), rawTps() * units, windowFps()), isPrecise(result));
}

// A live verdict waits for the lookahead after the click plus the largest late shift.
inline int liveVerdictDelayTicks() { return lookaheadTicks() + maxShiftTicks(); }

inline int maxDisplayFrames() { return integer("max-display-window"); }

// A window in frames, a free one (wider than the display limit) as the limit.
inline double framesOrLimit(tc::WindowResult const& result) {
    return result.exceeded ? static_cast<double>(maxDisplayFrames()) : frames(result);
}

// The window as the player sees it.
inline std::string windowLabel(double frames, bool precise, bool capped) {
    if (!precise) return tc::frameLabel(frames, false, capped);
    int const decimals = tc::windowDecimals(frames, static_cast<double>(real("decimals-below")),
        static_cast<int>(integer("hard-decimals")), static_cast<int>(integer("easy-decimals")));
    return tc::frameLabelDecimals(frames, decimals, capped);
}

// The counter list on the right side of the screen (and the notes on the left).
inline bool hudRight() { return text("hud-side") == "Right"; }
inline int maxWindowTicks() { return tc::maxTicksForFrames(maxDisplayFrames(), rawTps(), windowFps()); }

} // namespace tc::cfg
