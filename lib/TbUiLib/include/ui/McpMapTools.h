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

namespace tb::ui
{
class McpToolDispatcher;

/** Registers all map tools with the given dispatcher. */
void registerMcpMapTools(McpToolDispatcher& dispatcher);

/**
 * Registers the read-only inspection tools: get_map_info, get_scene_tree, get_nodes,
 * get_brush_geometry, find_nodes and get_selection.
 */
void registerMcpMapReadTools(McpToolDispatcher& dispatcher);

/**
 * Registers the evaluation and checkpoint tools: validate_map, get_map_stats,
 * checkpoint_create, checkpoint_diff, checkpoint_list, checkpoint_delete and
 * export_map_text.
 */
void registerMcpMapEvalTools(McpToolDispatcher& dispatcher);

} // namespace tb::ui
