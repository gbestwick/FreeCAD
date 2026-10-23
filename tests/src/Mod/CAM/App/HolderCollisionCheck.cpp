// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Mod/CAM/PathSimulator/AppGL/EndMill.h>
#include <Mod/CAM/PathSimulator/AppGL/HolderCollisionCheck.h>
#include <Mod/CAM/PathSimulator/AppGL/MillMotion.h>
#include <Mod/CAM/PathSimulator/AppGL/MillPathSegment.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>
#include <algorithm>
#include <atomic>

using namespace CAMSimulator;

namespace
{
TopoDS_Shape makeStock()
{
    return BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 10, 10).Shape();
}

TopoDS_Shape makeHolder()
{
    return BRepPrimAPI_MakeBox(gp_Pnt(-1, -1, 0), 2, 2, 5).Shape();
}

std::vector<SimulationIssue> analyzeRapid(bool crossStock)
{
    EndMill tool({1, 10, 1, 0}, 1, 2);
    MillMotion from, to;
    from.x = crossStock ? 5 : 20;
    from.y = 5;
    from.z = 20;
    from.tool = 1;
    from.sourceCommand = 3;
    to = from;
    to.z = 0;
    to.rapid = true;
    to.sourceCommand = 4;

    MillPathSegment segment(tool, from, to);
    std::unordered_map<int, TopoDS_Shape> holders {{1, makeHolder()}};
    HolderCollisionCheck check;
    return check.Analyze(makeStock(), holders, {&segment}, 0.5f);
}
}  // namespace

TEST(CAMSimulatorHolderCollision, FindsRapidIntersection)
{
    const auto issues = analyzeRapid(true);
    EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.kind == IssueKind::HolderStockCollision && issue.command == 4;
    }));
}

TEST(CAMSimulatorHolderCollision, DoesNotFlagClearRapid)
{
    const auto issues = analyzeRapid(false);
    EXPECT_FALSE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.kind == IssueKind::HolderStockCollision;
    }));
    EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.kind == IssueKind::CollisionCoverage;
    }));
}

TEST(CAMSimulatorHolderCollision, CancellationReportsUncheckedCoverage)
{
    EndMill tool({1, 10, 1, 0}, 1, 2);
    MillMotion motion;
    motion.tool = 1;
    MillPathSegment segment(tool, motion, motion);
    std::unordered_map<int, TopoDS_Shape> holders {{1, makeHolder()}};
    std::atomic_bool cancel {true};
    HolderCollisionCheck check;
    const auto issues = check.Analyze(makeStock(), holders, {&segment}, 0.5f, &cancel);

    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues.back().kind, IssueKind::CollisionCoverage);
    EXPECT_NE(issues.back().message.find("cancelled"), std::string::npos);
}
