#include "../src/Precision.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    using namespace tc::precision;

    // One 1.48-frame click (6.17 ms) one second into the level: very easy to
    // beat within a day, L* is tiny (NaN GD's HUD shows 0.00).
    {
        std::vector<TimedWindow> one = {{1.0, 1.48 / 240.0}};
        auto const l = prefixLStar(one);
        assert(l.size() == 1);
        assert(l[0] < 0.01);
    }

    // More and harder clicks need more precision; values never go down.
    {
        std::vector<TimedWindow> run;
        for (int i = 0; i < 300; ++i) run.push_back({1.0 + i * 0.25, (1.0 + (i % 5)) / 240.0});
        auto const l = prefixLStar(run);
        for (size_t i = 1; i < l.size(); ++i) assert(l[i] >= l[i - 1] - 1e-9);
        assert(l.back() > 50.0);
        // The solution hits the 24 hour target.
        double const t = expectedCompletion(run, run.size(), l.back());
        assert(std::abs(t - kTargetSeconds) / kTargetSeconds < 1e-3);
    }

    std::cout << "Precision tests passed\n";
}
