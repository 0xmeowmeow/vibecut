/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "vibecuttools.h"

#include "assets/model/assetparametermodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "core.h"
#include "dialogs/subtitleedit.h"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "kdenlivesettings.h"
#include "mainwindow.h"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/timelinemodel.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include "mlt++/MltConsumer.h"
#include "mlt++/MltProfile.h"
#include "mlt++/MltTractor.h"

#include <KLocalizedString>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <QProcess>
#include <QReadLocker>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace {
// Friendly aliases for the effects people ask for by description rather than
// MLT/Kdenlive asset id. This used to be the *entire* guard rail (effect_apply
// rejected anything not in this map, and the tool schema's JSON-Schema `enum`
// locked the model to exactly these keys) - per DESIGN_SPECS.md §2 ("within
// things Kdenlive itself can already do, the tool surface should grow
// freely"), the real guard rail is now EffectsRepository::exists() - this map
// is just a convenience lookup for the couple of effects worth a friendly
// name, checked first, falling through to treating the input as a literal
// asset id otherwise. "denoise" is DeepFilterNet, a deep-learning speech
// denoiser bundled via the Flatpak manifest (deepfilternet-ladspa) — it
// handles non-stationary noise like a busy cafe, which the RNNoise-based
// "denoise_light" cannot.
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

// Readable label for PlaylistState::ClipState (src/definitions.h). Exposed to
// the model in timeline_list_clips so it has a grounded answer for "which
// clip is audio vs. video" instead of guessing from clip names and finding
// out the hard way from an effect_apply rejection - see the 2026-09-01
// DEVLOG entry for the live mislabelling this caused.
QString clipStateLabel(PlaylistState::ClipState state)
{
    switch (state) {
    case PlaylistState::VideoOnly:
        return QStringLiteral("video_only");
    case PlaylistState::AudioOnly:
        return QStringLiteral("audio_only");
    case PlaylistState::Disabled:
        return QStringLiteral("disabled");
    default:
        return QStringLiteral("av"); // has both video and audio (or type unknown)
    }
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

TimelineWidget *currentTimelineWidget()
{
    if (!pCore || !pCore->window()) {
        return nullptr;
    }
    return pCore->window()->getCurrentTimeline();
}

// Real, settable parameter names for an effect, straight from its XML
// definition - the same file Kdenlive's own effect-stack UI reads to build
// its sliders. Found live 2026-09-02: an effect's *display* name ("Color
// Temperature") is not its real MLT parameter key ("av.temperature", not
// "temperature") - guessing from the display name silently sets nothing
// (MLT just stores an unused property with the guessed name) while the
// real parameter stays at its default. This is what closes that gap:
// effect_search callers get the exact keys effect_apply's `parameters`
// input needs, instead of guessing - and effect_apply itself validates
// against this same list before writing anything (see toolApplyEffect).
QJsonArray effectParameters(const QString &assetId)
{
    QJsonArray params;
    const QDomElement xml = EffectsRepository::get()->getXml(assetId);
    QDomNodeList nodes = xml.elementsByTagName(QStringLiteral("parameter"));
    for (int i = 0; i < nodes.count(); ++i) {
        const QDomElement p = nodes.at(i).toElement();
        const QString type = p.attribute(QStringLiteral("type"));
        // "fixed"/"widget"/"hidden"-ish decorative entries don't take a real
        // value the way animated/constant/bool/list ones do - a name+default
        // is the useful signal either way, so no need to be exhaustive here.
        params.append(QJsonObject{{QStringLiteral("name"), p.attribute(QStringLiteral("name"))},
                                  {QStringLiteral("type"), type},
                                  {QStringLiteral("default"), p.attribute(QStringLiteral("default"))},
                                  {QStringLiteral("min"), p.attribute(QStringLiteral("min"))},
                                  {QStringLiteral("max"), p.attribute(QStringLiteral("max"))}});
    }
    return params;
}

// Whether an XML `type=` attribute needs the "start=value" keyframe-list
// format rather than a bare value - a general version of the "0=6500, not
// 6500" bug found live 2026-09-02 on avfilter.colortemperature's
// type="animated" parameter. Rather than guess at which of the ~40 distinct
// parameter types (`grep -ohE 'type="[a-zA-Z0-9_]+"' .../effects/*.xml`)
// need this, this mirrors AssetParameterModel::isAnimated()'s own type list
// exactly (src/assets/model/assetparametermodel.cpp) - that function and its
// sibling getDefaultKeyframes() are `protected`, unreachable from here, so
// this reproduces their effect via the XML type *strings* (which
// paramTypeFromStr() maps to the enum values isAnimated() checks) instead of
// the enum itself. Kept as one named list so the next person doesn't have to
// re-derive it: KeyframeParam ("keyframe"/"animated"), AnimatedFakePoint,
// AnimatedPoint, AnimatedRect ("animatedrect"/"rect"), AnimatedFakeRect,
// ColorWheel, Roto_spline ("roto-spline"), and - somewhat surprisingly -
// Color itself (plain "color" params are keyframable too in Kdenlive).
bool paramTypeNeedsKeyframePrefix(const QString &xmlType)
{
    static const QSet<QString> kNeedsPrefix = {
        QStringLiteral("keyframe"),          QStringLiteral("animated"),      QStringLiteral("animatedfakepoint"),
        QStringLiteral("animatedpoint"),     QStringLiteral("animatedrect"),  QStringLiteral("rect"),
        QStringLiteral("animatedfakerect"),  QStringLiteral("colorwheel"),    QStringLiteral("roto-spline"),
        QStringLiteral("color"),
    };
    return kNeedsPrefix.contains(xmlType);
}

struct TimelineGap {
    int trackId;
    int startFrame;
    int lengthFrames;
};

// Per-track gap detection, the same way a human reads the timeline: sort
// each track's clips by position, and anywhere the next one doesn't start
// right where the previous one ends, that's a gap. TrackModel's own
// isBlankAt()/getBlankStart()/getBlankEnd() would be more direct but are
// `protected` (see KDENLIVE_INTERNALS.md) - this is deliberately the more
// primitive, always-reachable signal. Not deduplicated across an AV-split
// pair's two tracks (they'll each report their own matching gap) -
// requestDeleteBlankAt's affectAllTracks handles keeping them in sync.
QVector<TimelineGap> findTimelineGaps(const std::shared_ptr<TimelineItemModel> &model, int onlyTrackId)
{
    QVector<TimelineGap> gaps;
    for (int tid : model->getAllTracksIds()) {
        if (onlyTrackId != -1 && tid != onlyTrackId) {
            continue;
        }
        QVector<QPair<int, int>> spans; // start, end
        for (int cid : model->getItemsInRange(tid, 0, -1, false)) {
            if (!model->isClip(cid)) {
                continue;
            }
            const int start = model->getClipPosition(cid);
            spans.append({start, start + model->getClipPlaytime(cid)});
        }
        std::sort(spans.begin(), spans.end());
        for (int i = 1; i < spans.size(); ++i) {
            const int prevEnd = spans.at(i - 1).second;
            const int nextStart = spans.at(i).first;
            if (nextStart > prevEnd) {
                gaps.append({tid, prevEnd, nextStart - prevEnd});
            }
        }
    }
    return gaps;
}

} // namespace

VibeCutTools::VibeCutTools(QObject *parent)
    : QObject(parent)
{
}

QString VibeCutTools::resolveEffectId(const QString &key)
{
    const QString alias = effectAllowlist().value(key).toString();
    if (!alias.isEmpty()) {
        return alias;
    }
    // Not a friendly alias - treat it as a literal Kdenlive/MLT asset id and
    // validate against the real repository (the same one Kdenlive's own
    // "Add Effect" panel is populated from), rather than rejecting anything
    // outside the old two-entry list.
    return EffectsRepository::get()->exists(key) ? key : QString();
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

int VibeCutTools::resolveTargetClip(const std::shared_ptr<TimelineItemModel> &model, const QJsonObject &input,
                                    const std::function<bool(int)> &isEligible, QString &error)
{
    if (input.contains(QStringLiteral("clip_id"))) {
        const int cid = input.value(QStringLiteral("clip_id")).toInt(-1);
        if (!model->isClip(cid)) {
            error = QStringLiteral("Clip id %1 does not exist on the timeline.").arg(cid);
            return -1;
        }
        return cid; // an explicit id is honored even if a later type check rejects it - that gives a clearer error
    }

    TimelineController *controller = currentController();
    const int selected = controller ? controller->getMainSelectedClip() : -1;
    if (selected != -1 && model->isClip(selected) && (!isEligible || isEligible(selected))) {
        return selected;
    }

    QList<int> candidates;
    for (int tid : model->getAllTracksIds()) {
        for (int cid : model->getItemsInRange(tid, 0, -1, false)) {
            if (model->isClip(cid) && (!isEligible || isEligible(cid))) {
                candidates.append(cid);
            }
        }
    }
    if (candidates.size() == 1) {
        // The common single-clip project: no need to force a selection.
        return candidates.first();
    }
    if (candidates.isEmpty()) {
        error = QStringLiteral("There are no eligible clips on the timeline.");
    } else {
        QStringList names;
        for (int cid : candidates) {
            names << QStringLiteral("%1 (%2)").arg(model->getClipName(cid)).arg(cid);
        }
        error = QStringLiteral("Nothing is selected and there are %1 candidate clips (%2) — ask the user which "
                               "one, or call timeline_list_clips for exact ids.")
                    .arg(candidates.size())
                    .arg(names.join(QStringLiteral(", ")));
    }
    return -1;
}

QJsonArray VibeCutTools::schemas() const
{
    const QJsonObject noArgs{{QStringLiteral("type"), QStringLiteral("object")},
                             {QStringLiteral("properties"), QJsonObject{}},
                             {QStringLiteral("additionalProperties"), false}};

    QJsonObject applyEffectSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("effect"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("description"),
                           QStringLiteral("A friendly alias ('denoise', 'denoise_light') or a real Kdenlive/MLT "
                                          "effect id (e.g. 'avfilter.curves', 'frei0r.coloradj_RGB') - use "
                                          "effect_search first if you don't already know the exact id.")}}},
             {QStringLiteral("clip_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                          {QStringLiteral("description"),
                           QStringLiteral("Timeline clip id from timeline_list_clips. Omit to use the current selection.")}}},
             {QStringLiteral("parameters"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                          {QStringLiteral("description"),
                           QStringLiteral("Optional MLT parameter name -> value to set on the effect right after "
                                          "adding it (or on one already present). Use the exact names from "
                                          "effect_search's `parameters` field for this effect id, not a guess from "
                                          "its display name (e.g. 'av.temperature', not 'temperature' - a wrong "
                                          "name is reported back as parameters_unknown, not applied). Values are "
                                          "strings even for numeric params (MLT convention) - e.g. "
                                          "{\"av.temperature\": \"4500\"}. Omit to add the effect with its default "
                                          "parameters.")}}},
         }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("effect")}},
        {QStringLiteral("additionalProperties"), false}};

    QJsonObject effectSearchSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("query"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("Case-insensitive substring to match against effect names and ids "
                                                  "(e.g. 'color', 'contrast', 'levels', 'crop'). Results are capped, "
                                                  "so narrow this rather than leaving it broad.")}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}},
        {QStringLiteral("additionalProperties"), false}};

    QJsonObject closeGapsSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("track_id"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("Only close gaps on this track id. Omit to close every gap on "
                                                  "every track.")}}}}},
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

    QJsonObject generateSubtitlesSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("clip_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                          {QStringLiteral("description"),
                           QStringLiteral("Scope transcription to this clip's span on the timeline. Omit to use the "
                                          "current selection if there is one, otherwise the whole project.")}}},
             {QStringLiteral("model"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("description"),
                           QStringLiteral("Which installed Whisper model to use. Omit to use whatever speech_status "
                                          "reports as installed.")}}},
         }},
        {QStringLiteral("additionalProperties"), false}};

    return QJsonArray{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("timeline_list_clips")},
                    {QStringLiteral("description"),
                     QStringLiteral("List every clip on the active timeline with its stable id, name, track id, "
                                    "start frame, duration in frames, bin id, and type (\"video_only\", "
                                    "\"audio_only\", \"av\", or \"disabled\") - use type to know which clips can "
                                    "host an audio vs. video effect before calling effect_apply, not guesswork.")},
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
        QJsonObject{{QStringLiteral("name"), QStringLiteral("effect_search")},
                    {QStringLiteral("description"),
                     QStringLiteral("Search Kdenlive's real effect repository (the same list its own 'Add Effect' "
                                    "panel uses) by name/id substring. Use this to find the exact id to pass to "
                                    "effect_apply for anything beyond the 'denoise'/'denoise_light' aliases - color "
                                    "correction, transforms, crop, speed, transitions, whatever the user asks for. "
                                    "Each result includes its real parameter names/defaults/ranges - use those exact "
                                    "names in effect_apply's `parameters`, not a guess from the display name (e.g. "
                                    "Color Temperature's real key is 'av.temperature', not 'temperature' - setting "
                                    "the wrong name silently does nothing, the effect stays at its default).")},
                    {QStringLiteral("input_schema"), effectSearchSchema}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("timeline_close_gaps")},
                    {QStringLiteral("description"),
                     QStringLiteral("Close every gap between clips on the timeline (or on one track if track_id is "
                                    "given), shifting later clips left so they touch. This is a real structural edit, "
                                    "not just an effect - it changes clip positions. Reports exactly which gaps were "
                                    "closed and which (if any) couldn't be, verified against the timeline's real "
                                    "state afterward, not just whether the call errored.")},
                    {QStringLiteral("input_schema"), closeGapsSchema}},
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
        QJsonObject{{QStringLiteral("name"), QStringLiteral("generate_subtitles")},
                    {QStringLiteral("description"),
                     QStringLiteral("Transcribe audio with Whisper and add the result as a subtitle track. Requires "
                                    "speech_status to report a model installed first - call speech_setup otherwise. "
                                    "Scope defaults to the current selection if there is one, otherwise the whole "
                                    "project - on a long timeline that can take minutes, so if nothing is selected "
                                    "and the user didn't say 'the whole thing'/'everything', check timeline_list_clips "
                                    "and ask which clip they mean rather than silently transcribing everything. Runs "
                                    "in the background; returns immediately once started, progress and the final "
                                    "result appear in the panel on their own.")},
                    {QStringLiteral("input_schema"), generateSubtitlesSchema}},
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
    if (name == QLatin1String("effect_search")) {
        return toolEffectSearch(input);
    }
    if (name == QLatin1String("timeline_close_gaps")) {
        return toolCloseGaps(input);
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
    if (name == QLatin1String("generate_subtitles")) {
        return toolGenerateSubtitles(input);
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
                {QStringLiteral("type"), clipStateLabel(model->getClipState(cid).first)},
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
        return err(QStringLiteral("'%1' isn't a known effect id or alias. Call effect_search to find the real "
                                   "Kdenlive/MLT id for what you're after.")
                       .arg(key));
    }

    // An AV-split video-only or audio-only twin can never host an effect of
    // the wrong kind - exclude it up front instead of attempting and failing
    // (which is what used to happen when this was audio-only and applied to
    // "both" clips of a split pair, one guaranteed to fail). Generalised from
    // the old audio-only assumption now that any real effect id is allowed:
    // ask the repository what kind this effect actually is.
    const bool isAudioFx = EffectsRepository::get()->isAudioEffect(assetId);
    auto isCompatible = [&model, isAudioFx](int cid) {
        const PlaylistState::ClipState state = model->getClipState(cid).first;
        return isAudioFx ? (state != PlaylistState::VideoOnly) : (state != PlaylistState::AudioOnly);
    };
    QString resolveError;
    const int clipId = resolveTargetClip(model, input, isCompatible, resolveError);
    if (clipId == -1) {
        return err(resolveError);
    }
    if (!isCompatible(clipId)) {
        return err(QStringLiteral("Clip %1 is %2-only and can't host %3 effect '%4'.")
                       .arg(clipId)
                       .arg(isAudioFx ? QStringLiteral("video") : QStringLiteral("audio"),
                            isAudioFx ? QStringLiteral("an audio") : QStringLiteral("a video"), key));
    }

    std::shared_ptr<EffectStackModel> stack = model->getClipEffectStack(clipId);
    if (!stack) {
        return err(QStringLiteral("Clip %1 has no effect stack.").arg(clipId));
    }

    // Setting parameters (added an effect but couldn't configure it: a real
    // gap found live 2026-09-01 - avfilter.colorlevels/colorcorrect landed
    // at their identity defaults and visibly did nothing). Applies to either
    // path below (already present, or freshly added): the caller may want to
    // (re)tune an effect that's already on the stack just as much as a new
    // one. Verified per-parameter by reading the value straight back from
    // the asset model, not just trusting setParameter() didn't throw.
    auto applyParameters = [&stack, &assetId](const QJsonObject &params) {
        QJsonObject confirmedParams;
        QJsonArray failedParams;
        QJsonArray unknownParams;
        if (params.isEmpty()) {
            return QJsonObject{{QStringLiteral("parameters_set"), confirmedParams}};
        }
        // Validate names against the effect's real XML parameter list before
        // writing anything - reading a value straight back after
        // setParameter() (the original check here) is NOT sufficient proof
        // it worked: MLT will happily store an arbitrary unknown property
        // name and read it back unchanged, which made a *wrong* guessed name
        // look like a confirmed success. Found live 2026-09-02: "temperature"
        // isn't a real parameter of avfilter.colortemperature ("av.temperature"
        // is) - the old check would have reported that as applied.
        QMap<QString, QString> realTypes; // name -> XML "type" attribute
        for (const QJsonValue &pv : effectParameters(assetId)) {
            const QJsonObject p = pv.toObject();
            realTypes.insert(p.value(QStringLiteral("name")).toString(), p.value(QStringLiteral("type")).toString());
        }
        std::shared_ptr<AssetParameterModel> asset = stack->getAssetModelById(assetId);
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (!realTypes.contains(it.key())) {
                unknownParams.append(it.key());
                continue;
            }
            QString value = it.value().toVariant().toString();
            // Keyframable parameter types store a "start=value" list, not a
            // bare value - every untouched default in the project XML looks
            // like "0=6500", never plain "6500". Found live 2026-09-02:
            // writing a bare value round-trips fine through getParam() (so
            // the old verification here reported it as confirmed) but MLT's
            // animation parser can't interpret it and silently falls back to
            // the built-in default at render time. General fix, not just for
            // "animated": paramTypeNeedsKeyframePrefix() mirrors
            // AssetParameterModel::isAnimated()'s exact type list. Frame 0 =
            // constant-across-the-clip in Kdenlive's own convention.
            if (paramTypeNeedsKeyframePrefix(realTypes.value(it.key())) && !value.contains(QLatin1Char('='))) {
                value = QStringLiteral("0=") + value;
            }
            if (!asset) {
                failedParams.append(it.key());
                continue;
            }
            asset->setParameter(it.key(), value);
            if (asset->getParam(it.key()) == value) {
                confirmedParams.insert(it.key(), value);
            } else {
                failedParams.append(it.key());
            }
        }
        QJsonObject result{{QStringLiteral("parameters_set"), confirmedParams}};
        if (!failedParams.isEmpty()) {
            result.insert(QStringLiteral("parameters_failed"), failedParams);
        }
        if (!unknownParams.isEmpty()) {
            result.insert(QStringLiteral("parameters_unknown"), unknownParams);
            result.insert(QStringLiteral("real_parameter_names"), QJsonArray::fromStringList(realTypes.keys()));
        }
        return result;
    };
    const QJsonObject paramResult = applyParameters(input.value(QStringLiteral("parameters")).toObject());

    if (stack->hasFilter(assetId)) {
        QJsonObject result{{QStringLiteral("ok"), true},
                           {QStringLiteral("applied"), key},
                           {QStringLiteral("asset_id"), assetId},
                           {QStringLiteral("clip_id"), clipId},
                           {QStringLiteral("already_present"), true}};
        for (auto it = paramResult.begin(); it != paramResult.end(); ++it) {
            result.insert(it.key(), it.value());
        }
        return result;
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
    // Parameters (if any were requested) must be applied *after* the effect
    // actually exists on the stack - re-run now that it does, rather than
    // relying on the pre-add attempt above (which would have found no asset
    // model yet and reported every parameter as failed).
    const QJsonObject postAddParamResult = applyParameters(input.value(QStringLiteral("parameters")).toObject());
    QJsonObject finalResult{{QStringLiteral("ok"), true},
                       {QStringLiteral("applied"), key},
                       {QStringLiteral("asset_id"), assetId},
                       {QStringLiteral("clip_id"), clipId},
                       {QStringLiteral("already_present"), false},
                       {QStringLiteral("effect_count_on_clip"), stack->rowCount()}};
    for (auto it = postAddParamResult.begin(); it != postAddParamResult.end(); ++it) {
        finalResult.insert(it.key(), it.value());
    }
    return finalResult;
}

QJsonObject VibeCutTools::toolEffectSearch(const QJsonObject &input)
{
    const QString query = input.value(QStringLiteral("query")).toString().trimmed();
    if (query.isEmpty()) {
        return err(QStringLiteral("query must not be empty - the full effect list is too large to dump unfiltered."));
    }

    // getNames() is (id, name) pairs straight from the same repository
    // Kdenlive's own "Add Effect" panel is populated from - not a hand-picked
    // subset, so this can genuinely find anything the app itself can do.
    const QVector<QPair<QString, QString>> all = EffectsRepository::get()->getNames();
    constexpr int kMaxResults = 30;
    QJsonArray results;
    for (const auto &[id, displayName] : all) {
        if (id.contains(query, Qt::CaseInsensitive) || displayName.contains(query, Qt::CaseInsensitive)) {
            results.append(QJsonObject{{QStringLiteral("id"), id},
                                       {QStringLiteral("name"), displayName},
                                       {QStringLiteral("is_audio"), EffectsRepository::get()->isAudioEffect(id)},
                                       {QStringLiteral("parameters"), effectParameters(id)}});
            if (results.size() >= kMaxResults) {
                break;
            }
        }
    }
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("query"), query},
                       {QStringLiteral("count"), results.size()},
                       {QStringLiteral("truncated"), results.size() >= kMaxResults},
                       {QStringLiteral("effects"), results}};
}

QJsonObject VibeCutTools::toolCloseGaps(const QJsonObject &input)
{
    std::shared_ptr<TimelineItemModel> model = currentModel();
    if (!model) {
        return err(QStringLiteral("No timeline is open."));
    }
    const int onlyTrackId = input.contains(QStringLiteral("track_id")) ? input.value(QStringLiteral("track_id")).toInt(-1) : -1;

    QJsonArray closedGaps;
    QJsonArray failedGaps;
    QSet<qint64> stuck; // (trackId, startFrame) pairs already confirmed unclosable this call

    // Bounded, not unbounded: a real project has a finite number of gaps: this
    // just guards against looping forever if something we haven't foreseen
    // keeps reporting a "new" gap at the same spot after a "successful" close.
    constexpr int kMaxIterations = 500;
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        // Closing a gap shifts every later clip left, so gaps found in a
        // previous iteration may already be stale - re-derive fresh each
        // time rather than computing the whole list once upfront (see
        // KDENLIVE_INTERNALS.md).
        QVector<TimelineGap> gaps = findTimelineGaps(model, onlyTrackId);
        QVector<TimelineGap> candidates;
        for (const TimelineGap &g : gaps) {
            if (!stuck.contains((qint64(g.trackId) << 32) | quint32(g.startFrame))) {
                candidates.append(g);
            }
        }
        if (candidates.isEmpty()) {
            break;
        }
        std::sort(candidates.begin(), candidates.end(), [](const TimelineGap &a, const TimelineGap &b) { return a.startFrame < b.startFrame; });
        const TimelineGap gap = candidates.first();

        // affectAllTracks=true is the one that keeps an AV-split video/audio
        // pair in sync (their gap shares position by construction), but it
        // requires *every* unlocked track to have a blank at that exact
        // position - it can legitimately fail on a project with other
        // unrelated tracks that don't. Fall back to closing it on just this
        // one track rather than giving up on the whole gap.
        bool closed = TimelineFunctions::requestDeleteBlankAt(model, gap.trackId, gap.startFrame, true);
        if (!closed) {
            closed = TimelineFunctions::requestDeleteBlankAt(model, gap.trackId, gap.startFrame, false);
        }
        QJsonObject gapJson{{QStringLiteral("track_id"), gap.trackId},
                            {QStringLiteral("start_frame"), gap.startFrame},
                            {QStringLiteral("length_frames"), gap.lengthFrames}};
        if (closed) {
            closedGaps.append(gapJson);
        } else {
            failedGaps.append(gapJson);
            stuck.insert((qint64(gap.trackId) << 32) | quint32(gap.startFrame));
        }
    }

    // Verify against real state, not our own loop bookkeeping - re-derive
    // gaps one final time rather than trusting the closedGaps count.
    const int remaining = findTimelineGaps(model, onlyTrackId).size();
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("closed"), closedGaps},
                       {QStringLiteral("failed"), failedGaps},
                       {QStringLiteral("gaps_remaining"), remaining}};
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

QString VibeCutTools::vibecutVenvDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/vibecut-whisper-venv");
}

QString VibeCutTools::vibecutVenvPython() const
{
    return vibecutVenvDir() + QStringLiteral("/bin/python3");
}

QString VibeCutTools::whisperScript(const QString &relativeName)
{
    // Same lookup Kdenlive's own AbstractPythonInterface uses for its bundled
    // scripts - we just call them as plain command-line tools instead of
    // going through its install/venv state machine.
    return QStandardPaths::locate(QStandardPaths::AppDataLocation, QStringLiteral("scripts/%1").arg(relativeName));
}

QString VibeCutTools::whisperRequirementsFile()
{
    return whisperScript(QStringLiteral("whisper/requirements-whisper.txt"));
}

QString VibeCutTools::whisperModelCacheDir()
{
    // Matches openai-whisper's own default download_root (~/.cache/whisper,
    // or $XDG_CACHE_HOME/whisper - set inside the flatpak sandbox) so models
    // downloaded via our venv are found the same way whisper itself would
    // find them.
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/whisper");
}

bool VibeCutTools::vibecutDepsReady() const
{
    const QString python = vibecutVenvPython();
    if (!QFile::exists(python)) {
        return false;
    }
    // Actually try to import the packages, not just check that python/pip
    // executables exist - that weaker check is exactly what let Kdenlive's
    // own checkSetup() report "Installed" for a venv with nothing installed
    // in it.
    QProcess check;
    check.start(python, {QStringLiteral("-c"), QStringLiteral("import whisper, torch")});
    if (!check.waitForStarted(3000)) {
        return false;
    }
    check.waitForFinished(10000);
    return check.exitStatus() == QProcess::NormalExit && check.exitCode() == 0;
}

bool VibeCutTools::vibecutCudaAvailable() const
{
    QProcess check;
    check.start(vibecutVenvPython(), {QStringLiteral("-c"), QStringLiteral("import torch, sys; sys.exit(0 if torch.cuda.is_available() else 1)")});
    if (!check.waitForStarted(3000)) {
        return false;
    }
    check.waitForFinished(15000);
    return check.exitStatus() == QProcess::NormalExit && check.exitCode() == 0;
}

QMap<QString, QString> VibeCutTools::whisperModelUrls() const
{
    QMap<QString, QString> result;
    const QString script = whisperScript(QStringLiteral("whisper/whisperquery.py"));
    if (script.isEmpty()) {
        return result;
    }
    QProcess proc;
    proc.start(vibecutVenvPython(), {script, QStringLiteral("task=list")});
    if (!proc.waitForStarted(3000)) {
        return result;
    }
    proc.waitForFinished(10000);
    const QString output = QString::fromUtf8(proc.readAllStandardOutput());
    for (const QString &line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int sep = line.indexOf(QStringLiteral(" : "));
        if (sep < 0) {
            continue;
        }
        const QString key = line.left(sep).trimmed();
        if (key == QStringLiteral("root_folder")) {
            continue; // last line of task=list output, not a model
        }
        result.insert(key, line.mid(sep + 3).trimmed());
    }
    return result;
}

bool VibeCutTools::whisperModelDownloaded(const QString &model, const QMap<QString, QString> &urls) const
{
    if (!urls.contains(model)) {
        return false;
    }
    const QString fileName = QFileInfo(urls.value(model)).fileName();
    return fileName.isEmpty() ? false : QFile::exists(whisperModelCacheDir() + QLatin1Char('/') + fileName);
}

void VibeCutTools::speechSetupFailed(const QString &message)
{
    m_speechStage = SpeechStage::Idle;
    m_pendingModel.clear();
    Q_EMIT backgroundProgress(message);
}

void VibeCutTools::beginCreateVenv()
{
    const QString python3 = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python3.isEmpty()) {
        speechSetupFailed(QStringLiteral("No system python3 found to create the Whisper environment."));
        return;
    }
    QDir().mkpath(QFileInfo(vibecutVenvDir()).absolutePath());
    m_speechStage = SpeechStage::CreatingVenv;
    Q_EMIT backgroundProgress(QStringLiteral("Creating a Python environment for Whisper (vibecut's own, separate from "
                                              "Kdenlive's installer)…"));

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, proc](int exitCode, QProcess::ExitStatus status) {
                const QString output = QString::fromUtf8(proc->readAll());
                proc->deleteLater();
                if (status != QProcess::NormalExit || exitCode != 0 || !QFile::exists(vibecutVenvPython())) {
                    speechSetupFailed(QStringLiteral("Could not create the Whisper Python environment: %1")
                                          .arg(output.trimmed().isEmpty() ? QStringLiteral("unknown error") : output.trimmed()));
                    return;
                }
                beginInstallDeps();
            });
    proc->start(python3, {QStringLiteral("-m"), QStringLiteral("venv"), vibecutVenvDir()});
}

void VibeCutTools::beginInstallDeps()
{
    const QString reqFile = whisperRequirementsFile();
    if (reqFile.isEmpty() || !QFile::exists(reqFile)) {
        speechSetupFailed(QStringLiteral("Could not find Kdenlive's bundled Whisper requirements file."));
        return;
    }
    m_speechStage = SpeechStage::InstallingDeps;
    Q_EMIT backgroundProgress(
        QStringLiteral("Installing Whisper's Python dependencies (includes PyTorch - can take a few minutes)…"));

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, proc](int exitCode, QProcess::ExitStatus status) {
                const QString output = QString::fromUtf8(proc->readAll());
                proc->deleteLater();
                if (status != QProcess::NormalExit || exitCode != 0 || !vibecutDepsReady()) {
                    const QString tail = output.right(2000).trimmed();
                    speechSetupFailed(QStringLiteral("Installing Whisper's dependencies failed: %1")
                                          .arg(tail.isEmpty() ? QStringLiteral("unknown error") : tail));
                    return;
                }
                beginDownloadModel(m_pendingModel);
            });
    proc->start(vibecutVenvPython(),
                {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("-r"), reqFile});
}

void VibeCutTools::beginDownloadModel(const QString &model)
{
    const QString script = whisperScript(QStringLiteral("whisper/whisperquery.py"));
    if (script.isEmpty()) {
        speechSetupFailed(QStringLiteral("Could not find Kdenlive's bundled Whisper download script."));
        return;
    }
    // whisperquery.py's task=download takes a literal url= and download_root=,
    // not model= - looked wrong here once already (assumed model= from the
    // old removed code without re-checking the script itself, and it failed
    // immediately with "Please give an url and a path"). The url has to come
    // from task=list (openai-whisper's own _MODELS table): several aliases
    // share one file (turbo and large-v3-turbo both resolve to
    // large-v3-turbo.pt), so hardcoding a naming convention would silently
    // mis-detect which models are actually already downloaded.
    const QMap<QString, QString> urls = whisperModelUrls();
    if (!urls.contains(model)) {
        speechSetupFailed(QStringLiteral("'%1' is not a known Whisper model name.").arg(model));
        return;
    }
    const QString url = urls.value(model);
    const QString cacheDir = whisperModelCacheDir();
    QDir().mkpath(cacheDir);
    const QString expectedFile = cacheDir + QLatin1Char('/') + QFileInfo(url).fileName();

    m_speechStage = SpeechStage::DownloadingModel;
    Q_EMIT backgroundProgress(QStringLiteral("Downloading the Whisper '%1' model…").arg(model));

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, proc, model, expectedFile](int exitCode, QProcess::ExitStatus status) {
                const QString output = QString::fromUtf8(proc->readAll());
                proc->deleteLater();
                m_speechStage = SpeechStage::Idle;
                m_pendingModel.clear();
                // Trust the file on disk, not the exit code alone - whisper's
                // own _download() can exit 0 having only partially written.
                if (status != QProcess::NormalExit || exitCode != 0 || !QFile::exists(expectedFile)) {
                    const QString tail = output.right(2000).trimmed();
                    Q_EMIT backgroundProgress(QStringLiteral("Downloading model '%1' failed: %2")
                                                  .arg(model, tail.isEmpty() ? QStringLiteral("unknown error") : tail));
                    return;
                }
                Q_EMIT backgroundProgress(QStringLiteral("✓ Whisper is ready with the '%1' model.").arg(model));
            });
    proc->start(vibecutVenvPython(),
                {script, QStringLiteral("task=download"), QStringLiteral("url=%1").arg(url), QStringLiteral("download_root=%1").arg(cacheDir)});
}

QJsonObject VibeCutTools::toolSpeechStatus()
{
    const bool ready = vibecutDepsReady();
    QJsonArray models;
    if (ready) {
        const QMap<QString, QString> urls = whisperModelUrls();
        for (auto it = urls.constBegin(); it != urls.constEnd(); ++it) {
            if (whisperModelDownloaded(it.key(), urls)) {
                models.append(it.key());
            }
        }
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("engine"), QStringLiteral("whisper")},
        {QStringLiteral("dependencies_installed"), ready},
        {QStringLiteral("models_installed"), models},
        {QStringLiteral("setup_in_progress"), m_speechStage != SpeechStage::Idle},
    };
}

QJsonObject VibeCutTools::toolSpeechSetup(const QJsonObject &input)
{
    const QString model = input.value(QStringLiteral("model")).toString(QStringLiteral("turbo"));

    if (vibecutDepsReady() && whisperModelDownloaded(model, whisperModelUrls())) {
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("already_installed"), true}, {QStringLiteral("model"), model}};
    }
    if (m_speechStage != SpeechStage::Idle) {
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("started"), false},
                           {QStringLiteral("note"), QStringLiteral("A setup for model '%1' is already in progress.").arg(m_pendingModel)}};
    }

    m_pendingModel = model;
    // Pick up wherever the environment already is: skip venv creation if one
    // exists, skip dep install if imports already succeed.
    if (!QFile::exists(vibecutVenvPython())) {
        beginCreateVenv();
    } else if (!vibecutDepsReady()) {
        beginInstallDeps();
    } else {
        beginDownloadModel(model);
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("started"), true},
        {QStringLiteral("model"), model},
        {QStringLiteral("note"), QStringLiteral("Setting up Whisper in the background, using vibecut's own Python "
                                                "environment (independent of Kdenlive's own installer). Progress "
                                                "appears in this panel on its own; call speech_status later to confirm.")},
    };
}

bool VibeCutTools::ensureSubtitleTrack(const std::shared_ptr<TimelineItemModel> &model)
{
    if (model->hasSubtitleModel()) {
        return true;
    }
    std::shared_ptr<SubtitleModel> subtitleModel = model->createSubtitleModel();
    if (!subtitleModel) {
        return false;
    }
    if (pCore->subtitleWidget()) {
        pCore->subtitleWidget()->setModel(subtitleModel);
    }
    KdenliveSettings::setShowSubtitles(true);
    if (TimelineWidget *tw = currentTimelineWidget()) {
        tw->connectSubtitleModel(true);
    }
    return true;
}

// Mirrors SpeechDialog::slotProcessSpeech()'s audio-export step (same known
// limitation noted there: this renders synchronously on the calling thread.
// It's audio-only so it's fast relative to the timeline's length, not a
// full re-encode - but a genuinely non-blocking render is future work).
QString VibeCutTools::exportZoneAudio(const std::shared_ptr<TimelineItemModel> &model, int zoneIn, int zoneOut, QString &error)
{
    QTemporaryFile tmpPlaylist(QDir::temp().absoluteFilePath(QStringLiteral("XXXXXX.mlt")));
    QTemporaryFile tmpAudio(QDir::temp().absoluteFilePath(QStringLiteral("XXXXXX.wav")));
    tmpPlaylist.setAutoRemove(false);
    tmpAudio.setAutoRemove(false);
    QString sceneList;
    QString audio;
    if (tmpPlaylist.open()) {
        sceneList = tmpPlaylist.fileName();
    }
    tmpPlaylist.close();
    if (tmpAudio.open()) {
        audio = tmpAudio.fileName();
    }
    tmpAudio.close();
    if (sceneList.isEmpty() || audio.isEmpty()) {
        error = QStringLiteral("Could not create temporary files for audio export.");
        return QString();
    }

    model->sceneList(QDir::temp().absolutePath(), sceneList);

    QReadLocker lock(&pCore->xmlMutex);
    Mlt::Producer producer(model->tractor()->get_profile(), "xml", sceneList.toUtf8().constData());
    if (!producer.is_valid()) {
        QFile::remove(sceneList);
        error = QStringLiteral("Could not build a render producer from the timeline.");
        return QString();
    }
    int tracksCount = model->tractor()->count();
    std::shared_ptr<Mlt::Service> s(new Mlt::Service(producer));
    std::shared_ptr<Mlt::Multitrack> multi = nullptr;
    bool multitrackFound = false;
    for (int i = 0; i < 10; i++) {
        s.reset(s->producer());
        if (s == nullptr || !s->is_valid()) {
            break;
        }
        if (s->type() == mlt_service_multitrack_type) {
            multi.reset(new Mlt::Multitrack(*s.get()));
            if (multi->count() == tracksCount) {
                multitrackFound = true;
                break;
            }
        }
    }
    if (multitrackFound) {
        // Mute video tracks only; keep every audio track for a full mixdown
        // (unlike SpeechDialog, which can target one specific track, this
        // tool transcribes "everything audible" by default).
        for (int i = 0; i < multi->count(); i++) {
            std::shared_ptr<Mlt::Producer> tk(multi->track(i));
            if (tk->get_int("hide") == 1) {
                tk->set("hide", 3);
            }
        }
    }

    Mlt::Consumer xmlConsumer(model->tractor()->get_profile(), "avformat", audio.toUtf8().constData());
    if (!xmlConsumer.is_valid()) {
        QFile::remove(sceneList);
        error = QStringLiteral("Could not create an audio export consumer.");
        return QString();
    }
    xmlConsumer.set("terminate_on_pause", 1);
    xmlConsumer.set("properties", "WAV");
    producer.set_in_and_out(zoneIn, zoneOut);
    xmlConsumer.connect(producer);
    xmlConsumer.run();

    QFile::remove(sceneList);

    if (!QFile::exists(audio) || QFileInfo(audio).size() == 0) {
        QFile::remove(audio);
        error = QStringLiteral("Audio export produced no output.");
        return QString();
    }
    return audio;
}

QJsonObject VibeCutTools::toolGenerateSubtitles(const QJsonObject &input)
{
    std::shared_ptr<TimelineItemModel> model = currentModel();
    if (!model) {
        return err(QStringLiteral("No timeline is open."));
    }
    if (m_subtitleJobRunning) {
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("started"), false},
                           {QStringLiteral("note"), QStringLiteral("A subtitle generation job is already running.")}};
    }

    if (!vibecutDepsReady()) {
        return err(QStringLiteral("Whisper is not set up yet. Call speech_setup first."));
    }
    const QMap<QString, QString> urls = whisperModelUrls();
    QStringList installed;
    for (auto it = urls.constBegin(); it != urls.constEnd(); ++it) {
        if (whisperModelDownloaded(it.key(), urls)) {
            installed << it.key();
        }
    }
    if (installed.isEmpty()) {
        return err(QStringLiteral("No Whisper model is installed yet. Call speech_setup first."));
    }
    QString useModel = input.value(QStringLiteral("model")).toString();
    if (useModel.isEmpty() || !installed.contains(useModel)) {
        useModel = installed.contains(QStringLiteral("turbo")) ? QStringLiteral("turbo") : installed.first();
    }

    int zoneIn = 0;
    int zoneOut = model->duration();
    int scopedClipId = -1;
    if (input.contains(QStringLiteral("clip_id"))) {
        scopedClipId = input.value(QStringLiteral("clip_id")).toInt(-1);
        if (!model->isClip(scopedClipId)) {
            return err(QStringLiteral("Clip id %1 does not exist on the timeline.").arg(scopedClipId));
        }
    } else {
        // No explicit clip_id: honour a real timeline selection instead of
        // silently defaulting to the whole project (an unhelpful, slow
        // surprise on a long timeline - see DEVLOG 2026-08-31). Unlike
        // effect_apply, "whole project" is a legitimate outcome here when
        // nothing is selected, so this doesn't use resolveTargetClip()'s
        // single-candidate/ambiguity-error behaviour - it only follows a
        // selection that's actually there.
        const int selected = selectedClipId();
        if (selected != -1) {
            scopedClipId = selected;
        }
    }
    if (scopedClipId != -1) {
        zoneIn = model->getClipPosition(scopedClipId);
        zoneOut = zoneIn + model->getClipPlaytime(scopedClipId);
    }
    if (zoneOut <= zoneIn) {
        return err(QStringLiteral("Nothing to transcribe (empty zone)."));
    }

    if (!ensureSubtitleTrack(model)) {
        return err(QStringLiteral("Could not create a subtitle track."));
    }

    QString exportError;
    const QString audioPath = exportZoneAudio(model, zoneIn, zoneOut, exportError);
    if (audioPath.isEmpty()) {
        return err(exportError.isEmpty() ? QStringLiteral("Audio export failed.") : exportError);
    }

    const QString script = whisperScript(QStringLiteral("whisper/whispertosrt.py"));
    if (script.isEmpty()) {
        QFile::remove(audioPath);
        return err(QStringLiteral("Could not find Kdenlive's bundled Whisper transcription script."));
    }

    const QString srtPath = QDir::temp().absoluteFilePath(QFileInfo(audioPath).completeBaseName() + QStringLiteral(".srt"));
    // Don't trust KdenliveSettings::whisperDevice(): it's a leftover from the
    // installer flow we bypassed, its kcfg default is literally "cpu", and
    // nothing in this chat panel ever offers a way to change it - passing it
    // through silently forced every transcription onto the CPU regardless of
    // the CUDA already verified working in the vibecut-owned venv. Probe the
    // real venv instead so the fast path is actually the one used.
    const bool cuda = vibecutCudaAvailable();
    QStringList arguments = {script, audioPath, useModel,
                             QStringLiteral("ffmpeg_path=%1").arg(KdenliveSettings::ffmpegpath()),
                             QStringLiteral("device=%1").arg(cuda ? QStringLiteral("cuda") : QStringLiteral("cpu"))};
    if (KdenliveSettings::whisperDisableFP16()) {
        arguments << QStringLiteral("fp16=False");
    }

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    m_subtitleJobRunning = true;
    Q_EMIT backgroundProgress(QStringLiteral("Transcribing with Whisper (%1, %2)… this can take a while for long audio.")
                                  .arg(useModel, cuda ? QStringLiteral("GPU") : QStringLiteral("CPU - no CUDA device found, this will be slow")));

    // finished() alone isn't enough: if start() fails outright (bad path,
    // not executable, ...) Qt never emits finished(), only errorOccurred().
    // That gap is exactly what left two previous runs stuck forever -
    // m_subtitleJobRunning wedged true with no failure ever surfaced and
    // their multi-hundred-MB exported wav files never cleaned up (found
    // sitting in the sandbox's tmp dir). Handle both signals and always
    // clear the flag + the temp audio file.
    auto finish = [this, proc, srtPath, audioPath, zoneIn](bool crashedOrFailed, const QString &failureReason) {
        m_subtitleJobRunning = false;
        QFile::remove(audioPath);
        if (crashedOrFailed) {
            Q_EMIT backgroundProgress(QStringLiteral("Subtitle generation failed: %1").arg(failureReason));
            proc->deleteLater();
            return;
        }
        if (!QFile::exists(srtPath)) {
            const QString errOut = QString::fromUtf8(proc->readAllStandardOutput());
            Q_EMIT backgroundProgress(QStringLiteral("Subtitle generation failed: %1")
                                          .arg(errOut.isEmpty() ? QStringLiteral("no output produced") : errOut));
            proc->deleteLater();
            return;
        }
        std::shared_ptr<TimelineItemModel> liveModel = currentModel();
        std::shared_ptr<SubtitleModel> subModel = liveModel ? liveModel->getSubtitleModel() : nullptr;
        if (subModel) {
            subModel->importSubtitle(srtPath, zoneIn, true);
            Q_EMIT backgroundProgress(QStringLiteral("✓ Subtitles imported from the Whisper transcription."));
        } else {
            Q_EMIT backgroundProgress(
                QStringLiteral("Transcription finished, but no subtitle track was found to import into."));
        }
        QFile::remove(srtPath);
        proc->deleteLater();
    };

    connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [finish, proc](int exitCode, QProcess::ExitStatus status) {
                if (status == QProcess::CrashExit) {
                    finish(true, QStringLiteral("the transcription process crashed"));
                    return;
                }
                if (exitCode != 0) {
                    const QString errOut = QString::fromUtf8(proc->readAllStandardOutput());
                    finish(true, errOut.isEmpty() ? QStringLiteral("exited with code %1").arg(exitCode) : errOut);
                    return;
                }
                finish(false, QString());
            });
    connect(proc, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(true, QStringLiteral("could not launch the Whisper environment's python3 - it may be missing or broken; "
                                        "call speech_setup again"));
        }
        // Other QProcess::ProcessError values (Crashed, Timedout, ReadError, WriteError, UnknownError)
        // are followed by finished() firing on its own - let that path report it, don't double-report.
    });

    proc->start(vibecutVenvPython(), arguments);

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("started"), true},
        {QStringLiteral("model"), useModel},
        {QStringLiteral("device"), cuda ? QStringLiteral("cuda") : QStringLiteral("cpu")},
        {QStringLiteral("note"), QStringLiteral("Transcribing in the background. Progress and the final result will "
                                                "appear in this panel on their own.")},
    };
}
