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
  CHECK(doc.history.undoStack().empty());
  // the history model does not know the command that added the brush
  CHECK_FALSE(doc.history.consistent());
  CHECK(doc.checkpoints.size() == 0);
}

} // namespace tb::ui
