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

#include "ui/McpSelectionGuard.h"

#include "mdl/Brush.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/McpNodeRegistry.h"

#include <algorithm>

namespace tb::ui
{

void selectOnly(mdl::Map& map, const std::vector<mdl::Node*>& nodes)
{
  mdl::deselectAll(map);
  if (!nodes.empty())
  {
    mdl::selectNodes(map, nodes);
  }
}

McpSelectionGuard::McpSelectionGuard(mdl::Map& map, const McpNodeRegistry& registry)
  : m_map{map}
  , m_registry{registry}
  , m_nodes{map.selection().nodes}
  , m_currentMaterialName{map.currentMaterialName()}
{
  for (const auto& handle : map.selection().brushFaces)
  {
    m_faces.emplace_back(&handle.node(), handle.faceIndex());
  }
}

void McpSelectionGuard::restore()
{
  m_done = true;

  const auto& editorContext = m_map.editorContext();
  const auto isLive = [&](const mdl::Node* node) {
    // check the registry first, the pointer must not be dereferenced if it is not live
    return m_registry.isLive(node)
           && (node == &m_map.worldNode() || node->isDescendantOf(m_map.worldNode()));
  };

  auto nodes = std::vector<mdl::Node*>{};
  for (auto* node : m_nodes)
  {
    if (isLive(node) && editorContext.selectable(*node))
    {
      nodes.push_back(node);
    }
  }

  auto faces = std::vector<mdl::BrushFaceHandle>{};
  if (m_nodes.empty())
  {
    for (const auto& [brushNode, faceIndex] : m_faces)
    {
      if (
        isLive(brushNode) && faceIndex < brushNode->brush().faceCount()
        && editorContext.selectable(*brushNode, brushNode->brush().face(faceIndex)))
      {
        faces.emplace_back(*brushNode, faceIndex);
      }
    }
  }

  const auto& selection = m_map.selection();
  const auto unchanged = selection.nodes == nodes && selection.brushFaces == faces;
  if (!unchanged)
  {
    mdl::deselectAll(m_map);
    if (!nodes.empty())
    {
      mdl::selectNodes(m_map, nodes);
    }
    else if (!faces.empty())
    {
      mdl::selectBrushFaces(m_map, faces);
    }
  }

  if (m_map.currentMaterialName() != m_currentMaterialName)
  {
    m_map.setCurrentMaterialName(m_currentMaterialName);
  }
}

void McpSelectionGuard::dismiss()
{
  m_done = true;
}

void McpSelectionGuard::cancel()
{
  if (m_map.currentMaterialName() != m_currentMaterialName)
  {
    m_map.setCurrentMaterialName(m_currentMaterialName);
  }
  m_done = true;
}

bool McpSelectionGuard::done() const
{
  return m_done;
}

} // namespace tb::ui
