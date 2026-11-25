#pragma once
// L* (sigma per second), the precision metric of NaN GD's NaNDL list
// (https://nandl.pages.dev/), base model without nerve, fatigue or CPS terms:
//
//   every input i has a window of w_i seconds at time t_i of the attempt.
//   With timing errors X ~ N(0, 1/L^2) the input passes with
//       p_i = P(|X| <= w_i / 2) = 1 - erfc(w_i * L / (2 * sqrt 2)),
//   an attempt that dies at input i costs t_i, a full run costs t_n, so
//       E[T_C] = (t_n * P(C) + sum_i t_i * r_i * q_i) / P(C)
//   with r_i the chance to reach input i and q_i = 1 - p_i.
//   L* is the L for which E[T_C] equals the target (24 hours).
//
// Same model as the calculators of NaNDL and Frame Window Counter (C0nscious).
// Pure C++, no game types: unit-testable.

#include <algorithm>
#include <cmath>
#include <vector>

namespace tc::precision {

inline constexpr double kTargetSeconds = 86400.0;

struct TimedWindow {
    double time = 0.0;     // seconds from the level start
    double window = 0.0;   // seconds
};

// Expected time to beat the first n inputs at precision L (seconds).
inline double expectedCompletion(std::vector<TimedWindow> const& inputs, size_t n, double L) {
    if (n == 0 || L <= 0.0) return 1e300;
    double reach = 1.0;
    double failTime = 0.0;
    constexpr double kScale = 0.5 * 0.7071067811865475;
    for (size_t j = 0; j < n; ++j) {
        double const x = inputs[j].window * kScale * L;
        if (x > 6.0) continue;   // passes practically always
        double const q = std::erfc(x);
        double const p = std::max(1.0 - q, 1e-15);
        failTime += inputs[j].time * reach * q;
        reach *= p;
        if (reach < 1e-250) return 1e300;
    }
    double const tn = inputs[n - 1].time;
    return (tn * reach + failTime) / reach;
}

// L* after every input: result[i] is the precision needed to beat inputs 0..i
// within `target` seconds. Inputs must be sorted by time. Adding an input never
// makes a run easier, so the values do not decrease, which bounds each search.
inline std::vector<double> prefixLStar(std::vector<TimedWindow> const& inputs, double target = kTargetSeconds) {
    std::vector<double> result(inputs.size(), 0.0);
    double previous = 1e-3;
    for (size_t i = 0; i < inputs.size(); ++i) {
        size_t const n = i + 1;
        double lo = previous;
        if (expectedCompletion(inputs, n, lo) <= target) {
            result[i] = lo;
            continue;
        }
        double hi = std::max(lo * 2.0, 1.0);
        for (int guard = 0; guard < 200 && expectedCompletion(inputs, n, hi) > target; ++guard) {
            lo = hi;
            hi *= 2.0;
        }
        for (int iter = 0; iter < 60 && (hi - lo) > 1e-6 * hi; ++iter) {
            double const mid = 0.5 * (lo + hi);
            if (expectedCompletion(inputs, n, mid) > target) lo = mid;
            else hi = mid;
        }
        previous = hi;
        result[i] = hi;
    }
    return result;
}

} // namespace tc::precision
