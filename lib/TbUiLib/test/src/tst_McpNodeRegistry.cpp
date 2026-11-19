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
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpNodeRegistry.h"

#include <random>
#include <unordered_map>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

using Status = McpNodeRegistry::LookupStatus;

std::vector<QString> ids(std::initializer_list<const char*> list)
{
  auto result = std::vector<QString>{};
  for (const auto* id : list)
  {
    result.emplace_back(id);
  }
  return result;
}

/** Collects every non-world node in DFS pre-order. */
void collectNodes(mdl::Node& node, std::vector<mdl::Node*>& result)
{
  for (auto* child : node.children())
  {
    result.push_back(child);
    collectNodes(*child, result);
  }
}

std::vector<mdl::Node*> allNodes(mdl::Map& map)
{
  auto result = std::vector<mdl::Node*>{};
  collectNodes(map.worldNode(), result);
  return result;
}

} // namespace

TEST_CASE("McpNodeRegistry")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create(mdl::QuakeFixtureConfig);

  SECTION("McpNodeRegistry")
  {
    SECTION("assigns ids in DFS pre-order")
    {
      auto* layer = addLayer(map, "custom");
      auto* brush1 = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
      auto* door =
        addBrushEntity(map, "func_door", {vm::bbox3d{{32, 0, 0}, {48, 16, 16}}});
      auto* light = addPointEntity(map, "light", {0, 0, 64}, layer);
      auto* patch = addPatch(map, layer);

      const auto registry = McpNodeRegistry{map};
      CHECK(registry.idOf(map.worldNode()) == QStringLiteral("world"));
      CHECK(registry.idOf(*map.worldNode().defaultLayer()) == QStringLiteral("L1"));
      CHECK(registry.idOf(*brush1) == QStringLiteral("B2"));
      CHECK(registry.idOf(*door) == QStringLiteral("E3"));
      CHECK(registry.idOf(*door->children().front()) == QStringLiteral("B4"));
      CHECK(registry.idOf(*layer) == QStringLiteral("L5"));
      CHECK(registry.idOf(*light) == QStringLiteral("E6"));
      CHECK(registry.idOf(*patch) == QStringLiteral("P7"));
      CHECK(registry.liveCount() == 7);
      CHECK(registry.verifyBijection());
    }

    SECTION("identical maps get identical ids")
    {
      auto otherFixture = mdl::MapFixture{};
      auto& otherMap = otherFixture.create(mdl::QuakeFixtureConfig);

      for (auto* m : {&map, &otherMap})
      {
        addBrush(*m, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
        addPointEntity(*m, "light", {0, 0, 64});
        addBrushEntity(*m, "func_door", {vm::bbox3d{{32, 0, 0}, {48, 16, 16}}});
      }

      const auto registry = McpNodeRegistry{map};
      const auto otherRegistry = McpNodeRegistry{otherMap};
      const auto nodes = allNodes(map);
      const auto otherNodes = allNodes(otherMap);
      REQUIRE(nodes.size() == otherNodes.size());
      for (size_t i = 0; i < nodes.size(); ++i)
      {
        CHECK(registry.idOf(*nodes[i]) == otherRegistry.idOf(*otherNodes[i]));
      }
    }
  }

  SECTION("idOf")
  {
    auto registry = McpNodeRegistry{map};
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    CHECK(registry.idOf(*brush) == QStringLiteral("B2"));

    // a node that is not part of the map has no id
    auto orphan = mdl::LayerNode{mdl::Layer{"orphan"}};
    CHECK(registry.idOf(orphan).isEmpty());
  }

  SECTION("lookup")
  {
    auto registry = McpNodeRegistry{map};
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    auto* entity = addPointEntity(map, "light", {0, 0, 0});

    SECTION("live ids and aliases")
    {
      CHECK(registry.lookup(u"B2").status == Status::Live);
      CHECK(registry.lookup(u"B2").node == brush);
      CHECK(registry.lookup(u"E3").node == entity);
      CHECK(registry.lookup(u"world").node == &map.worldNode());
      CHECK(registry.lookup(u"default_layer").node == map.worldNode().defaultLayer());
      CHECK(registry.lookup(u"L1").node == map.worldNode().defaultLayer());
    }

    SECTION("type tag mismatch")
    {
      const auto lookup = registry.lookup(u"B3");
      CHECK(lookup.status == Status::WrongType);
      CHECK(lookup.node == entity);
      CHECK(registry.lookup(u"L2").status == Status::WrongType);
    }

    SECTION("unknown ids")
    {
      CHECK(registry.lookup(u"B4").status == Status::Unknown);
      CHECK(registry.lookup(u"E999999999999999999").status == Status::Unknown);
      CHECK(registry.lookup(u"B4").node == nullptr);
    }

    SECTION("malformed ids")
    {
      for (const auto* id :
           {"",
            "B",
            "B0",
            "B01",
            "b2",
            "X2",
            "2",
            "B2x",
            "B-2",
            "@selection",
            " B2",
            "E1234567890123456789",
            "World"})
      {
        CAPTURE(id);
        CHECK(registry.lookup(QString::fromLatin1(id)).status == Status::Malformed);
      }
    }

    SECTION("stale ids")
    {
      registry.setRemovalLabel("MCP: Delete Nodes");
      mdl::removeNodes(map, {brush});

      const auto lookup = registry.lookup(u"B2");
      CHECK(lookup.status == Status::Stale);
      CHECK(lookup.node == nullptr);
      CHECK(lookup.removedBy == QStringLiteral("MCP: Delete Nodes"));

      registry.setRemovalLabel("");
      mdl::removeNodes(map, {entity});
      CHECK(registry.lookup(u"E3").removedBy == QStringLiteral("editor"));
    }
  }

  SECTION("isLive")
  {
    auto registry = McpNodeRegistry{map};
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    CHECK(registry.isLive(&map.worldNode()));
    CHECK(registry.isLive(brush));

    mdl::removeNodes(map, {brush});
    CHECK_FALSE(registry.isLive(brush));
    CHECK_FALSE(registry.isLive(nullptr));
  }

  SECTION("nodesWereAdded and nodesWereRemoved")
  {
    auto registry = McpNodeRegistry{map};

    SECTION("adds take the next numbers")
    {
      auto* brush1 = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
      auto* brush2 = addBrush(map, vm::bbox3d{{32, 0, 0}, {48, 16, 16}});
      auto* door =
        addBrushEntity(map, "func_door", {vm::bbox3d{{64, 0, 0}, {80, 16, 16}}});
      CHECK(registry.idOf(*brush1) == QStringLiteral("B2"));
      CHECK(registry.idOf(*brush2) == QStringLiteral("B3"));
      CHECK(registry.idOf(*door) == QStringLiteral("E4"));
      CHECK(registry.idOf(*door->children().front()) == QStringLiteral("B5"));
      CHECK(registry.verifyBijection());
    }

    SECTION("delete followed by undo assigns a new id")
    {
      auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
      REQUIRE(registry.idOf(*brush) == QStringLiteral("B2"));

      mdl::removeNodes(map, {brush});
      CHECK(registry.lookup(u"B2").status == Status::Stale);
      CHECK(registry.verifyBijection());

      map.undoCommand();
      CHECK(brush->parent() != nullptr);
      CHECK(registry.idOf(*brush) == QStringLiteral("B3"));
      CHECK(registry.lookup(u"B2").status == Status::Stale);
      CHECK(registry.verifyBijection());

      map.redoCommand();
      CHECK(registry.lookup(u"B3").status == Status::Stale);
      CHECK(registry.verifyBijection());
    }

    SECTION("removing a container makes its descendants stale")
    {
      auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
      auto* group = groupNodes(map, {brush}, "group");
      const auto groupId = registry.idOf(*group);
      const auto brushId = registry.idOf(*brush);

      mdl::deselectAll(map);
      mdl::removeNodes(map, {group});
      CHECK(registry.lookup(groupId).status == Status::Stale);
      CHECK(registry.lookup(brushId).status == Status::Stale);
      CHECK(registry.verifyBijection());
    }
  }

  SECTION("ids survive edits and reparenting")
  {
    auto registry = McpNodeRegistry{map};
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    auto* light = addPointEntity(map, "light", {0, 0, 64});
    auto* layer = addLayer(map, "other");
    const auto brushId = registry.idOf(*brush);
    const auto lightId = registry.idOf(*light);

    SECTION("translate and property edits")
    {
      mdl::selectNodes(map, {brush, light});
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
      mdl::deselectAll(map);
      mdl::selectNodes(map, {light});
      REQUIRE(mdl::setEntityProperty(map, "light", "300"));

      CHECK(registry.idOf(*brush) == brushId);
      CHECK(registry.idOf(*light) == lightId);

      map.undoCommand();
      map.undoCommand();
      CHECK(registry.idOf(*brush) == brushId);
      CHECK(registry.idOf(*light) == lightId);
      CHECK(registry.verifyBijection());
    }

    SECTION("groupSelectedNodes and ungroupSelectedNodes")
    {
      auto* group = groupNodes(map, {brush, light}, "group");
      CHECK(registry.idOf(*brush) == brushId);
      CHECK(registry.idOf(*light) == lightId);
      CHECK(registry.idOf(*group).startsWith('G'));

      const auto groupId = registry.idOf(*group);
      mdl::ungroupSelectedNodes(map);
      CHECK(registry.idOf(*brush) == brushId);
      CHECK(registry.idOf(*light) == lightId);
      CHECK(registry.lookup(groupId).status == Status::Stale);
      CHECK(registry.verifyBijection());
    }

    SECTION("moveSelectedNodesToLayer")
    {
      mdl::selectNodes(map, {brush, light});
      REQUIRE(mdl::canMoveSelectedNodesToLayer(map, layer));
      mdl::moveSelectedNodesToLayer(map, layer);
      REQUIRE(brush->parent() == layer);
      CHECK(registry.idOf(*brush) == brushId);
      CHECK(registry.idOf(*light) == lightId);
      CHECK(registry.verifyBijection());
    }

    SECTION("reparentNodes")
    {
      REQUIRE(mdl::reparentNodes(map, {{layer, {brush}}}));
      REQUIRE(brush->parent() == layer);
      CHECK(registry.idOf(*brush) == brushId);

      map.undoCommand();
      CHECK(brush->parent() == map.worldNode().defaultLayer());
      CHECK(registry.idOf(*brush) == brushId);
      CHECK(registry.verifyBijection());
    }
  }

  SECTION("beginRecording and endRecording")
  {
    auto registry = McpNodeRegistry{map};
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    auto* layer = addLayer(map, "other");

    SECTION("created nodes")
    {
      registry.beginRecording();
      CHECK(registry.isRecording());
      addBrush(map, vm::bbox3d{{32, 0, 0}, {48, 16, 16}});
      addPointEntity(map, "light", {0, 0, 64});
      const auto changes = registry.endRecording();
      CHECK_FALSE(registry.isRecording());
      CHECK(changes.created == ids({"B4", "E5"}));
      CHECK(changes.removed.empty());
    }

    SECTION("removed nodes")
    {
      registry.beginRecording();
      mdl::removeNodes(map, {brush});
      const auto changes = registry.endRecording();
      CHECK(changes.created.empty());
      CHECK(changes.removed == ids({"B2"}));
      CHECK(changes.modified == std::vector<QString>{});
    }

    SECTION("created then removed nodes are dropped")
    {
      registry.beginRecording();
      auto* temporary = addBrush(map, vm::bbox3d{{32, 0, 0}, {48, 16, 16}});
      mdl::removeNodes(map, {temporary});
      const auto changes = registry.endRecording();
      CHECK(changes.created.empty());
      CHECK(changes.removed.empty());
    }

    SECTION("reparented nodes are neither created nor removed")
    {
      registry.beginRecording();
      REQUIRE(mdl::reparentNodes(map, {{layer, {brush}}}));
      const auto changes = registry.endRecording();
      CHECK(changes.created.empty());
      CHECK(changes.removed.empty());
    }

    SECTION("modified nodes exclude created nodes")
    {
      registry.beginRecording();
      auto* other = addBrush(map, vm::bbox3d{{32, 0, 0}, {48, 16, 16}});
      mdl::selectNodes(map, {brush, other});
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
      const auto changes = registry.endRecording();
      CHECK(changes.created == ids({"B4"}));
      CHECK(
        std::ranges::find(changes.modified, QStringLiteral("B2"))
        != changes.modified.end());
      CHECK(
        std::ranges::find(changes.modified, QStringLiteral("B4"))
        == changes.modified.end());
    }

    SECTION("world modifications come first")
    {
      registry.beginRecording();
      mdl::deselectAll(map);
      REQUIRE(mdl::setEntityProperty(map, "message", "hello"));
      mdl::selectNodes(map, {brush});
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
      const auto changes = registry.endRecording();
      REQUIRE_FALSE(changes.modified.empty());
      CHECK(changes.modified.front() == QStringLiteral("world"));
    }

    SECTION("toJson")
    {
      const auto changes = McpChangeSet{
        .created = ids({"B4"}), .removed = ids({"E2", "B3"}), .modified = {}};
      CHECK(
        compactJson(changes.toJson())
        == QStringLiteral(R"({"created":["B4"],"modified":[],"removed":["E2","B3"]})"));
    }

    SECTION("endRecording without beginRecording")
    {
      const auto changes = registry.endRecording();
      CHECK(changes.created.empty());
      CHECK(changes.removed.empty());
      CHECK(changes.modified.empty());
    }
  }

  SECTION("restoreIdsAfterRollback")
  {
    auto registry = McpNodeRegistry{map};
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    auto* entity =
      addBrushEntity(map, "func_door", {vm::bbox3d{{32, 0, 0}, {48, 16, 16}}});
    const auto entityBrush = entity->children().front();
    CHECK(registry.idOf(*brush) == "B2");
    CHECK(registry.idOf(*entity) == "E3");
    CHECK(registry.idOf(*entityBrush) == "B4");

    SECTION("removed and re-added nodes get their ids back")
    {
      registry.beginRecording();
      {
        auto transaction = mdl::Transaction{map, "test"};
        mdl::removeNodes(map, {brush, entity});
        addBrush(map, vm::bbox3d{{64, 0, 0}, {80, 16, 16}});
        transaction.cancel();
      }

      // the rollback re-added the nodes with new ids
      CHECK(registry.idOf(*brush) != "B2");
      registry.restoreIdsAfterRollback();
      CHECK(registry.idOf(*brush) == "B2");
      CHECK(registry.idOf(*entity) == "E3");
      CHECK(registry.idOf(*entityBrush) == "B4");
      CHECK(registry.lookup(u"B2").status == McpNodeRegistry::LookupStatus::Live);
      CHECK(registry.verifyBijection());

      const auto changes = registry.endRecording();
      CHECK(changes.created.empty());
      CHECK(changes.removed.empty());
    }

    SECTION("nodes that were not re-added stay removed")
    {
      registry.beginRecording();
      mdl::removeNodes(map, {brush});
      registry.restoreIdsAfterRollback();
      CHECK(registry.lookup(u"B2").status == McpNodeRegistry::LookupStatus::Stale);
      CHECK(registry.endRecording().removed == ids({"B2"}));
      CHECK(registry.verifyBijection());
    }

    SECTION("without recording")
    {
      registry.restoreIdsAfterRollback();
      CHECK(registry.verifyBijection());
    }
  }

  SECTION("reconcile")
  {
    auto registry = McpNodeRegistry{map};
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    auto* light = addPointEntity(map, "light", {0, 0, 64});
    const auto brushId = registry.idOf(*brush);
    const auto lightId = registry.idOf(*light);

    registry.disconnectForTesting();

    // the removed brush stays alive on the undo stack, the new nodes are unknown
    mdl::removeNodes(map, {brush});
    auto* added = addBrush(map, vm::bbox3d{{32, 0, 0}, {48, 16, 16}});
    auto* door = addBrushEntity(map, "func_door", {vm::bbox3d{{64, 0, 0}, {80, 16, 16}}});
    CHECK_FALSE(registry.verifyBijection());
    CHECK(registry.idOf(*added).isEmpty());

    registry.reconcile();
    CHECK(registry.verifyBijection());
    CHECK(registry.lookup(brushId).status == Status::Stale);
    CHECK(registry.idOf(*light) == lightId);
    CHECK(registry.idOf(*added) == QStringLiteral("B4"));
    CHECK(registry.idOf(*door) == QStringLiteral("E5"));
    CHECK(registry.idOf(*door->children().front()) == QStringLiteral("B6"));

    // reconciling again changes nothing
    registry.reconcile();
    CHECK(registry.idOf(*added) == QStringLiteral("B4"));
    CHECK(registry.liveCount() == 5);
  }

  SECTION("verifyBijection")
  {
    auto registry = McpNodeRegistry{map};
    CHECK(registry.verifyBijection());

    registry.disconnectForTesting();
    addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    CHECK_FALSE(registry.verifyBijection());
  }

  SECTION("aliasing stress")
  {
    auto registry = McpNodeRegistry{map};
    auto rng = std::mt19937{12345};
    const auto random = [&](const size_t n) {
      return std::uniform_int_distribution<size_t>{0, n - 1}(rng);
    };

    // every id must always refer to the same pointer while it is live
    auto pointerById = std::unordered_map<QString, const mdl::Node*>{};
    auto maxNumber = uint64_t{0};

    for (auto step = 0; step < 500; ++step)
    {
      CAPTURE(step);
      const auto nodes = allNodes(map);
      auto geometry = std::vector<mdl::Node*>{};
      for (auto* node : nodes)
      {
        if (!dynamic_cast<mdl::LayerNode*>(node))
        {
          geometry.push_back(node);
        }
      }

      switch (random(6))
      {
      case 0:
      case 1: {
        const auto x = double(random(64)) * 32.0;
        if (random(3) == 0)
        {
          addBrushEntity(
            map,
            "func_door",
            {vm::bbox3d{{x, 0, 0}, {x + 16, 16, 16}},
             vm::bbox3d{{x, 32, 0}, {x + 16, 48, 16}}});
        }
        else
        {
          addBrush(map, vm::bbox3d{{x, 0, 0}, {x + 16, 16, 16}});
        }
        break;
      }
      case 2:
        if (!geometry.empty())
        {
          mdl::deselectAll(map);
          mdl::removeNodes(map, {geometry[random(geometry.size())]});
        }
        break;
      case 3:
        if (map.canUndoCommand())
        {
          map.undoCommand();
        }
        break;
      case 4:
        if (map.redoCommandName())
        {
          map.redoCommand();
        }
        break;
      case 5:
        // a new command clears the redo stack, which frees the nodes it owns
        if (!geometry.empty())
        {
          auto* node = geometry[random(geometry.size())];
          if (map.editorContext().selectable(*node))
          {
            mdl::deselectAll(map);
            mdl::selectNodes(map, {node});
            mdl::translateSelection(map, {0, 0, 8});
            mdl::deselectAll(map);
          }
        }
        break;
      }

      REQUIRE(registry.verifyBijection());

      auto seenIds = std::unordered_set<QString>{};
      const auto maxNumberBefore = maxNumber;
      for (const auto* node : allNodes(map))
      {
        const auto id = registry.idOf(*node);
        REQUIRE_FALSE(id.isEmpty());
        REQUIRE(seenIds.insert(id).second);
        REQUIRE(registry.lookup(id).node == node);

        const auto [it, inserted] = pointerById.emplace(id, node);
        REQUIRE(it->second == node);
        if (inserted)
        {
          // ids are never reused
          const auto number = id.mid(1).toULongLong();
          REQUIRE(number > maxNumberBefore);
          maxNumber = std::max(maxNumber, number);
        }
      }
    }

    // the stress run must have exercised removals and re-additions
    CHECK(registry.tombstoneCount() > 10);
  }

  SECTION("currentChanges")
  {
    auto registry = McpNodeRegistry{map};
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});

    CHECK(registry.currentChanges().created.empty());

    registry.beginRecording();
    addBrush(map, vm::bbox3d{{32, 0, 0}, {48, 16, 16}});
    const auto first = registry.currentChanges();
    CHECK(first.created == ids({"B3"}));
    CHECK(registry.isRecording());

    mdl::removeNodes(map, {brush});
    const auto second = registry.currentChanges();
    CHECK(second.created == ids({"B3"}));
    CHECK(second.removed == ids({"B2"}));

    // currentChanges does not stop or reset the recording
    const auto changes = registry.endRecording();
    CHECK(changes.created == second.created);
    CHECK(changes.removed == second.removed);
    CHECK(changes.modified == second.modified);
  }
}

} // namespace tb::ui
