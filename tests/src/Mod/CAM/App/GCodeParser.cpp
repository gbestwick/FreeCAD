// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <Mod/CAM/PathSimulator/AppGL/GCodeParser.h>

using CAMSimulator::GCodeParser;
using CAMSimulator::eDwell;

TEST(CAMSimulatorParser, ModalFeedAndSpindleSettings)
{
    GCodeParser parser;
    parser.AddLine("M3 S12000");
    parser.AddLine("G1 X10 F600");
    parser.AddLine("G1 Z-2 F120");
    parser.AddLine("G0 Z5");
    parser.AddLine("G2 X20 Y5 I5 J0");
    ASSERT_EQ(parser.Operations.size(), 4);
    EXPECT_FLOAT_EQ(parser.Operations[0].feedRate, 600);
    EXPECT_FLOAT_EQ(parser.Operations[1].feedRate, 120);
    EXPECT_TRUE(parser.Operations[2].rapid);
    EXPECT_FALSE(parser.Operations[3].rapid);
    EXPECT_FLOAT_EQ(parser.Operations[3].feedRate, 120);
    EXPECT_FLOAT_EQ(parser.Operations[3].spindleSpeed, 12000);
    EXPECT_EQ(parser.Operations[3].spindleDirection, 1);
}

TEST(CAMSimulatorParser, SpindleChangesPreserveEarlierMoves)
{
    GCodeParser parser;
    parser.AddLine("M3 S12000");
    parser.AddLine("G1 X1 F100");
    parser.AddLine("M5");
    parser.AddLine("G0 X2");
    parser.AddLine("M4 S8000");
    parser.AddLine("G1 X3");
    parser.AddLine("S9000");
    parser.AddLine("G1 X4 ; F999 S999 M5");
    ASSERT_EQ(parser.Operations.size(), 4);
    EXPECT_EQ(parser.Operations[0].spindleDirection, 1);
    EXPECT_FLOAT_EQ(parser.Operations[0].spindleSpeed, 12000);
    EXPECT_EQ(parser.Operations[1].spindleDirection, 0);
    EXPECT_EQ(parser.Operations[2].spindleDirection, -1);
    EXPECT_FLOAT_EQ(parser.Operations[2].spindleSpeed, 8000);
    EXPECT_FLOAT_EQ(parser.Operations[3].spindleSpeed, 9000);
    EXPECT_FLOAT_EQ(parser.Operations[3].feedRate, 100);
    EXPECT_EQ(parser.Operations[3].spindleDirection, -1);
}

TEST(CAMSimulatorParser, ResetClearsSettings)
{
    GCodeParser parser;
    parser.AddLine("M3 S12000");
    parser.AddLine("G1 X1 F100");
    parser.Clear();
    parser.AddLine("G1 X2");
    ASSERT_EQ(parser.Operations.size(), 1);
    EXPECT_LT(parser.Operations[0].feedRate, 0);
    EXPECT_LT(parser.Operations[0].spindleSpeed, 0);
    EXPECT_EQ(parser.Operations[0].spindleDirection, 0);
}

TEST(CAMSimulatorParser, DrillingApproachAndRetract)
{
    for (const auto* cycle : {"G81 X10 Z-5 R2 F90", "G85 X10 Z-5 R2 F90"}) {
        GCodeParser parser;
        parser.AddLine("M3 S5000");
        parser.AddLine(cycle);
        ASSERT_EQ(parser.Operations.size(), 3);
        EXPECT_TRUE(parser.Operations[0].rapid);
        EXPECT_FALSE(parser.Operations[1].rapid);
        EXPECT_EQ(parser.Operations[2].rapid, !parser.Operations[2].feedRetract);
        for (const auto& motion : parser.Operations) {
            EXPECT_FLOAT_EQ(motion.feedRate, 90);
            EXPECT_FLOAT_EQ(motion.spindleSpeed, 5000);
        }
    }
}

TEST(CAMSimulatorParser, RapidRateDoesNotOverwriteCuttingFeed)
{
    GCodeParser parser;
    parser.AddLine("G1 X1 F120");
    parser.AddLine("G0 X10 F3000", 3000, 600);
    parser.AddLine("G1 X20");
    ASSERT_EQ(parser.Operations.size(), 3);
    EXPECT_FLOAT_EQ(parser.Operations[1].rapidFeed, 3000);
    EXPECT_FLOAT_EQ(parser.Operations[1].horizontalRapid, 3000);
    EXPECT_FLOAT_EQ(parser.Operations[1].verticalRapid, 600);
    EXPECT_FLOAT_EQ(parser.Operations[2].feedRate, 120);
    EXPECT_LT(parser.Operations[2].rapidFeed, 0);
}

TEST(CAMSimulatorParser, DrillExpansionRetainsRapidLimits)
{
    GCodeParser parser;
    parser.AddLine("G81 X10 Z-5 R2 F90", 3000, 600);
    ASSERT_EQ(parser.Operations.size(), 3);
    for (const auto& motion : parser.Operations) {
        EXPECT_FLOAT_EQ(motion.horizontalRapid, 3000);
        EXPECT_FLOAT_EQ(motion.verticalRapid, 600);
    }
}

TEST(CAMSimulatorParser, PeckCycleUsesExplicitClearance)
{
    GCodeParser chipBreak;
    chipBreak.AddLine("G73 X0 Z-3 R0 Q1 D0.25 F90");
    ASSERT_GE(chipBreak.Operations.size(), 5);
    EXPECT_FLOAT_EQ(chipBreak.Operations[2].z, -0.75f);
    EXPECT_FLOAT_EQ(chipBreak.Operations[0].peckClearance, 0.25f);

    GCodeParser deepDrill;
    deepDrill.AddLine("G83 X0 Z-3 R0 Q1 D0.25 F90");
    ASSERT_GE(deepDrill.Operations.size(), 8);
    EXPECT_FLOAT_EQ(deepDrill.Operations[3].z, -0.75f);
    EXPECT_FLOAT_EQ(deepDrill.Operations[6].z, -1.75f);
    EXPECT_FLOAT_EQ(deepDrill.Operations[0].peckClearance, 0.25f);
}

TEST(CAMSimulatorParser, DwellCyclesIncludeTheirPDuration)
{
    for (const auto* code : {"G74 X0 Z-2 R0 P1.5 F90", "G82 X0 Z-2 R0 P1.5 F90",
                             "G84 X0 Z-2 R0 P1.5 F90", "G86 X0 Z-2 R0 P1.5 F90",
                             "G89 X0 Z-2 R0 P1.5 F90"}) {
        GCodeParser parser;
        parser.AddLine(code);
        const auto dwell = std::find_if(parser.Operations.begin(), parser.Operations.end(), [](const auto& move) {
            return move.cmd == eDwell;
        });
        ASSERT_NE(dwell, parser.Operations.end()) << code;
        EXPECT_DOUBLE_EQ(dwell->dwellDuration, 1.5) << code;
    }
}

TEST(CAMSimulatorParser, AbsoluteCycleRepeatsAreExpandedAndIncrementalRepeatsStayPartial)
{
    GCodeParser absolute;
    absolute.AddLine("G81 X2 Z-5 R1 L3 F90");
    EXPECT_EQ(absolute.Operations.size(), 9);
    EXPECT_EQ(absolute.Operations[0].z, 1.0f);
    EXPECT_EQ(absolute.Operations[3].z, 1.0f);
    EXPECT_EQ(absolute.Operations[6].z, 1.0f);

    GCodeParser incremental;
    incremental.AddLine("G91");
    incremental.AddLine("G81 X2 Z-5 R1 L3 F90");
    EXPECT_EQ(incremental.Operations.size(), 3);
    EXPECT_TRUE(std::any_of(incremental.Issues.begin(), incremental.Issues.end(), [](const auto& issue) {
        return issue.command == 1 && issue.kind == CAMSimulator::IssueKind::ApproximateCycle;
    }));
}

TEST(CAMSimulatorParser, PeckExpansionHasAVisibleSafetyLimit)
{
    GCodeParser parser;
    parser.AddLine("G83 X0 Z-100 R0 Q0.001 L10000 F90");
    EXPECT_LE(parser.Operations.size(), 100000);
    EXPECT_TRUE(std::any_of(parser.Issues.begin(), parser.Issues.end(), [](const auto& issue) {
        return issue.command == 0 && issue.kind == CAMSimulator::IssueKind::ApproximateCycle
            && issue.message.find("safety limit") != std::string::npos;
    }));
}

TEST(CAMSimulatorParser, IndependentCoolantStates)
{
    GCodeParser parser;
    parser.AddLine("G0 X0");
    parser.AddLine("M8");
    parser.AddLine("G1 X10 F600");
    parser.AddLine("M7");
    parser.AddLine("G1 X20");
    parser.AddLine("M9");
    parser.AddLine("G0 X30");
    parser.AddLine("M7");
    parser.AddLine("G1 X40");
    ASSERT_EQ(parser.Operations.size(), 5);
    EXPECT_FALSE(parser.Operations[0].floodCoolant);
    EXPECT_FALSE(parser.Operations[0].mistCoolant);
    EXPECT_TRUE(parser.Operations[1].floodCoolant);
    EXPECT_FALSE(parser.Operations[1].mistCoolant);
    EXPECT_TRUE(parser.Operations[2].floodCoolant);
    EXPECT_TRUE(parser.Operations[2].mistCoolant);
    EXPECT_FALSE(parser.Operations[3].floodCoolant);
    EXPECT_FALSE(parser.Operations[3].mistCoolant);
    EXPECT_FALSE(parser.Operations[4].floodCoolant);
    EXPECT_TRUE(parser.Operations[4].mistCoolant);
}

TEST(CAMSimulatorParser, CoolantCommentsResetAndTrailingOff)
{
    GCodeParser parser;
    parser.AddLine("M7");
    parser.AddLine("G1 X10 M8 F600 ; M9");
    parser.AddLine("(M9)");
    parser.AddLine("G1 X20");
    parser.AddLine("M9");
    ASSERT_EQ(parser.Operations.size(), 2);
    EXPECT_TRUE(parser.Operations.back().floodCoolant);
    EXPECT_TRUE(parser.Operations.back().mistCoolant);
    EXPECT_FALSE(parser.lastState.floodCoolant);
    EXPECT_FALSE(parser.lastState.mistCoolant);
    parser.AddLine("M7 M8");
    parser.Clear();
    parser.AddLine("G1 X1");
    EXPECT_FALSE(parser.Operations.front().floodCoolant);
    EXPECT_FALSE(parser.Operations.front().mistCoolant);
}

TEST(CAMSimulatorParser, SourceCommandsIncludeNonMotionAndComments)
{
    GCodeParser parser;
    parser.AddLine("T1");
    parser.AddLine("(setup)");
    parser.AddLine("M8");
    parser.AddLine("G0 X0");
    parser.AddLine("G1 X10 F600");
    parser.AddLine("M9");
    ASSERT_EQ(parser.CommandStates.size(), 6);
    ASSERT_EQ(parser.Operations.size(), 2);
    EXPECT_EQ(parser.Operations[0].sourceCommand, 3);
    EXPECT_EQ(parser.Operations[1].sourceCommand, 4);
    EXPECT_TRUE(parser.CommandStates[2].floodCoolant);
    EXPECT_FALSE(parser.CommandStates[5].floodCoolant);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(parser.CommandStates[i].sourceCommand, i);
    parser.Clear();
    EXPECT_TRUE(parser.CommandStates.empty());
    parser.AddLine("M7");
    EXPECT_EQ(parser.CommandStates[0].sourceCommand, 0);
}

TEST(CAMSimulatorParser, ExpandedCycleKeepsOneSourceCommand)
{
    GCodeParser parser;
    parser.AddLine("G0 X0 Z5");
    parser.AddLine("M8");
    parser.AddLine("G81 X10 Z-5 R2 F90");
    parser.AddLine("M9");
    ASSERT_EQ(parser.Operations.size(), 4);
    for (int i = 1; i < 4; ++i) EXPECT_EQ(parser.Operations[i].sourceCommand, 2);
    ASSERT_EQ(parser.CommandStates.size(), 4);
    EXPECT_EQ(parser.CommandStates[2].cmd, CAMSimulator::eDril);
    EXPECT_TRUE(parser.CommandStates[2].floodCoolant);
    EXPECT_FALSE(parser.CommandStates[3].floodCoolant);
}

TEST(CAMSimulatorParser, UnsupportedCommandsRetainSourceAndExplainOmissions)
{
    GCodeParser parser;
    parser.AddLine("G0 X0");
    parser.AddLine("G4 P2");
    parser.AddLine("M6");
    parser.AddLine("G83 X10 Z-5 R2 Q1 F90");
    bool peck = false;
    for (const auto& issue : parser.Issues) {
        EXPECT_FALSE(issue.message.empty());
        if (issue.command == 3 && issue.kind == CAMSimulator::IssueKind::ApproximateCycle) peck = true;
    }
    ASSERT_GT(parser.CommandStates.size(), 1);
    EXPECT_EQ(parser.CommandStates[1].cmd, eDwell);
    EXPECT_FLOAT_EQ(parser.CommandStates[1].dwellDuration, 2.0f);
    EXPECT_TRUE(std::any_of(parser.Operations.begin(), parser.Operations.end(), [](const auto& operation) {
        return operation.cmd == eDwell && operation.dwellDuration == 2.0f;
    }));
    EXPECT_TRUE(parser.CommandStates[2].toolChange);
    EXPECT_TRUE(std::any_of(parser.Operations.begin(), parser.Operations.end(), [](const auto& operation) {
        return operation.sourceCommand == 2 && operation.toolChange;
    }));
    EXPECT_FALSE(std::any_of(parser.Issues.begin(), parser.Issues.end(), [](const auto& issue) {
        return issue.command == 2 && issue.kind == CAMSimulator::IssueKind::UnsupportedCommand;
    }));
    EXPECT_TRUE(peck);
    parser.Clear();
    EXPECT_TRUE(parser.Issues.empty());
}

TEST(CAMSimulatorParser, SupportedCommandsAndCommentsDoNotCreateIssues)
{
    GCodeParser parser;
    for (const auto* line : {"G17", "G21", "G90", "G94", "T1", "M3 S1000", "M8", "G0 X0",
                             "G1 X10 F600 ; G4 P2 M6", "(G83 Q1 M0)", "M9", "M5"}) {
        parser.AddLine(line);
    }
    EXPECT_TRUE(parser.Issues.empty());
}

TEST(CAMSimulatorParser, InvalidFeedDoesNotSilentlyInheritPreviousValidFeed)
{
    GCodeParser parser;
    parser.AddLine("G1 X1 F600");
    parser.AddLine("G1 X2 F-5");
    ASSERT_EQ(parser.Issues.size(), 1);
    EXPECT_EQ(parser.Issues[0].command, 1);
    EXPECT_EQ(parser.Issues[0].kind, CAMSimulator::IssueKind::MissingRate);
    EXPECT_FLOAT_EQ(parser.Operations.back().feedRate, -5);
    parser.AddLine("G1 X3 F0");
    EXPECT_FLOAT_EQ(parser.Operations.back().feedRate, 0);
    EXPECT_EQ(parser.Issues.size(), 2);
}

TEST(CAMSimulatorParser, UnsupportedModesAxesAndRadiusArcsAreReported)
{
    GCodeParser parser;
    parser.AddLine("G91");
    parser.AddLine("G1 A90");
    parser.AddLine("G2 X10 R5 F600");
    parser.AddLine("G90.1");
    ASSERT_EQ(parser.Issues.size(), 4);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(parser.Issues[i].command, i);
}
