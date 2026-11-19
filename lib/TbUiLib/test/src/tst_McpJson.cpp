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
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpJson.h"
#include "ui/McpNodeRegistry.h"

#include "vm/vec.h"

#include <cmath>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("McpJson")
{
  SECTION("canon")
  {
    CHECK(canon(1.0) == 1.0);
    CHECK(canon(1.0000004) == 1.0);
    CHECK(canon(0.1234567) == 0.123457);
    CHECK(canon(-2.5) == -2.5);
    CHECK(canon(1e20) == 1e20);

    const auto negativeZero = canon(-0.0);
    CHECK(negativeZero == 0.0);
    CHECK_FALSE(std::signbit(negativeZero));

    const auto tinyNegative = canon(-1e-9);
    CHECK(tinyNegative == 0.0);
    CHECK_FALSE(std::signbit(tinyNegative));

    // float values are printed as their short decimal representation
    CHECK(canon(double(0.1f)) == 0.1);
  }

  SECTION("toJson")
  {
    CHECK(
      compactJson(toJson(vm::vec3d{1.0, -0.0, 2.0000001})) == QStringLiteral("[1,0,2]"));
    CHECK(compactJson(toJson(vm::vec2f{0.5f, 0.1f})) == QStringLiteral("[0.5,0.1]"));
    CHECK(
      compactJson(toJson(vm::bbox3d{{-1, -2, -3}, {4, 5, 6}}))
      == QStringLiteral(R"({"max":[4,5,6],"min":[-1,-2,-3]})"));
    CHECK(
      compactJson(toJson(vm::plane3d{8.0, vm::vec3d{0, 0, 1}}))
      == QStringLiteral(R"({"distance":8,"normal":[0,0,1]})"));
  }

  SECTION("toUnicode / fromUnicode")
  {
    auto fixture = mdl::MapFixture{};
    auto& map = fixture.create(mdl::QuakeFixtureConfig);

    auto bytes = std::string{};
    for (auto c = 0x80; c <= 0x9F; ++c)
    {
      bytes.push_back(char(c));
    }
    bytes += "plain \xC5\xCE";

    const auto unicode = toUnicode(map, bytes);
    CHECK(unicode.size() == qsizetype(bytes.size()));
    CHECK(unicode[0x0E] == QChar{0x017D}); // byte 0x8E is Windows-1252 "Ž"
    CHECK(fromUnicode(map, unicode) == bytes);
  }

  SECTION("sha256Hex")
  {
    const auto digest = sha256Hex(QJsonObject{{"a", 1}});
    CHECK(digest.size() == 64);
    CHECK(digest == sha256Hex(QJsonObject{{"a", 1}}));
    CHECK(digest != sha256Hex(QJsonObject{{"a", 2}}));

    // SHA-256 of the compact text {"a":1}
    CHECK(
      digest
      == QStringLiteral(
        "015abd7f5cc57a2dd94b7590f04ad8084273905ee33ec5cebeae62276a97f862"));

    // scalars are wrapped in an array
    CHECK(sha256Hex(QJsonValue{1}) == sha256Hex(QJsonArray{1}));
  }

  SECTION("sideName")
  {
    CHECK(sideName(vm::vec3d{1, 0, 0}) == QStringLiteral("+x"));
    CHECK(sideName(vm::vec3d{-1, 0, 0}) == QStringLiteral("-x"));
    CHECK(sideName(vm::vec3d{0, 1, 0}) == QStringLiteral("+y"));
    CHECK(sideName(vm::vec3d{0, -1, 0}) == QStringLiteral("-y"));
    CHECK(sideName(vm::vec3d{0, 0, 1}) == QStringLiteral("+z"));
    CHECK(sideName(vm::vec3d{0, 0, -1}) == QStringLiteral("-z"));
    CHECK(sideName(vm::normalize(vm::vec3d{0, 1e-12, 1})) == QStringLiteral("+z"));
    CHECK(sideName(vm::vec3d{0, 0.6, 0.8}) == std::nullopt);
  }

  SECTION("sideNormal")
  {
    CHECK(sideNormal("+x") == vm::vec3d{1, 0, 0});
    CHECK(sideNormal("-z") == vm::vec3d{0, 0, -1});
    CHECK(sideNormal("x") == std::nullopt);
    CHECK(sideNormal("+w") == std::nullopt);
    CHECK(sideNormal("") == std::nullopt);
  }

  SECTION("brushVolume")
  {
    const auto worldBounds = vm::bbox3d{8192.0};
    const auto builder = mdl::BrushBuilder{mdl::MapFormat::Valve, worldBounds};

    const auto cube =
      builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64, 64, 64}}, "material").value();
    CHECK(brushVolume(cube) == 262144.0);

    const auto shifted =
      builder.createCuboid(vm::bbox3d{{-100, 20, 5}, {-36, 52, 21}}, "material").value();
    CHECK(brushVolume(shifted) == 64.0 * 32.0 * 16.0);

    const auto tetrahedron =
      builder
        .createBrush(
          std::vector<vm::vec3d>{{0, 0, 0}, {6, 0, 0}, {0, 6, 0}, {0, 0, 6}}, "material")
        .value();
    CHECK(brushVolume(tetrahedron) == 36.0);
  }

  SECTION("nodeTypeName")
  {
    auto fixture = mdl::MapFixture{};
    auto& map = fixture.create(mdl::QuakeFixtureConfig);
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});
    auto* entity = addPointEntity(map, "light", {0, 0, 0});
    auto* patch = addPatch(map);
    auto* group =
      groupNodes(map, {addBrush(map, vm::bbox3d{{32, 0, 0}, {48, 16, 16}})}, "g");

    CHECK(nodeTypeName(map.worldNode()) == QStringLiteral("world"));
    CHECK(nodeTypeName(*map.worldNode().defaultLayer()) == QStringLiteral("layer"));
    CHECK(nodeTypeName(*group) == QStringLiteral("group"));
    CHECK(nodeTypeName(*entity) == QStringLiteral("entity"));
    CHECK(nodeTypeName(*brush) == QStringLiteral("brush"));
    CHECK(nodeTypeName(*patch) == QStringLiteral("patch"));
  }

  SECTION("nodeSummary")
  {
    auto fixture = mdl::MapFixture{};
    auto& map = fixture.create(mdl::QuakeFixtureConfig);
    auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}}, "wall");
    auto* light = addPointEntity(map, "light", {8, 8, 64});
    auto* door = addBrushEntity(map, "func_door", {vm::bbox3d{{64, 0, 0}, {80, 16, 16}}});
    auto* patch = addPatch(map);
    mdl::selectNodes(map, {brush});

    const auto registry = McpNodeRegistry{map};
    auto* defaultLayer = map.worldNode().defaultLayer();
    const auto layerId = registry.idOf(*defaultLayer);

    const auto world = nodeSummary(registry, map, map.worldNode());
    CHECK(world.value("id") == QJsonValue{"world"});
    CHECK(world.value("type") == QJsonValue{"world"});
    CHECK(world.value("parent").isNull());
    CHECK(world.value("layer").isNull());
    CHECK(world.value("classname") == QJsonValue{"worldspawn"});
    CHECK(world.value("child_count") == QJsonValue{1});

    const auto layer = nodeSummary(registry, map, *defaultLayer);
    CHECK(layer.value("id") == QJsonValue{layerId});
    CHECK(layer.value("parent") == QJsonValue{"world"});
    CHECK(layer.value("layer") == QJsonValue{layerId});
    CHECK(layer.value("name") == QJsonValue{"Default Layer"});
    CHECK(layer.value("child_count") == QJsonValue{4});

    const auto brushSummary = nodeSummary(registry, map, *brush);
    CHECK(
      compactJson(brushSummary)
      == QString{R"({"bounds":{"max":[16,16,16],"min":[0,0,0]},"entity":"world",)"
                 R"("id":"%1","layer":"%2","locked":false,"materials":["wall"],)"
                 R"("parent":"%2","selected":true,"type":"brush","visible":true})"}
           .arg(registry.idOf(*brush), layerId));

    const auto lightSummary = nodeSummary(registry, map, *light);
    CHECK(lightSummary.value("classname") == QJsonValue{"light"});
    CHECK(lightSummary.value("child_count") == QJsonValue{0});
    CHECK(lightSummary.value("selected") == QJsonValue{false});

    const auto doorBrush = nodeSummary(registry, map, *door->children().front());
    CHECK(doorBrush.value("entity") == QJsonValue{registry.idOf(*door)});
    CHECK(doorBrush.value("parent") == QJsonValue{registry.idOf(*door)});
    CHECK(doorBrush.value("layer") == QJsonValue{layerId});

    const auto patchSummary = nodeSummary(registry, map, *patch);
    CHECK(patchSummary.value("material") == QJsonValue{"patch_material"});
    CHECK(patchSummary.value("entity") == QJsonValue{"world"});
  }
}

} // namespace tb::ui
