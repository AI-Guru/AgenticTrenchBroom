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

#include "ui/McpLogCapture.h"

#include <QString>

#include <algorithm>

namespace tb::ui
{

McpTeeLogger::McpTeeLogger(Logger* forwardTo)
  : m_forwardTo{forwardTo}
{
}

std::vector<McpLogLine>& McpTeeLogger::lines()
{
  return m_lines;
}

const std::vector<McpLogLine>& McpTeeLogger::lines() const
{
  return m_lines;
}

bool McpTeeLogger::hasErrors() const
{
  const auto lock = std::lock_guard{m_mutex};
  return std::ranges::any_of(
    m_lines, [](const auto& line) { return line.level == LogLevel::Error; });
}

void McpTeeLogger::doLog(const LogLevel level, const std::string_view message)
{
  if (m_forwardTo)
  {
    m_forwardTo->log(level, message);
  }

  if (level == LogLevel::Warn || level == LogLevel::Error)
  {
    const auto lock = std::lock_guard{m_mutex};
    m_lines.push_back(
      {level, QString::fromUtf8(message.data(), qsizetype(message.size()))});
  }
}

} // namespace tb::ui
