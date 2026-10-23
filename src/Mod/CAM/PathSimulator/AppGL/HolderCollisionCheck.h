// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "SimulationIssue.h"

#include <TopoDS_Shape.hxx>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>

namespace CAMSimulator
{
class MillPathSegment;

/// Conservative, voxel-based first-pass check for holder contact with remaining stock.
/// This intentionally supports only fixed +Z tool orientation and axisymmetric cutting profiles.
class HolderCollisionCheck
{
public:
    std::vector<SimulationIssue> Analyze(
        const TopoDS_Shape& stock,
        const std::unordered_map<int, TopoDS_Shape>& holders,
        const std::vector<MillPathSegment*>& segments,
        float requestedCellSize,
        const std::atomic_bool* cancel = nullptr,
        const std::function<void(int)>& progress = {}
    ) const;
};

}  // namespace CAMSimulator
