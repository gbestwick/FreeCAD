// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>

namespace CAMSimulator
{
enum class IssueSeverity { Warning, Error };
enum class IssueKind {
    MissingRate,
    MissingTool,
    UnsupportedCommand,
    ApproximateCycle,
    HolderStockCollision,
    CollisionCoverage
};

// Source-command references remain valid even when a command has no rendered movement.
struct SimulationIssue
{
    int command;
    IssueSeverity severity;
    IssueKind kind;
    std::string message;
};
}
