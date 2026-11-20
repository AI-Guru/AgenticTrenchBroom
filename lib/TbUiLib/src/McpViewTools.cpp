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

#include "ui/McpViewTools.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPixmap>
#include <QPointer>
#include <QStringList>

#include "gl/Camera.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/MapView.h"
#include "ui/MapView2D.h"
#include "ui/MapView3D.h"
#include "ui/MapViewBase.h"
#include "ui/MapWindow.h"
#include "ui/McpAppTools.h"
#include "ui/McpJson.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"
#include "ui/QPathUtils.h"

#include "kd/overload.h"
#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace tb::ui
{
namespace
{

const auto ReadOnly =
  mcp::ToolAnnotations{.readOnly = true, .destructive = false, .idempotent = true};
const auto Mutating =
  mcp::ToolAnnotations{.readOnly = false, .destructive = false, .idempotent = false};

const auto ViewNames = QStringList{"3d", "window", "top", "front", "side", "iso"};

constexpr auto MaxFrameIds = 1000;
constexpr size_t MaxCutawayIds = 50;
constexpr auto ImagesKey = "_images";

/** The direction from which the iso view and frame look at their target. */
const auto IsoOffset = vm::normalize(vm::vec3d{-1, -1, 0.7});

QJsonArray toJson(const vm::vec3f& v)
{
  return ui::toJson(vm::vec3d{v});
}

QJsonObject cameraJson(const gl::Camera& camera)
{
  return QJsonObject{
    {"position", toJson(camera.position())},
    {"direction", toJson(camera.direction())},
    {"up", toJson(camera.up())},
  };
}

/** Returns the window's 3D view, preferring a visible one, or nullptr. */
MapView3D* find3DView(MapWindow& window)
{
  const auto views = window.findChildren<MapView3D*>();
  const auto visible =
    std::ranges::find_if(views, [](const auto* view) { return view->isVisible(); });
  return visible != views.end() ? *visible : (views.empty() ? nullptr : views.front());
}

gl::Camera& cameraOf(MapViewBase& view)
{
  // MapView3D::camera is private, but public in MapViewBase
  return view.camera();
}

/** Returns a visible 2D view looking along the given direction, or nullptr. */
MapView2D* findVisible2DView(MapWindow& window, const vm::vec3f& direction)
{
  for (auto* view : window.findChildren<MapView2D*>())
  {
    if (view->isVisible() && vm::is_equal(cameraOf(*view).direction(), direction, 1e-4f))
    {
      return view;
    }
  }
  return nullptr;
}

/** Saves the state of the cameras it touches and restores them unless disabled. */
class CameraRestorer
{
public:
  explicit CameraRestorer(const bool enabled)
    : m_enabled{enabled}
  {
  }

  ~CameraRestorer()
  {
    if (!m_enabled)
    {
      return;
    }
    for (const auto& saved : m_saved)
    {
      if (saved.view)
      {
        auto& camera = cameraOf(*saved.view);
        camera.moveTo(saved.position);
        camera.setDirection(saved.direction, saved.up);
        if (saved.zoom > 0.0f)
        {
          camera.setZoom(saved.zoom);
        }
      }
    }
  }

  CameraRestorer(const CameraRestorer&) = delete;
  CameraRestorer& operator=(const CameraRestorer&) = delete;

  /** Remembers the camera of the given view before it is changed. */
  gl::Camera& save(MapViewBase& view)
  {
    auto& camera = cameraOf(view);
    if (std::ranges::none_of(
          m_saved, [&](const auto& saved) { return saved.view == &view; }))
    {
      m_saved.push_back({
        &view,
        camera.position(),
        camera.direction(),
        camera.up(),
        camera.zoom(),
      });
    }
    return camera;
  }

private:
  struct Saved
  {
    QPointer<MapViewBase> view;
    vm::vec3f position;
    vm::vec3f direction;
    vm::vec3f up;
    float zoom;
  };

  bool m_enabled;
  std::vector<Saved> m_saved;
};

/** A small box around the origin, framed when there is nothing else to frame. */
const auto EmptyFrame = vm::bbox3d{64.0};

/** The merged bounds of the given nodes (see nodeBounds), or nullopt if they have none.
 */
std::optional<vm::bbox3d> contentBounds(const std::vector<const mdl::Node*>& nodes)
{
  auto result = std::optional<vm::bbox3d>{};
  for (const auto* node : nodes)
  {
    if (const auto bounds = nodeBounds(*node))
    {
      result = result ? vm::merge(*result, *bounds) : *bounds;
    }
  }
  return result;
}

/** The bounds of everything in the map, or a small box around the origin if empty. */
vm::bbox3d contentBounds(const mdl::Map& map)
{
  return contentBounds(std::vector<const mdl::Node*>{&map.worldNode()})
    .value_or(EmptyFrame);
}

/** Places a perspective camera so that the given bounds fit into the view. */
void framePerspective(
  gl::Camera& camera,
  const float fov,
  const vm::bbox3d& frame,
  const vm::vec3d& direction,
  const vm::vec3d& up)
{
  const auto center = frame.center();
  const auto radius = std::max(vm::length(frame.size()) / 2.0, 1.0);

  // vm::perspective_matrix scales the vertical frustum by 0.75 and derives the horizontal
  // one from the aspect ratio, so fit the bounding sphere into the narrower of the two
  const auto& viewport = camera.viewport();
  const auto aspect = viewport.width > 0 && viewport.height > 0
                        ? double(viewport.width) / double(viewport.height)
                        : 1.0;
  const auto halfFov = vm::to_radians(double(std::clamp(fov, 1.0f, 150.0f))) / 2.0;
  const auto halfAngle = std::atan(std::tan(halfFov) * 0.75 * std::min(aspect, 1.0));
  const auto distance = radius / std::sin(halfAngle) * 1.1;
  camera.moveTo(vm::vec3f{center - direction * distance});
  camera.setDirection(vm::vec3f{direction}, vm::vec3f{up});
}

/** Centers an orthographic camera on the given bounds and zooms so that they fit. */
void frameOrthographic(gl::Camera& camera, const vm::bbox3d& frame)
{
  const auto center = vm::vec3f{frame.center()};
  const auto& direction = camera.direction();
  // keep the camera's depth so that its near and far planes stay as they are
  const auto depth = vm::dot(camera.position(), direction);
  camera.moveTo(center + direction * (depth - vm::dot(center, direction)));

  const auto size = vm::vec3f{frame.size()};
  const auto width = std::abs(vm::dot(size, camera.right()));
  const auto height = std::abs(vm::dot(size, camera.up()));
  const auto& viewport = camera.viewport();
  if (viewport.width > 0 && viewport.height > 0 && width > 0.0f && height > 0.0f)
  {
    const auto zoom =
      std::min(float(viewport.width) / width, float(viewport.height) / height) * 0.9f;
    camera.setZoom(std::clamp(zoom, 0.02f, 100.0f));
  }
}

struct ViewSetup
{
  /** The view to grab, or nullptr to grab the whole window. */
  QPointer<MapViewBase> view;
  QString projection;
  std::vector<QString> warnings;
};

struct AxisView
{
  vm::vec3d direction;
  vm::vec3d up;
};

std::optional<AxisView> axisView(const QString& viewName)
{
  if (viewName == u"top")
  {
    return AxisView{{0, 0, -1}, {0, 1, 0}};
  }
  if (viewName == u"front")
  {
    return AxisView{{0, 1, 0}, {0, 0, 1}};
  }
  if (viewName == u"side")
  {
    return AxisView{{-1, 0, 0}, {0, 0, 1}};
  }
  if (viewName == u"iso")
  {
    return AxisView{-IsoOffset, {0, 0, 1}};
  }
  return std::nullopt;
}

/** Points the camera of a suitable view for the given view name at the frame. */
McpResult<ViewSetup> setUpView(
  MapWindow& window,
  const mdl::Map& map,
  const QString& viewName,
  const std::optional<vm::bbox3d>& frame,
  CameraRestorer& restorer)
{
  auto* view3D = find3DView(window);

  if (const auto axis = axisView(viewName))
  {
    const auto bounds = frame ? *frame : contentBounds(map);
    if (viewName != u"iso")
    {
      if (auto* view2D = findVisible2DView(window, vm::vec3f{axis->direction}))
      {
        frameOrthographic(restorer.save(*view2D), bounds);
        return ViewSetup{view2D, "orthographic", {}};
      }
    }
    if (!view3D)
    {
      return toolError(
        "view_unavailable", "the map window has no 3D view", "use view \"window\"");
    }
    framePerspective(
      restorer.save(*view3D),
      view3D->perspectiveCamera().zoomedFov(),
      bounds,
      axis->direction,
      axis->up);
    auto warnings = std::vector<QString>{};
    if (!view3D->isVisible())
    {
      warnings.push_back("the 3D view is not visible; captured the whole window");
      return ViewSetup{nullptr, "window", std::move(warnings)};
    }
    return ViewSetup{
      view3D, viewName == u"iso" ? "perspective" : "perspective approximation", {}};
  }

  if (frame && view3D)
  {
    auto& camera = restorer.save(*view3D);
    framePerspective(
      camera,
      view3D->perspectiveCamera().zoomedFov(),
      *frame,
      vm::vec3d{camera.direction()},
      vm::vec3d{camera.up()});
  }

  if (viewName == u"window")
  {
    return ViewSetup{nullptr, "window", {}};
  }

  // "3d"
  if (!view3D)
  {
    return toolError(
      "view_unavailable", "the map window has no 3D view", "use view \"window\"");
  }
  if (!view3D->isVisible())
  {
    return ViewSetup{
      nullptr, "window", {"the 3D view is not visible; captured the whole window"}};
  }
  return ViewSetup{view3D, "perspective", {}};
}

// cutaway

/** The test hook that grab calls before rendering. */
std::function<void(MapWindow&)>& grabHook()
{
  static auto hook = std::function<void(MapWindow&)>{};
  return hook;
}

/** Tolerance for comparing brush bounds in the cutaway rules. */
constexpr auto CutawayEpsilon = 1e-3;

/** The minimum free height below a ceiling. */
constexpr auto CeilingClearance = 1.0;

struct CutawayArgs
{
  std::optional<double> aboveZ;
  std::vector<mdl::Node*> hideNodes;
  std::optional<vm::bbox3d> clipBounds;
  bool automatic = false;
};

struct Cutaway
{
  /** The nodes to hide while rendering. */
  std::unordered_set<const mdl::Node*> nodes;
  /** The brushes, patches and point entities that were visible and are hidden. */
  std::vector<const mdl::Node*> objects;
  /** The z threshold that auto used, if auto was given. */
  std::optional<double> autoAboveZ;
};

McpResult<std::optional<CutawayArgs>> cutawayArgument(
  McpDocumentState& doc, const QJsonObject& args)
{
  if (!args.contains("cutaway"))
  {
    return std::optional<CutawayArgs>{};
  }

  const auto json = args.value("cutaway").toObject();
  auto result = CutawayArgs{};
  if (json.contains("above_z"))
  {
    result.aboveZ = json.value("above_z").toDouble();
  }
  if (json.contains("clip_bounds"))
  {
    auto clip = bounds(json.value("clip_bounds"), "/cutaway/clip_bounds");
    if (clip.is_error())
    {
      return errorOf(clip);
    }
    result.clipBounds = clip.value();
  }
  if (json.contains("hide_ids"))
  {
    doc.nodes.reconcile();
    auto nodes = resolveNodes(doc, json.value("hide_ids").toArray(), MaxFrameIds);
    if (nodes.is_error())
    {
      return errorOf(nodes);
    }
    for (const auto* node : nodes.value())
    {
      if (node == &doc.map.worldNode())
      {
        return toolError(
          "wrong_node_type",
          "the world cannot be hidden",
          "pass the ids of the nodes to hide, or use above_z, clip_bounds or auto",
          {{"id", "world"},
           {"pointer", "/cutaway/hide_ids"},
           {"expected", "layer, group, entity, brush or patch"}});
      }
    }
    result.hideNodes = std::move(nodes).value();
  }
  result.automatic = json.value("auto").toBool(false);

  if (
    !result.aboveZ && !result.clipBounds && result.hideNodes.empty() && !result.automatic)
  {
    return toolError(
      "invalid_argument",
      "the cutaway hides nothing",
      "pass above_z, hide_ids, clip_bounds or auto:true",
      {{"pointer", "/cutaway"}});
  }
  return std::optional{std::move(result)};
}

bool isWorldBrush(const mdl::BrushNode& brushNode)
{
  return dynamic_cast<const mdl::WorldNode*>(brushNode.entity()) != nullptr;
}

/** Whether the x/y extents of the boxes overlap by more than CutawayEpsilon. */
bool overlapXY(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  for (size_t i = 0; i < 2; ++i)
  {
    if (
      std::min(lhs.max[i], rhs.max[i]) - std::max(lhs.min[i], rhs.min[i])
      <= CutawayEpsilon)
    {
      return false;
    }
  }
  return true;
}

/** Whether the boxes intersect or touch. */
bool touches(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  for (size_t i = 0; i < 3; ++i)
  {
    if (lhs.max[i] < rhs.min[i] - 1e-6 || rhs.max[i] < lhs.min[i] - 1e-6)
    {
      return false;
    }
  }
  return true;
}

/**
 * Whether the brush is a ceiling: no other brush rests on it (overlaps its x/y extent
 * and starts at its top), something lies below it, and at most two of five sample
 * points of its x/y extent (the center and the points at a quarter and three quarters
 * of both extents) are supported, i.e. have the highest top of a brush that contains
 * the point in x/y and lies below the brush less than CeilingClearance below it. A
 * brush with nothing below it is not a ceiling, so that a lone floor slab stays; a
 * single pillar or partition wall under a ceiling does not keep it. Brushes far above
 * (such as the top of a box that seals the map) do not keep a ceiling either.
 */
bool isCeiling(const vm::bbox3d& brush, const std::vector<vm::bbox3d>& others)
{
  const auto lerp = [](const double min, const double max, const double t) {
    return min + (max - min) * t;
  };
  const auto& mn = brush.min;
  const auto& mx = brush.max;
  const auto samples = std::array<vm::vec2d, 5>{
    vm::vec2d{lerp(mn.x(), mx.x(), 0.5), lerp(mn.y(), mx.y(), 0.5)},
    vm::vec2d{lerp(mn.x(), mx.x(), 0.25), lerp(mn.y(), mx.y(), 0.25)},
    vm::vec2d{lerp(mn.x(), mx.x(), 0.75), lerp(mn.y(), mx.y(), 0.25)},
    vm::vec2d{lerp(mn.x(), mx.x(), 0.25), lerp(mn.y(), mx.y(), 0.75)},
    vm::vec2d{lerp(mn.x(), mx.x(), 0.75), lerp(mn.y(), mx.y(), 0.75)},
  };
  auto floors = std::array<std::optional<double>, 5>{};
  for (const auto& other : others)
  {
    if (&other == &brush)
    {
      continue;
    }
    if (
      overlapXY(brush, other)
      && std::abs(other.min.z() - brush.max.z()) <= CutawayEpsilon)
    {
      return false;
    }
    if (other.max.z() > brush.min.z() + CutawayEpsilon)
    {
      continue;
    }
    for (size_t i = 0; i < samples.size(); ++i)
    {
      const auto& p = samples[i];
      if (
        other.min.x() <= p.x() && p.x() <= other.max.x() && other.min.y() <= p.y()
        && p.y() <= other.max.y())
      {
        floors[i] = std::max(floors[i].value_or(other.max.z()), other.max.z());
      }
    }
  }
  const auto anyBelow =
    std::ranges::any_of(floors, [](const auto& floor) { return floor.has_value(); });
  const auto supported = std::ranges::count_if(floors, [&](const auto& floor) {
    return floor && brush.min.z() - *floor < CeilingClearance;
  });
  return anyBelow && supported <= 2;
}

Cutaway computeCutaway(
  const mdl::Map& map, const CutawayArgs& args, const vm::bbox3d& frame)
{
  const auto& editorContext = map.editorContext();

  // the visible world brushes in DFS order
  auto worldBrushes = std::vector<const mdl::BrushNode*>{};
  auto allNodes = std::vector<std::tuple<const mdl::Node*, bool>>{};
  const auto hidden =
    std::unordered_set<const mdl::Node*>{args.hideNodes.begin(), args.hideNodes.end()};
  const auto visit =
    [&](const auto& self, const mdl::Node& node, const bool inHidden) -> void {
    const auto hide = inHidden || hidden.contains(&node);
    allNodes.emplace_back(&node, hide);
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      if (isWorldBrush(*brushNode) && editorContext.visible(*brushNode))
      {
        worldBrushes.push_back(brushNode);
      }
    }
    for (const auto* child : node.children())
    {
      self(self, *child, hide);
    }
  };
  visit(visit, map.worldNode(), false);

  auto result = Cutaway{};
  if (args.automatic)
  {
    result.autoAboveZ = frame.center().z();
  }

  auto ruleHidden = std::unordered_set<const mdl::Node*>{};
  auto boundsList = std::vector<vm::bbox3d>{};
  boundsList.reserve(worldBrushes.size());
  for (const auto* brushNode : worldBrushes)
  {
    boundsList.push_back(brushNode->logicalBounds());
  }
  for (size_t i = 0; i < worldBrushes.size(); ++i)
  {
    const auto& b = boundsList[i];
    const auto hide = (args.aboveZ && b.min.z() >= *args.aboveZ - 1e-6)
                      || (args.clipBounds && !touches(b, *args.clipBounds))
                      || (result.autoAboveZ && b.min.z() >= *result.autoAboveZ - 1e-6)
                      || (args.automatic && isCeiling(b, boundsList));
    if (hide)
    {
      ruleHidden.insert(worldBrushes[i]);
    }
  }

  for (const auto& [node, inHidden] : allNodes)
  {
    if (!inHidden && !ruleHidden.contains(node))
    {
      continue;
    }
    const auto isObject = node->accept(kdl::overload(
      [](const mdl::WorldNode&) { return false; },
      [](const mdl::LayerNode&) { return false; },
      [](const mdl::GroupNode&) { return false; },
      [](const mdl::EntityNode& entityNode) { return !entityNode.hasChildren(); },
      [](const mdl::BrushNode&) { return true; },
      [](const mdl::PatchNode&) { return true; }));
    if (isObject && editorContext.visible(*node))
    {
      result.objects.push_back(node);
    }
    result.nodes.insert(node);
  }
  return result;
}

/**
 * Hides nodes in the editor context of the window's map while it exists, without
 * changing the document, and restores the previous set afterwards, also on errors.
 */
class TemporaryHide
{
public:
  TemporaryHide(MapWindow& window, std::unordered_set<const mdl::Node*> nodes)
    : m_window{&window}
    , m_map{&window.document().map()}
    , m_previous{m_map->editorContext().temporarilyHiddenNodes()}
  {
    if (!nodes.empty())
    {
      auto merged = m_previous;
      merged.insert(nodes.begin(), nodes.end());
      m_map->editorContext().setTemporarilyHiddenNodes(std::move(merged));
      m_active = true;
    }
  }

  ~TemporaryHide()
  {
    // the map is only accessed if the window still shows the same map
    if (m_active && m_window && &m_window->document().map() == m_map)
    {
      // notifies the renderers, which then redraw everything
      m_map->editorContext().setTemporarilyHiddenNodes(std::move(m_previous));
      for (auto* view : m_window->findChildren<MapViewBase*>())
      {
        view->update();
      }
    }
  }

  TemporaryHide(const TemporaryHide&) = delete;
  TemporaryHide& operator=(const TemporaryHide&) = delete;

private:
  QPointer<MapWindow> m_window;
  mdl::Map* m_map;
  std::unordered_set<const mdl::Node*> m_previous;
  bool m_active = false;
};

QJsonObject cutawayJson(const McpDocumentState& doc, const Cutaway& cutaway)
{
  auto ids = QJsonArray{};
  for (size_t i = 0; i < cutaway.objects.size() && i < MaxCutawayIds; ++i)
  {
    ids.append(doc.nodes.idOf(*cutaway.objects[i]));
  }
  auto result = QJsonObject{
    {"hidden_count", qint64(cutaway.objects.size())},
    {"hidden_ids", ids},
  };
  if (cutaway.autoAboveZ)
  {
    result["auto_above_z"] = canon(*cutaway.autoAboveZ);
  }
  return result;
}

/** Computes the cutaway for the given frame, or nothing if no cutaway was requested. */
std::optional<Cutaway> cutawayFor(
  const mdl::Map& map,
  const std::optional<CutawayArgs>& args,
  const std::optional<vm::bbox3d>& frame)
{
  return args ? std::optional{computeCutaway(
                  map, *args, frame ? *frame : contentBounds(map))}
              : std::nullopt;
}

const auto NothingHiddenWarning =
  QStringLiteral("the cutaway hid nothing; check above_z, hide_ids and clip_bounds");

/** Whether the platform can render, i.e. is not a headless test platform. */
bool canRender()
{
  const auto platform = QGuiApplication::platformName();
  return platform != u"offscreen" && platform != u"minimal";
}

/** Renders the given view, or the whole window if view is null. */
QImage grab(MapWindow& window, const QPointer<MapViewBase>& view)
{
  if (const auto& hook = grabHook())
  {
    hook(window);
  }
  if (!canRender())
  {
    return {};
  }

  auto windowPointer = QPointer<MapWindow>{&window};
  window.raise();
  window.activateWindow();
  // let the window be exposed and laid out before it is rendered
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  if (!windowPointer)
  {
    return {};
  }

  if (view)
  {
    view->update();
    return view->grabFramebuffer();
  }
  return window.grab().toImage();
}

/** Whether every pixel of the image has the same color. */
bool isUniform(const QImage& image)
{
  if (image.isNull() || image.width() == 0 || image.height() == 0)
  {
    return true;
  }
  const auto first = image.pixel(0, 0);
  for (int y = 0; y < image.height(); ++y)
  {
    const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    if (std::any_of(line, line + image.width(), [&](const auto p) { return p != first; }))
    {
      return false;
    }
  }
  return true;
}

QByteArray encodePng(const QImage& image)
{
  auto bytes = QByteArray{};
  auto buffer = QBuffer{&bytes};
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

struct Shot
{
  QImage image;
  QString view;
  QString projection;
  QJsonObject camera;
  std::vector<QString> warnings;
};

McpResult<Shot> takeShot(
  MapWindow& window,
  const mdl::Map& map,
  const QString& viewName,
  const std::optional<vm::bbox3d>& frame,
  const int maxWidth,
  CameraRestorer& restorer)
{
  if (!window.isVisible() || window.isMinimized())
  {
    return toolError(
      "view_unavailable",
      "the map window is not visible",
      "ask the user to show the TrenchBroom window");
  }

  return setUpView(window, map, viewName, frame, restorer)
         | kdl::and_then([&](ViewSetup setup) -> McpResult<Shot> {
             auto* view3D = find3DView(window);
             const auto camera = setup.view ? cameraJson(cameraOf(*setup.view))
                                 : view3D   ? cameraJson(cameraOf(*view3D))
                                            : QJsonObject{};

             auto image = grab(window, setup.view);
             if (image.isNull())
             {
               return toolError(
                 "render_failed",
                 "the view could not be rendered",
                 "the TrenchBroom window must be visible on screen; screenshots do not "
                 "work on headless platforms");
             }

             image = image.convertToFormat(QImage::Format_RGB32);
             if (image.width() > maxWidth)
             {
               image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);
             }
             if (isUniform(image))
             {
               return toolError(
                 "render_failed",
                 "the rendered image has only one color",
                 "make sure the TrenchBroom window is visible and not covered or "
                 "minimized, and that the camera looks at the map");
             }

             return Shot{
               std::move(image),
               viewName,
               setup.projection,
               camera,
               std::move(setup.warnings),
             };
           });
}

McpResult<std::optional<vm::bbox3d>> frameArgument(
  McpDocumentState& doc, const QJsonObject& args)
{
  if (args.contains("frame_ids") && args.contains("frame_bounds"))
  {
    return toolError(
      "invalid_argument",
      "pass at most one of frame_ids and frame_bounds",
      {},
      {{"pointer", "/frame_bounds"}});
  }

  if (args.contains("frame_bounds"))
  {
    return bounds(args.value("frame_bounds"), "/frame_bounds")
           | kdl::transform([](const auto& b) { return std::optional{b}; });
  }

  if (args.contains("frame_ids"))
  {
    doc.nodes.reconcile();
    return resolveNodes(doc, args.value("frame_ids").toArray(), MaxFrameIds)
           | kdl::transform([](const std::vector<mdl::Node*>& nodes) {
               const auto constNodes =
                 std::vector<const mdl::Node*>{nodes.begin(), nodes.end()};
               return std::optional{contentBounds(constNodes).value_or(EmptyFrame)};
             });
  }

  return std::optional<vm::bbox3d>{};
}

QJsonArray warningsJson(const std::vector<QString>& warnings)
{
  auto result = QJsonArray{};
  for (const auto& warning : warnings)
  {
    result.append(warning);
  }
  return result;
}

/** Moves the image data out of the structured result into image content. */
QJsonArray imageContent(QJsonObject& structured)
{
  auto result = QJsonArray{};
  for (const auto& data : structured.value(ImagesKey).toArray())
  {
    result.append(QJsonObject{
      {"type", "image"},
      {"data", data},
      {"mimeType", "image/png"},
    });
  }
  structured.remove(ImagesKey);
  return result;
}

McpResult<std::filesystem::path> savePath(const QString& text, const bool overwrite)
{
  const auto path = pathFromQString(text).lexically_normal();
  if (!path.is_absolute() || !text.endsWith(".png", Qt::CaseInsensitive))
  {
    return toolError(
      "invalid_argument",
      "/save_path must be an absolute path ending in .png",
      {},
      {{"pointer", "/save_path"}});
  }
  auto error = std::error_code{};
  if (!std::filesystem::is_directory(path.parent_path(), error))
  {
    return toolError(
      "io_error",
      QString{"the directory %1 does not exist"}.arg(pathAsQString(path.parent_path())));
  }
  if (std::filesystem::is_directory(path, error))
  {
    return toolError("io_error", QString{"%1 is a directory"}.arg(pathAsQString(path)));
  }
  if (!overwrite && std::filesystem::exists(path, error))
  {
    return toolError(
      "file_exists",
      QString{"%1 already exists"}.arg(pathAsQString(path)),
      "pass overwrite:true to replace it, or choose another path",
      {{"path", pathAsGenericQString(path)}, {"pointer", "/save_path"}});
  }
  return path;
}

QJsonObject viewSchema(const QString& description)
{
  return schema::stringEnum(ViewNames, description);
}

void addFrameProperties(QJsonObject& properties)
{
  properties["frame_ids"] = schema::nodeIds(
    MaxFrameIds,
    "Frame the camera on the union of the bounds of these nodes before the shot.");
  properties["frame_bounds"] =
    schema::bounds("Frame the camera on these bounds before the shot.");
  properties["cutaway"] = schema::describe(
    schema::object({
      {"above_z",
       schema::number(
         -1e9,
         1e9,
         "Hide every world brush whose bounds' min z is >= above_z (world units).")},
      {"hide_ids",
       schema::nodeIds(
         MaxFrameIds,
         "Hide these nodes and everything in them (layers, groups, entities, brushes, "
         "patches; not the world).")},
      {"clip_bounds",
       schema::bounds(
         "Hide every world brush whose bounds do not intersect or touch these bounds.")},
      {"auto",
       schema::boolean(
         "Hide every world brush whose bounds' min z is >= the center z of the framed "
         "bounds (frame_ids, frame_bounds, else the whole map), plus every ceiling: a "
         "visible world brush that no other one overlapping it in x/y rests on (starts "
         "within 0.001 units of its top), that has a world brush below at least one "
         "of five points of its bottom (the center and the points at a quarter and "
         "three quarters of its x and y extents), and that is supported (less than 1 "
         "unit of free space down to the next world brush below) at no more than two "
         "of these points, so a pillar or a partition wall below does not keep it. "
         "Walls, floors, steps and boxes standing on a floor stay unless they lie "
         "entirely above the center z.")},
    }),
    "Hide nodes while rendering, to look into sealed rooms; the rules are combined (a "
    "node is hidden if any rule hides it). Only rendering changes: the map, the undo "
    "history, the selection and the visibility of nodes stay as they were. World "
    "brushes include grouped brushes but not brush entities (use hide_ids for "
    "those). The result's cutaway lists hidden_count and hidden_ids (at most 50).");
}

// get_camera

McpToolDef getCameraTool(McpAppHost& host)
{
  return {
    .name = "get_camera",
    .title = "Get Camera",
    .description =
      "Returns the 3D camera of the target map window: position, unit direction and up "
      "vectors, and the field of view in degrees.",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::App,
    .annotations = ReadOnly,
    .runApp = [&host](const QJsonObject&) -> McpResult<QJsonObject> {
      return host.target(McpToolKind::Read)
             | kdl::and_then([&](const McpTarget& target) -> McpResult<QJsonObject> {
                 auto* view = find3DView(*target.window);
                 if (!view)
                 {
                   return toolError("view_unavailable", "the map window has no 3D view");
                 }
                 auto result = cameraJson(cameraOf(*view));
                 result["fov"] = canon(double(view->perspectiveCamera().fov()));
                 return result;
               });
    },
  };
}

// set_camera

McpResult<void> checkNotParallel(const vm::vec3d& direction, const vm::vec3d& up)
{
  if (vm::length(vm::cross(vm::normalize(direction), vm::normalize(up))) < 1e-6)
  {
    return toolError(
      "invalid_argument",
      "the view direction must not be parallel to up",
      "pass another up vector",
      {{"pointer", "/up"}});
  }
  return kdl::void_success;
}

McpResult<vm::vec3d> nonZero(const vm::vec3d& v, const QString& pointer)
{
  if (vm::length(v) < 1e-6)
  {
    return toolError(
      "invalid_argument",
      QString{"%1 must not be zero"}.arg(pointer),
      {},
      {{"pointer", pointer}});
  }
  return v;
}

McpResult<QJsonObject> setCamera(McpAppHost& host, const QJsonObject& args)
{
  const auto modes = int(args.contains("look_at")) + int(args.contains("direction"))
                     + int(args.contains("frame"));
  if (modes != 1)
  {
    return toolError(
      "invalid_argument",
      "pass exactly one of look_at, direction and frame",
      {},
      {{"pointer", ""}});
  }
  if (args.contains("frame") && args.contains("position"))
  {
    return toolError(
      "invalid_argument",
      "frame computes the position; do not pass position",
      {},
      {{"pointer", "/position"}});
  }

  auto upResult =
    args.contains("up")
      ? vec3(args.value("up"), "/up")
          | kdl::and_then([](const vm::vec3d& up) { return nonZero(up, "/up"); })
      : McpResult<vm::vec3d>{vm::vec3d{0, 0, 1}};
  if (upResult.is_error())
  {
    return errorOf(upResult);
  }
  const auto up = vm::normalize(upResult.value());

  auto positionResult = std::optional<vm::vec3d>{};
  if (args.contains("position"))
  {
    auto position = vec3(args.value("position"), "/position");
    if (position.is_error())
    {
      return errorOf(position);
    }
    positionResult = position.value();
  }

  return host.target(McpToolKind::Read)
         | kdl::and_then([&](const McpTarget& target) -> McpResult<QJsonObject> {
             auto* view = find3DView(*target.window);
             if (!view)
             {
               return toolError("view_unavailable", "the map window has no 3D view");
             }
             auto& camera = cameraOf(*view);
             const auto position = positionResult.value_or(vm::vec3d{camera.position()});

             if (args.contains("frame"))
             {
               auto frame = bounds(args.value("frame"), "/frame");
               if (frame.is_error())
               {
                 return errorOf(frame);
               }
               if (const auto check = checkNotParallel(-IsoOffset, up); check.is_error())
               {
                 return errorOf(check);
               }
               framePerspective(
                 camera,
                 view->perspectiveCamera().zoomedFov(),
                 frame.value(),
                 -IsoOffset,
                 up);
             }
             else
             {
               auto direction =
                 args.contains("look_at")
                   ? vec3(args.value("look_at"), "/look_at")
                       | kdl::and_then([&](const vm::vec3d& lookAt) {
                           return nonZero(lookAt - position, "/look_at - position");
                         })
                   : vec3(args.value("direction"), "/direction")
                       | kdl::and_then([&](const vm::vec3d& givenDirection) {
                           return nonZero(givenDirection, "/direction");
                         });
               if (direction.is_error())
               {
                 return errorOf(direction);
               }
               if (const auto check = checkNotParallel(direction.value(), up);
                   check.is_error())
               {
                 return errorOf(check);
               }
               camera.moveTo(vm::vec3f{position});
               camera.setDirection(
                 vm::vec3f{vm::normalize(direction.value())}, vm::vec3f{up});
             }

             auto result = cameraJson(camera);
             result["fov"] = canon(double(view->perspectiveCamera().fov()));
             return result;
           });
}

McpToolDef setCameraTool(McpAppHost& host)
{
  return {
    .name = "set_camera",
    .title = "Set Camera",
    .description =
      "Moves the 3D camera of the target map window without animation. Pass exactly one "
      "of look_at (a point), direction (a vector) or frame (bounds to fit into view, "
      "seen from the -x -y side above). position defaults to the current position; up "
      "defaults to +z. Does not change the map.",
    .inputSchema = schema::object({
      {"position", schema::vec3("Camera position.")},
      {"look_at", schema::vec3("A point to look at.")},
      {"direction", schema::vec3("The view direction.")},
      {"up", schema::vec3("The up vector, [0,0,1] by default.")},
      {"frame", schema::bounds("Bounds to fit into the view.")},
    }),
    .kind = McpToolKind::App,
    .annotations = mcp::ToolAnnotations{.idempotent = true},
    .runApp = [&host](const QJsonObject& args) { return setCamera(host, args); },
  };
}

// focus_nodes

/** Disables command collation for the lifetime of the scope. */
class CollationScope
{
public:
  explicit CollationScope(mdl::Map& map)
    : m_map{map}
    , m_wasEnabled{map.isCommandCollationEnabled()}
  {
    m_map.setIsCommandCollationEnabled(false);
  }

  ~CollationScope() { m_map.setIsCommandCollationEnabled(m_wasEnabled); }

  CollationScope(const CollationScope&) = delete;
  CollationScope& operator=(const CollationScope&) = delete;

private:
  mdl::Map& m_map;
  bool m_wasEnabled;
};

McpToolDef focusNodesTool(McpAppHost& host)
{
  return {
    .name = "focus_nodes",
    .title = "Focus Nodes",
    .description =
      "Selects the given nodes and moves the cameras of the target map window to show "
      "them, to point the user at something. Unlike other tools, the selection is kept. "
      "One undo step.",
    .inputSchema = schema::object({{"ids", schema::nodeIds()}}, {"ids"}),
    .kind = McpToolKind::App,
    .annotations = Mutating,
    .runApp = [&host](const QJsonObject& args) -> McpResult<QJsonObject> {
      return host.target(McpToolKind::Mutate)
             | kdl::and_then([&](const McpTarget& target) -> McpResult<QJsonObject> {
                 auto& doc = *target.doc;
                 auto& map = doc.map;
                 doc.nodes.reconcile();

                 auto targets = resolveTargets(doc, args.value("ids").toArray());
                 if (targets.is_error())
                 {
                   return errorOf(targets);
                 }
                 if (const auto busy = requireNoActiveTransaction(map); busy.is_error())
                 {
                   return errorOf(busy);
                 }

                 {
                   const auto collation = CollationScope{map};
                   auto transaction = mdl::Transaction{map, "MCP: Focus Nodes"};
                   mdl::deselectAll(map);
                   mdl::selectNodes(map, targets.value());
                   if (!transaction.commit())
                   {
                     return toolError(
                       "operation_failed", "the nodes could not be selected");
                   }
                 }

                 for (auto* view : target.window->findChildren<MapViewBase*>())
                 {
                   static_cast<MapView&>(*view).focusCameraOnSelection(false);
                 }

                 auto ids = QJsonArray{};
                 for (const auto* node : targets.value())
                 {
                   ids.append(doc.nodes.idOf(*node));
                 }
                 auto* view3D = find3DView(*target.window);
                 return QJsonObject{
                   {"ids", ids},
                   {"camera",
                    view3D ? QJsonValue{cameraJson(cameraOf(*view3D))} : QJsonValue{}},
                 };
               });
    },
  };
}

// screenshot

McpResult<QJsonObject> screenshot(McpAppHost& host, const QJsonObject& args)
{
  const auto viewName = args.value("view").toString("3d");
  const auto maxWidth = int(sizeArgument(args, "max_width", 1280));

  auto path = std::optional<std::filesystem::path>{};
  if (args.contains("save_path"))
  {
    auto result =
      savePath(args.value("save_path").toString(), args.value("overwrite").toBool(false));
    if (result.is_error())
    {
      return errorOf(result);
    }
    path = std::move(result).value();
  }

  auto cutawayResult = QJsonObject{};
  return host.target(McpToolKind::Read) | kdl::and_then([&](const McpTarget& target) {
           return frameArgument(*target.doc, args)
                  | kdl::and_then([&](const std::optional<vm::bbox3d>& frame) {
                      return cutawayArgument(*target.doc, args)
                             | kdl::and_then(
                               [&](const std::optional<CutawayArgs>& cutawayArgs) {
                                 const auto cutaway =
                                   cutawayFor(target.doc->map, cutawayArgs, frame);
                                 // restore the cameras after the nodes are shown again
                                 auto restorer = CameraRestorer{
                                   args.value("restore_camera").toBool(true)};
                                 const auto hide = TemporaryHide{
                                   *target.window,
                                   cutaway ? cutaway->nodes
                                           : std::unordered_set<const mdl::Node*>{}};
                                 if (cutaway)
                                 {
                                   cutawayResult = cutawayJson(*target.doc, *cutaway);
                                 }
                                 return takeShot(
                                   *target.window,
                                   target.doc->map,
                                   viewName,
                                   frame,
                                   maxWidth,
                                   restorer);
                               });
                    });
         })
         | kdl::and_then([&](Shot shot) -> McpResult<QJsonObject> {
             auto result = QJsonObject{
               {"width", shot.image.width()},
               {"height", shot.image.height()},
               {"view", shot.view},
               {"projection", shot.projection},
               {"camera", shot.camera},
             };
             if (!cutawayResult.isEmpty())
             {
               result["cutaway"] = cutawayResult;
               if (cutawayResult.value("hidden_count").toInteger() == 0)
               {
                 shot.warnings.push_back(NothingHiddenWarning);
               }
             }

             const auto png = encodePng(shot.image);
             if (path)
             {
               auto file = QFile{pathAsQString(*path)};
               if (
                 !file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                 || file.write(png) != png.size())
               {
                 return toolError(
                   "io_error",
                   QString{"the screenshot could not be written to %1"}.arg(
                     pathAsQString(*path)));
               }
               result["saved_path"] = pathAsGenericQString(*path);
             }
             if (!shot.warnings.empty())
             {
               result["warnings"] = warningsJson(shot.warnings);
             }
             result[ImagesKey] = QJsonArray{QString::fromLatin1(png.toBase64())};
             return result;
           });
}

McpToolDef screenshotTool(McpAppHost& host)
{
  auto properties = QJsonObject{
    {"view",
     viewSchema(
       "\"3d\" (default) is the 3D view as the user sees it; \"window\" is the whole "
       "window; \"top\", \"front\" and \"side\" look along -z, +y and -x (orthographic "
       "if such a 2D view is visible, else the 3D camera); \"iso\" looks from -x -y "
       "above.")},
    {"max_width", schema::integer(64, 4096, "Maximum image width; default 1280.")},
    {"save_path",
     QJsonObject{
       {"type", "string"},
       {"minLength", 1},
       {"maxLength", 4096},
       {"pattern", "^.*\\.[pP][nN][gG]$"},
       {"description",
        "Also write the PNG to this absolute path; an existing file is only replaced "
        "with overwrite:true."},
     }},
    {"overwrite",
     schema::boolean("Replace an existing file at save_path; default false.")},
    {"restore_camera",
     schema::boolean(
       "Restore the cameras after the shot; default true. Set false to leave the "
       "framed view for the user.")},
  };
  addFrameProperties(properties);

  return {
    .name = "screenshot",
    .title = "Screenshot",
    .description =
      "Renders the target map window and returns a PNG image. Use frame_ids or "
      "frame_bounds to look at specific nodes. To see inside a closed room, pass "
      "cutaway {auto:true} (or above_z in world units) with view \"top\" or \"iso\" "
      "and frame_ids on the room: it hides the ceilings and everything above the "
      "frame's mid height for this shot only; check cutaway.hidden_count and "
      "hidden_ids in the result to see what was hidden. The window must be visible on "
      "screen. "
      "Screenshots are for showing results; verify geometry with get_brush_geometry, "
      "query tools and validate_map instead.",
    .inputSchema = schema::object(properties),
    .kind = McpToolKind::App,
    // not read-only: it can write a file and, with restore_camera false, move the view
    .annotations = Mutating,
    .runApp = [&host](const QJsonObject& args) { return screenshot(host, args); },
    .extraContent = imageContent,
  };
}

// screenshot_set

McpResult<QJsonObject> screenshotSet(McpAppHost& host, const QJsonObject& args)
{
  const auto views = args.contains("views")
                       ? args.value("views").toArray()
                       : QJsonArray{QStringLiteral("iso"), QStringLiteral("top")};
  const auto maxWidth = int(sizeArgument(args, "max_width", 768));

  return host.target(McpToolKind::Read) | kdl::and_then([&](const McpTarget& target) {
           return frameArgument(*target.doc, args)
                  | kdl::and_then(
                    [&](
                      const std::optional<vm::bbox3d>& frame) -> McpResult<QJsonObject> {
                      auto cutawayArgs = cutawayArgument(*target.doc, args);
                      if (cutawayArgs.is_error())
                      {
                        return errorOf(cutawayArgs);
                      }
                      const auto cutaway =
                        cutawayFor(target.doc->map, cutawayArgs.value(), frame);
                      // before rendering, which processes events
                      const auto cutawayResult =
                        cutaway ? cutawayJson(*target.doc, *cutaway) : QJsonObject{};
                      auto restorer = CameraRestorer{true};
                      const auto hide = TemporaryHide{
                        *target.window,
                        cutaway ? cutaway->nodes
                                : std::unordered_set<const mdl::Node*>{}};
                      auto shots = QJsonArray{};
                      auto images = QJsonArray{};
                      auto warnings = std::vector<QString>{};
                      for (const auto& view : views)
                      {
                        auto shot = takeShot(
                          *target.window,
                          target.doc->map,
                          view.toString(),
                          frame,
                          maxWidth,
                          restorer);
                        if (shot.is_error())
                        {
                          return errorOf(shot);
                        }
                        const auto& s = shot.value();
                        shots.append(QJsonObject{
                          {"view", s.view},
                          {"width", s.image.width()},
                          {"height", s.image.height()},
                          {"projection", s.projection},
                          {"camera", s.camera},
                        });
                        images.append(QString::fromLatin1(encodePng(s.image).toBase64()));
                        warnings.insert(
                          warnings.end(), s.warnings.begin(), s.warnings.end());
                      }

                      auto result = QJsonObject{{"shots", shots}, {ImagesKey, images}};
                      if (cutaway)
                      {
                        result["cutaway"] = cutawayResult;
                        if (cutaway->objects.empty())
                        {
                          warnings.push_back(NothingHiddenWarning);
                        }
                      }
                      if (!warnings.empty())
                      {
                        result["warnings"] = warningsJson(warnings);
                      }
                      return result;
                    });
         });
}

McpToolDef screenshotSetTool(McpAppHost& host)
{
  auto properties = QJsonObject{
    {"views",
     schema::array(
       viewSchema({}), 1, 6, "The views to render; default [\"iso\", \"top\"].")},
    {"max_width", schema::integer(64, 4096, "Maximum image width; default 768.")},
  };
  addFrameProperties(properties);

  return {
    .name = "screenshot_set",
    .title = "Screenshot Set",
    .description =
      "Renders several views of the target map window (see screenshot) and returns one "
      "PNG image per view, in order. The cameras are restored afterwards. cutaway (see "
      "screenshot) applies to every view.",
    .inputSchema = schema::object(properties),
    .kind = McpToolKind::App,
    .annotations = ReadOnly,
    .runApp = [&host](const QJsonObject& args) { return screenshotSet(host, args); },
    .extraContent = imageContent,
  };
}

} // namespace

void setMcpScreenshotHookForTesting(std::function<void(MapWindow&)> hook)
{
  grabHook() = std::move(hook);
}

void registerMcpViewTools(McpToolDispatcher& dispatcher, McpAppHost& host)
{
  dispatcher.add(getCameraTool(host));
  dispatcher.add(setCameraTool(host));
  dispatcher.add(focusNodesTool(host));
  dispatcher.add(screenshotTool(host));
  dispatcher.add(screenshotSetTool(host));
}

} // namespace tb::ui
