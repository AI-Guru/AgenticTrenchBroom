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

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringView>

#include "mcp/Tool.h"
#include "ui/McpDocumentState.h"
#include "ui/McpPreconditions.h"

#include <functional>
#include <memory>
#include <vector>

namespace tb::ui
{

/** A tool that acts on maps or on the application. */
struct McpToolDef
{
  QString name;
  QString title;
  QString description;
  QJsonObject inputSchema;
  McpToolKind kind = McpToolKind::Read;
  mcp::ToolAnnotations annotations = {};
  bool allowedInBatch = false;

  /** Runs a Read, Mutate or History tool on the document returned by the provider. */
  std::function<McpResult<QJsonObject>(McpCallContext& context, const QJsonObject& args)>
    run = {};

  /** Runs an App tool. App tools need no document. */
  std::function<McpResult<QJsonObject>(const QJsonObject& args)> runApp = {};

  /** Optional additional content for a successful result, e.g. an image. */
  std::function<QJsonArray(const QJsonObject& structured)> extraContent = {};
};

/**
 * Runs MCP tools. Every tool added to the dispatcher is also registered with the given
 * tool registry, so that the MCP server can list and call it. A call goes through these
 * steps:
 *
 * 1. The arguments are validated against the tool's input schema (invalid_argument).
 * 2. A call made while another call is running is rejected (editor_busy, reentrant_call).
 * 3. App tools are run directly. Other tools ask the document provider for the document,
 *    which may fail with no_document or editor_busy.
 * 4. The node registry of the document is reconciled with the node tree.
 * 5. Read tools are run. They must not change the map, its selection or its undo stack.
 *
 * The optional log capture callbacks are called around steps 3 to 5.
 */
class McpToolDispatcher
{
public:
  using BeginLogCapture = std::function<std::vector<McpLogLine>*()>;
  using EndLogCapture = std::function<void()>;

  McpToolDispatcher(
    mcp::ToolRegistry& registry,
    McpDocumentProvider provider,
    BeginLogCapture beginLogCapture = {},
    EndLogCapture endLogCapture = {});

  McpToolDispatcher(const McpToolDispatcher&) = delete;
  McpToolDispatcher& operator=(const McpToolDispatcher&) = delete;

  /**
   * Adds the given tool and registers it with the registry.
   *
   * Preconditions: the name is unique, the schema is supported by mcp::validate, and run
   * is set (runApp for App tools). Tools are programmer defined constants.
   */
  void add(McpToolDef def);

  /** Returns the tool with the given name, or nullptr. */
  const McpToolDef* find(QStringView name) const;

  /** Returns all tools in registration order. */
  std::vector<const McpToolDef*> tools() const;

  /**
   * Calls the given tool as the MCP server would and returns the CallToolResult object.
   * An unknown tool yields an invalid_argument tool error.
   */
  QJsonObject call(const QString& name, const QJsonObject& args);

  /** Returns whether a tool call is running. */
  bool isInCall() const;

private:
  mcp::ToolResult dispatch(const McpToolDef& def, const QJsonObject& args);
  mcp::ToolResult runRead(
    const McpToolDef& def, McpDocumentState& doc, const QJsonObject& args);

  mcp::ToolRegistry& m_registry;
  McpDocumentProvider m_provider;
  BeginLogCapture m_beginLogCapture;
  EndLogCapture m_endLogCapture;
  std::vector<std::unique_ptr<McpToolDef>> m_tools;
  std::vector<McpLogLine>* m_log = nullptr;
  bool m_inCall = false;
};

/** The closed set of error codes that tools may return. */
const std::vector<QString>& mcpToolErrorCodes();

/** Returns whether the given code is in mcpToolErrorCodes(). */
bool isMcpToolErrorCode(const QString& code);

} // namespace tb::ui
