/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "sseparser.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class VibeCutTools;

/** @brief Drives one conversation with the Anthropic Messages API.
 *
 * vibecad runs the provider SDK in a child process and bridges tool calls back
 * to the host over a pipe. Kdenlive has no Python layer, so the equivalent here
 * is a pure Qt client: QNetworkAccessManager streams `POST /v1/messages`
 * (Server-Sent Events), this class rebuilds the assistant message from the
 * stream, and on `stop_reason == "tool_use"` it runs the requested tools on the
 * GUI thread via VibeCutTools and feeds the results back — looping until the
 * model stops or a turn cap is hit.
 *
 * The network reply is event-driven (readyRead), so the whole thing lives on
 * the GUI thread without blocking it; no worker thread is needed.
 */
class VibeCutAgent : public QObject
{
    Q_OBJECT
public:
    explicit VibeCutAgent(VibeCutTools *tools, QObject *parent = nullptr);
    ~VibeCutAgent() override;

    /** True when ANTHROPIC_API_KEY was found in the environment. */
    bool hasApiKey() const;

    /** Whether a request/tool loop is currently in flight. */
    bool busy() const { return m_reply != nullptr; }

Q_SIGNALS:
    /** A user-visible status line ("Thinking…", "Ready", …). */
    void statusChanged(const QString &status);
    /** Live text delta as the model streams its reply. */
    void assistantTextDelta(const QString &text);
    /** The model's final natural-language reply for this turn. */
    void assistantMessage(const QString &text);
    /** A tool call is about to run (name + compact JSON of the arguments). */
    void toolInvoked(const QString &name, const QString &argsJson);
    /** The model called ask_user. */
    void userQuestionRaised(const QString &question);
    /** Any hard failure (no key, HTTP error, loop cap, …). */
    void errorOccurred(const QString &message);

public Q_SLOTS:
    /** Append a user message and start (or continue) the conversation. */
    void sendUserMessage(const QString &text);

private Q_SLOTS:
    void onReadyRead();
    void onFinished();

private:
    void startRequest();
    void handleEvent(const SseParser::Event &ev);
    void finishTurn();
    void fail(const QString &message);
    void resetStreamState();

    QNetworkAccessManager *m_nam;
    QNetworkReply *m_reply = nullptr;
    VibeCutTools *m_tools;
    SseParser m_sse;

    QString m_apiKey;
    QString m_model;
    QString m_systemPrompt;

    QJsonArray m_messages; ///< full conversation history sent every request

    // --- per-request stream accumulation ---
    QJsonArray m_blocks;       ///< assistant content blocks rebuilt from the stream
    QJsonObject m_curBlock;    ///< block currently being streamed
    QString m_curText;         ///< text_delta accumulator
    QString m_curThinking;     ///< thinking_delta accumulator
    QString m_curJson;         ///< input_json_delta accumulator (tool_use args)
    QString m_stopReason;
    bool m_turnFinished = false;
    int m_toolTurns = 0;

    enum { kMaxToolTurns = 8 };
};
