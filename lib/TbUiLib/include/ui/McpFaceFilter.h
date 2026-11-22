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

#pragma once

#include <QJsonObject>
#include <QString>

#include "ui/McpDocumentState.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class Map;
} // namespace tb::mdl

namespace tb::ui
{
class McpNodeRegistry;

/**
 * Picks brush faces by orientation, material, position and exposure. Shared by
 * find_faces and the face_filter of set_face_attributes.
 */
struct McpFaceFilter
{
  enum class Facing
  {
    /** Every orientation. */
    Any,
    /** The normal is within the tolerance of direction. */
    Direction,
    /** The normal is within the tolerance of the horizontal plane (walls). */
    Horizontal,
  };

  Facing facing = Facing::Any;
  /** Unit vector, used if facing is Direction. */
  vm::vec3d direction = vm::vec3d{0, 0, 1};
  double toleranceDegrees = 10.0;
  /** Case insensitive glob for the material name. */
  std::optional<QString> material;
  /** Drop faces that are entirely covered by faces of other world brushes. */
  bool exposedOnly = false;
  /** Part of the face (area > 1e-6) must lie in these bounds, boundary included. */
  std::optional<vm::bbox3d> inside;
};

/**
 * Returns the schema properties of a face filter: facing, tolerance_degrees, material,
 * exposed_only and inside.
 */
QJsonObject faceFilterSchemaProperties();

/**
 * Reads a face filter from the given (schema checked) object. pointerPrefix is prepended
 * to the error pointers, e.g. "/face_filter". Errors: invalid_argument.
 */
McpResult<McpFaceFilter> parseFaceFilter(
  const QJsonObject& args, const QString& pointerPrefix);

/**
 * Applies a face filter. For exposed_only, the constructor indexes the faces of all world
 * brushes of the map (brushes whose entity is the world, including brushes in groups and
 * hidden brushes) by plane. The matcher must not outlive a change of the map.
 */
class McpFaceMatcher
{
public:
  McpFaceMatcher(const mdl::Map& map, McpFaceFilter filter);

  bool matches(const mdl::BrushNode& brushNode, size_t faceIndex) const;

  /**
   * Whether the face is not entirely covered by the union of the touching, coplanar,
   * opposite facing faces of other world brushes. Conservative: if the coverage cannot
   * be established (no candidate, plane mismatch, too many pieces), the face is exposed.
   */
  bool isExposed(const mdl::BrushNode& brushNode, size_t faceIndex) const;

private:
  struct IndexedFace
  {
    const mdl::BrushNode* brushNode;
    size_t faceIndex;
    vm::bbox3d bounds;
  };
  using PlaneKey = std::array<std::int64_t, 4>;

  const mdl::Map& m_map;
  McpFaceFilter m_filter;
  double m_cosTolerance;
  double m_sinTolerance;
  std::map<PlaneKey, std::vector<IndexedFace>> m_facesByPlane;
};

/**
 * Whether the union of the given convex polygons covers the given convex polygon. All
 * polygons are in the same 2D plane, in any winding order. Pieces of the polygon that
 * remain uncovered are ignored if their area is below 1e-6 square units. Returns false
 * (not covered) if the polygon is degenerate or the uncovered remainder splits into more
 * than 1024 pieces.
 */
bool isCoveredByPolygons(
  const std::vector<vm::vec2d>& polygon,
  const std::vector<std::vector<vm::vec2d>>& covers);

/**
 * Returns {brush, index, side?, normal, area, center, material} for the given face.
 * side is only present for axis aligned faces.
 */
QJsonObject faceRefJson(
  const McpNodeRegistry& registry,
  const mdl::Map& map,
  const mdl::BrushNode& brushNode,
  size_t faceIndex);

/** Returns the area of the face polygon in square units. */
double faceArea(const mdl::BrushNode& brushNode, size_t faceIndex);

} // namespace tb::ui
