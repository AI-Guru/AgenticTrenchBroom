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
#include "ui/McpDocumentState.h"
#include "ui/McpJson.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <limits>
#include <optional>
#include <variant>
#include <vector>

namespace tb::mdl
{
class BrushFaceHandle;
class GroupNode;
class Map;
class Node;
} // namespace tb::mdl

namespace tb::ui
{

/**
 * Helpers that check agent input before it reaches a TrenchBroom function. Many of those
 * functions have contracts, and a contract violation aborts the application, so every
 * helper reports bad input as an mcp::ToolError instead.
 */

/** Creates a tool error. */
mcp::ToolError toolError(
  QString code, QString message, QString hint = {}, QJsonObject details = {});

/** Returns the error of a failed result. */
template <typename T>
mcp::ToolError errorOf(const McpResult<T>& result)
{
  return std::get<mcp::ToolError>(result.error());
}

/**
 * Resolves a single node id. @selection is rejected.
 *
 * Errors: invalid_argument (malformed), unknown_node, stale_node (details.removed_by),
 * wrong_node_type (the type tag does not match the node).
 */
McpResult<mdl::Node*> resolveNode(const McpDocumentState& doc, const QString& id);

/**
 * Resolves a single node id and checks that the node has the given type. The expected
 * name is reported to the client, e.g. "brush".
 */
template <typename T>
McpResult<T*> resolveAs(
  const McpDocumentState& doc, const QString& id, const QString& expected)
{
  return resolveNode(doc, id) | kdl::and_then([&](mdl::Node* node) -> McpResult<T*> {
           if (auto* typedNode = dynamic_cast<T*>(node))
           {
             return typedNode;
           }
           return toolError(
             "wrong_node_type",
             QString{"%1 is a %2, not a %3"}.arg(id, nodeTypeName(*node), expected),
             "call get_nodes to inspect the node",
             {{"id", id}, {"actual", nodeTypeName(*node)}, {"expected", expected}});
         });
}

/**
 * Resolves a list of node ids without checking whether the nodes may be edited. Expands
 * @selection to the selected nodes and removes duplicates, keeping the first occurrence.
 * For read tools. Returns invalid_argument if more than maxCount nodes remain, which
 * bounds the output when @selection expands to a large selection.
 */
McpResult<std::vector<mdl::Node*>> resolveNodes(
  const McpDocumentState& doc,
  const QJsonArray& ids,
  size_t maxCount = std::numeric_limits<size_t>::max());

enum class McpTargetPurpose
{
  /** The targets will be selected and edited. */
  Edit,
  /** The targets will be deleted. */
  Delete,
};

/**
 * Resolves the targets of an editing tool:
 *
 * - expands @selection, removes duplicates and drops ids that are descendants of other
 *   listed ids
 * - rejects the world and layers (wrong_node_type; for Delete, layers are not_supported)
 * - for Edit, expands brush entities to their brushes and patches
 * - every target must be selectable (node_not_selectable with details.reason hidden,
 *   locked, in_closed_group or group_open); for Delete, brush entities are accepted
 * - no target may be inside a linked group (not_supported)
 */
McpResult<std::vector<mdl::Node*>> resolveTargets(
  const McpDocumentState& doc,
  const QJsonArray& ids,
  McpTargetPurpose purpose = McpTargetPurpose::Edit);

/**
 * Resolves the parent for new nodes. Without an id, returns mdl::parentForNodes. Accepts
 * layers, groups that are not linked, and brush entities; "world" means the default
 * layer. Errors: invalid_parent, or any resolveNode error.
 */
McpResult<mdl::Node*> resolveParent(
  const McpDocumentState& doc, const std::optional<QString>& id);

/**
 * Resolves face references {brush, index | side | normal}. Exactly one of index, side
 * and normal must be given. Errors: invalid_argument, face_not_found (details.faces lists
 * the brush's faces), node_not_selectable, or any resolveAs error.
 */
McpResult<std::vector<mdl::BrushFaceHandle>> resolveFaces(
  const McpDocumentState& doc, const QJsonArray& faceRefs);

/** Largest coordinate accepted from agents. */
inline constexpr double MaxCoordinate = 1e6;

/**
 * Converts [x, y, z] to a vector. Every component must be a finite number whose
 * magnitude is at most MaxCoordinate. The pointer names the value in errors.
 */
McpResult<vm::vec3d> vec3(const QJsonValue& json, const QString& pointer);

/**
 * Converts {"min": [..], "max": [..]} to a bounding box. min must not be greater than
 * max on any axis.
 */
McpResult<vm::bbox3d> bounds(const QJsonValue& json, const QString& pointer);

/**
 * Checks that min < max by at least 1/1024 on every axis and that the box lies inside the
 * world bounds (out_of_world_bounds).
 */
McpResult<vm::bbox3d> boxInWorld(
  const mdl::Map& map, const vm::vec3d& min, const vm::vec3d& max);

/** Returns editor_busy (transaction_active) if a transaction is running. */
McpResult<void> requireNoActiveTransaction(const mdl::Map& map);

/** Whether Map::undoCommand may be called without violating its contracts. */
bool canUndo(const mdl::Map& map);

/**
 * Whether Map::redoCommand may be called without violating its contracts. Note that
 * Map::canRedoCommand checks the undo stack instead of the redo stack, so it must not be
 * used for this.
 */
bool canRedo(const mdl::Map& map);

enum class McpMapText
{
  Key,
  Value,
};

/**
 * Checks a string that will be written to the map file as an entity property key or
 * value: keys must not be empty, neither may contain '"', '\n' or '\r', and both are
 * limited to 32 KiB.
 */
McpResult<void> checkMapText(const QString& text, McpMapText kind);

/** Returns whether the given group is linked to at least one other group. */
bool isLinkedGroup(mdl::Map& map, const mdl::GroupNode& groupNode);

/**
 * Reads an optional non-negative integer argument that the schema has already checked.
 * Values are clamped to [0, 1e9] so that the conversion is always defined.
 */
size_t sizeArgument(const QJsonObject& args, const QString& key, size_t defaultValue);

/**
 * Matches a case insensitive glob pattern, where '*' matches any sequence of characters
 * and '?' matches a single character, against the whole text.
 */
bool globMatch(QStringView pattern, QStringView text);

} // namespace tb::ui
