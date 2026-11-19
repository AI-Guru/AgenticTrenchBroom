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

#include "ui/McpDocumentState.h"

namespace tb::ui
{

McpDocumentState::McpDocumentState(mdl::Map& map_, QString documentId_)
  : map{map_}
  , documentId{std::move(documentId_)}
  , nodes{map_}
  , history{map_}
{
}

QString mcpLogLevelName(const LogLevel level)
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

void McpLogHistory::append(const std::vector<McpLogLine>& lines)
{
  for (const auto& line : lines)
  {
    m_lines.push_back(line);
  }
  while (m_lines.size() > Capacity)
  {
    m_lines.pop_front();
  }
}

const std::deque<McpLogLine>& McpLogHistory::lines() const
{
  return m_lines;
}

} // namespace tb::ui
