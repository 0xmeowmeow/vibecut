/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class TimelineItemModel;

/** @brief Native-mode tool surface exposed to the assistant.
 *
 * Modelled on vibecad's per-tool service contract: each tool has a JSON
 * Schema spec (name, description, input_schema) and a handler that returns
 * `{"ok": bool, ...}` — on failure `{"ok": false, "error": "..."}`.
 *
 * The goal is for the chat panel to be able to drive Kdenlive the way
 * Windsurf/Claude Code drive a codebase — broad real capability, not a
 * narrow menu bolted on one button at a time. Concretely: tools call
 * Kdenlive's own internal operations directly (including things like its own
 * Python/pip installer for optional features, see the speech_* tools) rather
 * than pointing the user at a Settings dialog. That's a different boundary
 * than a raw "run any shell command" bridge, which stays out of scope here
 * (that escape hatch belongs to VibeScript, not Native mode) — but within
 * "things Kdenlive itself can already do," the tool surface should grow
 * freely rather than being gatekept per capability.
 *
 * All handlers run on the GUI thread (the agent marshals calls here), so they
 * may touch pCore / the timeline model directly.
 */
class VibeCutTools : public QObject
{
    Q_OBJECT
public:
    explicit VibeCutTools(QObject *parent = nullptr);

    /** Tool definitions in Anthropic Messages API shape (`tools` array). */
    QJsonArray schemas() const;

    /** Dispatch @p name with @p input; always returns an object with "ok". */
    QJsonObject invoke(const QString &name, const QJsonObject &input);

    /** Friendly effect key -> Kdenlive/MLT asset id. The allowlist *is* the
     *  guard rail — the model cannot apply anything not listed here. */
    static QString resolveEffectId(const QString &key);

    /** Id of the currently selected timeline clip, or -1 if nothing (or no
     *  timeline). Used by the dock to gate suggestions on a valid selection. */
    int selectedClipId() const;

Q_SIGNALS:
    /** Emitted when the model calls the ask_user tool. */
    void userQuestionRaised(const QString &question);
    /** Out-of-band progress for a long-running background operation (speech
     *  setup, model download, ...). Not tied to any particular tool call /
     *  agent turn — the dock shows these live as they arrive. */
    void backgroundProgress(const QString &message);

private:
    QJsonObject toolListClips();
    QJsonObject toolGetSelection();
    QJsonObject toolApplyEffect(const QJsonObject &input);
    QJsonObject toolEffectSearch(const QJsonObject &input);
    QJsonObject toolAskUser(const QJsonObject &input);
    QJsonObject toolSpeechStatus();
    QJsonObject toolSpeechSetup(const QJsonObject &input);
    QJsonObject toolGenerateSubtitles(const QJsonObject &input);

    // --- Whisper: entirely vibecut-owned, independent of Kdenlive's own
    // Python-plugin install machinery (AbstractPythonInterface). That
    // machinery proved unreliable in practice (one crash, one silent
    // no-op, one documented setting that wasn't honored) after the actual
    // call-path bugs in it had already been found and fixed - so rather
    // than keep debugging someone else's state machine, this drives a
    // small, fully-owned environment directly. Reuses only the static,
    // stable parts of Kdenlive's own Whisper support: its bundled Python
    // scripts, called as plain command-line tools.
    QString vibecutVenvDir() const;
    QString vibecutVenvPython() const;
    static QString whisperScript(const QString &relativeName);
    static QString whisperRequirementsFile();
    static QString whisperModelCacheDir();
    bool vibecutDepsReady() const;
    /** Whether the venv's torch actually sees a CUDA device. Queried fresh
     *  each call (not cached) since it's cheap and this is the only thing
     *  standing between transcription silently running on CPU and running on
     *  the GPU it was verified to have - see the "device=cpu" bug in
     *  KDENLIVE_INTERNALS.md: KdenliveSettings::whisperDevice() defaults to
     *  the literal string "cpu" and our flow never offers a way to change
     *  it, so it must not be trusted here. */
    bool vibecutCudaAvailable() const;
    /** Model alias (e.g. "turbo") -> its real download URL, straight from
     *  whisperquery.py's own `task=list` (which reads openai-whisper's
     *  `_MODELS` table) rather than guessing at a `<model>.pt` filename
     *  convention - several aliases share one file (e.g. "turbo" and
     *  "large-v3-turbo" both download large-v3-turbo.pt), so the URL's
     *  basename, not the alias, is the real thing to check on disk. Requires
     *  the venv's whisper package to already be importable; returns an empty
     *  map otherwise. */
    QMap<QString, QString> whisperModelUrls() const;
    /** Whether @p model's backing file already exists in whisperModelCacheDir(). */
    bool whisperModelDownloaded(const QString &model, const QMap<QString, QString> &urls) const;
    void beginCreateVenv();
    void beginInstallDeps();
    void beginDownloadModel(const QString &model);
    void speechSetupFailed(const QString &message);

    bool ensureSubtitleTrack(const std::shared_ptr<TimelineItemModel> &model);
    QString exportZoneAudio(const std::shared_ptr<TimelineItemModel> &model, int zoneIn, int zoneOut, QString &error);

    /** Resolve which clip a tool should act on when the caller didn't name
     *  one: an explicit clip_id in @p input always wins; otherwise the
     *  current selection; otherwise, if exactly one clip satisfies
     *  @p isEligible (pass {} to accept any clip), that clip. Multiple
     *  eligible clips with nothing selected is real ambiguity, not
     *  something to guess at - returns -1 with @p error listing the
     *  candidates so the model can ask a specific question. */
    int resolveTargetClip(const std::shared_ptr<TimelineItemModel> &model, const QJsonObject &input,
                          const std::function<bool(int)> &isEligible, QString &error);

    enum class SpeechStage { Idle, CreatingVenv, InstallingDeps, DownloadingModel };
    SpeechStage m_speechStage = SpeechStage::Idle;
    QString m_pendingModel; // the model being set up while m_speechStage != Idle
    bool m_subtitleJobRunning = false;
};
