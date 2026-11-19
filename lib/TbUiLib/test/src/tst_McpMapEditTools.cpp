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
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpCheckpoints.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

QJsonObject boundsJson(const vm::vec3d& min, const vm::vec3d& max)
{
  return QJsonObject{{"min", toJson(min)}, {"max", toJson(max)}};
}

QJsonObject nodeBounds(McpMapToolsFixture& f, const QString& id)
{
  return f.ok("get_nodes", QJsonObject{{"ids", QJsonArray{id}}})
    .value("nodes")
    .toArray()[0]
    .toObject()
    .value("bounds")
    .toObject();
}

QJsonObject properties(McpMapToolsFixture& f, const QString& id)
{
  const auto nodes =
    f.ok("get_nodes", QJsonObject{{"ids", QJsonArray{id}}}).value("nodes").toArray();
  auto result = QJsonObject{};
  for (const auto& property : nodes[0].toObject().value("properties").toArray())
  {
    result[property.toObject().value("key").toString()] =
      property.toObject().value("value");
  }
  return result;
}

QJsonArray faces(McpMapToolsFixture& f, const QString& id)
{
  return f.ok("get_brush_geometry", QJsonObject{{"ids", QJsonArray{id}}})
    .value("brushes")
    .toArray()[0]
    .toObject()
    .value("faces")
    .toArray();
}

QJsonObject faceWithSide(const QJsonArray& faces, const QString& side)
{
  for (const auto& face : faces)
  {
    if (face.toObject().value("side") == QJsonValue{side})
    {
      return face.toObject();
    }
  }
  FAIL("no face with side " << side.toStdString());
  return {};
}

QJsonObject details(const QJsonObject& error)
{
  return error.value("details").toObject();
}

} // namespace

TEST_CASE("McpMapEditTools")
{
  auto f = McpMapToolsFixture{};
  const auto scene = populateTestScene(f);

  SECTION("transform_nodes")
  {
    SECTION("translate")
    {
      const auto result =
        f.ok("transform_nodes", R"({"ids":["B2"],"translate":[16,0,8]})");
      CHECK(result.value("bounds") == QJsonValue{boundsJson({16, 0, 8}, {80, 64, 72})});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Transform Nodes"});
      CHECK(result.value("changes")
              .toObject()
              .value("modified")
              .toArray()
              .contains(QJsonValue{"B2"}));
      CHECK(f.id(*scene.wall) == "B2");
      CHECK(nodeBounds(f, "B2") == boundsJson({16, 0, 8}, {80, 64, 72}));
      CHECK_FALSE(result.contains("warnings"));
    }

    SECTION("rotate")
    {
      f.ok(
        "transform_nodes", R"({"ids":["B2"],"rotate":{"axis":"z","angle_degrees":90}})");
      CHECK(nodeBounds(f, "B2") == boundsJson({0, 0, 0}, {64, 64, 64}));

      f.ok(
        "transform_nodes",
        R"({"ids":["B2"],"rotate":{"axis":[0,0,2],"angle_degrees":90,"center":[0,0,0]}})");
      CHECK(nodeBounds(f, "B2") == boundsJson({-64, 0, 0}, {0, 64, 64}));

      const auto rotated = f.ok(
        "transform_nodes", R"({"ids":["B2"],"rotate":{"axis":"z","angle_degrees":45}})");
      CHECK(rotated.value("warnings")
              .toArray()
              .contains(
                QJsonValue{"some brush vertices are no longer on integer coordinates"}));
    }

    SECTION("scale")
    {
      f.ok("transform_nodes", R"({"ids":["B2"],"scale":{"factors":[2,1,0.5]}})");
      CHECK(nodeBounds(f, "B2") == boundsJson({-32, 0, 16}, {96, 64, 48}));

      f.ok(
        "transform_nodes",
        R"({"ids":["B2"],"scale":{"factors":[-1,1,1],"center":[0,0,0]}})");
      CHECK(nodeBounds(f, "B2") == boundsJson({-96, 0, 16}, {32, 64, 48}));
    }

    SECTION("flip")
    {
      f.ok("transform_nodes", R"({"ids":["B5"],"flip":{"axis":"x","center":[0,0,0]}})");
      CHECK(nodeBounds(f, "B5") == boundsJson({-144, 0, 0}, {-128, 16, 64}));
    }

    SECTION("brush entities, groups and the selection")
    {
      // a brush entity moves with its brushes
      f.ok("transform_nodes", R"({"ids":["E4"],"translate":[0,32,0]})");
      CHECK(nodeBounds(f, "B5") == boundsJson({128, 32, 0}, {144, 48, 64}));
      CHECK(nodeBounds(f, "B6") == boundsJson({160, 32, 0}, {176, 48, 64}));

      f.ok("transform_nodes", R"({"ids":["G8"],"translate":[0,0,16]})");
      CHECK(nodeBounds(f, "B7") == boundsJson({0, 128, 16}, {64, 192, 32}));

      // the point entity origin changes
      f.ok("transform_nodes", R"({"ids":["E3"],"translate":[8,0,0]})");
      CHECK(properties(f, "E3").value("origin") == QJsonValue{"40 32 128"});

      mdl::selectNodes(f.map, {scene.wall, scene.patch});
      f.ok("transform_nodes", R"({"ids":["@selection"],"translate":[0,0,-8]})");
      CHECK(nodeBounds(f, "B2") == boundsJson({0, 0, -8}, {64, 64, 56}));
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.wall, scene.patch});
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(f.err("transform_nodes", R"({"ids":["B2"]})") == "invalid_argument");
      CHECK(
        f.err(
          "transform_nodes", R"({"ids":["B2"],"translate":[1,0,0],"flip":{"axis":"x"}})")
        == "invalid_argument");
      CHECK(
        f.err("transform_nodes", R"({"ids":["B2"],"scale":{"factors":[1,0,1]}})")
        == "invalid_argument");
      CHECK(
        f.err("transform_nodes", R"({"ids":["B2"],"scale":{"factors":[1,1e5,1]}})")
        == "invalid_argument");
      // a tiny factor collapsed the group's brush and made the UV lock assert
      CHECK(
        f.err("transform_nodes", R"({"ids":["G8"],"scale":{"factors":[1e-300,1,1]}})")
        == "invalid_argument");
      CHECK(
        f.err("transform_nodes", R"({"ids":["G8"],"scale":{"factors":[1,-1e-5,1]}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "transform_nodes",
          R"({"ids":["B2"],"rotate":{"axis":[0,0,0],"angle_degrees":90}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "transform_nodes",
          R"({"ids":["B2"],"rotate":{"axis":"z","angle_degrees":4000}})")
        == "invalid_argument");
      CHECK(
        f.err("transform_nodes", R"({"ids":["B2"],"translate":[9000,0,0]})")
        == "out_of_world_bounds");
      CHECK(
        f.err("transform_nodes", R"({"ids":["B2"],"scale":{"factors":[1000,1,1]}})")
        == "out_of_world_bounds");
      CHECK(
        f.err("transform_nodes", R"({"ids":["world"],"translate":[1,0,0]})")
        == "wrong_node_type");
      CHECK(
        f.err("transform_nodes", R"({"ids":["L1"],"translate":[1,0,0]})")
        == "wrong_node_type");
      CHECK(
        f.err("transform_nodes", R"({"ids":["B99"],"translate":[1,0,0]})")
        == "unknown_node");
      CHECK(
        f.err("transform_nodes", R"({"ids":["@selection"],"translate":[1,0,0]})")
        == "invalid_argument");

      const auto closedGroup =
        f.error("transform_nodes", R"({"ids":["B7"],"translate":[1,0,0]})");
      CHECK(closedGroup.value("code") == QJsonValue{"node_not_selectable"});
      CHECK(details(closedGroup).value("reason") == QJsonValue{"in_closed_group"});

      mdl::lockNodes(f.map, {scene.wall});
      const auto locked =
        f.error("transform_nodes", R"({"ids":["B2"],"translate":[1,0,0]})");
      CHECK(details(locked).value("reason") == QJsonValue{"locked"});
      mdl::unlockNodes(f.map, {scene.wall});

      mdl::hideLayers(f.map, {scene.layer});
      const auto hidden =
        f.error("transform_nodes", R"({"ids":["E11"],"translate":[1,0,0]})");
      CHECK(details(hidden).value("reason") == QJsonValue{"hidden"});

      CHECK(contentDigest(f.doc->nodes, f.map) == [&] {
        auto g = McpMapToolsFixture{};
        populateTestScene(g);
        return contentDigest(g.doc->nodes, g.map);
      }());
      CHECK(f.map.selection().nodes == before.selectedNodes);
    }
  }

  SECTION("transform_nodes refuses entities with non-finite or huge data")
  {
    // such entities can come from map files; transforming them made vm assert on NaN
    // bounds or on a NaN rotation matrix, which aborts the editor
    auto* farAway = addPointEntity(f.map, "info_null", {2e6, 0, 0});
    auto* badAngles = addPointEntity(f.map, "info_null", {0, 0, 0});
    mdl::deselectAll(f.map);
    mdl::selectNodes(f.map, {badAngles});
    REQUIRE(mdl::setEntityProperty(f.map, "angles", "nan 0 0"));
    mdl::deselectAll(f.map);
    mdl::selectNodes(f.map, {scene.door->children().front()});
    REQUIRE(mdl::setEntityProperty(f.map, "angles", "inf 0 0"));
    mdl::deselectAll(f.map);

    const auto before = snapshot(f.map);
    for (const auto* operation :
         {R"("translate":[1,0,0])",
          R"("rotate":{"axis":"z","angle_degrees":45})",
          R"("scale":{"factors":[1,-1,2]})",
          R"("flip":{"axis":"x"})"})
    {
      CAPTURE(operation);
      const auto err = [&](const QString& id) {
        return f.err(
          "transform_nodes",
          parseObject(
            QString{R"({"ids":["%1"],%2})"}.arg(id, operation).toUtf8().constData()));
      };
      CHECK(err(f.id(*farAway)) == "out_of_world_bounds");
      CHECK(err(f.id(*badAngles)) == "invalid_argument");
      // a brush entity is transformed with its brushes, so its rotation is checked too
      CHECK(err("B5") == "invalid_argument");
      CHECK(err("E4") == "invalid_argument");
    }
    CHECK(snapshot(f.map) == before);
  }

  SECTION("delete_nodes")
  {
    SECTION("deletes nodes")
    {
      const auto result = f.ok("delete_nodes", R"({"ids":["B2","E3"]})");
      CHECK(result.value("deleted") == QJsonValue{QJsonArray{"B2", "E3"}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Delete Nodes"});
      CHECK(
        result.value("changes").toObject().value("removed")
        == QJsonValue{QJsonArray{"B2", "E3"}});

      const auto stale = f.error("get_nodes", R"({"ids":["B2"]})");
      CHECK(stale.value("code") == QJsonValue{"stale_node"});
      CHECK(details(stale).value("removed_by") == QJsonValue{"Delete Nodes"});

      // undo restores the nodes with new ids
      f.ok("undo");
      CHECK(f.err("get_nodes", R"({"ids":["B2"]})") == "stale_node");
      CHECK(
        f.ok("get_map_stats").value("counts").toObject().value("world_brushes")
        == QJsonValue{2});
    }

    SECTION("brush entities and their brushes")
    {
      const auto entity = f.ok("delete_nodes", R"({"ids":["E4"]})");
      CHECK(
        entity.value("changes").toObject().value("removed")
        == QJsonValue{QJsonArray{"E4", "B5", "B6"}});
      f.ok("undo");

      // deleting all brushes of a brush entity removes the entity too
      auto g = McpMapToolsFixture{};
      populateTestScene(g);
      const auto brushes = g.ok("delete_nodes", R"({"ids":["B5","B6"]})");
      CHECK(
        brushes.value("changes").toObject().value("removed")
        == QJsonValue{QJsonArray{"E4", "B5", "B6"}});

      // descendants of listed nodes are dropped
      const auto group = g.ok("delete_nodes", R"({"ids":["G8","P9"]})");
      CHECK(group.value("deleted") == QJsonValue{QJsonArray{"G8", "P9"}});
    }

    SECTION("selected nodes")
    {
      mdl::selectNodes(f.map, {scene.wall, scene.light});
      f.ok("delete_nodes", R"({"ids":["B2"]})");
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.light});

      f.ok("delete_nodes", R"({"ids":["@selection"]})");
      CHECK(f.map.selection().nodes.empty());
      CHECK(f.doc->nodes.verifyBijection());
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(f.err("delete_nodes", R"({"ids":["world"]})") == "wrong_node_type");
      CHECK(f.err("delete_nodes", R"({"ids":["L10"]})") == "not_supported");
      CHECK(f.err("delete_nodes", R"({"ids":["default_layer"]})") == "not_supported");
      CHECK(f.err("delete_nodes", R"({"ids":["B7"]})") == "node_not_selectable");
      CHECK(f.err("delete_nodes", R"({"ids":["B2","B99"]})") == "unknown_node");

      mdl::lockNodes(f.map, {scene.light});
      CHECK(f.err("delete_nodes", R"({"ids":["B2","E3"]})") == "node_not_selectable");
      mdl::unlockNodes(f.map, {scene.light});

      mdl::selectNodes(f.map, {scene.group});
      REQUIRE(mdl::createLinkedDuplicate(f.map) != nullptr);
      mdl::deselectAll(f.map);
      CHECK(f.err("delete_nodes", R"({"ids":["G8"]})") == "not_supported");
      CHECK(f.doc->nodes.isLive(scene.wall));
      CHECK(before.text != "");
    }
  }

  SECTION("set_entity_properties")
  {
    SECTION("point entities")
    {
      const auto result = f.ok(
        "set_entity_properties",
        R"({"ids":["E3"],"set":{"light":400,"angle":90},"rename":{"_color":"color"}})");
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Set Entity Properties"});
      CHECK(
        result.value("entities")
        == QJsonValue{QJsonArray{QJsonObject{
          {"id", "E3"},
          {"properties",
           QJsonArray{
             QJsonObject{{"key", "classname"}, {"value", "light"}},
             QJsonObject{{"key", "origin"}, {"value", "32 32 128"}},
             QJsonObject{{"key", "light"}, {"value", "400"}},
             QJsonObject{{"key", "color"}, {"value", "1 1 1"}},
             QJsonObject{{"key", "angle"}, {"value", "90"}},
           }},
        }}});
      CHECK(result.value("changes")
              .toObject()
              .value("modified")
              .toArray()
              .contains(QJsonValue{"E3"}));

      f.ok("set_entity_properties", R"({"ids":["E3"],"remove":["color","angle"]})");
      CHECK_FALSE(properties(f, "E3").contains("color"));
    }

    SECTION("brush entities")
    {
      mdl::selectNodes(f.map, {scene.patch});
      const auto result =
        f.ok("set_entity_properties", R"({"ids":["E4","E3"],"set":{"targetname":"t1"}})");
      CHECK(properties(f, "E4").value("targetname") == QJsonValue{"t1"});
      CHECK(properties(f, "E3").value("targetname") == QJsonValue{"t1"});
      CHECK(result.value("entities").toArray().size() == 2);
      // the brushes of the brush entity are not changed
      CHECK(properties(f, "E4").value("classname") == QJsonValue{"func_door"});
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.patch});
    }

    SECTION("world")
    {
      mdl::selectNodes(f.map, {scene.light});
      const auto result =
        f.ok("set_entity_properties", R"({"ids":["world"],"set":{"message":"hello"}})");
      CHECK(properties(f, "world").value("message") == QJsonValue{"hello"});
      CHECK(properties(f, "E3").value("message").isUndefined());
      CHECK(
        result.value("changes").toObject().value("modified").toArray().first()
        == QJsonValue{"world"});
      CHECK(
        result.value("entities").toArray()[0].toObject().value("id")
        == QJsonValue{"world"});
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.light});
    }

    SECTION("warnings")
    {
      const auto result = f.ok(
        "set_entity_properties",
        R"({"ids":["E3"],"set":{"angle":90,"angles":"0 90 0"}})");
      CHECK(
        result.value("warnings")
          .toArray()
          .contains(QJsonValue{"E3 has both angle and angles; angles takes precedence"}));
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      const auto err = [&](const char* args) {
        return f.err("set_entity_properties", args);
      };

      CHECK(err(R"({"ids":["E3"]})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"set":{}})") == "invalid_argument");
      CHECK(err(R"({"ids":["world","E3"],"set":{"a":"b"}})") == "invalid_argument");
      CHECK(err(R"({"ids":["B2"],"set":{"a":"b"}})") == "wrong_node_type");
      CHECK(err(R"({"ids":["G8"],"set":{"a":"b"}})") == "wrong_node_type");
      CHECK(err(R"({"ids":["E3"],"remove":["nothere"]})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3","E4"],"remove":["light"]})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"remove":["classname"]})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"rename":{"classname":"x"}})") == "invalid_argument");
      CHECK(
        err(R"({"ids":["E3"],"rename":{"light":"classname"}})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"rename":{"light":"_color"}})") == "invalid_argument");
      CHECK(
        err(R"({"ids":["E3"],"rename":{"light":"a","_color":"a"}})")
        == "invalid_argument");
      CHECK(
        err(R"({"ids":["E3"],"rename":{"light":"_color","_color":"x"}})")
        == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"rename":{"light":1}})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"rename":{"nothere":"x"}})") == "invalid_argument");
      CHECK(err(R"({"ids":["world"],"set":{"classname":"x"}})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"set":{"classname":""}})") == "invalid_argument");
      CHECK(
        err(R"({"ids":["E3"],"set":{"classname":"worldspawn"}})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"set":{"a":"x\"y"}})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"set":{"a":{"b":1}}})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"set":{"":"x"}})") == "invalid_argument");

      // non-finite or huge origins and rotations make transformations assert
      CHECK(err(R"({"ids":["E3"],"set":{"origin":"inf 0 0"}})") == "invalid_argument");
      CHECK(
        err(R"({"ids":["E3"],"set":{"origin":"1e308 1e308 1e308"}})")
        == "invalid_argument");
      CHECK(
        err(R"({"ids":["E3"],"set":{"angles":"nan nan nan"}})") == "invalid_argument");
      CHECK(
        err(R"x({"ids":["E3"],"set":{"mangle":"(0 1e400 0)"}})x") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"set":{"angle":"-inf"}})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"set":{"angle":1e300}})") == "invalid_argument");
      CHECK(err(R"({"ids":["E3"],"set":{"origin":[1e300,0,0]}})") == "invalid_argument");

      CHECK(snapshot(f.map) == before);

      // a rename must not move a bad value into a rotation key
      f.ok("set_entity_properties", R"({"ids":["E3"],"set":{"note":"inf nan"}})");
      CHECK(err(R"({"ids":["E3"],"rename":{"note":"angles"}})") == "invalid_argument");

      mdl::hideLayers(f.map, {scene.layer});
      const auto hidden =
        f.error("set_entity_properties", R"({"ids":["E11"],"set":{"a":"b"}})");
      CHECK(details(hidden).value("reason") == QJsonValue{"hidden"});
    }
  }

  SECTION("set_face_attributes")
  {
    SECTION("faces")
    {
      const auto result = f.ok(
        "set_face_attributes",
        R"({"faces":[{"brush":"B2","side":"+z"},{"brush":"B2","index":0}],
            "material":"grass"})");
      CHECK(result.value("faces_changed") == QJsonValue{2});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Set Face Attributes"});
      const auto brushFaces = faces(f, "B2");
      CHECK(faceWithSide(brushFaces, "+z").value("material") == QJsonValue{"grass"});
      CHECK(brushFaces[0].toObject().value("material") == QJsonValue{"grass"});
      CHECK(faceWithSide(brushFaces, "-z").value("material") == QJsonValue{"wall"});
      CHECK(
        result.value("warnings")
          .toArray()
          .contains(QJsonValue{"material 'grass' was not found; check list_materials"}));
    }

    SECTION("brushes")
    {
      const auto result = f.ok(
        "set_face_attributes",
        R"({"brushes":["B2"],"offset":[8,4],"scale":[0.5,-2],"rotation":90})");
      CHECK(result.value("faces_changed") == QJsonValue{6});
      for (const auto& face : faces(f, "B2"))
      {
        const auto uv = face.toObject().value("uv").toObject();
        CHECK(uv.value("offset") == QJsonValue{QJsonArray{8, 4}});
        CHECK(uv.value("scale") == QJsonValue{QJsonArray{0.5, -2}});
        CHECK(uv.value("rotation") == QJsonValue{90});
      }

      // a brush entity expands to its brushes
      const auto entity =
        f.ok("set_face_attributes", R"({"brushes":["E4"],"material":"door"})");
      CHECK(entity.value("faces_changed") == QJsonValue{12});
      CHECK(faces(f, "B6")[3].toObject().value("material") == QJsonValue{"door"});
    }

    SECTION("surface, align and fit")
    {
      f.ok(
        "set_face_attributes",
        R"({"faces":[{"brush":"B2","side":"+z"}],"surface":{"contents":1,"flags":2,"value":3}})");
      const auto surface = faceWithSide(faces(f, "B2"), "+z").value("surface").toObject();
      CHECK(surface.value("contents") == QJsonValue{1});
      CHECK(surface.value("flags") == QJsonValue{2});
      CHECK(surface.value("value") == QJsonValue{3});

      f.ok("set_face_attributes", R"({"brushes":["B2"],"align":"paraxial"})");
      f.ok("set_face_attributes", R"({"brushes":["B2"],"align":"parallel"})");
      f.ok("set_face_attributes", R"({"brushes":["B2"],"align":"reset","fit":true})");
      f.ok(
        "set_face_attributes",
        R"({"faces":[{"brush":"B2","normal":[0,0,-1]}],"fit":true})");
    }

    SECTION("keeps the user's face selection")
    {
      mdl::selectBrushFaces(f.map, {{*scene.floor, 1}});
      const auto material = f.map.currentMaterialName();
      f.ok(
        "set_face_attributes",
        R"({"faces":[{"brush":"B2","side":"+z"}],"material":"grass"})");
      CHECK(
        f.map.selection().brushFaces
        == std::vector<mdl::BrushFaceHandle>{{*scene.floor, 1}});
      CHECK(f.map.currentMaterialName() == material);
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      const auto err = [&](const char* args) {
        return f.err("set_face_attributes", args);
      };

      CHECK(err(R"({"material":"grass"})") == "invalid_argument");
      CHECK(
        err(R"({"brushes":["B2"],"faces":[{"brush":"B2","index":0}],"material":"grass"})")
        == "invalid_argument");
      CHECK(err(R"({"brushes":["B2"]})") == "invalid_argument");
      CHECK(err(R"({"brushes":["B2"],"fit":false})") == "invalid_argument");
      CHECK(err(R"({"brushes":["B2"],"scale":[0,1]})") == "invalid_argument");
      CHECK(err(R"({"brushes":["B2"],"offset":[1e7,1]})") == "invalid_argument");
      CHECK(err(R"({"brushes":["B2"],"material":"a b"})") == "invalid_argument");
      CHECK(err(R"({"brushes":["P9"],"material":"x"})") == "wrong_node_type");
      CHECK(err(R"({"brushes":["E3"],"material":"x"})") == "wrong_node_type");
      CHECK(err(R"({"brushes":["B7"],"material":"x"})") == "node_not_selectable");
      CHECK(
        err(R"({"faces":[{"brush":"B2","index":6}],"material":"x"})")
        == "face_not_found");
      CHECK(
        err(R"({"faces":[{"brush":"B2","normal":[1,1,0]}],"material":"x"})")
        == "face_not_found");
      CHECK(
        err(R"({"faces":[{"brush":"B2","index":0,"side":"+x"}],"material":"x"})")
        == "invalid_argument");
      CHECK(
        err(R"({"faces":[{"brush":"E3","index":0}],"material":"x"})")
        == "wrong_node_type");

      mdl::selectNodes(f.map, {scene.group});
      REQUIRE(mdl::createLinkedDuplicate(f.map) != nullptr);
      mdl::deselectAll(f.map);
      CHECK(
        err(R"({"faces":[{"brush":"B7","index":0}],"material":"x"})") == "not_supported");
      CHECK(snapshot(f.map).modificationCount > before.modificationCount);
    }
  }
}

} // namespace tb::ui
