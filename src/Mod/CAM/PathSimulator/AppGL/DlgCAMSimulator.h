// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2017 Shai Seger <shaise at gmail>                       *
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

#ifdef _MSC_VER
# pragma warning(disable : 4251)
#endif

#include "CommandsPanel.h"
#include <queue>
#include <functional>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_map>

#include <QOpenGLWidget>
#include <QPainter>
#include <QTimer>
#include <QExposeEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QOpenGLContext>

#include <Mod/Part/App/TopoShape.h>

class SoCamera;
class QProgressDialog;
namespace Mesh { class MeshObject; }

namespace Gui
{
class MDIView;
class Document;
}  // namespace Gui

namespace CAMSimulator
{

// use short declaration as using 'include' causes a header loop
class MillSimulation;
struct MillSimulationState;
struct Vertex;
class ViewCAMSimulator;
class GuiDisplay;
class Dummy3DViewer;

struct SimShape
{
public:
    float maxDimension() const;

public:
    std::vector<Vertex> verts;
    std::vector<GLushort> indices;
    bool needsUpdate = false;
};

struct SimHolder
{
    SimShape renderShape;
    Part::TopoShape collisionShape;
};

struct SimTool
{
public:
    std::vector<float> profile;
    int id;
    float diameter;
    float resolution;
};

class DlgCAMSimulator: public QOpenGLWidget
{
    Q_OBJECT

    typedef std::chrono::steady_clock clock;

public:
    explicit DlgCAMSimulator(QWidget* parent = nullptr);
    ~DlgCAMSimulator() override;

    void connectTo(GuiDisplay& gui, Dummy3DViewer& dv);
    void connectCommands(CommandsPanel& panel);
    void setGuiDocument(Gui::Document* document);
    void setOperation(const char* operation, const char* tool);
    void cloneFrom(const DlgCAMSimulator& from);

    static DlgCAMSimulator* instance(Gui::Document* doc = nullptr);

    void setAnimating(bool animating);
    void startSimulation(const Part::TopoShape& stock, float quality);
    void resetSimulation();

    void addGcodeCommand(const char* cmd, double horizontalRapid = -1, double verticalRapid = -1);
    void addTool(
        const std::vector<float>& toolProfilePoints,
        int toolNumber,
        float diameter,
        float resolution
    );
    void addHolderShape(const Part::TopoShape& shape, int toolNumber, float resolution);

    void setStockShape(const Part::TopoShape& shape, float resolution);
    void setStockVisible(bool b);
    void setBaseShape(const Part::TopoShape& shape, float resolution);
    void setBaseVisible(bool b);

    void setRotateEnabled(bool b);

    void setBackgroundColor(const QColor& c);
    void setPathColor(const QColor& normal, const QColor& rapid);

Q_SIGNALS:
    void simulationStarted();

protected:
    void timerEvent(QTimerEvent* event) override;

    void updateResources();
    void updateWindowScale();
    void updateCamera();

    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

    void updateGui();

private:
    void startHolderCollisionCheck();
    void cancelHolderCollisionCheck();
    void stopHolderCollisionCheck();
    void exportSimulationReport();
    void createVoxelStockPreview();
    void createIntermediateVoxelStockPreview();
    void createVoxelStockPreviewThroughCommand(int command);
    void createStockComparison();
    void stopVoxelStockPreview();

    bool mNeedsInitialize = false;
    bool mNeedsClear = false;
    bool mAnimating = false;
    int mAnimatingTimer = 0;

    std::unique_ptr<MillSimulation> mMillSimulator;
    Gui::Document* mGuiDocument = nullptr;
    float mQuality = 10;

    std::vector<SimulationCommand> mGCode;
    std::size_t mLastGCode = 0;

    std::vector<SimTool> mTools;
    std::unordered_map<int, SimHolder> mHolderShapes;
    Part::TopoShape mCollisionStockShape;
    Part::TopoShape mTargetShape;

    const SoCamera* mCamera = nullptr;
    SimShape mStock;
    SimShape mBase;

    std::unique_ptr<MillSimulationState> mState;
    clock::time_point mLastProcessSim = clock::time_point::min();

    int mLastPausedIssue = -1;
    std::atomic_bool mCancelCollisionCheck {false};
    std::thread mCollisionThread;
    bool mCollisionCheckRunning = false;
    std::uint64_t mCollisionGeneration = 0;
    std::thread mVoxelStockThread;
    std::atomic_bool mCancelVoxelStock {false};
    bool mVoxelStockRunning = false;
    std::uint64_t mVoxelStockGeneration = 0;
    QProgressDialog* mVoxelStockProgress = nullptr;
    CommandsPanel* mCommands = nullptr;
    int mOperationId = 0;
    std::string mOperation;
    std::string mToolLabel;
    GuiDisplay* mGui = nullptr;
    QColor mBackground;
    Dummy3DViewer* mDummyViewer = nullptr;
};

}  // namespace CAMSimulator
