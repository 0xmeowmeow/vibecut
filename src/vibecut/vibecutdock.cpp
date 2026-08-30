/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "vibecutdock.h"
#include "vibecutagent.h"
#include "vibecuttools.h"

#include "core.h"
#include "mainwindow.h"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {
const QString kNoisePrompt = QStringLiteral("Remove background noise from the selected clip.");

TimelineController *currentTimelineController()
{
    if (!pCore || !pCore->window()) {
        return nullptr;
    }
    TimelineWidget *tl = pCore->window()->getCurrentTimeline();
    return tl ? tl->controller() : nullptr;
}
} // namespace

VibeCutDock::VibeCutDock(QWidget *parent)
    : QWidget(parent)
    , m_transcript(new QTextEdit(this))
    , m_status(new QLabel(this))
    , m_suggestNoise(new QPushButton(i18n("Remove background noise"), this))
    , m_input(new QLineEdit(this))
    , m_send(new QPushButton(i18n("Send"), this))
    , m_tools(new VibeCutTools(this))
    , m_agent(new VibeCutAgent(m_tools, this))
{
    setObjectName(QStringLiteral("VibeCutDock"));

    m_transcript->setReadOnly(true);
    m_transcript->setAcceptRichText(false);
    m_input->setPlaceholderText(i18n("Ask VibeCut to edit the timeline…"));
    m_suggestNoise->setToolTip(i18n("Apply AI noise removal (DeepFilterNet) to the selected clip"));

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_send);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_transcript, 1);
    layout->addWidget(m_status);
    layout->addWidget(m_suggestNoise);
    layout->addLayout(inputRow);

    connect(m_send, &QPushButton::clicked, this, &VibeCutDock::submit);
    connect(m_input, &QLineEdit::returnPressed, this, &VibeCutDock::submit);
    connect(m_suggestNoise, &QPushButton::clicked, this, &VibeCutDock::runNoiseSuggestion);

    connect(m_agent, &VibeCutAgent::statusChanged, this, [this](const QString &s) {
        m_status->setText(s);
        setBusyUi(s != QStringLiteral("Ready"));
    });
    connect(m_agent, &VibeCutAgent::assistantTextDelta, this, [this](const QString &t) {
        if (!m_streamStarted) {
            m_transcript->append(QStringLiteral("VibeCut: "));
            m_streamStarted = true;
        }
        m_transcript->moveCursor(QTextCursor::End);
        m_transcript->insertPlainText(t);
        m_transcript->moveCursor(QTextCursor::End);
    });
    connect(m_agent, &VibeCutAgent::assistantMessage, this, [this](const QString &t) {
        if (!m_streamStarted && !t.isEmpty()) {
            appendLine(QStringLiteral("VibeCut: %1").arg(t));
        }
        m_streamStarted = false;
    });
    connect(m_agent, &VibeCutAgent::toolInvoked, this, [this](const QString &name, const QString &args) {
        appendLine(QStringLiteral("→ %1 %2").arg(name, args), QStringLiteral("#888"));
    });
    connect(m_agent, &VibeCutAgent::userQuestionRaised, this, [this](const QString &q) {
        appendLine(QStringLiteral("VibeCut asks: %1").arg(q), QStringLiteral("#c80"));
    });
    connect(m_agent, &VibeCutAgent::errorOccurred, this, [this](const QString &e) {
        appendLine(QStringLiteral("⚠ %1").arg(e), QStringLiteral("#c33"));
        m_streamStarted = false;
    });

    if (!m_agent->hasApiKey()) {
        appendLine(i18n("Set ANTHROPIC_API_KEY in the environment and restart to use VibeCut."), QStringLiteral("#c33"));
        m_input->setEnabled(false);
        m_send->setEnabled(false);
        m_suggestNoise->setEnabled(false);
        m_status->setText(QStringLiteral("No API key"));
    } else {
        m_status->setText(QStringLiteral("Ready"));
    }
}

void VibeCutDock::submit()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || m_agent->busy()) {
        return;
    }
    m_input->clear();
    cancelPendingSelection();
    sendPrompt(text);
}

void VibeCutDock::runNoiseSuggestion()
{
    if (m_agent->busy()) {
        return;
    }
    cancelPendingSelection();

    if (m_tools->selectedClipId() != -1) {
        sendPrompt(kNoisePrompt);
        return;
    }

    TimelineController *ctl = currentTimelineController();
    if (!ctl) {
        appendLine(i18n("Open a project and add a clip to the timeline first."), QStringLiteral("#c33"));
        return;
    }

    m_pendingPrompt = kNoisePrompt;
    m_awaitingSelection = true;
    m_status->setText(i18n("Waiting for a clip…"));
    appendLine(i18n("No clip selected — click the clip with your audio in the timeline and I'll apply it automatically."),
               QStringLiteral("#c80"));

    m_selectionConn = connect(ctl, &TimelineController::selectionChanged, this, [this]() {
        if (!m_awaitingSelection) {
            return;
        }
        if (m_tools->selectedClipId() == -1) {
            return;
        }
        const QString prompt = m_pendingPrompt;
        cancelPendingSelection();
        sendPrompt(prompt);
    });
}

void VibeCutDock::cancelPendingSelection()
{
    m_awaitingSelection = false;
    m_pendingPrompt.clear();
    if (m_selectionConn) {
        disconnect(m_selectionConn);
        m_selectionConn = {};
    }
}

void VibeCutDock::sendPrompt(const QString &text)
{
    appendLine(QStringLiteral("You: %1").arg(text), QStringLiteral("#39c"));
    m_streamStarted = false;
    m_agent->sendUserMessage(text);
}

void VibeCutDock::setBusyUi(bool busy)
{
    const bool hasKey = m_agent->hasApiKey();
    m_input->setEnabled(hasKey && !busy);
    m_send->setEnabled(hasKey && !busy);
    m_suggestNoise->setEnabled(hasKey && !busy);
    if (!busy && hasKey) {
        m_input->setFocus();
    }
}

void VibeCutDock::appendLine(const QString &text, const QString &cssColor)
{
    if (cssColor.isEmpty()) {
        m_transcript->append(text);
    } else {
        m_transcript->append(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(cssColor, text.toHtmlEscaped()));
    }
    m_transcript->moveCursor(QTextCursor::End);
}
