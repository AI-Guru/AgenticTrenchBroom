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
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/CatchConfig.h"
#include "ui/McpDocumentState.h"
#include "ui/McpPreconditions.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

template <typename T>
QString codeOf(const McpResult<T>& result)
{
  return result.is_error() ? errorOf(result).code : QString{};
}

template <typename T>
QJsonObject detailsOf(const McpResult<T>& result)
{
  return result.is_error() ? errorOf(result).details : QJsonObject{};
}

QJsonArray array(std::initializer_list<QString> ids)
{
  auto result = QJsonArray{};
  for (const auto& id : ids)
  {
    result.append(id);
  }
  return result;
}

QJsonArray faceRefs(const char* json)
{
  return parseObject(json).value("faces").toArray();
}

} // namespace

TEST_CASE("McpPreconditions")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create(mdl::QuakeFixtureConfig);
  auto doc = McpDocumentState{map, "d1"};
  const auto id = [&](const mdl::Node& node) { return doc.nodes.idOf(node); };

  auto* brush = addBrush(map, vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
  auto* light = addPointEntity(map, "light", {0, 0, 128});
  auto* door = addBrushEntity(
    map,
    "func_door",
    {vm::bbox3d{{128, 0, 0}, {144, 16, 16}}, vm::bbox3d{{160, 0, 0}, {176, 16, 16}}});
  auto* doorBrush1 = door->children()[0];
  auto* doorBrush2 = door->children()[1];

  SECTION("toolError")
  {
    const auto error = toolError("code", "message", "hint", {{"a", 1}});
    CHECK(error.code == QStringLiteral("code"));
    CHECK(error.message == QStringLiteral("message"));
    CHECK(error.hint == QStringLiteral("hint"));
    CHECK(error.details == QJsonObject{{"a", 1}});
  }

  SECTION("resolveNode")
  {
    CHECK(resolveNode(doc, id(*brush)).value() == brush);
    CHECK(resolveNode(doc, "world").value() == &map.worldNode());
    CHECK(resolveNode(doc, "default_layer").value() == map.worldNode().defaultLayer());

    CHECK(codeOf(resolveNode(doc, "B99")) == QStringLiteral("unknown_node"));
    CHECK(codeOf(resolveNode(doc, "B-1")) == QStringLiteral("invalid_argument"));
    CHECK(codeOf(resolveNode(doc, "@selection")) == QStringLiteral("invalid_argument"));

    const auto wrongType = resolveNode(doc, "B" + id(*light).mid(1));
    CHECK(codeOf(wrongType) == QStringLiteral("wrong_node_type"));
    CHECK(detailsOf(wrongType).value("actual") == QJsonValue{"entity"});
    CHECK(detailsOf(wrongType).value("expected") == QJsonValue{"brush"});
    CHECK(detailsOf(wrongType).value("actual_id") == QJsonValue{id(*light)});
    CHECK(
      errorOf(wrongType).message
      == QString{"B%1 is not a brush; %1 is an entity %2 (node numbers are shared by all "
                 "types)"}
           .arg(id(*light).mid(1), id(*light)));
    CHECK(
      errorOf(wrongType).hint
      == QString{"if you meant the entity, use %1; otherwise call find_nodes to look up "
                 "ids"}
           .arg(id(*light)));
    CHECK(errorOf(resolveNode(doc, "E" + id(*brush).mid(1)))
            .message.startsWith(QString{"E%1 is not an entity;"}.arg(id(*brush).mid(1))));

    const auto brushId = id(*brush);
    doc.nodes.setRemovalLabel("MCP: Delete Nodes");
    mdl::removeNodes(map, {brush});
    const auto stale = resolveNode(doc, brushId);
    CHECK(codeOf(stale) == QStringLiteral("stale_node"));
    CHECK(detailsOf(stale).value("removed_by") == QJsonValue{"MCP: Delete Nodes"});
    CHECK_FALSE(errorOf(stale).hint.isEmpty());
  }

  SECTION("resolveAs")
  {
    CHECK(resolveAs<mdl::BrushNode>(doc, id(*brush), "brush").value() == brush);

    const auto worldAsBrush = resolveAs<mdl::BrushNode>(doc, "world", "brush");
    CHECK(codeOf(worldAsBrush) == QStringLiteral("wrong_node_type"));
    CHECK(detailsOf(worldAsBrush).value("actual") == QJsonValue{"world"});
    CHECK(detailsOf(worldAsBrush).value("expected") == QJsonValue{"brush"});

    CHECK(codeOf(resolveAs<mdl::BrushNode>(doc, "B99", "brush")) == "unknown_node");
  }

  SECTION("resolveNodes")
  {
    mdl::selectNodes(map, {light});
    const auto nodes =
      resolveNodes(doc, array({id(*brush), "@selection", id(*brush), "world"})).value();
    CHECK(nodes == std::vector<mdl::Node*>{brush, light, &map.worldNode()});

    mdl::deselectAll(map);
    CHECK(codeOf(resolveNodes(doc, array({"@selection"}))) == "invalid_argument");
    CHECK(codeOf(resolveNodes(doc, array({"E99"}))) == "unknown_node");

    // @selection can expand beyond the schema's maxItems
    mdl::selectNodes(map, {brush, light, doorBrush1, doorBrush2});
    CHECK(resolveNodes(doc, array({"@selection"}), 4).value().size() == 4);
    const auto tooMany = resolveNodes(doc, array({"@selection"}), 3);
    CHECK(codeOf(tooMany) == "invalid_argument");
    CHECK(detailsOf(tooMany).value("count") == QJsonValue{4});
  }

  SECTION("resolveTargets")
  {
    SECTION("expands @selection and removes duplicates")
    {
      mdl::selectNodes(map, {brush, light});
      CHECK(
        resolveTargets(doc, array({"@selection", id(*light)})).value()
        == std::vector<mdl::Node*>{brush, light});
    }

    SECTION("an empty selection is an error")
    {
      CHECK(codeOf(resolveTargets(doc, array({"@selection"}))) == "invalid_argument");

      mdl::selectBrushFaces(map, {{*brush, 0}});
      const auto facesOnly = resolveTargets(doc, array({"@selection"}));
      CHECK(codeOf(facesOnly) == "invalid_argument");
      CHECK(errorOf(facesOnly).message.contains("brush faces"));
    }

    SECTION("world and layers are rejected")
    {
      CHECK(codeOf(resolveTargets(doc, array({"world"}))) == "wrong_node_type");
      CHECK(codeOf(resolveTargets(doc, array({"default_layer"}))) == "wrong_node_type");
      CHECK(
        codeOf(resolveTargets(doc, array({"default_layer"}), McpTargetPurpose::Delete))
        == "not_supported");
      CHECK(
        codeOf(resolveTargets(doc, array({"world"}), McpTargetPurpose::Delete))
        == "wrong_node_type");
    }

    SECTION("brush entities are expanded for editing")
    {
      CHECK(
        resolveTargets(doc, array({id(*door), id(*doorBrush1)})).value()
        == std::vector<mdl::Node*>{doorBrush1, doorBrush2});
      CHECK(
        resolveTargets(doc, array({id(*door)}), McpTargetPurpose::Delete).value()
        == std::vector<mdl::Node*>{door});
    }

    SECTION("descendants of other targets are dropped")
    {
      auto* group = groupNodes(map, {brush, light}, "group");
      mdl::deselectAll(map);
      CHECK(
        resolveTargets(doc, array({id(*brush), id(*group)})).value()
        == std::vector<mdl::Node*>{group});
    }

    SECTION("a brush in a closed group is not selectable")
    {
      auto* group = groupNodes(map, {brush}, "group");
      mdl::deselectAll(map);
      const auto result = resolveTargets(doc, array({id(*brush)}));
      CHECK(codeOf(result) == "node_not_selectable");
      CHECK(detailsOf(result).value("reason") == QJsonValue{"in_closed_group"});
      CHECK(errorOf(result).hint.contains("open_group"));

      mdl::openGroup(map, *group);
      CHECK(
        resolveTargets(doc, array({id(*brush)})).value()
        == std::vector<mdl::Node*>{brush});

      const auto openGroup = resolveTargets(doc, array({id(*group)}));
      CHECK(codeOf(openGroup) == "node_not_selectable");
      CHECK(detailsOf(openGroup).value("reason") == QJsonValue{"group_open"});
      mdl::closeGroup(map);
    }

    SECTION("a node in a hidden layer is not selectable")
    {
      auto* layer = addLayer(map, "hidden");
      auto* hiddenBrush =
        addBrush(map, vm::bbox3d{{0, 0, 256}, {16, 16, 272}}, "m", layer);
      mdl::hideLayers(map, {layer});
      const auto result = resolveTargets(doc, array({id(*hiddenBrush)}));
      CHECK(codeOf(result) == "node_not_selectable");
      CHECK(detailsOf(result).value("reason") == QJsonValue{"hidden"});
    }

    SECTION("a locked node is not selectable")
    {
      mdl::lockNodes(map, {light});
      const auto result = resolveTargets(doc, array({id(*light)}));
      CHECK(codeOf(result) == "node_not_selectable");
      CHECK(detailsOf(result).value("reason") == QJsonValue{"locked"});

      // locked nodes cannot be deleted either
      CHECK(
        codeOf(resolveTargets(doc, array({id(*light)}), McpTargetPurpose::Delete))
        == "node_not_selectable");
    }

    SECTION("nodes in linked groups are not supported")
    {
      auto* group = groupNodes(map, {brush}, "group");
      auto* linked = mdl::createLinkedDuplicate(map);
      REQUIRE(linked != nullptr);
      mdl::deselectAll(map);

      const auto result = resolveTargets(doc, array({id(*group)}));
      CHECK(codeOf(result) == "not_supported");
      CHECK(detailsOf(result).value("linked_group") == QJsonValue{id(*group)});
      CHECK(codeOf(resolveTargets(doc, array({id(*linked)}))) == "not_supported");

      mdl::openGroup(map, *group);
      CHECK(codeOf(resolveTargets(doc, array({id(*brush)}))) == "not_supported");
      mdl::closeGroup(map);
    }

    SECTION("unknown and stale ids")
    {
      CHECK(codeOf(resolveTargets(doc, array({"B99"}))) == "unknown_node");
      const auto lightId = id(*light);
      mdl::removeNodes(map, {light});
      CHECK(codeOf(resolveTargets(doc, array({lightId}))) == "stale_node");
    }
  }

  SECTION("resolveParent")
  {
    CHECK(resolveParent(doc, std::nullopt).value() == map.worldNode().defaultLayer());
    CHECK(resolveParent(doc, QString{"world"}).value() == map.worldNode().defaultLayer());
    CHECK(resolveParent(doc, id(*door)).value() == door);
    CHECK(codeOf(resolveParent(doc, id(*light))) == "invalid_parent");
    CHECK(codeOf(resolveParent(doc, id(*brush))) == "invalid_parent");
    CHECK(codeOf(resolveParent(doc, QString{"B99"})) == "unknown_node");

    auto* layer = addLayer(map, "layer");
    CHECK(resolveParent(doc, id(*layer)).value() == layer);

    auto* group = groupNodes(map, {brush}, "group");
    mdl::deselectAll(map);
    CHECK(resolveParent(doc, id(*group)).value() == group);

    mdl::selectNodes(map, {group});
    auto* linked = mdl::createLinkedDuplicate(map);
    REQUIRE(linked != nullptr);
    CHECK(codeOf(resolveParent(doc, id(*group))) == "not_supported");

    // an open linked group is not a valid default parent either
    mdl::deselectAll(map);
    mdl::openGroup(map, *group);
    CHECK(codeOf(resolveParent(doc, std::nullopt)) == "not_supported");
    mdl::closeGroup(map);

    // the default is the open group
    mdl::deselectAll(map);
    auto* other = groupNodes(map, {light}, "other");
    mdl::deselectAll(map);
    mdl::openGroup(map, *other);
    CHECK(resolveParent(doc, std::nullopt).value() == other);
    mdl::closeGroup(map);
  }

  SECTION("resolveFaces")
  {
    const auto brushId = id(*brush);
    const auto refs = [&](const QString& json) {
      return faceRefs(QString{R"({"faces":%1})"}.arg(json).toUtf8().constData());
    };

    SECTION("by index, side and normal")
    {
      const auto top = brush->brush().findFace(vm::vec3d{0, 0, 1}).value();
      const auto handles = resolveFaces(
                             doc,
                             refs(QString{R"([{"brush":"%1","index":0},)"
                                          R"({"brush":"%1","side":"+z"},)"
                                          R"({"brush":"%1","normal":[0,0,5]},)"
                                          R"({"brush":"%1","index":0}])"}
                                    .arg(brushId)))
                             .value();
      CHECK(handles == std::vector<mdl::BrushFaceHandle>{{*brush, 0}, {*brush, top}});
    }

    SECTION("index equal to the face count is not found")
    {
      const auto result =
        resolveFaces(doc, refs(QString{R"([{"brush":"%1","index":6}])"}.arg(brushId)));
      CHECK(codeOf(result) == "face_not_found");
      CHECK(detailsOf(result).value("faces").toArray().size() == 6);

      CHECK(
        codeOf(resolveFaces(
          doc, refs(QString{R"([{"brush":"%1","index":1e300}])"}.arg(brushId))))
        == "face_not_found");
    }

    SECTION("side not found")
    {
      mdl::selectNodes(map, {brush});
      REQUIRE(mdl::rotateSelection(map, {32, 32, 32}, {0, 0, 1}, vm::to_radians(45.0)));
      mdl::deselectAll(map);
      CHECK(
        codeOf(resolveFaces(
          doc, refs(QString{R"([{"brush":"%1","side":"+x"}])"}.arg(brushId))))
        == "face_not_found");
    }

    SECTION("exactly one of index, side and normal")
    {
      CHECK(
        codeOf(resolveFaces(doc, refs(QString{R"([{"brush":"%1"}])"}.arg(brushId))))
        == "invalid_argument");
      CHECK(
        codeOf(resolveFaces(
          doc, refs(QString{R"([{"brush":"%1","index":0,"side":"+x"}])"}.arg(brushId))))
        == "invalid_argument");
    }

    SECTION("zero and invalid normals")
    {
      CHECK(
        codeOf(resolveFaces(
          doc, refs(QString{R"([{"brush":"%1","normal":[0,0,0]}])"}.arg(brushId))))
        == "invalid_argument");
      CHECK(
        codeOf(resolveFaces(
          doc, refs(QString{R"([{"brush":"%1","normal":[0,0,1e9]}])"}.arg(brushId))))
        == "invalid_argument");
    }

    SECTION("not a brush")
    {
      CHECK(
        codeOf(resolveFaces(
          doc, refs(QString{R"([{"brush":"%1","index":0}])"}.arg(id(*light)))))
        == "wrong_node_type");
      CHECK(
        codeOf(resolveFaces(doc, refs(R"([{"brush":"world","index":0}])")))
        == "wrong_node_type");
    }

    SECTION("locked brush")
    {
      mdl::lockNodes(map, {brush});
      const auto result =
        resolveFaces(doc, refs(QString{R"([{"brush":"%1","index":0}])"}.arg(brushId)));
      CHECK(codeOf(result) == "node_not_selectable");
      CHECK(detailsOf(result).value("reason") == QJsonValue{"locked"});
    }
  }

  SECTION("vec3")
  {
    CHECK(vec3(QJsonArray{1, 2.5, -3}, "/v").value() == vm::vec3d{1, 2.5, -3});
    CHECK(vec3(QJsonArray{1e6, -1e6, 0}, "/v").value() == vm::vec3d{1e6, -1e6, 0});

    const auto tooLarge = vec3(QJsonArray{0, 1e7, 0}, "/v");
    CHECK(codeOf(tooLarge) == "invalid_argument");
    CHECK(detailsOf(tooLarge).value("pointer") == QJsonValue{"/v/1"});

    CHECK(codeOf(vec3(QJsonArray{1, 2}, "/v")) == "invalid_argument");
    CHECK(codeOf(vec3(QJsonArray{1, 2, "3"}, "/v")) == "invalid_argument");
    CHECK(codeOf(vec3(QJsonValue{3}, "/v")) == "invalid_argument");
  }

  SECTION("bounds")
  {
    const auto box = bounds(parseObject(R"({"min":[0,0,0],"max":[1,2,3]})"), "/b");
    CHECK(box.value() == vm::bbox3d{{0, 0, 0}, {1, 2, 3}});

    // flat boxes are allowed, inverted boxes are not
    CHECK(bounds(parseObject(R"({"min":[0,0,0],"max":[0,2,3]})"), "/b").is_success());
    const auto inverted = bounds(parseObject(R"({"min":[0,5,0],"max":[1,2,3]})"), "/b");
    CHECK(codeOf(inverted) == "invalid_argument");
    CHECK(detailsOf(inverted).value("axis") == QJsonValue{"y"});

    CHECK(codeOf(bounds(parseObject(R"({"min":[0,0,0]})"), "/b")) == "invalid_argument");
  }

  SECTION("boxInWorld")
  {
    CHECK(
      boxInWorld(map, {0, 0, 0}, {64, 64, 64}).value()
      == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});

    const auto empty = boxInWorld(map, {0, 0, 0}, {64, 0, 64});
    CHECK(codeOf(empty) == "invalid_argument");
    CHECK(detailsOf(empty).value("axis") == QJsonValue{"y"});
    // max does exceed min here, so the message must name the real limit
    const auto thin = boxInWorld(map, {400, 0, 0}, {400.0001, 64, 64});
    CHECK(codeOf(thin) == "invalid_argument");
    CHECK(errorOf(thin).message.contains("1/1024"));
    CHECK_FALSE(errorOf(thin).message.contains("must exceed"));
    CHECK(detailsOf(thin).value("minimum") == QJsonValue{1.0 / 1024.0});
    CHECK(detailsOf(thin).value("size").toDouble() > 0.0);
    CHECK(codeOf(boxInWorld(map, {0, 0, 0}, {64, 64, 0.0001})) == "invalid_argument");

    const auto outside = boxInWorld(map, {0, 0, 0}, {64, 64, 100000});
    CHECK(codeOf(outside) == "out_of_world_bounds");
    CHECK(detailsOf(outside).contains("world_bounds"));
  }

  SECTION("requireNoActiveTransaction")
  {
    CHECK(requireNoActiveTransaction(map).is_success());

    map.startTransaction("drag", mdl::TransactionScope::LongRunning);
    const auto busy = requireNoActiveTransaction(map);
    CHECK(codeOf(busy) == "editor_busy");
    CHECK(detailsOf(busy).value("reason") == QJsonValue{"transaction_active"});
    map.cancelTransaction();
  }

  SECTION("canUndo")
  {
    auto otherFixture = mdl::MapFixture{};
    auto& emptyMap = otherFixture.create(mdl::QuakeFixtureConfig);
    CHECK_FALSE(canUndo(emptyMap));

    CHECK(canUndo(map));
    map.startTransaction("drag", mdl::TransactionScope::LongRunning);
    CHECK_FALSE(canUndo(map));
    map.cancelTransaction();
  }

  SECTION("canRedo")
  {
    // regression: Map::canRedoCommand checks the undo stack
    REQUIRE(map.redoCommandName() == nullptr);
    CHECK(map.canRedoCommand());
    CHECK_FALSE(canRedo(map));

    map.undoCommand();
    CHECK(canRedo(map));
    map.startTransaction("drag", mdl::TransactionScope::LongRunning);
    CHECK_FALSE(canRedo(map));
    map.cancelTransaction();
  }

  SECTION("checkMapText")
  {
    CHECK(checkMapText("light", McpMapText::Key).is_success());
    CHECK(checkMapText("", McpMapText::Value).is_success());
    CHECK(codeOf(checkMapText("", McpMapText::Key)) == "invalid_argument");
    CHECK(codeOf(checkMapText("a\"b", McpMapText::Value)) == "invalid_argument");
    CHECK(codeOf(checkMapText("a\nb", McpMapText::Key)) == "invalid_argument");
    CHECK(codeOf(checkMapText("a\rb", McpMapText::Value)) == "invalid_argument");
    CHECK(checkMapText(QString(32 * 1024, 'x'), McpMapText::Value).is_success());
    CHECK(
      codeOf(checkMapText(QString(32 * 1024 + 1, 'x'), McpMapText::Value))
      == "invalid_argument");
  }

  SECTION("isLinkedGroup")
  {
    auto* group = groupNodes(map, {brush}, "group");
    CHECK_FALSE(isLinkedGroup(map, *group));
    auto* linked = mdl::createLinkedDuplicate(map);
    REQUIRE(linked != nullptr);
    CHECK(isLinkedGroup(map, *group));
    CHECK(isLinkedGroup(map, *linked));
  }

  SECTION("sizeArgument")
  {
    const auto args = parseObject(R"({"a":5,"b":1e300,"c":"x","d":-3})");
    CHECK(sizeArgument(args, "a", 1) == 5);
    CHECK(sizeArgument(args, "b", 1) == 1000000000);
    CHECK(sizeArgument(args, "c", 1) == 1);
    CHECK(sizeArgument(args, "d", 1) == 0);
    CHECK(sizeArgument(args, "missing", 7) == 7);
  }

  SECTION("globMatch")
  {
    CHECK(globMatch(u"light", u"light"));
    CHECK(globMatch(u"LIGHT", u"light"));
    CHECK(globMatch(u"light*", u"light_torch"));
    CHECK(globMatch(u"*torch", u"light_torch"));
    CHECK(globMatch(u"l?ght", u"light"));
    CHECK(globMatch(u"*", u""));
    CHECK(globMatch(u"a*b*c", u"aXXbYYc"));
    CHECK_FALSE(globMatch(u"light", u"light_torch"));
    CHECK_FALSE(globMatch(u"?", u""));
    CHECK_FALSE(globMatch(u"a*b*c", u"aXXbYY"));
    // regular expression characters are literal
    CHECK(globMatch(u"a.b[c]", u"a.b[c]"));
    CHECK_FALSE(globMatch(u"a.b", u"axb"));
  }

  SECTION("requireNotLinked")
  {
    auto* group = groupNodes(map, {brush}, "group");
    CHECK(requireNotLinked(doc, *group).is_success());
    CHECK(requireNotLinked(doc, *brush).is_success());
    CHECK(requireNotLinked(doc, *light).is_success());

    auto* linked = mdl::createLinkedDuplicate(map);
    REQUIRE(linked != nullptr);
    CHECK(codeOf(requireNotLinked(doc, *group)) == "not_supported");
    CHECK(codeOf(requireNotLinked(doc, *brush)) == "not_supported");
    CHECK(codeOf(requireNotLinked(doc, *linked->children().front())) == "not_supported");
    CHECK(
      detailsOf(requireNotLinked(doc, *brush)).value("linked_group")
      == QJsonValue{id(*group)});
    CHECK(requireNotLinked(doc, *light).is_success());
  }

  SECTION("propValueText")
  {
    const auto text = [](const char* json) {
      return propValueText(parseObject(json).value("v"), "/set/k");
    };

    CHECK(text(R"({"v":"300"})").value() == "300");
    CHECK(text(R"({"v":""})").value() == "");
    CHECK(text(R"({"v":300})").value() == "300");
    CHECK(text(R"({"v":-2})").value() == "-2");
    CHECK(text(R"({"v":0.5})").value() == "0.5");
    CHECK(text(R"({"v":0.1})").value() == "0.1");
    CHECK(text(R"({"v":-0.0})").value() == "0");
    CHECK(text(R"({"v":1e20})").value() == "1e+20");
    CHECK(text(R"({"v":true})").value() == "1");
    CHECK(text(R"({"v":false})").value() == "0");
    CHECK(text(R"({"v":[1, 0.5, -3]})").value() == "1 0.5 -3");

    CHECK(codeOf(text(R"({"v":[1, 2]})")) == "invalid_argument");
    CHECK(codeOf(text(R"({"v":[1, 2, "3"]})")) == "invalid_argument");
    CHECK(codeOf(text(R"({"v":null})")) == "invalid_argument");
    CHECK(codeOf(text(R"({"v":{"a":1}})")) == "invalid_argument");
    CHECK(codeOf(text(R"({"v":"a\"b"})")) == "invalid_argument");
    CHECK(detailsOf(text(R"({"v":"a\nb"})")).value("pointer") == QJsonValue{"/set/k"});
    CHECK(detailsOf(text(R"({"v":null})")).value("pointer") == QJsonValue{"/set/k"});
  }

  SECTION("checkMaterialName")
  {
    CHECK(checkMaterialName("base/wall1", "/material").is_success());
    CHECK(checkMaterialName("*water", "/material").is_success());
    CHECK(checkMaterialName(QString(256, 'x'), "/material").is_success());

    CHECK(codeOf(checkMaterialName("", "/material")) == "invalid_argument");
    CHECK(
      codeOf(checkMaterialName(QString(257, 'x'), "/material")) == "invalid_argument");
    CHECK(codeOf(checkMaterialName("a b", "/material")) == "invalid_argument");
    CHECK(codeOf(checkMaterialName("a\tb", "/material")) == "invalid_argument");
    CHECK(codeOf(checkMaterialName("a\"b", "/material")) == "invalid_argument");
    CHECK(codeOf(checkMaterialName("a(b", "/material")) == "invalid_argument");
    CHECK(codeOf(checkMaterialName("a)b", "/material")) == "invalid_argument");
    CHECK(
      detailsOf(checkMaterialName("a b", "/side_materials/+x")).value("pointer")
      == QJsonValue{"/side_materials/+x"});
  }
}

} // namespace tb::ui
