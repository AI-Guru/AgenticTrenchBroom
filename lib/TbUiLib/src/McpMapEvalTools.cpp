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

#include <QJsonArray>
#include <QJsonObject>

#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapHeader.h"
#include "mdl/ModelUtils.h"
#include "mdl/NodeWriter.h"
#include "mdl/PatchNode.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"
#include "ui/McpCheckpoints.h"
#include "ui/McpJson.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

#include "kd/overload.h"

#include "vm/bbox.h"
#include "vm/constants.h"
#include "vm/plane.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace tb::ui
{
namespace
{

constexpr auto MaxIdList = size_t(50);
constexpr auto DefaultMaxChanges = 200;
constexpr auto MaxChanges = 5000;
constexpr auto DefaultMaxBytes = 262144;
constexpr auto MinMaxBytes = 1024;
constexpr auto MaxMaxBytes = 8388608;
constexpr auto CheckpointNamePattern = "^[A-Za-z0-9_.-]{1,64}$";
constexpr auto CurrentCheckpoint = "current";

const auto ReadOnly =
  mcp::ToolAnnotations{.readOnly = true, .destructive = false, .idempotent = true};

/** Returns the given page of the given items and adds total, truncated and next_offset.
 */
QJsonArray page(const QJsonArray& items, const QJsonObject& args, QJsonObject& result)
{
  const auto total = size_t(items.size());
  const auto offset = std::min(sizeArgument(args, "offset", 0), total);
  const auto limit = sizeArgument(args, "limit", schema::DefaultLimit);
  const auto end = offset + std::min(limit, total - offset);

  auto pageItems = QJsonArray{};
  for (auto i = offset; i < end; ++i)
  {
    pageItems.append(items[qsizetype(i)]);
  }

  result["total"] = qint64(total);
  result["truncated"] = end < total;
  result["next_offset"] = end < total ? QJsonValue{qint64(end)} : QJsonValue{};
  return pageItems;
}

/** Visits the node and its descendants in DFS pre-order. */
void visitPreOrder(mdl::Node& node, const std::function<void(mdl::Node&)>& visitor)
{
  visitor(node);
  for (auto* child : node.children())
  {
    visitPreOrder(*child, visitor);
  }
}

bool hasIntegerVertices(const mdl::Brush& brush)
{
  return std::ranges::all_of(brush.vertexPositions(), [](const auto& v) {
    for (size_t i = 0; i < 3; ++i)
    {
      if (std::abs(v[i] - std::round(v[i])) > 1e-6)
      {
        return false;
      }
    }
    return true;
  });
}

bool isValidBrush(const mdl::Map& map, const mdl::Brush& brush)
{
  return brush.closed() && brush.fullySpecified()
         && map.worldBounds().contains(brush.bounds());
}

bool isWorldBrush(const mdl::BrushNode& brushNode)
{
  return dynamic_cast<const mdl::WorldNode*>(brushNode.entity()) != nullptr;
}

/** Adds "<key>" (at most MaxIdList ids) and "<key>_count" to the given object. */
void addIdList(QJsonObject& result, const QString& key, const QStringList& ids)
{
  auto array = QJsonArray{};
  for (qsizetype i = 0; i < ids.size() && size_t(i) < MaxIdList; ++i)
  {
    array.append(ids[i]);
  }
  result[key] = array;
  result[key + "_count"] = qint64(ids.size());
}

QJsonArray sortedStrings(const std::set<QString>& strings)
{
  auto result = QJsonArray{};
  for (const auto& str : strings)
  {
    result.append(str);
  }
  return result;
}

QJsonObject countsObject(const std::map<QString, qint64>& counts)
{
  auto result = QJsonObject{};
  for (const auto& [key, count] : counts)
  {
    result[key] = count;
  }
  return result;
}

/**
 * Returns the issues of the given node that are not hidden, unless includeHidden.
 *
 * Node::issues caches the issues found by the validators it is first called with, so it
 * must always be called with all registered validators. Use the types filter to report
 * only some issue types.
 */
std::vector<const mdl::Issue*> nodeIssues(
  mdl::Node& node,
  const std::vector<const mdl::Validator*>& allValidators,
  const bool includeHidden,
  const std::unordered_set<mdl::IssueType>* types = nullptr)
{
  auto result = std::vector<const mdl::Issue*>{};
  for (const auto* issue : node.issues(allValidators))
  {
    if ((includeHidden || !issue->hidden()) && (!types || types->contains(issue->type())))
    {
      result.push_back(issue);
    }
  }
  return result;
}

// validate_map

/** Returns the issue types selected by the types argument, or nullopt for all types. */
McpResult<std::optional<std::unordered_set<mdl::IssueType>>> selectIssueTypes(
  const std::vector<const mdl::Validator*>& validators, const QJsonObject& args)
{
  if (!args.contains("types"))
  {
    return std::optional<std::unordered_set<mdl::IssueType>>{};
  }

  auto allowed = QJsonArray{};
  for (const auto* validator : validators)
  {
    allowed.append(QString::fromStdString(validator->description()));
  }

  auto selected = std::unordered_set<mdl::IssueType>{};
  const auto types = args.value("types").toArray();
  for (qsizetype i = 0; i < types.size(); ++i)
  {
    const auto type = types[i].toString();
    const auto it = std::ranges::find_if(validators, [&](const auto* validator) {
      return QString::fromStdString(validator->description())
               .compare(type, Qt::CaseInsensitive)
             == 0;
    });
    if (it == validators.end())
    {
      return toolError(
        "invalid_argument",
        QString{"unknown issue type '%1'"}.arg(type),
        "pass validator descriptions as listed in the validators field of validate_map",
        {{"pointer", QString{"/types/%1"}.arg(i)}, {"allowed", allowed}});
    }
    selected.insert((*it)->type());
  }
  return std::optional{std::move(selected)};
}

/**
 * Returns the nodes passed as under, or nullopt if under was omitted, so that an empty
 * set could never be mistaken for the whole map.
 */
McpResult<std::optional<std::unordered_set<const mdl::Node*>>> underNodes(
  const McpDocumentState& doc, const QJsonObject& args)
{
  using Nodes = std::unordered_set<const mdl::Node*>;
  if (!args.contains("under"))
  {
    return std::optional<Nodes>{};
  }

  auto resolved =
    resolveNodes(doc, args.value("under").toArray(), size_t(schema::MaxNodeIds));
  if (resolved.is_error())
  {
    auto error = errorOf(resolved);
    if (error.details.value("pointer") == QJsonValue{"/ids"})
    {
      error.details["pointer"] = "/under";
    }
    return error;
  }
  const auto& nodes = resolved.value();
  return std::optional{Nodes{nodes.begin(), nodes.end()}};
}

bool isInSubtrees(
  const mdl::Node& node, const std::unordered_set<const mdl::Node*>& roots)
{
  for (const auto* current = &node; current; current = current->parent())
  {
    if (roots.contains(current))
    {
      return true;
    }
  }
  return false;
}

McpResult<QJsonObject> validateMap(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;
  const auto allValidators = map.worldNode().registeredValidators();

  auto typesResult = selectIssueTypes(allValidators, args);
  if (typesResult.is_error())
  {
    return errorOf(typesResult);
  }
  const auto types = std::move(typesResult).value();

  auto underResult = underNodes(doc, args);
  if (underResult.is_error())
  {
    return errorOf(underResult);
  }
  const auto under = std::move(underResult).value();

  const auto includeHidden = args.value("include_hidden").toBool(false);

  auto validatorIndex = std::unordered_map<mdl::IssueType, size_t>{};
  auto descriptions = std::unordered_map<mdl::IssueType, QString>{};
  auto validatorsJson = QJsonArray{};
  for (size_t i = 0; i < allValidators.size(); ++i)
  {
    const auto* validator = allValidators[i];
    const auto description = QString::fromStdString(validator->description());
    validatorIndex.emplace(validator->type(), i);
    descriptions.emplace(validator->type(), description);
    validatorsJson.append(
      QJsonObject{{"type_bit", qint64(validator->type())}, {"description", description}});
  }

  struct Item
  {
    size_t nodeIndex;
    size_t validatorIndex;
    QString description;
    QJsonObject json;
  };

  auto items = std::vector<Item>{};
  auto nodeIndex = size_t(0);
  visitPreOrder(map.worldNode(), [&](mdl::Node& node) {
    ++nodeIndex;
    if (under && !isInSubtrees(node, *under))
    {
      return;
    }

    for (const auto* issue :
         nodeIssues(node, allValidators, includeHidden, types ? &*types : nullptr))
    {
      const auto description = QString::fromStdString(issue->description());
      const auto typeIt = descriptions.find(issue->type());
      auto json = QJsonObject{
        {"node", doc.nodes.idOf(node)},
        {"type", typeIt != descriptions.end() ? typeIt->second : QString{}},
        {"description", description},
        // nodes that were never saved or loaded have no line
        {"line",
         issue->lineNumber() > 0 ? QJsonValue{qint64(issue->lineNumber())}
                                 : QJsonValue{}},
        {"hidden", issue->hidden()},
      };
      if (const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(issue))
      {
        json["face"] = qint64(faceIssue->faceIndex());
      }
      if (
        const auto* propertyIssue = dynamic_cast<const mdl::EntityPropertyIssue*>(issue))
      {
        json["property"] = toUnicode(map, propertyIssue->propertyKey());
      }

      const auto indexIt = validatorIndex.find(issue->type());
      items.push_back(Item{
        .nodeIndex = nodeIndex,
        .validatorIndex =
          indexIt != validatorIndex.end() ? indexIt->second : allValidators.size(),
        .description = description,
        .json = std::move(json),
      });
    }
  });

  std::ranges::stable_sort(items, [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.nodeIndex, lhs.validatorIndex, lhs.description)
           < std::tie(rhs.nodeIndex, rhs.validatorIndex, rhs.description);
  });

  auto byType = std::map<QString, qint64>{};
  auto itemsJson = QJsonArray{};
  for (const auto& item : items)
  {
    ++byType[item.json.value("type").toString()];
    itemsJson.append(item.json);
  }

  auto result = QJsonObject{
    {"by_type", countsObject(byType)},
    {"validators", validatorsJson},
  };
  result["items"] = page(itemsJson, args, result);
  return result;
}

McpToolDef validateMapTool()
{
  auto properties = QJsonObject{
    {"under",
     schema::nodeIds(
       schema::MaxNodeIds,
       "only check these nodes and their "
       "descendants")},
    {"types",
     schema::array(
       schema::string(1, 256),
       1,
       64,
       "only report these issue types (validator descriptions, see validators)")},
    {"include_hidden", schema::boolean("also report issues the user has hidden")},
  };
  schema::addPaging(properties);

  return {
    .name = "validate_map",
    .title = "Validate Map",
    .description =
      "Runs TrenchBroom's map validators (the issue browser) and lists the issues in "
      "node order, e.g. missing classnames, non-integer vertices or empty groups. Use "
      "this after edits to verify the map is clean. Each item has the node id and the "
      "line in the map file (null for nodes that were not saved or loaded yet).",
    .inputSchema = schema::object(properties),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = validateMap,
  };
}

// get_map_stats

QJsonObject mapStats(McpDocumentState& doc)
{
  auto& map = doc.map;
  const auto& registry = doc.nodes;
  const auto validators = map.worldNode().registeredValidators();

  auto layers = qint64(0);
  auto groups = qint64(0);
  auto pointEntities = qint64(0);
  auto brushEntities = qint64(0);
  auto worldBrushes = qint64(0);
  auto entityBrushes = qint64(0);
  auto patches = qint64(0);
  auto faces = qint64(0);
  auto vertices = qint64(0);
  auto issuesTotal = qint64(0);

  auto classnames = std::map<QString, qint64>{};
  auto materials = std::map<QString, qint64>{};
  auto missingMaterials = std::set<QString>{};
  auto unknownClassnames = std::set<QString>{};
  auto invalidBrushes = QStringList{};
  auto nonIntegerBrushes = QStringList{};
  auto pointEntityNodes = std::vector<const mdl::EntityNode*>{};
  auto worldBrushNodes = std::vector<const mdl::BrushNode*>{};
  auto geometryBounds = std::optional<vm::bbox3d>{};

  const auto mergeBounds = [&](const vm::bbox3d& bounds) {
    geometryBounds = geometryBounds ? vm::merge(*geometryBounds, bounds) : bounds;
  };

  visitPreOrder(map.worldNode(), [&](mdl::Node& node) {
    issuesTotal += qint64(nodeIssues(node, validators, false).size());

    node.accept(kdl::overload(
      [](const mdl::WorldNode&) {},
      [&](const mdl::LayerNode&) { ++layers; },
      [&](const mdl::GroupNode&) { ++groups; },
      [&](const mdl::EntityNode& entityNode) {
        const auto& entity = entityNode.entity();
        const auto classname = toUnicode(map, entity.classname());
        ++classnames[classname];
        if (!entity.definition())
        {
          unknownClassnames.insert(classname);
        }
        if (entity.pointEntity())
        {
          ++pointEntities;
          pointEntityNodes.push_back(&entityNode);
        }
        else
        {
          ++brushEntities;
        }
      },
      [&](const mdl::BrushNode& brushNode) {
        const auto& brush = brushNode.brush();
        if (isWorldBrush(brushNode))
        {
          ++worldBrushes;
          worldBrushNodes.push_back(&brushNode);
        }
        else
        {
          ++entityBrushes;
        }
        faces += qint64(brush.faceCount());
        vertices += qint64(brush.vertexCount());
        for (const auto& face : brush.faces())
        {
          const auto material = toUnicode(map, face.materialName());
          ++materials[material];
          if (!face.material())
          {
            missingMaterials.insert(material);
          }
        }
        if (!isValidBrush(map, brush))
        {
          invalidBrushes.push_back(registry.idOf(brushNode));
        }
        if (!hasIntegerVertices(brush))
        {
          nonIntegerBrushes.push_back(registry.idOf(brushNode));
        }
        mergeBounds(brushNode.logicalBounds());
      },
      [&](const mdl::PatchNode& patchNode) {
        ++patches;
        ++materials[toUnicode(map, patchNode.patch().materialName())];
        mergeBounds(patchNode.logicalBounds());
      }));
  });

  auto inSolid = QStringList{};
  for (const auto* entityNode : pointEntityNodes)
  {
    const auto& origin = entityNode->entity().origin();
    if (std::ranges::any_of(worldBrushNodes, [&](const auto* brushNode) {
          return brushNode->brush().containsPoint(origin);
        }))
    {
      inSolid.push_back(registry.idOf(*entityNode));
    }
  }

  auto result = QJsonObject{
    {"counts",
     QJsonObject{
       {"layers", layers},
       {"groups", groups},
       {"entities", pointEntities + brushEntities},
       {"point_entities", pointEntities},
       {"brush_entities", brushEntities},
       {"world_brushes", worldBrushes},
       {"entity_brushes", entityBrushes},
       {"patches", patches},
       {"faces", faces},
       {"vertices", vertices},
     }},
    {"classnames", countsObject(classnames)},
    {"materials", countsObject(materials)},
    {"missing_materials", sortedStrings(missingMaterials)},
    {"unknown_classnames", sortedStrings(unknownClassnames)},
    {"geometry_bounds",
     geometryBounds ? QJsonValue{toJson(*geometryBounds)} : QJsonValue{}},
    {"issues_total", issuesTotal},
    {"content_digest", contentDigest(registry, map)},
  };
  addIdList(result, "invalid_brushes", invalidBrushes);
  addIdList(result, "non_integer_brushes", nonIntegerBrushes);
  addIdList(result, "point_entities_in_solid", inSolid);
  return result;
}

McpResult<QJsonObject> getMapStats(McpCallContext& context, const QJsonObject&)
{
  return mapStats(context.doc);
}

McpToolDef getMapStatsTool()
{
  return {
    .name = "get_map_stats",
    .title = "Get Map Stats",
    .description =
      "Returns map statistics in one pass: node counts, classname and material usage, "
      "missing materials, unknown classnames, invalid or non-integer brushes, point "
      "entities inside world brushes, the geometry bounds, the number of issues and the "
      "id free content_digest (equal digests mean equal content).",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = getMapStats,
  };
}

// checkpoints

QJsonObject checkpointNameSchema(const QString& description)
{
  auto result = schema::string(1, 64, description);
  result["pattern"] = CheckpointNamePattern;
  return result;
}

mcp::ToolError unknownCheckpoint(const McpDocumentState& doc, const QString& name)
{
  return toolError(
    "unknown_checkpoint",
    QString{"there is no checkpoint '%1'"}.arg(name),
    "call checkpoint_list for the existing checkpoints or checkpoint_create",
    {{"name", name}, {"available", QJsonArray::fromStringList(doc.checkpoints.names())}});
}

McpResult<QJsonObject> checkpointCreate(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  const auto name = args.value("name").toString();
  if (name == CurrentCheckpoint)
  {
    return toolError(
      "invalid_argument",
      "'current' is reserved for the current state of the map",
      "choose another name",
      {{"pointer", "/name"}});
  }

  doc.history.sync();
  return doc.checkpoints.add(
           takeCheckpoint(doc.nodes, doc.map, doc.history, name),
           args.value("replace").toBool(false))
         | kdl::transform(
           [](const McpCheckpoint* checkpoint) { return checkpoint->info(); });
}

McpToolDef checkpointCreateTool()
{
  return {
    .name = "checkpoint_create",
    .title = "Create Checkpoint",
    .description =
      "Records the current state of every node under a name (at most 32 checkpoints). "
      "Call it before a change and checkpoint_diff after it to verify exactly what "
      "changed. Does not change the map or its undo history.",
    .inputSchema = schema::object(
      {
        {"name", checkpointNameSchema("checkpoint name; 'current' is reserved")},
        {"replace", schema::boolean("replace an existing checkpoint with this name")},
      },
      {"name"}),
    .kind = McpToolKind::Read,
    .annotations = {.readOnly = false, .destructive = false, .idempotent = false},
    .allowedInBatch = false,
    .run = checkpointCreate,
  };
}

McpResult<QJsonObject> checkpointDiff(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;

  auto current = std::optional<McpCheckpoint>{};
  const auto resolve = [&](const QString& name) -> McpResult<const McpCheckpoint*> {
    if (name == CurrentCheckpoint)
    {
      if (!current)
      {
        current = takeCheckpoint(doc.nodes, doc.map, doc.history, CurrentCheckpoint);
      }
      return &*current;
    }
    if (const auto* checkpoint = doc.checkpoints.find(name))
    {
      return checkpoint;
    }
    return unknownCheckpoint(doc, name);
  };

  const auto fromName = args.value("from").toString();
  const auto toName = args.value("to").toString(CurrentCheckpoint);
  const auto maxChanges = sizeArgument(args, "max_changes", DefaultMaxChanges);

  return resolve(fromName) | kdl::and_then([&](const McpCheckpoint* from) {
           return resolve(toName) | kdl::transform([&](const McpCheckpoint* to) {
                    auto result = diffCheckpoints(*from, *to, maxChanges);
                    result["from"] = fromName;
                    result["to"] = toName;
                    return result;
                  });
         });
}

McpToolDef checkpointDiffTool()
{
  return {
    .name = "checkpoint_diff",
    .title = "Diff Checkpoints",
    .description =
      "Compares two checkpoints (or a checkpoint with the current map) by node id: "
      "added, removed and modified nodes with the changed fields, properties and faces. "
      "Nodes that got new ids but have equal content (e.g. after delete and undo) are "
      "listed in reidentified. content_identical compares the id free content digests.",
    .inputSchema = schema::object(
      {
        {"from", checkpointNameSchema("the checkpoint to compare from, or 'current'")},
        {"to", checkpointNameSchema("the checkpoint to compare to (default 'current')")},
        {"max_changes",
         schema::integer(
           1, MaxChanges, "maximum number of entries per list (default 200)")},
      },
      {"from"}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = checkpointDiff,
  };
}

McpResult<QJsonObject> checkpointList(McpCallContext& context, const QJsonObject&)
{
  auto checkpoints = QJsonArray{};
  for (const auto* checkpoint : context.doc.checkpoints.list())
  {
    checkpoints.append(checkpoint->info());
  }
  return QJsonObject{
    {"checkpoints", checkpoints},
    {"limit", qint64(McpCheckpointStore::MaxCheckpoints)},
  };
}

McpToolDef checkpointListTool()
{
  return {
    .name = "checkpoint_list",
    .title = "List Checkpoints",
    .description = "Lists the checkpoints of the current map, sorted by name.",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = checkpointList,
  };
}

McpResult<QJsonObject> checkpointDelete(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  const auto name = args.value("name").toString();
  if (!doc.checkpoints.remove(name))
  {
    return unknownCheckpoint(doc, name);
  }
  return QJsonObject{{"deleted", name}};
}

McpToolDef checkpointDeleteTool()
{
  return {
    .name = "checkpoint_delete",
    .title = "Delete Checkpoint",
    .description = "Deletes a checkpoint. Does not change the map.",
    .inputSchema = schema::object(
      {{"name", checkpointNameSchema("the checkpoint to delete")}}, {"name"}),
    .kind = McpToolKind::Read,
    .annotations = {.readOnly = false, .destructive = true, .idempotent = false},
    .allowedInBatch = false,
    .run = checkpointDelete,
  };
}

// export_map_text

/**
 * Resolves the nodes to export: layers are replaced by their children, brushes and
 * patches of brush entities by their entity (the writer orders entity children by
 * pointer, so exporting them without their entity would not be deterministic), and
 * nodes contained in other exported nodes are dropped. The result is in DFS order.
 */
McpResult<std::vector<mdl::Node*>> exportNodes(
  McpCallContext& context, const QJsonArray& ids)
{
  auto& doc = context.doc;
  auto resolved = resolveNodes(doc, ids, size_t(schema::MaxNodeIds));
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }

  auto selected = std::unordered_set<const mdl::Node*>{};
  for (auto* node : resolved.value())
  {
    if (dynamic_cast<const mdl::WorldNode*>(node))
    {
      return toolError(
        "wrong_node_type",
        "the world cannot be exported as a node",
        "omit ids to export the whole map",
        {{"id", "world"},
         {"actual", "world"},
         {"expected",
          "layer, group, entity, brush "
          "or patch"}});
    }
    if (dynamic_cast<const mdl::LayerNode*>(node))
    {
      for (const auto* child : node->children())
      {
        selected.insert(child);
      }
    }
    else if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node->parent());
             entityNode && !dynamic_cast<const mdl::EntityNode*>(node))
    {
      context.warnings.push_back(QString{"%1 is exported with its entity %2"}.arg(
        doc.nodes.idOf(*node), doc.nodes.idOf(*entityNode)));
      selected.insert(entityNode);
    }
    else
    {
      selected.insert(node);
    }
  }

  auto result = std::vector<mdl::Node*>{};
  visitPreOrder(doc.map.worldNode(), [&](mdl::Node& node) {
    if (!selected.contains(&node))
    {
      return;
    }
    for (const auto* ancestor = node.parent(); ancestor; ancestor = ancestor->parent())
    {
      if (selected.contains(ancestor))
      {
        return;
      }
    }
    result.push_back(&node);
  });
  return result;
}

/**
 * Restores the file positions (line numbers) of all nodes and brush faces when it goes
 * out of scope. Serializing nodes sets their file positions to the lines of the written
 * text, but an export must not change what the issue browser and "select by line
 * number" report for the document.
 */
class PreserveFilePositions
{
public:
  explicit PreserveFilePositions(const mdl::Node& root) { visit(root); }

  ~PreserveFilePositions()
  {
    for (const auto& [node, lineNumber, lineCount] : m_nodes)
    {
      node->setFilePosition(lineNumber, lineCount);
    }
    for (const auto& [face, lineNumber] : m_faces)
    {
      // BrushFace does not expose its line count; serializers always write one line
      face->setFilePosition(lineNumber, lineNumber > 0 ? 1 : 0);
    }
  }

  PreserveFilePositions(const PreserveFilePositions&) = delete;
  PreserveFilePositions& operator=(const PreserveFilePositions&) = delete;

private:
  void visit(const mdl::Node& node)
  {
    m_nodes.emplace_back(&node, node.lineNumber(), node.lineCount());
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      for (const auto& face : brushNode->brush().faces())
      {
        m_faces.emplace_back(&face, face.lineNumber());
      }
    }
    for (const auto* child : node.children())
    {
      visit(*child);
    }
  }

  std::vector<std::tuple<const mdl::Node*, size_t, size_t>> m_nodes;
  std::vector<std::tuple<const mdl::BrushFace*, size_t>> m_faces;
};

/** Cuts the given text to at most maxBytes UTF-8 bytes at a code point boundary. */
QString truncateUtf8(const QByteArray& utf8, const qsizetype maxBytes)
{
  auto end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(utf8[end]) & 0xC0) == 0x80)
  {
    --end;
  }
  return QString::fromUtf8(utf8.left(end));
}

McpResult<QJsonObject> exportMapText(McpCallContext& context, const QJsonObject& args)
{
  auto& map = context.doc.map;

  auto nodes = std::optional<std::vector<mdl::Node*>>{};
  if (args.contains("ids"))
  {
    auto exported = exportNodes(context, args.value("ids").toArray());
    if (exported.is_error())
    {
      return errorOf(exported);
    }
    nodes = std::move(exported).value();
  }

  const auto preserveFilePositions = PreserveFilePositions{map.worldNode()};

  auto stream = std::stringstream{};
  if (args.value("include_header").toBool(false))
  {
    mdl::writeMapHeader(
      stream, map.gameInfo().gameConfig.name, map.worldNode().mapFormat(), "TrenchBroom");
  }

  auto writer = mdl::NodeWriter{map.worldNode(), stream};
  writer.setStripTbProperties(args.value("strip_tb_properties").toBool(false));
  if (nodes)
  {
    if (!nodes->empty())
    {
      writer.writeNodes(*nodes, map.taskManager());
    }
  }
  else
  {
    writer.writeMap(map.taskManager());
  }

  const auto text = toUnicode(map, stream.str());
  const auto utf8 = text.toUtf8();
  const auto maxBytes = qsizetype(sizeArgument(args, "max_bytes", DefaultMaxBytes));
  const auto truncated = utf8.size() > maxBytes;

  auto result = QJsonObject{
    {"text", truncated ? truncateUtf8(utf8, maxBytes) : text},
    {"bytes", qint64(utf8.size())},
    {"truncated", truncated},
  };
  if (nodes)
  {
    auto ids = QJsonArray{};
    for (const auto* node : *nodes)
    {
      ids.append(context.doc.nodes.idOf(*node));
    }
    result["ids"] = ids;
  }
  return result;
}

McpToolDef exportMapTextTool()
{
  return {
    .name = "export_map_text",
    .title = "Export Map Text",
    .description =
      "Returns the map, or the given nodes, as .map file text (bytes = full UTF-8 size; "
      "text is cut at max_bytes). The text can be pasted with paste_map_text. Does not "
      "change the selection.",
    .inputSchema = schema::object({
      {"ids",
       schema::nodeIds(
         schema::MaxNodeIds,
         "nodes to export (layers export their children); default: the whole map")},
      {"include_header",
       schema::boolean("start with the // Game and // Format comment lines")},
      {"strip_tb_properties",
       schema::boolean("omit TrenchBroom specific properties such as layers and groups")},
      {"max_bytes",
       schema::integer(
         MinMaxBytes,
         MaxMaxBytes,
         "maximum UTF-8 size of text (default 256 KiB, at most 8 MiB)")},
    }),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = exportMapText,
  };
}

// shared by query_point and check_brush_overlaps

/** Returns all brushes in DFS order. */
std::vector<const mdl::BrushNode*> allBrushes(const mdl::Map& map)
{
  auto result = std::vector<const mdl::BrushNode*>{};
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      result.push_back(brushNode);
    }
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, map.worldNode());
  return result;
}

QString entityIdOf(const McpNodeRegistry& registry, const mdl::BrushNode& brushNode)
{
  const auto* entity = brushNode.entity();
  return entity && !dynamic_cast<const mdl::WorldNode*>(entity) ? registry.idOf(*entity)
                                                                : QStringLiteral("world");
}

// query_point

constexpr auto MaxQueryPoints = 200;

McpResult<QJsonObject> queryPoint(McpCallContext& context, const QJsonObject& args)
{
  const auto& doc = context.doc;
  const auto& map = doc.map;

  const auto pointsArg = args.value("points").toArray();
  auto points = std::vector<vm::vec3d>{};
  for (qsizetype i = 0; i < pointsArg.size(); ++i)
  {
    auto point = vec3(pointsArg[i], QString{"/points/%1"}.arg(i));
    if (point.is_error())
    {
      return errorOf(point);
    }
    points.push_back(point.value());
  }

  const auto brushes = allBrushes(map);
  auto results = QJsonArray{};
  for (const auto& point : points)
  {
    auto containing = QJsonArray{};
    auto insideSolid = false;
    for (const auto* brushNode : brushes)
    {
      const auto& brush = brushNode->brush();
      if (brush.bounds().contains(point) && brush.containsPoint(point))
      {
        const auto entity = entityIdOf(doc.nodes, *brushNode);
        insideSolid = insideSolid || entity == u"world";
        containing.append(
          QJsonObject{{"id", doc.nodes.idOf(*brushNode)}, {"entity", entity}});
      }
    }
    results.append(QJsonObject{
      {"point", toJson(point)},
      {"in_world_bounds", map.worldBounds().contains(point)},
      {"inside_solid", insideSolid},
      {"containing_brushes", containing},
    });
  }

  return QJsonObject{{"results", results}};
}

McpToolDef queryPointTool()
{
  return {
    .name = "query_point",
    .title = "Query Point",
    .description =
      "Tells for each point whether it is inside solid world geometry and which brushes "
      "contain it (a point on a brush face counts as inside). Brushes of entities such "
      "as doors are listed but do not count as solid.",
    .inputSchema = schema::object(
      {
        {"points", schema::array(schema::vec3(), 1, MaxQueryPoints, "points to test")},
      },
      {"points"}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = queryPoint,
  };
}

// check_brush_overlaps

constexpr auto DefaultMaxPairs = 100;
constexpr auto MaxPairs = 1000;
constexpr auto OverlapEpsilon = 1e-6;

/** Returns the brushes of the given nodes and their descendants, in DFS order. */
McpResult<std::vector<const mdl::BrushNode*>> brushesOf(
  const McpDocumentState& doc, const QJsonObject& args)
{
  if (!args.contains("ids"))
  {
    return allBrushes(doc.map);
  }

  auto nodes = resolveNodes(doc, args.value("ids").toArray(), size_t(schema::MaxNodeIds));
  if (nodes.is_error())
  {
    return errorOf(nodes);
  }

  auto wanted = std::unordered_set<const mdl::BrushNode*>{};
  const auto collect = [&](const auto& self, const mdl::Node& node) -> void {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      wanted.insert(brushNode);
    }
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  for (const auto* node : nodes.value())
  {
    collect(collect, *node);
  }

  auto result = allBrushes(doc.map);
  std::erase_if(
    result, [&](const auto* brushNode) { return !wanted.contains(brushNode); });
  return result;
}

McpResult<QJsonObject> checkBrushOverlaps(
  McpCallContext& context, const QJsonObject& args)
{
  const auto& doc = context.doc;
  const auto& map = doc.map;

  auto brushesResult = brushesOf(doc, args);
  if (brushesResult.is_error())
  {
    return errorOf(brushesResult);
  }
  const auto& brushes = brushesResult.value();

  const auto minVolume = args.value("min_volume").toDouble(1.0);
  const auto maxPairs = sizeArgument(args, "max_pairs", DefaultMaxPairs);

  // sweep along x: candidates are pairs whose bounds overlap with positive extent
  auto order = std::vector<size_t>(brushes.size());
  for (size_t i = 0; i < order.size(); ++i)
  {
    order[i] = i;
  }
  std::ranges::stable_sort(order, [&](const auto lhs, const auto rhs) {
    return brushes[lhs]->brush().bounds().min.x()
           < brushes[rhs]->brush().bounds().min.x();
  });

  struct Pair
  {
    size_t a;
    size_t b;
    vm::bbox3d bounds;
    double volume;
  };
  auto pairs = std::vector<Pair>{};

  for (size_t i = 0; i < order.size(); ++i)
  {
    const auto& boundsA = brushes[order[i]]->brush().bounds();
    for (size_t j = i + 1; j < order.size(); ++j)
    {
      const auto& boundsB = brushes[order[j]]->brush().bounds();
      if (boundsB.min.x() >= boundsA.max.x() - OverlapEpsilon)
      {
        break;
      }

      auto overlapsWithExtent = true;
      for (size_t k = 0; k < 3; ++k)
      {
        const auto extent = std::min(boundsA.max[k], boundsB.max[k])
                            - std::max(boundsA.min[k], boundsB.min[k]);
        overlapsWithExtent = overlapsWithExtent && extent > OverlapEpsilon;
      }
      if (!overlapsWithExtent)
      {
        continue;
      }

      auto intersection = brushes[order[i]]->brush();
      if (intersection.intersect(map.worldBounds(), brushes[order[j]]->brush())
            .is_success())
      {
        const auto volume = brushVolume(intersection);
        if (volume > minVolume)
        {
          const auto a = std::min(order[i], order[j]);
          const auto b = std::max(order[i], order[j]);
          pairs.push_back({a, b, intersection.bounds(), volume});
        }
      }
    }
  }

  // brushes are in DFS order, so this is deterministic
  std::ranges::sort(pairs, [](const auto& lhs, const auto& rhs) {
    return std::pair{lhs.a, lhs.b} < std::pair{rhs.a, rhs.b};
  });

  auto pairsJson = QJsonArray{};
  for (const auto& pair : pairs)
  {
    if (size_t(pairsJson.size()) >= maxPairs)
    {
      break;
    }
    pairsJson.append(QJsonObject{
      {"a", doc.nodes.idOf(*brushes[pair.a])},
      {"b", doc.nodes.idOf(*brushes[pair.b])},
      {"overlap_bounds", toJson(pair.bounds)},
      {"overlap_volume", canon(pair.volume)},
    });
  }

  return QJsonObject{
    {"pairs", pairsJson},
    {"total", qint64(pairs.size())},
    {"truncated", pairs.size() > maxPairs},
    {"brush_count", qint64(brushes.size())},
  };
}

McpToolDef checkBrushOverlapsTool()
{
  return {
    .name = "check_brush_overlaps",
    .title = "Check Brush Overlaps",
    .description =
      "Finds pairs of brushes whose volumes overlap by more than min_volume cubic "
      "units; brushes that only touch do not overlap. Checks all brushes, or the "
      "brushes of the given nodes.",
    .inputSchema = schema::object({
      {"ids",
       schema::nodeIds(
         schema::MaxNodeIds, "brushes, or nodes whose brushes to check (default all)")},
      {"min_volume",
       schema::number(0, 1e18, "smallest overlap volume to report (default 1)")},
      {"max_pairs", schema::integer(1, MaxPairs, "most pairs to return (default 100)")},
    }),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = checkBrushOverlaps,
  };
}

// check_leaks

constexpr auto DefaultLeakResolution = 8.0;
constexpr auto MaxLeakResolution = 4096.0;
constexpr auto DefaultLeakMaxCells = 8000000;
constexpr auto MaxLeakStartPoints = 256;
constexpr auto MaxReportedLeaks = size_t(50);
constexpr auto MaxReportedStarts = size_t(50);
constexpr auto MaxLeakPathPoints = size_t(64);
constexpr auto LeakGridEpsilon = 1e-6;

constexpr auto CellSolid = uint8_t(1);
constexpr auto CellVisited = uint8_t(2);
// bits 2 to 4 of a visited cell: 1 + the direction from its parent, 0 for a start cell
constexpr auto CellParentShift = 2;

/** The neighbour directions in the order in which the flood fill visits them. */
constexpr auto LeakDirections = std::array<std::array<int, 3>, 6>{{
  {-1, 0, 0},
  {1, 0, 0},
  {0, -1, 0},
  {0, 1, 0},
  {0, 0, -1},
  {0, 0, 1},
}};

using CellCoords = std::array<size_t, 3>;

/** A grid of cubic cells whose first corner is origin. Indices are x fastest. */
struct LeakGrid
{
  vm::vec3d origin;
  double resolution;
  CellCoords counts;

  size_t size() const { return counts[0] * counts[1] * counts[2]; }

  size_t index(const CellCoords& c) const
  {
    return c[0] + counts[0] * (c[1] + counts[1] * c[2]);
  }

  CellCoords coords(const size_t index) const
  {
    return {
      index % counts[0],
      (index / counts[0]) % counts[1],
      index / (counts[0] * counts[1])};
  }

  vm::vec3d corner(const CellCoords& c) const
  {
    return {
      origin.x() + resolution * double(c[0]),
      origin.y() + resolution * double(c[1]),
      origin.z() + resolution * double(c[2])};
  }

  vm::vec3d center(const size_t index) const
  {
    const auto c = coords(index);
    return {
      origin.x() + resolution * (double(c[0]) + 0.5),
      origin.y() + resolution * (double(c[1]) + 0.5),
      origin.z() + resolution * (double(c[2]) + 0.5)};
  }

  bool onBoundary(const size_t index) const
  {
    const auto c = coords(index);
    for (size_t i = 0; i < 3; ++i)
    {
      if (c[i] == 0 || c[i] + 1 == counts[i])
      {
        return true;
      }
    }
    return false;
  }

  /**
   * Returns the cells whose closed boxes contain the given point, which must be inside
   * the grid: one cell, or up to eight if the point is on cell boundaries.
   */
  std::vector<size_t> cellsTouching(const vm::vec3d& point) const
  {
    auto candidates = std::array<std::vector<size_t>, 3>{};
    for (size_t i = 0; i < 3; ++i)
    {
      const auto f = (point[i] - origin[i]) / resolution;
      const auto r = std::round(f);
      // as close as brushContains counts a point as on a face
      if (
        std::abs(f - r) <= std::max(
          LeakGridEpsilon, vm::constants<double>::point_status_epsilon() / resolution))
      {
        for (const auto c : {r - 1.0, r})
        {
          if (c >= 0.0 && c < double(counts[i]))
          {
            candidates[i].push_back(size_t(c));
          }
        }
      }
      else
      {
        candidates[i].push_back(
          std::min(size_t(std::max(std::floor(f), 0.0)), counts[i] - 1));
      }
    }

    auto result = std::vector<size_t>{};
    for (const auto z : candidates[2])
    {
      for (const auto y : candidates[1])
      {
        for (const auto x : candidates[0])
        {
          result.push_back(index({x, y, z}));
        }
      }
    }
    return result;
  }

  /** Returns the cell containing the given point, or nullopt if it is outside. */
  std::optional<size_t> cellOf(const vm::vec3d& point) const
  {
    auto c = CellCoords{};
    for (size_t i = 0; i < 3; ++i)
    {
      const auto f = std::floor((point[i] - origin[i]) / resolution);
      if (!(f >= 0.0) || f > double(counts[i]))
      {
        return std::nullopt;
      }
      // a point on the far side of the grid belongs to the last cell
      c[i] = std::min(size_t(f), counts[i] - 1);
    }
    return index(c);
  }
};

/** Whether the point is inside the brush or on its boundary. */
bool brushContains(const mdl::Brush& brush, const vm::vec3d& point)
{
  return std::ranges::none_of(brush.faces(), [&](const auto& face) {
    return face.boundary().point_status(point) == vm::plane_status::above;
  });
}

/** Whether the point is inside the brush and not on its boundary. */
bool brushStrictlyContains(const mdl::Brush& brush, const vm::vec3d& point)
{
  return brush.bounds().contains(point)
         && std::ranges::all_of(brush.faces(), [&](const auto& face) {
              return face.boundary().point_status(point) == vm::plane_status::below;
            });
}

/**
 * Marks the cells that lie entirely inside one of the given brushes as solid: all eight
 * corners of the cell must be inside the same brush. A cell that two brushes cover only
 * together stays empty, so that a gap between brushes is never closed by rounding.
 * Returns the number of solid cells.
 */
size_t markSolidCells(
  const LeakGrid& grid,
  const std::vector<const mdl::Brush*>& brushes,
  std::vector<uint8_t>& cells)
{
  auto solidCells = size_t(0);
  auto lines = std::vector<std::pair<std::ptrdiff_t, std::ptrdiff_t>>{};

  for (const auto* brush : brushes)
  {
    const auto& bounds = brush->bounds();
    auto lo = CellCoords{};
    auto hi = CellCoords{};
    auto empty = false;
    for (size_t i = 0; i < 3 && !empty; ++i)
    {
      // the cells whose corners are all inside the brush's bounds
      const auto first =
        std::ceil((bounds.min[i] - grid.origin[i]) / grid.resolution - LeakGridEpsilon);
      const auto last =
        std::floor((bounds.max[i] - grid.origin[i]) / grid.resolution + LeakGridEpsilon)
        - 1.0;
      const auto from = std::max(first, 0.0);
      const auto to = std::min(last, double(grid.counts[i]) - 1.0);
      if (from > to)
      {
        empty = true;
      }
      else
      {
        lo[i] = size_t(from);
        hi[i] = size_t(to);
      }
    }
    if (empty)
    {
      continue;
    }

    // For each line of corners parallel to x, the range of corners inside the brush. The
    // planes give the range in O(faces) per line, so the work grows with the brush's
    // cross section instead of its volume. The end corners are then checked with the
    // same test as brushContains: if both ends are inside, every corner between them is
    // too, because the brush is convex, so rounding can only make the range smaller.
    const auto ny = hi[1] - lo[1] + 2;
    const auto nz = hi[2] - lo[2] + 2;
    const auto firstX = double(lo[0]);
    const auto lastX = double(hi[0] + 1);
    lines.resize(ny * nz);
    for (size_t z = 0; z < nz; ++z)
    {
      for (size_t y = 0; y < ny; ++y)
      {
        const auto cy = lo[1] + y;
        const auto cz = lo[2] + z;
        const auto p = grid.corner({0, cy, cz});

        auto from = firstX;
        auto to = lastX;
        for (const auto& face : brush->faces())
        {
          const auto& plane = face.boundary();
          const auto nx = plane.normal.x();
          // the corner at x index X is inside if nx * resolution * X <= rest
          const auto rest = plane.distance + vm::constants<double>::point_status_epsilon()
                            - vm::dot(plane.normal, p);
          if (nx == 0.0)
          {
            if (rest < 0.0)
            {
              to = from - 1.0;
            }
          }
          else
          {
            const auto bound = rest / (nx * grid.resolution);
            if (nx > 0.0)
            {
              to = std::min(to, std::floor(bound));
            }
            else
            {
              from = std::max(from, std::ceil(bound));
            }
          }
          if (!(from <= to))
          {
            break;
          }
        }

        auto& line = lines[y + ny * z];
        if (!(from <= to))
        {
          line = {1, 0};
          continue;
        }

        const auto inside = [&](const std::ptrdiff_t x) {
          return brushContains(*brush, grid.corner({size_t(x), cy, cz}));
        };
        auto first = std::ptrdiff_t(from);
        auto last = std::ptrdiff_t(to);
        while (first <= last && !inside(first))
        {
          ++first;
        }
        while (first <= last && !inside(last))
        {
          --last;
        }
        line = {first, last};
      }
    }

    for (auto z = lo[2]; z <= hi[2]; ++z)
    {
      for (auto y = lo[1]; y <= hi[1]; ++y)
      {
        // a cell is solid if the four corner lines along its x edges contain both of
        // its x corners
        auto first = std::ptrdiff_t(lo[0]);
        auto last = std::ptrdiff_t(hi[0] + 1);
        for (size_t k = 0; k < 4; ++k)
        {
          const auto& line = lines[(y - lo[1] + (k & 1)) + ny * (z - lo[2] + (k >> 1))];
          first = std::max(first, line.first);
          last = std::min(last, line.second);
        }
        for (auto x = first; x < last; ++x)
        {
          auto& cell = cells[grid.index({size_t(x), y, z})];
          if (!(cell & CellSolid))
          {
            cell |= CellSolid;
            ++solidCells;
          }
        }
      }
    }
  }

  return solidCells;
}

/**
 * Returns the polyline from the start point through the given cell centers: points
 * where the path goes straight on are removed and, if more than MaxLeakPathPoints remain,
 * evenly spaced points are kept. The first and the last point are always kept.
 */
std::vector<vm::vec3d> leakPath(
  const vm::vec3d& start, const std::vector<vm::vec3d>& centers)
{
  auto points = std::vector<vm::vec3d>{start};
  for (const auto& center : centers)
  {
    if (center != points.back())
    {
      points.push_back(center);
    }
  }

  auto corners = std::vector<vm::vec3d>{points.front()};
  for (size_t i = 1; i + 1 < points.size(); ++i)
  {
    const auto in = vm::normalize(points[i] - points[i - 1]);
    const auto out = vm::normalize(points[i + 1] - points[i]);
    if (vm::length(in - out) > LeakGridEpsilon)
    {
      corners.push_back(points[i]);
    }
  }
  if (points.size() > 1)
  {
    corners.push_back(points.back());
  }

  if (corners.size() <= MaxLeakPathPoints)
  {
    return corners;
  }

  auto result = std::vector<vm::vec3d>{};
  for (size_t i = 0; i < MaxLeakPathPoints; ++i)
  {
    result.push_back(corners[i * (corners.size() - 1) / (MaxLeakPathPoints - 1)]);
  }
  return result;
}

struct LeakStart
{
  vm::vec3d point;
  // empty for start points given by the client
  QString entityId;
};

QJsonObject leakStartJson(const LeakStart& start, const QString& pointKey)
{
  auto result = QJsonObject{{pointKey, toJson(start.point)}};
  if (!start.entityId.isEmpty())
  {
    result[pointKey == u"point" ? "entity_id" : "start_entity_id"] = start.entityId;
  }
  return result;
}

/** Whether the node is in a layer that is omitted from export, so a compiler never sees
 * it. */
bool omittedFromExport(const mdl::Node& node)
{
  const auto* layerNode = mdl::findContainingLayer(node);
  return layerNode && layerNode->layer().omitFromExport();
}

bool isFiniteCoordinate(const vm::vec3d& point)
{
  for (size_t i = 0; i < 3; ++i)
  {
    if (!std::isfinite(point[i]) || std::abs(point[i]) > MaxCoordinate)
    {
      return false;
    }
  }
  return true;
}

McpResult<QJsonObject> checkLeaks(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto resolution = args.value("resolution").toDouble(DefaultLeakResolution);
  const auto maxCells = sizeArgument(args, "max_cells", size_t(DefaultLeakMaxCells));

  // only world brushes seal, as in the Quake compilers, and only those that are exported
  auto brushes = std::vector<const mdl::Brush*>{};
  auto brushBounds = std::optional<vm::bbox3d>{};
  for (const auto* brushNode : allBrushes(map))
  {
    if (isWorldBrush(*brushNode) && !omittedFromExport(*brushNode))
    {
      brushes.push_back(&brushNode->brush());
      brushBounds = brushBounds ? vm::merge(*brushBounds, brushNode->brush().bounds())
                                : brushNode->brush().bounds();
    }
  }

  auto starts = std::vector<LeakStart>{};
  auto excluded = std::vector<std::pair<LeakStart, QString>>{};
  const auto explicitStarts = args.contains("start_points");
  if (explicitStarts)
  {
    const auto pointsArg = args.value("start_points").toArray();
    for (qsizetype i = 0; i < pointsArg.size(); ++i)
    {
      auto point = vec3(pointsArg[i], QString{"/start_points/%1"}.arg(i));
      if (point.is_error())
      {
        return errorOf(point);
      }
      starts.push_back({point.value(), {}});
    }
  }
  else
  {
    visitPreOrder(map.worldNode(), [&](mdl::Node& node) {
      if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
          entityNode && entityNode->entity().pointEntity())
      {
        auto start =
          LeakStart{entityNode->entity().origin(), doc.nodes.idOf(*entityNode)};
        if (omittedFromExport(*entityNode))
        {
          excluded.emplace_back(std::move(start), "omitted_from_export");
        }
        else if (isFiniteCoordinate(start.point))
        {
          starts.push_back(std::move(start));
        }
        else
        {
          excluded.emplace_back(std::move(start), "invalid_origin");
        }
      }
    });
  }

  auto box = vm::bbox3d{};
  if (args.contains("bounds"))
  {
    auto boundsResult = bounds(args.value("bounds"), "/bounds");
    if (boundsResult.is_error())
    {
      return errorOf(boundsResult);
    }
    box = boundsResult.value();
    if (explicitStarts)
    {
      for (size_t i = 0; i < starts.size(); ++i)
      {
        if (!box.contains(starts[i].point))
        {
          return toolError(
            "invalid_argument",
            QString{"start point %1 is outside the bounds"}.arg(i),
            "pass bounds that contain every start point, or omit bounds",
            {{"pointer", QString{"/start_points/%1"}.arg(i)}});
        }
      }
    }
  }
  else
  {
    auto merged = brushBounds;
    for (const auto& start : starts)
    {
      const auto pointBounds = vm::bbox3d{start.point};
      merged = merged ? vm::merge(*merged, pointBounds) : pointBounds;
    }
    if (!merged)
    {
      context.warnings.push_back(
        "the map has no world brushes and no point entities, so there is nothing to "
        "check");
      return QJsonObject{
        {"sealed", true},
        {"leaks", QJsonArray{}},
        {"leaks_total", 0},
        {"start_points_checked", 0},
        {"excluded_start_points", QJsonArray{}},
        {"excluded_start_points_count", 0},
        {"resolution", canon(resolution)},
        {"bounds", QJsonValue{}},
        {"grid", QJsonArray{0, 0, 0}},
        {"cells", 0},
        {"solid_cells", 0},
        {"elapsed_cells_visited", 0},
      };
    }
    const auto margin = vm::vec3d::fill(2.0 * resolution);
    box = vm::bbox3d{merged->min - margin, merged->max + margin};
  }

  auto counts = CellCoords{};
  auto totalCells = 1.0;
  for (size_t i = 0; i < 3; ++i)
  {
    const auto count =
      std::max(1.0, std::ceil((box.max[i] - box.min[i]) / resolution - LeakGridEpsilon));
    totalCells *= count;
    counts[i] = totalCells <= double(maxCells) ? size_t(count) : 0;
  }
  if (totalCells > double(maxCells))
  {
    return toolError(
      "invalid_argument",
      QString{"checking these bounds at resolution %1 needs %2 cells, more than "
              "max_cells (%3)"}
        .arg(canon(resolution))
        .arg(QString::number(totalCells, 'g', 6))
        .arg(maxCells),
      "raise resolution (e.g. 16 or 32), pass smaller bounds or raise max_cells (at "
      "most 8000000)",
      {{"pointer", "/resolution"},
       {"cells", totalCells},
       {"max_cells", qint64(maxCells)}});
  }

  const auto grid = LeakGrid{box.min, resolution, counts};
  auto cells = std::vector<uint8_t>(grid.size(), 0);
  const auto solidCells = markSolidCells(grid, brushes, cells);

  // the start points that are not inside solid, with the empty cells they touch
  auto active = std::vector<std::pair<LeakStart, std::vector<size_t>>>{};
  auto outsideBounds = size_t(0);
  for (auto& start : starts)
  {
    if (!box.contains(start.point) || !grid.cellOf(start.point))
    {
      excluded.emplace_back(std::move(start), "outside_bounds");
      ++outsideBounds;
      continue;
    }

    // A point on a face (an entity standing on the floor) is not in solid: every empty
    // cell it touches is filled, because the cell containing it may be on the wrong side.
    auto seeds = std::vector<size_t>{};
    if (std::ranges::none_of(
          brushes, [&](const auto* brush) { return brush->containsPoint(start.point); }))
    {
      // in empty space, so its cell cannot be solid
      seeds.push_back(*grid.cellOf(start.point));
    }
    else if (std::ranges::none_of(brushes, [&](const auto* brush) {
               return brushStrictlyContains(*brush, start.point);
             }))
    {
      for (const auto cell : grid.cellsTouching(start.point))
      {
        if (!(cells[cell] & CellSolid))
        {
          seeds.push_back(cell);
        }
      }
    }

    if (seeds.empty())
    {
      excluded.emplace_back(std::move(start), "inside_solid");
    }
    else
    {
      active.emplace_back(std::move(start), std::move(seeds));
    }
  }

  struct Leak
  {
    size_t start;
    std::vector<vm::vec3d> centers;
    std::vector<size_t> sharedStarts;
  };
  auto leaks = std::vector<Leak>{};
  auto flooded = std::vector<bool>(active.size(), false);
  auto queue = std::vector<uint32_t>{};
  auto visited = size_t(0);

  for (size_t i = 0; i < active.size(); ++i)
  {
    if (flooded[i])
    {
      continue;
    }

    // breadth first, so the first boundary cell is a nearest one
    auto exitCell = std::optional<size_t>{};
    queue.clear();
    for (const auto seed : active[i].second)
    {
      if (!(cells[seed] & CellVisited))
      {
        cells[seed] |= CellVisited;
        queue.push_back(uint32_t(seed));
      }
    }
    for (size_t head = 0; head < queue.size(); ++head)
    {
      const auto cell = size_t(queue[head]);
      ++visited;
      if (!exitCell && grid.onBoundary(cell))
      {
        exitCell = cell;
      }

      const auto c = grid.coords(cell);
      for (size_t d = 0; d < LeakDirections.size(); ++d)
      {
        auto n = c;
        auto inside = true;
        for (size_t k = 0; k < 3; ++k)
        {
          const auto delta = LeakDirections[d][k];
          if ((delta < 0 && c[k] == 0) || (delta > 0 && c[k] + 1 == grid.counts[k]))
          {
            inside = false;
          }
          n[k] = size_t(std::ptrdiff_t(c[k]) + delta);
        }
        if (!inside)
        {
          continue;
        }
        const auto neighbour = grid.index(n);
        auto& state = cells[neighbour];
        if (state & (CellSolid | CellVisited))
        {
          continue;
        }
        state |= CellVisited | uint8_t((d + 1) << CellParentShift);
        queue.push_back(uint32_t(neighbour));
      }
    }

    flooded[i] = true;
    auto shared = std::vector<size_t>{};
    for (size_t j = i + 1; j < active.size(); ++j)
    {
      if (!flooded[j] && std::ranges::all_of(active[j].second, [&](const auto seed) {
            return (cells[seed] & CellVisited) != 0;
          }))
      {
        flooded[j] = true;
        shared.push_back(j);
      }
    }

    if (exitCell)
    {
      auto pathCells = std::vector<size_t>{};
      for (auto cell = *exitCell;;)
      {
        pathCells.push_back(cell);
        const auto parent = (cells[cell] >> CellParentShift) & 7;
        if (parent == 0)
        {
          break;
        }
        const auto& delta = LeakDirections[size_t(parent - 1)];
        auto c = grid.coords(cell);
        for (size_t k = 0; k < 3; ++k)
        {
          c[k] = size_t(std::ptrdiff_t(c[k]) - delta[k]);
        }
        cell = grid.index(c);
      }
      std::ranges::reverse(pathCells);

      auto centers = std::vector<vm::vec3d>{};
      for (const auto cell : pathCells)
      {
        centers.push_back(grid.center(cell));
      }
      leaks.push_back({i, std::move(centers), std::move(shared)});
    }
  }

  auto leaksJson = QJsonArray{};
  for (const auto& leak : leaks)
  {
    if (size_t(leaksJson.size()) >= MaxReportedLeaks)
    {
      break;
    }
    const auto& start = active[leak.start].first;
    auto json = leakStartJson(start, "start_point");
    json["exit_point"] = toJson(leak.centers.back());

    auto length = vm::length(leak.centers.front() - start.point);
    for (size_t i = 1; i < leak.centers.size(); ++i)
    {
      length += vm::length(leak.centers[i] - leak.centers[i - 1]);
    }
    json["path_length"] = canon(length);

    auto path = QJsonArray{};
    for (const auto& point : leakPath(start.point, leak.centers))
    {
      path.append(toJson(point));
    }
    json["path"] = path;

    auto shared = QJsonArray{};
    for (size_t k = 0; k < leak.sharedStarts.size() && k < MaxReportedStarts; ++k)
    {
      shared.append(leakStartJson(active[leak.sharedStarts[k]].first, "point"));
    }
    json["shared_start_points"] = shared;
    json["shared_start_points_count"] = qint64(leak.sharedStarts.size());
    leaksJson.append(json);
  }

  auto excludedJson = QJsonArray{};
  for (size_t k = 0; k < excluded.size() && k < MaxReportedStarts; ++k)
  {
    auto json = leakStartJson(excluded[k].first, "point");
    json["reason"] = excluded[k].second;
    excludedJson.append(json);
  }

  if (outsideBounds > 0)
  {
    context.warnings.push_back(
      QString{"%1 start point(s) outside the bounds were not checked; sealed only covers "
              "the bounds"}
        .arg(outsideBounds));
  }
  if (active.empty())
  {
    context.warnings.push_back(
      "no start point outside solid geometry was flood filled, so sealed means nothing; "
      "add a point entity (e.g. info_player_start) or pass start_points");
  }

  return QJsonObject{
    {"sealed", leaks.empty()},
    {"leaks", leaksJson},
    {"leaks_total", qint64(leaks.size())},
    {"start_points_checked", qint64(active.size())},
    {"excluded_start_points", excludedJson},
    {"excluded_start_points_count", qint64(excluded.size())},
    {"resolution", canon(resolution)},
    {"bounds", toJson(box)},
    {"grid", QJsonArray{qint64(counts[0]), qint64(counts[1]), qint64(counts[2])}},
    {"cells", qint64(grid.size())},
    {"solid_cells", qint64(solidCells)},
    {"elapsed_cells_visited", qint64(visited)},
  };
}

McpToolDef checkLeaksTool()
{
  return {
    .name = "check_leaks",
    .title = "Check Leaks",
    .description =
      "Leak test like a Quake compiler: flood fills the empty space from each start "
      "point (default: the origins of all point entities that are not inside solid) "
      "through a voxel grid of resolution sized cells that starts at bounds.min; a "
      "start point leaks if the fill reaches the outermost cells of the bounds "
      "(default: all world brushes plus the start points, expanded by 2*resolution). "
      "Each leak has the shortest path to the outside (at most 64 corner points), and "
      "start points in the same space are listed with the first one. Only world "
      "brushes seal, including grouped ones; brush entities such as func_door, "
      "func_wall, triggers and even func_group do not; nodes in layers omitted from "
      "export are ignored, and materials are not considered (liquid, clip or detail "
      "world brushes seal here). A start point on a face fills the space next to it. "
      "A cell is solid only if it lies "
      "entirely inside one brush, so a gap is never closed by rounding, but walls "
      "thinner than 2*resolution, walls off the grid and brushes that only together "
      "cover a cell can show up as leaks: check them with query_point or a smaller "
      "resolution.",
    .inputSchema = schema::object({
      {"start_points",
       schema::array(
         schema::vec3(),
         1,
         MaxLeakStartPoints,
         "points to flood fill from (default: point entity origins)")},
      {"resolution",
       schema::number(1, MaxLeakResolution, "cell size in units (default 8)")},
      {"bounds",
       schema::bounds(
         "the region to fill; reaching its outermost cells is a leak (default: world "
         "brushes and start points plus 2*resolution)")},
      {"max_cells",
       schema::integer(
         1,
         DefaultLeakMaxCells,
         "most cells to allocate (default and maximum 8000000); raise resolution for "
         "big maps")},
    }),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = checkLeaks,
  };
}

// check_walkable

constexpr auto MaxWalkTargets = 64;
constexpr auto DefaultWalkMaxCells = 4000000;
constexpr auto MaxWalkMaxCells = 16000000;
constexpr auto DefaultHullWidth = 32.0;
constexpr auto DefaultHullHeight = 56.0;
constexpr auto DefaultStepHeight = 18.0;
constexpr auto DefaultMaxDrop = 256.0;
constexpr auto DefaultWalkResolution = 8.0;
constexpr auto MaxWalkResolution = 64.0;
// boxes are shrunk by this much, so that touching a floor or a wall is not a collision
constexpr auto WalkEpsilon = 1e-3;
constexpr auto WalkBucketColumns = int64_t(8);

/** The four neighbour columns in the order in which the search visits them. */
constexpr auto WalkDirections = std::array<std::array<int64_t, 2>, 4>{{
  {-1, 0},
  {1, 0},
  {0, -1},
  {0, 1},
}};

/** An axis-aligned box that may be empty; vm::bbox asserts on inverted boxes. */
struct WalkBox
{
  vm::vec3d min;
  vm::vec3d max;
};

/** A brush that the player collides with, prepared for the box tests. */
struct WalkSolid
{
  const mdl::Brush* brush;
  vm::bbox3d bounds;
  std::vector<vm::plane3d> planes;
  std::vector<vm::vec3d> vertices;
  // one per parallel class of edges
  std::vector<vm::vec3d> edgeDirections;
  // whether every face is axis-aligned, so that the brush is its bounds
  bool box;
};

/**
 * Whether the player collides with the brush: world brushes (also in groups) and the
 * brushes of func_group, func_detail* and func_wall, unless their layer is omitted from
 * export. Doors, platforms, triggers and other brush entities are ignored.
 */
bool isWalkSolid(const mdl::BrushNode& brushNode)
{
  if (omittedFromExport(brushNode))
  {
    return false;
  }
  if (isWorldBrush(brushNode))
  {
    return true;
  }
  const auto* entityNode = brushNode.entity();
  if (!entityNode)
  {
    return false;
  }
  auto classname = entityNode->entity().classname();
  std::ranges::transform(
    classname, classname.begin(), [](const unsigned char c) { return std::tolower(c); });
  return classname == "func_group" || classname == "func_wall"
         || classname.starts_with("func_detail");
}

WalkSolid makeWalkSolid(const mdl::Brush& brush)
{
  auto solid = WalkSolid{&brush, brush.bounds(), {}, brush.vertexPositions(), {}, true};
  for (const auto& face : brush.faces())
  {
    const auto& plane = face.boundary();
    solid.planes.push_back(plane);
    auto axes = size_t(0);
    for (size_t i = 0; i < 3; ++i)
    {
      axes += std::abs(plane.normal[i]) > 1e-9 ? 1 : 0;
    }
    solid.box = solid.box && axes == 1;
  }
  if (!solid.box)
  {
    for (const auto* edge : brush.edges())
    {
      const auto direction =
        vm::normalize(edge->secondVertex()->position() - edge->firstVertex()->position());
      if (std::ranges::none_of(solid.edgeDirections, [&](const auto& other) {
            return vm::length(vm::cross(direction, other)) < 1e-9;
          }))
      {
        solid.edgeDirections.push_back(direction);
      }
    }
  }
  return solid;
}

/**
 * Whether the box, shrunk by WalkEpsilon on every side, overlaps the inside of the
 * brush. Uses the separating axis test, which is exact for convex polyhedra.
 */
bool boxHitsSolid(const WalkBox& box, const WalkSolid& solid)
{
  auto lo = box.min + vm::vec3d::fill(WalkEpsilon);
  auto hi = box.max - vm::vec3d::fill(WalkEpsilon);
  for (size_t i = 0; i < 3; ++i)
  {
    if (!(lo[i] < hi[i]) || hi[i] <= solid.bounds.min[i] || lo[i] >= solid.bounds.max[i])
    {
      return false;
    }
  }
  if (solid.box)
  {
    return true;
  }

  const auto center = (lo + hi) / 2.0;
  const auto half = (hi - lo) / 2.0;
  const auto radius = [&](const vm::vec3d& axis) {
    return std::abs(axis.x()) * half.x() + std::abs(axis.y()) * half.y()
           + std::abs(axis.z()) * half.z();
  };
  for (const auto& plane : solid.planes)
  {
    if (vm::dot(plane.normal, center) - radius(plane.normal) >= plane.distance)
    {
      return false;
    }
  }
  for (const auto& direction : solid.edgeDirections)
  {
    for (size_t k = 0; k < 3; ++k)
    {
      auto unit = vm::vec3d{0, 0, 0};
      unit[k] = 1.0;
      const auto axis = vm::cross(direction, unit);
      if (vm::squared_length(axis) < 1e-12)
      {
        continue;
      }
      auto min = std::numeric_limits<double>::max();
      auto max = std::numeric_limits<double>::lowest();
      for (const auto& vertex : solid.vertices)
      {
        const auto d = vm::dot(axis, vertex);
        min = std::min(min, d);
        max = std::max(max, d);
      }
      const auto c = vm::dot(axis, center);
      const auto r = radius(axis);
      if (c + r <= min || c - r >= max)
      {
        return false;
      }
    }
  }
  return true;
}

/**
 * Returns the top of the brush on the vertical line through (x, y), or nullopt if the
 * line does not pass through the brush's inside.
 */
std::optional<double> solidTopAt(const WalkSolid& solid, const double x, const double y)
{
  auto lo = std::numeric_limits<double>::lowest();
  auto hi = std::numeric_limits<double>::max();
  for (const auto& plane : solid.planes)
  {
    const auto& n = plane.normal;
    const auto rest = plane.distance - n.x() * x - n.y() * y;
    if (n.z() > 1e-9)
    {
      hi = std::min(hi, rest / n.z());
    }
    else if (n.z() < -1e-9)
    {
      lo = std::max(lo, rest / n.z());
    }
    else if (rest < -1e-6)
    {
      return std::nullopt;
    }
  }
  return lo < hi - WalkEpsilon ? std::optional{hi} : std::nullopt;
}

/**
 * The indices of the first and the last column center inside the bounds along the given
 * horizontal axis, counted from the origin in resolution steps.
 */
std::pair<int64_t, int64_t> walkIndexRange(
  const vm::bbox3d& bounds,
  const vm::vec2d& origin,
  const double resolution,
  const size_t axis)
{
  return {
    int64_t(std::ceil((bounds.min[axis] - origin[axis]) / resolution - 1e-9)),
    int64_t(std::floor((bounds.max[axis] - origin[axis]) / resolution + 1e-9))};
}

size_t walkColumnCount(
  const vm::bbox3d& bounds,
  const vm::vec2d& origin,
  const double resolution,
  const size_t axis)
{
  const auto [first, last] = walkIndexRange(bounds, origin, resolution, axis);
  return size_t(std::max(last - first + 1, int64_t(1)));
}

struct WalkColumn
{
  // indices into the solids, ascending, of the brushes that overlap the footprint
  std::vector<uint32_t> solids;
  // the tops of those brushes under the footprint, ascending
  std::vector<double> floors;
  // per floor, the position index if the hull fits on it, else -1
  std::vector<int32_t> positions;
};

struct WalkPosition
{
  size_t column;
  double z;
};

/** A move from a reachable position into a column that has no reachable floor. */
struct WalkFailure
{
  uint32_t position;
  // the neighbour column, or nullopt at the edge of the bounds
  std::optional<size_t> column;
};

struct WalkBlocker
{
  QString kind;
  vm::vec3d at;
  QJsonObject details;
};

/**
 * The navigation model of check_walkable. The columns are the hull positions on a grid of
 * resolution spaced points in the xy plane, one of which is the from point. A column's
 * floors are the tops of the solid brushes under the hull's footprint; a position is a
 * floor on which the whole hull is free of solid. The search moves between positions in
 * the four neighbour columns. Columns are evaluated on demand and cached.
 */
class WalkGraph
{
public:
  struct Params
  {
    double width;
    double height;
    double step;
    double drop;
    double resolution;
  };

private:
  Params m_params;
  vm::bbox3d m_bounds;
  vm::vec2d m_origin;
  int64_t m_i0;
  int64_t m_j0;
  size_t m_nx;
  size_t m_ny;
  std::vector<WalkSolid> m_solids;
  size_t m_nbx;
  std::vector<std::vector<uint32_t>> m_buckets;
  std::unordered_map<size_t, WalkColumn> m_columns;
  std::vector<WalkPosition> m_positions;

public:
  WalkGraph(
    const Params& params,
    const vm::bbox3d& bounds,
    const vm::vec2d& origin,
    std::vector<WalkSolid> solids)
    : m_params{params}
    , m_bounds{bounds}
    , m_origin{origin}
    , m_i0{walkIndexRange(bounds, origin, params.resolution, 0).first}
    , m_j0{walkIndexRange(bounds, origin, params.resolution, 1).first}
    , m_nx{walkColumnCount(bounds, origin, params.resolution, 0)}
    , m_ny{walkColumnCount(bounds, origin, params.resolution, 1)}
    , m_solids{std::move(solids)}
    , m_nbx{(m_nx + size_t(WalkBucketColumns) - 1) / size_t(WalkBucketColumns)}
  {
    const auto nby = (m_ny + size_t(WalkBucketColumns) - 1) / size_t(WalkBucketColumns);
    m_buckets.resize(m_nbx * nby);
    const auto half = m_params.width / 2.0;
    for (size_t s = 0; s < m_solids.size(); ++s)
    {
      const auto& b = m_solids[s].bounds;
      // the columns whose footprint overlaps the brush by more than WalkEpsilon
      const auto range = [&](const size_t axis, const int64_t first, const size_t count) {
        const auto lo =
          int64_t(std::floor(
            (b.min[axis] + WalkEpsilon - half - m_origin[axis]) / m_params.resolution))
          + 1;
        const auto hi =
          int64_t(std::ceil(
            (b.max[axis] - WalkEpsilon + half - m_origin[axis]) / m_params.resolution))
          - 1;
        return std::pair{
          std::max(lo, first) - first, std::min(hi, first + int64_t(count) - 1) - first};
      };
      const auto [xlo, xhi] = range(0, m_i0, m_nx);
      const auto [ylo, yhi] = range(1, m_j0, m_ny);
      if (xlo > xhi || ylo > yhi)
      {
        continue;
      }
      for (auto by = ylo / WalkBucketColumns; by <= yhi / WalkBucketColumns; ++by)
      {
        for (auto bx = xlo / WalkBucketColumns; bx <= xhi / WalkBucketColumns; ++bx)
        {
          m_buckets[size_t(bx) + m_nbx * size_t(by)].push_back(uint32_t(s));
        }
      }
    }
  }

  const Params& params() const { return m_params; }
  size_t solidCount() const { return m_solids.size(); }
  const WalkSolid& solid(const size_t index) const { return m_solids[index]; }
  size_t columnsEvaluated() const { return m_columns.size(); }
  const std::vector<WalkPosition>& positions() const { return m_positions; }

  /**
   * The column whose center is nearest to the point, or nullopt if the point is outside
   * the bounds. Near the edge of the bounds, the nearest center can be outside of them;
   * then the column inside is used.
   */
  std::optional<size_t> columnAt(const vm::vec3d& point) const
  {
    if (!m_bounds.contains(point))
    {
      return std::nullopt;
    }
    const auto i = std::clamp(
      int64_t(std::llround((point.x() - m_origin.x()) / m_params.resolution)) - m_i0,
      int64_t(0),
      int64_t(m_nx) - 1);
    const auto j = std::clamp(
      int64_t(std::llround((point.y() - m_origin.y()) / m_params.resolution)) - m_j0,
      int64_t(0),
      int64_t(m_ny) - 1);
    return size_t(i) + m_nx * size_t(j);
  }

  std::optional<size_t> neighbour(const size_t column, const size_t direction) const
  {
    const auto i = int64_t(column % m_nx) + WalkDirections[direction][0];
    const auto j = int64_t(column / m_nx) + WalkDirections[direction][1];
    if (i < 0 || j < 0 || i >= int64_t(m_nx) || j >= int64_t(m_ny))
    {
      return std::nullopt;
    }
    return size_t(i) + m_nx * size_t(j);
  }

  vm::vec3d point(const size_t column, const double z) const
  {
    return {
      m_origin.x() + m_params.resolution * double(m_i0 + int64_t(column % m_nx)),
      m_origin.y() + m_params.resolution * double(m_j0 + int64_t(column / m_nx)),
      z};
  }

  vm::vec3d point(const WalkPosition& position) const
  {
    return point(position.column, position.z);
  }

  /** The hull's box at the column from z to z + height. */
  WalkBox hull(const size_t column, const double z, const double height) const
  {
    const auto center = point(column, z);
    const auto half = m_params.width / 2.0;
    return {
      {center.x() - half, center.y() - half, z},
      {center.x() + half, center.y() + half, z + height}};
  }

  bool isFree(const WalkBox& box, const std::vector<uint32_t>& solids) const
  {
    return std::ranges::none_of(
      solids, [&](const auto s) { return boxHitsSolid(box, m_solids[s]); });
  }

  /** Evaluates the column if needed. References stay valid. */
  const WalkColumn& column(const size_t key)
  {
    if (const auto it = m_columns.find(key); it != m_columns.end())
    {
      return it->second;
    }

    auto result = WalkColumn{};
    const auto i = key % m_nx;
    const auto j = key / m_nx;
    const auto& bucket =
      m_buckets[i / size_t(WalkBucketColumns) + m_nbx * (j / size_t(WalkBucketColumns))];
    const auto footprint = hull(key, 0.0, 0.0);
    for (const auto s : bucket)
    {
      const auto& b = m_solids[s].bounds;
      if (
        b.max.x() > footprint.min.x() + WalkEpsilon
        && b.min.x() < footprint.max.x() - WalkEpsilon
        && b.max.y() > footprint.min.y() + WalkEpsilon
        && b.min.y() < footprint.max.y() - WalkEpsilon)
      {
        result.solids.push_back(s);
      }
    }

    for (const auto s : result.solids)
    {
      const auto top = solidTop(m_solids[s], footprint);
      if (
        top && *top >= m_bounds.min.z() - WalkEpsilon
        && *top <= m_bounds.max.z() + WalkEpsilon)
      {
        result.floors.push_back(*top);
      }
    }
    std::ranges::sort(result.floors);
    const auto [first, last] = std::ranges::unique(
      result.floors, [](const auto a, const auto b) { return b - a <= 1e-6; });
    result.floors.erase(first, last);

    for (const auto z : result.floors)
    {
      if (isFree(hull(key, z, m_params.height), result.solids))
      {
        result.positions.push_back(int32_t(m_positions.size()));
        m_positions.push_back({key, z});
      }
      else
      {
        result.positions.push_back(-1);
      }
    }

    return m_columns.emplace(key, std::move(result)).first->second;
  }

  /**
   * Whether the hull can move from (a, z1) to (b, z2) with the given hull height: the
   * box spanning both footprints at the higher floor and the lower column's footprint
   * from the lower floor up to the higher one must be free.
   */
  bool canMove(
    const size_t a, const double z1, const size_t b, const double z2, const double height)
  {
    const auto& ca = column(a);
    const auto& cb = column(b);
    const auto zh = std::max(z1, z2);
    const auto ha = hull(a, zh, height);
    const auto hb = hull(b, zh, height);
    const auto span = WalkBox{vm::min(ha.min, hb.min), vm::max(ha.max, hb.max)};
    if (!isFree(span, ca.solids) || !isFree(span, cb.solids))
    {
      return false;
    }
    const auto zl = std::min(z1, z2);
    if (zh - zl > WalkEpsilon)
    {
      const auto lower = z1 < z2 ? a : b;
      const auto& solids = z1 < z2 ? ca.solids : cb.solids;
      auto box = hull(lower, zl, height);
      box.max[2] = zh + height;
      return isFree(box, solids);
    }
    return true;
  }

  /** Whether a step from z1 to z2 is within the given limits. */
  static bool inReach(
    const double z1, const double z2, const double step, const double drop)
  {
    return z2 - z1 <= step + 1e-6 && z1 - z2 <= drop + 1e-6;
  }

private:
  /**
   * The highest top of the brush under the footprint, sampled on a 3x3 grid inside the
   * footprint and at the brush's vertices inside it. A top that is missed can only make
   * the hull collide with the brush, so the model stays conservative.
   */
  std::optional<double> solidTop(const WalkSolid& solid, const WalkBox& footprint) const
  {
    if (solid.box)
    {
      return solid.bounds.max.z();
    }
    auto top = std::optional<double>{};
    const auto sample = [&](const double x, const double y) {
      if (const auto z = solidTopAt(solid, x, y))
      {
        top = top ? std::max(*top, *z) : *z;
      }
    };
    for (size_t v = 0; v < 3; ++v)
    {
      for (size_t u = 0; u < 3; ++u)
      {
        const auto fu = double(u) / 2.0;
        const auto fv = double(v) / 2.0;
        sample(
          (footprint.min.x() + WalkEpsilon) * (1.0 - fu)
            + (footprint.max.x() - WalkEpsilon) * fu,
          (footprint.min.y() + WalkEpsilon) * (1.0 - fv)
            + (footprint.max.y() - WalkEpsilon) * fv);
      }
    }
    for (const auto& vertex : solid.vertices)
    {
      if (
        vertex.x() > footprint.min.x() && vertex.x() < footprint.max.x()
        && vertex.y() > footprint.min.y() && vertex.y() < footprint.max.y())
      {
        sample(vertex.x(), vertex.y());
      }
    }
    return top;
  }
};

/**
 * The largest height in [0, max] for which isFree holds, to 1/1000 of a unit. The boxes
 * are shrunk by WalkEpsilon, which is taken off again.
 */
double freeHeight(const std::function<bool(double)>& isFree, const double max)
{
  if (isFree(max))
  {
    return max;
  }
  auto lo = 0.0;
  auto hi = max;
  for (size_t i = 0; i < 32; ++i)
  {
    const auto mid = (lo + hi) / 2.0;
    (isFree(mid) ? lo : hi) = mid;
  }
  return std::max(0.0, std::round((lo - WalkEpsilon) * 1000.0) / 1000.0);
}

/** The height of the box that tells headroom (free near the floor) from a wall. */
double probeHeight(const WalkGraph::Params& params)
{
  return std::min(params.height, std::max(params.step, 1.0));
}

struct WalkSnap
{
  std::optional<uint32_t> position;
  // the floor under the point, also if the hull does not fit on it
  std::optional<vm::vec3d> standingPoint;
  // empty if the hull fits
  QString reason;
  QString message;
  std::optional<double> clearance;
};

/**
 * Moves the point down to the highest floor in its column at or below it. If the hull
 * does not fit there but it fits on a higher floor at most step_height above the point,
 * such as the edge of the next step of stairs that are narrower than the hull, the point
 * is moved up to the lowest such floor instead, because the player rests on that edge.
 */
WalkSnap snapToFloor(WalkGraph& graph, const vm::vec3d& point)
{
  const auto column = graph.columnAt(point);
  if (!column)
  {
    return {{}, {}, "outside_bounds", "the point is outside the bounds", {}};
  }
  for (size_t s = 0; s < graph.solidCount(); ++s)
  {
    if (brushStrictlyContains(*graph.solid(s).brush, point))
    {
      return {{}, {}, "inside_solid", "the point is inside a solid brush", {}};
    }
  }

  const auto& c = graph.column(*column);
  auto floor = std::optional<size_t>{};
  for (size_t k = 0; k < c.floors.size() && c.floors[k] <= point.z() + WalkEpsilon; ++k)
  {
    floor = k;
  }
  if (!floor || c.positions[*floor] < 0)
  {
    for (auto k = floor ? *floor + 1 : size_t(0);
         k < c.floors.size() && c.floors[k] <= point.z() + graph.params().step + 1e-6;
         ++k)
    {
      if (c.positions[k] >= 0)
      {
        return {uint32_t(c.positions[k]), graph.point(*column, c.floors[k]), {}, {}, {}};
      }
    }
  }
  if (!floor)
  {
    return {{}, {}, "no_floor", "there is no floor below the point in the bounds", {}};
  }

  const auto z = c.floors[*floor];
  const auto standingPoint = graph.point(*column, z);
  if (c.positions[*floor] >= 0)
  {
    return {uint32_t(c.positions[*floor]), standingPoint, {}, {}, {}};
  }

  const auto& params = graph.params();
  const auto clearance = freeHeight(
    [&](const double h) { return graph.isFree(graph.hull(*column, z, h), c.solids); },
    params.height);
  if (graph.isFree(graph.hull(*column, z, probeHeight(params)), c.solids))
  {
    return {
      {},
      standingPoint,
      "no_headroom",
      QString{"only %1 of the %2 units of hull height are free above the floor"}
        .arg(canon(clearance))
        .arg(canon(params.height)),
      clearance};
  }
  return {
    {},
    standingPoint,
    "blocked",
    "a wall or obstacle next to the point does not leave room for the hull's width",
    clearance};
}

/** Parents of the visited positions: -1 for the root, -2 if not visited. */
using WalkParents = std::vector<int32_t>;

/**
 * Breadth first search over the positions from the root within the step and drop limits.
 * Records a failure for every move into a column that has no floor in reach.
 */
WalkParents walkSearch(
  WalkGraph& graph, const uint32_t root, std::vector<WalkFailure>& failures)
{
  const auto& params = graph.params();
  auto parents = WalkParents(graph.positions().size(), -2);
  parents[root] = -1;
  auto queue = std::vector<uint32_t>{root};
  for (size_t head = 0; head < queue.size(); ++head)
  {
    const auto p = queue[head];
    const auto [a, z1] = graph.positions()[p];
    for (size_t d = 0; d < WalkDirections.size(); ++d)
    {
      const auto b = graph.neighbour(a, d);
      if (!b)
      {
        failures.push_back({p, std::nullopt});
        continue;
      }

      const auto& cb = graph.column(*b);
      parents.resize(graph.positions().size(), -2);
      auto moved = false;
      for (size_t k = 0; k < cb.floors.size(); ++k)
      {
        const auto q = cb.positions[k];
        const auto z2 = cb.floors[k];
        if (
          q >= 0 && WalkGraph::inReach(z1, z2, params.step, params.drop)
          && graph.canMove(a, z1, *b, z2, params.height))
        {
          moved = true;
          if (parents[size_t(q)] == -2)
          {
            parents[size_t(q)] = int32_t(p);
            queue.push_back(uint32_t(q));
          }
        }
      }
      if (!moved)
      {
        failures.push_back({p, *b});
      }
    }
  }
  parents.resize(graph.positions().size(), -2);
  return parents;
}

/** How far a move from z1 to z2 exceeds the step or drop limit, 0 if it does not. */
double walkViolation(const WalkGraph::Params& params, const double z1, const double z2)
{
  return std::max({0.0, z2 - z1 - params.step, z1 - z2 - params.drop});
}

/**
 * Searches the positions from the root without step and drop limits for the paths whose
 * largest violation of the limits is smallest, and among those the ones with the fewest
 * moves (Dijkstra on the pair). Ties are broken by position index.
 */
WalkParents walkMinimaxSearch(WalkGraph& graph, const uint32_t root)
{
  using Entry = std::tuple<double, uint32_t, uint32_t>;
  const auto& params = graph.params();
  auto parents = WalkParents(graph.positions().size(), -2);
  auto best = std::vector<std::pair<double, uint32_t>>(
    graph.positions().size(), {std::numeric_limits<double>::max(), 0});
  auto done = std::vector<bool>(graph.positions().size(), false);
  auto queue = std::priority_queue<Entry, std::vector<Entry>, std::greater<>>{};

  parents[root] = -1;
  best[root] = {0.0, 0};
  queue.emplace(0.0, 0, root);
  while (!queue.empty())
  {
    const auto [violation, moves, p] = queue.top();
    queue.pop();
    if (done[p])
    {
      continue;
    }
    done[p] = true;

    const auto [a, z1] = graph.positions()[p];
    for (size_t d = 0; d < WalkDirections.size(); ++d)
    {
      const auto b = graph.neighbour(a, d);
      if (!b)
      {
        continue;
      }
      const auto& cb = graph.column(*b);
      const auto size = graph.positions().size();
      parents.resize(size, -2);
      best.resize(size, {std::numeric_limits<double>::max(), 0});
      done.resize(size, false);
      for (size_t k = 0; k < cb.floors.size(); ++k)
      {
        const auto q = cb.positions[k];
        const auto z2 = cb.floors[k];
        if (q < 0 || done[size_t(q)] || !graph.canMove(a, z1, *b, z2, params.height))
        {
          continue;
        }
        const auto cost =
          std::pair{std::max(violation, walkViolation(params, z1, z2)), moves + 1};
        if (cost < best[size_t(q)])
        {
          best[size_t(q)] = cost;
          parents[size_t(q)] = int32_t(p);
          queue.emplace(cost.first, cost.second, uint32_t(q));
        }
      }
    }
  }
  parents.resize(graph.positions().size(), -2);
  return parents;
}

/** The positions from the root to the given position. */
std::vector<uint32_t> walkPathTo(const WalkParents& parents, const uint32_t position)
{
  auto result = std::vector<uint32_t>{};
  for (auto p = int32_t(position); p >= 0; p = parents[size_t(p)])
  {
    result.push_back(uint32_t(p));
  }
  std::ranges::reverse(result);
  return result;
}

bool isReached(const WalkParents& parents, const size_t position)
{
  return position < parents.size() && parents[position] != -2;
}

int blockerPriority(const QString& kind)
{
  return kind == u"headroom" ? 0 : kind == u"step_too_high" ? 1 : 2;
}

/** Why the move from the failure's position into its column is not possible. */
WalkBlocker classifyFailure(WalkGraph& graph, const WalkFailure& failure)
{
  const auto& params = graph.params();
  const auto [a, z1] = graph.positions()[failure.position];
  if (!failure.column)
  {
    return {
      "gap",
      graph.point(a, z1),
      {{"reason", "bounds"},
       {"message", "the search reached the edge of the bounds; pass larger bounds"}}};
  }

  const auto b = *failure.column;
  const auto& cb = graph.column(b);
  const auto probe = probeHeight(params);

  // the floors in reach, nearest to z1 first
  auto inReach = std::vector<size_t>{};
  for (size_t k = 0; k < cb.floors.size(); ++k)
  {
    if (WalkGraph::inReach(z1, cb.floors[k], params.step, params.drop))
    {
      inReach.push_back(k);
    }
  }
  std::ranges::stable_sort(inReach, [&](const auto l, const auto r) {
    return std::abs(cb.floors[l] - z1) < std::abs(cb.floors[r] - z1);
  });

  auto obstacle = false;
  for (const auto k : inReach)
  {
    const auto z2 = cb.floors[k];
    const auto fits = cb.positions[k] >= 0;
    const auto isFree = [&](const double h) {
      return fits ? graph.canMove(a, z1, b, z2, h)
                  : graph.isFree(graph.hull(b, z2, h), cb.solids);
    };
    if (!isFree(probe))
    {
      obstacle = true;
      continue;
    }
    const auto clearance = freeHeight(isFree, params.height);
    return {
      "headroom",
      graph.point(b, z2),
      {{"clearance", clearance},
       {"hull_height", canon(params.height)},
       {"message",
        QString{"only %1 of the %2 units of hull height are free %3"}
          .arg(canon(clearance))
          .arg(canon(params.height))
          .arg(fits ? "along the move" : "above the floor")}}};
  }

  auto above = std::optional<double>{};
  auto below = std::optional<double>{};
  for (const auto z2 : cb.floors)
  {
    if (z2 > z1 + params.step + 1e-6 && !above)
    {
      above = z2;
    }
    if (z2 < z1 - params.drop - 1e-6)
    {
      below = z2;
    }
  }

  if (above || obstacle)
  {
    auto details = QJsonObject{{"step_height", canon(params.step)}};
    if (above)
    {
      details["rise"] = canon(*above - z1);
    }
    details["message"] =
      obstacle ? QString{"a wall or obstacle blocks the way"}
               : QString{"the floor rises by %1 units, more than step_height %2"}
                   .arg(canon(*above - z1))
                   .arg(canon(params.step));
    return {"step_too_high", graph.point(b, above ? *above : z1), details};
  }
  if (below)
  {
    return {
      "gap",
      graph.point(b, *below),
      {{"drop", canon(z1 - *below)},
       {"max_drop", canon(params.drop)},
       {"message",
        QString{"the floor drops by %1 units, more than max_drop %2"}
          .arg(canon(z1 - *below))
          .arg(canon(params.drop))}}};
  }
  return {
    "gap",
    graph.point(b, z1),
    {{"reason", "no_floor"}, {"message", "there is no floor to walk on"}}};
}

QJsonObject blockerJson(const WalkBlocker& blocker)
{
  return {
    {"kind", blocker.kind}, {"at", toJson(blocker.at)}, {"details", blocker.details}};
}

McpResult<QJsonObject> checkWalkable(McpCallContext& context, const QJsonObject& args)
{
  auto& map = context.doc.map;

  const auto hullArg = args.value("hull").toObject();
  const auto params = WalkGraph::Params{
    .width = hullArg.value("width").toDouble(DefaultHullWidth),
    .height = hullArg.value("height").toDouble(DefaultHullHeight),
    .step = args.value("step_height").toDouble(DefaultStepHeight),
    .drop = args.value("max_drop").toDouble(DefaultMaxDrop),
    .resolution = args.value("resolution").toDouble(DefaultWalkResolution),
  };
  if (params.resolution > params.width)
  {
    return toolError(
      "invalid_argument",
      QString{"resolution %1 is larger than the hull width %2"}
        .arg(canon(params.resolution))
        .arg(canon(params.width)),
      "pass a resolution of at most the hull width (default 8)",
      {{"pointer", "/resolution"}});
  }
  const auto maxCells = sizeArgument(args, "max_cells", size_t(DefaultWalkMaxCells));

  auto from = vec3(args.value("from"), "/from");
  if (from.is_error())
  {
    return errorOf(from);
  }
  auto targets = std::vector<vm::vec3d>{};
  const auto toArg = args.value("to").toArray();
  for (qsizetype i = 0; i < toArg.size(); ++i)
  {
    auto target = vec3(toArg[i], QString{"/to/%1"}.arg(i));
    if (target.is_error())
    {
      return errorOf(target);
    }
    targets.push_back(target.value());
  }

  auto solids = std::vector<WalkSolid>{};
  auto box = std::optional<vm::bbox3d>{};
  for (const auto* brushNode : allBrushes(map))
  {
    if (isWalkSolid(*brushNode))
    {
      solids.push_back(makeWalkSolid(brushNode->brush()));
      box = box ? vm::merge(*box, solids.back().bounds) : solids.back().bounds;
    }
  }

  if (args.contains("bounds"))
  {
    auto boundsResult = bounds(args.value("bounds"), "/bounds");
    if (boundsResult.is_error())
    {
      return errorOf(boundsResult);
    }
    box = boundsResult.value();
    if (!box->contains(from.value()))
    {
      return toolError(
        "invalid_argument",
        "from is outside the bounds",
        "pass bounds that contain from, or omit bounds",
        {{"pointer", "/from"}});
    }
  }
  else
  {
    auto merged = box ? vm::merge(*box, from.value()) : vm::bbox3d{from.value()};
    for (const auto& target : targets)
    {
      merged = vm::merge(merged, target);
    }
    const auto margin = params.width / 2.0 + params.resolution;
    box = vm::bbox3d{
      merged.min - vm::vec3d{margin, margin, params.resolution},
      merged.max + vm::vec3d{margin, margin, params.height}};
  }

  const auto origin = vm::vec2d{from.value().x(), from.value().y()};
  const auto nx = walkColumnCount(*box, origin, params.resolution, 0);
  const auto ny = walkColumnCount(*box, origin, params.resolution, 1);
  // the hull boxes reach up to a hull height above the floors, so thin bounds still cost
  // at least that many levels per column
  const auto nz = std::max(
    1.0,
    std::ceil(
      std::max(box->max.z() - box->min.z(), params.height) / params.resolution - 1e-9));
  const auto totalCells = double(nx) * double(ny) * nz;
  if (totalCells > double(maxCells))
  {
    return toolError(
      "invalid_argument",
      QString{"checking these bounds at resolution %1 needs %2 cells, more than "
              "max_cells (%3)"}
        .arg(canon(params.resolution))
        .arg(QString::number(totalCells, 'g', 6))
        .arg(maxCells),
      "raise resolution (e.g. 16), pass bounds around from and the targets, or raise "
      "max_cells (at most 16000000)",
      {{"pointer", "/resolution"},
       {"cells", totalCells},
       {"max_cells", qint64(maxCells)}});
  }

  auto graph = WalkGraph{params, *box, origin, std::move(solids)};

  const auto start = snapToFloor(graph, from.value());
  if (!start.position)
  {
    auto details = QJsonObject{{"pointer", "/from"}, {"reason", start.reason}};
    if (start.standingPoint)
    {
      details["standing_point"] = toJson(*start.standingPoint);
    }
    if (start.clearance)
    {
      details["clearance"] = *start.clearance;
    }
    return toolError(
      "invalid_argument",
      QString{"from is not a standable position: %1"}.arg(start.message),
      "pass the player's feet position on a floor (z of the floor surface) with room for "
      "the hull",
      details);
  }

  auto failures = std::vector<WalkFailure>{};
  const auto parents = walkSearch(graph, *start.position, failures);
  // the way that exceeds the step and drop limits the least, computed when needed
  auto relaxed = std::optional<WalkParents>{};

  const auto fromPoint = *start.standingPoint;
  const auto& positions = graph.positions();
  auto reachedCount = size_t(0);
  for (size_t p = 0; p < parents.size(); ++p)
  {
    reachedCount += isReached(parents, p) ? 1 : 0;
  }

  auto targetsJson = QJsonArray{};
  auto reachableCount = 0;
  for (size_t t = 0; t < targets.size(); ++t)
  {
    const auto& target = targets[t];
    const auto snap = snapToFloor(graph, target);
    auto json = QJsonObject{
      {"target", toJson(target)},
      {"standing_point", snap.standingPoint ? toJson(*snap.standingPoint) : QJsonValue{}},
    };

    if (snap.standingPoint && target.z() - snap.standingPoint->z() > params.step)
    {
      context.warnings.push_back(
        QString{"target %1 is %2 units above its floor and was moved down to it"}
          .arg(t)
          .arg(canon(target.z() - snap.standingPoint->z())));
    }

    if (snap.position && isReached(parents, *snap.position))
    {
      ++reachableCount;
      const auto path = walkPathTo(parents, *snap.position);
      auto centers = std::vector<vm::vec3d>{};
      auto length = 0.0;
      for (size_t i = 1; i < path.size(); ++i)
      {
        centers.push_back(graph.point(positions[path[i]]));
        length += vm::length(
          graph.point(positions[path[i]]) - graph.point(positions[path[i - 1]]));
      }
      auto pathJson = QJsonArray{};
      for (const auto& point : leakPath(fromPoint, centers))
      {
        pathJson.append(toJson(point));
      }
      json["reachable"] = true;
      json["path"] = pathJson;
      json["path_length"] = canon(length);
      targetsJson.append(json);
      continue;
    }

    json["reachable"] = false;

    // the reachable position nearest to the target
    auto nearest = size_t(*start.position);
    auto nearestDistance = vm::length(graph.point(positions[nearest]) - target);
    for (size_t p = 0; p < parents.size(); ++p)
    {
      const auto distance = vm::length(graph.point(positions[p]) - target);
      if (isReached(parents, p) && distance < nearestDistance - 1e-9)
      {
        nearest = p;
        nearestDistance = distance;
      }
    }
    json["nearest_reachable_point"] = toJson(graph.point(positions[nearest]));
    json["distance"] = canon(nearestDistance);

    auto blocker = std::optional<WalkBlocker>{};
    if (!snap.position)
    {
      auto details = QJsonObject{{"reason", snap.reason}, {"message", snap.message}};
      if (snap.clearance)
      {
        details["clearance"] = *snap.clearance;
      }
      blocker =
        WalkBlocker{"not_standable", snap.standingPoint.value_or(target), details};
    }
    else
    {
      // the failed moves from the reachable positions nearest to the target into
      // columns that are not reachable at all
      // by kind, then by distance to the target, then in search order
      auto best = std::optional<std::tuple<int, double, size_t>>{};
      const auto unreached = [&](const WalkFailure& failure) {
        if (!failure.column)
        {
          return true;
        }
        const auto& c = graph.column(*failure.column);
        return std::ranges::none_of(c.positions, [&](const auto q) {
          return q >= 0 && isReached(parents, size_t(q));
        });
      };
      auto minDistance = std::numeric_limits<double>::max();
      for (const auto& failure : failures)
      {
        if (unreached(failure))
        {
          minDistance = std::min(
            minDistance, vm::length(graph.point(positions[failure.position]) - target));
        }
      }
      for (size_t f = 0; f < failures.size(); ++f)
      {
        const auto& failure = failures[f];
        if (
          !unreached(failure)
          || vm::length(graph.point(positions[failure.position]) - target)
               > minDistance + 1e-6)
        {
          continue;
        }
        auto candidate = classifyFailure(graph, failure);
        const auto key = std::tuple{
          blockerPriority(candidate.kind), vm::length(candidate.at - target), f};
        if (!best || key < *best)
        {
          best = key;
          blocker = std::move(candidate);
        }
      }

      // Unless the nearest failure is a lack of headroom, the first move that is too high
      // or too deep on the way that exceeds the limits the least names the blocker.
      if (!blocker || blocker->kind != u"headroom")
      {
        if (!relaxed)
        {
          relaxed = walkMinimaxSearch(graph, *start.position);
        }
        if (isReached(*relaxed, *snap.position))
        {
          const auto path = walkPathTo(*relaxed, *snap.position);
          for (size_t i = 1; i < path.size(); ++i)
          {
            const auto& p = positions[path[i - 1]];
            const auto& q = positions[path[i]];
            if (!WalkGraph::inReach(p.z, q.z, params.step, params.drop))
            {
              const auto rise = q.z - p.z;
              auto details = QJsonObject{{"from", toJson(graph.point(p))}};
              if (rise > 0)
              {
                details["rise"] = canon(rise);
                details["step_height"] = canon(params.step);
                details["message"] =
                  QString{"the floor rises by %1 units, more than step_height %2"}
                    .arg(canon(rise))
                    .arg(canon(params.step));
              }
              else
              {
                details["drop"] = canon(-rise);
                details["max_drop"] = canon(params.drop);
                details["message"] =
                  QString{"the floor drops by %1 units, more than max_drop %2"}
                    .arg(canon(-rise))
                    .arg(canon(params.drop));
              }
              blocker =
                WalkBlocker{rise > 0 ? "step_too_high" : "gap", graph.point(q), details};
              break;
            }
          }
        }
      }
    }

    json["blocker"] = blocker ? QJsonValue{blockerJson(*blocker)} : QJsonValue{};
    targetsJson.append(json);
  }

  return QJsonObject{
    {"from",
     QJsonObject{{"point", toJson(from.value())}, {"standing_point", toJson(fromPoint)}}},
    {"targets", targetsJson},
    {"reachable_count", reachableCount},
    {"unreachable_count", qint64(targets.size()) - reachableCount},
    {"model",
     QJsonObject{
       {"hull",
        QJsonObject{{"width", canon(params.width)}, {"height", canon(params.height)}}},
       {"step_height", canon(params.step)},
       {"max_drop", canon(params.drop)},
       {"resolution", canon(params.resolution)},
     }},
    {"bounds", toJson(*box)},
    {"stats",
     QJsonObject{
       {"solid_brushes", qint64(graph.solidCount())},
       {"grid", QJsonArray{qint64(nx), qint64(ny), qint64(nz)}},
       {"cells", totalCells},
       {"columns_evaluated", qint64(graph.columnsEvaluated())},
       {"positions_found", qint64(positions.size())},
       {"positions_reachable", qint64(reachedCount)},
     }},
  };
}

McpToolDef checkWalkableTool()
{
  return {
    .name = "check_walkable",
    .title = "Check Walkable",
    .description =
      "Can the player walk from 'from' to each point in 'to'? All values are world "
      "units; points are FEET positions (z of the floor surface; a point above a floor "
      "is moved down to it; if the hull does not fit there, it is moved up by at most "
      "step_height onto a higher floor the hull rests on, such as the next stair's "
      "edge; see standing_point). Model (conservative and deterministic): solid = "
      "world brushes incl. grouped ones plus func_group, func_detail* and func_wall "
      "brushes "
      "(doors, triggers and other brush entities count as open; layers omitted from "
      "export are ignored; materials are not considered). Positions are the hull "
      "(width x width x height, default 32x32x56) standing on a brush top on a grid of "
      "resolution spaced columns through 'from' with no solid inside the hull. The "
      "player moves to the 4 neighbour columns if the floor rises by at most "
      "step_height (18) or drops by at most max_drop (256) and the hull is free along "
      "the move; no jumping, crouching, swimming or ladders, and slopes count as "
      "steps. A reachable target has a path (at most 64 corner points) and "
      "path_length; an unreachable one has nearest_reachable_point, distance and a "
      "blocker {kind: headroom (clearance), step_too_high (rise), gap (drop or no "
      "floor) or not_standable (reason inside_solid, no_floor, no_headroom, blocked, "
      "outside_bounds), at, details}. Errors if 'from' itself is not standable. "
      "Verify a fix by calling it again; query_point and describe_region show what is "
      "at 'at'.",
    .inputSchema = schema::object(
      {
        {"from", schema::vec3("the player's feet position to start from")},
        {"to",
         schema::array(schema::vec3(), 1, MaxWalkTargets, "feet positions to reach")},
        {"hull",
         schema::object({
           {"width", schema::number(1, 1024, "hull width and depth (default 32)")},
           {"height", schema::number(1, 1024, "hull height (default 56)")},
         })},
        {"step_height",
         schema::number(0, 1024, "highest step the player climbs (default 18)")},
        {"max_drop",
         schema::number(0, 65536, "deepest drop the player takes (default 256)")},
        {"resolution",
         schema::number(
           1, MaxWalkResolution, "column spacing, at most the hull width (default 8)")},
        {"bounds",
         schema::bounds(
           "the region to search (default: all solid brushes, from and the targets plus "
           "a margin)")},
        {"max_cells",
         schema::integer(
           1,
           MaxWalkMaxCells,
           "most columns x height levels of resolution size, counting at least the "
           "hull height (default 4000000, at most 16000000)")},
      },
      {"from", "to"}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = checkWalkable,
  };
}

} // namespace

void registerMcpMapEvalTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(validateMapTool());
  dispatcher.add(getMapStatsTool());
  dispatcher.add(checkpointCreateTool());
  dispatcher.add(checkpointDiffTool());
  dispatcher.add(checkpointListTool());
  dispatcher.add(checkpointDeleteTool());
  dispatcher.add(exportMapTextTool());
  dispatcher.add(queryPointTool());
  dispatcher.add(checkBrushOverlapsTool());
  dispatcher.add(checkLeaksTool());
  dispatcher.add(checkWalkableTool());
}

} // namespace tb::ui
