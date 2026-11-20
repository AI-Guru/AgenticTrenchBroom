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

#include <QJsonArray>
#include <QJsonObject>

#include "McpTestUtils.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PasteType.h"
#include "mdl/PatchNode.h"
#include "mdl/Transaction.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpCheckpoints.h"

#include "kd/overload.h"

#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

QStringList strings(const QJsonValue& value)
{
  auto result = QStringList{};
  for (const auto& item : value.toArray())
  {
    result.push_back(item.toString());
  }
  return result;
}

QStringList itemTypes(const QJsonObject& validation, const QString& node = {})
{
  auto result = QStringList{};
  for (const auto& item : validation.value("items").toArray())
  {
    const auto object = item.toObject();
    if (node.isEmpty() || object.value("node").toString() == node)
    {
      result.push_back(object.value("type").toString());
    }
  }
  return result;
}

mdl::IssueType issueType(mdl::Map& map, const std::string& description)
{
  for (const auto* validator : map.worldNode().registeredValidators())
  {
    if (validator->description() == description)
    {
      return validator->type();
    }
  }
  FAIL("no validator " << description);
  return 0;
}

mdl::EntityNode* addEntityWithoutClassname(mdl::Map& map)
{
  auto entity = mdl::Entity{};
  entity.addOrUpdateProperty("origin", "8 8 8");
  auto* node = mdl::addNode(
    map, mdl::parentForNodes(map), std::make_unique<mdl::EntityNode>(std::move(entity)));
  REQUIRE(node != nullptr);
  return node;
}

void rotate45(mdl::Map& map, mdl::Node* node)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, {node});
  REQUIRE(mdl::rotateSelection(map, {0, 0, 0}, {0, 0, 1}, vm::to_radians(45.0)));
  mdl::deselectAll(map);
}


/**
 * Adds the floor (-z), ceiling (+z) and walls around the given interior, 16 units thick,
 * except for the sides named in omit.
 */
std::vector<mdl::BrushNode*> addShell(
  mdl::Map& map, const vm::bbox3d& inner, const std::vector<std::string>& omit = {})
{
  const auto& lo = inner.min;
  const auto& hi = inner.max;
  const auto boxes = std::vector<std::pair<std::string, vm::bbox3d>>{
    {"-z", {{lo.x() - 16, lo.y() - 16, lo.z() - 16}, {hi.x() + 16, hi.y() + 16, lo.z()}}},
    {"+z", {{lo.x() - 16, lo.y() - 16, hi.z()}, {hi.x() + 16, hi.y() + 16, hi.z() + 16}}},
    {"-x", {{lo.x() - 16, lo.y() - 16, lo.z()}, {lo.x(), hi.y() + 16, hi.z()}}},
    {"+x", {{hi.x(), lo.y() - 16, lo.z()}, {hi.x() + 16, hi.y() + 16, hi.z()}}},
    {"-y", {{lo.x(), lo.y() - 16, lo.z()}, {hi.x(), lo.y(), hi.z()}}},
    {"+y", {{lo.x(), hi.y(), lo.z()}, {hi.x(), hi.y() + 16, hi.z()}}},
  };

  auto result = std::vector<mdl::BrushNode*>{};
  for (const auto& [side, box] : boxes)
  {
    if (std::ranges::find(omit, side) == omit.end())
    {
      result.push_back(addBrush(map, box));
    }
  }
  return result;
}

QJsonArray jsonVec(const double x, const double y, const double z)
{
  return QJsonArray{x, y, z};
}
} // namespace

TEST_CASE("McpMapEvalTools")
{
  auto f = McpMapToolsFixture{};
  auto& map = f.map;

  SECTION("validate_map")
  {
    SECTION("an empty map only has the world's issue")
    {
      // the fixture has no entity definitions, so the world has no definition
      const auto result = f.ok("validate_map");
      CHECK(result.value("total") == QJsonValue{1});
      CHECK(
        compactJson(result.value("items"))
        == QStringLiteral(
          R"([{"description":"worldspawn not found in entity definitions","hidden":false,"line":null,"node":"world","type":"Missing entity definition"}])"));
      CHECK(
        compactJson(result.value("by_type"))
        == QStringLiteral(R"({"Missing entity definition":1})"));
      CHECK(result.value("truncated") == QJsonValue{false});
      CHECK(result.value("next_offset").isNull());

      // the validators are listed in registration order
      const auto validators = result.value("validators").toArray();
      const auto registered = map.worldNode().registeredValidators();
      REQUIRE(size_t(validators.size()) == registered.size());
      for (size_t i = 0; i < registered.size(); ++i)
      {
        const auto validator = validators[qsizetype(i)].toObject();
        CHECK(
          validator.value("description")
          == QJsonValue{QString::fromStdString(registered[i]->description())});
        CHECK(validator.value("type_bit") == QJsonValue{qint64(registered[i]->type())});
      }
    }

    SECTION("reports an entity without classname")
    {
      auto* entityNode = addEntityWithoutClassname(map);
      const auto result = f.ok("validate_map");

      const auto items = result.value("items").toArray();
      REQUIRE_FALSE(items.isEmpty());
      CHECK(itemTypes(result, f.id(*entityNode)).contains("Missing entity classname"));

      const auto item = items[1].toObject();
      CHECK(item.value("node") == QJsonValue{f.id(*entityNode)});
      CHECK(item.value("hidden") == QJsonValue{false});
      CHECK(item.contains("line"));
      CHECK(item.contains("description"));
      CHECK_FALSE(item.contains("seq_id"));
      CHECK(
        result.value("by_type").toObject().value("Missing entity classname")
        == QJsonValue{1});
    }

    SECTION("reports non-integer vertices after a 45 degree rotation")
    {
      auto* brushNode = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      CHECK(itemTypes(f.ok("validate_map"), f.id(*brushNode)).isEmpty());

      rotate45(map, brushNode);
      const auto result = f.ok("validate_map");
      CHECK(itemTypes(result, f.id(*brushNode)) == QStringList{"Non-integer vertices"});
      CHECK(result.value("total") == QJsonValue{2});
    }

    SECTION("reports the property of property issues and the face of face issues")
    {
      auto* entityNode = addPointEntity(map, "info_null", {0, 0, 0});
      mdl::selectNodes(map, {entityNode});
      REQUIRE(mdl::setEntityProperty(map, "target", ""));
      mdl::deselectAll(map);

      const auto result = f.ok(
        "validate_map",
        QJsonObject{
          {"under", QJsonArray{f.id(*entityNode)}},
          {"types", QJsonArray{"Empty property value", "Missing entity definition"}}});
      const auto items = result.value("items").toArray();
      REQUIRE(items.size() == 2);
      // sorted by node, then validator registration order
      CHECK(items[0].toObject().value("type") == QJsonValue{"Missing entity definition"});
      CHECK(items[1].toObject().value("type") == QJsonValue{"Empty property value"});
      CHECK(items[1].toObject().value("property") == QJsonValue{"target"});
      CHECK_FALSE(items[1].toObject().contains("face"));
    }

    SECTION("orders issues by node")
    {
      const auto scene = populateTestScene(f);
      auto* noClassname = addEntityWithoutClassname(map);
      rotate45(map, scene.wall);

      const auto result = f.ok("validate_map");
      auto nodes = QStringList{};
      for (const auto& item : result.value("items").toArray())
      {
        const auto node = item.toObject().value("node").toString();
        if (nodes.isEmpty() || nodes.back() != node)
        {
          nodes.push_back(node);
        }
      }
      // DFS order: world, B2 (rotated), E3 light, E4 func_door, E12 (no classname) in
      // the default layer, then E11 info_player_start in the details layer
      CHECK(nodes == QStringList{"world", "B2", "E3", "E4", f.id(*noClassname), "E11"});
      CHECK(
        result.value("total")
        == QJsonValue{qint64(result.value("items").toArray().size())});
    }

    SECTION("under limits the checked nodes")
    {
      const auto scene = populateTestScene(f);
      rotate45(map, scene.wall);

      CHECK(
        itemTypes(f.ok("validate_map", R"({"under":["B2"]})"))
        == QStringList{"Non-integer vertices"});
      const auto inLayer = f.ok("validate_map", R"({"under":["L10"]})");
      CHECK(itemTypes(inLayer) == QStringList{"Missing entity definition"});
      CHECK(
        inLayer.value("items").toArray()[0].toObject().value("node")
        == QJsonValue{"E11"});

      CHECK(f.err("validate_map", R"({"under":["B99"]})") == "unknown_node");

      // an empty selection is an error rather than a check of the whole map
      mdl::deselectAll(map);
      CHECK(f.err("validate_map", R"({"under":["@selection"]})") == "invalid_argument");

      mdl::selectNodes(map, {scene.wall});
      CHECK(
        itemTypes(f.ok("validate_map", R"({"under":["@selection"]})"))
        == QStringList{"Non-integer vertices"});
    }

    SECTION("types filters by validator description")
    {
      const auto scene = populateTestScene(f);
      rotate45(map, scene.wall);

      const auto result = f.ok("validate_map", R"({"types":["non-integer VERTICES"]})");
      CHECK(itemTypes(result) == QStringList{"Non-integer vertices"});

      const auto error = f.error("validate_map", R"({"types":["Nonsense"]})");
      CHECK(error.value("code") == QJsonValue{"invalid_argument"});
      const auto details = error.value("details").toObject();
      CHECK(details.value("pointer") == QJsonValue{"/types/0"});
      CHECK(strings(details.value("allowed")).contains("Non-integer vertices"));
    }

    SECTION("types does not limit the issues cached by the nodes")
    {
      auto* entityNode = addEntityWithoutClassname(map);

      // the node's issues are computed for the first time with a type filter
      CHECK(itemTypes(
              f.ok("validate_map", R"({"types":["Empty property value"]})"),
              f.id(*entityNode))
              .isEmpty());

      CHECK(itemTypes(f.ok("validate_map"), f.id(*entityNode))
              .contains("Missing entity classname"));
      const auto issues = entityNode->issues(map.worldNode().registeredValidators());
      CHECK(std::ranges::any_of(issues, [&](const auto* issue) {
        return issue->type() == issueType(map, "Missing entity classname");
      }));
    }

    SECTION("hidden issues are only reported with include_hidden")
    {
      auto* brushNode = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      rotate45(map, brushNode);
      brushNode->setIssueHidden(issueType(map, "Non-integer vertices"), true);

      CHECK(itemTypes(f.ok("validate_map"), f.id(*brushNode)).isEmpty());
      const auto result = f.ok("validate_map", R"({"include_hidden":true})");
      REQUIRE(itemTypes(result, f.id(*brushNode)) == QStringList{"Non-integer vertices"});
      CHECK(
        result.value("items").toArray()[1].toObject().value("hidden")
        == QJsonValue{true});
    }

    SECTION("paging")
    {
      for (int i = 0; i < 5; ++i)
      {
        addEntityWithoutClassname(map);
      }
      const auto all = f.ok("validate_map", R"({"types":["Missing entity classname"]})");
      REQUIRE(all.value("total") == QJsonValue{5});

      const auto page = f.ok(
        "validate_map", R"({"types":["Missing entity classname"],"offset":1,"limit":2})");
      CHECK(page.value("total") == QJsonValue{5});
      CHECK(page.value("truncated") == QJsonValue{true});
      CHECK(page.value("next_offset") == QJsonValue{3});
      const auto items = page.value("items").toArray();
      REQUIRE(items.size() == 2);
      CHECK(items[0] == all.value("items").toArray()[1]);
      CHECK(items[1] == all.value("items").toArray()[2]);
    }

    SECTION("schema errors")
    {
      CHECK(f.err("validate_map", R"({"include_hidden":1})") == "invalid_argument");
      CHECK(f.err("validate_map", R"({"bogus":1})") == "invalid_argument");
    }
  }

  SECTION("get_map_stats")
  {
    SECTION("empty map")
    {
      const auto stats = f.ok("get_map_stats");
      CHECK(
        compactJson(stats.value("counts"))
        == QStringLiteral(
          R"({"brush_entities":0,"entities":0,"entity_brushes":0,"faces":0,"groups":0,"layers":1,"patches":0,"point_entities":0,"vertices":0,"world_brushes":0})"));
      CHECK(stats.value("geometry_bounds").isNull());
      CHECK(stats.value("issues_total") == QJsonValue{1});
      CHECK(
        stats.value("content_digest") == QJsonValue{contentDigest(f.doc->nodes, map)});
    }

    SECTION("golden numbers for a 64 unit cube")
    {
      addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "base");
      const auto stats = f.ok("get_map_stats");
      CHECK(
        compactJson(stats.value("counts"))
        == QStringLiteral(
          R"({"brush_entities":0,"entities":0,"entity_brushes":0,"faces":6,"groups":0,"layers":1,"patches":0,"point_entities":0,"vertices":8,"world_brushes":1})"));
      CHECK(compactJson(stats.value("materials")) == QStringLiteral(R"({"base":6})"));
      CHECK(
        compactJson(stats.value("missing_materials")) == QStringLiteral(R"(["base"])"));
      CHECK(
        compactJson(stats.value("geometry_bounds"))
        == QStringLiteral(R"({"max":[64,64,64],"min":[0,0,0]})"));
      CHECK(stats.value("invalid_brushes").toArray().isEmpty());
      CHECK(stats.value("invalid_brushes_count") == QJsonValue{0});
      CHECK(stats.value("non_integer_brushes").toArray().isEmpty());
      CHECK(stats.value("point_entities_in_solid").toArray().isEmpty());
      // the world has no entity definition
      CHECK(stats.value("issues_total") == QJsonValue{1});
    }

    SECTION("populated scene")
    {
      const auto scene = populateTestScene(f);
      auto* inSolid = addPointEntity(map, "light", {32, 32, 32});
      // a point on the boundary of a world brush counts as inside
      auto* onBoundary = addPointEntity(map, "light", {64, 32, 32});
      // entity brushes do not count as solid
      addPointEntity(map, "light", {136, 8, 32});
      rotate45(map, scene.floor);

      const auto stats = f.ok("get_map_stats");
      CHECK(
        compactJson(stats.value("counts"))
        == QStringLiteral(
          R"({"brush_entities":1,"entities":6,"entity_brushes":2,"faces":24,"groups":1,"layers":2,"patches":1,"point_entities":5,"vertices":32,"world_brushes":2})"));
      CHECK(
        compactJson(stats.value("classnames"))
        == QStringLiteral(R"({"func_door":1,"info_player_start":1,"light":4})"));
      CHECK(
        compactJson(stats.value("unknown_classnames"))
        == QStringLiteral(R"(["func_door","info_player_start","light"])"));
      CHECK(
        compactJson(stats.value("materials"))
        == QStringLiteral(R"({"floor":6,"material":12,"patch_material":1,"wall":6})"));
      // E11 info_player_start at [16,16,24] is inside the wall B2
      CHECK(
        strings(stats.value("point_entities_in_solid"))
        == QStringList{f.id(*inSolid), f.id(*onBoundary), "E11"});
      CHECK(stats.value("point_entities_in_solid_count") == QJsonValue{3});
      CHECK(strings(stats.value("non_integer_brushes")) == QStringList{"B7"});
      CHECK(stats.value("non_integer_brushes_count") == QJsonValue{1});
      CHECK(
        stats.value("issues_total") == QJsonValue{f.ok("validate_map").value("total")});
      CHECK(
        stats.value("content_digest") == QJsonValue{contentDigest(f.doc->nodes, map)});
    }

    SECTION("id lists are capped at 50")
    {
      auto transaction = mdl::Transaction{map, "Add"};
      for (int i = 0; i < 55; ++i)
      {
        auto* brushNode =
          addBrush(map, {{0.0, 0.0, 32.0 * i}, {16.0, 16.0, 32.0 * i + 16}});
        mdl::selectNodes(map, {brushNode});
      }
      REQUIRE(mdl::rotateSelection(map, {0, 0, 0}, {0, 0, 1}, vm::to_radians(45.0)));
      mdl::deselectAll(map);
      REQUIRE(transaction.commit());

      const auto stats = f.ok("get_map_stats");
      CHECK(stats.value("non_integer_brushes").toArray().size() == 50);
      CHECK(stats.value("non_integer_brushes_count") == QJsonValue{55});
    }
  }

  SECTION("checkpoint_create")
  {
    addBrush(map, {{0, 0, 0}, {64, 64, 64}});
    const auto before = snapshot(map);

    const auto result = f.ok("checkpoint_create", R"({"name":"start"})");
    CHECK(result.value("name") == QJsonValue{"start"});
    CHECK(result.value("node_count") == QJsonValue{2});
    CHECK(result.value("content_digest") == QJsonValue{contentDigest(f.doc->nodes, map)});
    CHECK(
      result.value("modification_count") == QJsonValue{qint64(map.modificationCount())});
    CHECK(result.value("history_depth") == QJsonValue{0});
    CHECK(snapshot(map) == before);
    CHECK(f.doc->checkpoints.find("start") != nullptr);

    SECTION("records the MCP history depth")
    {
      {
        auto transaction = mdl::Transaction{map, "MCP: Create Box"};
        addBrush(map, {{128, 0, 0}, {192, 64, 64}});
        REQUIRE(transaction.commit());
      }
      addBrush(map, {{256, 0, 0}, {272, 16, 16}});

      const auto deeper = f.ok("checkpoint_create", R"({"name":"deeper"})");
      CHECK(deeper.value("history_depth") == QJsonValue{1});
      const auto* checkpoint = f.doc->checkpoints.find("deeper");
      REQUIRE(checkpoint != nullptr);
      CHECK(checkpoint->historySerials == f.doc->history.undoSerials());
      CHECK(checkpoint->historyConsistent);
    }

    SECTION("an existing name requires replace")
    {
      CHECK(f.err("checkpoint_create", R"({"name":"start"})") == "checkpoint_exists");

      addBrush(map, {{128, 0, 0}, {192, 64, 64}});
      const auto replaced =
        f.ok("checkpoint_create", R"({"name":"start","replace":true})");
      CHECK(replaced.value("node_count") == QJsonValue{3});
      CHECK(f.doc->checkpoints.size() == 1);
    }

    SECTION("at most 32 checkpoints")
    {
      for (int i = 1; i < 32; ++i)
      {
        f.ok("checkpoint_create", QJsonObject{{"name", QString{"c%1"}.arg(i)}});
      }
      const auto error = f.error("checkpoint_create", R"({"name":"one_too_many"})");
      CHECK(error.value("code") == QJsonValue{"limit_exceeded"});
      CHECK(
        error.value("details").toObject().value("checkpoints").toArray().size() == 32);
    }

    SECTION("invalid names")
    {
      CHECK(f.err("checkpoint_create", R"({"name":"current"})") == "invalid_argument");
      CHECK(f.err("checkpoint_create", R"({"name":"with space"})") == "invalid_argument");
      CHECK(f.err("checkpoint_create", R"({"name":""})") == "invalid_argument");
      CHECK(f.err("checkpoint_create", R"({})") == "invalid_argument");
      CHECK(
        f.err("checkpoint_create", QJsonObject{{"name", QString{65, u'a'}}})
        == "invalid_argument");
    }

    SECTION("is rejected without a document")
    {
      f.doc.reset();
      CHECK(f.err("checkpoint_create", R"({"name":"x"})") == "no_document");
    }
  }

  SECTION("checkpoint_diff")
  {
    const auto scene = populateTestScene(f);
    f.ok("checkpoint_create", R"({"name":"before"})");

    SECTION("no changes")
    {
      const auto diff = f.ok("checkpoint_diff", R"({"from":"before"})");
      CHECK(diff.value("from") == QJsonValue{"before"});
      CHECK(diff.value("to") == QJsonValue{"current"});
      CHECK(diff.value("content_identical") == QJsonValue{true});
      CHECK(diff.value("unchanged_count") == QJsonValue{12});
    }

    SECTION("a translated brush and a new entity")
    {
      mdl::selectNodes(map, {scene.wall});
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
      mdl::deselectAll(map);
      auto* added = addPointEntity(map, "light", {0, 0, 256});

      const auto diff = f.ok("checkpoint_diff", R"({"from":"before","to":"current"})");
      CHECK(diff.value("content_identical") == QJsonValue{false});
      CHECK(diff.value("from_digest") != diff.value("to_digest"));
      const auto addedList = diff.value("added").toArray();
      REQUIRE(addedList.size() == 1);
      CHECK(addedList[0].toObject().value("id") == QJsonValue{f.id(*added)});
      CHECK(addedList[0].toObject().value("classname") == QJsonValue{"light"});
      const auto modified = diff.value("modified").toArray();
      REQUIRE(modified.size() == 1);
      CHECK(modified[0].toObject().value("id") == QJsonValue{"B2"});
      CHECK(modified[0].toObject().value("geometry_changed") == QJsonValue{true});
    }

    SECTION("delete followed by undo reidentifies the restored nodes")
    {
      mdl::removeNodes(map, {scene.light});
      map.undoCommand();
      CHECK(f.err("get_nodes", R"({"ids":["E3"]})") == "stale_node");

      const auto diff = f.ok("checkpoint_diff", R"({"from":"before"})");
      CHECK(diff.value("content_identical") == QJsonValue{true});
      CHECK(diff.value("added").toArray().isEmpty());
      CHECK(diff.value("removed").toArray().isEmpty());
      CHECK(
        compactJson(diff.value("reidentified"))
        == QStringLiteral(R"([{"from":"E3","to":"E12"}])"));
    }

    SECTION("between two checkpoints")
    {
      mdl::removeNodes(map, {scene.playerStart});
      f.ok("checkpoint_create", R"({"name":"after"})");
      addBrush(map, {{512, 0, 0}, {528, 16, 16}});

      const auto diff = f.ok("checkpoint_diff", R"({"from":"before","to":"after"})");
      CHECK(diff.value("added").toArray().isEmpty());
      CHECK(
        compactJson(diff.value("removed").toArray()[0].toObject().value("id"))
        == QStringLiteral(R"(["E11"])"));

      const auto reverse = f.ok("checkpoint_diff", R"({"from":"after","to":"before"})");
      CHECK(
        reverse.value("added").toArray()[0].toObject().value("id") == QJsonValue{"E11"});
      CHECK(reverse.value("removed").toArray().isEmpty());

      const auto fromCurrent =
        f.ok("checkpoint_diff", R"({"from":"current","to":"after"})");
      CHECK(fromCurrent.value("removed_count") == QJsonValue{1});
    }

    SECTION("max_changes")
    {
      for (int i = 0; i < 4; ++i)
      {
        addBrush(map, {{512.0 + 32 * i, 0, 0}, {528.0 + 32 * i, 16, 16}});
      }
      const auto diff = f.ok("checkpoint_diff", R"({"from":"before","max_changes":2})");
      CHECK(diff.value("added").toArray().size() == 2);
      CHECK(diff.value("added_count") == QJsonValue{4});
      CHECK(diff.value("truncated") == QJsonValue{true});
      CHECK(
        f.err("checkpoint_diff", R"({"from":"before","max_changes":0})")
        == "invalid_argument");
    }

    SECTION("unknown checkpoints")
    {
      const auto error = f.error("checkpoint_diff", R"({"from":"nope"})");
      CHECK(error.value("code") == QJsonValue{"unknown_checkpoint"});
      CHECK(
        strings(error.value("details").toObject().value("available"))
        == QStringList{"before"});
      CHECK(
        f.err("checkpoint_diff", R"({"from":"before","to":"nope"})")
        == "unknown_checkpoint");
      CHECK(f.err("checkpoint_diff", R"({})") == "invalid_argument");
    }

    SECTION("does not change the map")
    {
      mdl::selectNodes(map, {scene.wall});
      const auto before = snapshot(map);
      f.ok("checkpoint_diff", R"({"from":"before"})");
      CHECK(snapshot(map) == before);
    }
  }

  SECTION("checkpoint_list")
  {
    CHECK(
      compactJson(f.ok("checkpoint_list"))
      == QStringLiteral(R"({"checkpoints":[],"limit":32})"));

    f.ok("checkpoint_create", R"({"name":"b"})");
    addBrush(map, {{0, 0, 0}, {64, 64, 64}});
    f.ok("checkpoint_create", R"({"name":"a"})");

    const auto checkpoints = f.ok("checkpoint_list").value("checkpoints").toArray();
    REQUIRE(checkpoints.size() == 2);
    const auto a = checkpoints[0].toObject();
    CHECK(a.value("name") == QJsonValue{"a"});
    CHECK(a.value("node_count") == QJsonValue{2});
    CHECK(a.value("content_digest") == QJsonValue{contentDigest(f.doc->nodes, map)});
    CHECK(a.value("modification_count") == QJsonValue{qint64(map.modificationCount())});
    CHECK(a.value("history_depth") == QJsonValue{0});
    CHECK(checkpoints[1].toObject().value("name") == QJsonValue{"b"});
    CHECK(checkpoints[1].toObject().value("node_count") == QJsonValue{1});
  }

  SECTION("checkpoint_delete")
  {
    f.ok("checkpoint_create", R"({"name":"a"})");
    CHECK(
      compactJson(f.ok("checkpoint_delete", R"({"name":"a"})"))
      == QStringLiteral(R"({"deleted":"a"})"));
    CHECK(f.doc->checkpoints.size() == 0);
    CHECK(f.err("checkpoint_delete", R"({"name":"a"})") == "unknown_checkpoint");
    CHECK(f.err("checkpoint_diff", R"({"from":"a"})") == "unknown_checkpoint");
    CHECK(f.err("checkpoint_delete", R"({})") == "invalid_argument");
  }

  SECTION("export_map_text")
  {
    SECTION("the whole map")
    {
      const auto scene = populateTestScene(f);
      mdl::selectNodes(map, {scene.wall});
      const auto before = snapshot(map);

      const auto result = f.ok("export_map_text");
      const auto text = result.value("text").toString();
      CHECK(text == QString::fromStdString(mapText(map)));
      CHECK(text.contains(R"("classname" "light")"));
      CHECK(text.contains(R"("_tb_name" "details")"));
      CHECK(result.value("bytes") == QJsonValue{qint64(text.toUtf8().size())});
      CHECK(result.value("truncated") == QJsonValue{false});
      CHECK_FALSE(result.contains("ids"));
      CHECK_FALSE(text.startsWith("// Game"));
      CHECK(snapshot(map) == before);
    }

    SECTION("include_header")
    {
      const auto text =
        f.ok("export_map_text", R"({"include_header":true})").value("text").toString();
      CHECK(text.startsWith("// Game: Quake\n// Format: Valve\n"));
    }

    SECTION("strip_tb_properties")
    {
      populateTestScene(f);
      const auto text = f.ok("export_map_text", R"({"strip_tb_properties":true})")
                          .value("text")
                          .toString();
      CHECK_FALSE(text.contains("_tb_"));
    }

    SECTION("a subset of nodes")
    {
      populateTestScene(f);
      const auto result = f.ok("export_map_text", R"({"ids":["E3","B2"]})");
      CHECK(strings(result.value("ids")) == QStringList{"B2", "E3"});
      const auto text = result.value("text").toString();
      CHECK(text.contains(R"("classname" "light")"));
      CHECK(text.contains(R"("classname" "worldspawn")"));
      CHECK_FALSE(text.contains("func_door"));
      CHECK_FALSE(text.contains("info_player_start"));
    }

    SECTION("a layer exports its children")
    {
      populateTestScene(f);
      const auto result = f.ok("export_map_text", R"({"ids":["L10"]})");
      CHECK(strings(result.value("ids")) == QStringList{"E11"});
      CHECK(result.value("text").toString().contains("info_player_start"));
    }

    SECTION("brushes of brush entities are exported with their entity")
    {
      populateTestScene(f);
      const auto result = f.ok("export_map_text", R"({"ids":["B5","B6"]})");
      CHECK(strings(result.value("ids")) == QStringList{"E4"});
      CHECK(result.value("text").toString().contains("func_door"));
      CHECK(
        strings(result.value("warnings"))
        == QStringList{
          "B5 is exported with its entity E4", "B6 is exported with its entity E4"});
    }

    SECTION("descendants of exported nodes are not exported twice")
    {
      populateTestScene(f);
      const auto result = f.ok("export_map_text", R"({"ids":["B7","G8"]})");
      CHECK(strings(result.value("ids")) == QStringList{"G8"});
      CHECK(result.value("text").toString().count("// brush") == 1);
    }

    SECTION("the world is rejected")
    {
      CHECK(f.err("export_map_text", R"({"ids":["world"]})") == "wrong_node_type");
      CHECK(f.err("export_map_text", R"({"ids":["B42"]})") == "unknown_node");
    }

    SECTION("max_bytes truncates the text")
    {
      for (int i = 0; i < 20; ++i)
      {
        addBrush(map, {{32.0 * i, 0, 0}, {32.0 * i + 16, 16, 16}});
      }
      const auto full = f.ok("export_map_text");
      REQUIRE(full.value("bytes").toInt() > 1024);

      const auto cut = f.ok("export_map_text", R"({"max_bytes":1024})");
      CHECK(cut.value("truncated") == QJsonValue{true});
      CHECK(cut.value("bytes") == full.value("bytes"));
      CHECK(cut.value("text").toString().toUtf8().size() == 1024);
      CHECK(full.value("text").toString().startsWith(cut.value("text").toString()));

      CHECK(f.err("export_map_text", R"({"max_bytes":1023})") == "invalid_argument");
    }

    SECTION("truncation keeps whole characters")
    {
      auto* entityNode = addPointEntity(map, "info_null", {0, 0, 0});
      mdl::selectNodes(map, {entityNode});
      // 0xE9 is é in the Quake (Windows-1252) encoding, two bytes in UTF-8
      REQUIRE(mdl::setEntityProperty(map, "message", std::string(2000, '\xE9')));
      mdl::deselectAll(map);

      const auto full = f.ok("export_map_text").value("text").toString();
      const auto start = full.toUtf8().indexOf("\xC3\xA9");
      REQUIRE(start > 0);
      const auto maxBytes = start + 1025 - (start % 2 == 0 ? 0 : 1);
      const auto cut = f.ok(
        "export_map_text",
        QJsonObject{{"max_bytes", qint64(std::max(maxBytes, qsizetype(1024)))}});
      const auto text = cut.value("text").toString();
      CHECK(text.toUtf8().size() <= std::max(maxBytes, qsizetype(1024)));
      CHECK_FALSE(text.contains(QChar::ReplacementCharacter));
      CHECK(full.startsWith(text));
    }

    SECTION("does not change the file positions of nodes and faces")
    {
      const auto scene = populateTestScene(f);
      // assign file positions as when saving the whole map
      mapText(map);

      const auto positions = [&]() {
        auto result = std::vector<size_t>{};
        map.worldNode().accept(kdl::overload(
          [&](auto&& thisLambda, const mdl::WorldNode& worldNode) {
            result.push_back(worldNode.lineNumber());
            result.push_back(worldNode.lineCount());
            worldNode.visitChildren(thisLambda);
          },
          [&](auto&& thisLambda, const mdl::LayerNode& layerNode) {
            result.push_back(layerNode.lineNumber());
            result.push_back(layerNode.lineCount());
            layerNode.visitChildren(thisLambda);
          },
          [&](auto&& thisLambda, const mdl::GroupNode& groupNode) {
            result.push_back(groupNode.lineNumber());
            result.push_back(groupNode.lineCount());
            groupNode.visitChildren(thisLambda);
          },
          [&](auto&& thisLambda, const mdl::EntityNode& entityNode) {
            result.push_back(entityNode.lineNumber());
            result.push_back(entityNode.lineCount());
            entityNode.visitChildren(thisLambda);
          },
          [&](const mdl::BrushNode& brushNode) {
            result.push_back(brushNode.lineNumber());
            result.push_back(brushNode.lineCount());
            for (const auto& face : brushNode.brush().faces())
            {
              result.push_back(face.lineNumber());
            }
          },
          [&](const mdl::PatchNode& patchNode) {
            result.push_back(patchNode.lineNumber());
            result.push_back(patchNode.lineCount());
          }));
        return result;
      };

      const auto before = positions();
      REQUIRE(scene.playerStart->lineNumber() > 1);
      const auto lines = f.ok("validate_map").value("items");

      f.ok("export_map_text", R"({"ids":["E11","B7"]})");
      CHECK(positions() == before);
      f.ok("export_map_text", R"({"include_header":true})");
      CHECK(positions() == before);
      CHECK(f.ok("validate_map").value("items") == lines);
    }

    SECTION("pasting the exported text into a fresh map gives the same content digest")
    {
      addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "base");
      auto* light = addPointEntity(map, "light", {32, 32, 128});
      mdl::selectNodes(map, {light});
      REQUIRE(mdl::setEntityProperty(map, "light", "300"));
      mdl::deselectAll(map);
      addBrushEntity(map, "func_door", {{{128, 0, 0}, {144, 16, 64}}});
      auto* floor = addBrush(map, {{0, 128, 0}, {64, 192, 16}}, "floor");
      groupNodes(map, {floor}, "room");
      mdl::deselectAll(map);

      const auto text = f.ok("export_map_text").value("text").toString();
      const auto digest = f.ok("get_map_stats").value("content_digest");

      auto other = McpMapToolsFixture{};
      REQUIRE(
        mdl::paste(other.map, fromUnicode(other.map, text)) == mdl::PasteType::Node);
      mdl::deselectAll(other.map);
      CHECK(other.ok("get_map_stats").value("content_digest") == digest);
      CHECK(other.ok("export_map_text").value("text") == QJsonValue{text});
    }
  }

  SECTION("check_walkable")
  {
    const auto target = [](const QJsonObject& result, const qsizetype index = 0) {
      return result.value("targets").toArray()[index].toObject();
    };
    const auto blocker = [&](const QJsonObject& result, const qsizetype index = 0) {
      return target(result, index).value("blocker").toObject();
    };
    const auto details = [&](const QJsonObject& result, const qsizetype index = 0) {
      return blocker(result, index).value("details").toObject();
    };

    SECTION("a flat room is walkable")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 128}});

      const auto result =
        f.ok("check_walkable", R"({"from":[64,128,0],"to":[[448,128,0],[448,64,30]]})");
      CHECK(result.value("reachable_count") == QJsonValue{2});
      CHECK(result.value("unreachable_count") == QJsonValue{0});
      CHECK(
        result.value("from")
        == QJsonValue{QJsonObject{
          {"point", jsonVec(64, 128, 0)}, {"standing_point", jsonVec(64, 128, 0)}}});

      const auto straight = target(result, 0);
      CHECK(straight.value("reachable") == QJsonValue{true});
      CHECK(straight.value("standing_point") == QJsonValue{jsonVec(448, 128, 0)});
      CHECK(
        straight.value("path")
        == QJsonValue{QJsonArray{jsonVec(64, 128, 0), jsonVec(448, 128, 0)}});
      CHECK(straight.value("path_length") == QJsonValue{384});
      CHECK(!straight.contains("blocker"));

      // a point above the floor is moved down to it
      const auto above = target(result, 1);
      CHECK(above.value("reachable") == QJsonValue{true});
      CHECK(above.value("standing_point") == QJsonValue{jsonVec(448, 64, 0)});
      CHECK(above.value("path").toArray().last() == QJsonValue{jsonVec(448, 64, 0)});
      CHECK(above.value("path_length") == QJsonValue{384 + 64});
      CHECK(strings(result.value("warnings")).size() == 1);

      CHECK(
        result.value("model")
        == QJsonValue{QJsonObject{
          {"hull", QJsonObject{{"width", 32}, {"height", 56}}},
          {"step_height", 18},
          {"max_drop", 256},
          {"resolution", 8}}});
      const auto stats = result.value("stats").toObject();
      CHECK(stats.value("solid_brushes") == QJsonValue{6});
      // the room's interior is 512 x 256; the hull stays 16 units from the walls
      CHECK(
        stats.value("positions_reachable") == QJsonValue{(480 / 8 + 1) * (224 / 8 + 1)});
    }

    SECTION("stairs with 16 unit steps are walkable, with 24 unit steps they are not")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 256}});
      const auto addStairs = [&](const double stepHeight) {
        for (int k = 1; k <= 4; ++k)
        {
          addBrush(
            map,
            vm::bbox3d{
              {96.0 + 32.0 * (k - 1), 64, 0}, {96.0 + 32.0 * k, 192, stepHeight * k}});
        }
        // the landing
        addBrush(map, vm::bbox3d{{224, 0, 0}, {512, 256, stepHeight * 4}});
      };

      SECTION("16")
      {
        addStairs(16);
        const auto result =
          f.ok("check_walkable", R"({"from":[48,128,0],"to":[[448,128,64]]})");
        const auto top = target(result);
        CHECK(top.value("reachable") == QJsonValue{true});
        CHECK(top.value("standing_point") == QJsonValue{jsonVec(448, 128, 64)});
        const auto path = top.value("path").toArray();
        CHECK(path.size() <= 64);
        CHECK(path.first() == QJsonValue{jsonVec(48, 128, 0)});
        CHECK(path.last() == QJsonValue{jsonVec(448, 128, 64)});
        // the path climbs the steps one by one
        for (const auto z : {16, 32, 48})
        {
          CHECK(std::ranges::any_of(path, [&](const auto& point) {
            return point.toArray()[2] == QJsonValue{z};
          }));
        }

        // a step_height below 16 blocks the stairs
        const auto low = f.ok(
          "check_walkable",
          R"({"from":[48,128,0],"to":[[448,128,64]],"step_height":12})");
        CHECK(target(low).value("reachable") == QJsonValue{false});
        CHECK(blocker(low).value("kind") == QJsonValue{"step_too_high"});
        CHECK(details(low).value("rise") == QJsonValue{16});
      }

      SECTION("24")
      {
        addStairs(24);
        const auto result =
          f.ok("check_walkable", R"({"from":[48,128,0],"to":[[448,128,96]]})");
        CHECK(result.value("reachable_count") == QJsonValue{0});
        const auto top = target(result);
        CHECK(top.value("reachable") == QJsonValue{false});
        CHECK(top.value("standing_point") == QJsonValue{jsonVec(448, 128, 96)});
        CHECK(!top.contains("path"));
        CHECK(top.value("nearest_reachable_point").toArray()[2] == QJsonValue{0});
        CHECK(top.value("distance").toDouble() > 96);
        // the first step on the way up the stairs
        CHECK(blocker(result).value("kind") == QJsonValue{"step_too_high"});
        CHECK(blocker(result).value("at").toArray()[2] == QJsonValue{24});
        CHECK(blocker(result).value("at").toArray()[1] == QJsonValue{128});
        CHECK(details(result).value("rise") == QJsonValue{24});
        CHECK(details(result).value("step_height") == QJsonValue{18});
        CHECK(details(result).value("from").toArray()[2] == QJsonValue{0});

        // a higher step_height climbs them
        CHECK(
          target(f.ok(
                   "check_walkable",
                   R"({"from":[48,128,0],"to":[[448,128,96]],"step_height":24})"))
            .value("reachable")
          == QJsonValue{true});
      }
    }

    SECTION("the blocker is on the way that exceeds the limits the least")
    {
      // free-standing stairs with 8 unit steps rising towards +y; from behind them, the
      // shortest way is to climb the 32 unit back of the top step
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 512, 256}});
      for (int k = 1; k <= 4; ++k)
      {
        addBrush(
          map,
          vm::bbox3d{{192, 100.0 + 16.0 * (k - 1), 0}, {256, 100.0 + 16.0 * k, 8.0 * k}});
      }
      const auto args = R"({"from":[224,256,0],"to":[[224,160,32]],"step_height":4})";
      const auto result = f.ok("check_walkable", args);
      CHECK(blocker(result).value("kind") == QJsonValue{"step_too_high"});
      CHECK(details(result).value("rise") == QJsonValue{8});
      CHECK(details(result).value("from").toArray()[1].toDouble() < 100);
    }

    SECTION("a low ceiling blocks the hull")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {576, 256, 128}});
      // the ceiling of a passage at z 40 across the whole room
      addBrush(map, vm::bbox3d{{256, 0, 40}, {320, 256, 128}});

      const auto result =
        f.ok("check_walkable", R"({"from":[64,128,0],"to":[[512,128,0]]})");
      const auto far = target(result);
      CHECK(far.value("reachable") == QJsonValue{false});
      CHECK(far.value("standing_point") == QJsonValue{jsonVec(512, 128, 0)});
      // the hull stops 16 units before the passage
      CHECK(far.value("nearest_reachable_point") == QJsonValue{jsonVec(240, 128, 0)});
      CHECK(far.value("distance") == QJsonValue{272});
      CHECK(blocker(result).value("kind") == QJsonValue{"headroom"});
      CHECK(blocker(result).value("at") == QJsonValue{jsonVec(248, 128, 0)});
      CHECK(details(result).value("clearance") == QJsonValue{40});
      CHECK(details(result).value("hull_height") == QJsonValue{56});

      // a target under the low ceiling has no headroom itself
      const auto under =
        f.ok("check_walkable", R"({"from":[64,128,0],"to":[[288,128,0]]})");
      CHECK(blocker(under).value("kind") == QJsonValue{"not_standable"});
      CHECK(details(under).value("reason") == QJsonValue{"no_headroom"});
      CHECK(details(under).value("clearance") == QJsonValue{40});

      // a smaller hull fits through
      CHECK(
        target(f.ok(
                 "check_walkable",
                 R"({"from":[64,128,0],"to":[[512,128,0]],"hull":{"height":40}})"))
          .value("reachable")
        == QJsonValue{true});

      // from under the low ceiling is an error
      const auto error =
        f.error("check_walkable", R"({"from":[288,128,0],"to":[[64,128,0]]})");
      CHECK(error.value("code") == QJsonValue{"invalid_argument"});
      const auto errorDetails = error.value("details").toObject();
      CHECK(errorDetails.value("pointer") == QJsonValue{"/from"});
      CHECK(errorDetails.value("reason") == QJsonValue{"no_headroom"});
      CHECK(errorDetails.value("clearance") == QJsonValue{40});
      CHECK(errorDetails.value("standing_point") == QJsonValue{jsonVec(288, 128, 0)});
    }

    SECTION("a second floor is reachable only via the stairs")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {768, 256, 256}});
      // the second floor, 128 above the ground floor, with room below it
      addBrush(map, vm::bbox3d{{512, 0, 112}, {768, 256, 128}});
      auto steps = std::vector<mdl::Node*>{};
      for (int k = 1; k <= 8; ++k)
      {
        steps.push_back(addBrush(
          map,
          vm::bbox3d{
            {256.0 + 32.0 * (k - 1), 96, 0}, {256.0 + 32.0 * k, 160, 16.0 * k}}));
      }

      const auto args = R"({"from":[64,128,0],"to":[[704,128,128],[704,128,0]]})";
      const auto result = f.ok("check_walkable", args);
      CHECK(result.value("reachable_count") == QJsonValue{2});
      const auto upstairs = target(result, 0);
      CHECK(upstairs.value("standing_point") == QJsonValue{jsonVec(704, 128, 128)});
      CHECK(std::ranges::any_of(upstairs.value("path").toArray(), [](const auto& point) {
        const auto z = point.toArray()[2].toDouble();
        return z > 0 && z < 128;
      }));
      // below the second floor, on the ground floor
      CHECK(
        target(result, 1).value("standing_point") == QJsonValue{jsonVec(704, 128, 0)});
      // around the stairs, 48 units to the side and back
      CHECK(target(result, 1).value("path_length").toDouble() >= 640 + 2 * 48);

      mdl::deselectAll(map);
      mdl::selectNodes(map, steps);
      mdl::removeSelectedNodes(map);
      const auto without = f.ok("check_walkable", args);
      CHECK(target(without, 0).value("reachable") == QJsonValue{false});
      CHECK(blocker(without, 0).value("kind") == QJsonValue{"step_too_high"});
      CHECK(details(without, 0).value("rise") == QJsonValue{128});
      CHECK(target(without, 1).value("reachable") == QJsonValue{true});
    }

    SECTION("drops")
    {
      // two platforms in the open with a pit between them
      addBrush(map, vm::bbox3d{{0, 0, -16}, {256, 256, 0}});
      addBrush(map, vm::bbox3d{{320, 0, -16}, {576, 256, 0}});

      SECTION("too deep")
      {
        addBrush(map, vm::bbox3d{{256, 0, -528}, {320, 256, -512}});
        const auto result = f.ok(
          "check_walkable", R"({"from":[64,128,0],"to":[[512,128,0],[288,128,-512]]})");
        CHECK(blocker(result, 0).value("kind") == QJsonValue{"gap"});
        CHECK(details(result, 0).value("drop") == QJsonValue{512});
        CHECK(details(result, 0).value("max_drop") == QJsonValue{256});
        CHECK(blocker(result, 1).value("kind") == QJsonValue{"gap"});
        CHECK(blocker(result, 1).value("at").toArray()[2] == QJsonValue{-512});

        // a higher max_drop jumps into the pit, but there is no way out
        const auto deeper = f.ok(
          "check_walkable",
          R"({"from":[64,128,0],"to":[[512,128,0],[288,128,-512]],"max_drop":1000})");
        CHECK(target(deeper, 1).value("reachable") == QJsonValue{true});
        CHECK(blocker(deeper, 0).value("kind") == QJsonValue{"step_too_high"});
        CHECK(details(deeper, 0).value("rise") == QJsonValue{512});
      }

      SECTION("no floor")
      {
        const auto result =
          f.ok("check_walkable", R"({"from":[64,128,0],"to":[[512,128,0],[288,128,0]]})");
        CHECK(blocker(result, 0).value("kind") == QJsonValue{"gap"});
        CHECK(details(result, 0).value("reason") == QJsonValue{"no_floor"});
        CHECK(blocker(result, 1).value("kind") == QJsonValue{"not_standable"});
        CHECK(details(result, 1).value("reason") == QJsonValue{"no_floor"});
        CHECK(target(result, 1).value("standing_point") == QJsonValue{});
      }

      SECTION("a narrow gap is stepped over")
      {
        // the hull is 32 wide and stands on any part of a floor
        addBrush(map, vm::bbox3d{{256, 0, -16}, {304, 256, 0}});
        CHECK(
          target(f.ok("check_walkable", R"({"from":[64,128,0],"to":[[512,128,0]]})"))
            .value("reachable")
          == QJsonValue{true});
      }
    }

    SECTION("targets inside solid are not standable")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 128}});
      addBrush(map, vm::bbox3d{{200, 64, 0}, {264, 192, 64}});

      const auto result = f.ok(
        "check_walkable",
        R"({"from":[64,128,0],"to":[[232,128,32],[64,128,-8],[232,128,64],[1000,128,0]],
            "bounds":{"min":[-64,-64,-64],"max":[600,320,200]}})");
      CHECK(blocker(result, 0).value("kind") == QJsonValue{"not_standable"});
      CHECK(details(result, 0).value("reason") == QJsonValue{"inside_solid"});
      CHECK(blocker(result, 0).value("at") == QJsonValue{jsonVec(232, 128, 32)});
      CHECK(target(result, 0).value("nearest_reachable_point").isArray());
      CHECK(details(result, 1).value("reason") == QJsonValue{"inside_solid"});
      // the top of the block is standable but 64 high
      CHECK(blocker(result, 2).value("kind") == QJsonValue{"step_too_high"});
      CHECK(details(result, 3).value("reason") == QJsonValue{"outside_bounds"});

      // a hull next to a wall does not fit
      const auto wall = f.ok("check_walkable", R"({"from":[64,128,0],"to":[[4,128,0]]})");
      CHECK(details(wall).value("reason") == QJsonValue{"blocked"});
    }

    SECTION("brush entities")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 128}});
      const auto args = R"({"from":[64,128,0],"to":[[448,128,0]]})";

      // a door counts as open
      addBrushEntity(map, "func_door", {vm::bbox3d{{240, 0, 0}, {256, 256, 128}}});
      CHECK(target(f.ok("check_walkable", args)).value("reachable") == QJsonValue{true});

      // a func_wall blocks, like a wall
      addBrushEntity(map, "func_wall", {vm::bbox3d{{240, 0, 0}, {256, 256, 128}}});
      const auto walled = f.ok("check_walkable", args);
      CHECK(target(walled).value("reachable") == QJsonValue{false});
      CHECK(blocker(walled).value("kind") == QJsonValue{"step_too_high"});
      CHECK(
        details(walled).value("message")
        == QJsonValue{"a wall or obstacle blocks the way"});
    }

    SECTION("ramps")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 256}});
      // a ramp rising by 64 over 128 units up to a platform
      f.ok(
        "create_brush_from_points",
        R"({"points":[[128,64,0],[256,64,0],[256,64,64],[128,192,0],[256,192,0],[256,192,64]]})");
      addBrush(map, vm::bbox3d{{256, 0, 0}, {512, 256, 64}});
      const auto ramp =
        f.ok("check_walkable", R"({"from":[64,128,0],"to":[[448,128,64]]})");
      CHECK(target(ramp).value("reachable") == QJsonValue{true});
      // the path goes up the ramp, not around it
      CHECK(
        std::ranges::any_of(target(ramp).value("path").toArray(), [](const auto& point) {
          return point.toArray()[1] == QJsonValue{128}
                 && point.toArray()[2].toDouble() > 0
                 && point.toArray()[2].toDouble() < 64;
        }));
    }

    SECTION("rotated brushes")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 128}});
      // a pillar rotated by 45 degrees around z, walked around
      auto* pillar = addBrush(map, vm::bbox3d{{-24, -24, 0}, {24, 24, 128}});
      rotate45(map, pillar);
      mdl::selectNodes(map, {pillar});
      REQUIRE(mdl::translateSelection(map, {128, 128, 0}));
      mdl::deselectAll(map);

      const auto around =
        f.ok("check_walkable", R"({"from":[48,128,0],"to":[[208,128,0]]})");
      CHECK(target(around).value("reachable") == QJsonValue{true});
      CHECK(target(around).value("path").toArray().size() > 2);
      CHECK(target(around).value("path_length").toDouble() > 160);

      // the hull does not fit between the pillar and a wall 40 units away from its edge
      addBrush(map, vm::bbox3d{{0, 0, 0}, {512, 128 - 34 - 24, 128}});
      addBrush(map, vm::bbox3d{{0, 128 + 34 + 24, 0}, {512, 256, 128}});
      const auto blocked =
        f.ok("check_walkable", R"({"from":[48,128,0],"to":[[208,128,0]]})");
      CHECK(target(blocked).value("reachable") == QJsonValue{false});
    }

    SECTION("bounds and max_cells")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 128}});

      const auto bounded = f.ok(
        "check_walkable",
        R"({"from":[64,128,0],"to":[[448,128,0]],"bounds":{"min":[0,0,-8],"max":[256,256,128]}})");
      CHECK(details(bounded).value("reason") == QJsonValue{"outside_bounds"});
      CHECK(
        f.err(
          "check_walkable",
          R"({"from":[64,128,0],"to":[[448,128,0]],"bounds":{"min":[100,0,0],"max":[256,256,128]}})")
        == "invalid_argument");

      const auto error = f.error(
        "check_walkable", R"({"from":[64,128,0],"to":[[448,128,0]],"max_cells":1000})");
      CHECK(error.value("code") == QJsonValue{"invalid_argument"});
      CHECK(
        error.value("details").toObject().value("pointer") == QJsonValue{"/resolution"});
      CHECK(error.value("hint").toString().contains("resolution"));
      CHECK(
        f.ok(
           "check_walkable",
           R"({"from":[64,128,0],"to":[[448,128,0]],"resolution":32,"max_cells":5000})")
          .value("reachable_count")
        == QJsonValue{1});
    }

    SECTION("errors")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 128}});
      const auto reason = [&](const char* args) {
        return f.error("check_walkable", args)
          .value("details")
          .toObject()
          .value("reason")
          .toString();
      };
      CHECK(reason(R"({"from":[64,128,-8],"to":[[64,128,0]]})") == "inside_solid");
      CHECK(reason(R"({"from":[-100,128,0],"to":[[64,128,0]]})") == "no_floor");
      CHECK(reason(R"({"from":[4,128,0],"to":[[64,128,0]]})") == "blocked");
      CHECK(
        f.error(
           "check_walkable", R"({"from":[64,128,0],"to":[[64,128,0]],"resolution":40})")
          .value("details")
          .toObject()
          .value("pointer")
        == QJsonValue{"/resolution"});
      CHECK(
        f.err("check_walkable", R"({"from":[64,128,0],"to":[]})") == "invalid_argument");
      CHECK(f.err("check_walkable", R"({"to":[[64,128,0]]})") == "invalid_argument");
      CHECK(
        f.err("check_walkable", R"({"from":[64,128,0],"to":[[1e300,0,0]]})")
        == "invalid_argument");
      CHECK(
        f.err(
          "check_walkable", R"({"from":[64,128,0],"to":[[0,0,0]],"hull":{"width":0}})")
        == "invalid_argument");
    }

    SECTION("is deterministic and leaves the map unchanged")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {576, 256, 128}});
      addBrush(map, vm::bbox3d{{256, 0, 40}, {320, 256, 128}});
      addBrush(map, vm::bbox3d{{96, 64, 0}, {128, 192, 24}});
      const auto before = snapshot(map);
      const auto args =
        R"({"from":[64,128,0],"to":[[512,128,0],[112,128,24],[200,200,0],[288,128,0]]})";
      const auto first = compactJson(f.ok("check_walkable", args));
      CHECK(compactJson(f.ok("check_walkable", args)) == first);
      CHECK(snapshot(map) == before);
    }

    SECTION("points on stairs narrower than the hull rest on the next step")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 256}});
      // 8 unit steps that are 16 deep, so the hull always rests on two of them
      for (int k = 1; k <= 8; ++k)
      {
        addBrush(
          map,
          vm::bbox3d{{96.0 + 16.0 * (k - 1), 0, 0}, {96.0 + 16.0 * k, 256, 8.0 * k}});
      }
      addBrush(map, vm::bbox3d{{224, 0, 0}, {512, 256, 64}});

      // the middle of the third step; the hull there also rests on the fourth
      const auto result =
        f.ok("check_walkable", R"({"from":[48,128,0],"to":[[136,128,24],[448,128,64]]})");
      CHECK(result.value("reachable_count") == QJsonValue{2});
      CHECK(
        target(result, 0).value("standing_point") == QJsonValue{jsonVec(136, 128, 32)});
      CHECK(
        target(result, 0).value("path").toArray().last()
        == QJsonValue{jsonVec(136, 128, 32)});
      CHECK(!result.contains("warnings"));

      const auto fromStep =
        f.ok("check_walkable", R"({"from":[136,128,24],"to":[[448,128,64]]})");
      CHECK(
        fromStep.value("from").toObject().value("standing_point")
        == QJsonValue{jsonVec(136, 128, 32)});
      CHECK(target(fromStep).value("reachable") == QJsonValue{true});

      // not more than step_height up
      const auto low = f.ok(
        "check_walkable", R"({"from":[48,128,0],"to":[[136,128,24]],"step_height":4})");
      CHECK(blocker(low).value("kind") == QJsonValue{"not_standable"});
      CHECK(details(low).value("reason") == QJsonValue{"blocked"});
      CHECK(target(low).value("standing_point") == QJsonValue{jsonVec(136, 128, 24)});
    }

    SECTION("a point near the edge of the bounds uses the column inside them")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 128}});
      // the column centers are at 64 + 8 i, the last one inside the bounds at 296
      const auto result = f.ok(
        "check_walkable",
        R"({"from":[64,128,0],"to":[[300,128,0]],"bounds":{"min":[-16,-16,-16],"max":[302,272,144]}})");
      CHECK(target(result).value("reachable") == QJsonValue{true});
      CHECK(target(result).value("standing_point") == QJsonValue{jsonVec(296, 128, 0)});
    }

    SECTION("thin bounds count at least the hull height in max_cells")
    {
      addShell(map, vm::bbox3d{{0, 0, 0}, {512, 256, 128}});
      // the work is per column, and the hull boxes reach 56 above the floor
      const auto result = f.ok(
        "check_walkable",
        R"({"from":[64,128,0],"to":[[448,128,0]],"bounds":{"min":[0,0,0],"max":[512,256,1]}})");
      CHECK(
        result.value("stats").toObject().value("grid")
        == QJsonValue{QJsonArray{65, 33, 7}});
      CHECK(result.value("stats").toObject().value("cells") == QJsonValue{65 * 33 * 7});
      CHECK(
        f.err(
          "check_walkable",
          R"({"from":[64,128,0],"to":[[448,128,0]],"bounds":{"min":[0,0,0],"max":[512,256,1]},"max_cells":15000})")
        == "invalid_argument");
    }
  }

  SECTION("every eval tool leaves the map unchanged")
  {
    const auto scene = populateTestScene(f);
    mdl::selectNodes(map, {scene.wall});
    f.ok("checkpoint_create", R"({"name":"x"})");
    const auto before = snapshot(map);

    for (const auto* call : {
           R"(["validate_map",{}])",
           R"(["get_map_stats",{}])",
           R"(["checkpoint_create",{"name":"y"}])",
           R"(["checkpoint_diff",{"from":"x"}])",
           R"(["checkpoint_list",{}])",
           R"(["checkpoint_delete",{"name":"y"}])",
           R"(["export_map_text",{"ids":["@selection"]}])",
           R"(["check_leaks",{}])",
           R"(["check_walkable",{"from":[32,160,16],"to":[[32,32,64]]}])",
         })
    {
      const auto args =
        parseObject((std::string{R"({"a":)"} + call + "}").c_str()).value("a").toArray();
      f.ok(args[0].toString(), args[1].toObject());
      CHECK(snapshot(map) == before);
    }
  }

  SECTION("query_point")
  {
    populateTestScene(f);

    SECTION("reports solid world brushes and entity brushes")
    {
      const auto results =
        f.ok(
           "query_point",
           R"({"points":[[32,32,32],[136,8,32],[64,32,32],[32,160,8],[32,32,128],[9000,0,0]]})")
          .value("results")
          .toArray();
      REQUIRE(results.size() == 6);

      CHECK(
        results[0]
        == QJsonValue{QJsonObject{
          {"point", QJsonArray{32, 32, 32}},
          {"in_world_bounds", true},
          {"inside_solid", true},
          {"containing_brushes",
           QJsonArray{QJsonObject{{"id", "B2"}, {"entity", "world"}}}},
        }});

      // a door brush is listed but not solid
      CHECK(results[1].toObject().value("inside_solid") == QJsonValue{false});
      CHECK(
        results[1].toObject().value("containing_brushes")
        == QJsonValue{QJsonArray{QJsonObject{{"id", "B5"}, {"entity", "E4"}}}});

      // points on a face are inside
      CHECK(results[2].toObject().value("inside_solid") == QJsonValue{true});

      // brushes in groups belong to the world
      CHECK(results[3].toObject().value("inside_solid") == QJsonValue{true});

      CHECK(results[4].toObject().value("inside_solid") == QJsonValue{false});
      CHECK(
        results[4].toObject().value("containing_brushes") == QJsonValue{QJsonArray{}});

      CHECK(results[5].toObject().value("in_world_bounds") == QJsonValue{false});
      CHECK(results[5].toObject().value("inside_solid") == QJsonValue{false});
    }

    SECTION("errors")
    {
      CHECK(f.err("query_point", R"({"points":[]})") == "invalid_argument");
      CHECK(f.err("query_point", R"({"points":[[0,0]]})") == "invalid_argument");
      CHECK(f.err("query_point", R"({"points":[[1e7,0,0]]})") == "invalid_argument");
      CHECK(f.err("query_point") == "invalid_argument");
    }
  }

  SECTION("check_brush_overlaps")
  {
    populateTestScene(f);

    SECTION("a scene without overlaps")
    {
      const auto result = f.ok("check_brush_overlaps");
      CHECK(result.value("pairs") == QJsonValue{QJsonArray{}});
      CHECK(result.value("total") == QJsonValue{0});
      CHECK(result.value("truncated") == QJsonValue{false});
      CHECK(result.value("brush_count") == QJsonValue{4});
    }

    SECTION("finds overlaps, but not touching brushes")
    {
      // overlaps the wall B2 by 32^3
      addBrush(map, vm::bbox3d{{32, 32, 32}, {96, 96, 96}});
      // touches B2
      addBrush(map, vm::bbox3d{{-64, 0, 0}, {0, 64, 64}});
      // overlaps the door brush B5 by 8 x 16 x 64
      addBrush(map, vm::bbox3d{{136, 0, 0}, {152, 16, 64}});

      const auto result = f.ok("check_brush_overlaps");
      CHECK(
        result.value("pairs")
        == QJsonValue{QJsonArray{
          QJsonObject{
            {"a", "B2"},
            {"b", "B12"},
            {"overlap_bounds",
             QJsonObject{
               {"min", QJsonArray{32, 32, 32}}, {"max", QJsonArray{64, 64, 64}}}},
            {"overlap_volume", 32768},
          },
          QJsonObject{
            {"a", "B5"},
            {"b", "B14"},
            {"overlap_bounds",
             QJsonObject{
               {"min", QJsonArray{136, 0, 0}}, {"max", QJsonArray{144, 16, 64}}}},
            {"overlap_volume", 8192},
          },
        }});
      CHECK(result.value("total") == QJsonValue{2});

      // min_volume
      CHECK(
        f.ok("check_brush_overlaps", R"({"min_volume":10000})").value("total")
        == QJsonValue{1});

      // max_pairs
      const auto limited = f.ok("check_brush_overlaps", R"({"max_pairs":1})");
      CHECK(limited.value("pairs").toArray().size() == 1);
      CHECK(limited.value("truncated") == QJsonValue{true});
      CHECK(limited.value("total") == QJsonValue{2});

      // ids restrict the brushes; entities are expanded to their brushes
      CHECK(
        f.ok("check_brush_overlaps", R"({"ids":["B2","B13"]})").value("total")
        == QJsonValue{0});
      const auto door = f.ok("check_brush_overlaps", R"({"ids":["E4","B14"]})");
      CHECK(door.value("total") == QJsonValue{1});
      CHECK(door.value("brush_count") == QJsonValue{3});
      CHECK(
        f.ok("check_brush_overlaps", R"({"ids":["world"]})").value("total")
        == QJsonValue{2});
    }

    SECTION("errors")
    {
      CHECK(f.err("check_brush_overlaps", R"({"ids":["B999"]})") == "unknown_node");
      CHECK(f.err("check_brush_overlaps", R"({"max_pairs":0})") == "invalid_argument");
      CHECK(f.err("check_brush_overlaps", R"({"min_volume":-1})") == "invalid_argument");
      CHECK(
        f.err("check_brush_overlaps", R"({"ids":["@selection"]})") == "invalid_argument");
    }
  }

  SECTION("check_leaks")
  {
    const auto room = vm::bbox3d{{0, 0, 0}, {128, 128, 128}};

    SECTION("a hollow box is sealed")
    {
      addShell(map, room);
      addPointEntity(map, "light", {64, 64, 64});

      const auto result = f.ok("check_leaks");
      CHECK(result.value("sealed") == QJsonValue{true});
      CHECK(result.value("leaks") == QJsonValue{QJsonArray{}});
      CHECK(result.value("start_points_checked") == QJsonValue{1});
      CHECK(result.value("resolution") == QJsonValue{8});
      // the brush bounds expanded by 2 * resolution
      CHECK(
        result.value("bounds")
        == QJsonValue{
          QJsonObject{{"min", jsonVec(-32, -32, -32)}, {"max", jsonVec(160, 160, 160)}}});
      CHECK(result.value("grid") == QJsonValue{QJsonArray{24, 24, 24}});
      CHECK(result.value("cells") == QJsonValue{24 * 24 * 24});
      // the shell's volume in 8 unit cells
      CHECK(
        result.value("solid_cells")
        == QJsonValue{(160 * 160 * 160 - 128 * 128 * 128) / 512});
      // only the interior is flood filled
      CHECK(result.value("elapsed_cells_visited") == QJsonValue{16 * 16 * 16});
    }

    SECTION("a gap narrower than the resolution leaks")
    {
      addShell(map, room, {"+x"});
      // the +x wall in two pieces with a 1 unit gap at y 63..64
      addBrush(map, vm::bbox3d{{128, -16, 0}, {144, 63, 128}});
      addBrush(map, vm::bbox3d{{128, 64, 0}, {144, 144, 128}});
      auto* light = addPointEntity(map, "light", {64, 64, 64});

      const auto result = f.ok("check_leaks");
      CHECK(result.value("sealed") == QJsonValue{false});
      CHECK(result.value("leaks_total") == QJsonValue{1});
      const auto leaks = result.value("leaks").toArray();
      REQUIRE(leaks.size() == 1);
      const auto leak = leaks[0].toObject();
      CHECK(leak.value("start_point") == QJsonValue{jsonVec(64, 64, 64)});
      CHECK(leak.value("start_entity_id") == QJsonValue{f.id(*light)});
      // the nearest outermost cell behind the gap
      CHECK(leak.value("exit_point") == QJsonValue{jsonVec(156, 60, 68)});

      const auto path = leak.value("path").toArray();
      REQUIRE(path.size() >= 3);
      CHECK(path.size() <= 64);
      CHECK(path.first() == leak.value("start_point"));
      CHECK(path.last() == leak.value("exit_point"));
      // the path turns towards the gap
      CHECK(std::ranges::any_of(
        path, [](const auto& point) { return point.toArray()[1] == QJsonValue{60}; }));
      // 11 steps to the outside, one to the gap, plus the start's offset
      CHECK(
        std::abs(leak.value("path_length").toDouble() - (12 * 8 + std::sqrt(48.0)))
        < 1e-5);

      SECTION("explicit start points")
      {
        const auto explicitResult =
          f.ok("check_leaks", R"({"start_points":[[100,100,100]]})");
        CHECK(explicitResult.value("sealed") == QJsonValue{false});
        const auto explicitLeak =
          explicitResult.value("leaks").toArray().first().toObject();
        CHECK(explicitLeak.value("start_point") == QJsonValue{jsonVec(100, 100, 100)});
        CHECK(!explicitLeak.contains("start_entity_id"));
      }
    }

    SECTION("brush entities do not seal")
    {
      addShell(map, room, {"+x"});
      addBrushEntity(map, "func_wall", {vm::bbox3d{{128, -16, 0}, {144, 144, 128}}});
      addPointEntity(map, "light", {64, 64, 64});

      const auto result = f.ok("check_leaks");
      CHECK(result.value("sealed") == QJsonValue{false});
      CHECK(
        result.value("leaks").toArray().first().toObject().value("exit_point")
        == QJsonValue{jsonVec(156, 68, 68)});
    }

    SECTION("brushes in groups seal")
    {
      auto shell = addShell(map, room);
      groupNodes(map, std::vector<mdl::Node*>{shell.begin(), shell.end()}, "room");
      mdl::deselectAll(map);
      addPointEntity(map, "light", {64, 64, 64});

      CHECK(f.ok("check_leaks").value("sealed") == QJsonValue{true});
    }

    SECTION("two rooms joined by a doorway")
    {
      // room A [0..128], room B [144..272] share the wall at x 128..144 with a door
      addShell(map, room, {"+x"});
      auto b = addShell(map, vm::bbox3d{{144, 0, 0}, {272, 128, 128}}, {"-x"});
      addBrush(map, vm::bbox3d{{128, -16, 0}, {144, 32, 128}});
      addBrush(map, vm::bbox3d{{128, 96, 0}, {144, 144, 128}});
      addBrush(map, vm::bbox3d{{128, 32, 96}, {144, 96, 128}});
      auto* light = addPointEntity(map, "light", {64, 64, 64});
      auto* playerStart = addPointEntity(map, "info_player_start", {208, 64, 24});

      const auto sealed = f.ok("check_leaks");
      CHECK(sealed.value("sealed") == QJsonValue{true});
      CHECK(sealed.value("start_points_checked") == QJsonValue{2});
      // both rooms and the doorway, filled once
      CHECK(
        sealed.value("elapsed_cells_visited")
        == QJsonValue{2 * 16 * 16 * 16 + 2 * 8 * 12});

      // without the ceiling of room B, both start points leak through it
      f.ok("delete_nodes", QJsonObject{{"ids", QJsonArray{f.id(*b[1])}}});
      const auto leaking = f.ok("check_leaks");
      CHECK(leaking.value("sealed") == QJsonValue{false});
      const auto leaks = leaking.value("leaks").toArray();
      REQUIRE(leaks.size() == 1);
      const auto leak = leaks[0].toObject();
      CHECK(leak.value("start_entity_id") == QJsonValue{f.id(*light)});
      CHECK(leak.value("exit_point").toArray()[2] == QJsonValue{156});
      CHECK(
        leak.value("shared_start_points")
        == QJsonValue{QJsonArray{QJsonObject{
          {"point", jsonVec(208, 64, 24)}, {"entity_id", f.id(*playerStart)}}}});
      CHECK(leak.value("shared_start_points_count") == QJsonValue{1});
    }

    SECTION("start points inside solid are excluded")
    {
      addShell(map, room);
      auto* buried = addPointEntity(map, "light", {-8, 64, 64});
      addPointEntity(map, "light", {64, 64, 64});

      const auto result = f.ok("check_leaks");
      CHECK(result.value("sealed") == QJsonValue{true});
      CHECK(result.value("start_points_checked") == QJsonValue{1});
      CHECK(
        result.value("excluded_start_points")
        == QJsonValue{QJsonArray{QJsonObject{
          {"point", jsonVec(-8, 64, 64)},
          {"entity_id", f.id(*buried)},
          {"reason", "inside_solid"}}}});
      CHECK(result.value("excluded_start_points_count") == QJsonValue{1});

      // a point on a face is not inside solid; the room next to it is filled
      const auto onFace = f.ok("check_leaks", R"({"start_points":[[0,64,64]]})");
      CHECK(onFace.value("start_points_checked") == QJsonValue{1});
      CHECK(onFace.value("excluded_start_points") == QJsonValue{QJsonArray{}});
      CHECK(onFace.value("sealed") == QJsonValue{true});
      CHECK(onFace.value("elapsed_cells_visited") == QJsonValue{16 * 16 * 16});

      // a point on the seam between two wall brushes is inside solid
      const auto onSeam = f.ok("check_leaks", R"({"start_points":[[-8,64,0]]})");
      CHECK(onSeam.value("start_points_checked") == QJsonValue{0});
      CHECK(
        onSeam.value("excluded_start_points")
        == QJsonValue{QJsonArray{
          QJsonObject{{"point", jsonVec(-8, 64, 0)}, {"reason", "inside_solid"}}}});
      CHECK(onSeam.value("sealed") == QJsonValue{true});
      CHECK(strings(onSeam.value("warnings")).size() == 1);
    }

    SECTION("explicit bounds")
    {
      addShell(map, room);
      // the interior's outermost cells are the boundary
      const auto inner = f.ok(
        "check_leaks",
        R"({"start_points":[[64,64,64]],"bounds":{"min":[0,0,0],"max":[128,128,128]}})");
      CHECK(inner.value("sealed") == QJsonValue{false});
      CHECK(inner.value("grid") == QJsonValue{QJsonArray{16, 16, 16}});
      CHECK(
        f.err(
          "check_leaks",
          R"({"start_points":[[500,0,0]],"bounds":{"min":[0,0,0],"max":[128,128,128]}})")
        == "invalid_argument");
    }

    SECTION("an empty map has nothing to check")
    {
      const auto result = f.ok("check_leaks");
      CHECK(result.value("sealed") == QJsonValue{true});
      CHECK(result.value("cells") == QJsonValue{0});
      CHECK(strings(result.value("warnings")).size() == 1);
    }

    SECTION("an entity without world brushes leaks")
    {
      addPointEntity(map, "light", {64, 64, 64});
      CHECK(f.ok("check_leaks").value("sealed") == QJsonValue{false});
    }

    SECTION("is deterministic")
    {
      addShell(map, room, {"+x"});
      addPointEntity(map, "light", {64, 64, 64});
      addPointEntity(map, "light", {32, 96, 32});
      addPointEntity(map, "light", {-8, 64, 64});
      const auto first = compactJson(f.ok("check_leaks", R"({"resolution":4})"));
      CHECK(compactJson(f.ok("check_leaks", R"({"resolution":4})")) == first);
    }

    SECTION("max_cells caps the work")
    {
      addShell(map, room);
      addPointEntity(map, "light", {64, 64, 64});

      // the bounds [-18..146] need 164^3 cells at resolution 1
      const auto error =
        f.error("check_leaks", R"({"resolution":1,"max_cells":4000000})");
      CHECK(error.value("code") == QJsonValue{"invalid_argument"});
      CHECK(
        error.value("details").toObject().value("cells") == QJsonValue{164 * 164 * 164});
      CHECK(f.err("check_leaks", R"({"max_cells":1000})") == "invalid_argument");
      CHECK(
        f.ok("check_leaks", R"({"resolution":2})").value("sealed") == QJsonValue{true});
      CHECK(
        f.ok("check_leaks", R"({"max_cells":13824})").value("sealed")
        == QJsonValue{true});
    }

    SECTION("layers omitted from export do not seal")
    {
      // the compiler never sees these brushes and entities
      auto* omitted = addLayer(map, "omitted");
      mdl::setOmitLayerFromExport(map, omitted, true);
      addShell(map, room, {"+x"});
      addBrush(map, vm::bbox3d{{128, -16, 0}, {144, 144, 128}}, "material", omitted);
      addPointEntity(map, "light", {64, 64, 64});
      auto* hidden = addPointEntity(map, "light", {500, 500, 500}, omitted);

      CHECK(
        f.ok("check_leaks", R"({"start_points":[[64,64,64]]})").value("sealed")
        == QJsonValue{false});
      const auto result = f.ok("check_leaks");
      CHECK(result.value("sealed") == QJsonValue{false});
      CHECK(result.value("start_points_checked") == QJsonValue{1});
      CHECK(
        result.value("excluded_start_points")
        == QJsonValue{QJsonArray{QJsonObject{
          {"point", jsonVec(500, 500, 500)},
          {"entity_id", f.id(*hidden)},
          {"reason", "omitted_from_export"}}}});

      mdl::setOmitLayerFromExport(map, omitted, false);
      CHECK(f.ok("check_leaks").value("sealed") == QJsonValue{false});
      CHECK(
        f.ok("check_leaks", R"({"start_points":[[64,64,64]]})").value("sealed")
        == QJsonValue{true});
    }

    SECTION("a start point on a face floods the space next to it")
    {
      // a sealed room with a player start next to a leaking room whose only entity
      // stands exactly on its floor
      addShell(map, room);
      addPointEntity(map, "info_player_start", {64, 64, 24});
      const auto other = vm::bbox3d{{300, 0, 0}, {428, 128, 128}};
      addShell(map, other, {"+x"});
      addPointEntity(map, "light", {364, 64, 0});

      const auto result = f.ok("check_leaks");
      CHECK(result.value("sealed") == QJsonValue{false});
      CHECK(result.value("start_points_checked") == QJsonValue{2});
      CHECK(result.value("leaks").toArray().size() == 1);

      // on the ceiling's lower face, the cell containing the point is solid
      CHECK(
        f.ok("check_leaks", R"({"start_points":[[364,64,128]]})").value("sealed")
        == QJsonValue{false});
      // a point inside the wall stays excluded
      CHECK(
        f.ok("check_leaks", R"({"start_points":[[292,64,64]]})")
          .value("start_points_checked")
        == QJsonValue{0});
    }

    SECTION("solid cells of rotated brushes")
    {
      // slabs rotated about x and about z, so that the faces cut the cell lattice at
      // angles and some planes are parallel to x
      auto rotated = std::vector<mdl::BrushNode*>{};
      for (const auto& [axis, z] : std::vector<std::pair<vm::vec3d, double>>{
             {{1, 0, 0}, 0.0}, {{0, 0, 1}, 100.0}, {{0, 1, 1}, -150.0}})
      {
        auto* slab = addBrush(map, vm::bbox3d{{-100, -60, z}, {100, 60, z + 40}});
        mdl::deselectAll(map);
        mdl::selectNodes(map, {slab});
        REQUIRE(mdl::rotateSelection(map, {0, 0, 0}, axis, vm::to_radians(37.0)));
        mdl::deselectAll(map);
        rotated.push_back(slab);
      }
      addPointEntity(map, "light", {300, 300, 300});

      const auto result = f.ok("check_leaks", R"({"resolution":7})");
      const auto min = result.value("bounds").toObject().value("min").toArray();
      const auto grid = result.value("grid").toArray();

      // a cell is solid if its eight corners are inside the same brush
      auto expected = 0;
      for (int z = 0; z < grid[2].toInt(); ++z)
      {
        for (int y = 0; y < grid[1].toInt(); ++y)
        {
          for (int x = 0; x < grid[0].toInt(); ++x)
          {
            expected += std::ranges::any_of(rotated, [&](const auto* brushNode) {
              for (int k = 0; k < 8; ++k)
              {
                const auto corner = vm::vec3d{
                  min[0].toDouble() + 7.0 * (x + (k & 1)),
                  min[1].toDouble() + 7.0 * (y + ((k >> 1) & 1)),
                  min[2].toDouble() + 7.0 * (z + ((k >> 2) & 1))};
                if (!brushNode->brush().containsPoint(corner))
                {
                  return false;
                }
              }
              return true;
            });
          }
        }
      }
      CHECK(expected > 1000);
      CHECK(result.value("solid_cells") == QJsonValue{expected});
    }

    SECTION("errors")
    {
      CHECK(f.err("check_leaks", R"({"resolution":0.5})") == "invalid_argument");
      CHECK(f.err("check_leaks", R"({"max_cells":8000001})") == "invalid_argument");
      CHECK(f.err("check_leaks", R"({"start_points":[]})") == "invalid_argument");
      CHECK(
        f.err("check_leaks", R"({"start_points":[[1e7,0,0]]})") == "invalid_argument");
      CHECK(
        f.err("check_leaks", R"({"bounds":{"min":[1,0,0],"max":[0,1,1]}})")
        == "invalid_argument");
    }
  }
}

} // namespace tb::ui
