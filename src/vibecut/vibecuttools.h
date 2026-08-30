/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class SpeechToTextWhisper;

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
    QJsonObject toolAskUser(const QJsonObject &input);
    QJsonObject toolSpeechStatus();
    QJsonObject toolSpeechSetup(const QJsonObject &input);

    SpeechToTextWhisper *whisperEngine();
    void continueSpeechSetup(const QString &model);

    SpeechToTextWhisper *m_whisper = nullptr;
    QString m_pendingModel; // non-empty while waiting for deps before downloading a model
};
