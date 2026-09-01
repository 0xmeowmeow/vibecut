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

    /** True when the configured backend is usable: for Anthropic, a key was
     *  found in the environment; for Ollama, always true (no key needed) -
     *  see notReadyMessage() for what's actually missing when this is false. */
    bool hasApiKey() const;
    /** Empty when hasApiKey() is true; otherwise the reason to show the user. */
    QString notReadyMessage() const;
    /** "qwen3.8:27b (Ollama, local)" / "claude-sonnet-5 (Anthropic)" - which
     *  model is actually answering, for the welcome message. Backend choice
     *  is env-var only for now (VIBECUT_BACKEND=ollama, VIBECUT_MODEL=...,
     *  VIBECUT_OLLAMA_HOST=...) - a real settings panel is TODO.md tracked. */
    QString modelLabel() const;

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
    /** A tool call finished with {"ok": false, ...} — surfaced immediately so
     *  a failure is never silently swallowed by an empty final reply. */
    void toolFailed(const QString &name, const QString &errorMessage);
    /** A tool call finished, ok or not, with its full result. Lets the dock
     *  show what a read-only tool actually found even if the model never
     *  narrates it — the panel should never depend on the model choosing to
     *  report ground truth it already has. */
    void toolCompleted(const QString &name, const QString &resultJson);
    /** The model called ask_user. */
    void userQuestionRaised(const QString &question);
    /** Progress from a long-running background operation (speech setup,
     *  model download, ...), independent of any particular agent turn. */
    void backgroundProgress(const QString &message);
    /** Any hard failure (no key, HTTP error, loop cap, …). */
    void errorOccurred(const QString &message);

public Q_SLOTS:
    /** Append a user message and start (or continue) the conversation. */
    void sendUserMessage(const QString &text);
    /** Drop all history and start clean. Conversation history otherwise
     *  grows for as long as the panel/process lives with no cap - if a long
     *  session ever turns out to correlate with empty-turn failures, this is
     *  the workaround until real compaction exists. */
    void resetConversation();

private Q_SLOTS:
    void onReadyRead();
    void onFinished();

private:
    enum class Backend { Anthropic, Ollama };

    void startRequest();
    void startRequestAnthropic();
    void startRequestOllama();
    void handleEvent(const SseParser::Event &ev); ///< Anthropic SSE events
    void handleOllamaLine(const QByteArray &line); ///< one NDJSON line from Ollama's /api/chat
    void finishTurn();
    void fail(const QString &message);
    void resetStreamState();
    QString historyDiagnostic() const;

    /** m_messages stays Anthropic-content-block-shaped regardless of backend
     *  (single internal history format, one set of turn-management code) -
     *  these translate it to/from Ollama's flatter {role,content,tool_calls}
     *  shape only at the wire boundary. */
    static QJsonArray ollamaToolsFromSchemas(const QJsonArray &anthropicTools);
    QJsonArray ollamaMessagesFromHistory() const;

    QNetworkAccessManager *m_nam;
    QNetworkReply *m_reply = nullptr;
    VibeCutTools *m_tools;
    SseParser m_sse;
    QByteArray m_ndjsonBuf; ///< partial line buffer for the Ollama path

    Backend m_backend = Backend::Anthropic;
    QString m_ollamaHost;
    int m_ollamaNumCtx = 32768;
    double m_ollamaTemperature = 0.3;
    bool m_ollamaSawToolCallThisTurn = false;

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

    // --- per-exchange state (one user message through to a real reply) ---
    bool m_anyToolCalledThisExchange = false; ///< did any tool_use happen since the user's last message
    int m_emptyTurnRetries = 0;               ///< how many truly-empty responses we've retried this exchange

    enum { kMaxToolTurns = 8, kMaxEmptyTurnRetries = 2 };
};
