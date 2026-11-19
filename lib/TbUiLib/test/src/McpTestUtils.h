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

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "mcp/Tool.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/MapFixture.h"
#include "ui/CatchConfig.h" // IWYU pragma: keep
#include "ui/McpDocumentState.h"
#include "ui/McpToolDispatcher.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>


namespace tb::mdl
{
class BrushNode;
class EntityNode;
class GroupNode;
class LayerNode;
class Map;
class Node;
class PatchNode;
} // namespace tb::mdl

namespace tb::ui
{

/** Parses a JSON object; the text must be valid. */
QJsonObject parseObject(const char* text);

/** Returns the compact JSON serialization of the given value. */
QString compactJson(const QJsonValue& value);

/** Serializes the whole map in its file format. */
std::string mapText(mdl::Map& map);

/** The observable state of a map that read tools must not change. */
struct McpMapSnapshot
{
  std::string text;
  size_t modificationCount = 0;
  std::vector<mdl::Node*> selectedNodes;
  std::vector<mdl::BrushFaceHandle> selectedFaces;
  std::optional<std::string> undoLabel;
  std::optional<std::string> redoLabel;

  bool operator==(const McpMapSnapshot&) const = default;
};

McpMapSnapshot snapshot(mdl::Map& map);

/** Adds a cuboid brush to the given parent (default: the current layer or group). */
mdl::BrushNode* addBrush(
  mdl::Map& map,
  const vm::bbox3d& bounds,
  const std::string& material = "material",
  mdl::Node* parent = nullptr);

/** Adds a point entity without consulting entity definitions. */
mdl::EntityNode* addPointEntity(
  mdl::Map& map,
  const std::string& classname,
  const vm::vec3d& origin,
  mdl::Node* parent = nullptr);

/** Adds a brush entity with one cuboid brush per given bounds. */
mdl::EntityNode* addBrushEntity(
  mdl::Map& map,
  const std::string& classname,
  const std::vector<vm::bbox3d>& brushBounds,
  mdl::Node* parent = nullptr);

/** Adds a 3x3 patch. */
mdl::PatchNode* addPatch(mdl::Map& map, mdl::Node* parent = nullptr);

/** Adds a custom layer. */
mdl::LayerNode* addLayer(mdl::Map& map, const std::string& name);

/** Groups the given nodes and returns the group; the group is selected afterwards. */
mdl::GroupNode* groupNodes(
  mdl::Map& map, const std::vector<mdl::Node*>& nodes, const std::string& name);

/**
 * A headless map with an MCP dispatcher that has all map tools registered. The
 * document provider returns the map's document state unless busy is set, in which
 * case it returns that error, or doc is null, in which case it returns no_document.
 */
struct McpMapToolsFixture
{
  explicit McpMapToolsFixture(mdl::MapFixtureConfig config = mdl::QuakeFixtureConfig);

  mdl::MapFixture fixture;
  mdl::Map& map;
  std::unique_ptr<McpDocumentState> doc;
  mcp::ToolRegistry registry;
  std::optional<mcp::ToolError> busy;
  std::vector<McpToolKind> providerCalls;
  McpToolDispatcher dispatcher;

  /** Calls a tool and returns the CallToolResult. */
  QJsonObject call(const QString& tool, const QJsonObject& args);
  QJsonObject call(const QString& tool, const char* jsonArgs = "{}");

  /** Calls a tool, requires success and returns the structured content. */
  QJsonObject ok(const QString& tool, const QJsonObject& args);
  QJsonObject ok(const QString& tool, const char* jsonArgs = "{}");

  /** Calls a tool, requires an error and returns the error object. */
  QJsonObject error(const QString& tool, const QJsonObject& args);
  QJsonObject error(const QString& tool, const char* jsonArgs = "{}");

  /** Calls a tool, requires an error and returns the error code. */
  QString err(const QString& tool, const QJsonObject& args);
  QString err(const QString& tool, const char* jsonArgs = "{}");

  /** Returns the MCP id of the given node. */
  QString id(const mdl::Node& node) const;
};

/**
 * A map with:
 * L1 default layer
 *   B2 wall brush [0,0,0]-[64,64,64]
 *   E3 light at [32,32,128] with light=300, _color="1 1 1"
 *   E4 func_door with B5 [128,0,0]-[144,16,64] and B6 [160,0,0]-[176,16,64]
 *   G8 "room" with B7 floor brush [0,128,0]-[64,192,16] (the brush is created first)
 *   P9 patch
 * L10 "details" layer
 *   E11 info_player_start at [16,16,24]
 */
struct McpTestScene
{
  mdl::BrushNode* wall;
  mdl::EntityNode* light;
  mdl::EntityNode* door;
  mdl::BrushNode* floor;
  mdl::GroupNode* group;
  mdl::PatchNode* patch;
  mdl::LayerNode* layer;
  mdl::EntityNode* playerStart;
};

McpTestScene populateTestScene(McpMapToolsFixture& f);

} // namespace tb::ui
