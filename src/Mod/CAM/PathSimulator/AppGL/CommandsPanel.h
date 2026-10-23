// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QWidget>
#include "SimulationIssue.h"
#include "TimeBreakdown.h"
#include <string>
#include <vector>

class QPlainTextEdit;
class QLabel;
class QToolButton;
class QCheckBox;
class QListWidget;
class QTreeWidget;

namespace CAMSimulator
{
struct SimulationCommand
{
    std::string code;
    double horizontalRapid;
    double verticalRapid;
    int operationId;
    std::string operation;
    std::string tool;
};

class CommandsPanel: public QWidget
{
    Q_OBJECT
public:
    explicit CommandsPanel(QWidget* parent = nullptr);
    void setCommands(const std::vector<SimulationCommand>& commands);
    void setCurrentCommand(int command);
    void setIssues(const std::vector<SimulationIssue>& issues);
    void setTimeBreakdown(const std::vector<OperationTimeBreakdown>& operations);
    void setIssueState(bool pauseOnIssues, int pausedIssue, bool playing);
    void setPauseOnToolChanges(bool enabled);
    bool isFollowing() const;
    void setFollowing(bool follow);

Q_SIGNALS:
    void commandSelected(int command);
    void closeRequested();
    void pauseOnIssuesChanged(bool enabled);
    void pauseOnToolChangesChanged(bool enabled);
    void continueRequested(bool withoutPausing);
    void reportRequested();
    void voxelStockPreviewRequested();
    void intermediateStockPreviewRequested();
    void stockComparisonRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showIssue(int row, bool seek);
    std::vector<SimulationCommand> sourceCommands;
    std::vector<SimulationIssue> issues;
    QCheckBox* pauseIssues;
    QCheckBox* pauseToolChanges;
    QListWidget* issueList;
    QLabel* issueCount;
    QLabel* issueDetail;
    QToolButton* continueButton;
    QToolButton* continueAllButton;
    int pausedIssue = -1;
    void revealCurrent();
    void selectCurrent();
    bool mouseSelecting = false;
    QLabel* contextLabel;
    std::vector<QString> commandContexts;
    QPlainTextEdit* editor;
    QToolButton* followButton;
    QToolButton* timeBreakdownToggle;
    QTreeWidget* timeBreakdownTree;
    std::vector<int> commandBlocks;
    std::vector<int> operationStarts;
    std::vector<int> blockCommands;
    int currentCommand = -1;
};
}
