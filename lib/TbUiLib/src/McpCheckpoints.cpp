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

#include "ui/McpCheckpoints.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Command.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "ui/McpJson.h"
#include "ui/McpNodeRegistry.h"
#include "ui/McpPreconditions.h"

#include "kd/overload.h"

#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <unordered_set>

namespace tb::ui
{

// McpHistoryTracker

McpHistoryTracker::McpHistoryTracker(mdl::Map& map)
  : m_map{map}
{
  auto& commandProcessor = m_map.commandProcessor();
  m_notifierConnection +=
    commandProcessor.commandDoneNotifier.connect(this, &McpHistoryTracker::commandDone);
  m_notifierConnection += commandProcessor.transactionDoneNotifier.connect(
    this, &McpHistoryTracker::transactionDone);
  m_notifierConnection += commandProcessor.transactionUndoneNotifier.connect(
    this, &McpHistoryTracker::transactionUndone);

  // The tracker is created on the first MCP call for a document, which may already have
  // undo or redo history. Model the top of each stack with a non-MCP placeholder, so
  // that the model agrees with the map and checkpoints taken now can be reverted. The
  // placeholders are never undone by checkpoint_revert because they are not MCP entries;
  // undoing past them makes the model inconsistent, which only causes refusals.
  if (const auto* undoName = m_map.undoCommandName())
  {
    m_undoStack.push_back(
      Entry{.serial = m_nextSerial++, .label = *undoName, .mcp = false});
  }
  if (const auto* redoName = m_map.redoCommandName())
  {
    m_redoStack.push_back(
      Entry{.serial = m_nextSerial++, .label = *redoName, .mcp = false});
  }
}

McpHistoryTracker::~McpHistoryTracker() = default;

const std::vector<McpHistoryTracker::Entry>& McpHistoryTracker::undoStack() const
{
  return m_undoStack;
}

const std::vector<McpHistoryTracker::Entry>& McpHistoryTracker::redoStack() const
{
  return m_redoStack;
}

std::vector<uint64_t> McpHistoryTracker::undoSerials() const
{
  auto result = std::vector<uint64_t>{};
  result.reserve(m_undoStack.size());
  for (const auto& entry : m_undoStack)
  {
    result.push_back(entry.serial);
  }
  return result;
}

size_t McpHistoryTracker::mcpDepth() const
{
  return size_t(std::ranges::count_if(m_undoStack, [](const auto& e) { return e.mcp; }));
}

bool McpHistoryTracker::consistent() const
{
  if (m_map.commandProcessor().isTransactionActive())
  {
    return false;
  }

  const auto agrees = [](const std::vector<Entry>& stack, const std::string* label) {
    return stack.empty() ? label == nullptr : label && *label == stack.back().label;
  };
  return agrees(m_undoStack, m_map.undoCommandName())
         && agrees(m_redoStack, m_map.redoCommandName());
}

void McpHistoryTracker::sync()
{
  if (m_map.commandProcessor().isTransactionActive())
  {
    return;
  }

  if (!m_map.undoCommandName())
  {
    m_undoStack.clear();
  }
  if (!m_map.redoCommandName())
  {
    m_redoStack.clear();
  }
}

bool McpHistoryTracker::isMcpLabel(const std::string& label)
{
  return label.starts_with("MCP: ");
}

void McpHistoryTracker::commandDone(mdl::Command&)
{
  if (m_map.commandProcessor().isTransactionActive())
  {
    // A command executed inside a transaction clears the redo stack. Commands that are
    // redone are executed outside of any transaction.
    m_commandDoneInTransaction = true;
    m_redoStack.clear();
  }
  else
  {
    // A command executed outside of a transaction, or a command that is redone. This
    // also resets the flag after a top level transaction was rolled back.
    m_commandDoneInTransaction = false;
  }
}

void McpHistoryTracker::transactionDone(const std::string& name, bool, bool)
{
  if (m_map.commandProcessor().isTransactionActive())
  {
    // a nested transaction was committed into its parent
    return;
  }

  if (std::exchange(m_commandDoneInTransaction, false))
  {
    // A new transaction was committed and already pushed onto the undo stack. If the
    // top of the undo stack has a different name, it was collated with its predecessor.
    const auto* undoName = m_map.undoCommandName();
    if (undoName && *undoName != name && !m_undoStack.empty())
    {
      auto& top = m_undoStack.back();
      top.mcp = top.mcp && isMcpLabel(name);
      return;
    }
    push(name);
    return;
  }

  // Either a command was executed outside of a transaction, or a command was redone. A
  // redone command was popped from the redo stack before it was executed, while a new
  // command clears the redo stack only after this notification.
  if (!m_redoStack.empty() && m_redoStack.back().label == name)
  {
    const auto* redoName = m_map.redoCommandName();
    const auto* below =
      m_redoStack.size() > 1 ? &m_redoStack[m_redoStack.size() - 2].label : nullptr;
    const auto isRedo = below ? redoName && *redoName == *below : redoName == nullptr;
    if (isRedo)
    {
      m_undoStack.push_back(m_redoStack.back());
      m_redoStack.pop_back();
      return;
    }
  }

  push(name);
}

void McpHistoryTracker::transactionUndone(const std::string&, bool, bool)
{
  if (m_map.commandProcessor().isTransactionActive())
  {
    // A transaction was rolled back. If it was nested, the commands its parent executed
    // before are still part of the parent, so the flag must be kept. If it was the top
    // level transaction, the flag is reset by the next command (see commandDone).
    return;
  }

  m_commandDoneInTransaction = false;

  if (!m_undoStack.empty())
  {
    m_redoStack.push_back(m_undoStack.back());
    m_undoStack.pop_back();
  }
}

void McpHistoryTracker::push(const std::string& label)
{
  m_redoStack.clear();
  m_undoStack.push_back(
    Entry{.serial = m_nextSerial++, .label = label, .mcp = isMcpLabel(label)});
}

// McpCheckpoint

const McpNodeRecord* McpCheckpoint::find(const QString& id) const
{
  const auto it = index.find(id);
  return it != index.end() ? &records[it->second] : nullptr;
}

size_t McpCheckpoint::nodeCount() const
{
  return records.empty() ? 0 : records.size() - 1;
}

QJsonObject McpCheckpoint::info() const
{
  return {
    {"name", name},
    {"node_count", qint64(nodeCount())},
    {"content_digest", contentDigest},
    {"modification_count", qint64(modificationCount)},
    {"history_depth", qint64(mcpHistoryDepth)},
  };
}

void McpCheckpoint::reindex()
{
  index.clear();
  for (size_t i = 0; i < records.size(); ++i)
  {
    index.emplace(records[i].id, i);
  }
}

// records and digests

namespace
{

using LinkIdCounts = std::unordered_map<std::string, size_t>;

void visitPreOrder(
  const mdl::Node& node,
  const size_t depth,
  const std::function<void(const mdl::Node&, size_t)>& visitor)
{
  visitor(node, depth);
  for (const auto* child : node.children())
  {
    visitPreOrder(*child, depth + 1, visitor);
  }
}

LinkIdCounts countLinkIds(const mdl::Map& map)
{
  auto counts = LinkIdCounts{};
  visitPreOrder(map.worldNode(), 0, [&](const mdl::Node& node, size_t) {
    if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
    {
      ++counts[groupNode->linkId()];
    }
  });
  return counts;
}

QJsonArray propertiesJson(const mdl::Map& map, const mdl::Entity& entity)
{
  auto result = QJsonArray{};
  for (const auto& property : entity.properties())
  {
    result.append(
      QJsonArray{toUnicode(map, property.key()), toUnicode(map, property.value())});
  }
  return result;
}

QJsonArray matrixJson(const vm::mat4x4d& matrix)
{
  auto result = QJsonArray{};
  for (size_t r = 0; r < 4; ++r)
  {
    for (size_t c = 0; c < 4; ++c)
    {
      result.append(canon(matrix[c][r]));
    }
  }
  return result;
}


QJsonObject faceJson(const mdl::Map& map, const mdl::BrushFace& face)
{
  const auto& uv = face.uvAttributes();
  return {
    {"normal", toJson(face.boundary().normal)},
    {"distance", canon(face.boundary().distance)},
    {"material", toUnicode(map, face.materialName())},
    {"uv",
     QJsonObject{
       {"offset", toJson(uv.offset)},
       {"scale", toJson(uv.scale)},
       {"rotation", canon(double(uv.rotation))},
     }},
    {"surface", surfaceJson(face.surfaceAttributes())},
  };
}

QJsonObject recordOf(
  const McpNodeRegistry& registry,
  const mdl::Map& map,
  const mdl::Node& node,
  const LinkIdCounts& linkIdCounts)
{
  auto result = QJsonObject{
    {"type", nodeTypeName(node)},
    {"parent", node.parent() ? QJsonValue{registry.idOf(*node.parent())} : QJsonValue{}},
  };

  node.accept(kdl::overload(
    [&](const mdl::WorldNode& worldNode) {
      result["properties"] = propertiesJson(map, worldNode.entity());
    },
    [&](const mdl::LayerNode& layerNode) {
      const auto& layer = layerNode.layer();
      result["name"] = toUnicode(map, layer.name());
      result["sort_index"] = layer.sortIndex();
      result["omit_from_export"] = layer.omitFromExport();
    },
    [&](const mdl::GroupNode& groupNode) {
      const auto& group = groupNode.group();
      const auto it = linkIdCounts.find(groupNode.linkId());
      result["name"] = toUnicode(map, group.name());
      result["transformation"] = matrixJson(group.transformation());
      result["linked"] = it != linkIdCounts.end() && it->second > 1;
    },
    [&](const mdl::EntityNode& entityNode) {
      result["properties"] = propertiesJson(map, entityNode.entity());
    },
    [&](const mdl::BrushNode& brushNode) {
      const auto& brush = brushNode.brush();
      auto faces = QJsonArray{};
      for (const auto& face : brush.faces())
      {
        faces.append(faceJson(map, face));
      }
      result["faces"] = faces;
      result["vertices"] = sortedVertices(brush.vertexPositions());
    },
    [&](const mdl::PatchNode& patchNode) {
      const auto& patch = patchNode.patch();
      auto points = QJsonArray{};
      for (const auto& point : patch.controlPoints())
      {
        points.append(QJsonArray{
          canon(point[0]),
          canon(point[1]),
          canon(point[2]),
          canon(point[3]),
          canon(point[4])});
      }
      result["rows"] = qint64(patch.pointRowCount());
      result["columns"] = qint64(patch.pointColumnCount());
      result["material"] = toUnicode(map, patch.materialName());
      result["points"] = points;
    }));

  return result;
}

/** Identifies a node by its type and name or classname, but not by its id. */
QString pathSegment(const mdl::Map& map, const mdl::Node& node)
{
  return node.accept(kdl::overload(
    [](const mdl::WorldNode&) { return QStringLiteral("world"); },
    [&](const mdl::LayerNode& layerNode) {
      return "layer:" + toUnicode(map, layerNode.layer().name());
    },
    [&](const mdl::GroupNode& groupNode) {
      return "group:" + toUnicode(map, groupNode.group().name());
    },
    [&](const mdl::EntityNode& entityNode) {
      return "entity:" + toUnicode(map, entityNode.entity().classname());
    },
    [](const mdl::BrushNode&) { return QStringLiteral("brush"); },
    [](const mdl::PatchNode&) { return QStringLiteral("patch"); }));
}

QJsonObject idFreeRecord(QJsonObject record, const QJsonArray& parentPath)
{
  record.remove("parent");
  record["parent_path"] = parentPath;
  return record;
}

QByteArray compact(const QJsonObject& object)
{
  return QJsonDocument{object}.toJson(QJsonDocument::Compact);
}

/**
 * Computes SHA-256 digests with one QCryptographicHash. Constructing a hash object is
 * expensive with some backends (OpenSSL fetches the algorithm every time), so the
 * digests of all nodes share one object that is reset between uses.
 */
class Sha256
{
public:
  QByteArray operator()(const QByteArray& data)
  {
    m_hash.reset();
    m_hash.addData(data);
    return m_hash.result();
  }

  QByteArray operator()(const QByteArray& data, const std::vector<QByteArray>& children)
  {
    m_hash.reset();
    m_hash.addData(data);
    for (const auto& child : children)
    {
      m_hash.addData(QByteArrayView{"\n"});
      m_hash.addData(child);
    }
    return m_hash.result();
  }

private:
  QCryptographicHash m_hash{QCryptographicHash::Sha256};
};

using RecordVisitor = std::function<void(
  const mdl::Node&, QJsonObject record, const QJsonArray& parentPath, Sha256& sha256)>;

/**
 * Visits the given node and its descendants in DFS pre-order with their records and id
 * free parent paths, and returns the digest of the subtree: the hash of the node's id
 * free record and the sorted digests of its children. Sorting makes the digest
 * independent of the order of siblings, which changes e.g. when a deleted node is
 * restored by undo.
 */
QByteArray visitSubtree(
  const McpNodeRegistry& registry,
  const mdl::Map& map,
  const LinkIdCounts& linkIdCounts,
  const mdl::Node& node,
  QJsonArray& path,
  Sha256& sha256,
  const RecordVisitor& visitor)
{
  auto record = recordOf(registry, map, node, linkIdCounts);
  auto treeRecord = record;
  treeRecord.remove("parent");
  visitor(node, std::move(record), path, sha256);

  path.append(pathSegment(map, node));
  auto childDigests = std::vector<QByteArray>{};
  childDigests.reserve(node.childCount());
  for (const auto* child : node.children())
  {
    childDigests.push_back(
      visitSubtree(registry, map, linkIdCounts, *child, path, sha256, visitor));
  }
  path.removeLast();
  std::ranges::sort(childDigests);

  return sha256(compact(treeRecord), childDigests);
}

/** Visits every node and returns the content digest of the whole tree. */
QString visitRecords(
  const McpNodeRegistry& registry, const mdl::Map& map, const RecordVisitor& visitor)
{
  const auto linkIdCounts = countLinkIds(map);
  auto path = QJsonArray{};
  auto sha256 = Sha256{};
  return QString::fromLatin1(
    visitSubtree(registry, map, linkIdCounts, map.worldNode(), path, sha256, visitor)
      .toHex());
}

QJsonValue optionalValue(const QJsonObject& object, const QString& key)
{
  return object.contains(key) ? object.value(key) : QJsonValue{};
}

/** Returns key -> value of the first occurrence of each key, and the keys in order. */
std::pair<std::unordered_map<QString, QJsonValue>, QJsonArray> propertyMap(
  const QJsonArray& properties)
{
  auto values = std::unordered_map<QString, QJsonValue>{};
  auto keys = QJsonArray{};
  for (const auto& property : properties)
  {
    const auto pair = property.toArray();
    const auto key = pair.at(0).toString();
    keys.append(key);
    values.emplace(key, pair.at(1));
  }
  return {std::move(values), std::move(keys)};
}

QJsonObject modification(const McpNodeRecord& before, const McpNodeRecord& after)
{
  static const auto structuredKeys =
    std::set<QString>{"properties", "faces", "vertices", "points"};

  const auto& a = before.record;
  const auto& b = after.record;

  auto keys = std::set<QString>{};
  for (const auto& key : a.keys())
  {
    keys.insert(key);
  }
  for (const auto& key : b.keys())
  {
    keys.insert(key);
  }

  auto fields = QJsonObject{};
  for (const auto& key : keys)
  {
    if (structuredKeys.contains(key))
    {
      continue;
    }
    const auto valueBefore = optionalValue(a, key);
    const auto valueAfter = optionalValue(b, key);
    if (valueBefore != valueAfter)
    {
      fields[key] = QJsonObject{{"before", valueBefore}, {"after", valueAfter}};
    }
  }

  auto result = QJsonObject{{"id", after.id}, {"type", after.type}};

  if (a.contains("properties") || b.contains("properties"))
  {
    const auto [valuesBefore, keysBefore] = propertyMap(a.value("properties").toArray());
    const auto [valuesAfter, keysAfter] = propertyMap(b.value("properties").toArray());

    auto allKeys = std::set<QString>{};
    for (const auto& [key, value] : valuesBefore)
    {
      allKeys.insert(key);
    }
    for (const auto& [key, value] : valuesAfter)
    {
      allKeys.insert(key);
    }

    auto properties = QJsonObject{};
    for (const auto& key : allKeys)
    {
      const auto itBefore = valuesBefore.find(key);
      const auto itAfter = valuesAfter.find(key);
      const auto valueBefore =
        itBefore != valuesBefore.end() ? itBefore->second : QJsonValue{};
      const auto valueAfter =
        itAfter != valuesAfter.end() ? itAfter->second : QJsonValue{};
      if (valueBefore != valueAfter)
      {
        properties[key] = QJsonObject{{"before", valueBefore}, {"after", valueAfter}};
      }
    }

    if (!properties.isEmpty())
    {
      result["properties"] = properties;
    }
    else if (a.value("properties") != b.value("properties"))
    {
      fields["property_order"] =
        QJsonObject{{"before", keysBefore}, {"after", keysAfter}};
    }
  }

  auto geometryChanged = a.value("vertices") != b.value("vertices")
                         || a.value("points") != b.value("points")
                         || a.value("transformation") != b.value("transformation");

  if (a.contains("faces") || b.contains("faces"))
  {
    const auto facesBefore = a.value("faces").toArray();
    const auto facesAfter = b.value("faces").toArray();
    auto facesChanged = QJsonArray{};
    for (qsizetype i = 0; i < std::max(facesBefore.size(), facesAfter.size()); ++i)
    {
      const auto faceBefore = i < facesBefore.size() ? facesBefore[i] : QJsonValue{};
      const auto faceAfter = i < facesAfter.size() ? facesAfter[i] : QJsonValue{};
      if (faceBefore != faceAfter)
      {
        facesChanged.append(qint64(i));
        const auto planeChanged =
          faceBefore.toObject().value("normal") != faceAfter.toObject().value("normal")
          || faceBefore.toObject().value("distance")
               != faceAfter.toObject().value("distance");
        geometryChanged = geometryChanged || planeChanged;
      }
    }
    if (!facesChanged.isEmpty())
    {
      result["faces_changed"] = facesChanged;
    }
  }

  result["fields"] = fields;
  result["geometry_changed"] = geometryChanged;
  return result;
}

} // namespace

QJsonObject canonicalRecord(
  const McpNodeRegistry& registry, const mdl::Map& map, const mdl::Node& node)
{
  return recordOf(registry, map, node, countLinkIds(map));
}

QString contentDigest(const McpNodeRegistry& registry, const mdl::Map& map)
{
  return visitRecords(
    registry, map, [](const mdl::Node&, QJsonObject, const QJsonArray&, Sha256&) {});
}

McpCheckpoint takeCheckpoint(
  const McpNodeRegistry& registry,
  const mdl::Map& map,
  const McpHistoryTracker& history,
  QString name)
{
  auto checkpoint = McpCheckpoint{
    .name = std::move(name),
    .modificationCount = map.modificationCount(),
    .mcpHistoryDepth = history.mcpDepth(),
    .historySerials = history.undoSerials(),
    .historyConsistent = history.consistent(),
  };

  checkpoint.contentDigest = visitRecords(
    registry,
    map,
    [&](
      const mdl::Node& node,
      QJsonObject record,
      const QJsonArray& parentPath,
      Sha256& sha256) {
      auto contentDigest =
        QString::fromLatin1(sha256(compact(idFreeRecord(record, parentPath))).toHex());
      checkpoint.records.push_back(McpNodeRecord{
        .id = registry.idOf(node),
        .type = nodeTypeName(node),
        .record = std::move(record),
        .contentDigest = std::move(contentDigest),
        .summary = nodeSummary(registry, map, node),
      });
    });

  checkpoint.reindex();
  return checkpoint;
}

QJsonObject diffCheckpoints(
  const McpCheckpoint& from, const McpCheckpoint& to, const size_t maxChanges)
{
  auto removed = std::vector<const McpNodeRecord*>{};
  auto modified = std::vector<std::pair<const McpNodeRecord*, const McpNodeRecord*>>{};
  auto unchangedCount = size_t(0);

  for (const auto& record : from.records)
  {
    if (const auto* current = to.find(record.id))
    {
      if (current->record == record.record)
      {
        ++unchangedCount;
      }
      else
      {
        modified.emplace_back(&record, current);
      }
    }
    else
    {
      removed.push_back(&record);
    }
  }

  auto added = std::vector<const McpNodeRecord*>{};
  for (const auto& record : to.records)
  {
    if (!from.find(record.id))
    {
      added.push_back(&record);
    }
  }

  // pair removed and added nodes with equal content
  auto removedByDigest = std::unordered_map<QString, std::vector<size_t>>{};
  for (size_t i = removed.size(); i > 0; --i)
  {
    removedByDigest[removed[i - 1]->contentDigest].push_back(i - 1);
  }

  auto reidentified =
    std::vector<std::pair<const McpNodeRecord*, const McpNodeRecord*>>{};
  auto matchedRemoved = std::unordered_set<size_t>{};
  auto unmatchedAdded = std::vector<const McpNodeRecord*>{};
  for (const auto* record : added)
  {
    auto it = removedByDigest.find(record->contentDigest);
    if (it != removedByDigest.end() && !it->second.empty())
    {
      const auto index = it->second.back();
      it->second.pop_back();
      matchedRemoved.insert(index);
      reidentified.emplace_back(removed[index], record);
    }
    else
    {
      unmatchedAdded.push_back(record);
    }
  }

  auto unmatchedRemoved = std::vector<const McpNodeRecord*>{};
  for (size_t i = 0; i < removed.size(); ++i)
  {
    if (!matchedRemoved.contains(i))
    {
      unmatchedRemoved.push_back(removed[i]);
    }
  }

  auto truncated = false;
  const auto limited = [&](const auto& items, const auto& toJson) {
    auto result = QJsonArray{};
    for (size_t i = 0; i < items.size(); ++i)
    {
      if (i == maxChanges)
      {
        truncated = true;
        break;
      }
      result.append(toJson(items[i]));
    }
    return result;
  };

  auto result = QJsonObject{
    {"content_identical", from.contentDigest == to.contentDigest},
    {"from_digest", from.contentDigest},
    {"to_digest", to.contentDigest},
    {"added",
     limited(
       unmatchedAdded, [](const McpNodeRecord* record) { return record->summary; })},
    {"removed",
     limited(
       unmatchedRemoved,
       [](const McpNodeRecord* record) {
         return QJsonObject{
           {"id", record->id}, {"type", record->type}, {"summary", record->summary}};
       })},
    {"modified",
     limited(
       modified,
       [](const auto& pair) { return modification(*pair.first, *pair.second); })},
    {"reidentified",
     limited(
       reidentified,
       [](const auto& pair) {
         return QJsonObject{{"from", pair.first->id}, {"to", pair.second->id}};
       })},
    {"unchanged_count", qint64(unchangedCount)},
    {"added_count", qint64(unmatchedAdded.size())},
    {"removed_count", qint64(unmatchedRemoved.size())},
    {"modified_count", qint64(modified.size())},
    {"reidentified_count", qint64(reidentified.size())},
  };
  result["truncated"] = truncated;
  return result;
}

// McpCheckpointStore

kdl::result<const McpCheckpoint*, mcp::ToolError> McpCheckpointStore::add(
  McpCheckpoint checkpoint, const bool replace)
{
  const auto it = std::ranges::find_if(
    m_checkpoints, [&](const auto& c) { return c->name == checkpoint.name; });
  if (it != m_checkpoints.end())
  {
    if (!replace)
    {
      return toolError(
        "checkpoint_exists",
        QString{"checkpoint '%1' exists already"}.arg(checkpoint.name),
        "pass replace:true or choose another name",
        {{"name", checkpoint.name}});
    }
    **it = std::move(checkpoint);
    return it->get();
  }

  if (m_checkpoints.size() >= MaxCheckpoints)
  {
    return toolError(
      "limit_exceeded",
      QString{"at most %1 checkpoints can exist"}.arg(MaxCheckpoints),
      "call checkpoint_delete to remove checkpoints you no longer need",
      {{"limit", qint64(MaxCheckpoints)},
       {"checkpoints", QJsonArray::fromStringList(names())}});
  }

  m_checkpoints.push_back(std::make_unique<McpCheckpoint>(std::move(checkpoint)));
  return m_checkpoints.back().get();
}

const McpCheckpoint* McpCheckpointStore::find(const QString& name) const
{
  const auto it =
    std::ranges::find_if(m_checkpoints, [&](const auto& c) { return c->name == name; });
  return it != m_checkpoints.end() ? it->get() : nullptr;
}

bool McpCheckpointStore::remove(const QString& name)
{
  return std::erase_if(m_checkpoints, [&](const auto& c) { return c->name == name; }) > 0;
}

std::vector<const McpCheckpoint*> McpCheckpointStore::list() const
{
  auto result = std::vector<const McpCheckpoint*>{};
  result.reserve(m_checkpoints.size());
  for (const auto& checkpoint : m_checkpoints)
  {
    result.push_back(checkpoint.get());
  }
  std::ranges::sort(
    result, [](const auto* a, const auto* b) { return a->name < b->name; });
  return result;
}

QStringList McpCheckpointStore::names() const
{
  auto result = QStringList{};
  for (const auto* checkpoint : list())
  {
    result.push_back(checkpoint->name);
  }
  return result;
}

size_t McpCheckpointStore::size() const
{
  return m_checkpoints.size();
}

} // namespace tb::ui
