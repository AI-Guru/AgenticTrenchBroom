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

#include <functional>

namespace tb::ui
{
class MapWindow;
class McpAppHost;
class McpToolDispatcher;

/**
 * Registers the view tools: get_camera, set_camera, focus_nodes, screenshot and
 * screenshot_set. They need a map window with views and do nothing headless.
 */
void registerMcpViewTools(McpToolDispatcher& dispatcher, McpAppHost& host);

/**
 * Sets a function that the screenshot tools call right before each view is rendered,
 * also on headless platforms where nothing is rendered. For tests that check the
 * editor state during a shot. Pass an empty function to remove it.
 */
void setMcpScreenshotHookForTesting(std::function<void(MapWindow&)> hook);

} // namespace tb::ui
