/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "vibecuttools.h"

#include "core.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "mainwindow.h"
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
