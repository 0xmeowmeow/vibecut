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

#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTextCursor>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

namespace {
const QString kNoisePrompt = QStringLiteral("Remove background noise from the selected clip.");

struct Suggestion
{
    QString id;
    QString label;
    QString prompt;
};

const QVector<Suggestion> &suggestions()
{
    static const QVector<Suggestion> list = {
        {QStringLiteral("denoise"), QStringLiteral("Remove background noise from the selected clip"), kNoisePrompt},
        {QStringLiteral("subtitles"), QStringLiteral("Generate subtitles"),
         QStringLiteral("Generate subtitles for this project. Set up Whisper first if it isn't ready yet.")},
        {QStringLiteral("list-clips"), QStringLiteral("What clips are on my timeline?"),
         QStringLiteral("List the clips on my timeline.")},
        {QStringLiteral("help"), QStringLiteral("What can you help me with?"),
         QStringLiteral("What can you help me with right now?")},
    };
    return list;
}

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
    , m_transcript(new QTextBrowser(this))
    , m_status(new QLabel(this))
    , m_progress(new QProgressBar(this))
    , m_input(new QLineEdit(this))
    , m_send(new QPushButton(i18n("Send"), this))
    , m_tools(new VibeCutTools(this))
    , m_agent(new VibeCutAgent(m_tools, this))
{
    setObjectName(QStringLiteral("VibeCutDock"));

    m_transcript->setReadOnly(true);
    m_transcript->setAcceptRichText(false);
    m_transcript->setOpenLinks(false);
    m_transcript->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_input->setPlaceholderText(i18n("Ask VibeCut to edit the timeline…"));

    m_progress->setRange(0, 0); // indeterminate: we don't get token-level progress from the API
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(4);
    m_progress->setVisible(false);

    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_progress, 1);

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_send);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_transcript, 1);
    layout->addLayout(statusRow);
    layout->addLayout(inputRow);

    connect(m_send, &QPushButton::clicked, this, &VibeCutDock::submit);
    connect(m_input, &QLineEdit::returnPressed, this, &VibeCutDock::submit);
    connect(m_transcript, &QTextBrowser::anchorClicked, this, &VibeCutDock::onSuggestionClicked);

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
        if (!m_streamStarted) {
            // assistantMessage("") only ever arrives after at least one real
            // tool call this exchange (the agent turns a truly empty,
            // nothing-happened turn into errorOccurred instead) - so this is
            // a closing marker for what the lines above already showed, not
            // an independent claim that something worked.
            appendLine(t.isEmpty() ? i18n("✓ Finished (see above).") : QStringLiteral("VibeCut: %1").arg(t),
                       t.isEmpty() ? QStringLiteral("#2a8") : QString());
        }
        m_streamStarted = false;
    });
    connect(m_agent, &VibeCutAgent::toolInvoked, this, [this](const QString &name, const QString &args) {
        const QString friendly = describeTool(name, args);
        if (!friendly.isEmpty()) {
            appendLine(friendly, QStringLiteral("#888"));
        }
    });
    connect(m_agent, &VibeCutAgent::toolFailed, this, [this](const QString &name, const QString &error) {
        appendLine(i18n("⚠ %1 failed: %2", name, error), QStringLiteral("#c33"));
    });
    connect(m_agent, &VibeCutAgent::toolCompleted, this, [this](const QString &name, const QString &resultJson) {
        // Ground truth for read-only tools, shown regardless of whether the
        // model bothers to narrate it - see DESIGN_SPECS.md §3.
        const QString summary = describeToolResult(name, resultJson);
        if (!summary.isEmpty()) {
            appendLine(summary, QStringLiteral("#888"));
        }
    });
    connect(m_agent, &VibeCutAgent::userQuestionRaised, this, [this](const QString &q) {
        appendLine(QStringLiteral("VibeCut asks: %1").arg(q), QStringLiteral("#c80"));
    });
    connect(m_agent, &VibeCutAgent::backgroundProgress, this, [this](const QString &message) {
        appendLine(QStringLiteral("⏳ %1").arg(message), QStringLiteral("#888"));
    });
    connect(m_agent, &VibeCutAgent::errorOccurred, this, [this](const QString &e) {
        appendLine(QStringLiteral("⚠ %1").arg(e), QStringLiteral("#c33"));
        m_streamStarted = false;
    });

    if (!m_agent->hasApiKey()) {
        appendLine(i18n("Set ANTHROPIC_API_KEY in the environment and restart to use VibeCut."), QStringLiteral("#c33"));
        m_input->setEnabled(false);
        m_send->setEnabled(false);
        m_status->setText(QStringLiteral("No API key"));
    } else {
        appendWelcome();
        m_status->setText(QStringLiteral("Ready"));
    }
}

void VibeCutDock::appendWelcome()
{
    QString html = QStringLiteral("<b>%1</b><br>%2<br>")
                       .arg(i18n("Hi, I'm VibeCut."), i18n("I can act on your live timeline. Try one of these, or type your own request below:"));
    for (const Suggestion &s : suggestions()) {
        html += QStringLiteral("• <a href=\"vibecut://%1\">%2</a><br>").arg(s.id, s.label.toHtmlEscaped());
    }
    m_transcript->append(html);
    m_transcript->moveCursor(QTextCursor::End);
}

void VibeCutDock::onSuggestionClicked(const QUrl &url)
{
    if (m_agent->busy()) {
        return;
    }
    const QString id = url.host();
    if (id == QLatin1String("denoise")) {
        runNoiseSuggestion();
        return;
    }
    for (const Suggestion &s : suggestions()) {
        if (s.id == id) {
            cancelPendingSelection();
            sendPrompt(s.prompt);
            return;
        }
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
    m_progress->setVisible(busy);
    if (!busy && hasKey) {
        m_input->setFocus();
    }
}

QString VibeCutDock::describeTool(const QString &name, const QString &argsJson) const
{
    if (name == QLatin1String("timeline_list_clips")) {
        return i18n("Looking at the clips on your timeline…");
    }
    if (name == QLatin1String("timeline_get_selection")) {
        return i18n("Checking what's selected…");
    }
    if (name == QLatin1String("ask_user")) {
        return QString(); // the actual question is shown via userQuestionRaised
    }
    if (name == QLatin1String("speech_status")) {
        return i18n("Checking speech-to-text status…");
    }
    if (name == QLatin1String("speech_setup")) {
        return i18n("Starting Whisper speech-to-text setup…");
    }
    if (name == QLatin1String("generate_subtitles")) {
        return i18n("Exporting audio and starting Whisper transcription…");
    }
    if (name == QLatin1String("effect_apply")) {
        static const QHash<QString, QString> friendlyNames = {
            {QStringLiteral("denoise"), i18n("AI Noise Removal (DeepFilterNet)")},
            {QStringLiteral("denoise_light"), i18n("Noise Suppressor (RNNoise)")},
        };
        const QJsonObject args = QJsonDocument::fromJson(argsJson.toUtf8()).object();
        const QString key = args.value(QStringLiteral("effect")).toString();
        return i18n("Adding \"%1\"…", friendlyNames.value(key, key));
    }
    return i18n("Running %1…", name);
}

QString VibeCutDock::describeToolResult(const QString &name, const QString &resultJson) const
{
    const QJsonObject result = QJsonDocument::fromJson(resultJson.toUtf8()).object();
    if (!result.value(QStringLiteral("ok")).toBool()) {
        return QString(); // toolFailed already shows the failure line
    }
    if (name == QLatin1String("timeline_get_selection")) {
        const int cid = result.value(QStringLiteral("selected_clip_id")).toInt(-1);
        return cid == -1 ? i18n("→ Nothing is selected on the timeline.") : i18n("→ Clip %1 is selected.", cid);
    }
    if (name == QLatin1String("timeline_list_clips")) {
        return i18n("→ %1 clip(s) on the timeline.", result.value(QStringLiteral("clips")).toArray().size());
    }
    if (name == QLatin1String("speech_status")) {
        const bool ready = result.value(QStringLiteral("dependencies_installed")).toBool();
        const int models = result.value(QStringLiteral("models_installed")).toArray().size();
        return ready ? i18n("→ Whisper is ready (%1 model(s) installed).", models) : i18n("→ Whisper is not set up yet.");
    }
    return QString();
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
