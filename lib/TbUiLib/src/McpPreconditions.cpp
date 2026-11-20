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

#include "ui/McpPreconditions.h"

#include <QLocale>
#include <QRegularExpression>

#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace tb::ui
{
namespace
{

const auto StaleHint =
  QStringLiteral("call find_nodes or get_scene_tree for current ids");

QString typeNameForLetter(const QChar letter)
{
  switch (letter.unicode())
  {
  case u'L':
    return QStringLiteral("layer");
  case u'G':
    return QStringLiteral("group");
  case u'E':
    return QStringLiteral("entity");
  case u'B':
    return QStringLiteral("brush");
  case u'P':
    return QStringLiteral("patch");
  default:
    return QStringLiteral("node");
  }
}

mcp::ToolError notSelectable(
  const McpDocumentState& doc, const mdl::Node& node, const QString& reason)
{
  const auto id = doc.nodes.idOf(node);
  const auto hint = reason == u"in_closed_group"
                      ? QStringLiteral("pass the group id G<n> or call open_group")
                    : reason == u"group_open"
                      ? QStringLiteral("pass the nodes inside the open group instead")
                      : QStringLiteral("ask the user to show or unlock the node");
  return toolError(
    "node_not_selectable",
    QString{"%1 cannot be edited because it is %2"}.arg(
      id,
      reason == u"hidden"            ? QStringLiteral("hidden")
      : reason == u"locked"          ? QStringLiteral("locked")
      : reason == u"in_closed_group" ? QStringLiteral("inside a closed group")
      : reason == u"group_open"      ? QStringLiteral("an open group")
                                     : QStringLiteral("not selectable")),
    hint,
    {{"id", id}, {"reason", reason}});
}

/** Returns the reason why the node is not selectable, or nullopt if it is. */
std::optional<QString> notSelectableReason(
  const mdl::Map& map, const mdl::Node& node, const bool allowBrushEntity)
{
  const auto& editorContext = map.editorContext();
  if (!editorContext.visible(node))
  {
    return QStringLiteral("hidden");
  }
  if (!editorContext.editable(node))
  {
    return QStringLiteral("locked");
  }

  const auto* object = dynamic_cast<const mdl::Object*>(&node);
  if (object && !editorContext.inOpenGroup(*object))
  {
    return QStringLiteral("in_closed_group");
  }

  if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
  {
    if (groupNode->opened())
    {
      return QStringLiteral("group_open");
    }
  }

  const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
  if (allowBrushEntity && entityNode && entityNode->hasChildren())
  {
    return std::nullopt;
  }

  if (!editorContext.selectable(node))
  {
    return QStringLiteral("not_selectable");
  }
  return std::nullopt;
}

/** Caches whether link ids are shared by more than one group during one call. */
class LinkedGroupCache
{
public:
  explicit LinkedGroupCache(mdl::Map& map)
    : m_map{map}
  {
  }

  /** Returns the outermost linked group containing the node (or the node itself). */
  const mdl::GroupNode* linkedGroupOf(const mdl::Node& node)
  {
    const mdl::GroupNode* result = nullptr;
    for (const auto* current = &node; current; current = current->parent())
    {
      if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(current))
      {
        if (isLinked(*groupNode))
        {
          result = groupNode;
        }
      }
    }
    return result;
  }

private:
  bool isLinked(const mdl::GroupNode& groupNode)
  {
    const auto& linkId = groupNode.linkId();
    if (const auto it = m_cache.find(linkId); it != m_cache.end())
    {
      return it->second;
    }
    const auto linked = isLinkedGroup(m_map, groupNode);
    m_cache.emplace(linkId, linked);
    return linked;
  }

  mdl::Map& m_map;
  std::unordered_map<std::string, bool> m_cache;
};

mcp::ToolError linkedGroupError(
  const McpDocumentState& doc, const mdl::Node& node, const mdl::GroupNode& group)
{
  const auto id = doc.nodes.idOf(node);
  const auto groupId = doc.nodes.idOf(group);
  return toolError(
    "not_supported",
    QString{"%1 is inside the linked group %2; editing linked groups is not supported"}
      .arg(id, groupId),
    "edit nodes outside of linked groups",
    {{"id", id}, {"linked_group", groupId}});
}

McpResult<std::vector<mdl::Node*>> expandIds(
  const McpDocumentState& doc, const QJsonArray& ids)
{
  auto result = std::vector<mdl::Node*>{};
  auto seen = std::unordered_set<const mdl::Node*>{};

  const auto add = [&](mdl::Node* node) {
    if (seen.insert(node).second)
    {
      result.push_back(node);
    }
  };

  for (const auto& value : ids)
  {
    const auto id = value.toString();
    if (id == u"@selection")
    {
      const auto& selection = doc.map.selection();
      if (selection.nodes.empty())
      {
        return toolError(
          "invalid_argument",
          selection.hasBrushFaces()
            ? QStringLiteral("nothing is selected (only brush faces are selected)")
            : QStringLiteral("nothing is selected"),
          "pass explicit ids, or ask the user to select something");
      }
      for (auto* node : selection.nodes)
      {
        add(node);
      }
    }
    else
    {
      auto resolved = resolveNode(doc, id);
      if (resolved.is_error())
      {
        return errorOf(resolved);
      }
      add(resolved.value());
    }
  }

  return result;
}

} // namespace

mcp::ToolError toolError(QString code, QString message, QString hint, QJsonObject details)
{
  return {
    .code = std::move(code),
    .message = std::move(message),
    .hint = std::move(hint),
    .details = std::move(details),
  };
}

McpResult<mdl::Node*> resolveNode(const McpDocumentState& doc, const QString& id)
{
  using Status = McpNodeRegistry::LookupStatus;

  if (id == u"@selection")
  {
    return toolError(
      "invalid_argument",
      "\"@selection\" is only allowed in id lists",
      "pass a single node id",
      {{"id", id}});
  }

  const auto lookup = doc.nodes.lookup(id);
  switch (lookup.status)
  {
  case Status::Live:
    return lookup.node;
  case Status::Malformed:
    return toolError(
      "invalid_argument",
      QString{"malformed node id '%1'"}.arg(id.left(64)),
      "ids look like world, default_layer, L1, G2, E3, B4 or P5",
      {{"id", id.left(64)}});
  case Status::Unknown:
    return toolError(
      "unknown_node",
      QString{"%1 was never issued for this document"}.arg(id),
      StaleHint,
      {{"id", id}});
  case Status::Stale: {
    auto details = QJsonObject{{"id", id}};
    if (!lookup.removedBy.isEmpty())
    {
      details["removed_by"] = lookup.removedBy;
    }
    return toolError(
      "stale_node",
      lookup.removedBy.isEmpty()
        ? QString{"%1 no longer exists"}.arg(id)
        : QString{"%1 no longer exists; it was removed by %2"}.arg(id, lookup.removedBy),
      StaleHint,
      std::move(details));
  }
  case Status::WrongType: {
    const auto actualId = doc.nodes.idOf(*lookup.node);
    const auto actual = nodeTypeName(*lookup.node);
    const auto expected = typeNameForLetter(id[0]);
    return toolError(
      "wrong_node_type",
      QString{"%1 is not a %2; %3 is %4 %5"}.arg(
        id, expected, id.mid(1), actual, actualId),
      QString{"use %1"}.arg(actualId),
      {{"id", id}, {"actual", actual}, {"expected", expected}, {"actual_id", actualId}});
  }
  }

  return toolError("unknown_node", QString{"%1 is unknown"}.arg(id), StaleHint);
}

McpResult<std::vector<mdl::Node*>> resolveNodes(
  const McpDocumentState& doc, const QJsonArray& ids, const size_t maxCount)
{
  auto nodes = expandIds(doc, ids);
  if (nodes.is_success() && nodes.value().size() > maxCount)
  {
    return toolError(
      "invalid_argument",
      QString{"the ids name %1 nodes, but at most %2 are allowed"}
        .arg(nodes.value().size())
        .arg(maxCount),
      "pass fewer ids, e.g. a page of find_nodes results instead of \"@selection\"",
      {{"pointer", "/ids"},
       {"count", qint64(nodes.value().size())},
       {"max", qint64(maxCount)}});
  }
  return nodes;
}

McpResult<std::vector<mdl::Node*>> resolveTargets(
  const McpDocumentState& doc, const QJsonArray& ids, const McpTargetPurpose purpose)
{
  auto expanded = expandIds(doc, ids);
  if (expanded.is_error())
  {
    return expanded;
  }
  const auto nodes = std::move(expanded).value();

  auto& map = doc.map;
  for (const auto* node : nodes)
  {
    const auto id = doc.nodes.idOf(*node);
    if (node == &map.worldNode())
    {
      return toolError(
        "wrong_node_type",
        "world cannot be a target",
        "pass the ids of the nodes inside it, e.g. from find_nodes",
        {{"id", id}, {"actual", "world"}, {"expected", "group, entity, brush or patch"}});
    }
    if (dynamic_cast<const mdl::LayerNode*>(node))
    {
      if (purpose == McpTargetPurpose::Delete)
      {
        return toolError(
          "not_supported",
          QString{"%1 is a layer; deleting layers is not supported"}.arg(id),
          "delete the layer's children instead (find_nodes with under)",
          {{"id", id}});
      }
      return toolError(
        "wrong_node_type",
        QString{"%1 is a layer and cannot be a target"}.arg(id),
        "use the layer's children via find_nodes with under",
        {{"id", id}, {"actual", "layer"}, {"expected", "group, entity, brush or patch"}});
    }
  }

  // drop nodes that are descendants of other listed nodes
  const auto listed = std::unordered_set<const mdl::Node*>{nodes.begin(), nodes.end()};
  const auto hasListedAncestor = [&](const mdl::Node& node) {
    for (const auto* parent = node.parent(); parent; parent = parent->parent())
    {
      if (listed.contains(parent))
      {
        return true;
      }
    }
    return false;
  };

  auto targets = std::vector<mdl::Node*>{};
  auto seen = std::unordered_set<const mdl::Node*>{};
  const auto add = [&](mdl::Node* node) {
    if (seen.insert(node).second)
    {
      targets.push_back(node);
    }
  };

  for (auto* node : nodes)
  {
    if (hasListedAncestor(*node))
    {
      continue;
    }

    auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
    if (purpose == McpTargetPurpose::Edit && entityNode && entityNode->hasChildren())
    {
      for (auto* child : entityNode->children())
      {
        add(child);
      }
    }
    else
    {
      add(node);
    }
  }

  auto linkedGroups = LinkedGroupCache{map};
  for (const auto* node : targets)
  {
    if (
      const auto reason =
        notSelectableReason(map, *node, purpose == McpTargetPurpose::Delete))
    {
      return notSelectable(doc, *node, *reason);
    }
    if (const auto* group = linkedGroups.linkedGroupOf(*node))
    {
      return linkedGroupError(doc, *node, *group);
    }
  }

  return targets;
}

McpResult<mdl::Node*> resolveParent(
  const McpDocumentState& doc, const std::optional<QString>& id)
{
  auto& map = doc.map;
  if (!id)
  {
    // the default is the open group (or the current layer); an open linked group must
    // be rejected just like an explicitly passed one
    auto& parent = mdl::parentForNodes(map);
    auto linkedGroups = LinkedGroupCache{map};
    if (const auto* linkedGroup = linkedGroups.linkedGroupOf(parent))
    {
      return linkedGroupError(doc, parent, *linkedGroup);
    }
    return &parent;
  }

  auto resolved = resolveNode(doc, *id);
  if (resolved.is_error())
  {
    return resolved;
  }

  auto* node = resolved.value();
  const auto invalidParent = [&](const QString& reason) {
    return toolError(
      "invalid_parent",
      QString{"%1 cannot be a parent: %2"}.arg(*id, reason),
      "pass a layer, a group or a brush entity, or omit parent",
      {{"id", *id}, {"type", nodeTypeName(*node)}});
  };

  return node->accept(kdl::overload(
    [&](mdl::WorldNode& worldNode) -> McpResult<mdl::Node*> {
      return worldNode.defaultLayer();
    },
    [&](mdl::LayerNode& layerNode) -> McpResult<mdl::Node*> { return &layerNode; },
    [&](mdl::GroupNode& groupNode) -> McpResult<mdl::Node*> {
      if (isLinkedGroup(map, groupNode))
      {
        return toolError(
          "not_supported",
          QString{"%1 is a linked group; editing linked groups is not supported"}.arg(
            *id),
          "pass a layer or an unlinked group",
          {{"id", *id}});
      }
      auto linkedGroups = LinkedGroupCache{map};
      if (const auto* linkedGroup = linkedGroups.linkedGroupOf(groupNode))
      {
        return linkedGroupError(doc, groupNode, *linkedGroup);
      }
      return &groupNode;
    },
    [&](mdl::EntityNode& entityNode) -> McpResult<mdl::Node*> {
      if (!entityNode.hasChildren())
      {
        return invalidParent("point entities cannot have children");
      }
      auto linkedGroups = LinkedGroupCache{map};
      if (const auto* linkedGroup = linkedGroups.linkedGroupOf(entityNode))
      {
        return linkedGroupError(doc, entityNode, *linkedGroup);
      }
      return &entityNode;
    },
    [&](mdl::BrushNode&) -> McpResult<mdl::Node*> {
      return invalidParent("brushes cannot have children");
    },
    [&](mdl::PatchNode&) -> McpResult<mdl::Node*> {
      return invalidParent("patches cannot have children");
    }));
}

McpResult<std::vector<mdl::BrushFaceHandle>> resolveFaces(
  const McpDocumentState& doc, const QJsonArray& faceRefs)
{
  auto result = std::vector<mdl::BrushFaceHandle>{};
  const auto& editorContext = doc.map.editorContext();

  for (qsizetype i = 0; i < faceRefs.size(); ++i)
  {
    const auto pointer = QString{"/faces/%1"}.arg(i);
    const auto ref = faceRefs[i].toObject();
    const auto brushId = ref.value("brush").toString();

    const auto hasIndex = ref.contains("index");
    const auto hasSide = ref.contains("side");
    const auto hasNormal = ref.contains("normal");
    if (int(hasIndex) + int(hasSide) + int(hasNormal) != 1)
    {
      return toolError(
        "invalid_argument",
        QString{"%1: pass exactly one of index, side or normal"}.arg(pointer),
        "e.g. {\"brush\":\"B1\",\"side\":\"+z\"}",
        {{"pointer", pointer}});
    }

    auto resolved = resolveAs<mdl::BrushNode>(doc, brushId, "brush");
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    auto* brushNode = resolved.value();
    const auto& brush = brushNode->brush();

    auto faceIndex = std::optional<size_t>{};
    if (hasIndex)
    {
      const auto index = ref.value("index").toDouble();
      if (index >= 0.0 && index < double(brush.faceCount()))
      {
        faceIndex = size_t(index);
      }
    }
    else if (hasSide)
    {
      if (const auto normal = sideNormal(ref.value("side").toString()))
      {
        faceIndex = brush.findFace(*normal);
      }
    }
    else
    {
      auto normal = vec3(ref.value("normal"), pointer + "/normal");
      if (normal.is_error())
      {
        return errorOf(normal);
      }
      const auto length = vm::length(normal.value());
      if (length < 1e-9)
      {
        return toolError(
          "invalid_argument",
          QString{"%1/normal must not be zero"}.arg(pointer),
          {},
          {{"pointer", pointer + "/normal"}});
      }
      faceIndex = brush.findFace(normal.value() / length);
    }

    if (!faceIndex)
    {
      auto faces = QJsonArray{};
      for (size_t f = 0; f < brush.faceCount(); ++f)
      {
        const auto& face = brush.face(f);
        auto entry = QJsonObject{{"index", qint64(f)}, {"normal", toJson(face.normal())}};
        if (const auto side = sideName(face.normal()))
        {
          entry["side"] = *side;
        }
        faces.append(entry);
      }
      return toolError(
        "face_not_found",
        QString{"%1 has no such face (%2)"}.arg(brushId, pointer),
        "pick one of the listed faces",
        {{"brush", brushId}, {"pointer", pointer}, {"faces", faces}});
    }

    const auto& face = brush.face(*faceIndex);
    if (!editorContext.selectable(*brushNode, face))
    {
      return notSelectable(
        doc,
        *brushNode,
        !editorContext.visible(*brushNode, face) ? QStringLiteral("hidden")
                                                 : QStringLiteral("locked"));
    }

    auto handle = mdl::BrushFaceHandle{*brushNode, *faceIndex};
    if (std::ranges::find(result, handle) == result.end())
    {
      result.push_back(std::move(handle));
    }
  }

  return result;
}

McpResult<vm::vec3d> vec3(const QJsonValue& json, const QString& pointer)
{
  const auto array = json.toArray();
  if (!json.isArray() || array.size() != 3)
  {
    return toolError(
      "invalid_argument",
      QString{"%1 must be an array of 3 numbers"}.arg(pointer),
      {},
      {{"pointer", pointer}});
  }

  auto result = vm::vec3d{};
  for (qsizetype i = 0; i < 3; ++i)
  {
    const auto value = array[i];
    if (!value.isDouble())
    {
      return toolError(
        "invalid_argument",
        QString{"%1/%2 must be a number"}.arg(pointer).arg(i),
        {},
        {{"pointer", QString{"%1/%2"}.arg(pointer).arg(i)}});
    }
    const auto d = value.toDouble();
    if (!std::isfinite(d) || std::abs(d) > MaxCoordinate)
    {
      return toolError(
        "invalid_argument",
        QString{"%1/%2 must be between -%3 and %3"}.arg(pointer).arg(i).arg(
          MaxCoordinate, 0, 'f', 0),
        {},
        {{"pointer", QString{"%1/%2"}.arg(pointer).arg(i)}});
    }
    result[size_t(i)] = d;
  }
  return result;
}

McpResult<vm::bbox3d> bounds(const QJsonValue& json, const QString& pointer)
{
  const auto object = json.toObject();
  auto min = vec3(object.value("min"), pointer + "/min");
  if (min.is_error())
  {
    return errorOf(min);
  }
  auto max = vec3(object.value("max"), pointer + "/max");
  if (max.is_error())
  {
    return errorOf(max);
  }

  static const auto axes = std::array<const char*, 3>{"x", "y", "z"};
  for (size_t i = 0; i < 3; ++i)
  {
    if (min.value()[i] > max.value()[i])
    {
      return toolError(
        "invalid_argument",
        QString{"%1: min.%2 must not be greater than max.%2"}.arg(pointer, axes[i]),
        {},
        {{"pointer", pointer}, {"axis", axes[i]}});
    }
  }
  return vm::bbox3d{min.value(), max.value()};
}

McpResult<vm::bbox3d> boxInWorld(
  const mdl::Map& map, const vm::vec3d& min, const vm::vec3d& max)
{
  static const auto axes = std::array<const char*, 3>{"x", "y", "z"};
  for (size_t i = 0; i < 3; ++i)
  {
    if (!(max[i] - min[i] >= 1.0 / 1024.0))
    {
      return toolError(
        "invalid_argument",
        QString{"the box is empty along %1: max.%1 must exceed min.%1"}.arg(axes[i]),
        "pass min < max on every axis",
        {{"axis", axes[i]}});
    }
  }

  const auto box = vm::bbox3d{min, max};
  if (!map.worldBounds().contains(box))
  {
    return toolError(
      "out_of_world_bounds",
      "the box is not inside the world bounds",
      "keep coordinates inside world_bounds (see get_map_info)",
      {{"world_bounds", toJson(map.worldBounds())}});
  }
  return box;
}

McpResult<void> requireNoActiveTransaction(const mdl::Map& map)
{
  if (map.commandProcessor().isTransactionActive())
  {
    return toolError(
      "editor_busy",
      "the editor is in the middle of an operation",
      "retry in a moment",
      {{"reason", "transaction_active"}});
  }
  return kdl::void_success;
}

bool canUndo(const mdl::Map& map)
{
  return !map.commandProcessor().isTransactionActive()
         && map.undoCommandName() != nullptr;
}

bool canRedo(const mdl::Map& map)
{
  return !map.commandProcessor().isTransactionActive()
         && map.redoCommandName() != nullptr;
}

McpResult<void> checkMapText(const QString& text, const McpMapText kind)
{
  const auto what = kind == McpMapText::Key ? QStringLiteral("property key")
                                            : QStringLiteral("property value");
  if (kind == McpMapText::Key && text.isEmpty())
  {
    return toolError("invalid_argument", "property keys must not be empty");
  }
  if (text.contains(u'"') || text.contains(u'\n') || text.contains(u'\r'))
  {
    return toolError(
      "invalid_argument",
      QString{"%1 '%2' must not contain double quotes or line breaks"}.arg(
        what, text.left(64)),
      "remove the character; the map format cannot store it");
  }
  if (text.toUtf8().size() > 32 * 1024)
  {
    return toolError(
      "invalid_argument",
      QString{"%1 '%2...' is longer than 32 KiB"}.arg(what, text.left(32)));
  }
  return kdl::void_success;
}

bool isLinkedGroup(mdl::Map& map, const mdl::GroupNode& groupNode)
{
  return mdl::collectGroupsWithLinkId({&map.worldNode()}, groupNode.linkId()).size() > 1;
}

McpResult<void> requireNotLinked(const McpDocumentState& doc, const mdl::Node& node)
{
  auto linkedGroups = LinkedGroupCache{doc.map};
  if (const auto* linkedGroup = linkedGroups.linkedGroupOf(node))
  {
    return linkedGroupError(doc, node, *linkedGroup);
  }
  return kdl::void_success;
}

namespace
{

QString formatPropertyNumber(const double value)
{
  if (value == 0.0)
  {
    return QStringLiteral("0");
  }
  if (std::trunc(value) == value && std::abs(value) < 9007199254740992.0)
  {
    return QString::number(qint64(value));
  }
  return QString::number(value, 'g', QLocale::FloatingPointShortest);
}

} // namespace

McpResult<QString> propValueText(const QJsonValue& value, const QString& pointer)
{
  auto text = std::optional<QString>{};
  if (value.isString())
  {
    text = value.toString();
  }
  else if (value.isBool())
  {
    text = value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
  }
  else if (value.isDouble() && std::isfinite(value.toDouble()))
  {
    text = formatPropertyNumber(value.toDouble());
  }
  else if (value.isArray())
  {
    const auto array = value.toArray();
    if (array.size() == 3 && std::ranges::all_of(array, [](const auto& v) {
          return v.isDouble() && std::isfinite(v.toDouble());
        }))
    {
      text = QString{"%1 %2 %3"}.arg(
        formatPropertyNumber(array[0].toDouble()),
        formatPropertyNumber(array[1].toDouble()),
        formatPropertyNumber(array[2].toDouble()));
    }
  }

  if (!text)
  {
    return toolError(
      "invalid_argument",
      QString{"%1 must be a string, a number, a boolean or an array of 3 numbers"}.arg(
        pointer),
      "e.g. \"300\", 300, true or [0, 0, 90]",
      {{"pointer", pointer}});
  }

  if (auto check = checkMapText(*text, McpMapText::Value); check.is_error())
  {
    auto error = errorOf(check);
    error.details["pointer"] = pointer;
    return error;
  }
  return *text;
}

McpResult<void> checkEntityPropertyKeyNotReserved(
  const QString& key, const QString& pointer)
{
  if (key.startsWith(QString::fromStdString(mdl::EntityPropertyKeys::TbPrefix)))
  {
    return toolError(
      "invalid_argument",
      QString{"%1: '%2' is reserved; keys starting with _tb_ are managed by TrenchBroom"}
        .arg(pointer, key),
      "use group_nodes, create_layer or move_to_layer for groups and layers; soft map "
      "bounds, mods and entity definitions can only be changed in the TrenchBroom UI",
      {{"pointer", pointer}, {"key", key}});
  }
  return kdl::void_success;
}

McpResult<void> checkEntityPropertyValue(
  const QString& key, const QString& value, const QString& pointer)
{
  if (key != u"origin" && key != u"angle" && key != u"angles" && key != u"mangle")
  {
    return kdl::void_success;
  }

  // TrenchBroom parses these values with std::from_chars, which accepts inf and nan
  static const auto separators = QRegularExpression{R"([\s()]+)"};
  for (const auto& token : value.split(separators, Qt::SkipEmptyParts))
  {
    const auto text = token.toStdString();
    char* end = nullptr;
    const auto number = std::strtod(text.c_str(), &end);
    const auto isNumber = !text.empty() && end == text.c_str() + text.size();
    if (
      isNumber
      && (!std::isfinite(number) || std::abs(number) > McpMaxEntityPropertyNumber))
    {
      return toolError(
        "invalid_argument",
        QString{"%1: every number in %2 must be finite and at most %3 in magnitude"}
          .arg(pointer, key)
          .arg(McpMaxEntityPropertyNumber),
        "e.g. \"0 90 0\" for angles or \"0 0 24\" for origin",
        {{"pointer", pointer}});
    }
  }
  return kdl::void_success;
}

namespace
{

template <typename M>
bool isFiniteMatrix(const M& matrix)
{
  for (size_t c = 0; c < M::cols; ++c)
  {
    for (size_t r = 0; r < M::rows; ++r)
    {
      if (!std::isfinite(matrix[c][r]))
      {
        return false;
      }
    }
  }
  return true;
}

bool hasBoundedCoordinates(const vm::bbox3d& box)
{
  for (size_t i = 0; i < 3; ++i)
  {
    for (const auto value : {box.min[i], box.max[i]})
    {
      if (!std::isfinite(value) || std::abs(value) > McpMaxEntityPropertyNumber)
      {
        return false;
      }
    }
  }
  return true;
}

} // namespace

McpResult<void> checkTransformableNodes(
  const McpNodeRegistry& registry, const std::vector<const mdl::Node*>& nodes)
{
  const auto describe = [&](const mdl::Node& node) {
    const auto id = registry.idOf(node);
    if (!id.isEmpty())
    {
      return id;
    }
    if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node))
    {
      return QString{"an entity '%1'"}.arg(
        QString::fromStdString(entityNode->entity().classname()));
    }
    return QString{"a %1"}.arg(nodeTypeName(node));
  };

  auto result = McpResult<void>{kdl::void_success};
  const auto check = [&](const auto& self, const mdl::Node& node) -> void {
    if (result.is_error())
    {
      return;
    }
    if (
      !dynamic_cast<const mdl::LayerNode*>(&node)
      && !dynamic_cast<const mdl::WorldNode*>(&node)
      && (!hasBoundedCoordinates(node.logicalBounds()) || !hasBoundedCoordinates(node.physicalBounds())))
    {
      const auto what = describe(node);
      result = toolError(
        "out_of_world_bounds",
        QString{"%1 has coordinates that are not finite or larger than %2"}.arg(what).arg(
          McpMaxEntityPropertyNumber),
        "keep nodes inside the world bounds; fix entity origins with "
        "set_entity_properties",
        {{"node", what}});
      return;
    }
    if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
        entityNode && !isFiniteMatrix(entityNode->entity().rotation()))
    {
      const auto what = describe(node);
      result = toolError(
        "invalid_argument",
        QString{"%1 has an angle, angles or mangle property that is not finite"}.arg(
          what),
        "fix the rotation property with set_entity_properties",
        {{"node", what}});
      return;
    }
    if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node);
        groupNode && !isFiniteMatrix(groupNode->group().transformation()))
    {
      const auto what = describe(node);
      result = toolError(
        "invalid_argument",
        QString{"%1 has a transformation that is not finite"}.arg(what),
        {},
        {{"node", what}});
      return;
    }
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };

  for (const auto* node : nodes)
  {
    // brush entities are transformed with their brushes
    if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node->parent()))
    {
      check(check, *entityNode);
    }
    else
    {
      check(check, *node);
    }
  }
  return result;
}

McpResult<void> checkMaterialName(const QString& name, const QString& pointer)
{
  const auto invalid = [&](const QString& reason) {
    return toolError(
      "invalid_argument",
      QString{"%1: invalid material name '%2': %3"}.arg(pointer, name.left(64), reason),
      "material names look like base/wall1 or e1u1/floor; see list_materials",
      {{"pointer", pointer}});
  };

  if (name.isEmpty())
  {
    return invalid("must not be empty");
  }
  if (name.size() > 256)
  {
    return invalid("must be at most 256 characters long");
  }
  for (const auto c : name)
  {
    if (
      c.isSpace() || c.category() == QChar::Other_Control || c == u'"' || c == u'('
      || c == u')')
    {
      return invalid("must not contain whitespace, quotes or parentheses");
    }
  }
  return kdl::void_success;
}

size_t sizeArgument(
  const QJsonObject& args, const QString& key, const size_t defaultValue)
{
  const auto value = args.value(key);
  if (!value.isDouble())
  {
    return defaultValue;
  }
  const auto d = value.toDouble();
  return std::isfinite(d) ? size_t(std::clamp(d, 0.0, 1e9)) : defaultValue;
}

bool globMatch(const QStringView pattern, const QStringView text)
{
  // iterative wildcard matching with backtracking to the last '*', O(n*m) worst case
  auto p = qsizetype{0};
  auto t = qsizetype{0};
  auto starP = qsizetype{-1};
  auto starT = qsizetype{0};

  const auto equal = [](const QChar a, const QChar b) {
    return a == b || a.toCaseFolded() == b.toCaseFolded();
  };

  while (t < text.size())
  {
    if (
      p < pattern.size()
      && (pattern[p] == u'?' || (pattern[p] != u'*' && equal(pattern[p], text[t]))))
    {
      ++p;
      ++t;
    }
    else if (p < pattern.size() && pattern[p] == u'*')
    {
      starP = p++;
      starT = t;
    }
    else if (starP >= 0)
    {
      p = starP + 1;
      t = ++starT;
    }
    else
    {
      return false;
    }
  }

  while (p < pattern.size() && pattern[p] == u'*')
  {
    ++p;
  }
  return p == pattern.size();
}

} // namespace tb::ui
