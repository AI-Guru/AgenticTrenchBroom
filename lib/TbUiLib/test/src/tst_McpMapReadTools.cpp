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
#include "base/Color.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"

#include "vm/vec.h"

#include <algorithm>
#include <utility>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

QStringList strings(const QJsonValue& value)
{
  auto result = QStringList{};
  for (const auto& item : value.toArray())
  {
    result.push_back(item.toString());
  }
  return result;
}

QStringList nodeIds(const QJsonObject& sceneTree)
{
  auto result = QStringList{};
  for (const auto& node : sceneTree.value("nodes").toArray())
  {
    result.push_back(node.toObject().value("id").toString());
  }
  return result;
}

QJsonObject findById(const QJsonArray& nodes, const QString& id)
{
  for (const auto& node : nodes)
  {
    if (node.toObject().value("id") == QJsonValue{id})
    {
      return node.toObject();
    }
  }
  FAIL("node " << id.toStdString() << " not found");
  return {};
}

void setTestDefinitions(mdl::Map& map)
{
  using namespace mdl::PropertyValueTypes;
  map.entityDefinitionManager().setDefinitions({
    mdl::EntityDefinition{
      "light",
      tb::Color{},
      "A light.\nIt shines.",
      {
        {"light", Integer{300}, "Brightness", ""},
        {"style", Choice{{{"0", "normal"}, {"1", "flicker"}}, "0"}, "Style", ""},
        {"spawnflags", Flags{{{1, "Start off", ""}}, 0}, "", ""},
        {"target", LinkSource{}, "", ""},
      },
      mdl::PointEntityDefinition{vm::bbox3d{8.0}, {}, {}},
    },
    mdl::EntityDefinition{
      "info_player_start",
      tb::Color{},
      "Player start.",
      {},
      mdl::PointEntityDefinition{vm::bbox3d{{-16, -16, -24}, {16, 16, 32}}, {}, {}},
    },
    mdl::EntityDefinition{"func_door", tb::Color{}, "A door.", {}},
  });
}

} // namespace

TEST_CASE("McpMapReadTools")
{
  auto f = McpMapToolsFixture{};

  SECTION("get_map_info")
  {
    SECTION("empty map")
    {
      const auto info = f.ok("get_map_info");
      CHECK(info.value("document") == QJsonValue{"d1"});
      CHECK(info.value("game") == QJsonValue{"Quake"});
      CHECK(info.value("format") == QJsonValue{"Valve"});
      CHECK(info.value("path").isNull());
      CHECK(info.value("persistent") == QJsonValue{false});
      CHECK(info.value("modified") == QJsonValue{false});
      CHECK(info.value("encoding") == QJsonValue{"quake"});
      CHECK(
        compactJson(info.value("world_bounds"))
        == QStringLiteral(R"({"max":[8129,8129,8129],"min":[-8129,-8129,-8129]})"));
      CHECK(
        compactJson(info.value("soft_map_bounds"))
        == QStringLiteral(
          R"({"bounds":{"max":[4096,4096,4096],"min":[-4096,-4096,-4096]},"source":"game"})"));
      CHECK(info.value("grid_size") == QJsonValue{16});
      CHECK(info.value("current_layer") == QJsonValue{"L1"});
      CHECK(info.value("current_group").isNull());
      CHECK(info.value("can_undo") == QJsonValue{false});
      CHECK(info.value("undo_label").isNull());
      CHECK(info.value("can_redo") == QJsonValue{false});
      CHECK(info.value("redo_label").isNull());
      CHECK(info.value("transaction_active") == QJsonValue{false});
      CHECK(
        compactJson(info.value("selection"))
        == QStringLiteral(R"({"bounds":null,"face_count":0,"node_count":0})"));
      CHECK(
        compactJson(info.value("counts"))
        == QStringLiteral(
          R"({"brushes":0,"entities":0,"groups":0,"layers":1,"patches":0})"));
    }

    SECTION("populated map")
    {
      const auto scene = populateTestScene(f);
      mdl::selectNodes(f.map, {scene.wall});

      const auto info = f.ok("get_map_info");
      CHECK(info.value("modified") == QJsonValue{true});
      CHECK(info.value("can_undo") == QJsonValue{true});
      CHECK(info.value("undo_label").isString());
      // regression: the redo stack is empty even though the undo stack is not
      CHECK(info.value("can_redo") == QJsonValue{false});
      CHECK(
        compactJson(info.value("counts"))
        == QStringLiteral(
          R"({"brushes":4,"entities":3,"groups":1,"layers":2,"patches":1})"));
      CHECK(
        compactJson(info.value("selection"))
        == QStringLiteral(
          R"({"bounds":{"max":[64,64,64],"min":[0,0,0]},"face_count":0,"node_count":1})"));

      mdl::deselectAll(f.map);
      f.map.undoCommand();
      const auto afterUndo = f.ok("get_map_info");
      CHECK(afterUndo.value("can_redo") == QJsonValue{true});
      CHECK(afterUndo.value("redo_label").isString());

      mdl::openGroup(f.map, *scene.group);
      CHECK(f.ok("get_map_info").value("current_group") == QJsonValue{"G8"});
      mdl::closeGroup(f.map);
    }

    SECTION("during a transaction")
    {
      populateTestScene(f);
      f.map.startTransaction("drag", mdl::TransactionScope::LongRunning);
      const auto info = f.ok("get_map_info");
      CHECK(info.value("transaction_active") == QJsonValue{true});
      CHECK(info.value("can_undo") == QJsonValue{false});
      CHECK(info.value("undo_label").isNull());
      f.map.cancelTransaction();
    }
  }

  SECTION("get_scene_tree")
  {
    populateTestScene(f);

    SECTION("default")
    {
      const auto tree = f.ok("get_scene_tree");
      CHECK(nodeIds(tree) == QStringList{"world", "L1", "E3", "E4", "G8", "L10", "E11"});
      CHECK(tree.value("total") == QJsonValue{7});
      CHECK(tree.value("truncated") == QJsonValue{false});
      CHECK(tree.value("next_offset").isNull());

      const auto nodes = tree.value("nodes").toArray();
      const auto layer = findById(nodes, "L1");
      CHECK(layer.value("depth") == QJsonValue{1});
      CHECK(layer.value("brush_count") == QJsonValue{1});
      CHECK(layer.value("patch_count") == QJsonValue{1});
      CHECK(findById(nodes, "E4").value("brush_count") == QJsonValue{2});
      CHECK(findById(nodes, "G8").value("depth") == QJsonValue{2});
      CHECK(findById(nodes, "G8").value("brush_count") == QJsonValue{1});
      CHECK_FALSE(findById(nodes, "G8").contains("child_ids"));
    }

    SECTION("include_brushes")
    {
      const auto tree = f.ok("get_scene_tree", R"({"include_brushes":true})");
      CHECK(
        nodeIds(tree)
        == QStringList{
          "world", "L1", "B2", "E3", "E4", "B5", "B6", "G8", "B7", "P9", "L10", "E11"});
      CHECK_FALSE(findById(tree.value("nodes").toArray(), "L1").contains("brush_count"));
    }

    SECTION("max_depth lists the child ids at the cutoff")
    {
      const auto tree = f.ok("get_scene_tree", R"({"max_depth":1})");
      CHECK(nodeIds(tree) == QStringList{"world", "L1", "L10"});
      const auto nodes = tree.value("nodes").toArray();
      CHECK(
        strings(findById(nodes, "L1").value("child_ids"))
        == QStringList{"E3", "E4", "G8"});
      CHECK(strings(findById(nodes, "L10").value("child_ids")) == QStringList{"E11"});

      const auto rootOnly = f.ok("get_scene_tree", R"({"max_depth":0})");
      CHECK(nodeIds(rootOnly) == QStringList{"world"});
      CHECK(
        strings(rootOnly.value("nodes").toArray()[0].toObject().value("child_ids"))
        == QStringList{"L1", "L10"});
    }

    SECTION("root")
    {
      const auto tree = f.ok("get_scene_tree", R"({"root":"G8","include_brushes":true})");
      CHECK(nodeIds(tree) == QStringList{"G8", "B7"});
      CHECK(tree.value("nodes").toArray()[0].toObject().value("depth") == QJsonValue{0});

      CHECK(f.err("get_scene_tree", R"({"root":"G99"})") == "unknown_node");
      CHECK(f.err("get_scene_tree", R"({"root":"@selection"})") == "invalid_argument");
      CHECK(f.err("get_scene_tree", R"({"root":"G7"})") == "wrong_node_type");
    }

    SECTION("types")
    {
      CHECK(
        nodeIds(f.ok("get_scene_tree", R"({"types":["entity"]})"))
        == QStringList{"E3", "E4", "E11"});
      CHECK(
        nodeIds(f.ok("get_scene_tree", R"({"types":["brush","patch"]})"))
        == QStringList{"B2", "B5", "B6", "B7", "P9"});
    }

    SECTION("paging")
    {
      const auto first = f.ok("get_scene_tree", R"({"limit":3})");
      CHECK(nodeIds(first) == QStringList{"world", "L1", "E3"});
      CHECK(first.value("total") == QJsonValue{7});
      CHECK(first.value("truncated") == QJsonValue{true});
      CHECK(first.value("next_offset") == QJsonValue{3});

      const auto second = f.ok("get_scene_tree", R"({"offset":3,"limit":3})");
      CHECK(nodeIds(second) == QStringList{"E4", "G8", "L10"});
      CHECK(second.value("next_offset") == QJsonValue{6});

      const auto last = f.ok("get_scene_tree", R"({"offset":6})");
      CHECK(nodeIds(last) == QStringList{"E11"});
      CHECK(last.value("next_offset").isNull());

      const auto beyond = f.ok("get_scene_tree", R"({"offset":1e300})");
      CHECK(nodeIds(beyond).isEmpty());
      CHECK(beyond.value("total") == QJsonValue{7});
      CHECK(beyond.value("truncated") == QJsonValue{false});

      CHECK(f.err("get_scene_tree", R"({"limit":0})") == "invalid_argument");
      CHECK(f.err("get_scene_tree", R"({"limit":5001})") == "invalid_argument");
      CHECK(f.err("get_scene_tree", R"({"offset":-1})") == "invalid_argument");
      CHECK(f.err("get_scene_tree", R"({"max_depth":65})") == "invalid_argument");
    }
  }

  SECTION("get_nodes")
  {
    const auto scene = populateTestScene(f);

    SECTION("world")
    {
      const auto world =
        f.ok("get_nodes", R"({"ids":["world"]})").value("nodes").toArray()[0].toObject();
      CHECK(world.value("type") == QJsonValue{"world"});
      CHECK(strings(world.value("child_ids")) == QStringList{"L1", "L10"});
      const auto properties = world.value("properties").toArray();
      REQUIRE_FALSE(properties.isEmpty());
      CHECK(
        properties[0].toObject()
        == QJsonObject{{"key", "classname"}, {"value", "worldspawn"}});
    }

    SECTION("layer")
    {
      const auto nodes =
        f.ok("get_nodes", R"({"ids":["default_layer","L10"]})").value("nodes").toArray();
      REQUIRE(nodes.size() == 2);
      const auto defaultLayer = nodes[0].toObject();
      CHECK(defaultLayer.value("id") == QJsonValue{"L1"});
      CHECK(defaultLayer.value("is_default") == QJsonValue{true});
      CHECK(defaultLayer.value("hidden") == QJsonValue{false});
      CHECK(defaultLayer.value("omit_from_export") == QJsonValue{false});

      const auto custom = nodes[1].toObject();
      CHECK(custom.value("name") == QJsonValue{"details"});
      CHECK(custom.value("is_default") == QJsonValue{false});
      CHECK(custom.value("sort_index") == QJsonValue{0});

      mdl::hideLayers(f.map, {scene.layer});
      const auto hidden =
        f.ok("get_nodes", R"({"ids":["L10"]})").value("nodes").toArray()[0].toObject();
      CHECK(hidden.value("hidden") == QJsonValue{true});
      CHECK(hidden.value("visible") == QJsonValue{false});
    }

    SECTION("group")
    {
      const auto group =
        f.ok("get_nodes", R"({"ids":["G8"]})").value("nodes").toArray()[0].toObject();
      CHECK(group.value("name") == QJsonValue{"room"});
      CHECK(group.value("opened") == QJsonValue{false});
      CHECK(group.value("linked") == QJsonValue{false});
      CHECK(
        compactJson(group.value("transformation"))
        == QStringLiteral("[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]"));
      CHECK(strings(group.value("child_ids")) == QStringList{"B7"});

      mdl::selectNodes(f.map, {scene.group});
      REQUIRE(mdl::createLinkedDuplicate(f.map) != nullptr);
      mdl::deselectAll(f.map);
      CHECK(
        f.ok("get_nodes", R"({"ids":["G8"]})")
          .value("nodes")
          .toArray()[0]
          .toObject()
          .value("linked")
        == QJsonValue{true});
    }

    SECTION("entities")
    {
      const auto nodes =
        f.ok("get_nodes", R"({"ids":["E3","E4"]})").value("nodes").toArray();
      const auto light = nodes[0].toObject();
      CHECK(light.value("classname") == QJsonValue{"light"});
      CHECK(compactJson(light.value("origin")) == QStringLiteral("[32,32,128]"));
      CHECK(light.value("point_entity") == QJsonValue{true});
      CHECK(
        compactJson(light.value("properties"))
        == QStringLiteral(R"([{"key":"classname","value":"light"},)"
                          R"({"key":"origin","value":"32 32 128"},)"
                          R"({"key":"light","value":"300"},)"
                          R"({"key":"_color","value":"1 1 1"}])"));
      CHECK(
        compactJson(light.value("definition")) == QStringLiteral(R"({"known":false})"));

      const auto door = nodes[1].toObject();
      CHECK(door.value("point_entity") == QJsonValue{false});
      CHECK(strings(door.value("child_ids")) == QStringList{"B5", "B6"});
      CHECK(door.value("child_count") == QJsonValue{2});
    }

    SECTION("brush and patch")
    {
      const auto nodes =
        f.ok("get_nodes", R"({"ids":["B5","P9"]})").value("nodes").toArray();
      const auto brush = nodes[0].toObject();
      CHECK(brush.value("entity") == QJsonValue{"E4"});
      CHECK(brush.value("parent") == QJsonValue{"E4"});
      CHECK(brush.value("face_count") == QJsonValue{6});
      CHECK(
        compactJson(brush.value("valid"))
        == QStringLiteral(R"({"closed":true,"fully_specified":true,)"
                          R"("integer_vertices":true,"within_world_bounds":true})"));
      CHECK(
        compactJson(brush.value("logical_bounds"))
        == QStringLiteral(R"({"max":[144,16,64],"min":[128,0,0]})"));

      const auto patch = nodes[1].toObject();
      CHECK(patch.value("rows") == QJsonValue{3});
      CHECK(patch.value("columns") == QJsonValue{3});
      CHECK(patch.value("material") == QJsonValue{"patch_material"});
    }

    SECTION("@selection and duplicates")
    {
      mdl::selectNodes(f.map, {scene.wall, scene.light});
      const auto nodes =
        f.ok("get_nodes", R"({"ids":["@selection","B2","L1"]})").value("nodes").toArray();
      CHECK(nodes.size() == 3);
      CHECK(nodes[0].toObject().value("selected") == QJsonValue{true});
    }

    SECTION("errors")
    {
      CHECK(f.err("get_nodes", R"({"ids":["B99"]})") == "unknown_node");
      CHECK(f.err("get_nodes", R"({"ids":["E2"]})") == "wrong_node_type");
      CHECK(f.err("get_nodes", R"({"ids":[]})") == "invalid_argument");
      CHECK(f.err("get_nodes", R"({"ids":["@selection"]})") == "invalid_argument");
      CHECK(f.err("get_nodes", R"({})") == "invalid_argument");

      mdl::removeNodes(f.map, {scene.wall});
      CHECK(f.err("get_nodes", R"({"ids":["B2"]})") == "stale_node");
    }
  }

  SECTION("get_brush_geometry")
  {
    const auto scene = populateTestScene(f);

    SECTION("64 unit cube")
    {
      const auto brushes =
        f.ok("get_brush_geometry", R"({"ids":["B2"]})").value("brushes").toArray();
      REQUIRE(brushes.size() == 1);
      const auto brush = brushes[0].toObject();
      CHECK(brush.value("id") == QJsonValue{"B2"});
      CHECK(brush.value("entity") == QJsonValue{"world"});
      CHECK(
        compactJson(brush.value("bounds"))
        == QStringLiteral(R"({"max":[64,64,64],"min":[0,0,0]})"));
      CHECK(compactJson(brush.value("size")) == QStringLiteral("[64,64,64]"));
      CHECK(compactJson(brush.value("center")) == QStringLiteral("[32,32,32]"));
      CHECK(brush.value("volume") == QJsonValue{262144});
      CHECK(brush.value("vertex_count") == QJsonValue{8});
      CHECK(brush.value("edge_count") == QJsonValue{12});
      CHECK(brush.value("face_count") == QJsonValue{6});
      CHECK(
        compactJson(brush.value("valid"))
        == QStringLiteral(R"({"closed":true,"fully_specified":true,)"
                          R"("integer_vertices":true,"within_world_bounds":true})"));
      CHECK(
        compactJson(brush.value("vertices"))
        == QStringLiteral("[[0,0,0],[0,0,64],[0,64,0],[0,64,64],"
                          "[64,0,0],[64,0,64],[64,64,0],[64,64,64]]"));

      const auto faces = brush.value("faces").toArray();
      REQUIRE(faces.size() == 6);
      auto sides = QStringList{};
      for (const auto& faceValue : faces)
      {
        const auto face = faceValue.toObject();
        sides.push_back(face.value("side").toString());
        CHECK(face.value("area") == QJsonValue{4096});
        CHECK(face.value("material") == QJsonValue{"wall"});
        CHECK(face.value("material_found") == QJsonValue{false});
        CHECK(face.value("vertices").toArray().size() == 4);
        CHECK(face.value("plane_points").toArray().size() == 3);
        CHECK(face.value("uv").toObject().value("valid") == QJsonValue{true});
        CHECK_FALSE(face.contains("surface"));

        const auto side = face.value("side").toString();
        const auto distance = face.value("distance").toDouble();
        CHECK(distance == (side.startsWith('+') ? 64.0 : 0.0));
      }
      sides.sort();
      CHECK(sides == QStringList{"+x", "+y", "+z", "-x", "-y", "-z"});
    }

    SECTION("include_vertices and include_faces")
    {
      const auto brush =
        f.ok(
           "get_brush_geometry",
           R"({"ids":["B2"],"include_vertices":false,"include_faces":true})")
          .value("brushes")
          .toArray()[0]
          .toObject();
      CHECK_FALSE(brush.contains("vertices"));
      CHECK_FALSE(brush.value("faces").toArray()[0].toObject().contains("vertices"));

      const auto noFaces =
        f.ok("get_brush_geometry", R"({"ids":["B2"],"include_faces":false})")
          .value("brushes")
          .toArray()[0]
          .toObject();
      CHECK_FALSE(noFaces.contains("faces"));
      CHECK(noFaces.contains("vertices"));
    }

    SECTION("entity brushes and the selection")
    {
      mdl::selectNodes(f.map, {scene.door->children().front()});
      const auto brushes = f.ok("get_brush_geometry", R"({"ids":["@selection","B7"]})")
                             .value("brushes")
                             .toArray();
      REQUIRE(brushes.size() == 2);
      CHECK(brushes[0].toObject().value("entity") == QJsonValue{"E4"});
      CHECK(brushes[1].toObject().value("id") == QJsonValue{"B7"});
    }

    SECTION("non-integer vertices")
    {
      mdl::selectNodes(f.map, {scene.wall});
      REQUIRE(mdl::rotateSelection(f.map, {32, 32, 32}, {0, 0, 1}, vm::to_radians(45.0)));
      mdl::deselectAll(f.map);

      const auto brush = f.ok("get_brush_geometry", R"({"ids":["B2"]})")
                           .value("brushes")
                           .toArray()[0]
                           .toObject();
      CHECK(
        brush.value("valid").toObject().value("integer_vertices") == QJsonValue{false});
      // the four side faces are no longer axis aligned
      auto sideCount = 0;
      for (const auto& face : brush.value("faces").toArray())
      {
        sideCount += face.toObject().contains("side") ? 1 : 0;
      }
      CHECK(sideCount == 2);
    }

    SECTION("errors")
    {
      const auto error = f.error("get_brush_geometry", R"({"ids":["B2","E3"]})");
      CHECK(error.value("code") == QJsonValue{"wrong_node_type"});
      CHECK(error.value("details").toObject().value("actual") == QJsonValue{"entity"});
      CHECK(f.err("get_brush_geometry", R"({"ids":["P9"]})") == "wrong_node_type");
      CHECK(f.err("get_brush_geometry", R"({"ids":["B99"]})") == "unknown_node");
    }
  }

  SECTION("find_nodes")
  {
    const auto scene = populateTestScene(f);
    const auto find = [&](const char* args) {
      return strings(f.ok("find_nodes", args).value("ids"));
    };

    SECTION("without filters")
    {
      CHECK(
        find("{}")
        == QStringList{
          "world", "L1", "B2", "E3", "E4", "B5", "B6", "G8", "B7", "P9", "L10", "E11"});
    }

    SECTION("types")
    {
      CHECK(find(R"({"types":["brush"]})") == QStringList{"B2", "B5", "B6", "B7"});
      CHECK(find(R"({"types":["layer","group"]})") == QStringList{"L1", "G8", "L10"});
    }

    SECTION("classname")
    {
      CHECK(find(R"({"classname":"light"})") == QStringList{"E3"});
      CHECK(find(R"({"classname":"FUNC_*"})") == QStringList{"E4"});
      CHECK(find(R"({"classname":"*"})") == QStringList{"world", "E3", "E4", "E11"});
      CHECK(find(R"({"classname":"info_player_?tart"})") == QStringList{"E11"});
    }

    SECTION("name")
    {
      CHECK(find(R"({"name":"room"})") == QStringList{"G8"});
      CHECK(find(R"({"name":"*layer"})") == QStringList{"L1"});
    }

    SECTION("text")
    {
      CHECK(find(R"({"text":"300"})") == QStringList{"E3"});
      CHECK(find(R"({"text":"func_*"})") == QStringList{"E4"});
      CHECK(find(R"({"text":"no such text"})").isEmpty());
      CHECK(find(R"({"text":"FUNC_DOOR"})") == QStringList{"E4"});
      CHECK(find(R"({"text":"room"})") == QStringList{"G8"});
      CHECK(find(R"({"text":"floor"})") == QStringList{"B7"});
    }

    SECTION("text does not backtrack exponentially")
    {
      // Map::findNodes took about 5 minutes for this pattern on a 400 char value
      mdl::selectNodes(f.map, {scene.light});
      REQUIRE(mdl::setEntityProperty(f.map, "message", std::string(4000, 'a')));
      mdl::deselectAll(f.map);

      CHECK(find(R"({"text":"*a*a*a*a*a*a*a*a*b"})").isEmpty());
      CHECK(find(R"({"text":"*a*a*a*a*a*a*a*a*"})") == QStringList{"E3"});
    }

    SECTION("property")
    {
      CHECK(find(R"({"property":{"key":"light"}})") == QStringList{"E3"});
      CHECK(find(R"({"property":{"key":"light","value":"3*"}})") == QStringList{"E3"});
      CHECK(find(R"({"property":{"key":"light","value":"4*"}})").isEmpty());
      CHECK(f.err("find_nodes", R"({"property":{"value":"x"}})") == "invalid_argument");
    }

    SECTION("material")
    {
      CHECK(find(R"({"material":"wall"})") == QStringList{"B2"});
      CHECK(find(R"({"material":"*"})") == QStringList{"B2", "B5", "B6", "B7", "P9"});
      CHECK(find(R"({"material":"patch_*"})") == QStringList{"P9"});
    }

    SECTION("inside and intersects")
    {
      CHECK(
        find(R"({"inside":{"min":[-1,-1,-1],"max":[65,65,65]},"types":["brush"]})")
        == QStringList{"B2"});
      CHECK(
        find(R"({"intersects":{"min":[60,0,0],"max":[130,1,1]},"types":["brush"]})")
        == QStringList{"B2", "B5"});
      CHECK(
        f.err("find_nodes", R"({"inside":{"min":[1,0,0],"max":[0,1,1]}})")
        == "invalid_argument");
      CHECK(
        f.err("find_nodes", R"({"inside":{"min":[0,0,0],"max":[1e7,1,1]}})")
        == "invalid_argument");
    }

    SECTION("under")
    {
      CHECK(find(R"({"under":"E4"})") == QStringList{"B5", "B6"});
      CHECK(find(R"({"under":"L10"})") == QStringList{"E11"});
      CHECK(f.err("find_nodes", R"({"under":"G99"})") == "unknown_node");
    }

    SECTION("selected")
    {
      mdl::selectNodes(f.map, {scene.wall, scene.light});
      CHECK(find(R"({"selected":true})") == QStringList{"B2", "E3"});
      CHECK(find(R"({"selected":false,"types":["entity"]})") == QStringList{"E4", "E11"});
    }

    SECTION("point_entity")
    {
      CHECK(find(R"({"point_entity":true})") == QStringList{"E3", "E11"});
      CHECK(find(R"({"point_entity":false})") == QStringList{"E4"});
    }

    SECTION("conjunction and paging")
    {
      CHECK(
        find(R"({"types":["entity"],"under":"L1","point_entity":true})")
        == QStringList{"E3"});

      const auto page = f.ok("find_nodes", R"({"types":["brush"],"offset":1,"limit":2})");
      CHECK(strings(page.value("ids")) == QStringList{"B5", "B6"});
      CHECK(page.value("total") == QJsonValue{4});
      CHECK(page.value("truncated") == QJsonValue{true});
      CHECK(page.value("next_offset") == QJsonValue{3});
    }
  }

  SECTION("get_selection")
  {
    const auto scene = populateTestScene(f);

    CHECK(
      compactJson(f.ok("get_selection"))
      == QStringLiteral(R"({"bounds":null,"faces":[],"nodes":[]})"));

    mdl::selectNodes(f.map, {scene.wall, scene.light});
    const auto nodes = f.ok("get_selection");
    CHECK(strings(nodes.value("nodes")) == QStringList{"B2", "E3"});
    CHECK(nodes.value("bounds").isObject());

    mdl::deselectAll(f.map);
    const auto top = scene.wall->brush().findFace(vm::vec3d{0, 0, 1}).value();
    mdl::selectBrushFaces(f.map, {{*scene.wall, top}});
    const auto faces = f.ok("get_selection");
    CHECK(strings(faces.value("nodes")).isEmpty());
    CHECK(
      compactJson(faces.value("faces"))
      == QString{R"([{"brush":"B2","index":%1,"side":"+z"}])"}.arg(top));
  }

  SECTION("read tools do not change the map")
  {
    const auto scene = populateTestScene(f);

    const auto calls = std::vector<std::pair<QString, const char*>>{
      {"get_map_info", "{}"},
      {"get_scene_tree", R"({"include_brushes":true})"},
      {"get_scene_tree", R"({"root":"G8"})"},
      {"get_nodes", R"({"ids":["world","L1","B2","E3","E4","G8","P9","L10"]})"},
      {"get_nodes", R"({"ids":["B99"]})"},
      {"get_brush_geometry", R"({"ids":["B2","B5","B7"]})"},
      {"get_brush_geometry", R"({"ids":["E3"]})"},
      {"find_nodes", "{}"},
      {"find_nodes", R"({"text":"light","material":"*","selected":true})"},
      {"get_selection", "{}"},
    };

    const auto checkAll = [&]() {
      for (const auto& [tool, args] : calls)
      {
        CAPTURE(tool, args);
        const auto before = snapshot(f.map);
        f.call(tool, args);
        CHECK(snapshot(f.map) == before);
      }
    };

    SECTION("without selection")
    {
      checkAll();
    }

    SECTION("with selected nodes")
    {
      mdl::selectNodes(f.map, {scene.wall, scene.light});
      checkAll();
    }

    SECTION("with selected faces")
    {
      mdl::selectBrushFaces(f.map, {{*scene.wall, 0}, {*scene.floor, 1}});
      checkAll();
    }

    SECTION("with an undone command")
    {
      f.map.undoCommand();
      checkAll();
    }
  }

  SECTION("list_entity_definitions")
  {
    SECTION("without definitions")
    {
      const auto result = f.ok("list_entity_definitions");
      CHECK(result.value("definitions") == QJsonValue{QJsonArray{}});
      CHECK(compactJson(result.value("warnings")).contains("no entity definitions"));
    }

    SECTION("lists the definitions sorted by classname")
    {
      setTestDefinitions(f.map);
      const auto result = f.ok("list_entity_definitions");
      CHECK(
        result.value("definitions")
        == QJsonValue{QJsonArray{
          QJsonObject{
            {"classname", "func_door"},
            {"type", "brush"},
            {"summary", "A door."},
          },
          QJsonObject{
            {"classname", "info_player_start"},
            {"type", "point"},
            {"summary", "Player start."},
            {"point_bounds",
             QJsonObject{
               {"min", QJsonArray{-16, -16, -24}}, {"max", QJsonArray{16, 16, 32}}}},
          },
          QJsonObject{
            {"classname", "light"},
            {"type", "point"},
            {"summary", "A light."},
            {"point_bounds",
             QJsonObject{{"min", QJsonArray{-8, -8, -8}}, {"max", QJsonArray{8, 8, 8}}}},
          },
        }});
      CHECK(result.value("total") == QJsonValue{3});

      const auto lights = f.ok("list_entity_definitions", R"({"filter":"LIGHT*"})");
      CHECK(lights.value("total") == QJsonValue{1});
      const auto brushes = f.ok("list_entity_definitions", R"({"type":"brush"})");
      CHECK(brushes.value("total") == QJsonValue{1});
      const auto paged = f.ok("list_entity_definitions", R"({"offset":1,"limit":1})");
      CHECK(paged.value("definitions").toArray().size() == 1);
      CHECK(paged.value("next_offset") == QJsonValue{2});
    }
  }

  SECTION("get_entity_definition")
  {
    setTestDefinitions(f.map);

    SECTION("returns the properties")
    {
      const auto result = f.ok("get_entity_definition", R"({"classname":"light"})");
      CHECK(result.value("classname") == QJsonValue{"light"});
      CHECK(result.value("type") == QJsonValue{"point"});
      CHECK(result.value("description") == QJsonValue{"A light.\nIt shines."});
      CHECK(
        result.value("properties")
        == QJsonValue{QJsonArray{
          QJsonObject{
            {"key", "light"},
            {"kind", "integer"},
            {"short_description", "Brightness"},
            {"long_description", ""},
            {"read_only", false},
            {"default", "300"},
          },
          QJsonObject{
            {"key", "style"},
            {"kind", "choice"},
            {"short_description", "Style"},
            {"long_description", ""},
            {"read_only", false},
            {"default", "0"},
            {"choices",
             QJsonArray{
               QJsonObject{{"value", "0"}, {"description", "normal"}},
               QJsonObject{{"value", "1"}, {"description", "flicker"}},
             }},
          },
          QJsonObject{
            {"key", "spawnflags"},
            {"kind", "flags"},
            {"short_description", ""},
            {"long_description", ""},
            {"read_only", false},
            {"flags",
             QJsonArray{
               QJsonObject{
                 {"value", 1},
                 {"short_description", "Start off"},
                 {"long_description", ""},
               },
             }},
          },
          QJsonObject{
            {"key", "target"},
            {"kind", "link_source"},
            {"short_description", ""},
            {"long_description", ""},
            {"read_only", false},
          },
        }});
    }

    SECTION("suggests similar classnames")
    {
      const auto error = f.error("get_entity_definition", R"({"classname":"lihgt"})");
      CHECK(error.value("code") == QJsonValue{"definition_not_found"});
      CHECK(
        error.value("details").toObject().value("suggestions").toArray().first()
        == QJsonValue{"light"});
    }
  }

  SECTION("list_materials")
  {
    populateTestScene(f);

    SECTION("lists the materials used in the map")
    {
      const auto result = f.ok("list_materials");
      const auto items = result.value("items").toArray();
      auto names = QStringList{};
      for (const auto& item : items)
      {
        names.push_back(item.toObject().value("name").toString());
      }
      CHECK(names.contains("wall"));
      CHECK(names.contains("floor"));
      CHECK(std::is_sorted(names.begin(), names.end(), [](const auto& a, const auto& b) {
        return a.toLower() < b.toLower();
      }));
      CHECK(result.value("current_material").isString());
      CHECK(compactJson(result.value("warnings")).contains("no materials are loaded"));

      const auto wall = f.ok("list_materials", R"({"filter":"WA*"})").value("items");
      CHECK(
        wall
        == QJsonValue{QJsonArray{
          QJsonObject{{"name", "wall"}, {"usage_count", 6}, {"loaded", false}}}});
    }

    SECTION("counts every face")
    {
      f.ok(
        "set_face_attributes",
        R"({"faces":[{"brush":"B2","side":"+z"}],"material":"top"})");
      const auto items = f.ok("list_materials", R"({"filter":"top"})").value("items");
      CHECK(
        items
        == QJsonValue{QJsonArray{
          QJsonObject{{"name", "top"}, {"usage_count", 1}, {"loaded", false}}}});
      CHECK(
        f.ok("list_materials", R"({"filter":"wall","used_only":true})")
          .value("items")
          .toArray()[0]
          .toObject()
          .value("usage_count")
        == QJsonValue{5});
    }
  }

  SECTION("get_log")
  {
    CHECK(f.ok("get_log") == QJsonObject{{"lines", QJsonArray{}}, {"total", 0}});

    auto lines = std::vector<McpLogLine>{};
    for (auto i = 0; i < 600; ++i)
    {
      lines.push_back(
        {i % 2 == 0 ? LogLevel::Warn : LogLevel::Error, QString::number(i)});
    }
    f.doc->logHistory.append(lines);

    const auto result = f.ok("get_log", R"({"limit":2})");
    CHECK(
      result
      == QJsonObject{
        {"lines",
         QJsonArray{
           QJsonObject{{"level", "warn"}, {"text", "598"}},
           QJsonObject{{"level", "error"}, {"text", "599"}},
         }},
        {"total", 500},
      });
    CHECK(f.ok("get_log").value("lines").toArray().size() == 100);
    CHECK(f.err("get_log", R"({"limit":501})") == "invalid_argument");
  }
}

} // namespace tb::ui
