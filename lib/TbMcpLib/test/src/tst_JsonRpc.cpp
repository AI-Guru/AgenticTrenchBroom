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
#include "mcp/JsonRpc.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mcp::jsonrpc
{
namespace
{

Invalid requireInvalid(const QJsonValue& value)
{
  const auto message = parse(value);
  const auto* invalid = std::get_if<Invalid>(&message);
  REQUIRE(invalid != nullptr);
  return *invalid;
}

} // namespace

TEST_CASE("JsonRpc")
{
  SECTION("parse")
  {
    SECTION("request")
    {
      const auto message = parse(
        json(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"a":true}})"));
      const auto* request = std::get_if<Request>(&message);
      REQUIRE(request != nullptr);
      CHECK(request->id == QJsonValue{1});
      CHECK(request->method == "tools/list");
      CHECK(request->params == QJsonObject{{"a", true}});
    }

    SECTION("request without params")
    {
      const auto message = parse(json(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})"));
      const auto* request = std::get_if<Request>(&message);
      REQUIRE(request != nullptr);
      CHECK(request->id == QJsonValue{"abc"});
      CHECK(request->method == "ping");
      CHECK(request->params.isEmpty());
    }

    SECTION("valid id types")
    {
      const auto id = GENERATE(
        QJsonValue{0},
        QJsonValue{-7},
        QJsonValue{2.0},
        QJsonValue{9007199254740992.0},
        QJsonValue{""},
        QJsonValue{"x"});
      CAPTURE(id);

      auto object = QJsonObject{{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", id}};
      const auto message = parse(object);
      const auto* request = std::get_if<Request>(&message);
      REQUIRE(request != nullptr);
      CHECK(request->id == id);
    }

    SECTION("invalid id types")
    {
      const auto id = GENERATE(
        QJsonValue{QJsonValue::Null},
        QJsonValue{1.5},
        QJsonValue{true},
        QJsonValue{false},
        QJsonValue{1e300},
        QJsonValue{QJsonArray{1}},
        QJsonValue{QJsonObject{}});
      CAPTURE(id);

      const auto invalid =
        requireInvalid(QJsonObject{{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", id}});
      CHECK(invalid.id == QJsonValue{QJsonValue::Null});
      CHECK(invalid.code == InvalidRequest);
      CHECK(invalid.message == "id must be a string or an integer");
    }

    SECTION("notification")
    {
      const auto message = parse(json(
        R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{"x":1}})"));
      const auto* notification = std::get_if<Notification>(&message);
      REQUIRE(notification != nullptr);
      CHECK(notification->method == "notifications/initialized");
      CHECK(notification->params == QJsonObject{{"x", 1}});
    }

    SECTION("response")
    {
      SECTION("result")
      {
        const auto message = parse(json(R"({"jsonrpc":"2.0","id":5,"result":{}})"));
        const auto* response = std::get_if<Response>(&message);
        REQUIRE(response != nullptr);
        CHECK(response->id == QJsonValue{5});
      }

      SECTION("error")
      {
        const auto message =
          parse(json(R"({"jsonrpc":"2.0","id":"r","error":{"code":-1,"message":"x"}})"));
        const auto* response = std::get_if<Response>(&message);
        REQUIRE(response != nullptr);
        CHECK(response->id == QJsonValue{"r"});
      }

      SECTION("without id")
      {
        const auto invalid = requireInvalid(json(R"({"jsonrpc":"2.0","result":{}})"));
        CHECK(invalid.code == InvalidRequest);
        CHECK(invalid.message == "response is missing an id");
      }
    }

    SECTION("arrays are not supported")
    {
      const auto invalid =
        requireInvalid(json(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])"));
      CHECK(invalid.id == QJsonValue{QJsonValue::Null});
      CHECK(invalid.code == InvalidRequest);
      CHECK(invalid.message == "batching is not supported");
    }

    SECTION("non-object messages")
    {
      const auto value =
        GENERATE(QJsonValue{1}, QJsonValue{"x"}, QJsonValue{QJsonValue::Null});
      CAPTURE(value);

      const auto invalid = requireInvalid(value);
      CHECK(invalid.code == InvalidRequest);
      CHECK(invalid.message == "message must be a JSON object");
    }

    SECTION("wrong jsonrpc version")
    {
      const auto object = GENERATE(
        json(R"({"id":1,"method":"ping"})"),
        json(R"({"jsonrpc":"1.0","id":1,"method":"ping"})"),
        json(R"({"jsonrpc":2.0,"id":1,"method":"ping"})"));
      CAPTURE(object);

      const auto invalid = requireInvalid(object);
      CHECK(invalid.id == QJsonValue{1});
      CHECK(invalid.code == InvalidRequest);
      CHECK(invalid.message == R"(jsonrpc must be "2.0")");
    }

    SECTION("missing method")
    {
      const auto invalid = requireInvalid(json(R"({"jsonrpc":"2.0","id":3})"));
      CHECK(invalid.id == QJsonValue{3});
      CHECK(invalid.code == InvalidRequest);
      CHECK(invalid.message == "message is missing a method");
    }

    SECTION("method must be a string")
    {
      const auto invalid = requireInvalid(json(R"({"jsonrpc":"2.0","id":3,"method":4})"));
      CHECK(invalid.id == QJsonValue{3});
      CHECK(invalid.code == InvalidRequest);
      CHECK(invalid.message == "method must be a string");
    }

    SECTION("params must be an object")
    {
      auto invalid = requireInvalid(
        json(R"({"jsonrpc":"2.0","id":3,"method":"ping","params":[1,2]})"));
      CHECK(invalid.id == QJsonValue{3});
      CHECK(invalid.code == InvalidParams);
      CHECK(invalid.message == "params must be an object, not an array");

      invalid =
        requireInvalid(json(R"({"jsonrpc":"2.0","id":3,"method":"ping","params":"x"})"));
      CHECK(invalid.id == QJsonValue{3});
      CHECK(invalid.code == InvalidRequest);
      CHECK(invalid.message == "params must be an object");

      invalid =
        requireInvalid(json(R"({"jsonrpc":"2.0","method":"ping","params":null})"));
      CHECK(invalid.id == QJsonValue{QJsonValue::Null});
      CHECK(invalid.code == InvalidRequest);
    }
  }

  SECTION("makeResult")
  {
    CHECK(
      compact(makeResult(QJsonValue{7}, QJsonObject{{"tools", QJsonArray{}}}))
      == R"({"id":7,"jsonrpc":"2.0","result":{"tools":[]}})");
    CHECK(
      compact(makeResult(QJsonValue{"a"}, QJsonObject{}))
      == R"({"id":"a","jsonrpc":"2.0","result":{}})");
  }

  SECTION("makeError")
  {
    CHECK(
      compact(makeError(QJsonValue::Null, ParseError, "parse error"))
      == R"({"error":{"code":-32700,"message":"parse error"},"id":null,"jsonrpc":"2.0"})");
    CHECK(
      compact(makeError(
        QJsonValue{1},
        InvalidParams,
        "unknown tool",
        QJsonObject{{"suggestions", QJsonArray{"a"}}}))
      == R"({"error":{"code":-32602,"data":{"suggestions":["a"]},"message":"unknown tool"},"id":1,"jsonrpc":"2.0"})");

    CHECK(
      QJsonValue{makeError(QJsonValue{1}, InvalidRequest, "x")}["error"]["code"]
      == -32600);
    CHECK(
      QJsonValue{makeError(QJsonValue{1}, MethodNotFound, "x")}["error"]["code"]
      == -32601);
    CHECK(
      QJsonValue{makeError(QJsonValue{1}, InternalError, "x")}["error"]["code"]
      == -32603);
  }
}

} // namespace tb::mcp::jsonrpc
