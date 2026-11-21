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

#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/EmptyPropertyValueValidator.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionFileSpec.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Assets.h"
#include "mdl/MissingDefinitionValidator.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/SurfaceAttributes.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"
#include "ui/McpNodeRegistry.h"
#include "ui/QStringUtils.h"

#include "kd/overload.h"

#include "vm/vec.h"

#include <algorithm>
#include <array>
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
  if (surface.color)
  {
    result["color"] = QString::fromStdString(surface.color->toString());
  }
  return result;
}

std::optional<QString> optionalString(const QJsonObject& args, const QString& key)
{
  return args.contains(key) ? std::optional{args.value(key).toString()} : std::nullopt;
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

namespace
{

template <typename GetBounds>
std::optional<vm::bbox3d> contentBounds(const mdl::Node& node, const GetBounds& getBounds)
{
  if (
    !dynamic_cast<const mdl::WorldNode*>(&node)
    && !dynamic_cast<const mdl::LayerNode*>(&node))
  {
    return getBounds(node);
  }

  auto result = std::optional<vm::bbox3d>{};
  for (const auto* child : node.children())
  {
    if (const auto childBounds = contentBounds(*child, getBounds))
    {
      result = result ? vm::merge(*result, *childBounds) : *childBounds;
    }
  }
  return result;
}

} // namespace

std::optional<vm::bbox3d> nodeBounds(const mdl::Node& node)
{
  return contentBounds(node, [](const mdl::Node& n) { return n.logicalBounds(); });
}

std::optional<vm::bbox3d> nodePhysicalBounds(const mdl::Node& node)
{
  return contentBounds(node, [](const mdl::Node& n) { return n.physicalBounds(); });
}

QJsonObject nodeSummary(
  const McpNodeRegistry& registry, const mdl::Map& map, const mdl::Node& node)
{
  const auto bounds = nodeBounds(node);
  const auto& editorContext = map.editorContext();

  auto result = QJsonObject{
    {"id", registry.idOf(node)},
    {"type", nodeTypeName(node)},
    {"parent", idOrNull(registry, node.parent())},
    {"layer", idOrNull(registry, containingLayer(node))},
    {"bounds", bounds ? QJsonValue{toJson(*bounds)} : QJsonValue{}},
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

QJsonObject selectionJson(const McpNodeRegistry& registry, const mdl::Map& map)
{
  const auto& selection = map.selection();

  auto nodes = QJsonArray{};
  for (const auto* node : selection.nodes)
  {
    nodes.append(registry.idOf(*node));
  }

  auto faces = QJsonArray{};
  for (const auto& handle : selection.brushFaces)
  {
    auto face = QJsonObject{
      {"brush", registry.idOf(handle.node())},
      {"index", qint64(handle.faceIndex())},
    };
    if (const auto side = sideName(handle.face().normal()))
    {
      face["side"] = *side;
    }
    faces.append(face);
  }

  const auto& selectionBounds = map.selectionBounds();
  return QJsonObject{
    {"nodes", nodes},
    {"faces", faces},
    {"bounds", selectionBounds ? QJsonValue{toJson(*selectionBounds)} : QJsonValue{}},
  };
}

QString mcpGenericEntityDefinitionsNote()
{
  return "game Generic only defines worldspawn and info_player_start (Generic.fgd), so "
         "validate_map reports entities with other classnames, e.g. light, as 'not found "
         "in entity definitions'; they are kept and saved unchanged. Use game Quake if "
         "you want entity definitions (light, monsters, items, doors); it needs no game "
         "files either, but materials only load when its game path is configured";
}

std::optional<QString> expectedIssueReason(
  const mdl::Map& map,
  const std::vector<const mdl::Validator*>& validators,
  const mdl::Issue& issue)
{
  const auto it = std::ranges::find_if(
    validators, [&](const auto* validator) { return validator->type() == issue.type(); });
  if (it == validators.end())
  {
    return std::nullopt;
  }
  const auto* validator = *it;
  const auto& gameConfig = map.gameInfo().gameConfig;

  if (dynamic_cast<const mdl::EmptyPropertyValueValidator*>(validator))
  {
    const auto* propertyIssue = dynamic_cast<const mdl::EntityPropertyIssue*>(&issue);
    if (
      propertyIssue && dynamic_cast<const mdl::WorldNode*>(&issue.node())
      && gameConfig.materialConfig.property
      && propertyIssue->propertyKey() == *gameConfig.materialConfig.property)
    {
      return QString{
        "worldspawn's '%1' property lists the game's material collections (e.g. WAD "
        "files); TrenchBroom adds it empty to every new map and fills it when "
        "collections are added. Harmless, leave it as it is"}
        .arg(toUnicode(map, propertyIssue->propertyKey()));
    }
    return std::nullopt;
  }

  if (dynamic_cast<const mdl::MissingDefinitionValidator*>(validator))
  {
    // only with the game's own definition file, and only if it was loaded: otherwise
    // even worldspawn has no definition, and the reason would be wrong
    const auto& defFilePaths = gameConfig.entityConfig.defFilePaths;
    const auto spec = mdl::entityDefinitionFile(map);
    if (
      gameConfig.name == "Generic" && spec && !defFilePaths.empty()
      && *spec == mdl::EntityDefinitionFileSpec::makeBuiltin(defFilePaths.front())
      && map.entityDefinitionManager().definition("worldspawn") != nullptr)
    {
      return mcpGenericEntityDefinitionsNote();
    }
  }
  return std::nullopt;
}

bool addExpectedIssueFields(
  QJsonObject& item,
  const mdl::Map& map,
  const std::vector<const mdl::Validator*>& validators,
  const mdl::Issue& issue)
{
  if (const auto reason = expectedIssueReason(map, validators, issue))
  {
    item["expected_for_new_map"] = true;
    item["expected_reason"] = *reason;
    return true;
  }
  return false;
}

} // namespace tb::ui
