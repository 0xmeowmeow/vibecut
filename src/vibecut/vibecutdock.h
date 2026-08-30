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
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QUrl;
class VibeCutAgent;
class VibeCutTools;

/** @brief The VibeCut assistant dock: a chat transcript with inline suggestions.
 *
 * Owns the tool registry and the agent. Registered on the main window as a
 * KDDockWidgets panel, the same way Kdenlive registers the Library, Markers,
 * Speech Editor, etc.
 *
 * Follows the now-standard vibe-coding chat layout (Claude Code, Windsurf,
 * …): a welcome message with clickable suggestion links inline in the
 * transcript rather than separate buttons, tool activity narrated in plain
 * language as it happens, and an explicit busy/ready state (progress bar +
 * status line) so it's never ambiguous whether a request is still running.
 */
class VibeCutDock : public QWidget
{
    Q_OBJECT
public:
    explicit VibeCutDock(QWidget *parent = nullptr);

private Q_SLOTS:
    void submit();
    void onSuggestionClicked(const QUrl &url);

private:
    void appendWelcome();
    void runNoiseSuggestion();
    void sendPrompt(const QString &text);
    void setBusyUi(bool busy);
    void appendLine(const QString &text, const QString &cssColor = QString());
    void cancelPendingSelection();
    QString describeTool(const QString &name, const QString &argsJson) const;
    QString describeToolResult(const QString &name, const QString &resultJson) const;

    QTextBrowser *m_transcript;
    QLabel *m_status;
    QProgressBar *m_progress;
    QLineEdit *m_input;
    QPushButton *m_send;

    VibeCutTools *m_tools;
    VibeCutAgent *m_agent;
    bool m_streamStarted = false;

    // "Apply as soon as a clip is selected" flow for the denoise suggestion.
    QString m_pendingPrompt;
    bool m_awaitingSelection = false;
    QMetaObject::Connection m_selectionConn;
};
