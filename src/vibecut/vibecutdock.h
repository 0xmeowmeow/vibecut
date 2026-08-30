/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QMetaObject>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;
class VibeCutAgent;
class VibeCutTools;

/** @brief The VibeCut assistant dock: transcript + prompt line + quick actions.
 *
 * Owns the tool registry and the agent. Registered on the main window as a
 * KDDockWidgets panel, the same way Kdenlive registers the Library, Markers,
 * Speech Editor, etc.
 */
class VibeCutDock : public QWidget
{
    Q_OBJECT
public:
    explicit VibeCutDock(QWidget *parent = nullptr);

private Q_SLOTS:
    void submit();
    void runNoiseSuggestion();

private:
    void sendPrompt(const QString &text);
    void setBusyUi(bool busy);
    void appendLine(const QString &text, const QString &cssColor = QString());
    void cancelPendingSelection();

    QTextEdit *m_transcript;
    QLabel *m_status;
    QPushButton *m_suggestNoise;
    QLineEdit *m_input;
    QPushButton *m_send;

    VibeCutTools *m_tools;
    VibeCutAgent *m_agent;
    bool m_streamStarted = false;

    // "Apply as soon as a clip is selected" flow for the quick actions.
    QString m_pendingPrompt;
    bool m_awaitingSelection = false;
    QMetaObject::Connection m_selectionConn;
};
