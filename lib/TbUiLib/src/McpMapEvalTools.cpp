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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>

namespace tb::ui
{
namespace
{

constexpr auto MaxIdList = size_t(50);
constexpr auto DefaultMaxChanges = 200;
constexpr auto MaxChanges = 5000;
constexpr auto DefaultMaxBytes = 1048576;
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
        {"line", qint64(issue->lineNumber())},
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
      "this after edits to verify the map is clean.",
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
         schema::integer(1, MaxChanges, "maximum number of entries per list (200)")},
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
       schema::integer(MinMaxBytes, MaxMaxBytes, "maximum UTF-8 size of text (1 MiB)")},
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
}

} // namespace tb::ui
