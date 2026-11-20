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
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "ui/CatchConfig.h"
#include "ui/McpFaceFilter.h"
#include "ui/McpJson.h"

#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

using Polygon = std::vector<vm::vec2d>;

Polygon square(const double x0, const double y0, const double x1, const double y1)
{
  return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

Polygon reversed(Polygon polygon)
{
  std::ranges::reverse(polygon);
  return polygon;
}

/** Compact "B2/3" refs of the faces in a find_faces result. */
QStringList refs(const QJsonObject& result)
{
  auto list = QStringList{};
  for (const auto& face : result.value("faces").toArray())
  {
    const auto object = face.toObject();
    list.append(QString{"%1/%2"}
                  .arg(object.value("brush").toString())
                  .arg(object.value("index").toInt()));
  }
  return list;
}

} // namespace

TEST_CASE("McpFaceFilter")
{
  SECTION("isCoveredByPolygons")
  {
    const auto unit = square(0, 0, 64, 64);

    CHECK(isCoveredByPolygons(unit, {unit}));
    CHECK(isCoveredByPolygons(unit, {reversed(unit)}));
    CHECK(isCoveredByPolygons(reversed(unit), {square(-16, -16, 80, 80)}));
    // two halves and four quadrants in mixed windings
    CHECK(isCoveredByPolygons(unit, {square(0, 0, 32, 64), square(32, 0, 64, 64)}));
    CHECK(isCoveredByPolygons(
      unit,
      {square(0, 0, 32, 32),
       reversed(square(32, 0, 64, 32)),
       square(0, 32, 32, 64),
       reversed(square(32, 32, 64, 64))}));
    // overlapping covers
    CHECK(isCoveredByPolygons(unit, {square(0, 0, 48, 64), square(16, 0, 64, 64)}));
    // a triangle inside a square, and a square covered by two triangles
    CHECK(isCoveredByPolygons({{0, 0}, {64, 0}, {0, 64}}, {unit}));
    CHECK(isCoveredByPolygons(
      unit, {{{0, 0}, {64, 0}, {0, 64}}, {{64, 0}, {64, 64}, {0, 64}}}));
    // floating point noise on a shared edge still covers
    CHECK(isCoveredByPolygons(
      unit, {square(0, 0, 32 + 1e-9, 64), square(32 - 1e-9, 0, 64, 64)}));

    // partial covers
    CHECK_FALSE(isCoveredByPolygons(unit, {}));
    CHECK_FALSE(isCoveredByPolygons(unit, {square(0, 0, 32, 64)}));
    CHECK_FALSE(isCoveredByPolygons(
      unit, {square(0, 0, 32, 32), square(32, 0, 64, 32), square(0, 32, 32, 64)}));
    // a gap of 1/128 between two covers
    CHECK_FALSE(isCoveredByPolygons(
      unit, {square(0, 0, 32, 64), square(32 + 1.0 / 128.0, 0, 64, 64)}));
    // a cover that only touches the edge
    CHECK_FALSE(isCoveredByPolygons(unit, {square(64, 0, 128, 64)}));
    CHECK_FALSE(isCoveredByPolygons(unit, {{{0, 0}, {64, 0}, {0, 64}}}));
    // degenerate polygons are never covered, degenerate covers are ignored
    CHECK_FALSE(isCoveredByPolygons({{0, 0}, {64, 0}, {128, 0}}, {unit}));
    CHECK_FALSE(isCoveredByPolygons({}, {unit}));
    CHECK_FALSE(isCoveredByPolygons(unit, {{{0, 0}, {64, 64}}}));
  }

  auto f = McpMapToolsFixture{};
  const auto findFaces = [&](const QString& args) {
    return refs(f.ok("find_faces", parseObject(args.toUtf8().constData())));
  };

  // B2 +x wall, B3 -x wall, B4 +y wall, B5 -y wall, B6 floor, B7 ceiling; every brush has
  // the faces 0 -x, 1 -y, 2 -z, 3 +z, 4 +y, 5 +x
  f.ok("create_room", R"({
    "inner_bounds":{"min":[0,0,0],"max":[64,64,64]},
    "material":"base","floor_material":"stone"})");

  SECTION("find_faces")
  {
    SECTION("returns face refs with normal, area, center and material")
    {
      const auto result = f.ok("find_faces", R"({"facing":"up","exposed_only":true})");
      CHECK(
        result.value("faces")
        == QJsonValue{QJsonArray{
          QJsonObject{
            {"brush", "B6"},
            {"index", 3},
            {"side", "+z"},
            {"normal", QJsonArray{0, 0, 1}},
            {"area", 9216},
            {"center", QJsonArray{32, 32, 0}},
            {"material", "stone"},
          },
          QJsonObject{
            {"brush", "B7"},
            {"index", 3},
            {"side", "+z"},
            {"normal", QJsonArray{0, 0, 1}},
            {"area", 9216},
            {"center", QJsonArray{32, 32, 80}},
            {"material", "base"},
          },
        }});
      CHECK(result.value("total") == QJsonValue{2});
      CHECK(result.value("truncated") == QJsonValue{false});
      CHECK(result.value("brushes_searched") == QJsonValue{6});
    }

    SECTION("floors, ceilings and walls of a room")
    {
      // without exposed_only, the tops of the walls (under the ceiling) count as well
      CHECK(
        findFaces(R"({"facing":"up"})")
        == QStringList{"B2/3", "B3/3", "B4/3", "B5/3", "B6/3", "B7/3"});
      CHECK(
        findFaces(R"({"facing":"down"})")
        == QStringList{"B2/2", "B3/2", "B4/2", "B5/2", "B6/2", "B7/2"});
      // the walls stand on the floor and the ceiling rests on them
      CHECK(
        findFaces(R"({"facing":"up","exposed_only":true})")
        == QStringList{"B6/3", "B7/3"});
      CHECK(
        findFaces(R"({"facing":"down","exposed_only":true})")
        == QStringList{"B6/2", "B7/2"});

      // inside the room: the floor you walk on, the visible ceiling and the 4 inner walls
      const auto inner = R"("inside":{"min":[0,0,0],"max":[64,64,64]})";
      CHECK(
        findFaces(QString{R"({"facing":"up","exposed_only":true,%1})"}.arg(inner))
        == QStringList{"B6/3"});
      CHECK(
        findFaces(QString{R"({"facing":"down","exposed_only":true,%1})"}.arg(inner))
        == QStringList{"B7/2"});
      CHECK(
        findFaces(QString{R"({"facing":"horizontal","exposed_only":true,%1})"}.arg(inner))
        == QStringList{"B2/0", "B3/5", "B4/1", "B5/4"});
    }

    SECTION("exposed_only drops wall ends that touch adjoining walls")
    {
      // the y walls end at the inner faces of the x walls, which they partly cover
      CHECK(
        findFaces(R"({"facing":"horizontal","exposed_only":true})")
        == QStringList{"B2/0", "B2/1", "B2/4", "B2/5", "B3/0", "B3/1", "B3/4",
                       "B3/5", "B4/1", "B4/4", "B5/1", "B5/4", "B6/0", "B6/1",
                       "B6/4", "B6/5", "B7/0", "B7/1", "B7/4", "B7/5"});
      CHECK(findFaces(R"({"facing":"horizontal"})").size() == 24);
      CHECK(
        findFaces(R"({"ids":["B4"],"exposed_only":true})")
        == QStringList{"B4/1", "B4/4"});

      // removing the -x wall exposes the -x ends of the y walls and the floor top part
      f.ok("delete_nodes", R"({"ids":["B3"]})");
      CHECK(
        findFaces(R"({"ids":["B4","B5"],"facing":"-x","exposed_only":true})")
        == QStringList{"B4/0", "B5/0"});
    }

    SECTION("exposed_only uses the union of several covering world brushes")
    {
      auto g = McpMapToolsFixture{};
      const auto box = [&](const char* bounds) {
        return g.ok("create_box_brush", parseObject(bounds)).value("id").toString();
      };
      const auto exposedUp = [&](const QString& id) {
        return refs(g.ok(
          "find_faces",
          QJsonObject{
            {"ids", QJsonArray{id}}, {"facing", "up"}, {"exposed_only", true}}));
      };
      const auto slab = box(R"({"bounds":{"min":[0,0,-16],"max":[128,64,0]}})");
      const auto left = box(R"({"bounds":{"min":[0,0,0],"max":[64,64,64]}})");
      CHECK(exposedUp(slab) == QStringList{slab + "/3"});

      // two boxes side by side cover the whole top of the slab
      const auto right = box(R"({"bounds":{"min":[64,0,0],"max":[128,64,64]}})");
      CHECK(exposedUp(slab).isEmpty());
      CHECK(refs(g.ok(
                   "find_faces",
                   QJsonObject{
                     {"ids", QJsonArray{left, right}},
                     {"facing", "down"},
                     {"exposed_only", true}}))
              .isEmpty());

      // a gap of 1/8 unit exposes the slab again
      g.ok(
        "resize_nodes",
        QJsonObject{
          {"ids", QJsonArray{right}},
          {"bounds", parseObject(R"({"min":[64.125,0,0],"max":[128,64,64]})")}});
      CHECK(exposedUp(slab) == QStringList{slab + "/3"});
      g.ok("undo");
      CHECK(exposedUp(slab).isEmpty());

      // a brush entity does not hide world faces, a grouped world brush does
      g.ok("delete_nodes", QJsonObject{{"ids", QJsonArray{right}}});
      const auto detail = box(R"({"bounds":{"min":[64,0,0],"max":[128,64,8]}})");
      g.ok(
        "create_entity",
        QJsonObject{{"classname", "func_detail"}, {"brush_ids", QJsonArray{detail}}});
      CHECK(exposedUp(slab) == QStringList{slab + "/3"});
      // the brush entity's own faces are still tested against world brushes
      CHECK(
        refs(
          g.ok(
            "find_faces",
            QJsonObject{
              {"ids", QJsonArray{detail}}, {"facing", "down"}, {"exposed_only", true}}))
          .isEmpty());
      g.ok("undo");
      g.ok("group_nodes", QJsonObject{{"ids", QJsonArray{detail}}, {"name", "g"}});
      CHECK(exposedUp(slab).isEmpty());
    }

    SECTION("facing by axis, vector and tolerance")
    {
      CHECK(findFaces(R"({"ids":["B6"],"facing":"+x"})") == QStringList{"B6/5"});
      CHECK(findFaces(R"({"ids":["B6"],"facing":[2,0,0]})") == QStringList{"B6/5"});
      CHECK(findFaces(R"({"ids":["B6"],"facing":"-z"})") == QStringList{"B6/2"});
      CHECK(findFaces(R"({"ids":["B6"],"facing":"+z"})") == QStringList{"B6/3"});
      CHECK(findFaces(R"({"ids":["B6"],"facing":[1,1,0]})").isEmpty());
      CHECK(
        findFaces(R"({"ids":["B6"],"facing":[1,1,0],"tolerance_degrees":45})")
        == QStringList{"B6/4", "B6/5"});
      CHECK(
        findFaces(R"({"ids":["B6"],"facing":"up","tolerance_degrees":90})")
        == QStringList{"B6/0", "B6/1", "B6/3", "B6/4", "B6/5"});

      // a ramp whose slope is 26.57 degrees from the horizontal plane
      const auto ramp = f.ok("create_brush_from_points", R"({"points":[
        [256,0,0],[320,0,0],[256,64,0],[320,64,0],[320,0,32],[320,64,32]]})")
                          .value("id")
                          .toString();
      const auto rampFaces = [&](const char* filter) {
        auto args = parseObject(filter);
        args["ids"] = QJsonArray{ramp};
        return f.ok("find_faces", args).value("faces").toArray();
      };
      CHECK(rampFaces(R"({"facing":"up"})").isEmpty());
      const auto slope = rampFaces(R"({"facing":"up","tolerance_degrees":30})");
      REQUIRE(slope.size() == 1);
      CHECK_FALSE(slope[0].toObject().contains("side"));
      CHECK(
        slope[0].toObject().value("normal")
        == QJsonValue{QJsonArray{-0.447214, 0, 0.894427}});
      CHECK(
        slope[0].toObject().value("area")
        == QJsonValue{canon(64.0 * std::sqrt(64.0 * 64.0 + 32.0 * 32.0))});
      // three vertical sides; the slope is 63.43 degrees from the horizontal plane
      CHECK(rampFaces(R"({"facing":"horizontal","tolerance_degrees":60})").size() == 3);
      CHECK(rampFaces(R"({"facing":"horizontal","tolerance_degrees":64})").size() == 4);
    }

    SECTION("material and inside")
    {
      CHECK(
        findFaces(R"({"material":"ST*"})")
        == QStringList{"B6/0", "B6/1", "B6/2", "B6/3", "B6/4", "B6/5"});
      CHECK(findFaces(R"({"material":"st?ne","facing":"up"})") == QStringList{"B6/3"});
      CHECK(findFaces(R"({"material":"nothing"})").isEmpty());
      // the outer -x faces of the -x wall, the floor and the ceiling lie on x = -16
      CHECK(
        findFaces(R"({"inside":{"min":[-16,-16,-16],"max":[-16,80,80]}})")
        == QStringList{"B3/0", "B6/0", "B7/0"});
    }

    SECTION("ids")
    {
      auto* door =
        addBrushEntity(f.map, "func_door", {vm::bbox3d{{128, 0, 0}, {144, 16, 64}}});
      auto* light = addPointEntity(f.map, "light", {32, 32, 32});
      auto* patch = addPatch(f.map);
      const auto doorBrush = f.id(*door->children().front());

      // by default only world brushes
      CHECK(f.ok("find_faces").value("total") == QJsonValue{36});
      CHECK(f.ok("find_faces").value("brushes_searched") == QJsonValue{6});
      CHECK(
        findFaces(QString{R"({"ids":["%1"],"facing":"up"})"}.arg(f.id(*door)))
        == QStringList{doorBrush + "/3"});
      CHECK(f.ok("find_faces", R"({"ids":["world"]})").value("total") == QJsonValue{42});
      CHECK(
        f.ok("find_faces", R"({"ids":["default_layer","B2"]})").value("total")
        == QJsonValue{42});

      CHECK(
        f.err("find_faces", QJsonObject{{"ids", QJsonArray{f.id(*light)}}})
        == "wrong_node_type");
      CHECK(
        f.err("find_faces", QJsonObject{{"ids", QJsonArray{f.id(*patch)}}})
        == "wrong_node_type");
      CHECK(f.err("find_faces", R"({"ids":["B99"]})") == "unknown_node");
    }

    SECTION("groups are expanded")
    {
      auto g = McpMapToolsFixture{};
      g.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"group_name":"room"})");
      const auto up = R"({"facing":"up","exposed_only":true})";
      CHECK(refs(g.ok("find_faces", up)) == QStringList{"B6/3", "B7/3"});
      CHECK(
        refs(g.ok("find_faces", R"({"ids":["G8"],"facing":"up","exposed_only":true})"))
        == QStringList{"B6/3", "B7/3"});
    }

    SECTION("paging")
    {
      const auto result = f.ok("find_faces", R"({"facing":"up","offset":1,"limit":2})");
      CHECK(refs(result) == QStringList{"B3/3", "B4/3"});
      CHECK(result.value("total") == QJsonValue{6});
      CHECK(result.value("truncated") == QJsonValue{true});
      CHECK(result.value("next_offset") == QJsonValue{3});
    }

    SECTION("errors")
    {
      const auto zero = f.error("find_faces", R"({"facing":[0,0,0]})");
      CHECK(zero.value("code") == QJsonValue{"invalid_argument"});
      CHECK(zero.value("details").toObject().value("pointer") == QJsonValue{"/facing"});
      CHECK(f.err("find_faces", R"({"facing":"sideways"})") == "invalid_argument");
      CHECK(f.err("find_faces", R"({"facing":[1e300,0,0]})") == "invalid_argument");
      CHECK(f.err("find_faces", R"({"tolerance_degrees":91})") == "invalid_argument");
      CHECK(f.err("find_faces", R"({"tolerance_degrees":-1})") == "invalid_argument");
      CHECK(
        f.err("find_faces", R"({"inside":{"min":[1,0,0],"max":[0,1,1]}})")
        == "invalid_argument");
      CHECK(f.err("find_faces", R"({"material":""})") == "invalid_argument");
    }

    SECTION("results are reused as face refs by brush and index")
    {
      const auto found = f.ok(
                            "find_faces",
                            R"({"facing":"up","exposed_only":true,
                                 "inside":{"min":[0,0,0],"max":[64,64,64]}})")
                           .value("faces")
                           .toArray();
      REQUIRE(found.size() == 1);
      // a whole result has index, side and normal, but a face ref takes exactly one
      CHECK(
        f.err("set_face_attributes", QJsonObject{{"faces", found}, {"material", "x"}})
        == "invalid_argument");
      const auto ref = QJsonObject{
        {"brush", found[0].toObject().value("brush")},
        {"index", found[0].toObject().value("index")}};
      f.ok(
        "set_face_attributes",
        QJsonObject{{"faces", QJsonArray{ref}}, {"material", "floor_01"}});
      CHECK(findFaces(R"({"material":"floor_01"})") == QStringList{"B6/3"});

      const auto* tool = f.dispatcher.find(u"find_faces");
      REQUIRE(tool != nullptr);
      CHECK(tool->description.contains("pass {brush, index}"));
    }

    SECTION("door jambs lie outside the inner bounds")
    {
      auto g = McpMapToolsFixture{};
      const auto room = g.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[128,128,128]},
        "openings":[{"side":"+x","width":32,"height":64}]})");
      const auto doorWall = room.value("walls").toObject().value("+x").toArray();
      REQUIRE(doorWall.size() == 3);
      const auto jambs = [&](const QJsonObject& args) {
        auto result = QStringList{};
        for (const auto& face : g.ok("find_faces", args).value("faces").toArray())
        {
          const auto center = face.toObject().value("center").toArray();
          // in the wall and at the door (y 48..80), not at the outer wall ends
          if (
            center[0].toDouble() > 128.0 && center[0].toDouble() < 144.0
            && center[1].toDouble() >= 48.0 && center[1].toDouble() <= 80.0)
          {
            result.append(face.toObject().value("side").toString());
          }
        }
        return result;
      };
      const auto inner = parseObject(R"({"min":[0,0,0],"max":[128,128,128]})");
      CHECK(jambs(QJsonObject{
                    {"facing", "horizontal"}, {"exposed_only", true}, {"inside", inner}})
              .isEmpty());
      // the jambs and the lintel are exposed faces of the door wall pieces
      CHECK(
        jambs(QJsonObject{
          {"ids", doorWall}, {"facing", "horizontal"}, {"exposed_only", true}})
        == QStringList{"+y", "-y"});
      CHECK(
        jambs(QJsonObject{{"ids", doorWall}, {"facing", "down"}, {"exposed_only", true}})
        == QStringList{"-z"});
    }

    SECTION("does not change the map")
    {
      const auto before = snapshot(f.map);
      f.ok("find_faces", R"({"facing":"horizontal","exposed_only":true})");
      CHECK(snapshot(f.map) == before);
    }
  }

  SECTION("set_face_attributes with face_filter")
  {
    const auto all = R"(["B2","B3","B4","B5","B6","B7"])";

    SECTION("retextures only the exposed floor surface in one call")
    {
      f.map.setCurrentMaterialName("current");
      f.ok("select_nodes", R"({"ids":["B5"]})");
      const auto before = snapshot(f.map);

      const auto result = f.ok(
        "set_face_attributes",
        QString{
          R"({"brushes":%1,"face_filter":{"facing":"up","exposed_only":true,
               "inside":{"min":[0,0,0],"max":[64,64,64]}},"material":"floor_01"})"}
          .arg(all)
          .toUtf8()
          .constData());
      CHECK(result.value("faces_changed") == QJsonValue{1});
      CHECK(result.value("undo_label") == QJsonValue{"MCP: Set Face Attributes"});
      CHECK(
        result.value("changes").toObject().value("modified")
        == QJsonValue{QJsonArray{"B6"}});
      CHECK(findFaces(R"({"material":"floor_01"})") == QStringList{"B6/3"});
      CHECK(f.map.currentMaterialName() == "current");
      CHECK(f.map.selection().nodes == before.selectedNodes);
      CHECK(f.map.selection().brushFaces.empty());

      // the same call again changes nothing and adds no undo step
      const auto again = f.ok(
        "set_face_attributes",
        QString{R"({"brushes":%1,"face_filter":{"facing":"up","exposed_only":true,
               "inside":{"min":[0,0,0],"max":[64,64,64]}},"material":"floor_01"})"}
          .arg(all)
          .toUtf8()
          .constData());
      CHECK(again.value("undo_label") == QJsonValue{});

      f.ok("undo");
      CHECK(findFaces(R"({"material":"floor_01"})").isEmpty());
      CHECK(snapshot(f.map).text == before.text);
    }

    SECTION("walls and ceilings")
    {
      f.ok(
        "set_face_attributes",
        R"({"face_filter":{"facing":"horizontal","exposed_only":true,
        "inside":{"min":[0,0,0],"max":[64,64,64]}},"material":"brick"})");
      f.ok("set_face_attributes", R"({"face_filter":{"facing":"down","exposed_only":true,
        "inside":{"min":[0,0,0],"max":[64,64,64]}},"material":"sky"})");
      CHECK(
        findFaces(R"({"material":"brick"})")
        == QStringList{"B2/0", "B3/5", "B4/1", "B5/4"});
      CHECK(findFaces(R"({"material":"sky"})") == QStringList{"B7/2"});
      // the hidden faces kept their material
      CHECK(findFaces(R"({"material":"base"})").size() == 36 - 6 - 5);
    }

    SECTION("brushes default to all world brushes, including grouped ones")
    {
      auto g = McpMapToolsFixture{};
      g.ok("create_room", R"({
        "inner_bounds":{"min":[0,0,0],"max":[64,64,64]},"group_name":"room"})");
      addBrushEntity(g.map, "func_door", {vm::bbox3d{{128, 0, 0}, {144, 16, 64}}});
      const auto result = g.ok(
        "set_face_attributes", R"({"face_filter":{"facing":"up"},"material":"top"})");
      CHECK(result.value("faces_changed") == QJsonValue{6});
      CHECK(
        refs(g.ok("find_faces", R"({"ids":["world"],"material":"top"})"))
        == QStringList{"B2/3", "B3/3", "B4/3", "B5/3", "B6/3", "B7/3"});
      // explicit brushes still work with a group id
      CHECK(
        g.ok(
           "set_face_attributes",
           R"({"brushes":["G8"],"face_filter":{"facing":"down"},"material":"bottom"})")
          .value("faces_changed")
        == QJsonValue{6});
    }

    SECTION("a filter that matches nothing changes nothing")
    {
      const auto before = snapshot(f.map);
      const auto result = f.ok(
        "set_face_attributes",
        R"({"brushes":["B2"],"face_filter":{"material":"nothing*"},"material":"x"})");
      CHECK(result.value("faces_changed") == QJsonValue{0});
      CHECK(result.value("undo_label") == QJsonValue{});
      CHECK(result.value("warnings").toArray().size() >= 1);
      CHECK(snapshot(f.map) == before);
    }

    SECTION("errors")
    {
      const auto before = snapshot(f.map);
      CHECK(
        f.err(
          "set_face_attributes",
          R"({"faces":[{"brush":"B2","index":0}],"face_filter":{"facing":"up"},"material":"x"})")
        == "invalid_argument");
      const auto zero = f.error(
        "set_face_attributes",
        R"({"brushes":["B2"],"face_filter":{"facing":[0,0,0]},"material":"x"})");
      CHECK(
        zero.value("details").toObject().value("pointer")
        == QJsonValue{"/face_filter/facing"});
      CHECK(
        f.err(
          "set_face_attributes",
          R"({"brushes":["B2"],"face_filter":{"inside":{"min":[1,0,0],"max":[0,0,0]}},"material":"x"})")
        == "invalid_argument");
      CHECK(
        f.err(
          "set_face_attributes",
          R"({"face_filter":{"facing":"up","bogus":1},"material":"x"})")
        == "invalid_argument");
      CHECK(
        f.err("set_face_attributes", R"({"face_filter":{"facing":"up"}})")
        == "invalid_argument");
      CHECK(snapshot(f.map) == before);
    }
  }
}

} // namespace tb::ui
