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

#include "McpAppTestUtils.h"

#include <QApplication>
#include <QWidget>

#include "McpTestUtils.h"
#include "mdl/Map.h"
#include "ui/AppController.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/McpToolDispatcher.h"
#include "ui/QPathUtils.h"
#include "ui/WelcomeWindow.h"

#include <chrono>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

const auto GameConfig = R"({
    "version": 9,
    "name": "Test",
    "icon": "Icon.png",
    "fileformats": [
        { "format": "Standard" },
        { "format": "Valve" }
    ],
    "filesystem": {
        "searchpath": "id1",
        "packageformat": { "extension": "pak", "format": "idpak" }
    },
    "materials": {
        "root": "textures",
        "extensions": [".D"],
        "palette": "gfx/palette.lmp",
        "attribute": "wad"
    },
    "entities": {
        "definitions": [],
        "defaultcolor": "0.6 0.6 0.6 1.0",
        "modelformats": [ "mdl" ]
    },
    "tags": {
        "brush": [],
        "brushface": []
    }
})";

} // namespace

WelcomeWindow* findWelcomeWindow()
{
  for (auto* widget : QApplication::topLevelWidgets())
  {
    if (auto* welcomeWindow = dynamic_cast<WelcomeWindow*>(widget))
    {
      return welcomeWindow;
    }
  }
  return nullptr;
}

bool isWelcomeWindowVisible()
{
  const auto* welcomeWindow = findWelcomeWindow();
  return welcomeWindow != nullptr && welcomeWindow->isVisible();
}

McpModalWatchdog::McpModalWatchdog()
{
  using namespace std::chrono_literals;

  QObject::connect(&m_timer, &QTimer::timeout, [&]() {
    if (auto* modal = QApplication::activeModalWidget())
    {
      ++m_modalCount;
      modal->close();
    }
  });
  m_timer.start(10ms);
}

int McpModalWatchdog::modalCount() const
{
  return m_modalCount;
}

McpAppFixture::McpAppFixture()
  : appControllerFixture{[](const auto& writeGameConfig) {
    writeGameConfig(McpTestGameName, GameConfig, std::nullopt, std::nullopt);
  }}
  , appController{appControllerFixture.appController()}
  , controller{appController}
{
}

McpAppFixture::~McpAppFixture()
{
  closeAllMapWindows();
  if (auto* welcomeWindow = findWelcomeWindow())
  {
    welcomeWindow->close();
  }
}

QJsonObject McpAppFixture::call(const QString& tool, const QJsonObject& args)
{
  return controller.callTool(tool, args);
}

QJsonObject McpAppFixture::call(const QString& tool, const char* jsonArgs)
{
  return call(tool, parseObject(jsonArgs));
}

QJsonObject McpAppFixture::ok(const QString& tool, const QJsonObject& args)
{
  const auto result = call(tool, args);
  INFO(tool.toStdString() << " " << compactJson(args).toStdString());
  INFO(compactJson(result).left(4000).toStdString());
  REQUIRE_FALSE(result.value("isError").toBool());
  return result.value("structuredContent").toObject();
}

QJsonObject McpAppFixture::ok(const QString& tool, const char* jsonArgs)
{
  return ok(tool, parseObject(jsonArgs));
}

QJsonObject McpAppFixture::error(const QString& tool, const QJsonObject& args)
{
  const auto result = call(tool, args);
  INFO(tool.toStdString() << " " << compactJson(args).toStdString());
  INFO(compactJson(result).left(4000).toStdString());
  REQUIRE(result.value("isError").toBool());
  const auto error =
    result.value("structuredContent").toObject().value("error").toObject();
  CHECK(isMcpToolErrorCode(error.value("code").toString()));
  return error;
}

QJsonObject McpAppFixture::error(const QString& tool, const char* jsonArgs)
{
  return error(tool, parseObject(jsonArgs));
}

QString McpAppFixture::err(const QString& tool, const QJsonObject& args)
{
  return error(tool, args).value("code").toString();
}

QString McpAppFixture::err(const QString& tool, const char* jsonArgs)
{
  return err(tool, parseObject(jsonArgs));
}

MapWindow& McpAppFixture::newMap(const QString& format)
{
  ok(
    "new_map",
    QJsonObject{{"game", QString::fromStdString(McpTestGameName)}, {"format", format}});
  auto* window = appController.mapWindowManager().topMapWindow();
  REQUIRE(window != nullptr);
  return *window;
}

void McpAppFixture::closeAllMapWindows()
{
  auto index = 0;
  for (auto* mapWindow : appController.mapWindowManager().mapWindows())
  {
    // closing a modified map would ask whether to save it in a modal dialog
    auto& map = mapWindow->document().map();
    if (map.modified())
    {
      const auto path = pathFromQString(m_cleanupDir.path())
                        / ("cleanup" + std::to_string(index++) + ".map");
      REQUIRE(map.saveAs(path));
    }
    mapWindow->close();
  }
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace tb::ui
