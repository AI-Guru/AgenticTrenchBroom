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
#include "mcp/JsonSchema.h"
#include "ui/CatchConfig.h"
#include "ui/McpSchemas.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

bool valid(const QJsonObject& schema, const char* json)
{
  const auto value = parseObject(QString{R"({"v":%1})"}.arg(json).toUtf8().constData());
  return !mcp::validate(schema, value.value("v")).has_value();
}

} // namespace

TEST_CASE("McpSchemas")
{
  SECTION("object")
  {
    const auto schema =
      schema::object({{"a", schema::boolean()}, {"b", schema::integer()}}, {"a"});
    CHECK(mcp::usesOnlySupportedKeywords(schema));
    CHECK(schema.value("additionalProperties") == QJsonValue{false});
    CHECK(valid(schema, R"({"a":true})"));
    CHECK(valid(schema, R"({"a":true,"b":3})"));
    CHECK_FALSE(valid(schema, R"({"b":3})"));
    CHECK_FALSE(valid(schema, R"({"a":true,"c":3})"));

    CHECK_FALSE(schema::object({}).contains("required"));
  }

  SECTION("describe")
  {
    CHECK(
      schema::describe(schema::boolean(), "flag").value("description")
      == QJsonValue{"flag"});
  }

  SECTION("scalars")
  {
    CHECK(valid(schema::number(0, 1), "0.5"));
    CHECK_FALSE(valid(schema::number(0, 1), "2"));
    CHECK(valid(schema::integer(0, 10), "10"));
    CHECK_FALSE(valid(schema::integer(0, 10), "1.5"));
    CHECK(valid(schema::string(1, 3), R"("abc")"));
    CHECK_FALSE(valid(schema::string(1, 3), R"("")"));
    CHECK(valid(schema::stringEnum({"x", "y"}), R"("y")"));
    CHECK_FALSE(valid(schema::stringEnum({"x", "y"}), R"("z")"));
    CHECK(valid(schema::boolean(), "false"));
  }

  SECTION("array")
  {
    const auto schema = schema::array(schema::integer(), 1, 2);
    CHECK(valid(schema, "[1]"));
    CHECK_FALSE(valid(schema, "[]"));
    CHECK_FALSE(valid(schema, "[1,2,3]"));
    CHECK(valid(schema::array(schema::integer(), std::nullopt), "[]"));
  }

  SECTION("vec3 and vec2")
  {
    CHECK(valid(schema::vec3(), "[1,2,3]"));
    CHECK_FALSE(valid(schema::vec3(), "[1,2]"));
    CHECK_FALSE(valid(schema::vec3(), R"([1,2,"3"])"));
    CHECK(valid(schema::vec2(), "[1,2]"));
    CHECK_FALSE(valid(schema::vec2(), "[1,2,3]"));
  }

  SECTION("bounds")
  {
    CHECK(valid(schema::bounds(), R"({"min":[0,0,0],"max":[1,1,1]})"));
    CHECK_FALSE(valid(schema::bounds(), R"({"min":[0,0,0]})"));
    CHECK_FALSE(valid(schema::bounds(), R"({"min":[0,0,0],"max":[1,1,1],"x":1})"));
  }

  SECTION("nodeId and nodeIds")
  {
    for (const auto* id :
         {R"("world")",
          R"("default_layer")",
          R"("@selection")",
          R"("B1")",
          R"("L123456789012345678")",
          R"("P9")"})
    {
      CAPTURE(id);
      CHECK(valid(schema::nodeId(), id));
    }
    for (const auto* id :
         {R"("")",
          R"("B0")",
          R"("b1")",
          R"("X1")",
          R"("B1234567890123456789")",
          R"("world ")",
          R"("@sel")"})
    {
      CAPTURE(id);
      CHECK_FALSE(valid(schema::nodeId(), id));
    }

    CHECK(valid(schema::nodeIds(2), R"(["B1","E2"])"));
    CHECK_FALSE(valid(schema::nodeIds(2), R"([])"));
    CHECK_FALSE(valid(schema::nodeIds(2), R"(["B1","E2","G3"])"));
  }

  SECTION("side and axis")
  {
    CHECK(valid(schema::side(), R"("-z")"));
    CHECK_FALSE(valid(schema::side(), R"("z")"));
    CHECK(valid(schema::axis(), R"("x")"));
    CHECK(valid(schema::axis(), "[0,0,1]"));
    CHECK_FALSE(valid(schema::axis(), R"("w")"));
  }

  SECTION("faceRef")
  {
    CHECK(valid(schema::faceRef(), R"({"brush":"B1","index":0})"));
    CHECK(valid(schema::faceRef(), R"({"brush":"B1","side":"+x"})"));
    CHECK(valid(schema::faceRef(), R"({"brush":"B1","normal":[0,0,1]})"));
    CHECK_FALSE(valid(schema::faceRef(), R"({"index":0})"));
    CHECK_FALSE(valid(schema::faceRef(), R"({"brush":"B1","index":-1})"));
  }

  SECTION("material and propValue")
  {
    CHECK(valid(schema::material(), R"("base")"));
    CHECK_FALSE(valid(schema::material(), R"("")"));
    CHECK(valid(schema::propValue(), R"("x")"));
    CHECK(valid(schema::propValue(), "3"));
    CHECK(valid(schema::propValue(), "true"));
    CHECK(valid(schema::propValue(), "[1,2,3]"));
    CHECK_FALSE(valid(schema::propValue(), "{}"));
    CHECK_FALSE(valid(schema::propValue(), "null"));
  }

  SECTION("addPaging")
  {
    auto properties = QJsonObject{};
    schema::addPaging(properties);
    const auto schema = schema::object(properties);
    CHECK(mcp::usesOnlySupportedKeywords(schema));
    CHECK(valid(schema, R"({"offset":0,"limit":5000})"));
    CHECK_FALSE(valid(schema, R"({"limit":0})"));
    CHECK_FALSE(valid(schema, R"({"limit":5001})"));
    CHECK_FALSE(valid(schema, R"({"offset":-1})"));
  }

  SECTION("every builder uses supported keywords")
  {
    for (const auto& schema :
         {schema::boolean(),
          schema::number(),
          schema::integer(),
          schema::string(),
          schema::stringEnum({"a"}),
          schema::array(schema::boolean()),
          schema::vec3(),
          schema::vec2(),
          schema::bounds(),
          schema::nodeId(),
          schema::nodeIds(),
          schema::side(),
          schema::axis(),
          schema::faceRef(),
          schema::material(),
          schema::propValue()})
    {
      CAPTURE(compactJson(schema));
      CHECK(mcp::usesOnlySupportedKeywords(schema));
    }
  }
}

} // namespace tb::ui
