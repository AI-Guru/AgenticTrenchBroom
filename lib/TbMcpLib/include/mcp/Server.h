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

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include "mcp/Http.h"
#include "mcp/JsonRpc.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace tb::mcp
{
class ToolRegistry;

inline constexpr auto LatestProtocolVersion = "2025-06-18";

/**
 * Returns the case-insensitive Levenshtein distance of the given strings. Used to suggest
 * names for unknown ones, e.g. tool names or entity classnames.
 */
size_t editDistance(const QString& lhs, const QString& rhs);

/** Returns the supported protocol versions, latest first. */
QStringList supportedProtocolVersions();

struct ServerInfo
{
  QString name = "trenchbroom";
  QString title = "TrenchBroom";
  QString version;
  QString instructions;
};

struct Session
{
  QString id;
  QString protocolVersion;
  bool initialized = false;
  quint64 lastUse = 0;
};

/**
 * Implements the MCP lifecycle and the tools methods on top of the Streamable HTTP
 * transport (JSON responses only, no SSE). The server does not do any I/O: it maps an
 * HTTP request to an HTTP response.
 *
 * Requests are routed as follows:
 * 1. The path must be /mcp (404).
 * 2. The Host header must name 127.0.0.1, localhost or [::1], with the local port if a
 *    port is given (403).
 * 3. An Origin header, if present, must be an http(s) URL on one of these hosts (403).
 * 4. Only POST and DELETE are allowed (405). DELETE ends a known session (204, else 404).
 * 5. POST requires Content-Type: application/json (415), an Accept header (if present)
 *    that allows JSON (406), and a body that is a single JSON-RPC message (400).
 * 6. Every message other than initialize requires a known Mcp-Session-Id (400 if
 *    missing, 404 if unknown).
 * 7. An MCP-Protocol-Version header, if present, must be supported (400).
 * 8. Notifications and responses are answered with 202, requests with 200.
 *
 * The server is reentrant: a tool handler may cause handle() to be called again (e.g.
 * from a nested event loop). A tools/call request made while another one is being
 * dispatched gets the tool error editor_busy.
 */
class Server
{
public:
  static constexpr size_t MaxSessions = 16;

  /**
   * Creates a server offering the given tools. If makeSessionId is not set, session ids
   * are random UUIDs.
   */
  Server(
    ServerInfo info, ToolRegistry& tools, std::function<QString()> makeSessionId = {});

  HttpResponse handle(const HttpRequest& request, quint16 localPort);

  /** Called when a session is ended by the client or evicted. */
  std::function<void(const QString& sessionId)> sessionClosed;

  bool isDispatchingToolCall() const;

  /** Returns the ids of all current sessions, sorted. */
  std::vector<QString> sessionIds() const;

  /** Returns the session with the given id, or nullptr if there is none. */
  const Session* session(const QString& id) const;

private:
  HttpResponse handlePost(const HttpRequest& request);
  HttpResponse handleDelete(const HttpRequest& request);

  QJsonObject handleRequest(const jsonrpc::Request& request);
  void handleNotification(const jsonrpc::Notification& notification, Session& session);

  QJsonObject initialize(const jsonrpc::Request& request, QString& newSessionId);
  QJsonObject listTools(const jsonrpc::Request& request) const;
  QJsonObject callTool(const jsonrpc::Request& request);

  Session* findSession(const QString& id);
  void closeSession(QString id);

  ServerInfo m_info;
  ToolRegistry& m_tools;
  std::function<QString()> m_makeSessionId;

  std::map<QString, Session> m_sessions;
  quint64 m_clock = 0;
  bool m_dispatching = false;
};

} // namespace tb::mcp
