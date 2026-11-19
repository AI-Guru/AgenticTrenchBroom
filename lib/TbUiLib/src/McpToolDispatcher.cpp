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

#include "ui/McpToolDispatcher.h"

#include <QtGlobal>

#include "mcp/JsonSchema.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Issue.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"
#include "ui/McpJson.h"
#include "ui/McpSelectionGuard.h"

#include "kd/contracts.h"

#include <algorithm>
#include <unordered_map>

namespace tb::ui
{
namespace
{

/** Sets a flag for the lifetime of the scope. */
class FlagScope
{
public:
  explicit FlagScope(bool& flag)
    : m_flag{flag}
  {
    m_flag = true;
  }

  ~FlagScope() { m_flag = false; }

  FlagScope(const FlagScope&) = delete;
  FlagScope& operator=(const FlagScope&) = delete;

private:
  bool& m_flag;
};

/** Calls the log capture callbacks around a scope. */
class LogCaptureScope
{
public:
  LogCaptureScope(
    const McpToolDispatcher::BeginLogCapture& begin,
    const McpToolDispatcher::EndLogCapture& end,
    std::vector<McpLogLine>*& log)
    : m_end{end}
    , m_log{log}
  {
    m_log = begin ? begin() : nullptr;
  }

  ~LogCaptureScope()
  {
    m_log = nullptr;
    if (m_end)
    {
      m_end();
    }
  }

  LogCaptureScope(const LogCaptureScope&) = delete;
  LogCaptureScope& operator=(const LogCaptureScope&) = delete;

private:
  const McpToolDispatcher::EndLogCapture& m_end;
  std::vector<McpLogLine>*& m_log;
};

/** The observable state that a read tool must not change. */
struct ReadInvariant
{
  size_t modificationCount;
  std::vector<mdl::Node*> selectedNodes;
  std::vector<mdl::BrushFaceHandle> selectedFaces;
  const std::string* undoCommandName;
  const std::string* redoCommandName;

  explicit ReadInvariant(const mdl::Map& map)
    : modificationCount{map.modificationCount()}
    , selectedNodes{map.selection().nodes}
    , selectedFaces{map.selection().brushFaces}
    , undoCommandName{map.undoCommandName()}
    , redoCommandName{map.redoCommandName()}
  {
  }

  bool operator==(const ReadInvariant&) const = default;
};

/** Disables command collation for the lifetime of the scope. */
class CollationScope
{
public:
  explicit CollationScope(mdl::Map& map)
    : m_map{map}
    , m_wasEnabled{map.isCommandCollationEnabled()}
  {
    m_map.setIsCommandCollationEnabled(false);
  }

  ~CollationScope() { m_map.setIsCommandCollationEnabled(m_wasEnabled); }

  CollationScope(const CollationScope&) = delete;
  CollationScope& operator=(const CollationScope&) = delete;

private:
  mdl::Map& m_map;
  bool m_wasEnabled;
};

/** Records the changes of the node registry for the lifetime of the scope. */
class RecordingScope
{
public:
  RecordingScope(McpNodeRegistry& registry, QString removalLabel)
    : m_registry{registry}
  {
    m_registry.setRemovalLabel(std::move(removalLabel));
    m_registry.beginRecording();
  }

  ~RecordingScope()
  {
    if (m_registry.isRecording())
    {
      m_registry.endRecording();
    }
    m_registry.setRemovalLabel({});
  }

  McpChangeSet end() { return m_registry.endRecording(); }

  RecordingScope(const RecordingScope&) = delete;
  RecordingScope& operator=(const RecordingScope&) = delete;

private:
  McpNodeRegistry& m_registry;
};

QString logLevelName(const LogLevel level)
{
  switch (level)
  {
  case LogLevel::Debug:
    return QStringLiteral("debug");
  case LogLevel::Info:
    return QStringLiteral("info");
  case LogLevel::Warn:
    return QStringLiteral("warn");
  case LogLevel::Error:
    return QStringLiteral("error");
  }
  return QStringLiteral("info");
}

QJsonArray logJson(const std::vector<McpLogLine>* log)
{
  auto result = QJsonArray{};
  if (log)
  {
    for (const auto& line : *log)
    {
      result.append(
        QJsonObject{{"level", logLevelName(line.level)}, {"text", line.text}});
    }
  }
  return result;
}

/** Returns the ids of the created and modified nodes that are live, in id order. */
std::vector<mdl::Node*> touchedNodes(
  const McpDocumentState& doc, const McpChangeSet& changes)
{
  auto result = std::vector<mdl::Node*>{};
  const auto add = [&](const QString& id) {
    if (id == u"world")
    {
      result.push_back(&doc.map.worldNode());
      return;
    }
    const auto lookup = doc.nodes.lookup(id);
    if (lookup.status == McpNodeRegistry::LookupStatus::Live)
    {
      result.push_back(lookup.node);
    }
  };

  // both lists are sorted by number, merge them so that the result is in id order
  auto created = changes.created.begin();
  auto modified = changes.modified.begin();
  if (modified != changes.modified.end() && *modified == u"world")
  {
    add(*modified++);
  }
  const auto number = [](const QString& id) { return id.mid(1).toULongLong(); };
  while (created != changes.created.end() || modified != changes.modified.end())
  {
    if (
      modified == changes.modified.end()
      || (created != changes.created.end() && number(*created) < number(*modified)))
    {
      add(*created++);
    }
    else
    {
      add(*modified++);
    }
  }
  return result;
}

/** Returns the non-hidden issues of the given nodes, at most McpEnvelopeMaxIssues. */
QJsonObject issuesJson(const McpDocumentState& doc, const std::vector<mdl::Node*>& nodes)
{
  auto& map = doc.map;

  // Node::issues caches its result, so it must be called with all validators
  const auto validators = map.worldNode().registeredValidators();
  auto descriptions = std::unordered_map<mdl::IssueType, QString>{};
  for (const auto* validator : validators)
  {
    descriptions.emplace(
      validator->type(), QString::fromStdString(validator->description()));
  }

  auto items = QJsonArray{};
  auto total = qint64(0);
  for (auto* node : nodes)
  {
    for (const auto* issue : node->issues(validators))
    {
      if (issue->hidden())
      {
        continue;
      }
      ++total;
      if (size_t(items.size()) >= McpEnvelopeMaxIssues)
      {
        continue;
      }

      const auto typeIt = descriptions.find(issue->type());
      auto item = QJsonObject{
        {"node", doc.nodes.idOf(*node)},
        {"type", typeIt != descriptions.end() ? typeIt->second : QString{}},
        {"description", QString::fromStdString(issue->description())},
      };
      if (const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(issue))
      {
        item["face"] = qint64(faceIssue->faceIndex());
      }
      if (
        const auto* propertyIssue = dynamic_cast<const mdl::EntityPropertyIssue*>(issue))
      {
        item["property"] = toUnicode(map, propertyIssue->propertyKey());
      }
      items.append(item);
    }
  }

  return QJsonObject{
    {"items", items},
    {"total", total},
    {"truncated", total > items.size()},
  };
}

mcp::ToolOutput makeOutput(
  const McpToolDef& def, QJsonObject structured, const std::vector<QString>& warnings)
{
  if (!warnings.empty())
  {
    auto array = QJsonArray{};
    for (const auto& warning : warnings)
    {
      array.append(warning);
    }
    structured["warnings"] = array;
  }

  auto extraContent = def.extraContent ? def.extraContent(structured) : QJsonArray{};
  return {.structured = std::move(structured), .extraContent = std::move(extraContent)};
}

mcp::ToolResult toToolResult(
  const McpToolDef& def,
  McpResult<QJsonObject> result,
  const std::vector<QString>& warnings)
{
  if (result.is_error())
  {
    return errorOf(result);
  }
  return makeOutput(def, std::move(result).value(), warnings);
}

} // namespace

McpToolDispatcher::McpToolDispatcher(
  mcp::ToolRegistry& registry,
  McpDocumentProvider provider,
  BeginLogCapture beginLogCapture,
  EndLogCapture endLogCapture)
  : m_registry{registry}
  , m_provider{std::move(provider)}
  , m_beginLogCapture{std::move(beginLogCapture)}
  , m_endLogCapture{std::move(endLogCapture)}
{
}

void McpToolDispatcher::add(McpToolDef def)
{
  contract_pre(find(def.name) == nullptr);
  contract_pre(def.kind == McpToolKind::App ? bool(def.runApp) : bool(def.run));

  auto spec = mcp::ToolSpec{
    .name = def.name,
    .title = def.title,
    .description = def.description,
    .inputSchema = def.inputSchema,
    .annotations = def.annotations,
    .handler = [this, name = def.name](const QJsonObject& args) -> mcp::ToolResult {
      const auto* tool = find(name);
      return tool ? dispatch(*tool, args)
                  : mcp::ToolResult{toolError(
                      "invalid_argument", QString{"unknown tool '%1'"}.arg(name))};
    },
  };

  m_tools.push_back(std::make_unique<McpToolDef>(std::move(def)));
  m_registry.add(std::move(spec));
}

const McpToolDef* McpToolDispatcher::find(const QStringView name) const
{
  const auto it =
    std::ranges::find_if(m_tools, [&](const auto& tool) { return tool->name == name; });
  return it != m_tools.end() ? it->get() : nullptr;
}

std::vector<const McpToolDef*> McpToolDispatcher::tools() const
{
  auto result = std::vector<const McpToolDef*>{};
  result.reserve(m_tools.size());
  for (const auto& tool : m_tools)
  {
    result.push_back(tool.get());
  }
  return result;
}

QJsonObject McpToolDispatcher::call(const QString& name, const QJsonObject& args)
{
  const auto* spec = m_registry.find(name);
  return spec ? mcp::toolResultToCallResult(spec->handler(args))
              : mcp::toolErrorToCallResult(
                  toolError("invalid_argument", QString{"unknown tool '%1'"}.arg(name)));
}

bool McpToolDispatcher::isInCall() const
{
  return m_inCall;
}

mcp::ToolResult McpToolDispatcher::dispatch(
  const McpToolDef& def, const QJsonObject& args)
{
  if (const auto violation = mcp::validate(def.inputSchema, args))
  {
    auto details = violation->details;
    details["pointer"] = violation->pointer;
    return toolError(
      "invalid_argument",
      violation->message,
      QString{"see the inputSchema of %1 in tools/list"}.arg(def.name),
      std::move(details));
  }

  if (m_inCall)
  {
    return toolError(
      "editor_busy",
      "another tool call is still running",
      "retry in a moment",
      {{"reason", "reentrant_call"}});
  }

  const auto inCall = FlagScope{m_inCall};
  const auto logCapture = LogCaptureScope{m_beginLogCapture, m_endLogCapture, m_log};

  if (def.kind == McpToolKind::App)
  {
    return toToolResult(def, def.runApp(args), {});
  }

  auto docResult = m_provider(def.kind);
  if (docResult.is_error())
  {
    return errorOf(docResult);
  }
  auto& doc = *docResult.value();

  doc.nodes.reconcile();

  switch (def.kind)
  {
  case McpToolKind::Read:
    return runRead(def, doc, args);
  case McpToolKind::Mutate:
    return runMutate(def, doc, args);
  case McpToolKind::History:
    return runHistory(def, doc, args);
  case McpToolKind::App:
    break;
  }

  return toolError(
    "not_supported",
    QString{"%1 cannot be run by this version of the dispatcher"}.arg(def.name));
}

mcp::ToolResult McpToolDispatcher::runRead(
  const McpToolDef& def, McpDocumentState& doc, const QJsonObject& args)
{
  auto context = McpCallContext{.doc = doc, .log = m_log};

  const auto before = ReadInvariant{doc.map};
  auto result = def.run(context, args);
  if (ReadInvariant{doc.map} != before)
  {
    qCritical("MCP read tool %s changed the map", qPrintable(def.name));
  }

  return toToolResult(def, std::move(result), context.warnings);
}

mcp::ToolResult McpToolDispatcher::runMutate(
  const McpToolDef& def, McpDocumentState& doc, const QJsonObject& args)
{
  auto& map = doc.map;

  // the provider checks this too, but a tool must never be run inside the user's
  // transaction: cancelling it would also roll back what the tool did
  if (const auto busy = requireNoActiveTransaction(map); busy.is_error())
  {
    return errorOf(busy);
  }

  const auto collation = CollationScope{map};
  auto recording = RecordingScope{doc.nodes, def.title};
  auto context = McpCallContext{.doc = doc, .log = m_log};

  const auto* undoNameBefore = map.undoCommandName();

  auto transaction = mdl::Transaction{map, "MCP: " + def.title.toStdString()};
  auto guard = McpSelectionGuard{map, doc.nodes};

  auto result = def.run(context, args);
  if (result.is_error())
  {
    guard.dismiss();
    transaction.cancel();
    return errorOf(result);
  }

  if (def.keepsSelection)
  {
    guard.dismiss();
  }
  else
  {
    guard.restore();
  }

  if (!transaction.commit())
  {
    return toolError(
      "operation_failed",
      QString{"%1 could not be completed: linked groups could not be updated"}.arg(
        def.name),
      "check the log for details",
      {{"log", logJson(m_log)}});
  }

  const auto changes = recording.end();
  const auto touched = touchedNodes(doc, changes);

  auto summaries = QJsonArray{};
  for (const auto* node : touched)
  {
    if (size_t(summaries.size()) >= McpEnvelopeMaxNodes)
    {
      break;
    }
    summaries.append(nodeSummary(doc.nodes, map, *node));
  }

  // if nothing was done, the transaction was empty and no undo step was added
  const auto* undoNameAfter = map.undoCommandName();
  const auto addedUndoStep = undoNameAfter && undoNameAfter != undoNameBefore;
  if (!addedUndoStep)
  {
    context.warnings.push_back("nothing changed; no undo step was added");
  }

  // the tool's own fields take precedence, e.g. the node ids returned by select_nodes
  auto structured = std::move(result).value();
  const auto addEnvelope = [&](const QString& key, QJsonValue value) {
    if (!structured.contains(key))
    {
      structured[key] = std::move(value);
    }
  };
  addEnvelope("changes", changes.toJson());
  addEnvelope("nodes", summaries);
  addEnvelope("nodes_truncated", touched.size() > McpEnvelopeMaxNodes);
  addEnvelope("issues", issuesJson(doc, touched));
  addEnvelope("log", logJson(m_log));
  addEnvelope(
    "undo_label",
    addedUndoStep ? QJsonValue{QString::fromStdString(*undoNameAfter)} : QJsonValue{});

  return makeOutput(def, std::move(structured), context.warnings);
}

mcp::ToolResult McpToolDispatcher::runHistory(
  const McpToolDef& def, McpDocumentState& doc, const QJsonObject& args)
{
  auto& map = doc.map;
  if (const auto busy = requireNoActiveTransaction(map); busy.is_error())
  {
    return errorOf(busy);
  }

  const auto collation = CollationScope{map};
  auto recording = RecordingScope{doc.nodes, def.title};
  auto context = McpCallContext{.doc = doc, .log = m_log};

  auto result = def.run(context, args);
  if (result.is_error())
  {
    return errorOf(result);
  }

  auto structured = std::move(result).value();
  if (!structured.contains("changes"))
  {
    structured["changes"] = recording.end().toJson();
  }
  if (!structured.contains("log"))
  {
    structured["log"] = logJson(m_log);
  }
  return makeOutput(def, std::move(structured), context.warnings);
}

const std::vector<QString>& mcpToolErrorCodes()
{
  static const auto codes = std::vector<QString>{
    "invalid_argument",
    "no_document",
    "unknown_document",
    "editor_busy",
    "unknown_node",
    "stale_node",
    "wrong_node_type",
    "node_not_selectable",
    "invalid_parent",
    "face_not_found",
    "out_of_world_bounds",
    "geometry_invalid",
    "definition_type_mismatch",
    "definition_not_found",
    "unknown_game",
    "unsupported_format",
    "path_required",
    "file_exists",
    "unsaved_changes",
    "io_error",
    "parse_failed",
    "unknown_checkpoint",
    "checkpoint_exists",
    "limit_exceeded",
    "nothing_to_undo",
    "nothing_to_redo",
    "foreign_command",
    "no_open_group",
    "not_supported",
    "view_unavailable",
    "render_failed",
    "operation_failed",
  };
  return codes;
}

bool isMcpToolErrorCode(const QString& code)
{
  return std::ranges::find(mcpToolErrorCodes(), code) != mcpToolErrorCodes().end();
}

} // namespace tb::ui
