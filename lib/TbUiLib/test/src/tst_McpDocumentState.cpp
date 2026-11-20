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

#include "McpTestUtils.h"
#include "mdl/Map.h"
#include "ui/CatchConfig.h"
#include "ui/McpDocumentState.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("McpDocumentState")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create(mdl::QuakeFixtureConfig);
  addBrush(map, vm::bbox3d{{0, 0, 0}, {16, 16, 16}});

  const auto doc = McpDocumentState{map, "d7"};
  CHECK(&doc.map == &map);
  CHECK(doc.documentId == QStringLiteral("d7"));
  CHECK(doc.nodes.liveCount() == 2);
  CHECK(doc.nodes.verifyBijection());
  // the command that added the brush is modelled by a non-MCP placeholder
  CHECK(doc.history.undoStack().size() == 1);
  CHECK_FALSE(doc.history.undoStack().front().mcp);
  CHECK(doc.history.consistent());
  CHECK(doc.checkpoints.size() == 0);
  CHECK(doc.logHistory.lines().empty());
}

TEST_CASE("McpLogHistory")
{
  SECTION("append")
  {
    auto history = McpLogHistory{};
    history.append({{LogLevel::Warn, "a"}, {LogLevel::Error, "b"}});
    REQUIRE(history.lines().size() == 2);
    CHECK(history.lines().front().text == "a");
    CHECK(history.lines().back().level == LogLevel::Error);

    // the oldest lines are dropped
    auto lines = std::vector<McpLogLine>{};
    for (size_t i = 0; i < McpLogHistory::Capacity; ++i)
    {
      lines.push_back({LogLevel::Warn, QString::number(i)});
    }
    history.append(lines);
    CHECK(history.lines().size() == McpLogHistory::Capacity);
    CHECK(history.lines().front().text == "0");
    CHECK(history.lines().back().text == QString::number(McpLogHistory::Capacity - 1));
  }

  SECTION("mcpLogLevelName")
  {
    CHECK(mcpLogLevelName(LogLevel::Debug) == "debug");
    CHECK(mcpLogLevelName(LogLevel::Info) == "info");
    CHECK(mcpLogLevelName(LogLevel::Warn) == "warn");
    CHECK(mcpLogLevelName(LogLevel::Error) == "error");
  }
}

} // namespace tb::ui
