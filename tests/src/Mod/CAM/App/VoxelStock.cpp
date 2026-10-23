// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Mod/CAM/PathSimulator/AppGL/VoxelStock.h>
#include <Mod/CAM/PathSimulator/AppGL/EndMill.h>
#include <Mod/CAM/PathSimulator/AppGL/MillPathSegment.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>

using CAMSimulator::VoxelStock;
using namespace CAMSimulator;

TEST(CAMSimulatorVoxelStock, InitializesFromCadSolidAndReportsVoxelVolume)
{
    VoxelStock stock;
    const auto box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 4, 4, 4).Shape();
    ASSERT_TRUE(stock.Initialize(box, 1.0));
    EXPECT_EQ(stock.OccupiedCellCount(), 64);
    EXPECT_DOUBLE_EQ(stock.InitialVolume(), 64.0);
    EXPECT_DOUBLE_EQ(stock.RemainingVolume(), 64.0);
}

TEST(CAMSimulatorVoxelStock, AxisymmetricSweepRemovesMaterialAndProducesSurface)
{
    VoxelStock stock;
    ASSERT_TRUE(stock.InitializeBox({0, 0, 0}, {10, 10, 10}, 1.0));
    const double originalVolume = stock.RemainingVolume();
    const std::vector<VoxelStock::ProfileStation> cutter {{1.0, 0.0}, {1.0, 10.0}};
    ASSERT_TRUE(stock.RemoveSweep(cutter, {5, 5, 0}, {5, 5, 10}));
    EXPECT_LT(stock.RemainingVolume(), originalVolume);
    EXPECT_TRUE(stock.IsOccupied(0, 0, 5));
    EXPECT_FALSE(stock.IsOccupied(5, 5, 5));

    std::vector<VoxelStock::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    ASSERT_TRUE(stock.ExtractSurface(vertices, indices));
    ASSERT_FALSE(indices.empty());
    EXPECT_EQ(indices.size() % 3, 0);
    EXPECT_EQ(vertices.size() % 4, 0);
}

TEST(CAMSimulatorVoxelStock, RejectsTruncatedSweepsAndBoundsSurfaceOutput)
{
    VoxelStock stock;
    ASSERT_TRUE(stock.InitializeBox({0, 0, 0}, {20, 20, 20}, 1.0));
    const std::vector<VoxelStock::ProfileStation> cutter {{1.0, 0.0}, {1.0, 2.0}};
    EXPECT_FALSE(stock.RemoveSweep(cutter, {0, 10, 10}, {20, 10, 10}, 2));
    EXPECT_DOUBLE_EQ(stock.RemainingVolume(), stock.InitialVolume());

    std::vector<VoxelStock::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    EXPECT_FALSE(stock.ExtractSurface(vertices, indices, 12));
    EXPECT_TRUE(vertices.empty());
    EXPECT_TRUE(indices.empty());
}

TEST(CAMSimulatorVoxelStock, UsesTheSimulatorToolProfileAndSegmentInterpolation)
{
    VoxelStock stock;
    ASSERT_TRUE(stock.InitializeBox({0, 0, 0}, {10, 10, 10}, 1.0));
    EndMill tool({1, 10, 1, 0, 0, 0}, 1, 2);
    MillMotion from, to;
    from.x = to.x = 5;
    from.y = to.y = 5;
    from.z = 0;
    from.spindleDirection = 1;
    to.z = 5;
    to.spindleDirection = 1;
    MillPathSegment segment(tool, from, to);
    EXPECT_EQ(segment.spindleDirection, 1);

    ASSERT_TRUE(stock.RemoveToolSegment(tool, segment));
    EXPECT_FALSE(stock.IsOccupied(5, 5, 5));
    EXPECT_TRUE(stock.IsOccupied(0, 0, 5));
}

TEST(CAMSimulatorVoxelStock, ComparesStockAndTargetOnTheSameGrid)
{
    VoxelStock simulated;
    VoxelStock target;
    VoxelStock excess;
    VoxelStock missing;
    const auto stockBox = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 4, 4, 4).Shape();
    const auto targetBox = BRepPrimAPI_MakeBox(gp_Pnt(1, 1, 1), 2, 2, 2).Shape();
    ASSERT_TRUE(simulated.Initialize(stockBox, 1.0));
    bool targetWithinGrid = false;
    ASSERT_TRUE(target.InitializeOnGrid(targetBox, simulated, targetWithinGrid));
    EXPECT_TRUE(targetWithinGrid);
    ASSERT_TRUE(simulated.BuildDifferenceMasks(target, excess, missing));
    EXPECT_DOUBLE_EQ(excess.RemainingVolume(), 56.0);
    EXPECT_DOUBLE_EQ(missing.RemainingVolume(), 0.0);
    std::vector<VoxelStock::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    EXPECT_TRUE(excess.ExtractSurface(vertices, indices));
    EXPECT_FALSE(indices.empty());
    EXPECT_FALSE(missing.ExtractSurface(vertices, indices));
}

TEST(CAMSimulatorVoxelStock, ReportsMissingTargetMaterialAfterRemoval)
{
    VoxelStock target;
    VoxelStock simulated;
    VoxelStock targetOnGrid;
    VoxelStock excess;
    VoxelStock missing;
    const auto box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 4, 4, 4).Shape();
    ASSERT_TRUE(target.Initialize(box, 1.0));
    simulated = target;
    const std::vector<VoxelStock::ProfileStation> cutter {{1.1, 0.0}, {1.1, 4.0}};
    ASSERT_TRUE(simulated.RemoveSweep(cutter, {2, 2, 0}, {2, 2, 4}));
    bool targetWithinGrid = false;
    ASSERT_TRUE(targetOnGrid.InitializeOnGrid(box, simulated, targetWithinGrid));
    EXPECT_TRUE(targetWithinGrid);
    ASSERT_TRUE(simulated.BuildDifferenceMasks(targetOnGrid, excess, missing));
    EXPECT_DOUBLE_EQ(excess.RemainingVolume(), 0.0);
    EXPECT_GT(missing.RemainingVolume(), 0.0);
}

TEST(CAMSimulatorVoxelStock, ReportsTargetOutsideComparisonBounds)
{
    VoxelStock stock;
    VoxelStock target;
    const auto stockBox = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 4, 4, 4).Shape();
    const auto overhangingTarget = BRepPrimAPI_MakeBox(gp_Pnt(-1, 1, 1), 2, 2, 2).Shape();
    ASSERT_TRUE(stock.Initialize(stockBox, 1.0));
    bool targetWithinGrid = true;
    ASSERT_TRUE(target.InitializeOnGrid(overhangingTarget, stock, targetWithinGrid));
    EXPECT_FALSE(targetWithinGrid);
    EXPECT_GT(target.OccupiedCellCount(), 0);
}
