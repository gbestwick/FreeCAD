// SPDX-License-Identifier: LGPL-2.1-or-later

#include "VoxelStock.h"

#include "EndMill.h"
#include "MillPathSegment.h"

#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <Bnd_Box.hxx>
#include <TopAbs_State.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace CAMSimulator
{
namespace
{
class SolidSampler
{
public:
    explicit SolidSampler(const TopoDS_Shape& shape)
    {
        if (shape.ShapeType() == TopAbs_SOLID) {
            mClassifiers.push_back(std::make_unique<BRepClass3d_SolidClassifier>(shape));
        }
        else {
            for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
                mClassifiers.push_back(
                    std::make_unique<BRepClass3d_SolidClassifier>(TopoDS::Solid(it.Current()))
                );
            }
        }
    }

    bool IsValid() const { return !mClassifiers.empty(); }

    bool Contains(const VoxelStock::Point3& point)
    {
        const gp_Pnt sample(point.x, point.y, point.z);
        for (auto& classifier : mClassifiers) {
            classifier->Perform(sample, 1e-7);
            if (classifier->State() == TopAbs_IN || classifier->State() == TopAbs_ON || classifier->IsOnAFace()) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> mClassifiers;
};

double distance(const VoxelStock::Point3& a, const VoxelStock::Point3& b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double radiusAt(const std::vector<VoxelStock::ProfileStation>& profile, double z)
{
    double radius = -1;
    for (std::size_t i = 1; i < profile.size(); ++i) {
        const auto& a = profile[i - 1];
        const auto& b = profile[i];
        const double low = std::min(a.z, b.z);
        const double high = std::max(a.z, b.z);
        if (z < low || z > high) {
            continue;
        }
        const double dz = b.z - a.z;
        const double t = std::abs(dz) < 1e-12 ? 1.0 : std::clamp((z - a.z) / dz, 0.0, 1.0);
        radius = std::max(radius, a.radius + t * (b.radius - a.radius));
    }
    return radius;
}
}  // namespace

void VoxelStock::Clear()
{
    mOccupied.clear();
    mOccupied.shrink_to_fit();
    mMinimum = {};
    mMaximum = {};
    mCellSize = 0;
    mNx = mNy = mNz = 0;
    mInitialVolume = 0;
}

std::size_t VoxelStock::Index(int x, int y, int z) const
{
    return (static_cast<std::size_t>(z) * mNy + y) * mNx + x;
}

bool VoxelStock::ConfigureGrid(
    const Point3& minimum,
    const Point3& maximum,
    double requestedCellSize,
    std::size_t maxCells
)
{
    Clear();
    maxCells = std::min<std::size_t>(maxCells, 2000000);
    if (!std::isfinite(requestedCellSize) || requestedCellSize <= 0 || maxCells == 0
        || !std::isfinite(minimum.x) || !std::isfinite(minimum.y) || !std::isfinite(minimum.z)
        || !std::isfinite(maximum.x) || !std::isfinite(maximum.y) || !std::isfinite(maximum.z)
        || maximum.x <= minimum.x || maximum.y <= minimum.y || maximum.z <= minimum.z) {
        return false;
    }

    mMinimum = minimum;
    mMaximum = maximum;
    const double dx = maximum.x - minimum.x;
    const double dy = maximum.y - minimum.y;
    const double dz = maximum.z - minimum.z;
    const double maxDimension = std::max({dx, dy, dz});
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || !std::isfinite(maxDimension)) {
        return false;
    }
    mCellSize = std::max(requestedCellSize, maxDimension / 256.0);
    if (!std::isfinite(mCellSize) || mCellSize <= 0) return false;
    const auto cellCountAt = [&](double cell) -> long double {
        return std::ceil(dx / cell) * std::ceil(dy / cell) * std::ceil(dz / cell);
    };
    while (cellCountAt(mCellSize) > static_cast<long double>(maxCells)) {
        const long double factor = std::cbrt(cellCountAt(mCellSize) / static_cast<long double>(maxCells));
        mCellSize *= std::max(1.1, static_cast<double>(factor * 1.01L));
        if (!std::isfinite(mCellSize)) return false;
    }
    mNx = std::max(1, static_cast<int>(std::ceil(dx / mCellSize)));
    mNy = std::max(1, static_cast<int>(std::ceil(dy / mCellSize)));
    mNz = std::max(1, static_cast<int>(std::ceil(dz / mCellSize)));
    const std::size_t cellCount = static_cast<std::size_t>(mNx) * mNy * mNz;
    mOccupied.assign(cellCount, 0);
    return true;
}

bool VoxelStock::InitializeBox(const Point3& minimum, const Point3& maximum, double cellSize)
{
    if (!ConfigureGrid(minimum, maximum, cellSize, 2000000)) {
        return false;
    }
    std::fill(mOccupied.begin(), mOccupied.end(), 1);
    mInitialVolume = static_cast<double>(mOccupied.size()) * mCellSize * mCellSize * mCellSize;
    return true;
}

bool VoxelStock::Initialize(
    const TopoDS_Shape& shape,
    double requestedCellSize,
    std::size_t maxCells,
    const std::atomic_bool* cancel,
    const std::function<void(int)>& progress
)
{
    if (shape.IsNull() || !std::isfinite(requestedCellSize) || requestedCellSize <= 0 || maxCells == 0) {
        Clear();
        return false;
    }
    Bnd_Box bounds;
    BRepBndLib::Add(shape, bounds);
    if (bounds.IsVoid()) {
        Clear();
        return false;
    }
    double x0, y0, z0, x1, y1, z1;
    bounds.Get(x0, y0, z0, x1, y1, z1);
    if (!ConfigureGrid({x0, y0, z0}, {x1, y1, z1}, requestedCellSize, maxCells)) {
        return false;
    }

    SolidSampler sampler(shape);
    if (!sampler.IsValid()) {
        Clear();
        return false;
    }
    for (int z = 0; z < mNz; ++z) {
        if (cancel && cancel->load()) {
            Clear();
            return false;
        }
        for (int y = 0; y < mNy; ++y) {
            for (int x = 0; x < mNx; ++x) {
                Point3 center {
                    mMinimum.x + (x + 0.5) * mCellSize,
                    mMinimum.y + (y + 0.5) * mCellSize,
                    mMinimum.z + (z + 0.5) * mCellSize
                };
                mOccupied[Index(x, y, z)] = sampler.Contains(center) ? 1 : 0;
            }
        }
        if (progress) progress(5 + static_cast<int>(35.0 * (z + 1) / mNz));
    }
    mInitialVolume = static_cast<double>(OccupiedCellCount()) * mCellSize * mCellSize * mCellSize;
    if (mInitialVolume <= 0) {
        Clear();
        return false;
    }
    return true;
}

bool VoxelStock::InitializeOnGrid(
    const TopoDS_Shape& shape,
    const VoxelStock& grid,
    bool& shapeWithinGridBounds,
    const std::atomic_bool* cancel,
    const std::function<void(int)>& progress
)
{
    Clear();
    shapeWithinGridBounds = false;
    if (shape.IsNull() || !grid.IsInitialized()) {
        return false;
    }
    Bnd_Box bounds;
    BRepBndLib::Add(shape, bounds);
    if (bounds.IsVoid()) {
        return false;
    }
    double x0, y0, z0, x1, y1, z1;
    bounds.Get(x0, y0, z0, x1, y1, z1);
    const double tolerance = grid.mCellSize * 0.5;
    shapeWithinGridBounds = x0 >= grid.mMinimum.x - tolerance && y0 >= grid.mMinimum.y - tolerance
        && z0 >= grid.mMinimum.z - tolerance && x1 <= grid.mMaximum.x + tolerance
        && y1 <= grid.mMaximum.y + tolerance && z1 <= grid.mMaximum.z + tolerance;

    mMinimum = grid.mMinimum;
    mMaximum = grid.mMaximum;
    mCellSize = grid.mCellSize;
    mNx = grid.mNx;
    mNy = grid.mNy;
    mNz = grid.mNz;
    mOccupied.assign(grid.mOccupied.size(), 0);

    SolidSampler sampler(shape);
    if (!sampler.IsValid()) {
        Clear();
        return false;
    }
    for (int z = 0; z < mNz; ++z) {
        if (cancel && cancel->load()) {
            Clear();
            return false;
        }
        for (int y = 0; y < mNy; ++y) {
            for (int x = 0; x < mNx; ++x) {
                Point3 center {
                    mMinimum.x + (x + 0.5) * mCellSize,
                    mMinimum.y + (y + 0.5) * mCellSize,
                    mMinimum.z + (z + 0.5) * mCellSize
                };
                mOccupied[Index(x, y, z)] = sampler.Contains(center) ? 1 : 0;
            }
        }
        if (progress) progress(45 + static_cast<int>(10.0 * (z + 1) / mNz));
    }
    mInitialVolume = RemainingVolume();
    return mInitialVolume > 0 || !shapeWithinGridBounds;
}

bool VoxelStock::BuildDifferenceMasks(
    const VoxelStock& target,
    VoxelStock& excessMaterial,
    VoxelStock& missingTargetMaterial
) const
{
    if (!IsInitialized() || !target.IsInitialized() || mNx != target.mNx || mNy != target.mNy
        || mNz != target.mNz || std::abs(mCellSize - target.mCellSize) > 1e-9
        || mMinimum.x != target.mMinimum.x || mMinimum.y != target.mMinimum.y || mMinimum.z != target.mMinimum.z
        || mMaximum.x != target.mMaximum.x || mMaximum.y != target.mMaximum.y || mMaximum.z != target.mMaximum.z) {
        return false;
    }
    excessMaterial = *this;
    missingTargetMaterial = *this;
    std::size_t excessCount = 0;
    std::size_t missingCount = 0;
    for (std::size_t i = 0; i < mOccupied.size(); ++i) {
        const bool simulated = mOccupied[i] != 0;
        const bool wanted = target.mOccupied[i] != 0;
        const bool excess = simulated && !wanted;
        const bool missing = wanted && !simulated;
        excessMaterial.mOccupied[i] = excess ? 1 : 0;
        missingTargetMaterial.mOccupied[i] = missing ? 1 : 0;
        excessCount += excess;
        missingCount += missing;
    }
    excessMaterial.mInitialVolume = static_cast<double>(excessCount) * mCellSize * mCellSize * mCellSize;
    missingTargetMaterial.mInitialVolume = static_cast<double>(missingCount) * mCellSize * mCellSize * mCellSize;
    return true;
}

bool VoxelStock::RemoveSweep(
    const std::vector<ProfileStation>& profile,
    const Point3& from,
    const Point3& to,
    std::size_t maxSamples
)
{
    if (!IsInitialized() || profile.size() < 2 || maxSamples == 0) {
        return false;
    }
    if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z)
        || !std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z)) {
        return false;
    }
    constexpr std::size_t hardMaxSamples = 1000000;
    maxSamples = std::min(maxSamples, hardMaxSamples);
    double maxRadius = 0;
    double minZ = std::numeric_limits<double>::infinity();
    double maxZ = -std::numeric_limits<double>::infinity();
    for (const auto& station : profile) {
        if (!std::isfinite(station.radius) || !std::isfinite(station.z) || station.radius < 0) {
            return false;
        }
        maxRadius = std::max(maxRadius, station.radius);
        minZ = std::min(minZ, station.z);
        maxZ = std::max(maxZ, station.z);
    }
    const double length = distance(from, to);
    const double requiredSamples = std::max(1.0, std::ceil(length / (mCellSize * 0.5)));
    if (requiredSamples > static_cast<double>(maxSamples)) {
        return false;
    }
    const std::size_t samples = static_cast<std::size_t>(requiredSamples);
    bool removedAny = false;
    for (std::size_t sample = 0; sample <= samples; ++sample) {
        const double t = samples == 0 ? 0 : static_cast<double>(sample) / samples;
        const Point3 position {
            from.x + t * (to.x - from.x),
            from.y + t * (to.y - from.y),
            from.z + t * (to.z - from.z)
        };
        const int x0 = std::max(0, static_cast<int>(std::floor((position.x - maxRadius - mMinimum.x) / mCellSize)));
        const int x1 = std::min(mNx - 1, static_cast<int>(std::floor((position.x + maxRadius - mMinimum.x) / mCellSize)));
        const int y0 = std::max(0, static_cast<int>(std::floor((position.y - maxRadius - mMinimum.y) / mCellSize)));
        const int y1 = std::min(mNy - 1, static_cast<int>(std::floor((position.y + maxRadius - mMinimum.y) / mCellSize)));
        const int z0 = std::max(0, static_cast<int>(std::floor((position.z + minZ - mMinimum.z) / mCellSize)));
        const int z1 = std::min(mNz - 1, static_cast<int>(std::floor((position.z + maxZ - mMinimum.z) / mCellSize)));
        for (int z = z0; z <= z1; ++z) {
            const double centerZ = mMinimum.z + (z + 0.5) * mCellSize - position.z;
            const double radius = radiusAt(profile, centerZ);
            if (radius < 0) continue;
            for (int y = y0; y <= y1; ++y) {
                const double dy = mMinimum.y + (y + 0.5) * mCellSize - position.y;
                for (int x = x0; x <= x1; ++x) {
                    const double dx = mMinimum.x + (x + 0.5) * mCellSize - position.x;
                    if (dx * dx + dy * dy <= radius * radius && mOccupied[Index(x, y, z)]) {
                        mOccupied[Index(x, y, z)] = 0;
                        removedAny = true;
                    }
                }
            }
        }
    }
    return removedAny;
}

bool VoxelStock::RemoveToolSegment(
    const EndMill& tool,
    const MillPathSegment& segment,
    std::size_t maxSamples,
    const std::atomic_bool* cancel
)
{
    if (!IsInitialized() || tool.nPoints < 2 || segment.numSimSteps < 1 || maxSamples == 0) {
        return false;
    }
    const int profilePointCount = PROFILE_BUFFER_POINTS(tool.nPoints);
    if (profilePointCount < 2 || tool.profilePoints.size() < static_cast<std::size_t>(profilePointCount * 2)) {
        return false;
    }
    std::vector<ProfileStation> profile;
    profile.reserve(profilePointCount);
    for (int i = 0; i < profilePointCount; ++i) {
        profile.push_back({
            std::abs(tool.profilePoints[2 * i]),
            tool.profilePoints[2 * i + 1]
        });
    }

    const std::size_t sampleBudget = std::min<std::size_t>(maxSamples, 1000000);
    std::size_t requiredTotal = 0;
    for (int step = 0; step < segment.numSimSteps; ++step) {
        if (cancel && cancel->load()) return false;
        vec3 fromPosition, toPosition;
        segment.GetHeadPosition(fromPosition, static_cast<float>(step) / segment.numSimSteps);
        segment.GetHeadPosition(toPosition, static_cast<float>(step + 1) / segment.numSimSteps);
        const Point3 from {fromPosition[0], fromPosition[1], fromPosition[2]};
        const Point3 to {toPosition[0], toPosition[1], toPosition[2]};
        const double span = distance(from, to);
        const double neededValue = std::max(1.0, std::ceil(span / (mCellSize * 0.5)));
        if (!std::isfinite(neededValue) || neededValue > static_cast<double>(sampleBudget - requiredTotal)) {
            return false;
        }
        requiredTotal += static_cast<std::size_t>(neededValue);
    }
    for (int step = 0; step < segment.numSimSteps; ++step) {
        if (cancel && cancel->load()) return false;
        vec3 fromPosition, toPosition;
        segment.GetHeadPosition(fromPosition, static_cast<float>(step) / segment.numSimSteps);
        segment.GetHeadPosition(toPosition, static_cast<float>(step + 1) / segment.numSimSteps);
        const Point3 from {fromPosition[0], fromPosition[1], fromPosition[2]};
        const Point3 to {toPosition[0], toPosition[1], toPosition[2]};
        const auto needed = static_cast<std::size_t>(
            std::max(1.0, std::ceil(distance(from, to) / (mCellSize * 0.5)))
        );
        RemoveSweep(profile, from, to, needed);
    }
    return true;
}

bool VoxelStock::ExtractSurface(
    std::vector<MeshVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    std::size_t maxTriangles,
    const std::atomic_bool* cancel,
    const std::function<void(int)>& progress
) const
{
    vertices.clear();
    indices.clear();
    if (!IsInitialized() || maxTriangles == 0) {
        return false;
    }
    struct Face
    {
        int dx, dy, dz;
        std::array<std::array<int, 3>, 4> corners;
    };
    static constexpr std::array<Face, 6> faces {{
        {1, 0, 0, {{{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}}},
        {-1, 0, 0, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}},
        {0, 1, 0, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}}},
        {0, -1, 0, {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}}},
        {0, 0, 1, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}},
        {0, 0, -1, {{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}}},
    }};
    for (int z = 0; z < mNz; ++z) {
        if (cancel && cancel->load()) {
            vertices.clear();
            indices.clear();
            return false;
        }
        for (int y = 0; y < mNy; ++y) {
            for (int x = 0; x < mNx; ++x) {
                if (!IsOccupied(x, y, z)) continue;
                for (const auto& face : faces) {
                    if (IsOccupied(x + face.dx, y + face.dy, z + face.dz)) continue;
                    if (indices.size() / 3 + 2 > maxTriangles) {
                        vertices.clear();
                        indices.clear();
                        return false;
                    }
                    const auto first = static_cast<std::uint32_t>(vertices.size());
                    for (const auto& corner : face.corners) {
                        vertices.push_back({
                            static_cast<float>(mMinimum.x + (x + corner[0]) * mCellSize),
                            static_cast<float>(mMinimum.y + (y + corner[1]) * mCellSize),
                            static_cast<float>(mMinimum.z + (z + corner[2]) * mCellSize),
                            static_cast<float>(face.dx),
                            static_cast<float>(face.dy),
                            static_cast<float>(face.dz),
                        });
                    }
                    indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
                }
            }
        }
        if (progress) progress(85 + static_cast<int>(13.0 * (z + 1) / mNz));
    }
    return !indices.empty();
}

bool VoxelStock::IsOccupied(int x, int y, int z) const
{
    return x >= 0 && y >= 0 && z >= 0 && x < mNx && y < mNy && z < mNz && mOccupied[Index(x, y, z)] != 0;
}

std::size_t VoxelStock::OccupiedCellCount() const
{
    return static_cast<std::size_t>(std::count(mOccupied.begin(), mOccupied.end(), std::uint8_t {1}));
}

double VoxelStock::RemainingVolume() const
{
    return static_cast<double>(OccupiedCellCount()) * mCellSize * mCellSize * mCellSize;
}

}  // namespace CAMSimulator
