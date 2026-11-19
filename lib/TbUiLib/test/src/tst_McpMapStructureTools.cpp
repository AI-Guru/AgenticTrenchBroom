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
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "ui/CatchConfig.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("McpMapStructureTools")
{
  auto f = McpMapToolsFixture{};
  const auto scene = populateTestScene(f);
  const auto doorBrushes =
    std::vector<mdl::Node*>{scene.door->children().begin(), scene.door->children().end()};

  SECTION("select_nodes")
  {
    SECTION("replace")
    {
      mdl::selectNodes(f.map, {scene.patch});
      const auto result = f.ok("select_nodes", R"({"ids":["B2","E3"]})");
      CHECK(result.value("nodes") == QJsonValue{QJsonArray{"B2", "E3"}});
      CHECK(result.value("faces") == QJsonValue{QJsonArray{}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Select Nodes"});
      // the selection is the result and is kept
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.wall, scene.light});

      // undo restores the previous selection
      f.ok("undo");
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.patch});
    }

    SECTION("brush entities select their brushes")
    {
      const auto result = f.ok("select_nodes", R"({"ids":["E4"]})");
      CHECK(result.value("nodes") == QJsonValue{QJsonArray{"B5", "B6"}});
      CHECK(f.map.selection().nodes == doorBrushes);
    }

    SECTION("add and remove")
    {
      f.ok("select_nodes", R"({"ids":["B2"]})");
      const auto added = f.ok("select_nodes", R"({"ids":["E3","B2"],"mode":"add"})");
      CHECK(added.value("nodes") == QJsonValue{QJsonArray{"B2", "E3"}});

      const auto removed = f.ok("select_nodes", R"({"ids":["B2"],"mode":"remove"})");
      CHECK(removed.value("nodes") == QJsonValue{QJsonArray{"E3"}});

      // removing nodes that are not selected changes nothing
      const auto unchanged = f.ok("select_nodes", R"({"ids":["P9"],"mode":"remove"})");
      CHECK(unchanged.value("nodes") == QJsonValue{QJsonArray{"E3"}});
    }

    SECTION("clear")
    {
      mdl::selectNodes(f.map, {scene.patch});
      const auto result = f.ok("select_nodes", R"({"mode":"clear"})");
      CHECK(result.value("nodes") == QJsonValue{QJsonArray{}});
      CHECK(result.value("bounds").isNull());
      CHECK(f.map.selection().nodes.empty());
    }

    SECTION("faces")
    {
      const auto result = f.ok(
        "select_nodes",
        R"({"faces":[{"brush":"B2","side":"+z"},{"brush":"B7","index":0}]})");
      const auto faces = result.value("faces").toArray();
      REQUIRE(faces.size() == 2);
      CHECK(faces[0].toObject().value("brush") == QJsonValue{"B2"});
      CHECK(faces[0].toObject().value("side") == QJsonValue{"+z"});
      CHECK(faces[1].toObject().value("index") == QJsonValue{0});
      CHECK(f.map.selection().brushFaces.size() == 2);

      const auto removed =
        f.ok("select_nodes", R"({"faces":[{"brush":"B7","index":0}],"mode":"remove"})");
      CHECK(removed.value("faces").toArray().size() == 1);

      const auto added =
        f.ok("select_nodes", R"({"faces":[{"brush":"B7","index":1}],"mode":"add"})");
      CHECK(added.value("faces").toArray().size() == 2);

      // nodes cannot be added to a face selection and vice versa
      CHECK(
        f.err("select_nodes", R"({"ids":["B2"],"mode":"add"})") == "invalid_argument");
      f.ok("select_nodes", R"({"ids":["B2"]})");
      CHECK(
        f.err("select_nodes", R"({"faces":[{"brush":"B2","index":0}],"mode":"add"})")
        == "invalid_argument");
      CHECK(f.map.selection().brushFaces.empty());
    }

    SECTION("errors")
    {
      mdl::selectNodes(f.map, {scene.patch});
      const auto before = snapshot(f.map);
      CHECK(f.err("select_nodes", R"({})") == "invalid_argument");
      CHECK(f.err("select_nodes", R"({"mode":"add"})") == "invalid_argument");
      CHECK(
        f.err("select_nodes", R"({"ids":["B2"],"mode":"clear"})") == "invalid_argument");
      CHECK(
        f.err("select_nodes", R"({"ids":["B2"],"faces":[{"brush":"B2","index":0}]})")
        == "invalid_argument");
      CHECK(f.err("select_nodes", R"({"ids":["world"]})") == "wrong_node_type");
      CHECK(f.err("select_nodes", R"({"ids":["B7"]})") == "node_not_selectable");
      CHECK(f.err("select_nodes", R"({"ids":["B99"]})") == "unknown_node");
      CHECK(
        f.err("select_nodes", R"({"faces":[{"brush":"B2","index":99}]})")
        == "face_not_found");
      CHECK(
        f.err("select_nodes", R"({"ids":["B2"],"mode":"toggle"})") == "invalid_argument");

      mdl::hideLayers(f.map, {scene.layer});
      CHECK(f.err("select_nodes", R"({"ids":["E11"]})") == "node_not_selectable");
      CHECK(f.map.selection().nodes == before.selectedNodes);
    }
  }

  const auto node = [&](const QString& id) {
    return f.ok("get_nodes", QJsonObject{{"ids", QJsonArray{id}}})
      .value("nodes")
      .toArray()[0]
      .toObject();
  };

  SECTION("group_nodes")
  {
    SECTION("groups nodes and keeps their ids")
    {
      const auto result = f.ok("group_nodes", R"({"ids":["B2","E3"],"name":"stuff"})");
      CHECK(result.value("group") == QJsonValue{"G12"});
      CHECK(result.value("children") == QJsonValue{QJsonArray{"B2", "E3"}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Group Nodes"});
      CHECK(node("G12").value("name") == QJsonValue{"stuff"});
      CHECK(node("B2").value("parent") == QJsonValue{"G12"});
      CHECK(f.map.selection().nodes.empty());
    }

    SECTION("brush entities are grouped as a whole")
    {
      const auto result = f.ok("group_nodes", R"({"ids":["E4"],"name":"door"})");
      CHECK(result.value("children") == QJsonValue{QJsonArray{"E4"}});
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(
        f.err("group_nodes", R"({"ids":["B2"],"name":"a\"b"})") == "invalid_argument");
      CHECK(f.err("group_nodes", R"({"ids":["B2"],"name":""})") == "invalid_argument");
      CHECK(f.err("group_nodes", R"({"ids":["L1"],"name":"x"})") == "wrong_node_type");
      CHECK(
        f.err("group_nodes", R"({"ids":["B7"],"name":"x"})") == "node_not_selectable");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("ungroup_nodes")
  {
    SECTION("dissolves groups and keeps the children's ids")
    {
      const auto result = f.ok("ungroup_nodes", R"({"ids":["G8"]})");
      CHECK(result.value("ungrouped") == QJsonValue{QJsonArray{"G8"}});
      CHECK(result.value("children") == QJsonValue{QJsonArray{"B7"}});
      CHECK(node("B7").value("parent") == QJsonValue{"L1"});
      CHECK(f.err("get_nodes", R"({"ids":["G8"]})") == "stale_node");
      CHECK(f.map.selection().nodes.empty());
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(f.err("ungroup_nodes", R"({"ids":["B2"]})") == "wrong_node_type");
      CHECK(f.err("ungroup_nodes", R"({"ids":["G999"]})") == "unknown_node");
      mdl::selectNodes(f.map, {scene.group});
      REQUIRE(mdl::createLinkedDuplicate(f.map) != nullptr);
      mdl::deselectAll(f.map);
      CHECK(f.err("ungroup_nodes", R"({"ids":["G8"]})") == "not_supported");
      (void)before;
    }
  }

  SECTION("create_layer")
  {
    SECTION("adds a layer at the bottom")
    {
      const auto result = f.ok("create_layer", R"({"name":"decor"})");
      CHECK(result.value("id") == QJsonValue{"L12"});
      CHECK(result.value("current") == QJsonValue{false});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Layer"});
      CHECK(node("L12").value("name") == QJsonValue{"decor"});
      CHECK(
        node("L12").value("sort_index").toInt()
        > node("L10").value("sort_index").toInt());
      CHECK(f.ok("get_map_info").value("current_layer") == QJsonValue{"L1"});
    }

    SECTION("makes the layer current")
    {
      f.ok("create_layer", R"({"name":"decor","make_current":true})");
      CHECK(f.ok("get_map_info").value("current_layer") == QJsonValue{"L12"});
      f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
      CHECK(node("B13").value("parent") == QJsonValue{"L12"});
    }

    SECTION("warns about duplicate names")
    {
      const auto result = f.ok("create_layer", R"({"name":"details"})");
      CHECK(compactJson(result.value("warnings")).contains("already a layer"));
    }

    SECTION("errors")
    {
      CHECK(f.err("create_layer", R"({"name":""})") == "invalid_argument");
      CHECK(f.err("create_layer", R"({"name":"a\nb"})") == "invalid_argument");
      CHECK(f.err("create_layer") == "invalid_argument");
    }
  }

  SECTION("set_current_layer")
  {
    const auto result = f.ok("set_current_layer", R"({"id":"L10"})");
    CHECK(result.value("current_layer") == QJsonValue{"L10"});
    CHECK(result.value("undo_label") == QJsonValue{"MCP: Set Current Layer"});
    CHECK(f.ok("get_map_info").value("current_layer") == QJsonValue{"L10"});

    CHECK(f.err("set_current_layer", R"({"id":"L10"})") == "operation_failed");
    CHECK(f.err("set_current_layer", R"({"id":"B2"})") == "wrong_node_type");
    CHECK(f.err("set_current_layer", R"({"id":"world"})") == "wrong_node_type");
    CHECK(f.err("set_current_layer", R"({"id":"L999"})") == "unknown_node");

    f.ok("set_current_layer", R"({"id":"default_layer"})");
    CHECK(f.ok("get_map_info").value("current_layer") == QJsonValue{"L1"});

    // the current layer is not part of the map's content, but it is one undo step
    f.map.undoCommand();
    CHECK(f.ok("get_map_info").value("current_layer") == QJsonValue{"L10"});
  }

  SECTION("move_to_layer")
  {
    SECTION("moves nodes and keeps their ids")
    {
      const auto result = f.ok("move_to_layer", R"({"ids":["B2","G8"],"layer":"L10"})");
      CHECK(result.value("layer") == QJsonValue{"L10"});
      CHECK(result.value("moved") == QJsonValue{QJsonArray{"B2", "G8"}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Move To Layer"});
      CHECK(node("B2").value("parent") == QJsonValue{"L10"});
      CHECK(node("B7").value("layer") == QJsonValue{"L10"});
      CHECK(f.map.selection().nodes.empty());
    }

    SECTION("brush entities move as a whole")
    {
      f.ok("move_to_layer", R"({"ids":["B5"],"layer":"L10"})");
      CHECK(node("E4").value("parent") == QJsonValue{"L10"});
      CHECK(node("B6").value("layer") == QJsonValue{"L10"});
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(
        f.err("move_to_layer", R"({"ids":["E11"],"layer":"L10"})") == "operation_failed");
      CHECK(
        f.err("move_to_layer", R"({"ids":["B2"],"layer":"B2"})") == "wrong_node_type");
      CHECK(f.err("move_to_layer", R"({"ids":["B2"],"layer":"L99"})") == "unknown_node");
      CHECK(
        f.err("move_to_layer", R"({"ids":["B7"],"layer":"L10"})")
        == "node_not_selectable");
      CHECK(
        f.err("move_to_layer", R"({"ids":["L1"],"layer":"L10"})") == "wrong_node_type");
      CHECK(snapshot(f.map) == before);
    }
  }
}

} // namespace tb::ui
