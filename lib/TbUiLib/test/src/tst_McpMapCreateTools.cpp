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
}

} // namespace tb::ui
