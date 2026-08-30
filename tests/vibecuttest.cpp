/*
    SPDX-FileCopyrightText: 2026 vibecut contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "catch.hpp"

#include "vibecut/sseparser.h"
#include "vibecut/vibecuttools.h"

#include <QJsonArray>
#include <QJsonObject>

TEST_CASE("SSE parser assembles events across arbitrary chunk boundaries", "[vibecut]")
{
    SECTION("a single event delivered whole")
    {
        SseParser p;
        auto events = p.feed(
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\n\n");
        REQUIRE(events.size() == 1);
        CHECK(events.first().name == QStringLiteral("content_block_delta"));
        CHECK(events.first().data.contains("\"text\":\"Hi\""));
    }

    SECTION("an event split mid-token across two feeds")
    {
        SseParser p;
        auto first = p.feed("event: message_st");
        CHECK(first.isEmpty());
        auto second = p.feed("op\ndata: {\"type\":\"message_stop\"}\n\n");
        REQUIRE(second.size() == 1);
        CHECK(second.first().name == QStringLiteral("message_stop"));
    }

    SECTION("two events in one chunk")
    {
        SseParser p;
        auto events = p.feed(
            "event: a\ndata: {\"x\":1}\n\n"
            "event: b\ndata: {\"y\":2}\n\n");
        REQUIRE(events.size() == 2);
        CHECK(events.at(0).name == QStringLiteral("a"));
        CHECK(events.at(1).name == QStringLiteral("b"));
    }

    SECTION("CRLF line endings are handled")
    {
        SseParser p;
        auto events = p.feed("event: ping\r\ndata: {}\r\n\r\n");
        REQUIRE(events.size() == 1);
        CHECK(events.first().name == QStringLiteral("ping"));
    }

    SECTION("multiple data: lines are concatenated with newlines")
    {
        SseParser p;
        auto events = p.feed("event: x\ndata: line1\ndata: line2\n\n");
        REQUIRE(events.size() == 1);
        CHECK(events.first().data == QByteArray("line1\nline2"));
    }

    SECTION("reset drops a buffered partial record")
    {
        SseParser p;
        p.feed("event: half");
        p.reset();
        auto events = p.feed("event: whole\ndata: {}\n\n");
        REQUIRE(events.size() == 1);
        CHECK(events.first().name == QStringLiteral("whole"));
    }
}

TEST_CASE("effect allowlist is the guard rail", "[vibecut]")
{
    CHECK(VibeCutTools::resolveEffectId(QStringLiteral("denoise")) == QStringLiteral("ladspa.7843795"));
    CHECK(VibeCutTools::resolveEffectId(QStringLiteral("denoise_light")) == QStringLiteral("ladspa.9354877"));
    CHECK(VibeCutTools::resolveEffectId(QStringLiteral("system.exec")).isEmpty());
    CHECK(VibeCutTools::resolveEffectId(QString()).isEmpty());
}

TEST_CASE("tool schemas are well formed for the Messages API", "[vibecut]")
{
    VibeCutTools tools;
    const QJsonArray schemas = tools.schemas();

    QStringList names;
    for (const QJsonValue &v : schemas) {
        const QJsonObject o = v.toObject();
        names << o.value(QStringLiteral("name")).toString();
        REQUIRE(o.contains(QStringLiteral("description")));
        const QJsonObject schema = o.value(QStringLiteral("input_schema")).toObject();
        CHECK(schema.value(QStringLiteral("type")).toString() == QStringLiteral("object"));
        CHECK(schema.contains(QStringLiteral("properties")));
    }
    CHECK(names.contains(QStringLiteral("timeline_list_clips")));
    CHECK(names.contains(QStringLiteral("timeline_get_selection")));
    CHECK(names.contains(QStringLiteral("effect_apply")));
    CHECK(names.contains(QStringLiteral("ask_user")));
}
