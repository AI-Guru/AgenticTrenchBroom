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

#include "vm/bbox.h"
#include "vm/plane.h"
#include "vm/vec.h"

#include <optional>
#include <string>

namespace tb::mdl
{
class Brush;
class Map;
class Node;
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

/** Decodes a string stored in the map using the map's text encoding. */
QString toUnicode(const mdl::Map& map, const std::string& str);

/** Encodes a string for storing it in the map using the map's text encoding. */
std::string fromUnicode(const mdl::Map& map, const QString& str);

/**
 * Returns the lower case hex SHA-256 digest of the compact JSON serialization of the
 * given value. Scalars are wrapped in an array before hashing.
 */
QString sha256Hex(const QJsonValue& value);

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
 * - for every node: id, type, parent, layer, bounds, selected, visible, locked
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
 * Returns the selection of the given map: {nodes:[ids], faces:[{brush, index, side?}],
 * bounds}. The selected nodes must be registered with the given registry.
 */
QJsonObject selectionJson(const McpNodeRegistry& registry, const mdl::Map& map);

} // namespace tb::ui
