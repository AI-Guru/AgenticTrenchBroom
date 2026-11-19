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

#include "mdl/BrushFaceHandle.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/Selection.h"
#include "ui/McpJson.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpSchemas.h"
#include "ui/McpToolDispatcher.h"

namespace tb::ui
{
namespace
{

constexpr auto MaxFaceRefs = 1000;

// select_nodes

McpResult<QJsonObject> selectNodes(McpCallContext& context, const QJsonObject& args)
{
  auto& doc = context.doc;
  auto& map = doc.map;

  const auto mode = args.value("mode").toString("replace");
  const auto hasIds = args.contains("ids");
  const auto hasFaces = args.contains("faces");

  if (mode == u"clear")
  {
    if (hasIds || hasFaces)
    {
      return toolError(
        "invalid_argument",
        "mode clear takes no ids or faces",
        "omit ids and faces",
        {{"pointer", hasIds ? "/ids" : "/faces"}});
    }
    mdl::deselectAll(map);
    return selectionJson(doc.nodes, map);
  }

  if (hasIds == hasFaces)
  {
    return toolError(
      "invalid_argument",
      "pass exactly one of ids or faces",
      "select nodes and faces in separate calls; TrenchBroom selects either nodes or "
      "faces");
  }

  const auto& selection = map.selection();
  if (hasIds)
  {
    auto nodes = resolveTargets(doc, args.value("ids").toArray());
    if (nodes.is_error())
    {
      return errorOf(nodes);
    }

    if (mode == u"add" && selection.hasBrushFaces())
    {
      return toolError(
        "invalid_argument",
        "brush faces are selected, so nodes cannot be added to the selection",
        "use mode replace");
    }

    if (mode == u"remove")
    {
      mdl::deselectNodes(map, nodes.value());
    }
    else
    {
      if (mode == u"replace")
      {
        mdl::deselectAll(map);
      }
      mdl::selectNodes(map, nodes.value());
    }
  }
  else
  {
    auto faces = resolveFaces(doc, args.value("faces").toArray());
    if (faces.is_error())
    {
      return errorOf(faces);
    }

    if (mode == u"add" && selection.hasNodes())
    {
      return toolError(
        "invalid_argument",
        "nodes are selected, so faces cannot be added to the selection",
        "use mode replace");
    }

    if (mode == u"remove")
    {
      mdl::deselectBrushFaces(map, faces.value());
    }
    else
    {
      if (mode == u"replace")
      {
        mdl::deselectAll(map);
      }
      mdl::selectBrushFaces(map, faces.value());
    }
  }

  return selectionJson(doc.nodes, map);
}

McpToolDef selectNodesTool()
{
  return {
    .name = "select_nodes",
    .title = "Select Nodes",
    .description =
      "Changes the user's selection, e.g. to show them nodes or faces. Unlike other "
      "tools, the selection is kept. Other tools take ids and do not need a selection.",
    .inputSchema = schema::object({
      {"ids", schema::nodeIds()},
      {"faces", schema::array(schema::faceRef(), 1, MaxFaceRefs)},
      {"mode",
       schema::stringEnum(
         {"replace", "add", "remove", "clear"},
         "how to change the selection (default: replace)")},
    }),
    .kind = McpToolKind::Mutate,
    .annotations = {.readOnly = false, .destructive = false, .idempotent = true},
    .allowedInBatch = true,
    .keepsSelection = true,
    .run = selectNodes,
  };
}

} // namespace

void registerMcpMapStructureTools(McpToolDispatcher& dispatcher)
{
  dispatcher.add(selectNodesTool());
}

} // namespace tb::ui
