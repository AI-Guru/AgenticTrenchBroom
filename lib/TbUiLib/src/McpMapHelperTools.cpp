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
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
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
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>
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

// create_opening

/** The horizontal axis along a wall whose thickness is along the given axis. */
size_t otherHorizontalAxis(const size_t axis)
{
  return axis == 0 ? 1 : 0;
}

QString axisName(const size_t axis)
{
  return axis == 0 ? "x" : axis == 1 ? "y" : "z";
}

QString sideName(const size_t axis, const double sign)
{
  return (sign > 0 ? "+" : "-") + axisName(axis);
}

double volume(const vm::bbox3d& box)
{
  const auto size = box.size();
  return size.x() * size.y() * size.z();
}

/** The intersection of the boxes, if it has a volume. */
std::optional<vm::bbox3d> intersection(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  auto min = vm::vec3d{};
  auto max = vm::vec3d{};
  for (size_t i = 0; i < 3; ++i)
  {
    min[i] = std::max(lhs.min[i], rhs.min[i]);
    max[i] = std::min(lhs.max[i], rhs.max[i]);
    if (max[i] - min[i] <= Epsilon)
    {
      return std::nullopt;
    }
  }
  return vm::bbox3d{min, max};
}

/** Whether the brush is an axis-aligned cuboid, i.e. equal to its bounds. */
bool isCuboid(const mdl::Brush& brush)
{
  if (brush.faceCount() != 6)
  {
    return false;
  }
  return std::ranges::all_of(brush.faces(), [](const auto& face) {
    const auto& normal = face.boundary().normal;
    return std::ranges::any_of(std::array{0, 1, 2}, [&](const auto i) {
      return std::abs(std::abs(normal[size_t(i)]) - 1.0) < 1e-9;
    });
  });
}

/**
 * Creates a cuboid piece of the given brush. Each face of the piece copies the material,
 * UV and surface attributes of the brush's face with the same normal, so the pieces look
 * like the brush. Faces that do not lie on a face of the brush, i.e. the new faces around
 * an opening, get cutMaterial if one is given.
 */
McpResult<mdl::Brush> brushPiece(
  const mdl::Map& map,
  const mdl::Brush& brush,
  const vm::bbox3d& bounds,
  const std::optional<std::string>& cutMaterial)
{
  if (auto check = boxInWorld(map, bounds.min, bounds.max); check.is_error())
  {
    auto error = errorOf(check);
    error.message = "a piece of the wall: " + error.message;
    error.hint =
      "keep the opening at least 1/1024 away from the edges of the wall "
      "brushes, or align it with them";
    return error;
  }
  auto piece = brushBuilder(map).createCuboid(bounds, mdl::BrushFace::NoMaterialName);
  if (piece.is_error())
  {
    return toolError(
      "geometry_invalid",
      QString::fromStdString(std::get<Error>(piece.error()).msg),
      "check the sizes");
  }
  auto result = std::move(piece).value();
  for (auto& face : result.faces())
  {
    const auto index = brush.findFace(face.boundary().normal);
    if (!index)
    {
      continue;
    }
    const auto& original = brush.face(*index);
    if (const auto snapshot = original.takeUvCoordSystemSnapshot())
    {
      face.restoreUvCoordSystemSnapshot(*snapshot);
    }
    face.copyAttributes(original);
    if (
      cutMaterial
      && !vm::is_equal(face.boundary(), original.boundary(), vm::Cd::almost_zero()))
    {
      face.setMaterialName(*cutMaterial);
    }
  }
  return result;
}

/** Whether the brush can be changed, even inside a closed group. */
McpResult<void> requireEditableBrush(
  const McpDocumentState& doc, const mdl::BrushNode& brushNode)
{
  const auto& editorContext = doc.map.editorContext();
  if (!editorContext.visible(brushNode) || !editorContext.editable(brushNode))
  {
    const auto id = doc.nodes.idOf(brushNode);
    return toolError(
      "node_not_selectable",
      QString{"%1 is hidden or locked"}.arg(id),
      "show and unlock it first",
      {{"id", id}, {"reason", editorContext.visible(brushNode) ? "locked" : "hidden"}});
  }
  return requireNotLinked(doc, brushNode);
}

/** The wall to cut an opening into: a brush or the walls on one side of a room. */
struct Wall
{
  /** The axis of the wall's thickness. */
  size_t axis;
  /** The side of its room the wall is on, if known. */
  std::optional<double> sign;
  /** The candidates to cut, in DFS order. */
  std::vector<mdl::BrushNode*> brushNodes;
  /** The extent of the wall through its thickness. */
  double slabMin;
  double slabMax;
  /** Where an opening may be along the wall and vertically. */
  double alongMin;
  double alongMax;
  double zMin;
  double zMax;
};

McpResult<Wall> brushWall(
  const McpDocumentState& doc,
  mdl::BrushNode& brushNode,
  const std::optional<QString>& side)
{
  if (auto check = requireEditableBrush(doc, brushNode); check.is_error())
  {
    return errorOf(check);
  }
  const auto id = doc.nodes.idOf(brushNode);
  if (!isCuboid(brushNode.brush()))
  {
    return toolError(
      "not_supported",
      QString{"%1 is not an axis-aligned cuboid"}.arg(id),
      "cut it with csg_subtract and a box instead",
      {{"id", id}});
  }

  const auto& b = brushNode.logicalBounds();
  auto axis = size_t(0);
  if (side)
  {
    axis = sideAxis(*side);
  }
  else if (std::abs(b.size().x() - b.size().y()) <= Epsilon)
  {
    return invalidArgument(
      "/side",
      QString{"%1 is as thick in x as in y, so the direction through the wall is unclear"}
        .arg(id),
      "pass side");
  }
  else
  {
    axis = b.size().x() < b.size().y() ? 0 : 1;
  }
  const auto along = otherHorizontalAxis(axis);
  return Wall{
    .axis = axis,
    .sign = side ? std::optional{sideSign(*side)} : std::nullopt,
    .brushNodes = {&brushNode},
    .slabMin = b.min[axis],
    .slabMax = b.max[axis],
    .alongMin = b.min[along],
    .alongMax = b.max[along],
    .zMin = b.min.z(),
    .zMax = b.max.z(),
  };
}

/**
 * The walls on one side of a room group laid out like create_room does it: the brushes
 * in the group that reach the outer face of the group on that side but not across the
 * group; the walls of one axis span the corners (the x walls, unless the room was
 * rotated). Openings must lie between the walls of the two
 * neighbouring sides and within the height of the walls, as in create_room.
 */
McpResult<Wall> roomWall(
  const McpDocumentState& doc, mdl::GroupNode& groupNode, const QString& side)
{
  const auto& map = doc.map;
  const auto id = doc.nodes.idOf(groupNode);
  const auto& editorContext = map.editorContext();
  if (!editorContext.visible(groupNode) || !editorContext.editable(groupNode))
  {
    return toolError(
      "node_not_selectable",
      QString{"%1 is hidden or locked"}.arg(id),
      "show and unlock it first",
      {{"id", id}, {"reason", editorContext.visible(groupNode) ? "locked" : "hidden"}});
  }
  if (auto check = requireNotLinked(doc, groupNode); check.is_error())
  {
    return errorOf(check);
  }

  auto brushNodes = std::vector<mdl::BrushNode*>{};
  const auto collect = [&](const auto& self, mdl::Node& node) -> void {
    for (auto* child : node.children())
    {
      if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(child))
      {
        brushNodes.push_back(brushNode);
      }
      else if (dynamic_cast<mdl::GroupNode*>(child))
      {
        self(self, *child);
      }
    }
  };
  collect(collect, groupNode);
  if (brushNodes.empty())
  {
    return toolError(
      "invalid_argument",
      QString{"%1 contains no brushes"}.arg(id),
      "pass a room group made by create_room, or a wall brush",
      {{"pointer", "/wall"}});
  }

  auto outer = brushNodes.front()->logicalBounds();
  for (const auto* brushNode : brushNodes)
  {
    outer = vm::merge(outer, brushNode->logicalBounds());
  }

  const auto touches = [&](const vm::bbox3d& b, const size_t axis, const double sign) {
    return sign > 0 ? std::abs(b.max[axis] - outer.max[axis]) <= Epsilon
                    : std::abs(b.min[axis] - outer.min[axis]) <= Epsilon;
  };
  const auto touchesAny = [&](const vm::bbox3d& b, const size_t axis) {
    return touches(b, axis, -1.0) || touches(b, axis, 1.0);
  };

  // the walls of one horizontal axis own the corners: in create_room the x walls, but
  // after a rotation by 90 degrees the y walls; a corner brush is thinner across its wall
  // than along it
  auto votes = std::array<size_t, 2>{};
  for (const auto* brushNode : brushNodes)
  {
    const auto& b = brushNode->logicalBounds();
    const auto full = [&](const size_t axis) {
      return b.size()[axis] >= outer.size()[axis] - Epsilon;
    };
    if (touchesAny(b, 0) && touchesAny(b, 1) && !(full(0) && full(1)))
    {
      if (b.size().x() < b.size().y() - Epsilon)
      {
        ++votes[0];
      }
      else if (b.size().y() < b.size().x() - Epsilon)
      {
        ++votes[1];
      }
    }
  }
  const auto cornerAxis = votes[1] > votes[0] ? size_t(1) : size_t(0);

  // the brushes that form the wall on the given side of the group; the other walls must
  // not reach the outer faces of the walls that own the corners
  const auto wallsOn = [&](const size_t axis, const double sign) {
    auto result = std::vector<mdl::BrushNode*>{};
    for (auto* brushNode : brushNodes)
    {
      const auto& b = brushNode->logicalBounds();
      if (
        touches(b, axis, sign) && b.size()[axis] < outer.size()[axis] - Epsilon
        && (axis == cornerAxis || !touchesAny(b, cornerAxis)))
      {
        result.push_back(brushNode);
      }
    }
    return result;
  };

  const auto axis = sideAxis(side);
  const auto sign = sideSign(side);
  const auto along = otherHorizontalAxis(axis);
  auto walls = wallsOn(axis, sign);
  if (walls.empty())
  {
    return toolError(
      "invalid_argument",
      QString{"%1 has no wall on its %2 side"}.arg(id, side),
      "pass a room group made by create_room, or a wall brush",
      {{"pointer", "/side"}});
  }

  auto wall = Wall{
    .axis = axis,
    .sign = sign,
    .brushNodes = walls,
    .slabMin = outer.max[axis],
    .slabMax = outer.min[axis],
    .alongMin = outer.min[along],
    .alongMax = outer.max[along],
    .zMin = outer.max.z(),
    .zMax = outer.min.z(),
  };
  for (const auto* brushNode : walls)
  {
    const auto& b = brushNode->logicalBounds();
    wall.slabMin = std::min(wall.slabMin, b.min[axis]);
    wall.slabMax = std::max(wall.slabMax, b.max[axis]);
    wall.zMin = std::min(wall.zMin, b.min.z());
    wall.zMax = std::max(wall.zMax, b.max.z());
  }
  // openings stay between the neighbouring walls
  for (const auto* brushNode : wallsOn(along, -1.0))
  {
    wall.alongMin = std::max(wall.alongMin, brushNode->logicalBounds().max[along]);
  }
  for (const auto* brushNode : wallsOn(along, 1.0))
  {
    wall.alongMax = std::min(wall.alongMax, brushNode->logicalBounds().min[along]);
  }
  if (wall.alongMax - wall.alongMin <= Epsilon)
  {
    return toolError(
      "invalid_argument",
      QString{"the %1 wall of %2 has no room for an opening"}.arg(side, id),
      "pass a wall brush instead",
      {{"pointer", "/wall"}});
  }
  return wall;
}

McpResult<QJsonObject> createOpening(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto wallId = args.value("wall").toString();
  const auto side = optionalString(args, "side");
  auto node = resolveNode(doc, wallId);
  if (node.is_error())
  {
    auto error = errorOf(node);
    error.details["pointer"] = "/wall";
    return error;
  }

  const auto wallResult = [&]() -> McpResult<Wall> {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node.value()))
    {
      return brushWall(doc, *brushNode, side);
    }
    if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(node.value()))
    {
      if (!side)
      {
        return invalidArgument(
          "/side",
          QString{"%1 is a group; pass the side of the room to cut into"}.arg(wallId),
          "e.g. \"side\":\"+x\" for the wall at the room's maximum x");
      }
      return roomWall(doc, *groupNode, *side);
    }
    return toolError(
      "wrong_node_type",
      QString{"%1 is a %2, not a brush or group"}.arg(
        wallId, nodeTypeName(*node.value())),
      "pass a wall brush, or a room group with side",
      {{"pointer", "/wall"}});
  }();
  if (wallResult.is_error())
  {
    return errorOf(wallResult);
  }
  const auto& wall = wallResult.value();
  const auto along = otherHorizontalAxis(wall.axis);

  if (args.contains("center") && args.contains("offset"))
  {
    return invalidArgument(
      "/center",
      "pass center or offset, not both",
      "center is a coordinate, offset is relative to the middle of the wall");
  }
  const auto middle = (wall.alongMin + wall.alongMax) / 2.0;
  const auto center = args.contains("center")
                        ? args.value("center").toDouble()
                        : middle + args.value("offset").toDouble(0.0);
  const auto width = args.value("width").toDouble();
  const auto bottom = wall.zMin + args.value("bottom").toDouble(0.0);
  const auto height = args.value("height").toDouble();

  auto min = vm::vec3d{};
  auto max = vm::vec3d{};
  min[wall.axis] = wall.slabMin;
  max[wall.axis] = wall.slabMax;
  min[along] = center - width / 2.0;
  max[along] = center + width / 2.0;
  min[2] = bottom;
  max[2] = bottom + height;
  const auto hole = vm::bbox3d{min, max};

  if (
    hole.min[along] < wall.alongMin - Epsilon
    || hole.max[along] > wall.alongMax + Epsilon)
  {
    return invalidArgument(
      "/width",
      QString{"the opening does not fit into the wall: it spans %1 to %2 along %3, but "
              "the wall spans %4 to %5"}
        .arg(canon(hole.min[along]))
        .arg(canon(hole.max[along]))
        .arg(axisName(along))
        .arg(canon(wall.alongMin))
        .arg(canon(wall.alongMax)),
      "offset is measured from the middle of the wall, center is a coordinate; reduce "
      "width or move the opening");
  }
  if (hole.max.z() > wall.zMax + Epsilon)
  {
    return invalidArgument(
      "/height",
      QString{"the opening reaches z %1, above the top of the wall at %2"}
        .arg(canon(hole.max.z()))
        .arg(canon(wall.zMax)),
      QString{"bottom is measured from the bottom of the wall at z %1; reduce height or "
              "bottom"}
        .arg(canon(wall.zMin)));
  }

  // the brushes to cut and the solid volume of the hole
  auto cut = std::vector<mdl::BrushNode*>{};
  auto solid = 0.0;
  for (auto* brushNode : wall.brushNodes)
  {
    if (const auto common = intersection(brushNode->logicalBounds(), hole))
    {
      if (auto check = requireEditableBrush(doc, *brushNode); check.is_error())
      {
        return errorOf(check);
      }
      if (!isCuboid(brushNode->brush()))
      {
        const auto id = doc.nodes.idOf(*brushNode);
        return toolError(
          "not_supported",
          QString{"%1 is not an axis-aligned cuboid"}.arg(id),
          "cut it with csg_subtract and a box instead",
          {{"id", id}});
      }
      cut.push_back(brushNode);
      solid += volume(*common);
    }
  }
  if (solid < volume(hole) - 1e-3)
  {
    return toolError(
      "invalid_argument",
      QString{"the opening is not entirely inside the wall: %1 of its %2 cubic units "
              "are solid; it overlaps an existing opening or a gap"}
        .arg(canon(solid))
        .arg(canon(volume(hole))),
      "move the opening, or describe_region to see the wall",
      {{"pointer", "/wall"}, {"bounds", toJson(hole)}});
  }

  auto cutMaterial = std::optional<std::string>{};
  if (args.contains("material"))
  {
    auto material = materialArgument(map, args, {"material"});
    if (material.is_error())
    {
      return errorOf(material);
    }
    cutMaterial = material.value();
  }

  // split each brush around the part of the hole inside it
  const auto splitSide = sideName(wall.axis, 1.0);
  struct Split
  {
    mdl::BrushNode* brushNode;
    std::vector<mdl::Brush> pieces;
  };
  auto splits = std::vector<Split>{};
  for (auto* brushNode : cut)
  {
    const auto& b = brushNode->logicalBounds();
    const auto opening = Opening{
      .side = splitSide,
      .alongMin = std::max(hole.min[along], b.min[along]),
      .alongMax = std::min(hole.max[along], b.max[along]),
      .zMin = std::max(hole.min.z(), b.min.z()),
      .zMax = std::min(hole.max.z(), b.max.z()),
    };
    auto split = Split{brushNode, {}};
    for (const auto& pieceBounds : splitWall(splitSide, b, {opening}))
    {
      auto piece = brushPiece(map, brushNode->brush(), pieceBounds, cutMaterial);
      if (piece.is_error())
      {
        return errorOf(piece);
      }
      split.pieces.push_back(std::move(piece).value());
    }
    splits.push_back(std::move(split));
  }

  mdl::deselectAll(map);
  auto toUpdate = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
  auto toRemove = std::vector<mdl::Node*>{};
  for (auto& split : splits)
  {
    if (split.pieces.empty())
    {
      toRemove.push_back(split.brushNode);
    }
    else
    {
      toUpdate.emplace_back(split.brushNode, mdl::NodeContents{split.pieces.front()});
    }
  }
  const auto originalIds = idsOf(doc.nodes, cut);
  if (!toUpdate.empty() && !mdl::updateNodeContents(map, "Create Opening", toUpdate))
  {
    return toolError("operation_failed", "the wall could not be cut");
  }

  auto piecesJson = QJsonObject{};
  auto removed = QJsonArray{};
  for (size_t i = 0; i < splits.size(); ++i)
  {
    auto& split = splits[i];
    const auto originalId = originalIds[qsizetype(i)].toString();
    if (split.pieces.empty())
    {
      removed.append(originalId);
      continue;
    }
    auto nodes = std::vector<std::unique_ptr<mdl::Node>>{};
    for (size_t j = 1; j < split.pieces.size(); ++j)
    {
      nodes.push_back(std::make_unique<mdl::BrushNode>(std::move(split.pieces[j])));
    }
    auto ids = QJsonArray{originalId};
    if (!nodes.empty())
    {
      const auto added = mdl::addNodes(map, *split.brushNode->parent(), std::move(nodes));
      if (added.size() + 1 != split.pieces.size())
      {
        return toolError("operation_failed", "the wall pieces could not be added");
      }
      for (const auto* addedNode : added)
      {
        ids.append(doc.nodes.idOf(*addedNode));
      }
    }
    piecesJson[originalId] = ids;
  }
  if (!toRemove.empty())
  {
    mdl::removeNodes(map, toRemove);
  }

  auto openingJson = QJsonObject{{"bounds", toJson(hole)}};
  if (wall.sign)
  {
    openingJson["side"] = sideName(wall.axis, *wall.sign);
  }

  if (cutMaterial)
  {
    warnAboutMaterials(context, {*cutMaterial});
  }
  warnAboutBounds(context, hole);

  return QJsonObject{
    {"opening", openingJson},
    {"pieces", piecesJson},
    {"removed", removed},
  };
}

McpToolDef createOpeningTool()
{
  return {
    .name = "create_opening",
    .title = "Create Opening",
    .description =
      "Cuts a rectangular opening (door, window, passage) through an existing wall: "
      "either one wall brush, or the wall on one side of a room group made by "
      "create_room, also after moving or rotating it in 90 degree steps (wall = group "
      "id, side = the side of the room). The wall brushes "
      "that the opening touches are split into non-overlapping cuboids around it, in "
      "the same parent, keeping their materials; the first piece keeps the brush's id. "
      "Units: offset and width along the wall, bottom and height in z, all in world "
      "units; bottom is measured from the bottom of the wall (the room's floor). "
      "Returns opening {side (missing for a brush passed without side), bounds} "
      "spanning the wall thickness (pass it to "
      "create_corridor as from/to), pieces {original id: [piece ids]} and removed ids. "
      "Verify with query_point (empty inside the opening) and check_brush_overlaps.",
    .inputSchema = schema::object(
      {
        {"wall", schema::nodeId("a wall brush, or a room group together with side")},
        {"side",
         schema::stringEnum(
           {"+x", "-x", "+y", "-y"},
           "the side of the room the wall is on; required for a group, optional for a "
           "brush (then the thinner horizontal axis is the thickness)")},
        {"offset",
         schema::number(
           -MaxSize,
           MaxSize,
           "center of the opening along the wall, relative to the middle of the wall "
           "(default 0); a room's wall middle is the middle of its inner side")},
        {"center",
         schema::number(
           -MaxCoordinate,
           MaxCoordinate,
           "center of the opening along the wall as a coordinate (x for a +y/-y wall, y "
           "for a +x/-x wall); instead of offset")},
        {"width", schema::number(MinSize, MaxSize, "width along the wall")},
        {"height", schema::number(MinSize, MaxSize, "height")},
        {"bottom",
         schema::number(
           0, MaxSize, "height of the bottom above the bottom of the wall (default 0)")},
        {"material",
         schema::material(
           "material of the new faces around the opening (default: the wall's)")},
      },
      {"wall", "width", "height"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createOpening,
  };
}

// create_corridor

/** An opening that a corridor connects to. */
struct CorridorEnd
{
  /** The axis through the wall. */
  size_t axis;
  /** The direction out of the room, if known. */
  std::optional<double> sign;
  vm::bbox3d bounds;
};

McpResult<CorridorEnd> corridorEndArgument(const QJsonObject& args, const QString& key)
{
  const auto pointer = "/" + key;
  const auto object = args.value(key).toObject();
  auto b = bounds(object.value("bounds"), pointer + "/bounds");
  if (b.is_error())
  {
    return errorOf(b);
  }
  const auto& box = b.value();
  for (size_t i = 0; i < 3; ++i)
  {
    if (box.size()[i] <= Epsilon)
    {
      return invalidArgument(
        pointer + "/bounds",
        QString{"the opening is flat in %1"}.arg(axisName(i)),
        "pass the bounds of an opening as returned by create_room or create_opening");
    }
  }

  const auto side = optionalString(object, "side");
  if (side)
  {
    return CorridorEnd{sideAxis(*side), sideSign(*side), box};
  }
  if (std::abs(box.size().x() - box.size().y()) <= Epsilon)
  {
    return invalidArgument(
      pointer,
      "the opening is as deep in x as in y, so the direction through the wall is "
      "unclear",
      "pass side as returned by create_room or create_opening");
  }
  return CorridorEnd{box.size().x() < box.size().y() ? size_t(0) : size_t(1), {}, box};
}

/** A rectangle in the xy plane. */
struct Rect
{
  vm::vec2d min;
  vm::vec2d max;

  bool contains(const vm::vec2d& point) const
  {
    return point.x() > min.x() && point.x() < max.x() && point.y() > min.y()
           && point.y() < max.y();
  }
};

Rect rectOf(const vm::bbox3d& box)
{
  return {vm::vec2d{box.min.x(), box.min.y()}, vm::vec2d{box.max.x(), box.max.y()}};
}

/**
 * Splits the union of the included rectangles minus the excluded ones into
 * non-overlapping rectangles: a grid of all rectangle edges is formed, cells are merged
 * into runs along x and runs of equal extent are merged along y.
 */
std::vector<Rect> decompose(
  const std::vector<Rect>& include, const std::vector<Rect>& exclude)
{
  auto xs = std::set<double>{};
  auto ys = std::set<double>{};
  for (const auto* rects : {&include, &exclude})
  {
    for (const auto& rect : *rects)
    {
      xs.insert({rect.min.x(), rect.max.x()});
      ys.insert({rect.min.y(), rect.max.y()});
    }
  }
  const auto xv = std::vector<double>{xs.begin(), xs.end()};
  const auto yv = std::vector<double>{ys.begin(), ys.end()};

  const auto filled = [&](const vm::vec2d& point) {
    return std::ranges::any_of(include, [&](const auto& r) { return r.contains(point); })
           && std::ranges::none_of(
             exclude, [&](const auto& r) { return r.contains(point); });
  };

  auto result = std::vector<Rect>{};
  // rectangles that end at the current row, by index into result
  auto open = std::map<std::pair<double, double>, size_t>{};
  for (size_t j = 0; j + 1 < yv.size(); ++j)
  {
    const auto y0 = yv[j];
    const auto y1 = yv[j + 1];
    if (y1 - y0 <= Epsilon)
    {
      continue;
    }
    auto runs = std::vector<std::pair<double, double>>{};
    for (size_t i = 0; i + 1 < xv.size(); ++i)
    {
      const auto x0 = xv[i];
      const auto x1 = xv[i + 1];
      if (x1 - x0 <= Epsilon || !filled({(x0 + x1) / 2.0, (y0 + y1) / 2.0}))
      {
        continue;
      }
      if (!runs.empty() && runs.back().second == x0)
      {
        runs.back().second = x1;
      }
      else
      {
        runs.emplace_back(x0, x1);
      }
    }

    auto nextOpen = std::map<std::pair<double, double>, size_t>{};
    for (const auto& run : runs)
    {
      if (const auto it = open.find(run);
          it != open.end() && result[it->second].max.y() == y0)
      {
        result[it->second].max[1] = y1;
        nextOpen[run] = it->second;
      }
      else
      {
        nextOpen[run] = result.size();
        result.push_back({vm::vec2d{run.first, y0}, vm::vec2d{run.second, y1}});
      }
    }
    open = std::move(nextOpen);
  }

  std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
    return std::pair{lhs.min.y(), lhs.min.x()} < std::pair{rhs.min.y(), rhs.min.x()};
  });
  return result;
}

/** A box with the given extents along the given axis and the extents of base otherwise.
 */
vm::bbox3d withExtent(
  vm::bbox3d base, const size_t axis, const double min, const double max)
{
  base.min[axis] = min;
  base.max[axis] = max;
  return base;
}

/** Whether [min, max] lies within [lo, hi]. */
bool within(const double min, const double max, const double lo, const double hi)
{
  return min >= lo - Epsilon && max <= hi + Epsilon;
}

/** The brushes in the map that overlap the box by a volume. */
std::vector<const mdl::BrushNode*> overlappingBrushes(
  const mdl::Map& map, const vm::bbox3d& box)
{
  auto result = std::vector<const mdl::BrushNode*>{};
  auto cuboid = brushBuilder(map).createCuboid(box, mdl::BrushFace::NoMaterialName);
  if (cuboid.is_error())
  {
    return result;
  }
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    for (const auto* child : node.children())
    {
      if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(child))
      {
        if (!intersection(brushNode->logicalBounds(), box))
        {
          continue;
        }
        auto common = cuboid.value();
        if (common.intersect(map.worldBounds(), brushNode->brush()).is_success())
        {
          const auto size = common.bounds().size();
          if (size.x() > 1e-3 && size.y() > 1e-3 && size.z() > 1e-3)
          {
            result.push_back(brushNode);
          }
        }
      }
      else
      {
        self(self, *child);
      }
    }
  };
  visit(visit, map.worldNode());
  return result;
}

McpResult<QJsonObject> createCorridor(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  auto fromResult = corridorEndArgument(args, "from");
  if (fromResult.is_error())
  {
    return errorOf(fromResult);
  }
  auto toResult = corridorEndArgument(args, "to");
  if (toResult.is_error())
  {
    return errorOf(toResult);
  }
  const auto& from = fromResult.value();
  const auto& to = toResult.value();
  const auto t = args.value("wall_thickness").toDouble(16.0);

  const auto checkSide =
    [](const CorridorEnd& end, const QString& key, const double sign) -> McpResult<void> {
    if (end.sign && *end.sign != sign)
    {
      return invalidArgument(
        "/" + key + "/side",
        QString{"the opening faces %1, but the corridor has to leave it towards %2"}.arg(
          sideName(end.axis, *end.sign), sideName(end.axis, sign)),
        "connect openings that face each other or lie on perpendicular walls; use "
        "create_opening to cut one in another wall");
    }
    return kdl::void_success;
  };

  // the interior boxes from `from` to `to`; z is shared by all of them
  auto interiors = std::vector<vm::bbox3d>{};
  // grown by the wall thickness: which horizontal axes of each interior get walls
  auto grow = std::vector<std::array<bool, 2>>{};
  // the corridor stays out of the walls with the openings
  auto clip = Rect{
    vm::vec2d{-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()},
    vm::vec2d{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()}};
  const auto clipTo = [&](const size_t axis, const double sign, const double face) {
    if (sign > 0)
    {
      clip.min[axis] = std::max(clip.min[axis], face);
    }
    else
    {
      clip.max[axis] = std::min(clip.max[axis], face);
    }
  };

  const auto z0 = std::max(from.bounds.min.z(), to.bounds.min.z());
  const auto z1 = std::min(from.bounds.max.z(), to.bounds.max.z());
  if (z1 - z0 <= Epsilon)
  {
    return invalidArgument(
      "/to/bounds",
      QString{"the openings do not overlap in height: z %1 to %2 and %3 to %4"}
        .arg(canon(from.bounds.min.z()))
        .arg(canon(from.bounds.max.z()))
        .arg(canon(to.bounds.min.z()))
        .arg(canon(to.bounds.max.z())),
      "a corridor is level; make the openings overlap in z or add stairs");
  }

  auto shape = QString{};
  if (from.axis == to.axis)
  {
    shape = "straight";
    const auto a = from.axis;
    const auto b = otherHorizontalAxis(a);
    auto dir = 0.0;
    auto g0 = 0.0;
    auto g1 = 0.0;
    if (from.bounds.max[a] <= to.bounds.min[a] + Epsilon)
    {
      dir = 1.0;
      g0 = from.bounds.max[a];
      g1 = to.bounds.min[a];
    }
    else if (to.bounds.max[a] <= from.bounds.min[a] + Epsilon)
    {
      dir = -1.0;
      g0 = to.bounds.max[a];
      g1 = from.bounds.min[a];
    }
    else
    {
      return invalidArgument(
        "/to/bounds",
        QString{"the openings overlap along %1; they must be in different walls"}.arg(
          axisName(a)),
        "pass openings in two walls that face each other");
    }
    if (auto check = checkSide(from, "from", dir); check.is_error())
    {
      return errorOf(check);
    }
    if (auto check = checkSide(to, "to", -dir); check.is_error())
    {
      return errorOf(check);
    }
    if (g1 - g0 <= Epsilon)
    {
      return invalidArgument(
        "/to/bounds",
        "the walls touch, so the openings already connect",
        "no corridor is needed");
    }

    const auto b0 = std::max(from.bounds.min[b], to.bounds.min[b]);
    const auto b1 = std::min(from.bounds.max[b], to.bounds.max[b]);
    if (b1 - b0 <= Epsilon)
    {
      return invalidArgument(
        "/to/bounds",
        QString{"the openings face each other along %1 but do not overlap along %2: %3 "
                "to %4 and %5 to %6"}
          .arg(axisName(a), axisName(b))
          .arg(canon(from.bounds.min[b]))
          .arg(canon(from.bounds.max[b]))
          .arg(canon(to.bounds.min[b]))
          .arg(canon(to.bounds.max[b])),
        "line the openings up (create_opening with center), or connect an opening on "
        "a perpendicular wall for an L-shaped corridor");
    }
    for (const auto* end : {&from, &to})
    {
      if (!within(end->bounds.min[b], end->bounds.max[b], b0 - t, b1 + t))
      {
        return invalidArgument(
          end == &from ? "/from/bounds" : "/to/bounds",
          QString{"the openings differ along %1 by more than wall_thickness, so the "
                  "corridor walls would not close the larger one"}
            .arg(axisName(b)),
          "give both openings the same width and position");
      }
    }

    auto interior = vm::bbox3d{};
    interior.min[a] = g0;
    interior.max[a] = g1;
    interior.min[b] = b0;
    interior.max[b] = b1;
    interior.min[2] = z0;
    interior.max[2] = z1;
    interiors.push_back(interior);
    auto g = std::array<bool, 2>{};
    g[b] = true;
    grow.push_back(g);
  }
  else
  {
    shape = "l_shaped";
    // a: through the from wall, b: through the to wall
    const auto a = from.axis;
    const auto b = to.axis;

    // the first segment leaves `from` along a until it reaches the extent of `to`
    auto sa = 0.0;
    auto s1 = std::pair{0.0, 0.0};
    if (to.bounds.min[a] >= from.bounds.max[a] - Epsilon)
    {
      sa = 1.0;
      s1 = {from.bounds.max[a], to.bounds.min[a]};
    }
    else if (to.bounds.max[a] <= from.bounds.min[a] + Epsilon)
    {
      sa = -1.0;
      s1 = {to.bounds.max[a], from.bounds.min[a]};
    }
    else
    {
      return invalidArgument(
        "/to/bounds",
        QString{"the to opening lies within the extent of the from wall along %1, so "
                "there is no room for the turn"}
          .arg(axisName(a)),
        "move one of the openings along its wall");
    }
    // the second segment leaves the corner along b until it reaches `to`
    auto sb = 0.0;
    auto s2 = std::pair{0.0, 0.0};
    if (to.bounds.min[b] >= from.bounds.max[b] - Epsilon)
    {
      sb = 1.0;
      s2 = {from.bounds.max[b], to.bounds.min[b]};
    }
    else if (to.bounds.max[b] <= from.bounds.min[b] + Epsilon)
    {
      sb = -1.0;
      s2 = {to.bounds.max[b], from.bounds.min[b]};
    }
    else
    {
      return invalidArgument(
        "/to/bounds",
        QString{"the to wall lies within the width of the from opening along %1, so "
                "there is no room for the turn"}
          .arg(axisName(b)),
        "move one of the openings along its wall");
    }
    if (auto check = checkSide(from, "from", sa); check.is_error())
    {
      return errorOf(check);
    }
    if (auto check = checkSide(to, "to", -sb); check.is_error())
    {
      return errorOf(check);
    }

    auto corner = vm::bbox3d{};
    corner.min[a] = to.bounds.min[a];
    corner.max[a] = to.bounds.max[a];
    corner.min[b] = from.bounds.min[b];
    corner.max[b] = from.bounds.max[b];
    corner.min[2] = z0;
    corner.max[2] = z1;

    if (s1.second - s1.first > Epsilon)
    {
      interiors.push_back(withExtent(corner, a, s1.first, s1.second));
      auto g = std::array<bool, 2>{};
      g[b] = true;
      grow.push_back(g);
    }
    interiors.push_back(corner);
    grow.push_back({true, true});
    if (s2.second - s2.first > Epsilon)
    {
      interiors.push_back(withExtent(corner, b, s2.first, s2.second));
      auto g = std::array<bool, 2>{};
      g[a] = true;
      grow.push_back(g);
    }

    clipTo(a, sa, sa > 0 ? from.bounds.max[a] : from.bounds.min[a]);
    clipTo(b, -sb, sb > 0 ? to.bounds.min[b] : to.bounds.max[b]);
  }

  for (const auto* end : {&from, &to})
  {
    if (!within(end->bounds.min.z(), end->bounds.max.z(), z0 - t, z1 + t))
    {
      return invalidArgument(
        end == &from ? "/from/bounds" : "/to/bounds",
        "the openings differ in height by more than wall_thickness, so the corridor "
        "floor or ceiling would not close the larger one",
        "give both openings the same bottom and height");
    }
  }

  // the floor and ceiling cover the interiors and their walls; the walls fill the rest
  auto inner = std::vector<Rect>{};
  auto outer = std::vector<Rect>{};
  for (size_t i = 0; i < interiors.size(); ++i)
  {
    const auto rect = rectOf(interiors[i]);
    inner.push_back(rect);
    auto grown = rect;
    for (size_t k = 0; k < 2; ++k)
    {
      if (grow[i][k])
      {
        grown.min[k] = std::max(grown.min[k] - t, clip.min[k]);
        grown.max[k] = std::min(grown.max[k] + t, clip.max[k]);
      }
    }
    outer.push_back(grown);
  }

  auto material = materialArgument(map, args, {"material"});
  if (material.is_error())
  {
    return errorOf(material);
  }

  auto cuboids = std::vector<Cuboid>{};
  auto floorCount = size_t(0);
  const auto addSlabs =
    [&](const std::vector<Rect>& rects, const double zMin, const double zMax) {
      for (const auto& rect : rects)
      {
        cuboids.push_back(
          {vm::bbox3d{
             vm::vec3d{rect.min.x(), rect.min.y(), zMin},
             vm::vec3d{rect.max.x(), rect.max.y(), zMax}},
           material.value()});
      }
      return rects.size();
    };
  const auto slabs = decompose(outer, {});
  floorCount = addSlabs(slabs, z0 - t, z0);
  const auto ceilingCount = addSlabs(slabs, z1, z1 + t);
  addSlabs(decompose(outer, inner), z0, z1);

  auto all = cuboids.front().bounds;
  for (const auto& cuboid : cuboids)
  {
    all = vm::merge(all, cuboid.bounds);
    if (auto check = boxInWorld(map, cuboid.bounds.min, cuboid.bounds.max);
        check.is_error())
    {
      auto error = errorOf(check);
      error.message = "a corridor brush: " + error.message;
      if (error.code == u"invalid_argument")
      {
        error.hint = "use openings whose edges are whole units";
      }
      return error;
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

  // checked before the corridor exists, so that only other brushes are reported
  auto overlapping = std::set<QString>{};
  auto blocking = std::set<QString>{};
  const auto byNumber = [](const QString& lhs, const QString& rhs) {
    return lhs.mid(1).toULongLong() < rhs.mid(1).toULongLong();
  };
  for (const auto& cuboid : cuboids)
  {
    for (const auto* brushNode : overlappingBrushes(map, cuboid.bounds))
    {
      overlapping.insert(doc.nodes.idOf(*brushNode));
    }
  }
  for (const auto& interior : interiors)
  {
    for (const auto* brushNode : overlappingBrushes(map, interior))
    {
      blocking.insert(doc.nodes.idOf(*brushNode));
    }
  }
  const auto sortedIds = [&](const std::set<QString>& ids) {
    auto result = QStringList{ids.begin(), ids.end()};
    std::ranges::sort(result, byNumber);
    return result.join(", ");
  };
  if (!overlapping.empty())
  {
    context.warnings.push_back(
      QString{"the corridor brushes overlap %1; check check_brush_overlaps"}.arg(
        sortedIds(overlapping)));
  }
  if (!blocking.empty())
  {
    context.warnings.push_back(
      QString{"the corridor interior is blocked by %1"}.arg(sortedIds(blocking)));
  }

  auto added = addCuboids(map, *parent.value(), cuboids);
  if (added.is_error())
  {
    return errorOf(added);
  }
  const auto& brushNodes = added.value();
  const auto idsIn = [&](const size_t first, const size_t count) {
    return idsOf(
      doc.nodes,
      std::vector<mdl::BrushNode*>{
        brushNodes.begin() + qsizetype(first),
        brushNodes.begin() + qsizetype(first + count)});
  };

  auto interiorsJson = QJsonArray{};
  for (const auto& interior : interiors)
  {
    interiorsJson.append(toJson(interior));
  }

  auto result = QJsonObject{
    {"shape", shape},
    {"ids", idsOf(doc.nodes, brushNodes)},
    {"floor", idsIn(0, floorCount)},
    {"ceiling", idsIn(floorCount, ceilingCount)},
    {"walls",
     idsIn(floorCount + ceilingCount, brushNodes.size() - floorCount - ceilingCount)},
    {"interior_bounds", interiorsJson},
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

  warnAboutMaterials(context, {material.value()});
  warnAboutBounds(context, all);
  return result;
}

QJsonObject openingRefSchema(const QString& description)
{
  return schema::describe(
    schema::object(
      {
        {"bounds",
         schema::bounds("the opening through the wall, spanning its thickness")},
        {"side",
         schema::stringEnum(
           {"+x", "-x", "+y", "-y"},
           "the side of its room the wall is on, i.e. the direction out of the room")},
      },
      {"bounds"}),
    description);
}

McpToolDef createCorridorTool()
{
  return {
    .name = "create_corridor",
    .title = "Create Corridor",
    .description =
      "Builds a sealed corridor of non-overlapping cuboid brushes (floor, ceiling and "
      "walls of wall_thickness world units) between two wall openings, e.g. the "
      "openings returned by create_room or create_opening (pass them unchanged as from "
      "and to). Openings that face each other through parallel walls give a straight "
      "corridor; openings on perpendicular walls give an L-shaped one (two segments "
      "joined by a corner). The interior has the cross-section of the openings (their "
      "overlap if they differ by up to wall_thickness). The corridor touches the outer "
      "faces of the walls without overlapping them, so the rooms stay sealed. Returns "
      "ids, floor/ceiling/walls ids and interior_bounds (from 'from' to 'to'). Verify "
      "with check_leaks and check_brush_overlaps.",
    .inputSchema = schema::object(
      {
        {"from", openingRefSchema("the opening to start at")},
        {"to", openingRefSchema("the opening to end at")},
        {"wall_thickness",
         schema::number(
           MinSize, 4096, "thickness of floor, ceiling and walls (default 16)")},
        {"material", schema::material("material (default: current)")},
        {"group_name", schema::string(1, 128, "group the brushes under this name")},
        {"parent", schema::nodeId("a layer or group (default: the current one)")},
      },
      {"from", "to"}),
    .kind = McpToolKind::Mutate,
    .annotations = Mutating,
    .allowedInBatch = true,
    .run = createCorridor,
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
  dispatcher.add(describeRegionTool());
  dispatcher.add(createRoomTool());
  dispatcher.add(createOpeningTool());
  dispatcher.add(createCorridorTool());
  dispatcher.add(createStairsTool());
  dispatcher.add(dropToFloorTool());
  dispatcher.add(alignNodesTool());
}

} // namespace tb::ui
