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

#include <QJsonObject>

#include "McpTestUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "ui/CatchConfig.h"

#include "kd/overload.h"

#include <functional>
#include <variant>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

struct Call
{
  const char* tool;
  const char* args;
};

/** A direct edit, as the user would make it, between tool calls. */
using Edit = std::function<void(McpMapToolsFixture&, const McpTestScene&)>;

using Step = std::variant<Call, Edit>;

std::vector<QString> runScript(const std::vector<Step>& script)
{
  auto f = McpMapToolsFixture{};
  const auto scene = populateTestScene(f);

  auto results = std::vector<QString>{};
  for (const auto& step : script)
  {
    std::visit(
      kdl::overload(
        [&](const Call& call) {
          results.push_back(compactJson(f.call(call.tool, call.args)));
        },
        [&](const Edit& edit) { edit(f, scene); }),
      step);
  }
  return results;
}

} // namespace

TEST_CASE("McpDeterminism")
{
  const auto script = std::vector<Step>{
    Call{"get_map_info", "{}"},
    Call{"get_scene_tree", "{}"},
    Call{"get_scene_tree", R"({"include_brushes":true,"limit":5})"},
    Call{"get_scene_tree", R"({"include_brushes":true,"offset":5})"},
    Call{"find_nodes", "{}"},
    Call{"find_nodes", R"({"types":["brush"],"material":"*"})"},
    Call{"find_nodes", R"({"text":"*"})"},
    Call{"get_nodes", R"({"ids":["world","L1","B2","E3","E4","G8","P9","L10","E11"]})"},
    Call{"get_brush_geometry", R"({"ids":["B2","B5","B6","B7"]})"},
    Call{"checkpoint_create", R"({"name":"start"})"},
    Call{"validate_map", "{}"},
    Call{"get_map_stats", "{}"},
    // the full text contains random linked group ids
    Call{"export_map_text", R"({"strip_tb_properties":true})"},
    Call{"export_map_text", R"({"ids":["B2","E3"]})"},
    Edit{[](McpMapToolsFixture& f, const McpTestScene& scene) {
      mdl::selectNodes(f.map, {scene.wall, scene.light});
    }},
    Call{"get_selection", "{}"},
    Call{"get_map_info", "{}"},
    Call{"get_nodes", R"({"ids":["@selection"]})"},
    Edit{[](McpMapToolsFixture& f, const McpTestScene&) {
      mdl::rotateSelection(f.map, {32, 32, 32}, {0, 0, 1}, vm::to_radians(30.0));
    }},
    Call{"get_brush_geometry", R"({"ids":["@selection"]})"},
    Call{"get_brush_geometry", R"({"ids":["B2"],"include_faces":false})"},
    Edit{[](McpMapToolsFixture& f, const McpTestScene& scene) {
      mdl::deselectAll(f.map);
      mdl::removeNodes(f.map, {scene.door});
    }},
    Call{"get_nodes", R"({"ids":["E4"]})"},
    Call{"get_nodes", R"({"ids":["B5"]})"},
    Call{"find_nodes", R"({"classname":"*"})"},
    Edit{[](McpMapToolsFixture& f, const McpTestScene&) { f.map.undoCommand(); }},
    Call{"find_nodes", R"({"classname":"func_door"})"},
    Call{"get_scene_tree", R"({"types":["entity"]})"},
    Call{"checkpoint_diff", R"({"from":"start"})"},
    Call{"checkpoint_list", "{}"},
    Call{"get_map_info", "{}"},
    Edit{[](McpMapToolsFixture& f, const McpTestScene&) {
      addBrush(f.map, vm::bbox3d{{256, 0, 0}, {320, 64, 64}}, "new");
    }},
    Call{"find_nodes", R"({"material":"new"})"},
    Call{"get_brush_geometry", R"({"ids":["B15"]})"},
    Edit{[](McpMapToolsFixture& f, const McpTestScene& scene) {
      mdl::selectBrushFaces(f.map, {{*scene.floor, 2}});
    }},
    Call{"get_selection", "{}"},
    Call{"find_nodes", R"({"intersects":{"min":[0,0,0],"max":[300,300,300]}})"},
    Call{"get_scene_tree", R"({"root":"L10","max_depth":0})"},
    Call{"get_nodes", R"({"ids":["B999"]})"},
    Call{"get_brush_geometry", R"({"ids":["E3"]})"},
    Call{"find_nodes", R"({"inside":{"min":[1,0,0],"max":[0,1,1]}})"},
    Call{"get_map_info", "{}"},
    Call{"get_scene_tree", R"({"include_brushes":true})"},
    // mutating and history tools
    Call{"create_box_brush", R"({"bounds":{"min":[0,0,256],"max":[64,64,320]}})"},
    Call{
      "create_entity",
      R"({"classname":"light","origin":[8,8,8],"properties":{"light":1}})"},
    Call{"create_entity", R"({"classname":"func_wall","brush_ids":["B2"]})"},
    Call{"paste_map_text", R"({"text":"{\n\"classname\" \"info_null\"\n}\n"})"},
    Call{
      "transform_nodes",
      R"({"ids":["E3","B7"],"rotate":{"axis":"z","angle_degrees":33}})"},
    Call{"transform_nodes", R"({"ids":["G8"],"scale":{"factors":[1.5,1,1]}})"},
    Call{"set_entity_properties", R"({"ids":["E3"],"set":{"light":0.5}})"},
    Call{"set_face_attributes", R"({"brushes":["B2"],"rotation":17,"fit":true})"},
    Call{"select_nodes", R"({"ids":["E3"]})"},
    Call{"delete_nodes", R"({"ids":["@selection","P9"]})"},
    Call{"undo", R"({"count":3})"},
    Call{"redo", "{}"},
    Call{"undo", R"({"count":50})"},
    Call{"get_scene_tree", R"({"include_brushes":true})"},
    // building helpers, CSG, structure and batch tools
    Call{
      "create_hollow_box",
      R"({"inner_bounds":{"min":[256,0,0],"max":[512,256,128]},"wall_thickness":16,
          "group_name":"room"})"},
    Call{
      "create_primitive_brush",
      R"({"shape":"hollow_cylinder","bounds":{"min":[0,256,0],"max":[64,320,64]},
          "thickness":8})"},
    Call{
      "create_brush_from_points",
      R"({"points":[[0,0,512],[64,0,512],[0,64,512],[0,0,576]]})"},
    Call{"query_point", R"({"points":[[32,32,32],[384,128,64],[264,8,8]]})"},
    Call{"check_brush_overlaps", "{}"},
    Call{"resize_nodes", R"({"ids":["B2"],"bounds":{"min":[0,0,0],"max":[32,64,64]}})"},
    Call{"duplicate_nodes", R"({"ids":["E3","B2"],"offset":[0,0,64],"count":2})"},
    Call{"replace_material", R"({"from":"wall","to":"stone"})"},
    Call{"csg", R"({"op":"hollow","ids":["B2"]})"},
    Call{"create_box_brush", R"({"bounds":{"min":[248,96,0],"max":[264,160,96]}})"},
    Call{"csg_subtract", R"({"subtract_ids":["@selection"]})"},
    Call{"group_nodes", R"({"ids":["E3"],"name":"lights"})"},
    Call{"create_layer", R"({"name":"extra","make_current":true})"},
    Call{"move_to_layer", R"({"ids":["E4"],"layer":"L10"})"},
    Call{"list_materials", "{}"},
    Call{
      "batch",
      R"({"operations":[{"tool":"create_box_brush","arguments":{"bounds":{"min":[0,0,700],"max":[8,8,708]}}},
                        {"tool":"snap_vertices","arguments":{"ids":["$0"]}},
                        {"tool":"delete_nodes","arguments":{"ids":["B999"]}}]})"},
    Call{"get_scene_tree", R"({"include_brushes":true})"},
    Call{"get_log", "{}"},
  };

  const auto callCount = std::ranges::count_if(
    script, [](const auto& step) { return std::holds_alternative<Call>(step); });
  REQUIRE(callCount >= 30);

  const auto first = runScript(script);
  const auto second = runScript(script);
  REQUIRE(first.size() == second.size());
  for (size_t i = 0; i < first.size(); ++i)
  {
    CAPTURE(i);
    CHECK(first[i] == second[i]);
  }

  // the script must exercise errors as well as successes
  CHECK(std::ranges::any_of(
    first, [](const auto& r) { return r.contains("\"isError\":true"); }));
  CHECK(std::ranges::any_of(
    first, [](const auto& r) { return r.contains("\"isError\":false"); }));
}

} // namespace tb::ui
