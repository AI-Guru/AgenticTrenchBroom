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
#include "mcp/Http.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mcp
{
namespace
{

using State = HttpRequestParser::State;

HttpRequest parseRequest(const QByteArray& data)
{
  auto parser = HttpRequestParser{};
  REQUIRE(parser.feed(data) == State::Complete);
  return parser.takeRequest();
}

int parseError(const QByteArray& data, const HttpRequestParser::Limits limits = {})
{
  auto parser = HttpRequestParser{limits};
  REQUIRE(parser.feed(data) == State::Error);
  return parser.errorStatus();
}

const auto SimplePost = QByteArray{
  "POST /mcp HTTP/1.1\r\n"
  "Host: 127.0.0.1:6767\r\n"
  "Content-Type: application/json\r\n"
  "Content-Length: 13\r\n"
  "\r\n"
  R"({"jsonrpc":1})"};

} // namespace

TEST_CASE("HttpRequest")
{
  SECTION("header")
  {
    const auto request = HttpRequest{
      .method = "GET",
      .target = "/",
      .version = "HTTP/1.1",
      .headers = {{"host", "localhost"}, {"x-a", "1"}, {"x-a", "2"}},
      .body = {},
    };

    CHECK(request.header("host") == QByteArray{"localhost"});
    CHECK(request.header("x-a") == QByteArray{"1"});
    CHECK(request.header("missing") == std::nullopt);
  }

  SECTION("keepAlive")
  {
    using T = std::tuple<QByteArray, std::vector<QByteArray>, bool>;

    // clang-format off
    const auto
    [version,    connectionHeaders,            expected] = GENERATE(values<T>({
    {"HTTP/1.1", {},                           true},
    {"HTTP/1.1", {"close"},                    false},
    {"HTTP/1.1", {"Close"},                    false},
    {"HTTP/1.1", {"keep-alive"},               true},
    {"HTTP/1.1", {"upgrade, close"},           false},
    {"HTTP/1.1", {"upgrade", "close"},         false},
    {"HTTP/1.0", {},                           false},
    {"HTTP/1.0", {"keep-alive"},               true},
    {"HTTP/1.0", {"Keep-Alive"},               true},
    {"HTTP/1.0", {"foo, keep-alive"},          true},
    {"HTTP/1.0", {"close"},                    false},
    }));
    // clang-format on

    CAPTURE(version, connectionHeaders);

    auto request = HttpRequest{.method = "GET", .target = "/", .version = version};
    for (const auto& value : connectionHeaders)
    {
      request.headers.emplace_back("connection", value);
    }
    CHECK(request.keepAlive() == expected);
  }
}

TEST_CASE("HttpRequestParser")
{
  SECTION("feed")
  {
    SECTION("whole request")
    {
      auto parser = HttpRequestParser{};
      CHECK(parser.state() == State::NeedMore);
      CHECK(parser.feed(SimplePost) == State::Complete);
      CHECK(parser.state() == State::Complete);
      CHECK(parser.errorStatus() == 0);

      const auto request = parser.takeRequest();
      CHECK(request.method == "POST");
      CHECK(request.target == "/mcp");
      CHECK(request.version == "HTTP/1.1");
      CHECK(
        request.headers
        == std::vector<std::pair<QByteArray, QByteArray>>{
          {"host", "127.0.0.1:6767"},
          {"content-type", "application/json"},
          {"content-length", "13"},
        });
      CHECK(request.body == R"({"jsonrpc":1})");
      CHECK(parser.state() == State::NeedMore);
    }

    SECTION("fed one byte at a time")
    {
      auto parser = HttpRequestParser{};
      for (auto i = qsizetype{0}; i < SimplePost.size() - 1; ++i)
      {
        REQUIRE(parser.feed(SimplePost.sliced(i, 1)) == State::NeedMore);
      }
      CHECK(parser.feed(SimplePost.sliced(SimplePost.size() - 1)) == State::Complete);

      const auto request = parser.takeRequest();
      CHECK(request.method == "POST");
      CHECK(request.body == R"({"jsonrpc":1})");
    }

    SECTION("two pipelined requests")
    {
      auto parser = HttpRequestParser{};
      const auto data = SimplePost + "DELETE /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n"
                        + "POST /mcp HTTP/1.1\r\nContent-Length: 2\r\n\r\n{";
      CHECK(parser.feed(data) == State::Complete);

      auto first = parser.takeRequest();
      CHECK(first.method == "POST");
      CHECK(first.body == R"({"jsonrpc":1})");

      CHECK(parser.state() == State::Complete);
      auto second = parser.takeRequest();
      CHECK(second.method == "DELETE");
      CHECK(second.body.isEmpty());

      CHECK(parser.state() == State::NeedMore);
      CHECK(parser.feed("}") == State::Complete);
      auto third = parser.takeRequest();
      CHECK(third.body == "{}");
      CHECK(parser.state() == State::NeedMore);
    }

    SECTION("header names are case-insensitive")
    {
      const auto request = parseRequest(
        "POST /mcp HTTP/1.1\r\nHOST: localhost\r\nCoNtEnT-LeNgTh: 2\r\n\r\n{}");
      CHECK(request.header("host") == QByteArray{"localhost"});
      CHECK(request.header("content-length") == QByteArray{"2"});
      CHECK(request.body == "{}");
    }

    SECTION("header values are trimmed")
    {
      const auto request =
        parseRequest("GET / HTTP/1.1\r\nHost: \t localhost \t\r\nX-Empty:\r\n\r\n");
      CHECK(request.header("host") == QByteArray{"localhost"});
      CHECK(request.header("x-empty") == QByteArray{""});
    }

    SECTION("bare LF line endings are tolerated")
    {
      const auto request =
        parseRequest("POST /mcp HTTP/1.1\nHost: localhost\nContent-Length: 2\n\n{}");
      CHECK(request.header("host") == QByteArray{"localhost"});
      CHECK(request.body == "{}");
    }

    SECTION("leading empty lines are ignored")
    {
      const auto request = parseRequest("\r\n\nGET /mcp HTTP/1.1\r\n\r\n");
      CHECK(request.method == "GET");
      CHECK(request.target == "/mcp");
    }

    SECTION("GET and DELETE need no Content-Length")
    {
      CHECK(parseRequest("GET /mcp HTTP/1.1\r\n\r\n").body.isEmpty());
      CHECK(parseRequest("DELETE /mcp HTTP/1.1\r\n\r\n").body.isEmpty());
    }

    SECTION("duplicate Content-Length headers with equal values are accepted")
    {
      const auto request = parseRequest(
        "POST /mcp HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 02\r\n\r\n{}");
      CHECK(request.body == "{}");
    }

    SECTION("an empty body is complete immediately")
    {
      CHECK(
        parseRequest("POST /mcp HTTP/1.1\r\nContent-Length: 0\r\n\r\n").body.isEmpty());
    }

    SECTION("HTTP/1.0 is accepted")
    {
      CHECK(parseRequest("GET /mcp HTTP/1.0\r\n\r\n").version == "HTTP/1.0");
    }

    SECTION("body limit is inclusive")
    {
      auto parser = HttpRequestParser{{.maxHeaderBytes = 1024, .maxBodyBytes = 4}};
      CHECK(
        parser.feed("POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nabcd")
        == State::Complete);
    }

    SECTION("errors")
    {
      SECTION("400 bad request line")
      {
        const auto data = GENERATE(
          QByteArray{"GET\r\n\r\n"},
          QByteArray{"GET /mcp\r\n\r\n"},
          QByteArray{"GET  /mcp HTTP/1.1\r\n\r\n"},
          QByteArray{"GET /mcp HTTP/1.1 extra\r\n\r\n"},
          QByteArray{"G(T /mcp HTTP/1.1\r\n\r\n"},
          QByteArray{"GET /m\x01p HTTP/1.1\r\n\r\n"},
          QByteArray{"GET /mcp FOO/1.1\r\n\r\n"},
          QByteArray{"GET /mcp HTTP/11\r\n\r\n"},
          QByteArray{"GET /mcp http/1.1\r\n\r\n"});
        CAPTURE(data);
        CHECK(parseError(data) == 400);
      }

      SECTION("400 bad header")
      {
        const auto data = GENERATE(
          QByteArray{"GET / HTTP/1.1\r\nNoColon\r\n\r\n"},
          QByteArray{"GET / HTTP/1.1\r\n: value\r\n\r\n"},
          QByteArray{"GET / HTTP/1.1\r\nBad Name: value\r\n\r\n"},
          QByteArray{"GET / HTTP/1.1\r\nHost : localhost\r\n\r\n"},
          QByteArray{"GET / HTTP/1.1\r\nA: b\r\n folded\r\n\r\n"},
          QByteArray{"GET / HTTP/1.1\r\nA: b\x01\r\n\r\n"});
        CAPTURE(data);
        CHECK(parseError(data) == 400);
      }

      SECTION("400 bad Content-Length")
      {
        const auto value = GENERATE(
          QByteArray{"abc"},
          QByteArray{"-1"},
          QByteArray{"+1"},
          QByteArray{"1.0"},
          QByteArray{"0x10"},
          QByteArray{"1 2"},
          QByteArray{""},
          QByteArray{"99999999999999999999"});
        CAPTURE(value);
        CHECK(
          parseError("POST / HTTP/1.1\r\nContent-Length: " + value + "\r\n\r\n") == 400);
      }

      SECTION("400 conflicting Content-Length")
      {
        CHECK(
          parseError("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n")
          == 400);
      }

      SECTION("411 POST without Content-Length")
      {
        CHECK(parseError("POST /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n") == 411);
      }

      SECTION("413 body too large")
      {
        CHECK(
          parseError(
            "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\n",
            {.maxHeaderBytes = 1024, .maxBodyBytes = 4})
          == 413);
        CHECK(parseError("POST / HTTP/1.1\r\nContent-Length: 16777217\r\n\r\n") == 413);
      }

      SECTION("431 header section too large")
      {
        const auto limits = HttpRequestParser::Limits{.maxHeaderBytes = 64};

        // incomplete header section that exceeds the limit
        CHECK(
          parseError("GET / HTTP/1.1\r\nX-Long: " + QByteArray(64, 'a'), limits) == 431);

        // complete header section that exceeds the limit
        CHECK(
          parseError(
            "GET / HTTP/1.1\r\nX-Long: " + QByteArray(64, 'a') + "\r\n\r\n", limits)
          == 431);

        // the default limit
        CHECK(
          parseError("GET / HTTP/1.1\r\nX-Long: " + QByteArray(33 * 1024, 'a')) == 431);
      }

      SECTION("501 Transfer-Encoding")
      {
        CHECK(parseError("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == 501);
        CHECK(
          parseError(
            "POST / HTTP/1.1\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n")
          == 501);
      }

      SECTION("505 not HTTP/1.x")
      {
        CHECK(parseError("GET / HTTP/2.0\r\n\r\n") == 505);
        CHECK(parseError("GET / HTTP/0.9\r\n\r\n") == 505);
      }

      SECTION("errors are sticky")
      {
        auto parser = HttpRequestParser{};
        CHECK(parser.feed("GET / HTTP/2.0\r\n\r\n") == State::Error);
        CHECK(parser.feed("GET / HTTP/1.1\r\n\r\n") == State::Error);
        CHECK(parser.errorStatus() == 505);
      }

      SECTION("an error after a pipelined request")
      {
        auto parser = HttpRequestParser{};
        CHECK(
          parser.feed("GET / HTTP/1.1\r\n\r\nGET / HTTP/3.0\r\n\r\n") == State::Complete);
        CHECK(parser.takeRequest().version == "HTTP/1.1");
        CHECK(parser.state() == State::Error);
        CHECK(parser.errorStatus() == 505);
      }
    }
  }

  SECTION("chunking does not change the result of mutated inputs")
  {
    // A deterministic linear congruential generator, so failures are reproducible.
    auto seed = quint32{12345};
    const auto next = [&](const quint32 bound) {
      seed = seed * 1664525u + 1013904223u;
      return (seed >> 8) % bound;
    };

    const auto corpus = std::vector<QByteArray>{
      SimplePost,
      SimplePost + SimplePost,
      "GET /mcp HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
      "\r\n\nDELETE /mcp HTTP/1.1\nHost: localhost\n\nGET / HTTP/1.1\r\n\r\n",
    };
    const auto interesting = QByteArray{"\r\n: \t0-9aZ\x01\x7f\xff"};

    const auto drain = [](HttpRequestParser& parser, std::vector<HttpRequest>& out) {
      while (parser.state() == State::Complete)
      {
        out.push_back(parser.takeRequest());
      }
    };

    for (auto iteration = 0; iteration < 2000; ++iteration)
    {
      auto data = corpus[next(quint32(corpus.size()))];
      const auto mutations = next(4);
      for (quint32 m = 0; m < mutations; ++m)
      {
        const auto pos = qsizetype(next(quint32(data.size() + 1)));
        const auto c = interesting[qsizetype(next(quint32(interesting.size())))];
        switch (next(3))
        {
        case 0:
          data.insert(pos, c);
          break;
        case 1:
          if (pos < data.size())
          {
            data[pos] = c;
          }
          break;
        default:
          if (pos < data.size())
          {
            data.remove(pos, 1);
          }
          break;
        }
      }
      CAPTURE(data);

      auto whole = HttpRequestParser{};
      auto wholeRequests = std::vector<HttpRequest>{};
      whole.feed(data);
      drain(whole, wholeRequests);

      auto chunked = HttpRequestParser{};
      auto chunkedRequests = std::vector<HttpRequest>{};
      for (auto pos = qsizetype{0}; pos < data.size();)
      {
        const auto length = std::min(qsizetype(next(5)) + 1, data.size() - pos);
        chunked.feed(QByteArrayView{data}.sliced(pos, length));
        drain(chunked, chunkedRequests);
        pos += length;
      }

      CHECK(whole.state() == chunked.state());
      CHECK(whole.errorStatus() == chunked.errorStatus());
      REQUIRE(wholeRequests.size() == chunkedRequests.size());
      for (size_t i = 0; i < wholeRequests.size(); ++i)
      {
        CHECK(wholeRequests[i].method == chunkedRequests[i].method);
        CHECK(wholeRequests[i].target == chunkedRequests[i].target);
        CHECK(wholeRequests[i].version == chunkedRequests[i].version);
        CHECK(wholeRequests[i].headers == chunkedRequests[i].headers);
        CHECK(wholeRequests[i].body == chunkedRequests[i].body);
      }
    }
  }

  SECTION("state")
  {
    auto parser = HttpRequestParser{};
    CHECK(parser.state() == State::NeedMore);
    parser.feed("POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\n{");
    CHECK(parser.state() == State::NeedMore);
    parser.feed("}");
    CHECK(parser.state() == State::Complete);
  }

  SECTION("takeRequest")
  {
    auto parser = HttpRequestParser{};
    parser.feed("POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}GET / HTTP/1.1\r\n");
    CHECK(parser.takeRequest().body == "{}");
    CHECK(parser.state() == State::NeedMore);
    parser.feed("\r\n");
    CHECK(parser.takeRequest().method == "GET");
  }

  SECTION("errorStatus")
  {
    auto parser = HttpRequestParser{};
    CHECK(parser.errorStatus() == 0);
    parser.feed("POST / HTTP/1.1\r\n\r\n");
    CHECK(parser.errorStatus() == 411);
  }
}

TEST_CASE("HttpResponse")
{
  SECTION("serialize")
  {
    SECTION("with body, keep alive")
    {
      const auto response = HttpResponse{
        .status = 200,
        .headers = {{"Content-Type", "application/json"}, {"Mcp-Session-Id", "s1"}},
        .body = "{}",
      };
      CHECK(
        response.serialize(true)
        == "HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Mcp-Session-Id: s1\r\n"
           "Content-Length: 2\r\n"
           "Connection: keep-alive\r\n"
           "Cache-Control: no-store\r\n"
           "\r\n"
           "{}");
    }

    SECTION("without body, close")
    {
      CHECK(
        HttpResponse::empty(202).serialize(false)
        == "HTTP/1.1 202 Accepted\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n"
           "Cache-Control: no-store\r\n"
           "\r\n");
    }

    SECTION("204 has no Content-Length and no body")
    {
      auto response = HttpResponse::empty(204);
      response.body = "ignored";
      CHECK(
        response.serialize(true)
        == "HTTP/1.1 204 No Content\r\n"
           "Connection: keep-alive\r\n"
           "Cache-Control: no-store\r\n"
           "\r\n");
    }

    SECTION("a response to a HEAD request has Content-Length but no body")
    {
      const auto response = HttpResponse{
        .status = 405,
        .headers = {{"Allow", "POST, DELETE"}},
        .body = "abc",
      };
      CHECK(
        response.serialize(true, true)
        == "HTTP/1.1 405 Method Not Allowed\r\n"
           "Allow: POST, DELETE\r\n"
           "Content-Length: 3\r\n"
           "Connection: keep-alive\r\n"
           "Cache-Control: no-store\r\n"
           "\r\n");
    }

    SECTION("custom Content-Length and Connection headers are replaced")
    {
      const auto response = HttpResponse{
        .status = 405,
        .headers =
          {{"content-length", "99"}, {"Allow", "POST, DELETE"}, {"Connection", "x"}},
        .body = "abc",
      };
      CHECK(
        response.serialize(false)
        == "HTTP/1.1 405 Method Not Allowed\r\n"
           "Allow: POST, DELETE\r\n"
           "Content-Length: 3\r\n"
           "Connection: close\r\n"
           "Cache-Control: no-store\r\n"
           "\r\n"
           "abc");
    }
  }

  SECTION("header")
  {
    const auto response = HttpResponse{.status = 200, .headers = {{"Allow", "POST"}}};
    CHECK(response.header("allow") == QByteArray{"POST"});
    CHECK(response.header("ALLOW") == QByteArray{"POST"});
    CHECK(response.header("other") == std::nullopt);
  }

  SECTION("json")
  {
    const auto response = HttpResponse::json(400, QJsonObject{{"b", 1}, {"a", "x"}});
    CHECK(response.status == 400);
    CHECK(
      response.headers
      == std::vector<std::pair<QByteArray, QByteArray>>{
        {"Content-Type", "application/json"}});
    CHECK(response.body == R"({"a":"x","b":1})");
  }

  SECTION("empty")
  {
    const auto response = HttpResponse::empty(404);
    CHECK(response.status == 404);
    CHECK(response.headers.empty());
    CHECK(response.body.isEmpty());
  }
}

TEST_CASE("Http")
{
  SECTION("reasonPhrase")
  {
    using T = std::tuple<int, QByteArray>;

    // clang-format off
    const auto
    [status, expected] = GENERATE(values<T>({
    {200, "OK"},
    {202, "Accepted"},
    {204, "No Content"},
    {400, "Bad Request"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {406, "Not Acceptable"},
    {411, "Length Required"},
    {413, "Content Too Large"},
    {415, "Unsupported Media Type"},
    {431, "Request Header Fields Too Large"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {505, "HTTP Version Not Supported"},
    {299, "Unknown"},
    }));
    // clang-format on

    CAPTURE(status);
    CHECK(reasonPhrase(status) == expected);
  }
}

} // namespace tb::mcp
