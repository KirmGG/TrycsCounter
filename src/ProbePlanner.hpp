#pragma once
// Scheduler of the frame-window measurement.

#include "WindowMath.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace tc {

struct PlannerInput {
    int tick = 0;
    int button = 0;
    bool player1 = true;
    // A side that survives all the way to the neighbouring input on the same button counts as unlimited.
    bool relax = false;
    // A press that takes its release along: the next input on the same button moves by the same shift.
    bool carriesNext = false;
    // Survival horizon of this input after the next input, in raw ticks; < 0: PlannerConfig::lookahead.
    int lookahead = -1;
};

// How the recorded run ended.
enum class RunEnd {
    Death,
    Paused,
    Completed
};

struct PlannerConfig {
    int step = 2;             // raw ticks per shift step (2 = one physics frame, 1 = half a frame)
    int maxShift = 32;        // raw ticks, hard limit of a shift in either direction
    int lookahead = 180;      // raw ticks a shifted input must survive afterwards
    int maxWindowTicks = 32;  // largest window width (raw ticks) that is still displayed
    int endTick = 0;          // raw tick where the recorded run ended
    RunEnd endKind = RunEnd::Death;
    int minInputTick = 1;     // earliest raw tick a shifted input may land on
    bool anchorNext = true;   // lookahead starts at the next input instead of the shifted one
    bool controlTrial = false;// first run the unshifted input and require it to survive
    int onlyIndex = -1;       // >= 0: measure only this input
    int coarseSteps = 1;      // stride of the outward scan, in steps (one physics frame)
    bool gallop = false;      // fast search: doubling + bisection (assumes no holes)
    int firstCandidate = 1;   // first shift of the galloping search, in steps
    int unitsPerTick = 1;     // copied into the results (see WindowResult::unitsPerTick)
    // An input that survives every shift up to its neighbour on the same button does not matter on that side.
    bool relaxNeighbors = false;
    // > 0: after a side's edge is found.
    int islandSteps = 0;
    // >= 0: the side is already known; only the island search runs for it.
    int knownEarly = -1;
    int knownLate = -1;
    // Not empty: no measurement, only these shifts of onlyIndex are tried one after another.
    std::vector<int> fixedShifts;
};

struct Trial {
    size_t index = 0;  // index of the shifted input
    int shift = 0;     // raw ticks, negative = earlier
    int endTick = 0;   // raw tick at which the trial counts as survived
};

// Corrections tried when a moved input dies only after the next click.
inline std::optional<int> correctionShift(int shift, int frameUnits, int index, int maxFrames = 4, bool wide = false) {
    if (index < 1 || shift == 0 || frameUnits <= 0) return std::nullopt;
    std::vector<int> list{shift, -shift};
    int const frames = (std::abs(shift) + frameUnits - 1) / frameUnits;
    int const reach = wide ? std::max(maxFrames, 1) : std::min(std::max(maxFrames, 1), frames + 1);
    int const compensate = shift > 0 ? -1 : 1;
    for (int j = 1; j <= reach; ++j) {
        for (int sign : {compensate, -compensate}) {
            int const m = sign * j * frameUnits;
            if (std::find(list.begin(), list.end(), m) == list.end()) list.push_back(m);
        }
    }
    if (static_cast<size_t>(index) > list.size()) return std::nullopt;
    return list[static_cast<size_t>(index) - 1];
}

class ProbePlanner {
    enum class Island { None, Search, BisectStart, ScanEnd, BisectEnd };

public:
    void begin(std::vector<PlannerInput> inputs, PlannerConfig config) {
        m_inputs = std::move(inputs);
        m_config = config;
        m_config.step = std::max(1, m_config.step);
        m_results.assign(m_inputs.size(), WindowResult{});
        m_config.firstCandidate = std::max(1, m_config.firstCandidate);
        m_config.coarseSteps = std::max(1, m_config.coarseSteps);
        for (auto& result : m_results) {
            result.stepTicks = m_config.step;
            result.unitsPerTick = std::max(1, m_config.unitsPerTick);
        }
        m_pending = false;
        m_trialsReported = 0;
        m_fixedOutcomes.clear();
        m_index = -1;
        if (!m_inputs.empty()) {
            if (m_config.onlyIndex >= 0 && static_cast<size_t>(m_config.onlyIndex) < m_inputs.size())
                m_index = m_config.onlyIndex;
            else if (m_config.onlyIndex < 0)
                m_index = static_cast<int>(m_inputs.size()) - 1;
        }
        beginEvent();
        settle();
    }

    // Next trial to run, or nullopt when every input has been measured.
    std::optional<Trial> next() {
        if (m_pending) return m_current;
        if (!m_config.fixedShifts.empty()) return nextFixed();
        while (true) {
            settle();
            if (m_index < 0) return std::nullopt;

            auto const& input = m_inputs[static_cast<size_t>(m_index)];
            if (!m_control) m_candidate = nextCandidate();
            int const shift = m_control ? 0 : (m_early ? -m_candidate : m_candidate) * m_config.step;
            int const shifted = input.tick + shift;
            int anchor = shifted;
            if (m_config.anchorNext) {
                size_t following = static_cast<size_t>(m_index) + 1;
                if (input.carriesNext && m_partner >= 0) {
                    anchor = std::max(anchor, m_inputs[static_cast<size_t>(m_partner)].tick + shift);
                    if (following == static_cast<size_t>(m_partner)) ++following;
                }
                if (following < m_inputs.size()) anchor = std::max(anchor, m_inputs[following].tick);
            }

            int const required = anchor + (input.lookahead >= 0 ? input.lookahead : m_config.lookahead);
            int const end = std::min(required, m_config.endTick);
            if (end < required && m_config.endKind != RunEnd::Completed) {
                // Not enough verified gameplay after this input.
                m_unmeasurable = true;
                closeEvent();
                continue;
            }

            m_current = Trial{static_cast<size_t>(m_index), shift, end};
            m_pending = true;
            return m_current;
        }
    }

    // Outcome of the trial returned by the last next() call.
    void report(bool survived) {
        if (!m_pending || m_index < 0) return;
        m_pending = false;
        ++m_trialsReported;

        if (!m_config.fixedShifts.empty()) {
            m_fixedOutcomes.push_back(survived ? 1 : 0);
            return;
        }

        if (m_control) {
            m_control = false;
            if (!survived) {
                // The simulation does not reproduce the real attempt.
                m_unmeasurable = true;
                closeEvent();
            }
            return;
        }

        if (m_island != Island::None) {
            reportIsland(survived);
            return;
        }

        auto& result = m_results[static_cast<size_t>(m_index)];
        if (survived) {
            m_lo = m_candidate;
        } else {
            m_hi = m_candidate;
            m_coarse = false;
        }

        // The window is already known to be wide enough to be free: stop measuring.
        int const known = (m_early ? 0 : result.earlyTicks) + m_lo * m_config.step + m_config.step;
        if (survived && known > m_config.maxWindowTicks) {
            if (m_early) result.earlyTicks = m_lo * m_config.step;
            else result.lateTicks = m_lo * m_config.step;
            result.exceeded = true;
            closeEvent();
        }
    }

    bool finished() const { return m_index < 0; }
    // Edge check: per fixed shift 1 survived, 0 died, -1 not enough gameplay after it.
    std::vector<int> const& fixedOutcomes() const { return m_fixedOutcomes; }
    bool hasPendingTrial() const { return m_pending; }
    size_t trialsReported() const { return m_trialsReported; }
    size_t inputCount() const { return m_inputs.size(); }
    std::vector<WindowResult> const& results() const { return m_results; }

    // Number of inputs that are completely measured.
    size_t finishedInputs() const {
        if (m_config.onlyIndex >= 0) return m_index < 0 ? 1 : 0;
        return m_index < 0 ? m_inputs.size() : m_inputs.size() - 1 - static_cast<size_t>(m_index);
    }

private:
    // Edge check: the fixed shifts of onlyIndex, in order.
    std::optional<Trial> nextFixed() {
        while (m_index >= 0 && m_fixedOutcomes.size() < m_config.fixedShifts.size()) {
            auto const& input = m_inputs[static_cast<size_t>(m_index)];
            int const shift = m_config.fixedShifts[m_fixedOutcomes.size()];
            // Never across a neighbour on the same button or out of the run.
            if (shift < m_lower || shift > m_upper) {
                m_fixedOutcomes.push_back(-1);
                continue;
            }
            int anchor = input.tick + shift;
            if (m_config.anchorNext) {
                size_t following = static_cast<size_t>(m_index) + 1;
                if (input.carriesNext) {
                    for (size_t i = following; i < m_inputs.size(); ++i) {
                        if (m_inputs[i].button != input.button || m_inputs[i].player1 != input.player1) continue;
                        anchor = std::max(anchor, m_inputs[i].tick + shift);
                        if (following == i) ++following;
                        break;
                    }
                }
                if (following < m_inputs.size()) anchor = std::max(anchor, m_inputs[following].tick);
            }
            int const required = anchor + (input.lookahead >= 0 ? input.lookahead : m_config.lookahead);
            int const end = std::min(required, m_config.endTick);
            if (end < required && m_config.endKind != RunEnd::Completed) {
                m_fixedOutcomes.push_back(-1);
                continue;
            }
            m_current = Trial{static_cast<size_t>(m_index), shift, end};
            m_pending = true;
            return m_current;
        }
        m_index = -1;
        return std::nullopt;
    }

    static int floorToStep(int value, int step) {
        int const r = value % step;
        if (r == 0) return value;
        return value > 0 ? value - r : value - (step + r);
    }

    static int ceilToStep(int value, int step) {
        int const r = value % step;
        if (r == 0) return value;
        return value > 0 ? value + (step - r) : value - r;
    }

    // Next shift to try on the current side, in steps.
    int nextCandidate() const {
        switch (m_island) {
        case Island::Search: return std::min(m_iFail + m_config.coarseSteps, m_iLimit);
        case Island::BisectStart: return m_iFail + (m_iPass - m_iFail) / 2;
        case Island::ScanEnd: return std::min(m_iLo + m_config.coarseSteps, m_bound);
        case Island::BisectEnd: return m_iLo + (m_iHi - m_iLo) / 2;
        case Island::None: break;
        }
        if (m_coarse) {
            if (m_config.gallop) return std::min(m_lo == 0 ? m_config.firstCandidate : m_lo * 2, m_bound);
            return std::min(m_lo + m_config.coarseSteps, m_bound);
        }
        return m_lo + (m_hi - m_lo) / 2;
    }

    // Furthest allowed early shift and late shift of the current input.
    void beginEvent() {
        m_unmeasurable = false;
        m_early = true;
        m_control = m_config.controlTrial;
        m_lower = 0;
        m_upper = 0;
        m_lo = 0;
        m_hi = 1;
        m_bound = 0;
        if (m_index < 0) return;

        auto const& input = m_inputs[static_cast<size_t>(m_index)];
        int lower = -m_config.maxShift;
        bool lowerByNeighbor = false;
        for (int i = m_index - 1; i >= 0; --i) {
            auto const& prev = m_inputs[static_cast<size_t>(i)];
            if (prev.button == input.button && prev.player1 == input.player1) {
                int const limit = prev.tick - input.tick + 1;
                if (limit > lower) {
                    lower = limit;
                    lowerByNeighbor = true;
                }
                break;
            }
        }
        if (m_config.minInputTick - input.tick >= lower) {
            lower = m_config.minInputTick - input.tick;
            lowerByNeighbor = false;
        }
        m_lower = std::min(ceilToStep(lower, m_config.step), 0);

        int upper = m_config.maxShift;
        bool upperByNeighbor = false;
        m_partner = -1;
        int carriedTick = input.tick;   // the last input of the moved group
        for (size_t i = static_cast<size_t>(m_index) + 1; i < m_inputs.size(); ++i) {
            auto const& next = m_inputs[i];
            if (next.button != input.button || next.player1 != input.player1) continue;
            if (input.carriesNext && m_partner < 0) {
                // The partner moves along; the input after it is the neighbour.
                m_partner = static_cast<int>(i);
                carriedTick = next.tick;
                continue;
            }
            int const limit = next.tick - carriedTick - 1;
            if (limit < upper) {
                upper = limit;
                upperByNeighbor = true;
            }
            break;
        }
        if (m_config.endTick - 1 - carriedTick <= upper) {
            upper = m_config.endTick - 1 - carriedTick;
            upperByNeighbor = false;
        }
        m_upper = std::max(floorToStep(upper, m_config.step), 0);

        if (!m_config.fixedShifts.empty()) return;   // edge check: no sides to measure

        bool const relax = m_config.relaxNeighbors && input.relax;
        m_earlyFree = relax && lowerByNeighbor;
        m_lateFree = relax && upperByNeighbor;

        startSide(true);
    }

    // The early side may use the whole display width.
    void startSide(bool early) {
        m_early = early;
        m_lo = 0;
        m_coarse = true;
        m_island = Island::None;
        int range = early ? -m_lower : m_upper;
        if (!early && m_index >= 0) {
            int const rest = m_config.maxWindowTicks - m_results[static_cast<size_t>(m_index)].earlyTicks;
            range = std::min(range, std::max(rest, 0));
        }
        m_bound = range / m_config.step;
        m_hi = m_bound + 1;
        // A side measured before: settled at once (the edge failed right after it).
        int const known = early ? m_config.knownEarly : m_config.knownLate;
        if (known >= 0) {
            m_lo = std::min(known / m_config.step, m_bound);
            m_hi = m_lo + 1;
            m_coarse = false;
        }
    }

    // Island search of the current side, after its edge: m_hi is the first failing shift.
    bool wantIsland(bool capped) const {
        return m_config.islandSteps > 0 && !capped && m_hi < m_bound && !(m_early ? m_earlyFree : m_lateFree);
    }

    void beginIsland() {
        m_island = Island::Search;
        m_iFail = m_hi;
        m_iLimit = std::min(m_bound, m_hi + m_config.islandSteps);
    }

    void reportIsland(bool survived) {
        int const c = m_candidate;
        switch (m_island) {
        case Island::Search:
            if (survived) {
                m_iPass = c;
                m_island = Island::BisectStart;
            } else {
                m_iFail = c;
            }
            break;
        case Island::BisectStart:
            (survived ? m_iPass : m_iFail) = c;
            break;
        case Island::ScanEnd:
            if (survived) {
                m_iLo = c;
            } else {
                m_iHi = c;
                m_island = Island::BisectEnd;
            }
            break;
        case Island::BisectEnd:
            (survived ? m_iLo : m_iHi) = c;
            break;
        case Island::None: break;
        }
    }

    // True once the island search of the side is over (found or not).
    bool settleIsland() {
        while (true) {
            switch (m_island) {
            case Island::Search:
                if (m_iFail >= m_iLimit) {
                    m_island = Island::None;
                    return true;
                }
                return false;
            case Island::BisectStart:
                if (m_iPass - m_iFail > 1) return false;
                m_iLo = m_iPass;
                m_iHi = m_bound + 1;
                m_island = Island::ScanEnd;
                continue;
            case Island::ScanEnd:
                if (m_iLo < m_bound) return false;
                storeIsland();
                return true;
            case Island::BisectEnd:
                if (m_iHi - m_iLo > 1) return false;
                storeIsland();
                return true;
            case Island::None: return true;
            }
        }
    }

    void storeIsland() {
        auto& result = m_results[static_cast<size_t>(m_index)];
        int const from = m_iPass * m_config.step;
        int const to = m_iLo * m_config.step;
        if (m_early) {
            result.earlyIslandFrom = from;
            result.earlyIslandTo = to;
        } else {
            result.lateIslandFrom = from;
            result.lateIslandTo = to;
        }
        m_island = Island::None;
    }

    // The current side is done: the late side next, or the next input.
    void finishSide() {
        if (m_early) {
            startSide(false);
            return;
        }
        closeEvent();
    }

    void closeEvent() {
        if (m_index >= 0) m_results[static_cast<size_t>(m_index)].valid = !m_unmeasurable;
        m_index = m_config.onlyIndex >= 0 ? -1 : m_index - 1;
        beginEvent();
    }

    // Finish sides whose bisection has converged.
    void settle() {
        if (!m_config.fixedShifts.empty()) return;
        while (m_index >= 0 && !m_control) {
            if (m_island != Island::None) {
                if (!settleIsland()) return;
                finishSide();
                continue;
            }
            if (m_hi - m_lo > 1) return;
            auto& result = m_results[static_cast<size_t>(m_index)];
            int edge = m_lo * m_config.step;
            // Reaching the configured limit means the real window may be wider.
            bool capped = m_lo == m_bound && m_bound > 0 && edge >= m_config.maxShift;
            // An input that survived all the way to its neighbour is free on this side.
            if (m_lo == m_bound && (m_early ? m_earlyFree : m_lateFree)) {
                edge = std::max(edge, m_config.maxWindowTicks);
                capped = true;
            }
            if (m_early) {
                result.earlyTicks = edge;
                result.earlyCapped = capped;
                if (edge + m_config.step > m_config.maxWindowTicks) {
                    result.exceeded = true;
                    closeEvent();
                    continue;
                }
            } else {
                result.lateTicks = edge;
                result.lateCapped = capped;
                if (result.widthTicks() > m_config.maxWindowTicks) {
                    result.exceeded = true;
                    closeEvent();
                    continue;
                }
            }
            if (wantIsland(capped)) {
                beginIsland();
                continue;
            }
            finishSide();
        }
    }

    std::vector<PlannerInput> m_inputs;
    PlannerConfig m_config;
    std::vector<WindowResult> m_results;
    Trial m_current;
    bool m_pending = false;
    bool m_early = true;
    bool m_unmeasurable = false;
    bool m_control = false;
    bool m_coarse = true;
    bool m_earlyFree = false;
    int m_partner = -1;
    bool m_lateFree = false;
    int m_candidate = 0;
    int m_index = -1;
    int m_lo = 0;
    int m_hi = 1;
    int m_bound = 0;
    int m_lower = 0;
    int m_upper = 0;
    size_t m_trialsReported = 0;
    std::vector<int> m_fixedOutcomes;
    Island m_island = Island::None;
    int m_iFail = 0;    // island search: last failing shift (steps)
    int m_iPass = 0;    // first passing shift of the island
    int m_iLimit = 0;   // furthest shift the search looks at
    int m_iLo = 0;      // last passing shift of the island
    int m_iHi = 0;      // first failing shift after it
};

} // namespace tc
