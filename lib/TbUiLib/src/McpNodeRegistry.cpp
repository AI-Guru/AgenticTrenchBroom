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

#include "ui/McpNodeRegistry.h"

#include <QJsonArray>

#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"

#include <algorithm>
#include <ranges>

namespace tb::ui
{
namespace
{

QChar kindLetter(const mdl::Node& node)
{
  return node.accept(kdl::overload(
    [](const mdl::WorldNode&) { return QChar{}; },
    [](const mdl::LayerNode&) { return QChar{'L'}; },
    [](const mdl::GroupNode&) { return QChar{'G'}; },
    [](const mdl::EntityNode&) { return QChar{'E'}; },
    [](const mdl::BrushNode&) { return QChar{'B'}; },
    [](const mdl::PatchNode&) { return QChar{'P'}; }));
}

bool isWorld(const mdl::Map& map, const mdl::Node* node)
{
  return node == &map.worldNode();
}

/** Parses "<K><n>" and returns the letter and number, or nullopt. */
std::optional<std::pair<QChar, uint64_t>> parseNumberedId(const QStringView id)
{
  if (id.size() < 2 || id.size() > 19)
  {
    return std::nullopt;
  }

  const auto letter = id[0];
  if (
    letter != u'L' && letter != u'G' && letter != u'E' && letter != u'B'
    && letter != u'P')
  {
    return std::nullopt;
  }

  if (id[1] == u'0')
  {
    return std::nullopt;
  }

  auto number = uint64_t{0};
  for (const auto c : id.mid(1))
  {
    if (c < u'0' || c > u'9')
    {
      return std::nullopt;
    }
    number = number * 10 + uint64_t(c.unicode() - u'0');
  }
  return std::pair{letter, number};
}

QJsonArray toJsonArray(const std::vector<QString>& ids)
{
  auto result = QJsonArray{};
  for (const auto& id : ids)
  {
    result.append(id);
  }
  return result;
}

} // namespace

QJsonObject McpChangeSet::toJson() const
{
  return {
    {"created", toJsonArray(created)},
    {"removed", toJsonArray(removed)},
    {"modified", toJsonArray(modified)},
  };
}

McpNodeRegistry::McpNodeRegistry(mdl::Map& map)
  : m_map{map}
{
  connectObservers();
  assignInitialIds();
}

McpNodeRegistry::~McpNodeRegistry() = default;

mdl::Map& McpNodeRegistry::map() const
{
  return m_map;
}

QString McpNodeRegistry::idOf(const mdl::Node& node) const
{
  if (isWorld(m_map, &node))
  {
    return QStringLiteral("world");
  }

  const auto it = m_idByNode.find(&node);
  if (it == m_idByNode.end())
  {
    return {};
  }
  return QString{kindLetter(node)} + QString::number(it->second);
}

McpNodeRegistry::Lookup McpNodeRegistry::lookup(const QStringView id) const
{
  if (id == u"world")
  {
    return {LookupStatus::Live, &m_map.worldNode(), {}};
  }

  if (id == u"default_layer")
  {
    return {LookupStatus::Live, m_map.worldNode().defaultLayer(), {}};
  }

  const auto parsed = parseNumberedId(id);
  if (!parsed)
  {
    return {LookupStatus::Malformed, nullptr, {}};
  }

  const auto [letter, number] = *parsed;
  if (const auto it = m_nodeById.find(number); it != m_nodeById.end())
  {
    auto* node = it->second;
    return kindLetter(*node) == letter ? Lookup{LookupStatus::Live, node, {}}
                                       : Lookup{LookupStatus::WrongType, node, {}};
  }

  if (std::ranges::any_of(
        m_pending, [&](const auto& entry) { return entry.second == number; }))
  {
    return {LookupStatus::Stale, nullptr, currentRemovalLabel()};
  }

  if (const auto it = m_tombstones.find(number); it != m_tombstones.end())
  {
    return {LookupStatus::Stale, nullptr, it->second};
  }

  if (number < m_next)
  {
    // the tombstone was dropped because there were too many
    return {LookupStatus::Stale, nullptr, {}};
  }

  return {LookupStatus::Unknown, nullptr, {}};
}

bool McpNodeRegistry::isLive(const mdl::Node* node) const
{
  return isWorld(m_map, node) || m_idByNode.contains(node);
}

size_t McpNodeRegistry::liveCount() const
{
  return m_idByNode.size();
}

size_t McpNodeRegistry::tombstoneCount() const
{
  return m_tombstones.size();
}

void McpNodeRegistry::reconcile()
{
  auto seen = std::unordered_set<const mdl::Node*>{};
  seen.reserve(m_idByNode.size());
  reconcileSubtree(m_map.worldNode(), seen);

  // collect first, the pointers must not be dereferenced
  auto gone = std::vector<std::pair<const mdl::Node*, uint64_t>>{};
  for (const auto& [node, number] : m_idByNode)
  {
    if (!seen.contains(node))
    {
      gone.emplace_back(node, number);
    }
  }
  std::ranges::sort(gone, {}, [](const auto& entry) { return entry.second; });
  for (const auto& [node, number] : gone)
  {
    m_idByNode.erase(node);
    m_nodeById.erase(number);
    if (m_recording && !m_recording->created.contains(number))
    {
      m_recording->removed.insert(number);
    }
    addTombstone(number, currentRemovalLabel());
  }

  flushPending();
}

bool McpNodeRegistry::verifyBijection() const
{
  if (!m_pending.empty() || m_idByNode.size() != m_nodeById.size())
  {
    return false;
  }

  for (const auto& [node, number] : m_idByNode)
  {
    const auto it = m_nodeById.find(number);
    if (it == m_nodeById.end() || it->second != node || number >= m_next)
    {
      return false;
    }
  }

  auto count = size_t{0};
  return verifySubtree(m_map.worldNode(), count) && count == m_idByNode.size();
}

void McpNodeRegistry::setRemovalLabel(QString label)
{
  m_removalLabel = std::move(label);
}

void McpNodeRegistry::beginRecording()
{
  m_recording = Recording{.firstNumber = m_next};
}

McpChangeSet McpNodeRegistry::endRecording()
{
  auto result = currentChanges();
  m_recording = std::nullopt;
  return result;
}

McpChangeSet McpNodeRegistry::currentChanges() const
{
  if (!m_recording)
  {
    return {};
  }

  const auto& recording = *m_recording;
  const auto live = [&](const uint64_t number) { return m_nodeById.contains(number); };

  auto result = McpChangeSet{};
  result.created = sortedIds(recording.created, live);
  result.removed = sortedIds(recording.removed, [&](const uint64_t number) {
    return !live(number) && !recording.created.contains(number);
  });
  result.modified = sortedIds(recording.modified, [&](const uint64_t number) {
    return live(number) && !recording.created.contains(number);
  });

  if (recording.worldModified)
  {
    result.modified.insert(result.modified.begin(), QStringLiteral("world"));
  }
  return result;
}

bool McpNodeRegistry::isRecording() const
{
  return m_recording.has_value();
}

void McpNodeRegistry::restoreIdsAfterRollback()
{
  if (!m_recording)
  {
    return;
  }

  auto revived = std::unordered_set<uint64_t>{};
  auto replaced = std::vector<uint64_t>{};
  for (const auto& [removedNode, oldNumber] : m_recording->removedNodes)
  {
    // only compares the pointer: the node may not be live
    const auto it = m_idByNode.find(removedNode);
    if (
      it == m_idByNode.end() || it->second == oldNumber || m_nodeById.contains(oldNumber))
    {
      continue;
    }

    // the node is live again, so the pointer is valid
    const auto newNumber = it->second;
    auto* node = m_nodeById.at(newNumber);
    m_nodeById.erase(newNumber);
    replaced.push_back(newNumber);

    it->second = oldNumber;
    m_nodeById.emplace(oldNumber, node);
    m_letters.insert_or_assign(oldNumber, kindLetter(*node));
    if (m_tombstones.erase(oldNumber) > 0)
    {
      revived.insert(oldNumber);
    }
  }

  // one pass, since a failed call may have removed many thousands of nodes; this must
  // happen before new tombstones are added, because evicting a tombstone also erases the
  // letter of its number
  if (!revived.empty())
  {
    std::erase_if(
      m_tombstoneOrder, [&](const auto number) { return revived.contains(number); });
  }

  const auto label = currentRemovalLabel();
  for (const auto number : replaced)
  {
    addTombstone(number, label);
  }
}

bool McpNodeRegistry::reassignId(const mdl::Node& node, const QStringView staleId)
{
  const auto it = m_idByNode.find(&node);
  const auto parsed = parseNumberedId(staleId);
  if (it == m_idByNode.end() || !parsed)
  {
    return false;
  }

  const auto [letter, oldNumber] = *parsed;
  if (
    letter != kindLetter(node) || oldNumber >= m_next || m_nodeById.contains(oldNumber)
    || std::ranges::any_of(
      m_pending, [&](const auto& entry) { return entry.second == oldNumber; }))
  {
    return false;
  }

  const auto newNumber = it->second;
  auto* liveNode = m_nodeById.at(newNumber);
  m_nodeById.erase(newNumber);
  it->second = oldNumber;
  m_nodeById.emplace(oldNumber, liveNode);
  m_letters.insert_or_assign(oldNumber, letter);
  if (m_tombstones.erase(oldNumber) > 0)
  {
    std::erase(m_tombstoneOrder, oldNumber);
  }

  if (m_recording)
  {
    const auto wasCreated = m_recording->created.erase(newNumber) > 0;
    m_recording->modified.erase(newNumber);
    if (m_recording->removed.erase(oldNumber) > 0)
    {
      // removed and re-added during the recording
      m_recording->modified.insert(oldNumber);
    }
    else if (wasCreated)
    {
      m_recording->created.insert(oldNumber);
    }
  }

  addTombstone(newNumber, currentRemovalLabel());
  return true;
}

void McpNodeRegistry::disconnectForTesting()
{
  m_notifierConnection.disconnect();
}

void McpNodeRegistry::connectObservers()
{
  m_notifierConnection +=
    m_map.nodesWereAddedNotifier.connect(this, &McpNodeRegistry::nodesWereAdded);
  m_notifierConnection +=
    m_map.nodesWereRemovedNotifier.connect(this, &McpNodeRegistry::nodesWereRemoved);
  m_notifierConnection +=
    m_map.nodesDidChangeNotifier.connect(this, &McpNodeRegistry::nodesDidChange);

  auto& commandProcessor = m_map.commandProcessor();
  m_notifierConnection +=
    commandProcessor.commandDoneNotifier.connect([&](auto&) { flushPending(); });
  m_notifierConnection +=
    commandProcessor.commandDoFailedNotifier.connect([&](auto&) { flushPending(); });
  m_notifierConnection +=
    commandProcessor.commandUndoneNotifier.connect([&](auto&) { flushPending(); });
  m_notifierConnection +=
    commandProcessor.commandUndoFailedNotifier.connect([&](auto&) { flushPending(); });
}

void McpNodeRegistry::assignInitialIds()
{
  for (auto* child : m_map.worldNode().children())
  {
    registerSubtree(*child);
  }
}

void McpNodeRegistry::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  for (auto* node : nodes)
  {
    registerSubtree(*node);
  }
}

void McpNodeRegistry::nodesWereRemoved(const std::vector<mdl::Node*>& nodes)
{
  for (const auto* node : nodes)
  {
    markSubtreeRemoved(*node);
  }
}

void McpNodeRegistry::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  if (!m_recording)
  {
    return;
  }

  for (const auto* node : nodes)
  {
    if (isWorld(m_map, node))
    {
      m_recording->worldModified = true;
    }
    else if (const auto it = m_idByNode.find(node); it != m_idByNode.end())
    {
      m_recording->modified.insert(it->second);
    }
  }
}

void McpNodeRegistry::flushPending()
{
  if (m_pending.empty())
  {
    return;
  }

  auto numbers = m_pending | std::views::values | kdl::ranges::to<std::vector>();
  std::ranges::sort(numbers);

  const auto label = currentRemovalLabel();
  for (const auto number : numbers)
  {
    addTombstone(number, label);
  }
  m_pending.clear();
}

void McpNodeRegistry::addLive(mdl::Node& node)
{
  const auto number = m_next++;
  m_idByNode.emplace(&node, number);
  m_nodeById.emplace(number, &node);
  m_letters.emplace(number, kindLetter(node));
  if (m_recording)
  {
    m_recording->created.insert(number);
  }
}

void McpNodeRegistry::revive(mdl::Node& node, const uint64_t number)
{
  m_pending.erase(&node);
  m_idByNode.emplace(&node, number);
  m_nodeById.emplace(number, &node);
}

void McpNodeRegistry::registerSubtree(mdl::Node& node)
{
  if (!isWorld(m_map, &node) && !m_idByNode.contains(&node))
  {
    if (const auto it = m_pending.find(&node); it != m_pending.end())
    {
      // removed and re-added by the same command, e.g. when reparenting
      revive(node, it->second);
    }
    else
    {
      addLive(node);
    }
  }

  for (auto* child : node.children())
  {
    registerSubtree(*child);
  }
}

void McpNodeRegistry::reconcileSubtree(
  mdl::Node& node, std::unordered_set<const mdl::Node*>& seen)
{
  if (!isWorld(m_map, &node))
  {
    seen.insert(&node);
    if (!m_idByNode.contains(&node))
    {
      if (const auto it = m_pending.find(&node); it != m_pending.end())
      {
        revive(node, it->second);
      }
      else
      {
        addLive(node);
      }
    }
  }

  for (auto* child : node.children())
  {
    reconcileSubtree(*child, seen);
  }
}

void McpNodeRegistry::markSubtreeRemoved(const mdl::Node& node)
{
  if (const auto it = m_idByNode.find(&node); it != m_idByNode.end())
  {
    const auto number = it->second;
    m_idByNode.erase(it);
    m_nodeById.erase(number);
    m_pending.emplace(&node, number);
    if (m_recording && !m_recording->created.contains(number))
    {
      m_recording->removed.insert(number);
    }
    if (m_recording && number < m_recording->firstNumber)
    {
      m_recording->removedNodes.emplace_back(&node, number);
    }
  }

  for (const auto* child : node.children())
  {
    markSubtreeRemoved(*child);
  }
}

bool McpNodeRegistry::verifySubtree(const mdl::Node& node, size_t& count) const
{
  if (!isWorld(m_map, &node))
  {
    ++count;
    const auto it = m_idByNode.find(&node);
    if (it == m_idByNode.end())
    {
      return false;
    }
    const auto letter = m_letters.find(it->second);
    if (letter == m_letters.end() || letter->second != kindLetter(node))
    {
      return false;
    }
  }

  return std::ranges::all_of(
    node.children(), [&](const auto* child) { return verifySubtree(*child, count); });
}

void McpNodeRegistry::addTombstone(const uint64_t number, QString label)
{
  if (m_tombstones.insert_or_assign(number, std::move(label)).second)
  {
    m_tombstoneOrder.push_back(number);
  }

  while (m_tombstones.size() > MaxTombstones)
  {
    m_tombstones.erase(m_tombstoneOrder.front());
    m_letters.erase(m_tombstoneOrder.front());
    m_tombstoneOrder.pop_front();
  }
}

QString McpNodeRegistry::currentRemovalLabel() const
{
  return m_removalLabel.isEmpty() ? QStringLiteral("editor") : m_removalLabel;
}

std::vector<QString> McpNodeRegistry::sortedIds(
  const std::unordered_set<uint64_t>& numbers,
  const std::function<bool(uint64_t)>& predicate) const
{
  auto selected =
    numbers | std::views::filter(predicate) | kdl::ranges::to<std::vector>();
  std::ranges::sort(selected);

  auto result = std::vector<QString>{};
  result.reserve(selected.size());
  for (const auto number : selected)
  {
    const auto it = m_letters.find(number);
    const auto letter = it != m_letters.end() ? it->second : QChar{'N'};
    result.push_back(QString{letter} + QString::number(number));
  }
  return result;
}

} // namespace tb::ui
