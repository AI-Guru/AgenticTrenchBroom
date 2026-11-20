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
#include "mdl/CommandProcessor.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/TransactionScope.h"
#include "ui/CatchConfig.h"
#include "ui/McpCheckpoints.h"
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

McpToolDef mutateTool(
  const QString& name,
  std::function<McpResult<QJsonObject>(McpCallContext&, const QJsonObject&)> run)
{
  auto title = name;
  title.replace('_', ' ');
  for (qsizetype i = 0; i < title.size(); ++i)
  {
    if (i == 0 || title[i - 1] == u' ')
    {
      title[i] = title[i].toUpper();
    }
  }

  return {
    .name = name,
    .title = title,
    .description = "test tool",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::Mutate,
    .annotations = {.readOnly = false, .destructive = false, .idempotent = false},
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

      lines.push_back({LogLevel::Warn, "careful"});
      dispatcher.call("probe", {});
      CHECK(seenLog == &lines);
      CHECK(begun == 1);
      CHECK(ended == 1);

      // the captured lines are kept for get_log
      REQUIRE(f.doc->logHistory.lines().size() == 1);
      CHECK(f.doc->logHistory.lines().front().text == "careful");

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

    SECTION("mutating tools run in one transaction")
    {
      populateTestScene(f);
      const auto modificationCount = f.map.modificationCount();
      const auto undoDepth = f.doc->history.undoStack().size();

      f.dispatcher.add(
        mutateTool("mutate_tool", [&](McpCallContext& context, const auto&) {
          CHECK(f.map.commandProcessor().isTransactionActive());
          CHECK_FALSE(f.map.isCommandCollationEnabled());
          addBrush(f.map, vm::bbox3d{{256, 0, 0}, {272, 16, 16}});
          addBrush(f.map, vm::bbox3d{{288, 0, 0}, {304, 16, 16}});
          context.warnings.push_back("a warning");
          return McpResult<QJsonObject>{QJsonObject{{"value", 1}}};
        }));

      const auto result = f.ok("mutate_tool");
      CHECK(result.value("value") == QJsonValue{1});
      CHECK(
        result.value("changes")
        == QJsonValue{QJsonObject{
          {"created", QJsonArray{"B12", "B13"}},
          {"removed", QJsonArray{}},
          {"modified", QJsonArray{}},
        }});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Mutate Tool"});
      CHECK(result.value("warnings") == QJsonValue{QJsonArray{"a warning"}});
      CHECK(result.value("log") == QJsonValue{QJsonArray{}});

      const auto nodes = result.value("nodes").toArray();
      REQUIRE(nodes.size() == 2);
      CHECK(nodes[0].toObject().value("id") == QJsonValue{"B12"});
      CHECK(nodes[1].toObject().value("id") == QJsonValue{"B13"});
      CHECK(result.value("nodes_truncated") == QJsonValue{false});

      const auto issues = result.value("issues").toObject();
      CHECK(issues.value("items").isArray());
      CHECK(issues.value("total").isDouble());

      CHECK(f.map.modificationCount() > modificationCount);
      CHECK(f.doc->history.undoStack().size() == undoDepth + 1);
      CHECK(*f.map.undoCommandName() == "MCP: Mutate Tool");
      CHECK_FALSE(f.map.commandProcessor().isTransactionActive());
      CHECK_FALSE(f.doc->nodes.isRecording());

      // one undo reverts the whole call
      f.map.undoCommand();
      CHECK(f.map.modificationCount() == modificationCount);
      CHECK(f.doc->history.undoStack().size() == undoDepth);
    }

    SECTION("a failing mutating tool is rolled back")
    {
      const auto scene = populateTestScene(f);
      mdl::selectNodes(f.map, {scene.light});
      const auto before = snapshot(f.map);
      const auto digest = contentDigest(f.doc->nodes, f.map);

      auto createdId = QString{};
      f.dispatcher.add(mutateTool("failing", [&](McpCallContext& context, const auto&) {
        auto* brush = addBrush(f.map, vm::bbox3d{{256, 0, 0}, {272, 16, 16}});
        createdId = context.doc.nodes.idOf(*brush);
        mdl::deselectAll(f.map);
        mdl::selectNodes(f.map, {brush});
        REQUIRE(mdl::translateSelection(f.map, {16, 0, 0}));
        mdl::removeNodes(f.map, {scene.wall});
        return McpResult<QJsonObject>{toolError("operation_failed", "it failed")};
      }));

      CHECK(f.err("failing") == "operation_failed");
      // the order of the layer's children may change because the wall is re-added
      const auto after = snapshot(f.map);
      CHECK(after.modificationCount == before.modificationCount);
      CHECK(after.selectedNodes == before.selectedNodes);
      CHECK(after.undoLabel == before.undoLabel);
      CHECK(after.redoLabel == before.redoLabel);
      CHECK(contentDigest(f.doc->nodes, f.map) == digest);
      CHECK_FALSE(f.map.commandProcessor().isTransactionActive());
      CHECK_FALSE(f.doc->nodes.isRecording());
      CHECK(f.doc->nodes.verifyBijection());

      // the brush created during the call is gone, the wall is back
      CHECK(
        f.doc->nodes.lookup(createdId).status == McpNodeRegistry::LookupStatus::Stale);
      CHECK(f.doc->nodes.isLive(scene.wall));
    }

    SECTION("exceptions roll back mutating tools")
    {
      populateTestScene(f);
      const auto before = snapshot(f.map);

      f.dispatcher.add(mutateTool(
        "throwing", [&](McpCallContext&, const auto&) -> McpResult<QJsonObject> {
          addBrush(f.map, vm::bbox3d{{256, 0, 0}, {272, 16, 16}});
          throw std::runtime_error{"boom"};
        }));

      CHECK_THROWS_AS(f.dispatcher.call("throwing", {}), std::runtime_error);
      CHECK(snapshot(f.map) == before);
      CHECK_FALSE(f.dispatcher.isInCall());
      CHECK_FALSE(f.doc->nodes.isRecording());
      CHECK(f.map.isCommandCollationEnabled() == false);
    }

    SECTION("the user's selection is restored")
    {
      const auto scene = populateTestScene(f);
      mdl::selectNodes(f.map, {scene.light, scene.wall});

      f.dispatcher.add(mutateTool("selecting", [&](McpCallContext&, const auto&) {
        mdl::deselectAll(f.map);
        mdl::selectNodes(f.map, {scene.patch});
        REQUIRE(mdl::translateSelection(f.map, {16, 0, 0}));
        mdl::removeNodes(f.map, {scene.light});
        return McpResult<QJsonObject>{QJsonObject{}};
      }));

      f.ok("selecting");
      // the removed light cannot be selected again
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.wall});

      // undo restores the light and the selection before the call
      f.map.undoCommand();
      auto selected = f.map.selection().nodes;
      CHECK(selected.size() == 2);
    }

    SECTION("tools that keep their selection")
    {
      const auto scene = populateTestScene(f);
      mdl::selectNodes(f.map, {scene.light});

      auto def = mutateTool("keeping", [&](McpCallContext&, const auto&) {
        mdl::deselectAll(f.map);
        mdl::selectNodes(f.map, {scene.wall});
        return McpResult<QJsonObject>{QJsonObject{}};
      });
      def.keepsSelection = true;
      f.dispatcher.add(std::move(def));

      f.ok("keeping");
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.wall});
    }

    SECTION("calls that change nothing add no undo step")
    {
      populateTestScene(f);
      const auto undoLabel = *f.map.undoCommandName();
      f.dispatcher.add(mutateTool("noop", [](McpCallContext&, const auto&) {
        return McpResult<QJsonObject>{QJsonObject{}};
      }));

      const auto result = f.ok("noop");
      CHECK(result.value("undo_label").isNull());
      CHECK(
        result.value("warnings")
        == QJsonValue{QJsonArray{"nothing changed; no undo step was added"}});
      CHECK(*f.map.undoCommandName() == undoLabel);
    }

    SECTION("tools do not leave repeatable actions on the user's repeat stack")
    {
      // the user moves a node; Edit > Repeat would move the selection again
      const auto scene = populateTestScene(f);
      mdl::selectNodes(f.map, {scene.light});
      REQUIRE(mdl::translateSelection(f.map, {16, 0, 0}));
      REQUIRE(f.map.canRepeatCommands());

      // a repeat after the agent's rotation must not rotate the user's selection
      f.ok(
        "transform_nodes", R"({"ids":["B2"],"rotate":{"axis":"z","angle_degrees":90}})");
      CHECK_FALSE(f.map.canRepeatCommands());

      f.ok("duplicate_nodes", R"({"ids":["B2"],"offset":[128,0,0]})");
      CHECK_FALSE(f.map.canRepeatCommands());

      f.ok(
        "batch",
        R"({"operations":[{"tool":"transform_nodes","arguments":{"ids":["B2"],"translate":[0,0,8]}}]})");
      CHECK_FALSE(f.map.canRepeatCommands());
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.light});
    }

    SECTION("collation is disabled during calls")
    {
      f.map.setIsCommandCollationEnabled(true);
      f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[16,16,16]}})");
      f.ok("create_box_brush", R"({"bounds":{"min":[32,0,0],"max":[48,16,16]}})");
      CHECK(f.map.isCommandCollationEnabled());

      // two calls are two undo steps although the GUI collates commands
      CHECK(*f.map.undoCommandName() == "MCP: Create Box Brush");
      f.map.undoCommand();
      CHECK(*f.map.undoCommandName() == "MCP: Create Box Brush");
      CHECK(f.doc->nodes.lookup(u"B2").status == McpNodeRegistry::LookupStatus::Live);
      f.map.undoCommand();
      CHECK(f.doc->nodes.lookup(u"B2").status == McpNodeRegistry::LookupStatus::Stale);
    }

    SECTION("mutating and history tools are refused during a transaction")
    {
      f.map.startTransaction("drag", mdl::TransactionScope::LongRunning);
      auto called = false;
      f.dispatcher.add(mutateTool("mutate_tool", [&](McpCallContext&, const auto&) {
        called = true;
        return McpResult<QJsonObject>{QJsonObject{}};
      }));

      const auto mutate = f.error("mutate_tool");
      CHECK(mutate.value("code") == QJsonValue{"editor_busy"});
      CHECK(
        mutate.value("details").toObject().value("reason")
        == QJsonValue{"transaction_active"});
      CHECK(f.err("undo") == "editor_busy");
      CHECK(f.err("redo") == "editor_busy");
      CHECK_FALSE(called);
      CHECK(f.map.commandProcessor().isTransactionActive());
      f.map.cancelTransaction();

      // the read tools still work
      f.ok("get_map_info");
    }

    SECTION("history tools run without a transaction")
    {
      populateTestScene(f);
      auto def = mutateTool("history_tool", [&](McpCallContext&, const auto&) {
        CHECK_FALSE(f.map.commandProcessor().isTransactionActive());
        CHECK_FALSE(f.map.isCommandCollationEnabled());
        f.map.undoCommand();
        return McpResult<QJsonObject>{QJsonObject{{"value", 1}}};
      });
      def.kind = McpToolKind::History;
      f.dispatcher.add(std::move(def));

      const auto result = f.ok("history_tool");
      CHECK(result.value("value") == QJsonValue{1});
      // undoing the creation of E11 removes it
      CHECK(
        result.value("changes").toObject().value("removed")
        == QJsonValue{QJsonArray{"E11"}});
      CHECK(f.map.redoCommandName() != nullptr);
    }

    SECTION("transaction invariants")
    {
      // every mutating tool: one undo step that undo reverts and redo repeats; the
      // failing variant changes nothing
      struct Row
      {
        const char* tool;
        const char* args;
        const char* failingArgs;
        const char* failingCode;
        /** An optional create_box_brush call that prepares the scene. */
        const char* setupBox = nullptr;
      };

      const auto rows = std::vector<Row>{
        {"create_box_brush",
         R"({"bounds":{"min":[256,0,0],"max":[320,64,64]},"material":"stone"})",
         R"({"bounds":{"min":[256,0,0],"max":[256,64,64]}})",
         "invalid_argument"},
        {"create_entity",
         R"({"classname":"light","origin":[0,0,256],"properties":{"light":200}})",
         R"({"classname":"light"})",
         "invalid_argument"},
        {"paste_map_text",
         R"({"text":"{\n\"classname\" \"info_null\"\n\"origin\" \"8 8 8\"\n}\n"})",
         R"({"text":"this is not a map"})",
         "parse_failed"},
        {"transform_nodes",
         R"({"ids":["B2","E3"],"translate":[16,0,0]})",
         R"({"ids":["B2","E3"],"translate":[100000,0,0]})",
         "out_of_world_bounds"},
        {"delete_nodes",
         R"({"ids":["B2","E4"]})",
         R"({"ids":["B2","L1"]})",
         "not_supported"},
        {"set_entity_properties",
         R"({"ids":["E3","E4"],"set":{"target":"t1"}})",
         R"({"ids":["E3","E4"],"remove":["light"]})",
         "invalid_argument"},
        {"set_face_attributes",
         R"({"faces":[{"brush":"B2","side":"+z"}],"material":"grass"})",
         R"({"faces":[{"brush":"B2","normal":[1,1,0]}],"material":"grass"})",
         "face_not_found"},
        {"set_face_attributes",
         R"({"brushes":["B2","G8"],"face_filter":{"facing":"up"},"material":"grass"})",
         R"({"brushes":["B2"],"face_filter":{"facing":[0,0,0]},"material":"grass"})",
         "invalid_argument"},
        {"create_hollow_box",
         R"({"inner_bounds":{"min":[256,0,0],"max":[512,256,128]},"wall_thickness":16,
             "group_name":"room"})",
         R"({"inner_bounds":{"min":[256,0,0],"max":[8128,256,128]},"wall_thickness":16})",
         "out_of_world_bounds"},
        {"create_primitive_brush",
         R"({"shape":"hollow_cylinder","bounds":{"min":[256,0,0],"max":[320,64,64]},
             "thickness":8})",
         R"({"shape":"hollow_cylinder","bounds":{"min":[256,0,0],"max":[320,64,64]}})",
         "invalid_argument"},
        {"create_brush_from_points",
         R"({"points":[[256,0,0],[320,0,0],[256,64,0],[256,0,64]]})",
         R"({"points":[[256,0,0],[320,0,0],[256,64,0],[320,64,0]]})",
         "geometry_invalid"},
        {"resize_nodes",
         R"({"ids":["B2","E3"],"bounds":{"min":[0,0,0],"max":[128,128,128]}})",
         R"({"ids":["B2","E3"],"bounds":{"min":[0,0,0],"max":[128,128,9000]}})",
         "out_of_world_bounds"},
        {"snap_vertices",
         R"({"ids":["B2"],"to":48})",
         R"({"ids":["B2","E3"],"to":48})",
         "wrong_node_type"},
        {"duplicate_nodes",
         R"({"ids":["B2","E4"],"offset":[0,256,0],"count":2})",
         R"({"ids":["B2","E4"],"offset":[0,4096,0],"count":2})",
         "out_of_world_bounds"},
        {"replace_material",
         R"({"from":"wall","to":"brick"})",
         R"({"from":"wall","to":"br ick"})",
         "invalid_argument"},
        {"csg",
         R"({"op":"merge","ids":["B2","B5"]})",
         R"({"op":"intersect","ids":["B2","B5"]})",
         "operation_failed"},
        {"csg_subtract",
         R"({"subtract_ids":["B12"]})",
         R"({"subtract_ids":["B12"],"from_ids":["B5"]})",
         "operation_failed",
         R"({"bounds":{"min":[32,32,32],"max":[96,96,96]}})"},
        {"group_nodes",
         R"({"ids":["B2","E3"],"name":"stuff"})",
         R"({"ids":["B2","L10"],"name":"stuff"})",
         "wrong_node_type"},
        {"ungroup_nodes",
         R"({"ids":["G8"]})",
         R"({"ids":["G8","B2"]})",
         "wrong_node_type"},
        {"create_layer", R"({"name":"new"})", R"({"name":""})", "invalid_argument"},
        {"move_to_layer",
         R"({"ids":["B2","E4"],"layer":"L10"})",
         R"({"ids":["E11"],"layer":"L10"})",
         "operation_failed"},
        {"clip_brush",
         R"({"id":"B2","plane":{"point":[32,0,0],"normal":[1,0,0]},"keep":"both"})",
         R"({"id":"B2","plane":{"point":[96,0,0],"normal":[1,0,0]}})",
         "operation_failed"},
        {"create_room",
         R"({"inner_bounds":{"min":[256,0,0],"max":[512,256,128]},
             "openings":[{"side":"+x","width":64,"height":96}],"group_name":"room"})",
         R"({"inner_bounds":{"min":[256,0,0],"max":[512,256,128]},
             "openings":[{"side":"+x","width":512,"height":96}]})",
         "invalid_argument"},
        {"create_stairs",
         R"({"start":[256,0,0],"direction":"+y","steps":4,"group_name":"stairs"})",
         R"({"start":[256,0,8000],"direction":"+y","steps":100})",
         "out_of_world_bounds"},
        {"drop_to_floor",
         R"({"ids":["E3","E4"]})",
         R"({"ids":["L1"]})",
         "wrong_node_type"},
        {"align_nodes",
         R"({"ids":["B2","E3"],"to":"E4","side":"+x","mode":"flush_outside"})",
         R"({"ids":["B2","E3"],"to":"B99","side":"+x"})",
         "unknown_node"},
        {"batch",
         R"({"operations":[
             {"tool":"create_box_brush","arguments":{"bounds":{"min":[256,0,0],"max":[320,64,64]}}},
             {"tool":"transform_nodes","arguments":{"ids":["$0","B2"],"translate":[0,0,64]}}]})",
         R"({"operations":[
             {"tool":"create_box_brush","arguments":{"bounds":{"min":[256,0,0],"max":[320,64,64]}}},
             {"tool":"transform_nodes","arguments":{"ids":["$0"],"translate":[0,0,90000]}}]})",
         "out_of_world_bounds"},
      };

      for (const auto& row : rows)
      {
        auto g = McpMapToolsFixture{};
        const auto scene = populateTestScene(g);
        CAPTURE(row.tool);
        if (row.setupBox)
        {
          g.ok("create_box_brush", row.setupBox);
        }
        mdl::selectNodes(g.map, {scene.patch});

        const auto d0 = contentDigest(g.doc->nodes, g.map);
        const auto selection0 = g.map.selection().nodes;
        const auto undoDepth = g.doc->history.undoStack().size();
        const auto modificationCount = g.map.modificationCount();

        // the failing variant changes nothing
        CHECK(g.err(row.tool, row.failingArgs) == row.failingCode);
        CHECK(contentDigest(g.doc->nodes, g.map) == d0);
        CHECK(g.map.modificationCount() == modificationCount);
        CHECK(g.map.selection().nodes == selection0);
        CHECK(g.doc->history.undoStack().size() == undoDepth);

        const auto result = g.ok(row.tool, row.args);
        const auto d1 = contentDigest(g.doc->nodes, g.map);
        CHECK(d1 != d0);
        CHECK(g.map.selection().nodes == selection0);
        CHECK(g.doc->history.undoStack().size() == undoDepth + 1);
        REQUIRE(g.map.undoCommandName() != nullptr);
        CHECK(McpHistoryTracker::isMcpLabel(*g.map.undoCommandName()));
        CHECK(
          result.value("undo_label")
          == QJsonValue{QString::fromStdString(*g.map.undoCommandName())});

        g.map.undoCommand();
        CHECK(contentDigest(g.doc->nodes, g.map) == d0);
        CHECK(g.map.selection().nodes == selection0);
        CHECK(g.map.modificationCount() == modificationCount);

        g.map.redoCommand();
        CHECK(contentDigest(g.doc->nodes, g.map) == d1);
        CHECK(g.doc->nodes.verifyBijection());
      }
    }
  }

  SECTION("addBatchTool")
  {
    const auto scene = populateTestScene(f);
    mdl::selectNodes(f.map, {scene.patch});
    const auto selection = f.map.selection().nodes;

    SECTION("placeholders refer to the ids created by earlier operations")
    {
      const auto modificationCount = f.map.modificationCount();
      const auto undoDepth = f.doc->history.undoStack().size();

      const auto result = f.ok("batch", R"({"operations":[
        {"tool":"create_box_brush","arguments":{"bounds":{"min":[256,0,0],"max":[320,64,64]}}},
        {"tool":"create_hollow_box","arguments":{"inner_bounds":{"min":[512,0,0],"max":[576,64,64]},
                                                 "wall_thickness":8}},
        {"tool":"create_entity","arguments":{"classname":"func_door","brush_ids":["$0","$1.5"]}},
        {"tool":"get_nodes","arguments":{"ids":["$2"]}}
      ]})");

      const auto results = result.value("results").toArray();
      REQUIRE(results.size() == 4);
      CHECK(results[0].toObject().value("tool") == QJsonValue{"create_box_brush"});
      CHECK(
        results[0].toObject().value("result").toObject().value("id")
        == QJsonValue{"B12"});
      CHECK(
        results[0].toObject().value("result").toObject().value("changes")
        == QJsonValue{QJsonObject{
          {"created", QJsonArray{"B12"}},
          {"removed", QJsonArray{}},
          {"modified", QJsonArray{}},
        }});
      CHECK(
        results[2].toObject().value("result").toObject().value("id")
        == QJsonValue{"E19"});

      // reading tools see the changes of the earlier operations
      const auto door = results[3]
                          .toObject()
                          .value("result")
                          .toObject()
                          .value("nodes")
                          .toArray()[0]
                          .toObject();
      CHECK(door.value("classname") == QJsonValue{"func_door"});
      CHECK(door.value("child_ids") == QJsonValue{QJsonArray{"B12", "B18"}});

      // one undo step for the whole batch, and the selection is restored
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Batch"});
      CHECK(f.doc->history.undoStack().size() == undoDepth + 1);
      CHECK(f.map.selection().nodes == selection);
      CHECK(result.value("changes").toObject().value("created").toArray().size() == 8);

      f.map.undoCommand();
      CHECK(f.map.modificationCount() == modificationCount);
      CHECK(f.err("get_nodes", R"({"ids":["E19"]})") == "stale_node");
    }

    SECTION("a failing operation rolls back everything")
    {
      const auto digest = contentDigest(f.doc->nodes, f.map);
      const auto modificationCount = f.map.modificationCount();
      const auto undoDepth = f.doc->history.undoStack().size();

      const auto error = f.error("batch", R"({"operations":[
        {"tool":"create_box_brush","arguments":{"bounds":{"min":[256,0,0],"max":[320,64,64]}}},
        {"tool":"delete_nodes","arguments":{"ids":["B2"]}},
        {"tool":"transform_nodes","arguments":{"ids":["$0"],"translate":[0,0,90000]}},
        {"tool":"create_box_brush","arguments":{"bounds":{"min":[512,0,0],"max":[576,64,64]}}}
      ]})");
      CHECK(error.value("code") == QJsonValue{"out_of_world_bounds"});
      const auto details = error.value("details").toObject();
      CHECK(details.value("failed_index") == QJsonValue{2});
      CHECK(details.value("tool") == QJsonValue{"transform_nodes"});
      CHECK(
        details.value("error").toObject().value("code")
        == QJsonValue{"out_of_world_bounds"});
      CHECK(details.value("results").toArray().size() == 2);
      CHECK(
        error.value("message").toString().startsWith("operation 2 (transform_nodes)"));

      CHECK(contentDigest(f.doc->nodes, f.map) == digest);
      CHECK(f.map.modificationCount() == modificationCount);
      CHECK(f.doc->history.undoStack().size() == undoDepth);
      CHECK(f.map.selection().nodes == selection);
      CHECK(f.ok("get_nodes", R"({"ids":["B2"]})").value("nodes").toArray().size() == 1);
      CHECK(f.doc->nodes.verifyBijection());
    }

    SECTION("every operation is checked before anything runs")
    {
      const auto before = snapshot(f.map);
      const auto check = [&](const char* args, const char* pointer) {
        CAPTURE(args);
        const auto error = f.error("batch", args);
        CHECK(error.value("code") == QJsonValue{"invalid_argument"});
        CHECK(error.value("details").toObject().value("pointer") == QJsonValue{pointer});
        CHECK(snapshot(f.map) == before);
      };

      check(
        R"({"operations":[{"tool":"create_box_brush","arguments":{"bounds":{"min":[0,0,0],"max":[8,8,8]}}},
                          {"tool":"create_box_brush","arguments":{"bounds":{"min":[0,0,0]}}}]})",
        "/operations/1/arguments/bounds/max");
      check(
        R"({"operations":[{"tool":"get_nodes","arguments":{"ids":["B2"]}},{"tool":"bogus"}]})",
        "/operations/1/tool");
      check(R"({"operations":[{"tool":"undo"}]})", "/operations/0/tool");
      check(R"({"operations":[{"tool":"batch"}]})", "/operations/0/tool");
      check(
        R"({"operations":[{"tool":"checkpoint_create","arguments":{"name":"a"}}]})",
        "/operations/0/tool");
      check(
        R"({"operations":[{"tool":"get_nodes","arguments":{"ids":["$0"]}}]})",
        "/operations/0/arguments/ids/0");
      check(
        R"({"operations":[{"tool":"get_nodes","arguments":{"ids":["B2"]}},
                          {"tool":"get_nodes","arguments":{"ids":["$2"]}}]})",
        "/operations/1/arguments/ids/0");
      check(R"({"operations":[]})", "/operations");
    }

    SECTION("placeholders out of range")
    {
      const auto digest = contentDigest(f.doc->nodes, f.map);
      const auto error = f.error("batch", R"({"operations":[
        {"tool":"create_box_brush","arguments":{"bounds":{"min":[256,0,0],"max":[320,64,64]}}},
        {"tool":"get_nodes","arguments":{"ids":["$0.1"]}}
      ]})");
      CHECK(error.value("code") == QJsonValue{"invalid_argument"});
      CHECK(error.value("details").toObject().value("failed_index") == QJsonValue{1});
      CHECK(contentDigest(f.doc->nodes, f.map) == digest);
    }

    SECTION("strings that only look like placeholders are kept")
    {
      const auto result = f.ok("batch", R"({"operations":[
        {"tool":"set_entity_properties","arguments":{"ids":["E3"],"set":{"message":"$x","cost":"$ 5"}}}
      ]})");
      const auto entity = result.value("results")
                            .toArray()[0]
                            .toObject()
                            .value("result")
                            .toObject()
                            .value("entities")
                            .toArray()[0]
                            .toObject();
      CHECK(compactJson(entity).contains(R"({"key":"message","value":"$x"})"));
      CHECK(compactJson(entity).contains(R"({"key":"cost","value":"$ 5"})"));
    }

    SECTION("selection changes are visible to later operations")
    {
      const auto result = f.ok("batch", R"({"operations":[
        {"tool":"select_nodes","arguments":{"ids":["B2"]}},
        {"tool":"transform_nodes","arguments":{"ids":["@selection"],"translate":[0,0,16]}}
      ]})");
      CHECK(
        result.value("results").toArray()[1].toObject().value("result").toObject().value(
          "bounds")
        == QJsonValue{
          QJsonObject{{"min", QJsonArray{0, 0, 16}}, {"max", QJsonArray{64, 64, 80}}}});
      // but the user's selection is restored afterwards
      CHECK(f.map.selection().nodes == selection);
    }

    SECTION("dry_run reports the results and leaves the map unchanged")
    {
      const auto before = snapshot(f.map);
      const auto text = f.ok("export_map_text").value("text");
      const auto undoDepth = f.doc->history.undoStack().size();

      const auto result = f.ok("batch", R"({"dry_run":true,"operations":[
        {"tool":"create_box_brush","arguments":{"bounds":{"min":[256,0,0],"max":[320,64,64]}}},
        {"tool":"transform_nodes","arguments":{"ids":["B2","E3"],"translate":[0,0,16]}},
        {"tool":"set_entity_properties","arguments":{"ids":["E3"],"set":{"light":500}}},
        {"tool":"create_entity","arguments":{"classname":"func_door","brush_ids":["$0"]}},
        {"tool":"get_map_stats"}
      ]})");
      CHECK(result.value("dry_run") == QJsonValue{true});
      CHECK(result.value("undo_label") == QJsonValue{});
      const auto results = result.value("results").toArray();
      REQUIRE(results.size() == 5);
      CHECK(
        results[0].toObject().value("result").toObject().value("id")
        == QJsonValue{"B12"});
      // later operations see the earlier ones
      CHECK(
        results[4].toObject().value("result").toObject().value("counts").toObject().value(
          "brush_entities")
        == QJsonValue{2});
      const auto changes = result.value("changes").toObject();
      CHECK(changes.value("created").toArray().contains(QJsonValue{"B12"}));
      CHECK(changes.value("modified").toArray().contains(QJsonValue{"B2"}));
      CHECK(result.value("issues").toObject().contains("items"));
      CHECK(compactJson(result.value("nodes")).contains(R"("classname":"func_door")"));

      // byte identical afterwards, with the same ids, selection and history
      CHECK(f.ok("export_map_text").value("text") == text);
      CHECK(snapshot(f.map) == before);
      CHECK(f.doc->history.undoStack().size() == undoDepth);
      CHECK(f.map.selection().nodes == selection);
      CHECK(f.err("get_nodes", R"({"ids":["B12"]})") == "stale_node");
      CHECK(f.doc->nodes.verifyBijection());
    }

    SECTION("dry_run restores deleted nodes with their ids")
    {
      const auto before = snapshot(f.map);
      const auto digest = contentDigest(f.doc->nodes, f.map);

      const auto result = f.ok("batch", R"({"dry_run":true,"operations":[
        {"tool":"delete_nodes","arguments":{"ids":["B2","E3"]}},
        {"tool":"get_map_stats"}
      ]})");
      CHECK(
        result.value("changes").toObject().value("removed")
        == QJsonValue{QJsonArray{"B2", "E3"}});

      // the rollback re-adds deleted nodes at the end of their parent, like undo, so
      // only the order of the nodes in the file may differ
      CHECK(contentDigest(f.doc->nodes, f.map) == digest);
      auto lines = [](const std::string& text) {
        auto sorted = QString::fromStdString(text).split('\n');
        sorted.sort();
        return sorted;
      };
      CHECK(lines(mapText(f.map)) == lines(before.text));
      CHECK(f.map.modificationCount() == before.modificationCount);
      CHECK(f.map.selection().nodes == selection);
      CHECK(f.id(*scene.wall) == "B2");
      CHECK(f.id(*scene.light) == "E3");
      CHECK(f.doc->nodes.verifyBijection());
    }

    SECTION("dry_run fails like a batch")
    {
      const auto before = snapshot(f.map);
      const auto error = f.error("batch", R"({"dry_run":true,"operations":[
        {"tool":"create_box_brush","arguments":{"bounds":{"min":[256,0,0],"max":[320,64,64]}}},
        {"tool":"delete_nodes","arguments":{"ids":["B99"]}}
      ]})");
      CHECK(error.value("code") == QJsonValue{"unknown_node"});
      CHECK(error.value("details").toObject().value("failed_index") == QJsonValue{1});
      CHECK(snapshot(f.map) == before);
      CHECK(
        f.err("batch", R"({"dry_run":1,"operations":[{"tool":"get_map_stats"}]})")
        == "invalid_argument");
    }

    SECTION("failed batches and dry runs restore the current material")
    {
      // selecting faces sets the current material, which is not undoable
      f.map.setCurrentMaterialName("current");
      const auto selectFace =
        R"({"tool":"select_nodes","arguments":{"faces":[{"brush":"B2","side":"+x"}]}})";

      CHECK(
        f.err(
          "batch",
          QString{
            R"({"operations":[%1,{"tool":"delete_nodes","arguments":{"ids":["B99"]}}]})"}
            .arg(selectFace)
            .toUtf8()
            .constData())
        == "unknown_node");
      CHECK(f.map.currentMaterialName() == "current");

      f.ok(
        "batch",
        QString{
          R"({"dry_run":true,"operations":[{"tool":"set_face_attributes","arguments":{"brushes":["B2"],"material":"rolled_back"}},%1]})"}
          .arg(selectFace)
          .toUtf8()
          .constData());
      CHECK(f.map.currentMaterialName() == "current");
      CHECK(f.map.selection().nodes == selection);
    }

    SECTION("warnings of operations are returned with their results")
    {
      const auto result = f.ok("batch", R"({"operations":[
        {"tool":"create_box_brush","arguments":{"bounds":{"min":[256,0,0],"max":[320.5,64,64]}}}
      ]})");
      CHECK(result.value("results")
              .toArray()[0]
              .toObject()
              .value("result")
              .toObject()
              .contains("warnings"));
    }
  }

  SECTION("isInCall")
  {
    CHECK_FALSE(f.dispatcher.isInCall());
  }

  SECTION("mcpToolErrorCodes")
  {
    CHECK(mcpToolErrorCodes().size() == 33);
    CHECK(isMcpToolErrorCode("history_mismatch"));
    CHECK(isMcpToolErrorCode("stale_node"));
    CHECK(isMcpToolErrorCode("operation_failed"));
    CHECK_FALSE(isMcpToolErrorCode("bogus"));
  }
}

} // namespace tb::ui
