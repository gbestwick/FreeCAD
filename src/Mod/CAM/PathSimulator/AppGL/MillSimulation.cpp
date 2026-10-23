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

#include "MillSimulation.h"
#include "HolderCollisionCheck.h"
#include "VoxelStock.h"

#include <Base/Vector3D.h>
#include <Mod/Mesh/App/Core/Elements.h>
#include <Mod/Mesh/App/Mesh.h>

#include "GlUtils.h"
#include <algorithm>
#include <iostream>

// include this last as the defines can mess up other includes
#include "OpenGlWrapper.h"

using namespace std::literals;

#define DRAG_ZOOM_FACTOR 10

namespace CAMSimulator
{

MillSimulation::MillSimulation()
{}

MillSimulation::~MillSimulation()
{
    Clear();
}

void MillSimulation::ClearMillPathSegments()
{
    for (unsigned int i = 0; i < MillPathSegments.size(); i++) {
        delete MillPathSegments[i];
    }
    MillPathSegments.clear();
    mTimeline.clear();
    mHasEstimatedRates = false;
}

void MillSimulation::Clear()
{
    mCodeParser.Clear();
    mIssues.clear();
    mHolderObjects.clear();
    mHolderCollisionShapes.clear();
    mCollisionStock.Nullify();
    mCommandSteps.clear();
    mCommandTimes.clear();
    mCommandTimeBreakdown.clear();
    mDwellIntervals.clear();
    mIssueTimes.clear();
    mPauseOnIssues = true;
    mPauseOnToolChanges = true;
    mNextIssue = 0;
    mPausedIssue = -1;
    mPausedToolChange = -1;
    mToolChangeCommands.clear();
    mNextToolChange = 0;
    mSelectedCommand = -1;

    ClearMillPathSegments();

    for (unsigned int i = 0; i < mToolTable.size(); i++) {
        delete mToolTable[i];
    }
    mToolTable.clear();

    mStockObject.Clear();
    simDisplay.CleanGL();

    mCurStep = 0;
    mPathStep = -1;
    mNTotalSteps = 0;
    mNPathSteps = 0;
    mElapsedSeconds = 0;
    mStepFraction = 0;
    mSimPlaying = false;

    simulationInitiated = false;
}

void MillSimulation::InitSimulation(float quality, float maxStockDimension)
{
    ClearMillPathSegments();
    mIssues = mCodeParser.Issues;
    mCommandSteps.assign(mCodeParser.CommandStates.size() + 1, 0);
    mCommandTimes.assign(mCodeParser.CommandStates.size(), 0.0);
    mIssueTimes.clear();
    mNextIssue = 0;
    mPausedIssue = -1;
    mPausedToolChange = -1;
    mToolChangeCommands.clear();
    mNextToolChange = 0;
    millPathLine.Clear();
    mSelectedCommand = -1;
    // mViewSSAO = guiDisplay.IsChecked(eGuiItemAmbientOclusion);

    // gDestPos = curMillOperation->startPos;
    mCurStep = 0;
    mPathStep = -1;
    mNTotalSteps = 0;
    mSimPlaying = false;
    mSimSpeed = 1;
    mElapsedSeconds = 0;
    mStepFraction = 0;

    MillPathSegment::SetQuality(quality, maxStockDimension);

    const int nCommands = static_cast<int>(mCodeParser.CommandStates.size());
    mCommandTimeBreakdown.assign(nCommands, {});
    int segId = 0;
    std::vector<bool> rateReported(mCodeParser.CommandStates.size(), false);
    std::vector<bool> toolReported(mCodeParser.CommandStates.size(), false);
    for (const auto& issue : mIssues) {
        if (issue.kind == IssueKind::MissingRate) rateReported[issue.command] = true;
    }

    MillMotion prevMotion;
    bool havePosition = false;
    std::size_t operation = 0;
    for (int command = 0; command < nCommands; ++command) {
        mCommandSteps[command] = mNTotalSteps;
        mCommandTimes[command] = mTimeline.duration();
        while (operation < mCodeParser.Operations.size()
               && mCodeParser.Operations[operation].sourceCommand == command) {
            const MillMotion curMotion = mCodeParser.Operations[operation];
            if (curMotion.toolChange) {
                mToolChangeCommands.push_back(command);
                ++operation;
                continue;
            }
            if (curMotion.cmd == eDwell) {
                if (curMotion.dwellDuration > 0) {
                    mCommandTimeBreakdown[command].dwellSeconds += curMotion.dwellDuration;
                    const double start = mTimeline.duration();
                    mTimeline.append(0, curMotion.dwellDuration);
                    mDwellIntervals.push_back({start, mTimeline.duration(), command});
                }
                ++operation;
                continue;
            }

            if (!havePosition) {
                prevMotion = curMotion;
                havePosition = true;
                MillPathPosition initial;
                initial.X = curMotion.x;
                initial.Y = curMotion.y;
                initial.Z = curMotion.z;
                initial.SegmentId = segId++;
                millPathLine.MillPathPointsBuffer.push_back(initial);
                ++operation;
                continue;
            }

            const EndMill* tool = GetTool(curMotion.tool);
            if (tool != nullptr) {
                auto segment = new MillPathSegment(*tool, prevMotion, curMotion);
                segment->indexInArray = static_cast<int>(operation);
                segment->segmentIndex = segId++;
                mNTotalSteps += segment->numSimSteps;
                if (segment->timingEstimated && !rateReported[command]) {
                    rateReported[command] = true;
                    mIssues.push_back({command, IssueSeverity::Warning, IssueKind::MissingRate,
                        curMotion.rapid ? "Rapid speed is missing or invalid. This movement uses an assumed speed of 1 mm/s."
                                        : "Cutting feed is missing or invalid. This movement uses an assumed speed of 1 mm/s."});
                }
                mTimeline.append(segment->numSimSteps, segment->travelDuration);
                mCommandTimeBreakdown[command].addMove(
                    curMotion.rapid, segment->travelDuration, segment->timingEstimated
                );
                mHasEstimatedRates |= segment->timingEstimated;
                MillPathSegments.push_back(segment);
                segment->AppendPathPoints(millPathLine.MillPathPointsBuffer);
            }
            else if (!toolReported[command]) {
                toolReported[command] = true;
                mCommandTimeBreakdown[command].omittedTime = true;
                mIssues.push_back({command, IssueSeverity::Error, IssueKind::MissingTool,
                    "No tool profile is available for T" + std::to_string(curMotion.tool)
                    + ". This movement, its material removal, and its time are omitted. Continuing gives an incomplete preview."});
            }
            prevMotion = curMotion;
            ++operation;
        }
    }

    for (const auto& issue : mCodeParser.Issues) {
        if (issue.command >= 0 && issue.command < nCommands
            && issue.kind == IssueKind::UnsupportedCommand) {
            mCommandTimeBreakdown[issue.command].omittedTime = true;
        }
    }

    mCommandSteps[nCommands] = mNTotalSteps;
    std::stable_sort(mIssues.begin(), mIssues.end(), [](const auto& a, const auto& b) {
        if (a.kind == IssueKind::CollisionCoverage) return false;
        if (b.kind == IssueKind::CollisionCoverage) return true;
        return a.command < b.command;
    });
    for (const auto& issue : mIssues) {
        if (issue.command >= 0 && issue.command < nCommands) {
            mIssueTimes.push_back(mCommandTimes[issue.command]);
        }
        else {
            mIssueTimes.push_back(mTimeline.duration());
        }
    }

    assert(mNTotalSteps >= 0);

    mNPathSteps = (int)MillPathSegments.size();
    millPathLine.GenerateModel();

    InitDisplay(quality);

    CalcSegmentPositions();
    simulationInitiated = true;
}

std::vector<SimulationIssue> MillSimulation::AnalyzeHolderCollisions(
    const std::atomic_bool* cancel,
    const std::function<void(int)>& progress
) const
{
    HolderCollisionCheck collisionCheck;
    return collisionCheck.Analyze(
        mCollisionStock,
        mHolderCollisionShapes,
        MillPathSegments,
        0.5f,
        cancel,
        progress
    );
}

void MillSimulation::SetCollisionIssues(std::vector<SimulationIssue> issues)
{
    std::erase_if(mIssues, [](const auto& issue) {
        return issue.kind == IssueKind::HolderStockCollision || issue.kind == IssueKind::CollisionCoverage;
    });
    mIssues.insert(mIssues.end(), std::make_move_iterator(issues.begin()), std::make_move_iterator(issues.end()));
    std::stable_sort(mIssues.begin(), mIssues.end(), [](const auto& a, const auto& b) {
        if (a.kind == IssueKind::CollisionCoverage) return false;
        if (b.kind == IssueKind::CollisionCoverage) return true;
        return a.command < b.command;
    });
    mIssueTimes.clear();
    for (const auto& issue : mIssues) {
        if (issue.command >= 0 && issue.command < static_cast<int>(mCommandTimes.size())) {
            mIssueTimes.push_back(mCommandTimes[issue.command]);
        }
        else {
            mIssueTimes.push_back(mTimeline.duration());
        }
    }
    mNextIssue = static_cast<int>(std::lower_bound(mIssueTimes.begin(), mIssueTimes.end(), mElapsedSeconds)
                                  - mIssueTimes.begin());
}

EndMill* MillSimulation::GetTool(int toolId)
{
    for (unsigned int i = 0; i < mToolTable.size(); i++) {
        if (mToolTable[i]->toolId == toolId) {
            return mToolTable[i];
        }
    }
    return nullptr;
}

void MillSimulation::RemoveTool(const int toolId)
{
    EndMill* tool = GetTool(toolId);
    if (tool == nullptr) {
        return;
    }

    if (const auto it = std::ranges::find(mToolTable, tool); it != mToolTable.end()) {
        mToolTable.erase(it);
    }
    delete tool;
}


void MillSimulation::AddTool(EndMill* tool)
{
    // if we have another tool with same id, remove it
    RemoveTool(tool->toolId);
    mToolTable.push_back(tool);
}

void MillSimulation::AddTool(const std::vector<float>& toolProfile, int toolid, float diameter)
{
    // if we have another tool with same id, remove it
    RemoveTool(toolid);
    EndMill* tool = new EndMill(toolProfile, toolid, diameter);
    mToolTable.push_back(tool);
}

void MillSimulation::SetHolderObject(
    int toolId,
    const std::vector<Vertex>& verts,
    const std::vector<GLushort>& indices
)
{
    mHolderObjects.erase(toolId);
    if (verts.empty() || indices.empty()) {
        return;
    }
    auto holder = std::make_unique<SolidObject>();
    holder->GenerateSolid(verts, indices);
    mHolderObjects.emplace(toolId, std::move(holder));
}

void MillSimulation::SetCollisionStock(const TopoDS_Shape& stock)
{
    mCollisionStock = stock;
}

bool MillSimulation::BuildFinalVoxelMesh(
    Mesh::MeshObject& mesh,
    double requestedCellSize,
    double& actualCellSize,
    const std::atomic_bool* cancel,
    const std::function<void(int)>& progress,
    int throughCommand
) const
{
    if (mCollisionStock.IsNull() || MillPathSegments.empty()) {
        return false;
    }
    VoxelStock stock;
    if (!stock.Initialize(mCollisionStock, requestedCellSize, 2000000, cancel, [&](int value) {
            if (progress) progress(value);
        })) {
        return false;
    }
    actualCellSize = stock.CellSize();
    const std::size_t count = std::count_if(
        MillPathSegments.begin(), MillPathSegments.end(), [throughCommand](const auto* segment) {
            return throughCommand < 0 || segment->sourceCommand <= throughCommand;
        }
    );
    if (count == 0) return false;
    std::size_t processed = 0;
    for (std::size_t i = 0; i < MillPathSegments.size(); ++i) {
        if (cancel && cancel->load()) return false;
        const auto* segment = MillPathSegments[i];
        if (throughCommand >= 0 && segment->sourceCommand > throughCommand) continue;
        if (segment->spindleDirection != 0 && segment->endmill
            && !stock.RemoveToolSegment(*segment->endmill, *segment, 1000000, cancel)) {
            return false;
        }
        if (progress) progress(40 + static_cast<int>(45.0 * (++processed) / count));
    }

    std::vector<VoxelStock::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    if (!stock.ExtractSurface(vertices, indices, 2000000, cancel, progress) || (cancel && cancel->load())) {
        return false;
    }
    std::vector<MeshCore::MeshGeomFacet> facets;
    facets.reserve(indices.size() / 3);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto& a = vertices[indices[i]];
        const auto& b = vertices[indices[i + 1]];
        const auto& c = vertices[indices[i + 2]];
        facets.emplace_back(
            Base::Vector3f(a.x, a.y, a.z),
            Base::Vector3f(b.x, b.y, b.z),
            Base::Vector3f(c.x, c.y, c.z)
        );
    }
    if (facets.empty()) return false;
    mesh.setFacets(facets);
    if (progress) progress(100);
    return true;
}

bool MillSimulation::BuildFinalVoxelComparison(
    const TopoDS_Shape& target,
    Mesh::MeshObject& excessMaterial,
    Mesh::MeshObject& missingTargetMaterial,
    double requestedCellSize,
    double& actualCellSize,
    double& excessVolume,
    double& missingVolume,
    bool& targetWithinStockBounds,
    const std::atomic_bool* cancel,
    const std::function<void(int)>& progress
) const
{
    if (mCollisionStock.IsNull() || target.IsNull() || MillPathSegments.empty()) {
        return false;
    }
    VoxelStock stock;
    if (!stock.Initialize(mCollisionStock, requestedCellSize, 2000000, cancel, progress)) {
        return false;
    }
    actualCellSize = stock.CellSize();
    const std::size_t count = MillPathSegments.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (cancel && cancel->load()) return false;
        const auto* segment = MillPathSegments[i];
        if (segment->spindleDirection != 0 && segment->endmill
            && !stock.RemoveToolSegment(*segment->endmill, *segment, 1000000, cancel)) {
            return false;
        }
        if (progress) progress(40 + static_cast<int>(40.0 * (i + 1) / count));
    }

    VoxelStock targetVoxels;
    if (!targetVoxels.InitializeOnGrid(target, stock, targetWithinStockBounds, cancel)) {
        return false;
    }
    if (cancel && cancel->load()) return false;

    VoxelStock excessVoxels;
    VoxelStock missingVoxels;
    if (!stock.BuildDifferenceMasks(targetVoxels, excessVoxels, missingVoxels)) {
        return false;
    }
    excessVolume = excessVoxels.RemainingVolume();
    missingVolume = missingVoxels.RemainingVolume();

    const auto toMesh = [&](const VoxelStock& voxels, Mesh::MeshObject& mesh,
                            const std::function<void(int)>& surfaceProgress) {
        if (voxels.RemainingVolume() <= 0) return true;
        std::vector<VoxelStock::MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
        if (!voxels.ExtractSurface(vertices, indices, 2000000, cancel, surfaceProgress)) {
            return false;
        }
        std::vector<MeshCore::MeshGeomFacet> facets;
        facets.reserve(indices.size() / 3);
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const auto& a = vertices[indices[i]];
            const auto& b = vertices[indices[i + 1]];
            const auto& c = vertices[indices[i + 2]];
            facets.emplace_back(
                Base::Vector3f(a.x, a.y, a.z),
                Base::Vector3f(b.x, b.y, b.z),
                Base::Vector3f(c.x, c.y, c.z)
            );
        }
        if (facets.empty()) return false;
        mesh.setFacets(facets);
        return true;
    };
    if (progress) progress(86);
    if (!toMesh(excessVoxels, excessMaterial, [&](int value) {
            if (progress) progress(86 + (value - 85) / 3);
        })) {
        return false;
    }
    if (cancel && cancel->load()) return false;
    if (!toMesh(missingVoxels, missingTargetMaterial, [&](int value) {
            if (progress) progress(91 + (value - 85) / 2);
        })) {
        return false;
    }
    if (progress) progress(100);
    return true;
}

void MillSimulation::SetHolderCollisionShape(int toolId, const TopoDS_Shape& holder)
{
    if (holder.IsNull()) {
        mHolderCollisionShapes.erase(toolId);
    }
    else {
        mHolderCollisionShapes[toolId] = holder;
    }
}

bool MillSimulation::ToolExists(int toolid)
{
    return GetTool(toolid) != nullptr;
}

void MillSimulation::GlsimStart()
{
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_STENCIL_TEST);
}

void MillSimulation::GlsimToolStep1(void)
{
    glCullFace(GL_BACK);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_ZERO, GL_ZERO, GL_REPLACE);
}

void MillSimulation::GlsimToolStep2(void)
{
    glCullFace(GL_FRONT);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void MillSimulation::GlsimClipBack(void)
{
    glCullFace(GL_FRONT);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_REPLACE, GL_REPLACE, GL_ZERO);
}

void MillSimulation::GlsimRenderStock(void)
{
    glCullFace(GL_BACK);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthFunc(GL_EQUAL);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void MillSimulation::GlsimRenderTools(void)
{
    glCullFace(GL_FRONT);
}

void MillSimulation::GlsimEnd(void)
{
    glCullFace(GL_BACK);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glDisable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
}

void MillSimulation::renderSegmentForward(int iSeg)
{
    MillPathSegment* p = MillPathSegments.at(iSeg);
    if (!p->isMultyPart) {
        const float step = iSeg == mPathStep ? mSubStep + mStepFraction : p->numSimSteps;
        GlsimToolStep1();
        p->render(step);
        GlsimToolStep2();
        p->render(step);
        return;
    }
    int step = iSeg == mPathStep ? mSubStep : p->numSimSteps;
    int start = p->isMultyPart ? 1 : step;
    for (int i = start; i <= step; i++) {
        GlsimToolStep1();
        p->render(i);
        GlsimToolStep2();
        p->render(i);
    }
}

void MillSimulation::renderSegmentReversed(int iSeg)
{
    MillPathSegment* p = MillPathSegments.at(iSeg);
    if (!p->isMultyPart) {
        const float step = iSeg == mPathStep ? mSubStep + mStepFraction : p->numSimSteps;
        GlsimToolStep1();
        p->render(step);
        GlsimToolStep2();
        p->render(step);
        return;
    }
    int step = iSeg == mPathStep ? mSubStep : p->numSimSteps;
    int end = p->isMultyPart ? 1 : step;
    for (int i = step; i >= end; i--) {
        GlsimToolStep1();
        p->render(i);
        GlsimToolStep2();
        p->render(i);
    }
}

void MillSimulation::CalcSegmentPositions()
{
    if (MillPathSegments.empty()) {
        mPathStep = -1;
        mSubStep = 0;
        return;
    }
    mSubStep = mCurStep;
    for (mPathStep = 0; mPathStep < mNPathSteps; mPathStep++) {
        MillPathSegment* p = MillPathSegments[mPathStep];
        if (mSubStep < p->numSimSteps) {
            break;
        }
        mSubStep -= p->numSimSteps;
    }
    if (mPathStep >= mNPathSteps) {
        mPathStep = mNPathSteps - 1;
        mSubStep = MillPathSegments[mPathStep]->numSimSteps;
    }

}

void MillSimulation::RenderSimulation()
{
    if ((mViewItems & VIEWITEM_SIMULATION) == 0) {
        return;
    }

    simDisplay.StartDepthPass();

    GlsimStart();
    mStockObject.render();

    GlsimToolStep2();

    for (int i = 0; i <= mPathStep; i++) {
        renderSegmentForward(i);
    }

    for (int i = mPathStep; i >= 0; i--) {
        renderSegmentForward(i);
    }

    for (int i = 0; i < mPathStep; i++) {
        renderSegmentReversed(i);
    }

    for (int i = mPathStep; i >= 0; i--) {
        renderSegmentReversed(i);
    }

    GlsimClipBack();
    mStockObject.render();

    // start coloring
    simDisplay.StartGeometryPass(stockColor, false);
    GlsimRenderStock();
    mStockObject.render();

    // render cuts (back faces of tools)
    simDisplay.StartGeometryPass(cutColor, true);
    GlsimRenderTools();
    for (int i = 0; i <= mPathStep; i++) {
        MillPathSegment* p = MillPathSegments.at(i);
        if (!p->isMultyPart) {
            p->render(i == mPathStep ? mSubStep + mStepFraction : p->numSimSteps);
            continue;
        }
        int step = (i == mPathStep) ? mSubStep : p->numSimSteps;
        int start = p->isMultyPart ? 1 : step;
        for (int j = start; j <= step; j++) {
            MillPathSegments.at(i)->render(j);
        }
    }

    GlsimEnd();
}

void MillSimulation::RenderTool()
{
    if (mPathStep < 0) {
        return;
    }

    MillPathSegment* p = MillPathSegments.at(mPathStep);
    vec3 toolPos;
    p->GetHeadPosition(toolPos, (mSubStep + mStepFraction) / p->numSimSteps);
    mat4x4 tmat;
    mat4x4_translate(tmat, toolPos[0], toolPos[1], toolPos[2]);
    // mat4x4_translate(tmat, toolPos.x, toolPos.y, toolPos.z);
    simDisplay.StartGeometryPass(toolColor, false);
    p->endmill->toolShape.Render(tmat, identityMat);

    const auto holder = mHolderObjects.find(p->endmill->toolId);
    if (holder != mHolderObjects.end()) {
        static const vec3 holderColor = {0.62f, 0.66f, 0.72f};
        simDisplay.StartGeometryPass(holderColor, false);
        holder->second->SetPosition(toolPos);
        holder->second->render();
    }
}

void MillSimulation::RenderPath()
{
    if (!mViewPath) {
        return;
    }
    simDisplay.SetupLinePathPass(mPathStep, false);
    millPathLine.Render();
    simDisplay.SetupLinePathPass(mPathStep, true);
    millPathLine.Render();
    glDepthMask(GL_TRUE);
}

void MillSimulation::RenderBaseShape()
{
    if ((mViewItems & VIEWITEM_BASE_SHAPE) == 0) {
        return;
    }
    simDisplay.StartDepthPass();
    glPolygonOffset(0, -2);
    glEnable(GL_POLYGON_OFFSET_FILL);
    simDisplay.StartGeometryPass(baseShapeColor, false);
    mBaseShape.render();
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void MillSimulation::Render()
{
    if (!simulationInitiated) {
        return;
    }

    // set background
    glClearColor(bgndColor[0], bgndColor[1], bgndColor[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // render the simulation offscreen in an FBO

    if (simDisplay.updateDisplay) {
        simDisplay.PrepareFrameBuffer();
        RenderSimulation();
        RenderTool();
        RenderBaseShape();
        RenderPath();
        simDisplay.updateDisplay = false;
        simDisplay.RenderResult(true, mViewSSAO);
    }
    else {
        simDisplay.RenderResult(false, mViewSSAO);
    }

    /*   if (mDebug > 0) {
           mat4x4 test;
           mat4x4_identity(test);
           mat4x4_translate_in_place(test, 20, 20, 3);
           mat4x4_rotate_Z(test, test, 30.f * 3.14f / 180.f);
           int dpos = mNPathSteps - mDebug2;
           MillPathSegment* p = MillPathSegments.at(dpos);
           if (mDebug > p->numSimSteps) {
               mDebug = 1;
           }
           p->render(mDebug);
       }*/

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void MillSimulation::ProcessSim(const clock::duration& elapsed)
{
    SimNext(elapsed);
    Render();
}

void MillSimulation::SimNext(const clock::duration& elapsed)
{
    if (!mSimPlaying) return;
    const double targetTime = std::min(
        mTimeline.duration(),
        mElapsedSeconds + std::max(0.0, std::chrono::duration<double>(elapsed).count()) * mSimSpeed
    );
    while (mNextToolChange < mToolChangeCommands.size()) {
        const int command = mToolChangeCommands[mNextToolChange];
        const double changeTime = mCommandTimes[command];
        if (changeTime > targetTime) break;
        ++mNextToolChange;
        if (mPauseOnToolChanges) {
            SeekCommand(command);
            mPausedToolChange = command;
            return;
        }
    }
    while (mNextIssue < static_cast<int>(mIssues.size()) && mIssueTimes[mNextIssue] <= targetTime) {
        const int issue = mNextIssue;
        const int command = mIssues[issue].command;
        if (mPauseOnIssues && mIssues[issue].kind != IssueKind::CollisionCoverage) {
            SeekCommand(command);
            mPausedIssue = issue;
        }
        // Acknowledge all findings on this command together. Continue never re-pauses here.
        while (mNextIssue < static_cast<int>(mIssues.size()) && mIssues[mNextIssue].command == command) {
            ++mNextIssue;
        }
        if (mPauseOnIssues && mIssues[issue].kind != IssueKind::CollisionCoverage) return;
    }
    mElapsedSeconds = targetTime;
    const double position = mTimeline.stepAtTime(mElapsedSeconds);
    if (mElapsedSeconds >= mTimeline.duration()) mSimPlaying = false;

    const int step = static_cast<int>(position);
    const double fraction = position - step;
    if (step != mCurStep || fraction != mStepFraction) {
        mCurStep = step;
        mStepFraction = fraction;
        CalcSegmentPositions();
        simDisplay.updateDisplay = true;
    }
}

void MillSimulation::InitDisplay(float quality)
{
    // generate tools
    for (unsigned int i = 0; i < mToolTable.size(); i++) {
        mToolTable[i]->GenerateDisplayLists(quality);
    }

    // Make sure the next call to UpdateWindowScale will not return early.
    mWidth = -1;
    mHeight = -1;

    // init 3d display
    simDisplay.InitGL();
}

void MillSimulation::SetBoxStock(float x, float y, float z, float l, float w, float h)
{
    mStockObject.GenerateBoxStock(x, y, z, l, w, h);
    simDisplay.ScaleViewToStock(&mStockObject);
}

void MillSimulation::SetArbitraryStock(
    const std::vector<Vertex>& verts,
    const std::vector<GLushort>& indices
)
{
    mStockObject.GenerateSolid(verts, indices);
    simDisplay.ScaleViewToStock(&mStockObject);
}

void MillSimulation::SetStockVisible(bool b)
{
    if (b == IsStockVisible()) {
        return;
    }

    mViewItems ^= VIEWITEM_SIMULATION;
    simDisplay.updateDisplay = true;
}

bool MillSimulation::IsStockVisible() const
{
    return mViewItems & VIEWITEM_SIMULATION;
}

void MillSimulation::SetBaseObject(const std::vector<Vertex>& verts, const std::vector<GLushort>& indices)
{
    mBaseShape.GenerateSolid(verts, indices);
}

void MillSimulation::SetBaseVisible(bool b)
{
    if (b == IsBaseVisible()) {
        return;
    }

    mViewItems ^= VIEWITEM_BASE_SHAPE;
    simDisplay.updateDisplay = true;
}

bool MillSimulation::IsBaseVisible() const
{
    return mViewItems & VIEWITEM_BASE_SHAPE;
}

void MillSimulation::UpdateWindowScale(int width, int height)
{
    if (width == mWidth && height == mHeight) {
        return;
    }

    mWidth = width;
    mHeight = height;

    simDisplay.UpdateWindowScale(width, height);
}

void MillSimulation::SetPathVisible(bool b)
{
    if (b == mViewPath) {
        return;
    }

    mViewPath = b;
    simDisplay.updateDisplay = true;
}

void MillSimulation::EnableSsao(bool b)
{
    if (b == mViewSSAO) {
        return;
    }

    mViewSSAO = b;
    simDisplay.updateDisplay = true;
}

void MillSimulation::UpdateCamera(const SoCamera& camera)
{
    simDisplay.UpdateCamera(camera);
}

bool MillSimulation::LoadGCodeFile(const char* fileName)
{
    if (mCodeParser.Parse(fileName)) {
        std::cout << "GCode file loaded successfully" << std::endl;
        return true;
    }
    return false;
}

bool MillSimulation::AddGcodeLine(const char* line, double horizontalRapid, double verticalRapid)
{
    return mCodeParser.AddLine(line, horizontalRapid, verticalRapid);
}

void MillSimulation::SetPlaying(bool b)
{
    if (b == mSimPlaying) {
        return;
    }

    mSimPlaying = b;
    if (b) {
        mSelectedCommand = -1;
        mPausedIssue = -1;
        mPausedToolChange = -1;
    }
    simDisplay.updateDisplay = true;
}

void MillSimulation::SetPauseOnIssues(bool enabled)
{
    mPauseOnIssues = enabled;
}

void MillSimulation::SetPauseOnToolChanges(bool enabled)
{
    mPauseOnToolChanges = enabled;
}

void MillSimulation::ContinueAfterIssue(bool withoutPausing)
{
    if (withoutPausing) SetPauseOnIssues(false);
    // An issue selected manually can be acknowledged with the same Continue controls.
    while (mNextIssue < static_cast<int>(mIssues.size())
           && mIssues[mNextIssue].command == mSelectedCommand) {
        ++mNextIssue;
    }
    SetPlaying(true);
}

void MillSimulation::SingleStep()
{
    const int current = GetCurrentCommand();
    if (current + 1 < static_cast<int>(mCodeParser.CommandStates.size())) {
        SeekCommand(current + 1);
    }
    else {
        SetPlaying(false);
        SetSimulationStage(1);
    }
}

int MillSimulation::GetCurrentCommand() const
{
    if (mSelectedCommand < 0) {
        for (const auto& dwell : mDwellIntervals) {
            if (mElapsedSeconds >= dwell.start && mElapsedSeconds < dwell.end) return dwell.command;
        }
    }
    if (mSelectedCommand >= 0) {
        return mSelectedCommand;
    }
    const auto* motion = GetCurrentMotion();
    return motion ? motion->sourceCommand : -1;
}

void MillSimulation::SeekCommand(int command)
{
    if (command < 0 || command >= static_cast<int>(mCodeParser.CommandStates.size())) {
        return;
    }
    const int position = mCommandSteps.empty() ? 0 : mCommandSteps[command];
    mPausedIssue = -1;
    mPausedToolChange = -1;
    mNextToolChange = static_cast<std::size_t>(std::upper_bound(
        mToolChangeCommands.begin(), mToolChangeCommands.end(), command
    ) - mToolChangeCommands.begin());
    mNextIssue = static_cast<int>(std::lower_bound(mIssues.begin(), mIssues.end(), command,
        [](const auto& issue, int source) { return issue.command < source; }) - mIssues.begin());
    SetPlaying(false);
    mCurStep = position;
    mStepFraction = 0;
    mElapsedSeconds = mCommandTimes.empty() ? 0.0 : mCommandTimes[command];
    mSelectedCommand = command;
    CalcSegmentPositions();
    simDisplay.updateDisplay = true;
}

void MillSimulation::SetSpeed(int s)
{
    mSimSpeed = s;
}

void MillSimulation::SetSimulationStage(float stage)
{
    mSelectedCommand = -1;
    mPausedIssue = -1;
    mPausedToolChange = -1;
    const double position = std::clamp(static_cast<double>(stage), 0.0, 1.0) * mNTotalSteps;
    mCurStep = static_cast<int>(position);
    mStepFraction = position - mCurStep;
    mElapsedSeconds = mTimeline.timeAtStep(position);
    mNextToolChange = static_cast<std::size_t>(std::upper_bound(
        mToolChangeCommands.begin(), mToolChangeCommands.end(), mElapsedSeconds,
        [this](double time, int command) { return time < mCommandTimes[command]; }
    ) - mToolChangeCommands.begin());
    mNextIssue = static_cast<int>(std::lower_bound(mIssueTimes.begin(), mIssueTimes.end(), mElapsedSeconds)
                                  - mIssueTimes.begin());
    CalcSegmentPositions();
    simDisplay.updateDisplay = true;
}

void MillSimulation::SetState(const MillSimulationState& state)
{
    mSelectedCommand = -1;
    SetPlaying(state.mSimPlaying);

    mElapsedSeconds = std::clamp(state.mElapsedSeconds, 0.0, mTimeline.duration());
    mNextToolChange = static_cast<std::size_t>(std::upper_bound(
        mToolChangeCommands.begin(), mToolChangeCommands.end(), mElapsedSeconds,
        [this](double time, int command) { return time < mCommandTimes[command]; }
    ) - mToolChangeCommands.begin());
    const double position = mTimeline.stepAtTime(mElapsedSeconds);
    mCurStep = static_cast<int>(position);
    mStepFraction = position - mCurStep;
    CalcSegmentPositions();
    simDisplay.updateDisplay = true;

    if (!state.mSimPlaying && state.mSelectedCommand >= 0) {
        SeekCommand(state.mSelectedCommand);
    }
    mPauseOnIssues = state.mPauseOnIssues;
    mPauseOnToolChanges = state.mPauseOnToolChanges;
    mNextIssue = std::clamp(state.mNextIssue, 0, static_cast<int>(mIssues.size()));
    mPausedIssue = state.mPausedIssue >= 0 && state.mPausedIssue < static_cast<int>(mIssues.size())
        ? state.mPausedIssue : -1;
    mPausedToolChange = state.mPausedToolChange;
    SetSpeed(state.mSimSpeed);

    mViewItems = state.mViewItems;
    mViewPath = state.mViewPath;
    mViewSSAO = state.mViewSSAO;
}

const MillSimulationState& MillSimulation::GetState() const
{
    return *this;
}

const MillMotion* MillSimulation::GetCurrentMotion() const
{
    if (mSelectedCommand < 0) {
        for (const auto& dwell : mDwellIntervals) {
            if (mElapsedSeconds >= dwell.start && mElapsedSeconds < dwell.end) {
                return &mCodeParser.CommandStates[dwell.command];
            }
        }
    }
    if (mSelectedCommand >= 0) {
        return &mCodeParser.CommandStates[mSelectedCommand];
    }
    if (MillPathSegments.empty()) {
        return mCodeParser.CommandStates.empty() ? nullptr : &mCodeParser.CommandStates.back();
    }
    // Apply commands after the final movement too (for example a trailing M9).
    if (mCurStep >= mNTotalSteps) {
        return &mCodeParser.lastState;
    }
    // Before the first frame, show the first segment's programmed settings.
    const int step = std::clamp(mPathStep, 0, static_cast<int>(MillPathSegments.size()) - 1);
    const int operation = MillPathSegments[step]->indexInArray;
    return &mCodeParser.Operations[operation];
}

double MillSimulation::GetCurrentFeed() const
{
    return mSelectedCommand >= 0 ? -1 : (mPathStep >= 0 ? MillPathSegments[mPathStep]->travelFeed : -1);
}

bool MillSimulation::IsTimingEstimated() const
{
    for (const auto& dwell : mDwellIntervals) {
        if (mElapsedSeconds >= dwell.start && mElapsedSeconds < dwell.end) return false;
    }
    return mPathStep >= 0 && MillPathSegments[mPathStep]->timingEstimated;
}

double MillSimulation::GetTotalDuration() const
{
    return mTimeline.duration();
}

bool MillSimulation::HasEstimatedRates() const
{
    return mHasEstimatedRates;
}

void MillSimulation::SetBackgroundColor(const vec3& c)
{
    bgndColor[0] = c[0];
    bgndColor[1] = c[1];
    bgndColor[2] = c[2];
}

void MillSimulation::SetPathColor(const vec3& normal, const vec3& rapid)
{
    simDisplay.SetPathColor(normal, rapid);
}


}  // namespace CAMSimulator
