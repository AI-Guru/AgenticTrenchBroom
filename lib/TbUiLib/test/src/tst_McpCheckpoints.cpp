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
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/Transaction.h"
#include "mdl/TransactionScope.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpCheckpoints.h"
#include "ui/McpPreconditions.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/** Adds a brush in a transaction with the given name. */
mdl::BrushNode* addBrushIn(
  mdl::Map& map, const std::string& transactionName, const vm::bbox3d& bounds)
{
  auto transaction = mdl::Transaction{map, transactionName};
  auto* brushNode = addBrush(map, bounds);
  REQUIRE(transaction.commit());
  return brushNode;
}

std::vector<std::string> labels(const std::vector<McpHistoryTracker::Entry>& entries)
{
  auto result = std::vector<std::string>{};
  for (const auto& entry : entries)
  {
    result.push_back(entry.label);
  }
  return result;
}

McpCheckpoint checkpoint(McpMapToolsFixture& f, const QString& name = "c")
{
  return takeCheckpoint(f.doc->nodes, f.map, f.doc->history, name);
}

} // namespace

TEST_CASE("McpHistoryTracker")
{
  auto f = McpMapToolsFixture{};
  auto& map = f.map;
  auto& history = f.doc->history;

  REQUIRE(history.undoStack().empty());
  REQUIRE(history.redoStack().empty());
  REQUIRE(history.consistent());

  SECTION("undoStack")
  {
    SECTION("records top level transactions with their labels")
    {
      addBrushIn(map, "MCP: Create Box", {{0, 0, 0}, {16, 16, 16}});
      addBrushIn(map, "Create Brush", {{32, 0, 0}, {48, 16, 16}});

      CHECK(
        labels(history.undoStack())
        == std::vector<std::string>{"MCP: Create Box", "Create Brush"});
      CHECK(history.undoStack()[0].mcp);
      CHECK_FALSE(history.undoStack()[1].mcp);
      CHECK(history.undoStack()[0].serial == 1);
      CHECK(history.undoStack()[1].serial == 2);
      CHECK(history.consistent());
    }

    SECTION("records commands executed outside of a transaction")
    {
      auto* brushNode = addBrushIn(map, "MCP: Create Box", {{0, 0, 0}, {16, 16, 16}});
      mdl::selectNodes(map, {brushNode});

      CHECK(
        labels(history.undoStack())
        == std::vector<std::string>{"MCP: Create Box", "Select 1 Object"});
      CHECK(history.consistent());
    }

    SECTION("ignores nested transactions")
    {
      {
        auto outer = mdl::Transaction{map, "MCP: Batch"};
        addBrushIn(map, "MCP: Create Box", {{0, 0, 0}, {16, 16, 16}});
        addBrushIn(map, "MCP: Create Box", {{32, 0, 0}, {48, 16, 16}});
        REQUIRE(outer.commit());
      }

      CHECK(labels(history.undoStack()) == std::vector<std::string>{"MCP: Batch"});
      CHECK(history.consistent());
    }

    SECTION("ignores cancelled transactions")
    {
      addBrushIn(map, "MCP: Create Box", {{0, 0, 0}, {16, 16, 16}});
      {
        auto transaction = mdl::Transaction{map, "MCP: Failing"};
        addBrush(map, {{32, 0, 0}, {48, 16, 16}});
        transaction.cancel();
      }

      CHECK(labels(history.undoStack()) == std::vector<std::string>{"MCP: Create Box"});
      CHECK(history.redoStack().empty());
      CHECK(history.consistent());

      // the next command executed outside of a transaction is not mistaken for a
      // collated transaction
      mdl::selectAllNodes(map);
      CHECK(
        labels(history.undoStack())
        == std::vector<std::string>{"MCP: Create Box", "Select All Objects"});
      CHECK(history.consistent());
    }

    SECTION("does not push collated transactions")
    {
      auto* brushNode = addBrushIn(map, "MCP: Create Box", {{0, 0, 0}, {16, 16, 16}});
      mdl::selectNodes(map, {brushNode});
      map.setIsCommandCollationEnabled(true);

      const auto move = [&](const std::string& name, const bool withCancelledNested) {
        auto transaction = mdl::Transaction{map, name};
        REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
        if (withCancelledNested)
        {
          auto nested = mdl::Transaction{map, "nested"};
          REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
          nested.cancel();
        }
        REQUIRE(transaction.commit());
      };

      move("Move A", false);
      REQUIRE(*map.undoCommandName() == "Move A");

      SECTION("a plain transaction")
      {
        move("Move B", false);
      }

      SECTION("a transaction with a cancelled nested transaction")
      {
        move("Move B", true);
      }

      // both moves were collated into "Move A"
      REQUIRE(*map.undoCommandName() == "Move A");
      CHECK(
        labels(history.undoStack())
        == std::vector<std::string>{"MCP: Create Box", "Select 1 Object", "Move A"});
      CHECK(history.consistent());
    }

    SECTION("moves undone entries to the redo stack")
    {
      addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
      addBrushIn(map, "MCP: B", {{32, 0, 0}, {48, 16, 16}});

      map.undoCommand();
      CHECK(labels(history.undoStack()) == std::vector<std::string>{"MCP: A"});
      CHECK(labels(history.redoStack()) == std::vector<std::string>{"MCP: B"});
      CHECK(history.consistent());

      map.undoCommand();
      CHECK(history.undoStack().empty());
      CHECK(labels(history.redoStack()) == std::vector<std::string>{"MCP: B", "MCP: A"});
      CHECK(history.consistent());
    }
  }

  SECTION("redoStack")
  {
    SECTION("redo moves the entry back with its serial")
    {
      addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
      addBrushIn(map, "MCP: B", {{32, 0, 0}, {48, 16, 16}});
      const auto before = history.undoStack();

      map.undoCommand();
      map.undoCommand();
      map.redoCommand();
      CHECK(labels(history.undoStack()) == std::vector<std::string>{"MCP: A"});
      CHECK(labels(history.redoStack()) == std::vector<std::string>{"MCP: B"});
      map.redoCommand();

      CHECK(history.undoStack() == before);
      CHECK(history.redoStack().empty());
      CHECK(history.consistent());
    }

    SECTION("redo of a command executed outside of a transaction")
    {
      auto* brushNode = addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
      mdl::selectNodes(map, {brushNode});
      const auto before = history.undoStack();

      map.undoCommand();
      CHECK(labels(history.redoStack()) == std::vector<std::string>{"Select 1 Object"});
      map.redoCommand();

      CHECK(history.undoStack() == before);
      CHECK(history.redoStack().empty());
      CHECK(history.consistent());
    }

    SECTION("a new transaction with the same name as the redo entry clears it")
    {
      addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
      const auto serial = history.undoStack().back().serial;
      map.undoCommand();

      addBrushIn(map, "MCP: A", {{32, 0, 0}, {48, 16, 16}});
      CHECK(labels(history.undoStack()) == std::vector<std::string>{"MCP: A"});
      CHECK(history.undoStack().back().serial != serial);
      CHECK(history.redoStack().empty());
      CHECK(history.consistent());
    }

    SECTION("a new command outside of a transaction clears it")
    {
      auto* brushNode = addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
      addBrushIn(map, "MCP: B", {{32, 0, 0}, {48, 16, 16}});
      map.undoCommand();

      mdl::selectNodes(map, {brushNode});
      CHECK(
        labels(history.undoStack())
        == std::vector<std::string>{"MCP: A", "Select 1 Object"});
      CHECK(history.redoStack().empty());
      CHECK(history.consistent());
    }
  }

  SECTION("undoSerials")
  {
    addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
    addBrushIn(map, "MCP: B", {{32, 0, 0}, {48, 16, 16}});
    CHECK(history.undoSerials() == std::vector<uint64_t>{1, 2});

    map.undoCommand();
    addBrushIn(map, "MCP: C", {{64, 0, 0}, {80, 16, 16}});
    CHECK(history.undoSerials() == std::vector<uint64_t>{1, 3});
  }

  SECTION("mcpDepth")
  {
    CHECK(history.mcpDepth() == 0);
    auto* brushNode = addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
    mdl::selectNodes(map, {brushNode});
    addBrushIn(map, "MCP: B", {{32, 0, 0}, {48, 16, 16}});
    CHECK(history.mcpDepth() == 2);

    map.undoCommand();
    CHECK(history.mcpDepth() == 1);
  }

  SECTION("consistent")
  {
    SECTION("false while a transaction is running")
    {
      map.startTransaction("drag", mdl::TransactionScope::LongRunning);
      CHECK_FALSE(history.consistent());
      map.cancelTransaction();
      CHECK(history.consistent());
    }

    SECTION("a tracker created late models the existing history with placeholders")
    {
      addBrushIn(map, "User A", {{0, 0, 0}, {16, 16, 16}});
      addBrushIn(map, "User B", {{32, 0, 0}, {48, 16, 16}});
      map.undoCommand();
      auto lateTracker = McpHistoryTracker{map};
      CHECK(lateTracker.consistent());
      CHECK(labels(lateTracker.undoStack()) == std::vector<std::string>{"User A"});
      CHECK(labels(lateTracker.redoStack()) == std::vector<std::string>{"User B"});
      CHECK(lateTracker.mcpDepth() == 0);
      CHECK_FALSE(lateTracker.undoStack().front().mcp);

      addBrushIn(map, "MCP: C", {{64, 0, 0}, {80, 16, 16}});
      CHECK(lateTracker.consistent());
      CHECK(
        labels(lateTracker.undoStack()) == std::vector<std::string>{"User A", "MCP: C"});
      CHECK(lateTracker.redoStack().empty());
    }

    SECTION("false if the tracker missed commands")
    {
      addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
      addBrushIn(map, "MCP: B", {{32, 0, 0}, {48, 16, 16}});
      auto lateTracker = McpHistoryTracker{map};
      CHECK(lateTracker.consistent());

      // only the top entry is modelled; undoing below it is noticed
      map.undoCommand();
      CHECK(lateTracker.undoStack().empty());
      CHECK(labels(lateTracker.redoStack()) == std::vector<std::string>{"MCP: B"});
      CHECK_FALSE(lateTracker.consistent());

      // a new command clears the redo stacks
      addBrushIn(map, "MCP: C", {{64, 0, 0}, {80, 16, 16}});
      CHECK(lateTracker.consistent());
    }
  }

  SECTION("sync")
  {
    addBrushIn(map, "MCP: A", {{0, 0, 0}, {16, 16, 16}});
    addBrushIn(map, "MCP: B", {{32, 0, 0}, {48, 16, 16}});
    map.undoCommand();
    REQUIRE(history.consistent());

    map.commandProcessor().clear();
    CHECK_FALSE(history.consistent());

    // does nothing while a transaction is running
    map.startTransaction("drag", mdl::TransactionScope::LongRunning);
    history.sync();
    CHECK(history.undoStack().size() == 1);
    CHECK(history.redoStack().size() == 1);
    map.cancelTransaction();

    history.sync();
    CHECK(history.undoStack().empty());
    CHECK(history.redoStack().empty());
    CHECK(history.consistent());

    // keeps entries that the map still has
    addBrushIn(map, "MCP: C", {{64, 0, 0}, {80, 16, 16}});
    history.sync();
    CHECK(labels(history.undoStack()) == std::vector<std::string>{"MCP: C"});
  }

  SECTION("isMcpLabel")
  {
    CHECK(McpHistoryTracker::isMcpLabel("MCP: Create Box"));
    CHECK_FALSE(McpHistoryTracker::isMcpLabel("MCP:Create Box"));
    CHECK_FALSE(McpHistoryTracker::isMcpLabel("Create Box"));
    CHECK_FALSE(McpHistoryTracker::isMcpLabel(""));
  }
}

TEST_CASE("McpCheckpointStore")
{
  auto f = McpMapToolsFixture{};
  auto store = McpCheckpointStore{};

  SECTION("add")
  {
    auto added = store.add(checkpoint(f, "a"), false);
    REQUIRE(added.is_success());
    CHECK(added.value()->name == "a");
    CHECK(store.size() == 1);

    SECTION("an existing name is rejected")
    {
      const auto again = store.add(checkpoint(f, "a"), false);
      REQUIRE(again.is_error());
      CHECK(errorOf(again).code == "checkpoint_exists");
      CHECK(store.size() == 1);
    }

    SECTION("an existing name is replaced if requested")
    {
      addBrush(f.map, {{0, 0, 0}, {16, 16, 16}});
      const auto replaced = store.add(checkpoint(f, "a"), true);
      REQUIRE(replaced.is_success());
      CHECK(store.size() == 1);
      CHECK(store.find("a")->nodeCount() == 2);
    }

    SECTION("at most MaxCheckpoints checkpoints can exist")
    {
      for (size_t i = 1; i < McpCheckpointStore::MaxCheckpoints; ++i)
      {
        REQUIRE(store.add(checkpoint(f, QString{"c%1"}.arg(i)), false).is_success());
      }
      REQUIRE(store.size() == McpCheckpointStore::MaxCheckpoints);

      const auto tooMany = store.add(checkpoint(f, "z"), false);
      REQUIRE(tooMany.is_error());
      CHECK(errorOf(tooMany).code == "limit_exceeded");
      CHECK(errorOf(tooMany).details.value("limit") == QJsonValue{32});

      // replacing is still possible
      CHECK(store.add(checkpoint(f, "a"), true).is_success());
    }
  }

  SECTION("find")
  {
    CHECK(store.find("a") == nullptr);
    REQUIRE(store.add(checkpoint(f, "a"), false).is_success());
    REQUIRE(store.find("a") != nullptr);
    CHECK(store.find("a")->name == "a");
    CHECK(store.find("A") == nullptr);
  }

  SECTION("remove")
  {
    REQUIRE(store.add(checkpoint(f, "a"), false).is_success());
    CHECK_FALSE(store.remove("b"));
    CHECK(store.remove("a"));
    CHECK(store.size() == 0);
    CHECK_FALSE(store.remove("a"));
  }

  SECTION("list")
  {
    REQUIRE(store.add(checkpoint(f, "b"), false).is_success());
    REQUIRE(store.add(checkpoint(f, "a"), false).is_success());
    REQUIRE(store.add(checkpoint(f, "c"), false).is_success());

    const auto list = store.list();
    REQUIRE(list.size() == 3);
    CHECK(list[0]->name == "a");
    CHECK(list[1]->name == "b");
    CHECK(list[2]->name == "c");
  }

  SECTION("names")
  {
    REQUIRE(store.add(checkpoint(f, "b"), false).is_success());
    REQUIRE(store.add(checkpoint(f, "a"), false).is_success());
    CHECK(store.names() == QStringList{"a", "b"});
  }

  SECTION("size")
  {
    CHECK(store.size() == 0);
    REQUIRE(store.add(checkpoint(f, "a"), false).is_success());
    CHECK(store.size() == 1);
  }
}

TEST_CASE("McpCheckpoints")
{
  auto f = McpMapToolsFixture{};
  auto& map = f.map;
  const auto& registry = f.doc->nodes;

  SECTION("canonicalRecord")
  {
    SECTION("brush: golden record of a 64 unit cube")
    {
      auto* brushNode = addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "base");
      const auto record = canonicalRecord(registry, map, *brushNode);

      CHECK(record.value("type") == QJsonValue{"brush"});
      CHECK(record.value("parent") == QJsonValue{"L1"});
      CHECK(
        compactJson(record.value("vertices"))
        == QStringLiteral(
          "[[0,0,0],[0,0,64],[0,64,0],[0,64,64],[64,0,0],[64,0,64],[64,64,0],[64,64,64]"
          "]"));

      const auto faces = record.value("faces").toArray();
      REQUIRE(faces.size() == 6);
      auto planes = QStringList{};
      for (const auto& face : faces)
      {
        const auto object = face.toObject();
        CHECK(object.value("material") == QJsonValue{"base"});
        CHECK(
          compactJson(object.value("uv"))
          == QStringLiteral(R"({"offset":[0,0],"rotation":0,"scale":[1,1]})"));
        CHECK(object.value("surface") == QJsonValue{QJsonObject{}});
        planes.push_back(
          compactJson(object.value("normal")) + " "
          + QString::number(object.value("distance").toDouble()));
      }
      planes.sort();
      CHECK(
        planes
        == QStringList{
          "[-1,0,0] 0",
          "[0,-1,0] 0",
          "[0,0,-1] 0",
          "[0,0,1] 64",
          "[0,1,0] 64",
          "[1,0,0] 64",
        });
    }

    SECTION("world, layers, groups, entities and patches")
    {
      const auto scene = populateTestScene(f);

      const auto world = canonicalRecord(registry, map, map.worldNode());
      CHECK(world.value("type") == QJsonValue{"world"});
      CHECK(world.value("parent").isNull());
      CHECK(
        world.value("properties").toArray().first()
        == QJsonArray{"classname", "worldspawn"});

      const auto layer = canonicalRecord(registry, map, *scene.layer);
      CHECK(
        compactJson(layer)
        == QStringLiteral(
          R"({"name":"details","omit_from_export":false,"parent":"world","sort_index":0,"type":"layer"})"));

      const auto group = canonicalRecord(registry, map, *scene.group);
      CHECK(group.value("name") == QJsonValue{"room"});
      CHECK(group.value("linked") == QJsonValue{false});
      CHECK(
        compactJson(group.value("transformation"))
        == QStringLiteral("[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]"));

      const auto light = canonicalRecord(registry, map, *scene.light);
      CHECK(
        compactJson(light)
        == QStringLiteral(
          R"({"parent":"L1","properties":[["classname","light"],["origin","32 32 128"],["light","300"],["_color","1 1 1"]],"type":"entity"})"));

      const auto doorBrush =
        canonicalRecord(registry, map, *scene.door->children().front());
      CHECK(doorBrush.value("parent") == QJsonValue{"E4"});

      const auto patch = canonicalRecord(registry, map, *scene.patch);
      CHECK(patch.value("rows") == QJsonValue{3});
      CHECK(patch.value("columns") == QJsonValue{3});
      CHECK(patch.value("material") == QJsonValue{"patch_material"});
      CHECK(patch.value("points").toArray().size() == 9);
      CHECK(
        compactJson(patch.value("points").toArray().at(4))
        == QStringLiteral("[1,1,2,0,0]"));
    }
  }

  SECTION("contentDigest")
  {
    const auto empty = contentDigest(registry, map);
    CHECK(empty.size() == 64);

    auto* brushNode = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
    const auto withBrush = contentDigest(registry, map);
    CHECK(withBrush != empty);
    CHECK(withBrush == checkpoint(f).contentDigest);

    SECTION("is independent of ids")
    {
      // the same content in a second map gets the same digest, although the ids
      // differ because a node was created and removed before
      auto other = McpMapToolsFixture{};
      auto* temporary = addBrush(other.map, {{0, 0, 0}, {8, 8, 8}});
      mdl::removeNodes(other.map, {temporary});
      auto* otherBrush = addBrush(other.map, {{0, 0, 0}, {64, 64, 64}});
      REQUIRE(other.id(*otherBrush) != f.id(*brushNode));
      CHECK(contentDigest(other.doc->nodes, other.map) == withBrush);
    }

    SECTION("is equal after delete and undo")
    {
      mdl::removeNodes(map, {brushNode});
      CHECK(contentDigest(registry, map) == empty);
      map.undoCommand();
      CHECK(contentDigest(registry, map) == withBrush);
    }

    SECTION("changes with properties and geometry")
    {
      mdl::selectNodes(map, {brushNode});
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
      const auto moved = contentDigest(registry, map);
      CHECK(moved != withBrush);

      auto* entityNode = addPointEntity(map, "light", {0, 0, 0});
      const auto withEntity = contentDigest(registry, map);
      mdl::deselectAll(map);
      mdl::selectNodes(map, {entityNode});
      REQUIRE(mdl::setEntityProperty(map, "light", "200"));
      CHECK(contentDigest(registry, map) != withEntity);
    }

    SECTION("does not depend on the selection")
    {
      mdl::selectNodes(map, {brushNode});
      CHECK(contentDigest(registry, map) == withBrush);
    }
  }

  SECTION("takeCheckpoint")
  {
    const auto scene = populateTestScene(f);
    {
      auto transaction = mdl::Transaction{map, "MCP: Test"};
      addBrush(map, {{256, 0, 0}, {272, 16, 16}});
      REQUIRE(transaction.commit());
    }

    const auto c = checkpoint(f, "before");
    CHECK(c.name == "before");
    CHECK(c.modificationCount == map.modificationCount());
    CHECK(c.mcpHistoryDepth == 1);
    CHECK(c.historySerials == f.doc->history.undoSerials());
    CHECK(c.historyConsistent);
    CHECK(c.contentDigest == contentDigest(registry, map));

    // DFS order starting with the world
    auto ids = QStringList{};
    for (const auto& record : c.records)
    {
      ids.push_back(record.id);
    }
    CHECK(
      ids
      == QStringList{
        "world",
        "L1",
        "B2",
        "E3",
        "E4",
        "B5",
        "B6",
        "G8",
        "B7",
        "P9",
        "B12",
        "L10",
        "E11"});
    CHECK(c.nodeCount() == 12);

    REQUIRE(c.find("E3") != nullptr);
    CHECK(c.find("E3")->type == "entity");
    CHECK(c.find("E3")->record == canonicalRecord(registry, map, *scene.light));
    CHECK(c.find("E3")->summary.value("classname") == QJsonValue{"light"});
    CHECK(c.find("E3")->contentDigest.size() == 64);
    CHECK(c.find("B99") == nullptr);

    // the brushes of the door have equal content apart from their position
    CHECK(c.find("B5")->contentDigest != c.find("B6")->contentDigest);

    CHECK(
      compactJson(c.info())
      == QString{R"({"content_digest":"%1","history_depth":1,"modification_count":%2,"name":"before","node_count":12})"}
           .arg(c.contentDigest)
           .arg(map.modificationCount()));
  }

  SECTION("diffCheckpoints")
  {
    const auto scene = populateTestScene(f);
    const auto before = checkpoint(f, "before");

    SECTION("identical checkpoints")
    {
      const auto diff = diffCheckpoints(before, checkpoint(f, "after"), 200);
      CHECK(diff.value("content_identical") == QJsonValue{true});
      CHECK(diff.value("from_digest") == diff.value("to_digest"));
      CHECK(diff.value("added").toArray().isEmpty());
      CHECK(diff.value("removed").toArray().isEmpty());
      CHECK(diff.value("modified").toArray().isEmpty());
      CHECK(diff.value("reidentified").toArray().isEmpty());
      CHECK(diff.value("unchanged_count") == QJsonValue{12});
      CHECK(diff.value("truncated") == QJsonValue{false});
    }

    SECTION("added, removed and modified nodes")
    {
      mdl::selectNodes(map, {scene.wall});
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
      mdl::deselectAll(map);
      mdl::selectNodes(map, {scene.light});
      REQUIRE(mdl::setEntityProperty(map, "light", "400"));
      REQUIRE(mdl::setEntityProperty(map, "_extra", "x"));
      mdl::deselectAll(map);
      mdl::removeNodes(map, {scene.playerStart});
      auto* added = addBrush(map, {{512, 0, 0}, {528, 16, 16}});

      const auto diff = diffCheckpoints(before, checkpoint(f, "after"), 200);
      CHECK(diff.value("content_identical") == QJsonValue{false});

      const auto addedList = diff.value("added").toArray();
      REQUIRE(addedList.size() == 1);
      CHECK(addedList[0].toObject().value("id") == QJsonValue{f.id(*added)});
      CHECK(addedList[0].toObject().value("type") == QJsonValue{"brush"});

      CHECK(
        compactJson(diff.value("removed"))
        == QString{R"([{"id":"E11","summary":%1,"type":"entity"}])"}.arg(
          compactJson(before.find("E11")->summary)));

      const auto modified = diff.value("modified").toArray();
      auto modifiedIds = QStringList{};
      for (const auto& entry : modified)
      {
        modifiedIds.push_back(entry.toObject().value("id").toString());
      }
      CHECK(modifiedIds == QStringList{"B2", "E3"});

      const auto wall = modified[0].toObject();
      CHECK(wall.value("geometry_changed") == QJsonValue{true});
      CHECK(wall.value("fields") == QJsonValue{QJsonObject{}});
      CHECK(wall.value("faces_changed").toArray().size() == 6);
      CHECK_FALSE(wall.contains("properties"));

      const auto light = modified[1].toObject();
      CHECK(light.value("geometry_changed") == QJsonValue{false});
      CHECK(
        compactJson(light.value("properties"))
        == QStringLiteral(
          R"({"_extra":{"after":"x","before":null},"light":{"after":"400","before":"300"}})"));

      CHECK(diff.value("added_count") == QJsonValue{1});
      CHECK(diff.value("removed_count") == QJsonValue{1});
      CHECK(diff.value("modified_count") == QJsonValue{2});
      CHECK(diff.value("reidentified_count") == QJsonValue{0});
      // world, L1, E4, B5, B6, G8, B7, P9, L10
      CHECK(diff.value("unchanged_count") == QJsonValue{9});
    }

    SECTION("changed fields and faces")
    {
      mdl::selectNodes(map, {scene.wall});
      REQUIRE(mdl::setBrushFaceAttributes(map, {.materialName = "other"}));
      mdl::deselectAll(map);
      mdl::removeNodes(map, {scene.light});
      mdl::selectNodes(map, {scene.patch});
      mdl::moveSelectedNodesToLayer(map, scene.layer);

      const auto diff = diffCheckpoints(before, checkpoint(f, "after"), 200);
      const auto modified = diff.value("modified").toArray();
      REQUIRE(modified.size() == 2);

      const auto wall = modified[0].toObject();
      CHECK(wall.value("id") == QJsonValue{"B2"});
      CHECK(wall.value("geometry_changed") == QJsonValue{false});
      CHECK(compactJson(wall.value("faces_changed")) == QStringLiteral("[0,1,2,3,4,5]"));

      const auto patch = modified[1].toObject();
      CHECK(patch.value("id") == QJsonValue{"P9"});
      CHECK(
        compactJson(patch.value("fields"))
        == QStringLiteral(R"({"parent":{"after":"L10","before":"L1"}})"));
      CHECK(patch.value("geometry_changed") == QJsonValue{false});
    }

    SECTION("nodes restored by undo are reidentified")
    {
      mdl::removeNodes(map, {scene.door});
      map.undoCommand();
      REQUIRE(registry.lookup(u"E4").status == McpNodeRegistry::LookupStatus::Stale);

      const auto after = checkpoint(f, "after");
      const auto diff = diffCheckpoints(before, after, 200);
      CHECK(diff.value("content_identical") == QJsonValue{true});
      CHECK(diff.value("added").toArray().isEmpty());
      CHECK(diff.value("removed").toArray().isEmpty());
      CHECK(diff.value("modified").toArray().isEmpty());

      // undo re-adds the door as the last child of the layer
      const auto newDoor = f.id(*map.worldNode().defaultLayer()->children().back());
      CHECK(newDoor == "E12");
      CHECK(
        compactJson(diff.value("reidentified"))
        == QStringLiteral(
          R"([{"from":"E4","to":"E12"},{"from":"B5","to":"B13"},{"from":"B6","to":"B14"}])"));
      CHECK(diff.value("reidentified_count") == QJsonValue{3});
    }

    SECTION("nodes with equal content are paired in order")
    {
      auto* a = addBrush(map, {{512, 0, 0}, {528, 16, 16}});
      auto* b = addBrush(map, {{512, 0, 0}, {528, 16, 16}});
      const auto withCopies = checkpoint(f, "copies");
      mdl::removeNodes(map, {a, b});
      map.undoCommand();

      const auto diff = diffCheckpoints(withCopies, checkpoint(f, "after"), 200);
      CHECK(
        compactJson(diff.value("reidentified"))
        == QStringLiteral(R"([{"from":"B12","to":"B14"},{"from":"B13","to":"B15"}])"));
    }

    SECTION("lists are limited to maxChanges entries")
    {
      for (int i = 0; i < 5; ++i)
      {
        addBrush(map, {{512.0 + 32 * i, 0, 0}, {528.0 + 32 * i, 16, 16}});
      }
      const auto diff = diffCheckpoints(before, checkpoint(f, "after"), 3);
      CHECK(diff.value("added").toArray().size() == 3);
      CHECK(diff.value("added_count") == QJsonValue{5});
      CHECK(diff.value("truncated") == QJsonValue{true});
    }

    SECTION("property order changes")
    {
      auto entity = scene.light->entity();
      entity.removeProperty("light");
      entity.addOrUpdateProperty("light", "300");
      mdl::updateNodeContents(
        map, "Reorder", {{scene.light, mdl::NodeContents{std::move(entity)}}});

      const auto diff = diffCheckpoints(before, checkpoint(f, "after"), 200);
      const auto modified = diff.value("modified").toArray();
      REQUIRE(modified.size() == 1);
      CHECK(
        compactJson(modified[0].toObject().value("fields"))
        == QStringLiteral(
          R"({"property_order":{"after":["classname","origin","_color","light"],"before":["classname","origin","light","_color"]}})"));
      CHECK_FALSE(modified[0].toObject().contains("properties"));
    }
  }
}

} // namespace tb::ui
