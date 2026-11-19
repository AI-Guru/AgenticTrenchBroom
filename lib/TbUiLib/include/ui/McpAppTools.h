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

#include <QString>

#include "ui/McpDocumentState.h"

#include <vector>

namespace tb::ui
{
class AppController;
class MapWindow;
class McpToolDispatcher;

/** The map window that a tool acts on, and the MCP state of its document. */
struct McpTarget
{
  MapWindow* window = nullptr;
  McpDocumentState* doc = nullptr;
};

/**
 * What the application and view tools need from their host, the McpController. All
 * functions must be called on the main thread.
 */
class McpAppHost
{
public:
  virtual ~McpAppHost();

  virtual AppController& appController() = 0;

  /** The dispatcher that the tools are registered with. */
  virtual McpToolDispatcher& dispatcher() = 0;

  /**
   * Returns the window that tools act on and its document state, which is created if
   * necessary. The target is the window pinned by set_active_document, new_map or
   * open_map if it is still open, and otherwise the most recently used map window.
   *
   * Applies the busy gate for the given kind of tool: no_document if no map is open,
   * editor_busy if a modal dialog or popup is open, and for Mutate and History tools also
   * if a transaction is running, a modal tool is active or the selection cannot be
   * changed.
   */
  virtual McpResult<McpTarget> target(McpToolKind kind) = 0;

  /**
   * Returns editor_busy if a modal dialog or popup is open. Used by tools that may run
   * without a document.
   */
  virtual McpResult<void> requireNoModalUi() = 0;

  /** Returns the id of the given window's document, e.g. "d1". Ids are never reused. */
  virtual QString documentId(MapWindow& window) = 0;

  /** Returns the open window whose document has the given id, or nullptr. */
  virtual MapWindow* findDocument(const QString& documentId) = 0;

  /** Makes the given window the target of later calls; nullptr unpins. */
  virtual void pin(MapWindow* window) = 0;

  /** Returns the target window without applying the busy gate, or nullptr. */
  virtual MapWindow* currentTarget() = 0;

  /**
   * Destroys the MCP state of the given window's document. Called before a window is
   * closed.
   */
  virtual void forgetDocumentState(MapWindow& window) = 0;

  /**
   * The warnings and errors logged by the target document during the running call, or
   * nullptr if none are captured.
   */
  virtual const std::vector<McpLogLine>* capturedLog() = 0;
};

/**
 * Registers the document and session tools: list_games, list_documents,
 * set_active_document, new_map, open_map, save_map and close_map.
 */
void registerMcpAppTools(McpToolDispatcher& dispatcher, McpAppHost& host);

} // namespace tb::ui
