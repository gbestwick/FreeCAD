// SPDX-License-Identifier: LGPL-2.1-or-later

#include "HolderCollisionCheck.h"

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
#include <cstdint>
#include <memory>
#include <sstream>

namespace CAMSimulator
{
namespace
{
constexpr std::uint64_t MaxStockCells = 20000;
constexpr std::size_t MaxShapeCells = 5000;
constexpr std::uint64_t MaxAnalysisSamples = 5000000;

struct CellPoint
{
    float x;
    float y;
    float z;
};

struct SampledShape
{
    std::vector<CellPoint> points;
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
    float pitch = 0;
};

struct Grid
{
    float x0 = 0;
    float y0 = 0;
    float z0 = 0;
    float cell = 1;
    int nx = 0;
    int ny = 0;
    int nz = 0;
    std::vector<unsigned char> occupied;

    std::size_t index(int x, int y, int z) const
    {
        return (static_cast<std::size_t>(z) * ny + y) * nx + x;
    }

    bool contains(int x, int y, int z) const
    {
        return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz;
    }

    bool at(int x, int y, int z) const
    {
        return contains(x, y, z) && occupied[index(x, y, z)] != 0;
    }

    void remove(int x, int y, int z)
    {
        if (contains(x, y, z)) {
            occupied[index(x, y, z)] = 0;
        }
    }

    CellPoint center(int x, int y, int z) const
    {
        return {x0 + (x + 0.5f) * cell, y0 + (y + 0.5f) * cell, z0 + (z + 0.5f) * cell};
    }

    bool cellAt(float x, float y, float z, int& ix, int& iy, int& iz) const
    {
        ix = static_cast<int>(std::floor((x - x0) / cell));
        iy = static_cast<int>(std::floor((y - y0) / cell));
        iz = static_cast<int>(std::floor((z - z0) / cell));
        return contains(ix, iy, iz);
    }
};

class ShapeSampler
{
public:
    explicit ShapeSampler(const TopoDS_Shape& shape)
    {
        if (shape.ShapeType() == TopAbs_SOLID) {
            mClassifiers.push_back(std::make_unique<BRepClass3d_SolidClassifier>(shape));
            return;
        }
        for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
            auto classifier = std::make_unique<BRepClass3d_SolidClassifier>(TopoDS::Solid(it.Current()));
            mClassifiers.push_back(std::move(classifier));
        }
    }

    bool valid() const { return !mClassifiers.empty(); }

    bool contains(float x, float y, float z)
    {
        const gp_Pnt point(x, y, z);
        for (auto& classifier : mClassifiers) {
            classifier->Perform(point, 1e-6);
            const auto state = classifier->State();
            if (state == TopAbs_IN || state == TopAbs_ON || classifier->IsOnAFace()) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> mClassifiers;
};

bool getBounds(const TopoDS_Shape& shape, float& x0, float& y0, float& z0, float& x1, float& y1, float& z1)
{
    if (shape.IsNull()) {
        return false;
    }
    Bnd_Box bounds;
    BRepBndLib::Add(shape, bounds);
    if (bounds.IsVoid()) {
        return false;
    }
    Standard_Real a, b, c, d, e, f;
    bounds.Get(a, b, c, d, e, f);
    x0 = static_cast<float>(a);
    y0 = static_cast<float>(b);
    z0 = static_cast<float>(c);
    x1 = static_cast<float>(d);
    y1 = static_cast<float>(e);
    z1 = static_cast<float>(f);
    return x1 > x0 && y1 > y0 && z1 > z0;
}

bool makeGrid(
    const TopoDS_Shape& shape,
    float requestedCell,
    Grid& grid,
    const std::atomic_bool* cancel,
    const std::function<void(int)>& progress
)
{
    float x1, y1, z1;
    if (!getBounds(shape, grid.x0, grid.y0, grid.z0, x1, y1, z1)) {
        return false;
    }
    const float maxDimension = std::max({x1 - grid.x0, y1 - grid.y0, z1 - grid.z0});
    grid.cell = std::max(requestedCell, maxDimension / 64.0f);
    grid.cell = std::max(grid.cell, 0.25f);
    grid.nx = std::max(1, static_cast<int>(std::ceil((x1 - grid.x0) / grid.cell)));
    grid.ny = std::max(1, static_cast<int>(std::ceil((y1 - grid.y0) / grid.cell)));
    grid.nz = std::max(1, static_cast<int>(std::ceil((z1 - grid.z0) / grid.cell)));
    while (static_cast<std::uint64_t>(grid.nx) * grid.ny * grid.nz > MaxStockCells) {
        grid.cell *= 1.2f;
        grid.nx = std::max(1, static_cast<int>(std::ceil((x1 - grid.x0) / grid.cell)));
        grid.ny = std::max(1, static_cast<int>(std::ceil((y1 - grid.y0) / grid.cell)));
        grid.nz = std::max(1, static_cast<int>(std::ceil((z1 - grid.z0) / grid.cell)));
    }

    grid.occupied.assign(static_cast<std::size_t>(grid.nx) * grid.ny * grid.nz, 0);
    ShapeSampler sampler(shape);
    if (!sampler.valid()) {
        return false;
    }
    std::uint64_t checkedCells = 0;
    for (int z = 0; z < grid.nz; ++z) {
        if (cancel && cancel->load()) return false;
        for (int y = 0; y < grid.ny; ++y) {
            for (int x = 0; x < grid.nx; ++x) {
                if ((checkedCells++ & 127) == 0 && cancel && cancel->load()) return false;
                const auto point = grid.center(x, y, z);
                bool inside = sampler.contains(point.x, point.y, point.z);
                // Include cell corners so thin stock near a cell boundary is less likely
                // to disappear from the conservative occupancy approximation.
                for (int dz : {0, 1}) {
                    for (int dy : {0, 1}) {
                        for (int dx : {0, 1}) {
                            if (inside) break;
                            const float sx = grid.x0 + (x + dx) * grid.cell;
                            const float sy = grid.y0 + (y + dy) * grid.cell;
                            const float sz = grid.z0 + (z + dz) * grid.cell;
                            inside = sampler.contains(sx, sy, sz);
                        }
                    }
                }
                grid.occupied[grid.index(x, y, z)] = inside;
            }
        }
        if (progress) {
            progress(5 + static_cast<int>(20.0 * (z + 1) / grid.nz));
        }
    }
    // Dilate the stock by one voxel. Combined with the holder neighborhood check,
    // this favors a conservative warning over a missed coarse-grid contact.
    const auto original = grid.occupied;
    for (int z = 0; z < grid.nz; ++z) {
        for (int y = 0; y < grid.ny; ++y) {
            for (int x = 0; x < grid.nx; ++x) {
                if (original[grid.index(x, y, z)] == 0) continue;
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (grid.contains(x + dx, y + dy, z + dz)) {
                                grid.occupied[grid.index(x + dx, y + dy, z + dz)] = 1;
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

SampledShape sampleShape(const TopoDS_Shape& shape, float cell, const std::atomic_bool* cancel)
{
    SampledShape sampled;
    float x0, y0, z0, x1, y1, z1;
    if (!getBounds(shape, x0, y0, z0, x1, y1, z1)) {
        return sampled;
    }
    ShapeSampler sampler(shape);
    if (!sampler.valid()) {
        return sampled;
    }

    const float maxDimension = std::max({x1 - x0, y1 - y0, z1 - z0});
    float step = std::max(cell, maxDimension / 40.0f);
    auto sampleCount = [&]() {
        const int nx = std::max(1, static_cast<int>(std::ceil((x1 - x0) / step)));
        const int ny = std::max(1, static_cast<int>(std::ceil((y1 - y0) / step)));
        const int nz = std::max(1, static_cast<int>(std::ceil((z1 - z0) / step)));
        return static_cast<std::uint64_t>(nx) * ny * nz;
    };
    while (sampleCount() > MaxShapeCells) {
        step *= 1.1f;
    }
    sampled.pitch = step;
    const int nx = std::max(1, static_cast<int>(std::ceil((x1 - x0) / step)));
    const int ny = std::max(1, static_cast<int>(std::ceil((y1 - y0) / step)));
    const int nz = std::max(1, static_cast<int>(std::ceil((z1 - z0) / step)));
    for (int z = 0; z < nz; ++z) {
        if (cancel && cancel->load()) return {};
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const CellPoint point {x0 + (x + 0.5f) * step,
                    y0 + (y + 0.5f) * step,
                    z0 + (z + 0.5f) * step};
                if (sampler.contains(point.x, point.y, point.z)) {
                    sampled.points.push_back(point);
                }
            }
        }
    }
    sampled.minX = x0; sampled.minY = y0; sampled.minZ = z0;
    sampled.maxX = x1; sampled.maxY = y1; sampled.maxZ = z1;
    return sampled;
}

float profileRadius(const EndMill& tool, float z)
{
    if (tool.nPoints < 2 || tool.profilePoints.size() < static_cast<std::size_t>(tool.nPoints * 2)) {
        return 0;
    }
    for (int i = 0; i < tool.nPoints - 1; ++i) {
        const float r0 = tool.profilePoints[2 * i];
        const float z0 = tool.profilePoints[2 * i + 1];
        const float r1 = tool.profilePoints[2 * (i + 1)];
        const float z1 = tool.profilePoints[2 * (i + 1) + 1];
        const float low = std::min(z0, z1);
        const float high = std::max(z0, z1);
        if (z >= low && z <= high) {
            const float t = std::abs(z1 - z0) < 1e-6f ? 0 : (z - z0) / (z1 - z0);
            return std::max(0.0f, r0 + t * (r1 - r0));
        }
    }
    return 0;
}

float profileMaxRadius(const EndMill& tool)
{
    float radius = std::max(tool.radius, 0.01f);
    for (int i = 0; i < tool.nPoints; ++i) {
        radius = std::max(radius, tool.profilePoints[2 * i]);
    }
    return radius;
}

bool overlapsGrid(const Grid& grid, const SampledShape& shape, float px, float py, float pz)
{
    return px + shape.maxX >= grid.x0 && px + shape.minX <= grid.x0 + grid.nx * grid.cell
        && py + shape.maxY >= grid.y0 && py + shape.minY <= grid.y0 + grid.ny * grid.cell
        && pz + shape.maxZ >= grid.z0 && pz + shape.minZ <= grid.z0 + grid.nz * grid.cell;
}

bool collidesWithVoxels(
    const Grid& grid,
    const SampledShape& shape,
    float px,
    float py,
    float pz,
    bool conservativeNeighbourhood,
    std::uint64_t& remainingSamples,
    bool& budgetExhausted
)
{
    // A one-cell neighbourhood is a conservative allowance for the discretized
    // holder/stock surfaces. It can report near misses within roughly one cell.
    if (!overlapsGrid(grid, shape, px, py, pz)) {
        return false;
    }
    for (const auto& point : shape.points) {
        if (remainingSamples == 0) {
            budgetExhausted = true;
            return false;
        }
        --remainingSamples;
        int ix, iy, iz;
        if (!grid.cellAt(px + point.x, py + point.y, pz + point.z, ix, iy, iz)) {
            continue;
        }
        const int pad = conservativeNeighbourhood ? 1 : 0;
        for (int dz = -pad; dz <= pad; ++dz) {
            for (int dy = -pad; dy <= pad; ++dy) {
                for (int dx = -pad; dx <= pad; ++dx) {
                    if (grid.at(ix + dx, iy + dy, iz + dz)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

SampledShape sampleCutter(const EndMill& tool, float cell)
{
    SampledShape sampled;
    float maxZ = 0;
    for (int i = 0; i < tool.nPoints; ++i) {
        maxZ = std::max(maxZ, tool.profilePoints[2 * i + 1]);
    }
    const float radius = profileMaxRadius(tool);
    float step = std::max(cell, std::max(radius, maxZ) / 40.0f);
    auto sampleCount = [&]() {
        const int n = std::max(1, static_cast<int>(std::ceil(2 * radius / step)));
        const int nz = std::max(1, static_cast<int>(std::ceil(maxZ / step)));
        return static_cast<std::uint64_t>(n) * n * nz;
    };
    while (sampleCount() > MaxShapeCells) {
        step *= 1.1f;
    }
    sampled.pitch = step;
    sampled.minX = sampled.minY = -radius;
    sampled.maxX = sampled.maxY = radius;
    sampled.minZ = 0;
    sampled.maxZ = maxZ;
    const int nx = std::max(1, static_cast<int>(std::ceil(2 * radius / step)));
    const int ny = nx;
    const int nz = std::max(1, static_cast<int>(std::ceil(maxZ / step)));
    for (int z = 0; z < nz; ++z) {
        const float pz = (z + 0.5f) * step;
        const float profile = profileRadius(tool, pz);
        for (int y = 0; y < ny; ++y) {
            const float py = -radius + (y + 0.5f) * step;
            for (int x = 0; x < nx; ++x) {
                const float px = -radius + (x + 0.5f) * step;
                if (px * px + py * py <= profile * profile) {
                    sampled.points.push_back({px, py, pz});
                }
            }
        }
    }
    return sampled;
}

void removeCutterAt(
    Grid& grid,
    const SampledShape& cutter,
    float px,
    float py,
    float pz,
    std::uint64_t& remainingSamples,
    bool& budgetExhausted
)
{
    if (!overlapsGrid(grid, cutter, px, py, pz)) {
        return;
    }
    for (const auto& point : cutter.points) {
        if (remainingSamples == 0) {
            budgetExhausted = true;
            return;
        }
        --remainingSamples;
        int ix, iy, iz;
        if (grid.cellAt(px + point.x, py + point.y, pz + point.z, ix, iy, iz)) {
            grid.remove(ix, iy, iz);
        }
    }
}

}  // namespace

std::vector<SimulationIssue> HolderCollisionCheck::Analyze(
    const TopoDS_Shape& stock,
    const std::unordered_map<int, TopoDS_Shape>& holders,
    const std::vector<MillPathSegment*>& segments,
    float requestedCellSize,
    const std::atomic_bool* cancel,
    const std::function<void(int)>& progress
) const
{
    std::vector<SimulationIssue> issues;
    if (progress) progress(0);
    if (holders.empty()) {
        issues.push_back({-1, IssueSeverity::Warning, IssueKind::CollisionCoverage,
            "Holder/fixture collision checking was not run: no tool assemblies are assigned."});
        return issues;
    }

    Grid grid;
    if (!makeGrid(stock, requestedCellSize, grid, cancel, progress)) {
        if (cancel && cancel->load()) {
            issues.push_back({-1, IssueSeverity::Warning, IssueKind::CollisionCoverage,
                "Holder collision check was cancelled before motion analysis completed; unchecked motion is not clear."});
            if (progress) progress(100);
            return issues;
        }
        issues.push_back({0, IssueSeverity::Warning, IssueKind::CollisionCoverage,
            "Holder collision checking was not run: the stock could not be represented by the experimental voxel model."});
        return issues;
    }

    std::unordered_map<int, SampledShape> holderVoxels;
    std::unordered_map<int, SampledShape> cutterVoxels;
    float effectivePitch = grid.cell;
    std::size_t shapeIndex = 0;
    for (const auto& [toolId, shape] : holders) {
        if (cancel && cancel->load()) break;
        auto sampled = sampleShape(shape, grid.cell, cancel);
        effectivePitch = std::max(effectivePitch, sampled.pitch);
        holderVoxels.emplace(toolId, std::move(sampled));
        ++shapeIndex;
        if (progress) progress(25 + static_cast<int>(10.0 * shapeIndex / holders.size()));
    }

    std::unordered_map<int, bool> reported;
    std::uint64_t remainingSamples = MaxAnalysisSamples;
    bool budgetExhausted = false;
    std::size_t segmentIndex = 0;
    for (auto* segment : segments) {
        if (budgetExhausted || (cancel && cancel->load())) {
            break;
        }
        if (!segment || !segment->endmill) {
            continue;
        }
        const auto holder = holderVoxels.find(segment->endmill->toolId);
        if (holder == holderVoxels.end() || holder->second.points.empty()) {
            continue;
        }
        auto cutter = cutterVoxels.find(segment->endmill->toolId);
        if (cutter == cutterVoxels.end()) {
            auto sampled = sampleCutter(*segment->endmill, grid.cell);
            effectivePitch = std::max(effectivePitch, sampled.pitch);
            cutter = cutterVoxels.emplace(segment->endmill->toolId, std::move(sampled)).first;
        }

        const float sourceResolution = MillPathSegment::SamplingResolution();
        const int subdivisions = std::max(1, static_cast<int>(std::ceil(
            sourceResolution / std::max(grid.cell * 0.5f, 0.05f))));
        const int samples = std::max(1, segment->numSimSteps * subdivisions);
        vec3 position;
        for (int sample = 0; sample <= samples; ++sample) {
            if (budgetExhausted || (cancel && cancel->load())) {
                break;
            }
            const float progress = static_cast<float>(sample) / samples;
            segment->GetHeadPosition(position, progress);
            const float px = position[0], py = position[1], pz = position[2];

            if (segment->rapid) {
                if (!reported[segment->sourceCommand]
                    && (collidesWithVoxels(grid, cutter->second, px, py, pz, true,
                                           remainingSamples, budgetExhausted)
                        || collidesWithVoxels(grid, holder->second, px, py, pz, true,
                                              remainingSamples, budgetExhausted))) {
                    std::ostringstream message;
                    message << "Possible rapid tool/holder-to-stock collision for T" << segment->endmill->toolId
                            << " near X" << px << " Y" << py << " Z" << pz
                            << " mm (voxel pitch " << grid.cell << " mm).";
                    issues.push_back({segment->sourceCommand, IssueSeverity::Error,
                        IssueKind::HolderStockCollision, message.str()});
                    reported[segment->sourceCommand] = true;
                }
            }
            else {
                removeCutterAt(grid, cutter->second, px, py, pz,
                               remainingSamples, budgetExhausted);
                if (!reported[segment->sourceCommand]
                    && collidesWithVoxels(grid, holder->second, px, py, pz, true,
                                          remainingSamples, budgetExhausted)) {
                    std::ostringstream message;
                    message << "Possible holder-to-stock collision for T" << segment->endmill->toolId
                            << " near X" << px << " Y" << py << " Z" << pz
                            << " mm (voxel pitch " << grid.cell << " mm).";
                    issues.push_back({segment->sourceCommand, IssueSeverity::Error,
                        IssueKind::HolderStockCollision, message.str()});
                    reported[segment->sourceCommand] = true;
                }
            }
        }
        ++segmentIndex;
        if (progress) {
            progress(35 + static_cast<int>(65.0 * segmentIndex / std::max<std::size_t>(1, segments.size())));
        }
    }

    std::ostringstream coverage;
    coverage << "Experimental holder-to-stock check used a " << grid.cell
             << " mm stock grid (sampled geometry pitch up to " << effectivePitch
             << " mm; capped at " << MaxStockCells << " stock cells, " << MaxShapeCells
             << " samples per shape, and " << MaxAnalysisSamples
             << " motion/shape samples) and axisymmetric cutter removal. Only tools with assigned assemblies were checked. "
                "It covers canonical +Z 3-axis motion. Voxel dilation can report close-but-clear overlaps; contacts smaller than the sampling pitch can still be missed. Fixtures, rotary motion, and exact shank cutting limits are not checked. This is an experimental warning, not a clearance guarantee.";
    if (budgetExhausted) {
        coverage << " Analysis stopped at its bounded work limit; later motion was not checked, so this result is incomplete.";
    }
    if (cancel && cancel->load()) {
        coverage << " Analysis was cancelled; later motion was not checked, so this result is incomplete.";
    }
    issues.push_back({-1, IssueSeverity::Warning, IssueKind::CollisionCoverage, coverage.str()});
    if (progress) progress(100);
    return issues;
}

}  // namespace CAMSimulator
