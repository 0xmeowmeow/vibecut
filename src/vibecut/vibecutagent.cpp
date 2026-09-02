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
constexpr char kAnthropicEndpoint[] = "https://api.anthropic.com/v1/messages";
constexpr char kApiVersion[] = "2023-06-01";

// Matches vibecad's default model. The claude-api skill's house default is
// claude-opus-5; we deliberately track vibecad here. One constant to change.
constexpr char kAnthropicModel[] = "claude-sonnet-5";
constexpr char kOllamaDefaultModel[] = "qwen3.8:27b";
constexpr char kOllamaDefaultHost[] = "http://localhost:11434";

constexpr int kMaxTokens = 8192;

// Deliberately tiny, like vibecad's VIBECAD_SYSTEM_INSTRUCTIONS. Keep it well
// under a few KB; it is sent as a cached system block on every request.
const QString kSystemPrompt = QStringLiteral(
    "You are VibeCut, an assistant embedded in the Kdenlive video editor. You act on the user's live "
    "timeline through the provided tools only. Never invent clip ids or effect names: read the timeline "
    "or the selection first, then act. Prefer the current selection when the user does not name a clip. "
    "One piece of source media on the timeline is usually two linked clips, not one: a video-only clip and "
    "an audio-only mirror (an 'AV split'), each with its own id and effect stack — timeline_list_clips' "
    "`type` field (video_only/audio_only/av/disabled) tells you which is which. A user's 'this clip' means "
    "the whole visual unit, which may mean acting on both ids (e.g. one video effect + one audio effect), "
    "not just whichever id happens to come up first. "
    "Use ask_user only when the answer changes which clip or effect to touch. When a tool fails, report "
    "exactly what failed instead of guessing, and never tell the user something worked unless the tool "
    "result confirms it. effect_apply accepts 'denoise'/'denoise_light' as friendly aliases, or any real "
    "Kdenlive effect id — call effect_search first to find the right id for anything else the user asks for "
    "(color, contrast, transforms, speed, transitions, ...); never refuse a request just because it isn't "
    "denoise. Effects land with default parameter values, which for things like color levels/correction is "
    "an identity transform (visibly does nothing) — pass a parameters object to effect_apply to actually set "
    "values, don't just add the effect and call it done. Use the exact parameter names effect_search returns "
    "for that effect, never a guess from its display name — a wrong name (e.g. 'temperature' instead of the "
    "real 'av.temperature') is reported back as parameters_unknown, not applied silently. effect_apply reports "
    "already_present, effect_count_on_clip, and parameters_set/parameters_failed/parameters_unknown — say "
    "concretely what was added (or that it was already there), not just 'done'. There is no layout-switching "
    "tool — layout_list is read-only; if asked to change the UI layout, tell the user to do it themselves via "
    "Kdenlive's own layout switcher rather than offering to do it. For speech-to-text: call "
    "speech_status first; if not ready, call speech_setup yourself (it uses Kdenlive's own installer and "
    "runs in the background — tell the user a one-time confirmation dialog may appear) rather than telling "
    "the user to open Settings. A compound request (e.g. denoise AND subtitles) means do every part before "
    "stopping, not just the first. Never end a turn silently: if a tool result makes the next step "
    "ambiguous, call ask_user with the specific options instead of giving up with no text and no action. "
    "When a task genuinely finished, never close with a bare 'Done' - a flat done is meaningless on its own. "
    "End with one specific, contextual suggestion for what to do next given what's actually on the timeline "
    "now (e.g. after color-correcting a clip, suggest checking it in the monitor or doing the next clip; "
    "after subtitles, suggest reviewing them) - a real next step, not a generic 'let me know if you need "
    "anything else'. Keep replies short.");

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
    , m_systemPrompt(kSystemPrompt)
{
    // Backend choice is env-var only for now, deliberately: this is a
    // prototype to prove qwen3.8:27b via Ollama can stand in for Claude at
    // all before building the real settings-panel picker (TODO.md). Anthropic
    // stays the default so nothing changes for an unconfigured checkout.
    const QString backendEnv = qEnvironmentVariable("VIBECUT_BACKEND").trimmed().toLower();
    m_backend = (backendEnv == QLatin1String("ollama")) ? Backend::Ollama : Backend::Anthropic;

    if (m_backend == Backend::Ollama) {
        const QString modelEnv = qEnvironmentVariable("VIBECUT_MODEL").trimmed();
        m_model = modelEnv.isEmpty() ? QString::fromLatin1(kOllamaDefaultModel) : modelEnv;
        const QString hostEnv = qEnvironmentVariable("VIBECUT_OLLAMA_HOST").trimmed();
        m_ollamaHost = hostEnv.isEmpty() ? QString::fromLatin1(kOllamaDefaultHost) : hostEnv;
        bool ctxOk = false;
        const int ctxEnv = qEnvironmentVariable("VIBECUT_OLLAMA_NUM_CTX").trimmed().toInt(&ctxOk);
        if (ctxOk && ctxEnv > 0) {
            m_ollamaNumCtx = ctxEnv;
        }
        bool tempOk = false;
        const double tempEnv = qEnvironmentVariable("VIBECUT_OLLAMA_TEMPERATURE").trimmed().toDouble(&tempOk);
        if (tempOk && tempEnv >= 0.0) {
            m_ollamaTemperature = tempEnv;
        }
    } else {
        m_model = QString::fromLatin1(kAnthropicModel);
        m_apiKey = qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed();
    }

    connect(m_tools, &VibeCutTools::userQuestionRaised, this, &VibeCutAgent::userQuestionRaised);
    connect(m_tools, &VibeCutTools::backgroundProgress, this, &VibeCutAgent::backgroundProgress);
}

VibeCutAgent::~VibeCutAgent() = default;

bool VibeCutAgent::hasApiKey() const
{
    return m_backend == Backend::Ollama || !m_apiKey.isEmpty();
}

QString VibeCutAgent::notReadyMessage() const
{
    if (hasApiKey()) {
        return QString();
    }
    return QStringLiteral("Set ANTHROPIC_API_KEY in the environment and restart to use VibeCut.");
}

QString VibeCutAgent::modelLabel() const
{
    return m_backend == Backend::Ollama ? QStringLiteral("%1 (Ollama, local)").arg(m_model)
                                         : QStringLiteral("%1 (Anthropic)").arg(m_model);
}

void VibeCutAgent::sendUserMessage(const QString &text)
{
    if (m_reply) {
        fail(QStringLiteral("Still working on the previous message."));
        return;
    }
    if (!hasApiKey()) {
        fail(notReadyMessage());
        return;
    }
    m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                  {QStringLiteral("content"), text}});
    m_toolTurns = 0;
    m_anyToolCalledThisExchange = false;
    m_emptyTurnRetries = 0;
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
    m_emptyTurnRetries = 0;
    Q_EMIT statusChanged(QStringLiteral("Ready"));
}

void VibeCutAgent::resetStreamState()
{
    m_sse.reset();
    m_ndjsonBuf.clear();
    m_ollamaSawToolCallThisTurn = false;
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
    if (m_backend == Backend::Ollama) {
        startRequestOllama();
    } else {
        startRequestAnthropic();
    }
    Q_EMIT statusChanged(QStringLiteral("Thinking…"));
}

void VibeCutAgent::startRequestAnthropic()
{
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

    QNetworkRequest req{QUrl(QString::fromLatin1(kAnthropicEndpoint))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setRawHeader(QByteArrayLiteral("x-api-key"), m_apiKey.toUtf8());
    req.setRawHeader(QByteArrayLiteral("anthropic-version"), QByteArrayLiteral(kApiVersion));

    m_reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &VibeCutAgent::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &VibeCutAgent::onFinished);
}

QJsonArray VibeCutAgent::ollamaToolsFromSchemas(const QJsonArray &anthropicTools)
{
    // Anthropic shape: {name, description, input_schema}. Ollama/OpenAI
    // shape: {type:"function", function:{name, description, parameters}} -
    // same JSON-Schema object, just renamed and one level deeper.
    QJsonArray out;
    for (const QJsonValue &v : anthropicTools) {
        const QJsonObject t = v.toObject();
        QJsonObject fn{
            {QStringLiteral("name"), t.value(QStringLiteral("name"))},
            {QStringLiteral("description"), t.value(QStringLiteral("description"))},
            {QStringLiteral("parameters"), t.value(QStringLiteral("input_schema"))},
        };
        out.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("function"), fn}});
    }
    return out;
}

QJsonArray VibeCutAgent::ollamaMessagesFromHistory() const
{
    // m_messages is Anthropic-content-block-shaped regardless of backend (see
    // the header comment on this method) - translate to Ollama's flatter
    // {role, content, tool_calls} shape, one message per line here, with each
    // Anthropic tool_result block expanded into its own separate
    // {role:"tool", content:...} message (verified live: Ollama expects one
    // tool message per call, not a batch - see the curl probe in DEVLOG).
    QJsonArray out;
    out.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), m_systemPrompt}});
    for (const QJsonValue &mv : m_messages) {
        const QJsonObject msg = mv.toObject();
        const QString role = msg.value(QStringLiteral("role")).toString();
        const QJsonValue content = msg.value(QStringLiteral("content"));

        if (content.isString()) {
            // Plain user turn - already flat.
            out.append(QJsonObject{{QStringLiteral("role"), role}, {QStringLiteral("content"), content}});
            continue;
        }

        const QJsonArray blocks = content.toArray();
        if (role == QLatin1String("assistant")) {
            QString text;
            QJsonArray toolCalls;
            for (const QJsonValue &bv : blocks) {
                const QJsonObject b = bv.toObject();
                const QString bt = b.value(QStringLiteral("type")).toString();
                if (bt == QLatin1String("text")) {
                    text += b.value(QStringLiteral("text")).toString();
                } else if (bt == QLatin1String("tool_use")) {
                    QJsonObject fn{{QStringLiteral("name"), b.value(QStringLiteral("name"))},
                                   {QStringLiteral("arguments"), b.value(QStringLiteral("input"))}};
                    toolCalls.append(QJsonObject{{QStringLiteral("function"), fn}});
                }
                // thinking blocks are Anthropic-specific (carry a signature) -
                // deliberately dropped here, Ollama doesn't expect them replayed.
            }
            QJsonObject out_msg{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), text}};
            if (!toolCalls.isEmpty()) {
                out_msg.insert(QStringLiteral("tool_calls"), toolCalls);
            }
            out.append(out_msg);
        } else {
            // role == "user" carrying tool_result blocks.
            for (const QJsonValue &bv : blocks) {
                const QJsonObject b = bv.toObject();
                if (b.value(QStringLiteral("type")).toString() != QLatin1String("tool_result")) {
                    continue;
                }
                out.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                       {QStringLiteral("content"), b.value(QStringLiteral("content"))}});
            }
        }
    }
    return out;
}

void VibeCutAgent::startRequestOllama()
{
    m_ollamaSawToolCallThisTurn = false;
    QJsonObject body{
        {QStringLiteral("model"), m_model},
        {QStringLiteral("stream"), true},
        {QStringLiteral("messages"), ollamaMessagesFromHistory()},
        {QStringLiteral("tools"), ollamaToolsFromSchemas(m_tools->schemas())},
        // Ollama defaults num_ctx to 4096 regardless of what the model
        // actually supports (qwen3.8:27b's own metadata reports 262144) -
        // verified live: the uncapped, ever-growing m_messages history blew
        // past the 4096 default within a couple of exchanges. 32k is a
        // deliberately modest bump given the model already spills out of the
        // 16GB card at the smaller window (ollama ps showed 24%/76% CPU/GPU
        // split) - a bigger window means more KV-cache memory, i.e. more
        // CPU offload and slower turns, not a free win. Override with
        // VIBECUT_OLLAMA_NUM_CTX if that trade-off needs revisiting.
        // temperature is dropped from the model's own baked-in default (1.0,
        // per `ollama show qwen3.8:27b`) to 0.3 - less sampling randomness
        // means less chance of the early-stop-after-thinking behaviour that
        // was producing empty turns right after a tool result (see the
        // m_emptyTurnRetries reset above). A mitigation, not a proven full
        // fix - the retry-budget change is the backstop either way.
        {QStringLiteral("options"), QJsonObject{{QStringLiteral("num_ctx"), m_ollamaNumCtx},
                                                {QStringLiteral("temperature"), m_ollamaTemperature}}},
    };

    QNetworkRequest req{QUrl(m_ollamaHost + QStringLiteral("/api/chat"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));

    m_reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &VibeCutAgent::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &VibeCutAgent::onFinished);
}

void VibeCutAgent::onReadyRead()
{
    if (!m_reply) {
        return;
    }
    if (m_backend == Backend::Ollama) {
        // Ollama's stream is NDJSON, not SSE: one complete JSON object per
        // line, no "event:"/"data:" framing - buffer and split on '\n'
        // ourselves rather than reusing SseParser (which is Anthropic's
        // framing specifically).
        m_ndjsonBuf += m_reply->readAll();
        int nl;
        while ((nl = m_ndjsonBuf.indexOf('\n')) != -1) {
            const QByteArray line = m_ndjsonBuf.left(nl);
            m_ndjsonBuf.remove(0, nl + 1);
            if (!line.trimmed().isEmpty()) {
                handleOllamaLine(line);
            }
        }
        return;
    }
    const QList<SseParser::Event> events = m_sse.feed(m_reply->readAll());
    for (const SseParser::Event &ev : events) {
        handleEvent(ev);
    }
}

void VibeCutAgent::handleOllamaLine(const QByteArray &line)
{
    const QJsonObject obj = QJsonDocument::fromJson(line).object();
    const QJsonObject message = obj.value(QStringLiteral("message")).toObject();

    const QString textDelta = message.value(QStringLiteral("content")).toString();
    if (!textDelta.isEmpty()) {
        m_curText += textDelta;
        Q_EMIT assistantTextDelta(textDelta);
    }
    // thinking deltas: accumulated but not shown live, matching the
    // Anthropic path (thinking_delta isn't emitted as assistantTextDelta
    // there either).
    m_curThinking += message.value(QStringLiteral("thinking")).toString();

    const QJsonArray toolCalls = message.value(QStringLiteral("tool_calls")).toArray();
    for (const QJsonValue &tv : toolCalls) {
        // Verified live: Ollama emits each tool call whole in one line, no
        // incremental argument streaming the way Anthropic's
        // input_json_delta works - so this is a direct append, no
        // accumulator needed.
        const QJsonObject fn = tv.toObject().value(QStringLiteral("function")).toObject();
        const QString id = tv.toObject().value(QStringLiteral("id")).toString(QStringLiteral("call_%1").arg(m_blocks.size()));
        m_blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                                    {QStringLiteral("id"), id},
                                    {QStringLiteral("name"), fn.value(QStringLiteral("name"))},
                                    {QStringLiteral("input"), fn.value(QStringLiteral("arguments"))}});
        m_ollamaSawToolCallThisTurn = true;
    }

    if (obj.value(QStringLiteral("done")).toBool()) {
        if (!m_curText.isEmpty()) {
            m_blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), m_curText}});
        }
        m_stopReason = m_ollamaSawToolCallThisTurn ? QStringLiteral("tool_use") : QStringLiteral("end_turn");
        finishTurn();
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
    if (m_backend == Backend::Ollama) {
        m_ndjsonBuf += trailing;
        int nl;
        while ((nl = m_ndjsonBuf.indexOf('\n')) != -1) {
            const QByteArray line = m_ndjsonBuf.left(nl);
            m_ndjsonBuf.remove(0, nl + 1);
            if (!line.trimmed().isEmpty()) {
                handleOllamaLine(line);
            }
        }
        if (!m_ndjsonBuf.trimmed().isEmpty()) {
            // Stream ended without a trailing newline on the last line.
            handleOllamaLine(m_ndjsonBuf);
            m_ndjsonBuf.clear();
        }
    } else if (!trailing.isEmpty()) {
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

    if (m_blocks.isEmpty() && normalStop && m_emptyTurnRetries < kMaxEmptyTurnRetries) {
        // The model produced nothing whatsoever - no text, no tool call - on
        // what otherwise looks like a normal completion. Confirmed live: this
        // can happen more than once in the *same* exchange (a compound
        // request can hit it after its first tool call, then again after its
        // second) - retry with a small budget per exchange, not a single
        // one-shot allowance. Don't record an empty assistant turn (it isn't
        // valid history to replay anyway).
        ++m_emptyTurnRetries;
        qWarning().noquote() << QStringLiteral("[VibeCut] empty end_turn - retrying (%1/%2) (%3)")
                                     .arg(m_emptyTurnRetries)
                                     .arg(static_cast<int>(kMaxEmptyTurnRetries))
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
        // Real progress just happened (a tool actually ran) - give the next
        // request its own fresh empty-turn retry budget rather than making a
        // compound, multi-tool exchange share one small pool across every
        // step. Verified live against qwen3.8:27b/Ollama: the turn right
        // after a tool result is exactly where an empty response tends to
        // land (a reasoning model occasionally stops right after its
        // thinking closes, before committing to output) - a 3-tool-call
        // exchange was exhausting the old shared budget by its second step.
        m_emptyTurnRetries = 0;
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
