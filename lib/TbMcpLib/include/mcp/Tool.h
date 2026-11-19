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

#include "kd/result.h"

#include <functional>
#include <vector>

namespace tb::mcp
{

/**
 * MCP tool annotations. The openWorldHint is always false, since tools only act on the
 * editor.
 */
struct ToolAnnotations
{
  bool readOnly = false;
  bool destructive = false;
  bool idempotent = false;
};

/**
 * An error reported to the client as a tool result with isError set. The code is a
 * stable, machine readable identifier such as "invalid_argument".
 */
struct ToolError
{
  QString code;
  QString message;
  QString hint;
  QJsonObject details;
};

struct ToolOutput
{
  QJsonObject structured;

  /** Additional content, e.g. {"type":"image","data":<b64>,"mimeType":"image/png"} */
  QJsonArray extraContent;
};

using ToolResult = kdl::result<ToolOutput, ToolError>;

struct ToolSpec
{
  QString name;
  QString title;
  QString description;
  QJsonObject inputSchema;
  ToolAnnotations annotations;
  std::function<ToolResult(const QJsonObject& arguments)> handler;
};

/**
 * Holds the tools offered by an MCP server, in registration order.
 */
class ToolRegistry
{
public:
  /**
   * Adds the given tool.
   *
   * Preconditions: no tool with the same name exists, the handler is set, and the input
   * schema only uses keywords supported by validate(). Tools are programmer defined
   * constants, so violating these is a programming error.
   */
  void add(ToolSpec spec);

  /** Returns the tool with the given name, or nullptr if there is none. */
  const ToolSpec* find(QStringView name) const;

  /** Returns all tools in registration order. */
  const std::vector<ToolSpec>& tools() const;

private:
  std::vector<ToolSpec> m_tools;
};

/**
 * Converts the given error into an MCP CallToolResult:
 *
 * {"content":[{"type":"text","text":"<code>: <message>[ Hint: <hint>]"}],
 *  "structuredContent":{"error":{"code","message","hint","details"}}, "isError":true}
 */
QJsonObject toolErrorToCallResult(const ToolError& error);

/**
 * Converts the given output into an MCP CallToolResult:
 *
 * {"content":[{"type":"text","text":"<compact JSON of structured>"}, ...extraContent],
 *  "structuredContent":structured, "isError":false}
 */
QJsonObject toolOutputToCallResult(const ToolOutput& output);

/**
 * Converts the given result into an MCP CallToolResult.
 */
QJsonObject toolResultToCallResult(const ToolResult& result);

} // namespace tb::mcp
