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

#include <QJsonArray>
#include <QSet>

#include "mdl/Map.h"
#include "ui/McpCheckpoints.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

#include <algorithm>
#include <limits>
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

// checkpoint_revert

mcp::ToolError historyMismatch(
  const QString& message, const QString& reason, QJsonObject details = {})
{
  details["reason"] = reason;
  return toolError(
    "history_mismatch",
    message,
    "use undo or redo, or compare with checkpoint_diff and repair the map by hand",
    std::move(details));
}

/**
 * Gives nodes that the undo steps re-added the ids they had when the checkpoint was
 * taken: undoing a delete re-adds the node, which gets a new id. Nodes are matched by
 * their id free content digests, as checkpoint_diff does for reidentified nodes.
 */
size_t restoreCheckpointIds(McpDocumentState& doc, const McpCheckpoint& checkpoint)
{
  const auto current = takeCheckpoint(doc.nodes, doc.map, doc.history, "current");
  const auto diff =
    diffCheckpoints(checkpoint, current, std::numeric_limits<size_t>::max());

  const auto createdIds = doc.nodes.currentChanges().created;
  const auto created = QSet<QString>{createdIds.begin(), createdIds.end()};

  auto count = size_t(0);
  for (const auto& pair : diff.value("reidentified").toArray())
  {
    const auto from = pair.toObject().value("from").toString();
    const auto to = pair.toObject().value("to").toString();
    const auto lookup = doc.nodes.lookup(to);
    if (
      created.contains(to) && lookup.status == McpNodeRegistry::LookupStatus::Live
      && doc.nodes.reassignId(*lookup.node, from))
    {
      ++count;
    }
  }
  return count;
}

McpResult<QJsonObject> checkpointRevert(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto name = args.value("name").toString();
  const auto* checkpoint = doc.checkpoints.find(name);
  if (!checkpoint)
  {
    return toolError(
      "unknown_checkpoint",
      QString{"there is no checkpoint '%1'"}.arg(name),
      "call checkpoint_list for the existing checkpoints",
      {{"name", name},
       {"available", QJsonArray::fromStringList(doc.checkpoints.names())}});
  }

  doc.history.sync();
  if (!checkpoint->historyConsistent || !doc.history.consistent())
  {
    return historyMismatch(
      "the undo history cannot be matched to the checkpoint",
      "history_unknown",
      {{"name", name}});
  }

  // the checkpoint's history must be the bottom of the current one
  const auto serials = doc.history.undoSerials();
  const auto& prefix = checkpoint->historySerials;
  if (
    prefix.size() > serials.size()
    || !std::equal(prefix.begin(), prefix.end(), serials.begin()))
  {
    return historyMismatch(
      QString{"the checkpoint '%1' is not in the current undo history: steps before it "
              "were undone or replaced"}
        .arg(name),
      "not_an_ancestor",
      {{"name", name}});
  }

  // check everything before undoing anything
  const auto& undoStack = doc.history.undoStack();
  auto labels = std::vector<std::string>{};
  for (auto i = undoStack.size(); i > prefix.size(); --i)
  {
    const auto& entry = undoStack[i - 1];
    if (!entry.mcp)
    {
      return historyMismatch(
        QString{"'%1' was done after the checkpoint and was not made by an MCP tool"}.arg(
          QString::fromStdString(entry.label)),
        "user_command",
        {{"name", name}, {"undo_label", QString::fromStdString(entry.label)}});
    }
    labels.push_back(entry.label);
  }

  auto undone = QJsonArray{};
  for (const auto& label : labels)
  {
    const auto* top = map.undoCommandName();
    if (!canUndo(map) || !top || *top != label)
    {
      return toolError(
        "operation_failed",
        QString{"the undo history changed while reverting to '%1'"}.arg(name),
        "call get_map_info and checkpoint_diff",
        {{"undone", undone}});
    }

    map.undoCommand();

    const auto* moved = map.redoCommandName();
    if (!moved || *moved != label)
    {
      return toolError(
        "operation_failed",
        QString{"'%1' could not be undone"}.arg(QString::fromStdString(label)),
        "check the log",
        {{"undone", undone}});
    }
    undone.append(QString::fromStdString(label));
  }

  const auto reidentified = restoreCheckpointIds(doc, *checkpoint);

  const auto after = takeCheckpoint(doc.nodes, map, doc.history, "current");
  const auto diff = diffCheckpoints(*checkpoint, after, 0);
  const auto remaining = diff.value("added_count").toInteger()
                         + diff.value("removed_count").toInteger()
                         + diff.value("modified_count").toInteger()
                         + diff.value("reidentified_count").toInteger();
  if (remaining > 0 || !diff.value("content_identical").toBool())
  {
    context.warnings.push_back(
      QString{"the map differs from the checkpoint in %1 nodes; see checkpoint_diff"}.arg(
        remaining));
  }

  auto result = historyState(map);
  result["checkpoint"] = name;
  result["undone"] = undone;
  result["reidentified"] = qint64(reidentified);
  result["content_identical"] = diff.value("content_identical");
  result["identical"] = remaining == 0 && diff.value("content_identical").toBool();
  return result;
}

McpToolDef checkpointRevertTool()
{
  auto nameSchema = schema::string(1, 64, "the checkpoint to revert to");
  nameSchema["pattern"] = "^[A-Za-z0-9_.-]{1,64}$";
  return {
    .name = "checkpoint_revert",
    .title = "Revert To Checkpoint",
    .description =
      "Undoes every MCP step made since the checkpoint was created, so that the map "
      "and the node ids are as they were (checkpoint_diff then reports no changes). "
      "Refuses with history_mismatch if a step was made by the user or if steps before "
      "the checkpoint were undone; the undone steps can be redone.",
    .inputSchema = schema::object({{"name", nameSchema}}, {"name"}),
    .kind = McpToolKind::History,
    .annotations = {.readOnly = false, .destructive = true, .idempotent = false},
    .allowedInBatch = false,
    .run = checkpointRevert,
  };
}

} // namespace

void registerMcpMapHistoryTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(undoTool());
  dispatcher.add(redoTool());
  dispatcher.add(checkpointRevertTool());
}

} // namespace tb::ui
