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

#include "mdl/BrushFaceHandle.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/McpJson.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

#include <algorithm>

namespace tb::ui
{
namespace
{

constexpr auto MaxFaceRefs = 1000;

// select_nodes

McpResult<QJsonObject> selectNodes(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto mode = args.value("mode").toString("replace");
  const auto hasIds = args.contains("ids");
  const auto hasFaces = args.contains("faces");

  if (mode == u"clear")
  {
    if (hasIds || hasFaces)
    {
      return toolError(
        "invalid_argument",
        "mode clear takes no ids or faces",
        "omit ids and faces",
        {{"pointer", hasIds ? "/ids" : "/faces"}});
    }
    mdl::deselectAll(map);
    return selectionJson(doc.nodes, map);
  }

  if (hasIds == hasFaces)
  {
    return toolError(
      "invalid_argument",
      "pass exactly one of ids or faces",
      "select nodes and faces in separate calls; TrenchBroom selects either nodes or "
      "faces");
  }

  const auto& selection = map.selection();
  if (hasIds)
  {
    auto nodes = resolveTargets(doc, args.value("ids").toArray());
    if (nodes.is_error())
    {
      return errorOf(nodes);
    }

    if (mode == u"add" && selection.hasBrushFaces())
    {
      return toolError(
        "invalid_argument",
        "brush faces are selected, so nodes cannot be added to the selection",
        "use mode replace");
    }

    if (mode == u"remove")
    {
      mdl::deselectNodes(map, nodes.value());
    }
    else
    {
      if (mode == u"replace")
      {
        mdl::deselectAll(map);
      }
      mdl::selectNodes(map, nodes.value());
    }
  }
  else
  {
    auto faces = resolveFaces(doc, args.value("faces").toArray());
    if (faces.is_error())
    {
      return errorOf(faces);
    }

    if (mode == u"add" && selection.hasNodes())
    {
      return toolError(
        "invalid_argument",
        "nodes are selected, so faces cannot be added to the selection",
        "use mode replace");
    }

    if (mode == u"remove")
    {
      mdl::deselectBrushFaces(map, faces.value());
    }
    else
    {
      if (mode == u"replace")
      {
        mdl::deselectAll(map);
      }
      mdl::selectBrushFaces(map, faces.value());
    }
  }

  return selectionJson(doc.nodes, map);
}

McpToolDef selectNodesTool()
{
  return {
    .name = "select_nodes",
    .title = "Select Nodes",
    .description =
      "Changes the user's selection, e.g. to show them nodes or faces. Unlike other "
      "tools, the selection is kept. Other tools take ids and do not need a selection.",
    .inputSchema = schema::object({
      {"ids", schema::nodeIds()},
      {"faces", schema::array(schema::faceRef(), 1, MaxFaceRefs)},
      {"mode",
       schema::stringEnum(
         {"replace", "add", "remove", "clear"},
         "how to change the selection (default: replace)")},
    }),
    .kind = McpToolKind::Mutate,
    .annotations = {.readOnly = false, .destructive = false, .idempotent = true},
    .allowedInBatch = true,
    .keepsSelection = true,
    .run = selectNodes,
  };
}

const auto Mutating =
  mcp::ToolAnnotations{.readOnly = false, .destructive = false, .idempotent = false};

constexpr auto MaxNameLength = 128;

QJsonArray idsOf(const McpNodeRegistry& registry, const auto& nodes)
{
  auto result = QJsonArray{};
  for (const auto* node : nodes)
  {
    result.append(registry.idOf(*node));
  }
  return result;
}

McpResult<std::string> nameArgument(
  const mdl::Map& map, const QJsonObject& args, const QString& key)
{
  const auto name = args.value(key).toString();
  if (auto check = checkMapText(name, McpMapText::Value); check.is_error())
  {
    auto error = errorOf(check);
    error.details["pointer"] = "/" + key;
    return error;
  }
  return fromUnicode(map, name);
}

/** Replaces the selection with the given nodes. */
void selectOnly(mdl::Map& map, const std::vector<mdl::Node*>& nodes)
{
  mdl::deselectAll(map);
  if (!nodes.empty())
  {
    mdl::selectNodes(map, nodes);
  }
}

// group_nodes

McpResult<QJsonObject> groupNodes(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto name = nameArgument(map, args, "name");
  if (name.is_error())
  {
    return errorOf(name);
  }

  auto targets = resolveTargets(doc, args.value("ids").toArray());
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  selectOnly(map, targets.value());
  auto* groupNode = mdl::groupSelectedNodes(map, name.value());
  if (!groupNode)
  {
    return toolError("operation_failed", "the nodes could not be grouped");
  }

  return QJsonObject{
    {"group", doc.nodes.idOf(*groupNode)},
    {"children", idsOf(doc.nodes, groupNode->children())},
  };
}

McpToolDef groupNodesTool()
{
  return {
    .name = "group_nodes",
    .title = "Group Nodes",
    .description =
      "Puts the given nodes into a new group with the given name. Groups are selected "
      "and moved as a whole; their ids stay valid. Ids of the grouped nodes do not "
      "change.",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds()},
        {"name", schema::string(1, MaxNameLength, "the group name")},
      },
      {"ids", "name"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = groupNodes,
  };
}

// ungroup_nodes

McpResult<QJsonObject> ungroupNodes(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto targets = resolveTargets(doc, args.value("ids").toArray());
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto children = std::vector<mdl::Node*>{};
  for (auto* node : targets.value())
  {
    auto* groupNode = dynamic_cast<mdl::GroupNode*>(node);
    if (!groupNode)
    {
      const auto id = doc.nodes.idOf(*node);
      return toolError(
        "wrong_node_type",
        QString{"%1 is a %2, not a group"}.arg(id, nodeTypeName(*node)),
        "pass group ids (G<n>)",
        {{"id", id}, {"actual", nodeTypeName(*node)}, {"expected", "group"}});
    }
    children.insert(
      children.end(), groupNode->children().begin(), groupNode->children().end());
  }

  const auto ungrouped = idsOf(doc.nodes, targets.value());
  selectOnly(map, targets.value());
  mdl::ungroupSelectedNodes(map);

  for (const auto& id : ungrouped)
  {
    if (doc.nodes.lookup(id.toString()).status == McpNodeRegistry::LookupStatus::Live)
    {
      return toolError(
        "operation_failed", QString{"%1 could not be ungrouped"}.arg(id.toString()));
    }
  }

  return QJsonObject{
    {"ungrouped", ungrouped},
    {"children", idsOf(doc.nodes, children)},
  };
}

McpToolDef ungroupNodesTool()
{
  return {
    .name = "ungroup_nodes",
    .title = "Ungroup Nodes",
    .description =
      "Dissolves the given groups; their children move to the groups' parents and keep "
      "their ids.",
    .inputSchema = schema::object(
      {{"ids", schema::nodeIds(schema::MaxNodeIds, "group ids")}}, {"ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = ungroupNodes,
  };
}

// create_layer

McpResult<QJsonObject> createLayer(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto name = nameArgument(map, args, "name");
  if (name.is_error())
  {
    return errorOf(name);
  }

  const auto customLayers = map.worldNode().customLayersUserSorted();
  if (
    std::ranges::any_of(
      customLayers,
      [&](const auto* layerNode) { return layerNode->name() == name.value(); })
    || map.worldNode().defaultLayer()->name() == name.value())
  {
    context.warnings.push_back(
      QString{"there is already a layer named '%1'"}.arg(args.value("name").toString()));
  }

  // like the layer editor: sort the new layer at the bottom of the list
  auto layer = mdl::Layer{name.value()};
  layer.setSortIndex(
    !customLayers.empty() ? customLayers.back()->layer().sortIndex() + 1 : 0);

  auto* layerNode = mdl::addNode(
    map, map.worldNode(), std::make_unique<mdl::LayerNode>(std::move(layer)));
  if (!layerNode)
  {
    return toolError("operation_failed", "the layer could not be added");
  }

  const auto makeCurrent = args.value("make_current").toBool(false);
  if (makeCurrent)
  {
    mdl::setCurrentLayer(map, layerNode);
  }

  return QJsonObject{
    {"id", doc.nodes.idOf(*layerNode)},
    {"current", makeCurrent},
  };
}

McpToolDef createLayerTool()
{
  return {
    .name = "create_layer",
    .title = "Create Layer",
    .description =
      "Adds a layer below the existing layers. With make_current, new nodes are added "
      "to it by default. Use move_to_layer to move nodes into it.",
    .inputSchema = schema::object(
      {
        {"name", schema::string(1, MaxNameLength, "the layer name")},
        {"make_current", schema::boolean("make it the current layer")},
      },
      {"name"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createLayer,
  };
}

// set_current_layer

McpResult<QJsonObject> setCurrentLayer(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto id = args.value("id").toString();
  auto layerNode = resolveAs<mdl::LayerNode>(doc, id, "layer");
  if (layerNode.is_error())
  {
    return errorOf(layerNode);
  }
  if (!mdl::canSetCurrentLayer(map, layerNode.value()))
  {
    return toolError(
      "operation_failed",
      QString{"%1 is already the current layer"}.arg(doc.nodes.idOf(*layerNode.value())),
      "check current_layer in get_map_info");
  }

  mdl::setCurrentLayer(map, layerNode.value());
  return QJsonObject{{"current_layer", doc.nodes.idOf(*layerNode.value())}};
}

McpToolDef setCurrentLayerTool()
{
  return {
    .name = "set_current_layer",
    .title = "Set Current Layer",
    .description =
      "Makes the given layer the current layer, to which new nodes are added by "
      "default. Closes any open group.",
    .inputSchema =
      schema::object({{"id", schema::nodeId("a layer id or default_layer")}}, {"id"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = setCurrentLayer,
  };
}

// move_to_layer

McpResult<QJsonObject> moveToLayer(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto layerNode =
    resolveAs<mdl::LayerNode>(doc, args.value("layer").toString(), "layer");
  if (layerNode.is_error())
  {
    auto error = errorOf(layerNode);
    error.details["pointer"] = "/layer";
    return error;
  }

  auto targets = resolveTargets(doc, args.value("ids").toArray());
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  selectOnly(map, targets.value());
  if (!mdl::canMoveSelectedNodesToLayer(map, layerNode.value()))
  {
    return toolError(
      "operation_failed",
      QString{"the nodes cannot be moved to %1"}.arg(doc.nodes.idOf(*layerNode.value())),
      "nodes inside groups cannot be moved (move the group instead), and at least one "
      "node must be in another layer");
  }

  mdl::moveSelectedNodesToLayer(map, layerNode.value());

  auto moved = QJsonArray{};
  for (const auto* node : targets.value())
  {
    if (mdl::findContainingLayer(*node) == layerNode.value())
    {
      moved.append(doc.nodes.idOf(*node));
    }
  }

  return QJsonObject{
    {"layer", doc.nodes.idOf(*layerNode.value())},
    {"moved", moved},
  };
}

McpToolDef moveToLayerTool()
{
  return {
    .name = "move_to_layer",
    .title = "Move To Layer",
    .description =
      "Moves the given nodes to the given layer; brushes of brush entities move with "
      "their entity. Ids do not change.",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds()},
        {"layer", schema::nodeId("a layer id or default_layer")},
      },
      {"ids", "layer"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = moveToLayer,
  };
}

} // namespace

void registerMcpMapStructureTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(selectNodesTool());
  dispatcher.add(groupNodesTool());
  dispatcher.add(ungroupNodesTool());
  dispatcher.add(createLayerTool());
  dispatcher.add(setCurrentLayerTool());
  dispatcher.add(moveToLayerTool());
}

} // namespace tb::ui
