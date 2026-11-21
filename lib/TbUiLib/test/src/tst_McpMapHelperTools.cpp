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
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_NodeLocking.h"
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

  SECTION("create_opening")
  {
    const auto faceOn = [&](const QString& id, const QString& side) {
      for (const auto& face :
           f.ok("get_brush_geometry", QJsonObject{{"ids", QJsonArray{id}}})
             .value("brushes")
             .toArray()[0]
             .toObject()
             .value("faces")
             .toArray())
      {
        if (face.toObject().value("side") == QJsonValue{side})
        {
          return face.toObject();
        }
      }
      FAIL("no face with side " << side.toStdString());
      return QJsonObject{};
    };

    SECTION("cuts into the wall of a moved room group like create_room")
    {
      f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[256,128,96]},"group_name":"room"})");
      f.ok("transform_nodes", R"({"ids":["G8"],"translate":[64,0,0]})");

      const auto result =
        f.ok("create_opening", R"({"wall":"G8","side":"+x","width":64,"height":72})");
      CHECK(
        result.value("opening")
        == QJsonValue{
          QJsonObject{{"side", "+x"}, {"bounds", box({320, 32, 0}, {336, 96, 72})}}});
      CHECK(
        result.value("pieces")
        == QJsonValue{QJsonObject{{"B2", QJsonArray{"B2", "B9", "B10"}}}});
      CHECK(result.value("removed") == QJsonValue{QJsonArray{}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Opening"});

      // the same pieces as create_room makes for this opening
      CHECK(boundsOf("B2") == box({320, -16, 0}, {336, 32, 96}));
      CHECK(boundsOf("B9") == box({320, 32, 72}, {336, 96, 96}));
      CHECK(boundsOf("B10") == box({320, 96, 0}, {336, 144, 96}));
      // the pieces stay in the group
      CHECK(
        nodeOf("G8").value("child_ids")
        == QJsonValue{QJsonArray{"B2", "B3", "B4", "B5", "B6", "B7", "B9", "B10"}});
      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
      CHECK_FALSE(solid({328, 64, 36}));
      CHECK(solid({328, 64, 84}));
      CHECK(solid({328, 16, 36}));

      SECTION("a second opening splits the pieces it touches")
      {
        const auto second = f.ok("create_opening", R"({
          "wall":"G8","side":"+x","center":16,"width":16,"height":16,"bottom":40})");
        CHECK(
          second.value("opening").toObject().value("bounds")
          == box({320, 8, 40}, {336, 24, 56}));
        CHECK(
          second.value("pieces")
          == QJsonValue{QJsonObject{{"B2", QJsonArray{"B2", "B11", "B12", "B13"}}}});
        CHECK(boundsOf("B2") == box({320, -16, 0}, {336, 8, 96}));
        CHECK(boundsOf("B11") == box({320, 8, 0}, {336, 24, 40}));
        CHECK(boundsOf("B12") == box({320, 8, 56}, {336, 24, 96}));
        CHECK(boundsOf("B13") == box({320, 24, 0}, {336, 32, 96}));
        CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
        CHECK_FALSE(solid({328, 16, 48}));
      }

      SECTION("pieces of a cut wall are not taken for the neighbouring walls")
      {
        // B10 reaches the +y face of the room, but it belongs to the +x wall
        const auto next = f.ok("create_opening", R"({
          "wall":"G8","side":"+y","offset":96,"width":32,"height":32})");
        CHECK(
          next.value("opening")
          == QJsonValue{
            QJsonObject{{"side", "+y"}, {"bounds", box({272, 128, 0}, {304, 144, 32})}}});
        CHECK(
          next.value("pieces")
          == QJsonValue{QJsonObject{{"B4", QJsonArray{"B4", "B11", "B12"}}}});
        CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
        CHECK(
          f.err("create_opening", R"({"wall":"G8","side":"+y","width":257,"height":32})")
          == "invalid_argument");
        CHECK(
          f.err("create_opening", R"({"wall":"G8","side":"+x","width":129,"height":32})")
          == "invalid_argument");
      }

      SECTION("an opening over several pieces")
      {
        // spans the door and the pieces on both sides of it
        const auto second = f.ok("create_opening", R"({
          "wall":"G8","side":"+x","width":96,"height":16,"bottom":76})");
        CHECK(
          second.value("pieces")
          == QJsonValue{QJsonObject{
            {"B2", QJsonArray{"B2", "B11", "B12"}},
            {"B9", QJsonArray{"B9", "B13"}},
            {"B10", QJsonArray{"B10", "B14", "B15"}},
          }});
        CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
        CHECK_FALSE(solid({328, 24, 84}));
        CHECK_FALSE(solid({328, 64, 84}));
        CHECK(solid({328, 64, 94}));
        CHECK(
          totalVolume(QJsonArray{"B2", "B9", "B10", "B11", "B12", "B13", "B14", "B15"})
          == 16.0 * 160.0 * 96.0 - 16.0 * 64.0 * 72.0 - 16.0 * 96.0 * 16.0);
      }

      SECTION("undo restores the wall")
      {
        f.ok("undo");
        CHECK(boundsOf("B2") == box({320, -16, 0}, {336, 144, 96}));
        CHECK(solid({328, 64, 36}));
      }
    }

    SECTION("cuts into every wall of a room rotated by 90 degrees")
    {
      // after the rotation, the y walls span the corners instead of the x walls
      f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[256,128,96]},"group_name":"room"})");
      f.ok(
        "transform_nodes",
        R"({"ids":["G8"],"rotate":{"axis":"z","angle_degrees":90,"center":[0,0,0]}})");
      // inner bounds are now x -128..0, y 0..256
      const auto py =
        f.ok("create_opening", R"({"wall":"G8","side":"+y","width":64,"height":72})");
      CHECK(
        py.value("opening")
        == QJsonValue{
          QJsonObject{{"side", "+y"}, {"bounds", box({-96, 256, 0}, {-32, 272, 72})}}});
      const auto px =
        f.ok("create_opening", R"({"wall":"G8","side":"+x","width":64,"height":72})");
      CHECK(
        px.value("opening")
        == QJsonValue{
          QJsonObject{{"side", "+x"}, {"bounds", box({0, 96, 0}, {16, 160, 72})}}});
      // the opening may span the whole inner side of the wall, but not the corners
      f.ok("create_opening", R"({"wall":"G8","side":"-x","width":256,"height":32})");
      CHECK(
        f.err("create_opening", R"({"wall":"G8","side":"-y","width":129,"height":32})")
        == "invalid_argument");
      f.ok("create_opening", R"({"wall":"G8","side":"-y","width":128,"height":32})");
      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
      CHECK_FALSE(solid({-64, 264, 36}));
      CHECK_FALSE(solid({8, 128, 36}));
      CHECK(solid({8, 264, 36}));
    }

    SECTION("a wall brush keeps its face attributes")
    {
      auto* wall = addBrush(f.map, vm::bbox3d{{0, 0, 0}, {16, 128, 96}}, "brick");
      REQUIRE(f.id(*wall) == "B2");
      f.ok(
        "set_face_attributes",
        R"({"faces":[{"brush":"B2","side":"+x"}],"offset":[5,7]})");

      const auto result = f.ok(
        "create_opening", R"({"wall":"B2","width":32,"height":64,"material":"trim"})");
      // without side, the thinner horizontal axis is the thickness
      CHECK(
        result.value("opening")
        == QJsonValue{QJsonObject{{"bounds", box({0, 48, 0}, {16, 80, 64})}}});
      CHECK(
        result.value("pieces")
        == QJsonValue{QJsonObject{{"B2", QJsonArray{"B2", "B3", "B4"}}}});
      CHECK(boundsOf("B2") == box({0, 0, 0}, {16, 48, 96}));
      CHECK(boundsOf("B3") == box({0, 48, 64}, {16, 80, 96}));
      CHECK(boundsOf("B4") == box({0, 80, 0}, {16, 128, 96}));

      // outer faces keep material and offset; the faces in the opening get the material
      for (const auto& id : {"B2", "B3", "B4"})
      {
        CAPTURE(id);
        CHECK(faceOn(id, "+x").value("material") == QJsonValue{"brick"});
        CHECK(
          faceOn(id, "+x").value("uv").toObject().value("offset")
          == QJsonValue{QJsonArray{5, 7}});
        CHECK(faceOn(id, "-x").value("material") == QJsonValue{"brick"});
      }
      CHECK(faceOn("B2", "+y").value("material") == QJsonValue{"trim"});
      CHECK(faceOn("B2", "-y").value("material") == QJsonValue{"brick"});
      CHECK(faceOn("B3", "-z").value("material") == QJsonValue{"trim"});
      CHECK(faceOn("B3", "+z").value("material") == QJsonValue{"brick"});
      CHECK(faceOn("B4", "-y").value("material") == QJsonValue{"trim"});
      CHECK(faceOn("B4", "+y").value("material") == QJsonValue{"brick"});
    }

    SECTION("without material, the new faces copy the wall's faces")
    {
      addBrush(f.map, vm::bbox3d{{0, 0, 0}, {128, 8, 64}}, "brick");
      const auto result = f.ok(
        "create_opening",
        R"({"wall":"B2","side":"-y","offset":-32,"width":32,"height":32,"bottom":16})");
      CHECK(
        result.value("opening")
        == QJsonValue{
          QJsonObject{{"side", "-y"}, {"bounds", box({16, 0, 16}, {48, 8, 48})}}});
      CHECK(result.value("pieces").toObject().value("B2").toArray().size() == 4);
      for (const auto& id : {"B2", "B3", "B4", "B5"})
      {
        CHECK(nodeOf(id).value("materials") == QJsonValue{QJsonArray{"brick"}});
      }
    }

    SECTION("an opening as large as a brush removes it")
    {
      addBrush(f.map, vm::bbox3d{{0, 0, 0}, {16, 64, 64}});
      const auto result =
        f.ok("create_opening", R"({"wall":"B2","width":64,"height":64})");
      CHECK(result.value("pieces") == QJsonValue{QJsonObject{}});
      CHECK(result.value("removed") == QJsonValue{QJsonArray{"B2"}});
      CHECK(
        result.value("changes").toObject().value("removed")
        == QJsonValue{QJsonArray{"B2"}});
    }

    SECTION("removing the only brush of the open group")
    {
      auto* brush = addBrush(f.map, vm::bbox3d{{0, 0, 0}, {16, 64, 64}});
      groupNodes(f.map, {brush}, "wall");
      mdl::deselectAll(f.map);
      f.ok("open_group", R"({"id":"G3"})");
      const auto result =
        f.ok("create_opening", R"({"wall":"B2","width":64,"height":64})");
      CHECK(result.value("removed") == QJsonValue{QJsonArray{"B2"}});
      CHECK(f.map.editorContext().currentGroup() == nullptr);
      // undo brings the brush and its group back (under new ids)
      f.ok("undo");
      CHECK(solid({8, 32, 32}));
    }

    SECTION("errors")
    {
      f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[256,128,96]},"group_name":"room"})");
      addBrush(f.map, vm::bbox3d{{512, 0, 0}, {576, 64, 64}});
      auto* light = addPointEntity(f.map, "light", {64, 64, 64});
      f.ok("create_brush_from_points", R"({"points":[[600,0,0],[664,0,0],[600,64,0],
        [664,64,0],[664,0,32],[664,64,32]]})");
      auto* floorOnly = addBrush(f.map, vm::bbox3d{{0, 512, 0}, {64, 576, 16}});
      groupNodes(f.map, {floorOnly}, "floor");
      mdl::deselectAll(f.map);
      REQUIRE(f.id(*light) == "E10");
      const auto wedge = QString{"B11"};

      const auto before = snapshot(f.map);
      const auto check = [&](const QString& args, const char* code, const char* pointer) {
        CAPTURE(args);
        const auto error = f.error("create_opening", args.toUtf8().constData());
        CHECK(error.value("code") == QJsonValue{code});
        if (pointer)
        {
          CHECK(
            error.value("details").toObject().value("pointer") == QJsonValue{pointer});
        }
        CHECK(snapshot(f.map) == before);
      };

      check(R"({"wall":"G8","width":32,"height":32})", "invalid_argument", "/side");
      check(
        R"({"wall":"G8","side":"+x","width":160,"height":32})",
        "invalid_argument",
        "/width");
      check(
        R"({"wall":"G8","side":"+x","width":32,"height":32,"offset":60})",
        "invalid_argument",
        "/width");
      check(
        R"({"wall":"G8","side":"-y","width":32,"height":32,"bottom":80})",
        "invalid_argument",
        "/height");
      check(
        R"({"wall":"G8","side":"-y","width":32,"height":32,"offset":0,"center":4})",
        "invalid_argument",
        "/center");
      check(
        R"({"wall":"G8","side":"+z","width":32,"height":32})",
        "invalid_argument",
        "/side");
      check(R"({"wall":"B9","width":32,"height":32})", "invalid_argument", "/side");
      check(R"({"wall":"E10","width":32,"height":32})", "wrong_node_type", "/wall");
      check(
        QString{R"({"wall":"%1","side":"+x","width":32,"height":16})"}.arg(wedge),
        "not_supported",
        nullptr);
      check(
        R"({"wall":"G13","side":"+x","width":32,"height":8})",
        "invalid_argument",
        "/side");
      check(R"({"wall":"B99","width":32,"height":32})", "unknown_node", "/wall");
      check(
        R"({"wall":"G8","side":"+x","width":32,"height":32,"material":"a b"})",
        "invalid_argument",
        "/material");
      check(
        R"({"wall":"G8","side":"+x","width":32,"height":0.0001})",
        "invalid_argument",
        nullptr);

      // an opening that overlaps an existing one
      f.ok("create_opening", R"({"wall":"G8","side":"+x","width":32,"height":32})");
      const auto again = snapshot(f.map);
      const auto error =
        f.error("create_opening", R"({"wall":"G8","side":"+x","width":64,"height":32})");
      CHECK(error.value("code") == QJsonValue{"invalid_argument"});
      CHECK(error.value("message").toString().contains("existing opening"));
      CHECK(snapshot(f.map) == again);
    }

    SECTION("locked and linked walls are refused")
    {
      f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[128,128,96]},"group_name":"room"})");
      auto* group = dynamic_cast<mdl::GroupNode*>(f.doc->nodes.lookup(u"G8").node);
      REQUIRE(group);
      const auto* wall = group->children().front();
      const auto args = R"({"wall":"G8","side":"+x","width":32,"height":32})";

      mdl::lockNodes(f.map, {group});
      CHECK(f.err("create_opening", args) == "node_not_selectable");
      CHECK(
        f.err("create_opening", R"({"wall":"B2","width":32,"height":32})")
        == "node_not_selectable");
      mdl::unlockNodes(f.map, {group});

      mdl::selectNodes(f.map, {group});
      REQUIRE(mdl::createLinkedDuplicate(f.map) != nullptr);
      mdl::deselectAll(f.map);
      const auto before = snapshot(f.map);
      CHECK(f.err("create_opening", args) == "not_supported");
      CHECK(
        f.err("create_opening", R"({"wall":"B2","width":32,"height":32})")
        == "not_supported");
      CHECK(snapshot(f.map) == before);
      CHECK(f.doc->nodes.isLive(wall));
    }
  }

  SECTION("create_corridor")
  {
    const auto addLights = [&](const std::vector<vm::vec3d>& origins) {
      for (const auto& origin : origins)
      {
        addPointEntity(f.map, "light", origin);
      }
    };
    const auto sealed = [&]() { return f.ok("check_leaks").value("sealed").toBool(); };

    SECTION("straight between two rooms")
    {
      const auto a = f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[512,512,256]},
        "openings":[{"side":"+x","width":128,"height":128}]})");
      const auto b = f.ok("create_room", R"({
        "inner_bounds":{"min":[640,0,0],"max":[1152,512,256]},
        "openings":[{"side":"-x","width":128,"height":128}]})");
      const auto from = a.value("openings").toArray()[0];
      const auto to = b.value("openings").toArray()[0];
      addLights({{256, 256, 128}, {896, 256, 128}});
      CHECK_FALSE(sealed());

      const auto result = f.ok(
        "create_corridor", QJsonObject{{"from", from}, {"to", to}, {"material", "c"}});
      CHECK(result.value("shape") == QJsonValue{"straight"});
      CHECK(
        result.value("interior_bounds")
        == QJsonValue{QJsonArray{box({528, 192, 0}, {624, 320, 128})}});
      const auto ids = result.value("ids").toArray();
      REQUIRE(ids.size() == 4);
      CHECK(result.value("floor") == QJsonValue{QJsonArray{ids[0]}});
      CHECK(result.value("ceiling") == QJsonValue{QJsonArray{ids[1]}});
      CHECK(result.value("walls") == QJsonValue{QJsonArray{ids[2], ids[3]}});
      CHECK(boundsOf(ids[0].toString()) == box({528, 176, -16}, {624, 336, 0}));
      CHECK(boundsOf(ids[1].toString()) == box({528, 176, 128}, {624, 336, 144}));
      CHECK(boundsOf(ids[2].toString()) == box({528, 176, 0}, {624, 192, 128}));
      CHECK(boundsOf(ids[3].toString()) == box({528, 320, 0}, {624, 336, 128}));
      CHECK(nodeOf(ids[0].toString()).value("materials") == QJsonValue{QJsonArray{"c"}});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Create Corridor"});
      CHECK_FALSE(result.contains("group"));

      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
      CHECK(sealed());
      CHECK_FALSE(solid({576, 256, 64}));
      CHECK_FALSE(solid({520, 256, 64}));

      SECTION("the corridor is needed to seal the rooms")
      {
        f.ok("delete_nodes", QJsonObject{{"ids", QJsonArray{ids[1]}}});
        CHECK_FALSE(sealed());
      }

      SECTION("the other way round gives the same corridor")
      {
        auto expected = QJsonArray{};
        for (const auto& id : ids)
        {
          expected.append(boundsOf(id.toString()));
        }
        f.ok("undo");
        const auto back = f.ok(
          "create_corridor",
          QJsonObject{{"from", to}, {"to", from}, {"group_name", "c"}});
        CHECK(back.value("interior_bounds") == result.value("interior_bounds"));
        const auto backIds = back.value("ids").toArray();
        REQUIRE(backIds.size() == 4);
        for (qsizetype i = 0; i < 4; ++i)
        {
          CHECK(boundsOf(backIds[i].toString()) == expected[i]);
        }
        CHECK(back.contains("group"));
        CHECK(sealed());
      }
    }

    SECTION("L-shaped between openings cut with create_opening")
    {
      f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[256,256,128]},"group_name":"a"})");
      f.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[256,256,128]},"group_name":"b"})");
      // the room is built in place and moved, as an agent would
      f.ok("transform_nodes", R"({"ids":["G15"],"translate":[400,400,0]})");
      const auto from =
        f.ok("create_opening", R"({"wall":"G8","side":"+x","width":64,"height":96})")
          .value("opening");
      const auto to =
        f.ok("create_opening", R"({"wall":"G15","side":"-y","width":64,"height":96})")
          .value("opening");
      CHECK(
        to
        == QJsonValue{
          QJsonObject{{"side", "-y"}, {"bounds", box({496, 384, 0}, {560, 400, 96})}}});
      addLights({{128, 128, 64}, {528, 528, 64}});

      const auto result =
        f.ok("create_corridor", QJsonObject{{"from", from}, {"to", to}});
      CHECK(result.value("shape") == QJsonValue{"l_shaped"});
      CHECK(
        result.value("interior_bounds")
        == QJsonValue{QJsonArray{
          box({272, 96, 0}, {496, 160, 96}),
          box({496, 96, 0}, {560, 160, 96}),
          box({496, 160, 0}, {560, 384, 96}),
        }});
      const auto boundsList = [&](const QJsonValue& ids) {
        auto list = QJsonArray{};
        for (const auto& id : ids.toArray())
        {
          list.append(boundsOf(id.toString()));
        }
        return list;
      };
      CHECK(
        boundsList(result.value("floor"))
        == QJsonArray{
          box({272, 80, -16}, {576, 176, 0}), box({480, 176, -16}, {576, 384, 0})});
      CHECK(
        boundsList(result.value("ceiling"))
        == QJsonArray{
          box({272, 80, 96}, {576, 176, 112}), box({480, 176, 96}, {576, 384, 112})});
      CHECK(
        boundsList(result.value("walls"))
        == QJsonArray{
          box({272, 80, 0}, {576, 96, 96}),
          box({560, 96, 0}, {576, 384, 96}),
          box({272, 160, 0}, {496, 176, 96}),
          box({480, 176, 0}, {496, 384, 96}),
        });
      CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
      CHECK(sealed());
      CHECK_FALSE(solid({528, 128, 48}));
      CHECK_FALSE(solid({528, 300, 48}));
      CHECK_FALSE(compactJson(result.value("warnings")).contains("overlap"));

      SECTION("the other way round gives the same space")
      {
        f.ok("undo");
        const auto back =
          f.ok("create_corridor", QJsonObject{{"from", to}, {"to", from}});
        CHECK(back.value("shape") == QJsonValue{"l_shaped"});
        const auto interiors = back.value("interior_bounds").toArray();
        REQUIRE(interiors.size() == 3);
        CHECK(interiors[0] == box({496, 160, 0}, {560, 384, 96}));
        CHECK(interiors[1] == box({496, 96, 0}, {560, 160, 96}));
        CHECK(interiors[2] == box({272, 96, 0}, {496, 160, 96}));
        CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
        CHECK(sealed());
      }

      SECTION("a corner right at the wall needs no first segment")
      {
        f.ok("undo");
        // the second opening starts right where the first wall ends
        const auto near =
          QJsonObject{{"side", "+x"}, {"bounds", box({256, 96, 0}, {272, 160, 96})}};
        const auto close =
          QJsonObject{{"side", "-y"}, {"bounds", box({272, 384, 0}, {336, 400, 96})}};
        const auto corner =
          f.ok("create_corridor", QJsonObject{{"from", near}, {"to", close}});
        CHECK(
          corner.value("interior_bounds")
          == QJsonValue{QJsonArray{
            box({272, 96, 0}, {336, 160, 96}), box({272, 160, 0}, {336, 384, 96})}});
        CHECK(f.ok("check_brush_overlaps").value("pairs") == QJsonValue{QJsonArray{}});
      }
    }

    SECTION("openings that differ slightly use their overlap")
    {
      const auto from =
        QJsonObject{{"side", "+x"}, {"bounds", box({0, 0, 0}, {16, 64, 96})}};
      const auto to =
        QJsonObject{{"side", "-x"}, {"bounds", box({128, 8, 0}, {144, 72, 80})}};
      const auto result =
        f.ok("create_corridor", QJsonObject{{"from", from}, {"to", to}});
      CHECK(
        result.value("interior_bounds")
        == QJsonValue{QJsonArray{box({16, 8, 0}, {128, 64, 80})}});
    }

    SECTION("warns about brushes in the way")
    {
      addBrush(f.map, vm::bbox3d{{64, 16, 0}, {80, 48, 32}});
      addBrush(f.map, vm::bbox3d{{64, -16, 0}, {80, 0, 32}});
      const auto from =
        QJsonObject{{"side", "+x"}, {"bounds", box({0, 0, 0}, {16, 64, 96})}};
      const auto to =
        QJsonObject{{"side", "-x"}, {"bounds", box({128, 0, 0}, {144, 64, 96})}};
      const auto warnings =
        compactJson(f.ok("create_corridor", QJsonObject{{"from", from}, {"to", to}})
                      .value("warnings"));
      CHECK(warnings.contains("the corridor brushes overlap B3"));
      CHECK(warnings.contains("the corridor interior is blocked by B2"));
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      const auto open = [](const char* side, const QJsonValue& bounds) {
        auto result = QJsonObject{{"bounds", bounds}};
        if (side)
        {
          result["side"] = side;
        }
        return result;
      };
      const auto px = box({0, 0, 0}, {16, 64, 96});
      const auto check = [&](
                           const QJsonObject& from,
                           const QJsonObject& to,
                           const char* code,
                           const char* pointer,
                           const char* message = nullptr) {
        CAPTURE(from, to);
        const auto error =
          f.error("create_corridor", QJsonObject{{"from", from}, {"to", to}});
        CHECK(error.value("code") == QJsonValue{code});
        if (pointer)
        {
          CHECK(
            error.value("details").toObject().value("pointer") == QJsonValue{pointer});
        }
        if (message)
        {
          CHECK(error.value("message").toString().contains(message));
        }
        CHECK(snapshot(f.map) == before);
      };

      // parallel walls without overlap
      check(
        open("+x", px),
        open("-x", box({128, 100, 0}, {144, 164, 96})),
        "invalid_argument",
        "/to/bounds",
        "do not overlap along y");
      // facing away
      check(
        open("-x", px),
        open("-x", box({128, 0, 0}, {144, 64, 96})),
        "invalid_argument",
        "/from/side");
      check(
        open("+x", px),
        open("+x", box({128, 0, 0}, {144, 64, 96})),
        "invalid_argument",
        "/to/side");
      // the same opening
      check(open("+x", px), open("-x", px), "invalid_argument", "/to/bounds");
      // touching walls
      check(
        open("+x", px),
        open("-x", box({16, 0, 0}, {32, 64, 96})),
        "invalid_argument",
        "/to/bounds",
        "touch");
      // no common height
      check(
        open("+x", px),
        open("-x", box({128, 0, 100}, {144, 64, 196})),
        "invalid_argument",
        "/to/bounds",
        "height");
      // much wider than the other
      check(
        open("+x", px),
        open("-x", box({128, 0, 0}, {144, 128, 96})),
        "invalid_argument",
        "/to/bounds",
        "wall_thickness");
      check(
        open("+x", px),
        open("-x", box({128, 0, 0}, {144, 64, 32})),
        "invalid_argument",
        "/from/bounds",
        "height");
      // the direction through the wall is unclear
      check(
        open(nullptr, box({0, 0, 0}, {16, 16, 96})),
        open("-x", box({128, 0, 0}, {144, 64, 96})),
        "invalid_argument",
        "/from");
      check(
        open("+x", box({0, 0, 0}, {16, 64, 0})),
        open("-x", box({128, 0, 0}, {144, 64, 96})),
        "invalid_argument",
        "/from/bounds");
      // perpendicular walls without room for the turn
      check(
        open("+x", px),
        open("-y", box({8, 128, 0}, {72, 144, 96})),
        "invalid_argument",
        "/to/bounds",
        "turn");
      check(
        open("+x", px),
        open("-y", box({64, 32, 0}, {128, 48, 96})),
        "invalid_argument",
        "/to/bounds",
        "turn");
      check(
        open("+x", px),
        open("+y", box({64, 128, 0}, {128, 144, 96})),
        "invalid_argument",
        "/to/side");
      // outside the world
      check(
        open("+x", box({30000, 0, 0}, {32000, 64, 96})),
        open("-x", box({40000, 0, 0}, {40016, 64, 96})),
        "out_of_world_bounds",
        nullptr);
      CHECK(
        f.err(
          "create_corridor",
          QJsonObject{{"from", open("+x", px)}, {"to", open("+x", px)}, {"bogus", 1}})
        == "invalid_argument");
      CHECK(
        f.err(
          "create_corridor",
          QJsonObject{
            {"from", QJsonObject{{"bounds", px}, {"side", "+z"}}},
            {"to", open("-x", box({128, 0, 0}, {144, 64, 96}))}})
        == "invalid_argument");
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
