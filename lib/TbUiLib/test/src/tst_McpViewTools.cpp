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
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QTemporaryDir>

#include "McpAppTestUtils.h"
#include "McpTestUtils.h"
#include "mdl/Map.h"
#include "mdl/Selection.h"
#include "mdl/TransactionScope.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"

#include <cmath>

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

} // namespace

TEST_CASE("McpViewTools")
{
  auto f = McpAppFixture{};
  auto watchdog = McpModalWatchdog{};

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
