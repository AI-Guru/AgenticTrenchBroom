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

#include "ui/McpNodeRegistry.h"

#include "vm/bbox.h"
#include "vm/plane.h"
#include "vm/vec.h"

#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Brush;
class Issue;
class Map;
class Node;
class Validator;
struct SurfaceAttributes;
} // namespace tb::mdl

namespace tb::ui
{
class McpNodeRegistry;

/**
 * Rounds the given value to 6 decimal places so that the JSON output of the MCP tools is
 * stable across platforms and small floating point differences. Negative zero becomes
 * zero. Values whose magnitude is at least 1e15 are returned unchanged, since they cannot
 * carry 6 decimals anyway.
 */
double canon(double v);

QJsonArray toJson(const vm::vec3d& v);
QJsonArray toJson(const vm::vec2f& v);

/** Returns {"min":[x,y,z],"max":[x,y,z]}. */
QJsonObject toJson(const vm::bbox3d& bounds);

/** Returns {"normal":[x,y,z],"distance":d}. */
QJsonObject toJson(const vm::plane3d& plane);

/**
 * Returns the given positions as canonical [x,y,z] arrays (see canon), sorted. Checkpoint
 * digests and get_brush_geometry use this, so equal geometry gives equal output.
 */
QJsonArray sortedVertices(const std::vector<vm::vec3d>& positions);

/** Returns the set surface attributes: contents, flags, value and color. */
QJsonObject surfaceJson(const mdl::SurfaceAttributes& surface);

/** Returns the string argument with the given key, or nullopt if it is absent. */
std::optional<QString> optionalString(const QJsonObject& args, const QString& key);

/** Returns the ids of the given nodes, which must be registered with the registry. */
QJsonArray idsOf(const McpNodeRegistry& registry, const auto& nodes)
{
  auto result = QJsonArray{};
  for (const auto* node : nodes)
  {
    result.append(registry.idOf(*node));
  }
  return result;
}

/** Decodes a string stored in the map using the map's text encoding. */
QString toUnicode(const mdl::Map& map, const std::string& str);

/** Encodes a string for storing it in the map using the map's text encoding. */
std::string fromUnicode(const mdl::Map& map, const QString& str);

/**
 * Returns "+x", "-x", "+y", "-y", "+z" or "-z" if the given unit normal points along
 * an axis, and nullopt otherwise.
 */
std::optional<QString> sideName(const vm::vec3d& normal);

/** Returns the axis aligned unit normal for the given side name such as "+x". */
std::optional<vm::vec3d> sideNormal(const QString& side);

/** Returns the volume of the given brush in cubic units. */
double brushVolume(const mdl::Brush& brush);

/**
 * Returns the MCP type name of the given node: "world", "layer", "group", "entity",
 * "brush" or "patch".
 */
QString nodeTypeName(const mdl::Node& node);

/**
 * Returns a short summary of the given node:
 *
 * - for every node: id, type, parent, layer, bounds (see nodeBounds; null for an empty
 *   world or layer), selected, visible, locked
 * - name for layers and groups, classname for entities and the world
 * - child_count for worlds, layers, groups and entities
 * - materials (distinct and sorted) for brushes, material for patches
 * - entity (the id of the owning entity or "world") for brushes and patches
 *
 * The node must be registered with the given registry.
 */
QJsonObject nodeSummary(
  const McpNodeRegistry& registry, const mdl::Map& map, const mdl::Node& node);

/**
 * Returns the logical bounds of the given node, or nullopt if it has none. The world and
 * layers are replaced by the merged bounds of their contents: the world has no bounds of
 * its own, and a layer's cached bounds are not updated when children are added or
 * removed.
 */
std::optional<vm::bbox3d> nodeBounds(const mdl::Node& node);

/** Like nodeBounds, but for the physical bounds (which include entity models). */
std::optional<vm::bbox3d> nodePhysicalBounds(const mdl::Node& node);

/**
 * Returns the selection of the given map: {nodes:[ids], faces:[{brush, index, side?}],
 * bounds}. The selected nodes must be registered with the given registry.
 */
QJsonObject selectionJson(const McpNodeRegistry& registry, const mdl::Map& map);

/**
 * Explains that game Generic has no entity definitions except worldspawn and
 * info_player_start. Used by get_conventions and by expectedIssueReason.
 */
QString mcpGenericEntityDefinitionsNote();

/**
 * Returns why the given issue is expected in a new map and harmless, or nullopt. These
 * are the two benign issues that agents see in fresh maps:
 * - "Empty property value" for worldspawn's material collection property (e.g. wad),
 *   which TrenchBroom adds empty to every new map of games that have one, such as Quake.
 * - "Missing entity definition" in game Generic with its own definition file, which only
 *   defines worldspawn and info_player_start, so e.g. light is reported.
 * The validators are the map's registered validators.
 */
std::optional<QString> expectedIssueReason(
  const mdl::Map& map,
  const std::vector<const mdl::Validator*>& validators,
  const mdl::Issue& issue);

/**
 * Adds expected_for_new_map: true and expected_reason to the JSON of the given issue if
 * expectedIssueReason returns a reason. Returns whether it did.
 */
bool addExpectedIssueFields(
  QJsonObject& item,
  const mdl::Map& map,
  const std::vector<const mdl::Validator*>& validators,
  const mdl::Issue& issue);

} // namespace tb::ui
