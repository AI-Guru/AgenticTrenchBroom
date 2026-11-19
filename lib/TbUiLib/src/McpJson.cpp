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

#include "ui/McpJson.h"

#include <QCryptographicHash>
#include <QJsonDocument>

#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "ui/McpNodeRegistry.h"
#include "ui/QStringUtils.h"

#include "kd/overload.h"

#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace tb::ui
{

double canon(const double v)
{
  if (!std::isfinite(v) || std::abs(v) >= 1e15)
  {
    return v;
  }

  const auto rounded = std::round(v * 1e6) / 1e6;
  return rounded == 0.0 ? 0.0 : rounded;
}

QJsonArray toJson(const vm::vec3d& v)
{
  return {canon(v.x()), canon(v.y()), canon(v.z())};
}

QJsonArray toJson(const vm::vec2f& v)
{
  return {canon(double(v.x())), canon(double(v.y()))};
}

QJsonObject toJson(const vm::bbox3d& bounds)
{
  return {{"min", toJson(bounds.min)}, {"max", toJson(bounds.max)}};
}

QJsonObject toJson(const vm::plane3d& plane)
{
  return {{"normal", toJson(plane.normal)}, {"distance", canon(plane.distance)}};
}

QString toUnicode(const mdl::Map& map, const std::string& str)
{
  return mapStringToUnicode(map.encoding(), str);
}

std::string fromUnicode(const mdl::Map& map, const QString& str)
{
  return mapStringFromUnicode(map.encoding(), str);
}

QString sha256Hex(const QJsonValue& value)
{
  const auto document = value.isObject()  ? QJsonDocument{value.toObject()}
                        : value.isArray() ? QJsonDocument{value.toArray()}
                                          : QJsonDocument{QJsonArray{value}};
  return QString::fromLatin1(
    QCryptographicHash::hash(
      document.toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)
      .toHex());
}

std::optional<QString> sideName(const vm::vec3d& normal)
{
  static const auto sides = std::array<std::pair<vm::vec3d, const char*>, 6>{{
    {vm::vec3d{1, 0, 0}, "+x"},
    {vm::vec3d{-1, 0, 0}, "-x"},
    {vm::vec3d{0, 1, 0}, "+y"},
    {vm::vec3d{0, -1, 0}, "-y"},
    {vm::vec3d{0, 0, 1}, "+z"},
    {vm::vec3d{0, 0, -1}, "-z"},
  }};

  for (const auto& [axis, name] : sides)
  {
    if (vm::dot(normal, axis) > 1.0 - 1e-9)
    {
      return QString::fromLatin1(name);
    }
  }
  return std::nullopt;
}

std::optional<vm::vec3d> sideNormal(const QString& side)
{
  if (side.size() != 2 || (side[0] != u'+' && side[0] != u'-'))
  {
    return std::nullopt;
  }

  const auto sign = side[0] == u'+' ? 1.0 : -1.0;
  if (side[1] == u'x')
  {
    return vm::vec3d{sign, 0, 0};
  }
  if (side[1] == u'y')
  {
    return vm::vec3d{0, sign, 0};
  }
  if (side[1] == u'z')
  {
    return vm::vec3d{0, 0, sign};
  }
  return std::nullopt;
}

double brushVolume(const mdl::Brush& brush)
{
  auto sum = 0.0;
  for (const auto& face : brush.faces())
  {
    const auto vertices = face.vertexPositions();
    for (size_t i = 1; i + 1 < vertices.size(); ++i)
    {
      sum += vm::dot(vertices[0], vm::cross(vertices[i], vertices[i + 1]));
    }
  }
  return std::abs(sum) / 6.0;
}

QString nodeTypeName(const mdl::Node& node)
{
  return node.accept(kdl::overload(
    [](const mdl::WorldNode&) { return QStringLiteral("world"); },
    [](const mdl::LayerNode&) { return QStringLiteral("layer"); },
    [](const mdl::GroupNode&) { return QStringLiteral("group"); },
    [](const mdl::EntityNode&) { return QStringLiteral("entity"); },
    [](const mdl::BrushNode&) { return QStringLiteral("brush"); },
    [](const mdl::PatchNode&) { return QStringLiteral("patch"); }));
}

namespace
{

QJsonValue idOrNull(const McpNodeRegistry& registry, const mdl::Node* node)
{
  return node ? QJsonValue{registry.idOf(*node)} : QJsonValue{};
}

const mdl::LayerNode* containingLayer(const mdl::Node& node)
{
  for (const auto* current = &node; current; current = current->parent())
  {
    if (const auto* layerNode = dynamic_cast<const mdl::LayerNode*>(current))
    {
      return layerNode;
    }
  }
  return nullptr;
}

QJsonValue ownerId(const McpNodeRegistry& registry, const mdl::EntityNodeBase* entity)
{
  if (!entity || dynamic_cast<const mdl::WorldNode*>(entity))
  {
    return QStringLiteral("world");
  }
  return registry.idOf(*entity);
}

QJsonArray brushMaterials(const mdl::Map& map, const mdl::Brush& brush)
{
  auto materials = std::set<QString>{};
  for (const auto& face : brush.faces())
  {
    materials.insert(toUnicode(map, face.materialName()));
  }

  auto result = QJsonArray{};
  for (const auto& material : materials)
  {
    result.append(material);
  }
  return result;
}

} // namespace

QJsonObject nodeSummary(
  const McpNodeRegistry& registry, const mdl::Map& map, const mdl::Node& node)
{
  const auto& editorContext = map.editorContext();

  auto result = QJsonObject{
    {"id", registry.idOf(node)},
    {"type", nodeTypeName(node)},
    {"parent", idOrNull(registry, node.parent())},
    {"layer", idOrNull(registry, containingLayer(node))},
    {"bounds", toJson(node.logicalBounds())},
    {"selected", node.selected()},
    {"visible", editorContext.visible(node)},
    {"locked", node.locked()},
  };

  node.accept(kdl::overload(
    [&](const mdl::WorldNode& worldNode) {
      result["classname"] = toUnicode(map, worldNode.entity().classname());
      result["child_count"] = qint64(worldNode.childCount());
    },
    [&](const mdl::LayerNode& layerNode) {
      result["name"] = toUnicode(map, layerNode.layer().name());
      result["child_count"] = qint64(layerNode.childCount());
    },
    [&](const mdl::GroupNode& groupNode) {
      result["name"] = toUnicode(map, groupNode.group().name());
      result["child_count"] = qint64(groupNode.childCount());
    },
    [&](const mdl::EntityNode& entityNode) {
      result["classname"] = toUnicode(map, entityNode.entity().classname());
      result["child_count"] = qint64(entityNode.childCount());
    },
    [&](const mdl::BrushNode& brushNode) {
      result["entity"] = ownerId(registry, brushNode.entity());
      result["materials"] = brushMaterials(map, brushNode.brush());
    },
    [&](const mdl::PatchNode& patchNode) {
      result["entity"] = ownerId(registry, patchNode.entity());
      result["material"] = toUnicode(map, patchNode.patch().materialName());
    }));

  return result;
}

} // namespace tb::ui
