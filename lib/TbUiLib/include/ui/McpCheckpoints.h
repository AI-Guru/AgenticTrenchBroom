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

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "base/NotifierConnection.h"
#include "mcp/Tool.h"

#include "kd/result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::mdl
{
class Command;
class Map;
class Node;
} // namespace tb::mdl

namespace tb::ui
{
class McpNodeRegistry;

/**
 * Keeps a model of the map's undo and redo stacks so that MCP can tell how many of the
 * commands that can be undone were made by MCP tools (labels starting with "MCP: ").
 *
 * The command processor does not expose its stacks, so the model is built from its
 * notifications: a top level transaction or command that is done is pushed onto the
 * undo stack, an undo moves the top entry to the redo stack, a redo moves it back, and
 * any new command clears the redo stack. Rollbacks and nested transactions are ignored.
 * Every entry gets a serial number that is never reused, so a checkpoint can later tell
 * whether its history is still a prefix of the current history.
 *
 * The model can get out of sync when the user's own commands are collated, or when the
 * command history is cleared. consistent() detects this by comparing the top labels with
 * the map's, and sync() drops entries when the map's stacks are empty.
 */
class McpHistoryTracker
{
public:
  struct Entry
  {
    uint64_t serial = 0;
    std::string label;
    /** Whether the entry was made by an MCP tool and contains no other command. */
    bool mcp = false;

    bool operator==(const Entry&) const = default;
  };

  explicit McpHistoryTracker(mdl::Map& map);
  ~McpHistoryTracker();

  McpHistoryTracker(const McpHistoryTracker&) = delete;
  McpHistoryTracker& operator=(const McpHistoryTracker&) = delete;

  /** The modelled undo stack; the most recent command is at the back. */
  const std::vector<Entry>& undoStack() const;

  /** The modelled redo stack; the next command to redo is at the back. */
  const std::vector<Entry>& redoStack() const;

  /** The serial numbers of the undo stack entries, oldest first. */
  std::vector<uint64_t> undoSerials() const;

  /** The number of MCP entries on the undo stack. */
  size_t mcpDepth() const;

  /**
   * Whether no transaction is running and the model agrees with the map: each stack is
   * empty exactly if the map's is, and the top labels are equal.
   */
  bool consistent() const;

  /**
   * Drops the modelled undo (redo) stack if the map's undo (redo) stack is empty. Does
   * nothing while a transaction is running.
   */
  void sync();

  /** Returns whether the given undo label belongs to an MCP tool. */
  static bool isMcpLabel(const std::string& label);

private:
  void commandDone(mdl::Command& command);
  void transactionDone(const std::string& name, bool observable, bool isModification);
  void transactionUndone(const std::string& name, bool observable, bool isModification);
  void push(const std::string& label);

  mdl::Map& m_map;
  uint64_t m_nextSerial = 1;
  std::vector<Entry> m_undoStack;
  std::vector<Entry> m_redoStack;
  /** Set when a command is done inside a transaction, i.e. a new transaction is built. */
  bool m_commandDoneInTransaction = false;
  NotifierConnection m_notifierConnection;
};

/** The state of one node at the time a checkpoint was taken. */
struct McpNodeRecord
{
  QString id;
  QString type;

  /**
   * The canonical record of the node (see canonicalRecord). Contains the parent's id, so
   * equal records mean that nothing about the node changed.
   */
  QJsonObject record;

  /**
   * The SHA-256 of the record with the parent id replaced by the type and name path of
   * the parent, so equal digests identify equal content under different ids.
   */
  QString contentDigest;

  /** The node summary (see nodeSummary) at the time the checkpoint was taken. */
  QJsonObject summary;
};

/** A snapshot of every node of a map. */
struct McpCheckpoint
{
  QString name;
  size_t modificationCount = 0;

  /** The number of MCP commands on the undo stack (McpHistoryTracker::mcpDepth). */
  size_t mcpHistoryDepth = 0;

  /** The serials of the undo stack entries (McpHistoryTracker::undoSerials). */
  std::vector<uint64_t> historySerials;

  /** Whether the history model agreed with the map (McpHistoryTracker::consistent). */
  bool historyConsistent = false;

  /** The id free digest of the whole node tree (see contentDigest). */
  QString contentDigest;

  /** The records of all nodes in DFS pre-order, starting with the world. */
  std::vector<McpNodeRecord> records;

  /** Returns the record with the given id, or nullptr. */
  const McpNodeRecord* find(const QString& id) const;

  /** The number of nodes, excluding the world. */
  size_t nodeCount() const;

  /** Returns {name, node_count, content_digest, modification_count, history_depth}. */
  QJsonObject info() const;

  /** Maps ids to indices into records; see reindex(). */
  std::unordered_map<QString, size_t> index = {};

  /** Rebuilds the id index; must be called after records changed. */
  void reindex();
};

/**
 * Returns the canonical record of the given node. All numbers are rounded with canon().
 *
 * - every node: type, parent (the parent's id, null for the world)
 * - world and entities: properties as an array of [key, value] pairs in file order
 * - layers: name, sort_index, omit_from_export
 * - groups: name, transformation (16 numbers, row major), linked
 * - brushes: faces in face order as {normal, distance, material, uv: {offset, scale,
 *   rotation}, surface}, and the vertices in lexicographic order
 * - patches: rows, columns, material and the control points as [x, y, z, u, v]
 */
QJsonObject canonicalRecord(
  const McpNodeRegistry& registry, const mdl::Map& map, const mdl::Node& node);

/**
 * Returns a SHA-256 digest of the content and the structure of the node tree that does
 * not depend on ids or on the order of siblings: each node is hashed from its record
 * without the parent id and the sorted digests of its children. Maps with equal digests
 * contain the same nodes with the same contents under the same parents. Sibling order is
 * ignored because undoing a deletion re-adds the nodes at the end of their parent.
 */
QString contentDigest(const McpNodeRegistry& registry, const mdl::Map& map);

/** Takes a checkpoint of every node of the given map. */
McpCheckpoint takeCheckpoint(
  const McpNodeRegistry& registry,
  const mdl::Map& map,
  const McpHistoryTracker& history,
  QString name);

/**
 * Compares two checkpoints by node id and returns
 *
 * {content_identical, from_digest, to_digest, added: [summary], removed: [{id, type,
 *  summary}], modified: [{id, type, fields: {key: {before, after}}, properties?: {key:
 *  {before, after}}, faces_changed?: [indices], geometry_changed}], reidentified: [{from,
 *  to}], unchanged_count, added_count, removed_count, modified_count,
 *  reidentified_count, truncated}
 *
 * A removed and an added node with equal content digests are reported as reidentified
 * instead, e.g. after a delete was undone. Each list is limited to maxChanges entries;
 * truncated is set if any list was cut.
 */
QJsonObject diffCheckpoints(
  const McpCheckpoint& from, const McpCheckpoint& to, size_t maxChanges);

/** The named checkpoints of one document. */
class McpCheckpointStore
{
public:
  static constexpr size_t MaxCheckpoints = 32;

  /**
   * Adds the given checkpoint. Errors: checkpoint_exists if a checkpoint with the same
   * name exists and replace is false, limit_exceeded if MaxCheckpoints checkpoints exist
   * already.
   */
  kdl::result<const McpCheckpoint*, mcp::ToolError> add(
    McpCheckpoint checkpoint, bool replace);

  /** Returns the checkpoint with the given name, or nullptr. */
  const McpCheckpoint* find(const QString& name) const;

  /** Removes the checkpoint with the given name and returns whether it existed. */
  bool remove(const QString& name);

  /** Returns all checkpoints sorted by name. */
  std::vector<const McpCheckpoint*> list() const;

  /** Returns the names of all checkpoints, sorted. */
  QStringList names() const;

  size_t size() const;

private:
  std::vector<std::unique_ptr<McpCheckpoint>> m_checkpoints;
};

} // namespace tb::ui
