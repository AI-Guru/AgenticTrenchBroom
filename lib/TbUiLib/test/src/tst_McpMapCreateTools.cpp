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
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_Selection.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpCheckpoints.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

void setTestDefinitions(mdl::Map& map)
{
  map.entityDefinitionManager().setDefinitions({
    mdl::EntityDefinition{
      "point_entity",
      Color{},
      "a point entity",
      {
        {"some_default", mdl::PropertyValueTypes::String{"value"}, "", ""},
        {"light", mdl::PropertyValueTypes::String{"300"}, "", ""},
      },
      mdl::PointEntityDefinition{vm::bbox3d{16.0}, {}, {}},
    },
    mdl::EntityDefinition{
      "brush_entity",
      Color{},
      "a brush entity",
      {},
    },
  });
}

QJsonObject faceWithSide(const QJsonObject& geometry, const QString& side)
{
  for (const auto& face :
       geometry.value("brushes").toArray()[0].toObject().value("faces").toArray())
  {
    if (face.toObject().value("side") == QJsonValue{side})
    {
      return face.toObject();
    }
  }
  FAIL("no face with side " << side.toStdString());
  return {};
}

QJsonObject entityProperties(McpMapToolsFixture& f, const QString& id)
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

} // namespace

TEST_CASE("McpMapCreateTools")
{
  auto f = McpMapToolsFixture{};

  SECTION("create_box_brush")
  {
    SECTION("creates a box")
    {
      const auto result = f.ok(
        "create_box_brush",
        R"({"bounds":{"min":[0,0,0],"max":[64,64,64]},"material":"wall"})");
      CHECK(result.value("id") == QJsonValue{"B2"});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Box Brush"});
      CHECK(
        result.value("changes").toObject().value("created")
        == QJsonValue{QJsonArray{"B2"}});
      CHECK(
        result.value("nodes").toArray()[0].toObject().value("materials")
        == QJsonValue{QJsonArray{"wall"}});

      const auto geometry = f.ok("get_brush_geometry", R"({"ids":["B2"]})");
      const auto brush = geometry.value("brushes").toArray()[0].toObject();
      CHECK(
        brush.value("bounds")
        == QJsonValue{
          QJsonObject{{"min", QJsonArray{0, 0, 0}}, {"max", QJsonArray{64, 64, 64}}}});
      CHECK(brush.value("face_count") == QJsonValue{6});
      CHECK(brush.value("volume") == QJsonValue{262144});

      // the brush is added to the current layer and the selection is unchanged
      CHECK(f.map.selection().nodes.empty());
      const auto nodes = f.ok("get_nodes", R"({"ids":["B2"]})").value("nodes").toArray();
      CHECK(nodes[0].toObject().value("parent") == QJsonValue{"L1"});
    }

    SECTION("uses the current material by default")
    {
      f.map.setCurrentMaterialName("current");
      f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[16,16,16]}})");
      const auto nodes = f.ok("get_nodes", R"({"ids":["B2"]})").value("nodes").toArray();
      CHECK(nodes[0].toObject().value("materials") == QJsonValue{QJsonArray{"current"}});
    }

    SECTION("side_materials put the material on the face whose normal is that side")
    {
      f.ok(
        "create_box_brush",
        R"({"bounds":{"min":[0,0,0],"max":[64,32,16]},"material":"other",
            "side_materials":{"-x":"left","+x":"right","-y":"front","+y":"back",
                              "+z":"top"}})");
      const auto geometry = f.ok("get_brush_geometry", R"({"ids":["B2"]})");
      CHECK(faceWithSide(geometry, "-x").value("material") == QJsonValue{"left"});
      CHECK(faceWithSide(geometry, "+x").value("material") == QJsonValue{"right"});
      CHECK(faceWithSide(geometry, "-y").value("material") == QJsonValue{"front"});
      CHECK(faceWithSide(geometry, "+y").value("material") == QJsonValue{"back"});
      CHECK(faceWithSide(geometry, "+z").value("material") == QJsonValue{"top"});
      CHECK(faceWithSide(geometry, "-z").value("material") == QJsonValue{"other"});
    }

    SECTION("parent")
    {
      const auto scene = populateTestScene(f);

      f.ok(
        "create_box_brush",
        R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"parent":"L10"})");
      CHECK(f.map.worldNode().defaultLayer() != nullptr);
      auto nodes = f.ok("get_nodes", R"({"ids":["B12"]})").value("nodes").toArray();
      CHECK(nodes[0].toObject().value("parent") == QJsonValue{"L10"});

      // a closed group and a brush entity can be parents
      f.ok(
        "create_box_brush",
        R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"parent":"G8"})");
      CHECK(scene.group->childCount() == 2);
      f.ok(
        "create_box_brush",
        R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"parent":"E4"})");
      CHECK(scene.door->childCount() == 3);

      CHECK(
        f.err(
          "create_box_brush",
          R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"parent":"E3"})")
        == "invalid_parent");
      CHECK(
        f.err(
          "create_box_brush",
          R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"parent":"B2"})")
        == "invalid_parent");
      CHECK(
        f.err(
          "create_box_brush",
          R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"parent":"B999"})")
        == "unknown_node");

      mdl::selectNodes(f.map, {scene.group});
      REQUIRE(mdl::createLinkedDuplicate(f.map) != nullptr);
      mdl::deselectAll(f.map);
      CHECK(
        f.err(
          "create_box_brush",
          R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"parent":"G8"})")
        == "not_supported");
    }

    SECTION("warnings")
    {
      const auto result = f.ok(
        "create_box_brush",
        R"({"bounds":{"min":[0,0,0],"max":[16.5,16,16]},"material":"nomaterial"})");
      const auto warnings = result.value("warnings").toArray();
      CHECK(warnings.contains(
        QJsonValue{"material 'nomaterial' was not found; check list_materials"}));
      CHECK(warnings.contains(QJsonValue{
        "the bounds are not on integer coordinates; keep brush vertices on integers"}));
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(
        f.err("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[0,16,16]}})")
        == "invalid_argument");
      CHECK(
        f.err("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[-1,16,16]}})")
        == "invalid_argument");
      CHECK(
        f.err("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[16,16,9000]}})")
        == "out_of_world_bounds");
      CHECK(
        f.err(
          "create_box_brush",
          R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"material":"a b"})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_box_brush",
          R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"side_materials":{"+x":"a\"b"}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_box_brush",
          R"({"bounds":{"min":[0,0,0],"max":[16,16,16]},"side_materials":{"x":"a"}})")
        == "invalid_argument");
      CHECK(f.err("create_box_brush", R"({})") == "invalid_argument");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("create_entity")
  {
    SECTION("creates a point entity")
    {
      const auto result = f.ok(
        "create_entity",
        R"({"classname":"light","origin":[32,32,96],
            "properties":{"light":300,"_color":[1,0.5,0],"wait":0.25,"spawnflags":true,
                          "message":"hällo"}})");
      CHECK(result.value("id") == QJsonValue{"E2"});
      CHECK(result.value("classname") == QJsonValue{"light"});
      CHECK(result.value("origin") == QJsonValue{QJsonArray{32, 32, 96}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Entity"});
      CHECK(
        result.value("properties")
        == QJsonValue{QJsonArray{
          QJsonObject{{"key", "classname"}, {"value", "light"}},
          QJsonObject{{"key", "_color"}, {"value", "1 0.5 0"}},
          QJsonObject{{"key", "light"}, {"value", "300"}},
          QJsonObject{{"key", "message"}, {"value", QString::fromUtf8("h\xC3\xA4llo")}},
          QJsonObject{{"key", "spawnflags"}, {"value", "1"}},
          QJsonObject{{"key", "wait"}, {"value", "0.25"}},
          QJsonObject{{"key", "origin"}, {"value", "32 32 96"}},
        }});
      // without definitions, the classname is unknown
      CHECK(result.value("warnings")
              .toArray()
              .contains(QJsonValue{
                "unknown classname 'light': there is no entity definition for it"}));

      const auto properties = entityProperties(f, "E2");
      CHECK(properties.value("light") == QJsonValue{"300"});
      CHECK(properties.value("origin") == QJsonValue{"32 32 96"});
    }

    SECTION("creates a brush entity from brushes")
    {
      const auto scene = populateTestScene(f);
      auto* other = addBrush(f.map, vm::bbox3d{{256, 0, 0}, {272, 16, 16}});
      REQUIRE(f.id(*other) == "B12");

      const auto result = f.ok(
        "create_entity",
        R"({"classname":"func_wall","brush_ids":["B2","B12"],"properties":{"targetname":"w"}})");
      CHECK(result.value("id") == QJsonValue{"E13"});

      // the brushes keep their ids
      CHECK(scene.wall->parent() == other->parent());
      CHECK(f.id(*scene.wall) == "B2");
      auto* entity = dynamic_cast<mdl::EntityNode*>(scene.wall->parent());
      REQUIRE(entity != nullptr);
      CHECK(f.id(*entity) == "E13");
      CHECK(entity->childCount() == 2);
      CHECK(entity->entity().classname() == "func_wall");
      CHECK(entityProperties(f, "E13").value("targetname") == QJsonValue{"w"});

      // brushes can be taken from another brush entity, which is removed if it is empty
      const auto moved =
        f.ok("create_entity", R"({"classname":"func_wall","brush_ids":["B5","B6"]})");
      CHECK(
        moved.value("changes").toObject().value("removed")
        == QJsonValue{QJsonArray{"E4"}});
      CHECK(f.doc->nodes.lookup(u"B5").status == McpNodeRegistry::LookupStatus::Live);

      // the origin of a brush entity is ignored
      auto* third = addBrush(f.map, vm::bbox3d{{512, 0, 0}, {528, 16, 16}});
      const auto withOrigin = f.ok(
        "create_entity",
        QJsonObject{
          {"classname", "func_wall"},
          {"brush_ids", QJsonArray{f.id(*third)}},
          {"origin", QJsonArray{1, 2, 3}}});
      CHECK_FALSE(withOrigin.value("warnings").toArray().isEmpty());
    }

    SECTION("reserved _tb_ keys are refused")
    {
      // a func_group with _tb_type _tb_group or _tb_layer looks like a group or layer
      // to the entity inspector, which aborts on a contract when it is selected
      const auto scene = populateTestScene(f);
      const auto before = snapshot(f.map);

      const auto error = f.error(
        "create_entity",
        R"({"classname":"func_group","brush_ids":["B2"],"properties":{"_tb_type":"_tb_group"}})");
      CHECK(error.value("code") == QJsonValue{"invalid_argument"});
      CHECK(
        error.value("details").toObject().value("pointer")
        == QJsonValue{"/properties/_tb_type"});
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"func_group","origin":[0,0,0],"properties":{"_tb_type":"_tb_layer"}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"light","origin":[0,0,0],"properties":{"_tb_layer":"1"}})")
        == "invalid_argument");
      CHECK(snapshot(f.map) == before);
      CHECK(scene.wall != nullptr);
    }

    SECTION("brush_ids errors")
    {
      const auto scene = populateTestScene(f);
      const auto before = snapshot(f.map);

      CHECK(
        f.err("create_entity", R"({"classname":"func_wall","brush_ids":["E4"]})")
        == "wrong_node_type");
      CHECK(
        f.err("create_entity", R"({"classname":"func_wall","brush_ids":["G8"]})")
        == "wrong_node_type");
      CHECK(
        f.err("create_entity", R"({"classname":"func_wall","brush_ids":["B7"]})")
        == "node_not_selectable");

      mdl::lockNodes(f.map, {scene.wall});
      CHECK(
        f.err("create_entity", R"({"classname":"func_wall","brush_ids":["B2"]})")
        == "node_not_selectable");
      mdl::unlockNodes(f.map, {scene.wall});

      mdl::selectNodes(f.map, {scene.group});
      REQUIRE(mdl::createLinkedDuplicate(f.map) != nullptr);
      mdl::deselectAll(f.map);
      mdl::openGroup(f.map, *scene.group);
      CHECK(
        f.err("create_entity", R"({"classname":"func_wall","brush_ids":["B7"]})")
        == "not_supported");
      mdl::closeGroup(f.map);
      CHECK(f.map.selection().nodes.empty());
      CHECK(before.text != "");
    }

    SECTION("entity definitions")
    {
      setTestDefinitions(f.map);
      populateTestScene(f);

      CHECK(
        f.err("create_entity", R"({"classname":"point_entity","brush_ids":["B2"]})")
        == "definition_type_mismatch");
      CHECK(
        f.err("create_entity", R"({"classname":"brush_entity","origin":[0,0,0]})")
        == "definition_type_mismatch");

      const auto point = f.ok(
        "create_entity",
        R"({"classname":"point_entity","origin":[0,0,256],"properties":{"bogus":1}})");
      CHECK(point.value("warnings")
              .toArray()
              .contains(QJsonValue{"property 'bogus' is not defined for point_entity"}));
      CHECK_FALSE(
        entityProperties(f, point.value("id").toString()).contains("some_default"));

      const auto brush =
        f.ok("create_entity", R"({"classname":"brush_entity","brush_ids":["B2"]})");
      CHECK_FALSE(brush.contains("warnings"));
    }

    SECTION("apply_definition_defaults is deterministic")
    {
      const auto create = [](McpMapToolsFixture& g) {
        setTestDefinitions(g.map);
        return g.ok(
          "create_entity",
          R"({"classname":"point_entity","origin":[0,0,0],"properties":{"light":200},
              "apply_definition_defaults":true})");
      };

      auto g = McpMapToolsFixture{};
      const auto first = create(f);
      const auto second = create(g);
      CHECK(compactJson(first) == compactJson(second));

      // existing properties are kept, missing ones get their defaults
      const auto properties = entityProperties(f, first.value("id").toString());
      CHECK(properties.value("light") == QJsonValue{"200"});
      CHECK(properties.value("some_default") == QJsonValue{"value"});
    }

    SECTION("parent")
    {
      const auto scene = populateTestScene(f);
      const auto result = f.ok(
        "create_entity", R"({"classname":"light","origin":[0,0,256],"parent":"L10"})");
      const auto nodes =
        f.ok("get_nodes", QJsonObject{{"ids", QJsonArray{result.value("id")}}})
          .value("nodes")
          .toArray();
      CHECK(nodes[0].toObject().value("parent") == QJsonValue{"L10"});

      CHECK(
        f.err("create_entity", R"({"classname":"light","origin":[0,0,0],"parent":"E4"})")
        == "invalid_parent");
      CHECK(
        f.err("create_entity", R"({"classname":"light","origin":[0,0,0],"parent":"E3"})")
        == "invalid_parent");
      CHECK(scene.door->childCount() == 2);
    }

    SECTION("warns about origins in solid")
    {
      populateTestScene(f);
      const auto result =
        f.ok("create_entity", R"({"classname":"light","origin":[32,32,32]})");
      CHECK(result.value("warnings")
              .toArray()
              .contains(QJsonValue{
                "the origin is inside a world brush; the entity may be stuck in solid"}));
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(f.err("create_entity", R"({"classname":"light"})") == "invalid_argument");
      CHECK(
        f.err("create_entity", R"({"classname":"worldspawn","origin":[0,0,0]})")
        == "invalid_argument");
      CHECK(
        f.err("create_entity", R"({"classname":"WorldSpawn","origin":[0,0,0]})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"light","origin":[0,0,0],"properties":{"classname":"x"}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"light","origin":[0,0,0],"properties":{"origin":"1 2 3"}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"light","origin":[0,0,0],"properties":{"a":"x\"y"}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"light","origin":[0,0,0],"properties":{"":"x"}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"light","origin":[0,0,0],"properties":{"a":null}})")
        == "invalid_argument");
      CHECK(
        f.err("create_entity", R"({"classname":"li\"ght","origin":[0,0,0]})")
        == "invalid_argument");
      CHECK(
        f.err("create_entity", R"({"classname":"light","origin":[0,0,9000]})")
        == "out_of_world_bounds");
      CHECK(
        f.err("create_entity", R"({"classname":"light","origin":[0,0,1e7]})")
        == "invalid_argument");
      // non-finite rotations and far away origins made TrenchBroom assert later
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"light","origin":[0,0,0],"properties":{"angles":"nan 0 0"}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "create_entity",
          R"({"classname":"func_wall","brush_ids":["B2"],"properties":{"origin":"1e308 0 0"}})")
        == "invalid_argument");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("paste_map_text")
  {
    SECTION("export and paste give the same content")
    {
      const auto scene = populateTestScene(f);
      // paste flattens layers and cannot recreate patches identically in all formats,
      // so only use nodes of the default layer
      auto g = McpMapToolsFixture{};
      addBrush(g.map, vm::bbox3d{{0, 0, 0}, {64, 64, 64}}, "wall");
      addPointEntity(g.map, "light", {32, 32, 128});
      addBrushEntity(g.map, "func_door", {vm::bbox3d{{128, 0, 0}, {144, 16, 64}}});
      const auto text = g.ok("export_map_text").value("text").toString();

      auto h = McpMapToolsFixture{};
      const auto result = h.ok("paste_map_text", QJsonObject{{"text", text}});
      CHECK(result.value("ids") == QJsonValue{QJsonArray{"B2", "E3", "E4", "B5"}});
      CHECK(
        h.ok("get_map_stats").value("content_digest")
        == g.ok("get_map_stats").value("content_digest"));
      CHECK(h.map.selection().nodes.empty());
      CHECK(scene.wall != nullptr);
    }

    SECTION("offset")
    {
      const auto result = f.ok(
        "paste_map_text",
        R"({"text":"{\n\"classname\" \"info_null\"\n\"origin\" \"8 8 8\"\n}\n",
            "offset":[0,128,0]})");
      CHECK(result.value("ids") == QJsonValue{QJsonArray{"E2"}});
      CHECK(entityProperties(f, "E2").value("origin") == QJsonValue{"8 136 8"});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Paste Map Text"});

      CHECK(
        f.err(
          "paste_map_text",
          R"({"text":"{\n\"classname\" \"info_null\"\n\"origin\" \"8 8 8\"\n}\n",
              "offset":[0,9000,0]})")
        == "out_of_world_bounds");
    }

    SECTION("keeps the user's selection")
    {
      const auto scene = populateTestScene(f);
      mdl::selectNodes(f.map, {scene.light});
      f.ok(
        "paste_map_text",
        R"({"text":"{\n\"classname\" \"info_null\"\n\"origin\" \"8 8 8\"\n}\n"})");
      CHECK(f.map.selection().nodes == std::vector<mdl::Node*>{scene.light});
    }

    SECTION("nodes far outside the world or with non-finite rotations are rejected")
    {
      // adding such nodes violated a contract in the node octree or made later
      // transformations assert
      const auto before = snapshot(f.map);
      const auto entity = [&](const char* properties) {
        return f.error(
          "paste_map_text",
          QJsonObject{
            {"text", QString{"{\n\"classname\" \"info_null\"\n%1}\n"}.arg(properties)}});
      };
      const auto farAway = entity("\"origin\" \"1e308 1e308 1e308\"\n");
      CHECK(farAway.value("code") == QJsonValue{"out_of_world_bounds"});
      CHECK(farAway.value("details").toObject().value("pointer") == QJsonValue{"/text"});
      CHECK(
        entity("\"origin\" \"inf 0 0\"\n").value("code")
        == QJsonValue{"out_of_world_bounds"});
      CHECK(
        entity("\"origin\" \"0 9000000 0\"\n").value("code")
        == QJsonValue{"out_of_world_bounds"});
      CHECK(
        entity("\"origin\" \"0 0 0\"\n\"angles\" \"nan 0 0\"\n").value("code")
        == QJsonValue{"invalid_argument"});
      CHECK(snapshot(f.map) == before);
    }

    SECTION("face text is rejected")
    {
      const auto scene = populateTestScene(f);
      mdl::selectBrushFaces(f.map, {{*scene.wall, 0}});
      const auto faceText = mdl::serializeSelectedBrushFaces(f.map);
      REQUIRE_FALSE(faceText.empty());
      mdl::deselectAll(f.map);

      // select another face that a face paste would change
      mdl::selectBrushFaces(f.map, {{*scene.floor, 0}});
      const auto before = snapshot(f.map);
      const auto digest = contentDigest(f.doc->nodes, f.map);
      CHECK(
        f.err("paste_map_text", QJsonObject{{"text", QString::fromStdString(faceText)}})
        == "parse_failed");
      CHECK(contentDigest(f.doc->nodes, f.map) == digest);
      CHECK(snapshot(f.map).selectedFaces == before.selectedFaces);
      CHECK(snapshot(f.map).modificationCount == before.modificationCount);
    }

    SECTION("errors")
    {
      populateTestScene(f);
      const auto before = snapshot(f.map);
      CHECK(f.err("paste_map_text", R"({"text":"this is not a map"})") == "parse_failed");
      CHECK(f.err("paste_map_text", R"({"text":"{"})") == "parse_failed");
      CHECK(f.err("paste_map_text", R"({"text":"( ( ( ("})") == "parse_failed");
      const auto error = f.error("paste_map_text", R"({"text":"garbage"})");
      CHECK(error.value("details").toObject().value("log").isArray());
      CHECK(f.err("paste_map_text", R"({"text":""})") == "invalid_argument");
      CHECK(
        f.err(
          "paste_map_text",
          R"({"text":"{\n\"classname\" \"info_null\"\n}\n","offset":[1e7,0,0]})")
        == "invalid_argument");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("create_hollow_box")
  {
    const auto wallBounds = [&](const QString& id) {
      return f.ok("get_nodes", QJsonObject{{"ids", QJsonArray{id}}})
        .value("nodes")
        .toArray()[0]
        .toObject()
        .value("bounds");
    };
    const auto box = [](const QJsonArray& min, const QJsonArray& max) {
      return QJsonValue{QJsonObject{{"min", min}, {"max", max}}};
    };
    const auto solid = [&](const QJsonArray& point) {
      return f.ok("query_point", QJsonObject{{"points", QJsonArray{QJsonValue{point}}}})
        .value("results")
        .toArray()[0]
        .toObject()
        .value("inside_solid")
        .toBool();
    };

    SECTION("creates six walls that do not overlap")
    {
      const auto result = f.ok(
        "create_hollow_box",
        R"({"inner_bounds":{"min":[0,0,0],"max":[256,128,96]},"wall_thickness":16,
            "material":"wall","side_materials":{"-z":"floor"}})");
      CHECK(
        result.value("walls")
        == QJsonValue{QJsonObject{
          {"+x", "B2"},
          {"-x", "B3"},
          {"+y", "B4"},
          {"-y", "B5"},
          {"+z", "B6"},
          {"-z", "B7"}}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Hollow Box"});
      CHECK(
        result.value("outer_bounds")
        == box(QJsonArray{-16, -16, -16}, QJsonArray{272, 144, 112}));
      CHECK(result.value("changes").toObject().value("created").toArray().size() == 6);
      CHECK(!result.contains("group"));

      // the exact layout: z slabs span the outer box, x walls the outer y and the inner
      // z, y walls the inner x and z
      CHECK(wallBounds("B2") == box({256, -16, 0}, {272, 144, 96}));
      CHECK(wallBounds("B3") == box({-16, -16, 0}, {0, 144, 96}));
      CHECK(wallBounds("B4") == box({0, 128, 0}, {256, 144, 96}));
      CHECK(wallBounds("B5") == box({0, -16, 0}, {256, 0, 96}));
      CHECK(wallBounds("B6") == box({-16, -16, 96}, {272, 144, 112}));
      CHECK(wallBounds("B7") == box({-16, -16, -16}, {272, 144, 0}));

      const auto nodes =
        f.ok("get_nodes", R"({"ids":["B6","B7"]})").value("nodes").toArray();
      CHECK(nodes[0].toObject().value("materials") == QJsonValue{QJsonArray{"wall"}});
      CHECK(nodes[1].toObject().value("materials") == QJsonValue{QJsonArray{"floor"}});

      const auto overlaps = f.ok("check_brush_overlaps");
      CHECK(overlaps.value("pairs") == QJsonValue{QJsonArray{}});
      CHECK(overlaps.value("brush_count") == QJsonValue{6});

      CHECK_FALSE(solid({128, 64, 48}));
      CHECK(solid({264, 64, 48}));
      CHECK(solid({-8, -8, -8}));
      CHECK(solid({128, 64, 104}));
      CHECK_FALSE(solid({300, 64, 48}));

      // the walls enclose the inner box exactly
      auto volume = 0.0;
      for (const auto& brush :
           f.ok("get_brush_geometry", R"({"ids":["B2","B3","B4","B5","B6","B7"]})")
             .value("brushes")
             .toArray())
      {
        volume += brush.toObject().value("volume").toDouble();
      }
      CHECK(volume == 288.0 * 160.0 * 128.0 - 256.0 * 128.0 * 96.0);
    }

    SECTION("omits sides and groups the walls")
    {
      const auto result = f.ok(
        "create_hollow_box",
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":8,
            "omit_sides":["+z","+x"],"group_name":"room"})");
      CHECK(
        result.value("walls")
        == QJsonValue{
          QJsonObject{{"-x", "B2"}, {"+y", "B3"}, {"-y", "B4"}, {"-z", "B5"}}});
      CHECK(result.value("group") == QJsonValue{"G6"});

      const auto group =
        f.ok("get_nodes", R"({"ids":["G6"]})").value("nodes").toArray()[0].toObject();
      CHECK(group.value("name") == QJsonValue{"room"});
      CHECK(group.value("child_ids") == QJsonValue{QJsonArray{"B2", "B3", "B4", "B5"}});
      // the walls are grouped in the same undo step and the selection is restored
      CHECK(f.map.selection().nodes.empty());
      REQUIRE(f.map.undoCommandName() != nullptr);
      CHECK(*f.map.undoCommandName() == "MCP: Create Hollow Box");

      CHECK_FALSE(solid({68, 32, 32}));
      CHECK_FALSE(solid({32, 32, 68}));
      CHECK(solid({-4, 32, 32}));
      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
    }

    SECTION("errors")
    {
      const auto scene = populateTestScene(f);
      const auto before = snapshot(f.map);
      const auto err = [&](const char* args) { return f.err("create_hollow_box", args); };

      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":0})")
        == "invalid_argument");
      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[64,0,64]},"wall_thickness":8})")
        == "invalid_argument");
      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[32768,64,64]},"wall_thickness":8})")
        == "out_of_world_bounds");
      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":8,
                "omit_sides":["+x","+x"]})")
        == "invalid_argument");
      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":8,
                "omit_sides":["+x","-x","+y","-y","+z","-z"]})")
        == "invalid_argument");
      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":8,
                "material":"a b"})")
        == "invalid_argument");
      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":8,
                "group_name":"a\"b"})")
        == "invalid_argument");
      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":8,
                "parent":"E4","group_name":"room"})")
        == "invalid_argument");
      CHECK(
        err(R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":8,
                "parent":"E3"})")
        == "invalid_parent");
      CHECK(snapshot(f.map) == before);
      (void)scene;
    }
  }

  SECTION("create_primitive_brush")
  {
    const auto geometry = [&](const QJsonArray& ids) {
      return f
        .ok("get_brush_geometry", QJsonObject{{"ids", ids}, {"include_faces", false}})
        .value("brushes")
        .toArray();
    };

    SECTION("creates single brush shapes")
    {
      struct Row
      {
        const char* args;
        qint64 faceCount;
      };
      const auto rows = std::vector<Row>{
        {R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[64,64,128]}})", 10},
        {R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[64,64,128]},"sides":12,
             "alignment":"vertex"})",
         14},
        {R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[128,64,64]},"axis":"x"})",
         10},
        {R"({"shape":"cone","bounds":{"min":[0,0,0],"max":[64,64,128]},"sides":12})", 13},
        {R"({"shape":"uv_sphere","bounds":{"min":[0,0,0],"max":[64,64,64]}})", 0},
        {R"({"shape":"ico_sphere","bounds":{"min":[0,0,0],"max":[64,64,64]}})", 0},
        {R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[64,64,64]},
             "alignment":"scalable","precision":1})",
         0},
      };
      for (const auto& row : rows)
      {
        CAPTURE(row.args);
        auto g = McpMapToolsFixture{};
        const auto result = g.ok("create_primitive_brush", row.args);
        REQUIRE(result.value("ids") == QJsonValue{QJsonArray{"B2"}});
        CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Primitive Brush"});

        const auto brush = g.ok("get_brush_geometry", R"({"ids":["B2"]})")
                             .value("brushes")
                             .toArray()[0]
                             .toObject();
        if (row.faceCount > 0)
        {
          CHECK(brush.value("face_count") == QJsonValue{row.faceCount});
        }
        CHECK(brush.value("valid").toObject().value("closed") == QJsonValue{true});

        // the shape stays within the bounds
        const auto args = parseObject(row.args);
        const auto bounds = brush.value("bounds").toObject();
        const auto requested = args.value("bounds").toObject();
        for (qsizetype i = 0; i < 3; ++i)
        {
          CHECK(
            bounds.value("min").toArray()[i].toDouble()
            >= requested.value("min").toArray()[i].toDouble() - 1e-6);
          CHECK(
            bounds.value("max").toArray()[i].toDouble()
            <= requested.value("max").toArray()[i].toDouble() + 1e-6);
        }
      }
    }

    SECTION("edge aligned cylinders fill their bounds")
    {
      f.ok(
        "create_primitive_brush",
        R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[64,64,128]}})");
      CHECK(
        geometry({"B2"})[0].toObject().value("bounds")
        == QJsonValue{
          QJsonObject{{"min", QJsonArray{0, 0, 0}}, {"max", QJsonArray{64, 64, 128}}}});
    }

    SECTION("creates hollow cylinders and arches from several brushes")
    {
      const auto cylinder = f.ok(
        "create_primitive_brush",
        R"({"shape":"hollow_cylinder","bounds":{"min":[0,0,0],"max":[128,128,64]},
            "sides":8,"thickness":16,"material":"stone"})");
      CHECK(cylinder.value("ids").toArray().size() == 8);
      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
      // the center is empty, the ring is solid
      const auto points = f.ok("query_point", R"({"points":[[64,64,32],[4,64,32]]})")
                            .value("results")
                            .toArray();
      CHECK(points[0].toObject().value("inside_solid") == QJsonValue{false});
      CHECK(points[1].toObject().value("inside_solid") == QJsonValue{true});

      const auto arch = f.ok(
        "create_primitive_brush",
        R"({"shape":"arch","bounds":{"min":[256,0,0],"max":[384,32,64]},"axis":"y",
            "thickness":16})");
      CHECK(arch.value("ids").toArray().size() >= 2);
      for (const auto& brush : geometry(arch.value("ids").toArray()))
      {
        CHECK(
          brush.toObject().value("valid").toObject().value("closed") == QJsonValue{true});
      }
    }

    SECTION("small primitives with many sides do not crash")
    {
      // regression: the vertices of a 1 unit cylinder with 64 sides are so close that
      // the convex hull treated three of them as colinear and violated a contract
      for (const auto* args : {
             R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[1,1,1]},"axis":"x",
                 "sides":64})",
             R"({"shape":"cylinder","bounds":{"min":[0.5,0,-3],"max":[2.5,1000,1]},
                 "axis":"x","alignment":"scalable","precision":3})",
           })
      {
        CAPTURE(args);
        const auto result = f.call("create_primitive_brush", args);
        if (!result.value("isError").toBool())
        {
          CHECK(f.doc->nodes.verifyBijection());
        }
      }
    }

    SECTION("warns about non-integer vertices")
    {
      const auto result = f.ok(
        "create_primitive_brush",
        R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[64,64,64]},"sides":7})");
      CHECK(compactJson(result.value("warnings")).contains("snap_vertices"));
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      const auto err = [&](const char* args) {
        return f.err("create_primitive_brush", args);
      };
      CHECK(
        err(R"({"shape":"hollow_cylinder","bounds":{"min":[0,0,0],"max":[64,64,64]}})")
        == "invalid_argument");
      // regression: BrushBuilder::createUvSphere reads past the end of its ring tables
      // for scalable circles with a precision above 0
      CHECK(
        err(R"({"shape":"uv_sphere","bounds":{"min":[0,0,0],"max":[64,64,64]},
                "alignment":"scalable","precision":1})")
        == "invalid_argument");
      CHECK(
        err(R"({"shape":"hollow_cylinder","bounds":{"min":[0,0,0],"max":[64,64,64]},
                "thickness":32})")
        == "invalid_argument");
      CHECK(
        err(R"({"shape":"arch","bounds":{"min":[0,0,0],"max":[64,64,64]},
                "thickness":0})")
        == "invalid_argument");
      CHECK(
        err(R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[64,64,0.5]}})")
        == "invalid_argument");
      CHECK(
        err(R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[64,64,64]},"sides":2})")
        == "invalid_argument");
      CHECK(
        err(R"({"shape":"cylinder","bounds":{"min":[0,0,0],"max":[64,64,64]},
                "alignment":"scalable","precision":4})")
        == "invalid_argument");
      CHECK(
        err(R"({"shape":"torus","bounds":{"min":[0,0,0],"max":[64,64,64]}})")
        == "invalid_argument");
      CHECK(
        err(R"({"shape":"cone","bounds":{"min":[0,0,0],"max":[64,64,64000]}})")
        == "out_of_world_bounds");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("create_brush_from_points")
  {
    SECTION("creates the convex hull")
    {
      const auto result = f.ok(
        "create_brush_from_points",
        R"({"points":[[0,0,0],[64,0,0],[0,64,0],[0,0,64],[8,8,8]],"material":"rock"})");
      CHECK(result.value("id") == QJsonValue{"B2"});
      CHECK(result.value("vertex_count") == QJsonValue{4});
      CHECK(result.value("face_count") == QJsonValue{4});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Brush From Points"});

      const auto brush = f.ok("get_brush_geometry", R"({"ids":["B2"]})")
                           .value("brushes")
                           .toArray()[0]
                           .toObject();
      CHECK(
        brush.value("vertices")
        == QJsonValue{QJsonArray{
          QJsonArray{0, 0, 0},
          QJsonArray{0, 0, 64},
          QJsonArray{0, 64, 0},
          QJsonArray{64, 0, 0}}});
      CHECK(
        f.ok("get_nodes", R"({"ids":["B2"]})")
          .value("nodes")
          .toArray()[0]
          .toObject()
          .value("materials")
        == QJsonValue{QJsonArray{"rock"}});
    }

    SECTION("almost colinear points close to each other")
    {
      // regression: is_colinear accepted the third point, but it was too far from the
      // edge of the first two, which violated a contract in the convex hull
      const auto result = f.ok(
        "create_brush_from_points",
        R"({"points":[[0,0,0],[0.5,0.003,0],[1,0,0],[2,5,0],[2,2,5]]})");
      CHECK(result.value("face_count").toInt() >= 4);
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      const auto err = [&](const char* args) {
        return f.err("create_brush_from_points", args);
      };
      CHECK(
        err(R"({"points":[[0,0,0],[64,0,0],[0,64,0],[64,64,0]]})") == "geometry_invalid");
      CHECK(err(R"({"points":[[0,0,0],[0,0,0],[0,0,0],[0,0,0]]})") == "geometry_invalid");
      CHECK(err(R"({"points":[[0,0,0],[64,0,0],[0,64,0]]})") == "invalid_argument");
      CHECK(
        err(R"({"points":[[0,0,0],[64,0,0],[0,64,0],[0,0,64000]]})")
        == "out_of_world_bounds");
      CHECK(
        err(R"({"points":[[0,0,0],[64,0,0],[0,64,0],[0,0,1e7]]})") == "invalid_argument");
      CHECK(
        err(R"({"points":[[0,0,0],[64,0,0],[0,64,0],[0,0,64]],"parent":"B999"})")
        == "unknown_node");
      CHECK(snapshot(f.map) == before);
    }
  }
}

} // namespace tb::ui
