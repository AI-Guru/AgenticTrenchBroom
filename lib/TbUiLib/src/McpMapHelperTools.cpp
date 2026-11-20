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
#include <QStringList>

#include "base/Error.h"
#include "gl/MaterialManager.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Map_World.h"
#include "mdl/NodeHandleManager.h"
#include "mdl/NodeHandles.h"
#include "mdl/PatchNode.h"
#include "mdl/SoftMapBounds.h"
#include "mdl/WorldNode.h"
#include "ui/McpJson.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpSelectionGuard.h"
#include "ui/McpToolDispatcher.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

namespace tb::ui
{
namespace
{

const auto Mutating =
  mcp::ToolAnnotations{.readOnly = false, .destructive = false, .idempotent = false};
const auto ReadOnly =
  mcp::ToolAnnotations{.readOnly = true, .destructive = false, .idempotent = true};

/** Extents up to this size are treated as zero. */
constexpr auto Epsilon = 1e-6;

constexpr auto MinSize = 0.001;
constexpr auto MaxSize = 65536.0;

// shared by the helper tools

/** Returns the material named by the first of the given keys that is present. */
McpResult<std::string> materialArgument(
  const mdl::Map& map, const QJsonObject& args, const QStringList& keys)
{
  for (const auto& key : keys)
  {
    if (args.contains(key))
    {
      const auto name = args.value(key).toString();
      if (auto check = checkMaterialName(name, "/" + key); check.is_error())
      {
        return errorOf(check);
      }
      return fromUnicode(map, name);
    }
  }
  return map.currentMaterialName();
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

bool isIntegral(const vm::bbox3d& bounds)
{
  for (size_t i = 0; i < 3; ++i)
  {
    if (
      std::round(bounds.min[i]) != bounds.min[i]
      || std::round(bounds.max[i]) != bounds.max[i])
    {
      return false;
    }
  }
  return true;
}

void warnAboutBounds(McpCallContext& context, const vm::bbox3d& bounds)
{
  if (!isIntegral(bounds))
  {
    context.warnings.push_back(
      "some brush coordinates are not integers; keep brush vertices on integers");
  }

  const auto softBounds = mdl::softMapBounds(context.doc.map);
  if (softBounds.bounds && !softBounds.bounds->contains(bounds))
  {
    context.warnings.push_back(
      "the brushes are outside the soft map bounds (see get_map_info)");
  }
}

mdl::BrushBuilder brushBuilder(const mdl::Map& map)
{
  const auto& faceAttribsConfig = map.gameInfo().gameConfig.faceAttribsConfig;
  return mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    faceAttribsConfig.defaultUvAttributes,
    faceAttribsConfig.defaultSurfaceAttributes};
}

/** A cuboid brush to create. */
struct Cuboid
{
  vm::bbox3d bounds;
  std::string material;
};

/** Creates cuboid brushes, adds them to the parent and returns them in order. */
McpResult<std::vector<mdl::BrushNode*>> addCuboids(
  mdl::Map& map, mdl::Node& parent, const std::vector<Cuboid>& cuboids)
{
  const auto builder = brushBuilder(map);
  auto nodes = std::vector<std::unique_ptr<mdl::Node>>{};
  auto brushNodes = std::vector<mdl::BrushNode*>{};
  for (const auto& cuboid : cuboids)
  {
    auto brush = builder.createCuboid(cuboid.bounds, cuboid.material);
    if (brush.is_error())
    {
      return toolError(
        "geometry_invalid",
        QString::fromStdString(std::get<Error>(brush.error()).msg),
        "check the sizes");
    }
    auto brushNode = std::make_unique<mdl::BrushNode>(std::move(brush).value());
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

/** Groups the given nodes, which must have the same parent, and returns the group id. */
McpResult<QString> groupNewNodes(
  McpDocumentState& doc,
  const std::vector<mdl::BrushNode*>& brushNodes,
  const QString& name)
{
  auto& map = doc.map;
  mdl::deselectAll(map);
  mdl::selectNodes(map, std::vector<mdl::Node*>{brushNodes.begin(), brushNodes.end()});
  auto* groupNode = mdl::groupSelectedNodes(map, fromUnicode(map, name));
  if (!groupNode)
  {
    return toolError("operation_failed", "the brushes could not be grouped");
  }
  return doc.nodes.idOf(*groupNode);
}

size_t sideAxis(const QString& side)
{
  return side.endsWith(u'x') ? 0 : side.endsWith(u'y') ? 1 : 2;
}

double sideSign(const QString& side)
{
  return side.startsWith(u'+') ? 1.0 : -1.0;
}

mcp::ToolError invalidArgument(
  const QString& pointer, const QString& message, const QString& hint = {})
{
  return toolError(
    "invalid_argument", pointer + ": " + message, hint, {{"pointer", pointer}});
}

// create_room

constexpr auto MaxOpenings = 16;

const auto RoomWallSides = std::array<QString, 4>{"+x", "-x", "+y", "-y"};

struct Opening
{
  QString side;
  /** The extent along the wall. */
  double alongMin;
  double alongMax;
  double zMin;
  double zMax;
};

/** The axis along the wall on the given side. */
size_t alongAxis(const QString& side)
{
  return sideAxis(side) == 0 ? 1 : 0;
}

/**
 * Returns the bounds of the wall on the given side. The walls do not overlap: the x
 * walls span the outer box in y, the y walls span the inner box in x, and both span the
 * inner box in z. The floor and the ceiling span the outer box in x and y.
 */
vm::bbox3d roomWallBounds(
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
  return {{i.min.x(), o.min.y(), i.min.z()}, {i.max.x(), i.min.y(), i.max.z()}};
}

/** Returns the bounds of the given opening, spanning the thickness of the wall. */
vm::bbox3d openingBounds(const Opening& opening, const vm::bbox3d& wall)
{
  auto min = wall.min;
  auto max = wall.max;
  const auto along = alongAxis(opening.side);
  min[along] = opening.alongMin;
  max[along] = opening.alongMax;
  min[2] = opening.zMin;
  max[2] = opening.zMax;
  return {min, max};
}

/** Returns the parts of [min, max] that are not covered by the given intervals. */
std::vector<std::pair<double, double>> uncovered(
  const double min, const double max, std::vector<std::pair<double, double>> covered)
{
  std::ranges::sort(covered);
  auto result = std::vector<std::pair<double, double>>{};
  auto start = min;
  for (const auto& [lo, hi] : covered)
  {
    if (lo - start > Epsilon)
    {
      result.emplace_back(start, lo);
    }
    start = std::max(start, hi);
  }
  if (max - start > Epsilon)
  {
    result.emplace_back(start, max);
  }
  return result;
}

/**
 * Splits a wall into cuboids that leave the openings free. The wall is cut into columns
 * at the edges of the openings; each column is cut vertically around the openings that
 * cover it, and neighbouring columns with equal cuts are merged again. A wall with one
 * door thus becomes a left, a right and an upper piece.
 */
std::vector<vm::bbox3d> splitWall(
  const QString& side, const vm::bbox3d& wall, const std::vector<Opening>& openings)
{
  const auto along = alongAxis(side);

  auto cuts = std::set<double>{wall.min[along], wall.max[along]};
  for (const auto& opening : openings)
  {
    cuts.insert(opening.alongMin);
    cuts.insert(opening.alongMax);
  }
  const auto positions = std::vector<double>{cuts.begin(), cuts.end()};

  struct Column
  {
    double min;
    double max;
    std::vector<std::pair<double, double>> solid;
  };
  auto columns = std::vector<Column>{};
  for (size_t i = 0; i + 1 < positions.size(); ++i)
  {
    const auto min = positions[i];
    const auto max = positions[i + 1];
    if (max - min <= Epsilon)
    {
      continue;
    }

    auto covered = std::vector<std::pair<double, double>>{};
    for (const auto& opening : openings)
    {
      if (opening.alongMin <= min + Epsilon && opening.alongMax >= max - Epsilon)
      {
        covered.emplace_back(opening.zMin, opening.zMax);
      }
    }
    auto solid = uncovered(wall.min.z(), wall.max.z(), std::move(covered));
    if (!columns.empty() && columns.back().max == min && columns.back().solid == solid)
    {
      columns.back().max = max;
    }
    else
    {
      columns.push_back({min, max, std::move(solid)});
    }
  }

  auto result = std::vector<vm::bbox3d>{};
  for (const auto& column : columns)
  {
    for (const auto& [zMin, zMax] : column.solid)
    {
      auto min = wall.min;
      auto max = wall.max;
      min[along] = column.min;
      max[along] = column.max;
      min[2] = zMin;
      max[2] = zMax;
      result.emplace_back(min, max);
    }
  }
  return result;
}

McpResult<std::vector<Opening>> openingsArgument(
  const QJsonObject& args, const vm::bbox3d& inner)
{
  auto result = std::vector<Opening>{};
  const auto openings = args.value("openings").toArray();
  for (qsizetype i = 0; i < openings.size(); ++i)
  {
    const auto pointer = QString{"/openings/%1"}.arg(i);
    const auto object = openings[i].toObject();
    const auto side = object.value("side").toString();
    const auto along = alongAxis(side);
    const auto center = inner.center()[along] + object.value("offset").toDouble(0.0);
    const auto width = object.value("width").toDouble();
    const auto bottom = inner.min.z() + object.value("bottom").toDouble(0.0);
    const auto opening = Opening{
      .side = side,
      .alongMin = center - width / 2.0,
      .alongMax = center + width / 2.0,
      .zMin = bottom,
      .zMax = bottom + object.value("height").toDouble(),
    };

    if (
      opening.alongMin < inner.min[along] - Epsilon
      || opening.alongMax > inner.max[along] + Epsilon)
    {
      return invalidArgument(
        pointer,
        QString{"the opening does not fit into the %1 wall: it spans %2 to %3 along %4, "
                "but the room spans %5 to %6"}
          .arg(side)
          .arg(canon(opening.alongMin))
          .arg(canon(opening.alongMax))
          .arg(along == 0 ? "x" : "y")
          .arg(canon(inner.min[along]))
          .arg(canon(inner.max[along])),
        "offset is measured from the center of the wall; reduce width or offset");
    }
    if (opening.zMax > inner.max.z() + Epsilon)
    {
      return invalidArgument(
        pointer,
        QString{"the opening reaches z %1, above the ceiling at %2"}
          .arg(canon(opening.zMax))
          .arg(canon(inner.max.z())),
        "bottom is measured from the floor; reduce height or bottom");
    }

    for (size_t j = 0; j < result.size(); ++j)
    {
      const auto& other = result[j];
      if (
        other.side == side
        && std::min(other.alongMax, opening.alongMax)
               - std::max(other.alongMin, opening.alongMin)
             > Epsilon
        && std::min(other.zMax, opening.zMax) - std::max(other.zMin, opening.zMin)
             > Epsilon)
      {
        return invalidArgument(
          pointer,
          QString{"the opening overlaps opening %1"}.arg(j),
          "merge the openings into one or move them apart");
      }
    }
    result.push_back(opening);
  }
  return result;
}

McpResult<QJsonObject> createRoom(McpCallContext& context, const QJsonObject& args)
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

  const auto thickness = args.value("wall_thickness").toDouble(16.0);
  const auto delta = vm::vec3d::fill(thickness);
  auto outerBox = boxInWorld(map, inner.min - delta, inner.max + delta);
  if (outerBox.is_error())
  {
    auto error = errorOf(outerBox);
    error.message = "the outer box (inner_bounds plus wall_thickness): " + error.message;
    return error;
  }
  const auto outer = outerBox.value();

  auto wallMaterial = materialArgument(map, args, {"wall_material", "material"});
  auto floorMaterial = materialArgument(map, args, {"floor_material", "material"});
  auto ceilingMaterial = materialArgument(map, args, {"ceiling_material", "material"});
  for (const auto* material : {&wallMaterial, &floorMaterial, &ceilingMaterial})
  {
    if (material->is_error())
    {
      return errorOf(*material);
    }
  }

  auto openings = openingsArgument(args, inner);
  if (openings.is_error())
  {
    return errorOf(openings);
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

  auto cuboids = std::vector<Cuboid>{};
  auto pieceCounts = std::vector<size_t>{};
  for (const auto& side : RoomWallSides)
  {
    auto wallOpenings = std::vector<Opening>{};
    std::ranges::copy_if(
      openings.value(), std::back_inserter(wallOpenings), [&](const auto& opening) {
        return opening.side == side;
      });
    const auto pieces = splitWall(side, roomWallBounds(side, inner, outer), wallOpenings);
    for (const auto& piece : pieces)
    {
      cuboids.push_back({piece, wallMaterial.value()});
    }
    pieceCounts.push_back(pieces.size());
  }
  cuboids.push_back(
    {vm::bbox3d{outer.min, {outer.max.x(), outer.max.y(), inner.min.z()}},
     floorMaterial.value()});
  cuboids.push_back(
    {vm::bbox3d{{outer.min.x(), outer.min.y(), inner.max.z()}, outer.max},
     ceilingMaterial.value()});

  auto added = addCuboids(map, *parent.value(), cuboids);
  if (added.is_error())
  {
    return errorOf(added);
  }
  const auto& brushNodes = added.value();

  auto walls = QJsonObject{};
  auto index = size_t(0);
  for (size_t i = 0; i < RoomWallSides.size(); ++i)
  {
    auto ids = QJsonArray{};
    for (size_t j = 0; j < pieceCounts[i]; ++j)
    {
      ids.append(doc.nodes.idOf(*brushNodes[index++]));
    }
    walls[RoomWallSides[i]] = ids;
  }

  auto openingsJson = QJsonArray{};
  for (const auto& opening : openings.value())
  {
    openingsJson.append(QJsonObject{
      {"side", opening.side},
      {"bounds",
       toJson(openingBounds(opening, roomWallBounds(opening.side, inner, outer)))},
    });
  }

  auto result = QJsonObject{
    {"walls", walls},
    {"floor", doc.nodes.idOf(*brushNodes[index])},
    {"ceiling", doc.nodes.idOf(*brushNodes[index + 1])},
    {"openings", openingsJson},
    {"inner_bounds", toJson(inner)},
    {"outer_bounds", toJson(outer)},
  };

  if (groupName.value())
  {
    auto groupId = groupNewNodes(doc, brushNodes, *groupName.value());
    if (groupId.is_error())
    {
      return errorOf(groupId);
    }
    result["group"] = groupId.value();
  }

  warnAboutMaterials(
    context, {wallMaterial.value(), floorMaterial.value(), ceilingMaterial.value()});
  auto allBounds = outer;
  for (const auto& opening : openings.value())
  {
    allBounds = vm::merge(
      allBounds, openingBounds(opening, roomWallBounds(opening.side, inner, outer)));
  }
  warnAboutBounds(context, allBounds);

  return result;
}

McpToolDef createRoomTool()
{
  return {
    .name = "create_room",
    .title = "Create Room",
    .description =
      "Creates a room: a floor, a ceiling and four walls of non-overlapping cuboid "
      "brushes around inner_bounds. Each opening (door, window or passage) leaves a "
      "rectangular hole in a wall: the wall is split into pieces around it. Returns the "
      "brush ids per wall and the bounds of each opening through the wall, which "
      "query_point reports as empty. Verify with check_brush_overlaps and query_point.",
    .inputSchema = schema::object(
      {
        {"inner_bounds", schema::bounds("the empty space inside the room")},
        {"wall_thickness",
         schema::number(
           MinSize, 4096, "thickness of walls, floor and ceiling (default 16)")},
        {"material", schema::material("default material (default: current)")},
        {"floor_material", schema::material("material of the floor")},
        {"ceiling_material", schema::material("material of the ceiling")},
        {"wall_material", schema::material("material of the four walls")},
        {"openings",
         schema::array(
           schema::object(
             {
               {"side",
                schema::stringEnum(
                  {"+x", "-x", "+y", "-y"}, "the wall with the opening")},
               {"offset",
                schema::number(
                  -MaxSize,
                  MaxSize,
                  "center of the opening along the wall, relative to the center of the "
                  "wall (default 0)")},
               {"width", schema::number(MinSize, MaxSize, "width along the wall")},
               {"height", schema::number(MinSize, MaxSize, "height")},
               {"bottom",
                schema::number(
                  0, MaxSize, "height of the bottom above the floor (0, e.g. a door)")},
             },
             {"side", "width", "height"}),
           1,
           MaxOpenings,
           "doors, windows and passages; openings must fit into the wall and must not "
           "overlap")},
        {"group_name", schema::string(1, 128, "group the brushes under this name")},
        {"parent", schema::nodeId("a layer or group (default: the current one)")},
      },
      {"inner_bounds"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createRoom,
  };
}

// create_stairs

constexpr auto MaxSteps = 128;
constexpr auto MaxStepSize = 1024.0;

McpResult<QJsonObject> createStairs(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto startResult = vec3(args.value("start"), "/start");
  if (startResult.is_error())
  {
    return errorOf(startResult);
  }
  const auto start = startResult.value();

  const auto direction = args.value("direction").toString();
  const auto axis = sideAxis(direction);
  const auto sign = sideSign(direction);
  const auto lateral = axis == 0 ? size_t(1) : size_t(0);

  const auto steps = std::max(size_t(1), sizeArgument(args, "steps", 1));
  const auto stepHeight = args.value("step_height").toDouble(8.0);
  const auto stepDepth = args.value("step_depth").toDouble(16.0);
  const auto width = args.value("width").toDouble(64.0);
  const auto solid = args.value("solid").toBool(true);

  auto stepBounds = std::vector<vm::bbox3d>{};
  for (size_t i = 0; i < steps; ++i)
  {
    auto min = start;
    auto max = start;
    const auto front = start[axis] + sign * double(i) * stepDepth;
    const auto back = start[axis] + sign * double(i + 1) * stepDepth;
    min[axis] = std::min(front, back);
    max[axis] = std::max(front, back);
    min[lateral] = start[lateral] - width / 2.0;
    max[lateral] = start[lateral] + width / 2.0;
    min[2] = solid ? start.z() : start.z() + double(i) * stepHeight;
    max[2] = start.z() + double(i + 1) * stepHeight;
    stepBounds.emplace_back(min, max);
  }

  auto all = stepBounds.front();
  for (const auto& b : stepBounds)
  {
    all = vm::merge(all, b);
  }
  if (auto check = boxInWorld(map, all.min, all.max); check.is_error())
  {
    auto error = errorOf(check);
    error.message = "the stairs: " + error.message;
    return error;
  }

  auto material = materialArgument(map, args, {"material"});
  if (material.is_error())
  {
    return errorOf(material);
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

  auto cuboids = std::vector<Cuboid>{};
  for (const auto& b : stepBounds)
  {
    cuboids.push_back({b, material.value()});
  }
  auto added = addCuboids(map, *parent.value(), cuboids);
  if (added.is_error())
  {
    return errorOf(added);
  }

  auto top = start;
  top[axis] += sign * double(steps) * stepDepth;
  top[2] += double(steps) * stepHeight;

  auto result = QJsonObject{
    {"ids", idsOf(doc.nodes, added.value())},
    {"top", toJson(top)},
    {"bounds", toJson(all)},
  };

  if (groupName.value())
  {
    auto groupId = groupNewNodes(doc, added.value(), *groupName.value());
    if (groupId.is_error())
    {
      return errorOf(groupId);
    }
    result["group"] = groupId.value();
  }

  warnAboutMaterials(context, {material.value()});
  warnAboutBounds(context, all);
  if (stepHeight > 18.0)
  {
    context.warnings.push_back(
      "steps higher than 18 units cannot be climbed by a Quake player (see "
      "get_conventions)");
  }

  return result;
}

McpToolDef createStairsTool()
{
  return {
    .name = "create_stairs",
    .title = "Create Stairs",
    .description =
      "Creates a straight staircase of cuboid brushes, one per step, rising in "
      "direction. start is the bottom front center of the first step. Returns the step "
      "ids (bottom first), the top back center of the last step (where a floor should "
      "continue) and the bounds.",
    .inputSchema = schema::object(
      {
        {"start", schema::vec3("bottom front center of the first step")},
        {"direction",
         schema::stringEnum({"+x", "-x", "+y", "-y"}, "the direction to climb")},
        {"steps", schema::integer(1, MaxSteps, "number of steps")},
        {"step_height",
         schema::number(MinSize, MaxStepSize, "rise per step (default 8)")},
        {"step_depth", schema::number(MinSize, MaxStepSize, "run per step (default 16)")},
        {"width", schema::number(MinSize, MaxSize, "width of the stairs (default 64)")},
        {"material", schema::material("material (default: current)")},
        {"solid",
         schema::boolean(
           "true (default): each step reaches down to start.z; false: floating slabs "
           "of step_height")},
        {"group_name", schema::string(1, 128, "group the steps under this name")},
        {"parent", schema::nodeId("a layer or group (default: the current one)")},
      },
      {"start", "direction", "steps"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createStairs,
  };
}

// drop_to_floor and align_nodes

/**
 * Nodes that are moved together. A brush entity is moved with all of its brushes; any
 * other node is moved alone, groups with their contents.
 */
struct MoveUnit
{
  QString id;
  std::vector<mdl::Node*> nodes;
  vm::bbox3d bounds;
};

McpResult<std::vector<MoveUnit>> resolveMoveUnits(
  const McpDocumentState& doc, const QJsonArray& ids)
{
  // checks that every target can be edited
  auto targets = resolveTargets(doc, ids);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto nodes = resolveNodes(doc, ids);
  if (nodes.is_error())
  {
    return errorOf(nodes);
  }

  const auto listed =
    std::unordered_set<const mdl::Node*>{nodes.value().begin(), nodes.value().end()};
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

  auto result = std::vector<MoveUnit>{};
  for (auto* node : nodes.value())
  {
    if (hasListedAncestor(*node))
    {
      continue;
    }
    auto unit = MoveUnit{.id = doc.nodes.idOf(*node), .nodes = {}, .bounds = {}};
    if (dynamic_cast<mdl::EntityNode*>(node) && node->hasChildren())
    {
      unit.nodes = node->children() | kdl::ranges::to<std::vector>();
    }
    else
    {
      unit.nodes = {node};
    }
    unit.bounds = unit.nodes.front()->logicalBounds();
    for (const auto* unitNode : unit.nodes)
    {
      unit.bounds = vm::merge(unit.bounds, unitNode->logicalBounds());
    }
    result.push_back(std::move(unit));
  }

  auto checked = std::vector<const mdl::Node*>{};
  for (const auto& unit : result)
  {
    checked.insert(checked.end(), unit.nodes.begin(), unit.nodes.end());
  }
  if (auto check = checkTransformableNodes(doc.nodes, checked); check.is_error())
  {
    return errorOf(check);
  }
  return result;
}

QJsonObject movedJson(const QString& id, const vm::vec3d& delta)
{
  return {{"id", id}, {"delta", toJson(delta)}};
}

/** Moves the units by their deltas; units with a zero delta are not moved. */
McpResult<void> moveUnits(
  mdl::Map& map, const std::vector<std::pair<const MoveUnit*, vm::vec3d>>& moves)
{
  for (const auto& [unit, delta] : moves)
  {
    if (!map.worldBounds().contains(unit->bounds.translate(delta)))
    {
      return toolError(
        "out_of_world_bounds",
        QString{"%1 would be moved outside the world bounds"}.arg(unit->id),
        "check the target",
        {{"id", unit->id}, {"world_bounds", toJson(map.worldBounds())}});
    }
  }
  for (const auto& [unit, delta] : moves)
  {
    selectOnly(map, unit->nodes);
    if (!mdl::translateSelection(map, delta))
    {
      return toolError(
        "operation_failed",
        QString{"%1 could not be moved"}.arg(unit->id),
        "check the log");
    }
  }
  return kdl::void_success;
}

/** Whether brushes of this entity are not solid, e.g. triggers. */
bool isNonSolidEntity(const mdl::EntityNodeBase* entity)
{
  return entity
         && QString::fromStdString(entity->entity().classname())
              .startsWith("trigger", Qt::CaseInsensitive);
}

void collectSubtree(const mdl::Node& node, std::unordered_set<const mdl::Node*>& nodes)
{
  nodes.insert(&node);
  for (const auto* child : node.children())
  {
    collectSubtree(*child, nodes);
  }
}

constexpr auto DefaultMaxDistance = 8192.0;
constexpr auto MaxDistance = 131072.0;
constexpr auto MaxGap = 4096.0;

struct Hit
{
  double distance;
  const mdl::BrushNode* brushNode;
};

/**
 * Sweeps the bounds in the direction and returns the nearest brush hit. The swept volume
 * is a box with the footprint of the bounds, reaching from their leading face up to
 * maxDistance. Each brush is intersected with that box exactly, so sloped brushes are hit
 * where their surface is highest within the footprint.
 */
std::optional<Hit> sweep(
  const mdl::Map& map,
  const vm::bbox3d& bounds,
  const size_t axis,
  const double sign,
  const double maxDistance,
  const std::vector<const mdl::BrushNode*>& obstacles)
{
  const auto& worldBounds = map.worldBounds();
  const auto lead = sign < 0 ? bounds.min[axis] : bounds.max[axis];

  // the swept box starts slightly inside the bounds so that a surface touching them is
  // hit; it stays a unit inside the world bounds, because brush geometry is clipped by
  // the world bounds and a box on them cannot be intersected
  constexpr auto Overlap = 1e-3;
  constexpr auto WorldMargin = 1.0;
  auto sweptMin = bounds.min;
  auto sweptMax = bounds.max;
  if (sign < 0)
  {
    sweptMin[axis] = lead - maxDistance;
    sweptMax[axis] = lead + Overlap;
  }
  else
  {
    sweptMin[axis] = lead - Overlap;
    sweptMax[axis] = lead + maxDistance;
  }
  for (size_t i = 0; i < 3; ++i)
  {
    sweptMin[i] = std::max(sweptMin[i], worldBounds.min[i] + WorldMargin);
    sweptMax[i] = std::min(sweptMax[i], worldBounds.max[i] - WorldMargin);
    if (sweptMax[i] - sweptMin[i] <= Epsilon)
    {
      return std::nullopt;
    }
  }
  const auto swept = vm::bbox3d{sweptMin, sweptMax};

  auto sweptBrush = brushBuilder(map).createCuboid(swept, mdl::BrushFace::NoMaterialName);
  if (sweptBrush.is_error())
  {
    return std::nullopt;
  }

  auto result = std::optional<Hit>{};
  for (const auto* obstacle : obstacles)
  {
    const auto& obstacleBounds = obstacle->logicalBounds();
    auto overlapsSwept = true;
    for (size_t i = 0; i < 3; ++i)
    {
      if (
        std::min(obstacleBounds.max[i], swept.max[i])
          - std::max(obstacleBounds.min[i], swept.min[i])
        <= Epsilon)
      {
        overlapsSwept = false;
      }
    }
    if (!overlapsSwept)
    {
      continue;
    }

    auto intersection = sweptBrush.value();
    if (intersection.intersect(worldBounds, obstacle->brush()).is_error())
    {
      continue;
    }
    const auto& hitBounds = intersection.bounds();
    const auto hit = sign < 0 ? std::min(hitBounds.max[axis], lead)
                              : std::max(hitBounds.min[axis], lead);
    const auto distance = canon(std::abs(hit - lead));
    if (!result || distance < result->distance)
    {
      result = Hit{distance, obstacle};
    }
  }
  return result;
}

McpResult<QJsonObject> dropToFloor(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  if (auto check = requireNoVertexHandlesSelected(map); check.is_error())
  {
    return errorOf(check);
  }

  auto units = resolveMoveUnits(doc, args.value("ids").toArray());
  if (units.is_error())
  {
    return errorOf(units);
  }

  const auto direction = args.value("direction").toString("-z");
  const auto axis = sideAxis(direction);
  const auto sign = sideSign(direction);
  const auto maxDistance = args.value("max_distance").toDouble(DefaultMaxDistance);
  const auto gap = args.value("gap").toDouble(0.0);

  // all visible solid brushes are obstacles, at their positions before the call
  auto brushNodes = std::vector<const mdl::BrushNode*>{};
  const auto collect = [&](const auto& self, const mdl::Node& node) -> void {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      if (
        map.editorContext().visible(*brushNode) && !isNonSolidEntity(brushNode->entity()))
      {
        brushNodes.push_back(brushNode);
      }
      return;
    }
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  collect(collect, map.worldNode());

  auto moves = std::vector<std::pair<const MoveUnit*, vm::vec3d>>{};
  auto moved = QJsonArray{};
  auto notMoved = QJsonArray{};
  for (const auto& unit : units.value())
  {
    auto own = std::unordered_set<const mdl::Node*>{};
    for (const auto* node : unit.nodes)
    {
      collectSubtree(*node, own);
    }
    auto obstacles = std::vector<const mdl::BrushNode*>{};
    std::ranges::copy_if(brushNodes, std::back_inserter(obstacles), [&](const auto* b) {
      return !own.contains(b);
    });

    const auto hit = sweep(map, unit.bounds, axis, sign, maxDistance, obstacles);
    if (!hit)
    {
      notMoved.append(QJsonObject{
        {"id", unit.id},
        {"reason", "no_surface"},
        {"message",
         QString{"no brush within %1 units in direction %2"}
           .arg(maxDistance)
           .arg(direction)},
      });
      continue;
    }

    // a positive distance moves in the direction, the gap moves back
    auto delta = vm::vec3d{};
    delta[axis] = canon(sign * (hit->distance - gap));
    if (std::abs(delta[axis]) < Epsilon)
    {
      notMoved.append(QJsonObject{
        {"id", unit.id},
        {"reason", "already_in_place"},
        {"on", doc.nodes.idOf(*hit->brushNode)},
      });
      continue;
    }
    moves.emplace_back(&unit, delta);
    auto entry = movedJson(unit.id, delta);
    entry["on"] = doc.nodes.idOf(*hit->brushNode);
    entry["distance"] = hit->distance;
    moved.append(entry);
  }

  if (auto result = moveUnits(map, moves); result.is_error())
  {
    return errorOf(result);
  }

  return QJsonObject{
    {"moved", moved},
    {"not_moved", notMoved},
  };
}

McpToolDef dropToFloorTool()
{
  return {
    .name = "drop_to_floor",
    .title = "Drop To Floor",
    .description =
      "Moves each node (entity, brush, group or brush entity as a whole) in direction "
      "(default -z) until its bounds rest on the nearest brush below them, gap units "
      "above it. Sloped brushes are hit exactly. Other nodes count at their positions "
      "before the call; hidden brushes and trigger brushes are ignored. Nodes with "
      "nothing within max_distance are listed in not_moved.",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds()},
        {"direction", schema::side("the direction to move in (-z)")},
        {"max_distance",
         schema::number(
           MinSize, MaxDistance, "how far to look for a surface (default 8192)")},
        {"gap",
         schema::number(0, MaxGap, "space to leave above the surface (default 0)")},
      },
      {"ids"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = dropToFloor,
  };
}

McpResult<QJsonObject> alignNodes(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  if (auto check = requireNoVertexHandlesSelected(map); check.is_error())
  {
    return errorOf(check);
  }

  auto reference = std::optional<vm::bbox3d>{};
  const auto to = args.value("to");
  if (to.isString())
  {
    auto node = resolveNode(doc, to.toString());
    if (node.is_error())
    {
      auto error = errorOf(node);
      error.details["pointer"] = "/to";
      return error;
    }
    reference = nodeBounds(*node.value());
    if (!reference)
    {
      return invalidArgument("/to", QString{"%1 is empty"}.arg(to.toString()));
    }
  }
  else
  {
    auto b = bounds(to, "/to");
    if (b.is_error())
    {
      return errorOf(b);
    }
    reference = b.value();
  }

  auto units = resolveMoveUnits(doc, args.value("ids").toArray());
  if (units.is_error())
  {
    return errorOf(units);
  }

  const auto side = args.value("side").toString();
  const auto axis = sideAxis(side);
  const auto positive = sideSign(side) > 0;
  const auto mode = args.value("mode").toString("flush_inside");
  const auto axesOnly = args.value("axes_only").toBool(true);

  auto moves = std::vector<std::pair<const MoveUnit*, vm::vec3d>>{};
  auto moved = QJsonArray{};
  auto unchanged = QJsonArray{};
  for (const auto& unit : units.value())
  {
    const auto& b = unit.bounds;
    const auto& r = *reference;
    auto delta = vm::vec3d{};
    for (size_t i = 0; i < 3; ++i)
    {
      if (i == axis)
      {
        delta[i] = mode == u"center" ? r.center()[i] - b.center()[i]
                   : mode == u"flush_outside"
                     ? (positive ? r.max[i] - b.min[i] : r.min[i] - b.max[i])
                   : positive ? r.max[i] - b.max[i]
                              : r.min[i] - b.min[i];
      }
      else if (!axesOnly)
      {
        delta[i] = r.center()[i] - b.center()[i];
      }
      delta[i] = canon(delta[i]);
    }

    if (vm::is_zero(delta, Epsilon))
    {
      unchanged.append(unit.id);
      continue;
    }
    moves.emplace_back(&unit, delta);
    moved.append(movedJson(unit.id, delta));
  }

  if (auto result = moveUnits(map, moves); result.is_error())
  {
    return errorOf(result);
  }

  return QJsonObject{
    {"moved", moved},
    {"unchanged", unchanged},
  };
}

McpToolDef alignNodesTool()
{
  return {
    .name = "align_nodes",
    .title = "Align Nodes",
    .description =
      "Moves each node (by its bounds) so that it lines up with the side of to (a node "
      "or bounds). flush_inside: the given sides coincide; flush_outside: the node is "
      "placed against that side from outside, e.g. side +z stacks it on top; center: "
      "the centers line up along that axis. With axes_only false the node is also "
      "centered on the other two axes. Brush entities move as a whole.",
    .inputSchema = schema::object(
      {
        {"ids", schema::nodeIds()},
        {"to",
         QJsonObject{
           {"oneOf",
            QJsonArray{
              schema::nodeId("a node to align to"),
              schema::bounds("bounds to align to"),
            }},
           {"description", "a node id or bounds"},
         }},
        {"side", schema::side("the side of to to align with")},
        {"mode",
         schema::stringEnum(
           {"flush_inside", "flush_outside", "center"}, "how to align (flush_inside)")},
        {"axes_only",
         schema::boolean(
           "true (default): only move along the axis of side; false: also center on the "
           "other axes")},
      },
      {"ids", "to", "side"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = alignNodes,
  };
}

// get_conventions

McpResult<QJsonObject> getConventions(McpCallContext& context, const QJsonObject&)
{
  const auto& map = context.doc.map;
  const auto softBounds = mdl::softMapBounds(map);
  return QJsonObject{
    {"units", "1 unit is about 1 inch (Quake scale)"},
    {"up_axis", "+z"},
    {"handedness", "right"},
    {"angles", "entity 'angle' is the yaw in degrees around +z; 0 = +x, 90 = +y"},
    {"grid_size", canon(map.grid().actualSize())},
    {"world_bounds", toJson(map.worldBounds())},
    {"soft_map_bounds",
     QJsonObject{
       {"source", softBounds.source == mdl::SoftMapBoundsType::Game ? "game" : "map"},
       {"bounds",
        softBounds.bounds ? QJsonValue{toJson(*softBounds.bounds)} : QJsonValue{}},
     }},
    {"player",
     QJsonObject{
       {"width", 32},
       {"height", 56},
       {"eye_height", 22},
       {"step_height", 18},
       {"jump_height", 45},
       {"source", "default"},
     }},
    {"common_dimensions",
     QJsonObject{
       {"door", QJsonObject{{"width", 64}, {"height", 96}}},
       {"corridor_min_width", 64},
       {"room_height_typical", QJsonArray{128, 256}},
       {"wall_thickness_typical", QJsonArray{8, 16}},
       {"stair_step", QJsonObject{{"height", 8}, {"depth", 16}}},
     }},
    {"material_default", toUnicode(map, map.currentMaterialName())},
    {"map_format", QString::fromStdString(mdl::formatName(map.worldNode().mapFormat()))},
    {"note",
     "guidance for building playable spaces; player sizes are Quake defaults, not read "
     "from the game configuration"},
  };
}

McpToolDef getConventionsTool()
{
  return {
    .name = "get_conventions",
    .title = "Get Conventions",
    .description =
      "Returns the coordinate conventions, the grid and world bounds, and typical sizes "
      "(player, doors, corridors, stairs) to build playable spaces. Needs an open map: "
      "call it after new_map or open_map.",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = getConventions,
  };
}

// describe_region

constexpr auto DefaultMaxItems = 100;
constexpr auto MaxItems = 500;

QString number(const double value)
{
  return QString::number(canon(value), 'g', 12);
}

QString pointText(const vm::vec3d& point)
{
  return QString{"(%1 %2 %3)"}.arg(
    number(point.x()), number(point.y()), number(point.z()));
}

QString boundsText(const vm::bbox3d& bounds)
{
  return pointText(bounds.min) + " to " + pointText(bounds.max);
}

uint64_t idNumber(const QString& id)
{
  return id.mid(1).toULongLong();
}

McpResult<QJsonObject> describeRegion(McpCallContext& context, const QJsonObject& args)
{
  const auto& doc = context.doc;
  const auto& map = doc.map;

  auto regionResult = bounds(args.value("bounds"), "/bounds");
  if (regionResult.is_error())
  {
    return errorOf(regionResult);
  }
  const auto region = regionResult.value();
  const auto maxItems = sizeArgument(args, "max_items", DefaultMaxItems);

  struct Item
  {
    uint64_t number;
    QJsonObject json;
  };
  auto entities = std::vector<Item>{};
  auto brushes = std::vector<Item>{};
  auto groups = std::vector<Item>{};
  auto patches = size_t(0);
  auto pointEntities = size_t(0);
  auto classnames = std::map<QString, qint64>{};
  auto materials = std::set<QString>{};
  auto contents = std::optional<vm::bbox3d>{};

  const auto merge = [&](const vm::bbox3d& b) {
    contents = contents ? vm::merge(*contents, b) : b;
  };

  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    const auto inRegion = [&]() { return node.logicalBounds().intersects(region); };
    node.accept(kdl::overload(
      [&](const mdl::WorldNode&) {},
      [&](const mdl::LayerNode&) {},
      [&](const mdl::GroupNode& groupNode) {
        if (inRegion())
        {
          const auto id = doc.nodes.idOf(groupNode);
          groups.push_back(
            {idNumber(id),
             {{"id", id},
              {"name", toUnicode(map, groupNode.name())},
              {"bounds", toJson(groupNode.logicalBounds())}}});
        }
      },
      [&](const mdl::EntityNode& entityNode) {
        if (inRegion())
        {
          const auto id = doc.nodes.idOf(entityNode);
          const auto& entity = entityNode.entity();
          const auto classname = toUnicode(map, entity.classname());
          auto json = QJsonObject{
            {"id", id},
            {"classname", classname},
            {"point_entity", entity.pointEntity()},
          };
          if (entity.pointEntity())
          {
            json["origin"] = toJson(entity.origin());
            ++pointEntities;
          }
          else
          {
            json["bounds"] = toJson(entityNode.logicalBounds());
            json["brush_count"] = qint64(entityNode.childCount());
          }
          ++classnames[classname];
          merge(entityNode.logicalBounds());
          entities.push_back({idNumber(id), json});
        }
      },
      [&](const mdl::BrushNode& brushNode) {
        if (inRegion())
        {
          const auto id = doc.nodes.idOf(brushNode);
          auto brushMaterials = std::set<QString>{};
          for (const auto& face : brushNode.brush().faces())
          {
            brushMaterials.insert(toUnicode(map, face.materialName()));
          }
          materials.insert(brushMaterials.begin(), brushMaterials.end());
          const auto* owner = brushNode.entity();
          merge(brushNode.logicalBounds());
          brushes.push_back(
            {idNumber(id),
             {{"id", id},
              {"entity",
               owner && owner != &map.worldNode() ? doc.nodes.idOf(*owner) : "world"},
              {"bounds", toJson(brushNode.logicalBounds())},
              {"materials",
               QJsonArray::fromStringList(
                 {brushMaterials.begin(), brushMaterials.end()})}}});
        }
      },
      [&](const mdl::PatchNode& patchNode) {
        if (inRegion())
        {
          merge(patchNode.logicalBounds());
          ++patches;
        }
      }));
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, map.worldNode());

  auto truncated = false;
  const auto list = [&](std::vector<Item>& items) {
    std::ranges::sort(items, {}, &Item::number);
    auto result = QJsonArray{};
    for (const auto& item : items)
    {
      if (size_t(result.size()) == maxItems)
      {
        truncated = true;
        break;
      }
      result.append(item.json);
    }
    return result;
  };

  // whether the center of the region is solid, counting world brushes only
  const auto center = region.center();
  auto centerSolid = false;
  const auto visitBrushes = [&](const auto& self, const mdl::Node& node) -> void {
    for (const auto* child : node.children())
    {
      if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(child))
      {
        const auto* owner = brushNode->entity();
        if (
          (!owner || owner == &map.worldNode())
          && brushNode->logicalBounds().contains(center)
          && brushNode->brush().containsPoint(center))
        {
          centerSolid = true;
        }
      }
      else if (
        dynamic_cast<const mdl::LayerNode*>(child)
        || dynamic_cast<const mdl::GroupNode*>(child))
      {
        self(self, *child);
      }
    }
  };
  visitBrushes(visitBrushes, map.worldNode());

  auto classnameTexts = QStringList{};
  for (const auto& [classname, count] : classnames)
  {
    classnameTexts.append(QString{"%1 x%2"}.arg(classname).arg(count));
  }

  auto summary = QString{"Region %1 (size %2 x %3 x %4): "}
                   .arg(boundsText(region))
                   .arg(number(region.size().x()))
                   .arg(number(region.size().y()))
                   .arg(number(region.size().z()));
  summary += QString{"%1 brushes"}.arg(brushes.size());
  if (!materials.empty())
  {
    summary += QString{" (materials: %1)"}.arg(
      QStringList{materials.begin(), materials.end()}.join(", "));
  }
  summary += QString{", %1 entities"}.arg(entities.size());
  if (!classnameTexts.isEmpty())
  {
    summary += QString{" (%1)"}.arg(classnameTexts.join(", "));
  }
  summary += QString{", %1 groups, %2 patches."}.arg(groups.size()).arg(patches);
  if (contents)
  {
    summary += QString{" Contents span %1."}.arg(boundsText(*contents));
  }
  summary += QString{" The center %1 is %2."}.arg(
    pointText(center), centerSolid ? "inside a world brush" : "empty");

  return QJsonObject{
    {"summary", summary},
    {"counts",
     QJsonObject{
       {"brushes", qint64(brushes.size())},
       {"entities", qint64(entities.size())},
       {"point_entities", qint64(pointEntities)},
       {"brush_entities", qint64(entities.size() - pointEntities)},
       {"groups", qint64(groups.size())},
       {"patches", qint64(patches)},
     }},
    {"entities", list(entities)},
    {"brushes", list(brushes)},
    {"groups", list(groups)},
    {"content_bounds", contents ? QJsonValue{toJson(*contents)} : QJsonValue{}},
    {"center_solid", centerSolid},
    {"truncated", truncated},
  };
}

McpToolDef describeRegionTool()
{
  return {
    .name = "describe_region",
    .title = "Describe Region",
    .description =
      "Summarizes what is in the given bounds (touching counts): a short text, counts "
      "by type, the entities (id, classname, origin), brushes (id, bounds, materials) "
      "and groups sorted by id, and whether the center of the region is solid.",
    .inputSchema = schema::object(
      {
        {"bounds", schema::bounds("the region to describe")},
        {"max_items",
         schema::integer(
           1, MaxItems, "maximum number of entries per list (default 100)")},
      },
      {"bounds"}),
    .kind = McpToolKind::Read,
    .annotations = ReadOnly,
    .allowedInBatch = true,
    .run = describeRegion,
  };
}

} // namespace

void registerMcpMapHelperTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(getConventionsTool());
  dispatcher.add(describeRegionTool());
  dispatcher.add(createRoomTool());
  dispatcher.add(createStairsTool());
  dispatcher.add(dropToFloorTool());
  dispatcher.add(alignNodesTool());
}

} // namespace tb::ui
