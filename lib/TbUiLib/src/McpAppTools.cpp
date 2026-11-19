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

#include "ui/McpAppTools.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QThread>

#include "base/PreferenceManager.h"
#include "base/Result.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/MapHeader.h"
#include "mdl/WorldNode.h"
#include "ui/AppController.h"
#include "ui/CompilationDialog.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"
#include "ui/QPathUtils.h"

#include "kd/filesystem_utils.h"
#include "kd/result.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>

namespace tb::ui
{

McpAppHost::~McpAppHost() = default;

namespace
{

const auto ReadOnly =
  mcp::ToolAnnotations{.readOnly = true, .destructive = false, .idempotent = true};
const auto Mutating =
  mcp::ToolAnnotations{.readOnly = false, .destructive = false, .idempotent = false};
const auto Destructive =
  mcp::ToolAnnotations{.readOnly = false, .destructive = true, .idempotent = false};

constexpr auto DocumentIdPattern = "^d[1-9][0-9]{0,8}$";
constexpr auto MaxPathLength = 4096;

/** Added to the modification count of a map whose file could not be written. */
constexpr auto FailedSaveModificationCount = size_t(1'000'000);

QJsonObject pathSchema(const QString& pattern, const QString& description)
{
  return QJsonObject{
    {"type", "string"},
    {"minLength", 1},
    {"maxLength", MaxPathLength},
    {"pattern", pattern},
    {"description", description},
  };
}

QString gameName(const mdl::GameInfo& gameInfo)
{
  return QString::fromStdString(gameInfo.gameConfig.name);
}

QStringList gameNames(const mdl::GameManager& gameManager)
{
  auto result = QStringList{};
  for (const auto& gameInfo : gameManager.gameInfos())
  {
    result.push_back(gameName(gameInfo));
  }
  return result;
}

QStringList formatNames(const mdl::GameInfo& gameInfo)
{
  auto result = QStringList{};
  for (const auto& formatConfig : gameInfo.gameConfig.fileFormats)
  {
    result.push_back(QString::fromStdString(formatConfig.format));
  }
  return result;
}

McpResult<const mdl::GameInfo*> findGame(
  AppController& appController, const QString& name)
{
  auto& gameManager = appController.gameManager();
  if (const auto* gameInfo = gameManager.gameInfo(name.toStdString()))
  {
    return gameInfo;
  }
  const auto available = gameNames(gameManager);
  return toolError(
    "unknown_game",
    QString{"there is no game named '%1'"}.arg(name),
    "call list_games for the available games",
    {{"available", QJsonArray::fromStringList(available)}});
}

/**
 * Returns the map format with the given name, which must be one of the formats of the
 * given game. Without a name, returns the game's first format.
 */
McpResult<mdl::MapFormat> findFormat(
  const mdl::GameInfo& gameInfo, const std::optional<QString>& name)
{
  const auto available = formatNames(gameInfo);
  const auto unsupported = [&](const QString& message) {
    return toolError(
      "unsupported_format",
      message,
      "call list_games for the formats of each game",
      {{"available", QJsonArray::fromStringList(available)}});
  };

  if (!name)
  {
    if (available.empty())
    {
      return unsupported(QString{"game '%1' has no map formats"}.arg(gameName(gameInfo)));
    }
    const auto format = mdl::formatFromName(available.front().toStdString());
    if (format == mdl::MapFormat::Unknown)
    {
      return unsupported(
        QString{"the default format of game '%1' is unknown"}.arg(gameName(gameInfo)));
    }
    return format;
  }

  const auto format = mdl::formatFromName(name->toStdString());
  const auto it = std::ranges::find_if(
    available, [&](const auto& formatName) { return formatName == *name; });
  if (format == mdl::MapFormat::Unknown || it == available.end())
  {
    return unsupported(QString{"game '%1' does not support the map format '%2'"}.arg(
      gameName(gameInfo), *name));
  }
  return format;
}

/**
 * Converts a TrenchBroom result to a tool result. Note that kdl::transform_error cannot
 * be used for this, since it turns the error into a value.
 */
template <typename F>
McpResult<void> toToolResult(const Result<void>& result, const F& makeError)
{
  if (result.is_error())
  {
    return makeError(std::get<Error>(result.error()));
  }
  return kdl::void_success;
}

std::optional<QString> optionalString(const QJsonObject& args, const QString& key)
{
  return args.contains(key) ? std::optional{args.value(key).toString()} : std::nullopt;
}

McpResult<std::filesystem::path> absolutePath(const QString& text, const QString& pointer)
{
  auto path = pathFromQString(text);
  if (!path.is_absolute())
  {
    return toolError(
      "invalid_argument",
      QString{"%1 must be an absolute path"}.arg(pointer),
      {},
      {{"pointer", pointer}});
  }
  return path.lexically_normal();
}

QJsonObject documentJson(McpAppHost& host, MapWindow& window)
{
  const auto& map = window.document().map();
  return QJsonObject{
    {"document", host.documentId(window)},
    {"filename", QString::fromStdString(map.filename())},
    {"path",
     map.persistent() ? QJsonValue{pathAsGenericQString(map.path())} : QJsonValue{}},
    {"persistent", map.persistent()},
    {"modified", map.modified()},
    {"game", QString::fromStdString(map.gameInfo().gameConfig.name)},
    {"format", QString::fromStdString(mdl::formatName(map.worldNode().mapFormat()))},
    {"target", host.currentTarget() == &window},
  };
}

/**
 * Returns the get_map_info fields of the target document and its scene tree up to depth
 * 2, by running the read tools directly.
 */
McpResult<QJsonObject> describeNewDocument(McpAppHost& host)
{
  return host.target(McpToolKind::Read) | kdl::and_then([&](const McpTarget& target) {
           auto& dispatcher = host.dispatcher();
           auto context = McpCallContext{.doc = *target.doc};
           target.doc->nodes.reconcile();

           const auto* mapInfoTool = dispatcher.find(u"get_map_info");
           const auto* sceneTreeTool = dispatcher.find(u"get_scene_tree");
           if (!mapInfoTool || !sceneTreeTool)
           {
             return McpResult<QJsonObject>{QJsonObject{
               {"document", target.doc->documentId},
             }};
           }

           return mapInfoTool->run(context, {}) | kdl::and_then([&](QJsonObject result) {
                    return sceneTreeTool->run(context, {{"max_depth", 2}})
                           | kdl::transform([&](const QJsonObject& scene) {
                               result["scene"] = scene;
                               return result;
                             });
                  });
         });
}

// list_games

McpToolDef listGamesTool(McpAppHost& host)
{
  return {
    .name = "list_games",
    .title = "List Games",
    .description =
      "Lists the games TrenchBroom knows, with their map formats. Pass a game name and "
      "one of its formats to new_map. \"Generic\" needs no game files.",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::App,
    .annotations = ReadOnly,
    .runApp = [&host](const QJsonObject&) -> McpResult<QJsonObject> {
      auto games = QJsonArray{};
      for (const auto& gameInfo : host.appController().gameManager().gameInfos())
      {
        games.append(QJsonObject{
          {"name", gameName(gameInfo)},
          {"formats", QJsonArray::fromStringList(formatNames(gameInfo))},
          {"experimental", gameInfo.gameConfig.experimental},
          {"game_path_configured", !pref(gameInfo.gamePathPreference).empty()},
        });
      }
      return QJsonObject{{"games", games}};
    },
  };
}

// list_documents

McpToolDef listDocumentsTool(McpAppHost& host)
{
  return {
    .name = "list_documents",
    .title = "List Documents",
    .description =
      "Lists the open maps. Map tools act on the document marked as target; change it "
      "with set_active_document.",
    .inputSchema = schema::object({}),
    .kind = McpToolKind::App,
    .annotations = ReadOnly,
    .runApp = [&host](const QJsonObject&) -> McpResult<QJsonObject> {
      auto documents = QJsonArray{};
      for (auto* window : host.appController().mapWindowManager().mapWindows())
      {
        documents.append(documentJson(host, *window));
      }
      return QJsonObject{{"documents", documents}};
    },
  };
}

// set_active_document

McpToolDef setActiveDocumentTool(McpAppHost& host)
{
  return {
    .name = "set_active_document",
    .title = "Set Active Document",
    .description =
      "Makes the given document (an id from list_documents) the target of all map tools "
      "and brings its window to the front.",
    .inputSchema = schema::object(
      {
        {"document",
         QJsonObject{
           {"type", "string"},
           {"pattern", DocumentIdPattern},
           {"description", "A document id such as \"d1\"."},
         }},
      },
      {"document"}),
    .kind = McpToolKind::App,
    .annotations = mcp::ToolAnnotations{.idempotent = true},
    .runApp = [&host](const QJsonObject& args) -> McpResult<QJsonObject> {
      const auto documentId = args.value("document").toString();
      auto* window = host.findDocument(documentId);
      if (!window)
      {
        return toolError(
          "unknown_document",
          QString{"there is no open document '%1'"}.arg(documentId),
          "call list_documents for the open documents");
      }
      if (const auto busy = host.requireNoModalUi(); busy.is_error())
      {
        return errorOf(busy);
      }

      host.pin(window);
      window->raise();
      window->activateWindow();
      return QJsonObject{{"document", documentId}};
    },
  };
}

// new_map

McpToolDef newMapTool(McpAppHost& host)
{
  return {
    .name = "new_map",
    .title = "New Map",
    .description =
      "Creates a new map for the given game (see list_games) in a new window and makes "
      "it the target of all map tools. The format defaults to the game's first format. "
      "New maps may contain a starter brush. Returns the get_map_info fields and the "
      "scene tree.",
    .inputSchema = schema::object(
      {
        {"game", schema::string(1, 256, "A game name from list_games.")},
        {"format", schema::string(1, 64, "A map format of the game, e.g. \"Valve\".")},
      },
      {"game"}),
    .kind = McpToolKind::App,
    .annotations = Mutating,
    .runApp = [&host](const QJsonObject& args) -> McpResult<QJsonObject> {
      if (const auto busy = host.requireNoModalUi(); busy.is_error())
      {
        return errorOf(busy);
      }

      auto& appController = host.appController();
      return findGame(appController, args.value("game").toString())
             | kdl::and_then([&](const mdl::GameInfo* gameInfo) {
                 return findFormat(*gameInfo, optionalString(args, "format"))
                        | kdl::and_then([&](const mdl::MapFormat format) {
                            return toToolResult(
                              appController.createDocument(*gameInfo, format),
                              [](const Error& e) {
                                return toolError(
                                  "io_error",
                                  QString{"the map could not be created: %1"}.arg(
                                    QString::fromStdString(e.msg)));
                              });
                          });
               })
             | kdl::and_then([&]() {
                 host.pin(appController.mapWindowManager().topMapWindow());
                 return describeNewDocument(host);
               });
    },
  };
}

// open_map

/** Reads the game and format from the map header unless they are given. */
McpResult<std::pair<const mdl::GameInfo*, mdl::MapFormat>> gameAndFormatForFile(
  AppController& appController,
  const std::filesystem::path& path,
  const std::optional<QString>& game,
  const std::optional<QString>& format)
{
  auto headerGame = std::optional<std::string>{};
  auto headerFormat = mdl::MapFormat::Unknown;
  if (!game || !format)
  {
    if (auto header = kdl::with_istream(path, mdl::readMapHeader); header.is_success())
    {
      std::tie(headerGame, headerFormat) = std::move(header).value();
    }
  }

  const auto gameNameToUse =
    game
      ? std::optional{*game}
      : (headerGame ? std::optional{QString::fromStdString(*headerGame)} : std::nullopt);
  if (!gameNameToUse)
  {
    return toolError(
      "invalid_argument",
      "the map file does not name its game",
      "pass game (see list_games)",
      {{"pointer", "/game"}});
  }

  return findGame(appController, *gameNameToUse)
         | kdl::and_then(
           [&](const mdl::GameInfo* gameInfo)
             -> McpResult<std::pair<const mdl::GameInfo*, mdl::MapFormat>> {
             if (!format)
             {
               // An unknown format is fine: the reader tries all formats of the game
               return std::pair{gameInfo, headerFormat};
             }

             const auto mapFormat = mdl::formatFromName(format->toStdString());
             if (mapFormat == mdl::MapFormat::Unknown)
             {
               return toolError(
                 "unsupported_format",
                 QString{"'%1' is not a map format"}.arg(*format),
                 "call list_games for the formats of each game",
                 {{"available", QJsonArray::fromStringList(formatNames(*gameInfo))}});
             }
             return std::pair{gameInfo, mapFormat};
           });
}

McpToolDef openMapTool(McpAppHost& host)
{
  return {
    .name = "open_map",
    .title = "Open Map",
    .description =
      "Opens a .map file in a new window and makes it the target of all map tools. The "
      "game and format are read from the file's header comment unless given. Returns "
      "the get_map_info fields and the scene tree.",
    .inputSchema = schema::object(
      {
        {"path", pathSchema(".*", "Absolute path of the map file.")},
        {"game", schema::string(1, 256, "A game name from list_games.")},
        {"format", schema::string(1, 64, "A map format, e.g. \"Valve\".")},
      },
      {"path"}),
    .kind = McpToolKind::App,
    .annotations = Mutating,
    .runApp = [&host](const QJsonObject& args) -> McpResult<QJsonObject> {
      if (const auto busy = host.requireNoModalUi(); busy.is_error())
      {
        return errorOf(busy);
      }

      auto& appController = host.appController();
      return absolutePath(args.value("path").toString(), "/path")
             | kdl::and_then(
               [&](std::filesystem::path path) -> McpResult<std::filesystem::path> {
                 auto error = std::error_code{};
                 if (!std::filesystem::is_regular_file(path, error))
                 {
                   return toolError(
                     "io_error",
                     QString{"%1 not found"}.arg(pathAsQString(path)),
                     "pass the absolute path of an existing .map file",
                     {{"path", pathAsGenericQString(path)}});
                 }
                 return path;
               })
             | kdl::and_then([&](std::filesystem::path path) {
                 return gameAndFormatForFile(
                          appController,
                          path,
                          optionalString(args, "game"),
                          optionalString(args, "format"))
                        | kdl::and_then([&](const auto& gameAndFormat) {
                            return toToolResult(
                              appController.loadDocument(
                                *gameAndFormat.first, gameAndFormat.second, path),
                              [&](const Error& e) {
                                return toolError(
                                  "io_error",
                                  QString{"%1 could not be opened: %2"}.arg(
                                    pathAsQString(path), QString::fromStdString(e.msg)),
                                  "check the game and format");
                              });
                          });
               })
             | kdl::and_then([&]() {
                 host.pin(appController.mapWindowManager().topMapWindow());
                 return describeNewDocument(host);
               });
    },
  };
}

// save_map

bool endsWithMapExtension(const QString& path)
{
  return path.endsWith(".map", Qt::CaseInsensitive);
}

/** Checks that the map was written, since Map::saveTo hides write errors. */
McpResult<qint64> checkSavedFile(
  McpAppHost& host,
  const std::filesystem::path& path,
  const std::filesystem::file_time_type& callStart)
{
  auto error = std::error_code{};
  const auto exists = std::filesystem::is_regular_file(path, error);
  const auto size = exists ? std::filesystem::file_size(path, error) : 0u;
  const auto modified = exists ? std::filesystem::last_write_time(path, error)
                               : std::filesystem::file_time_type{};

  auto log = QJsonArray{};
  auto hasErrors = false;
  if (const auto* lines = host.capturedLog())
  {
    for (const auto& line : *lines)
    {
      log.append(line.text);
      // Map::saveTo logs write errors with this prefix and returns success anyway;
      // other errors, e.g. from reloading assets for the new directory, don't matter
      hasErrors =
        hasErrors
        || (line.level == LogLevel::Error && line.text.startsWith("Could not save document"));
    }
  }

  if (error || !exists || size == 0 || modified < callStart || hasErrors)
  {
    return toolError(
      "io_error",
      QString{"the map could not be written to %1"}.arg(pathAsQString(path)),
      "check that the directory is writable",
      {{"path", pathAsGenericQString(path)}, {"log", log}});
  }
  return qint64(size);
}

McpToolDef saveMapTool(McpAppHost& host)
{
  return {
    .name = "save_map",
    .title = "Save Map",
    .description =
      "Saves the target map. Without path, the map must have been saved or opened "
      "before. With path (absolute, ending in .map), saves under that name; an existing "
      "file is only replaced with overwrite:true. Returns the path and the file size in "
      "bytes.",
    .inputSchema = schema::object({
      {"path",
       pathSchema("^.*\\.[mM][aA][pP]$", "Absolute path of the .map file to save to.")},
      {"overwrite",
       schema::boolean("Replace an existing file at path. Defaults to false.")},
    }),
    .kind = McpToolKind::App,
    .annotations = Mutating,
    .runApp = [&host](const QJsonObject& args) -> McpResult<QJsonObject> {
      return host.target(McpToolKind::Mutate)
             | kdl::and_then([&](const McpTarget& target) -> McpResult<QJsonObject> {
                 auto& map = target.window->document().map();
                 // file times have a coarse resolution on some file systems
                 const auto callStart = std::filesystem::file_time_type::clock::now()
                                        - std::chrono::seconds{2};

                 auto path = std::filesystem::path{};
                 if (args.contains("path"))
                 {
                   const auto pathText = args.value("path").toString();
                   auto absPath = absolutePath(pathText, "/path");
                   if (absPath.is_error())
                   {
                     return errorOf(absPath);
                   }
                   path = std::move(absPath).value();
                   if (!endsWithMapExtension(pathAsQString(path)))
                   {
                     return toolError(
                       "invalid_argument",
                       "/path must end with .map",
                       {},
                       {{"pointer", "/path"}});
                   }

                   auto error = std::error_code{};
                   if (!std::filesystem::is_directory(path.parent_path(), error))
                   {
                     return toolError(
                       "io_error",
                       QString{"the directory %1 does not exist"}.arg(
                         pathAsQString(path.parent_path())),
                       "create the directory first or choose another path");
                   }
                   if (std::filesystem::is_directory(path, error))
                   {
                     return toolError(
                       "io_error", QString{"%1 is a directory"}.arg(pathAsQString(path)));
                   }
                   if (
                     std::filesystem::exists(path, error)
                     && !args.value("overwrite").toBool(false))
                   {
                     return toolError(
                       "file_exists",
                       QString{"%1 already exists"}.arg(pathAsQString(path)),
                       "pass overwrite:true to replace it, or choose another path",
                       {{"path", pathAsGenericQString(path)}});
                   }

                   if (const auto result = map.saveAs(path); result.is_error())
                   {
                     return toolError(
                       "io_error",
                       QString::fromStdString(std::get<Error>(result.error()).msg));
                   }
                 }
                 else
                 {
                   if (!map.persistent())
                   {
                     return toolError(
                       "path_required",
                       "the map has never been saved",
                       "pass path, e.g. an absolute path ending in .map");
                   }
                   path = map.path();
                   if (const auto result = map.save(); result.is_error())
                   {
                     return toolError(
                       "io_error",
                       QString::fromStdString(std::get<Error>(result.error()).msg));
                   }
                 }

                 auto bytes = checkSavedFile(host, path, callStart);
                 if (bytes.is_error())
                 {
                   // Map::saveAs marks the map as saved even if the file could not be
                   // written, so closing it would lose the changes without a warning.
                   // Mark it as modified again, by more than the undo steps that could
                   // bring the modification count back to the failed save.
                   map.incModificationCount(FailedSaveModificationCount);
                   return errorOf(bytes);
                 }
                 return QJsonObject{
                   {"path", pathAsGenericQString(path)},
                   {"bytes", bytes.value()},
                 };
               });
    },
  };
}

// close_map

McpToolDef closeMapTool(McpAppHost& host)
{
  return {
    .name = "close_map",
    .title = "Close Map",
    .description =
      "Closes the target map's window. Refuses with unsaved_changes if the map was "
      "modified since it was last saved; call save_map first, or pass "
      "discard_changes:true to throw the changes away. Never shows a save prompt. "
      "Closing the last map shows the welcome window; the server keeps running.",
    .inputSchema = schema::object({
      {"discard_changes",
       schema::boolean(
         "Close without saving and lose all unsaved changes (default false).")},
    }),
    .kind = McpToolKind::App,
    .annotations = Destructive,
    .runApp = [&host](const QJsonObject& args) -> McpResult<QJsonObject> {
      return host.target(McpToolKind::Mutate)
             | kdl::and_then([&](const McpTarget& target) -> McpResult<QJsonObject> {
                 auto* window = target.window;
                 const auto& map = window->document().map();

                 const auto discardChanges = args.value("discard_changes").toBool(false);
                 if (map.modified() && !discardChanges)
                 {
                   return toolError(
                     "unsaved_changes",
                     "the map has unsaved changes",
                     "call save_map first or pass discard_changes:true");
                 }
                 if (const auto* compilationDialog =
                       window->findChild<CompilationDialog*>();
                     compilationDialog && compilationDialog->isVisible())
                 {
                   return toolError(
                     "editor_busy",
                     "a compilation dialog is open",
                     "ask the user to close the compilation dialog",
                     {{"reason", "compile_running"}});
                 }

                 // A nested event loop such as QDrag::exec in the entity browser serves
                 // this call too. The window would be deleted from that loop, while the
                 // code that started the loop still uses it.
                 if (QThread::currentThread()->loopLevel() > 1)
                 {
                   return toolError(
                     "editor_busy",
                     "the user is in the middle of an operation, such as drag and drop",
                     "retry in a moment",
                     {{"reason", "nested_event_loop"}});
                 }

                 auto& appController = host.appController();
                 auto& mapWindowManager = appController.mapWindowManager();

                 // Show the welcome window first, or the application quits on Linux
                 // and Windows when the last window is closed
                 if (mapWindowManager.mapWindows().size() == 1)
                 {
                   appController.showWelcomeWindow();
                 }

                 host.forgetDocumentState(*window);
                 // Neither variant shows the save prompt: an unmodified map has nothing
                 // to save, and closeDocumentDiscardingChanges skips the prompt
                 const auto closed = discardChanges
                                       ? window->closeDocumentDiscardingChanges()
                                       : window->close();
                 if (!closed)
                 {
                   return toolError(
                     "operation_failed",
                     "the window refused to close",
                     "ask the user to close the window");
                 }
                 host.pin(nullptr);

                 return QJsonObject{
                   {"closed", true},
                   {"remaining_documents", qint64(mapWindowManager.mapWindows().size())},
                 };
               });
    },
  };
}

} // namespace

void registerMcpAppTools(McpToolDispatcher& dispatcher, McpAppHost& host)
{
  dispatcher.add(listGamesTool(host));
  dispatcher.add(listDocumentsTool(host));
  dispatcher.add(setActiveDocumentTool(host));
  dispatcher.add(newMapTool(host));
  dispatcher.add(openMapTool(host));
  dispatcher.add(saveMapTool(host));
  dispatcher.add(closeMapTool(host));
}

} // namespace tb::ui
