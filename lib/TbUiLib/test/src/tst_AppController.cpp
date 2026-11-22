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

#include <QApplication>
#include <QWidget>

#include "AppTestUtils.h"
#include "TestEnvironment.h"
#include "base/Result.h"
#include "fs/TestEnvironment.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/WorldNode.h"
#include "ui/AppController.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/WelcomeWindow.h"

#include "kd/result.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

const auto GameName = std::string{"Test"};

// The Valve format has an initial map which the test overwrites with garbage so that
// creating a Valve document fails.
const auto GameConfig = R"({
    "version": 9,
    "name": "Test",
    "icon": "Icon.png",
    "fileformats": [
        { "format": "Standard" },
        { "format": "Valve", "initialmap": "initial_valve.map" }
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

const auto MalformedMap = std::string{"{\n\"classname\" \"worldspawn\"\n{ ( 0 0 0 ) (\n"};

void writeFile(const std::filesystem::path& path, const std::string& contents)
{
  auto stream = std::ofstream{path, std::ios::out | std::ios::trunc};
  stream << contents;
}

/**
 * Closes all map windows that were opened during a test before the app controller is
 * destroyed. The documents are never modified, so closing them shows no dialog.
 */
class CloseMapWindows
{
private:
  AppController& m_appController;

public:
  explicit CloseMapWindows(AppController& appController)
    : m_appController{appController}
  {
  }

  ~CloseMapWindows()
  {
    for (auto* mapWindow : m_appController.mapWindowManager().mapWindows())
    {
      mapWindow->close();
    }
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
};

} // namespace

TEST_CASE("AppController")
{
  auto appControllerFixture = AppControllerFixture{[](const auto& writeGameConfig) {
    writeGameConfig(GameName, GameConfig, std::nullopt, std::nullopt);
  }};
  auto& appController = appControllerFixture.appController();
  auto closeMapWindows = CloseMapWindows{appController};

  const auto* gameInfo = appController.gameManager().gameInfo(GameName);
  REQUIRE(gameInfo != nullptr);

  auto& mapWindowManager = appController.mapWindowManager();
  REQUIRE(mapWindowManager.allMapWindowsClosed());

  auto testEnvironment = fs::TestEnvironment{};

  appController.showWelcomeWindow();
  REQUIRE(isWelcomeWindowVisible());

  auto watchdog = ModalWatchdog{};

  SECTION("newDocument")
  {
    SECTION("still asks for the game and format in a modal dialog")
    {
      // The watchdog rejects the dialog, so no document is created
      CHECK(!appController.newDocument());
      CHECK(watchdog.modalCount() == 1);

      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }
  }

  SECTION("createDocument")
  {
    SECTION("creates a document in a new map window and closes the welcome window")
    {
      CHECK(appController.createDocument(*gameInfo, mdl::MapFormat::Standard));

      const auto mapWindows = mapWindowManager.mapWindows();
      REQUIRE(mapWindows.size() == 1);
      CHECK(mapWindowManager.topMapWindow() == mapWindows.front());

      const auto& map = mapWindows.front()->document().map();
      CHECK(map.gameInfo().gameConfig.name == GameName);
      CHECK(map.worldNode().mapFormat() == mdl::MapFormat::Standard);
      CHECK(!map.modified());
      CHECK(map.persistent() == false);

      CHECK(!isWelcomeWindowVisible());
    }

    SECTION("creates another map window on each call")
    {
      REQUIRE(appController.createDocument(*gameInfo, mdl::MapFormat::Standard));
      const auto* firstWindow = mapWindowManager.topMapWindow();

      REQUIRE(appController.createDocument(*gameInfo, mdl::MapFormat::Standard));
      CHECK(mapWindowManager.mapWindows().size() == 2);
      CHECK(mapWindowManager.topMapWindow() != firstWindow);
    }

    SECTION("returns an error if the format is unknown")
    {
      // A world with an unknown format would later violate a contract in
      // StandardMapParser, e.g. when pasting into it
      const auto result =
        appController.createDocument(*gameInfo, mdl::MapFormat::Unknown);
      CHECK(!result);

      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }

    SECTION("returns an error and keeps the welcome window if creation fails")
    {
      const auto initialMapPath =
        gameInfo->gameConfig.path.parent_path() / "initial_valve.map";
      writeFile(initialMapPath, MalformedMap);

      const auto result = appController.createDocument(*gameInfo, mdl::MapFormat::Valve);
      CHECK(!result);

      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }

    CHECK(watchdog.modalCount() == 0);
  }

  SECTION("loadDocument")
  {
    const auto fixturePath = std::filesystem::absolute(
                               getFixtureRoot() / "test/ui/MapDocument/emptyValveMap.map")
                               .lexically_normal();
    REQUIRE(std::filesystem::is_regular_file(fixturePath));

    SECTION("loads a document in a new map window and closes the welcome window")
    {
      CHECK(appController.loadDocument(*gameInfo, mdl::MapFormat::Valve, fixturePath));

      const auto mapWindows = mapWindowManager.mapWindows();
      REQUIRE(mapWindows.size() == 1);

      const auto& map = mapWindows.front()->document().map();
      CHECK(map.path() == fixturePath);
      CHECK(map.gameInfo().gameConfig.name == GameName);
      CHECK(map.worldNode().mapFormat() == mdl::MapFormat::Valve);
      CHECK(!map.modified());

      CHECK(!isWelcomeWindowVisible());
    }

    SECTION("lets the map reader choose a format if the format is unknown")
    {
      CHECK(appController.loadDocument(*gameInfo, mdl::MapFormat::Unknown, fixturePath));

      const auto* mapWindow = mapWindowManager.topMapWindow();
      REQUIRE(mapWindow != nullptr);
      CHECK(mapWindow->document().map().path() == fixturePath);
      CHECK(
        mapWindow->document().map().worldNode().mapFormat() != mdl::MapFormat::Unknown);
      CHECK(!isWelcomeWindowVisible());
    }

    SECTION("returns an error if the file does not exist")
    {
      const auto result = appController.loadDocument(
        *gameInfo, mdl::MapFormat::Valve, testEnvironment.dir() / "missing.map");
      CHECK(!result);

      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }

    SECTION("returns an error if the path is relative")
    {
      const auto result = appController.loadDocument(
        *gameInfo,
        mdl::MapFormat::Valve,
        std::filesystem::path{"test/ui/MapDocument/emptyValveMap.map"});
      CHECK(!result);

      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }

    SECTION("returns an error if the file cannot be parsed")
    {
      testEnvironment.createFile("malformed.map", MalformedMap);

      const auto result = appController.loadDocument(
        *gameInfo, mdl::MapFormat::Standard, testEnvironment.dir() / "malformed.map");
      CHECK(!result);

      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }

    SECTION("does not reopen the welcome window once it was closed")
    {
      REQUIRE(appController.loadDocument(*gameInfo, mdl::MapFormat::Valve, fixturePath));
      REQUIRE(!isWelcomeWindowVisible());

      CHECK(!appController.loadDocument(
        *gameInfo, mdl::MapFormat::Valve, testEnvironment.dir() / "missing.map"));
      CHECK(mapWindowManager.mapWindows().size() == 1);
      CHECK(!isWelcomeWindowVisible());
    }

    CHECK(watchdog.modalCount() == 0);
  }

  if (auto* welcomeWindow = findWelcomeWindow())
  {
    welcomeWindow->close();
  }
}

} // namespace tb::ui
