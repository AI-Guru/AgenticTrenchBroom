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

#include "base/Logger.h"
#include "ui/McpDocumentState.h"

#include <mutex>
#include <string_view>
#include <vector>

namespace tb::ui
{

/**
 * A logger that forwards every message to another logger, e.g. a map window's console,
 * and records the warnings and errors so that they can be returned to an MCP client.
 *
 * It is installed as a document's target logger for the duration of one tool call.
 * Messages may be logged from worker threads, so recording is synchronized; lines()
 * must only be read while no other thread logs, i.e. after the call.
 */
class McpTeeLogger : public Logger
{
public:
  /** Forwards to the given logger, which may be null. */
  explicit McpTeeLogger(Logger* forwardTo = nullptr);

  /** The recorded warnings and errors, in the order they were logged. */
  std::vector<McpLogLine>& lines();
  const std::vector<McpLogLine>& lines() const;

  /** Whether an error was recorded. */
  bool hasErrors() const;

private:
  void doLog(LogLevel level, std::string_view message) override;

  Logger* m_forwardTo;
  mutable std::mutex m_mutex;
  std::vector<McpLogLine> m_lines;
};

} // namespace tb::ui
