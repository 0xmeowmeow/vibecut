/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "vibecutagent.h"
#include "vibecuttools.h"

#include <QDebug>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {
constexpr char kEndpoint[] = "https://api.anthropic.com/v1/messages";
constexpr char kApiVersion[] = "2023-06-01";

// Matches vibecad's default model. The claude-api skill's house default is
// claude-opus-5; we deliberately track vibecad here. One constant to change.
constexpr char kModel[] = "claude-sonnet-5";

constexpr int kMaxTokens = 8192;

// Deliberately tiny, like vibecad's VIBECAD_SYSTEM_INSTRUCTIONS. Keep it well
// under a few KB; it is sent as a cached system block on every request.
const QString kSystemPrompt = QStringLiteral(
    "You are VibeCut, an assistant embedded in the Kdenlive video editor. You act on the user's live "
    "timeline through the provided tools only. Never invent clip ids or effect names: read the timeline "
    "or the selection first, then act. Prefer the current selection when the user does not name a clip. "
    "Use ask_user only when the answer changes which clip or effect to touch. When a tool fails, report "
    "exactly what failed instead of guessing, and never tell the user something worked unless the tool "
    "result confirms it. effect_apply reports already_present and effect_count_on_clip on success — say "
    "concretely what was added (or that it was already there), not just 'done'. For speech-to-text: call "
    "speech_status first; if not ready, call speech_setup yourself (it uses Kdenlive's own installer and "
    "runs in the background — tell the user a one-time confirmation dialog may appear) rather than telling "
    "the user to open Settings. A compound request (e.g. denoise AND subtitles) means do every part before "
    "stopping, not just the first. Never end a turn silently: if a tool result makes the next step "
    "ambiguous, call ask_user with the specific options instead of giving up with no text and no action. "
    "Keep replies short.");

QByteArray compact(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}
} // namespace

// Recurring empty-turn reports have all come from a panel that had already
// been through several prior exchanges in the same process lifetime - this
// makes the accumulated history size visible in the diagnostic instead of
// having to guess whether context growth is the actual trigger.
QString VibeCutAgent::historyDiagnostic() const
{
    return QStringLiteral("messages=%1 approx_bytes=%2")
        .arg(m_messages.size())
        .arg(QJsonDocument(m_messages).toJson(QJsonDocument::Compact).size());
}

VibeCutAgent::VibeCutAgent(VibeCutTools *tools, QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_tools(tools)
    , m_model(QString::fromLatin1(kModel))
    , m_systemPrompt(kSystemPrompt)
{
    m_apiKey = qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed();
    connect(m_tools, &VibeCutTools::userQuestionRaised, this, &VibeCutAgent::userQuestionRaised);
    connect(m_tools, &VibeCutTools::backgroundProgress, this, &VibeCutAgent::backgroundProgress);
}

VibeCutAgent::~VibeCutAgent() = default;

bool VibeCutAgent::hasApiKey() const
{
    return !m_apiKey.isEmpty();
}

void VibeCutAgent::sendUserMessage(const QString &text)
{
    if (m_reply) {
        fail(QStringLiteral("Still working on the previous message."));
        return;
    }
    if (!hasApiKey()) {
        fail(QStringLiteral("ANTHROPIC_API_KEY is not set in the environment."));
        return;
    }
    m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                  {QStringLiteral("content"), text}});
    m_toolTurns = 0;
    m_anyToolCalledThisExchange = false;
    m_retriedEmptyTurn = false;
    startRequest();
}

void VibeCutAgent::resetConversation()
{
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
        r->deleteLater();
    }
    m_messages = QJsonArray();
    m_toolTurns = 0;
    m_anyToolCalledThisExchange = false;
    m_retriedEmptyTurn = false;
    Q_EMIT statusChanged(QStringLiteral("Ready"));
}

void VibeCutAgent::resetStreamState()
{
    m_sse.reset();
    m_blocks = QJsonArray();
    m_curBlock = QJsonObject();
    m_curText.clear();
    m_curThinking.clear();
    m_curJson.clear();
    m_stopReason.clear();
    m_turnFinished = false;
}

void VibeCutAgent::startRequest()
{
    resetStreamState();

    QJsonObject systemBlock{{QStringLiteral("type"), QStringLiteral("text")},
                            {QStringLiteral("text"), m_systemPrompt},
                            {QStringLiteral("cache_control"), QJsonObject{{QStringLiteral("type"), QStringLiteral("ephemeral")}}}};

    QJsonObject body{
        {QStringLiteral("model"), m_model},
        {QStringLiteral("max_tokens"), kMaxTokens},
        {QStringLiteral("stream"), true},
        // Disabled thinking is known to make Claude occasionally end an
        // agentic turn without emitting the tool_use block it clearly
        // intended to (empty final text, no error) - that's what happened
        // here: get_selection ran, effect_apply never did. Adaptive thinking
        // fixes it; the stream handler already replays thinking blocks
        // (with their signature) unchanged, so no other change is needed.
        {QStringLiteral("thinking"), QJsonObject{{QStringLiteral("type"), QStringLiteral("adaptive")}}},
        {QStringLiteral("system"), QJsonArray{systemBlock}},
        {QStringLiteral("tools"), m_tools->schemas()},
        {QStringLiteral("messages"), m_messages},
    };

    QNetworkRequest req{QUrl(QString::fromLatin1(kEndpoint))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setRawHeader(QByteArrayLiteral("x-api-key"), m_apiKey.toUtf8());
    req.setRawHeader(QByteArrayLiteral("anthropic-version"), QByteArrayLiteral(kApiVersion));

    m_reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &VibeCutAgent::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &VibeCutAgent::onFinished);

    Q_EMIT statusChanged(QStringLiteral("Thinking…"));
}

void VibeCutAgent::onReadyRead()
{
    if (!m_reply) {
        return;
    }
    const QList<SseParser::Event> events = m_sse.feed(m_reply->readAll());
    for (const SseParser::Event &ev : events) {
        handleEvent(ev);
    }
}

void VibeCutAgent::handleEvent(const SseParser::Event &ev)
{
    const QJsonObject obj = QJsonDocument::fromJson(ev.data).object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("content_block_start")) {
        m_curBlock = obj.value(QStringLiteral("content_block")).toObject();
        m_curText.clear();
        m_curThinking.clear();
        m_curJson.clear();
    } else if (type == QLatin1String("content_block_delta")) {
        const QJsonObject delta = obj.value(QStringLiteral("delta")).toObject();
        const QString dt = delta.value(QStringLiteral("type")).toString();
        if (dt == QLatin1String("text_delta")) {
            const QString t = delta.value(QStringLiteral("text")).toString();
            m_curText += t;
            Q_EMIT assistantTextDelta(t);
        } else if (dt == QLatin1String("thinking_delta")) {
            m_curThinking += delta.value(QStringLiteral("thinking")).toString();
        } else if (dt == QLatin1String("input_json_delta")) {
            m_curJson += delta.value(QStringLiteral("partial_json")).toString();
        } else if (dt == QLatin1String("signature_delta")) {
            m_curBlock.insert(QStringLiteral("signature"), delta.value(QStringLiteral("signature")));
        }
    } else if (type == QLatin1String("content_block_stop")) {
        const QString bt = m_curBlock.value(QStringLiteral("type")).toString();
        if (bt == QLatin1String("text")) {
            m_curBlock.insert(QStringLiteral("text"), m_curText);
        } else if (bt == QLatin1String("thinking")) {
            m_curBlock.insert(QStringLiteral("thinking"), m_curThinking);
        } else if (bt == QLatin1String("tool_use")) {
            const QJsonObject input = QJsonDocument::fromJson(m_curJson.toUtf8()).object();
            m_curBlock.insert(QStringLiteral("input"), input);
        }
        m_blocks.append(m_curBlock);
        m_curBlock = QJsonObject();
    } else if (type == QLatin1String("message_delta")) {
        const QString sr = obj.value(QStringLiteral("delta")).toObject().value(QStringLiteral("stop_reason")).toString();
        if (!sr.isEmpty()) {
            m_stopReason = sr;
        }
    } else if (type == QLatin1String("message_stop")) {
        finishTurn();
    } else if (type == QLatin1String("error")) {
        fail(obj.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(QStringLiteral("stream error")));
    }
}

void VibeCutAgent::onFinished()
{
    if (!m_reply) {
        return;
    }
    const QNetworkReply::NetworkError netErr = m_reply->error();
    const QString netErrString = m_reply->errorString();
    const QByteArray trailing = m_reply->readAll();
    if (!trailing.isEmpty()) {
        for (const SseParser::Event &ev : m_sse.feed(trailing)) {
            handleEvent(ev);
        }
    }

    m_reply->deleteLater();
    m_reply = nullptr;

    if (netErr != QNetworkReply::NoError && !m_turnFinished) {
        // Non-streamed error bodies (e.g. HTTP 400) carry the useful detail.
        QString detail = netErrString;
        const QJsonObject errObj = QJsonDocument::fromJson(trailing).object();
        const QString apiMsg = errObj.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        if (!apiMsg.isEmpty()) {
            detail = apiMsg;
        }
        fail(QStringLiteral("Request failed: %1").arg(detail));
        return;
    }
    if (!m_turnFinished) {
        // Stream ended without a message_stop event; finalise with what we have.
        finishTurn();
    }
}

void VibeCutAgent::finishTurn()
{
    if (m_turnFinished) {
        return;
    }
    m_turnFinished = true;

    const bool normalStop = m_stopReason == QLatin1String("end_turn") || m_stopReason.isEmpty();

    if (m_blocks.isEmpty() && normalStop && !m_retriedEmptyTurn) {
        // The model produced nothing whatsoever - no text, no tool call - on
        // what otherwise looks like a normal completion. Confirmed live: this
        // can happen on *any* turn of an exchange, not just the first one
        // before any tool ran - so retry regardless of tool history, once,
        // rather than only guarding the opening turn. Don't record an empty
        // assistant turn (it isn't valid history to replay anyway), and give
        // it one clean retry before giving up honestly.
        m_retriedEmptyTurn = true;
        qWarning().noquote() << QStringLiteral("[VibeCut] empty end_turn with no tool calls this exchange — retrying once (%1)")
                                     .arg(historyDiagnostic());
        Q_EMIT statusChanged(QStringLiteral("Retrying (no response)…"));
        startRequest();
        return;
    }

    if (!m_blocks.isEmpty()) {
        m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                      {QStringLiteral("content"), m_blocks}});
    }

    if (m_stopReason == QLatin1String("tool_use")) {
        m_anyToolCalledThisExchange = true;
        if (++m_toolTurns > kMaxToolTurns) {
            fail(QStringLiteral("Stopped after %1 tool turns.").arg(kMaxToolTurns));
            return;
        }
        QJsonArray toolResults;
        for (const QJsonValue &v : m_blocks) {
            const QJsonObject block = v.toObject();
            if (block.value(QStringLiteral("type")).toString() != QLatin1String("tool_use")) {
                continue;
            }
            const QString name = block.value(QStringLiteral("name")).toString();
            const QString id = block.value(QStringLiteral("id")).toString();
            const QJsonObject input = block.value(QStringLiteral("input")).toObject();
            Q_EMIT toolInvoked(name, QString::fromUtf8(compact(input)));

            const QJsonObject result = m_tools->invoke(name, input);
            Q_EMIT toolCompleted(name, QString::fromUtf8(compact(result)));
            if (!result.value(QStringLiteral("ok")).toBool()) {
                Q_EMIT toolFailed(name, result.value(QStringLiteral("error")).toString(QStringLiteral("unknown error")));
            }
            toolResults.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                                           {QStringLiteral("tool_use_id"), id},
                                           {QStringLiteral("content"), QString::fromUtf8(compact(result))}});
        }
        m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                      {QStringLiteral("content"), toolResults}});
        startRequest();
        return;
    }

    QString finalText;
    for (const QJsonValue &v : m_blocks) {
        const QJsonObject block = v.toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
            finalText += block.value(QStringLiteral("text")).toString();
        }
    }
    finalText = finalText.trimmed();

    // A turn can stop for a reason other than genuinely finishing (hitting
    // max_tokens mid-thought, a stop sequence, a paused turn) - those are not
    // "done", they're a truncation, and silently showing success for one
    // would repeat the exact bug this code used to have.
    if (m_stopReason != QLatin1String("end_turn") && !m_stopReason.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[VibeCut] turn ended with stop_reason=%1 (not end_turn), "
                                                "text=%2 blocks=%3 (%4)")
                                     .arg(m_stopReason, finalText.isEmpty() ? QStringLiteral("<empty>") : finalText,
                                          QString::fromUtf8(compact(QJsonObject{{QStringLiteral("blocks"), m_blocks}})),
                                          historyDiagnostic());
        fail(QStringLiteral("Turn ended unexpectedly (%1) instead of finishing normally.").arg(m_stopReason));
        return;
    }
    if (finalText.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[VibeCut] turn ended with empty text on end_turn (tool called this "
                                                "exchange: %1); blocks=%2 (%3)")
                                     .arg(m_anyToolCalledThisExchange ? QStringLiteral("yes") : QStringLiteral("no"),
                                          QString::fromUtf8(compact(QJsonObject{{QStringLiteral("blocks"), m_blocks}})),
                                          historyDiagnostic());
        if (!m_anyToolCalledThisExchange) {
            // No tool ever ran, and even the retry came back with nothing.
            // This is a genuine dead end, not a success - never call this
            // "Done."
            fail(QStringLiteral("The model didn't respond or take any action. Try again or rephrase."));
            return;
        }
        // A tool did run and its own result/failure was already shown above
        // this line in the panel (toolInvoked/toolFailed); the model just
        // didn't add closing narration. assistantMessage("") is the panel's
        // cue to show a plain "finished" marker, not a fresh claim of success.
    }
    Q_EMIT assistantMessage(finalText);
    Q_EMIT statusChanged(QStringLiteral("Ready"));
}

void VibeCutAgent::fail(const QString &message)
{
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
        r->deleteLater();
    }
    Q_EMIT errorOccurred(message);
    Q_EMIT statusChanged(QStringLiteral("Ready"));
}
