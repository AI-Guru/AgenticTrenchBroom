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

#include <string>
#include <utility>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class Map;
class Node;
} // namespace tb::mdl

namespace tb::ui
{
class McpNodeRegistry;

/**
 * Saves the selection of a map and restores it later. Replaces mdl::PushSelection for
 * MCP tools: restoring checks every saved node against the registry before touching it,
 * so nodes that were deleted in the meantime are skipped instead of being dereferenced.
 *
 * The guard does nothing on destruction. Call restore() before committing a transaction,
 * or dismiss() if the transaction is cancelled, which restores the selection anyway.
 */
class McpSelectionGuard
{
public:
  McpSelectionGuard(mdl::Map& map, const McpNodeRegistry& registry);

  McpSelectionGuard(const McpSelectionGuard&) = delete;
  McpSelectionGuard& operator=(const McpSelectionGuard&) = delete;

  /**
   * Replaces the current selection with the saved nodes that are still live and
   * selectable, or, if no nodes were saved, with the saved brush faces whose brush is
   * live and which are still selectable. Also restores the current material, which
   * selecting faces changes.
   */
  void restore();

  /** Marks the guard as done without changing the selection. */
  void dismiss();

  bool done() const;

private:
  mdl::Map& m_map;
  const McpNodeRegistry& m_registry;
  std::vector<mdl::Node*> m_nodes;
  std::vector<std::pair<mdl::BrushNode*, size_t>> m_faces;
  std::string m_currentMaterialName;
  bool m_done = false;
};

} // namespace tb::ui
