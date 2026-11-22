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

#include "mcp/HttpTransport.h"

#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "mcp/JsonRpc.h"

#include <exception>
#include <vector>

namespace tb::mcp
{
namespace
{

/** The size of the chunks read from a socket, and the socket's read buffer size. */
constexpr auto ReadChunkSize = qint64(64 * 1024);

/**
 * Requests on a connection are not processed while more than this many bytes of
 * responses wait to be sent, so that a client that pipelines requests without reading
 * the responses cannot make the write buffer grow without bounds.
 */
constexpr auto MaxPendingResponseBytes = qint64(32 * 1024 * 1024);

HttpResponse errorResponse(
  const int status, const jsonrpc::ErrorCode code, const QString& message)
{
  return HttpResponse::json(status, jsonrpc::makeError(QJsonValue::Null, code, message));
}

} // namespace

struct HttpTransport::Connection
{
  HttpRequestParser parser;
  QTimer* idleTimer = nullptr; // child of the socket

  /** Set while requests of this connection are being processed. */
  bool busy = false;

  /** Set once the connection is being closed; no further requests are processed. */
  bool closing = false;

  /** Set once the connection has been removed from the transport. */
  bool removed = false;

  explicit Connection(const HttpRequestParser::Limits& limits)
    : parser{limits}
  {
  }
};

HttpTransport::HttpTransport(Handler handler, QObject* parent)
  : HttpTransport{std::move(handler), Options{}, parent}
{
}

HttpTransport::HttpTransport(Handler handler, Options options, QObject* parent)
  : QObject{parent}
  , m_handler{std::move(handler)}
  , m_options{std::move(options)}
  , m_server{new QTcpServer{this}}
{
  connect(m_server, &QTcpServer::newConnection, this, &HttpTransport::onNewConnection);
}

HttpTransport::~HttpTransport()
{
  close();
}

kdl::result<quint16, QString> HttpTransport::listen(const quint16 port)
{
  if (m_server->isListening())
  {
    return QString{"already listening on port %1"}.arg(m_server->serverPort());
  }

  if (!m_server->listen(QHostAddress::LocalHost, port))
  {
    return QString{"cannot listen on 127.0.0.1:%1: %2"}.arg(port).arg(
      m_server->errorString());
  }

  return m_server->serverPort();
}

void HttpTransport::close()
{
  m_server->close();

  auto sockets = std::vector<QTcpSocket*>{};
  sockets.reserve(m_connections.size());
  for (const auto& [socket, connection] : m_connections)
  {
    sockets.push_back(socket);
  }

  for (auto* socket : sockets)
  {
    removeConnection(socket);
  }
}

bool HttpTransport::isListening() const
{
  return m_server->isListening();
}

quint16 HttpTransport::port() const
{
  return m_server->isListening() ? m_server->serverPort() : quint16{0};
}

QString HttpTransport::address() const
{
  return m_server->isListening() ? m_server->serverAddress().toString() : QString{};
}

size_t HttpTransport::connectionCount() const
{
  return m_connections.size();
}

void HttpTransport::onNewConnection()
{
  while (auto* socket = m_server->nextPendingConnection())
  {
    if (int(m_connections.size()) >= m_options.maxConnections)
    {
      socket->abort();
      socket->deleteLater();
      continue;
    }

    auto connection = std::make_shared<Connection>(m_options.limits);
    connection->idleTimer = new QTimer{socket};
    connection->idleTimer->setSingleShot(true);
    connection->idleTimer->setInterval(m_options.idleTimeout);

    socket->setReadBufferSize(ReadChunkSize);

    connect(
      socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
    connect(
      socket, &QTcpSocket::bytesWritten, this, [this, socket]() { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
      onDisconnected(socket);
    });
    connect(connection->idleTimer, &QTimer::timeout, this, [this, socket]() {
      onIdleTimeout(socket);
    });

    connection->idleTimer->start();
    m_connections.emplace(socket, std::move(connection));

    // the client may have sent data before the connection was accepted
    if (socket->bytesAvailable() > 0)
    {
      onReadyRead(socket);
    }
  }
}

void HttpTransport::onReadyRead(QTcpSocket* socket)
{
  if (const auto it = m_connections.find(socket); it != m_connections.end())
  {
    // keep the connection alive even if it is removed while it is being processed
    const auto connection = it->second;
    process(socket, *connection);
  }
}

void HttpTransport::onDisconnected(QTcpSocket* socket)
{
  removeConnection(socket);
}

void HttpTransport::onIdleTimeout(QTcpSocket* socket)
{
  if (const auto it = m_connections.find(socket); it != m_connections.end())
  {
    if (it->second->closing)
    {
      // the client doesn't read the remaining response bytes
      removeConnection(socket);
    }
    else
    {
      closeConnection(socket);
    }
  }
}

void HttpTransport::process(QTcpSocket* socket, Connection& connection)
{
  // A nested event loop in the handler can deliver further signals for this connection.
  // They are ignored here; the outer call picks up the remaining data.
  if (connection.busy || connection.closing || connection.removed)
  {
    return;
  }

  const auto self = QPointer<HttpTransport>{this};
  const auto guardedSocket = QPointer<QTcpSocket>{socket};

  connection.busy = true;
  while (socket->bytesToWrite() <= MaxPendingResponseBytes)
  {
    const auto state = connection.parser.state();
    if (state == HttpRequestParser::State::Complete)
    {
      connection.idleTimer->stop();

      const auto request = connection.parser.takeRequest();
      const auto keepAlive = request.keepAlive();
      const auto headRequest = request.method == "HEAD";
      const auto response = callHandler(request, socket->localPort());

      if (!self)
      {
        // The transport was destroyed while the handler was running, e.g. by a deferred
        // deletion in a nested event loop. The connection was removed then, and the
        // socket was kept alive because this function is called from one of its signals.
        if (guardedSocket)
        {
          guardedSocket->deleteLater();
        }
        return;
      }
      if (connection.removed || !guardedSocket)
      {
        // the connection was closed while the handler was running
        break;
      }

      socket->write(response.serialize(keepAlive, headRequest));

      // Hand the response to the operating system right away. If the client has shut
      // down its sending side, the socket is closed as soon as control returns to the
      // event loop, and any data still buffered here would be lost.
      socket->flush();
      if (connection.removed)
      {
        break;
      }

      if (!keepAlive)
      {
        closeConnection(socket);
        break;
      }

      connection.idleTimer->start();
    }
    else if (state == HttpRequestParser::State::Error)
    {
      const auto status = connection.parser.errorStatus();
      socket->write(
        errorResponse(
          status, jsonrpc::InvalidRequest, QString::fromLatin1(reasonPhrase(status)))
          .serialize(false));
      socket->flush();
      closeConnection(socket);
      break;
    }
    else
    {
      const auto data = socket->read(ReadChunkSize);
      if (data.isEmpty())
      {
        break;
      }
      connection.parser.feed(data);
    }
  }
  connection.busy = false;

  if (connection.removed && guardedSocket)
  {
    // the connection was removed while it was being processed, see removeConnection
    guardedSocket->deleteLater();
  }
}

void HttpTransport::closeConnection(QTcpSocket* socket)
{
  if (const auto it = m_connections.find(socket); it != m_connections.end())
  {
    auto& connection = *it->second;
    if (!connection.closing)
    {
      connection.closing = true;

      // abort the connection if the client doesn't read the remaining bytes in time
      connection.idleTimer->start();

      // may emit disconnected synchronously, which removes the connection
      socket->disconnectFromHost();
    }
  }
}

void HttpTransport::removeConnection(QTcpSocket* socket)
{
  if (const auto it = m_connections.find(socket); it != m_connections.end())
  {
    auto connection = std::move(it->second);
    m_connections.erase(it);

    connection->removed = true;
    connection->idleTimer->stop();

    socket->disconnect(this);
    connection->idleTimer->disconnect(this);
    socket->abort();

    if (connection->busy)
    {
      // process() is running for this socket, called from one of its signals, and the
      // handler may be spinning a nested event loop that would run a deferred deletion
      // right away. process() deletes the socket once the handler has returned. Until
      // then, the socket must not be deleted with the server either.
      socket->setParent(nullptr);
    }
    else
    {
      // the socket may be emitting a signal right now; the timer is deleted with it
      socket->deleteLater();
    }
  }
}

HttpResponse HttpTransport::callHandler(
  const HttpRequest& request, const quint16 localPort)
{
  try
  {
    // the handler may destroy the transport, e.g. in a nested event loop
    const auto handler = m_handler;
    return handler(request, localPort);
  }
  catch (const std::exception& e)
  {
    return errorResponse(
      500, jsonrpc::InternalError, QString{"internal error: %1"}.arg(e.what()));
  }
}

} // namespace tb::mcp
