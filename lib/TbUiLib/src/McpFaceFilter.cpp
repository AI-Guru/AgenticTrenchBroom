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

#include "ui/McpFaceFilter.h"

#include <QJsonArray>

#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"
#include "ui/McpJson.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"

#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace tb::ui
{
namespace
{

using Polygon2 = std::vector<vm::vec2d>;

/** Distances below this count as on the plane or line. */
constexpr auto PlaneEpsilon = 1e-3;
/** Signed distances to a clipping line below this are snapped to 0. */
constexpr auto LineEpsilon = 1e-7;
/** Uncovered pieces smaller than this (square units) are ignored. */
constexpr auto AreaEpsilon = 1e-6;
/** Coverage gives up (exposed) when the remainder has more pieces. */
constexpr auto MaxPieces = size_t(1024);
/** Two normals are opposite if their dot product is at most -1 + this. */
constexpr auto OppositeEpsilon = 1e-9;
/** Angle comparisons allow this much numeric slack. */
constexpr auto AngleEpsilon = 1e-9;

const auto FacingNames =
  QStringList{"up", "down", "horizontal", "+x", "-x", "+y", "-y", "+z", "-z"};

double cross2(const vm::vec2d& a, const vm::vec2d& b)
{
  return a.x() * b.y() - a.y() * b.x();
}

double signedArea(const Polygon2& polygon)
{
  auto result = 0.0;
  for (size_t i = 0; i < polygon.size(); ++i)
  {
    result += cross2(polygon[i], polygon[(i + 1) % polygon.size()]);
  }
  return result / 2.0;
}

/** Returns a counter-clockwise copy, or an empty polygon if it is degenerate. */
Polygon2 counterClockwise(Polygon2 polygon)
{
  if (polygon.size() < 3)
  {
    return {};
  }
  const auto area = signedArea(polygon);
  if (std::abs(area) < AreaEpsilon)
  {
    return {};
  }
  if (area < 0.0)
  {
    std::ranges::reverse(polygon);
  }
  return polygon;
}

/**
 * Splits a convex polygon by the line through a with unit direction d into the part on
 * the left (inside a counter-clockwise polygon with this edge) and the part on the right.
 */
std::pair<Polygon2, Polygon2> split(
  const Polygon2& polygon, const vm::vec2d& a, const vm::vec2d& d)
{
  auto distances = std::vector<double>{};
  distances.reserve(polygon.size());
  auto anyLeft = false;
  auto anyRight = false;
  for (const auto& p : polygon)
  {
    auto s = cross2(d, p - a);
    if (std::abs(s) < LineEpsilon)
    {
      s = 0.0;
    }
    anyLeft = anyLeft || s > 0.0;
    anyRight = anyRight || s < 0.0;
    distances.push_back(s);
  }
  if (!anyRight)
  {
    return {polygon, {}};
  }
  if (!anyLeft)
  {
    return {{}, polygon};
  }

  auto left = Polygon2{};
  auto right = Polygon2{};
  for (size_t i = 0; i < polygon.size(); ++i)
  {
    const auto j = (i + 1) % polygon.size();
    const auto& p = polygon[i];
    const auto& q = polygon[j];
    const auto sp = distances[i];
    const auto sq = distances[j];
    if (sp >= 0.0)
    {
      left.push_back(p);
    }
    if (sp <= 0.0)
    {
      right.push_back(p);
    }
    if ((sp > 0.0 && sq < 0.0) || (sp < 0.0 && sq > 0.0))
    {
      const auto x = p + (q - p) * (sp / (sp - sq));
      left.push_back(x);
      right.push_back(x);
    }
  }
  return {std::move(left), std::move(right)};
}

/** Orthonormal basis (u, v) of the plane with the given unit normal. */
std::pair<vm::vec3d, vm::vec3d> planeBasis(const vm::vec3d& normal)
{
  auto axis = size_t(0);
  for (size_t i = 1; i < 3; ++i)
  {
    if (std::abs(normal[i]) < std::abs(normal[axis]))
    {
      axis = i;
    }
  }
  auto e = vm::vec3d{0, 0, 0};
  e[axis] = 1.0;
  const auto u = vm::normalize(vm::cross(normal, e));
  const auto v = vm::cross(normal, u);
  return {u, v};
}

Polygon2 project(
  const std::vector<vm::vec3d>& positions, const vm::vec3d& u, const vm::vec3d& v)
{
  auto result = Polygon2{};
  result.reserve(positions.size());
  for (const auto& p : positions)
  {
    result.emplace_back(vm::dot(p, u), vm::dot(p, v));
  }
  return result;
}

std::int64_t quantize(const double value, const double scale)
{
  return std::llround(value * scale);
}

/** Opposite planes have exactly negated keys because llround is symmetric. */
std::array<std::int64_t, 4> planeKey(const vm::vec3d& normal, const double distance)
{
  return {
    quantize(normal.x(), 1e4),
    quantize(normal.y(), 1e4),
    quantize(normal.z(), 1e4),
    quantize(distance, 1e2),
  };
}

bool isWorldBrush(const mdl::BrushNode& brushNode)
{
  return dynamic_cast<const mdl::WorldNode*>(brushNode.entity()) != nullptr;
}

void collectWorldBrushes(
  const mdl::Node& node, std::vector<const mdl::BrushNode*>& result)
{
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    if (isWorldBrush(*brushNode))
    {
      result.push_back(brushNode);
    }
    return;
  }
  for (const auto* child : node.children())
  {
    collectWorldBrushes(*child, result);
  }
}

/** Area of the part of the planar convex polygon that lies in the box (boundary
 * included). */
double areaInside(
  std::vector<vm::vec3d> polygon, const vm::bbox3d& box, const vm::vec3d& normal)
{
  const auto normalAxis = vm::find_abs_max_component(normal);
  for (size_t axis = 0; axis < 3 && polygon.size() >= 3; ++axis)
  {
    for (const auto sign : {1.0, -1.0})
    {
      // keep the points with sign * p[axis] <= limit
      // only along the normal: a face on the boundary plane counts, but a face that
      // ends at the boundary must not leave a sliver
      const auto epsilon = axis == normalAxis ? PlaneEpsilon : 0.0;
      const auto limit = (sign > 0.0 ? box.max[axis] : -box.min[axis]) + epsilon;
      auto clipped = std::vector<vm::vec3d>{};
      for (size_t i = 0; i < polygon.size(); ++i)
      {
        const auto& p = polygon[i];
        const auto& q = polygon[(i + 1) % polygon.size()];
        const auto dp = sign * p[axis] - limit;
        const auto dq = sign * q[axis] - limit;
        if (dp <= 0.0)
        {
          clipped.push_back(p);
        }
        if ((dp < 0.0 && dq > 0.0) || (dp > 0.0 && dq < 0.0))
        {
          clipped.push_back(p + (q - p) * (dp / (dp - dq)));
        }
      }
      polygon = std::move(clipped);
      if (polygon.size() < 3)
      {
        return 0.0;
      }
    }
  }

  auto sum = vm::vec3d{0, 0, 0};
  for (size_t i = 0; i < polygon.size(); ++i)
  {
    sum = sum + vm::cross(polygon[i], polygon[(i + 1) % polygon.size()]);
  }
  return std::abs(vm::dot(sum, normal)) / 2.0;
}

bool boundsTouch(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  for (size_t i = 0; i < 3; ++i)
  {
    if (lhs.min[i] > rhs.max[i] + PlaneEpsilon || rhs.min[i] > lhs.max[i] + PlaneEpsilon)
    {
      return false;
    }
  }
  return true;
}

} // namespace

QJsonObject faceFilterSchemaProperties()
{
  return QJsonObject{
    {"facing",
     QJsonObject{
       {"oneOf",
        QJsonArray{
          schema::stringEnum(FacingNames),
          schema::vec3("a direction the face normal must point to"),
        }},
       {"description",
        "the direction the face normal (pointing out of the brush) must point to: up = "
        "floor surfaces you can walk on, down = the visible undersides of ceilings, "
        "horizontal = walls, +x..-z = along that axis, or a vector [x,y,z]"},
     }},
    {"tolerance_degrees",
     schema::number(
       0,
       90,
       "maximum angle in degrees between the normal and facing (for horizontal: "
       "between the normal and the horizontal plane) (default 10)")},
    {"material",
     schema::string(1, 256, "material name glob (* and ?, case insensitive)")},
    {"exposed_only",
     schema::boolean(
       "drop faces that are entirely covered by touching faces of other world brushes, "
       "e.g. the bottom of a wall standing on a floor (default false); faces facing the "
       "void outside the map count as exposed")},
    {"inside",
     schema::bounds(
       "only faces with some area in these bounds (faces on the boundary count), e.g. a "
       "room's inner_bounds keeps the faces that face into the room and skips the outer "
       "faces; the faces of a door opening (jambs, lintel) lie in the wall, outside the "
       "inner bounds, so find them without inside, e.g. with the ids of the wall "
       "pieces")},
  };
}

McpResult<McpFaceFilter> parseFaceFilter(
  const QJsonObject& args, const QString& pointerPrefix)
{
  auto filter = McpFaceFilter{};

  if (args.contains("facing"))
  {
    const auto facing = args.value("facing");
    if (facing.isString())
    {
      const auto name = facing.toString();
      if (name == u"horizontal")
      {
        filter.facing = McpFaceFilter::Facing::Horizontal;
      }
      else
      {
        filter.facing = McpFaceFilter::Facing::Direction;
        filter.direction = name == u"up" ? vm::vec3d{0, 0, 1}
                           : name == u"down"
                             ? vm::vec3d{0, 0, -1}
                             : sideNormal(name).value_or(vm::vec3d{0, 0, 1});
      }
    }
    else
    {
      const auto pointer = pointerPrefix + "/facing";
      auto direction = vec3(facing, pointer);
      if (direction.is_error())
      {
        return errorOf(direction);
      }
      const auto length = vm::length(direction.value());
      if (!(length >= 1e-9))
      {
        return toolError(
          "invalid_argument",
          QString{"%1 must not be a zero vector"}.arg(pointer),
          "e.g. [0,0,1] for faces pointing up",
          {{"pointer", pointer}});
      }
      filter.facing = McpFaceFilter::Facing::Direction;
      filter.direction = direction.value() / length;
    }
  }

  filter.toleranceDegrees =
    std::clamp(args.value("tolerance_degrees").toDouble(10.0), 0.0, 90.0);
  filter.material = optionalString(args, "material");
  filter.exposedOnly = args.value("exposed_only").toBool(false);

  if (args.contains("inside"))
  {
    auto inside = bounds(args.value("inside"), pointerPrefix + "/inside");
    if (inside.is_error())
    {
      return errorOf(inside);
    }
    filter.inside = inside.value();
  }

  return filter;
}

McpFaceMatcher::McpFaceMatcher(const mdl::Map& map, McpFaceFilter filter)
  : m_map{map}
  , m_filter{std::move(filter)}
  , m_cosTolerance{std::cos(m_filter.toleranceDegrees * std::numbers::pi / 180.0)}
  , m_sinTolerance{std::sin(m_filter.toleranceDegrees * std::numbers::pi / 180.0)}
{
  if (m_filter.exposedOnly)
  {
    auto brushNodes = std::vector<const mdl::BrushNode*>{};
    collectWorldBrushes(map.worldNode(), brushNodes);
    for (const auto* brushNode : brushNodes)
    {
      const auto& brush = brushNode->brush();
      for (size_t i = 0; i < brush.faceCount(); ++i)
      {
        const auto& face = brush.face(i);
        m_facesByPlane[planeKey(face.normal(), face.boundary().distance)].push_back(
          IndexedFace{brushNode, i, face.bounds()});
      }
    }
  }
}

bool McpFaceMatcher::matches(
  const mdl::BrushNode& brushNode, const size_t faceIndex) const
{
  const auto& face = brushNode.brush().face(faceIndex);
  const auto& normal = face.normal();

  switch (m_filter.facing)
  {
  case McpFaceFilter::Facing::Any:
    break;
  case McpFaceFilter::Facing::Direction:
    if (vm::dot(normal, m_filter.direction) < m_cosTolerance - AngleEpsilon)
    {
      return false;
    }
    break;
  case McpFaceFilter::Facing::Horizontal:
    if (std::abs(normal.z()) > m_sinTolerance + AngleEpsilon)
    {
      return false;
    }
    break;
  }

  if (
    m_filter.material
    && !globMatch(*m_filter.material, toUnicode(m_map, face.materialName())))
  {
    return false;
  }

  if (m_filter.inside)
  {
    if (!(areaInside(face.vertexPositions(), *m_filter.inside, face.normal())
          >= AreaEpsilon))
    {
      return false;
    }
  }

  return !m_filter.exposedOnly || isExposed(brushNode, faceIndex);
}

bool McpFaceMatcher::isExposed(
  const mdl::BrushNode& brushNode, const size_t faceIndex) const
{
  const auto& face = brushNode.brush().face(faceIndex);
  const auto& normal = face.normal();
  const auto distance = face.boundary().distance;

  const auto it = m_facesByPlane.find(planeKey(-normal, -distance));
  if (it == m_facesByPlane.end())
  {
    return true;
  }

  const auto bounds = face.bounds();
  const auto [u, v] = planeBasis(normal);

  auto covers = std::vector<Polygon2>{};
  for (const auto& candidate : it->second)
  {
    if (candidate.brushNode == &brushNode || !boundsTouch(bounds, candidate.bounds))
    {
      continue;
    }
    const auto& other = candidate.brushNode->brush().face(candidate.faceIndex);
    if (vm::dot(normal, other.normal()) > -1.0 + OppositeEpsilon)
    {
      continue;
    }
    const auto positions = other.vertexPositions();
    if (std::ranges::any_of(positions, [&](const auto& p) {
          return std::abs(vm::dot(p, normal) - distance) > PlaneEpsilon;
        }))
    {
      continue;
    }
    covers.push_back(project(positions, u, v));
  }

  return covers.empty()
         || !isCoveredByPolygons(project(face.vertexPositions(), u, v), covers);
}

bool isCoveredByPolygons(
  const std::vector<vm::vec2d>& polygon,
  const std::vector<std::vector<vm::vec2d>>& covers)
{
  auto remaining = std::vector<Polygon2>{};
  if (auto first = counterClockwise(polygon); !first.empty())
  {
    remaining.push_back(std::move(first));
  }
  else
  {
    return false;
  }

  for (const auto& coverPolygon : covers)
  {
    const auto cover = counterClockwise(coverPolygon);
    if (cover.empty())
    {
      continue;
    }

    auto next = std::vector<Polygon2>{};
    for (auto piece : remaining)
    {
      for (size_t i = 0; i < cover.size() && !piece.empty(); ++i)
      {
        const auto& a = cover[i];
        const auto edge = cover[(i + 1) % cover.size()] - a;
        const auto length = vm::length(edge);
        if (length < LineEpsilon)
        {
          continue;
        }
        auto [left, right] = split(piece, a, edge / length);
        if (right.size() >= 3 && std::abs(signedArea(right)) >= AreaEpsilon)
        {
          next.push_back(std::move(right));
        }
        piece = left.size() >= 3 && std::abs(signedArea(left)) >= AreaEpsilon
                  ? std::move(left)
                  : Polygon2{};
      }
      // what is left of piece lies inside the cover
      if (next.size() > MaxPieces)
      {
        return false;
      }
    }
    remaining = std::move(next);
    if (remaining.empty())
    {
      return true;
    }
  }
  return remaining.empty();
}

double faceArea(const mdl::BrushNode& brushNode, const size_t faceIndex)
{
  const auto& face = brushNode.brush().face(faceIndex);
  const auto positions = face.vertexPositions();
  auto sum = vm::vec3d{0, 0, 0};
  for (size_t i = 0; i < positions.size(); ++i)
  {
    sum = sum + vm::cross(positions[i], positions[(i + 1) % positions.size()]);
  }
  return std::abs(vm::dot(sum, face.normal())) / 2.0;
}

QJsonObject faceRefJson(
  const McpNodeRegistry& registry,
  const mdl::Map& map,
  const mdl::BrushNode& brushNode,
  const size_t faceIndex)
{
  const auto& face = brushNode.brush().face(faceIndex);
  auto result = QJsonObject{
    {"brush", registry.idOf(brushNode)},
    {"index", qint64(faceIndex)},
    {"normal", toJson(face.normal())},
    {"area", canon(faceArea(brushNode, faceIndex))},
    {"center", toJson(face.center())},
    {"material", toUnicode(map, face.materialName())},
  };
  if (const auto side = sideName(face.normal()))
  {
    result["side"] = *side;
  }
  return result;
}

} // namespace tb::ui
