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

#include <QJsonDocument>

#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeWriter.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"

#include "kd/result.h"

#include <sstream>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

QJsonObject parseObject(const char* text)
{
  auto error = QJsonParseError{};
  const auto document = QJsonDocument::fromJson(QByteArray{text}, &error);
  INFO(text);
  REQUIRE(error.error == QJsonParseError::NoError);
  REQUIRE(document.isObject());
  return document.object();
}

QString compactJson(const QJsonValue& value)
{
  const auto document = value.isObject()  ? QJsonDocument{value.toObject()}
                        : value.isArray() ? QJsonDocument{value.toArray()}
                                          : QJsonDocument{QJsonArray{value}};
  return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}

std::string mapText(mdl::Map& map)
{
  auto stream = std::stringstream{};
  auto writer = mdl::NodeWriter{map.worldNode(), stream};
  writer.writeMap(map.taskManager());
  return stream.str();
}

McpMapSnapshot snapshot(mdl::Map& map)
{
  const auto optionalString = [](const std::string* str) {
    return str ? std::optional{*str} : std::nullopt;
  };

  return {
    .text = mapText(map),
    .modificationCount = map.modificationCount(),
    .selectedNodes = map.selection().nodes,
    .selectedFaces = map.selection().brushFaces,
    .undoLabel = optionalString(map.undoCommandName()),
    .redoLabel = optionalString(map.redoCommandName()),
  };
}

mdl::BrushNode* addBrush(
  mdl::Map& map, const vm::bbox3d& bounds, const std::string& material, mdl::Node* parent)
{
  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
    map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};
  auto brush = builder.createCuboid(bounds, material);
  REQUIRE(brush.is_success());

  auto* node = mdl::addNode(
    map,
    parent ? *parent : mdl::parentForNodes(map),
    std::make_unique<mdl::BrushNode>(std::move(brush).value()));
  REQUIRE(node != nullptr);
  return node;
}

mdl::EntityNode* addPointEntity(
  mdl::Map& map, const std::string& classname, const vm::vec3d& origin, mdl::Node* parent)
{
  auto entity = mdl::Entity{};
  entity.setClassname(classname);
  entity.setOrigin(origin);

  auto* node = mdl::addNode(
    map,
    parent ? *parent : mdl::parentForNodes(map),
    std::make_unique<mdl::EntityNode>(std::move(entity)));
  REQUIRE(node != nullptr);
  return node;
}

mdl::EntityNode* addBrushEntity(
  mdl::Map& map,
  const std::string& classname,
  const std::vector<vm::bbox3d>& brushBounds,
  mdl::Node* parent)
{
  auto entity = mdl::Entity{};
  entity.setClassname(classname);

  auto* entityNode = mdl::addNode(
    map,
    parent ? *parent : mdl::parentForNodes(map),
    std::make_unique<mdl::EntityNode>(std::move(entity)));
  REQUIRE(entityNode != nullptr);

  for (const auto& bounds : brushBounds)
  {
    addBrush(map, bounds, "material", entityNode);
  }
  return entityNode;
}

mdl::PatchNode* addPatch(mdl::Map& map, mdl::Node* parent)
{
  // clang-format off
  auto patch = mdl::BezierPatch{3, 3, {
    {0, 0, 0}, {1, 0, 1}, {2, 0, 0},
    {0, 1, 1}, {1, 1, 2}, {2, 1, 1},
    {0, 2, 0}, {1, 2, 1}, {2, 2, 0} }, "patch_material"};
  // clang-format on

  auto* node = mdl::addNode(
    map,
    parent ? *parent : mdl::parentForNodes(map),
    std::make_unique<mdl::PatchNode>(std::move(patch)));
  REQUIRE(node != nullptr);
  return node;
}

mdl::LayerNode* addLayer(mdl::Map& map, const std::string& name)
{
  auto layer = mdl::Layer{name};
  const auto customLayers = map.worldNode().customLayersUserSorted();
  layer.setSortIndex(
    customLayers.empty() ? 0 : customLayers.back()->layer().sortIndex() + 1);

  auto* node = mdl::addNode(
    map, map.worldNode(), std::make_unique<mdl::LayerNode>(std::move(layer)));
  REQUIRE(node != nullptr);
  return node;
}

mdl::GroupNode* groupNodes(
  mdl::Map& map, const std::vector<mdl::Node*>& nodes, const std::string& name)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, nodes);
  auto* group = mdl::groupSelectedNodes(map, name);
  REQUIRE(group != nullptr);
  return group;
}

McpMapToolsFixture::McpMapToolsFixture(mdl::MapFixtureConfig config)
  : map{fixture.create(std::move(config))}
  , doc{std::make_unique<McpDocumentState>(map, "d1")}
  , dispatcher{registry, [&](const McpToolKind kind) -> McpResult<McpDocumentState*> {
                 providerCalls.push_back(kind);
                 if (busy)
                 {
                   return *busy;
                 }
                 if (!doc)
                 {
                   return toolError(
                     "no_document",
                     "No map is open. Call new_map or open_map.",
                     "call new_map or open_map");
                 }
                 return doc.get();
               }}
{
  registerMcpMapTools(dispatcher);
}

QJsonObject McpMapToolsFixture::call(const QString& tool, const QJsonObject& args)
{
  return dispatcher.call(tool, args);
}

QJsonObject McpMapToolsFixture::call(const QString& tool, const char* jsonArgs)
{
  return call(tool, parseObject(jsonArgs));
}

QJsonObject McpMapToolsFixture::ok(const QString& tool, const QJsonObject& args)
{
  const auto result = call(tool, args);
  INFO(tool.toStdString() << " " << compactJson(args).toStdString());
  INFO(compactJson(result).toStdString());
  REQUIRE_FALSE(result.value("isError").toBool());
  return result.value("structuredContent").toObject();
}

QJsonObject McpMapToolsFixture::ok(const QString& tool, const char* jsonArgs)
{
  return ok(tool, parseObject(jsonArgs));
}

QJsonObject McpMapToolsFixture::error(const QString& tool, const QJsonObject& args)
{
  const auto result = call(tool, args);
  INFO(tool.toStdString() << " " << compactJson(args).toStdString());
  INFO(compactJson(result).toStdString());
  REQUIRE(result.value("isError").toBool());
  const auto error =
    result.value("structuredContent").toObject().value("error").toObject();
  CHECK(isMcpToolErrorCode(error.value("code").toString()));
  return error;
}

QJsonObject McpMapToolsFixture::error(const QString& tool, const char* jsonArgs)
{
  return error(tool, parseObject(jsonArgs));
}

QString McpMapToolsFixture::err(const QString& tool, const QJsonObject& args)
{
  return error(tool, args).value("code").toString();
}

QString McpMapToolsFixture::err(const QString& tool, const char* jsonArgs)
{
  return err(tool, parseObject(jsonArgs));
}

QString McpMapToolsFixture::id(const mdl::Node& node) const
{
  return doc->nodes.idOf(node);
}

McpTestScene populateTestScene(McpMapToolsFixture& f)
{
  auto& map = f.map;
  auto scene = McpTestScene{};
  scene.wall = addBrush(map, vm::bbox3d{{0, 0, 0}, {64, 64, 64}}, "wall");
  scene.light = addPointEntity(map, "light", {32, 32, 128});
  mdl::selectNodes(map, {scene.light});
  REQUIRE(mdl::setEntityProperty(map, "light", "300"));
  REQUIRE(mdl::setEntityProperty(map, "_color", "1 1 1"));
  mdl::deselectAll(map);
  scene.door = addBrushEntity(
    map,
    "func_door",
    {vm::bbox3d{{128, 0, 0}, {144, 16, 64}}, vm::bbox3d{{160, 0, 0}, {176, 16, 64}}});
  scene.floor = addBrush(map, vm::bbox3d{{0, 128, 0}, {64, 192, 16}}, "floor");
  scene.group = groupNodes(map, {scene.floor}, "room");
  mdl::deselectAll(map);
  scene.patch = addPatch(map);
  scene.layer = addLayer(map, "details");
  scene.playerStart = addPointEntity(map, "info_player_start", {16, 16, 24}, scene.layer);

  REQUIRE(f.id(*scene.wall) == "B2");
  REQUIRE(f.id(*scene.light) == "E3");
  REQUIRE(f.id(*scene.door) == "E4");
  REQUIRE(f.id(*scene.group) == "G8");
  REQUIRE(f.id(*scene.floor) == "B7");
  REQUIRE(f.id(*scene.patch) == "P9");
  REQUIRE(f.id(*scene.layer) == "L10");
  REQUIRE(f.id(*scene.playerStart) == "E11");
  return scene;
}

} // namespace tb::ui
