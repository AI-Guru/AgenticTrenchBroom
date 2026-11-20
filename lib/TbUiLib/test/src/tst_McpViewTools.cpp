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

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QTemporaryDir>

#include "McpAppTestUtils.h"
#include "McpTestUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/McpViewTools.h"

#include "vm/bbox.h"
#include "vm/bbox_io.h" // IWYU pragma: keep

#include <cmath>
#include <functional>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

const auto Box = R"({"bounds":{"min":[0,0,0],"max":[64,64,64]}})";

bool nearlyEqual(
  const QJsonValue& lhs, const QJsonValue& rhs, const double epsilon = 1e-3)
{
  const auto a = lhs.toArray();
  const auto b = rhs.toArray();
  if (a.size() != b.size())
  {
    return false;
  }
  for (qsizetype i = 0; i < a.size(); ++i)
  {
    if (std::abs(a[i].toDouble() - b[i].toDouble()) > epsilon)
    {
      return false;
    }
  }
  return true;
}

bool sameCamera(const QJsonObject& lhs, const QJsonObject& rhs)
{
  return nearlyEqual(lhs.value("position"), rhs.value("position"))
         && nearlyEqual(lhs.value("direction"), rhs.value("direction"))
         && nearlyEqual(lhs.value("up"), rhs.value("up"));
}

double length(const QJsonValue& value)
{
  const auto a = value.toArray();
  return std::sqrt(
    a[0].toDouble() * a[0].toDouble() + a[1].toDouble() * a[1].toDouble()
    + a[2].toDouble() * a[2].toDouble());
}

/** Whether screenshots can be rendered, i.e. the tests do not run headless. */
bool canRender()
{
  const auto platform = QGuiApplication::platformName();
  return platform != "offscreen" && platform != "minimal";
}

QImage decodeImage(const QJsonObject& content)
{
  REQUIRE(content.value("type") == "image");
  REQUIRE(content.value("mimeType") == "image/png");
  const auto png = QByteArray::fromBase64(content.value("data").toString().toLatin1());
  REQUIRE(png.startsWith("\x89PNG"));
  return QImage::fromData(png, "PNG");
}

/** The visibility of every node: its state and whether the editor shows it. */
std::vector<std::tuple<const mdl::Node*, mdl::VisibilityState, bool>> visibility(
  const mdl::Map& map)
{
  auto result = std::vector<std::tuple<const mdl::Node*, mdl::VisibilityState, bool>>{};
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    result.emplace_back(&node, node.visibilityState(), map.editorContext().visible(node));
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, map.worldNode());
  return result;
}

/** Everything a cutaway must leave as it was. */
struct CutawayState
{
  McpMapSnapshot map;
  std::vector<std::tuple<const mdl::Node*, mdl::VisibilityState, bool>> visibility;
  std::unordered_set<const mdl::Node*> temporarilyHidden;
  const mdl::LayerNode* currentLayer;
  const mdl::GroupNode* currentGroup;
  std::string currentMaterial;
  bool canRepeat;

  bool operator==(const CutawayState&) const = default;
};

CutawayState cutawayState(mdl::Map& map)
{
  return {
    snapshot(map),
    visibility(map),
    map.editorContext().temporarilyHiddenNodes(),
    map.editorContext().currentLayer(),
    map.editorContext().currentGroup(),
    map.currentMaterialName(),
    map.canRepeatCommands(),
  };
}

const mdl::BrushNode* findBrush(const mdl::Map& map, const vm::bbox3d& bounds)
{
  const mdl::BrushNode* result = nullptr;
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
        brushNode && brushNode->logicalBounds() == bounds)
    {
      result = brushNode;
    }
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, map.worldNode());
  REQUIRE(result != nullptr);
  return result;
}

/** Records what the editor hides while a screenshot is rendered. */
class ShotRecorder
{
public:
  explicit ShotRecorder(mdl::Map& map)
    : m_map{map}
  {
    setMcpScreenshotHookForTesting([&](MapWindow&) {
      ++shots;
      hidden = m_map.editorContext().temporarilyHiddenNodes();
      stateDuringShot = snapshot(m_map);
      for (const auto& [node, state, visible] : ui::visibility(m_map))
      {
        if (!visible)
        {
          invisible.insert(node);
        }
      }
    });
  }

  ~ShotRecorder() { setMcpScreenshotHookForTesting({}); }

  ShotRecorder(const ShotRecorder&) = delete;
  ShotRecorder& operator=(const ShotRecorder&) = delete;

  void reset()
  {
    shots = 0;
    hidden.clear();
    invisible.clear();
    stateDuringShot.reset();
  }

  int shots = 0;
  std::unordered_set<const mdl::Node*> hidden;
  std::unordered_set<const mdl::Node*> invisible;
  std::optional<McpMapSnapshot> stateDuringShot;

private:
  mdl::Map& m_map;
};

/** The mean squared deviation of the pixel brightness from its mean. */
double variance(const QImage& image)
{
  auto sum = 0.0;
  auto sumSquares = 0.0;
  for (int y = 0; y < image.height(); ++y)
  {
    for (int x = 0; x < image.width(); ++x)
    {
      const auto gray = double(qGray(image.pixel(x, y)));
      sum += gray;
      sumSquares += gray * gray;
    }
  }
  const auto count = double(image.width()) * double(image.height());
  const auto mean = sum / count;
  return sumSquares / count - mean * mean;
}

} // namespace

TEST_CASE("McpViewTools")
{
  auto f = McpAppFixture{};
  auto watchdog = ModalWatchdog{};

  SECTION("get_camera")
  {
    CHECK(f.err("get_camera") == "no_document");

    f.newMap();
    const auto camera = f.ok("get_camera");
    CHECK(camera.value("position").toArray().size() == 3);
    CHECK(std::abs(length(camera.value("direction")) - 1.0) < 1e-3);
    CHECK(std::abs(length(camera.value("up")) - 1.0) < 1e-3);
    CHECK(camera.value("fov").toDouble() > 0.0);
  }

  SECTION("set_camera")
  {
    auto& window = f.newMap();
    const auto* undoName = window.document().map().undoCommandName();

    SECTION("look_at")
    {
      const auto result =
        f.ok("set_camera", R"({"position":[-100,0,0],"look_at":[100,0,0]})");
      CHECK(nearlyEqual(result.value("position"), QJsonArray{-100, 0, 0}));
      CHECK(nearlyEqual(result.value("direction"), QJsonArray{1, 0, 0}));
      CHECK(nearlyEqual(result.value("up"), QJsonArray{0, 0, 1}));
      CHECK(sameCamera(f.ok("get_camera"), result));
    }

    SECTION("direction")
    {
      const auto result = f.ok("set_camera", R"({"direction":[0,10,0],"up":[0,0,5]})");
      CHECK(nearlyEqual(result.value("direction"), QJsonArray{0, 1, 0}));

      // the position is kept
      const auto position = result.value("position");
      const auto moved = f.ok("set_camera", R"({"direction":[1,0,0]})");
      CHECK(nearlyEqual(moved.value("position"), position));
    }

    SECTION("frame")
    {
      const auto result =
        f.ok("set_camera", R"({"frame":{"min":[0,0,0],"max":[64,64,64]}})");
      const auto position = result.value("position").toArray();
      CHECK(position[0].toDouble() < 0.0);
      CHECK(position[1].toDouble() < 0.0);
      CHECK(position[2].toDouble() > 64.0);

      // the camera looks at the center of the bounds
      const auto direction = result.value("direction").toArray();
      const auto t = (32.0 - position[0].toDouble()) / direction[0].toDouble();
      CHECK(std::abs(position[1].toDouble() + t * direction[1].toDouble() - 32.0) < 0.1);
      CHECK(std::abs(position[2].toDouble() + t * direction[2].toDouble() - 32.0) < 0.1);
    }

    SECTION("errors")
    {
      const auto before = f.ok("get_camera");
      CHECK(f.err("set_camera", "{}") == "invalid_argument");
      CHECK(
        f.err("set_camera", R"({"look_at":[0,0,0],"direction":[1,0,0]})")
        == "invalid_argument");
      CHECK(
        f.err(
          "set_camera", R"({"position":[0,0,0],"frame":{"min":[0,0,0],"max":[1,1,1]}})")
        == "invalid_argument");
      CHECK(
        f.err("set_camera", R"({"position":[1,2,3],"look_at":[1,2,3]})")
        == "invalid_argument");
      CHECK(f.err("set_camera", R"({"direction":[0,0,0]})") == "invalid_argument");
      CHECK(f.err("set_camera", R"({"direction":[0,0,-1]})") == "invalid_argument");
      CHECK(
        f.err("set_camera", R"({"direction":[1,0,0],"up":[0,0,0]})")
        == "invalid_argument");
      CHECK(
        f.err("set_camera", R"({"position":[1e9,0,0],"direction":[1,0,0]})")
        == "invalid_argument");
      CHECK(
        f.err("set_camera", R"({"frame":{"min":[1,0,0],"max":[0,1,1]}})")
        == "invalid_argument");
      CHECK(sameCamera(f.ok("get_camera"), before));
    }

    // moving the camera is not an undoable change
    CHECK(window.document().map().undoCommandName() == undoName);
  }

  SECTION("focus_nodes")
  {
    auto& window = f.newMap();
    auto& map = window.document().map();
    const auto box = f.ok("create_box_brush", Box).value("id").toString();

    SECTION("selects the nodes in one undo step and keeps the selection")
    {
      const auto result = f.ok("focus_nodes", QJsonObject{{"ids", QJsonArray{box}}});
      CHECK(result.value("ids") == QJsonArray{box});
      CHECK(result.value("camera").isObject());
      CHECK(f.ok("get_selection").value("nodes") == QJsonArray{box});
      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "MCP: Focus Nodes");
    }

    SECTION("errors leave the selection unchanged")
    {
      CHECK(f.err("focus_nodes", R"({"ids":["B999"]})") == "unknown_node");
      CHECK(f.err("focus_nodes", R"({"ids":["world"]})") == "wrong_node_type");
      CHECK(f.err("focus_nodes", "{}") == "invalid_argument");
      CHECK(map.selection().nodes.empty());

      map.startTransaction("user", mdl::TransactionScope::LongRunning);
      CHECK(f.err("focus_nodes", QJsonObject{{"ids", QJsonArray{box}}}) == "editor_busy");
      map.cancelTransaction();
    }
  }

  SECTION("screenshot")
  {
    CHECK(f.err("screenshot") == "no_document");

    auto& window = f.newMap();
    const auto box = f.ok("create_box_brush", Box).value("id").toString();
    auto dir = QTemporaryDir{};

    SECTION("checks the arguments before rendering")
    {
      CHECK(f.err("screenshot", R"({"view":"bogus"})") == "invalid_argument");
      CHECK(f.err("screenshot", R"({"max_width":10})") == "invalid_argument");
      CHECK(f.err("screenshot", R"({"save_path":"relative.png"})") == "invalid_argument");
      CHECK(f.err("screenshot", R"({"save_path":"/tmp/x.jpg"})") == "invalid_argument");
      CHECK(
        f.err("screenshot", QJsonObject{{"save_path", dir.filePath("missing/x.png")}})
        == "io_error");
      // an existing file is only replaced with overwrite:true
      const auto existing = dir.filePath("exists.png");
      {
        auto file = QFile{existing};
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("notpng");
      }
      CHECK(f.err("screenshot", QJsonObject{{"save_path", existing}}) == "file_exists");
      {
        auto file = QFile{existing};
        REQUIRE(file.open(QIODevice::ReadOnly));
        CHECK(file.readAll() == "notpng");
      }
      CHECK(f.err("screenshot", R"({"frame_ids":["B999"]})") == "unknown_node");
      CHECK(
        f.err(
          "screenshot",
          QJsonObject{
            {"frame_ids", QJsonArray{box}},
            {"frame_bounds", parseObject(R"({"min":[0,0,0],"max":[1,1,1]})")}})
        == "invalid_argument");
    }

    SECTION("returns an image or render_failed and restores the camera")
    {
      const auto before = f.ok("get_camera");
      const auto path = dir.filePath("shot.png");
      const auto result = f.call(
        "screenshot",
        QJsonObject{
          {"view", "iso"},
          {"frame_ids", QJsonArray{box}},
          {"max_width", 320},
          {"save_path", path}});

      const auto structured = result.value("structuredContent").toObject();
      CHECK_FALSE(structured.contains("_images"));
      if (result.value("isError").toBool())
      {
        CHECK(structured.value("error").toObject().value("code") == "render_failed");
        CHECK_FALSE(canRender());
        CHECK_FALSE(QFileInfo::exists(path));
      }
      else
      {
        const auto content = result.value("content").toArray();
        REQUIRE(content.size() == 2);
        const auto image = decodeImage(content[1].toObject());
        CHECK(image.width() <= 320);
        CHECK(image.width() == structured.value("width").toInt());
        CHECK(structured.value("view") == "iso");
        CHECK(structured.value("projection") == "perspective");
        CHECK(QFileInfo::exists(path));
      }
      CHECK(sameCamera(f.ok("get_camera"), before));
    }

    SECTION("keeps the framed camera if restore_camera is false")
    {
      const auto before = f.ok("get_camera");
      f.call(
        "screenshot",
        QJsonObject{
          {"view", "top"}, {"frame_ids", QJsonArray{box}}, {"restore_camera", false}});
      const auto after = f.ok("get_camera");
      CHECK_FALSE(sameCamera(after, before));
      CHECK(nearlyEqual(after.value("direction"), QJsonArray{0, 0, -1}));
    }

    SECTION("frames the contents of the map, the world and layers")
    {
      // the world has no bounds of its own, and a layer's bounds are not updated when
      // nodes are added, so framing must use their contents
      f.ok(
        "create_box_brush", R"({"bounds":{"min":[2000,2000,0],"max":[2064,2064,64]}})");
      // the starter brush and the box span [-64, 2064] in x and y
      const auto center = QJsonArray{1000, 1000};

      for (const auto* args :
           {R"({"view":"top","restore_camera":false})",
            R"({"view":"top","frame_ids":["world"],"restore_camera":false})",
            R"({"view":"top","frame_ids":["default_layer"],"restore_camera":false})"})
      {
        INFO(args);
        f.ok("set_camera", R"({"position":[0,0,0],"direction":[1,0,0]})");
        f.call("screenshot", args);
        const auto position = f.ok("get_camera").value("position").toArray();
        CHECK(nearlyEqual(QJsonArray{position[0], position[1]}, center));
        CHECK(position[2].toDouble() > 64.0);
      }
    }

    SECTION("is refused while the window is hidden")
    {
      window.hide();
      const auto error = f.error("screenshot");
      CHECK(error.value("code") == "view_unavailable");
      window.show();
    }
  }

  SECTION("cutaway")
  {
    auto& window = f.newMap();
    auto& map = window.document().map();

    // a sealed room with a box and an entity inside, next to the starter brush
    const auto room = f.ok(
      "create_room",
      R"({"inner_bounds":{"min":[256,0,0],"max":[512,256,128]},"group_name":"room"})");
    const auto group = room.value("group").toString();
    const auto ceilingId = room.value("ceiling").toString();
    const auto floorId = room.value("floor").toString();
    const auto boxId =
      f.ok("create_box_brush", R"({"bounds":{"min":[352,96,0],"max":[416,160,32]}})")
        .value("id")
        .toString();
    const auto entityId =
      f.ok("create_entity", R"({"classname":"light","origin":[300,40,64]})")
        .value("id")
        .toString();

    const auto* ceiling = findBrush(map, vm::bbox3d{{240, -16, 128}, {528, 272, 144}});
    const auto* floor = findBrush(map, vm::bbox3d{{240, -16, -16}, {528, 272, 0}});
    const auto* box = findBrush(map, vm::bbox3d{{352, 96, 0}, {416, 160, 32}});
    const auto* starter = findBrush(map, vm::bbox3d{{-64, -64, -16}, {64, 64, 16}});
    const auto* groupNode = ceiling->parent();
    REQUIRE(dynamic_cast<const mdl::GroupNode*>(groupNode));
    const mdl::Node* entity = nullptr;
    for (const auto* node : map.worldNode().defaultLayer()->children())
    {
      if (dynamic_cast<const mdl::EntityNode*>(node))
      {
        entity = node;
      }
    }
    REQUIRE(entity != nullptr);

    auto recorder = ShotRecorder{map};
    const auto before = cutawayState(map);

    // calls the tool and checks that nothing changed afterwards
    const auto shoot = [&](const QString& tool, const QJsonObject& args) {
      recorder.reset();
      const auto result = f.call(tool, args);
      CHECK(cutawayState(map) == before);
      if (recorder.stateDuringShot)
      {
        CHECK(*recorder.stateDuringShot == before.map);
      }
      const auto structured = result.value("structuredContent").toObject();
      if (result.value("isError").toBool())
      {
        CHECK(structured.value("error").toObject().value("code") == "render_failed");
        CHECK_FALSE(canRender());
        return QJsonObject{};
      }
      CHECK(canRender());
      return structured;
    };

    const auto top = [&](const char* cutaway) {
      return QJsonObject{
        {"view", "top"},
        {"frame_ids", QJsonArray{group}},
        {"cutaway", parseObject(cutaway)},
      };
    };

    SECTION("auto hides the ceiling and what lies above the frame's center")
    {
      const auto result = shoot("screenshot", top(R"({"auto":true})"));
      CHECK(recorder.shots == 1);
      CHECK(recorder.hidden == std::unordered_set<const mdl::Node*>{ceiling});
      CHECK(recorder.invisible.contains(ceiling));
      CHECK_FALSE(recorder.invisible.contains(floor));
      CHECK_FALSE(recorder.invisible.contains(box));
      CHECK_FALSE(recorder.invisible.contains(entity));
      CHECK_FALSE(recorder.invisible.contains(starter));
      if (!result.isEmpty())
      {
        const auto cutaway = result.value("cutaway").toObject();
        CHECK(cutaway.value("hidden_count").toInt() == 1);
        CHECK(cutaway.value("hidden_ids") == QJsonArray{ceilingId});
        CHECK(cutaway.value("auto_above_z").toDouble() == 64.0);
        CHECK_FALSE(result.contains("warnings"));
      }
    }

    SECTION("auto hides the ceilings of rooms of every height")
    {
      // a low room far below the frame's center keeps its walls, but not its ceiling
      f.ok("create_room", R"({"inner_bounds":{"min":[0,512,0],"max":[128,640,64]}})");
      f.ok("create_box_brush", R"({"bounds":{"min":[0,1024,0],"max":[64,1088,2048]}})");
      const auto* lowCeiling = findBrush(map, vm::bbox3d{{-16, 496, 64}, {144, 656, 80}});
      const auto* lowFloor = findBrush(map, vm::bbox3d{{-16, 496, -16}, {144, 656, 0}});
      const auto after = cutawayState(map);

      recorder.reset();
      f.call("screenshot", R"({"view":"top","cutaway":{"auto":true}})");
      CHECK(cutawayState(map) == after);
      CHECK(recorder.hidden.contains(lowCeiling));
      CHECK(recorder.hidden.contains(ceiling));
      CHECK_FALSE(recorder.hidden.contains(lowFloor));
      CHECK_FALSE(recorder.hidden.contains(box));
      CHECK_FALSE(recorder.hidden.contains(starter));
    }

    SECTION("auto finds ceilings above a pillar and below a far away slab")
    {
      // a low room with a pillar under the middle of its ceiling, and a slab high
      // above all rooms (as in a map sealed in a hollow box)
      f.ok("create_room", R"({"inner_bounds":{"min":[0,512,0],"max":[128,640,64]}})");
      f.ok("create_box_brush", R"({"bounds":{"min":[56,568,0],"max":[72,584,64]}})");
      f.ok(
        "create_box_brush",
        R"({"bounds":{"min":[-512,-512,2048],"max":[1024,1024,2064]}})");
      const auto* lowCeiling = findBrush(map, vm::bbox3d{{-16, 496, 64}, {144, 656, 80}});
      const auto* pillar = findBrush(map, vm::bbox3d{{56, 568, 0}, {72, 584, 64}});
      const auto* hull =
        findBrush(map, vm::bbox3d{{-512, -512, 2048}, {1024, 1024, 2064}});
      const auto after = cutawayState(map);

      recorder.reset();
      f.call("screenshot", R"({"view":"top","cutaway":{"auto":true}})");
      CHECK(cutawayState(map) == after);
      CHECK(recorder.hidden.contains(lowCeiling));
      CHECK(recorder.hidden.contains(ceiling));
      CHECK(recorder.hidden.contains(hull));
      CHECK_FALSE(recorder.hidden.contains(pillar));
      CHECK_FALSE(recorder.hidden.contains(box));
      CHECK_FALSE(recorder.hidden.contains(floor));
      CHECK_FALSE(recorder.hidden.contains(starter));
    }

    SECTION("above_z hides world brushes that start at or above it")
    {
      shoot("screenshot", top(R"({"above_z":128})"));
      CHECK(recorder.hidden == std::unordered_set<const mdl::Node*>{ceiling});

      shoot("screenshot", top(R"({"above_z":-16})"));
      // every world brush, but not the entity
      CHECK(recorder.hidden.size() == 8);
      CHECK(recorder.hidden.contains(starter));
      CHECK(recorder.hidden.contains(floor));
      CHECK_FALSE(recorder.invisible.contains(entity));
    }

    SECTION("hide_ids hides nodes and their contents, even if they are selected")
    {
      f.ok("focus_nodes", QJsonObject{{"ids", QJsonArray{group, entityId}}});
      const auto selected = cutawayState(map);
      REQUIRE(map.selection().nodes.size() == 2);

      recorder.reset();
      const auto result = f.call(
        "screenshot",
        QJsonObject{
          {"view", "top"},
          {"cutaway", QJsonObject{{"hide_ids", QJsonArray{"@selection"}}}}});
      CHECK(cutawayState(map) == selected);
      CHECK(recorder.invisible.contains(ceiling));
      CHECK(recorder.invisible.contains(entity));
      CHECK(recorder.invisible.contains(floor));
      CHECK(recorder.invisible.contains(groupNode));
      CHECK_FALSE(recorder.invisible.contains(box));
      CHECK_FALSE(recorder.invisible.contains(starter));
      if (!result.value("isError").toBool())
      {
        const auto cutaway =
          result.value("structuredContent").toObject().value("cutaway").toObject();
        // the six room brushes and the entity
        CHECK(cutaway.value("hidden_count").toInt() == 7);
      }
    }

    SECTION("clip_bounds hides world brushes outside of the bounds")
    {
      shoot(
        "screenshot", top(R"({"clip_bounds":{"min":[256,0,0],"max":[512,256,128]}})"));
      CHECK(recorder.hidden == std::unordered_set<const mdl::Node*>{starter});
    }

    SECTION("combines the rules")
    {
      shoot(
        "screenshot",
        QJsonObject{
          {"view", "iso"},
          {"cutaway",
           QJsonObject{
             {"above_z", 30},
             {"hide_ids", QJsonArray{entityId}},
             {"clip_bounds", parseObject(R"({"min":[0,0,0],"max":[1,1,1]})")}}}});
      CHECK(recorder.invisible.contains(entity));
      CHECK(recorder.hidden.contains(ceiling));
      CHECK(recorder.hidden.contains(floor));
      CHECK_FALSE(recorder.hidden.contains(starter));
    }

    SECTION("warns if nothing is hidden")
    {
      const auto result = shoot("screenshot", top(R"({"above_z":100000})"));
      CHECK(recorder.hidden.empty());
      if (!result.isEmpty())
      {
        CHECK(result.value("cutaway").toObject().value("hidden_count").toInt() == 0);
        CHECK(result.value("warnings").toArray().size() == 1);
      }
    }

    SECTION("screenshot_set hides the nodes for every view")
    {
      auto hiddenPerShot = std::vector<std::unordered_set<const mdl::Node*>>{};
      setMcpScreenshotHookForTesting([&](MapWindow&) {
        hiddenPerShot.push_back(map.editorContext().temporarilyHiddenNodes());
      });
      const auto result = shoot(
        "screenshot_set",
        QJsonObject{
          {"views", QJsonArray{"top", "iso"}},
          {"frame_ids", QJsonArray{group}},
          {"cutaway", parseObject(R"({"auto":true})")}});
      // offscreen, the first view fails and the second is not rendered
      REQUIRE(!hiddenPerShot.empty());
      CHECK(hiddenPerShot.size() == (canRender() ? 2u : 1u));
      for (const auto& hidden : hiddenPerShot)
      {
        CHECK(hidden == std::unordered_set<const mdl::Node*>{ceiling});
      }
      if (!result.isEmpty())
      {
        CHECK(result.value("cutaway").toObject().value("hidden_count").toInt() == 1);
      }
    }

    SECTION("restores the state when the window is not visible")
    {
      window.hide();
      CHECK(f.err("screenshot", top(R"({"auto":true})")) == "view_unavailable");
      window.show();
      CHECK(cutawayState(map) == before);
    }

    SECTION("checks the arguments before hiding anything")
    {
      for (const auto* cutaway :
           {R"({})",
            R"({"auto":false})",
            R"({"bogus":1})",
            R"({"above_z":"high"})",
            R"({"hide_ids":[]})",
            R"({"hide_ids":["world"]})",
            R"({"clip_bounds":{"min":[1,0,0],"max":[0,1,1]}})"})
      {
        INFO(cutaway);
        recorder.reset();
        CHECK(f.err("screenshot", top(cutaway)) != "render_failed");
        CHECK(
          f.err(
            "screenshot_set",
            QJsonObject{
              {"frame_ids", QJsonArray{group}}, {"cutaway", parseObject(cutaway)}})
          != "render_failed");
        CHECK(recorder.shots == 0);
        CHECK(cutawayState(map) == before);
      }
      CHECK(f.err("screenshot", top(R"({"hide_ids":["world"]})")) == "wrong_node_type");
      CHECK(f.err("screenshot", top(R"({"hide_ids":["B999"]})")) == "unknown_node");
      CHECK(f.err("screenshot", top(R"({})")) == "invalid_argument");
    }

    SECTION("shows the inside of a sealed room")
    {
      if (canRender())
      {
        const auto image = [&](const QJsonObject& args) {
          const auto result = f.call("screenshot", args);
          REQUIRE_FALSE(result.value("isError").toBool());
          return decodeImage(result.value("content").toArray()[1].toObject());
        };
        auto args = QJsonObject{{"view", "top"}, {"frame_ids", QJsonArray{group}}};
        const auto closed = image(args);
        args["cutaway"] = parseObject(R"({"auto":true})");
        const auto open = image(args);
        CHECK(closed != open);
        CHECK(variance(open) > variance(closed));
        CHECK(cutawayState(map) == before);
      }
    }

    CHECK(map.editorContext().temporarilyHiddenNodes().empty());
    CHECK(
      f.ok("get_nodes", QJsonObject{{"ids", QJsonArray{boxId, floorId}}})
        .value("nodes")
        .toArray()
        .size()
      == 2);
  }

  SECTION("screenshot_set")
  {
    f.newMap();
    const auto box = f.ok("create_box_brush", Box).value("id").toString();

    CHECK(f.err("screenshot_set", R"({"views":[]})") == "invalid_argument");
    CHECK(
      f.err("screenshot_set", R"({"views":["3d","3d","3d","3d","3d","3d","3d"]})")
      == "invalid_argument");

    const auto before = f.ok("get_camera");
    const auto result = f.call(
      "screenshot_set",
      QJsonObject{
        {"views", QJsonArray{"iso", "top", "front"}}, {"frame_ids", QJsonArray{box}}});
    const auto structured = result.value("structuredContent").toObject();
    if (result.value("isError").toBool())
    {
      CHECK(structured.value("error").toObject().value("code") == "render_failed");
      CHECK_FALSE(canRender());
    }
    else
    {
      const auto shots = structured.value("shots").toArray();
      REQUIRE(shots.size() == 3);
      CHECK(shots[1].toObject().value("view") == "top");
      const auto content = result.value("content").toArray();
      REQUIRE(content.size() == 4);
      for (qsizetype i = 1; i < 4; ++i)
      {
        CHECK(decodeImage(content[i].toObject()).width() <= 768);
      }
    }
    CHECK(sameCamera(f.ok("get_camera"), before));
  }

  CHECK(watchdog.modalCount() == 0);
}

} // namespace tb::ui
