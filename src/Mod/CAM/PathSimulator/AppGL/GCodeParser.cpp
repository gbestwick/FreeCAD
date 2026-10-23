// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2024 Shai Seger <shaise at gmail>
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#ifdef _MSC_VER
# ifndef _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_WARNINGS
# endif
#endif

#include "GCodeParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace CAMSimulator
{

static char TokTypes[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

GCodeParser::~GCodeParser()
{
    Clear();
}

void GCodeParser::Clear()
{
    Operations.clear();
    CommandStates.clear();
    Issues.clear();
    lastState = {};
    lastTool = -1;
    absoluteDistanceMode = true;
}

bool GCodeParser::Parse(const char* filename)
{
    Clear();

    FILE* fl;
    if ((fl = fopen(filename, "rt")) == nullptr) {
        return false;
    }

    char line[120];

    while (!feof(fl)) {
        if (fgets(line, 120, fl) != NULL) {
            AddLine(line);
        }
    }
    fclose(fl);
    return false;
}

const char* GCodeParser::GetNextToken(const char* ptr, GCToken* token)
{
    float tokval;
    token->letter = '*';
    while (*ptr != 0) {
        char letter = toupper(*ptr);
        ptr++;

        if (letter == ' ') {
            continue;
        }
        if (letter == '(') {
            break;
        }

        if (IsValidToken(letter)) {
            ptr = ParseFloat(ptr, &tokval);
            token->letter = letter;
            token->fval = tokval;
            token->ival = (int)(tokval + 0.5);
            break;
        }
    }
    return ptr;
}

bool GCodeParser::IsValidToken(char tok)
{
    int len = (int)strlen(TokTypes);
    for (int i = 0; i < len; i++) {
        if (tok == TokTypes[i]) {
            return true;
        }
    }
    return false;
}

const char* GCodeParser::ParseFloat(const char* ptr, float* retFloat)
{
    float decPos = 10;
    float sign = 1;
    bool decimalPointFound = false;
    float res = 0;
    while (*ptr != 0) {
        char letter = toupper(*ptr);
        ptr++;

        if (letter == ' ') {
            continue;
        }

        if (letter == '-') {
            sign = -1;
        }
        else if (letter == '.') {
            decimalPointFound = true;
        }
        else if (letter >= '0' && letter <= '9') {
            float digitVal = (float)(letter - '0');
            if (decimalPointFound) {
                res = res + digitVal / decPos;
                decPos *= 10;
            }
            else {
                res = res * 10 + digitVal;
            }
        }
        else {
            ptr--;
            break;
        }
    }
    *retFloat = res * sign;
    return ptr;
}

bool GCodeParser::ParseLine(const char* ptr)
{
    // Truncate at first semicolon (annotations / comment)
    const char* comment = strchr(ptr, ';');
    std::string line;
    if (comment) {
        line = std::string(ptr, comment - ptr);
        ptr = line.c_str();
    }

    GCToken token;
    bool validMotion = false;
    bool exitLoop = false;
    int cmd = 0;

    // By default GCode words are not sticky, except for some exceptions. We copy the needed
    // parameters explicitly (instead of assigning the full lastState).

    MillMotion newState;

    newState.x = lastState.x;
    newState.y = lastState.y;
    newState.z = lastState.z;

    newState.tool = lastState.tool;
    newState.toolChange = false;
    newState.feedRate = lastState.feedRate;
    newState.spindleSpeed = lastState.spindleSpeed;
    newState.spindleDirection = lastState.spindleDirection;
    newState.floodCoolant = lastState.floodCoolant;
    newState.mistCoolant = lastState.mistCoolant;
    newState.rapid = lastState.rapid;
    newState.horizontalRapid = lastState.horizontalRapid;
    newState.verticalRapid = lastState.verticalRapid;
    float blockFeed = -1;
    bool hasBlockFeed = false;
    bool hasDwellParameter = false;
    float dwellParameter = 0.0f;
    bool hasPeckClearance = false;
    bool hasRepeatParameter = false;
    const auto report = [this](IssueKind kind, const std::string& message) {
        Issues.push_back({static_cast<int>(CommandStates.size()), IssueSeverity::Warning, kind, message});
    };

    newState.retract_mode = lastState.retract_mode;
    newState.retract_z = lastState.retract_z;

    while (*ptr != 0 && !exitLoop) {
        ptr = GetNextToken(ptr, &token);
        switch (token.letter) {
            case '*':
                exitLoop = true;
                break;

            case 'G':
                cmd = token.ival;
                newState.cannedCycle = cmd;
                if (token.fval != cmd) {
                    report(IssueKind::UnsupportedCommand,
                           "Fractional G-code is not interpreted accurately; motion and timing may be inaccurate.");
                }
                if (cmd == 0 || cmd == 1) {
                    newState.cmd = eMoveLiner;
                    newState.rapid = cmd == 0;
                }
                else if (cmd == 2) {
                    newState.cmd = eRotateCW;
                    newState.rapid = false;
                }
                else if (cmd == 3) {
                    newState.cmd = eRotateCCW;
                    newState.rapid = false;
                }
                else if (
                    cmd == 73 || cmd == 74 || cmd == 81 || cmd == 82 || cmd == 83 || cmd == 84
                    || cmd == 85 || cmd == 86 || cmd == 88 || cmd == 89
                ) {
                    if (cmd == 74 || cmd == 84 || cmd == 86 || cmd == 88 || cmd == 89) {
                        report(IssueKind::ApproximateCycle,
                               "G" + std::to_string(cmd) + " tapping/boring synchronization is not modeled. "
                               "Playback continues with a simplified feed and retract preview.");
                    }
                    newState.feedRetract = cmd == 74 || cmd == 84 || cmd == 85 || cmd == 89;
                    newState.cmd = eDril;
                    newState.rapid = false;
                    newState.retract_z = lastState.z;
                }
                else if (cmd == 4) {
                    newState.cmd = eDwell;
                }
                else if (cmd == 98 || cmd == 99) {
                    newState.retract_mode = cmd;
                }
                else if (cmd == 80) {
                    newState.retract_mode = 0;
                }
                else if (cmd == 90) {
                    absoluteDistanceMode = true;
                }
                else if (cmd == 91) {
                    absoluteDistanceMode = false;
                    report(IssueKind::UnsupportedCommand,
                           "G91 incremental positioning is not simulated; cycle repeats and axis positions may be inaccurate.");
                }
                else if (cmd != 17 && cmd != 21 && cmd != 94) {
                    report(IssueKind::UnsupportedCommand,
                           "G" + std::to_string(cmd) + " is not interpreted. "
                           "The preview continues using its existing assumptions; motion and timing may be inaccurate.");
                }
                break;

            case 'F':
                blockFeed = token.fval;
                hasBlockFeed = true;
                if (blockFeed <= 0) {
                    report(IssueKind::MissingRate,
                           "Feed rate is zero or negative. Affected movements use an assumed speed of 1 mm/s unless valid rapid limits apply.");
                }
                break;

            case 'S':
                newState.spindleSpeed = token.fval;
                break;

            case 'M':
                if (token.ival == 3) {
                    newState.spindleDirection = 1;
                }
                else if (token.ival == 4) {
                    newState.spindleDirection = -1;
                }
                else if (token.ival == 5) {
                    newState.spindleDirection = 0;
                }
                else if (token.ival == 7) {
                    newState.mistCoolant = true;
                }
                else if (token.ival == 8) {
                    newState.floodCoolant = true;
                }
                else if (token.ival == 9) {
                    newState.floodCoolant = false;
                    newState.mistCoolant = false;
                }
                else if (token.ival == 6) {
                    newState.toolChange = true;
                }
                else {
                    report(IssueKind::UnsupportedCommand,
                           "M" + std::to_string(token.ival) + " is not simulated. "
                           "Its action and any associated delay are omitted; preview playback can continue.");
                }
                break;

            case 'T':
                newState.tool = token.ival;
                break;

            case 'X':
                if (cmd != 4) { newState.x = token.fval; validMotion = true; }
                else report(IssueKind::UnsupportedCommand, "Axis words on G4 are ignored.");
                break;

            case 'Y':
                if (cmd != 4) { newState.y = token.fval; validMotion = true; }
                else report(IssueKind::UnsupportedCommand, "Axis words on G4 are ignored.");
                break;

            case 'Z':
                if (cmd != 4) { newState.z = token.fval; validMotion = true; }
                else report(IssueKind::UnsupportedCommand, "Axis words on G4 are ignored.");
                break;

            case 'I':
                newState.i = token.fval;
                break;

            case 'J':
                newState.j = token.fval;
                break;

            case 'K':
                newState.k = token.fval;
                break;

            case 'R':
                newState.r = token.fval;
                break;

            case 'P':
                dwellParameter = token.fval;
                hasDwellParameter = true;
                break;

            case 'Q':
                newState.peckDepth = token.fval;
                break;

            case 'D':
                newState.peckClearance = token.fval;
                hasPeckClearance = true;
                break;

            case 'L':
                if (token.fval != token.ival || token.ival < 1) {
                    report(IssueKind::ApproximateCycle,
                           "L repeat count must be a positive integer; this command uses one cycle.");
                    newState.repeatCount = 1;
                }
                else if (token.ival > 10000) {
                    report(IssueKind::ApproximateCycle,
                           "L repeat count exceeds 10,000 and is capped to bound preview work.");
                    newState.repeatCount = 10000;
                }
                else {
                    newState.repeatCount = token.ival;
                }
                hasRepeatParameter = true;
                break;

            case 'N':  // Optional source line number.
                break;

            default:
                report(IssueKind::UnsupportedCommand,
                       std::string("Parameter ") + token.letter
                       + " is ignored; its effect on movement or timing is not simulated.");
                break;
        }
    }

    // A rapid's explicit F is simulation metadata, not a new modal cutting feed.
    if (hasBlockFeed) {
        if (newState.rapid && validMotion) {
            newState.rapidFeed = blockFeed;
        }
        else {
            newState.feedRate = blockFeed;
        }
    }

    if (cmd == 4) {
        if (hasDwellParameter && dwellParameter >= 0) {
            newState.cmd = eDwell;
            newState.dwellDuration = dwellParameter;  // CAM dwell values are in seconds.
            newState.rapid = false;
        }
        else {
            report(IssueKind::UnsupportedCommand,
                   "G4 dwell has no valid P duration. Its time cannot be included in the estimate.");
        }
    }
    else if (hasDwellParameter
             && (cmd == 74 || cmd == 82 || cmd == 84 || cmd == 86 || cmd == 89)
             && dwellParameter >= 0) {
        newState.dwellDuration = dwellParameter;
    }
    else if (hasDwellParameter) {
        report(IssueKind::UnsupportedCommand,
               "P dwell is not modeled for this command; its time is omitted here.");
    }

    if (hasPeckClearance && cmd != 73 && cmd != 83) {
        report(IssueKind::UnsupportedCommand, "D peck clearance is only modeled for G73 and G83.");
    }
    else if (hasPeckClearance && newState.peckClearance < 0) {
        report(IssueKind::ApproximateCycle,
               "Negative D peck clearance is invalid; the simulator uses its fallback clearance estimate.");
        newState.peckClearance = -1.0f;
    }

    if (newState.cmd == eDril && (cmd == 73 || cmd == 83)) {
        if (newState.peckDepth <= 0) {
            report(IssueKind::ApproximateCycle,
                   "G" + std::to_string(cmd) + " has no positive Q peck depth. The cycle is shown as one plunge.");
        }
        else {
            report(IssueKind::ApproximateCycle,
                   cmd == 73
                       ? (newState.peckClearance >= 0
                              ? "G73 chip-break retracts use the D clearance value; controller configuration may override this behavior."
                              : "G73 chip-break retracts use a 5% of Q fallback clearance estimate; provide D when supported by the controller.")
                       : (newState.peckClearance >= 0
                              ? "G83 full retracts use the D clearance value for rapid re-entry; controller configuration may override it."
                              : "G83 full retracts are modeled with rapid re-entry 5% of Q above the prior peck as a fallback; provide D when supported by the controller."));
        }
    }
    else if (newState.peckDepth > 0 && cmd != 73 && cmd != 83) {
        report(IssueKind::UnsupportedCommand, "Q peck depth is not supported for this command.");
    }
    if (hasRepeatParameter && newState.repeatCount > 1
        && (newState.cmd != eDril || !absoluteDistanceMode)) {
        report(IssueKind::ApproximateCycle,
               "L repeats outside absolute-position drilling cycles are not simulated. Only one cycle is shown; total time is understated.");
    }
    if ((newState.cmd == eRotateCW || newState.cmd == eRotateCCW)
        && newState.i == 0 && newState.j == 0) {
        report(IssueKind::UnsupportedCommand,
               "Arc has no XY center offsets. The preview uses a straight movement; removal and timing are approximate.");
    }
    lastState = newState;
    return validMotion;
}

bool GCodeParser::AddLine(const char* ptr, double horizontalRapid, double verticalRapid)
{
    lastState.horizontalRapid = horizontalRapid;
    lastState.verticalRapid = verticalRapid;
    bool res = ParseLine(ptr);
    lastState.sourceCommand = static_cast<int>(CommandStates.size());
    if (lastState.toolChange) {
        Operations.push_back(lastState);
    }
    else if (lastState.cmd == eDwell && lastState.dwellDuration > 0) {
        Operations.push_back(lastState);
    }
    else if (res) {
        if (lastState.cmd == eDril) {
            // split to several motions
            lastState.cmd = eMoveLiner;
            const float rPlane = lastState.r;
            const float finalRetract = lastState.retract_mode == 99 ? rPlane : lastState.retract_z;
            const float finalDepth = lastState.z;
            const MillMotion cycle = lastState;
            constexpr int maxExpandedCycleMotions = 100000;
            int expandedCycleMotions = 0;
            bool expansionCapped = false;
            float lastCycleZ = cycle.z;
            const auto emitMove = [&](float z, bool rapid) {
                if (expandedCycleMotions >= maxExpandedCycleMotions) {
                    expansionCapped = true;
                    return false;
                }
                MillMotion move = cycle;
                move.cmd = eMoveLiner;
                move.z = z;
                move.rapid = rapid;
                move.dwellDuration = 0;
                Operations.push_back(move);
                ++expandedCycleMotions;
                lastCycleZ = z;
                return true;
            };
            const auto emitDwell = [&]() {
                if (expandedCycleMotions >= maxExpandedCycleMotions) {
                    expansionCapped = true;
                    return false;
                }
                MillMotion dwell = cycle;
                dwell.cmd = eDwell;
                dwell.rapid = false;
                Operations.push_back(dwell);
                ++expandedCycleMotions;
                return true;
            };
            const int repeats = absoluteDistanceMode ? cycle.repeatCount : 1;
            for (int repeat = 0; repeat < repeats; ++repeat) {
                if (!emitMove(rPlane, true)) break;
                if (cycle.peckDepth > 0 && cycle.sourceCommand >= 0) {
                    float depth = rPlane;
                    const float direction = finalDepth < rPlane ? -1.0f : 1.0f;
                    const float peck = std::abs(cycle.peckDepth) * direction;
                    const float clearance = cycle.peckClearance >= 0
                        ? cycle.peckClearance : std::abs(cycle.peckDepth) * 0.05f;
                    int pecks = 0;
                    while ((direction < 0 ? depth > finalDepth : depth < finalDepth)
                           && pecks < 100000 && !expansionCapped) {
                        const float nextDepth = direction < 0 ? std::max(depth + peck, finalDepth)
                                                              : std::min(depth + peck, finalDepth);
                        if (!emitMove(nextDepth, false)) break;
                        if (nextDepth != finalDepth) {
                            if (cycle.cannedCycle == 73) {
                                if (!emitMove(nextDepth - direction * clearance, true)) break;
                            }
                            else {
                                if (!emitMove(rPlane, true)) break;
                                const float approach = nextDepth - direction * clearance;
                                if (approach != rPlane && !emitMove(approach, true)) break;
                            }
                        }
                        else if (cycle.dwellDuration > 0) {
                            if (!emitDwell()) break;
                        }
                        depth = nextDepth;
                        ++pecks;
                    }
                    if (pecks >= 100000) {
                        expansionCapped = true;
                    }
                }
                else {
                    if (!emitMove(finalDepth, false)) break;
                    if (cycle.dwellDuration > 0 && !emitDwell()) break;
                }
                if (!emitMove(finalRetract, !cycle.feedRetract)) break;
            }
            if (expansionCapped) {
                Issues.push_back({cycle.sourceCommand, IssueSeverity::Warning, IssueKind::ApproximateCycle,
                    "Canned-cycle expansion reached its 100,000-motion safety limit. The displayed path and time are incomplete."});
            }
            lastState.z = expansionCapped ? lastCycleZ : finalRetract;
            lastState.rapid = false;
            lastState.cmd = eDril;
        }
        else {
            Operations.push_back(lastState);
        }
    }
    CommandStates.push_back(lastState);
    return res;
}

}  // namespace CAMSimulator
