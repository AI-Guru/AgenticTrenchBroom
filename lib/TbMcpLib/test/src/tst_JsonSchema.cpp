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

#include "TestUtils.h"
#include "mcp/JsonSchema.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mcp
{
namespace
{

/** Returns the violation of the given value against the given schema (both JSON text). */
std::optional<SchemaViolation> check(const char* schema, const QJsonValue& value)
{
  const auto schemaObject = jsonObject(schema);
  REQUIRE(usesOnlySupportedKeywords(schemaObject));
  return validate(schemaObject, value);
}

QString pointerOf(const std::optional<SchemaViolation>& violation)
{
  REQUIRE(violation);
  return violation->pointer;
}

QString messageOf(const std::optional<SchemaViolation>& violation)
{
  REQUIRE(violation);
  return violation->message;
}

const auto BoxSchema = R"({
  "type": "object",
  "properties": {
    "bounds": {
      "type": "object",
      "properties": {
        "min": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
        "max": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}
      },
      "required": ["min", "max"],
      "additionalProperties": false
    },
    "material": {"type": "string", "minLength": 1},
    "parent": {"type": "string"}
  },
  "required": ["bounds"],
  "additionalProperties": false
})";

} // namespace

TEST_CASE("JsonSchema")
{
  SECTION("validate")
  {
    SECTION("empty schema accepts everything")
    {
      const auto value = GENERATE(
        QJsonValue{1},
        QJsonValue{"x"},
        QJsonValue{true},
        QJsonValue{QJsonValue::Null},
        QJsonValue{QJsonArray{1, 2}},
        QJsonValue{QJsonObject{{"a", 1}}});
      CAPTURE(value);
      CHECK(check("{}", value) == std::nullopt);
    }

    SECTION("type")
    {
      using T = std::tuple<const char*, QJsonValue, bool>;

      // clang-format off
      const auto
      [type,        value,                           valid] = GENERATE(values<T>({
      {"object",    QJsonObject{},                   true},
      {"object",    QJsonArray{},                    false},
      {"array",     QJsonArray{},                    true},
      {"array",     QJsonObject{},                   false},
      {"string",    "x",                             true},
      {"string",    1,                               false},
      {"number",    1,                               true},
      {"number",    1.5,                             true},
      {"number",    "1",                             false},
      {"integer",   1,                               true},
      {"integer",   -3.0,                            true},
      {"integer",   1.5,                             false},
      {"integer",   "1",                             false},
      {"boolean",   true,                            true},
      {"boolean",   0,                               false},
      {"null",      QJsonValue::Null,                true},
      {"null",      false,                           false},
      }));
      // clang-format on

      CAPTURE(type, value);

      const auto schema = QJsonObject{{"type", type}};
      const auto result = validate(schema, value);
      CHECK(result.has_value() == !valid);
    }

    SECTION("type mismatch message and details")
    {
      const auto result = check(R"({"type":"string"})", QJsonValue{1.5});
      REQUIRE(result);
      CHECK(result->pointer == "");
      CHECK(result->message == "expected string, got number");
      CHECK(
        result->details
        == QJsonObject{{"expected", QJsonArray{"string"}}, {"actual", "number"}});

      CHECK(
        messageOf(check(R"({"type":"object"})", QJsonValue{QJsonValue::Undefined}))
        == "expected object, got nothing");
    }

    SECTION("type array")
    {
      constexpr auto schema = R"({"type":["string","number","boolean","array"]})";
      CHECK(check(schema, QJsonValue{"x"}) == std::nullopt);
      CHECK(check(schema, QJsonValue{2}) == std::nullopt);
      CHECK(check(schema, QJsonValue{false}) == std::nullopt);
      CHECK(check(schema, QJsonValue{QJsonArray{}}) == std::nullopt);
      CHECK(
        messageOf(check(schema, QJsonValue{QJsonObject{}}))
        == "expected string or number or boolean or array, got object");
    }

    SECTION("properties")
    {
      constexpr auto schema =
        R"({"type":"object","properties":{"a":{"type":"integer"},"b":{"type":"string"}}})";
      CHECK(check(schema, json(R"({"a":1,"b":"x"})")) == std::nullopt);
      CHECK(check(schema, json(R"({})")) == std::nullopt);

      const auto result = check(schema, json(R"({"a":1,"b":2})"));
      CHECK(pointerOf(result) == "/b");
      CHECK(messageOf(result) == "expected string, got number");

      // additional properties are allowed by default
      CHECK(check(schema, json(R"({"a":1,"c":[]})")) == std::nullopt);
    }

    SECTION("required")
    {
      constexpr auto schema = R"({"type":"object","required":["a","b"]})";
      CHECK(check(schema, json(R"({"a":1,"b":2})")) == std::nullopt);

      const auto result = check(schema, json(R"({"a":1})"));
      REQUIRE(result);
      CHECK(result->pointer == "/b");
      CHECK(result->message == "missing required argument 'b'");
      CHECK(result->details == QJsonObject{{"missing", "b"}});

      // required properties are checked in order
      CHECK(pointerOf(check(schema, json(R"({})"))) == "/a");
    }

    SECTION("additionalProperties")
    {
      constexpr auto schema =
        R"({"type":"object","properties":{"b":{},"a":{},"c":{}},"additionalProperties":false})";
      CHECK(check(schema, json(R"({"a":1,"b":2,"c":3})")) == std::nullopt);

      const auto result = check(schema, json(R"({"a":1,"x":2})"));
      REQUIRE(result);
      CHECK(result->pointer == "/x");
      CHECK(result->message == "unknown argument 'x'; allowed: a, b, c");
      CHECK(result->details == QJsonObject{{"allowed", QJsonArray{"a", "b", "c"}}});

      CHECK(
        messageOf(check(R"({"additionalProperties":false})", json(R"({"y":1})")))
        == "unknown argument 'y'; allowed: (none)");
    }

    SECTION("items")
    {
      constexpr auto schema = R"({"type":"array","items":{"type":"number"}})";
      CHECK(check(schema, json("[]")) == std::nullopt);
      CHECK(check(schema, json("[1,2,3]")) == std::nullopt);
      CHECK(pointerOf(check(schema, json(R"([1,2,"x"])"))) == "/2");
    }

    SECTION("minItems")
    {
      constexpr auto schema = R"({"type":"array","minItems":2})";
      CHECK(check(schema, json("[1,2]")) == std::nullopt);

      const auto result = check(schema, json("[1]"));
      REQUIRE(result);
      CHECK(result->pointer == "");
      CHECK(result->message == "must have at least 2 items, got 1");
      CHECK(result->details == QJsonObject{{"minItems", 2}, {"actual", 1}});
    }

    SECTION("maxItems")
    {
      constexpr auto schema = R"({"type":"array","maxItems":2})";
      CHECK(check(schema, json("[1,2]")) == std::nullopt);
      CHECK(
        messageOf(check(schema, json("[1,2,3]"))) == "must have at most 2 items, got 3");
    }

    SECTION("enum")
    {
      constexpr auto schema = R"({"enum":["+x","-x",1,null]})";
      CHECK(check(schema, QJsonValue{"+x"}) == std::nullopt);
      CHECK(check(schema, QJsonValue{1}) == std::nullopt);
      CHECK(check(schema, QJsonValue{1.0}) == std::nullopt);
      CHECK(check(schema, QJsonValue{QJsonValue::Null}) == std::nullopt);

      const auto result = check(schema, QJsonValue{"+y"});
      REQUIRE(result);
      CHECK(result->message == "must be one of: +x, -x, 1, null");
      CHECK(result->details == QJsonObject{{"allowed", json(R"(["+x","-x",1,null])")}});
    }

    SECTION("const")
    {
      constexpr auto schema = R"({"properties":{"a":{"const":"replace"}}})";
      CHECK(check(schema, json(R"({"a":"replace"})")) == std::nullopt);

      const auto result = check(schema, json(R"({"a":"add"})"));
      REQUIRE(result);
      CHECK(result->pointer == "/a");
      CHECK(result->message == "must be replace");
      CHECK(result->details == QJsonObject{{"expected", "replace"}});

      CHECK(check(R"({"const":[1,2]})", json("[1,2]")) == std::nullopt);
      CHECK(messageOf(check(R"({"const":[1,2]})", json("[2,1]"))) == "must be [1,2]");
    }

    SECTION("minimum")
    {
      constexpr auto schema = R"({"type":"number","minimum":0.5})";
      CHECK(check(schema, QJsonValue{0.5}) == std::nullopt);
      CHECK(check(schema, QJsonValue{10}) == std::nullopt);

      const auto result = check(schema, QJsonValue{0.25});
      REQUIRE(result);
      CHECK(result->message == "must be >= 0.5");
      CHECK(result->details == QJsonObject{{"minimum", 0.5}});
    }

    SECTION("maximum")
    {
      constexpr auto schema = R"({"type":"integer","maximum":64})";
      CHECK(check(schema, QJsonValue{64}) == std::nullopt);
      CHECK(messageOf(check(schema, QJsonValue{65})) == "must be <= 64");
    }

    SECTION("minLength")
    {
      constexpr auto schema = R"({"type":"string","minLength":2})";
      CHECK(check(schema, QJsonValue{"ab"}) == std::nullopt);
      CHECK(
        messageOf(check(schema, QJsonValue{"a"}))
        == "must be at least 2 characters long");

      // a surrogate pair is one code point
      CHECK(check(schema, QJsonValue{QString::fromUtf8("\xF0\x9F\x98\x80")}).has_value());
    }

    SECTION("maxLength")
    {
      constexpr auto schema = R"({"type":"string","maxLength":1})";
      CHECK(check(schema, QJsonValue{"a"}) == std::nullopt);
      CHECK(
        check(schema, QJsonValue{QString::fromUtf8("\xF0\x9F\x98\x80")}) == std::nullopt);
      CHECK(
        messageOf(check(schema, QJsonValue{"ab"}))
        == "must be at most 1 characters long");
    }

    SECTION("pattern")
    {
      constexpr auto schema = R"({"type":"string","pattern":"^[LGEBP][1-9][0-9]*$"})";
      CHECK(check(schema, QJsonValue{"B17"}) == std::nullopt);

      const auto result = check(schema, QJsonValue{"B0"});
      REQUIRE(result);
      CHECK(result->message == "must match the pattern ^[LGEBP][1-9][0-9]*$");
      CHECK(result->details == QJsonObject{{"pattern", "^[LGEBP][1-9][0-9]*$"}});

      // patterns are not anchored implicitly
      CHECK(check(R"({"pattern":"b"})", QJsonValue{"abc"}) == std::nullopt);

      // case-sensitive
      CHECK(check(schema, QJsonValue{"b17"}).has_value());
    }

    SECTION("oneOf")
    {
      constexpr auto schema = R"({"oneOf":[
        {"enum":["x","y","z"]},
        {"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}
      ]})";
      CHECK(check(schema, QJsonValue{"x"}) == std::nullopt);
      CHECK(check(schema, json("[0,0,1]")) == std::nullopt);

      const auto result = check(schema, QJsonValue{"w"});
      REQUIRE(result);
      CHECK(result->pointer == "");
      CHECK(
        result->message
        == "does not match any of the allowed alternatives (must be one of: x, y, z; "
           "expected array, got string)");
      CHECK(
        result->details
        == QJsonObject{
          {"alternatives",
           QJsonArray{
             QJsonObject{{"pointer", ""}, {"message", "must be one of: x, y, z"}},
             QJsonObject{{"pointer", ""}, {"message", "expected array, got string"}},
           }},
        });

      // matching more than one alternative is a violation
      constexpr auto ambiguous = R"({"oneOf":[{"type":"number"},{"type":"integer"}]})";
      CHECK(check(ambiguous, QJsonValue{1.5}) == std::nullopt);

      const auto ambiguousResult = check(ambiguous, QJsonValue{1});
      REQUIRE(ambiguousResult);
      CHECK(
        ambiguousResult->message == "matches 2 alternatives, but must match exactly one");
      CHECK(ambiguousResult->details == QJsonObject{{"matching", 2}});
    }

    SECTION("description, title and default are ignored")
    {
      constexpr auto schema =
        R"({"type":"integer","description":"d","title":"t","default":"not an integer"})";
      CHECK(check(schema, QJsonValue{1}) == std::nullopt);
    }

    SECTION("nested pointers")
    {
      CHECK(
        check(BoxSchema, json(R"({"bounds":{"min":[0,0,0],"max":[1,1,1]}})"))
        == std::nullopt);

      CHECK(
        pointerOf(check(BoxSchema, json(R"({"bounds":{"min":[0,0,"a"],"max":[1,1,1]}})")))
        == "/bounds/min/2");
      CHECK(
        pointerOf(check(BoxSchema, json(R"({"bounds":{"min":[0,0,0]}})")))
        == "/bounds/max");
      CHECK(
        pointerOf(check(BoxSchema, json(R"({"bounds":{"min":[0,0],"max":[1,1,1]}})")))
        == "/bounds/min");
      CHECK(
        pointerOf(
          check(BoxSchema, json(R"({"bounds":{"min":[0,0,0],"max":[1,1,1],"mid":[]}})")))
        == "/bounds/mid");
      CHECK(
        pointerOf(check(
          BoxSchema, json(R"({"bounds":{"min":[0,0,0],"max":[1,1,1]},"material":""})")))
        == "/material");
      CHECK(pointerOf(check(BoxSchema, json(R"({})"))) == "/bounds");
    }

    SECTION("pointer tokens are escaped")
    {
      constexpr auto schema =
        R"({"properties":{"a/b":{"type":"string"},"c~d":{"type":"string"}}})";
      CHECK(pointerOf(check(schema, json(R"({"a/b":1})"))) == "/a~1b");
      CHECK(pointerOf(check(schema, json(R"({"c~d":1})"))) == "/c~0d");
    }

    SECTION("an invalid pattern is reported")
    {
      const auto schema = QJsonObject{{"pattern", "("}};
      const auto result = validate(schema, QJsonValue{"x"});
      REQUIRE(result);
      CHECK(result->message == "schema pattern '(' is invalid");
    }
  }

  SECTION("usesOnlySupportedKeywords")
  {
    SECTION("supported schemas")
    {
      CHECK(usesOnlySupportedKeywords(QJsonObject{}));
      CHECK(usesOnlySupportedKeywords(jsonObject(BoxSchema)));
      CHECK(usesOnlySupportedKeywords(jsonObject(R"({
        "type": ["string", "null"],
        "title": "t",
        "description": "d",
        "default": null,
        "enum": ["a", null],
        "const": "a",
        "minLength": 0,
        "maxLength": 10,
        "pattern": "^a$",
        "minimum": -1.5,
        "maximum": 2,
        "minItems": 0,
        "maxItems": 3,
        "oneOf": [{"type": "string"}, {"items": {"type": "integer"}}]
      })")));
    }

    SECTION("unsupported schemas")
    {
      using T = std::tuple<const char*, QString>;

      // clang-format off
      const auto
      [schema,                                                    offending] = GENERATE(values<T>({
      {R"({"format":"uri"})",                                     "/format"},
      {R"({"$ref":"#/x"})",                                       "/$ref"},
      {R"({"anyOf":[]})",                                         "/anyOf"},
      {R"({"type":"int"})",                                       "/type"},
      {R"({"type":[]})",                                          "/type"},
      {R"({"type":["string",1]})",                                "/type"},
      {R"({"properties":{"a":{"exclusiveMinimum":0}}})",          "/properties/a/exclusiveMinimum"},
      {R"({"properties":{"a":1}})",                               "/properties/a"},
      {R"({"properties":[]})",                                    "/properties"},
      {R"({"required":"a"})",                                     "/required"},
      {R"({"required":[1]})",                                     "/required"},
      {R"({"additionalProperties":true})",                        "/additionalProperties"},
      {R"({"additionalProperties":{"type":"string"}})",           "/additionalProperties"},
      {R"({"items":[{"type":"string"}]})",                        "/items"},
      {R"({"items":{"uniqueItems":true}})",                       "/items/uniqueItems"},
      {R"({"minItems":-1})",                                      "/minItems"},
      {R"({"maxItems":1.5})",                                     "/maxItems"},
      {R"({"minLength":"1"})",                                    "/minLength"},
      {R"({"maximum":"1"})",                                      "/maximum"},
      {R"({"enum":[]})",                                          "/enum"},
      {R"({"pattern":"("})",                                      "/pattern"},
      {R"({"pattern":1})",                                        "/pattern"},
      {R"({"oneOf":[]})",                                         "/oneOf"},
      {R"({"oneOf":[1]})",                                        "/oneOf/0"},
      {R"({"oneOf":[{},{"not":{}}]})",                            "/oneOf/1/not"},
      {R"({"description":1})",                                    "/description"},
      }));
      // clang-format on

      CAPTURE(schema);

      auto actual = QString{};
      CHECK_FALSE(usesOnlySupportedKeywords(jsonObject(schema), &actual));
      CHECK(actual == offending);

      // offending may be null
      CHECK_FALSE(usesOnlySupportedKeywords(jsonObject(schema)));
    }
  }
}

} // namespace tb::mcp
