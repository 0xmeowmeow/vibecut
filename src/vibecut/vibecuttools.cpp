/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "vibecuttools.h"

#include "bin/model/subtitlemodel.hpp"
#include "core.h"
#include "dialogs/subtitleedit.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "kdenlivesettings.h"
#include "mainwindow.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/timelinemodel.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include "mlt++/MltConsumer.h"
#include "mlt++/MltProfile.h"
#include "mlt++/MltTractor.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QReadLocker>
#include <QStandardPaths>
#include <QTemporaryFile>

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

TimelineWidget *currentTimelineWidget()
{
    if (!pCore || !pCore->window()) {
        return nullptr;
    }
    return pCore->window()->getCurrentTimeline();
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

    QJsonObject generateSubtitlesSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("clip_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                          {QStringLiteral("description"),
                           QStringLiteral("Scope transcription to this clip's span on the timeline. Omit to "
                                          "transcribe the whole project.")}}},
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
        QJsonObject{{QStringLiteral("name"), QStringLiteral("generate_subtitles")},
                    {QStringLiteral("description"),
                     QStringLiteral("Transcribe audio with Whisper and add the result as a subtitle track. Requires "
                                    "speech_status to report a model installed first - call speech_setup otherwise. "
                                    "Runs in the background and can take a while for long clips; returns immediately "
                                    "once started, progress and the final result appear in the panel on their own.")},
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

    // effect_apply is currently audio-only, and an AV-split video-only twin
    // can never host an audio effect - exclude it up front instead of
    // attempting and failing on it (which is what used to happen: applying
    // to "both" clips of a split pair, one guaranteed to fail).
    auto hasAudio = [&model](int cid) { return model->getClipState(cid).first != PlaylistState::VideoOnly; };
    QString resolveError;
    const int clipId = resolveTargetClip(model, input, hasAudio, resolveError);
    if (clipId == -1) {
        return err(resolveError);
    }
    if (!hasAudio(clipId)) {
        return err(QStringLiteral("Clip %1 is video-only and can't host an audio effect.").arg(clipId));
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
    if (input.contains(QStringLiteral("clip_id"))) {
        const int clipId = input.value(QStringLiteral("clip_id")).toInt(-1);
        if (!model->isClip(clipId)) {
            return err(QStringLiteral("Clip id %1 does not exist on the timeline.").arg(clipId));
        }
        zoneIn = model->getClipPosition(clipId);
        zoneOut = zoneIn + model->getClipPlaytime(clipId);
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
    QStringList arguments = {script, audioPath, useModel,
                             QStringLiteral("ffmpeg_path=%1").arg(KdenliveSettings::ffmpegpath())};
    if (!KdenliveSettings::whisperDevice().isEmpty()) {
        arguments << QStringLiteral("device=%1").arg(KdenliveSettings::whisperDevice());
    }
    if (KdenliveSettings::whisperDisableFP16()) {
        arguments << QStringLiteral("fp16=False");
    }

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    m_subtitleJobRunning = true;
    Q_EMIT backgroundProgress(QStringLiteral("Transcribing with Whisper (%1)… this can take a while for long audio.").arg(useModel));

    connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, proc, srtPath, audioPath, zoneIn](int exitCode, QProcess::ExitStatus status) {
                m_subtitleJobRunning = false;
                QFile::remove(audioPath);
                if (status == QProcess::CrashExit) {
                    Q_EMIT backgroundProgress(QStringLiteral("Subtitle generation crashed."));
                    proc->deleteLater();
                    return;
                }
                if (exitCode != 0 || !QFile::exists(srtPath)) {
                    const QString errOut = QString::fromUtf8(proc->readAllStandardError());
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
            });

    proc->start(vibecutVenvPython(), arguments);

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("started"), true},
        {QStringLiteral("model"), useModel},
        {QStringLiteral("note"), QStringLiteral("Transcribing in the background. Progress and the final result will "
                                                "appear in this panel on their own.")},
    };
}
