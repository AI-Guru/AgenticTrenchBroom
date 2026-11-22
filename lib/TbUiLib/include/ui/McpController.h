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
#include <QtGlobal>

#include "base/Notifier.h"

#include <memory>
#include <optional>

namespace tb::ui
{
class AppController;
class McpToolDispatcher;

/** The URL of the MCP endpoint on the given port: http://127.0.0.1:<port>/mcp. */
QString mcpServerUrl(quint16 port);

/** The shell command that registers the MCP server on the given port with Claude Code. */
QString mcpClaudeAddCommand(quint16 port);

/**
 * Runs the MCP server that lets AI agents control TrenchBroom. It listens on 127.0.0.1
 * only and handles every request on the main thread.
 *
 * The server runs while the preference MCP/Server enabled is set, on the port given by
 * MCP/Server port, and follows changes of these preferences. A port passed to start()
 * (the --mcp-port command line option) starts the server regardless of the preferences
 * and without changing them, until the user changes one of the preferences.
 *
 * Map tools act on a target map window: the one chosen by set_active_document, new_map
 * or open_map while it is open, otherwise the most recently used one. Tool calls are
 * refused with editor_busy while the user is interacting with the editor, e.g. while a
 * modal dialog is open or the user is dragging.
 *
 * Create it on the stack in main() after the app controller and before any modal dialog
 * may be shown, and destroy it before the app controller. It registers itself with the
 * app controller (AppController::mcpController()) so that the preference pane can show
 * its state.
 */
class McpController
{
public:
  /**
   * Notified after the server was started or stopped or failed to start, i.e. whenever
   * isListening(), port(), lastError() or isCommandLineOverride() may have changed.
   */
  Notifier<> statusDidChangeNotifier;

  explicit McpController(AppController& appController);
  ~McpController();

  McpController(const McpController&) = delete;
  McpController& operator=(const McpController&) = delete;

  /**
   * Starts the server according to the preferences, or on the given port if one is given
   * (0 picks a free port). Prints the URL or an error to stderr.
   */
  void start(std::optional<quint16> port = std::nullopt);

  /** Stops the server. It is started again by start() or a preference change. */
  void stop();

  bool isListening() const;

  /** The port the server listens on, or 0. */
  quint16 port() const;

  /** The error of the last failed attempt to listen, or an empty string. */
  QString lastError() const;

  /**
   * Whether the server runs on the port given to start() instead of following the
   * preferences. This ends when the user changes an MCP preference.
   */
  bool isCommandLineOverride() const;

  /** The URL of the server endpoint, or an empty string if not listening. */
  QString url() const;

  /** The tools offered by the server. */
  McpToolDispatcher& dispatcher();

  /** Calls a tool as the server would and returns the CallToolResult object. */
  QJsonObject callTool(const QString& name, const QJsonObject& arguments = {});

private:
  AppController& m_appController;
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace tb::ui
