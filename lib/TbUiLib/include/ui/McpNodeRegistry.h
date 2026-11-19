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
#include <QStringView>

#include "base/NotifierConnection.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::mdl
{
class Map;
class Node;
} // namespace tb::mdl

namespace tb::ui
{

/**
 * The net effect of an MCP tool call on the node tree. Each list is sorted by the
 * numeric part of the ids; "world" comes first if it is contained.
 */
struct McpChangeSet
{
  std::vector<QString> created;
  std::vector<QString> removed;
  std::vector<QString> modified;

  /** Returns {"created":[...],"removed":[...],"modified":[...]}. */
  QJsonObject toJson() const;
};

/**
 * Assigns stable, never reused ids to the nodes of a map.
 *
 * Ids have the form <K><n>, where K is a type tag (L layer, G group, E entity, B brush,
 * P patch) and n >= 1 comes from a single counter per registry. The world node has the id
 * "world", and "default_layer" is accepted as an alias of the default layer's id.
 *
 * On construction, the ids are assigned in DFS pre-order, so the same map always yields
 * the same ids. Afterwards, the registry observes the map: added nodes take the next
 * numbers and removed nodes become stale. A removed node is kept in a pending set until
 * the command that removed it is done or undone, so a node that is removed and re-added
 * by the same command (e.g. when it is reparented) keeps its id. The removing command
 * owns the removed node at least until then, so a pending pointer is never dangling.
 */
class McpNodeRegistry
{
public:
  static constexpr size_t MaxTombstones = 100000;

  enum class LookupStatus
  {
    /** The id refers to a node in the map. */
    Live,
    /** The id was issued, but the node was removed. */
    Stale,
    /** The id was never issued by this registry. */
    Unknown,
    /** The id is not well formed, or it is "@selection". */
    Malformed,
    /** The id refers to a live node, but its type tag does not match the node. */
    WrongType,
  };

  struct Lookup
  {
    LookupStatus status = LookupStatus::Unknown;
    /** Set if status is Live or WrongType. */
    mdl::Node* node = nullptr;
    /** For Stale: the label of the operation that removed the node, if known. */
    QString removedBy;
  };

  explicit McpNodeRegistry(mdl::Map& map);
  ~McpNodeRegistry();

  McpNodeRegistry(const McpNodeRegistry&) = delete;
  McpNodeRegistry& operator=(const McpNodeRegistry&) = delete;

  mdl::Map& map() const;

  /**
   * Returns the id of the given node, e.g. "B12" or "world". Returns an empty string if
   * the node is not registered, which only happens if the caller passes a node that is
   * not part of the map.
   */
  QString idOf(const mdl::Node& node) const;

  /**
   * Looks up the given id. Never dereferences pointers to nodes that are not live, and
   * checks the type tag against the kind of the live node.
   */
  Lookup lookup(QStringView id) const;

  /** Returns whether the given pointer refers to a live node. Never dereferences it. */
  bool isLive(const mdl::Node* node) const;

  /** Returns the number of registered nodes, excluding the world node. */
  size_t liveCount() const;

  /** Returns the number of remembered removed ids. */
  size_t tombstoneCount() const;

  /**
   * Walks the node tree and repairs the registry: unregistered nodes are assigned new ids
   * in DFS order, and registered pointers that are no longer part of the tree become
   * stale without being dereferenced. Called at the start of every MCP tool call.
   */
  void reconcile();

  /**
   * Returns whether the registry maps exactly the nodes of the tree to unique ids and
   * back, and whether no removals are pending.
   */
  bool verifyBijection() const;

  /**
   * Sets the label recorded for nodes that are removed from now on. An empty label
   * records "editor".
   */
  void setRemovalLabel(QString label);

  /** Starts recording changes, discarding any previous recording. */
  void beginRecording();

  /** Stops recording and returns the net changes since beginRecording(). */
  McpChangeSet endRecording();

  /**
   * Returns the net changes recorded so far without stopping the recording, or an empty
   * change set if no recording is running.
   */
  McpChangeSet currentChanges() const;

  bool isRecording() const;

  /**
   * Gives nodes that existed when the recording began and were removed and re-added
   * during the recording their old ids back. Called after the transaction of a failed
   * call was rolled back: rolling back re-adds removed nodes, which would otherwise get
   * new ids, so that a failed call would change ids although it changed nothing else.
   * The ids that the re-added nodes got in the meantime become stale.
   */
  void restoreIdsAfterRollback();

  /**
   * Gives the given live node the given id back, which must be a stale id of the same
   * kind, e.g. after reverting to a checkpoint re-added a node that was deleted after the
   * checkpoint. The node's current id becomes stale. Returns false and changes nothing if
   * the node is not live or the id is not a stale id of the node's kind.
   */
  bool reassignId(const mdl::Node& node, QStringView staleId);

  /**
   * Stops observing the map. Only intended for tests that need to put the registry out
   * of sync with the map to check reconcile().
   */
  void disconnectForTesting();

private:
  struct Recording
  {
    std::unordered_set<uint64_t> created;
    std::unordered_set<uint64_t> removed;
    std::unordered_set<uint64_t> modified;
    bool worldModified = false;
    /** The first number assigned during the recording. */
    uint64_t firstNumber = 0;
    /** Removed nodes that existed when the recording began, in removal order. */
    std::vector<std::pair<const mdl::Node*, uint64_t>> removedNodes;
  };

  void connectObservers();
  void assignInitialIds();

  void nodesWereAdded(const std::vector<mdl::Node*>& nodes);
  void nodesWereRemoved(const std::vector<mdl::Node*>& nodes);
  void nodesDidChange(const std::vector<mdl::Node*>& nodes);
  void flushPending();

  void addLive(mdl::Node& node);
  void revive(mdl::Node& node, uint64_t number);
  void registerSubtree(mdl::Node& node);
  void reconcileSubtree(mdl::Node& node, std::unordered_set<const mdl::Node*>& seen);
  void markSubtreeRemoved(const mdl::Node& node);
  bool verifySubtree(const mdl::Node& node, size_t& count) const;
  void addTombstone(uint64_t number, QString label);
  QString currentRemovalLabel() const;
  std::vector<QString> sortedIds(
    const std::unordered_set<uint64_t>& numbers,
    const std::function<bool(uint64_t)>& predicate) const;

  mdl::Map& m_map;
  uint64_t m_next = 1;
  std::unordered_map<const mdl::Node*, uint64_t> m_idByNode;
  std::unordered_map<uint64_t, mdl::Node*> m_nodeById;
  std::unordered_map<const mdl::Node*, uint64_t> m_pending;
  std::unordered_map<uint64_t, QString> m_tombstones;
  std::deque<uint64_t> m_tombstoneOrder;
  /** The type tag of every live or remembered id. */
  std::unordered_map<uint64_t, QChar> m_letters;
  QString m_removalLabel;
  std::optional<Recording> m_recording;
  NotifierConnection m_notifierConnection;
};

} // namespace tb::ui
