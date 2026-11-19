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

#pragma once

#include <QObject>
#include <QString>

#include "mcp/Http.h"

#include "kd/result.h"

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace tb::mcp
{

/**
 * A minimal HTTP/1.1 server that listens on the IPv4 loopback interface only and passes
 * each request to a handler, e.g. Server::handle.
 *
 * Everything happens on the thread that owns the transport. Requests are parsed with
 * HttpRequestParser and the handler is called synchronously. Connections are persistent
 * (keep-alive) unless the request asks otherwise, and pipelined requests are answered in
 * order. Requests on one connection are never handled concurrently, not even if the
 * handler spins a nested event loop.
 *
 * Malformed requests and requests exceeding the size limits get an error response with
 * "Connection: close", after which the connection is closed. Connections beyond the
 * connection limit are aborted immediately. A connection that stays idle (no request
 * being received or handled completely) for longer than the idle timeout is closed.
 *
 * The transport doesn't validate the Host and Origin headers; that is the job of the
 * handler (see Server::handle), which receives the local port for this purpose.
 *
 * The transport must not be destroyed from within the handler; use deleteLater instead,
 * which is safe even if it runs in a nested event loop spun by the handler. Calling
 * close() from within the handler is fine, but the pending response is dropped. A client
 * may disconnect at any time, also while its request is being handled.
 */
class HttpTransport : public QObject
{
  Q_OBJECT
public:
  using Handler = std::function<HttpResponse(const HttpRequest&, quint16 localPort)>;

  struct Options
  {
    int maxConnections = 16;
    std::chrono::milliseconds idleTimeout{60000};
    HttpRequestParser::Limits limits = {};
  };

  explicit HttpTransport(Handler handler, QObject* parent = nullptr);
  HttpTransport(Handler handler, Options options, QObject* parent = nullptr);
  ~HttpTransport() override;

  /**
   * Starts listening on 127.0.0.1 at the given port, or at an ephemeral port if the given
   * port is 0. Returns the port listened on, or an error message if the port cannot be
   * bound or the transport is already listening.
   */
  kdl::result<quint16, QString> listen(quint16 port);

  /**
   * Stops listening and closes all open connections. Responses that have not been
   * written completely yet are dropped. Does nothing if the transport isn't listening.
   */
  void close();

  bool isListening() const;

  /**
   * Returns the port listened on, or 0 if the transport isn't listening.
   */
  quint16 port() const;

  /**
   * Returns the address listened on ("127.0.0.1"), or an empty string if the transport
   * isn't listening.
   */
  QString address() const;

  /**
   * Returns the number of open connections.
   */
  size_t connectionCount() const;

private:
  struct Connection;

  void onNewConnection();
  void onReadyRead(QTcpSocket* socket);
  void onDisconnected(QTcpSocket* socket);
  void onIdleTimeout(QTcpSocket* socket);

  void process(QTcpSocket* socket, Connection& connection);
  void closeConnection(QTcpSocket* socket);
  void removeConnection(QTcpSocket* socket);
  HttpResponse callHandler(const HttpRequest& request, quint16 localPort);

  Handler m_handler;
  Options m_options;
  QTcpServer* m_server;
  std::unordered_map<QTcpSocket*, std::shared_ptr<Connection>> m_connections;
};

} // namespace tb::mcp
