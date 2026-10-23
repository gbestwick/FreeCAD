// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <Mod/CAM/PathSimulator/AppGL/SimulationTimeline.h>
#include <numbers>

using namespace CAMSimulator;

TEST(CAMSimulatorTiming, LinearAndPlungeTravel)
{
    MillMotion from, to;
    to.x = 60;
    to.feedRate = 600;
    EXPECT_DOUBLE_EQ(motionDuration(motionLength(from, to), 60, 0, to), 6);
    to.x = 0;
    to.z = -10;
    to.feedRate = 120;
    EXPECT_DOUBLE_EQ(motionDuration(motionLength(from, to), 0, 10, to), 5);
}

TEST(CAMSimulatorTiming, ArcAndHelicalLengths)
{
    MillMotion from, to;
    from.x = 10;
    to.y = 10;
    to.i = -10;
    to.cmd = eRotateCCW;
    EXPECT_NEAR(motionLength(from, to), 5 * std::numbers::pi, 1e-9);
    to.cmd = eRotateCW;
    EXPECT_NEAR(motionLength(from, to), 15 * std::numbers::pi, 1e-9);
    to.cmd = eRotateCCW;
    to.z = 5;
    EXPECT_NEAR(motionLength(from, to), std::hypot(5 * std::numbers::pi, 5), 1e-9);
    to.x = 10;
    to.y = 0;
    to.z = 0;
    EXPECT_NEAR(motionLength(from, to), 20 * std::numbers::pi, 1e-9);
}

TEST(CAMSimulatorTiming, RapidRespectsBothLimits)
{
    MillMotion move;
    move.rapid = true;
    move.horizontalRapid = 6000;
    move.verticalRapid = 600;
    EXPECT_DOUBLE_EQ(motionDuration(100, 100, 0, move), 1);
    EXPECT_DOUBLE_EQ(motionDuration(20, 0, 20, move), 2);
    EXPECT_DOUBLE_EQ(motionDuration(std::hypot(100, 20), 100, 20, move), 2);
    move.verticalRapid = 0;
    EXPECT_LT(motionDuration(20, 0, 20, move), 0);
    move.rapidFeed = 1200;
    EXPECT_DOUBLE_EQ(motionDuration(20, 0, 20, move), 1);
}

TEST(CAMSimulatorTiming, MissingAndZeroRates)
{
    MillMotion move;
    EXPECT_LT(motionDuration(10, 10, 0, move), 0);
    move.feedRate = 0;
    EXPECT_LT(motionDuration(10, 10, 0, move), 0);
    EXPECT_DOUBLE_EQ(motionDuration(0, 0, 0, move), 0);
}

TEST(CAMSimulatorTiming, QualityDoesNotChangeDuration)
{
    SimulationTimeline coarse, fine;
    coarse.append(10, 6);
    fine.append(100, 6);
    EXPECT_DOUBLE_EQ(coarse.duration(), fine.duration());
    EXPECT_DOUBLE_EQ(coarse.stepAtTime(3) / 10, fine.stepAtTime(3) / 100);
    EXPECT_DOUBLE_EQ(coarse.stepAtTime(6), 10);
    EXPECT_DOUBLE_EQ(fine.stepAtTime(6), 100);
}

TEST(CAMSimulatorTiming, RateChangesAndSeeking)
{
    SimulationTimeline timeline;
    timeline.append(100, 10);
    timeline.append(100, 20);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(5), 50);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(10), 100);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(20), 150);
    EXPECT_DOUBLE_EQ(timeline.timeAtStep(150), 20);
    EXPECT_DOUBLE_EQ(timeline.timeAtStep(25), 2.5);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(timeline.timeAtStep(25) + 1), 35);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(300), 200);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(-1), 0);
}

TEST(CAMSimulatorTiming, EmptyAndZeroLengthMoves)
{
    SimulationTimeline timeline;
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(10), 0);
    EXPECT_DOUBLE_EQ(timeline.timeAtStep(10), 0);
    timeline.append(1, 0);
    timeline.append(10, 1);
    timeline.append(1, 0);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(0), 1);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(0.5), 6);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(1), 12);
    timeline.clear();
    EXPECT_DOUBLE_EQ(timeline.duration(), 0);
    EXPECT_DOUBLE_EQ(timeline.stepAtTime(1), 0);
}
