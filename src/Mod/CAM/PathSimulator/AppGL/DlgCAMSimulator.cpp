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


#include "DlgCAMSimulator.h"

#include "Dummy3DViewer.h"
#include "GuiDisplay.h"
#include "MillSimulation.h"
#include "ViewCAMSimulator.h"
#include "MillPathSegment.h"
#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/Selection/Selection.h>
#include <Gui/ViewProvider.h>
#include <Gui/ViewProviderGeometryObject.h>
#include <Gui/View3DInventorViewer.h>
#include <Mod/Mesh/App/MeshFeature.h>
#include <Mod/Mesh/App/Mesh.h>
#include <Inventor/nodes/SoCamera.h>
#include <Standard_Failure.hxx>
#include <QSurfaceFormat>
#include <QMetaObject>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>

// include this last as the defines can mess up other includes
#include "OpenGlWrapper.h"

using namespace std::literals;

namespace CAMSimulator
{

float SimShape::maxDimension() const
{
    float xmin = NAN, ymin = NAN, zmin = NAN;
    float xmax = NAN, ymax = NAN, zmax = NAN;

    for (const auto& v : verts) {
        xmin = std::fmin(xmin, v.x);
        ymin = std::fmin(ymin, v.x);
        zmin = std::fmin(zmin, v.x);

        xmax = std::fmax(xmax, v.x);
        ymax = std::fmax(ymax, v.x);
        zmax = std::fmax(zmax, v.x);
    }

    const float xsize = xmax - xmin;
    const float ysize = ymax - ymin;
    const float zsize = zmax - zmin;

    return std::max(std::max(xsize, ysize), zsize);
}


DlgCAMSimulator::DlgCAMSimulator(QWidget* parent)
    : QOpenGLWidget(parent)
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    int samples = Gui::View3DInventorViewer::getNumSamples();
    if (samples > 1) {
        format.setSamples(samples);
    }
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    setFormat(format);

    setMouseTracking(true);

    mMillSimulator.reset(new MillSimulation);
}

DlgCAMSimulator::~DlgCAMSimulator()
{
    stopHolderCollisionCheck();
    stopVoxelStockPreview();
    makeCurrent();
    mMillSimulator = nullptr;
}

void DlgCAMSimulator::setGuiDocument(Gui::Document* document)
{
    mGuiDocument = document;
}

void DlgCAMSimulator::connectTo(GuiDisplay& gui, Dummy3DViewer& dv)
{
    // connect to gui

    mGui = &gui;
    if (mBackground.isValid()) {
        mGui->setBackgroundColor(mBackground);
    }
    updateGui();

    connect(&gui, &GuiDisplay::play, this, [this](bool b) { mMillSimulator->SetPlaying(b); });
    connect(&gui, &GuiDisplay::collisionCheckRequested, this, &DlgCAMSimulator::startHolderCollisionCheck);
    connect(&gui, &GuiDisplay::collisionCheckCancelRequested, this, &DlgCAMSimulator::cancelHolderCollisionCheck);

    connect(&gui, &GuiDisplay::singleStep, this, [this] { mMillSimulator->SingleStep(); });

    connect(&gui, &GuiDisplay::speedChanged, this, [this](int speed) {
        mMillSimulator->SetSpeed(speed);
    });

    connect(&gui, &GuiDisplay::stageChanged, this, [this](float f) {
        mMillSimulator->SetSimulationStage(f);
    });

    connect(&gui, &GuiDisplay::rotateEnableChanged, this, &DlgCAMSimulator::setRotateEnabled);

    connect(&gui, &GuiDisplay::pathVisibleChanged, this, [this](bool b) {
        mMillSimulator->SetPathVisible(b);
    });

    connect(&gui, &GuiDisplay::ssaoEnableChanged, this, [this](bool b) {
        mMillSimulator->EnableSsao(b);
    });

    connect(&gui, &GuiDisplay::stockVisibleChanged, this, &DlgCAMSimulator::setStockVisible);
    connect(&gui, &GuiDisplay::baseVisibleChanged, this, &DlgCAMSimulator::setBaseVisible);

    // connect to dummy viewer

    mDummyViewer = &dv;

    mDummyViewer->setStockVisible(mMillSimulator->IsStockVisible());
    mDummyViewer->setBaseVisible(mMillSimulator->IsBaseVisible());

    // connect gui and dummy viewer

    connect(
        &gui,
        &GuiDisplay::viewAll,
        &dv,
        static_cast<void (Dummy3DViewer::*)()>(&Dummy3DViewer::viewAll)
    );
}

void DlgCAMSimulator::connectCommands(CommandsPanel& panel)
{
    mCommands = &panel;
    connect(&panel, &CommandsPanel::pauseOnIssuesChanged, this, [this](bool enabled) {
        mMillSimulator->SetPauseOnIssues(enabled);
    });
    connect(&panel, &CommandsPanel::pauseOnToolChangesChanged, this, [this](bool enabled) {
        mMillSimulator->SetPauseOnToolChanges(enabled);
    });
    connect(&panel, &CommandsPanel::reportRequested, this, &DlgCAMSimulator::exportSimulationReport);
    connect(&panel, &CommandsPanel::voxelStockPreviewRequested, this, &DlgCAMSimulator::createVoxelStockPreview);
    connect(&panel, &CommandsPanel::intermediateStockPreviewRequested, this, &DlgCAMSimulator::createIntermediateVoxelStockPreview);
    connect(&panel, &CommandsPanel::stockComparisonRequested, this, &DlgCAMSimulator::createStockComparison);
    connect(&panel, &CommandsPanel::continueRequested, this, [this](bool withoutPausing) {
        if (mNeedsClear || mNeedsInitialize) return;
        mMillSimulator->ContinueAfterIssue(withoutPausing);
        updateGui();
    });
    connect(&panel, &CommandsPanel::commandSelected, this, [this](int command) {
        if (mNeedsClear || mNeedsInitialize) return;
        mMillSimulator->SeekCommand(command);
        updateGui();
        update();
    });
}

void DlgCAMSimulator::setOperation(const char* operation, const char* tool)
{
    ++mOperationId;
    mOperation = operation;
    mToolLabel = tool;
}

void DlgCAMSimulator::updateGui()
{
    if (!mGui) {
        return;
    }

    const auto state = mMillSimulator->GetState();
    if (mCommands) {
        mCommands->setIssueState(state.mPauseOnIssues, state.mPausedIssue, state.mSimPlaying);
        mCommands->setPauseOnToolChanges(state.mPauseOnToolChanges);
        if (state.mPausedIssue >= 0 && state.mPausedIssue != mLastPausedIssue) {
            mGui->setCommandsVisible(true);
            mCommands->setFollowing(true);
        }
        if (state.mPausedToolChange >= 0) {
            mGui->setCommandsVisible(true);
            mCommands->setFollowing(true);
        }
        mCommands->setCurrentCommand(mMillSimulator->GetCurrentCommand());
    }
    mLastPausedIssue = state.mPausedIssue;
    mGui->setIssueStatus(mMillSimulator->GetIssues().size(), state.mPausedIssue >= 0);

    mGui->setPlaying(state.mSimPlaying);
    mGui->setSpeed(state.mSimSpeed);
    mGui->setMotion(
        mMillSimulator->GetCurrentMotion(),
        mMillSimulator->GetCurrentFeed(),
        mMillSimulator->IsTimingEstimated()
    );

    mGui->setMachiningTime(
        state.mElapsedSeconds,
        mMillSimulator->GetTotalDuration(),
        mMillSimulator->HasEstimatedRates()
    );

    const float stage = state.mNTotalSteps > 0 ? (float)state.mCurStep / state.mNTotalSteps : 0.0f;
    mGui->setStage(stage, state.mNTotalSteps);

    mGui->setStockVisible(state.mViewItems & VIEWITEM_SIMULATION);
    mGui->setBaseVisible(state.mViewItems & VIEWITEM_BASE_SHAPE);

    mGui->setPathVisible(state.mViewPath);
    mGui->setSsaoEnabled(state.mViewSSAO);

    if (mDummyViewer && !mDummyViewer->isAnimating()) {
        mGui->setRotateEnabled(false);
    }
}

void DlgCAMSimulator::cloneFrom(const DlgCAMSimulator& from)
{
    mNeedsInitialize = true;
    mNeedsClear = true;
    setAnimating(from.mAnimating);

    mQuality = from.mQuality;

    mGCode = from.mGCode;
    mOperationId = from.mOperationId;
    mOperation = from.mOperation;
    mToolLabel = from.mToolLabel;
    if (mCommands) mCommands->setCommands(mGCode);
    mTools = from.mTools;

    mStock = from.mStock;
    mStock.needsUpdate = true;

    mBase = from.mBase;
    mBase.needsUpdate = true;
    mTargetShape = from.mTargetShape;

    const auto state = from.mMillSimulator->GetState();
    mState = std::make_unique<MillSimulationState>(state);
}

DlgCAMSimulator* DlgCAMSimulator::instance(Gui::Document* doc)
{
    return &ViewCAMSimulator::instance(doc).dlg();
}

void DlgCAMSimulator::setAnimating(bool animating)
{
    if (animating == mAnimating) {
        return;
    }

    mAnimating = animating;

    if (animating && mAnimatingTimer == 0) {
        mAnimatingTimer = startTimer(0);
    }
    else if (!animating && mAnimatingTimer != 0) {
        killTimer(mAnimatingTimer);
        mAnimatingTimer = 0;
    }
}

void DlgCAMSimulator::startSimulation(const Part::TopoShape& stock, float quality)
{
    stopHolderCollisionCheck();
    stopVoxelStockPreview();
    if (mGui) mGui->clearCollisionCheckState();
    if (mCommands) mCommands->setCommands(mGCode);
    mQuality = quality;
    mNeedsInitialize = true;

    setStockShape(stock, 1);
    setAnimating(true);

    Q_EMIT simulationStarted();
}

void DlgCAMSimulator::resetSimulation()
{
    stopHolderCollisionCheck();
    stopVoxelStockPreview();
    if (mGui) mGui->clearCollisionCheckState();
    mNeedsClear = true;

    mGCode.clear();
    mOperationId = 0;
    mOperation.clear();
    mToolLabel.clear();
    if (mCommands) mCommands->setCommands(mGCode);
    mTools.clear();
    mHolderShapes.clear();
    mCollisionStockShape = Part::TopoShape();
    mTargetShape = Part::TopoShape();
    mStock = {};
    mBase = {};
}

void DlgCAMSimulator::startHolderCollisionCheck()
{
    if (mCollisionCheckRunning || mNeedsClear || mNeedsInitialize || !mMillSimulator) {
        return;
    }
    if (mCollisionThread.joinable()) {
        mCollisionThread.join();
    }

    const auto generation = ++mCollisionGeneration;
    mCancelCollisionCheck.store(false);
    mCollisionCheckRunning = true;
    mMillSimulator->SetPlaying(false);
    if (mGui) mGui->setPlaying(false);
    if (mGui) mGui->setCollisionCheckState(tr("Preparing collision check…"), 0, true);

    mCollisionThread = std::thread([this, generation] {
        std::vector<SimulationIssue> issues;
        try {
            issues = mMillSimulator->AnalyzeHolderCollisions(
                &mCancelCollisionCheck,
                [this, generation](int progress) {
                    QMetaObject::invokeMethod(this, [this, generation, progress] {
                        if (generation != mCollisionGeneration || !mCollisionCheckRunning
                            || mCancelCollisionCheck.load() || !mGui) {
                            return;
                        }
                        mGui->setCollisionCheckState(
                            tr("Checking holders and stock… %1%").arg(progress), progress, true
                        );
                    }, Qt::QueuedConnection);
                }
            );
        }
        catch (const Standard_Failure& failure) {
            const char* message = failure.GetMessageString();
            issues.push_back({-1, IssueSeverity::Warning, IssueKind::CollisionCoverage,
                std::string("Collision analysis failed: ") + (message ? message : "OpenCascade error")});
        }
        catch (const std::exception& error) {
            issues.push_back({-1, IssueSeverity::Warning, IssueKind::CollisionCoverage,
                std::string("Collision analysis failed: ") + error.what()});
        }
        catch (...) {
            issues.push_back({-1, IssueSeverity::Warning, IssueKind::CollisionCoverage,
                "Collision analysis failed due to an unknown error; no clear result is available."});
        }

        QMetaObject::invokeMethod(this, [this, generation, issues = std::move(issues)]() mutable {
            if (generation != mCollisionGeneration || !mCollisionCheckRunning) {
                return;
            }
            const bool cancelled = mCancelCollisionCheck.load();
            mMillSimulator->SetCollisionIssues(std::move(issues));
            if (mCommands) mCommands->setIssues(mMillSimulator->GetIssues());
            mCollisionCheckRunning = false;
            if (mGui) {
                mGui->setCollisionCheckState(
                    cancelled ? tr("Collision check cancelled — results are partial")
                              : tr("Collision check complete — review Commands for findings"),
                    cancelled ? 0 : 100,
                    false
                );
            }
            updateGui();
            update();
        }, Qt::QueuedConnection);
    });
}

void DlgCAMSimulator::cancelHolderCollisionCheck()
{
    if (!mCollisionCheckRunning) {
        return;
    }
    mCancelCollisionCheck.store(true);
    if (mGui) mGui->setCollisionCheckState(tr("Cancelling collision check…"), 0, true);
}

void DlgCAMSimulator::stopHolderCollisionCheck()
{
    ++mCollisionGeneration;
    mCancelCollisionCheck.store(true);
    if (mCollisionThread.joinable()) {
        mCollisionThread.join();
    }
    mCollisionCheckRunning = false;
}

static QString formatReportDuration(double seconds)
{
    const auto tenths = static_cast<qint64>(std::llround(std::max(0.0, seconds) * 10.0));
    return QStringLiteral("%1:%2:%3.%4")
        .arg(tenths / 36000, 2, 10, QLatin1Char('0'))
        .arg((tenths / 600) % 60, 2, 10, QLatin1Char('0'))
        .arg((tenths / 10) % 60, 2, 10, QLatin1Char('0'))
        .arg(tenths % 10);
}

void DlgCAMSimulator::exportSimulationReport()
{
    if (mNeedsClear || mNeedsInitialize || !mMillSimulator) {
        QMessageBox::information(this, tr("Simulation report"),
                                 tr("Start the simulation and wait for initialization before exporting a report."));
        return;
    }

    const QString fileName = QFileDialog::getSaveFileName(
        this, tr("Export CAM simulation report"), QStringLiteral("CAM-Simulation-Report.html"),
        tr("HTML report (*.html)")
    );
    if (fileName.isEmpty()) return;

    const auto& commandTimes = mMillSimulator->GetCommandTimeBreakdown();
    std::vector<OperationTimeBreakdown> operationTimes;
    std::unordered_map<int, std::size_t> operationRows;
    std::unordered_map<int, std::set<std::string>> operationTools;
    QCryptographicHash inputHash(QCryptographicHash::Sha256);
    const auto commandCount = std::min(mGCode.size(), commandTimes.size());
    for (std::size_t i = 0; i < commandCount; ++i) {
        const auto& source = mGCode[i];
        inputHash.addData(QByteArray::fromStdString(source.code));
        inputHash.addData(QByteArrayLiteral("\n"));
        inputHash.addData(QByteArray::fromStdString(source.tool));
        inputHash.addData(QByteArrayLiteral("\n"));
        inputHash.addData(QByteArray::number(source.operationId));
        inputHash.addData(QByteArray::fromStdString(source.operation));
        inputHash.addData(QByteArray::number(source.horizontalRapid, 'g', 17));
        inputHash.addData(QByteArray::number(source.verticalRapid, 'g', 17));
        auto [row, inserted] = operationRows.emplace(source.operationId, operationTimes.size());
        if (inserted) {
            OperationTimeBreakdown operation;
            operation.operationId = source.operationId;
            operation.name = source.operation.empty()
                ? "Operation " + std::to_string(operationTimes.size() + 1) : source.operation;
            operationTimes.push_back(std::move(operation));
        }
        auto& target = operationTimes[row->second].time;
        if (!source.tool.empty()) operationTools[source.operationId].insert(source.tool);
        const auto& sourceTime = commandTimes[i];
        target.cuttingSeconds += sourceTime.cuttingSeconds;
        target.rapidSeconds += sourceTime.rapidSeconds;
        target.dwellSeconds += sourceTime.dwellSeconds;
        target.assumedRateSeconds += sourceTime.assumedRateSeconds;
        target.omittedTime |= sourceTime.omittedTime;
    }

    QByteArray screenshot;
    QBuffer screenshotBuffer(&screenshot);
    screenshotBuffer.open(QIODevice::WriteOnly);
    grabFramebuffer().save(&screenshotBuffer, "PNG");
    screenshotBuffer.close();

    Gui::Document* guiDocument = Gui::Application::Instance
        ? Gui::Application::Instance->activeDocument() : nullptr;
    App::Document* document = guiDocument ? guiDocument->getDocument() : nullptr;
    const QString documentName = document ? QString::fromUtf8(document->Label.getStrValue()) : tr("Unknown");
    const QString documentPath = document ? QString::fromUtf8(document->getFileName()) : tr("Unavailable");
    const QString modified = guiDocument && guiDocument->isModified() ? tr("Yes (unsaved changes)") : tr("No");
    const auto state = mMillSimulator->GetState();

    QString html;
    QTextStream out(&html);
    out << "<!doctype html><html><head><meta charset=\"utf-8\"><title>CAM Simulation Report</title>"
           "<style>body{font:14px sans-serif;max-width:1100px;margin:2em auto;padding:0 1em;color:#222}"
           "table{border-collapse:collapse;width:100%;margin:1em 0}th,td{border:1px solid #bbb;padding:.4em;text-align:left}"
           "th{background:#eee}img{max-width:100%;border:1px solid #bbb}code{overflow-wrap:anywhere}</style>"
           "</head><body><h1>CAM Simulation Report</h1>";
    out << "<p>Generated " << QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toHtmlEscaped() << " UTC</p>";
    out << "<h2>Job identity</h2><table><tr><th>Document</th><td>" << documentName.toHtmlEscaped()
        << "</td></tr><tr><th>File</th><td>" << documentPath.toHtmlEscaped()
        << "</td></tr><tr><th>Unsaved changes</th><td>" << modified.toHtmlEscaped()
        << "</td></tr><tr><th>Internal CAM input SHA-256</th><td><code>"
        << QString::fromLatin1(inputHash.result().toHex()).toHtmlEscaped()
        << "</code></td></tr><tr><th>Commands simulated</th><td>" << commandCount
        << "</td></tr><tr><th>Operations</th><td>" << operationTimes.size() << "</td></tr></table>";

    out << "<h2>Modeled time by operation</h2><table><thead><tr><th>Operation</th><th>Tool(s)</th><th>Cutting</th>"
           "<th>Rapid</th><th>Dwell</th><th>Assumed-rate subset</th><th>Total</th></tr></thead><tbody>";
    CommandTimeBreakdown total;
    for (const auto& operation : operationTimes) {
        const auto& time = operation.time;
        const double operationTotal = time.cuttingSeconds + time.rapidSeconds + time.dwellSeconds;
        QStringList tools;
        for (const auto& tool : operationTools[operation.operationId]) {
            tools << QString::fromStdString(tool);
        }
        out << "<tr><td>" << QString::fromStdString(operation.name).toHtmlEscaped()
            << (time.omittedTime ? QStringLiteral(" *") : QString()) << "</td><td>"
            << tools.join(QStringLiteral(", ")).toHtmlEscaped() << "</td><td>"
            << formatReportDuration(time.cuttingSeconds) << "</td><td>"
            << formatReportDuration(time.rapidSeconds) << "</td><td>"
            << formatReportDuration(time.dwellSeconds) << "</td><td>"
            << formatReportDuration(time.assumedRateSeconds) << "</td><td>"
            << formatReportDuration(operationTotal) << "</td></tr>";
        total.cuttingSeconds += time.cuttingSeconds;
        total.rapidSeconds += time.rapidSeconds;
        total.dwellSeconds += time.dwellSeconds;
        total.assumedRateSeconds += time.assumedRateSeconds;
        total.omittedTime |= time.omittedTime;
    }
    out << "<tr><th>Selected operations</th><th></th><th>" << formatReportDuration(total.cuttingSeconds)
        << "</th><th>" << formatReportDuration(total.rapidSeconds) << "</th><th>"
        << formatReportDuration(total.dwellSeconds) << "</th><th>"
        << formatReportDuration(total.assumedRateSeconds) << "</th><th>"
        << formatReportDuration(total.cuttingSeconds + total.rapidSeconds + total.dwellSeconds)
        << "</th></tr></tbody></table><p>* Some movement or delay in this operation was omitted.</p>";

    out << "<h2>Simulation state</h2><p>Elapsed " << formatReportDuration(state.mElapsedSeconds)
        << " of " << formatReportDuration(mMillSimulator->GetTotalDuration())
        << (mMillSimulator->HasEstimatedRates() ? "; at least one movement used an assumed rate." : ".")
        << "</p><h2>Findings</h2>";
    const auto& issues = mMillSimulator->GetIssues();
    if (issues.empty()) {
        out << "<p>No findings were recorded.</p>";
    }
    else {
        out << "<table><thead><tr><th>Severity</th><th>Command</th><th>Operation</th><th>Finding</th></tr></thead><tbody>";
        for (const auto& issue : issues) {
            const QString severity = issue.severity == IssueSeverity::Error ? tr("Error") : tr("Warning");
            const QString operation = issue.command >= 0 && issue.command < static_cast<int>(mGCode.size())
                ? QString::fromStdString(mGCode[issue.command].operation) : QString();
            out << "<tr><td>" << severity.toHtmlEscaped() << "</td><td>"
                << (issue.command >= 0 ? QString::number(issue.command + 1) : QStringLiteral("—"))
                << "</td><td>" << operation.toHtmlEscaped() << "</td><td>"
                << QString::fromStdString(issue.message).toHtmlEscaped() << "</td></tr>";
        }
        out << "</tbody></table>";
    }
    out << "<h2>Assumptions and scope</h2><ul>"
           "<li>Simulation uses internal CAM commands, not posted NC verification.</li>"
           "<li>Motion time assumes constant programmed feed or configured rapid rates; acceleration and deceleration are not modeled.</li>"
           "<li>Assumed-rate time is included in cutting or rapid time and in the total.</li>"
           "<li>Tool-change and spindle delays are not included.</li>"
           "<li>Cycle clearance and unsupported behavior follow the reported findings and simulator assumptions.</li>"
           "<li>Collision findings are advisory and cover only the geometry and motion classes stated in the collision coverage finding.</li>"
           "</ul><h2>Simulator view</h2>";
    if (!screenshot.isEmpty()) {
        out << "<img alt=\"Simulator screenshot\" src=\"data:image/png;base64,"
            << QString::fromLatin1(screenshot.toBase64()) << "\">";
    }
    out << "</body></html>";

    QSaveFile report(fileName);
    const QByteArray reportData = html.toUtf8();
    if (!report.open(QIODevice::WriteOnly | QIODevice::Text)
        || report.write(reportData) != reportData.size() || !report.commit()) {
        QMessageBox::warning(this, tr("Simulation report"),
                             tr("Could not write the report file: %1").arg(report.errorString()));
    }
}

void DlgCAMSimulator::createVoxelStockPreview()
{
    createVoxelStockPreviewThroughCommand(-1);
}

void DlgCAMSimulator::createIntermediateVoxelStockPreview()
{
    if (!mMillSimulator) return;
    const int command = mMillSimulator->GetCurrentCommand();
    if (command < 0) {
        QMessageBox::information(this, tr("Stock snapshot"),
            tr("Select or play a simulation command first, then create the snapshot."));
        return;
    }
    createVoxelStockPreviewThroughCommand(command);
}

void DlgCAMSimulator::createVoxelStockPreviewThroughCommand(int throughCommand)
{
    Base::Console().log(
        "[CAMVoxelPreview] Button clicked (needsClear={}, needsInitialize={}, running={}, simulator={}, guiDocumentPresent={}).\n",
        mNeedsClear,
        mNeedsInitialize,
        mVoxelStockRunning,
        static_cast<bool>(mMillSimulator),
        mGuiDocument != nullptr
    );
    if (mNeedsClear || mNeedsInitialize || mVoxelStockRunning || !mMillSimulator) {
        Base::Console().warning(
            "[CAMVoxelPreview] Aborted before reconstruction because the simulator is not ready.\n"
        );
        QMessageBox::information(this, tr("Voxel stock preview"),
            tr("Wait for the simulation to finish initializing before creating a stock mesh preview."));
        return;
    }
    if (mVoxelStockThread.joinable()) {
        mVoxelStockThread.join();
    }
    mCancelVoxelStock.store(false);
    mVoxelStockRunning = true;
    const auto generation = ++mVoxelStockGeneration;
    const bool finalSnapshot = throughCommand < 0;
    const QString snapshotTitle = finalSnapshot
        ? tr("Create final stock mesh preview")
        : tr("Create stock snapshot at command %1").arg(throughCommand + 1);
    const QString successTitle = finalSnapshot ? tr("Voxel stock preview") : tr("Stock snapshot");
    mVoxelStockProgress = new QProgressDialog(
        finalSnapshot ? tr("Reconstructing approximate final voxel stock…")
                      : tr("Reconstructing approximate stock through command %1…").arg(throughCommand + 1),
        tr("Cancel"), 0, 100, this
    );
    mVoxelStockProgress->setWindowTitle(snapshotTitle);
    mVoxelStockProgress->setWindowModality(Qt::ApplicationModal);
    mVoxelStockProgress->setMinimumDuration(0);
    mVoxelStockProgress->setAutoClose(false);
    mVoxelStockProgress->setAutoReset(false);
    connect(mVoxelStockProgress, &QProgressDialog::canceled, this, [this] {
        mCancelVoxelStock.store(true);
        if (mVoxelStockProgress) mVoxelStockProgress->setLabelText(tr("Cancelling stock reconstruction…"));
    });
    mVoxelStockProgress->show();

    // Use the same minimum spatial resolution as the cutting simulator, while
    // VoxelStock applies an upper grid-size bound for larger stock objects.
    const double requestedCellSize = std::max(0.25f, MillPathSegment::SamplingResolution());
    Base::Console().log(
        "[CAMVoxelPreview] Starting reconstruction (requested cell size {:.3f} mm, generation {}).\n",
        requestedCellSize,
        static_cast<unsigned long long>(generation)
    );
    mVoxelStockThread = std::thread([this, generation, requestedCellSize, throughCommand, finalSnapshot, successTitle] {
        auto mesh = std::make_shared<Mesh::MeshObject>();
        double actualCellSize = 0;
        const bool success = mMillSimulator->BuildFinalVoxelMesh(
            *mesh,
            requestedCellSize,
            actualCellSize,
            &mCancelVoxelStock,
            [this, generation](int value) {
                QMetaObject::invokeMethod(this, [this, generation, value] {
                    if (generation == mVoxelStockGeneration && mVoxelStockProgress) {
                        mVoxelStockProgress->setValue(value);
                    }
                }, Qt::QueuedConnection);
            },
            throughCommand
        );
        const auto facetCount = mesh->countFacets();
        const bool queued = QMetaObject::invokeMethod(this, [this, generation, mesh = std::move(mesh), actualCellSize, success, successTitle, finalSnapshot, throughCommand] {
            Base::Console().log(
                "[CAMVoxelPreview] GUI completion callback entered (generation {}, current {}, success={}, facets={}, cancelRequested={}).\n",
                static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(mVoxelStockGeneration),
                success,
                static_cast<unsigned long>(mesh ? mesh->countFacets() : 0),
                mCancelVoxelStock.load()
            );
            if (generation != mVoxelStockGeneration) {
                Base::Console().warning("[CAMVoxelPreview] Ignored stale completion callback.\n");
                return;
            }
            mVoxelStockRunning = false;
            if (mVoxelStockProgress) {
                mVoxelStockProgress->close();
                mVoxelStockProgress->deleteLater();
                mVoxelStockProgress = nullptr;
            }
            if (mCancelVoxelStock.load() && !success) {
                Base::Console().warning("[CAMVoxelPreview] Reconstruction was cancelled.\n");
                return;
            }
            if (mCancelVoxelStock.load() && success) {
                Base::Console().warning(
                    "[CAMVoxelPreview] Cancellation arrived after reconstruction completed; keeping the completed mesh.\n"
                );
            }
            Base::Console().warning(
                "[CAMVoxelPreview] Stage: validating result (success={}, mesh={}, facets={}).\n",
                success,
                static_cast<bool>(mesh),
                static_cast<unsigned long>(mesh ? mesh->countFacets() : 0)
            );
            if (!success || !mesh || mesh->countFacets() == 0) {
                Base::Console().error(
                    "[CAMVoxelPreview] Reconstruction failed (success={}, mesh={}, facets={}).\n",
                    success,
                    static_cast<bool>(mesh),
                    mesh ? static_cast<unsigned long>(mesh->countFacets()) : 0UL
                );
                QMessageBox::warning(this, successTitle,
                    tr("The stock mesh could not be generated. The stock may be missing, there may be no completed cutting moves through this command, or the voxel mesh/work budget may have been exceeded."));
                return;
            }
            // The simulator is an MDI view tied to the CAM job's document. Use
            // that document explicitly; the active GUI document may differ if
            // focus changed while the modal progress dialog was running.
            Gui::Document* guiDocument = mGuiDocument;
            Base::Console().warning(
                "[CAMVoxelPreview] Stage: resolving target Gui document (present={}).\n",
                guiDocument != nullptr
            );
            App::Document* document = guiDocument ? guiDocument->getDocument() : nullptr;
            if (!document) {
                Base::Console().error(
                    "[CAMVoxelPreview] No target document (guiDocumentPresent={}).\n",
                    guiDocument != nullptr
                );
                QMessageBox::warning(this, successTitle,
                    tr("No active FreeCAD document is available to receive the mesh."));
                return;
            }
            Base::Console().warning(
                "[CAMVoxelPreview] Stage: inserting into document '{}' (facets={}, cells={:.3f} mm).\n",
                document->getName(),
                static_cast<unsigned long>(mesh->countFacets()),
                actualCellSize
            );
            bool transactionOpen = false;
            try {
                Base::Console().warning("[CAMVoxelPreview] Stage: opening transaction.\n");
                document->openTransaction("Create CAM voxel stock preview");
                transactionOpen = true;
                Base::Console().warning("[CAMVoxelPreview] Stage: adding Mesh::Feature.\n");
                auto* feature = document->addObject<Mesh::Feature>("CAMVoxelStockPreview");
                if (!feature) {
                    throw Base::Exception("Document::addObject returned null for Mesh::Feature");
                }
                Base::Console().warning(
                    "[CAMVoxelPreview] Stage: assigning mesh to '{}'.\n",
                    feature->getNameInDocument()
                );
                const QString label = finalSnapshot
                    ? QStringLiteral("Voxel stock preview (approx., %1 mm cells)").arg(actualCellSize, 0, 'f', 2)
                    : QStringLiteral("Voxel stock at command %1 (approx., %2 mm cells)")
                        .arg(throughCommand + 1).arg(actualCellSize, 0, 'f', 2);
                feature->Label.setValue(label.toStdString());
                feature->Mesh.setValue(*mesh);
                Base::Console().warning("[CAMVoxelPreview] Stage: recomputing and committing object.\n");
                document->recompute();
                document->commitTransaction();
                transactionOpen = false;
                Base::Console().warning(
                    "[CAMVoxelPreview] Stage: committed object '{}'; document now has {} objects.\n",
                    feature->getNameInDocument(),
                    static_cast<unsigned long>(document->countObjects())
                );
                if (auto* viewProvider = guiDocument->getViewProvider(feature)) {
                    viewProvider->setVisible(true);
                    Base::Console().warning("[CAMVoxelPreview] Stage: view provider set visible.\n");
                }
                guiDocument->setModified(true);
                Gui::Selection().clearSelection();
                const bool selected = Gui::Selection().addSelection(document->getName(), feature->getNameInDocument());
                Base::Console().warning(
                    "[CAMVoxelPreview] Stage: selection result={}, document='{}', object='{}'.\n",
                    selected,
                    document->getName(),
                    feature->getNameInDocument()
                );
                QMessageBox::information(
                    this,
                    successTitle,
                    tr("Created '%1' in document '%2' with %3 facets. The object is selected in the Model tree.")
                        .arg(QString::fromUtf8(feature->Label.getValue()),
                             QString::fromUtf8(document->getName()),
                             QString::number(mesh->countFacets()))
                );
            }
            catch (const Base::Exception& error) {
                if (transactionOpen) document->abortTransaction();
                Base::Console().error("[CAMVoxelPreview] FreeCAD exception during insertion: {}\n", error.what());
                QMessageBox::critical(this, successTitle, QString::fromUtf8(error.what()));
            }
            catch (const std::exception& error) {
                if (transactionOpen) document->abortTransaction();
                Base::Console().error("[CAMVoxelPreview] Standard exception during insertion: {}\n", error.what());
                QMessageBox::critical(this, successTitle, QString::fromUtf8(error.what()));
            }
            catch (...) {
                if (transactionOpen) document->abortTransaction();
                Base::Console().error("[CAMVoxelPreview] Unknown exception during insertion.\n");
                QMessageBox::critical(this, successTitle, tr("Unknown error while adding the mesh to the document."));
            }
        }, Qt::QueuedConnection);
        if (!queued) {
            Base::Console().error(
                "[CAMVoxelPreview] Could not queue the GUI completion callback (success={}, facets={}).\n",
                success,
                static_cast<unsigned long>(facetCount)
            );
        }
    });
}

void DlgCAMSimulator::createStockComparison()
{
    if (mNeedsClear || mNeedsInitialize || mVoxelStockRunning || !mMillSimulator) {
        QMessageBox::information(this, tr("Stock comparison"),
            tr("Wait for the simulation to finish initializing before comparing stock to the target."));
        return;
    }
    if (mTargetShape.isNull()) {
        QMessageBox::information(this, tr("Stock comparison"),
            tr("This CAM Job has no target model shape to compare against."));
        return;
    }
    if (mVoxelStockThread.joinable()) mVoxelStockThread.join();

    const TopoDS_Shape targetShape = mTargetShape.getShape();
    const double requestedCellSize = std::max(0.25f, MillPathSegment::SamplingResolution());
    mCancelVoxelStock.store(false);
    mVoxelStockRunning = true;
    const auto generation = ++mVoxelStockGeneration;
    mVoxelStockProgress = new QProgressDialog(
        tr("Comparing approximate voxel stock to the target…"), tr("Cancel"), 0, 100, this
    );
    mVoxelStockProgress->setWindowTitle(tr("Stock comparison"));
    mVoxelStockProgress->setWindowModality(Qt::ApplicationModal);
    mVoxelStockProgress->setMinimumDuration(0);
    mVoxelStockProgress->setAutoClose(false);
    mVoxelStockProgress->setAutoReset(false);
    connect(mVoxelStockProgress, &QProgressDialog::canceled, this, [this] {
        mCancelVoxelStock.store(true);
        if (mVoxelStockProgress) mVoxelStockProgress->setLabelText(tr("Cancelling stock comparison…"));
    });
    mVoxelStockProgress->show();

    Base::Console().log(
        "[CAMStockCompare] Starting target comparison at requested {:.3f} mm cell pitch.\n",
        requestedCellSize
    );
    mVoxelStockThread = std::thread([this, generation, requestedCellSize, targetShape] {
        auto excessMesh = std::make_shared<Mesh::MeshObject>();
        auto missingMesh = std::make_shared<Mesh::MeshObject>();
        double actualCellSize = 0;
        double excessVolume = 0;
        double missingVolume = 0;
        bool targetWithinStockBounds = false;
        const bool success = mMillSimulator->BuildFinalVoxelComparison(
            targetShape,
            *excessMesh,
            *missingMesh,
            requestedCellSize,
            actualCellSize,
            excessVolume,
            missingVolume,
            targetWithinStockBounds,
            &mCancelVoxelStock,
            [this, generation](int value) {
                QMetaObject::invokeMethod(this, [this, generation, value] {
                    if (generation == mVoxelStockGeneration && mVoxelStockProgress) {
                        mVoxelStockProgress->setValue(value);
                    }
                }, Qt::QueuedConnection);
            }
        );
        QMetaObject::invokeMethod(this,
            [this, generation, excessMesh = std::move(excessMesh), missingMesh = std::move(missingMesh),
             actualCellSize, excessVolume, missingVolume, targetWithinStockBounds, success] {
                Base::Console().log(
                    "[CAMStockCompare] GUI completion: generation {}, current {}, success={}, excess={}, missing={}, targetWithinStockBounds={}, cancelled={}.\n",
                    generation,
                    mVoxelStockGeneration,
                    success,
                    excessMesh ? excessMesh->countFacets() : 0,
                    missingMesh ? missingMesh->countFacets() : 0,
                    targetWithinStockBounds,
                    mCancelVoxelStock.load()
                );
                if (generation != mVoxelStockGeneration) return;
                mVoxelStockRunning = false;
                if (mVoxelStockProgress) {
                    mVoxelStockProgress->close();
                    mVoxelStockProgress->deleteLater();
                    mVoxelStockProgress = nullptr;
                }
                if (!success) {
                    Base::Console().warning("[CAMStockCompare] Comparison failed or was cancelled before completion.\n");
                    if (!mCancelVoxelStock.load()) {
                        QMessageBox::warning(this, tr("Stock comparison"),
                            tr("Could not compare the stock and target. Check that both are valid solids and that the path simulation completed."));
                    }
                    return;
                }
                if (mTargetShape.isNull()) {
                    QMessageBox::warning(this, tr("Stock comparison"), tr("The target model is no longer available."));
                    return;
                }
                Gui::Document* guiDocument = mGuiDocument;
                App::Document* document = guiDocument ? guiDocument->getDocument() : nullptr;
                if (!document) {
                    QMessageBox::warning(this, tr("Stock comparison"),
                        tr("No target document is available to receive comparison meshes."));
                    return;
                }

                const auto addDeviation = [&](Mesh::MeshObject& mesh, const char* internalName,
                                              const QString& label, const Base::Color& color) {
                    if (!mesh.countFacets()) return static_cast<Mesh::Feature*>(nullptr);
                    auto* feature = document->addObject<Mesh::Feature>(internalName);
                    feature->Label.setValue(label.toStdString());
                    feature->Mesh.setValue(mesh);
                    if (auto* viewProvider = guiDocument->getViewProvider(feature)) {
                        viewProvider->setVisible(true);
                        if (auto* geometryView = dynamic_cast<Gui::ViewProviderGeometryObject*>(viewProvider)) {
                            geometryView->ShapeAppearance.setDiffuseColor(color);
                            geometryView->Transparency.setValue(20);
                        }
                    }
                    return feature;
                };

                document->openTransaction("Compare CAM final stock to target");
                try {
                    const auto pitch = QString::number(actualCellSize, 'f', 2);
                    auto* excess = addDeviation(
                        *excessMesh,
                        "CAMExcessStock",
                        tr("Excess stock (approx., %1 mm cells)").arg(pitch),
                        Base::Color(1.0F, 0.55F, 0.05F)
                    );
                    auto* missing = addDeviation(
                        *missingMesh,
                        "CAMMissingTargetMaterial",
                        tr("Missing target material (approx., %1 mm cells)").arg(pitch),
                        Base::Color(0.95F, 0.08F, 0.08F)
                    );
                    document->recompute();
                    document->commitTransaction();
                    guiDocument->setModified(true);
                    Gui::Selection().clearSelection();
                    bool selected = false;
                    if (excess) selected |= Gui::Selection().addSelection(document->getName(), excess->getNameInDocument());
                    if (missing) selected |= Gui::Selection().addSelection(document->getName(), missing->getNameInDocument());
                    Base::Console().log(
                        "[CAMStockCompare] Added deviation objects in document '{}'; excess {:.3f} mm^3, missing {:.3f} mm^3, targetWithinStockBounds={}, selected={}.\n",
                        document->getName(), excessVolume, missingVolume, targetWithinStockBounds, selected
                    );

                    const QString details = tr("Approximate excess stock: %1 mm³\nApproximate missing target material: %2 mm³\nVoxel pitch: %3 mm")
                        .arg(excessVolume, 0, 'f', 2)
                        .arg(missingVolume, 0, 'f', 2)
                        .arg(actualCellSize, 0, 'f', 2);
                    const bool hasDeviationMeshes = excess || missing;
                    const QString outcome = hasDeviationMeshes
                        ? tr("Deviation meshes were added to the document (amber = excess, red = missing target material).")
                        : tr("No deviation cells were detected at this voxel resolution.");
                    QMessageBox::information(
                        this,
                        tr("Stock comparison"),
                        targetWithinStockBounds
                            ? details + QStringLiteral("\n") + outcome
                            : details + tr("\nThe target extends outside the initial stock bounds; comparison coverage is partial.")
                    );
                }
                catch (const Base::Exception& error) {
                    document->abortTransaction();
                    Base::Console().error("[CAMStockCompare] FreeCAD exception while adding deviations: {}\n", error.what());
                    QMessageBox::critical(this, tr("Stock comparison"), QString::fromUtf8(error.what()));
                }
                catch (const std::exception& error) {
                    document->abortTransaction();
                    Base::Console().error("[CAMStockCompare] Exception while adding deviations: {}\n", error.what());
                    QMessageBox::critical(this, tr("Stock comparison"), QString::fromUtf8(error.what()));
                }
            }, Qt::QueuedConnection);
    });
}

void DlgCAMSimulator::stopVoxelStockPreview()
{
    ++mVoxelStockGeneration;
    mCancelVoxelStock.store(true);
    if (mVoxelStockThread.joinable()) {
        mVoxelStockThread.join();
    }
    mVoxelStockRunning = false;
    if (mVoxelStockProgress) {
        mVoxelStockProgress->close();
        mVoxelStockProgress->deleteLater();
        mVoxelStockProgress = nullptr;
    }
}

void DlgCAMSimulator::addGcodeCommand(const char* cmd, double horizontalRapid, double verticalRapid)
{
    mGCode.push_back({cmd, horizontalRapid, verticalRapid, mOperationId, mOperation, mToolLabel});
}

void DlgCAMSimulator::addTool(
    const std::vector<float>& toolProfilePoints,
    int toolNumber,
    float diameter,
    float resolution
)
{
    Q_UNUSED(resolution)

    std::string toolCmd = "T" + std::to_string(toolNumber);
    addGcodeCommand(toolCmd.c_str());
    mTools.emplace_back(toolProfilePoints, toolNumber, diameter);
}

static SimShape getMeshData(const Part::TopoShape& shape, float resolution)
{
    SimShape ret;

    std::vector<int> normalCount;
    int nVerts = 0;
    for (auto& shape : shape.getSubTopoShapes(TopAbs_FACE)) {
        std::vector<Base::Vector3d> points;
        std::vector<Data::ComplexGeoData::Facet> facets;
        shape.getFaces(points, facets, resolution);

        std::vector<Base::Vector3d> normals(points.size());
        std::vector<int> normalCount(points.size());

        // copy triangle indices and calculate normals
        for (auto face : facets) {
            ret.indices.push_back(face.I1 + nVerts);
            ret.indices.push_back(face.I2 + nVerts);
            ret.indices.push_back(face.I3 + nVerts);

            // calculate normal
            Base::Vector3d vAB = points[face.I2] - points[face.I1];
            Base::Vector3d vAC = points[face.I3] - points[face.I1];
            Base::Vector3d vNorm = vAB.Cross(vAC).Normalize();

            normals[face.I1] += vNorm;
            normals[face.I2] += vNorm;
            normals[face.I3] += vNorm;

            normalCount[face.I1]++;
            normalCount[face.I2]++;
            normalCount[face.I3]++;
        }

        // copy points and set normals
        for (unsigned int i = 0; i < points.size(); i++) {
            Base::Vector3d& point = points[i];
            Base::Vector3d& normal = normals[i];
            int count = normalCount[i];
            normal /= count;
            ret.verts.push_back(Vertex(point.x, point.y, point.z, normal.x, normal.y, normal.z));
        }

        nVerts = ret.verts.size();
    }

    ret.needsUpdate = true;
    return ret;
}

void DlgCAMSimulator::addHolderShape(
    const Part::TopoShape& shape,
    int toolNumber,
    float resolution
)
{
    if (shape.isNull()) {
        mHolderShapes.erase(toolNumber);
        return;
    }
    SimHolder holder;
    holder.renderShape = getMeshData(shape, resolution);
    holder.collisionShape = shape;
    mHolderShapes[toolNumber] = std::move(holder);
}

void DlgCAMSimulator::setStockShape(const Part::TopoShape& shape, float resolution)
{
    mStock = getMeshData(shape, resolution);
    mCollisionStockShape = shape;

    if (mDummyViewer) {
        mDummyViewer->setStockShape(shape);
    }

    update();
}

void DlgCAMSimulator::setStockVisible(bool b)
{
    if (b == mMillSimulator->IsStockVisible()) {
        return;
    }

    mMillSimulator->SetStockVisible(b);

    if (mDummyViewer) {
        mDummyViewer->setStockVisible(b);
    }

    update();
}

void DlgCAMSimulator::setBaseShape(const Part::TopoShape& shape, float resolution)
{
    mTargetShape = shape;
    mBase = getMeshData(shape, resolution);

    if (mDummyViewer) {
        mDummyViewer->setBaseShape(shape);
    }

    update();
}

void DlgCAMSimulator::setBaseVisible(bool b)
{
    if (b == mMillSimulator->IsBaseVisible()) {
        return;
    }

    mMillSimulator->SetBaseVisible(b);

    if (mDummyViewer) {
        mDummyViewer->setBaseVisible(b);
    }

    update();
}

// this is very similar to DemoMode::getDirection in Gui/DemoMode.cpp

static SbVec3f getRotationDirection(Gui::View3DInventorViewer* viewer)
{
    const SbVec3f viewAxis = {0, 0, -1};

    SoCamera* cam = viewer->getSoRenderManager()->getCamera();
    if (!cam) {
        return viewAxis;
    }
    SbRotation rot = cam->orientation.getValue();
    SbRotation inv = rot.inverse();
    SbVec3f vec(viewAxis);
    inv.multVec(vec, vec);
    if (vec.length() < std::numeric_limits<float>::epsilon()) {
        vec = viewAxis;
    }
    vec.normalize();
    return vec;
}

void DlgCAMSimulator::setRotateEnabled(bool b)
{
    if (b) {
        mDummyViewer->startSpinningAnimation(getRotationDirection(mDummyViewer), 0.5f);
    }
    else {
        mDummyViewer->stopAnimating();
    }
}

void DlgCAMSimulator::setBackgroundColor(const QColor& c)
{
    mMillSimulator->SetBackgroundColor({c.redF(), c.greenF(), c.blueF()});
    mBackground = c;
    if (mGui) {
        mGui->setBackgroundColor(c);
    }
    update();
}

void DlgCAMSimulator::setPathColor(const QColor& normal, const QColor& rapid)
{
    const vec3 vnormal = {normal.redF(), normal.greenF(), normal.blueF()};
    const vec3 vrapid = {rapid.redF(), rapid.greenF(), rapid.blueF()};
    mMillSimulator->SetPathColor(vnormal, vrapid);
}

void DlgCAMSimulator::timerEvent(QTimerEvent* event)
{
    (void)event;

    update();

    // TODO: keep things simple for now, should probably only update gui if something changed

    updateGui();
}

void DlgCAMSimulator::updateResources()
{
    // clear simulator

    if (mNeedsClear) {
        mMillSimulator->Clear();
        mLastGCode = 0;
        for (auto& [toolId, holder] : mHolderShapes) {
            Q_UNUSED(toolId)
            holder.renderShape.needsUpdate = true;
        }
        mNeedsClear = false;
    }

    if (!mCollisionStockShape.isNull()) {
        mMillSimulator->SetCollisionStock(mCollisionStockShape.getShape());
    }

    // update gcode

    for (int i = mLastGCode; i < (int)mGCode.size(); i++) {
        const auto& cmd = mGCode[i];
        mMillSimulator->AddGcodeLine(cmd.code.c_str(), cmd.horizontalRapid, cmd.verticalRapid);
    }

    mLastGCode = mGCode.size();

    // update tools

    for (const auto& tool : mTools) {
        if (!mMillSimulator->ToolExists(tool.id)) {
            mMillSimulator->AddTool(tool.profile, tool.id, tool.diameter);
        }
    }

    for (auto& [toolId, holder] : mHolderShapes) {
        if (holder.renderShape.needsUpdate) {
            mMillSimulator->SetHolderObject(
                toolId, holder.renderShape.verts, holder.renderShape.indices
            );
            mMillSimulator->SetHolderCollisionShape(
                toolId, holder.collisionShape.getShape()
            );
            holder.renderShape.needsUpdate = false;
        }
    }

    // initialize simulator

    if (mNeedsInitialize) {
        // TODO: mStock is set when we arrive here, still this could be handled nicer
        const float maxStockDimension = mStock.maxDimension();

        mMillSimulator->InitSimulation(mQuality, maxStockDimension);
        mLastPausedIssue = -1;
        if (mCommands) {
            mCommands->setIssues(mMillSimulator->GetIssues());
            std::vector<OperationTimeBreakdown> operationTimes;
            std::unordered_map<int, std::size_t> operationRows;
            const auto& commandTimes = mMillSimulator->GetCommandTimeBreakdown();
            const auto commandCount = std::min(mGCode.size(), commandTimes.size());
            for (std::size_t command = 0; command < commandCount; ++command) {
                const auto& source = mGCode[command];
                auto [row, inserted] = operationRows.emplace(source.operationId, operationTimes.size());
                if (inserted) {
                    OperationTimeBreakdown operation;
                    operation.operationId = source.operationId;
                    operation.name = source.operation;
                    if (operation.name.empty()) {
                        operation.name = "Operation " + std::to_string(operationTimes.size() + 1);
                    }
                    operationTimes.push_back(std::move(operation));
                }
                auto& target = operationTimes[row->second].time;
                const auto& sourceTime = commandTimes[command];
                target.cuttingSeconds += sourceTime.cuttingSeconds;
                target.rapidSeconds += sourceTime.rapidSeconds;
                target.dwellSeconds += sourceTime.dwellSeconds;
                target.assumedRateSeconds += sourceTime.assumedRateSeconds;
                target.omittedTime |= sourceTime.omittedTime;
            }
            mCommands->setTimeBreakdown(operationTimes);
        }
        mNeedsInitialize = false;

        mLastProcessSim = clock::time_point::min();
    }

    // update stock and base

    if (mStock.needsUpdate) {
        mMillSimulator->SetArbitraryStock(mStock.verts, mStock.indices);
        mStock.needsUpdate = false;
    }

    if (mBase.needsUpdate) {
        mMillSimulator->SetBaseObject(mBase.verts, mBase.indices);
        mBase.needsUpdate = false;
    }

    // update state

    if (mState) {
        mMillSimulator->SetState(*mState);
        mState = nullptr;
    }
}

void DlgCAMSimulator::updateWindowScale()
{
    const qreal ratio = devicePixelRatioF();
    mMillSimulator->UpdateWindowScale(width() * ratio, height() * ratio);
}

void DlgCAMSimulator::updateCamera()
{
    if (!mDummyViewer) {
        return;
    }

    const SoCamera& camera = *mDummyViewer->getCamera();
    mMillSimulator->UpdateCamera(camera);
}

void DlgCAMSimulator::initializeGL()
{
    gOpenGLFunctions.initializeOpenGLFunctions();
}

void DlgCAMSimulator::paintGL()
{
    updateResources();

    // We need to call updateWindowScale on every render since the devicePixelRatio we get in
    // resizeGL might be wrong on the first resize.

    updateWindowScale();
    updateCamera();

    const auto now = clock::now();
    const auto elapsed = mLastProcessSim != clock::time_point::min() ? now - mLastProcessSim : 0s;

#if 0

    static std::deque<clock::duration> q;
    while (q.size() >= 60) {
        q.pop_front();
    }

    q.push_back(elapsed);

    const auto average = std::accumulate(q.begin(), q.end(), clock::duration()) / (float)q.size();

    std::cerr
        << "elapsed: "
        << std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(average).count()
        << "ms" << std::endl;

#endif


    mMillSimulator->ProcessSim(elapsed);

    mLastProcessSim = now;
}

void DlgCAMSimulator::resizeGL(int w, int h)
{
    (void)w, (void)h;
}

}  // namespace CAMSimulator
