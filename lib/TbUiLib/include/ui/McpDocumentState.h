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

#include "base/Logger.h"
#include "mcp/Tool.h"
#include "ui/McpCheckpoints.h"
#include "ui/McpNodeRegistry.h"

#include "kd/result.h"

#include <deque>
#include <functional>
#include <vector>

namespace tb::mdl
{
class Map;
}

namespace tb::ui
{

template <typename T>
using McpResult = kdl::result<T, mcp::ToolError>;

struct McpLogLine
{
  LogLevel level;
  QString text;
};

/** Returns "debug", "info", "warn" or "error". */
QString mcpLogLevelName(LogLevel level);

/**
 * The most recent warnings and errors logged during MCP calls on one document, oldest
 * first. Returned by get_log.
 */
class McpLogHistory
{
public:
  static constexpr size_t Capacity = 500;

  /** Appends the given lines, dropping the oldest lines beyond the capacity. */
  void append(const std::vector<McpLogLine>& lines);

  const std::deque<McpLogLine>& lines() const;

private:
  std::deque<McpLogLine> m_lines;
};

/**
 * The MCP state of one live map: its node registry, a model of its undo history, its
 * checkpoints and the id under which MCP clients refer to the document. A new state must
 * be created whenever the map is replaced, e.g. when a document is reloaded, so that ids
 * start again at 1.
 */
struct McpDocumentState
{
  McpDocumentState(mdl::Map& map, QString documentId);

  McpDocumentState(const McpDocumentState&) = delete;
  McpDocumentState& operator=(const McpDocumentState&) = delete;

  mdl::Map& map;
  QString documentId;
  /** Connected to the map before any other MCP observer. */
  McpNodeRegistry nodes;
  McpHistoryTracker history;
  McpCheckpointStore checkpoints;
  McpLogHistory logHistory;
};


/** Passed to a map tool's run function. */
struct McpCallContext
{
  McpDocumentState& doc;

  /** Captured warnings and errors logged during the call, or nullptr when headless. */
  std::vector<McpLogLine>* log = nullptr;

  /** Tools append warnings here; they are returned to the client. */
  std::vector<QString> warnings = {};
  /**
   * Set by a Mutate tool to roll back what it did although it succeeded; its result is
   * still returned (batch with dry_run).
   */
  bool discardChanges = false;
};

enum class McpToolKind
{
  /** Inspects a map. Must not change the map, its selection or its undo history. */
  Read,
  /** Changes a map. Runs as one undoable transaction. */
  Mutate,
  /** Undoes or redoes commands. Runs without a transaction. */
  History,
  /** Acts on the application, e.g. opens or closes documents. Needs no document. */
  App,
};

/**
 * Returns the document a tool of the given kind should act on, or a no_document or
 * editor_busy error if there is none or the editor cannot be used right now.
 */
using McpDocumentProvider = std::function<McpResult<McpDocumentState*>(McpToolKind kind)>;

} // namespace tb::ui
