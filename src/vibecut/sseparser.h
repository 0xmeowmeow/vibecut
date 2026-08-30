/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

/** @brief Incremental parser for a Server-Sent Events (SSE) byte stream.
 *
 * The Anthropic Messages API returns streamed responses as SSE: a sequence of
 * `event: <name>\n` / `data: <json>\n` line pairs separated by a blank line.
 * Network data arrives in arbitrary chunks, so feed() buffers a partial tail
 * and only emits events once their terminating blank line has been seen.
 *
 * Kept header-only and free of Qt Network / GUI dependencies so it can be
 * exercised directly from the unit tests.
 */
class SseParser
{
public:
    struct Event
    {
        QString name;  ///< the `event:` field, e.g. "content_block_delta"
        QByteArray data; ///< the concatenated `data:` payload (JSON, not yet parsed)
    };

    /** Feed a chunk of raw bytes; returns every complete event unlocked by it. */
    QList<Event> feed(const QByteArray &chunk)
    {
        QList<Event> events;
        m_buffer.append(chunk);

        int sep;
        // SSE record separator is a blank line. Normalise CRLF to LF first so
        // "\r\n\r\n" is handled too.
        m_buffer.replace("\r\n", "\n");
        while ((sep = m_buffer.indexOf("\n\n")) != -1) {
            const QByteArray record = m_buffer.left(sep);
            m_buffer.remove(0, sep + 2);
            Event ev = parseRecord(record);
            if (!ev.name.isEmpty() || !ev.data.isEmpty()) {
                events.append(ev);
            }
        }
        return events;
    }

    /** Drop any buffered partial record (call between requests). */
    void reset() { m_buffer.clear(); }

private:
    static Event parseRecord(const QByteArray &record)
    {
        Event ev;
        const QList<QByteArray> lines = record.split('\n');
        for (const QByteArray &line : lines) {
            if (line.startsWith("event:")) {
                ev.name = QString::fromUtf8(line.mid(6).trimmed());
            } else if (line.startsWith("data:")) {
                QByteArray chunk = line.mid(5);
                if (chunk.startsWith(' ')) {
                    chunk.remove(0, 1);
                }
                if (!ev.data.isEmpty()) {
                    ev.data.append('\n');
                }
                ev.data.append(chunk);
            }
            // ":" comment lines and unknown fields are ignored per the SSE spec.
        }
        return ev;
    }

    QByteArray m_buffer;
};
