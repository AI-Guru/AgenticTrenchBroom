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

#include "McpTestUtils.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpNodeRegistry.h"
#include "ui/McpSelectionGuard.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("McpSelectionGuard")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create(mdl::QuakeFixtureConfig);
  const auto registry = McpNodeRegistry{map};

  auto* brush1 = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}}, "one");
  auto* brush2 = addBrush(map, vm::bbox3d{{32, 0, 0}, {48, 16, 16}}, "two");
  auto* brush3 = addBrush(map, vm::bbox3d{{64, 0, 0}, {80, 16, 16}}, "three");

  SECTION("restore")
  {
    SECTION("restores the saved nodes")
    {
      mdl::selectNodes(map, {brush1, brush2});
      auto guard = McpSelectionGuard{map, registry};

      mdl::deselectAll(map);
      mdl::selectNodes(map, {brush3});

      guard.restore();
      CHECK(guard.done());
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brush1, brush2});
    }

    SECTION("restores an empty selection")
    {
      auto guard = McpSelectionGuard{map, registry};
      mdl::selectNodes(map, {brush3});

      guard.restore();
      CHECK_FALSE(map.selection().hasAny());
    }

    SECTION("skips deleted nodes")
    {
      // mdl::PushSelection would reselect the deleted node and violate a contract
      mdl::selectNodes(map, {brush1, brush2});
      auto guard = McpSelectionGuard{map, registry};

      mdl::deselectAll(map);
      mdl::removeNodes(map, {brush1});

      guard.restore();
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brush2});
    }

    SECTION("skips nodes that are no longer selectable")
    {
      mdl::selectNodes(map, {brush1, brush2});
      auto guard = McpSelectionGuard{map, registry};

      mdl::deselectAll(map);
      mdl::lockNodes(map, {brush2});

      guard.restore();
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brush1});
    }

    SECTION("restores faces and the current material")
    {
      mdl::selectBrushFaces(map, {{*brush1, 0}, {*brush2, 1}});
      map.setCurrentMaterialName("current");
      auto guard = McpSelectionGuard{map, registry};

      mdl::deselectAll(map);
      mdl::selectBrushFaces(map, {{*brush3, 2}});
      REQUIRE(map.currentMaterialName() == "three");

      guard.restore();
      CHECK(
        map.selection().brushFaces
        == std::vector<mdl::BrushFaceHandle>{{*brush1, 0}, {*brush2, 1}});
      CHECK(map.currentMaterialName() == "current");
    }

    SECTION("skips faces whose index is out of range")
    {
      mdl::selectBrushFaces(map, {{*brush1, 5}, {*brush2, 5}});
      auto guard = McpSelectionGuard{map, registry};
      mdl::deselectAll(map);

      // replace brush1 with a tetrahedron, which has only 4 faces
      const auto builder =
        mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
      auto tetrahedron =
        builder
          .createBrush(
            std::vector<vm::vec3d>{{0, 0, 0}, {16, 0, 0}, {0, 16, 0}, {0, 0, 16}}, "t")
          .value();
      REQUIRE(mdl::updateNodeContents(
        map, "Replace", {{brush1, mdl::NodeContents{std::move(tetrahedron)}}}));
      REQUIRE(brush1->brush().faceCount() == 4);

      guard.restore();
      CHECK(
        map.selection().brushFaces == std::vector<mdl::BrushFaceHandle>{{*brush2, 5}});
    }

    SECTION("skips faces of deleted brushes")
    {
      mdl::selectBrushFaces(map, {{*brush1, 0}, {*brush2, 0}});
      auto guard = McpSelectionGuard{map, registry};
      mdl::deselectAll(map);
      mdl::removeNodes(map, {brush2});

      guard.restore();
      CHECK(
        map.selection().brushFaces == std::vector<mdl::BrushFaceHandle>{{*brush1, 0}});
    }
  }

  SECTION("dismiss")
  {
    mdl::selectNodes(map, {brush1});
    auto guard = McpSelectionGuard{map, registry};
    mdl::deselectAll(map);
    mdl::selectNodes(map, {brush3});

    CHECK_FALSE(guard.done());
    guard.dismiss();
    CHECK(guard.done());
    CHECK(map.selection().nodes == std::vector<mdl::Node*>{brush3});
  }
}

} // namespace tb::ui
