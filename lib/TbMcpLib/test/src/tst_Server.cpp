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

#include <QUuid>

#include "TestUtils.h"
#include "mcp/Server.h"
#include "mcp/Tool.h"

#include <optional>
#include <stdexcept>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mcp
{
namespace
{

constexpr auto Port = quint16{6767};

using Headers = std::vector<std::pair<QByteArray, QByteArray>>;

/** Header names must be lower case, as produced by HttpRequestParser. */
HttpRequest makeRequest(
  QByteArray method, QByteArray target, Headers headers, QByteArray body = {})
{
  return HttpRequest{
    .method = std::move(method),
    .target = std::move(target),
    .version = "HTTP/1.1",
    .headers = std::move(headers),
    .body = std::move(body),
  };
}

Headers defaultHeaders(const std::optional<QString>& sessionId = std::nullopt)
{
  auto headers = Headers{
    {"host", "127.0.0.1:6767"},
    {"content-type", "application/json"},
    {"accept", "application/json, text/event-stream"},
  };
  if (sessionId)
  {
    headers.emplace_back("mcp-session-id", sessionId->toUtf8());
  }
  return headers;
}

HttpRequest post(QByteArray body, const std::optional<QString>& sessionId = std::nullopt)
{
  return makeRequest("POST", "/mcp", defaultHeaders(sessionId), std::move(body));
}

HttpRequest withHeader(HttpRequest request, QByteArray name, QByteArray value)
{
  for (auto& [n, v] : request.headers)
  {
    if (n == name)
    {
      v = std::move(value);
      return request;
    }
  }
  request.headers.emplace_back(std::move(name), std::move(value));
  return request;
}

HttpRequest withoutHeader(HttpRequest request, const QByteArray& name)
{
  std::erase_if(
    request.headers, [&](const auto& header) { return header.first == name; });
  return request;
}

QByteArray requestBody(
  const QJsonValue& id, const QString& method, const QJsonObject& params = {})
{
  auto object = QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
  if (!params.isEmpty())
  {
    object.insert("params", params);
  }
  return compact(object);
}

QByteArray notificationBody(const QString& method)
{
  return compact(QJsonObject{{"jsonrpc", "2.0"}, {"method", method}});
}

const QJsonObject bodyOf(const HttpResponse& response)
{
  REQUIRE(response.header("content-type") == QByteArray{"application/json"});
  auto error = QJsonParseError{};
  const auto document = QJsonDocument::fromJson(response.body, &error);
  REQUIRE(error.error == QJsonParseError::NoError);
  REQUIRE(document.isObject());
  return document.object();
}

int errorCodeOf(const HttpResponse& response)
{
  return bodyOf(response)["error"]["code"].toInt();
}

QString initialize(Server& server, const QString& version = LatestProtocolVersion)
{
  const auto response = server.handle(
    post(requestBody(0, "initialize", {{"protocolVersion", version}})), Port);
  REQUIRE(response.status == 200);
  const auto sessionId = response.header("mcp-session-id");
  REQUIRE(sessionId);
  return QString::fromUtf8(*sessionId);
}

const QJsonObject call(
  Server& server,
  const QString& sessionId,
  const QString& method,
  const QJsonObject& params = {},
  const QJsonValue& id = 1)
{
  const auto response =
    server.handle(post(requestBody(id, method, params), sessionId), Port);
  REQUIRE(response.status == 200);
  return bodyOf(response);
}

QJsonObject objectSchema(const QJsonObject& properties = {})
{
  return QJsonObject{
    {"type", "object"},
    {"properties", properties},
    {"additionalProperties", false},
  };
}

} // namespace

TEST_CASE("Server")
{
  auto tools = ToolRegistry{};
  auto receivedArguments = std::optional<QJsonObject>{};

  tools.add(ToolSpec{
    .name = "echo",
    .title = "Echo",
    .description = "Returns its arguments.",
    .inputSchema = objectSchema({{"text", QJsonObject{{"type", "string"}}}}),
    .annotations = {.readOnly = true, .destructive = false, .idempotent = true},
    .handler = [&](const QJsonObject& arguments) -> ToolResult {
      receivedArguments = arguments;
      return ToolOutput{.structured = arguments, .extraContent = {}};
    },
  });
  tools.add(ToolSpec{
    .name = "fail",
    .title = "Fail",
    .description = "Always fails.",
    .inputSchema = {},
    .annotations = {.readOnly = false, .destructive = true, .idempotent = false},
    .handler = [](const QJsonObject&) -> ToolResult {
      return ToolError{
        .code = "invalid_argument",
        .message = "always fails",
        .hint = "don't call it",
        .details = {{"pointer", "/x"}},
      };
    },
  });
  tools.add(ToolSpec{
    .name = "throw",
    .title = "Throw",
    .description = "Throws an exception.",
    .inputSchema = objectSchema(),
    .annotations = {},
    .handler = [](const QJsonObject&) -> ToolResult { throw std::runtime_error{"boom"}; },
  });

  auto nextSession = 1;
  auto closedSessions = std::vector<QString>{};
  auto server = Server{
    ServerInfo{
      .name = "trenchbroom",
      .title = "TrenchBroom",
      .version = "1.2.3",
      .instructions = "Be nice.",
    },
    tools,
    [&]() { return QString{"s%1"}.arg(nextSession++); }};
  server.sessionClosed = [&](const QString& id) { closedSessions.push_back(id); };

  SECTION("handle")
  {
    SECTION("initialize")
    {
      SECTION("creates a session and returns the server's capabilities")
      {
        const auto response = server.handle(
          post(requestBody(
            7,
            "initialize",
            {{"protocolVersion", "2025-06-18"},
             {"capabilities", QJsonObject{}},
             {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}})),
          Port);

        CHECK(response.status == 200);
        CHECK(response.header("mcp-session-id") == QByteArray{"s1"});
        CHECK(bodyOf(response) == jsonObject(R"({
            "jsonrpc": "2.0",
            "id": 7,
            "result": {
              "protocolVersion": "2025-06-18",
              "capabilities": {"tools": {"listChanged": false}},
              "serverInfo": {"name": "trenchbroom", "title": "TrenchBroom", "version": "1.2.3"},
              "instructions": "Be nice."
            }
          })"));

        CHECK(server.sessionIds() == std::vector<QString>{"s1"});
        const auto* session = server.session("s1");
        REQUIRE(session != nullptr);
        CHECK(session->id == "s1");
        CHECK(session->protocolVersion == "2025-06-18");
        CHECK_FALSE(session->initialized);
      }

      SECTION("echoes a supported version")
      {
        const auto sessionId = initialize(server, "2025-03-26");
        REQUIRE(server.session(sessionId) != nullptr);
        CHECK(server.session(sessionId)->protocolVersion == "2025-03-26");
      }

      SECTION("falls back to the latest version")
      {
        const auto version = GENERATE(
          QJsonValue{"1999-01-01"}, QJsonValue{5}, QJsonValue{QJsonValue::Undefined});
        CAPTURE(version);

        auto params = QJsonObject{};
        if (!version.isUndefined())
        {
          params.insert("protocolVersion", version);
        }
        const auto response =
          server.handle(post(requestBody(1, "initialize", params)), Port);
        REQUIRE(response.status == 200);
        CHECK(bodyOf(response)["result"]["protocolVersion"] == "2025-06-18");
      }

      SECTION("omits empty instructions")
      {
        auto otherServer = Server{ServerInfo{.version = "1"}, tools};
        const auto response =
          otherServer.handle(post(requestBody(1, "initialize", {})), Port);
        REQUIRE(response.status == 200);
        CHECK_FALSE(bodyOf(response)["result"].toObject().contains("instructions"));
      }

      SECTION("default session ids are UUIDs")
      {
        auto otherServer = Server{ServerInfo{.version = "1"}, tools};
        const auto response =
          otherServer.handle(post(requestBody(1, "initialize", {})), Port);
        const auto sessionId = response.header("mcp-session-id");
        REQUIRE(sessionId);
        CHECK(sessionId->size() == 36);
        CHECK_FALSE(QUuid::fromString(QString::fromUtf8(*sessionId)).isNull());
      }

      SECTION("does not require a session header and ignores one")
      {
        const auto first = initialize(server);
        const auto response = server.handle(
          post(requestBody(1, "initialize", {{"protocolVersion", "2025-06-18"}}), first),
          Port);
        CHECK(response.status == 200);
        CHECK(response.header("mcp-session-id") == QByteArray{"s2"});
        CHECK(server.sessionIds() == std::vector<QString>{"s1", "s2"});
      }

      SECTION("a notification is not an initialize request")
      {
        const auto response = server.handle(post(notificationBody("initialize")), Port);
        CHECK(response.status == 400);
        CHECK(server.sessionIds().empty());
      }
    }

    SECTION("notifications and responses get 202")
    {
      const auto sessionId = initialize(server);

      auto response = server.handle(
        post(notificationBody("notifications/initialized"), sessionId), Port);
      CHECK(response.status == 202);
      CHECK(response.body.isEmpty());
      REQUIRE(server.session(sessionId) != nullptr);
      CHECK(server.session(sessionId)->initialized);

      response =
        server.handle(post(notificationBody("notifications/cancelled"), sessionId), Port);
      CHECK(response.status == 202);

      response =
        server.handle(post(notificationBody("unknown/notification"), sessionId), Port);
      CHECK(response.status == 202);

      response = server.handle(
        post(R"({"jsonrpc":"2.0","id":"server-1","result":{}})", sessionId), Port);
      CHECK(response.status == 202);
      CHECK(response.body.isEmpty());
    }

    SECTION("ping")
    {
      const auto sessionId = initialize(server);
      CHECK(
        call(server, sessionId, "ping", {}, "p")
        == jsonObject(R"({"jsonrpc":"2.0","id":"p","result":{}})"));
    }

    SECTION("tools/list")
    {
      const auto sessionId = initialize(server);
      const auto body = call(server, sessionId, "tools/list");
      CHECK(body["result"].toObject() == jsonObject(R"({"tools": [
          {
            "name": "echo",
            "title": "Echo",
            "description": "Returns its arguments.",
            "inputSchema": {
              "type": "object",
              "properties": {"text": {"type": "string"}},
              "additionalProperties": false
            },
            "annotations": {
              "title": "Echo",
              "readOnlyHint": true,
              "destructiveHint": false,
              "idempotentHint": true,
              "openWorldHint": false
            }
          },
          {
            "name": "fail",
            "title": "Fail",
            "description": "Always fails.",
            "inputSchema": {"type": "object"},
            "annotations": {
              "title": "Fail",
              "readOnlyHint": false,
              "destructiveHint": true,
              "idempotentHint": false,
              "openWorldHint": false
            }
          },
          {
            "name": "throw",
            "title": "Throw",
            "description": "Throws an exception.",
            "inputSchema": {"type": "object", "properties": {}, "additionalProperties": false},
            "annotations": {
              "title": "Throw",
              "readOnlyHint": false,
              "destructiveHint": false,
              "idempotentHint": false,
              "openWorldHint": false
            }
          }
        ]})"));
    }

    SECTION("tools/call")
    {
      const auto sessionId = initialize(server);

      SECTION("success")
      {
        const auto body = call(
          server,
          sessionId,
          "tools/call",
          {{"name", "echo"}, {"arguments", QJsonObject{{"text", "hi"}}}});
        CHECK(receivedArguments == QJsonObject{{"text", "hi"}});
        CHECK(body == jsonObject(R"({"jsonrpc": "2.0", "id": 1, "result": {
            "content": [{"type": "text", "text": "{\"text\":\"hi\"}"}],
            "structuredContent": {"text": "hi"},
            "isError": false
          }})"));
      }

      SECTION("absent arguments are passed as an empty object")
      {
        const auto body = call(server, sessionId, "tools/call", {{"name", "echo"}});
        CHECK(receivedArguments == QJsonObject{});
        CHECK(body["result"]["isError"] == false);
      }

      SECTION("tool error")
      {
        const auto body = call(server, sessionId, "tools/call", {{"name", "fail"}});
        CHECK(body == jsonObject(R"({"jsonrpc": "2.0", "id": 1, "result": {
            "content": [{"type": "text", "text": "invalid_argument: always fails Hint: don't call it"}],
            "structuredContent": {"error": {
              "code": "invalid_argument",
              "message": "always fails",
              "hint": "don't call it",
              "details": {"pointer": "/x"}
            }},
            "isError": true
          }})"));
      }

      SECTION("missing or non-string name")
      {
        const auto params = GENERATE(
          QJsonObject{},
          QJsonObject{{"name", 1}},
          QJsonObject{{"name", QJsonValue::Null}});
        CAPTURE(params);

        // an empty params object is omitted from the request
        const auto body = call(server, sessionId, "tools/call", params);
        CHECK(body == jsonObject(R"({"jsonrpc": "2.0", "id": 1, "error": {
            "code": -32602,
            "message": "tools/call requires a string 'name'"
          }})"));
      }

      SECTION("unknown tool gives suggestions")
      {
        const auto body = call(server, sessionId, "tools/call", {{"name", "ech"}});
        CHECK(body == jsonObject(R"({"jsonrpc": "2.0", "id": 1, "error": {
            "code": -32602,
            "message": "unknown tool 'ech'",
            "data": {"suggestions": ["echo", "fail", "throw"]}
          }})"));

        CHECK(
          call(server, sessionId, "tools/call", {{"name", "THROWS"}})["error"]["data"]
                                                                     ["suggestions"][0]
          == "throw");
      }

      SECTION("a huge unknown tool name is truncated in the error")
      {
        const auto name = QString(1024 * 1024, u'e') + u"cho";
        const auto body = call(server, sessionId, "tools/call", {{"name", name}});
        CHECK(body["error"]["code"] == -32602);
        CHECK(
          body["error"]["message"]
          == QString{"unknown tool '%1...'"}.arg(QString(128, u'e')));
        CHECK(body["error"]["data"]["suggestions"].toArray().size() == 3);
      }

      SECTION("non-object arguments")
      {
        const auto arguments = GENERATE(
          QJsonValue{QJsonArray{}}, QJsonValue{"x"}, QJsonValue{QJsonValue::Null});
        CAPTURE(arguments);

        const auto body = call(
          server, sessionId, "tools/call", {{"name", "echo"}, {"arguments", arguments}});
        CHECK(body["error"]["code"] == -32602);
        CHECK(body["error"]["message"] == "tools/call 'arguments' must be an object");
        CHECK(receivedArguments == std::nullopt);
      }

      SECTION("a throwing handler gives -32603 and the server keeps working")
      {
        const auto body = call(server, sessionId, "tools/call", {{"name", "throw"}});
        CHECK(body == jsonObject(R"({"jsonrpc": "2.0", "id": 1, "error": {
            "code": -32603,
            "message": "internal error: boom"
          }})"));
        CHECK_FALSE(server.isDispatchingToolCall());

        const auto next = call(server, sessionId, "tools/call", {{"name", "echo"}});
        CHECK(next["result"]["isError"] == false);
      }
    }

    SECTION("reentrant tools/call gives editor_busy while ping still works")
    {
      auto reentrantTools = ToolRegistry{};
      auto* reentrantServer = static_cast<Server*>(nullptr);
      auto sessionId = QString{};
      auto dispatchingDuringCall = false;
      auto innerCall = QJsonObject{};
      auto innerPing = QJsonObject{};
      auto innerList = QJsonObject{};

      reentrantTools.add(ToolSpec{
        .name = "nested",
        .title = "Nested",
        .description = "Calls the server from within the handler.",
        .inputSchema = objectSchema(),
        .annotations = {},
        .handler = [&](const QJsonObject&) -> ToolResult {
          dispatchingDuringCall = reentrantServer->isDispatchingToolCall();
          innerCall =
            call(*reentrantServer, sessionId, "tools/call", {{"name", "nested"}}, 2);
          innerPing = call(*reentrantServer, sessionId, "ping", {}, 3);
          innerList = call(*reentrantServer, sessionId, "tools/list", {}, 4);
          return ToolOutput{.structured = {{"ok", true}}, .extraContent = {}};
        },
      });

      auto otherServer = Server{ServerInfo{.version = "1"}, reentrantTools};
      reentrantServer = &otherServer;
      sessionId = initialize(otherServer);

      CHECK_FALSE(otherServer.isDispatchingToolCall());
      const auto outer = call(otherServer, sessionId, "tools/call", {{"name", "nested"}});
      CHECK_FALSE(otherServer.isDispatchingToolCall());

      CHECK(dispatchingDuringCall);
      CHECK(outer["result"]["structuredContent"] == QJsonObject{{"ok", true}});
      CHECK(innerCall == jsonObject(R"({"jsonrpc": "2.0", "id": 2, "result": {
          "content": [{"type": "text", "text": "editor_busy: another tool call is still running Hint: retry in a moment"}],
          "structuredContent": {"error": {
            "code": "editor_busy",
            "message": "another tool call is still running",
            "hint": "retry in a moment",
            "details": {"reason": "reentrant_call"}
          }},
          "isError": true
        }})"));
      CHECK(innerPing == jsonObject(R"({"jsonrpc":"2.0","id":3,"result":{}})"));
      CHECK(std::as_const(innerList)["result"]["tools"][0]["name"] == "nested");
    }

    SECTION("a handler may end its own session and evict others")
    {
      auto reentrantTools = ToolRegistry{};
      auto* reentrantServer = static_cast<Server*>(nullptr);
      auto sessionId = QString{};
      auto deleteStatus = 0;

      reentrantTools.add(ToolSpec{
        .name = "teardown",
        .title = "Teardown",
        .description = "Ends the calling session from within the handler.",
        .inputSchema = objectSchema(),
        .annotations = {},
        .handler = [&](const QJsonObject&) -> ToolResult {
          deleteStatus =
            reentrantServer
              ->handle(
                makeRequest(
                  "DELETE",
                  "/mcp",
                  {{"host", "127.0.0.1:6767"}, {"mcp-session-id", sessionId.toUtf8()}}),
                Port)
              .status;
          for (size_t i = 0; i < Server::MaxSessions + 1; ++i)
          {
            initialize(*reentrantServer);
          }
          return ToolOutput{.structured = {{"ok", true}}, .extraContent = {}};
        },
      });

      auto otherServer = Server{ServerInfo{.version = "1"}, reentrantTools};
      reentrantServer = &otherServer;
      sessionId = initialize(otherServer);

      const auto outer =
        call(otherServer, sessionId, "tools/call", {{"name", "teardown"}});
      CHECK(deleteStatus == 204);
      CHECK(outer["result"]["structuredContent"] == QJsonObject{{"ok", true}});
      CHECK(otherServer.session(sessionId) == nullptr);
      CHECK(otherServer.sessionIds().size() == Server::MaxSessions);
      CHECK_FALSE(otherServer.isDispatchingToolCall());
    }

    SECTION("unknown method gives -32601")
    {
      const auto sessionId = initialize(server);
      CHECK(
        call(server, sessionId, "resources/list")
        == jsonObject(R"({"jsonrpc": "2.0", "id": 1, "error": {
          "code": -32601,
          "message": "method not found: resources/list"
        }})"));
    }

    SECTION("path")
    {
      const auto sessionId = initialize(server);

      const auto ok = server.handle(
        makeRequest(
          "POST", "/mcp?x=1", defaultHeaders(sessionId), requestBody(1, "ping")),
        Port);
      CHECK(ok.status == 200);

      const auto target = GENERATE(
        QByteArray{"/"}, QByteArray{"/mcp/"}, QByteArray{"/MCP"}, QByteArray{"/other"});
      CAPTURE(target);

      const auto response = server.handle(
        makeRequest("POST", target, defaultHeaders(sessionId), requestBody(1, "ping")),
        Port);
      CHECK(response.status == 404);
      CHECK(errorCodeOf(response) == -32600);
    }

    SECTION("Host")
    {
      using T = std::tuple<std::optional<QByteArray>, int>;

      // clang-format off
      const auto
      [host,                                 status] = GENERATE(values<T>({
      {QByteArray{"127.0.0.1:6767"},         200},
      {QByteArray{"127.0.0.1"},              200},
      {QByteArray{"localhost:6767"},         200},
      {QByteArray{"LocalHost:6767"},         200},
      {QByteArray{"localhost"},              200},
      {QByteArray{"[::1]:6767"},             200},
      {QByteArray{"[::1]"},                  200},
      {QByteArray{"127.0.0.1:1234"},         403},
      {QByteArray{"localhost:80"},           403},
      {QByteArray{"localhost:"},             403},
      {QByteArray{"localhost:67a7"},         403},
      {QByteArray{"localhost:6767:6767"},    403},
      {QByteArray{"evil.com:6767"},          403},
      {QByteArray{"evil.com"},               403},
      {QByteArray{"localhost.evil.com:6767"},403},
      {QByteArray{"127.0.0.2:6767"},         403},
      {QByteArray{"0.0.0.0:6767"},           403},
      {QByteArray{"::1"},                    403},
      {QByteArray{"[::1"},                   403},
      {QByteArray{"[::1]x"},                 403},
      {QByteArray{"[::2]:6767"},             403},
      {QByteArray{""},                       403},
      {std::nullopt,                         403},
      }));
      // clang-format on

      CAPTURE(host);

      auto request = post(requestBody(1, "initialize", {}));
      request = host ? withHeader(std::move(request), "host", *host)
                     : withoutHeader(std::move(request), "host");

      const auto response = server.handle(request, Port);
      CHECK(response.status == status);
      if (status == 403)
      {
        CHECK(errorCodeOf(response) == -32600);
        CHECK(server.sessionIds().empty());
      }
    }

    SECTION("Origin")
    {
      using T = std::tuple<QByteArray, int>;

      // clang-format off
      const auto
      [origin,                             status] = GENERATE(values<T>({
      {"http://localhost:3000",            200},
      {"http://localhost",                 200},
      {"http://127.0.0.1",                 200},
      {"https://127.0.0.1:8443",           200},
      {"http://[::1]:6767",                200},
      {"HTTP://LOCALHOST:3000",            200},
      {"http://evil.com",                  403},
      {"https://evil.com:6767",            403},
      {"http://localhost.evil.com",        403},
      {"http://127.0.0.1.evil.com",        403},
      {"null",                             403},
      {"file://localhost",                 403},
      {"ws://localhost:6767",              403},
      {"localhost",                        403},
      {"",                                 403},
      {"http://",                          403},
      }));
      // clang-format on

      CAPTURE(origin);

      const auto response = server.handle(
        withHeader(post(requestBody(1, "initialize", {})), "origin", origin), Port);
      CHECK(response.status == status);
      CHECK_FALSE(response.header("access-control-allow-origin"));
    }

    SECTION("methods other than POST and DELETE give 405")
    {
      const auto method = GENERATE(
        QByteArray{"GET"},
        QByteArray{"PUT"},
        QByteArray{"PATCH"},
        QByteArray{"OPTIONS"},
        QByteArray{"HEAD"});
      CAPTURE(method);

      const auto response =
        server.handle(makeRequest(method, "/mcp", defaultHeaders()), Port);
      CHECK(response.status == 405);
      CHECK(response.header("allow") == QByteArray{"POST, DELETE"});
    }

    SECTION("DELETE")
    {
      const auto first = initialize(server);
      const auto second = initialize(server);

      SECTION("known session gives 204")
      {
        const auto response =
          server.handle(makeRequest("DELETE", "/mcp", defaultHeaders(first)), Port);
        CHECK(response.status == 204);
        CHECK(closedSessions == std::vector<QString>{first});
        CHECK(server.sessionIds() == std::vector<QString>{second});

        // the session is gone
        CHECK(server.handle(post(requestBody(1, "ping"), first), Port).status == 404);
        CHECK(
          server.handle(makeRequest("DELETE", "/mcp", defaultHeaders(first)), Port).status
          == 404);
        CHECK(server.handle(post(requestBody(1, "ping"), second), Port).status == 200);
      }

      SECTION("unknown or missing session gives 404")
      {
        CHECK(
          server.handle(makeRequest("DELETE", "/mcp", defaultHeaders("bogus")), Port)
            .status
          == 404);
        CHECK(
          server.handle(makeRequest("DELETE", "/mcp", defaultHeaders()), Port).status
          == 404);
        CHECK(closedSessions.empty());
        CHECK(server.sessionIds().size() == 2);
      }
    }

    SECTION("Content-Type must be application/json")
    {
      using T = std::tuple<std::optional<QByteArray>, int>;

      // clang-format off
      const auto
      [contentType,                                    status] = GENERATE(values<T>({
      {QByteArray{"application/json"},                 200},
      {QByteArray{"application/json; charset=utf-8"},  200},
      {QByteArray{"Application/JSON"},                 200},
      {QByteArray{"text/plain"},                       415},
      {QByteArray{"application/jsonx"},                415},
      {QByteArray{"text/event-stream"},                415},
      {QByteArray{""},                                 415},
      {std::nullopt,                                   415},
      }));
      // clang-format on

      CAPTURE(contentType);

      auto request = post(requestBody(1, "initialize", {}));
      request = contentType ? withHeader(std::move(request), "content-type", *contentType)
                            : withoutHeader(std::move(request), "content-type");
      CHECK(server.handle(request, Port).status == status);
    }

    SECTION("Accept must allow JSON")
    {
      using T = std::tuple<std::optional<QByteArray>, int>;

      // clang-format off
      const auto
      [accept,                                         status] = GENERATE(values<T>({
      {QByteArray{"application/json"},                 200},
      {QByteArray{"application/json, text/event-stream"}, 200},
      {QByteArray{"text/event-stream, application/json;q=0.9"}, 200},
      {QByteArray{"application/*"},                    200},
      {QByteArray{"*/*"},                              200},
      {std::nullopt,                                   200},
      {QByteArray{"text/event-stream"},                406},
      {QByteArray{"text/html, text/*"},                406},
      {QByteArray{""},                                 406},
      }));
      // clang-format on

      CAPTURE(accept);

      auto request = post(requestBody(1, "initialize", {}));
      request = accept ? withHeader(std::move(request), "accept", *accept)
                       : withoutHeader(std::move(request), "accept");
      const auto response = server.handle(request, Port);
      CHECK(response.status == status);
      if (status == 406)
      {
        CHECK(errorCodeOf(response) == -32600);
      }
    }

    SECTION("400")
    {
      const auto sessionId = initialize(server);

      SECTION("malformed JSON gives -32700")
      {
        const auto body = GENERATE(
          QByteArray{""}, QByteArray{"{"}, QByteArray{"not json"}, QByteArray{"{}}"});
        CAPTURE(body);

        const auto response = server.handle(post(body, sessionId), Port);
        CHECK(response.status == 400);
        const auto responseBody = bodyOf(response);
        CHECK(responseBody["error"]["code"] == -32700);
        CHECK(responseBody["id"] == QJsonValue::Null);
      }

      SECTION("a batch array gives -32600")
      {
        const auto response =
          server.handle(post("[" + requestBody(1, "ping") + "]", sessionId), Port);
        CHECK(response.status == 400);
        CHECK(bodyOf(response) == jsonObject(R"({"jsonrpc": "2.0", "id": null, "error": {
            "code": -32600,
            "message": "batching is not supported"
          }})"));
      }

      SECTION("an invalid envelope gives -32600")
      {
        const auto response = server.handle(
          post(R"({"jsonrpc":"1.0","id":9,"method":"ping"})", sessionId), Port);
        CHECK(response.status == 400);
        CHECK(bodyOf(response) == jsonObject(R"({"jsonrpc": "2.0", "id": 9, "error": {
            "code": -32600,
            "message": "jsonrpc must be \"2.0\""
          }})"));
      }

      SECTION("missing Mcp-Session-Id")
      {
        const auto response = server.handle(post(requestBody(4, "ping")), Port);
        CHECK(response.status == 400);
        CHECK(bodyOf(response) == jsonObject(R"({"jsonrpc": "2.0", "id": 4, "error": {
            "code": -32600,
            "message": "missing Mcp-Session-Id header; send initialize first"
          }})"));

        CHECK(
          server.handle(post(notificationBody("notifications/initialized")), Port).status
          == 400);
      }

      SECTION("unsupported MCP-Protocol-Version")
      {
        const auto response = server.handle(
          withHeader(
            post(requestBody(1, "ping"), sessionId),
            "mcp-protocol-version",
            "1999-01-01"),
          Port);
        CHECK(response.status == 400);
        CHECK(
          bodyOf(response)["error"]["message"]
          == "unsupported MCP-Protocol-Version '1999-01-01'; supported: 2025-06-18, "
             "2025-03-26");

        const auto initializeResponse = server.handle(
          withHeader(post(requestBody(1, "initialize", {})), "mcp-protocol-version", "x"),
          Port);
        CHECK(initializeResponse.status == 400);
      }
    }

    SECTION("MCP-Protocol-Version")
    {
      const auto sessionId = initialize(server);
      const auto version = GENERATE(
        std::optional<QByteArray>{"2025-06-18"},
        std::optional<QByteArray>{"2025-03-26"},
        std::optional<QByteArray>{});
      CAPTURE(version);

      auto request = post(requestBody(1, "ping"), sessionId);
      if (version)
      {
        request = withHeader(std::move(request), "mcp-protocol-version", *version);
      }
      CHECK(server.handle(request, Port).status == 200);
    }

    SECTION("unknown session gives 404")
    {
      initialize(server);
      const auto response = server.handle(post(requestBody(5, "ping"), "bogus"), Port);
      CHECK(response.status == 404);
      CHECK(bodyOf(response) == jsonObject(R"({"jsonrpc": "2.0", "id": 5, "error": {
          "code": -32600,
          "message": "unknown or expired session; send initialize to start a new session"
        }})"));
    }

    SECTION("least recently used sessions are evicted")
    {
      for (size_t i = 0; i < Server::MaxSessions; ++i)
      {
        initialize(server);
      }
      CHECK(server.sessionIds().size() == Server::MaxSessions);
      CHECK(closedSessions.empty());

      SECTION("the oldest session is evicted")
      {
        initialize(server);
        CHECK(server.sessionIds().size() == Server::MaxSessions);
        CHECK(closedSessions == std::vector<QString>{"s1"});
        CHECK(server.handle(post(requestBody(1, "ping"), "s1"), Port).status == 404);
        CHECK(server.handle(post(requestBody(1, "ping"), "s2"), Port).status == 200);
        CHECK(server.handle(post(requestBody(1, "ping"), "s17"), Port).status == 200);
      }

      SECTION("using a session makes it recent")
      {
        CHECK(server.handle(post(requestBody(1, "ping"), "s1"), Port).status == 200);
        initialize(server);
        CHECK(closedSessions == std::vector<QString>{"s2"});
        CHECK(server.handle(post(requestBody(1, "ping"), "s1"), Port).status == 200);
        CHECK(server.handle(post(requestBody(1, "ping"), "s2"), Port).status == 404);
      }
    }
  }

  SECTION("isDispatchingToolCall")
  {
    CHECK_FALSE(server.isDispatchingToolCall());
  }

  SECTION("sessionIds")
  {
    CHECK(server.sessionIds().empty());
    initialize(server);
    initialize(server);
    CHECK(server.sessionIds() == std::vector<QString>{"s1", "s2"});
  }

  SECTION("session")
  {
    CHECK(server.session("s1") == nullptr);
    initialize(server, "2025-03-26");
    const auto* session = server.session("s1");
    REQUIRE(session != nullptr);
    CHECK(session->protocolVersion == "2025-03-26");
  }
}

TEST_CASE("editDistance")
{
  CHECK(editDistance("", "") == 0);
  CHECK(editDistance("abc", "") == 3);
  CHECK(editDistance("get_nodes", "get_node") == 1);
  CHECK(editDistance("Get_Nodes", "get_nodes") == 0);
  CHECK(editDistance("kitten", "sitting") == 3);
}

TEST_CASE("supportedProtocolVersions")
{
  CHECK(supportedProtocolVersions() == QStringList{"2025-06-18", "2025-03-26"});
  CHECK(supportedProtocolVersions().front() == LatestProtocolVersion);
}

} // namespace tb::mcp
