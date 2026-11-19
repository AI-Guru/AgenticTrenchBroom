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

#include <QSet>

#include "gl/MaterialManager.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeHandleManager.h"
#include "mdl/NodeHandles.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/WorldNode.h"
#include "ui/McpJson.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

#include "kd/string_compare.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_set>

namespace tb::ui
{
namespace
{

const auto Mutating =
  mcp::ToolAnnotations{.readOnly = false, .destructive = false, .idempotent = false};
const auto Destructive =
  mcp::ToolAnnotations{.readOnly = false, .destructive = true, .idempotent = false};

constexpr auto MaxFaceRefs = 1000;
constexpr auto MaxAngleDegrees = 3600.0;
constexpr auto MinScaleFactor = 1e-4;
constexpr auto MaxScaleFactor = 1e4;
constexpr auto MaxUvValue = 1e6;
constexpr auto MaxSurfaceValue = 1e9;

McpResult<double> finiteNumber(
  const QJsonValue& value, const QString& pointer, const double maxAbs)
{
  const auto d = value.toDouble();
  if (!value.isDouble() || !std::isfinite(d) || std::abs(d) > maxAbs)
  {
    return toolError(
      "invalid_argument",
      QString{"%1 must be a number between -%2 and %2"}.arg(pointer).arg(maxAbs),
      {},
      {{"pointer", pointer}});
  }
  return d;
}

McpResult<vm::vec2d> vec2(const QJsonValue& value, const QString& pointer, double maxAbs)
{
  const auto array = value.toArray();
  if (!value.isArray() || array.size() != 2)
  {
    return toolError(
      "invalid_argument",
      QString{"%1 must be an array of 2 numbers"}.arg(pointer),
      {},
      {{"pointer", pointer}});
  }
  auto result = vm::vec2d{};
  for (qsizetype i = 0; i < 2; ++i)
  {
    auto component = finiteNumber(array[i], QString{"%1/%2"}.arg(pointer).arg(i), maxAbs);
    if (component.is_error())
    {
      return errorOf(component);
    }
    result[size_t(i)] = component.value();
  }
  return result;
}

/** Returns whether any vertex of the given brushes is not within 1e-6 of an integer. */
bool hasNonIntegerVertices(const std::vector<mdl::BrushNode*>& brushNodes)
{
  return std::ranges::any_of(brushNodes, [](const auto* brushNode) {
    return std::ranges::any_of(
      brushNode->brush().vertexPositions(), [](const vm::vec3d& position) {
        for (size_t i = 0; i < 3; ++i)
        {
          if (std::abs(position[i] - std::round(position[i])) > 1e-6)
          {
            return true;
          }
        }
        return false;
      });
  });
}

QJsonArray propertiesJson(const mdl::Map& map, const mdl::Entity& entity)
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

McpResult<void> requireNoVertexHandlesSelected(const mdl::Map& map)
{
  if (map.nodeHandles().anyHandleSelected<mdl::VertexHandle>())
  {
    return toolError(
      "editor_busy",
      "brush vertices are selected in the vertex tool",
      "ask the user to exit the vertex/clip tool",
      {{"reason", "modal_tool_active"}});
  }
  return kdl::void_success;
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

// transform_nodes

McpResult<vm::vec3d> rotationAxis(const QJsonValue& value)
{
  if (value.isString())
  {
    const auto name = value.toString();
    return name == u"x"   ? vm::vec3d{1, 0, 0}
           : name == u"y" ? vm::vec3d{0, 1, 0}
                          : vm::vec3d{0, 0, 1};
  }

  auto axis = vec3(value, "/rotate/axis");
  if (axis.is_error())
  {
    return axis;
  }
  const auto length = vm::length(axis.value());
  if (!(length > 1e-9))
  {
    return toolError(
      "invalid_argument",
      "/rotate/axis must not be zero",
      "pass \"x\", \"y\", \"z\" or a non-zero vector",
      {{"pointer", "/rotate/axis"}});
  }
  return axis.value() / length;
}

vm::axis::type flipAxis(const QString& name)
{
  return name == u"x" ? vm::axis::x : name == u"y" ? vm::axis::y : vm::axis::z;
}

McpResult<QJsonObject> transformNodes(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto operations =
    std::array<const char*, 4>{"translate", "rotate", "scale", "flip"};
  const auto operationCount = std::ranges::count_if(
    operations, [&](const char* operation) { return args.contains(operation); });
  if (operationCount != 1)
  {
    return toolError(
      "invalid_argument",
      "pass exactly one of translate, rotate, scale or flip",
      "call transform_nodes once per operation, or use batch",
      {{"allowed", QJsonArray{"translate", "rotate", "scale", "flip"}}});
  }

  if (auto check = requireNoVertexHandlesSelected(map); check.is_error())
  {
    return errorOf(check);
  }

  auto targets = resolveTargets(doc, args.value("ids").toArray());
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  // validate the arguments before changing anything
  auto translation = vm::vec3d{};
  auto rotationAxisValue = vm::vec3d{};
  auto angle = 0.0;
  auto scaleFactors = vm::vec3d{};
  auto mirrorAxis = vm::axis::x;
  auto center = std::optional<vm::vec3d>{};

  const auto centerOf = [&](const QString& key) -> McpResult<void> {
    const auto object = args.value(key).toObject();
    if (object.contains("center"))
    {
      auto c = vec3(object.value("center"), "/" + key + "/center");
      if (c.is_error())
      {
        return errorOf(c);
      }
      center = c.value();
    }
    return kdl::void_success;
  };

  if (args.contains("translate"))
  {
    auto t = vec3(args.value("translate"), "/translate");
    if (t.is_error())
    {
      return errorOf(t);
    }
    translation = t.value();
  }
  else if (args.contains("rotate"))
  {
    const auto rotate = args.value("rotate").toObject();
    auto axis = rotationAxis(rotate.value("axis"));
    if (axis.is_error())
    {
      return errorOf(axis);
    }
    rotationAxisValue = axis.value();
    auto degrees = finiteNumber(
      rotate.value("angle_degrees"), "/rotate/angle_degrees", MaxAngleDegrees);
    if (degrees.is_error())
    {
      return errorOf(degrees);
    }
    angle = vm::to_radians(degrees.value());
    if (auto c = centerOf("rotate"); c.is_error())
    {
      return errorOf(c);
    }
  }
  else if (args.contains("scale"))
  {
    const auto scale = args.value("scale").toObject();
    auto factors = vec3(scale.value("factors"), "/scale/factors");
    if (factors.is_error())
    {
      return errorOf(factors);
    }
    for (size_t i = 0; i < 3; ++i)
    {
      const auto f = factors.value()[i];
      // tiny factors collapse brushes so that their face normals cannot be computed
      if (std::abs(f) < MinScaleFactor || std::abs(f) > MaxScaleFactor)
      {
        return toolError(
          "invalid_argument",
          QString{"/scale/factors/%1 must be between %2 and %3 in magnitude"}
            .arg(i)
            .arg(MinScaleFactor)
            .arg(MaxScaleFactor),
          "negative factors mirror the nodes",
          {{"pointer", QString{"/scale/factors/%1"}.arg(i)}});
      }
    }
    scaleFactors = factors.value();
    if (auto c = centerOf("scale"); c.is_error())
    {
      return errorOf(c);
    }
  }
  else
  {
    mirrorAxis = flipAxis(args.value("flip").toObject().value("axis").toString());
    if (auto c = centerOf("flip"); c.is_error())
    {
      return errorOf(c);
    }
  }

  if (auto check = checkTransformableNodes(
        doc.nodes,
        std::vector<const mdl::Node*>{targets.value().begin(), targets.value().end()});
      check.is_error())
  {
    return errorOf(check);
  }

  selectOnly(map, targets.value());
  const auto selectionBounds = map.selectionBounds();
  if (!selectionBounds)
  {
    return toolError("operation_failed", "the targets have no bounds");
  }
  const auto c = center.value_or(selectionBounds->center());

  const auto transformation =
    args.contains("translate") ? vm::translation_matrix(translation)
    : args.contains("rotate")
      ? vm::translation_matrix(c) * vm::rotation_matrix(rotationAxisValue, angle)
          * vm::translation_matrix(-c)
    : args.contains("scale")
      ? vm::translation_matrix(c) * vm::scaling_matrix(scaleFactors)
          * vm::translation_matrix(-c)
      : vm::translation_matrix(c) * vm::mirror_matrix<double>(mirrorAxis)
          * vm::translation_matrix(-c);

  if (!map.worldBounds().contains(selectionBounds->transform(transformation)))
  {
    return toolError(
      "out_of_world_bounds",
      "the transformed nodes would not be inside the world bounds",
      "use a smaller transformation",
      {{"world_bounds", toJson(map.worldBounds())}});
  }

  const auto success =
    args.contains("translate") ? mdl::translateSelection(map, translation)
    : args.contains("rotate")  ? mdl::rotateSelection(map, c, rotationAxisValue, angle)
    : args.contains("scale")   ? mdl::scaleSelection(map, c, scaleFactors)
                               : mdl::flipSelection(map, c, mirrorAxis);
  if (!success)
  {
    return toolError(
      "operation_failed",
      "the nodes could not be transformed",
      "a brush may have become invalid; check the log");
  }

  if (hasNonIntegerVertices(map.selection().allBrushes()))
  {
    context.warnings.push_back(
      "some brush vertices are no longer on integer coordinates");
  }

  const auto& newBounds = map.selectionBounds();
  return QJsonObject{
    {"bounds", newBounds ? QJsonValue{toJson(*newBounds)} : QJsonValue{}},
    {"alignment_lock", map.editorContext().alignmentLock()},
  };
}

McpToolDef transformNodesTool()
{
  const auto center = schema::vec3(
    "center of the operation (default: the center of the "
    "targets' bounds)");
  return {
    .name = "transform_nodes",
    .title = "Transform Nodes",
    .description =
      "Moves, rotates (degrees), scales or mirrors the given nodes; pass exactly one "
      "operation. Brush entities move with their brushes. Verify with "
      "get_brush_geometry or get_nodes.",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds()},
        {"translate", schema::vec3("offset in world units")},
        {"rotate",
         schema::object(
           {
             {"axis", schema::axis("\"x\", \"y\", \"z\" or a vector")},
             {"angle_degrees",
              schema::number(
                -MaxAngleDegrees, MaxAngleDegrees, "counter-clockwise angle")},
             {"center", center},
           },
           {"axis", "angle_degrees"})},
        {"scale",
         schema::object(
           {
             {"factors",
              schema::vec3("scale factors per axis, 0.0001 to 10000 in magnitude")},
             {"center", center},
           },
           {"factors"})},
        {"flip",
         schema::object(
           {
             {"axis", schema::stringEnum({"x", "y", "z"}, "the axis to mirror along")},
             {"center", center},
           },
           {"axis"})},
      },
      {"ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = transformNodes,
  };
}

// delete_nodes

McpResult<QJsonObject> deleteNodes(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto targets =
    resolveTargets(doc, args.value("ids").toArray(), McpTargetPurpose::Delete);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto deleted = QJsonArray{};
  for (const auto* node : targets.value())
  {
    if (!node->parent() || !node->parent()->canRemoveChild(*node))
    {
      const auto id = doc.nodes.idOf(*node);
      return toolError(
        "operation_failed",
        QString{"%1 cannot be removed from its parent"}.arg(id),
        {},
        {{"id", id}});
    }
    deleted.append(doc.nodes.idOf(*node));
  }

  // removing selected nodes would leave stale selection state
  mdl::deselectAll(map);
  mdl::removeNodes(map, targets.value());

  return QJsonObject{{"deleted", deleted}};
}

McpToolDef deleteNodesTool()
{
  return {
    .name = "delete_nodes",
    .title = "Delete Nodes",
    .description =
      "Deletes the given groups, entities, brushes and patches. Entities and groups "
      "that become empty are deleted too; all deleted ids are in changes.removed.",
    .inputSchema = schema::object({{"ids", schema::nodeIds()}}, {"ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Destructive,
    .allowedInBatch = true,
    .run = deleteNodes,
  };
}

// set_entity_properties

struct EntityTargets
{
  /** The entities to change, in id order. */
  std::vector<mdl::EntityNodeBase*> entities;
  /** The nodes to select so that Selection::allEntities() yields the entities. */
  std::vector<mdl::Node*> selection;
  bool world = false;
};

McpResult<EntityTargets> resolveEntityTargets(
  const McpDocumentState& doc, const QJsonArray& ids)
{
  auto nodes = resolveNodes(doc, ids, size_t(schema::MaxNodeIds));
  if (nodes.is_error())
  {
    return errorOf(nodes);
  }

  auto result = EntityTargets{};
  for (auto* node : nodes.value())
  {
    if (node == &doc.map.worldNode())
    {
      result.world = true;
    }
    else if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node))
    {
      result.entities.push_back(entityNode);
    }
    else
    {
      const auto id = doc.nodes.idOf(*node);
      return toolError(
        "wrong_node_type",
        QString{"%1 is a %2, not an entity"}.arg(id, nodeTypeName(*node)),
        "pass entity ids (E<n>) or world",
        {{"id", id}, {"actual", nodeTypeName(*node)}, {"expected", "entity or world"}});
    }
  }

  if (result.world)
  {
    if (!result.entities.empty())
    {
      return toolError(
        "invalid_argument",
        "world cannot be changed together with other entities",
        "call set_entity_properties once for world and once for the entities",
        {{"pointer", "/ids"}});
    }
    result.entities.push_back(&doc.map.worldNode());
    return result;
  }

  // point entities are selected themselves, brush entities through their brushes
  auto targets = resolveTargets(doc, ids, McpTargetPurpose::Edit);
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  result.selection = std::move(targets).value();
  return result;
}

McpResult<QJsonObject> setEntityProperties(
  McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto set = args.value("set").toObject();
  const auto remove = args.value("remove").toArray();
  const auto rename = args.value("rename").toObject();
  if (set.isEmpty() && remove.isEmpty() && rename.isEmpty())
  {
    return toolError(
      "invalid_argument",
      "pass at least one of set, remove or rename",
      "e.g. {\"ids\":[\"E3\"],\"set\":{\"light\":300}}");
  }

  auto targetsResult = resolveEntityTargets(doc, args.value("ids").toArray());
  if (targetsResult.is_error())
  {
    return errorOf(targetsResult);
  }
  const auto targets = std::move(targetsResult).value();

  const auto keyError = [](const QString& pointer, const QString& message) {
    return toolError("invalid_argument", message, {}, {{"pointer", pointer}});
  };
  const auto checkKey = [&](
                          const QString& key, const QString& pointer) -> McpResult<void> {
    if (auto check = checkMapText(key, McpMapText::Key); check.is_error())
    {
      auto error = errorOf(check);
      error.details["pointer"] = pointer;
      return error;
    }
    return kdl::void_success;
  };
  const auto hasKeyEverywhere = [&](const std::string& key) {
    return std::ranges::all_of(targets.entities, [&](const auto* entityNode) {
      return entityNode->entity().hasProperty(key);
    });
  };
  const auto hasKeyAnywhere = [&](const std::string& key) {
    return std::ranges::any_of(targets.entities, [&](const auto* entityNode) {
      return entityNode->entity().hasProperty(key);
    });
  };
  const auto missingKey = [&](const QString& key, const QString& pointer) {
    for (const auto* entityNode : targets.entities)
    {
      if (!entityNode->entity().hasProperty(fromUnicode(map, key)))
      {
        const auto id = doc.nodes.idOf(*entityNode);
        return toolError(
          "invalid_argument",
          QString{"%1 has no property '%2'"}.arg(id, key),
          "call get_nodes to see the properties",
          {{"pointer", pointer}, {"id", id}});
      }
    }
    return toolError("invalid_argument", QString{"missing property '%1'"}.arg(key));
  };

  // renames
  auto renames = std::vector<std::pair<std::string, std::string>>{};
  auto newKeys = std::unordered_set<QString>{};
  for (auto it = rename.begin(); it != rename.end(); ++it)
  {
    const auto pointer = "/rename/" + it.key();
    if (!it.value().isString())
    {
      return keyError(pointer, QString{"%1 must be a string"}.arg(pointer));
    }
    const auto newKey = it.value().toString();
    if (auto check = checkKey(it.key(), pointer); check.is_error())
    {
      return errorOf(check);
    }
    if (auto check = checkKey(newKey, pointer); check.is_error())
    {
      return errorOf(check);
    }
    if (it.key() == u"classname" || newKey == u"classname")
    {
      return keyError(pointer, "classname cannot be renamed");
    }
    if (!hasKeyEverywhere(fromUnicode(map, it.key())))
    {
      return missingKey(it.key(), pointer);
    }
    if (newKey != it.key() && hasKeyAnywhere(fromUnicode(map, newKey)))
    {
      return keyError(
        pointer,
        QString{"cannot rename '%1' to '%2': '%2' already exists"}.arg(it.key(), newKey));
    }
    if (rename.contains(newKey) && newKey != it.key())
    {
      return keyError(
        pointer, QString{"'%1' is renamed and also the target of a rename"}.arg(newKey));
    }
    if (!newKeys.insert(newKey).second)
    {
      return keyError(pointer, QString{"'%1' is the target of two renames"}.arg(newKey));
    }
    for (const auto* entityNode : targets.entities)
    {
      if (const auto* value = entityNode->entity().property(fromUnicode(map, it.key())))
      {
        if (auto check =
              checkEntityPropertyValue(newKey, toUnicode(map, *value), pointer);
            check.is_error())
        {
          return errorOf(check);
        }
      }
    }
    renames.emplace_back(fromUnicode(map, it.key()), fromUnicode(map, newKey));
  }

  // sets
  auto sets = std::vector<std::pair<std::string, std::string>>{};
  for (auto it = set.begin(); it != set.end(); ++it)
  {
    const auto pointer = "/set/" + it.key();
    if (auto check = checkKey(it.key(), pointer); check.is_error())
    {
      return errorOf(check);
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
    if (it.key() == u"classname")
    {
      if (targets.world)
      {
        return keyError(pointer, "the classname of world cannot be changed");
      }
      if (
        value.value().isEmpty()
        || value.value().compare(u"worldspawn", Qt::CaseInsensitive) == 0)
      {
        return keyError(pointer, "classname must not be empty or worldspawn");
      }
    }
    sets.emplace_back(fromUnicode(map, it.key()), fromUnicode(map, value.value()));
  }

  // removals
  auto removals = std::vector<std::string>{};
  for (qsizetype i = 0; i < remove.size(); ++i)
  {
    const auto pointer = QString{"/remove/%1"}.arg(i);
    const auto key = remove[i].toString();
    if (auto check = checkKey(key, pointer); check.is_error())
    {
      return errorOf(check);
    }
    if (key == u"classname")
    {
      return keyError(pointer, "classname cannot be removed");
    }
    if (!hasKeyEverywhere(fromUnicode(map, key)))
    {
      return missingKey(key, pointer);
    }
    removals.push_back(fromUnicode(map, key));
  }

  // with nothing selected, the property functions change worldspawn
  selectOnly(map, targets.selection);

  auto expected = targets.entities;
  std::ranges::sort(expected);
  auto actual = map.selection().allEntities();
  std::ranges::sort(actual);
  if (actual != expected)
  {
    return toolError(
      "operation_failed", "the entities could not be selected for editing");
  }

  for (const auto& [oldKey, newKey] : renames)
  {
    if (!mdl::renameEntityProperty(map, oldKey, newKey))
    {
      return toolError(
        "operation_failed",
        QString{"property '%1' could not be renamed"}.arg(toUnicode(map, oldKey)));
    }
  }
  for (const auto& [key, value] : sets)
  {
    if (!mdl::setEntityProperty(map, key, value))
    {
      return toolError(
        "operation_failed",
        QString{"property '%1' could not be set"}.arg(toUnicode(map, key)));
    }
  }
  for (const auto& key : removals)
  {
    if (!mdl::removeEntityProperty(map, key))
    {
      return toolError(
        "operation_failed",
        QString{"property '%1' could not be removed"}.arg(toUnicode(map, key)));
    }
  }

  auto entities = QJsonArray{};
  for (const auto* entityNode : targets.entities)
  {
    const auto& entity = entityNode->entity();
    if (entity.hasProperty("angle") && entity.hasProperty("angles"))
    {
      context.warnings.push_back(
        QString{"%1 has both angle and angles; angles takes precedence"}.arg(
          doc.nodes.idOf(*entityNode)));
    }
    entities.append(QJsonObject{
      {"id", doc.nodes.idOf(*entityNode)},
      {"properties", propertiesJson(map, entity)},
    });
  }

  return QJsonObject{{"entities", entities}};
}

McpToolDef setEntityPropertiesTool()
{
  return {
    .name = "set_entity_properties",
    .title = "Set Entity Properties",
    .description =
      "Renames, sets and removes properties (in this order) of the given entities, or of "
      "worldspawn with ids [\"world\"]. Values may be strings, numbers, booleans or "
      "[x,y,z]. Verify with get_nodes.",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds(schema::MaxNodeIds, "entity ids, or [\"world\"]")},
        {"set",
         schema::describe({{"type", "object"}}, "key to value, e.g. {\"light\": 300}")},
        {"remove", schema::array(schema::string(1, 256), 1, 256, "keys to remove")},
        {"rename",
         schema::describe(
           {{"type", "object"}},
           "old key to new key, e.g. {\"target\": \"killtarget\"}")},
      },
      {"ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = setEntityProperties,
  };
}

// set_face_attributes

McpResult<QJsonObject> setFaceAttributes(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  if (args.contains("faces") == args.contains("brushes"))
  {
    return toolError(
      "invalid_argument",
      "pass exactly one of faces or brushes",
      "faces selects single faces, brushes all faces of the brushes");
  }

  auto update = mdl::UpdateBrushFaceAttributes{};
  auto material = std::optional<std::string>{};
  if (args.contains("material"))
  {
    const auto name = args.value("material").toString();
    if (auto check = checkMaterialName(name, "/material"); check.is_error())
    {
      return errorOf(check);
    }
    material = fromUnicode(map, name);
    update.materialName = material;
  }
  if (args.contains("offset"))
  {
    auto offset = vec2(args.value("offset"), "/offset", MaxUvValue);
    if (offset.is_error())
    {
      return errorOf(offset);
    }
    update.xOffset = mdl::SetValue{float(offset.value().x())};
    update.yOffset = mdl::SetValue{float(offset.value().y())};
  }
  if (args.contains("scale"))
  {
    auto scale = vec2(args.value("scale"), "/scale", MaxUvValue);
    if (scale.is_error())
    {
      return errorOf(scale);
    }
    for (size_t i = 0; i < 2; ++i)
    {
      if (std::abs(scale.value()[i]) < 1e-6)
      {
        return toolError(
          "invalid_argument",
          QString{"/scale/%1 must not be 0"}.arg(i),
          "negative scales mirror the texture",
          {{"pointer", QString{"/scale/%1"}.arg(i)}});
      }
    }
    update.xScale = mdl::SetValue{float(scale.value().x())};
    update.yScale = mdl::SetValue{float(scale.value().y())};
  }
  if (args.contains("rotation"))
  {
    auto rotation = finiteNumber(args.value("rotation"), "/rotation", MaxUvValue);
    if (rotation.is_error())
    {
      return errorOf(rotation);
    }
    update.rotation = mdl::SetValue{float(rotation.value())};
  }
  if (args.contains("surface"))
  {
    const auto surface = args.value("surface").toObject();
    if (surface.contains("contents"))
    {
      update.surfaceContents = mdl::SetFlags{surface.value("contents").toInt()};
    }
    if (surface.contains("flags"))
    {
      update.surfaceFlags = mdl::SetFlags{surface.value("flags").toInt()};
    }
    if (surface.contains("value"))
    {
      auto value =
        finiteNumber(surface.value("value"), "/surface/value", MaxSurfaceValue);
      if (value.is_error())
      {
        return errorOf(value);
      }
      update.surfaceValue = mdl::SetValue{float(value.value())};
    }
  }
  if (args.contains("align"))
  {
    const auto align = args.value("align").toString();
    update.axis = align == u"reset"      ? mdl::AxisOp{mdl::ResetAxis{}}
                  : align == u"paraxial" ? mdl::AxisOp{mdl::ToParaxial{}}
                                         : mdl::AxisOp{mdl::ToParallel{}};
  }
  const auto fit = args.value("fit").toBool(false);

  const auto hasUpdate = update.materialName || update.xOffset || update.xScale
                         || update.rotation || update.surfaceContents
                         || update.surfaceFlags || update.surfaceValue || update.axis;
  if (!hasUpdate && !fit)
  {
    return toolError(
      "invalid_argument",
      "pass at least one attribute",
      "material, offset, scale, rotation, surface, align or fit:true");
  }

  auto faces = std::vector<mdl::BrushFaceHandle>{};
  auto brushes = std::vector<mdl::Node*>{};
  if (args.contains("faces"))
  {
    auto facesResult = resolveFaces(doc, args.value("faces").toArray());
    if (facesResult.is_error())
    {
      return errorOf(facesResult);
    }
    faces = std::move(facesResult).value();
    auto checked = std::unordered_set<const mdl::BrushNode*>{};
    for (const auto& handle : faces)
    {
      if (checked.insert(&handle.node()).second)
      {
        if (auto check = requireNotLinked(doc, handle.node()); check.is_error())
        {
          return errorOf(check);
        }
      }
    }
  }
  else
  {
    auto targets = resolveTargets(doc, args.value("brushes").toArray());
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    for (auto* node : targets.value())
    {
      if (!dynamic_cast<mdl::BrushNode*>(node))
      {
        const auto id = doc.nodes.idOf(*node);
        return toolError(
          "wrong_node_type",
          QString{"%1 is a %2, not a brush"}.arg(id, nodeTypeName(*node)),
          "pass brush ids; brush entities are expanded to their brushes",
          {{"id", id}, {"actual", nodeTypeName(*node)}, {"expected", "brush"}});
      }
    }
    brushes = std::move(targets).value();
  }

  mdl::deselectAll(map);
  if (!faces.empty())
  {
    mdl::selectBrushFaces(map, faces);
  }
  else
  {
    mdl::selectNodes(map, brushes);
  }

  const auto faceCount = map.selection().allBrushFaces().size();
  if (faceCount == 0)
  {
    return toolError("operation_failed", "no faces could be selected");
  }

  if (hasUpdate && !mdl::setBrushFaceAttributes(map, update))
  {
    return toolError(
      "operation_failed",
      "the face attributes could not be set",
      "check that offset, scale and rotation are valid");
  }
  if (fit)
  {
    mdl::autoFitUv(map);
  }

  if (
    material && *material != mdl::BrushFace::NoMaterialName
    && !map.materialManager().material(*material))
  {
    context.warnings.push_back(
      QString{"material '%1' was not found; check list_materials"}.arg(
        toUnicode(map, *material)));
  }

  return QJsonObject{{"faces_changed", qint64(faceCount)}};
}

McpToolDef setFaceAttributesTool()
{
  return {
    .name = "set_face_attributes",
    .title = "Set Face Attributes",
    .description =
      "Sets the material, UV offset, scale and rotation (degrees), surface attributes "
      "or UV alignment of the given faces or of all faces of the given brushes; fit "
      "fits the texture to each face. Verify with get_brush_geometry.",
    .inputSchema = schema::object({
      {"faces", schema::array(schema::faceRef(), 1, MaxFaceRefs, "faces to change")},
      {"brushes", schema::nodeIds(schema::MaxNodeIds, "brushes whose faces to change")},
      {"material", schema::material()},
      {"offset", schema::vec2("UV offset in texels")},
      {"scale", schema::vec2("UV scale; must not be 0")},
      {"rotation", schema::number(-MaxUvValue, MaxUvValue, "UV rotation in degrees")},
      {"surface",
       schema::object({
         {"contents",
          schema::integer(
            std::numeric_limits<int>::min(), std::numeric_limits<int>::max())},
         {"flags",
          schema::integer(
            std::numeric_limits<int>::min(), std::numeric_limits<int>::max())},
         {"value", schema::number(-MaxSurfaceValue, MaxSurfaceValue)},
       })},
      {"align",
       schema::stringEnum(
         {"reset", "paraxial", "parallel"}, "reset or convert the UV axes")},
      {"fit", schema::boolean("fit the texture to each face")},
    }),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = setFaceAttributes,
  };
}

// shared by the brush editing tools

QJsonArray idsOf(const McpNodeRegistry& registry, const auto& nodes)
{
  auto result = QJsonArray{};
  for (const auto* node : nodes)
  {
    result.append(registry.idOf(*node));
  }
  return result;
}

mcp::ToolError notABrush(const McpDocumentState& doc, const mdl::Node& node)
{
  const auto id = doc.nodes.idOf(node);
  return toolError(
    "wrong_node_type",
    QString{"%1 is a %2, not a brush"}.arg(id, nodeTypeName(node)),
    "pass brush ids; brush entities are expanded to their brushes",
    {{"id", id}, {"actual", nodeTypeName(node)}, {"expected", "brush"}});
}

/** Resolves the given ids to brushes; brush entities are expanded to their brushes. */
McpResult<std::vector<mdl::BrushNode*>> resolveBrushes(
  const McpDocumentState& doc, const QJsonArray& ids)
{
  auto targets = resolveTargets(doc, ids);
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  auto result = std::vector<mdl::BrushNode*>{};
  for (auto* node : targets.value())
  {
    auto* brushNode = dynamic_cast<mdl::BrushNode*>(node);
    if (!brushNode)
    {
      return notABrush(doc, *node);
    }
    result.push_back(brushNode);
  }
  return result;
}

/** Whether the brush can be edited although it may be inside a closed group. */
bool isEditableBrush(const McpDocumentState& doc, const mdl::BrushNode& brushNode)
{
  const auto& editorContext = doc.map.editorContext();
  return editorContext.visible(brushNode) && editorContext.editable(brushNode)
         && requireNotLinked(doc, brushNode).is_success();
}

/**
 * Resolves the given ids to brushes. Brush entities and groups are expanded to the
 * brushes inside them that are visible, editable and not linked; the number of skipped
 * brushes and patches is returned too.
 */
McpResult<std::pair<std::vector<mdl::BrushNode*>, size_t>> resolveBrushesInside(
  const McpDocumentState& doc, const QJsonArray& ids)
{
  auto targets = resolveTargets(doc, ids);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto result = std::vector<mdl::BrushNode*>{};
  auto seen = std::unordered_set<const mdl::BrushNode*>{};
  auto skipped = size_t(0);
  const auto collect = [&](const auto& self, mdl::Node& node) -> void {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
    {
      if (!isEditableBrush(doc, *brushNode))
      {
        ++skipped;
      }
      else if (seen.insert(brushNode).second)
      {
        result.push_back(brushNode);
      }
      return;
    }
    for (auto* child : node.children())
    {
      self(self, *child);
    }
  };

  for (auto* node : targets.value())
  {
    if (dynamic_cast<mdl::BrushNode*>(node) || dynamic_cast<mdl::GroupNode*>(node))
    {
      collect(collect, *node);
    }
    else if (dynamic_cast<mdl::PatchNode*>(node))
    {
      // e.g. a patch of a brush entity
      ++skipped;
    }
    else
    {
      return notABrush(doc, *node);
    }
  }
  return std::pair{std::move(result), skipped};
}

/**
 * Returns the ids in after that are not in before. Uses a hash set, since
 * duplicate_nodes can create hundreds of thousands of nodes.
 */
QJsonArray newIds(const std::vector<QString>& before, const std::vector<QString>& after)
{
  const auto beforeSet = QSet<QString>{before.begin(), before.end()};
  auto result = QJsonArray{};
  for (const auto& id : after)
  {
    if (!beforeSet.contains(id))
    {
      result.append(id);
    }
  }
  return result;
}

std::vector<vm::vec3d> sortedVertices(const mdl::Brush& brush)
{
  auto vertices = brush.vertexPositions();
  std::ranges::sort(vertices);
  return vertices;
}

// resize_nodes

McpResult<QJsonObject> resizeNodes(McpCallContext& context, const QJsonObject& args)
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
  const auto newBounds = boxResult.value();

  if (auto check = requireNoVertexHandlesSelected(map); check.is_error())
  {
    return errorOf(check);
  }

  auto targets = resolveTargets(doc, args.value("ids").toArray());
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  if (auto check = checkTransformableNodes(
        doc.nodes,
        std::vector<const mdl::Node*>{targets.value().begin(), targets.value().end()});
      check.is_error())
  {
    return errorOf(check);
  }

  selectOnly(map, targets.value());
  const auto oldBounds = map.selectionBounds();
  if (!oldBounds)
  {
    return toolError("operation_failed", "the targets have no bounds");
  }

  static const auto axes = std::array<const char*, 3>{"x", "y", "z"};
  const auto oldSize = oldBounds->size();
  const auto newSize = newBounds.size();
  for (size_t i = 0; i < 3; ++i)
  {
    if (oldSize[i] < 1.0 / 1024.0)
    {
      return toolError(
        "invalid_argument",
        QString{"the targets are flat along %1 and cannot be resized"}.arg(axes[i]),
        "use transform_nodes to move them",
        {{"axis", axes[i]}});
    }
    const auto factor = newSize[i] / oldSize[i];
    if (factor < MinScaleFactor || factor > MaxScaleFactor)
    {
      return toolError(
        "invalid_argument",
        QString{"the size along %1 would change by a factor of %2"}.arg(axes[i]).arg(
          factor),
        QString{"the factor must be between %1 and %2"}
          .arg(MinScaleFactor)
          .arg(MaxScaleFactor),
        {{"axis", axes[i]}, {"pointer", "/bounds"}});
    }
  }

  if (!mdl::scaleSelection(map, *oldBounds, newBounds))
  {
    return toolError(
      "operation_failed",
      "the nodes could not be resized",
      "a brush may have become invalid; check the log");
  }

  if (hasNonIntegerVertices(map.selection().allBrushes()))
  {
    context.warnings.push_back(
      "some brush vertices are not on integer coordinates; call snap_vertices to fix "
      "them");
  }

  const auto& resultBounds = map.selectionBounds();
  return QJsonObject{
    {"bounds", resultBounds ? QJsonValue{toJson(*resultBounds)} : QJsonValue{}},
  };
}

McpToolDef resizeNodesTool()
{
  return {
    .name = "resize_nodes",
    .title = "Resize Nodes",
    .description =
      "Scales and moves the given nodes so that their combined bounds become the given "
      "bounds, like dragging the bounds in the editor. Verify with get_brush_geometry.",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds()},
        {"bounds", schema::bounds("the new combined bounds")},
      },
      {"ids", "bounds"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = resizeNodes,
  };
}

// snap_vertices

McpResult<QJsonObject> snapVertices(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto snapTo = args.value("to").toDouble(1.0);

  auto brushes = resolveBrushes(doc, args.value("ids").toArray());
  if (brushes.is_error())
  {
    return errorOf(brushes);
  }

  auto before = std::vector<std::vector<vm::vec3d>>{};
  for (const auto* brushNode : brushes.value())
  {
    before.push_back(sortedVertices(brushNode->brush()));
  }

  selectOnly(
    map, std::vector<mdl::Node*>{brushes.value().begin(), brushes.value().end()});
  if (!mdl::snapVertices(map, snapTo))
  {
    return toolError(
      "operation_failed", "the vertices could not be snapped", "check the log");
  }

  auto changed = QJsonArray{};
  auto unchanged = QJsonArray{};
  for (size_t i = 0; i < brushes.value().size(); ++i)
  {
    const auto* brushNode = brushes.value()[i];
    (sortedVertices(brushNode->brush()) != before[i] ? changed : unchanged)
      .append(doc.nodes.idOf(*brushNode));
  }
  if (changed.isEmpty())
  {
    context.warnings.push_back(
      "nothing changed: the vertices were already snapped or could not be snapped");
  }

  return QJsonObject{{"changed", changed}, {"unchanged", unchanged}};
}

McpToolDef snapVerticesTool()
{
  return {
    .name = "snap_vertices",
    .title = "Snap Vertices",
    .description =
      "Moves the vertices of the given brushes to the nearest multiple of to (default "
      "1, i.e. integer coordinates). Brushes that would become invalid are left "
      "unchanged and listed in unchanged.",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds(schema::MaxNodeIds, "brushes or brush entities")},
        {"to", schema::number(0.001, 4096, "grid to snap to (default 1)")},
      },
      {"ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = snapVertices,
  };
}

// duplicate_nodes

constexpr auto MaxDuplicates = 64;

McpResult<QJsonObject> duplicateNodes(McpCallContext& context, const QJsonObject& args)
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
  const auto count = sizeArgument(args, "count", 1);

  if (auto check = requireNoVertexHandlesSelected(map); check.is_error())
  {
    return errorOf(check);
  }

  auto targets = resolveTargets(doc, args.value("ids").toArray());
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  if (auto check = checkTransformableNodes(
        doc.nodes,
        std::vector<const mdl::Node*>{targets.value().begin(), targets.value().end()});
      check.is_error())
  {
    return errorOf(check);
  }

  selectOnly(map, targets.value());
  const auto selectionBounds = map.selectionBounds();
  if (!selectionBounds)
  {
    return toolError("operation_failed", "the targets have no bounds");
  }
  for (size_t k = 1; k <= count; ++k)
  {
    if (!map.worldBounds().contains(selectionBounds->translate(offset * double(k))))
    {
      return toolError(
        "out_of_world_bounds",
        QString{"copy %1 would not be inside the world bounds"}.arg(k),
        "use a smaller offset or count",
        {{"world_bounds", toJson(map.worldBounds())}});
    }
  }

  auto copies = QJsonArray{};
  for (size_t k = 0; k < count; ++k)
  {
    const auto before = doc.nodes.currentChanges().created;
    mdl::duplicateSelectedNodes(map);
    if (!map.selection().hasNodes())
    {
      return toolError("operation_failed", "the nodes could not be duplicated");
    }
    if (offset != vm::vec3d{0, 0, 0} && !mdl::translateSelection(map, offset))
    {
      return toolError("operation_failed", "the copies could not be moved");
    }

    copies.append(newIds(before, doc.nodes.currentChanges().created));
  }

  if (offset == vm::vec3d{0, 0, 0})
  {
    context.warnings.push_back("the offset is zero, so the copies overlap the originals");
  }

  return QJsonObject{{"copies", copies}};
}

McpToolDef duplicateNodesTool()
{
  return {
    .name = "duplicate_nodes",
    .title = "Duplicate Nodes",
    .description =
      "Copies the given nodes count times; copy k is moved by k times offset, e.g. a "
      "row of pillars. Returns the ids of each copy (including the brushes inside "
      "copied groups and entities).",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds()},
        {"offset", schema::vec3("offset between copies (default [0,0,0])")},
        {"count", schema::integer(1, MaxDuplicates, "number of copies (default 1)")},
      },
      {"ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = duplicateNodes,
  };
}

// replace_material

McpResult<QJsonObject> replaceMaterial(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto from = fromUnicode(map, args.value("from").toString());
  const auto toName = args.value("to").toString();
  if (auto check = checkMaterialName(toName, "/to"); check.is_error())
  {
    return errorOf(check);
  }
  const auto to = fromUnicode(map, toName);

  auto brushes = std::vector<mdl::BrushNode*>{};
  auto skipped = size_t(0);
  if (args.contains("ids"))
  {
    auto resolved = resolveBrushesInside(doc, args.value("ids").toArray());
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    std::tie(brushes, skipped) = std::move(resolved).value();
  }
  else
  {
    const auto visit = [&](const auto& self, mdl::Node& node) -> void {
      if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
      {
        if (isEditableBrush(doc, *brushNode))
        {
          brushes.push_back(brushNode);
        }
        else
        {
          ++skipped;
        }
        return;
      }
      for (auto* child : node.children())
      {
        self(self, *child);
      }
    };
    visit(visit, map.worldNode());
  }

  auto faces = std::vector<mdl::BrushFaceHandle>{};
  const auto& editorContext = map.editorContext();
  for (auto* brushNode : brushes)
  {
    const auto& brushFaces = brushNode->brush().faces();
    for (size_t i = 0; i < brushFaces.size(); ++i)
    {
      if (
        kdl::ci::str_is_equal(brushFaces[i].materialName(), from)
        && editorContext.selectable(*brushNode, brushFaces[i]))
      {
        faces.emplace_back(*brushNode, i);
      }
    }
  }

  if (skipped > 0)
  {
    context.warnings.push_back(
      QString{"%1 hidden, locked or linked brushes were skipped"}.arg(skipped));
  }
  if (faces.empty())
  {
    context.warnings.push_back(
      QString{"no face has the material '%1'"}.arg(args.value("from").toString()));
    return QJsonObject{{"faces_changed", 0}, {"brushes", QJsonArray{}}};
  }

  mdl::deselectAll(map);
  mdl::selectBrushFaces(map, faces);
  if (!mdl::setBrushFaceAttributes(
        map, mdl::UpdateBrushFaceAttributes{.materialName = to}))
  {
    return toolError("operation_failed", "the material could not be set");
  }

  auto changedBrushes = std::vector<const mdl::BrushNode*>{};
  for (const auto& handle : faces)
  {
    if (std::ranges::find(changedBrushes, &handle.node()) == changedBrushes.end())
    {
      changedBrushes.push_back(&handle.node());
    }
  }

  if (to != mdl::BrushFace::NoMaterialName && !map.materialManager().material(to))
  {
    context.warnings.push_back(
      QString{"material '%1' was not found; check list_materials"}.arg(toName));
  }

  return QJsonObject{
    {"faces_changed", qint64(faces.size())},
    {"brushes", idsOf(doc.nodes, changedBrushes)},
  };
}

McpToolDef replaceMaterialTool()
{
  return {
    .name = "replace_material",
    .title = "Replace Material",
    .description =
      "Replaces the material from (case-insensitive) by to on every face of the given "
      "brushes, groups and entities, or of the whole map. Hidden and locked brushes are "
      "skipped.",
    .inputSchema = schema::object(
      {
        {"from", schema::string(1, 256, "the material to replace")},
        {"to", schema::material("the new material")},
        {"ids",
         schema::nodeIds(
           schema::MaxNodeIds, "brushes, groups or entities (default: the whole map)")},
      },
      {"from", "to"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = replaceMaterial,
  };
}

// csg

McpResult<QJsonObject> csg(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto op = args.value("op").toString();
  auto brushes = resolveBrushes(doc, args.value("ids").toArray());
  if (brushes.is_error())
  {
    return errorOf(brushes);
  }
  const auto minCount = op == u"hollow" ? size_t(1) : size_t(2);
  if (brushes.value().size() < minCount)
  {
    return toolError(
      "invalid_argument",
      QString{"%1 needs at least %2 brushes"}.arg(op).arg(minCount),
      {},
      {{"pointer", "/ids"}});
  }

  if (op == u"intersect")
  {
    // TrenchBroom deletes the brushes if they do not intersect
    auto intersection = brushes.value().front()->brush();
    for (size_t i = 1; i < brushes.value().size(); ++i)
    {
      if (intersection.intersect(map.worldBounds(), brushes.value()[i]->brush())
            .is_error())
      {
        return toolError(
          "operation_failed",
          "the brushes do not have a common volume",
          "check with check_brush_overlaps");
      }
    }
  }

  selectOnly(
    map, std::vector<mdl::Node*>{brushes.value().begin(), brushes.value().end()});

  const auto before = doc.nodes.currentChanges().created;
  const auto success = op == u"merge"       ? mdl::csgConvexMerge(map)
                       : op == u"intersect" ? mdl::csgIntersect(map)
                                            : mdl::csgHollow(map);
  if (!success)
  {
    return toolError(
      "operation_failed",
      QString{"csg %1 failed"}.arg(op),
      op == u"hollow"
        ? QStringLiteral("the brushes may be thinner than twice the grid size; try "
                         "create_hollow_box")
        : QStringLiteral("check the log"),
      {{"log", [&]() {
          auto log = QJsonArray{};
          if (context.log)
          {
            for (const auto& line : *context.log)
            {
              log.append(line.text);
            }
          }
          return log;
        }()}});
  }

  const auto ids = newIds(before, doc.nodes.currentChanges().created);
  if (op != u"hollow" && ids.isEmpty())
  {
    // csgIntersect deletes the brushes and reports success if their intersection is
    // empty; the check above should catch that, but the order of the brushes may differ
    return toolError(
      "operation_failed",
      QString{"csg %1 did not create a brush"}.arg(op),
      "check with check_brush_overlaps");
  }

  auto result = QJsonObject{{"ids", ids}};
  if (op == u"hollow")
  {
    result["wall_thickness"] = canon(map.grid().actualSize());
  }
  return result;
}

McpToolDef csgTool()
{
  return {
    .name = "csg",
    .title = "CSG",
    .description =
      "merge replaces the brushes by their convex hull, intersect by their common "
      "volume, hollow turns each brush into walls as thick as the current grid size "
      "(create_hollow_box gives more control). The results get new ids.",
    .inputSchema = schema::object(
      {
        {"op", schema::stringEnum({"merge", "intersect", "hollow"})},
        {"ids", schema::nodeIds(schema::MaxNodeIds, "brushes or brush entities")},
      },
      {"op", "ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Destructive,
    .allowedInBatch = true,
    .run = csg,
  };
}

// csg_subtract

constexpr auto MinOverlapVolume = 1e-3;
constexpr auto ExtentEpsilon = 1e-6;

/** Whether the brushes share a volume, not only a face or an edge. */
bool overlaps(const mdl::Map& map, const mdl::Brush& lhs, const mdl::Brush& rhs)
{
  const auto& a = lhs.bounds();
  const auto& b = rhs.bounds();
  for (size_t i = 0; i < 3; ++i)
  {
    if (std::min(a.max[i], b.max[i]) - std::max(a.min[i], b.min[i]) <= ExtentEpsilon)
    {
      return false;
    }
  }
  auto intersection = lhs;
  return intersection.intersect(map.worldBounds(), rhs).is_success()
         && brushVolume(intersection) > MinOverlapVolume;
}

McpResult<QJsonObject> csgSubtract(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto subtrahendsResult =
    resolveBrushesInside(doc, args.value("subtract_ids").toArray());
  if (subtrahendsResult.is_error())
  {
    auto error = errorOf(subtrahendsResult);
    error.details["pointer"] = "/subtract_ids";
    return error;
  }
  const auto subtrahends = subtrahendsResult.value().first;
  if (subtrahends.empty())
  {
    return toolError(
      "invalid_argument",
      "subtract_ids contain no editable brushes",
      {},
      {{"pointer", "/subtract_ids"}});
  }
  const auto subtrahendSet =
    std::unordered_set<const mdl::BrushNode*>{subtrahends.begin(), subtrahends.end()};

  auto candidates = std::vector<mdl::BrushNode*>{};
  if (args.contains("from_ids"))
  {
    auto fromResult = resolveBrushesInside(doc, args.value("from_ids").toArray());
    if (fromResult.is_error())
    {
      auto error = errorOf(fromResult);
      error.details["pointer"] = "/from_ids";
      return error;
    }
    candidates = fromResult.value().first;
    for (const auto* brushNode : candidates)
    {
      if (subtrahendSet.contains(brushNode))
      {
        const auto id = doc.nodes.idOf(*brushNode);
        return toolError(
          "invalid_argument",
          QString{"%1 is in subtract_ids and in from_ids"}.arg(id),
          "a brush cannot be subtracted from itself",
          {{"pointer", "/from_ids"}, {"id", id}});
      }
    }
  }
  else
  {
    const auto visit = [&](const auto& self, mdl::Node& node) -> void {
      if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
      {
        if (!subtrahendSet.contains(brushNode) && isEditableBrush(doc, *brushNode))
        {
          candidates.push_back(brushNode);
        }
        return;
      }
      for (auto* child : node.children())
      {
        self(self, *child);
      }
    };
    visit(visit, map.worldNode());
  }

  // touching brushes are not minuends, unlike in TrenchBroom's subtract command
  struct Minuend
  {
    mdl::BrushNode* node;
    std::vector<const mdl::Brush*> subtrahends;
  };
  auto minuends = std::vector<Minuend>{};
  for (auto* candidate : candidates)
  {
    auto overlapping = std::vector<const mdl::Brush*>{};
    for (const auto* subtrahend : subtrahends)
    {
      if (overlaps(map, candidate->brush(), subtrahend->brush()))
      {
        overlapping.push_back(&subtrahend->brush());
      }
    }
    if (!overlapping.empty())
    {
      minuends.push_back({candidate, std::move(overlapping)});
    }
  }
  if (minuends.empty())
  {
    return toolError(
      "operation_failed",
      "nothing overlaps the subtracted brushes",
      "brushes that only touch are not changed; check with check_brush_overlaps");
  }

  auto toAdd = kdl::flat_map<mdl::Node*, std::vector<std::unique_ptr<mdl::Node>>>{};
  auto fragmentNodes = std::vector<std::vector<const mdl::Node*>>{};
  auto failedFragments = size_t(0);
  for (const auto& minuend : minuends)
  {
    auto& fragments = fragmentNodes.emplace_back();
    for (auto& fragment : minuend.node->brush().subtract(
           map.worldNode().mapFormat(),
           map.worldBounds(),
           map.currentMaterialName(),
           minuend.subtrahends))
    {
      if (fragment.is_error())
      {
        ++failedFragments;
        continue;
      }
      auto node = std::make_unique<mdl::BrushNode>(std::move(fragment).value());
      fragments.push_back(node.get());
      toAdd[minuend.node->parent()].push_back(std::move(node));
    }
  }

  auto toRemove = std::vector<mdl::Node*>{};
  auto fragmentsJson = QJsonObject{};
  for (size_t i = 0; i < minuends.size(); ++i)
  {
    toRemove.push_back(minuends[i].node);
  }
  const auto keep = args.value("keep_subtractors").toBool(false);
  if (!keep)
  {
    toRemove.insert(toRemove.end(), subtrahends.begin(), subtrahends.end());
  }
  const auto minuendIds = idsOf(doc.nodes, toRemove);

  mdl::deselectAll(map);
  if (!toAdd.empty() && mdl::addNodes(map, std::move(toAdd)).empty())
  {
    return toolError("operation_failed", "the fragments could not be added");
  }
  mdl::removeNodes(map, toRemove);

  for (size_t i = 0; i < minuends.size(); ++i)
  {
    fragmentsJson[minuendIds[qsizetype(i)].toString()] =
      idsOf(doc.nodes, fragmentNodes[i]);
  }

  if (failedFragments > 0)
  {
    context.warnings.push_back(
      QString{"%1 fragments were invalid and left out; check the result"}.arg(
        failedFragments));
  }

  return QJsonObject{
    {"fragments", fragmentsJson},
    {"subtractors_kept", keep},
  };
}

McpToolDef csgSubtractTool()
{
  return {
    .name = "csg_subtract",
    .title = "CSG Subtract",
    .description =
      "Cuts the brushes of subtract_ids out of the brushes they overlap (or out of "
      "from_ids), e.g. to cut a doorway into a wall. Brushes that only touch are not "
      "changed. The cut brushes are replaced by fragments with new ids; the subtracted "
      "brushes are deleted unless keep_subtractors is true.",
    .inputSchema = schema::object(
      {
        {"subtract_ids",
         schema::nodeIds(schema::MaxNodeIds, "brushes, groups or brush entities")},
        {"from_ids",
         schema::nodeIds(
           schema::MaxNodeIds, "brushes to cut (default: all brushes they overlap)")},
        {"keep_subtractors", schema::boolean("keep the subtracted brushes")},
      },
      {"subtract_ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Destructive,
    .allowedInBatch = true,
    .run = csgSubtract,
  };
}

} // namespace

void registerMcpMapEditTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(transformNodesTool());
  dispatcher.add(deleteNodesTool());
  dispatcher.add(setEntityPropertiesTool());
  dispatcher.add(setFaceAttributesTool());
  dispatcher.add(resizeNodesTool());
  dispatcher.add(snapVerticesTool());
  dispatcher.add(duplicateNodesTool());
  dispatcher.add(replaceMaterialTool());
  dispatcher.add(csgTool());
  dispatcher.add(csgSubtractTool());
}

} // namespace tb::ui
