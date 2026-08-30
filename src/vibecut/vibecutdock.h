/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;
class VibeCutAgent;
class VibeCutTools;

/** @brief The VibeCut assistant dock: transcript + prompt line.
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

private:
    void appendLine(const QString &text, const QString &cssColor = QString());

    QTextEdit *m_transcript;
    QLabel *m_status;
    QLineEdit *m_input;
    QPushButton *m_send;

    VibeCutTools *m_tools;
    VibeCutAgent *m_agent;
    bool m_streamStarted = false;
};
