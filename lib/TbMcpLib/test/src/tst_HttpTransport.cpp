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

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include "TestUtils.h"
#include "mcp/HttpTransport.h"
#include "mcp/Server.h"
#include "mcp/Tool.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mcp
{
namespace
{

using namespace std::chrono_literals;

constexpr auto Timeout = 5s;
constexpr auto TimeoutMs = int(Timeout / 1ms);

/**
 * Processes events on the main thread until the given future is ready, so that the
 * transport can serve a client running on another thread.
 */
template <typename T>
T waitFor(std::future<T>& future)
{
  const auto deadline = QDeadlineTimer{Timeout};
  while (future.wait_for(0ms) != std::future_status::ready && !deadline.hasExpired())
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  return future.get();
}

/**
 * Processes events until the given predicate holds or the timeout expires.
 */
bool processEventsUntil(const std::function<bool()>& predicate)
{
  const auto deadline = QDeadlineTimer{Timeout};
  while (!predicate() && !deadline.hasExpired())
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  return predicate();
}

/**
 * Lets a client thread pause until the main thread has checked the transport's state.
 * Each client socket must stay on the thread that created it, so a client that is used
 * across several steps runs on one thread and synchronizes with checkpoints.
 */
class Checkpoint
{
public:
  /** Called by the client thread. Returns false if the main thread doesn't continue. */
  bool reach()
  {
    m_reached.set_value();
    return m_continued.wait_for(Timeout) == std::future_status::ready;
  }

  /** Called by the main thread. Processes events until the checkpoint is reached. */
  bool waitUntilReached()
  {
    const auto deadline = QDeadlineTimer{Timeout};
    while (m_reachedFuture.wait_for(0ms) != std::future_status::ready
           && !deadline.hasExpired())
    {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return m_reachedFuture.wait_for(0ms) == std::future_status::ready;
  }

  /** Called by the main thread to let the client thread continue. */
  void proceed() { m_continue.set_value(); }

private:
  std::promise<void> m_reached;
  std::future<void> m_reachedFuture = m_reached.get_future();
  std::promise<void> m_continue;
  std::future<void> m_continued = m_continue.get_future();
};

template <typename F>
auto runClient(F f)
{
  return std::async(std::launch::async, std::move(f));
}

struct ClientResponse
{
  int status = 0;
  std::vector<std::pair<QByteArray, QByteArray>> headers;
  QByteArray body;

  std::optional<QByteArray> header(const QByteArrayView lowerName) const
  {
    for (const auto& [name, value] : headers)
    {
      if (name == lowerName)
      {
        return value;
      }
    }
    return std::nullopt;
  }
};

/**
 * A blocking HTTP client for use on a thread without an event loop.
 */
class Client
{
public:
  bool connect(const quint16 port)
  {
    m_socket.connectToHost(QHostAddress::LocalHost, port);
    return m_socket.waitForConnected(TimeoutMs);
  }

  bool write(const QByteArray& data)
  {
    if (m_socket.write(data) != data.size())
    {
      return false;
    }
    while (m_socket.bytesToWrite() > 0)
    {
      if (!m_socket.waitForBytesWritten(TimeoutMs))
      {
        return false;
      }
    }
    return true;
  }

  /**
   * Reads one response. Returns nullopt if the server closes the connection before a
   * complete response has been received.
   */
  std::optional<ClientResponse> read()
  {
    auto headEnd = qsizetype(-1);
    while ((headEnd = m_buffer.indexOf("\r\n\r\n")) < 0)
    {
      if (!receive())
      {
        return std::nullopt;
      }
    }

    auto response = ClientResponse{};
    const auto lines = m_buffer.left(headEnd).split('\n');
    const auto statusLine = lines.front().trimmed().split(' ');
    if (statusLine.size() < 2 || statusLine[0] != "HTTP/1.1")
    {
      return std::nullopt;
    }
    response.status = statusLine[1].toInt();

    auto contentLength = qsizetype(0);
    for (auto i = qsizetype(1); i < lines.size(); ++i)
    {
      const auto colon = lines[i].indexOf(':');
      const auto name = lines[i].left(colon).trimmed().toLower();
      const auto value = lines[i].mid(colon + 1).trimmed();
      response.headers.emplace_back(name, value);
      if (name == "content-length")
      {
        contentLength = value.toLongLong();
      }
    }

    const auto bodyStart = headEnd + 4;
    while (m_buffer.size() < bodyStart + contentLength)
    {
      if (!receive())
      {
        return std::nullopt;
      }
    }
    response.body = m_buffer.mid(bodyStart, contentLength);
    m_buffer.remove(0, bodyStart + contentLength);
    return response;
  }

  /**
   * Reads the status line and headers of one response but no body, as for a response to
   * a HEAD request. Returns nullopt if the connection is closed first.
   */
  std::optional<ClientResponse> readHead()
  {
    auto headEnd = qsizetype(-1);
    while ((headEnd = m_buffer.indexOf("\r\n\r\n")) < 0)
    {
      if (!receive())
      {
        return std::nullopt;
      }
    }

    auto response = ClientResponse{};
    const auto lines = m_buffer.left(headEnd).split('\n');
    response.status = lines.front().trimmed().split(' ').value(1).toInt();
    for (auto i = qsizetype(1); i < lines.size(); ++i)
    {
      const auto colon = lines[i].indexOf(':');
      response.headers.emplace_back(
        lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
    }
    m_buffer.remove(0, headEnd + 4);
    return response;
  }

  /**
   * Waits until the server closes the connection. Returns true if it did so without
   * sending any further data.
   */
  bool waitForClose()
  {
    while (receive())
    {
    }
    return m_buffer.isEmpty() && m_socket.state() == QAbstractSocket::UnconnectedState;
  }

#ifndef _WIN32
  /** Closes the sending side of the connection only. */
  bool shutdownWrite()
  {
    return ::shutdown(int(m_socket.socketDescriptor()), SHUT_WR) == 0;
  }
#endif

  void close()
  {
    m_socket.abort();
    m_buffer.clear();
  }

private:
  bool receive()
  {
    if (m_socket.bytesAvailable() == 0 && !m_socket.waitForReadyRead(TimeoutMs))
    {
      return false;
    }
    const auto data = m_socket.readAll();
    m_buffer += data;
    return !data.isEmpty();
  }

  QTcpSocket m_socket;
  QByteArray m_buffer;
};

QByteArray post(const QByteArray& body, const QByteArray& extraHeaders = {})
{
  return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
         "Content-Length: "
         + QByteArray::number(body.size()) + "\r\n" + extraHeaders + "\r\n" + body;
}

/**
 * Records the requests handled by a transport and answers each one with a 200 response
 * whose body is the request body.
 */
struct Recorder
{
  std::vector<HttpRequest> requests;
  std::vector<quint16> localPorts;

  HttpTransport::Handler handler()
  {
    return [&](const HttpRequest& request, const quint16 localPort) {
      requests.push_back(request);
      localPorts.push_back(localPort);
      auto response = HttpResponse{};
      response.headers.emplace_back("Content-Type", "application/json");
      response.body = request.body;
      return response;
    };
  }
};

quint16 listen(HttpTransport& transport)
{
  auto result = transport.listen(0);
  REQUIRE(result.is_success());
  return result.value();
}

} // namespace

TEST_CASE("HttpTransport")
{
  auto argc = 0;
  auto app = QCoreApplication{argc, nullptr};

  auto recorder = Recorder{};

  SECTION("listen")
  {
    auto transport = HttpTransport{recorder.handler()};

    SECTION("listens on an ephemeral port of the loopback interface")
    {
      const auto port = listen(transport);
      CHECK(port != 0);
      CHECK(transport.isListening());
      CHECK(transport.port() == port);
      CHECK(transport.address() == QHostAddress{QHostAddress::LocalHost}.toString());
      CHECK(transport.address() == "127.0.0.1");
    }

    SECTION("listens on the given port")
    {
      // find a free port
      auto probe = QTcpServer{};
      REQUIRE(probe.listen(QHostAddress::LocalHost, 0));
      const auto freePort = probe.serverPort();
      probe.close();

      auto result = transport.listen(freePort);
      REQUIRE(result.is_success());
      CHECK(result.value() == freePort);
      CHECK(transport.port() == freePort);
    }

    SECTION("fails if the port is in use")
    {
      const auto port = listen(transport);

      auto other = HttpTransport{recorder.handler()};
      auto result = other.listen(port);
      REQUIRE(result.is_error());
      CHECK(std::get<QString>(result.error()).contains(QString::number(port)));
      CHECK_FALSE(other.isListening());
      CHECK(other.port() == 0);
    }

    SECTION("fails if already listening")
    {
      const auto port = listen(transport);

      auto result = transport.listen(0);
      REQUIRE(result.is_error());
      CHECK(
        std::get<QString>(result.error())
        == QString{"already listening on port %1"}.arg(port));
      CHECK(transport.port() == port);
    }
  }

  SECTION("close")
  {
    auto transport = HttpTransport{recorder.handler()};

    SECTION("does nothing if not listening")
    {
      transport.close();
      CHECK_FALSE(transport.isListening());
    }

    SECTION("stops listening and closes open connections")
    {
      const auto port = listen(transport);

      auto served = Checkpoint{};
      auto closed = runClient([&]() {
        auto client = Client{};
        return client.connect(port) && client.write(post("{}")) && client.read()
               && served.reach() && client.waitForClose();
      });
      REQUIRE(served.waitUntilReached());
      REQUIRE(transport.connectionCount() == 1);

      transport.close();
      served.proceed();
      CHECK_FALSE(transport.isListening());
      CHECK(transport.port() == 0);
      CHECK(transport.address() == "");
      CHECK(transport.connectionCount() == 0);

      CHECK(waitFor(closed));

      auto refused = runClient([&]() {
        auto other = Client{};
        return other.connect(port);
      });
      CHECK_FALSE(waitFor(refused));

      SECTION("can listen again afterwards")
      {
        const auto newPort = listen(transport);
        auto servedAgain = runClient([&]() {
          auto other = Client{};
          return other.connect(newPort) && other.write(post("{}")) && other.read();
        });
        CHECK(waitFor(servedAgain));
      }
    }
  }

  SECTION("isListening")
  {
    auto transport = HttpTransport{recorder.handler()};
    CHECK_FALSE(transport.isListening());
    listen(transport);
    CHECK(transport.isListening());
    transport.close();
    CHECK_FALSE(transport.isListening());
  }

  SECTION("port")
  {
    auto transport = HttpTransport{recorder.handler()};
    CHECK(transport.port() == 0);
    const auto port = listen(transport);
    CHECK(transport.port() == port);
  }

  SECTION("address")
  {
    auto transport = HttpTransport{recorder.handler()};
    CHECK(transport.address() == "");
    listen(transport);
    CHECK(transport.address() == "127.0.0.1");
  }

  SECTION("connectionCount")
  {
    auto transport = HttpTransport{recorder.handler()};
    const auto port = listen(transport);
    CHECK(transport.connectionCount() == 0);

    auto served = Checkpoint{};
    auto closed = Checkpoint{};
    auto result = runClient([&]() {
      auto client1 = Client{};
      auto client2 = Client{};
      if (
        client1.connect(port) && client1.write(post("1")) && client1.read()
        && client2.connect(port) && client2.write(post("2")) && client2.read()
        && served.reach())
      {
        client1.close();
        return closed.reach();
      }
      return false;
    });

    REQUIRE(served.waitUntilReached());
    CHECK(transport.connectionCount() == 2);
    served.proceed();

    REQUIRE(closed.waitUntilReached());
    CHECK(processEventsUntil([&]() { return transport.connectionCount() == 1; }));
    closed.proceed();
    CHECK(waitFor(result));
  }

  SECTION("serving requests")
  {
    auto options = HttpTransport::Options{};

    SECTION("a request is passed to the handler with the local port")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto response = runClient([&]() -> std::optional<ClientResponse> {
        auto client = Client{};
        if (!client.connect(port) || !client.write(post(R"({"a":1})", "X-Test: yes\r\n")))
        {
          return std::nullopt;
        }
        return client.read();
      });
      const auto received = waitFor(response);

      REQUIRE(recorder.requests.size() == 1);
      const auto& request = recorder.requests.front();
      CHECK(request.method == "POST");
      CHECK(request.target == "/mcp");
      CHECK(request.version == "HTTP/1.1");
      CHECK(request.header("host") == "127.0.0.1");
      CHECK(request.header("x-test") == "yes");
      CHECK(request.body == R"({"a":1})");
      CHECK(recorder.localPorts == std::vector<quint16>{port});

      REQUIRE(received.has_value());
      CHECK(received->status == 200);
      CHECK(received->header("content-type") == "application/json");
      CHECK(received->header("connection") == "keep-alive");
      CHECK(received->header("cache-control") == "no-store");
      CHECK(received->body == R"({"a":1})");
    }

    SECTION("keep-alive connections serve several requests")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto served = Checkpoint{};
      auto bodies = runClient([&]() {
        auto result = std::vector<QByteArray>{};
        auto client = Client{};
        if (client.connect(port))
        {
          for (const auto* body : {"first", "second", "third"})
          {
            if (!client.write(post(body)))
            {
              break;
            }
            if (const auto response = client.read())
            {
              result.push_back(response->body);
            }
          }
        }
        served.reach();
        return result;
      });

      REQUIRE(served.waitUntilReached());
      CHECK(transport.connectionCount() == 1);
      served.proceed();

      CHECK(waitFor(bodies) == std::vector<QByteArray>{"first", "second", "third"});
      CHECK(recorder.requests.size() == 3);
    }

    SECTION("a response to a HEAD request has no body on a kept-alive connection")
    {
      auto transport = HttpTransport{[](const HttpRequest& request, quint16) {
        return HttpResponse{
          .status = 405, .headers = {}, .body = request.method + request.body};
      }};
      const auto port = listen(transport);

      auto responses = runClient([&]() {
        auto result = std::vector<std::pair<int, QByteArray>>{};
        auto client = Client{};
        if (
          client.connect(port)
          && client.write("HEAD /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"))
        {
          if (const auto head = client.readHead())
          {
            result.emplace_back(
              head->status, head->header("content-length").value_or(""));
            if (client.write(post("second")))
            {
              if (const auto response = client.read())
              {
                result.emplace_back(response->status, response->body);
              }
            }
          }
        }
        return result;
      });

      CHECK(
        waitFor(responses)
        == std::vector<std::pair<int, QByteArray>>{{405, "4"}, {405, "POSTsecond"}});
    }

    SECTION("pipelined requests are answered in order")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto bodies = runClient([&]() {
        auto result = std::vector<QByteArray>{};
        auto client = Client{};
        if (client.connect(port) && client.write(post("a") + post("b") + post("c")))
        {
          while (const auto response = client.read())
          {
            result.push_back(response->body);
            if (result.size() == 3)
            {
              break;
            }
          }
        }
        return result;
      });

      CHECK(waitFor(bodies) == std::vector<QByteArray>{"a", "b", "c"});
    }

    SECTION("a body split across writes with a delay is received completely")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      const auto request = post(R"({"text":"split across several writes"})");
      auto response = runClient([&]() -> std::optional<ClientResponse> {
        auto client = Client{};
        if (!client.connect(port))
        {
          return std::nullopt;
        }
        for (auto i = qsizetype(0); i < request.size(); i += 20)
        {
          if (!client.write(request.mid(i, 20)))
          {
            return std::nullopt;
          }
          QThread::msleep(20);
        }
        return client.read();
      });

      const auto received = waitFor(response);
      REQUIRE(received.has_value());
      CHECK(received->body == R"({"text":"split across several writes"})");
      CHECK(recorder.requests.size() == 1);
    }

    SECTION("Connection: close closes the connection after the response")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto result = runClient([&]() -> std::optional<std::pair<ClientResponse, bool>> {
        auto client = Client{};
        if (client.connect(port) && client.write(post("x", "Connection: close\r\n")))
        {
          if (auto response = client.read())
          {
            return std::pair{*response, client.waitForClose()};
          }
        }
        return std::nullopt;
      });

      const auto received = waitFor(result);
      REQUIRE(received.has_value());
      CHECK(received->first.status == 200);
      CHECK(received->first.header("connection") == "close");
      CHECK(received->first.body == "x");
      CHECK(received->second);
      CHECK(processEventsUntil([&]() { return transport.connectionCount() == 0; }));
    }

    SECTION("HTTP/1.0 requests close the connection unless keep-alive is requested")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto result = runClient([&]() {
        auto client = Client{};
        const auto request = QByteArray{"GET /mcp HTTP/1.0\r\nHost: 127.0.0.1\r\n"};
        return client.connect(port)
               && client.write(request + "Connection: keep-alive\r\n\r\n")
               && client.read() && client.write(request + "\r\n") && client.read()
               && client.waitForClose();
      });

      CHECK(waitFor(result));
      CHECK(recorder.requests.size() == 2);
    }

    SECTION("a client disconnecting mid-body doesn't affect other connections")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto aborted = runClient([&]() {
        auto client = Client{};
        const auto request = post(QByteArray(1000, 'x'));
        const auto written = client.connect(port) && client.write(request.left(500));
        client.close();
        return written;
      });
      REQUIRE(waitFor(aborted));
      CHECK(processEventsUntil([&]() { return transport.connectionCount() == 0; }));
      CHECK(recorder.requests.empty());

      auto served = runClient([&]() {
        auto client = Client{};
        return client.connect(port) && client.write(post("next")) && client.read();
      });
      CHECK(waitFor(served));
      CHECK(recorder.requests.size() == 1);
    }

    SECTION("a client disconnecting before reading the response is handled")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto sent = runClient([&]() {
        auto client = Client{};
        const auto written = client.connect(port) && client.write(post("gone"));
        client.close();
        return written;
      });
      REQUIRE(waitFor(sent));
      CHECK(processEventsUntil([&]() { return transport.connectionCount() == 0; }));

      auto served = runClient([&]() {
        auto client = Client{};
        return client.connect(port) && client.write(post("next")) && client.read();
      });
      CHECK(waitFor(served));
    }

    SECTION("a socket is not deleted while its request is handled")
    {
      // The client goes away while the handler spins a nested event loop, which delivers
      // the disconnect. The socket must survive until the handler has returned, because
      // the transport is called from within the socket's readyRead signal.
      auto transport = std::unique_ptr<HttpTransport>{};
      auto handled = false;
      auto socketsAlive = false;
      auto sockets = std::vector<QPointer<QTcpSocket>>{};
      transport =
        std::make_unique<HttpTransport>([&](const HttpRequest& request, quint16) {
          if (request.body != "x")
          {
            return HttpResponse{};
          }
          for (auto* socket : transport->findChildren<QTcpSocket*>())
          {
            sockets.emplace_back(socket);
          }
          handled =
            processEventsUntil([&]() { return transport->connectionCount() == 0; });

          // give deferred deletions a chance to run
          const auto deadline = QDeadlineTimer{50ms};
          while (!deadline.hasExpired())
          {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
          }

          socketsAlive = !sockets.empty()
                         && std::ranges::all_of(
                           sockets, [](const auto& socket) { return !socket.isNull(); });
          return HttpResponse{};
        });
      const auto port = listen(*transport);

      auto sent = runClient([&]() {
        auto client = Client{};
        const auto written = client.connect(port) && client.write(post("x"));
        client.close();
        return written;
      });

      CHECK(waitFor(sent));
      CHECK(processEventsUntil([&]() { return handled; }));
      CHECK(socketsAlive);
      CHECK(transport->connectionCount() == 0);

      // the socket is deleted once the handler has returned
      CHECK(processEventsUntil([&]() {
        return std::ranges::all_of(
          sockets, [](const auto& socket) { return socket.isNull(); });
      }));

      auto served = runClient([&]() {
        auto client = Client{};
        return client.connect(port) && client.write(post("next")) && client.read();
      });
      CHECK(waitFor(served));
    }

#ifndef _WIN32
    SECTION("a client that half-closes the connection receives the response")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto response = runClient([&]() -> std::optional<ClientResponse> {
        auto client = Client{};
        if (client.connect(port) && client.write(post("half")) && client.shutdownWrite())
        {
          return client.read();
        }
        return std::nullopt;
      });
      const auto received = waitFor(response);
      REQUIRE(received.has_value());
      CHECK(received->status == 200);
      CHECK(received->body == "half");
      CHECK(processEventsUntil([&]() { return transport.connectionCount() == 0; }));
    }
#endif

    SECTION("malformed requests get an error response and the connection is closed")
    {
      options.limits =
        HttpRequestParser::Limits{.maxHeaderBytes = 256, .maxBodyBytes = 16};
      auto transport = HttpTransport{recorder.handler(), options};
      const auto port = listen(transport);

      using T = std::pair<QByteArray, int>;
      const auto [request, expectedStatus] = GENERATE(
        T{"GARBAGE\r\n\r\n", 400},
        T{"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 411},
        T{"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 17\r\n\r\n", 413},
        T{"GET /mcp HTTP/1.1\r\nX-Long: " + QByteArray(300, 'x') + "\r\n\r\n", 431},
        T{"POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        T{"GET /mcp HTTP/2.0\r\n\r\n", 505});

      CAPTURE(request, expectedStatus);

      auto result = runClient([&]() -> std::optional<std::pair<ClientResponse, bool>> {
        auto client = Client{};
        // a valid request before the malformed one is still served
        if (client.connect(port) && client.write(post("ok") + request))
        {
          const auto first = client.read();
          if (first && first->body == "ok")
          {
            if (auto response = client.read())
            {
              return std::pair{*response, client.waitForClose()};
            }
          }
        }
        return std::nullopt;
      });

      const auto received = waitFor(result);
      REQUIRE(received.has_value());
      const auto& [response, closed] = *received;
      CHECK(response.status == expectedStatus);
      CHECK(response.header("connection") == "close");
      CHECK(response.header("content-type") == "application/json");
      const auto body = QJsonDocument::fromJson(response.body).object();
      CHECK(body["jsonrpc"] == "2.0");
      CHECK(body["id"] == QJsonValue::Null);
      CHECK(body["error"].toObject()["code"] == -32600);
      CHECK(
        body["error"].toObject()["message"]
        == QString::fromLatin1(reasonPhrase(expectedStatus)));
      CHECK(closed);

      CHECK(recorder.requests.size() == 1);
      CHECK(processEventsUntil([&]() { return transport.connectionCount() == 0; }));
    }

    SECTION("a request at the body limit is accepted")
    {
      options.limits =
        HttpRequestParser::Limits{.maxHeaderBytes = 256, .maxBodyBytes = 16};
      auto transport = HttpTransport{recorder.handler(), options};
      const auto port = listen(transport);

      auto response = runClient([&]() -> std::optional<ClientResponse> {
        auto client = Client{};
        if (client.connect(port) && client.write(post(QByteArray(16, 'x'))))
        {
          return client.read();
        }
        return std::nullopt;
      });
      const auto received = waitFor(response);
      REQUIRE(received.has_value());
      CHECK(received->status == 200);
      CHECK(received->body == QByteArray(16, 'x'));
    }

    SECTION("a large body is received")
    {
      auto transport = HttpTransport{recorder.handler()};
      const auto port = listen(transport);

      auto body = QByteArray{};
      for (auto i = 0; i < 300000; ++i)
      {
        body += char('a' + i % 26);
      }

      auto response = runClient([&]() -> std::optional<ClientResponse> {
        auto client = Client{};
        if (client.connect(port) && client.write(post(body)))
        {
          return client.read();
        }
        return std::nullopt;
      });
      const auto received = waitFor(response);
      REQUIRE(received.has_value());
      CHECK(received->body == body);
    }

    SECTION("connections beyond the limit are aborted")
    {
      options.maxConnections = 2;
      auto transport = HttpTransport{recorder.handler(), options};
      const auto port = listen(transport);

      auto served = Checkpoint{};
      auto rejected = Checkpoint{};
      auto closed = Checkpoint{};
      auto reconnected = Checkpoint{};
      auto result = runClient([&]() {
        auto client1 = Client{};
        auto client2 = Client{};
        if (!(client1.connect(port) && client1.write(post("1")) && client1.read()
              && client2.connect(port) && client2.write(post("2")) && client2.read()
              && served.reach()))
        {
          return false;
        }

        // the operating system accepts the connection, but the transport aborts it
        auto client3 = Client{};
        if (!(client3.connect(port) && !client3.read() && client3.waitForClose()
              && rejected.reach()))
        {
          return false;
        }

        // a new connection is accepted once another one is closed
        client1.close();
        if (!closed.reach())
        {
          return false;
        }
        auto client4 = Client{};
        return client4.connect(port) && client4.write(post("4")) && client4.read()
               && client2.write(post("2")) && client2.read() && reconnected.reach();
      });

      REQUIRE(served.waitUntilReached());
      CHECK(transport.connectionCount() == 2);
      served.proceed();

      REQUIRE(rejected.waitUntilReached());
      CHECK(transport.connectionCount() == 2);
      CHECK(recorder.requests.size() == 2);
      rejected.proceed();

      REQUIRE(closed.waitUntilReached());
      CHECK(processEventsUntil([&]() { return transport.connectionCount() == 1; }));
      closed.proceed();

      REQUIRE(reconnected.waitUntilReached());
      CHECK(transport.connectionCount() == 2);
      CHECK(recorder.requests.size() == 4);
      reconnected.proceed();

      CHECK(waitFor(result));
    }

    SECTION("idle connections are closed after the idle timeout")
    {
      options.idleTimeout = 100ms;
      auto transport = HttpTransport{recorder.handler(), options};
      const auto port = listen(transport);

      SECTION("without any request")
      {
        auto result = runClient([&]() {
          auto timer = QElapsedTimer{};
          auto client = Client{};
          if (!client.connect(port))
          {
            return std::optional<std::chrono::milliseconds>{};
          }
          timer.start();
          return client.waitForClose()
                   ? std::optional{std::chrono::milliseconds{timer.elapsed()}}
                   : std::nullopt;
        });

        const auto elapsed = waitFor(result);
        REQUIRE(elapsed.has_value());
        CHECK(*elapsed >= 50ms);
        CHECK(*elapsed < Timeout);
      }

      SECTION("after a request")
      {
        auto result = runClient([&]() {
          auto client = Client{};
          return client.connect(port) && client.write(post("1")) && client.read()
                 && client.waitForClose();
        });
        CHECK(waitFor(result));
        CHECK(recorder.requests.size() == 1);
      }

      SECTION("requests restart the timer")
      {
        auto result = runClient([&]() {
          auto client = Client{};
          if (!client.connect(port))
          {
            return false;
          }
          for (auto i = 0; i < 5; ++i)
          {
            QThread::msleep(50);
            if (!client.write(post("x")) || !client.read())
            {
              return false;
            }
          }
          return true;
        });
        CHECK(waitFor(result));
        CHECK(recorder.requests.size() == 5);
      }

      CHECK(processEventsUntil([&]() { return transport.connectionCount() == 0; }));
    }

    SECTION("an exception thrown by the handler gives a 500 response")
    {
      auto transport = HttpTransport{[](const HttpRequest&, quint16) -> HttpResponse {
        throw std::runtime_error{"oops"};
      }};
      const auto port = listen(transport);

      auto response = runClient([&]() -> std::optional<ClientResponse> {
        auto client = Client{};
        if (client.connect(port) && client.write(post("{}")))
        {
          return client.read();
        }
        return std::nullopt;
      });
      const auto received = waitFor(response);
      REQUIRE(received.has_value());
      CHECK(received->status == 500);
      CHECK(received->header("connection") == "keep-alive");
      const auto body = QJsonDocument::fromJson(received->body).object();
      CHECK(body["error"].toObject()["code"] == -32603);
      CHECK(body["error"].toObject()["message"] == "internal error: oops");
    }

    SECTION("requests of one connection are not handled re-entrantly")
    {
      auto depth = 0;
      auto maxDepth = 0;
      auto bodies = std::vector<QByteArray>{};
      auto transport = HttpTransport{[&](const HttpRequest& request, quint16) {
        ++depth;
        maxDepth = std::max(depth, maxDepth);
        bodies.push_back(request.body);
        if (request.body == "slow")
        {
          // spin a nested event loop while the next request arrives
          const auto deadline = QDeadlineTimer{300ms};
          while (!deadline.hasExpired())
          {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
          }
        }
        --depth;
        auto response = HttpResponse{};
        response.body = request.body;
        return response;
      }};
      const auto port = listen(transport);

      auto result = runClient([&]() {
        auto received = std::vector<QByteArray>{};
        auto client = Client{};
        // The response to the first request is sent while the second one is handled,
        // so the transport is notified about written bytes in the nested event loop
        // while the third request is already buffered.
        if (
          client.connect(port)
          && client.write(post("first") + post("slow") + post("fast")))
        {
          while (const auto response = client.read())
          {
            received.push_back(response->body);
            if (received.size() == 3)
            {
              break;
            }
          }
        }
        return received;
      });

      CHECK(waitFor(result) == std::vector<QByteArray>{"first", "slow", "fast"});
      CHECK(bodies == std::vector<QByteArray>{"first", "slow", "fast"});
      CHECK(maxDepth == 1);
    }

    SECTION("other connections are served while a handler spins an event loop")
    {
      auto transport = std::unique_ptr<HttpTransport>{};
      auto innerServed = false;
      transport = std::make_unique<HttpTransport>(
        [&](const HttpRequest& request, const quint16 localPort) {
          if (request.body == "outer")
          {
            auto inner = runClient([localPort]() {
              auto client = Client{};
              return client.connect(localPort) && client.write(post("inner"))
                     && client.read().has_value();
            });
            innerServed = waitFor(inner);
          }
          auto response = HttpResponse{};
          response.body = request.body;
          return response;
        });
      const auto port = listen(*transport);

      auto result = runClient([&]() {
        auto client = Client{};
        if (client.connect(port) && client.write(post("outer")))
        {
          const auto response = client.read();
          return response && response->body == "outer";
        }
        return false;
      });

      CHECK(waitFor(result));
      CHECK(innerServed);
    }

    SECTION("the handler may close the transport")
    {
      auto transport = std::unique_ptr<HttpTransport>{};
      transport = std::make_unique<HttpTransport>([&](const HttpRequest&, quint16) {
        transport->close();
        return HttpResponse{};
      });
      const auto port = listen(*transport);

      auto result = runClient([&]() {
        auto client = Client{};
        return client.connect(port) && client.write(post("x") + post("y"))
               && !client.read() && client.waitForClose();
      });

      CHECK(waitFor(result));
      CHECK_FALSE(transport->isListening());
      CHECK(transport->connectionCount() == 0);

      // process the deferred deletion of the socket
      QCoreApplication::processEvents();
    }

    SECTION("the handler may schedule the deletion of the transport")
    {
      auto guard = QPointer<HttpTransport>{};
      auto handled = false;
      auto* transport = new HttpTransport{[&](const HttpRequest&, quint16) {
        handled = true;
        guard->deleteLater();
        return HttpResponse{};
      }};
      guard = transport;
      const auto port = listen(*transport);

      auto result = runClient([&]() {
        auto client = Client{};
        if (client.connect(port) && client.write(post("x")))
        {
          client.read();
          client.waitForClose();
          return true;
        }
        return false;
      });

      CHECK(waitFor(result));
      CHECK(handled);
      CHECK(processEventsUntil([&]() { return guard.isNull(); }));
    }

    SECTION("the transport may be deleted in a nested event loop of the handler")
    {
      auto guard = QPointer<HttpTransport>{};
      auto deletedInHandler = false;
      auto* transport = new HttpTransport{[&](const HttpRequest&, quint16) {
        QTimer::singleShot(0, [&]() { guard->deleteLater(); });
        deletedInHandler = processEventsUntil([&]() { return guard.isNull(); });
        return HttpResponse{};
      }};
      guard = transport;
      const auto port = listen(*transport);

      auto result = runClient([&]() {
        auto client = Client{};
        return client.connect(port) && client.write(post("x")) && !client.read()
               && client.waitForClose();
      });

      CHECK(waitFor(result));
      CHECK(deletedInHandler);
      CHECK(guard.isNull());

      // process the deferred deletion of the socket
      QCoreApplication::processEvents();
    }

    SECTION("serves the MCP server")
    {
      auto tools = ToolRegistry{};
      auto server = Server{ServerInfo{.version = "1"}, tools, []() { return "s1"; }};
      auto transport =
        HttpTransport{[&](const HttpRequest& request, const quint16 localPort) {
          return server.handle(request, localPort);
        }};
      const auto port = listen(transport);

      const auto initialize = QByteArray{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})"};
      const auto request = [&](const QByteArray& host) {
        return "POST /mcp HTTP/1.1\r\nHost: " + host
               + "\r\nContent-Type: application/json\r\nAccept: application/json, "
                 "text/event-stream\r\nContent-Length: "
               + QByteArray::number(initialize.size()) + "\r\n\r\n" + initialize;
      };

      auto result = runClient([&]() {
        auto responses = std::vector<ClientResponse>{};
        auto client = Client{};
        if (client.connect(port))
        {
          for (const auto& host :
               {QByteArray{"evil.com"},
                "127.0.0.1:" + QByteArray::number(port + 1),
                "127.0.0.1:" + QByteArray::number(port)})
          {
            if (!client.write(request(host)))
            {
              break;
            }
            if (auto response = client.read())
            {
              responses.push_back(*response);
            }
          }
        }
        return responses;
      });

      const auto responses = waitFor(result);
      REQUIRE(responses.size() == 3);
      CHECK(responses[0].status == 403);
      CHECK(responses[0].header("connection") == "keep-alive");
      CHECK(responses[1].status == 403);
      CHECK(responses[2].status == 200);
      CHECK(responses[2].header("mcp-session-id") == "s1");
      const auto body = QJsonDocument::fromJson(responses[2].body).object();
      CHECK(body["id"] == 1);
      CHECK(body["result"].toObject()["protocolVersion"] == "2025-06-18");
      CHECK(server.sessionIds() == std::vector<QString>{"s1"});
    }

    SECTION("destroying the transport closes open connections")
    {
      auto transport = std::make_unique<HttpTransport>(recorder.handler());
      const auto port = listen(*transport);

      auto served = Checkpoint{};
      auto closed = runClient([&]() {
        auto client = Client{};
        return client.connect(port) && client.write(post("x")) && client.read()
               && served.reach() && client.waitForClose();
      });

      REQUIRE(served.waitUntilReached());
      transport.reset();
      served.proceed();

      CHECK(waitFor(closed));
    }
  }
}

} // namespace tb::mcp
