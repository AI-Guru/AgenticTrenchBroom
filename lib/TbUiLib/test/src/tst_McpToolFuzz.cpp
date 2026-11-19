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
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PatchNode.h"
#include "mdl/TransactionScope.h"
#include "ui/CatchConfig.h"
#include "ui/McpToolDispatcher.h"

#include <random>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

enum class EditorState
{
  EmptyMap,
  PopulatedMap,
  OpenGroup,
  HiddenLayer,
  LockedNode,
  NodesSelected,
  FacesSelected,
  LinkedGroup,
  TransactionActive,
};

constexpr auto AllStates = std::array{
  EditorState::EmptyMap,
  EditorState::PopulatedMap,
  EditorState::OpenGroup,
  EditorState::HiddenLayer,
  EditorState::LockedNode,
  EditorState::NodesSelected,
  EditorState::FacesSelected,
  EditorState::LinkedGroup,
  EditorState::TransactionActive,
};

const char* stateName(const EditorState state)
{
  switch (state)
  {
  case EditorState::EmptyMap:
    return "empty map";
  case EditorState::PopulatedMap:
    return "populated map";
  case EditorState::OpenGroup:
    return "open group";
  case EditorState::HiddenLayer:
    return "hidden layer";
  case EditorState::LockedNode:
    return "locked node";
  case EditorState::NodesSelected:
    return "nodes selected";
  case EditorState::FacesSelected:
    return "faces selected";
  case EditorState::LinkedGroup:
    return "linked group";
  case EditorState::TransactionActive:
    return "transaction active";
  }
  return "";
}

/** Sets up the given state and returns the ids to try in id slots. */
QStringList setUpState(McpMapToolsFixture& f, const EditorState state)
{
  auto& map = f.map;

  const auto addStaleId = [&](QStringList& ids) {
    auto* removed = addBrush(map, vm::bbox3d{{-64, -64, -64}, {-48, -48, -48}});
    ids.push_back(f.id(*removed));
    mdl::removeNodes(map, {removed});
  };

  auto ids = QStringList{"world", "default_layer", "@selection", "B999999", "L1"};
  if (state == EditorState::EmptyMap)
  {
    addStaleId(ids);
    return ids;
  }

  const auto scene = populateTestScene(f);
  addStaleId(ids);
  ids += QStringList{
    f.id(*scene.wall),
    f.id(*scene.light),
    f.id(*scene.door),
    f.id(*scene.group),
    f.id(*scene.floor),
    f.id(*scene.patch),
    f.id(*scene.layer),
    f.id(*scene.playerStart),
    // wrong type tags
    "G" + f.id(*scene.wall).mid(1),
    "B" + f.id(*scene.light).mid(1),
  };

  switch (state)
  {
  case EditorState::EmptyMap:
  case EditorState::PopulatedMap:
    break;
  case EditorState::OpenGroup:
    mdl::openGroup(map, *scene.group);
    break;
  case EditorState::HiddenLayer:
    mdl::hideLayers(map, {scene.layer});
    break;
  case EditorState::LockedNode:
    mdl::lockNodes(map, {scene.wall});
    break;
  case EditorState::NodesSelected:
    mdl::selectNodes(map, {scene.wall, scene.light, scene.door->children().front()});
    break;
  case EditorState::FacesSelected:
    mdl::selectBrushFaces(map, {{*scene.wall, 0}, {*scene.floor, 3}});
    break;
  case EditorState::LinkedGroup: {
    mdl::selectNodes(map, {scene.group});
    auto* linked = mdl::createLinkedDuplicate(map);
    REQUIRE(linked != nullptr);
    mdl::deselectAll(map);
    ids += QStringList{f.id(*linked), f.id(*linked->children().front())};
    break;
  }
  case EditorState::TransactionActive:
    map.startTransaction("drag", mdl::TransactionScope::LongRunning);
    break;
  }
  return ids;
}

void tearDownState(McpMapToolsFixture& f, const EditorState state)
{
  if (state == EditorState::TransactionActive)
  {
    f.map.cancelTransaction();
  }
}

bool isNodeIdSchema(const QJsonObject& schema)
{
  return schema.value("type") == QJsonValue{"string"} && schema.contains("pattern");
}

bool isNodeIdsSchema(const QJsonObject& schema)
{
  return schema.value("type") == QJsonValue{"array"}
         && isNodeIdSchema(schema.value("items").toObject());
}

QJsonValue wrongTypeValue(const QJsonObject& schema)
{
  const auto type = schema.value("type");
  if (type == QJsonValue{"string"})
  {
    return 42;
  }
  if (type == QJsonValue{"number"} || type == QJsonValue{"integer"})
  {
    return "42";
  }
  if (type == QJsonValue{"boolean"})
  {
    return "true";
  }
  if (type == QJsonValue{"array"})
  {
    return QJsonObject{{"0", 1}};
  }
  if (type == QJsonValue{"object"})
  {
    return QJsonArray{1, 2};
  }
  return QJsonValue{};
}

class ArgumentGenerator
{
public:
  ArgumentGenerator(const uint32_t seed, QStringList ids)
    : m_rng{seed}
    , m_ids{std::move(ids)}
  {
  }

  QJsonValue generate(const QJsonObject& schema, const int depth = 0)
  {
    if (schema.contains("oneOf"))
    {
      const auto alternatives = schema.value("oneOf").toArray();
      return generate(pick(alternatives).toObject(), depth + 1);
    }

    if (schema.contains("enum"))
    {
      return chance(10) ? QJsonValue{"bogus"} : pick(schema.value("enum").toArray());
    }

    auto type = schema.value("type");
    if (type.isArray())
    {
      type = pick(type.toArray());
    }

    const auto typeName = type.toString();
    if (typeName == u"object")
    {
      return generateObject(schema, depth);
    }
    if (typeName == u"array")
    {
      return generateArray(schema, depth);
    }
    if (typeName == u"string")
    {
      return generateString(schema);
    }
    if (typeName == u"integer")
    {
      static const auto values =
        QJsonArray{0, 1, 2, 3, 5, 64, -1, 5000, 5001, 1e9, 1e300, -1e300};
      if (chance(30))
      {
        if (schema.contains("minimum"))
        {
          return schema.value("minimum");
        }
        if (schema.contains("maximum"))
        {
          return schema.value("maximum");
        }
      }
      return pick(values);
    }
    if (typeName == u"number")
    {
      static const auto values =
        QJsonArray{0, 1, -1, 0.5, 16, 64, 1e6, -1e6, 1e7, 1e300, -1e300, 1e-300};
      return pick(values);
    }
    if (typeName == u"boolean")
    {
      return chance(50);
    }
    return QJsonValue{};
  }

private:
  QJsonValue generateObject(const QJsonObject& schema, const int depth)
  {
    auto result = QJsonObject{};
    const auto properties = schema.value("properties").toObject();
    const auto required = schema.value("required").toArray();
    for (auto it = properties.begin(); it != properties.end(); ++it)
    {
      const auto isRequired = required.contains(it.key());
      if ((isRequired && !chance(5)) || (!isRequired && chance(depth == 0 ? 40 : 30)))
      {
        result[it.key()] = generate(it.value().toObject(), depth + 1);
      }
    }
    if (chance(3))
    {
      result["bogus"] = 1;
    }
    return result;
  }

  QJsonValue generateArray(const QJsonObject& schema, const int depth)
  {
    const auto minItems = schema.value("minItems").toInt(0);
    const auto maxItems = schema.value("maxItems").toInt(minItems + 4);
    auto count =
      minItems + int(random(size_t(std::min(maxItems, minItems + 4) - minItems + 1)));
    if (chance(3))
    {
      count = chance(50) ? std::max(0, minItems - 1) : maxItems + 1;
    }
    count = std::min(count, 300);

    auto result = QJsonArray{};
    for (auto i = 0; i < count; ++i)
    {
      result.append(generate(schema.value("items").toObject(), depth + 1));
    }
    return result;
  }

  QJsonValue generateString(const QJsonObject& schema)
  {
    if (schema.contains("pattern") && !chance(10))
    {
      return m_ids[qsizetype(random(size_t(m_ids.size())))];
    }

    static const auto values = QJsonArray{
      "",
      "a",
      "light",
      "func_*",
      "*",
      "?",
      "wall",
      "*a*b*",
      "\\",
      "\"quoted\"",
      "B2",
      QString(300, 'x'),
      QString::fromUtf8("\xC3\xA4\xE2\x82\xAC"),
    };
    return pick(values);
  }

  QJsonValue pick(const QJsonArray& values)
  {
    return values[qsizetype(random(size_t(values.size())))];
  }

  bool chance(const int percent) { return int(random(100)) < percent; }

  size_t random(const size_t n)
  {
    return n == 0 ? 0 : std::uniform_int_distribution<size_t>{0, n - 1}(m_rng);
  }

  std::mt19937 m_rng;
  QStringList m_ids;
};

void checkCall(McpMapToolsFixture& f, const McpToolDef& tool, const QJsonObject& args)
{
  CAPTURE(tool.name, compactJson(args));

  const auto before = snapshot(f.map);
  const auto result = f.call(tool.name, args);

  REQUIRE(result.value("isError").isBool());
  if (result.value("isError").toBool())
  {
    const auto code = result.value("structuredContent")
                        .toObject()
                        .value("error")
                        .toObject()
                        .value("code")
                        .toString();
    CAPTURE(code);
    CHECK(isMcpToolErrorCode(code));
  }
  else
  {
    CHECK(result.value("structuredContent").isObject());
  }

  if (tool.kind == McpToolKind::Read)
  {
    CHECK(snapshot(f.map) == before);
  }
  REQUIRE(f.doc->nodes.verifyBijection());
}

} // namespace

TEST_CASE("McpToolFuzz")
{
  for (const auto state : AllStates)
  {
    CAPTURE(stateName(state));

    auto f = McpMapToolsFixture{};
    const auto ids = setUpState(f, state);
    auto generator = ArgumentGenerator{uint32_t(state) + 1, ids};

    for (const auto* tool : f.dispatcher.tools())
    {
      const auto properties = tool->inputSchema.value("properties").toObject();

      // no arguments
      checkCall(f, *tool, {});

      // each property with the wrong type
      for (auto it = properties.begin(); it != properties.end(); ++it)
      {
        checkCall(f, *tool, {{it.key(), wrongTypeValue(it.value().toObject())}});
      }

      // each id slot with every id
      for (auto it = properties.begin(); it != properties.end(); ++it)
      {
        const auto propertySchema = it.value().toObject();
        for (const auto& id : ids)
        {
          if (isNodeIdSchema(propertySchema))
          {
            checkCall(f, *tool, {{it.key(), id}});
          }
          else if (isNodeIdsSchema(propertySchema))
          {
            checkCall(f, *tool, {{it.key(), QJsonArray{id}}});
            checkCall(f, *tool, {{it.key(), QJsonArray{id, "@selection", ids.back()}}});
          }
        }
      }

      // random arguments derived from the schema
      for (auto i = 0; i < 200; ++i)
      {
        checkCall(f, *tool, generator.generate(tool->inputSchema).toObject());
      }
    }

    tearDownState(f, state);
  }
}

} // namespace tb::ui
