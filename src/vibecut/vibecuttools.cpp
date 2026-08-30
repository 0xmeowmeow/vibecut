/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "vibecuttools.h"

#include "core.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "mainwindow.h"
#include "pythoninterfaces/abstractpythoninterface.h"
#include "pythoninterfaces/speechtotextwhisper.h"
#include "timeline2/model/timelinemodel.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include <KLocalizedString>

namespace {
// Allowlisted audio-cleanup effects. "denoise" is DeepFilterNet, a
// deep-learning speech denoiser bundled via the Flatpak manifest
// (deepfilternet-ladspa) — it handles non-stationary noise like a busy cafe,
// which the RNNoise-based "denoise_light" cannot.
QJsonObject effectAllowlist()
{
    return QJsonObject{
        {QStringLiteral("denoise"), QStringLiteral("ladspa.7843795")},
        {QStringLiteral("denoise_light"), QStringLiteral("ladspa.9354877")},
    };
}

QJsonObject err(const QString &message)
{
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

TimelineController *currentController()
{
    if (!pCore || !pCore->window()) {
        return nullptr;
    }
    TimelineWidget *tl = pCore->window()->getCurrentTimeline();
    return tl ? tl->controller() : nullptr;
}

std::shared_ptr<TimelineItemModel> currentModel()
{
    if (!pCore || !pCore->window()) {
        return nullptr;
    }
    TimelineWidget *tl = pCore->window()->getCurrentTimeline();
    return tl ? tl->model() : nullptr;
}
} // namespace

VibeCutTools::VibeCutTools(QObject *parent)
    : QObject(parent)
{
}

QString VibeCutTools::resolveEffectId(const QString &key)
{
    return effectAllowlist().value(key).toString();
}

int VibeCutTools::selectedClipId() const
{
    TimelineController *controller = currentController();
    std::shared_ptr<TimelineItemModel> model = currentModel();
    if (!controller || !model) {
        return -1;
    }
    const int cid = controller->getMainSelectedClip();
    return (cid != -1 && model->isClip(cid)) ? cid : -1;
}

QJsonArray VibeCutTools::schemas() const
{
    const QJsonObject noArgs{{QStringLiteral("type"), QStringLiteral("object")},
                             {QStringLiteral("properties"), QJsonObject{}},
                             {QStringLiteral("additionalProperties"), false}};

    QJsonArray effectKeys;
    for (const QString &k : effectAllowlist().keys()) {
        effectKeys.append(k);
    }

    QJsonObject applyEffectSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("effect"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("enum"), effectKeys},
                          {QStringLiteral("description"), QStringLiteral("Which allowlisted effect to add.")}}},
             {QStringLiteral("clip_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                          {QStringLiteral("description"),
                           QStringLiteral("Timeline clip id from timeline_list_clips. Omit to use the current selection.")}}},
         }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("effect")}},
        {QStringLiteral("additionalProperties"), false}};

    QJsonObject askUserSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("question"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"), QStringLiteral("The question to put to the user.")}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("question")}},
        {QStringLiteral("additionalProperties"), false}};

    QJsonObject speechSetupSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("model"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("Whisper model name (e.g. 'turbo', 'tiny', 'base', 'small', 'medium', "
                                                  "'large-v3'). Defaults to 'turbo', Kdenlive's own recommended "
                                                  "default (~1.4GB, good accuracy/speed balance).")}}}}},
        {QStringLiteral("additionalProperties"), false}};

    return QJsonArray{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("timeline_list_clips")},
                    {QStringLiteral("description"),
                     QStringLiteral("List every clip on the active timeline with its stable id, name, track id, "
                                    "start frame, duration in frames, and bin id.")},
                    {QStringLiteral("input_schema"), noArgs}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("timeline_get_selection")},
                    {QStringLiteral("description"),
                     QStringLiteral("Return the id of the currently selected timeline clip, or -1 if nothing is selected.")},
                    {QStringLiteral("input_schema"), noArgs}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("effect_apply")},
                    {QStringLiteral("description"),
                     QStringLiteral("Add an allowlisted effect to a timeline clip. For background-noise removal use "
                                    "'denoise' (DeepFilterNet, a deep-learning denoiser that handles real-world noise "
                                    "like a cafe, street or crowd); use 'denoise_light' only if the user asks for the "
                                    "lighter RNNoise filter or wants to preserve more room ambience.")},
                    {QStringLiteral("input_schema"), applyEffectSchema}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("ask_user")},
                    {QStringLiteral("description"),
                     QStringLiteral("Ask the user a clarifying question when an answer would change which clip or "
                                    "effect to act on. The user's reply arrives as their next message.")},
                    {QStringLiteral("input_schema"), askUserSchema}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("speech_status")},
                    {QStringLiteral("description"),
                     QStringLiteral("Report whether Whisper speech-to-text is installed and ready, which models are "
                                    "installed, and whether a setup is currently in progress. Call this before "
                                    "generating subtitles, and to check on a setup you previously started.")},
                    {QStringLiteral("input_schema"), noArgs}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("speech_setup")},
                    {QStringLiteral("description"),
                     QStringLiteral("Install Whisper speech-to-text using Kdenlive's own built-in Python/pip "
                                    "installer and download a model, in the background. Returns immediately once "
                                    "started; progress and completion show up in the panel on their own, and "
                                    "speech_status confirms when it's done. Kdenlive's installer shows one native "
                                    "confirmation dialog the first time (downloading model data requires network + "
                                    "disk) — tell the user to expect and accept it.")},
                    {QStringLiteral("input_schema"), speechSetupSchema}},
    };
}

QJsonObject VibeCutTools::invoke(const QString &name, const QJsonObject &input)
{
    if (name == QLatin1String("timeline_list_clips")) {
        return toolListClips();
    }
    if (name == QLatin1String("timeline_get_selection")) {
        return toolGetSelection();
    }
    if (name == QLatin1String("effect_apply")) {
        return toolApplyEffect(input);
    }
    if (name == QLatin1String("ask_user")) {
        return toolAskUser(input);
    }
    if (name == QLatin1String("speech_status")) {
        return toolSpeechStatus();
    }
    if (name == QLatin1String("speech_setup")) {
        return toolSpeechSetup(input);
    }
    return err(QStringLiteral("Unknown tool: %1").arg(name));
}

QJsonObject VibeCutTools::toolListClips()
{
    std::shared_ptr<TimelineItemModel> model = currentModel();
    if (!model) {
        return err(QStringLiteral("No timeline is open."));
    }
    QJsonArray clips;
    for (int tid : model->getAllTracksIds()) {
        for (int cid : model->getItemsInRange(tid, 0, -1, false)) {
            if (!model->isClip(cid)) {
                continue;
            }
            clips.append(QJsonObject{
                {QStringLiteral("id"), cid},
                {QStringLiteral("name"), model->getClipName(cid)},
                {QStringLiteral("track"), model->getClipTrackId(cid)},
                {QStringLiteral("position"), model->getClipPosition(cid)},
                {QStringLiteral("duration"), model->getClipPlaytime(cid)},
                {QStringLiteral("bin_id"), model->getClipBinId(cid)},
            });
        }
    }
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("clips"), clips}};
}

QJsonObject VibeCutTools::toolGetSelection()
{
    TimelineController *controller = currentController();
    if (!controller) {
        return err(QStringLiteral("No timeline is open."));
    }
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("selected_clip_id"), controller->getMainSelectedClip()}};
}

QJsonObject VibeCutTools::toolApplyEffect(const QJsonObject &input)
{
    TimelineController *controller = currentController();
    std::shared_ptr<TimelineItemModel> model = currentModel();
    if (!controller || !model) {
        return err(QStringLiteral("No timeline is open."));
    }

    const QString key = input.value(QStringLiteral("effect")).toString();
    const QString assetId = resolveEffectId(key);
    if (assetId.isEmpty()) {
        return err(QStringLiteral("Effect '%1' is not on the allowlist.").arg(key));
    }

    int clipId = -1;
    if (input.contains(QStringLiteral("clip_id"))) {
        clipId = input.value(QStringLiteral("clip_id")).toInt(-1);
    } else {
        clipId = controller->getMainSelectedClip();
        if (clipId == -1) {
            return err(QStringLiteral("No clip_id given and nothing is selected."));
        }
    }
    if (!model->isClip(clipId)) {
        return err(QStringLiteral("Clip id %1 does not exist on the timeline.").arg(clipId));
    }

    std::shared_ptr<EffectStackModel> stack = model->getClipEffectStack(clipId);
    if (!stack) {
        return err(QStringLiteral("Clip %1 has no effect stack.").arg(clipId));
    }
    if (stack->hasFilter(assetId)) {
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("applied"), key},
                           {QStringLiteral("asset_id"), assetId},
                           {QStringLiteral("clip_id"), clipId},
                           {QStringLiteral("already_present"), true}};
    }

    // addEffectToClip() itself returns void, so it cannot tell us whether
    // Kdenlive actually accepted the effect. Call the underlying model method
    // directly instead: it reports which clips it actually touched, and we
    // re-check the stack afterward so this tool never claims success it can't
    // back up.
    const QVariantList affected = model->addClipEffect(clipId, assetId);
    const bool confirmed = affected.contains(clipId) && stack->hasFilter(assetId);
    if (!confirmed) {
        return err(QStringLiteral("Kdenlive did not add '%1' to clip %2 — it never showed up on the "
                                   "clip's effect stack. The clip may not support an audio effect, or the "
                                   "LADSPA plugin failed to load.")
                       .arg(key)
                       .arg(clipId));
    }
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("applied"), key},
                       {QStringLiteral("asset_id"), assetId},
                       {QStringLiteral("clip_id"), clipId},
                       {QStringLiteral("already_present"), false},
                       {QStringLiteral("effect_count_on_clip"), stack->rowCount()}};
}

QJsonObject VibeCutTools::toolAskUser(const QJsonObject &input)
{
    const QString question = input.value(QStringLiteral("question")).toString();
    if (question.isEmpty()) {
        return err(QStringLiteral("question must not be empty."));
    }
    Q_EMIT userQuestionRaised(question);
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("note"), QStringLiteral("Question shown to the user; await their next message.")}};
}

SpeechToTextWhisper *VibeCutTools::whisperEngine()
{
    if (m_whisper) {
        return m_whisper;
    }
    m_whisper = new SpeechToTextWhisper(this);
    connect(m_whisper, &AbstractPythonInterface::dependenciesAvailable, this, [this]() {
        if (!m_pendingModel.isEmpty()) {
            continueSpeechSetup(m_pendingModel);
        }
    });
    connect(m_whisper, &AbstractPythonInterface::dependenciesMissing, this, [this](const QStringList &messages) {
        Q_EMIT backgroundProgress(QStringLiteral("Whisper setup incomplete: %1").arg(messages.join(QStringLiteral("; "))));
        m_pendingModel.clear();
    });
    connect(m_whisper, &AbstractPythonInterface::setupError, this, [this](const QString &message) {
        Q_EMIT backgroundProgress(QStringLiteral("Whisper setup error: %1").arg(message));
        m_pendingModel.clear();
    });
    connect(m_whisper, &AbstractPythonInterface::installFeedback, this,
            [this](const QString &message) { Q_EMIT backgroundProgress(message); });
    connect(m_whisper, &AbstractPythonInterface::concurrentScriptFinished, this, [this](const QString &script, const QStringList &args) {
        Q_UNUSED(script)
        if (args.contains(QStringLiteral("task=download"))) {
            Q_EMIT backgroundProgress(QStringLiteral("Model download finished. Call speech_status to confirm it installed correctly."));
            m_pendingModel.clear();
        }
    });
    return m_whisper;
}

void VibeCutTools::continueSpeechSetup(const QString &model)
{
    Q_EMIT backgroundProgress(QStringLiteral("Whisper is ready — downloading model '%1' now…").arg(model));
    whisperEngine()->runConcurrentScript(QStringLiteral("whisper/whisperquery.py"),
                                         {QStringLiteral("task=download"), QStringLiteral("model=%1").arg(model)}, true);
}

QJsonObject VibeCutTools::toolSpeechStatus()
{
    SpeechToTextWhisper *w = whisperEngine();
    QJsonArray models;
    for (const QString &m : w->getInstalledModels()) {
        models.append(m);
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("engine"), QStringLiteral("whisper")},
        {QStringLiteral("dependencies_installed"), w->status() == AbstractPythonInterface::Installed},
        {QStringLiteral("models_installed"), models},
        {QStringLiteral("setup_in_progress"), !m_pendingModel.isEmpty() || w->installInProcess()},
    };
}

QJsonObject VibeCutTools::toolSpeechSetup(const QJsonObject &input)
{
    SpeechToTextWhisper *w = whisperEngine();
    const QString model = input.value(QStringLiteral("model")).toString(QStringLiteral("turbo"));

    if (w->getInstalledModels().contains(model)) {
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("already_installed"), true}, {QStringLiteral("model"), model}};
    }
    if (!m_pendingModel.isEmpty()) {
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("started"), false},
                           {QStringLiteral("note"), QStringLiteral("A setup for model '%1' is already in progress.").arg(m_pendingModel)}};
    }

    m_pendingModel = model;
    if (w->status() == AbstractPythonInterface::Installed) {
        continueSpeechSetup(model);
    } else {
        Q_EMIT backgroundProgress(QStringLiteral("Setting up Whisper via Kdenlive's own installer — a confirmation "
                                                  "dialog may appear; please click Continue."));
        w->installMissingDependencies();
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("started"), true},
        {QStringLiteral("model"), model},
        {QStringLiteral("note"), QStringLiteral("Installing in the background via Kdenlive's own installer. Progress "
                                                "appears in this panel on its own; call speech_status later to confirm.")},
    };
}
