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

#include "GCodeParser.h"
#include "SimulationTimeline.h"
#include "TimeBreakdown.h"
#include "MillPathLine.h"
#include "MillPathSegment.h"
#include "SimDisplay.h"
#include "SolidObject.h"
#include "StockObject.h"
#include "linmath.h"
#include <chrono>
#include <atomic>
#include <functional>
#include <TopoDS_Shape.hxx>
#include <unordered_map>
#include <vector>

namespace Mesh { class MeshObject; }

#define VIEWITEM_SIMULATION 1
#define VIEWITEM_BASE_SHAPE 2
#define VIEWITEM_MAX 4

namespace CAMSimulator
{

struct MillSimulationState
{
    bool mPauseOnIssues = true;
    bool mPauseOnToolChanges = true;
    int mNextIssue = 0;
    int mPausedIssue = -1;
    int mPausedToolChange = -1;
    int mSelectedCommand = -1;
    int mCurStep = 0;
    int mNTotalSteps = 0;
    int mPathStep = -1;
    int mSubStep = 0;
    int mNPathSteps = 0;
    int mSimSpeed = 1;
    double mElapsedSeconds = 0;
    double mStepFraction = 0;
    int mViewItems = VIEWITEM_SIMULATION;
    bool mViewPath = false;
    bool mViewSSAO = false;

    bool mSimPlaying = false;
};

class MillSimulation: private MillSimulationState
{
    typedef std::chrono::steady_clock clock;

public:
    MillSimulation();
    ~MillSimulation();
    void ClearMillPathSegments();
    void Clear();
    void InitSimulation(float quality, float maxStockDimension);
    std::vector<SimulationIssue> AnalyzeHolderCollisions(
        const std::atomic_bool* cancel,
        const std::function<void(int)>& progress
    ) const;
    void SetCollisionIssues(std::vector<SimulationIssue> issues);
    void AddTool(EndMill* tool);
    void AddTool(const std::vector<float>& toolProfile, int toolid, float diameter);
    void SetHolderObject(
        int toolId,
        const std::vector<Vertex>& verts,
        const std::vector<GLushort>& indices
    );
    void SetCollisionStock(const TopoDS_Shape& stock);
    bool BuildFinalVoxelMesh(
        Mesh::MeshObject& mesh,
        double requestedCellSize,
        double& actualCellSize,
        const std::atomic_bool* cancel = nullptr,
        const std::function<void(int)>& progress = {},
        int throughCommand = -1
    ) const;
    bool BuildFinalVoxelComparison(
        const TopoDS_Shape& target,
        Mesh::MeshObject& excessMaterial,
        Mesh::MeshObject& missingTargetMaterial,
        double requestedCellSize,
        double& actualCellSize,
        double& excessVolume,
        double& missingVolume,
        bool& targetWithinStockBounds,
        const std::atomic_bool* cancel = nullptr,
        const std::function<void(int)>& progress = {}
    ) const;
    void SetHolderCollisionShape(int toolId, const TopoDS_Shape& holder);
    bool ToolExists(int toolid);
    void RenderSimulation();
    void RenderTool();
    void RenderPath();
    void RenderBaseShape();
    void Render();
    void ProcessSim(const clock::duration& elapsed);
    void SimNext(const clock::duration& elapsed);

    bool LoadGCodeFile(const char* fileName);
    bool AddGcodeLine(const char* line, double horizontalRapid = -1, double verticalRapid = -1);

    void SetPlaying(bool b);
    void SingleStep();
    void SeekCommand(int command);
    int GetCurrentCommand() const;
    const std::vector<SimulationIssue>& GetIssues() const { return mIssues; }
    void SetPauseOnIssues(bool enabled);
    void SetPauseOnToolChanges(bool enabled);
    void ContinueAfterIssue(bool withoutPausing = false);
    void SetSpeed(int s);

    void SetSimulationStage(float stage);
    void SetState(const MillSimulationState& state);
    const MillSimulationState& GetState() const;
    const MillMotion* GetCurrentMotion() const;
    double GetCurrentFeed() const;
    bool IsTimingEstimated() const;
    double GetTotalDuration() const;
    bool HasEstimatedRates() const;
    const std::vector<CommandTimeBreakdown>& GetCommandTimeBreakdown() const
    {
        return mCommandTimeBreakdown;
    }

    void SetBoxStock(float x, float y, float z, float l, float w, float h);
    void SetArbitraryStock(const std::vector<Vertex>& verts, const std::vector<GLushort>& indices);
    void SetStockVisible(bool b);
    bool IsStockVisible() const;

    void SetBaseObject(const std::vector<Vertex>& verts, const std::vector<GLushort>& indices);
    void SetBaseVisible(bool b);
    bool IsBaseVisible() const;

    void SetPathVisible(bool b);
    void EnableSsao(bool b);

    void UpdateWindowScale(int width, int height);
    void UpdateCamera(const SoCamera& camera);

    void SetBackgroundColor(const vec3& c);
    void SetPathColor(const vec3& normal, const vec3& rapid);

protected:
    void InitDisplay(float quality);
    void GlsimStart();
    void GlsimToolStep1(void);
    void GlsimToolStep2(void);
    void GlsimClipBack(void);
    void GlsimRenderStock(void);
    void GlsimRenderTools(void);
    void GlsimEnd(void);
    void renderSegmentForward(int iSeg);
    void renderSegmentReversed(int iSeg);
    void CalcSegmentPositions();
    EndMill* GetTool(int tool);
    void RemoveTool(int toolId);

protected:
    bool simulationInitiated = false;

    // protected:
public:
    std::vector<EndMill*> mToolTable;
    std::unordered_map<int, std::unique_ptr<SolidObject>> mHolderObjects;
    TopoDS_Shape mCollisionStock;
    std::unordered_map<int, TopoDS_Shape> mHolderCollisionShapes;
    GCodeParser mCodeParser;
    SimDisplay simDisplay;
    MillPathLine millPathLine;
    std::vector<MillPathSegment*> MillPathSegments;

    int mWidth = -1;
    int mHeight = -1;

    StockObject mStockObject;
    SolidObject mBaseShape;

    vec3 bgndColor = {0.1f, 0.2f, 0.3f};
    vec3 stockColor = {0.5f, 0.55f, 0.9f};
    vec3 cutColor = {0.5f, 0.84f, 0.73f};
    vec3 toolColor = {0.5f, 0.4f, 0.3f};
    vec3 baseShapeColor = {0.7f, 0.6f, 0.5f};

    std::vector<SimulationIssue> mIssues;
    std::vector<int> mCommandSteps;
    std::vector<double> mCommandTimes;
    std::vector<int> mToolChangeCommands;
    std::size_t mNextToolChange = 0;
    std::vector<CommandTimeBreakdown> mCommandTimeBreakdown;
    struct DwellInterval { double start; double end; int command; };
    std::vector<DwellInterval> mDwellIntervals;
    std::vector<double> mIssueTimes;
    SimulationTimeline mTimeline;
    bool mHasEstimatedRates = false;
};

}  // namespace CAMSimulator
