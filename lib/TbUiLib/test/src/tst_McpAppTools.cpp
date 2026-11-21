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
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTimer>

#include "McpAppTestUtils.h"
#include "McpTestUtils.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/EntityDefinitionFileSpec.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_Assets.h"
#include "ui/AppController.h"
#include "ui/CompilationDialog.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/McpToolDispatcher.h"

#include <algorithm>
#include <fstream>
#include <memory>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

const auto Box = R"({"bounds":{"min":[0,0,0],"max":[16,16,16]}})";

QStringList stringList(const QJsonValue& value)
{
  auto result = QStringList{};
  for (const auto& item : value.toArray())
  {
    result.push_back(item.toString());
  }
  return result;
}

void writeFile(const QString& path, const QByteArray& contents)
{
  auto file = QFile{path};
  REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(contents);
}

QByteArray readFile(const QString& path)
{
  auto file = QFile{path};
  REQUIRE(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

} // namespace

TEST_CASE("McpAppTools")
{
  auto f = McpAppFixture{};
  auto& mapWindowManager = f.appController.mapWindowManager();
  auto dir = QTemporaryDir{};
  REQUIRE(dir.isValid());

  f.appController.showWelcomeWindow();
  REQUIRE(isWelcomeWindowVisible());

  auto watchdog = ModalWatchdog{};

  SECTION("list_games")
  {
    const auto games = f.ok("list_games").value("games").toArray();
    REQUIRE(games.size() == 1);
    const auto game = games.first().toObject();
    CHECK(game.value("name") == "Test");
    CHECK(stringList(game.value("formats")) == QStringList{"Standard", "Valve"});
    CHECK(game.value("experimental") == false);
    CHECK(game.value("game_path_configured") == false);

    CHECK(f.err("list_games", R"({"x":1})") == "invalid_argument");
  }

  SECTION("list_documents")
  {
    CHECK(f.ok("list_documents").value("documents").toArray().isEmpty());

    f.newMap("Standard");
    f.newMap("Valve");

    const auto documents = f.ok("list_documents").value("documents").toArray();
    REQUIRE(documents.size() == 2);

    auto ids = QStringList{};
    auto targets = 0;
    for (const auto& value : documents)
    {
      const auto document = value.toObject();
      ids.push_back(document.value("document").toString());
      CHECK(document.value("game") == "Test");
      CHECK(document.value("persistent") == false);
      CHECK(document.value("modified") == false);
      CHECK(document.value("path").isNull());
      CHECK(!document.value("filename").toString().isEmpty());
      if (document.value("target").toBool())
      {
        ++targets;
        CHECK(document.value("format") == "Valve");
      }
    }
    ids.sort();
    CHECK(ids == QStringList{"d1", "d2"});
    CHECK(targets == 1);
  }

  SECTION("set_active_document")
  {
    auto& first = f.newMap();
    auto& second = f.newMap();
    CHECK(f.ok("get_map_info").value("document") == "d2");

    CHECK(f.ok("set_active_document", R"({"document":"d1"})").value("document") == "d1");
    CHECK(f.ok("get_map_info").value("document") == "d1");

    // map tools act on the pinned document
    f.ok("create_box_brush", Box);
    CHECK(first.document().map().modified());
    CHECK_FALSE(second.document().map().modified());

    CHECK(f.err("set_active_document", R"({"document":"d99"})") == "unknown_document");
    CHECK(f.err("set_active_document", R"({"document":"x"})") == "invalid_argument");
    CHECK(f.err("set_active_document", "{}") == "invalid_argument");
  }

  SECTION("new_map")
  {
    SECTION("creates a map in a new window and makes it the target")
    {
      const auto result = f.ok("new_map", R"({"game":"Test","format":"Valve"})");
      CHECK(result.value("document") == "d1");
      CHECK(result.value("game") == "Test");
      CHECK(result.value("format") == "Valve");
      CHECK(result.value("modified") == false);
      CHECK(result.value("scene").toObject().contains("nodes"));
      CHECK(result.contains("counts"));

      REQUIRE(mapWindowManager.mapWindows().size() == 1);
      CHECK(!isWelcomeWindowVisible());

      f.ok("new_map", R"({"game":"Test"})");
      CHECK(mapWindowManager.mapWindows().size() == 2);
      const auto info = f.ok("get_map_info");
      CHECK(info.value("document") == "d2");
      CHECK(info.value("format") == "Standard");
    }

    SECTION("refuses unknown games and formats without a dialog")
    {
      const auto unknownGame = f.error("new_map", R"({"game":"Quake 9"})");
      CHECK(unknownGame.value("code") == "unknown_game");
      CHECK(
        stringList(unknownGame.value("details").toObject().value("available"))
        == QStringList{"Test"});

      const auto unknownFormat =
        f.error("new_map", R"({"game":"Test","format":"Quake2"})");
      CHECK(unknownFormat.value("code") == "unsupported_format");
      CHECK(
        stringList(unknownFormat.value("details").toObject().value("available"))
        == QStringList{"Standard", "Valve"});
      CHECK(
        f.err("new_map", R"({"game":"Test","format":"Bogus"})") == "unsupported_format");
      CHECK(f.err("new_map", R"({"format":"Valve"})") == "invalid_argument");
      CHECK(f.err("new_map", R"({"game":""})") == "invalid_argument");

      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }
  }

  SECTION("save_map")
  {
    f.newMap();
    const auto path = dir.filePath("saved.map");

    SECTION("needs a path for a new map")
    {
      CHECK(f.err("save_map") == "path_required");
    }

    SECTION("saves to the given path")
    {
      f.ok("create_box_brush", Box);
      const auto result = f.ok("save_map", QJsonObject{{"path", path}});
      CHECK(QFileInfo{result.value("path").toString()} == QFileInfo{path});
      CHECK(result.value("bytes").toInteger() == QFileInfo{path}.size());
      CHECK(result.value("bytes").toInteger() > 0);
      CHECK(readFile(path).contains("// Game: Test"));

      const auto info = f.ok("get_map_info");
      CHECK(info.value("modified") == false);
      CHECK(info.value("persistent") == true);

      // without a path, the map is saved where it was saved before
      f.ok("create_box_brush", R"({"bounds":{"min":[32,0,0],"max":[48,16,16]}})");
      const auto again = f.ok("save_map");
      CHECK(QFileInfo{again.value("path").toString()} == QFileInfo{path});
      CHECK(again.value("bytes").toInteger() > result.value("bytes").toInteger());
    }

    SECTION("refuses to replace a file unless overwrite is set")
    {
      writeFile(path, "existing");
      CHECK(f.err("save_map", QJsonObject{{"path", path}}) == "file_exists");
      CHECK(readFile(path) == "existing");

      f.ok("save_map", QJsonObject{{"path", path}, {"overwrite", true}});
      CHECK(readFile(path) != "existing");
    }

    SECTION("keeps the map modified if the file could not be written")
    {
      f.ok("create_box_brush", Box);
      REQUIRE(QDir{dir.path()}.mkdir("readonly"));
      const auto readOnlyDir = dir.filePath("readonly");
      REQUIRE(QFile::setPermissions(
        readOnlyDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
      if (QFile{readOnlyDir + "/probe"}.open(QIODevice::WriteOnly))
      {
        SKIP("the directory is writable despite its permissions, e.g. as root");
      }

      const auto error =
        f.error("save_map", QJsonObject{{"path", readOnlyDir + "/x.map"}});
      CHECK(error.value("code") == "io_error");

      // Map::saveAs marks the map as saved even though nothing was written
      CHECK(f.ok("get_map_info").value("modified") == true);
      f.ok("undo");
      CHECK(f.ok("get_map_info").value("modified") == true);
      CHECK(f.err("close_map") == "unsaved_changes");

      REQUIRE(QFile::setPermissions(
        readOnlyDir,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
      f.ok("save_map", QJsonObject{{"path", path}});
      CHECK(f.ok("get_map_info").value("modified") == false);
    }

    SECTION("checks the path")
    {
      CHECK(f.err("save_map", R"({"path":"relative.map"})") == "invalid_argument");
      CHECK(
        f.err("save_map", QJsonObject{{"path", dir.filePath("wrong.txt")}})
        == "invalid_argument");
      CHECK(
        f.err("save_map", QJsonObject{{"path", dir.filePath("missing/x.map")}})
        == "io_error");

      REQUIRE(QDir{dir.path()}.mkdir("folder.map"));
      CHECK(
        f.err("save_map", QJsonObject{{"path", dir.filePath("folder.map")}})
        == "io_error");
      CHECK(
        f.err("save_map", QJsonObject{{"path", path}, {"overwrite", "yes"}})
        == "invalid_argument");
    }
  }

  SECTION("open_map")
  {
    const auto path = dir.filePath("open.map");

    SECTION("opens a saved map with the same content")
    {
      f.newMap("Valve");
      f.ok("create_box_brush", Box);
      f.ok(
        "create_entity",
        R"({"classname":"light","origin":[8,8,64],"properties":{"light":200}})");
      f.ok("save_map", QJsonObject{{"path", path}});
      const auto digest = f.ok("get_map_stats").value("content_digest");
      f.ok("close_map");
      REQUIRE(mapWindowManager.allMapWindowsClosed());

      const auto result = f.ok("open_map", QJsonObject{{"path", path}});
      CHECK(result.value("game") == "Test");
      CHECK(result.value("format") == "Valve");
      CHECK(result.value("persistent") == true);
      CHECK(QFileInfo{result.value("path").toString()} == QFileInfo{path});
      CHECK(f.ok("get_map_stats").value("content_digest") == digest);
      CHECK(!isWelcomeWindowVisible());
    }

    SECTION("needs the game if the file does not name it")
    {
      writeFile(path, "{\n\"classname\" \"worldspawn\"\n}\n");
      CHECK(f.err("open_map", QJsonObject{{"path", path}}) == "invalid_argument");
      CHECK(
        f.err("open_map", QJsonObject{{"path", path}, {"game", "Nope"}})
        == "unknown_game");
      CHECK(
        f.err(
          "open_map", QJsonObject{{"path", path}, {"game", "Test"}, {"format", "Bogus"}})
        == "unsupported_format");
      CHECK(mapWindowManager.allMapWindowsClosed());

      f.ok("open_map", QJsonObject{{"path", path}, {"game", "Test"}});
      CHECK(mapWindowManager.mapWindows().size() == 1);
    }

    SECTION("opens a map with any format name without crashing")
    {
      f.newMap();
      f.ok("create_box_brush", Box);
      f.ok("save_map", QJsonObject{{"path", path}});

      for (const auto* format :
           {"Standard",
            "Quake2",
            "Quake2 (Valve)",
            "Valve",
            "Hexen2",
            "Daikatana",
            "Quake3 (legacy)",
            "Quake3 (Valve)",
            "Quake3"})
      {
        INFO(format);
        const auto result =
          f.call("open_map", QJsonObject{{"path", path}, {"format", format}});
        if (result.value("isError").toBool())
        {
          CHECK(isMcpToolErrorCode(result.value("structuredContent")
                                     .toObject()
                                     .value("error")
                                     .toObject()
                                     .value("code")
                                     .toString()));
        }
        else
        {
          f.ok("create_box_brush", R"({"bounds":{"min":[32,0,0],"max":[48,16,16]}})");
          f.ok("export_map_text");
          f.ok("validate_map");
          f.ok("undo");
          f.ok("close_map");
        }
      }
    }

    SECTION("reports missing and malformed files")
    {
      CHECK(
        f.err("open_map", QJsonObject{{"path", dir.filePath("missing.map")}})
        == "io_error");
      CHECK(f.err("open_map", QJsonObject{{"path", dir.path()}}) == "io_error");
      CHECK(f.err("open_map", R"({"path":"relative.map"})") == "invalid_argument");

      writeFile(
        path,
        "// Game: Test\n// Format: Standard\n{\n\"classname\" \"worldspawn\"\n{ ( 0 0 0 "
        ") (\n");
      CHECK(f.err("open_map", QJsonObject{{"path", path}}) == "io_error");
      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }
  }

  SECTION("close_map")
  {
    SECTION("refuses to close a modified map")
    {
      auto& window = f.newMap();
      f.ok("create_box_brush", Box);

      const auto error = f.error("close_map");
      CHECK(error.value("code") == "unsaved_changes");
      CHECK(error.value("hint").toString().contains("discard_changes"));
      CHECK(f.err("close_map", R"({"discard_changes":false})") == "unsaved_changes");
      CHECK(mapWindowManager.mapWindows() == std::vector<MapWindow*>{&window});
      CHECK(window.document().map().modified());
    }

    SECTION("discards unsaved changes without asking")
    {
      auto& window = f.newMap();
      f.ok("create_box_brush", Box);
      const auto path = dir.filePath("saved.map");
      f.ok("save_map", QJsonObject{{"path", path}});
      const auto savedText = readFile(path);

      f.ok("create_box_brush", R"({"bounds":{"min":[32,0,0],"max":[48,16,16]}})");
      REQUIRE(window.document().map().modified());

      // A local watchdog, so that a save prompt is attributed to this call
      const auto closeWatchdog = ModalWatchdog{};
      const auto result = f.ok("close_map", R"({"discard_changes":true})");
      CHECK(closeWatchdog.modalCount() == 0);

      CHECK(result.value("closed") == true);
      CHECK(result.value("remaining_documents") == 0);
      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
      CHECK(f.err("get_map_info") == "no_document");

      // the file on disk is not touched
      CHECK(readFile(path) == savedText);

      // and the discarded brush is gone when the map is opened again
      f.ok("open_map", QJsonObject{{"path", path}});
      CHECK(f.ok("get_map_stats").value("counts")["world_brushes"] == 2);
    }

    SECTION("discards the changes of a map that was never saved")
    {
      f.newMap();
      f.ok("create_box_brush", Box);

      const auto closeWatchdog = ModalWatchdog{};
      CHECK(f.ok("close_map", R"({"discard_changes":true})").value("closed") == true);
      CHECK(closeWatchdog.modalCount() == 0);
      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }

    SECTION("discard_changes closes an unmodified map")
    {
      auto& window = f.newMap();
      REQUIRE(!window.document().map().modified());

      CHECK(f.ok("close_map", R"({"discard_changes":true})").value("closed") == true);
      CHECK(mapWindowManager.allMapWindowsClosed());
    }

    SECTION("discards only the changes of the target")
    {
      auto& first = f.newMap();
      f.ok("create_box_brush", Box);
      auto& second = f.newMap();
      f.ok("create_box_brush", Box);
      REQUIRE(f.ok("get_map_info").value("document") == "d2");

      CHECK(
        f.ok("close_map", R"({"discard_changes":true})").value("remaining_documents")
        == 1);
      CHECK(!isWelcomeWindowVisible());
      CHECK(mapWindowManager.mapWindows() == std::vector<MapWindow*>{&first});
      CHECK(&second != &first);

      // the remaining map keeps its changes and still refuses a plain close
      CHECK(first.document().map().modified());
      CHECK(f.ok("get_map_info").value("document") == "d1");
      CHECK(f.err("close_map") == "unsaved_changes");

      CHECK(f.ok("close_map", R"({"discard_changes":true})").value("closed") == true);
      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
    }

    SECTION("refuses while the compilation dialog is open")
    {
      auto& window = f.newMap();
      f.ok("create_box_brush", Box);

      auto camera = gl::PerspectiveCamera{};
      auto compilationDialog = std::make_unique<CompilationDialog>(
        f.appController, window.document(), camera, &window);
      compilationDialog->show();
      QApplication::processEvents();
      REQUIRE(compilationDialog->isVisible());

      const auto error = f.error("close_map", R"({"discard_changes":true})");
      CHECK(error.value("code") == "editor_busy");
      CHECK(error.value("details")["reason"] == "compile_running");
      CHECK(mapWindowManager.mapWindows() == std::vector<MapWindow*>{&window});
      CHECK(window.document().map().modified());

      compilationDialog.reset();
      CHECK(f.ok("close_map", R"({"discard_changes":true})").value("closed") == true);
    }

    SECTION("refuses while a nested event loop runs")
    {
      // QDrag::exec (e.g. dragging from the entity browser) runs a nested event loop,
      // and the MCP server is served from it. A window deleted there would be deleted
      // while the drag code that started the loop still uses it.
      auto& window = f.newMap();
      f.ok("create_box_brush", Box);

      // The outer loop stands in for the application's main event loop
      auto outerLoop = QEventLoop{};
      auto innerLoop = QEventLoop{};
      auto nestedResult = QJsonObject{};
      QTimer::singleShot(0, [&]() {
        QTimer::singleShot(0, [&]() {
          nestedResult = f.call("close_map", R"({"discard_changes":true})");
          innerLoop.quit();
        });
        innerLoop.exec();
        outerLoop.quit();
      });
      outerLoop.exec();

      const auto error = nestedResult.value("structuredContent")["error"].toObject();
      CHECK(nestedResult.value("isError") == true);
      CHECK(error.value("code") == "editor_busy");
      CHECK(error.value("details")["reason"] == "nested_event_loop");
      CHECK(mapWindowManager.mapWindows() == std::vector<MapWindow*>{&window});
      CHECK(!isWelcomeWindowVisible());
      CHECK(window.document().map().modified());

      // Called from the main event loop, close_map closes the map
      auto mainLoop = QEventLoop{};
      auto mainResult = QJsonObject{};
      QTimer::singleShot(0, [&]() {
        mainResult = f.call("close_map", R"({"discard_changes":true})");
        mainLoop.quit();
      });
      mainLoop.exec();

      CHECK(mainResult.value("isError") != true);
      CHECK(mapWindowManager.allMapWindowsClosed());
    }

    SECTION("rejects a non-boolean discard_changes")
    {
      f.newMap();
      f.ok("create_box_brush", Box);
      CHECK(f.err("close_map", R"({"discard_changes":"yes"})") == "invalid_argument");
      CHECK(f.err("close_map", R"({"discard_changes":1})") == "invalid_argument");
      CHECK(mapWindowManager.mapWindows().size() == 1);
    }

    SECTION("closes the last map and shows the welcome window")
    {
      f.newMap();
      const auto result = f.ok("close_map");
      CHECK(result.value("closed") == true);
      CHECK(result.value("remaining_documents") == 0);
      CHECK(mapWindowManager.allMapWindowsClosed());
      CHECK(isWelcomeWindowVisible());
      CHECK(f.err("get_map_info") == "no_document");
      CHECK(f.err("close_map") == "no_document");
    }

    SECTION("closes the target and falls back to another map")
    {
      auto& first = f.newMap();
      f.newMap();
      f.ok("set_active_document", R"({"document":"d1"})");
      f.ok("create_box_brush", Box);
      f.ok("save_map", QJsonObject{{"path", dir.filePath("first.map")}});

      CHECK(f.ok("close_map").value("remaining_documents") == 1);
      CHECK(!isWelcomeWindowVisible());
      const auto remaining = mapWindowManager.mapWindows();
      CHECK(std::ranges::find(remaining, &first) == remaining.end());
      CHECK(f.ok("get_map_info").value("document") == "d2");

      // document ids are never reused
      f.newMap();
      CHECK(f.ok("get_map_info").value("document") == "d3");
    }
  }

  CHECK(watchdog.modalCount() == 0);
}

namespace
{

// like the Generic game shipped with TrenchBroom: one definition file, new maps
// have a starter brush
const auto GenericGameConfig = R"({
    "version": 9,
    "name": "Generic",
    "icon": "Icon.png",
    "fileformats": [
        { "format": "Standard" },
        { "format": "Valve" }
    ],
    "filesystem": {
        "searchpath": ".",
        "packageformat": { "extension": ".pak", "format": "idpak" }
    },
    "materials": {
        "root": "textures",
        "format": { "extensions": [".png"], "format": "image" }
    },
    "entities": {
        "definitions": [ "Generic.fgd" ],
        "defaultcolor": "0.6 0.6 0.6 1.0"
    },
    "tags": {
        "brush": [],
        "brushface": []
    }
})";

// like Quake: a wad property, a soft map bounds, two definition files of which only the
// first one exists
const auto QuakeLikeGameConfig = R"({
    "version": 9,
    "name": "QuakeLike",
    "icon": "Icon.png",
    "fileformats": [
        { "format": "Valve" },
        { "format": "Standard" }
    ],
    "filesystem": {
        "searchpath": "id1",
        "packageformat": { "extension": ".pak", "format": "idpak" }
    },
    "materials": {
        "root": "textures",
        "extensions": [".D"],
        "palette": "gfx/palette.lmp",
        "attribute": "wad"
    },
    "entities": {
        "definitions": [ "QuakeLike.fgd", "Missing.fgd" ],
        "defaultcolor": "0.6 0.6 0.6 1.0"
    },
    "softMapBounds": "-4096 -4096 -4096 4096 4096 4096",
    "tags": {
        "brush": [],
        "brushface": []
    }
})";

const auto GenericFgd = R"(@SolidClass = worldspawn : "World entity" []
@PointClass size(-16 -16 -24, 16 16 32) = info_player_start : "Player 1 start" []
)";

const auto QuakeLikeFgd = R"(@SolidClass = worldspawn : "World entity" []
@PointClass size(-16 -16 -24, 16 16 32) = info_player_start : "Player 1 start" []
@PointClass size(-8 -8 -8, 8 8 8) = light : "Light" [ light(integer) : "Brightness" : 300 ]
)";

void writeDefinitionFile(
  AppController& appController,
  const std::string& game,
  const std::string& file,
  const char* contents)
{
  const auto* gameInfo = appController.gameManager().gameInfo(game);
  REQUIRE(gameInfo != nullptr);
  auto stream = std::ofstream{gameInfo->gameConfig.findConfigFile(file)};
  REQUIRE(stream.good());
  stream << contents;
}

QJsonObject findItem(const QJsonObject& validation, const QString& description)
{
  for (const auto& item : validation.value("items").toArray())
  {
    if (item.toObject().value("description").toString() == description)
    {
      return item.toObject();
    }
  }
  return {};
}

} // namespace

TEST_CASE("McpAppTools get_conventions")
{
  auto f = McpAppFixture{[](const auto& writeGameConfig) {
    writeGameConfig("Generic", GenericGameConfig, std::nullopt, std::nullopt);
    writeGameConfig("QuakeLike", QuakeLikeGameConfig, std::nullopt, std::nullopt);
  }};
  writeDefinitionFile(f.appController, "Generic", "Generic.fgd", GenericFgd);
  writeDefinitionFile(f.appController, "QuakeLike", "QuakeLike.fgd", QuakeLikeFgd);

  f.appController.showWelcomeWindow();
  auto watchdog = ModalWatchdog{};

  const auto games = f.ok("list_games").value("games");
  REQUIRE(games.toArray().size() == 3);

  SECTION("works without a document")
  {
    const auto result = f.ok("get_conventions");

    // game independent conventions
    CHECK(result.value("up_axis") == "+z");
    CHECK(result.value("handedness") == "right");
    CHECK(result.value("units").toString().contains("inch"));
    CHECK(
      result.value("player").toObject()
      == QJsonObject{
        {"width", 32},
        {"height", 56},
        {"eye_height", 22},
        {"step_height", 18},
        {"jump_height", 45},
        {"source", "default"}});
    CHECK(
      result.value("common_dimensions").toObject().value("door")
      == QJsonValue{QJsonObject{{"width", 64}, {"height", 96}}});
    CHECK(result.value("entity_definitions_note").toString().contains("Generic"));
    CHECK(result.value("entity_definitions_note").toString().contains("Quake"));

    // the games, as list_games lists them
    CHECK(result.value("games") == games);

    // no document fields, and a pointer to new_map
    CHECK(result.value("document").isNull());
    CHECK(result.value("next_step").toString().contains("new_map"));
    for (const auto* key :
         {"grid_size",
          "world_bounds",
          "soft_map_bounds",
          "material_default",
          "map_format",
          "game"})
    {
      CHECK_FALSE(result.contains(key));
    }

    // nothing was opened
    CHECK(f.appController.mapWindowManager().allMapWindowsClosed());
    CHECK(isWelcomeWindowVisible());

    // deterministic
    CHECK(f.ok("get_conventions") == result);
  }

  SECTION("describes a game without a document")
  {
    SECTION("Generic")
    {
      const auto result = f.ok("get_conventions", R"({"game":"Generic"})");
      CHECK(result.value("document").isNull());
      const auto game = result.value("game").toObject();
      CHECK(game.value("name") == "Generic");
      CHECK(stringList(game.value("formats")) == QStringList{"Standard", "Valve"});
      CHECK(game.value("default_format") == "Standard");
      CHECK(game.value("material_default") == "__TB_empty");
      CHECK(game.value("material_property").isNull());
      CHECK(game.value("soft_map_bounds").isNull());
      CHECK(game.value("experimental") == false);
      CHECK(game.value("game_path_configured") == false);
      CHECK(
        game.value("entity_definitions").toObject()
        == QJsonObject{
          {"files",
           QJsonArray{QJsonObject{{"file", "Generic.fgd"}, {"available", true}}}},
          {"default", "Generic.fgd"},
          {"default_available", true},
        });
      // the note says that light has no definition and suggests Quake
      const auto note = game.value("note").toString();
      CHECK(note.contains("light"));
      CHECK(note.contains("Quake"));
      CHECK(note == result.value("entity_definitions_note").toString());
    }

    SECTION("a game with a material property and a missing definition file")
    {
      const auto game =
        f.ok("get_conventions", R"({"game":"QuakeLike"})").value("game").toObject();
      CHECK(game.value("default_format") == "Valve");
      CHECK(game.value("material_property") == "wad");
      CHECK(
        game.value("soft_map_bounds")
        == QJsonValue{toJson(vm::bbox3d{{-4096, -4096, -4096}, {4096, 4096, 4096}})});
      CHECK(
        game.value("entity_definitions").toObject()
        == QJsonObject{
          {"files",
           QJsonArray{
             QJsonObject{{"file", "QuakeLike.fgd"}, {"available", true}},
             QJsonObject{{"file", "Missing.fgd"}, {"available", false}},
           }},
          {"default", "QuakeLike.fgd"},
          {"default_available", true},
        });
      CHECK_FALSE(game.contains("note"));
    }

    SECTION("a game without definition files")
    {
      const auto game =
        f.ok("get_conventions", R"({"game":"Test"})").value("game").toObject();
      CHECK(
        game.value("entity_definitions").toObject()
        == QJsonObject{
          {"files", QJsonArray{}},
          {"default", QJsonValue{}},
          {"default_available", false},
        });
    }

    SECTION("an unknown game")
    {
      const auto error = f.error("get_conventions", R"({"game":"Nope"})");
      CHECK(error.value("code") == "unknown_game");
      CHECK(
        stringList(error.value("details").toObject().value("available"))
        == QStringList{"Generic", "QuakeLike", "Test"});
    }

    SECTION("schema errors")
    {
      CHECK(f.err("get_conventions", R"({"game":""})") == "invalid_argument");
      CHECK(f.err("get_conventions", R"({"game":1})") == "invalid_argument");
      CHECK(f.err("get_conventions", R"({"x":1})") == "invalid_argument");
    }
  }

  SECTION("with a document")
  {
    f.ok("new_map", R"({"game":"QuakeLike","format":"Valve"})");
    auto* window = f.appController.mapWindowManager().topMapWindow();
    REQUIRE(window != nullptr);
    auto& map = window->document().map();
    const auto before = snapshot(map);

    const auto result = f.ok("get_conventions");
    CHECK(result.value("document") == "d1");
    CHECK_FALSE(result.contains("next_step"));

    // the document fields, as before
    CHECK(result.value("grid_size") == QJsonValue{canon(map.grid().actualSize())});
    CHECK(result.value("world_bounds") == QJsonValue{toJson(map.worldBounds())});
    CHECK(
      result.value("soft_map_bounds")
      == QJsonObject{
        {"source", "game"},
        {"bounds", toJson(vm::bbox3d{{-4096, -4096, -4096}, {4096, 4096, 4096}})}});
    CHECK(
      result.value("material_default")
      == QJsonValue{QString::fromStdString(map.currentMaterialName())});
    CHECK(result.value("map_format") == "Valve");

    // and the game independent ones
    CHECK(result.value("up_axis") == "+z");
    CHECK(result.value("player").toObject().value("step_height") == 18);
    CHECK(result.value("games") == games);

    // game describes the map's game unless another one is given
    CHECK(result.value("game").toObject().value("name") == "QuakeLike");
    CHECK(
      f.ok("get_conventions", R"({"game":"Generic"})")
        .value("game")
        .toObject()
        .value("name")
      == "Generic");
    CHECK(f.err("get_conventions", R"({"game":"Nope"})") == "unknown_game");

    // read only and deterministic
    CHECK(f.ok("get_conventions") == result);
    CHECK(snapshot(map) == before);

    // the busy gate applies with a document
    {
      auto dialog = QDialog{};
      dialog.setModal(true);
      dialog.open();
      REQUIRE(QApplication::activeModalWidget() == &dialog);
      const auto error = f.error("get_conventions");
      CHECK(error.value("code") == "editor_busy");
      dialog.close();
    }

    // an app tool cannot be used in a batch
    CHECK(
      f.err("batch", R"({"operations":[{"tool":"get_conventions","arguments":{}}]})")
      == "invalid_argument");

    // after closing the map it works without a document again
    f.closeAllMapWindows();
    CHECK(f.ok("get_conventions").value("document").isNull());
  }

  SECTION("validate_map marks the issues of a new map as expected")
  {
    SECTION("the empty wad of worldspawn")
    {
      f.ok("new_map", R"({"game":"QuakeLike"})");
      auto validation = f.ok("validate_map");
      const auto wad =
        findItem(validation, "Property 'wad' of worldspawn has an empty value.");
      REQUIRE_FALSE(wad.isEmpty());
      CHECK(wad.value("node") == "world");
      CHECK(wad.value("property") == "wad");
      CHECK(wad.value("expected_for_new_map") == true);
      CHECK(wad.value("expected_reason").toString().contains("wad"));
      CHECK(validation.value("expected_count") == 1);
      CHECK(validation.value("note").toString().contains("expected_for_new_map"));

      // a light has a definition in this game, so nothing else is reported
      f.ok("create_entity", R"({"classname":"light","origin":[0,0,64]})");
      validation = f.ok("validate_map");
      CHECK(validation.value("total") == 1);
      CHECK(validation.value("expected_count") == 1);

      // other empty properties are not expected
      f.ok("set_entity_properties", R"({"ids":["world"],"set":{"message":""}})");
      validation = f.ok("validate_map");
      const auto message =
        findItem(validation, "Property 'message' of worldspawn has an empty value.");
      REQUIRE_FALSE(message.isEmpty());
      CHECK_FALSE(message.contains("expected_for_new_map"));
      CHECK_FALSE(message.contains("expected_reason"));
      CHECK(validation.value("expected_count") == 1);

      // once the wad is set, the issue is gone; setting it reloads the entity
      // definitions, which used to crash set_entity_properties (it kept copies of the
      // entities, and with them references to the old definitions)
      f.ok(
        "set_entity_properties",
        R"({"ids":["world"],"set":{"wad":"base.wad"},"remove":["message"]})");
      validation = f.ok("validate_map");
      CHECK(validation.value("total") == 0);
      CHECK(validation.value("expected_count") == 0);
      CHECK_FALSE(validation.contains("note"));
    }

    SECTION("light in game Generic")
    {
      f.ok("new_map", R"({"game":"Generic"})");
      CHECK(f.ok("validate_map").value("total") == 0);

      const auto light =
        f.ok("create_entity", R"({"classname":"light","origin":[0,0,64]})");
      const auto lightId = light.value("id").toString();
      f.ok("create_entity", R"({"classname":"info_player_start","origin":[0,64,24]})");

      // the envelope of the mutating call marks the issue too
      const auto issues = light.value("issues").toObject().value("items").toArray();
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].toObject().value("expected_for_new_map") == true);

      const auto validation = f.ok("validate_map");
      CHECK(validation.value("total") == 1);
      const auto item = findItem(validation, "light not found in entity definitions");
      REQUIRE_FALSE(item.isEmpty());
      CHECK(item.value("node") == lightId);
      CHECK(item.value("type") == "Missing entity definition");
      CHECK(item.value("expected_for_new_map") == true);
      CHECK(item.value("expected_reason").toString().contains("Quake"));
      CHECK(validation.value("expected_count") == 1);

      // deterministic
      CHECK(f.ok("validate_map") == validation);
    }

    SECTION("a missing definition in another game is not expected")
    {
      f.ok("new_map", R"({"game":"QuakeLike"})");
      f.ok("create_entity", R"({"classname":"monster_nope","origin":[0,0,64]})");
      const auto validation = f.ok("validate_map");
      const auto item =
        findItem(validation, "monster_nope not found in entity definitions");
      REQUIRE_FALSE(item.isEmpty());
      CHECK_FALSE(item.contains("expected_for_new_map"));
      CHECK(validation.value("expected_count") == 1);
    }

    SECTION("game Generic with an external definition file is not expected")
    {
      f.ok("new_map", R"({"game":"Generic"})");
      auto* window = f.appController.mapWindowManager().topMapWindow();
      REQUIRE(window != nullptr);
      auto& map = window->document().map();
      // as if the user chose the file in the entity definition file chooser
      mdl::setEntityDefinitionFile(
        map,
        mdl::EntityDefinitionFileSpec::makeExternal(
          map.gameInfo().gameConfig.findConfigFile("Generic.fgd")));
      f.ok("create_entity", R"({"classname":"light","origin":[0,0,64]})");
      const auto item =
        findItem(f.ok("validate_map"), "light not found in entity definitions");
      REQUIRE_FALSE(item.isEmpty());
      CHECK_FALSE(item.contains("expected_for_new_map"));
    }

    SECTION("game Generic with another builtin definition file is not expected")
    {
      f.ok("new_map", R"({"game":"Generic"})");
      auto* window = f.appController.mapWindowManager().topMapWindow();
      REQUIRE(window != nullptr);
      auto& map = window->document().map();
      // a file that does not exist, so not even worldspawn has a definition
      mdl::setEntityDefinitionFile(
        map, mdl::EntityDefinitionFileSpec::makeBuiltin("Missing.fgd"));
      f.ok("create_entity", R"({"classname":"light","origin":[0,0,64]})");
      const auto validation = f.ok("validate_map");
      const auto light = findItem(validation, "light not found in entity definitions");
      REQUIRE_FALSE(light.isEmpty());
      CHECK_FALSE(light.contains("expected_for_new_map"));
      const auto world =
        findItem(validation, "worldspawn not found in entity definitions");
      REQUIRE_FALSE(world.isEmpty());
      CHECK_FALSE(world.contains("expected_for_new_map"));
      CHECK(validation.value("expected_count") == 0);
    }
  }

  f.closeAllMapWindows();
  CHECK(watchdog.modalCount() == 0);
}

} // namespace tb::ui
