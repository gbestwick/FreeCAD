// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2024 Shai Seger <shaise at gmail>                       *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#pragma once

#include "linmath.h"

namespace CAMSimulator
{

enum eEndMillType
{
    eEndmillFlat,
    eEndmillV,
    eEndmillBall,
    eEndmillFillet
};

enum eCmdType
{
    eNop,
    eMoveLiner,
    eRotateCW,
    eRotateCCW,
    eDril,
    eDwell,
    eChangeTool
};

struct MillMotion
{
    eCmdType cmd = eNop;
    int tool = -1;
    bool toolChange = false;
    int sourceCommand = -1;
    // Programmed settings, independent of the simulation playback multiplier.
    float feedRate = -1.0f;  // mm/min; negative means unspecified
    float spindleSpeed = -1.0f;  // RPM; negative means unspecified
    int spindleDirection = 0;  // 0: stopped, 1: clockwise, -1: counterclockwise
    bool floodCoolant = false;
    bool mistCoolant = false;
    bool rapid = false;
    float rapidFeed = -1.0f;  // Explicit rate on this rapid move, in mm/min
    float peckDepth = 0.0f;
    float peckClearance = -1.0f;  // Optional controller-provided D clearance, in mm
    double dwellDuration = 0.0;  // seconds
    int repeatCount = 1;
    int cannedCycle = 0;
    float horizontalRapid = -1.0f;
    float verticalRapid = -1.0f;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float i = 0.0f, j = 0.0f, k = 0.0f;
    float r = 0.0f;
    bool feedRetract = false;  // Tapping and G85/G89 retract at the programmed feed
    char retract_mode = '\0';
    float retract_z = 0;
};

static inline void MotionPosToVec(vec3 vec, const MillMotion& motion)
{
    vec[0] = motion.x;
    vec[1] = motion.y;
    vec[2] = motion.z;
}

}  // namespace CAMSimulator
