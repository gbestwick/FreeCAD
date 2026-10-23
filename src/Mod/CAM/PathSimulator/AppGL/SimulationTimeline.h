// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "MillMotion.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <numbers>

namespace CAMSimulator
{

inline double motionLength(const MillMotion& from, const MillMotion& to)
{
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double dz = to.z - from.z;
    if ((to.cmd == eRotateCW || to.cmd == eRotateCCW) && (to.i != 0 || to.j != 0)) {
        const double radius = std::hypot(to.i, to.j);
        const double start = std::atan2(-to.j, -to.i);
        const double end = std::atan2(dy - to.j, dx - to.i);
        double sweep = to.cmd == eRotateCCW ? end - start : start - end;
        if (sweep <= 0) {
            sweep += 2 * std::numbers::pi;
        }
        return std::hypot(radius * sweep, dz);
    }
    return std::hypot(dx, dy, dz);
}

// Constant-speed travel time in seconds. A negative result means the rate is unknown.
inline double motionDuration(double length, double horizontal, double vertical, const MillMotion& move)
{
    if (length <= 0) {
        return 0;
    }
    if (move.rapid) {
        // Simultaneous XY/Z movement must respect both configured rapid limits.
        if ((horizontal <= 0 || move.horizontalRapid > 0)
            && (vertical <= 0 || move.verticalRapid > 0)) {
            const double xy = horizontal > 0 ? horizontal * 60 / move.horizontalRapid : 0;
            const double z = vertical > 0 ? vertical * 60 / move.verticalRapid : 0;
            return std::max(xy, z);
        }
        return move.rapidFeed > 0 ? length * 60 / move.rapidFeed : -1;
    }
    return move.feedRate > 0 ? length * 60 / move.feedRate : -1;
}

// Maps wall-clock playback to geometric steps without tying speed to tessellation quality.
class SimulationTimeline
{
public:
    void clear()
    {
        times = {0};
        steps = {0};
    }

    void append(int count, double seconds)
    {
        steps.push_back(steps.back() + count);
        times.push_back(times.back() + std::max(0.0, seconds));
    }

    double duration() const { return times.back(); }

    double stepAtTime(double seconds) const
    {
        seconds = std::clamp(seconds, 0.0, duration());
        const auto end = std::upper_bound(times.begin(), times.end(), seconds);
        if (end == times.end()) {
            return steps.back();
        }
        const auto i = static_cast<std::size_t>(end - times.begin());
        const double fraction = (seconds - times[i - 1]) / (times[i] - times[i - 1]);
        if (steps[i] == steps[i - 1]) return steps[i];  // Dwell: time advances while the tool remains still.
        return steps[i - 1] + fraction * (steps[i] - steps[i - 1]);
    }

    double timeAtStep(double step) const
    {
        if (step <= 0) return 0;
        step = std::clamp(step, 0.0, static_cast<double>(steps.back()));
        const auto end = std::upper_bound(steps.begin(), steps.end(), step);
        if (end == steps.end()) {
            return duration();
        }
        const auto i = static_cast<std::size_t>(end - steps.begin());
        const double fraction = (step - steps[i - 1]) / (steps[i] - steps[i - 1]);
        return times[i - 1] + fraction * (times[i] - times[i - 1]);
    }

private:
    std::vector<double> times {0};
    std::vector<int> steps {0};
};

}  // namespace CAMSimulator
