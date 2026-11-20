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

#include <QSet>
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
#include "ui/McpSchemas.h"
#include "ui/McpSelectionGuard.h"

#include "kd/contracts.h"

#include <algorithm>
#include <functional>
#include <optional>
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

/**
 * Records the changes of the node registry for the lifetime of the scope. If the
 * recording was not ended when the scope ends, the call failed; if its transaction was
 * rolled back, the ids of the nodes that were removed and re-added are restored.
 */
class RecordingScope
{
public:
  RecordingScope(
    McpNodeRegistry& registry, QString removalLabel, const bool rolledBackOnFailure)
    : m_registry{registry}
    , m_rolledBackOnFailure{rolledBackOnFailure}
  {
    m_registry.setRemovalLabel(std::move(removalLabel));
    m_registry.beginRecording();
  }

  ~RecordingScope()
  {
    if (m_registry.isRecording())
    {
      if (m_rolledBackOnFailure)
      {
        m_registry.restoreIdsAfterRollback();
      }
      m_registry.endRecording();
    }
    m_registry.setRemovalLabel({});
  }

  McpChangeSet end() { return m_registry.endRecording(); }

  RecordingScope(const RecordingScope&) = delete;
  RecordingScope& operator=(const RecordingScope&) = delete;

private:
  McpNodeRegistry& m_registry;
  bool m_rolledBackOnFailure;
};

void addNothingChangedWarning(std::vector<QString>& warnings)
{
  static const auto warning = QStringLiteral("nothing changed; no undo step was added");
  if (std::ranges::find(warnings, warning) == warnings.end())
  {
    warnings.push_back(warning);
  }
}

QJsonArray logJson(const std::vector<McpLogLine>* log)
{
  auto result = QJsonArray{};
  if (log)
  {
    for (const auto& line : *log)
    {
      result.append(
        QJsonObject{{"level", mcpLogLevelName(line.level)}, {"text", line.text}});
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


// batch

constexpr auto MaxBatchOperations = 200;

struct Placeholder
{
  size_t operation;
  size_t index;
};

/** Parses "$<i>" or "$<i>.<k>". */
std::optional<Placeholder> parsePlaceholder(const QString& str)
{
  if (!str.startsWith(u'$'))
  {
    return std::nullopt;
  }

  const auto parseNumber = [](const QStringView digits) -> std::optional<size_t> {
    if (digits.isEmpty() || digits.size() > 9)
    {
      return std::nullopt;
    }
    for (const auto c : digits)
    {
      if (c < u'0' || c > u'9')
      {
        return std::nullopt;
      }
    }
    return size_t(digits.toULongLong());
  };

  const auto body = QStringView{str}.mid(1);
  const auto dot = body.indexOf(u'.');
  const auto operation = parseNumber(dot < 0 ? body : body.left(dot));
  const auto index = dot < 0 ? std::optional<size_t>{0} : parseNumber(body.mid(dot + 1));
  if (!operation || !index)
  {
    return std::nullopt;
  }
  return Placeholder{*operation, *index};
}

using PlaceholderResolver =
  std::function<McpResult<QString>(const Placeholder&, const QString& pointer)>;

/** Replaces every placeholder string in the given value. */
McpResult<QJsonValue> substitutePlaceholders(
  const QJsonValue& value, const QString& pointer, const PlaceholderResolver& resolve)
{
  if (value.isString())
  {
    if (const auto placeholder = parsePlaceholder(value.toString()))
    {
      return resolve(*placeholder, pointer)
             | kdl::transform([](const QString& id) { return QJsonValue{id}; });
    }
    return value;
  }
  if (value.isArray())
  {
    auto array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i)
    {
      auto element =
        substitutePlaceholders(array[i], QString{"%1/%2"}.arg(pointer).arg(i), resolve);
      if (element.is_error())
      {
        return element;
      }
      array[i] = element.value();
    }
    return array;
  }
  if (value.isObject())
  {
    auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
    {
      auto element =
        substitutePlaceholders(it.value(), pointer + "/" + it.key(), resolve);
      if (element.is_error())
      {
        return element;
      }
      it.value() = element.value();
    }
    return object;
  }
  return value;
}

/**
 * Returns the ids in after that are not in before, in the order of after. Uses a hash
 * set, since a batch or a duplicate_nodes call can create hundreds of thousands of nodes.
 */
std::vector<QString> idsAdded(
  const std::vector<QString>& before, const std::vector<QString>& after)
{
  const auto beforeSet = QSet<QString>{before.begin(), before.end()};
  auto result = std::vector<QString>{};
  for (const auto& id : after)
  {
    if (!beforeSet.contains(id))
    {
      result.push_back(id);
    }
  }
  return result;
}

/** The changes made by one operation of a batch. */
McpChangeSet operationChanges(const McpChangeSet& before, const McpChangeSet& after)
{
  auto removed = idsAdded(before.removed, after.removed);
  // nodes created by an earlier operation and removed by this one vanish from the net
  // change set of the batch
  const auto createdAfter = QSet<QString>{after.created.begin(), after.created.end()};
  auto removedSet = QSet<QString>{removed.begin(), removed.end()};
  for (const auto& id : before.created)
  {
    if (!createdAfter.contains(id) && !removedSet.contains(id))
    {
      removed.push_back(id);
      removedSet.insert(id);
    }
  }
  const auto number = [](const QString& id) { return id.mid(1).toULongLong(); };
  std::ranges::sort(
    removed, [&](const auto& lhs, const auto& rhs) { return number(lhs) < number(rhs); });

  return McpChangeSet{
    .created = idsAdded(before.created, after.created),
    .removed = std::move(removed),
    .modified = idsAdded(before.modified, after.modified),
  };
}

mcp::ToolError schemaError(
  const McpToolDef& def, const mcp::SchemaViolation& violation, const QString& prefix)
{
  auto details = violation.details;
  details["pointer"] = prefix + violation.pointer;
  return toolError(
    "invalid_argument",
    violation.message,
    QString{"see the inputSchema of %1 in tools/list"}.arg(def.name),
    std::move(details));
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
    return schemaError(def, *violation, {});
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

  auto result = [&]() -> mcp::ToolResult {
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
  }();

  // map tools cannot close their document, so it is still alive here
  if (m_log)
  {
    doc.logHistory.append(*m_log);
  }
  return result;
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
  // declared before the transaction, so that the ids are restored after a rollback
  auto recording = RecordingScope{doc.nodes, def.title, true};
  auto context = McpCallContext{.doc = doc, .log = m_log};

  const auto* undoNameBefore = map.undoCommandName();

  auto transaction = mdl::Transaction{map, "MCP: " + def.title.toStdString()};
  auto guard = McpSelectionGuard{map, doc.nodes};

  auto result = def.run(context, args);
  if (result.is_error())
  {
    transaction.cancel();
    guard.cancel();
    return errorOf(result);
  }

  // the tool's own fields take precedence, e.g. the node ids returned by select_nodes
  auto structured = std::move(result).value();
  const auto addEnvelope = [&](const McpChangeSet& changes) {
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

    const auto add = [&](const QString& key, QJsonValue value) {
      if (!structured.contains(key))
      {
        structured[key] = std::move(value);
      }
    };
    add("changes", changes.toJson());
    add("nodes", summaries);
    add("nodes_truncated", touched.size() > McpEnvelopeMaxNodes);
    add("issues", issuesJson(doc, touched));
    add("log", logJson(m_log));
  };

  if (context.discardChanges)
  {
    // a dry run: report what the call did, then roll it back; the recording scope
    // restores the ids of nodes that the rollback re-adds
    addEnvelope(doc.nodes.currentChanges());
    structured["undo_label"] = QJsonValue{};
    structured["dry_run"] = true;
    transaction.cancel();
    guard.cancel();
    return makeOutput(def, std::move(structured), context.warnings);
  }

  if (context.nothingChanged)
  {
    // e.g. selecting and reselecting nodes would otherwise add an empty undo step
    transaction.cancel();
    guard.cancel();
    addEnvelope(McpChangeSet{});
    if (!structured.contains("undo_label"))
    {
      structured["undo_label"] = QJsonValue{};
    }
    addNothingChangedWarning(context.warnings);
    return makeOutput(def, std::move(structured), context.warnings);
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

  // Map::startTransaction also records the repeatable actions of the tool (transforms
  // and duplicates) on the user's repeat stack. Edit > Repeat would replay them on the
  // user's selection, e.g. a rotation around the centre of the agent's nodes, so they
  // are dropped like Map::undoCommand does.
  map.clearRepeatableCommands();

  addEnvelope(recording.end());

  // if nothing was done, the transaction was empty and no undo step was added
  const auto* undoNameAfter = map.undoCommandName();
  const auto addedUndoStep = undoNameAfter && undoNameAfter != undoNameBefore;
  if (!addedUndoStep)
  {
    addNothingChangedWarning(context.warnings);
  }
  if (!structured.contains("undo_label"))
  {
    structured["undo_label"] =
      addedUndoStep ? QJsonValue{QString::fromStdString(*undoNameAfter)} : QJsonValue{};
  }

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
  auto recording = RecordingScope{doc.nodes, def.title, false};
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

void McpToolDispatcher::addBatchTool()
{
  add({
    .name = "batch",
    .title = "Batch",
    .description =
      "Runs up to 200 map tool calls in order as ONE undo step; if any operation fails, "
      "nothing is changed. In arguments, a string \"$<i>\" is replaced by the first id "
      "created by operation i, and \"$<i>.<k>\" by the k-th (from 0), e.g. "
      "{\"tool\":\"create_entity\",\"arguments\":{\"classname\":\"func_door\","
      "\"brush_ids\":[\"$0\"]}}. With dry_run the batch is previewed: results and "
      "issues are returned, then everything is rolled back.",
    .inputSchema = schema::object(
      {
        {"operations",
         schema::array(
           schema::object(
             {
               {"tool",
                schema::string(1, 128, "a map tool, e.g. create_box_brush or get_nodes")},
               {"arguments",
                schema::describe({{"type", "object"}}, "the tool's arguments")},
             },
             {"tool"}),
           1,
           MaxBatchOperations,
           "the tool calls to run in order")},
        {"dry_run",
         schema::boolean(
           "run everything, report the results and issues, then undo it all: the map "
           "is left unchanged (default false)")},
      },
      {"operations"}),
    .kind = McpToolKind::Mutate,
    .annotations = {.readOnly = false, .destructive = true, .idempotent = false},
    .allowedInBatch = false,
    .run = [this](
             McpCallContext& context,
             const QJsonObject& args) { return runBatch(context, args); },
  });
}

McpResult<QJsonObject> McpToolDispatcher::runBatch(
  McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  const auto operations = args.value("operations").toArray();

  struct Operation
  {
    const McpToolDef* def;
    QJsonObject arguments;
  };

  const auto batchableTools = [&]() {
    auto names = QJsonArray{};
    for (const auto& tool : m_tools)
    {
      if (tool->allowedInBatch)
      {
        names.append(tool->name);
      }
    }
    return names;
  };

  // check every operation before anything is run
  auto ops = std::vector<Operation>{};
  for (qsizetype i = 0; i < operations.size(); ++i)
  {
    const auto pointer = QString{"/operations/%1"}.arg(i);
    const auto operation = operations[i].toObject();
    const auto name = operation.value("tool").toString();
    const auto* def = find(name);
    if (!def)
    {
      return toolError(
        "invalid_argument",
        QString{"operation %1: unknown tool '%2'"}.arg(i).arg(name.left(128)),
        "pass the name of a map tool from tools/list",
        {{"pointer", pointer + "/tool"},
         {"failed_index", qint64(i)},
         {"allowed", batchableTools()}});
    }
    if (
      !def->allowedInBatch
      || (def->kind != McpToolKind::Read && def->kind != McpToolKind::Mutate))
    {
      return toolError(
        "invalid_argument",
        QString{"operation %1: %2 cannot be used in a batch"}.arg(i).arg(name),
        "call it separately; batches may contain map reading and editing tools",
        {{"pointer", pointer + "/tool"},
         {"failed_index", qint64(i)},
         {"allowed", batchableTools()}});
    }

    const auto arguments = operation.value("arguments").toObject();
    // placeholders must refer to earlier operations; the ids they stand for are not
    // known yet, so the schema is checked with a well-formed id in their place
    auto checked = substitutePlaceholders(
      arguments,
      pointer + "/arguments",
      [&](const Placeholder& placeholder, const QString& at) -> McpResult<QString> {
        if (placeholder.operation >= size_t(i))
        {
          return toolError(
            "invalid_argument",
            QString{"operation %1: %2 must refer to an earlier operation"}.arg(i).arg(at),
            "\"$0\" is the first id created by operation 0",
            {{"pointer", at}, {"failed_index", qint64(i)}});
        }
        return QStringLiteral("B1");
      });
    if (checked.is_error())
    {
      return errorOf(checked);
    }
    if (const auto violation = mcp::validate(def->inputSchema, checked.value()))
    {
      auto error = schemaError(*def, *violation, pointer + "/arguments");
      error.message = QString{"operation %1 (%2): %3"}.arg(i).arg(name, error.message);
      error.details["failed_index"] = qint64(i);
      return error;
    }
    ops.push_back({def, arguments});
  }

  auto results = QJsonArray{};
  auto createdByOperation = std::vector<std::vector<QString>>{};
  auto changedNothing = true;
  auto selectedNodes = false;

  const auto failure = [&](const qsizetype index, const mcp::ToolError& error) {
    return toolError(
      error.code,
      QString{"operation %1 (%2) failed: %3"}.arg(index).arg(
        ops[size_t(index)].def->name, error.message),
      error.hint,
      {
        {"failed_index", qint64(index)},
        {"tool", ops[size_t(index)].def->name},
        {"error",
         QJsonObject{
           {"code", error.code},
           {"message", error.message},
           {"hint", error.hint},
           {"details", error.details},
         }},
        {"results", results},
      });
  };

  for (qsizetype i = 0; i < qsizetype(ops.size()); ++i)
  {
    const auto& op = ops[size_t(i)];
    auto arguments = substitutePlaceholders(
      op.arguments,
      QString{"/operations/%1/arguments"}.arg(i),
      [&](const Placeholder& placeholder, const QString& at) -> McpResult<QString> {
        const auto& created = createdByOperation[placeholder.operation];
        if (placeholder.index >= created.size())
        {
          return toolError(
            "invalid_argument",
            QString{"%1: operation %2 created %3 nodes, so it has no id %4"}
              .arg(at)
              .arg(placeholder.operation)
              .arg(created.size())
              .arg(placeholder.index),
            "check the results of the earlier operation",
            {{"pointer", at}});
        }
        return created[placeholder.index];
      });
    if (arguments.is_error())
    {
      return failure(i, errorOf(arguments));
    }

    const auto opArgs = arguments.value().toObject();
    if (const auto violation = mcp::validate(op.def->inputSchema, opArgs))
    {
      return failure(
        i, schemaError(*op.def, *violation, QString{"/operations/%1/arguments"}.arg(i)));
    }

    const auto before = doc.nodes.currentChanges();
    auto opChangedNothing = false;
    auto result = runNested(*op.def, context, opArgs, opChangedNothing);
    if (result.is_error())
    {
      return failure(i, errorOf(result));
    }
    const auto changes = operationChanges(before, doc.nodes.currentChanges());
    createdByOperation.push_back(changes.created);

    auto structured = std::move(result).value();
    if (op.def->kind == McpToolKind::Mutate && !structured.contains("changes"))
    {
      structured["changes"] = changes.toJson();
    }
    results.append(QJsonObject{{"tool", op.def->name}, {"result", structured}});
    changedNothing = changedNothing && opChangedNothing;
    selectedNodes = selectedNodes || op.def->keepsSelection;
  }

  if (selectedNodes)
  {
    context.warnings.push_back(
      "select_nodes in a batch only selects for the later operations; the user's "
      "selection is restored after the batch");
  }
  if (changedNothing)
  {
    context.nothingChanged = true;
  }

  if (args.value("dry_run").toBool(false))
  {
    context.discardChanges = true;
  }
  return QJsonObject{{"results", results}};
}

McpResult<QJsonObject> McpToolDispatcher::runNested(
  const McpToolDef& def,
  McpCallContext& outer,
  const QJsonObject& args,
  bool& changedNothing)
{
  changedNothing = false;
  auto& doc = outer.doc;
  auto& map = doc.map;
  auto context = McpCallContext{.doc = doc, .log = outer.log};

  const auto addWarnings = [&](McpResult<QJsonObject> result) {
    if (result.is_success() && !context.warnings.empty())
    {
      auto structured = std::move(result).value();
      auto warnings = QJsonArray{};
      for (const auto& warning : context.warnings)
      {
        warnings.append(warning);
      }
      structured["warnings"] = warnings;
      return McpResult<QJsonObject>{std::move(structured)};
    }
    return result;
  };

  if (def.kind == McpToolKind::Read)
  {
    changedNothing = true;
    return addWarnings(def.run(context, args));
  }

  // like a mutating call, but inside the transaction of the batch
  auto transaction = mdl::Transaction{map, "MCP: " + def.title.toStdString()};
  auto guard = McpSelectionGuard{map, doc.nodes};

  auto result = def.run(context, args);
  if (result.is_error())
  {
    transaction.cancel();
    guard.cancel();
    return result;
  }

  if (context.nothingChanged)
  {
    transaction.cancel();
    guard.cancel();
    changedNothing = true;
    auto structured = std::move(result).value();
    structured["changes"] = McpChangeSet{}.toJson();
    addNothingChangedWarning(context.warnings);
    return addWarnings(std::move(structured));
  }
  // a selection change only applies to the later operations of the batch
  changedNothing = def.keepsSelection;

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
  return addWarnings(std::move(result));
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
    "history_mismatch",
  };
  return codes;
}

bool isMcpToolErrorCode(const QString& code)
{
  return std::ranges::find(mcpToolErrorCodes(), code) != mcpToolErrorCodes().end();
}

} // namespace tb::ui
