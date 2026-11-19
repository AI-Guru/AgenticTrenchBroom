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
}

} // namespace tb::ui
