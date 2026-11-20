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

#pragma once

#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include "AppTestUtils.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h" // IWYU pragma: keep
#include "ui/McpController.h"

#include <string>

namespace tb::ui
{
class AppController;
class MapWindow;
class WelcomeWindow;

/** The name of the game that McpAppFixture configures. */
inline const auto McpTestGameName = std::string{"Test"};

/**
 * An application with the game "Test" (formats Standard and Valve, no game files) and an
 * MCP controller whose server is not started. All map windows are closed when the
 * fixture is destroyed.
 */
struct McpAppFixture
{
  McpAppFixture();
  ~McpAppFixture();

  McpAppFixture(const McpAppFixture&) = delete;
  McpAppFixture& operator=(const McpAppFixture&) = delete;

  AppControllerFixture appControllerFixture;
  AppController& appController;
  McpController controller;

  /** Calls a tool and returns the CallToolResult. */
  QJsonObject call(const QString& tool, const QJsonObject& args);
  QJsonObject call(const QString& tool, const char* jsonArgs = "{}");

  /** Calls a tool, requires success and returns the structured content. */
  QJsonObject ok(const QString& tool, const QJsonObject& args);
  QJsonObject ok(const QString& tool, const char* jsonArgs = "{}");

  /** Calls a tool, requires an error and returns the error object. */
  QJsonObject error(const QString& tool, const QJsonObject& args);
  QJsonObject error(const QString& tool, const char* jsonArgs = "{}");

  /** Calls a tool, requires an error and returns the error code. */
  QString err(const QString& tool, const QJsonObject& args);
  QString err(const QString& tool, const char* jsonArgs = "{}");

  /** Creates a map with new_map and returns its window, which is the target. */
  MapWindow& newMap(const QString& format = "Standard");

  /** Closes all map windows and deletes them. Modified maps are saved first. */
  void closeAllMapWindows();

private:
  QTemporaryDir m_cleanupDir;
};

} // namespace tb::ui
