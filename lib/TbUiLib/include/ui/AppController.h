/*
 Copyright (C) 2025 Kristian Duske

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

#include <QObject>
#include <QtSystemDetection>

#include "base/Result.h"
#include "mdl/MapFormat.h"

#include <filesystem>
#include <memory>

class QMenu;
class QNetworkAccessManager;
class QOffscreenSurface;
class QOpenGLContext;
class QTemporaryFile;
class QTimer;

namespace kdl
{
class task_manager;
}

namespace upd
{
class HttpClient;
class Updater;
} // namespace upd

namespace tb
{
namespace gl
{
class GlManager;
} // namespace gl

namespace mdl
{
class GameManager;
struct EnvironmentConfig;
struct GameInfo;
} // namespace mdl

namespace ui
{
class AboutDialog;
class ActionManager;
class MapWindowManager;
class McpController;
class RecentDocuments;
class WelcomeWindow;

class AppController : public QObject
{
  Q_OBJECT
private:
  std::unique_ptr<kdl::task_manager> m_taskManager;
  std::unique_ptr<mdl::EnvironmentConfig> m_environmentConfig;
  std::unique_ptr<mdl::GameManager> m_gameManager;

  std::unique_ptr<gl::GlManager> m_glManager;

  QOpenGLContext* m_glContext = nullptr;
  QOffscreenSurface* m_offscreenSurface = nullptr;

  QNetworkAccessManager* m_networkManager = nullptr;
  QTimer* m_reloadRecentDocumentsTimer = nullptr;
  QTimer* m_processResourcesTimer = nullptr;

  upd::HttpClient* m_httpClient = nullptr;
  upd::Updater* m_updater = nullptr;

  MapWindowManager* m_mapWindowManager = nullptr;
  RecentDocuments* m_recentDocuments = nullptr;
  std::unique_ptr<ActionManager> m_actionManager;
  std::unique_ptr<WelcomeWindow> m_welcomeWindow;
  std::unique_ptr<AboutDialog> m_aboutDialog;

  QTemporaryFile* m_manualRedirectFile = nullptr;

  McpController* m_mcpController = nullptr;

public:
  AppController(
    std::unique_ptr<kdl::task_manager> taskManager,
    std::unique_ptr<mdl::EnvironmentConfig> environmentConfig,
    std::unique_ptr<mdl::GameManager> gameManager);

  static Result<std::unique_ptr<AppController>> create();

  ~AppController() override;

  kdl::task_manager& taskManager();

  gl::GlManager& glManager();

  const mdl::EnvironmentConfig& environmentConfig() const;

  mdl::GameManager& gameManager();

  upd::Updater& updater();

  MapWindowManager& mapWindowManager();

  const RecentDocuments& recentDocuments() const;
  RecentDocuments& recentDocuments();

  ActionManager& actionManager();

  /**
   * The MCP controller of this application, or nullptr if there is none. The MCP
   * controller registers itself here when it is created and unregisters when it is
   * destroyed.
   */
  McpController* mcpController();
  void setMcpController(McpController* mcpController);

  void askForAutoUpdates();
  void triggerAutoUpdateCheck();

  /**
   * Asks the user for a game and map format and creates a new document. Errors are
   * reported in a message box.
   *
   * Returns true if a document was created.
   */
  bool newDocument();

  /**
   * Asks the user for a map file and opens it.
   */
  void openDocument();

  /**
   * Opens the map file at the given path. If the game or the map format cannot be
   * detected, the user is asked for them. Errors are reported in a message box.
   *
   * Returns true if the document was loaded.
   */
  bool openDocument(const std::filesystem::path& path);

  /**
   * Creates a new document in a new map window and closes the welcome window.
   * MapFormat::Unknown results in an error.
   *
   * Never shows a dialog; errors are returned instead.
   */
  Result<void> createDocument(const mdl::GameInfo& gameInfo, mdl::MapFormat mapFormat);

  /**
   * Loads the map file at the given absolute path into a new map window and closes the
   * welcome window. A relative path results in an error.
   *
   * Never shows a dialog; errors are returned instead.
   */
  Result<void> loadDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    std::filesystem::path absPath);

  void showWelcomeWindow();

  void showManual(const QString& fragment = "");
  void showPreferences();
  void showAboutDialog();
  void debugShowCrashReportDialog();

private:
  void connectObservers();

  void processGlResources();
};

} // namespace ui
} // namespace tb
