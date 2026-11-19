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
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpCheckpoints.h"
#include "ui/McpJson.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

QJsonValue box(const QJsonArray& min, const QJsonArray& max)
{
  return QJsonObject{{"min", min}, {"max", max}};
}

} // namespace

TEST_CASE("McpMapHelperTools")
{
  auto f = McpMapToolsFixture{};

  const auto nodeOf = [&](const QString& id) {
    return f.ok("get_nodes", QJsonObject{{"ids", QJsonArray{id}}})
      .value("nodes")
      .toArray()[0]
      .toObject();
  };
  const auto boundsOf = [&](const QString& id) { return nodeOf(id).value("bounds"); };
  const auto solid = [&](const QJsonArray& point) {
    return f.ok("query_point", QJsonObject{{"points", QJsonArray{QJsonValue{point}}}})
      .value("results")
      .toArray()[0]
      .toObject()
      .value("inside_solid")
      .toBool();
  };
  const auto totalVolume = [&](const QJsonArray& ids) {
    auto volume = 0.0;
    for (const auto& brush :
         f.ok("get_brush_geometry", QJsonObject{{"ids", ids}, {"include_faces", false}})
           .value("brushes")
           .toArray())
    {
      volume += brush.toObject().value("volume").toDouble();
    }
    return volume;
  };

  SECTION("create_room")
  {
    SECTION("splits walls around the openings")
    {
      const auto result = f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[256,128,96]},
        "material":"base","floor_material":"floor","ceiling_material":"sky",
        "openings":[
          {"side":"+x","width":64,"height":72},
          {"side":"-y","offset":32,"width":32,"height":32,"bottom":40}
        ]})");

      CHECK(
        result.value("walls")
        == QJsonValue{QJsonObject{
          {"+x", QJsonArray{"B2", "B3", "B4"}},
          {"-x", QJsonArray{"B5"}},
          {"+y", QJsonArray{"B6"}},
          {"-y", QJsonArray{"B7", "B8", "B9", "B10"}},
        }});
      CHECK(result.value("floor") == QJsonValue{"B11"});
      CHECK(result.value("ceiling") == QJsonValue{"B12"});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Room"});
      CHECK(result.value("inner_bounds") == box({0, 0, 0}, {256, 128, 96}));
      CHECK(result.value("outer_bounds") == box({-16, -16, -16}, {272, 144, 112}));
      CHECK(
        result.value("openings")
        == QJsonValue{QJsonArray{
          QJsonObject{{"side", "+x"}, {"bounds", box({256, 32, 0}, {272, 96, 72})}},
          QJsonObject{{"side", "-y"}, {"bounds", box({144, -16, 40}, {176, 0, 72})}},
        }});

      // the +x wall: left of the door, above the door, right of the door
      CHECK(boundsOf("B2") == box({256, -16, 0}, {272, 32, 96}));
      CHECK(boundsOf("B3") == box({256, 32, 72}, {272, 96, 96}));
      CHECK(boundsOf("B4") == box({256, 96, 0}, {272, 144, 96}));
      CHECK(boundsOf("B5") == box({-16, -16, 0}, {0, 144, 96}));
      CHECK(boundsOf("B6") == box({0, 128, 0}, {256, 144, 96}));
      // the -y wall: left of the window, below, above, right of it
      CHECK(boundsOf("B7") == box({0, -16, 0}, {144, 0, 96}));
      CHECK(boundsOf("B8") == box({144, -16, 0}, {176, 0, 40}));
      CHECK(boundsOf("B9") == box({144, -16, 72}, {176, 0, 96}));
      CHECK(boundsOf("B10") == box({176, -16, 0}, {256, 0, 96}));
      CHECK(boundsOf("B11") == box({-16, -16, -16}, {272, 144, 0}));
      CHECK(boundsOf("B12") == box({-16, -16, 96}, {272, 144, 112}));

      CHECK(nodeOf("B2").value("materials") == QJsonValue{QJsonArray{"base"}});
      CHECK(nodeOf("B11").value("materials") == QJsonValue{QJsonArray{"floor"}});
      CHECK(nodeOf("B12").value("materials") == QJsonValue{QJsonArray{"sky"}});

      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
      CHECK_FALSE(solid({128, 64, 48}));
      CHECK_FALSE(solid({264, 64, 36}));
      CHECK(solid({264, 8, 36}));
      CHECK(solid({264, 64, 84}));
      CHECK_FALSE(solid({160, -8, 56}));
      CHECK(solid({160, -8, 20}));
      CHECK(solid({160, -8, 84}));

      const auto all =
        QJsonArray{"B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "B10", "B11", "B12"};
      CHECK(
        totalVolume(all)
        == 288.0 * 160.0 * 128.0 - 256.0 * 128.0 * 96.0 - 16.0 * 64.0 * 72.0
             - 16.0 * 32.0 * 32.0);
    }

    SECTION("defaults, wall_material and grouping")
    {
      const auto result = f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[64,64,64]},
        "wall_material":"brick","material":"base","group_name":"room"})");
      CHECK(result.value("outer_bounds") == box({-16, -16, -16}, {80, 80, 80}));
      CHECK(result.value("openings") == QJsonValue{QJsonArray{}});
      CHECK(result.value("group") == QJsonValue{"G8"});
      CHECK(nodeOf("B2").value("materials") == QJsonValue{QJsonArray{"brick"}});
      CHECK(nodeOf("B6").value("materials") == QJsonValue{QJsonArray{"base"}});
      CHECK(
        nodeOf("G8").value("child_ids")
        == QJsonValue{QJsonArray{"B2", "B3", "B4", "B5", "B6", "B7"}});
      REQUIRE(f.map.undoCommandName() != nullptr);
      CHECK(*f.map.undoCommandName() == "MCP: Create Room");
    }

    SECTION("an opening as large as the wall leaves only the corners")
    {
      const auto result = f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[64,128,64]},"wall_thickness":8,
        "openings":[{"side":"+x","width":128,"height":64}]})");
      CHECK(
        result.value("walls").toObject().value("+x")
        == QJsonValue{QJsonArray{"B2", "B3"}});
      CHECK(boundsOf("B2") == box({64, -8, 0}, {72, 0, 64}));
      CHECK(boundsOf("B3") == box({64, 128, 0}, {72, 136, 64}));
      CHECK_FALSE(solid({68, 64, 32}));
    }

    SECTION("openings side by side and stacked")
    {
      const auto result = f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[128,64,128]},"wall_thickness":8,
        "openings":[{"side":"+y","offset":-32,"width":32,"height":32},
                    {"side":"+y","offset":0,"width":32,"height":32},
                    {"side":"+y","offset":0,"width":32,"height":32,"bottom":64}]})");
      // columns [0,16] [16,48] [48,80] [80,128]; the third column has two openings
      const auto walls = result.value("walls").toObject().value("+y").toArray();
      REQUIRE(walls.size() == 5);
      CHECK(boundsOf(walls[0].toString()) == box({0, 64, 0}, {16, 72, 128}));
      CHECK(boundsOf(walls[1].toString()) == box({16, 64, 32}, {48, 72, 128}));
      CHECK(boundsOf(walls[2].toString()) == box({48, 64, 32}, {80, 72, 64}));
      CHECK(boundsOf(walls[3].toString()) == box({48, 64, 96}, {80, 72, 128}));
      CHECK(boundsOf(walls[4].toString()) == box({80, 64, 0}, {128, 72, 128}));
      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      const auto check = [&](const char* args, const char* code, const char* pointer) {
        CAPTURE(args);
        const auto error = f.error("create_room", args);
        CHECK(error.value("code") == QJsonValue{code});
        if (pointer)
        {
          CHECK(
            error.value("details").toObject().value("pointer") == QJsonValue{pointer});
        }
        CHECK(snapshot(f.map) == before);
      };

      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},
            "openings":[{"side":"+x","width":80,"height":32}]})",
        "invalid_argument",
        "/openings/0");
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},
            "openings":[{"side":"+x","offset":20,"width":32,"height":32}]})",
        "invalid_argument",
        "/openings/0");
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},
            "openings":[{"side":"-x","width":32,"height":32,"bottom":40}]})",
        "invalid_argument",
        "/openings/0");
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},
            "openings":[{"side":"-x","width":32,"height":32},
                        {"side":"-x","offset":16,"width":32,"height":16,"bottom":8}]})",
        "invalid_argument",
        "/openings/1");
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},
            "openings":[{"side":"+z","width":32,"height":32}]})",
        "invalid_argument",
        "/openings/0/side");
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},
            "openings":[{"side":"+x","width":0,"height":32}]})",
        "invalid_argument",
        "/openings/0/width");
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"wall_thickness":0})",
        "invalid_argument",
        "/wall_thickness");
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,0,64]}})",
        "invalid_argument",
        nullptr);
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"material":"a b"})",
        "invalid_argument",
        "/material");
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"parent":"B99"})",
        "unknown_node",
        nullptr);
      check(
        R"({"inner_bounds":{"min":[-40000,0,0],"max":[64,64,64]}})",
        "out_of_world_bounds",
        nullptr);
      check(
        R"({"inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"bogus":1})",
        "invalid_argument",
        "/bogus");
    }
  }

  SECTION("create_stairs")
  {
    SECTION("solid steps")
    {
      const auto result = f.ok(
        "create_stairs",
        R"({"start":[0,0,0],"direction":"+x","steps":3,"material":"step"})");
      CHECK(result.value("ids") == QJsonValue{QJsonArray{"B2", "B3", "B4"}});
      CHECK(result.value("top") == QJsonValue{QJsonArray{48, 0, 24}});
      CHECK(result.value("bounds") == box({0, -32, 0}, {48, 32, 24}));
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Stairs"});
      CHECK(boundsOf("B2") == box({0, -32, 0}, {16, 32, 8}));
      CHECK(boundsOf("B3") == box({16, -32, 0}, {32, 32, 16}));
      CHECK(boundsOf("B4") == box({32, -32, 0}, {48, 32, 24}));
      CHECK(nodeOf("B3").value("materials") == QJsonValue{QJsonArray{"step"}});
      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
      CHECK(!result.contains("group"));
    }

    SECTION("floating steps in -y, grouped")
    {
      const auto result = f.ok("create_stairs", R"({
        "start":[100,50,8],"direction":"-y","steps":2,"step_height":10,"step_depth":20,
        "width":40,"solid":false,"group_name":"stairs"})");
      CHECK(result.value("ids") == QJsonValue{QJsonArray{"B2", "B3"}});
      CHECK(result.value("top") == QJsonValue{QJsonArray{100, 10, 28}});
      CHECK(result.value("bounds") == box({80, 10, 8}, {120, 50, 28}));
      CHECK(result.value("group") == QJsonValue{"G4"});
      CHECK(boundsOf("B2") == box({80, 30, 8}, {120, 50, 18}));
      CHECK(boundsOf("B3") == box({80, 10, 18}, {120, 30, 28}));
      CHECK(nodeOf("G4").value("child_ids") == QJsonValue{QJsonArray{"B2", "B3"}});
    }

    SECTION("warns about steps that are too high")
    {
      const auto result = f.ok(
        "create_stairs",
        R"({"start":[0,0,0],"direction":"-x","steps":1,"step_height":24})");
      CHECK(boundsOf("B2") == box({-16, -32, 0}, {0, 32, 24}));
      CHECK(compactJson(result.value("warnings")).contains("18 units"));
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      const auto err = [&](const char* args) { return f.err("create_stairs", args); };
      CHECK(
        err(R"({"start":[0,0,8100],"direction":"+x","steps":128})")
        == "out_of_world_bounds");
      CHECK(err(R"({"start":[0,0,0],"direction":"+z","steps":1})") == "invalid_argument");
      CHECK(err(R"({"start":[0,0,0],"direction":"+x","steps":0})") == "invalid_argument");
      CHECK(
        err(R"({"start":[0,0,0],"direction":"+x","steps":129})") == "invalid_argument");
      CHECK(
        err(R"({"start":[0,0,0],"direction":"+x","steps":1,"width":0})")
        == "invalid_argument");
      CHECK(err(R"({"start":[0,0],"direction":"+x","steps":1})") == "invalid_argument");
      CHECK(
        err(
          R"({"start":[0,0,0],"direction":"+x","steps":1,"parent":"world","group_name":""})")
        == "invalid_argument");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("drop_to_floor")
  {
    auto* floor = addBrush(f.map, vm::bbox3d{{0, 0, 0}, {256, 256, 16}}, "floor");
    auto* light = addPointEntity(f.map, "light", {64, 64, 128});
    auto* crate = addBrush(f.map, vm::bbox3d{{100, 100, 64}, {132, 132, 96}}, "crate");
    REQUIRE(f.id(*floor) == "B2");
    REQUIRE(f.id(*light) == "E3");
    REQUIRE(f.id(*crate) == "B4");

    SECTION("drops entities and brushes onto the floor")
    {
      const auto result = f.ok("drop_to_floor", R"({"ids":["E3","B4"]})");
      CHECK(
        result.value("moved")
        == QJsonValue{QJsonArray{
          QJsonObject{
            {"id", "E3"},
            {"delta", QJsonArray{0, 0, -104}},
            {"on", "B2"},
            {"distance", 104}},
          QJsonObject{
            {"id", "B4"},
            {"delta", QJsonArray{0, 0, -48}},
            {"on", "B2"},
            {"distance", 48}},
        }});
      CHECK(result.value("not_moved") == QJsonValue{QJsonArray{}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Drop To Floor"});
      CHECK(light->entity().origin() == vm::vec3d{64, 64, 24});
      CHECK(boundsOf("B4") == box({100, 100, 16}, {132, 132, 48}));
      // one undo step
      f.map.undoCommand();
      CHECK(light->entity().origin() == vm::vec3d{64, 64, 128});
      CHECK(boundsOf("B4") == box({100, 100, 64}, {132, 132, 96}));
    }

    SECTION("gap and stacking on other nodes")
    {
      // the light is above the crate, which counts at its position before the call
      auto* lamp = addPointEntity(f.map, "light", {116, 116, 200});
      const auto result = f.ok(
        "drop_to_floor", QJsonObject{{"ids", QJsonArray{f.id(*lamp), "B4"}}, {"gap", 1}});
      const auto moved = result.value("moved").toArray();
      REQUIRE(moved.size() == 2);
      CHECK(moved[0].toObject().value("on") == QJsonValue{"B4"});
      CHECK(moved[0].toObject().value("delta") == QJsonValue{QJsonArray{0, 0, -95}});
      CHECK(moved[1].toObject().value("delta") == QJsonValue{QJsonArray{0, 0, -47}});
      CHECK(lamp->entity().origin() == vm::vec3d{116, 116, 105});
      CHECK(boundsOf("B4") == box({100, 100, 17}, {132, 132, 49}));
    }

    SECTION("sloped brushes are hit exactly")
    {
      // a ramp rising along +x from z 0 at x 300 to z 32 at x 364
      f.ok("create_brush_from_points", R"({"points":[[300,0,0],[364,0,0],[300,64,0],
        [364,64,0],[364,0,32],[364,64,32]]})");
      auto* box16 = addBrush(f.map, vm::bbox3d{{316, 16, 100}, {332, 32, 116}}, "crate");
      const auto result =
        f.ok("drop_to_floor", QJsonObject{{"ids", QJsonArray{f.id(*box16)}}});
      CHECK(
        result.value("moved").toArray()[0].toObject().value("delta")
        == QJsonValue{QJsonArray{0, 0, -84}});
      CHECK(boundsOf(f.id(*box16)) == box({316, 16, 16}, {332, 32, 32}));
    }

    SECTION("brush entities move as a whole, groups exclude their own brushes")
    {
      auto* door = addBrushEntity(
        f.map,
        "func_door",
        {vm::bbox3d{{0, 0, 32}, {16, 16, 64}}, vm::bbox3d{{16, 0, 40}, {32, 16, 72}}});
      auto* inner = addBrush(f.map, vm::bbox3d{{200, 200, 40}, {216, 216, 48}});
      auto* top = addBrush(f.map, vm::bbox3d{{200, 200, 80}, {216, 216, 96}});
      auto* group = groupNodes(f.map, {inner, top}, "pile");
      mdl::deselectAll(f.map);

      const auto result = f.ok(
        "drop_to_floor", QJsonObject{{"ids", QJsonArray{f.id(*door), f.id(*group)}}});
      CHECK(
        result.value("moved")
        == QJsonValue{QJsonArray{
          QJsonObject{
            {"id", f.id(*door)},
            {"delta", QJsonArray{0, 0, -16}},
            {"on", "B2"},
            {"distance", 16}},
          QJsonObject{
            {"id", f.id(*group)},
            {"delta", QJsonArray{0, 0, -24}},
            {"on", "B2"},
            {"distance", 24}},
        }});
      CHECK(door->logicalBounds() == vm::bbox3d{{0, 0, 16}, {32, 16, 56}});
      CHECK(inner->logicalBounds() == vm::bbox3d{{200, 200, 16}, {216, 216, 24}});
    }

    SECTION("nodes without a surface or already resting are not moved")
    {
      auto* outside = addPointEntity(f.map, "light", {1000, 1000, 100});
      auto* resting = addBrush(f.map, vm::bbox3d{{0, 0, 16}, {16, 16, 32}});
      auto* trigger =
        addBrushEntity(f.map, "trigger_once", {vm::bbox3d{{56, 56, 40}, {72, 72, 60}}});
      REQUIRE(trigger);
      // max_distance
      const auto far = f.ok("drop_to_floor", R"({"ids":["E3"],"max_distance":50})");
      CHECK(far.value("moved") == QJsonValue{QJsonArray{}});
      CHECK(
        far.value("not_moved").toArray()[0].toObject().value("reason")
        == QJsonValue{"no_surface"});

      const auto result = f.ok(
        "drop_to_floor",
        QJsonObject{
          {"ids", QJsonArray{f.id(*outside), f.id(*resting), "E3"}},
          {"max_distance", 500}});
      CHECK(
        result.value("not_moved").toArray()[0].toObject().value("reason")
        == QJsonValue{"no_surface"});
      CHECK(
        result.value("not_moved").toArray()[1].toObject()
        == QJsonObject{
          {"id", f.id(*resting)}, {"reason", "already_in_place"}, {"on", "B2"}});
      // the trigger is ignored
      CHECK(
        result.value("moved").toArray()[0].toObject().value("on") == QJsonValue{"B2"});
    }

    SECTION("other directions")
    {
      addBrush(f.map, vm::bbox3d{{0, 0, 256}, {256, 256, 272}}, "ceiling");
      addBrush(f.map, vm::bbox3d{{-16, 0, 0}, {0, 256, 256}}, "wall");
      const auto up = f.ok("drop_to_floor", R"({"ids":["B4"],"direction":"+z"})");
      CHECK(
        up.value("moved").toArray()[0].toObject().value("delta")
        == QJsonValue{QJsonArray{0, 0, 160}});
      const auto west = f.ok("drop_to_floor", R"({"ids":["B4"],"direction":"-x"})");
      CHECK(
        west.value("moved").toArray()[0].toObject().value("delta")
        == QJsonValue{QJsonArray{-100, 0, 0}});
      CHECK(boundsOf("B4") == box({0, 100, 224}, {32, 132, 256}));
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(f.err("drop_to_floor", R"({"ids":["world"]})") == "wrong_node_type");
      CHECK(f.err("drop_to_floor", R"({"ids":["B99"]})") == "unknown_node");
      CHECK(f.err("drop_to_floor", R"({"ids":["@selection"]})") == "invalid_argument");
      CHECK(
        f.err("drop_to_floor", R"({"ids":["B4"],"direction":"down"})")
        == "invalid_argument");
      CHECK(f.err("drop_to_floor", R"({"ids":["B4"],"gap":-1})") == "invalid_argument");
      CHECK(
        f.err("drop_to_floor", R"({"ids":["B4"],"max_distance":0})")
        == "invalid_argument");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("align_nodes")
  {
    auto* reference = addBrush(f.map, vm::bbox3d{{0, 0, 0}, {100, 100, 100}});
    auto* target = addBrush(f.map, vm::bbox3d{{10, 10, 10}, {20, 20, 20}});
    REQUIRE(f.id(*reference) == "B2");
    REQUIRE(f.id(*target) == "B3");

    const auto align = [&](const char* args) {
      const auto result = f.ok("align_nodes", args);
      const auto bounds = boundsOf("B3");
      f.map.undoCommand();
      return std::pair{result, bounds};
    };

    CHECK(
      align(R"({"ids":["B3"],"to":"B2","side":"+z"})").second
      == box({10, 10, 90}, {20, 20, 100}));
    CHECK(
      align(R"({"ids":["B3"],"to":"B2","side":"+z","mode":"flush_outside"})").second
      == box({10, 10, 100}, {20, 20, 110}));
    CHECK(
      align(R"({"ids":["B3"],"to":"B2","side":"-x","mode":"flush_outside"})").second
      == box({-10, 10, 10}, {0, 20, 20}));
    CHECK(
      align(R"({"ids":["B3"],"to":"B2","side":"-y"})").second
      == box({10, 0, 10}, {20, 10, 20}));
    CHECK(
      align(R"({"ids":["B3"],"to":"B2","side":"+x","mode":"center"})").second
      == box({45, 10, 10}, {55, 20, 20}));
    CHECK(
      align(
        R"({"ids":["B3"],"to":"B2","side":"+z","mode":"flush_outside","axes_only":false})")
        .second
      == box({45, 45, 100}, {55, 55, 110}));
    CHECK(
      align(R"({"ids":["B3"],"to":{"min":[200,0,0],"max":[300,50,50]},"side":"+x"})")
        .second
      == box({290, 10, 10}, {300, 20, 20}));

    const auto [result, bounds] = align(R"({"ids":["B3"],"to":"B2","side":"+z"})");
    CHECK(
      result.value("moved")
      == QJsonValue{
        QJsonArray{QJsonObject{{"id", "B3"}, {"delta", QJsonArray{0, 0, 80}}}}});
    CHECK(result.value("unchanged") == QJsonValue{QJsonArray{}});
    CHECK(result.value("undo_label") == QJsonValue{"MCP: Align Nodes"});

    SECTION("aligned nodes are unchanged")
    {
      const auto unchanged = f.ok(
        "align_nodes",
        R"({"ids":["B3","B2"],"to":"B2","side":"-z","mode":"flush_inside","axes_only":true})");
      CHECK(unchanged.value("unchanged") == QJsonValue{QJsonArray{"B2"}});
      CHECK(unchanged.value("moved").toArray().size() == 1);
    }

    SECTION("aligns to the contents of a layer")
    {
      CHECK(
        align(R"({"ids":["B3"],"to":"default_layer","side":"+x"})").second
        == box({90, 10, 10}, {100, 20, 20}));
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(
        f.err("align_nodes", R"({"ids":["B3"],"to":"B99","side":"+z"})")
        == "unknown_node");
      CHECK(
        f.err(
          "align_nodes",
          R"({"ids":["B3"],"to":{"min":[0,0,0],"max":[10,10,-10]},"side":"+z"})")
        == "invalid_argument");
      CHECK(
        f.err("align_nodes", R"({"ids":["B3"],"to":5,"side":"+z"})")
        == "invalid_argument");
      CHECK(
        f.err("align_nodes", R"({"ids":["B3"],"to":"B2","side":"+z","mode":"x"})")
        == "invalid_argument");
      CHECK(
        f.err(
          "align_nodes",
          R"({"ids":["B3"],"to":{"min":[0,0,8100],"max":[10,10,8129]},"side":"+z","mode":"flush_outside"})")
        == "out_of_world_bounds");
      CHECK(
        f.err("align_nodes", R"({"ids":["default_layer"],"to":"B2","side":"+z"})")
        == "wrong_node_type");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("get_conventions")
  {
    const auto result = f.ok("get_conventions");
    CHECK(result.value("up_axis") == QJsonValue{"+z"});
    CHECK(result.value("handedness") == QJsonValue{"right"});
    CHECK(result.value("grid_size") == QJsonValue{canon(f.map.grid().actualSize())});
    CHECK(result.value("world_bounds") == QJsonValue{toJson(f.map.worldBounds())});
    CHECK(
      result.value("player").toObject()
      == QJsonObject{
        {"width", 32},
        {"height", 56},
        {"eye_height", 22},
        {"step_height", 18},
        {"jump_height", 45},
        {"source", "default"}});
    CHECK(
      result.value("common_dimensions").toObject().value("door")
      == QJsonValue{QJsonObject{{"width", 64}, {"height", 96}}});
    CHECK(result.value("map_format") == QJsonValue{"Valve"});
    CHECK(result.contains("soft_map_bounds"));
    CHECK(result.contains("material_default"));
    // deterministic
    CHECK(f.ok("get_conventions") == result);
    CHECK(f.err("get_conventions", R"({"x":1})") == "invalid_argument");
  }

  SECTION("describe_region")
  {
    const auto scene = populateTestScene(f);
    (void)scene;

    SECTION("summarizes the nodes in the region")
    {
      const auto result =
        f.ok("describe_region", R"({"bounds":{"min":[0,0,0],"max":[64,64,64]}})");
      CHECK(
        result.value("summary")
        == QJsonValue{
          "Region (0 0 0) to (64 64 64) (size 64 x 64 x 64): 1 brushes (materials: "
          "wall), 1 entities (info_player_start x1), 0 groups, 1 patches. Contents "
          "span (0 0 0) to (64 64 64). The center (32 32 32) is inside a world brush."});
      CHECK(
        result.value("counts").toObject()
        == QJsonObject{
          {"brushes", 1},
          {"entities", 1},
          {"point_entities", 1},
          {"brush_entities", 0},
          {"groups", 0},
          {"patches", 1}});
      CHECK(
        result.value("brushes")
        == QJsonValue{QJsonArray{QJsonObject{
          {"id", "B2"},
          {"entity", "world"},
          {"bounds", box({0, 0, 0}, {64, 64, 64})},
          {"materials", QJsonArray{"wall"}}}}});
      CHECK(
        result.value("entities")
        == QJsonValue{QJsonArray{QJsonObject{
          {"id", "E11"},
          {"classname", "info_player_start"},
          {"point_entity", true},
          {"origin", QJsonArray{16, 16, 24}}}}});
      CHECK(result.value("center_solid") == QJsonValue{true});
      CHECK(result.value("truncated") == QJsonValue{false});
    }

    SECTION("brush entities, groups and truncation")
    {
      const auto result = f.ok(
        "describe_region",
        R"({"bounds":{"min":[100,0,0],"max":[200,200,200]},"max_items":1})");
      CHECK(
        result.value("entities")
        == QJsonValue{QJsonArray{QJsonObject{
          {"id", "E4"},
          {"classname", "func_door"},
          {"point_entity", false},
          {"bounds", box({128, 0, 0}, {176, 16, 64})},
          {"brush_count", 2}}}});
      CHECK(result.value("counts").toObject().value("brushes") == QJsonValue{2});
      CHECK(result.value("brushes").toArray().size() == 1);
      CHECK(
        result.value("brushes").toArray()[0].toObject().value("entity")
        == QJsonValue{"E4"});
      CHECK(result.value("truncated") == QJsonValue{true});
      CHECK(result.value("center_solid") == QJsonValue{false});

      const auto group =
        f.ok("describe_region", R"({"bounds":{"min":[0,128,0],"max":[8,136,8]}})");
      CHECK(
        group.value("groups")
        == QJsonValue{QJsonArray{QJsonObject{
          {"id", "G8"}, {"name", "room"}, {"bounds", box({0, 128, 0}, {64, 192, 16})}}}});
    }

    SECTION("an empty region")
    {
      const auto result =
        f.ok("describe_region", R"({"bounds":{"min":[5000,0,0],"max":[5100,10,10]}})");
      CHECK(result.value("content_bounds") == QJsonValue{});
      CHECK(result.value("summary").toString().contains("0 brushes, 0 entities"));
      CHECK(result.value("summary").toString().endsWith("is empty."));
    }

    SECTION("errors")
    {
      CHECK(
        f.err("describe_region", R"({"bounds":{"min":[0,0,0],"max":[-1,0,0]}})")
        == "invalid_argument");
      CHECK(
        f.err(
          "describe_region", R"({"bounds":{"min":[0,0,0],"max":[1,1,1]},"max_items":0})")
        == "invalid_argument");
    }
  }
}

} // namespace tb::ui
