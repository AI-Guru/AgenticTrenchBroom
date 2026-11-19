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

#include "gl/MaterialManager.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/MapTextEncoding.h"
#include "mdl/Map_World.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/SoftMapBounds.h"
#include "mdl/WorldNode.h"
#include "ui/McpJson.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

#include "kd/overload.h"

#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace tb::ui
{
namespace
{

constexpr auto MaxGetNodes = 200;
constexpr auto MaxGetBrushGeometry = 100;

const auto NodeTypes = QStringList{"world", "layer", "group", "entity", "brush", "patch"};

const auto ReadOnly =
  mcp::ToolAnnotations{.readOnly = true, .destructive = false, .idempotent = true};

QJsonValue stringOrNull(const std::string* str)
{
  return str ? QJsonValue{QString::fromStdString(*str)} : QJsonValue{};
}

QJsonValue idOrNull(const McpNodeRegistry& registry, const mdl::Node* node)
{
  return node ? QJsonValue{registry.idOf(*node)} : QJsonValue{};
}

QJsonArray idArray(const McpNodeRegistry& registry, const auto& nodes)
{
  auto result = QJsonArray{};
  for (const auto* node : nodes)
  {
    result.append(registry.idOf(*node));
  }
  return result;
}

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
void visitPreOrder(
  const mdl::Node& node,
  size_t depth,
  const std::function<bool(const mdl::Node&, size_t)>& visitor)
{
  if (visitor(node, depth))
  {
    for (const auto* child : node.children())
    {
      visitPreOrder(*child, depth + 1, visitor);
    }
  }
}

bool isGeometry(const mdl::Node& node)
{
  return dynamic_cast<const mdl::BrushNode*>(&node)
         || dynamic_cast<const mdl::PatchNode*>(&node);
}

QJsonObject brushValidity(const mdl::Map& map, const mdl::Brush& brush)
{
  const auto vertices = brush.vertexPositions();
  const auto integerVertices = std::ranges::all_of(vertices, [](const auto& v) {
    for (size_t i = 0; i < 3; ++i)
    {
      if (std::abs(v[i] - std::round(v[i])) > 1e-6)
      {
        return false;
      }
    }
    return true;
  });

  return {
    {"closed", brush.closed()},
    {"fully_specified", brush.fullySpecified()},
    {"integer_vertices", integerVertices},
    {"within_world_bounds", map.worldBounds().contains(brush.bounds())},
  };
}

QJsonArray sortedVertices(const std::vector<vm::vec3d>& positions)
{
  auto canonical = std::vector<std::array<double, 3>>{};
  canonical.reserve(positions.size());
  for (const auto& p : positions)
  {
    canonical.push_back({canon(p.x()), canon(p.y()), canon(p.z())});
  }
  std::ranges::sort(canonical);

  auto result = QJsonArray{};
  for (const auto& p : canonical)
  {
    result.append(QJsonArray{p[0], p[1], p[2]});
  }
  return result;
}

QJsonArray vertexArray(const std::vector<vm::vec3d>& positions)
{
  auto result = QJsonArray{};
  for (const auto& p : positions)
  {
    result.append(toJson(p));
  }
  return result;
}

QJsonArray entityProperties(const mdl::Map& map, const mdl::Entity& entity)
{
  auto result = QJsonArray{};
  for (const auto& property : entity.properties())
  {
    result.append(QJsonObject{
      {"key", toUnicode(map, property.key())},
      {"value", toUnicode(map, property.value())},
    });
  }
  return result;
}

QJsonObject surfaceJson(const mdl::SurfaceAttributes& surface)
{
  auto result = QJsonObject{};
  if (surface.contents)
  {
    result["contents"] = *surface.contents;
  }
  if (surface.flags)
  {
    result["flags"] = *surface.flags;
  }
  if (surface.value)
  {
    result["value"] = canon(double(*surface.value));
  }
  return result;
}

QString formatJsonName(const mdl::Map& map)
{
  return QString::fromStdString(mdl::formatName(map.worldNode().mapFormat()));
}

// get_map_info

McpResult<QJsonObject> getMapInfo(McpCallContext& context, const QJsonObject&)
{
  const auto& doc = context.doc;
  auto& map = doc.map;
  const auto& registry = doc.nodes;
  const auto& editorContext = map.editorContext();

  auto layers = 0;
  auto groups = 0;
  auto entities = 0;
  auto brushes = 0;
  auto patches = 0;
  visitPreOrder(map.worldNode(), 0, [&](const mdl::Node& node, size_t) {
    node.accept(kdl::overload(
      [](const mdl::WorldNode&) {},
      [&](const mdl::LayerNode&) { ++layers; },
      [&](const mdl::GroupNode&) { ++groups; },
      [&](const mdl::EntityNode&) { ++entities; },
      [&](const mdl::BrushNode&) { ++brushes; },
      [&](const mdl::PatchNode&) { ++patches; }));
    return true;
  });

  const auto softBounds = mdl::softMapBounds(map);
  const auto& selection = map.selection();
  const auto& selectionBounds = map.selectionBounds();

  return QJsonObject{
    {"document", doc.documentId},
    {"game", QString::fromStdString(map.gameInfo().gameConfig.name)},
    {"format", formatJsonName(map)},
    {"path",
     map.persistent() ? QJsonValue{QString::fromStdString(map.path().generic_string())}
                      : QJsonValue{}},
    {"persistent", map.persistent()},
    {"modified", map.modified()},
    {"modification_count", qint64(map.modificationCount())},
    {"encoding", map.encoding() == mdl::MapTextEncoding::Quake ? "quake" : "utf8"},
    {"world_bounds", toJson(map.worldBounds())},
    {"soft_map_bounds",
     QJsonObject{
       {"source", softBounds.source == mdl::SoftMapBoundsType::Game ? "game" : "map"},
       {"bounds",
        softBounds.bounds ? QJsonValue{toJson(*softBounds.bounds)} : QJsonValue{}},
     }},
    {"grid_size", canon(map.grid().actualSize())},
    {"uv_lock", editorContext.uvLock()},
    {"current_layer", idOrNull(registry, editorContext.currentLayer())},
    {"current_group", idOrNull(registry, editorContext.currentGroup())},
    {"current_material", toUnicode(map, map.currentMaterialName())},
    {"can_undo", canUndo(map)},
    {"undo_label", stringOrNull(map.undoCommandName())},
    {"can_redo", canRedo(map)},
    {"redo_label", stringOrNull(map.redoCommandName())},
    {"transaction_active", map.commandProcessor().isTransactionActive()},
    {"selection",
     QJsonObject{
       {"node_count", qint64(selection.nodes.size())},
       {"face_count", qint64(selection.brushFaces.size())},
       {"bounds", selectionBounds ? QJsonValue{toJson(*selectionBounds)} : QJsonValue{}},
     }},
    {"counts",
     QJsonObject{
       {"layers", layers},
       {"groups", groups},
       {"entities", entities},
       {"brushes", brushes},
       {"patches", patches},
     }},
    {"material_count", qint64(map.materialManager().materials().size())},
    {"entity_definition_count",
     qint64(map.entityDefinitionManager().definitions().size())},
  };
}

McpToolDef getMapInfoTool()
{
  return {
    .name = "get_map_info",
    .title = "Get Map Info",
    .description =
      "Returns an overview of the current map: game, format, file, world and soft map "
      "bounds, grid size, current layer/group/material, undo and redo state, the "
      "selection and node counts. Call it first to orient yourself.",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = getMapInfo,
  };
}

// get_scene_tree

McpResult<QJsonObject> getSceneTree(McpCallContext& context, const QJsonObject& args)
{
  const auto& doc = context.doc;
  const auto& map = doc.map;
  const auto& registry = doc.nodes;

  auto root = resolveNode(doc, args.value("root").toString("world"));
  if (root.is_error())
  {
    return errorOf(root);
  }

  const auto maxDepth = sizeArgument(args, "max_depth", 3);
  const auto includeBrushes = args.value("include_brushes").toBool(false);

  auto types = std::unordered_set<QString>{};
  for (const auto& type : args.value("types").toArray())
  {
    types.insert(type.toString());
  }

  const auto listed = [&](const mdl::Node& node) {
    const auto type = nodeTypeName(node);
    if (isGeometry(node) && !includeBrushes && !types.contains(type))
    {
      return false;
    }
    return types.empty() || types.contains(type);
  };

  const auto traversed = [&](const mdl::Node& node) {
    return !isGeometry(node) || includeBrushes || types.contains(nodeTypeName(node));
  };

  auto nodes = QJsonArray{};
  visitPreOrder(*root.value(), 0, [&](const mdl::Node& node, const size_t depth) {
    if (!traversed(node))
    {
      return false;
    }

    if (listed(node))
    {
      auto summary = nodeSummary(registry, map, node);
      summary["depth"] = qint64(depth);

      if (!includeBrushes && node.hasChildren())
      {
        auto brushCount = 0;
        auto patchCount = 0;
        for (const auto* child : node.children())
        {
          brushCount += dynamic_cast<const mdl::BrushNode*>(child) ? 1 : 0;
          patchCount += dynamic_cast<const mdl::PatchNode*>(child) ? 1 : 0;
        }
        summary["brush_count"] = brushCount;
        summary["patch_count"] = patchCount;
      }

      if (depth == maxDepth && node.hasChildren())
      {
        auto childIds = QJsonArray{};
        for (const auto* child : node.children())
        {
          if (traversed(*child))
          {
            childIds.append(registry.idOf(*child));
          }
        }
        summary["child_ids"] = childIds;
      }

      nodes.append(summary);
    }
    return depth < maxDepth;
  });

  auto result = QJsonObject{};
  result["nodes"] = page(nodes, args, result);
  return result;
}

McpToolDef getSceneTreeTool()
{
  auto properties = QJsonObject{
    {"root", schema::nodeId("the node to start at; default world")},
    {"max_depth", schema::integer(0, 64, "depth below root to descend into; default 3")},
    {"include_brushes",
     schema::boolean(
       "list brushes and patches; default false, which reports brush_count and "
       "patch_count per container instead")},
    {"types",
     schema::array(
       schema::stringEnum(NodeTypes),
       1,
       6,
       "only list nodes of these types; brush and patch imply include_brushes")},
  };
  schema::addPaging(properties);

  return {
    .name = "get_scene_tree",
    .title = "Get Scene Tree",
    .description =
      "Lists the node tree as a flat DFS pre-order list of node summaries with their "
      "depth below root. Nodes at max_depth list their child_ids so you can descend "
      "further with root. Use get_nodes for details.",
    .inputSchema = schema::object(properties),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = getSceneTree,
  };
}

// get_nodes

QJsonObject nodeDetail(
  const McpNodeRegistry& registry, mdl::Map& map, const mdl::Node& node)
{
  auto result = nodeSummary(registry, map, node);
  result["logical_bounds"] = toJson(node.logicalBounds());
  result["physical_bounds"] = toJson(node.physicalBounds());
  result["line_number"] = qint64(node.lineNumber());
  result["child_ids"] = idArray(registry, node.children());

  node.accept(kdl::overload(
    [&](const mdl::WorldNode& worldNode) {
      result["properties"] = entityProperties(map, worldNode.entity());
    },
    [&](const mdl::LayerNode& layerNode) {
      const auto& layer = layerNode.layer();
      result["sort_index"] = layer.sortIndex();
      result["is_default"] = layerNode.isDefaultLayer();
      result["omit_from_export"] = layer.omitFromExport();
      result["hidden"] = layerNode.hidden();
      result["locked"] = layerNode.locked();
    },
    [&](const mdl::GroupNode& groupNode) {
      const auto& transformation = groupNode.group().transformation();
      auto matrix = QJsonArray{};
      for (size_t r = 0; r < 4; ++r)
      {
        for (size_t c = 0; c < 4; ++c)
        {
          matrix.append(canon(transformation[c][r]));
        }
      }
      result["opened"] = groupNode.opened();
      result["linked"] = isLinkedGroup(map, groupNode);
      result["transformation"] = matrix;
    },
    [&](const mdl::EntityNode& entityNode) {
      const auto& entity = entityNode.entity();
      const auto* definition = entity.definition();
      auto definitionJson = QJsonObject{{"known", definition != nullptr}};
      if (definition)
      {
        definitionJson["type"] =
          mdl::getType(*definition) == mdl::EntityDefinitionType::Point ? "point"
                                                                        : "brush";
      }
      result["origin"] = toJson(entity.origin());
      result["point_entity"] = entity.pointEntity();
      result["properties"] = entityProperties(map, entity);
      result["definition"] = definitionJson;
    },
    [&](const mdl::BrushNode& brushNode) {
      result["face_count"] = qint64(brushNode.brush().faceCount());
      result["valid"] = brushValidity(map, brushNode.brush());
    },
    [&](const mdl::PatchNode& patchNode) {
      result["rows"] = qint64(patchNode.patch().pointRowCount());
      result["columns"] = qint64(patchNode.patch().pointColumnCount());
    }));

  return result;
}

McpResult<QJsonObject> getNodes(McpCallContext& context, const QJsonObject& args)
{
  const auto& doc = context.doc;
  return resolveNodes(doc, args.value("ids").toArray(), size_t(MaxGetNodes))
         | kdl::transform([&](const std::vector<mdl::Node*>& nodes) {
             auto details = QJsonArray{};
             for (const auto* node : nodes)
             {
               details.append(nodeDetail(doc.nodes, doc.map, *node));
             }
             return QJsonObject{{"nodes", details}};
           });
}

McpToolDef getNodesTool()
{
  return {
    .name = "get_nodes",
    .title = "Get Nodes",
    .description =
      "Returns details for the given nodes: summary, bounds, children and type specific "
      "fields such as entity properties (in file order), layer and group state, or "
      "brush validity. Use get_brush_geometry for vertices and faces.",
    .inputSchema = schema::object({{"ids", schema::nodeIds(MaxGetNodes)}}, {"ids"}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = getNodes,
  };
}

// get_brush_geometry

QJsonObject faceGeometry(
  const mdl::Map& map,
  const mdl::BrushFace& face,
  const size_t index,
  const bool includeVertices)
{
  const auto uv = face.uvAttributes();
  auto planePoints = QJsonArray{};
  for (const auto& point : face.points())
  {
    planePoints.append(toJson(point));
  }

  auto result = QJsonObject{
    {"index", qint64(index)},
    {"normal", toJson(face.normal())},
    {"distance", canon(face.boundary().distance)},
    {"plane_points", planePoints},
    {"center", toJson(face.center())},
    {"area", canon(face.area())},
    {"material", toUnicode(map, face.materialName())},
    {"material_found", face.material() != nullptr},
    {"uv",
     QJsonObject{
       {"offset", toJson(uv.offset)},
       {"scale", toJson(uv.scale)},
       {"rotation", canon(double(uv.rotation))},
       {"valid", uv.valid()},
     }},
  };

  if (const auto side = sideName(face.normal()))
  {
    result["side"] = *side;
  }
  if (includeVertices)
  {
    result["vertices"] = vertexArray(face.vertexPositions());
  }
  if (const auto surface = surfaceJson(face.surfaceAttributes()); !surface.isEmpty())
  {
    result["surface"] = surface;
  }
  return result;
}

McpResult<QJsonObject> getBrushGeometry(McpCallContext& context, const QJsonObject& args)
{
  const auto& doc = context.doc;
  const auto& map = doc.map;
  const auto includeVertices = args.value("include_vertices").toBool(true);
  const auto includeFaces = args.value("include_faces").toBool(true);

  auto resolved =
    resolveNodes(doc, args.value("ids").toArray(), size_t(MaxGetBrushGeometry));
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }

  auto brushes = QJsonArray{};
  for (const auto* node : resolved.value())
  {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    if (!brushNode)
    {
      const auto id = doc.nodes.idOf(*node);
      return toolError(
        "wrong_node_type",
        QString{"%1 is a %2, not a brush"}.arg(id, nodeTypeName(*node)),
        "pass brush ids, e.g. from find_nodes with types [\"brush\"]",
        {{"id", id}, {"actual", nodeTypeName(*node)}, {"expected", "brush"}});
    }

    const auto& brush = brushNode->brush();
    const auto& bounds = brush.bounds();
    auto entry = QJsonObject{
      {"id", doc.nodes.idOf(*brushNode)},
      {"entity",
       brushNode->entity() && brushNode->entity() != &map.worldNode()
         ? doc.nodes.idOf(*brushNode->entity())
         : QStringLiteral("world")},
      {"bounds", toJson(bounds)},
      {"size", toJson(bounds.size())},
      {"center", toJson(bounds.center())},
      {"volume", canon(brushVolume(brush))},
      {"vertex_count", qint64(brush.vertexCount())},
      {"edge_count", qint64(brush.edgeCount())},
      {"face_count", qint64(brush.faceCount())},
      {"valid", brushValidity(map, brush)},
    };

    if (includeVertices)
    {
      entry["vertices"] = sortedVertices(brush.vertexPositions());
    }
    if (includeFaces)
    {
      auto faces = QJsonArray{};
      for (size_t i = 0; i < brush.faceCount(); ++i)
      {
        faces.append(faceGeometry(map, brush.face(i), i, includeVertices));
      }
      entry["faces"] = faces;
    }
    brushes.append(entry);
  }

  return QJsonObject{{"brushes", brushes}};
}

McpToolDef getBrushGeometryTool()
{
  return {
    .name = "get_brush_geometry",
    .title = "Get Brush Geometry",
    .description =
      "Returns exact brush geometry: bounds, size, center, volume, sorted vertices and "
      "per face the plane, side (+x..-z), boundary vertices, area, material and UV "
      "attributes, plus validity flags. Use it to verify that brushes were built as "
      "intended.",
    .inputSchema = schema::object(
      {
        {"ids",
         schema::nodeIds(MaxGetBrushGeometry, "brush ids; \"@selection\" is allowed")},
        {"include_vertices", schema::boolean("include vertex lists; default true")},
        {"include_faces", schema::boolean("include the faces; default true")},
      },
      {"ids"}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = getBrushGeometry,
  };
}

// find_nodes

struct NodeFilter
{
  std::unordered_set<QString> types;
  std::optional<QString> classname;
  std::optional<QString> name;
  std::optional<QString> text;
  std::optional<QString> propertyKey;
  std::optional<QString> propertyValue;
  std::optional<QString> material;
  std::optional<vm::bbox3d> inside;
  std::optional<vm::bbox3d> intersects;
  const mdl::Node* under = nullptr;
  std::optional<bool> selected;
  std::optional<bool> pointEntity;
};

/**
 * Returns whether any of the strings that TrenchBroom's node index holds for the node
 * (entity property keys and values, group names, materials) matches the pattern.
 *
 * Map::findNodes is not used: its trie matcher backtracks without memoization, so a
 * pattern such as "*a*a*a*b" takes minutes on a property value of a few hundred chars.
 */
bool matchesText(const mdl::Map& map, const QString& pattern, const mdl::Node& node)
{
  const auto match = [&](const std::string& str) {
    return globMatch(pattern, toUnicode(map, str));
  };

  return node.accept(kdl::overload(
    [&](const mdl::WorldNode& worldNode) {
      return std::ranges::any_of(worldNode.entity().properties(), [&](const auto& p) {
        return match(p.key()) || match(p.value());
      });
    },
    [](const mdl::LayerNode&) { return false; },
    [&](const mdl::GroupNode& groupNode) { return match(groupNode.group().name()); },
    [&](const mdl::EntityNode& entityNode) {
      return std::ranges::any_of(entityNode.entity().properties(), [&](const auto& p) {
        return match(p.key()) || match(p.value());
      });
    },
    [&](const mdl::BrushNode& brushNode) {
      return std::ranges::any_of(brushNode.brush().faces(), [&](const auto& face) {
        return match(face.materialName());
      });
    },
    [&](const mdl::PatchNode& patchNode) {
      return match(patchNode.patch().materialName());
    }));
}

bool matches(const mdl::Map& map, const NodeFilter& filter, const mdl::Node& node)
{
  if (!filter.types.empty() && !filter.types.contains(nodeTypeName(node)))
  {
    return false;
  }

  if (filter.under && !node.isDescendantOf(*filter.under))
  {
    return false;
  }

  if (filter.text && !matchesText(map, *filter.text, node))
  {
    return false;
  }

  if (filter.selected && node.selected() != *filter.selected)
  {
    return false;
  }

  const auto* entityNodeBase = dynamic_cast<const mdl::EntityNodeBase*>(&node);
  if (filter.classname || filter.propertyKey)
  {
    if (!entityNodeBase)
    {
      return false;
    }
    const auto& entity = entityNodeBase->entity();
    if (
      filter.classname
      && !globMatch(*filter.classname, toUnicode(map, entity.classname())))
    {
      return false;
    }
    if (filter.propertyKey)
    {
      const auto* value = entity.property(fromUnicode(map, *filter.propertyKey));
      if (
        !value
        || (filter.propertyValue && !globMatch(*filter.propertyValue, toUnicode(map, *value))))
      {
        return false;
      }
    }
  }

  if (filter.pointEntity)
  {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
    if (!entityNode || entityNode->entity().pointEntity() != *filter.pointEntity)
    {
      return false;
    }
  }

  if (filter.name)
  {
    const auto* layerNode = dynamic_cast<const mdl::LayerNode*>(&node);
    const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node);
    const auto name = layerNode   ? layerNode->layer().name()
                      : groupNode ? groupNode->group().name()
                                  : std::optional<std::string>{};
    if (!name || !globMatch(*filter.name, toUnicode(map, *name)))
    {
      return false;
    }
  }

  if (filter.material)
  {
    const auto materialMatches = [&](const std::string& materialName) {
      return globMatch(*filter.material, toUnicode(map, materialName));
    };
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      if (!std::ranges::any_of(brushNode->brush().faces(), [&](const auto& face) {
            return materialMatches(face.materialName());
          }))
      {
        return false;
      }
    }
    else if (const auto* patchNode = dynamic_cast<const mdl::PatchNode*>(&node))
    {
      if (!materialMatches(patchNode->patch().materialName()))
      {
        return false;
      }
    }
    else
    {
      return false;
    }
  }

  if (filter.inside && !filter.inside->contains(node.logicalBounds()))
  {
    return false;
  }

  if (filter.intersects && !filter.intersects->intersects(node.logicalBounds()))
  {
    return false;
  }

  return true;
}

McpResult<NodeFilter> makeNodeFilter(const McpDocumentState& doc, const QJsonObject& args)
{
  auto filter = NodeFilter{};
  for (const auto& type : args.value("types").toArray())
  {
    filter.types.insert(type.toString());
  }

  const auto optionalString = [&](const char* key) {
    return args.contains(key) ? std::optional{args.value(key).toString()} : std::nullopt;
  };
  const auto optionalBool = [&](const char* key) {
    return args.contains(key) ? std::optional{args.value(key).toBool()} : std::nullopt;
  };

  filter.classname = optionalString("classname");
  filter.name = optionalString("name");
  filter.material = optionalString("material");
  filter.selected = optionalBool("selected");
  filter.pointEntity = optionalBool("point_entity");

  if (args.contains("property"))
  {
    const auto property = args.value("property").toObject();
    filter.propertyKey = property.value("key").toString();
    if (property.contains("value"))
    {
      filter.propertyValue = property.value("value").toString();
    }
  }

  filter.text = optionalString("text");

  for (const auto& key : {QStringLiteral("inside"), QStringLiteral("intersects")})
  {
    if (args.contains(key))
    {
      auto box = bounds(args.value(key), "/" + key);
      if (box.is_error())
      {
        return errorOf(box);
      }
      (key == u"inside" ? filter.inside : filter.intersects) = box.value();
    }
  }

  if (args.contains("under"))
  {
    auto under = resolveNode(doc, args.value("under").toString());
    if (under.is_error())
    {
      return errorOf(under);
    }
    filter.under = under.value();
  }

  return filter;
}

McpResult<QJsonObject> findNodes(McpCallContext& context, const QJsonObject& args)
{
  const auto& doc = context.doc;
  return makeNodeFilter(doc, args) | kdl::transform([&](const NodeFilter& filter) {
           auto ids = QJsonArray{};
           visitPreOrder(doc.map.worldNode(), 0, [&](const mdl::Node& node, size_t) {
             if (matches(doc.map, filter, node))
             {
               ids.append(doc.nodes.idOf(node));
             }
             return true;
           });

           auto result = QJsonObject{};
           result["ids"] = page(ids, args, result);
           return result;
         });
}

McpToolDef findNodesTool()
{
  const auto glob = [](const QString& description) {
    return schema::string(1, 256, description);
  };

  auto properties = QJsonObject{
    {"types", schema::array(schema::stringEnum(NodeTypes), 1, 6, "node types to find")},
    {"classname", glob("entity classname glob (* and ?, case insensitive)")},
    {"name", glob("layer or group name glob")},
    {"text",
     schema::string(
       1,
       256,
       "glob (* and ?, case insensitive) matched against entity property keys and "
       "values, group names and materials")},
    {"property",
     schema::object(
       {
         {"key", schema::string(1, 256, "the property key, which must be present")},
         {"value", glob("glob the property value must match")},
       },
       {"key"})},
    {"material", glob("brushes with any face (or patches) whose material matches")},
    {"inside", schema::bounds("nodes whose bounds are contained in these bounds")},
    {"intersects", schema::bounds("nodes whose bounds intersect these bounds")},
    {"under", schema::nodeId("only descendants of this node")},
    {"selected", schema::boolean("only selected (true) or unselected (false) nodes")},
    {"point_entity", schema::boolean("only point (true) or brush (false) entities")},
  };
  schema::addPaging(properties);

  return {
    .name = "find_nodes",
    .title = "Find Nodes",
    .description =
      "Finds nodes matching all given filters and returns their ids in DFS order. "
      "Without filters, returns every node. Use get_nodes or get_brush_geometry to "
      "inspect the results.",
    .inputSchema = schema::object(properties),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = findNodes,
  };
}

// get_selection

McpResult<QJsonObject> getSelection(McpCallContext& context, const QJsonObject&)
{
  return selectionJson(context.doc.nodes, context.doc.map);
}

McpToolDef getSelectionTool()
{
  return {
    .name = "get_selection",
    .title = "Get Selection",
    .description =
      "Returns what the user has selected: node ids, selected brush faces and the "
      "selection bounds. Pass \"@selection\" to other tools to act on the selection.",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = getSelection,
  };
}

} // namespace

void registerMcpMapReadTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(getMapInfoTool());
  dispatcher.add(getSceneTreeTool());
  dispatcher.add(getNodesTool());
  dispatcher.add(getBrushGeometryTool());
  dispatcher.add(findNodesTool());
  dispatcher.add(getSelectionTool());
}

} // namespace tb::ui
