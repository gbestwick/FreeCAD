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

#include "GuiDisplay.h"
#include "MillMotion.h"

#include "ui_GuiDisplay.h"
#include <algorithm>
#include <cmath>
#include <limits>

// include this last as the defines can mess up other includes
#include "OpenGlWrapper.h"

namespace CAMSimulator
{

GuiDisplay::GuiDisplay(QWidget* parent)
    : QWidget(parent)
    , ui(new Ui_GuiDisplay)
{
    ui->setupUi(this);
    ui->singleStepButton->setToolTip(tr("Advance to the next CAM command"));
    connect(ui->commandsButton, &QToolButton::toggled, this, &GuiDisplay::commandsVisibleChanged);
    setMotion(nullptr);
    setMachiningTime(0, 0, false);

    playing = true;
    setPlaying(false);

    speed = 0;
    setSpeed(1);

    connect(ui->slowerButton, &QToolButton::clicked, this, &GuiDisplay::onSlowerFasterButtonClicked);
    connect(ui->fasterButton, &QToolButton::clicked, this, &GuiDisplay::onSlowerFasterButtonClicked);
    connect(ui->viewAllButton, &QToolButton::clicked, this, &GuiDisplay::viewAll);
    connect(ui->checkCollisionsButton, &QToolButton::clicked, this, &GuiDisplay::collisionCheckRequested);
    connect(ui->cancelCollisionCheckButton, &QToolButton::clicked, this,
            &GuiDisplay::collisionCheckCancelRequested);
    connect(ui->rotateButton, &QToolButton::toggled, this, &GuiDisplay::rotateEnableChanged);
    connect(ui->pathButton, &QToolButton::toggled, this, &GuiDisplay::pathVisibleChanged);
    connect(ui->ssaoButton, &QToolButton::toggled, this, &GuiDisplay::ssaoEnableChanged);
}

void GuiDisplay::setCommandsVisible(bool visible)
{
    ui->commandsButton->setChecked(visible);
}

GuiDisplay::~GuiDisplay()
{
    delete ui;
}

void GuiDisplay::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    setMask(childrenRegion());
}

void GuiDisplay::setPlaying(bool b)
{
    if (b == playing) {
        return;
    }

    playing = b;

    const auto icon = playing ? ":/gl_simulator/Pause.png" : ":/gl_simulator/Play.png";
    ui->playButton->setIcon(QIcon(QString::fromUtf8(icon)));
}

void GuiDisplay::on_playButton_clicked()
{
    setPlaying(!playing);
    Q_EMIT play(playing);
}

void GuiDisplay::on_singleStepButton_clicked()
{
    setPlaying(false);
    Q_EMIT singleStep();
}

void GuiDisplay::setSpeed(int s)
{
    if (s == speed) {
        return;
    }

    speed = s;
    ui->speedLabel->setText(tr("x%1").arg(speed));
}

static const std::vector<int> speeds = {1, 2, 5, 10, 25, 50};

std::vector<int>::const_iterator findNearestSpeed(int speed)
{
    int dist = std::numeric_limits<int>::max();
    auto ret = speeds.cend();

    for (auto it = speeds.cbegin(); it != speeds.cend(); it++) {
        const int curdist = std::abs(*it - speed);
        if (curdist < dist) {
            dist = curdist;
            ret = it;
        }
    }

    return ret;
}

void GuiDisplay::onSlowerFasterButtonClicked()
{
    auto it = findNearestSpeed(speed);

    const bool slower = sender() == ui->slowerButton;
    const bool faster = !slower;

    if (slower && it != speeds.begin()) {
        it--;
    }
    else if (faster && it != (speeds.end() - 1)) {
        it++;
    }

    setSpeed(*it);
    Q_EMIT speedChanged(*it);
}

void GuiDisplay::setMotion(const MillMotion* motion, double travelFeed, bool estimated)
{
    QString feed = tr("Not specified");
    QString spindle = tr("Not specified");
    if (motion) {
        if (motion->rapid) {
            feed = travelFeed >= 0
                ? tr("%1 mm/min (rapid)").arg(locale().toString(travelFeed, 'f', 1))
                : tr("Rapid");
        }
        else if (motion->feedRate >= 0) {
            feed = tr("%1 mm/min").arg(locale().toString(motion->feedRate, 'f', 1));
        }
        if (motion->spindleDirection == 0) {
            spindle = tr("Stopped");
        }
        else {
            const QString direction = motion->spindleDirection > 0 ? tr("CW") : tr("CCW");
            spindle = motion->spindleSpeed >= 0
                ? tr("%1 RPM (%2)").arg(locale().toString(motion->spindleSpeed, 'f', 0), direction)
                : tr("%1 (RPM not specified)").arg(direction);
        }
    }
    const QString flood = motion ? (motion->floodCoolant ? tr("On") : tr("Off")) : tr("Not specified");
    const QString mist = motion ? (motion->mistCoolant ? tr("On") : tr("Off")) : tr("Not specified");
    QString text = tr("Feed: %1    |    Spindle: %2    |    Flood: %3    |    Mist: %4")
                       .arg(feed, spindle, flood, mist);
    if (estimated) {
        text += tr("    |    Timing estimated (missing feed rate)");
    }
    motionText = text;
    updateInformation();
}

static QString formatMachiningTime(double seconds)
{
    // Duration arithmetic rather than QTime keeps jobs longer than 24 hours intact.
    const qint64 rounded = qRound64(std::max(0.0, seconds));
    return QStringLiteral("%1:%2:%3")
        .arg(rounded / 3600, 2, 10, QLatin1Char('0'))
        .arg((rounded / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(rounded % 60, 2, 10, QLatin1Char('0'));
}

void GuiDisplay::setMachiningTime(double elapsed, double total, bool assumedRates)
{
    timeText = tr("Simulated elapsed: %1    |    Estimated total: %2")
                   .arg(formatMachiningTime(elapsed), formatMachiningTime(total));
    if (assumedRates) {
        timeText += tr(" (includes assumed speeds)");
    }
    updateInformation();
}

void GuiDisplay::setIssueStatus(int count, bool paused)
{
    issueText = count == 0 ? QString() : paused
        ? tr("Paused on an issue — review Commands, then Continue or Continue without pausing (%1 issues).").arg(count)
        : tr("%1 issues — preview/time may be incomplete or approximate. Review Commands for details.").arg(count);
    updateInformation();
}

void GuiDisplay::setCollisionCheckState(const QString& status, int progress, bool running)
{
    ui->collisionStatusLabel->setText(status);
    ui->collisionStatusLabel->setVisible(true);
    ui->collisionProgressBar->setValue(std::clamp(progress, 0, 100));
    ui->collisionProgressBar->setVisible(true);
    ui->cancelCollisionCheckButton->setVisible(running);
    ui->cancelCollisionCheckButton->setEnabled(running);
    ui->checkCollisionsButton->setEnabled(!running);
    ui->playButton->setEnabled(!running);
    ui->singleStepButton->setEnabled(!running);
    ui->stageSlider->setEnabled(!running);
    ui->verticalLayout->activate();
    setMask(childrenRegion());
}

void GuiDisplay::clearCollisionCheckState()
{
    ui->collisionStatusLabel->setVisible(false);
    ui->collisionProgressBar->setVisible(false);
    ui->cancelCollisionCheckButton->setVisible(false);
    ui->checkCollisionsButton->setEnabled(true);
    ui->playButton->setEnabled(true);
    ui->singleStepButton->setEnabled(true);
    ui->stageSlider->setEnabled(true);
    ui->verticalLayout->activate();
    setMask(childrenRegion());
}

void GuiDisplay::updateInformation()
{
    QString text = timeText.isEmpty() ? motionText : motionText + QLatin1Char('\n') + timeText;
    if (!issueText.isEmpty()) text += QLatin1Char('\n') + issueText;
    if (ui->motionLabel->text() == text) {
        return;
    }
    ui->motionLabel->setText(text);
    // The overlay uses a child-region mask to leave the 3D view interactive.
    ui->verticalLayout->activate();
    setMask(childrenRegion());
}

void GuiDisplay::setStage(float f, int total)
{
    ui->stageSlider->setMaximum(total);
    ui->stageSlider->setValue(f * total);
}

void GuiDisplay::on_stageSlider_sliderMoved(int value)
{
    const float f = (float)value / ui->stageSlider->maximum();
    Q_EMIT stageChanged(f);
}

void GuiDisplay::setStockVisible(bool b)
{
    stockVisible = b;

    QSignalBlocker blocker(ui->stockModelButton);
    ui->stockModelButton->setChecked(stockVisible && baseVisible);
}

void GuiDisplay::setBaseVisible(bool b)
{
    baseVisible = b;

    QSignalBlocker blocker(ui->stockModelButton);
    ui->stockModelButton->setChecked(stockVisible && baseVisible);
}

void GuiDisplay::on_stockModelButton_clicked()
{
    // stock -> base -> both
    //   ^---------------'

    bool sv = false;
    bool bv = false;

    if (stockVisible == baseVisible) {
        sv = true;
        bv = false;
    }
    else if (!baseVisible) {
        sv = false;
        bv = true;
    }
    else if (!stockVisible) {
        sv = true;
        bv = true;
    }

    if (sv != stockVisible) {
        setStockVisible(sv);
        Q_EMIT stockVisibleChanged(sv);
    }

    if (bv != baseVisible) {
        setBaseVisible(bv);
        Q_EMIT baseVisibleChanged(bv);
    }
}

void GuiDisplay::setRotateEnabled(bool b)
{
    ui->rotateButton->setChecked(b);
}

void GuiDisplay::setPathVisible(bool b)
{
    QSignalBlocker blocker(ui->pathButton);
    ui->pathButton->setChecked(b);
}

void GuiDisplay::setSsaoEnabled(bool b)
{
    QSignalBlocker blocker(ui->ssaoButton);
    ui->ssaoButton->setChecked(b);
}

void GuiDisplay::setBackgroundColor(const QColor& background)
{
    const double luma = 0.299 * background.redF() + 0.587 * background.greenF()
        + 0.114 * background.blueF();
    const QColor ink = luma > 0.55 ? QColor(25, 25, 25) : QColor(Qt::white);
    ui->speedLabel->setStyleSheet(QStringLiteral("color:%1").arg(ink.name()));
}

}  // namespace CAMSimulator
