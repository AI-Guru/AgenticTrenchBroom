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

#include <algorithm>
#include <cmath>
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
}

} // namespace tb::ui
