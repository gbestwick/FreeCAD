// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>

namespace CAMSimulator
{
struct CommandTimeBreakdown
{
    double cuttingSeconds = 0;
    double rapidSeconds = 0;
    double dwellSeconds = 0;
    double assumedRateSeconds = 0;
    bool omittedTime = false;

    void addMove(bool rapid, double seconds, bool assumedRate)
    {
        (rapid ? rapidSeconds : cuttingSeconds) += seconds;
        if (assumedRate) assumedRateSeconds += seconds;
    }
};

struct OperationTimeBreakdown
{
    int operationId = -1;
    std::string name;
    CommandTimeBreakdown time;
};
}  // namespace CAMSimulator
