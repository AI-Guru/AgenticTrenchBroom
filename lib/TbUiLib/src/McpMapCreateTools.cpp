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

#include "base/Error.h"
#include "base/Logger.h"
#include "base/SimpleParserStatus.h"
#include "gl/MaterialManager.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Map_World.h"
#include "mdl/NodeReader.h"
#include "mdl/PasteType.h"
#include "mdl/PatchNode.h"
#include "mdl/Polyhedron3.h"
#include "mdl/Selection.h"
#include "mdl/SoftMapBounds.h"
#include "mdl/WorldNode.h"
#include "ui/McpJson.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

#include "kd/ranges/to.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <ranges>
#include <set>
#include <variant>

namespace tb::ui
{
namespace
{

const auto Mutating =
  mcp::ToolAnnotations{.readOnly = false, .destructive = false, .idempotent = false};

/** Returns the material argument or the current material. */
McpResult<std::string> materialArgument(
  const mdl::Map& map, const QJsonObject& args, const QString& key)
{
  if (!args.contains(key))
  {
    return map.currentMaterialName();
  }
  const auto name = args.value(key).toString();
  if (auto check = checkMaterialName(name, "/" + key); check.is_error())
  {
    return errorOf(check);
  }
  return fromUnicode(map, name);
}

void warnAboutMaterials(
  McpCallContext& context, const std::set<std::string>& materialNames)
{
  const auto& map = context.doc.map;
  for (const auto& name : materialNames)
  {
    if (name != mdl::BrushFace::NoMaterialName && !map.materialManager().material(name))
    {
      context.warnings.push_back(
        QString{"material '%1' was not found; check list_materials"}.arg(
          toUnicode(map, name)));
    }
  }
}

bool isIntegral(const vm::vec3d& v)
{
  for (size_t i = 0; i < 3; ++i)
  {
    if (std::round(v[i]) != v[i])
    {
      return false;
    }
  }
  return true;
}

void warnAboutBounds(McpCallContext& context, const vm::bbox3d& bounds)
{
  if (!isIntegral(bounds.min) || !isIntegral(bounds.max))
  {
    context.warnings.push_back(
      "the bounds are not on integer coordinates; keep brush vertices on integers");
  }

  const auto softBounds = mdl::softMapBounds(context.doc.map);
  if (softBounds.bounds && !softBounds.bounds->contains(bounds))
  {
    context.warnings.push_back(
      "the bounds are outside the soft map bounds (see get_map_info)");
  }
}

void visitWorldBrushes(
  const mdl::Map& map, const std::function<void(const mdl::BrushNode&)>& visitor)
{
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    for (const auto* child : node.children())
    {
      if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(child))
      {
        visitor(*brushNode);
      }
      else if (
        dynamic_cast<const mdl::LayerNode*>(child)
        || dynamic_cast<const mdl::GroupNode*>(child))
      {
        self(self, *child);
      }
    }
  };
  visit(visit, map.worldNode());
}

// create_box_brush

const auto SideNames = std::array<QString, 6>{"-x", "+x", "-y", "+y", "+z", "-z"};

McpResult<QJsonObject> createBoxBrush(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto boundsResult = bounds(args.value("bounds"), "/bounds");
  if (boundsResult.is_error())
  {
    return errorOf(boundsResult);
  }
  auto boxResult = boxInWorld(map, boundsResult.value().min, boundsResult.value().max);
  if (boxResult.is_error())
  {
    return errorOf(boxResult);
  }
  const auto box = boxResult.value();

  auto materialResult = materialArgument(map, args, "material");
  if (materialResult.is_error())
  {
    return errorOf(materialResult);
  }
  const auto material = materialResult.value();

  // in the order of BrushBuilder's six material overload: left, right, front, back,
  // top, bottom
  auto sideMaterials = std::optional<std::array<std::string, 6>>{};
  if (args.contains("side_materials"))
  {
    const auto sides = args.value("side_materials").toObject();
    sideMaterials = std::array<std::string, 6>{};
    for (size_t i = 0; i < SideNames.size(); ++i)
    {
      if (sides.contains(SideNames[i]))
      {
        const auto name = sides.value(SideNames[i]).toString();
        if (auto check = checkMaterialName(name, "/side_materials/" + SideNames[i]);
            check.is_error())
        {
          return errorOf(check);
        }
        (*sideMaterials)[i] = fromUnicode(map, name);
      }
      else
      {
        (*sideMaterials)[i] = material;
      }
    }
  }

  auto parentResult = resolveParent(doc, optionalString(args, "parent"));
  if (parentResult.is_error())
  {
    return errorOf(parentResult);
  }
  auto* parent = parentResult.value();

  const auto& faceAttribsConfig = map.gameInfo().gameConfig.faceAttribsConfig;
  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    faceAttribsConfig.defaultUvAttributes,
    faceAttribsConfig.defaultSurfaceAttributes};

  auto brush = sideMaterials ? builder.createCuboid(
                                 box,
                                 (*sideMaterials)[0],
                                 (*sideMaterials)[1],
                                 (*sideMaterials)[2],
                                 (*sideMaterials)[3],
                                 (*sideMaterials)[4],
                                 (*sideMaterials)[5])
                             : builder.createCuboid(box, material);
  if (brush.is_error())
  {
    return toolError(
      "geometry_invalid",
      QString::fromStdString(std::get<Error>(brush.error()).msg),
      "check the bounds");
  }

  auto* brushNode = mdl::addNode(
    map, *parent, std::make_unique<mdl::BrushNode>(std::move(brush).value()));
  if (!brushNode)
  {
    return toolError("operation_failed", "the brush could not be added");
  }

  auto materials = std::set<std::string>{material};
  if (sideMaterials)
  {
    materials = std::set<std::string>{sideMaterials->begin(), sideMaterials->end()};
  }
  warnAboutMaterials(context, materials);
  warnAboutBounds(context, box);

  return QJsonObject{{"id", doc.nodes.idOf(*brushNode)}};
}

McpToolDef createBoxBrushTool()
{
  auto sideMaterials = QJsonObject{};
  for (const auto& side : SideNames)
  {
    sideMaterials[side] = schema::material();
  }

  return {
    .name = "create_box_brush",
    .title = "Create Box Brush",
    .description =
      "Creates an axis-aligned box brush with the given bounds (world units). "
      "side_materials sets the material of the face whose normal points to that side. "
      "Verify with get_brush_geometry.",
    .inputSchema = schema::object(
      {
        {"bounds", schema::bounds("the box; min must be less than max on every axis")},
        {"material",
         schema::material("material of all faces (default: the current material)")},
        {"side_materials",
         schema::describe(
           schema::object(sideMaterials),
           "materials per side, e.g. {\"+z\":\"floor\"}; other sides use material")},
        {"parent",
         schema::nodeId("a layer, group or brush entity (default: the current "
                        "group or layer)")},
      },
      {"bounds"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createBoxBrush,
  };
}

// create_entity

/** Resolves the brushes and patches for a brush entity. */
McpResult<std::vector<mdl::Node*>> resolveEntityBrushes(
  const McpDocumentState& doc, const QJsonArray& ids)
{
  auto nodes = resolveNodes(doc, ids, size_t(schema::MaxNodeIds));
  if (nodes.is_error())
  {
    return nodes;
  }
  for (const auto* node : nodes.value())
  {
    if (
      !dynamic_cast<const mdl::BrushNode*>(node)
      && !dynamic_cast<const mdl::PatchNode*>(node))
    {
      const auto id = doc.nodes.idOf(*node);
      return toolError(
        "wrong_node_type",
        QString{"%1 is a %2; brush_ids must be brushes or patches"}.arg(
          id, nodeTypeName(*node)),
        "pass the brush ids, e.g. from find_nodes with types [\"brush\"]",
        {{"id", id}, {"actual", nodeTypeName(*node)}, {"expected", "brush or patch"}});
    }
  }
  return resolveTargets(doc, ids, McpTargetPurpose::Edit);
}

McpResult<QJsonObject> createEntity(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto classname = args.value("classname").toString();
  if (auto check = checkMapText(classname, McpMapText::Key); check.is_error())
  {
    auto error = errorOf(check);
    error.details["pointer"] = "/classname";
    return error;
  }
  if (classname.compare(u"worldspawn", Qt::CaseInsensitive) == 0)
  {
    return toolError(
      "invalid_argument",
      "worldspawn cannot be created",
      "use set_entity_properties with ids [\"world\"] to edit worldspawn",
      {{"pointer", "/classname"}});
  }

  const auto hasOrigin = args.contains("origin");
  auto origin = vm::vec3d{};
  if (hasOrigin)
  {
    auto originResult = vec3(args.value("origin"), "/origin");
    if (originResult.is_error())
    {
      return errorOf(originResult);
    }
    origin = originResult.value();
  }

  const auto propertiesArg = args.value("properties").toObject();
  auto properties = std::vector<std::pair<QString, QString>>{};
  for (auto it = propertiesArg.begin(); it != propertiesArg.end(); ++it)
  {
    const auto pointer = "/properties/" + it.key();
    if (auto check = checkMapText(it.key(), McpMapText::Key); check.is_error())
    {
      auto error = errorOf(check);
      error.details["pointer"] = pointer;
      return error;
    }
    if (auto check = checkEntityPropertyKeyNotReserved(it.key(), pointer);
        check.is_error())
    {
      return errorOf(check);
    }
    if (it.key() == u"classname")
    {
      return toolError(
        "invalid_argument",
        "properties must not contain classname",
        "pass the classname argument instead",
        {{"pointer", pointer}});
    }
    if (it.key() == u"origin" && hasOrigin)
    {
      return toolError(
        "invalid_argument",
        "properties must not contain origin if the origin argument is given",
        "pass either origin or properties.origin",
        {{"pointer", pointer}});
    }

    auto value = propValueText(it.value(), pointer);
    if (value.is_error())
    {
      return errorOf(value);
    }
    if (auto check = checkEntityPropertyValue(it.key(), value.value(), pointer);
        check.is_error())
    {
      return errorOf(check);
    }
    properties.emplace_back(it.key(), value.value());
  }

  const auto classnameStr = fromUnicode(map, classname);
  const auto* definition = map.entityDefinitionManager().definition(classnameStr);
  const auto hasBrushIds = args.contains("brush_ids");

  if (definition)
  {
    const auto type = mdl::getType(*definition);
    if (type == mdl::EntityDefinitionType::Point && hasBrushIds)
    {
      return toolError(
        "definition_type_mismatch",
        QString{"%1 is a point entity and cannot have brushes"}.arg(classname),
        "omit brush_ids and pass origin",
        {{"classname", classname}, {"type", "point"}});
    }
    if (type == mdl::EntityDefinitionType::Brush && !hasBrushIds)
    {
      return toolError(
        "definition_type_mismatch",
        QString{"%1 is a brush entity and needs brushes"}.arg(classname),
        "pass brush_ids, e.g. the id from create_box_brush",
        {{"classname", classname}, {"type", "brush"}});
    }
  }

  if (!hasBrushIds)
  {
    if (!hasOrigin)
    {
      return toolError(
        "invalid_argument",
        "a point entity needs an origin",
        "pass origin, e.g. [0, 0, 24]",
        {{"pointer", "/origin"}});
    }
    if (!map.worldBounds().contains(origin))
    {
      return toolError(
        "out_of_world_bounds",
        "the origin is not inside the world bounds",
        "keep coordinates inside world_bounds (see get_map_info)",
        {{"world_bounds", toJson(map.worldBounds())}});
    }
  }

  auto brushes = std::vector<mdl::Node*>{};
  if (hasBrushIds)
  {
    auto brushesResult = resolveEntityBrushes(doc, args.value("brush_ids").toArray());
    if (brushesResult.is_error())
    {
      return errorOf(brushesResult);
    }
    brushes = std::move(brushesResult).value();
    if (hasOrigin)
    {
      context.warnings.push_back(
        "origin is ignored for brush entities; pass properties.origin to set the "
        "origin property");
    }
  }

  auto* parent = static_cast<mdl::Node*>(nullptr);
  if (const auto parentId = optionalString(args, "parent"))
  {
    auto parentResult = resolveParent(doc, parentId);
    if (parentResult.is_error())
    {
      return errorOf(parentResult);
    }
    parent = parentResult.value();
    if (dynamic_cast<const mdl::EntityNode*>(parent))
    {
      return toolError(
        "invalid_parent",
        QString{"%1 cannot be a parent: entities cannot contain entities"}.arg(*parentId),
        "pass a layer or a group, or omit parent",
        {{"id", *parentId}, {"type", "entity"}});
    }
  }
  else if (!brushes.empty())
  {
    parent = &mdl::parentForNodes(map, brushes);
  }
  else
  {
    auto parentResult = resolveParent(doc, std::nullopt);
    if (parentResult.is_error())
    {
      return errorOf(parentResult);
    }
    parent = parentResult.value();
  }

  auto entity = mdl::Entity{};
  entity.setClassname(classnameStr);
  for (const auto& [key, value] : properties)
  {
    entity.addOrUpdateProperty(fromUnicode(map, key), fromUnicode(map, value));
  }
  if (!hasBrushIds)
  {
    entity.setOrigin(origin);
  }
  if (definition && args.value("apply_definition_defaults").toBool(false))
  {
    mdl::setDefaultProperties(
      *definition, entity, mdl::SetDefaultPropertyMode::SetMissing);
  }

  auto* entityNode =
    mdl::addNode(map, *parent, std::make_unique<mdl::EntityNode>(std::move(entity)));
  if (!entityNode)
  {
    return toolError("operation_failed", "the entity could not be added");
  }

  if (!brushes.empty())
  {
    // reparenting may remove emptied brush entities, which must not be selected
    mdl::deselectAll(map);
    if (!mdl::reparentNodes(map, {{entityNode, brushes}}))
    {
      return toolError(
        "operation_failed",
        "the brushes could not be moved into the entity",
        "check that the brushes are not inside linked groups");
    }
  }

  if (!definition)
  {
    context.warnings.push_back(
      QString{"unknown classname '%1': there is no entity definition for it"}.arg(
        classname));
  }
  else
  {
    for (const auto& [key, value] : properties)
    {
      if (!mdl::getPropertyDefinition(*definition, fromUnicode(map, key)))
      {
        context.warnings.push_back(
          QString{"property '%1' is not defined for %2"}.arg(key, classname));
      }
    }
  }

  if (!hasBrushIds)
  {
    auto inSolid = false;
    visitWorldBrushes(map, [&](const mdl::BrushNode& brushNode) {
      inSolid =
        inSolid
        || (brushNode.entity() == &map.worldNode() && brushNode.brush().containsPoint(origin));
    });
    if (inSolid)
    {
      context.warnings.push_back(
        "the origin is inside a world brush; the entity may be stuck in solid");
    }
  }

  auto propertiesJson = QJsonArray{};
  for (const auto& property : entityNode->entity().properties())
  {
    propertiesJson.append(QJsonObject{
      {"key", toUnicode(map, property.key())},
      {"value", toUnicode(map, property.value())},
    });
  }

  return QJsonObject{
    {"id", doc.nodes.idOf(*entityNode)},
    {"classname", classname},
    {"origin", toJson(entityNode->entity().origin())},
    {"properties", propertiesJson},
  };
}

McpToolDef createEntityTool()
{
  return {
    .name = "create_entity",
    .title = "Create Entity",
    .description =
      "Creates a point entity at origin, or a brush entity from existing brushes "
      "(brush_ids). Property values may be strings, numbers, booleans or [x,y,z]; keys "
      "starting with _tb_ are reserved for TrenchBroom. Verify with get_nodes.",
    .inputSchema = schema::object(
      {
        {"classname", schema::string(1, 256, "e.g. light or info_player_start")},
        {"origin", schema::vec3("position of a point entity")},
        {"properties",
         schema::describe(
           {{"type", "object"}}, "property key to value, e.g. {\"light\": 300}")},
        {"brush_ids",
         schema::nodeIds(
           schema::MaxNodeIds, "brushes or patches that become the brush entity")},
        {"parent", schema::nodeId("a layer or group (default: the current one)")},
        {"apply_definition_defaults",
         schema::boolean("add the default values of the entity definition for "
                         "properties that are not given")},
      },
      {"classname"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createEntity,
  };
}

// paste_map_text

McpResult<QJsonObject> pasteMapText(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto offset = vm::vec3d{};
  if (args.contains("offset"))
  {
    auto offsetResult = vec3(args.value("offset"), "/offset");
    if (offsetResult.is_error())
    {
      return errorOf(offsetResult);
    }
    offset = offsetResult.value();
  }

  // paste adds to the current group or layer, which must not be linked
  if (auto parent = resolveParent(doc, std::nullopt); parent.is_error())
  {
    return errorOf(parent);
  }

  const auto text = fromUnicode(map, args.value("text").toString());

  // TrenchBroom adds pasted nodes to its octree, which violates a contract for nodes far
  // outside the world, so parse the text first and check what it contains; text that
  // does not parse as nodes is handled by mdl::paste below
  {
    auto nullLogger = NullLogger{};
    auto parserStatus = SimpleParserStatus{nullLogger};
    if (auto nodes = mdl::NodeReader::read(
          text,
          map.worldNode().mapFormat(),
          map.worldBounds(),
          map.worldNode().entityPropertyConfig(),
          parserStatus,
          map.taskManager());
        nodes.is_success())
    {
      const auto roots = nodes.value() | std::views::transform([](const auto& node) {
                           return static_cast<const mdl::Node*>(node.get());
                         })
                         | kdl::ranges::to<std::vector>();
      if (auto check = checkTransformableNodes(doc.nodes, roots); check.is_error())
      {
        auto error = errorOf(check);
        error.details["pointer"] = "/text";
        return error;
      }
    }
  }

  // a face paste would apply to the selected faces, so nothing may be selected
  mdl::deselectAll(map);

  const auto logDetails = [&]() {
    auto log = QJsonArray{};
    if (context.log)
    {
      for (const auto& line : *context.log)
      {
        if (line.level >= LogLevel::Warn)
        {
          log.append(line.text);
        }
      }
    }
    return log;
  };

  const auto pasteType = mdl::paste(map, text);
  if (pasteType == mdl::PasteType::Failed)
  {
    return toolError(
      "parse_failed",
      "the text could not be parsed as map entities or brushes",
      "pass text in the map's format, e.g. from export_map_text",
      {{"log", logDetails()}});
  }
  if (pasteType == mdl::PasteType::BrushFace)
  {
    return toolError(
      "parse_failed",
      "the text contains face attributes, not entities or brushes",
      "use set_face_attributes to change faces",
      {{"log", logDetails()}});
  }

  if (offset != vm::vec3d{0, 0, 0})
  {
    const auto& selectionBounds = map.selectionBounds();
    if (!selectionBounds)
    {
      context.warnings.push_back(
        "the pasted nodes are not selectable, so the offset was not applied");
    }
    else
    {
      if (!map.worldBounds().contains(selectionBounds->translate(offset)))
      {
        return toolError(
          "out_of_world_bounds",
          "the pasted nodes would be moved outside the world bounds",
          "use a smaller offset",
          {{"world_bounds", toJson(map.worldBounds())}});
      }
      if (!mdl::translateSelection(map, offset))
      {
        return toolError("operation_failed", "the pasted nodes could not be moved");
      }
    }
  }

  auto ids = QJsonArray{};
  for (const auto& id : doc.nodes.currentChanges().created)
  {
    ids.append(id);
  }
  return QJsonObject{{"ids", ids}};
}

McpToolDef pasteMapTextTool()
{
  return {
    .name = "paste_map_text",
    .title = "Paste Map Text",
    .description =
      "Adds the entities and brushes in the given .map text (as from export_map_text) "
      "to the current layer or group, optionally moved by offset. Pasted nodes get new "
      "ids, which are returned.",
    .inputSchema = schema::object(
      {
        {"text", schema::string(1, 8388608, ".map text with entities and brushes")},
        {"offset", schema::vec3("translation applied to the pasted nodes")},
      },
      {"text"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = pasteMapText,
  };
}

// shared by the brush creation tools

mdl::BrushBuilder brushBuilder(const mdl::Map& map)
{
  const auto& faceAttribsConfig = map.gameInfo().gameConfig.faceAttribsConfig;
  return mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    faceAttribsConfig.defaultUvAttributes,
    faceAttribsConfig.defaultSurfaceAttributes};
}

mcp::ToolError geometryError(const Error& error, const QString& hint)
{
  return toolError("geometry_invalid", QString::fromStdString(error.msg), hint);
}

bool hasNonIntegerVertices(const mdl::Brush& brush)
{
  return std::ranges::any_of(brush.vertexPositions(), [](const vm::vec3d& position) {
    for (size_t i = 0; i < 3; ++i)
    {
      if (std::abs(position[i] - std::round(position[i])) > 1e-6)
      {
        return true;
      }
    }
    return false;
  });
}

/** Adds the given brushes to the parent and returns the added nodes in order. */
McpResult<std::vector<mdl::BrushNode*>> addBrushes(
  mdl::Map& map, mdl::Node& parent, std::vector<mdl::Brush> brushes)
{
  auto nodes = std::vector<std::unique_ptr<mdl::Node>>{};
  auto brushNodes = std::vector<mdl::BrushNode*>{};
  for (auto& brush : brushes)
  {
    auto brushNode = std::make_unique<mdl::BrushNode>(std::move(brush));
    brushNodes.push_back(brushNode.get());
    nodes.push_back(std::move(brushNode));
  }
  if (mdl::addNodes(map, parent, std::move(nodes)).empty())
  {
    return toolError("operation_failed", "the brushes could not be added");
  }
  return brushNodes;
}

McpResult<std::optional<QString>> groupNameArgument(const QJsonObject& args)
{
  if (!args.contains("group_name"))
  {
    return std::nullopt;
  }
  const auto name = args.value("group_name").toString();
  if (auto check = checkMapText(name, McpMapText::Value); check.is_error())
  {
    auto error = errorOf(check);
    error.details["pointer"] = "/group_name";
    return error;
  }
  return std::optional{name};
}

/** Groups the given nodes; they must have the same parent. */
McpResult<mdl::GroupNode*> groupNewNodes(
  mdl::Map& map, const std::vector<mdl::Node*>& nodes, const QString& name)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, nodes);
  auto* groupNode = mdl::groupSelectedNodes(map, fromUnicode(map, name));
  if (!groupNode)
  {
    return toolError("operation_failed", "the brushes could not be grouped");
  }
  return groupNode;
}

McpResult<mdl::Node*> brushParentArgument(
  const McpDocumentState& doc, const QJsonObject& args, const bool grouped)
{
  auto parent = resolveParent(doc, optionalString(args, "parent"));
  if (parent.is_error())
  {
    return parent;
  }
  if (grouped && dynamic_cast<const mdl::EntityNode*>(parent.value()))
  {
    return toolError(
      "invalid_argument",
      "group_name cannot be used with a brush entity parent",
      "omit group_name or pass a layer or group as parent",
      {{"pointer", "/group_name"}});
  }
  return parent;
}

// create_hollow_box

constexpr auto MinWallThickness = 0.001;
constexpr auto MaxWallThickness = 4096.0;

/** The walls of a hollow box in the order in which they are created. */
const auto WallSides = std::array<QString, 6>{"+x", "-x", "+y", "-y", "+z", "-z"};

/**
 * Returns the wall on the given side. The walls do not overlap: the z slabs span the
 * outer box in x and y, the x walls span the outer box in y and the inner box in z, and
 * the y walls span the inner box in x and z.
 */
vm::bbox3d wallBounds(
  const QString& side, const vm::bbox3d& inner, const vm::bbox3d& outer)
{
  const auto& i = inner;
  const auto& o = outer;
  if (side == u"+x")
  {
    return {{i.max.x(), o.min.y(), i.min.z()}, {o.max.x(), o.max.y(), i.max.z()}};
  }
  if (side == u"-x")
  {
    return {{o.min.x(), o.min.y(), i.min.z()}, {i.min.x(), o.max.y(), i.max.z()}};
  }
  if (side == u"+y")
  {
    return {{i.min.x(), i.max.y(), i.min.z()}, {i.max.x(), o.max.y(), i.max.z()}};
  }
  if (side == u"-y")
  {
    return {{i.min.x(), o.min.y(), i.min.z()}, {i.max.x(), i.min.y(), i.max.z()}};
  }
  if (side == u"+z")
  {
    return {{o.min.x(), o.min.y(), i.max.z()}, o.max};
  }
  return {o.min, {o.max.x(), o.max.y(), i.min.z()}};
}

McpResult<QJsonObject> createHollowBox(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto innerResult = bounds(args.value("inner_bounds"), "/inner_bounds");
  if (innerResult.is_error())
  {
    return errorOf(innerResult);
  }
  auto innerBox = boxInWorld(map, innerResult.value().min, innerResult.value().max);
  if (innerBox.is_error())
  {
    auto error = errorOf(innerBox);
    error.details["pointer"] = "/inner_bounds";
    return error;
  }
  const auto inner = innerBox.value();

  const auto thickness = args.value("wall_thickness").toDouble();
  const auto delta = vm::vec3d::fill(thickness);
  auto outerBox = boxInWorld(map, inner.min - delta, inner.max + delta);
  if (outerBox.is_error())
  {
    auto error = errorOf(outerBox);
    error.message = "the outer box (inner_bounds plus wall_thickness): " + error.message;
    return error;
  }
  const auto outer = outerBox.value();

  auto materialResult = materialArgument(map, args, "material");
  if (materialResult.is_error())
  {
    return errorOf(materialResult);
  }
  const auto material = materialResult.value();

  const auto sideMaterials = args.value("side_materials").toObject();
  auto wallMaterials = std::map<QString, std::string>{};
  for (const auto& side : WallSides)
  {
    wallMaterials[side] = material;
    if (sideMaterials.contains(side))
    {
      const auto name = sideMaterials.value(side).toString();
      if (auto check = checkMaterialName(name, "/side_materials/" + side);
          check.is_error())
      {
        return errorOf(check);
      }
      wallMaterials[side] = fromUnicode(map, name);
    }
  }

  auto omitted = std::set<QString>{};
  const auto omitSides = args.value("omit_sides").toArray();
  for (qsizetype i = 0; i < omitSides.size(); ++i)
  {
    if (!omitted.insert(omitSides[i].toString()).second)
    {
      return toolError(
        "invalid_argument",
        QString{"/omit_sides/%1: %2 is listed twice"}.arg(i).arg(omitSides[i].toString()),
        {},
        {{"pointer", QString{"/omit_sides/%1"}.arg(i)}});
    }
  }

  auto groupName = groupNameArgument(args);
  if (groupName.is_error())
  {
    return errorOf(groupName);
  }

  auto parent = brushParentArgument(doc, args, groupName.value().has_value());
  if (parent.is_error())
  {
    return errorOf(parent);
  }

  const auto builder = brushBuilder(map);
  auto brushes = std::vector<mdl::Brush>{};
  auto sides = std::vector<QString>{};
  for (const auto& side : WallSides)
  {
    if (omitted.contains(side))
    {
      continue;
    }
    auto brush =
      builder.createCuboid(wallBounds(side, inner, outer), wallMaterials[side]);
    if (brush.is_error())
    {
      return geometryError(std::get<Error>(brush.error()), "check the bounds");
    }
    brushes.push_back(std::move(brush).value());
    sides.push_back(side);
  }

  auto added = addBrushes(map, *parent.value(), std::move(brushes));
  if (added.is_error())
  {
    return errorOf(added);
  }

  auto walls = QJsonObject{};
  for (size_t i = 0; i < sides.size(); ++i)
  {
    walls[sides[i]] = doc.nodes.idOf(*added.value()[i]);
  }
  auto result = QJsonObject{
    {"walls", walls},
    {"inner_bounds", toJson(inner)},
    {"outer_bounds", toJson(outer)},
  };

  if (groupName.value())
  {
    auto groupNode = groupNewNodes(
      map,
      std::vector<mdl::Node*>{added.value().begin(), added.value().end()},
      *groupName.value());
    if (groupNode.is_error())
    {
      return errorOf(groupNode);
    }
    result["group"] = doc.nodes.idOf(*groupNode.value());
  }

  auto materials = std::set<std::string>{};
  for (const auto& side : sides)
  {
    materials.insert(wallMaterials[side]);
  }
  warnAboutMaterials(context, materials);
  warnAboutBounds(context, outer);

  return result;
}

McpToolDef createHollowBoxTool()
{
  auto sideMaterials = QJsonObject{};
  for (const auto& side : WallSides)
  {
    sideMaterials[side] = schema::material();
  }

  return {
    .name = "create_hollow_box",
    .title = "Create Hollow Box",
    .description =
      "Creates a sealed room: six non-overlapping wall brushes around inner_bounds, "
      "each wall_thickness thick. omit_sides leaves walls out, e.g. [\"+z\"] for no "
      "ceiling. Verify with query_point and check_brush_overlaps.",
    .inputSchema = schema::object(
      {
        {"inner_bounds", schema::bounds("the empty space inside the walls")},
        {"wall_thickness", schema::number(MinWallThickness, MaxWallThickness, "e.g. 16")},
        {"material", schema::material("material of all walls (default: current)")},
        {"side_materials",
         schema::describe(
           schema::object(sideMaterials),
           "material per wall, e.g. {\"-z\":\"floor\",\"+z\":\"ceiling\"}")},
        {"omit_sides", schema::array(schema::side(), 1, 5, "walls to leave out")},
        {"group_name", schema::string(1, 128, "group the walls under this name")},
        {"parent", schema::nodeId("a layer or group (default: the current one)")},
      },
      {"inner_bounds", "wall_thickness"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createHollowBox,
  };
}

// create_primitive_brush

constexpr auto MaxPrimitiveSides = 64;

vm::axis::type axisArgument(const QJsonObject& args)
{
  const auto axis = args.value("axis").toString("z");
  return axis == u"x" ? vm::axis::x : axis == u"y" ? vm::axis::y : vm::axis::z;
}

mdl::CircleShape circleShapeArgument(const QJsonObject& args)
{
  const auto alignment = args.value("alignment").toString("edge");
  const auto sides = sizeArgument(args, "sides", 8);
  if (alignment == u"vertex")
  {
    return mdl::VertexAlignedCircle{sides};
  }
  if (alignment == u"scalable")
  {
    return mdl::ScalableCircle{sizeArgument(args, "precision", 0)};
  }
  return mdl::EdgeAlignedCircle{sides};
}

McpResult<QJsonObject> createPrimitiveBrush(
  McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto boundsResult = bounds(args.value("bounds"), "/bounds");
  if (boundsResult.is_error())
  {
    return errorOf(boundsResult);
  }
  auto boxResult = boxInWorld(map, boundsResult.value().min, boundsResult.value().max);
  if (boxResult.is_error())
  {
    return errorOf(boxResult);
  }
  const auto box = boxResult.value();
  const auto size = box.size();
  for (size_t i = 0; i < 3; ++i)
  {
    if (size[i] < 1.0)
    {
      return toolError(
        "invalid_argument",
        "primitives must be at least 1 unit large along every axis",
        "pass larger bounds",
        {{"pointer", "/bounds"}});
    }
  }

  const auto shape = args.value("shape").toString();
  const auto axis = axisArgument(args);
  const auto circleShape = circleShapeArgument(args);

  // BrushBuilder::createUvSphere reads past the end of its ring tables for scalable
  // circles with a precision above 0
  if (
    shape == u"uv_sphere" && std::holds_alternative<mdl::ScalableCircle>(circleShape)
    && std::get<mdl::ScalableCircle>(circleShape).precision > 0)
  {
    return toolError(
      "invalid_argument",
      "scalable uv spheres only support precision 0",
      "omit precision, or use edge or vertex alignment with sides and rings",
      {{"pointer", "/precision"}});
  }

  const auto needsThickness = shape == u"hollow_cylinder" || shape == u"arch";
  auto thickness = 0.0;
  if (needsThickness)
  {
    // the two extents across the axis
    const auto crossSection = std::min(size[(axis + 1) % 3], size[(axis + 2) % 3]);
    thickness = args.value("thickness").toDouble(0.0);
    if (
      !args.contains("thickness") || !(thickness > 0.0 && thickness < crossSection / 2.0))
    {
      return toolError(
        "invalid_argument",
        QString{"%1 needs a thickness greater than 0 and less than %2"}.arg(shape).arg(
          crossSection / 2.0),
        "thickness is the wall thickness, e.g. 8",
        {{"pointer", "/thickness"}});
    }
  }
  else if (args.contains("thickness"))
  {
    context.warnings.push_back(QString{"thickness is ignored for %1"}.arg(shape));
  }

  auto materialResult = materialArgument(map, args, "material");
  if (materialResult.is_error())
  {
    return errorOf(materialResult);
  }
  const auto material = materialResult.value();

  auto parent = brushParentArgument(doc, args, false);
  if (parent.is_error())
  {
    return errorOf(parent);
  }

  const auto builder = brushBuilder(map);
  const auto single = [](Result<mdl::Brush> brush) -> Result<std::vector<mdl::Brush>> {
    if (brush.is_error())
    {
      return std::get<Error>(brush.error());
    }
    auto result = std::vector<mdl::Brush>{};
    result.push_back(std::move(brush).value());
    return result;
  };

  auto brushes =
    shape == u"cylinder"
      ? single(builder.createCylinder(box, circleShape, axis, material))
    : shape == u"cone" ? single(builder.createCone(box, circleShape, axis, material))
    : shape == u"uv_sphere"
      ? single(builder.createUvSphere(
          box, circleShape, sizeArgument(args, "rings", 4), axis, material))
    : shape == u"ico_sphere" ? single(builder.createIcoSphere(
                                 box, sizeArgument(args, "iterations", 1), material))
    : shape == u"hollow_cylinder"
      ? builder.createHollowCylinder(box, thickness, circleShape, axis, material)
      : builder.createArch(box, thickness, circleShape, axis, material);
  if (brushes.is_error())
  {
    return geometryError(
      std::get<Error>(brushes.error()), "try larger bounds or other parameters");
  }
  if (brushes.value().empty())
  {
    return toolError("geometry_invalid", "no brushes were created", "try larger bounds");
  }

  const auto nonInteger = std::ranges::any_of(
    brushes.value(), [](const auto& brush) { return hasNonIntegerVertices(brush); });

  auto added = addBrushes(map, *parent.value(), std::move(brushes).value());
  if (added.is_error())
  {
    return errorOf(added);
  }

  auto ids = QJsonArray{};
  for (const auto* brushNode : added.value())
  {
    ids.append(doc.nodes.idOf(*brushNode));
  }

  warnAboutMaterials(context, {material});
  warnAboutBounds(context, box);
  if (nonInteger)
  {
    context.warnings.push_back(
      "some vertices are not on integer coordinates; call snap_vertices to fix them");
  }

  return QJsonObject{{"ids", ids}};
}

McpToolDef createPrimitiveBrushTool()
{
  return {
    .name = "create_primitive_brush",
    .title = "Create Primitive Brush",
    .description =
      "Creates a cylinder, cone, sphere, hollow cylinder or arch filling the given "
      "bounds, built like TrenchBroom's shape tool. Hollow cylinders and arches consist "
      "of several brushes and need a thickness. Verify with get_brush_geometry.",
    .inputSchema = schema::object(
      {
        {"shape",
         schema::stringEnum(
           {"cylinder", "cone", "uv_sphere", "ico_sphere", "hollow_cylinder", "arch"})},
        {"bounds", schema::bounds("the box the shape fills")},
        {"axis",
         schema::stringEnum(
           {"x", "y", "z"},
           "the axis of cylinders, cones and spheres, or the tunnel direction of arches "
           "(default z)")},
        {"sides", schema::integer(3, MaxPrimitiveSides, "number of sides (default 8)")},
        {"alignment",
         schema::stringEnum(
           {"edge", "vertex", "scalable"},
           "edge or vertex aligned circles with the given sides, or scalable circles "
           "with 12, 24, 48 or 96 sides depending on precision (default edge)")},
        {"precision",
         schema::integer(
           0, 3, "precision of scalable circles (default 0; uv spheres only support 0)")},
        {"rings", schema::integer(2, 32, "rings of uv spheres (default 4)")},
        {"iterations", schema::integer(0, 3, "subdivisions of ico spheres (default 1)")},
        {"thickness",
         schema::number(0, 1e6, "wall thickness of hollow cylinders and arches")},
        {"material", schema::material("default: the current material")},
        {"parent", schema::nodeId("a layer, group or brush entity (default: current)")},
      },
      {"shape", "bounds"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createPrimitiveBrush,
  };
}

// create_brush_from_points

constexpr auto MinBrushPoints = 4;
constexpr auto MaxBrushPoints = 256;

McpResult<QJsonObject> createBrushFromPoints(
  McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto pointsArg = args.value("points").toArray();
  auto points = std::vector<vm::vec3d>{};
  for (qsizetype i = 0; i < pointsArg.size(); ++i)
  {
    const auto pointer = QString{"/points/%1"}.arg(i);
    auto point = vec3(pointsArg[i], pointer);
    if (point.is_error())
    {
      return errorOf(point);
    }
    if (!map.worldBounds().contains(point.value()))
    {
      return toolError(
        "out_of_world_bounds",
        QString{"%1 is not inside the world bounds"}.arg(pointer),
        "keep coordinates inside world_bounds (see get_map_info)",
        {{"pointer", pointer}, {"world_bounds", toJson(map.worldBounds())}});
    }
    points.push_back(point.value());
  }

  auto materialResult = materialArgument(map, args, "material");
  if (materialResult.is_error())
  {
    return errorOf(materialResult);
  }
  const auto material = materialResult.value();

  auto parent = brushParentArgument(doc, args, false);
  if (parent.is_error())
  {
    return errorOf(parent);
  }

  const auto polyhedron = mdl::Polyhedron3{points};
  if (!polyhedron.polyhedron() || !polyhedron.closed())
  {
    return toolError(
      "geometry_invalid",
      "the points do not span a volume",
      "points must span a volume: pass at least 4 points that are not in one plane",
      {{"pointer", "/points"}});
  }

  auto brush = brushBuilder(map).createBrush(polyhedron, material);
  if (brush.is_error())
  {
    return geometryError(std::get<Error>(brush.error()), "points must span a volume");
  }

  const auto nonInteger = hasNonIntegerVertices(brush.value());
  auto* brushNode = mdl::addNode(
    map, *parent.value(), std::make_unique<mdl::BrushNode>(std::move(brush).value()));
  if (!brushNode)
  {
    return toolError("operation_failed", "the brush could not be added");
  }

  warnAboutMaterials(context, {material});
  warnAboutBounds(context, brushNode->logicalBounds());
  if (nonInteger)
  {
    context.warnings.push_back(
      "some vertices are not on integer coordinates; call snap_vertices to fix them");
  }

  return QJsonObject{
    {"id", doc.nodes.idOf(*brushNode)},
    {"vertex_count", qint64(brushNode->brush().vertexCount())},
    {"face_count", qint64(brushNode->brush().faceCount())},
  };
}

McpToolDef createBrushFromPointsTool()
{
  return {
    .name = "create_brush_from_points",
    .title = "Create Brush From Points",
    .description =
      "Creates the convex hull of the given points as one brush, e.g. a wedge or a "
      "ramp. Points inside the hull are ignored. Verify with get_brush_geometry.",
    .inputSchema = schema::object(
      {
        {"points",
         schema::array(
           schema::vec3(), MinBrushPoints, MaxBrushPoints, "the corners of the brush")},
        {"material", schema::material("default: the current material")},
        {"parent", schema::nodeId("a layer, group or brush entity (default: current)")},
      },
      {"points"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createBrushFromPoints,
  };
}

} // namespace

void registerMcpMapCreateTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(createBoxBrushTool());
  dispatcher.add(createEntityTool());
  dispatcher.add(pasteMapTextTool());
  dispatcher.add(createHollowBoxTool());
  dispatcher.add(createPrimitiveBrushTool());
  dispatcher.add(createBrushFromPointsTool());
}

} // namespace tb::ui
