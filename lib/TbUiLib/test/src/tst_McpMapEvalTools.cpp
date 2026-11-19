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
          R"([{"description":"worldspawn not found in entity definitions","hidden":false,"line":0,"node":"world","type":"Missing entity definition"}])"));
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
         })
    {
      const auto args =
        parseObject((std::string{R"({"a":)"} + call + "}").c_str()).value("a").toArray();
      f.ok(args[0].toString(), args[1].toObject());
      CHECK(snapshot(map) == before);
    }
  }
}

} // namespace tb::ui
