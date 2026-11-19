/*
 Copyright (C) 2026 TrenchBroom contributors

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include <QJsonArray>
#include <QJsonObject>

#include "McpTestUtils.h"
#include "mcp/JsonSchema.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "ui/CatchConfig.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

McpToolDef readTool(
  const QString& name,
  std::function<McpResult<QJsonObject>(McpCallContext&, const QJsonObject&)> run)
{
  return {
    .name = name,
    .title = name,
    .description = "test tool",
    .inputSchema =
      schema::object({{"count", schema::integer(0, 10)}, {"ids", schema::nodeIds(5)}}),
    .kind = McpToolKind::Read,
    .annotations = {.readOnly = true, .destructive = false, .idempotent = true},
    .allowedInBatch = true,
    .run = std::move(run),
  };
}

QJsonObject structured(const QJsonObject& callResult)
{
  return callResult.value("structuredContent").toObject();
}

QString errorCode(const QJsonObject& callResult)
{
  REQUIRE(callResult.value("isError").toBool());
  return structured(callResult).value("error").toObject().value("code").toString();
}

} // namespace

TEST_CASE("McpToolDispatcher")
{
  auto f = McpMapToolsFixture{};

  SECTION("add")
  {
    f.dispatcher.add(readTool("test_tool", [](auto&, const auto&) {
      return McpResult<QJsonObject>{QJsonObject{{"ok", true}}};
    }));

    const auto* spec = f.registry.find(u"test_tool");
    REQUIRE(spec != nullptr);
    CHECK(spec->title == QStringLiteral("test_tool"));
    CHECK(spec->annotations.readOnly);
    CHECK(mcp::usesOnlySupportedKeywords(spec->inputSchema));

    // the registry handler runs the tool through the dispatcher
    const auto result = spec->handler({});
    REQUIRE(result.is_success());
    CHECK(result.value().structured == QJsonObject{{"ok", true}});
  }

  SECTION("find")
  {
    CHECK(f.dispatcher.find(u"get_map_info") != nullptr);
    CHECK(f.dispatcher.find(u"get_map_info")->kind == McpToolKind::Read);
    CHECK(f.dispatcher.find(u"no_such_tool") == nullptr);
  }

  SECTION("tools")
  {
    const auto tools = f.dispatcher.tools();
    REQUIRE(tools.size() == f.registry.tools().size());
    for (size_t i = 0; i < tools.size(); ++i)
    {
      CHECK(tools[i]->name == f.registry.tools()[i].name);
      CHECK(mcp::usesOnlySupportedKeywords(tools[i]->inputSchema));
      CHECK(tools[i]->inputSchema.value("additionalProperties") == QJsonValue{false});
      CHECK_FALSE(tools[i]->description.isEmpty());
    }
  }

  SECTION("call")
  {
    SECTION("unknown tool")
    {
      CHECK(errorCode(f.dispatcher.call("no_such_tool", {})) == "invalid_argument");
    }

    SECTION("schema errors")
    {
      auto called = false;
      f.dispatcher.add(readTool("test_tool", [&](auto&, const auto&) {
        called = true;
        return McpResult<QJsonObject>{QJsonObject{}};
      }));

      const auto unknownArgument = f.error("test_tool", R"({"cuont":1})");
      CHECK(unknownArgument.value("code") == QJsonValue{"invalid_argument"});
      CHECK(
        unknownArgument.value("message")
        == QJsonValue{"unknown argument 'cuont'; allowed: count, ids"});
      CHECK(
        unknownArgument.value("details").toObject().value("allowed")
        == QJsonValue{QJsonArray{"count", "ids"}});
      CHECK(
        unknownArgument.value("details").toObject().value("pointer")
        == QJsonValue{"/cuont"});

      const auto outOfRange = f.error("test_tool", R"({"count":11})");
      CHECK(
        outOfRange.value("details").toObject().value("pointer") == QJsonValue{"/count"});

      const auto badId = f.error("test_tool", R"({"ids":["B1","bogus"]})");
      CHECK(badId.value("details").toObject().value("pointer") == QJsonValue{"/ids/1"});

      CHECK_FALSE(called);
      CHECK(f.providerCalls.empty());
    }

    SECTION("no document")
    {
      f.doc.reset();
      CHECK(f.err("get_map_info") == "no_document");
      CHECK(f.providerCalls == std::vector<McpToolKind>{McpToolKind::Read});
    }

    SECTION("busy editor")
    {
      f.busy =
        toolError("editor_busy", "a dialog is open", {}, {{"reason", "modal_dialog"}});
      const auto error = f.error("get_map_info");
      CHECK(error.value("code") == QJsonValue{"editor_busy"});
      CHECK(
        error.value("details").toObject().value("reason") == QJsonValue{"modal_dialog"});
    }

    SECTION("re-entrant calls")
    {
      auto inner = QJsonObject{};
      f.dispatcher.add(readTool("reentrant", [&](auto&, const auto&) {
        CHECK(f.dispatcher.isInCall());
        inner = f.dispatcher.call("get_map_info", {});
        return McpResult<QJsonObject>{QJsonObject{}};
      }));

      CHECK_FALSE(f.dispatcher.isInCall());
      f.ok("reentrant");
      CHECK_FALSE(f.dispatcher.isInCall());
      CHECK(errorCode(inner) == "editor_busy");
      CHECK(
        structured(inner).value("error").toObject().value("details").toObject().value(
          "reason")
        == QJsonValue{"reentrant_call"});

      // the next call works again
      f.ok("get_map_info");
    }

    SECTION("exceptions reset the call state")
    {
      f.dispatcher.add(
        readTool("throwing", [](auto&, const auto&) -> McpResult<QJsonObject> {
          throw std::runtime_error{"boom"};
        }));

      CHECK_THROWS_AS(f.dispatcher.call("throwing", {}), std::runtime_error);
      CHECK_FALSE(f.dispatcher.isInCall());
      f.ok("get_map_info");
    }

    SECTION("tool errors are passed through")
    {
      f.dispatcher.add(
        readTool("failing", [](auto&, const auto&) -> McpResult<QJsonObject> {
          return toolError("unknown_node", "B9 was never issued", "call find_nodes");
        }));
      const auto error = f.error("failing");
      CHECK(error.value("code") == QJsonValue{"unknown_node"});
      CHECK(error.value("hint") == QJsonValue{"call find_nodes"});
    }

    SECTION("the registry is reconciled before a call")
    {
      f.doc->nodes.disconnectForTesting();
      auto* brush = addBrush(f.map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});

      auto seenId = QString{};
      f.dispatcher.add(readTool("probe", [&](McpCallContext& context, const auto&) {
        seenId = context.doc.nodes.idOf(*brush);
        return McpResult<QJsonObject>{QJsonObject{}};
      }));
      f.ok("probe");
      CHECK(seenId == QStringLiteral("B2"));
      CHECK(f.doc->nodes.verifyBijection());
    }

    SECTION("warnings and extra content")
    {
      auto def = readTool("warning", [](McpCallContext& context, const auto&) {
        context.warnings.push_back("first");
        context.warnings.push_back("second");
        return McpResult<QJsonObject>{QJsonObject{{"value", 1}}};
      });
      def.extraContent = [](const QJsonObject& output) {
        return QJsonArray{QJsonObject{{"type", "text"}, {"text", compactJson(output)}}};
      };
      f.dispatcher.add(std::move(def));

      const auto result = f.call("warning");
      CHECK(
        structured(result).value("warnings")
        == QJsonValue{QJsonArray{"first", "second"}});
      const auto content = result.value("content").toArray();
      REQUIRE(content.size() == 2);
      CHECK(
        content[1].toObject().value("text")
        == QJsonValue{R"({"value":1,"warnings":["first","second"]})"});
    }

    SECTION("log capture")
    {
      auto lines = std::vector<McpLogLine>{};
      auto begun = 0;
      auto ended = 0;
      auto registry = mcp::ToolRegistry{};
      auto dispatcher = McpToolDispatcher{
        registry,
        [&](McpToolKind) -> McpResult<McpDocumentState*> { return f.doc.get(); },
        [&]() {
          ++begun;
          return &lines;
        },
        [&]() { ++ended; }};

      std::vector<McpLogLine>* seenLog = nullptr;
      dispatcher.add(readTool("probe", [&](McpCallContext& context, const auto&) {
        seenLog = context.log;
        return McpResult<QJsonObject>{QJsonObject{}};
      }));

      dispatcher.call("probe", {});
      CHECK(seenLog == &lines);
      CHECK(begun == 1);
      CHECK(ended == 1);

      // schema errors are reported before the log is captured
      dispatcher.call("probe", parseObject(R"({"bogus":1})"));
      CHECK(begun == 1);

      // errors from the provider still end the capture
      f.doc.reset();
      auto noDocument = McpToolDispatcher{
        registry,
        [&](McpToolKind) -> McpResult<McpDocumentState*> {
          return toolError("no_document", "none");
        },
        [&]() {
          ++begun;
          return &lines;
        },
        [&]() { ++ended; }};
      noDocument.add(readTool("probe2", [](auto&, const auto&) {
        return McpResult<QJsonObject>{QJsonObject{}};
      }));
      noDocument.call("probe2", {});
      CHECK(begun == 2);
      CHECK(ended == 2);
    }

    SECTION("app tools need no document")
    {
      f.doc.reset();
      f.dispatcher.add({
        .name = "app_tool",
        .title = "App Tool",
        .description = "test",
        .inputSchema = schema::object({}),
        .kind = McpToolKind::App,
        .runApp = [](const QJsonObject&) -> McpResult<QJsonObject> {
          return QJsonObject{{"games", QJsonArray{}}};
        },
      });

      CHECK(f.ok("app_tool") == QJsonObject{{"games", QJsonArray{}}});
      CHECK(f.providerCalls.empty());
    }

    SECTION("mutating tools are not supported yet")
    {
      f.dispatcher.add({
        .name = "mutate_tool",
        .title = "Mutate Tool",
        .description = "test",
        .inputSchema = schema::object({}),
        .kind = McpToolKind::Mutate,
        .run = [](auto&, const auto&) -> McpResult<QJsonObject> { return QJsonObject{}; },
      });
      CHECK(f.err("mutate_tool") == "not_supported");
    }
  }

  SECTION("isInCall")
  {
    CHECK_FALSE(f.dispatcher.isInCall());
  }

  SECTION("mcpToolErrorCodes")
  {
    CHECK(mcpToolErrorCodes().size() == 32);
    CHECK(isMcpToolErrorCode("stale_node"));
    CHECK(isMcpToolErrorCode("operation_failed"));
    CHECK_FALSE(isMcpToolErrorCode("bogus"));
  }
}

} // namespace tb::ui
