/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

/** @brief Native-mode tool surface exposed to the assistant.
 *
 * Modelled on vibecad's per-tool service contract: each tool has a JSON
 * Schema spec (name, description, input_schema) and a handler that returns
 * `{"ok": bool, ...}` — on failure `{"ok": false, "error": "..."}`.
 *
 * The surface is deliberately tiny for the first proof of concept: read the
 * timeline, read the selection, apply one allowlisted effect, ask the user a
 * question. It is meant to grow one reviewed entry at a time, never to become a
 * generic "run any command" bridge.
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

Q_SIGNALS:
    /** Emitted when the model calls the ask_user tool. */
    void userQuestionRaised(const QString &question);

private:
    QJsonObject toolListClips();
    QJsonObject toolGetSelection();
    QJsonObject toolApplyEffect(const QJsonObject &input);
    QJsonObject toolAskUser(const QJsonObject &input);
};
