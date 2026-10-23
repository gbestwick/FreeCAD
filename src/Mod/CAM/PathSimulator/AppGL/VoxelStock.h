// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <TopoDS_Shape.hxx>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace CAMSimulator
{
class EndMill;
class MillPathSegment;

/// CPU-side approximate stock state for inspection and later surface extraction.
/// Tool axes are fixed to +Z; occupancy is sampled at voxel centers.
class VoxelStock
{
public:
    struct Point3
    {
        double x = 0;
        double y = 0;
        double z = 0;
    };

    struct ProfileStation
    {
        double radius = 0;
        double z = 0;
    };

    struct MeshVertex
    {
        float x, y, z;
        float nx, ny, nz;
    };

    bool Initialize(
        const TopoDS_Shape& shape,
        double requestedCellSize,
        std::size_t maxCells = 2000000,
        const std::atomic_bool* cancel = nullptr,
        const std::function<void(int)>& progress = {}
    );
    /// Classify another solid on exactly the same grid as an existing stock model.
    bool InitializeOnGrid(
        const TopoDS_Shape& shape,
        const VoxelStock& grid,
        bool& shapeWithinGridBounds,
        const std::atomic_bool* cancel = nullptr,
        const std::function<void(int)>& progress = {}
    );
    /// Split this occupancy against a target occupancy into excess and missing material masks.
    bool BuildDifferenceMasks(
        const VoxelStock& target,
        VoxelStock& excessMaterial,
        VoxelStock& missingTargetMaterial
    ) const;
    bool InitializeBox(const Point3& minimum, const Point3& maximum, double cellSize);

    /// Remove material for an axisymmetric tool profile moved on a linear path.
    /// The profile Z values are offsets from the supplied path positions.
    bool RemoveSweep(
        const std::vector<ProfileStation>& profile,
        const Point3& from,
        const Point3& to,
        std::size_t maxSamples = 1000000
    );

    /// Apply a simulator path segment using the same tool profile and position
    /// interpolation used by the OpenGL preview. False means the work budget
    /// was insufficient or the segment/tool data was invalid.
    bool RemoveToolSegment(
        const EndMill& tool,
        const MillPathSegment& segment,
        std::size_t maxSamples = 1000000,
        const std::atomic_bool* cancel = nullptr
    );

    /// Extract exposed voxel faces as a triangle mesh. False means the requested
    /// triangle budget was exceeded; output vectors are cleared in that case.
    bool ExtractSurface(
        std::vector<MeshVertex>& vertices,
        std::vector<std::uint32_t>& indices,
        std::size_t maxTriangles = 2000000,
        const std::atomic_bool* cancel = nullptr,
        const std::function<void(int)>& progress = {}
    ) const;

    void Clear();
    bool IsInitialized() const { return !mOccupied.empty(); }
    bool IsOccupied(int x, int y, int z) const;
    double CellSize() const { return mCellSize; }
    std::array<int, 3> Dimensions() const { return {mNx, mNy, mNz}; }
    Point3 Minimum() const { return mMinimum; }
    Point3 Maximum() const { return mMaximum; }
    std::size_t OccupiedCellCount() const;
    double RemainingVolume() const;
    double InitialVolume() const { return mInitialVolume; }
    double RemovedVolume() const { return std::max(0.0, mInitialVolume - RemainingVolume()); }

private:
    std::size_t Index(int x, int y, int z) const;
    bool ConfigureGrid(const Point3& minimum, const Point3& maximum, double requestedCellSize, std::size_t maxCells);

    Point3 mMinimum;
    Point3 mMaximum;
    double mCellSize = 0;
    int mNx = 0;
    int mNy = 0;
    int mNz = 0;
    double mInitialVolume = 0;
    std::vector<std::uint8_t> mOccupied;
};

}  // namespace CAMSimulator
