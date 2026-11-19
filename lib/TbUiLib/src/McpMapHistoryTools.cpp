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

#include "mdl/Map.h"
#include "ui/McpCheckpoints.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

#include <string>

namespace tb::ui
{
namespace
{

enum class HistoryDirection
{
  Undo,
  Redo,
};

QJsonValue labelOrNull(const std::string* label)
{
  return label ? QJsonValue{QString::fromStdString(*label)} : QJsonValue{};
}

QJsonObject historyState(const mdl::Map& map)
{
  return QJsonObject{
    {"can_undo", canUndo(map)},
    {"undo_label", labelOrNull(map.undoCommandName())},
    {"can_redo", canRedo(map)},
    {"redo_label", labelOrNull(map.redoCommandName())},
  };
}

McpResult<QJsonObject> runHistory(
  McpCallContext& context, const QJsonObject& args, const HistoryDirection direction)
{
  auto& map = context.doc.map;
  const auto undo = direction == HistoryDirection::Undo;
  const auto count = std::max(size_t(1), sizeArgument(args, "count", 1));
  const auto allowUserCommands = args.value("allow_user_commands").toBool(false);

  // never use Map::canRedoCommand, it checks the undo stack (see canRedo)
  const auto canStep = [&]() { return undo ? canUndo(map) : canRedo(map); };
  const auto label = [&]() {
    return undo ? map.undoCommandName() : map.redoCommandName();
  };

  auto done = QJsonArray{};
  const auto withDone = [&](mcp::ToolError error) {
    error.details[undo ? "undone" : "redone"] = done;
    return error;
  };

  if (args.contains("expect_label"))
  {
    const auto expected = args.value("expect_label").toString();
    const auto* actual = canStep() ? label() : nullptr;
    if (!actual || QString::fromStdString(*actual) != expected)
    {
      return toolError(
        "invalid_argument",
        QString{"the next %1 step is %2, not '%3'"}.arg(
          undo ? "undo" : "redo",
          actual ? "'" + QString::fromStdString(*actual) + "'" : "nothing",
          expected),
        "call get_map_info for the undo and redo labels",
        {{"pointer", "/expect_label"},
         {undo ? "undo_label" : "redo_label", labelOrNull(actual)}});
    }
  }

  for (size_t i = 0; i < count; ++i)
  {
    if (!canStep())
    {
      return withDone(toolError(
        undo ? "nothing_to_undo" : "nothing_to_redo",
        done.isEmpty() ? QString{"there is nothing to %1"}.arg(undo ? "undo" : "redo")
                       : QString{"%1 of %2 steps were %3; there is nothing more to %4"}
                           .arg(done.size())
                           .arg(count)
                           .arg(undo ? "undone" : "redone", undo ? "undo" : "redo"),
        "call get_map_info for the undo and redo state"));
    }

    const auto currentLabel = *label();
    if (!allowUserCommands && !McpHistoryTracker::isMcpLabel(currentLabel))
    {
      return withDone(toolError(
        "foreign_command",
        QString{"the next %1 step '%2' was not made by an MCP tool"}.arg(
          undo ? "undo" : "redo", QString::fromStdString(currentLabel)),
        "pass allow_user_commands:true to also undo or redo the user's edits",
        {{undo ? "undo_label" : "redo_label", QString::fromStdString(currentLabel)}}));
    }

    if (undo)
    {
      map.undoCommand();
    }
    else
    {
      map.redoCommand();
    }

    // a command that fails to undo or redo is dropped from the history
    const auto* moved = undo ? map.redoCommandName() : map.undoCommandName();
    if (!moved || *moved != currentLabel)
    {
      return withDone(toolError(
        "operation_failed",
        QString{"'%1' could not be %2"}.arg(
          QString::fromStdString(currentLabel), undo ? "undone" : "redone"),
        "check the log"));
    }
    done.append(QString::fromStdString(currentLabel));
  }

  auto result = historyState(map);
  result[undo ? "undone" : "redone"] = done;
  return result;
}

QJsonObject historySchema(const QString& verb)
{
  return schema::object({
    {"count",
     schema::integer(1, 100, QString{"number of steps to %1 (default 1)"}.arg(verb))},
    {"allow_user_commands",
     schema::boolean(
       QString{"also %1 steps that were not made by MCP tools (default false)"}.arg(
         verb))},
    {"expect_label",
     schema::string(
       0,
       1024,
       QString{"fail unless the next step to %1 has this label, e.g. \"MCP: Create Box "
               "Brush\""}
         .arg(verb))},
  });
}

McpToolDef undoTool()
{
  return {
    .name = "undo",
    .title = "Undo",
    .description =
      "Undoes the last count steps. Only steps made by MCP tools (labelled \"MCP: ...\") "
      "are undone unless allow_user_commands is true. Ids of nodes restored by undoing a "
      "delete are new.",
    .inputSchema = historySchema("undo"),
    .kind = McpToolKind::History,
    .annotations = {.readOnly = false, .destructive = true, .idempotent = false},
    .allowedInBatch = false,
    .run =
      [](McpCallContext& context, const QJsonObject& args) {
        return runHistory(context, args, HistoryDirection::Undo);
      },
  };
}

McpToolDef redoTool()
{
  return {
    .name = "redo",
    .title = "Redo",
    .description =
      "Redoes the last count undone steps. Only steps made by MCP tools are redone "
      "unless allow_user_commands is true.",
    .inputSchema = historySchema("redo"),
    .kind = McpToolKind::History,
    .annotations = {.readOnly = false, .destructive = false, .idempotent = false},
    .allowedInBatch = false,
    .run =
      [](McpCallContext& context, const QJsonObject& args) {
        return runHistory(context, args, HistoryDirection::Redo);
      },
  };
}

} // namespace

void registerMcpMapHistoryTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(undoTool());
  dispatcher.add(redoTool());
}

} // namespace tb::ui
