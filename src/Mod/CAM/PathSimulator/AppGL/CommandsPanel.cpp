// SPDX-License-Identifier: LGPL-2.1-or-later
#include "CommandsPanel.h"

#include <algorithm>

#include <QEvent>
#include <QCheckBox>
#include <QListWidget>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QToolButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QBrush>
#include <QFont>
#include <QStringList>
#include <cmath>

namespace CAMSimulator
{
CommandsPanel::CommandsPanel(QWidget* parent): QWidget(parent)
{
    setMinimumWidth(220);
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QHBoxLayout;
    heading->addWidget(new QLabel(tr("CAM Commands")));
    heading->addStretch();
    auto* close = new QToolButton;
    close->setText(tr("Hide"));
    heading->addWidget(close);
    layout->addLayout(heading);
    followButton = new QToolButton;
    followButton->setObjectName(QStringLiteral("followCommandButton"));
    followButton->setText(tr("Follow current command"));
    followButton->setCheckable(true);
    followButton->setChecked(true);
    layout->addWidget(followButton);
    auto* operationNavigation = new QHBoxLayout;
    auto* previousOperation = new QToolButton;
    previousOperation->setObjectName(QStringLiteral("previousOperation"));
    previousOperation->setText(tr("Previous operation"));
    auto* nextOperation = new QToolButton;
    nextOperation->setObjectName(QStringLiteral("nextOperation"));
    nextOperation->setText(tr("Next operation"));
    operationNavigation->addWidget(previousOperation);
    operationNavigation->addWidget(nextOperation);
    layout->addLayout(operationNavigation);
    connect(previousOperation, &QToolButton::clicked, this, [this] {
        if (operationStarts.empty()) return;
        auto next = std::lower_bound(operationStarts.begin(), operationStarts.end(), currentCommand);
        if (next == operationStarts.begin()) return;
        if (next == operationStarts.end() || *next >= currentCommand) --next;
        Q_EMIT commandSelected(*next);
    });
    connect(nextOperation, &QToolButton::clicked, this, [this] {
        if (operationStarts.empty()) return;
        auto next = std::upper_bound(operationStarts.begin(), operationStarts.end(), currentCommand);
        if (next != operationStarts.end()) Q_EMIT commandSelected(*next);
    });
    contextLabel = new QLabel;
    contextLabel->setWordWrap(true);
    contextLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(contextLabel);
    timeBreakdownToggle = new QToolButton;
    timeBreakdownToggle->setObjectName(QStringLiteral("timeBreakdownToggle"));
    timeBreakdownToggle->setText(tr("Time breakdown"));
    timeBreakdownToggle->setCheckable(true);
    timeBreakdownToggle->setChecked(false);
    layout->addWidget(timeBreakdownToggle);
    timeBreakdownTree = new QTreeWidget;
    timeBreakdownTree->setObjectName(QStringLiteral("timeBreakdownTree"));
    timeBreakdownTree->setColumnCount(6);
    timeBreakdownTree->setHeaderLabels({tr("Operation"), tr("Cut"), tr("Rapid"), tr("Dwell"),
                                        tr("Assumed"), tr("Total")});
    timeBreakdownTree->setRootIsDecorated(false);
    timeBreakdownTree->setAlternatingRowColors(true);
    timeBreakdownTree->setMaximumHeight(165);
    timeBreakdownTree->setToolTip(tr("Assumed-rate time is a subset of cutting/rapid time and is included in Total."));
    timeBreakdownTree->hide();
    layout->addWidget(timeBreakdownTree);
    auto* timeBreakdownNote = new QLabel(
        tr("Modeled motion and dwell only. Tool changes, spindle delays, and unsupported movement are omitted.")
    );
    timeBreakdownNote->setObjectName(QStringLiteral("timeBreakdownNote"));
    timeBreakdownNote->setWordWrap(true);
    timeBreakdownNote->setVisible(false);
    layout->addWidget(timeBreakdownNote);
    connect(timeBreakdownToggle, &QToolButton::toggled, this, [this, timeBreakdownNote](bool visible) {
        timeBreakdownTree->setVisible(visible);
        timeBreakdownNote->setVisible(visible);
    });
    editor = new QPlainTextEdit;
    editor->setObjectName(QStringLiteral("commandsEditor"));
    editor->setReadOnly(true);
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setToolTip(tr("Internal CAM commands, before postprocessing. Feed rates are shown in mm/min. "
                          "Select a command to pause and seek to its start. Unsupported commands remain "
                          "visible but are not necessarily simulated."));
    layout->addWidget(editor, 1);

    issueCount = new QLabel(tr("Issues: not checked yet"));
    layout->addWidget(issueCount);
    pauseIssues = new QCheckBox(tr("Pause on issues"));
    pauseIssues->setObjectName(QStringLiteral("pauseOnIssues"));
    pauseIssues->setChecked(true);
    pauseIssues->setToolTip(tr("Pause at each affected command. Issues are still listed when this is off. "
                              "Rewinding makes those issues eligible to pause again."));
    layout->addWidget(pauseIssues);
    pauseToolChanges = new QCheckBox(tr("Pause at tool changes"));
    pauseToolChanges->setObjectName(QStringLiteral("pauseOnToolChanges"));
    pauseToolChanges->setChecked(true);
    pauseToolChanges->setToolTip(tr("Pause playback at each M6 tool-change command. Press Play to continue."));
    layout->addWidget(pauseToolChanges);
    issueList = new QListWidget;
    issueList->setObjectName(QStringLiteral("simulationIssues"));
    issueList->setMaximumHeight(130);
    layout->addWidget(issueList);
    issueDetail = new QLabel;
    issueDetail->setTextFormat(Qt::PlainText);
    issueDetail->setWordWrap(true);
    issueDetail->setObjectName(QStringLiteral("issueDetail"));
    layout->addWidget(issueDetail);
    auto* navigation = new QHBoxLayout;
    auto* previous = new QToolButton;
    previous->setText(tr("Previous issue"));
    previous->setObjectName(QStringLiteral("previousIssue"));
    auto* next = new QToolButton;
    next->setText(tr("Next issue"));
    next->setObjectName(QStringLiteral("nextIssue"));
    navigation->addWidget(previous);
    navigation->addWidget(next);
    navigation->addStretch();
    layout->addLayout(navigation);
    continueButton = new QToolButton;
    continueButton->setText(tr("Continue"));
    continueButton->setObjectName(QStringLiteral("continueIssue"));
    continueButton->setEnabled(false);
    continueAllButton = new QToolButton;
    continueAllButton->setText(tr("Continue without pausing"));
    continueAllButton->setObjectName(QStringLiteral("continueAllIssues"));
    continueAllButton->setEnabled(false);
    auto* resume = new QHBoxLayout;
    resume->addWidget(continueButton);
    resume->addWidget(continueAllButton);
    resume->addStretch();
    layout->addLayout(resume);
    connect(pauseIssues, &QCheckBox::toggled, this, &CommandsPanel::pauseOnIssuesChanged);
    connect(pauseToolChanges, &QCheckBox::toggled, this, &CommandsPanel::pauseOnToolChangesChanged);
    auto* exportReport = new QToolButton;
    exportReport->setObjectName(QStringLiteral("exportSimulationReport"));
    exportReport->setText(tr("Export simulation report…"));
    exportReport->setToolTip(tr("Save timing, issues, assumptions, and a simulator screenshot as an HTML report."));
    layout->addWidget(exportReport);
    connect(exportReport, &QToolButton::clicked, this, &CommandsPanel::reportRequested);
    auto* voxelPreview = new QToolButton;
    voxelPreview->setObjectName(QStringLiteral("createVoxelStockPreview"));
    voxelPreview->setText(tr("Create final stock mesh preview…"));
    voxelPreview->setToolTip(tr("Build an approximate voxel mesh from the selected simulation and add it to the document."));
    layout->addWidget(voxelPreview);
    connect(voxelPreview, &QToolButton::clicked, this, &CommandsPanel::voxelStockPreviewRequested);
    auto* intermediatePreview = new QToolButton;
    intermediatePreview->setObjectName(QStringLiteral("createIntermediateVoxelStockPreview"));
    intermediatePreview->setText(tr("Create stock snapshot at current command…"));
    intermediatePreview->setToolTip(tr("Reconstruct stock after all cutting moves through the currently selected command."));
    layout->addWidget(intermediatePreview);
    connect(intermediatePreview, &QToolButton::clicked, this, &CommandsPanel::intermediateStockPreviewRequested);
    auto* compareStock = new QToolButton;
    compareStock->setObjectName(QStringLiteral("compareStockToTarget"));
    compareStock->setText(tr("Compare final stock to target…"));
    compareStock->setToolTip(tr("Highlight approximate leftover stock and target material that was cut away."));
    layout->addWidget(compareStock);
    connect(compareStock, &QToolButton::clicked, this, &CommandsPanel::stockComparisonRequested);
    connect(continueButton, &QToolButton::clicked, this, [this] { Q_EMIT continueRequested(false); });
    connect(continueAllButton, &QToolButton::clicked, this, [this] { Q_EMIT continueRequested(true); });
    connect(issueList, &QListWidget::currentRowChanged, this, [this](int row) { showIssue(row, true); });
    // Clicking an already-selected finding should seek again too.
    connect(issueList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        showIssue(issueList->row(item), true);
    });
    connect(previous, &QToolButton::clicked, this, [this] {
        if (issues.empty()) return;
        const int row = issueList->currentRow() <= 0 ? 0 : issueList->currentRow() - 1;
        showIssue(row, true);
    });
    connect(next, &QToolButton::clicked, this, [this] {
        if (issues.empty()) return;
        const int row = std::min(issueList->currentRow() + 1, static_cast<int>(issues.size()) - 1);
        showIssue(row, true);
    });
    editor->viewport()->installEventFilter(this);
    connect(close, &QToolButton::clicked, this, &CommandsPanel::closeRequested);
    connect(followButton, &QToolButton::toggled, this, [this](bool follow) {
        if (follow) revealCurrent();
    });
    connect(editor->verticalScrollBar(), &QScrollBar::actionTriggered,
            this, [this] { setFollowing(false); });
    connect(editor->verticalScrollBar(), &QScrollBar::sliderMoved,
            this, [this] { setFollowing(false); });
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!mouseSelecting) selectCurrent();
    });
}

void CommandsPanel::selectCurrent()
{
    const int block = editor->textCursor().blockNumber();
    if (block >= 0 && block < static_cast<int>(blockCommands.size()) && blockCommands[block] >= 0) {
        setFollowing(false);
        Q_EMIT commandSelected(blockCommands[block]);
    }
}

bool CommandsPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == editor->viewport() && event->type() == QEvent::Wheel) {
        setFollowing(false);
    }
    if (watched == editor->viewport() && event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        mouseSelecting = true;
    }
    if (watched == editor->viewport() && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        mouseSelecting = false;
        selectCurrent();
    }
    return QWidget::eventFilter(watched, event);
}

void CommandsPanel::setCommands(const std::vector<SimulationCommand>& commands)
{
    const QSignalBlocker blocker(editor);
    sourceCommands = commands;
    setIssues({});
    setTimeBreakdown({});
    issueCount->setText(tr("Issues: not checked yet"));
    commandContexts.clear();
    contextLabel->clear();
    commandBlocks.clear();
    operationStarts.clear();
    blockCommands.clear();
    currentCommand = -1;
    QStringList lines;
    int previousOperation = -1;
    int operationNumber = 0;
    int operationLine = 0;
    int operationCount = 0;
    for (const auto& command : commands) {
        if (command.operationId != previousOperation) {
            ++operationCount;
            previousOperation = command.operationId;
        }
    }
    previousOperation = -1;
    for (int i = 0; i < static_cast<int>(commands.size()); ++i) {
        const auto& command = commands[i];
        if (command.operationId != previousOperation) {
            previousOperation = command.operationId;
            operationStarts.push_back(i);
            ++operationNumber;
            operationLine = 0;
            lines << tr("Operation: %1").arg(QString::fromStdString(command.operation))
                  << QString::fromStdString(command.tool);
            blockCommands.insert(blockCommands.end(), 2, -1);
        }
        commandContexts.push_back(tr("Operation %1/%2: %3\n%4\nCommand %5 (line %6)")
            .arg(operationNumber).arg(operationCount)
            .arg(command.operation.empty() ? tr("Unknown") : QString::fromStdString(command.operation))
            .arg(command.tool.empty() ? tr("Tool unknown") : QString::fromStdString(command.tool))
            .arg(++operationLine).arg(i + 1));
        commandBlocks.push_back(lines.size());
        // One block per command even if a comment contains embedded line breaks.
        QString code = QString::fromStdString(command.code);
        code.replace('\n', ' ');
        code.replace('\r', ' ');
        lines << QStringLiteral("%1  %2").arg(i + 1, 5).arg(code.trimmed());
        blockCommands.push_back(i);
    }
    editor->setPlainText(lines.join('\n'));
    editor->setExtraSelections({});
}

static QString formatBreakdownTime(double seconds)
{
    const auto tenths = static_cast<qint64>(std::llround(std::max(0.0, seconds) * 10.0));
    const auto hours = tenths / 36000;
    const auto minutes = (tenths / 600) % 60;
    const auto wholeSeconds = (tenths / 10) % 60;
    const auto fraction = tenths % 10;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(wholeSeconds, 2, 10, QLatin1Char('0'))
        .arg(fraction);
}

void CommandsPanel::setTimeBreakdown(const std::vector<OperationTimeBreakdown>& operations)
{
    timeBreakdownTree->clear();
    CommandTimeBreakdown total;
    for (const auto& operation : operations) {
        const auto& time = operation.time;
        auto* row = new QTreeWidgetItem(timeBreakdownTree);
        row->setText(0, operation.name.empty() ? tr("Operation") : QString::fromStdString(operation.name));
        row->setText(1, formatBreakdownTime(time.cuttingSeconds));
        row->setText(2, formatBreakdownTime(time.rapidSeconds));
        row->setText(3, formatBreakdownTime(time.dwellSeconds));
        row->setText(4, formatBreakdownTime(time.assumedRateSeconds));
        row->setText(5, formatBreakdownTime(
            time.cuttingSeconds + time.rapidSeconds + time.dwellSeconds
        ));
        if (time.omittedTime) {
            row->setToolTip(0, tr("Some movement or delay in this operation was not included. See Issues."));
            row->setText(0, row->text(0) + QStringLiteral(" *"));
        }
        total.cuttingSeconds += time.cuttingSeconds;
        total.rapidSeconds += time.rapidSeconds;
        total.dwellSeconds += time.dwellSeconds;
        total.assumedRateSeconds += time.assumedRateSeconds;
        total.omittedTime |= time.omittedTime;
    }

    auto* totalRow = new QTreeWidgetItem(timeBreakdownTree);
    totalRow->setText(0, tr("Selected operations"));
    totalRow->setText(1, formatBreakdownTime(total.cuttingSeconds));
    totalRow->setText(2, formatBreakdownTime(total.rapidSeconds));
    totalRow->setText(3, formatBreakdownTime(total.dwellSeconds));
    totalRow->setText(4, formatBreakdownTime(total.assumedRateSeconds));
    totalRow->setText(5, formatBreakdownTime(
        total.cuttingSeconds + total.rapidSeconds + total.dwellSeconds
    ));
    for (int column = 0; column < timeBreakdownTree->columnCount(); ++column) {
        totalRow->setBackground(column, QBrush(palette().alternateBase()));
        QFont bold = totalRow->font(column);
        bold.setBold(true);
        totalRow->setFont(column, bold);
    }
    timeBreakdownTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int column = 1; column < timeBreakdownTree->columnCount(); ++column) {
        timeBreakdownTree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
}

void CommandsPanel::setCurrentCommand(int command)
{
    if (command == currentCommand) return;
    currentCommand = command;
    if (command < 0 || command >= static_cast<int>(commandBlocks.size())) {
        editor->setExtraSelections({});
        contextLabel->clear();
        return;
    }
    contextLabel->setText(commandContexts[command]);
    QTextEdit::ExtraSelection selection;
    selection.cursor = QTextCursor(editor->document()->findBlockByNumber(commandBlocks[command]));
    selection.format.setBackground(palette().highlight());
    selection.format.setForeground(palette().highlightedText());
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    editor->setExtraSelections({selection});
    if (isFollowing()) revealCurrent();
}

void CommandsPanel::revealCurrent()
{
    if (currentCommand < 0 || currentCommand >= static_cast<int>(commandBlocks.size())) return;
    const QSignalBlocker blocker(editor);
    editor->setTextCursor(QTextCursor(editor->document()->findBlockByNumber(commandBlocks[currentCommand])));
    editor->centerCursor();
}

void CommandsPanel::setIssues(const std::vector<SimulationIssue>& findings)
{
    const QSignalBlocker blocker(issueList);
    issues = findings;
    issueList->clear();
    issueDetail->clear();
    pausedIssue = -1;
    continueButton->setEnabled(false);
    continueAllButton->setEnabled(false);
    issueCount->setText(issues.empty() ? tr("No issues found by available checks")
                                      : tr("Issues: %1 — preview may be incomplete").arg(issues.size()));
    for (const auto& issue : issues) {
        const QString severity = issue.severity == IssueSeverity::Error ? tr("Error") : tr("Warning");
        const QString operation = issue.command >= 0 && issue.command < static_cast<int>(sourceCommands.size())
            ? QString::fromStdString(sourceCommands[issue.command].operation) : tr("Unknown operation");
        auto* item = new QListWidgetItem(tr("%1 · Line %2 · %3").arg(severity).arg(issue.command + 1).arg(operation), issueList);
        item->setToolTip(QString::fromStdString(issue.message));
    }
}

void CommandsPanel::showIssue(int row, bool seek)
{
    if (row < 0 || row >= static_cast<int>(issues.size())) return;
    const QSignalBlocker blocker(issueList);
    issueList->setCurrentRow(row);
    issueList->scrollToItem(issueList->item(row));
    const auto& issue = issues[row];
    QString detail = QString::fromStdString(issue.message);
    if (issue.command >= 0 && issue.command < static_cast<int>(sourceCommands.size())) {
        detail.prepend(QString::fromStdString(sourceCommands[issue.command].tool) + QLatin1Char('\n'));
    }
    issueDetail->setText(detail);
    if (seek && issue.command >= 0) {
        Q_EMIT commandSelected(issue.command);
        setFollowing(true);
        revealCurrent();
    }
}

void CommandsPanel::setIssueState(bool enabled, int paused, bool playing)
{
    const QSignalBlocker blocker(pauseIssues);
    pauseIssues->setChecked(enabled);
    continueButton->setEnabled(!playing && !issues.empty());
    continueAllButton->setEnabled(!playing && !issues.empty());
    if (paused >= 0 && paused != pausedIssue) showIssue(paused, false);
    pausedIssue = paused;
}

void CommandsPanel::setPauseOnToolChanges(bool enabled)
{
    const QSignalBlocker blocker(pauseToolChanges);
    pauseToolChanges->setChecked(enabled);
}

bool CommandsPanel::isFollowing() const { return followButton->isChecked(); }
void CommandsPanel::setFollowing(bool follow) { followButton->setChecked(follow); }
}
