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
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Selection.h"
#include "mdl/TransactionScope.h"
#include "ui/CatchConfig.h"
#include "ui/McpCheckpoints.h"
#include "ui/McpPreconditions.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

QJsonObject details(const QJsonObject& error)
{
  return error.value("details").toObject();
}

} // namespace

TEST_CASE("McpMapHistoryTools")
{
  auto f = McpMapToolsFixture{};
  const auto scene = populateTestScene(f);

  const auto createBox = [&](const int x) {
    return f
      .ok(
        "create_box_brush",
        QJsonObject{
          {"bounds",
           QJsonObject{
             {"min", QJsonArray{x, 0, 256}}, {"max", QJsonArray{x + 16, 16, 272}}}}})
      .value("id")
      .toString();
  };

  SECTION("undo")
  {
    SECTION("undoes MCP steps")
    {
      const auto digest = contentDigest(f.doc->nodes, f.map);
      const auto id = createBox(0);
      f.ok(
        "transform_nodes",
        QJsonObject{{"ids", QJsonArray{id}}, {"translate", QJsonArray{16, 0, 0}}});

      const auto first = f.ok("undo");
      CHECK(first.value("undone") == QJsonValue{QJsonArray{"MCP: Transform Nodes"}});
      CHECK(first.value("can_undo") == QJsonValue{true});
      CHECK(first.value("undo_label") == QJsonValue{"MCP: Create Box Brush"});
      CHECK(first.value("can_redo") == QJsonValue{true});
      CHECK(first.value("redo_label") == QJsonValue{"MCP: Transform Nodes"});
      CHECK(first.value("changes")
              .toObject()
              .value("modified")
              .toArray()
              .contains(QJsonValue{id}));

      const auto second = f.ok("undo", R"({"expect_label":"MCP: Create Box Brush"})");
      CHECK(
        second.value("changes").toObject().value("removed")
        == QJsonValue{QJsonArray{id}});
      CHECK(contentDigest(f.doc->nodes, f.map) == digest);
      CHECK(f.err("get_nodes", QJsonObject{{"ids", QJsonArray{id}}}) == "stale_node");
    }

    SECTION("count")
    {
      createBox(0);
      createBox(32);
      createBox(64);
      const auto result = f.ok("undo", R"({"count":2})");
      CHECK(
        result.value("undone")
        == QJsonValue{QJsonArray{"MCP: Create Box Brush", "MCP: Create Box Brush"}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Box Brush"});
    }

    SECTION("foreign commands are protected")
    {
      createBox(0);
      mdl::selectNodes(f.map, {scene.wall});
      REQUIRE(mdl::translateSelection(f.map, {16, 0, 0}));
      const auto userLabel = *f.map.undoCommandName();

      const auto error = f.error("undo");
      CHECK(error.value("code") == QJsonValue{"foreign_command"});
      CHECK(
        details(error).value("undo_label")
        == QJsonValue{QString::fromStdString(userLabel)});
      CHECK(details(error).value("undone") == QJsonValue{QJsonArray{}});
      CHECK(*f.map.undoCommandName() == userLabel);

      const auto allowed = f.ok("undo", R"({"allow_user_commands":true})");
      CHECK(
        allowed.value("undone")
        == QJsonValue{QJsonArray{QString::fromStdString(userLabel)}});
    }

    SECTION("stops at foreign commands")
    {
      mdl::selectNodes(f.map, {scene.wall});
      REQUIRE(mdl::translateSelection(f.map, {16, 0, 0}));
      mdl::deselectAll(f.map);
      createBox(0);

      const auto error = f.error("undo", R"({"count":3})");
      CHECK(error.value("code") == QJsonValue{"foreign_command"});
      CHECK(
        details(error).value("undone")
        == QJsonValue{QJsonArray{"MCP: Create Box Brush"}});
    }

    SECTION("nothing to undo")
    {
      auto g = McpMapToolsFixture{};
      CHECK(g.err("undo") == "nothing_to_undo");
      CHECK(g.err("undo", R"({"allow_user_commands":true})") == "nothing_to_undo");

      createBox(0);
      const auto partial = f.error("undo", R"({"count":100,"allow_user_commands":true})");
      CHECK(partial.value("code") == QJsonValue{"nothing_to_undo"});
      CHECK(
        details(partial).value("undone").toArray().first()
        == QJsonValue{"MCP: Create Box Brush"});
      CHECK_FALSE(canUndo(f.map));
    }

    SECTION("expect_label")
    {
      createBox(0);
      const auto error = f.error("undo", R"({"expect_label":"MCP: Delete Nodes"})");
      CHECK(error.value("code") == QJsonValue{"invalid_argument"});
      CHECK(details(error).value("undo_label") == QJsonValue{"MCP: Create Box Brush"});
      CHECK(*f.map.undoCommandName() == "MCP: Create Box Brush");
    }

    SECTION("is refused during a transaction")
    {
      createBox(0);
      f.map.startTransaction("drag", mdl::TransactionScope::LongRunning);
      const auto error = f.error("undo");
      CHECK(error.value("code") == QJsonValue{"editor_busy"});
      CHECK(details(error).value("reason") == QJsonValue{"transaction_active"});
      f.map.cancelTransaction();
      f.ok("undo");
    }
  }

  SECTION("redo")
  {
    SECTION("redoes MCP steps")
    {
      createBox(0);
      const auto digest = contentDigest(f.doc->nodes, f.map);
      f.ok("undo");
      const auto result = f.ok("redo");
      CHECK(result.value("redone") == QJsonValue{QJsonArray{"MCP: Create Box Brush"}});
      CHECK(result.value("can_redo") == QJsonValue{false});
      CHECK(result.value("redo_label").isNull());
      CHECK(contentDigest(f.doc->nodes, f.map) == digest);
      // the redone brush gets a new id
      CHECK(result.value("changes").toObject().value("created").toArray().size() == 1);
    }

    SECTION("nothing to redo with a non-empty undo stack")
    {
      // regression: Map::canRedoCommand checks the undo stack, and redo aborts on an
      // empty redo stack
      createBox(0);
      REQUIRE(f.map.canRedoCommand());
      REQUIRE(f.map.redoCommandName() == nullptr);
      const auto error = f.error("redo");
      CHECK(error.value("code") == QJsonValue{"nothing_to_redo"});
      CHECK(details(error).value("redone") == QJsonValue{QJsonArray{}});
      CHECK(f.err("redo", R"({"allow_user_commands":true})") == "nothing_to_redo");
    }

    SECTION("count and partial redo")
    {
      createBox(0);
      createBox(32);
      f.ok("undo", R"({"count":2})");
      const auto error = f.error("redo", R"({"count":3})");
      CHECK(error.value("code") == QJsonValue{"nothing_to_redo"});
      CHECK(details(error).value("redone").toArray().size() == 2);
    }

    SECTION("foreign commands are protected")
    {
      mdl::selectNodes(f.map, {scene.wall});
      REQUIRE(mdl::translateSelection(f.map, {16, 0, 0}));
      f.map.undoCommand();
      CHECK(f.err("redo") == "foreign_command");
      CHECK(
        f.ok("redo", R"({"allow_user_commands":true})").value("redone").toArray().size()
        == 1);
    }

    SECTION("expect_label")
    {
      createBox(0);
      f.ok("undo");
      CHECK(f.err("redo", R"({"expect_label":"x"})") == "invalid_argument");
      CHECK(
        f.ok("redo", R"({"expect_label":"MCP: Create Box Brush"})")
          .value("redone")
          .toArray()
          .size()
        == 1);
    }

    SECTION("is refused during a transaction")
    {
      createBox(0);
      f.ok("undo");
      f.map.startTransaction("drag", mdl::TransactionScope::LongRunning);
      CHECK(f.err("redo") == "editor_busy");
      f.map.cancelTransaction();
    }
  }

  SECTION("checkpoint_revert")
  {
    const auto diffIsEmpty = [&](const QString& name) {
      const auto diff = f.ok("checkpoint_diff", QJsonObject{{"from", name}});
      CHECK(diff.value("content_identical") == QJsonValue{true});
      CHECK(diff.value("added") == QJsonValue{QJsonArray{}});
      CHECK(diff.value("removed") == QJsonValue{QJsonArray{}});
      CHECK(diff.value("modified") == QJsonValue{QJsonArray{}});
      CHECK(diff.value("reidentified") == QJsonValue{QJsonArray{}});
    };

    SECTION("undoes the MCP steps since the checkpoint and restores the ids")
    {
      f.ok("checkpoint_create", R"({"name":"base"})");
      const auto digest = contentDigest(f.doc->nodes, f.map);
      const auto box = createBox(0);
      f.ok("transform_nodes", R"({"ids":["B2"],"translate":[16,0,0]})");
      f.ok("set_entity_properties", R"({"ids":["E4"],"set":{"speed":100}})");
      f.ok("delete_nodes", R"({"ids":["E3","G8"]})");
      CHECK(f.err("get_nodes", R"({"ids":["E3"]})") == "stale_node");

      const auto result = f.ok("checkpoint_revert", R"({"name":"base"})");
      CHECK(result.value("checkpoint") == QJsonValue{"base"});
      CHECK(
        result.value("undone")
        == QJsonValue{QJsonArray{
          "MCP: Delete Nodes",
          "MCP: Set Entity Properties",
          "MCP: Transform Nodes",
          "MCP: Create Box Brush"}});
      CHECK(result.value("identical") == QJsonValue{true});
      CHECK(result.value("content_identical") == QJsonValue{true});
      // E3, G8 and B7 were re-added by undoing the delete
      CHECK(result.value("reidentified") == QJsonValue{3});
      CHECK(result.value("can_redo") == QJsonValue{true});
      CHECK(result.value("redo_label") == QJsonValue{"MCP: Create Box Brush"});
      CHECK(!compactJson(result).contains("warnings"));

      CHECK(contentDigest(f.doc->nodes, f.map) == digest);
      diffIsEmpty("base");
      CHECK(f.id(*scene.light) == "E3");
      CHECK(
        f.ok("get_nodes", R"({"ids":["E3","G8","B7"]})").value("nodes").toArray().size()
        == 3);
      CHECK(f.err("get_nodes", QJsonObject{{"ids", QJsonArray{box}}}) == "stale_node");
      CHECK(f.doc->nodes.verifyBijection());

      // the changes report the restored ids
      const auto created = result.value("changes").toObject().value("created").toArray();
      CHECK(created.contains(QJsonValue{"E3"}));
      CHECK(created.contains(QJsonValue{"G8"}));
      CHECK(created.contains(QJsonValue{"B7"}));

      // the undone steps can be redone
      CHECK(f.ok("redo", R"({"count":4})").value("redone").toArray().size() == 4);
      CHECK(f.err("get_nodes", R"({"ids":["E3"]})") == "stale_node");
    }

    SECTION("nothing to revert")
    {
      f.ok("checkpoint_create", R"({"name":"now"})");
      const auto result = f.ok("checkpoint_revert", R"({"name":"now"})");
      CHECK(result.value("undone") == QJsonValue{QJsonArray{}});
      CHECK(result.value("identical") == QJsonValue{true});
      diffIsEmpty("now");
    }

    SECTION("refuses to undo user commands")
    {
      f.ok("checkpoint_create", R"({"name":"base"})");
      createBox(0);
      mdl::selectNodes(f.map, {scene.wall});
      REQUIRE(mdl::translateSelection(f.map, {16, 0, 0}));
      mdl::deselectAll(f.map);
      createBox(32);
      const auto before = snapshot(f.map);

      const auto error = f.error("checkpoint_revert", R"({"name":"base"})");
      CHECK(error.value("code") == QJsonValue{"history_mismatch"});
      CHECK(details(error).value("reason") == QJsonValue{"user_command"});
      // nothing was undone
      CHECK(snapshot(f.map) == before);
    }

    SECTION("refuses if steps before the checkpoint were undone")
    {
      createBox(0);
      f.ok("checkpoint_create", R"({"name":"after_box"})");
      f.ok("undo");
      auto error = f.error("checkpoint_revert", R"({"name":"after_box"})");
      CHECK(error.value("code") == QJsonValue{"history_mismatch"});
      CHECK(details(error).value("reason") == QJsonValue{"not_an_ancestor"});

      // a new step replaces the undone one
      createBox(64);
      error = f.error("checkpoint_revert", R"({"name":"after_box"})");
      CHECK(details(error).value("reason") == QJsonValue{"not_an_ancestor"});
    }

    SECTION("errors")
    {
      const auto error = f.error("checkpoint_revert", R"({"name":"missing"})");
      CHECK(error.value("code") == QJsonValue{"unknown_checkpoint"});
      CHECK(f.err("checkpoint_revert", R"({"name":"a b"})") == "invalid_argument");
      CHECK(f.err("checkpoint_revert", R"({})") == "invalid_argument");

      f.ok("checkpoint_create", R"({"name":"base"})");
      createBox(0);
      f.map.startTransaction("drag", mdl::TransactionScope::LongRunning);
      CHECK(f.err("checkpoint_revert", R"({"name":"base"})") == "editor_busy");
      f.map.cancelTransaction();
    }
  }
}

} // namespace tb::ui
