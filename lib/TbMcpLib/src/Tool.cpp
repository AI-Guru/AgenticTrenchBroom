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

#include "mcp/Tool.h"

#include <QJsonDocument>

#include "mcp/JsonSchema.h"

#include "kd/contracts.h"

#include <algorithm>
#include <variant>

namespace tb::mcp
{

void ToolRegistry::add(ToolSpec spec)
{
  contract_pre(find(spec.name) == nullptr);
  contract_pre(spec.handler != nullptr);
  contract_pre(usesOnlySupportedKeywords(spec.inputSchema));

  m_tools.push_back(std::move(spec));
}

const ToolSpec* ToolRegistry::find(const QStringView name) const
{
  const auto it =
    std::ranges::find_if(m_tools, [&](const auto& tool) { return tool.name == name; });
  return it != m_tools.end() ? &*it : nullptr;
}

const std::vector<ToolSpec>& ToolRegistry::tools() const
{
  return m_tools;
}

QJsonObject toolErrorToCallResult(const ToolError& error)
{
  auto text = error.code + u": " + error.message;
  if (!error.hint.isEmpty())
  {
    text += u" Hint: " + error.hint;
  }

  return QJsonObject{
    {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}},
    {"structuredContent",
     QJsonObject{
       {"error",
        QJsonObject{
          {"code", error.code},
          {"message", error.message},
          {"hint", error.hint},
          {"details", error.details},
        }},
     }},
    {"isError", true},
  };
}

QJsonObject toolOutputToCallResult(const ToolOutput& output)
{
  const auto text =
    QString::fromUtf8(QJsonDocument{output.structured}.toJson(QJsonDocument::Compact));

  auto content = QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}};
  for (const auto& item : output.extraContent)
  {
    content.push_back(item);
  }

  return QJsonObject{
    {"content", content},
    {"structuredContent", output.structured},
    {"isError", false},
  };
}

QJsonObject toolResultToCallResult(const ToolResult& result)
{
  return result.is_success()
           ? toolOutputToCallResult(result.value())
           : std::visit(
               [](const ToolError& e) { return toolErrorToCallResult(e); },
               result.error());
}

} // namespace tb::mcp
