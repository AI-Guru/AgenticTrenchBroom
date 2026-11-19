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

#include "mcp/Server.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QUuid>

#include "mcp/Tool.h"

#include "kd/set_temp.h"

#include <algorithm>
#include <exception>
#include <numeric>

namespace tb::mcp
{
namespace
{

constexpr auto SessionIdHeader = "Mcp-Session-Id";

/**
 * Unknown tool names are truncated to this length before they are echoed or compared to
 * the known names. This bounds the cost of computing suggestions, which is quadratic in
 * the name length, and the size of the error response for a huge name.
 */
constexpr auto MaxReportedToolNameLength = qsizetype{128};

QString makeRandomSessionId()
{
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

HttpResponse jsonRpcErrorResponse(
  const int status,
  const QJsonValue& id,
  const jsonrpc::ErrorCode code,
  const QString& message,
  const QJsonValue& data = {})
{
  return HttpResponse::json(status, jsonrpc::makeError(id, code, message, data));
}

HttpResponse httpError(const int status, const QString& message)
{
  return jsonRpcErrorResponse(status, QJsonValue::Null, jsonrpc::InvalidRequest, message);
}

bool isLoopbackHostName(const QByteArrayView name)
{
  return name == "127.0.0.1" || name == "localhost" || name == "[::1]";
}

bool isAllowedHost(const QByteArray& hostHeader, const quint16 localPort)
{
  const auto host = hostHeader.trimmed().toLower();

  auto name = QByteArray{};
  auto port = std::optional<QByteArray>{};
  if (host.startsWith('['))
  {
    const auto close = host.indexOf(']');
    if (close < 0)
    {
      return false;
    }
    name = host.left(close + 1);
    const auto rest = host.mid(close + 1);
    if (!rest.isEmpty())
    {
      if (!rest.startsWith(':'))
      {
        return false;
      }
      port = rest.mid(1);
    }
  }
  else
  {
    const auto colon = host.indexOf(':');
    if (colon >= 0)
    {
      if (host.lastIndexOf(':') != colon)
      {
        return false;
      }
      name = host.left(colon);
      port = host.mid(colon + 1);
    }
    else
    {
      name = host;
    }
  }

  if (!isLoopbackHostName(name))
  {
    return false;
  }

  if (port)
  {
    if (port->isEmpty() || !std::ranges::all_of(*port, [](const char c) {
          return c >= '0' && c <= '9';
        }))
    {
      return false;
    }
    auto ok = false;
    const auto value = port->toUInt(&ok);
    return ok && value == localPort;
  }

  return true;
}

bool isAllowedOrigin(const QByteArray& origin)
{
  const auto url = QUrl{QString::fromUtf8(origin.trimmed()), QUrl::StrictMode};
  if (!url.isValid())
  {
    return false;
  }

  const auto scheme = url.scheme().toLower();
  if (scheme != u"http" && scheme != u"https")
  {
    return false;
  }

  const auto host = url.host().toLower();
  return host == u"localhost" || host == u"127.0.0.1" || host == u"::1";
}

QByteArray mediaType(const QByteArrayView value)
{
  const auto semicolon = value.indexOf(';');
  const auto type = semicolon >= 0 ? value.first(semicolon) : value;
  return type.trimmed().toByteArray().toLower();
}

bool acceptsJson(const QByteArray& accept)
{
  return std::ranges::any_of(accept.split(','), [](const auto& range) {
    const auto type = mediaType(range);
    return type == "application/json" || type == "application/*" || type == "*/*";
  });
}

QJsonArray suggestToolNames(const ToolRegistry& tools, const QString& name)
{
  auto candidates = std::vector<std::pair<size_t, QString>>{};
  for (const auto& tool : tools.tools())
  {
    candidates.emplace_back(editDistance(name, tool.name), tool.name);
  }
  std::ranges::sort(candidates);

  auto result = QJsonArray{};
  for (size_t i = 0; i < candidates.size() && i < 3; ++i)
  {
    result.push_back(candidates[i].second);
  }
  return result;
}

} // namespace

size_t editDistance(const QString& lhs, const QString& rhs)
{
  const auto a = lhs.toLower();
  const auto b = rhs.toLower();
  const auto n = static_cast<size_t>(b.size());

  auto previous = std::vector<size_t>(n + 1);
  auto current = std::vector<size_t>(n + 1);
  std::iota(previous.begin(), previous.end(), size_t{0});

  for (auto i = qsizetype{0}; i < a.size(); ++i)
  {
    current[0] = static_cast<size_t>(i) + 1;
    for (auto j = size_t{0}; j < n; ++j)
    {
      const auto substitution =
        previous[j] + (a[i] == b[static_cast<qsizetype>(j)] ? 0u : 1u);
      current[j + 1] = std::min({previous[j + 1] + 1, current[j] + 1, substitution});
    }
    std::swap(previous, current);
  }
  return previous[n];
}

QStringList supportedProtocolVersions()
{
  return {LatestProtocolVersion, "2025-03-26"};
}

Server::Server(
  ServerInfo info, ToolRegistry& tools, std::function<QString()> makeSessionId)
  : m_info{std::move(info)}
  , m_tools{tools}
  , m_makeSessionId{makeSessionId ? std::move(makeSessionId) : makeRandomSessionId}
{
}

HttpResponse Server::handle(const HttpRequest& request, const quint16 localPort)
{
  const auto queryStart = request.target.indexOf('?');
  const auto path = queryStart >= 0 ? request.target.left(queryStart) : request.target;
  if (path != "/mcp")
  {
    return httpError(404, "not found; the MCP endpoint is /mcp");
  }

  const auto host = request.header("host");
  if (!host || !isAllowedHost(*host, localPort))
  {
    return httpError(403, "the Host header must name a loopback address");
  }

  if (const auto origin = request.header("origin"); origin && !isAllowedOrigin(*origin))
  {
    return httpError(403, "requests from this origin are not allowed");
  }

  if (request.method == "POST")
  {
    return handlePost(request);
  }
  if (request.method == "DELETE")
  {
    return handleDelete(request);
  }

  auto response = httpError(405, "only POST and DELETE are supported");
  response.headers.emplace_back("Allow", "POST, DELETE");
  return response;
}

bool Server::isDispatchingToolCall() const
{
  return m_dispatching;
}

std::vector<QString> Server::sessionIds() const
{
  auto result = std::vector<QString>{};
  for (const auto& [id, session] : m_sessions)
  {
    result.push_back(id);
  }
  return result;
}

const Session* Server::session(const QString& id) const
{
  const auto it = m_sessions.find(id);
  return it != m_sessions.end() ? &it->second : nullptr;
}

HttpResponse Server::handlePost(const HttpRequest& request)
{
  const auto contentType = request.header("content-type");
  if (!contentType || mediaType(*contentType) != "application/json")
  {
    return httpError(415, "Content-Type must be application/json");
  }

  if (const auto accept = request.header("accept"); accept && !acceptsJson(*accept))
  {
    return httpError(406, "the Accept header must allow application/json");
  }

  auto parseError = QJsonParseError{};
  const auto document = QJsonDocument::fromJson(request.body, &parseError);
  if (parseError.error != QJsonParseError::NoError)
  {
    return jsonRpcErrorResponse(
      400,
      QJsonValue::Null,
      jsonrpc::ParseError,
      QString{"parse error at offset %1: %2"}
        .arg(parseError.offset)
        .arg(parseError.errorString()));
  }

  const auto value = document.isArray()    ? QJsonValue{document.array()}
                     : document.isObject() ? QJsonValue{document.object()}
                                           : QJsonValue{};
  const auto message = jsonrpc::parse(value);
  if (const auto* invalid = std::get_if<jsonrpc::Invalid>(&message))
  {
    return jsonRpcErrorResponse(400, invalid->id, invalid->code, invalid->message);
  }

  const auto* jsonRpcRequest = std::get_if<jsonrpc::Request>(&message);
  const auto isInitialize = jsonRpcRequest && jsonRpcRequest->method == u"initialize";
  const auto replyId = jsonRpcRequest ? jsonRpcRequest->id : QJsonValue{QJsonValue::Null};

  auto* session = static_cast<Session*>(nullptr);
  if (!isInitialize)
  {
    const auto sessionId = request.header("mcp-session-id");
    if (!sessionId)
    {
      return jsonRpcErrorResponse(
        400,
        replyId,
        jsonrpc::InvalidRequest,
        "missing Mcp-Session-Id header; send initialize first");
    }

    session = findSession(QString::fromUtf8(*sessionId));
    if (!session)
    {
      return jsonRpcErrorResponse(
        404,
        replyId,
        jsonrpc::InvalidRequest,
        "unknown or expired session; send initialize to start a new session");
    }
  }

  if (const auto version = request.header("mcp-protocol-version");
      version && !supportedProtocolVersions().contains(QString::fromUtf8(*version)))
  {
    return jsonRpcErrorResponse(
      400,
      replyId,
      jsonrpc::InvalidRequest,
      QString{"unsupported MCP-Protocol-Version '%1'; supported: %2"}.arg(
        QString::fromUtf8(*version), supportedProtocolVersions().join(", ")));
  }

  if (session)
  {
    session->lastUse = ++m_clock;
  }

  if (isInitialize)
  {
    auto newSessionId = QString{};
    auto response = HttpResponse::json(200, initialize(*jsonRpcRequest, newSessionId));
    response.headers.emplace_back(SessionIdHeader, newSessionId.toUtf8());
    return response;
  }

  if (jsonRpcRequest)
  {
    // the session must not be accessed after this point, since a tool handler may cause
    // it to be removed
    return HttpResponse::json(200, handleRequest(*jsonRpcRequest));
  }

  if (const auto* notification = std::get_if<jsonrpc::Notification>(&message))
  {
    handleNotification(*notification, *session);
  }

  // notifications and responses
  return HttpResponse::empty(202);
}

HttpResponse Server::handleDelete(const HttpRequest& request)
{
  const auto sessionId = request.header("mcp-session-id");
  if (!sessionId || !findSession(QString::fromUtf8(*sessionId)))
  {
    return httpError(404, "unknown or expired session");
  }

  closeSession(QString::fromUtf8(*sessionId));
  return HttpResponse::empty(204);
}

QJsonObject Server::handleRequest(const jsonrpc::Request& request)
{
  if (request.method == u"ping")
  {
    return jsonrpc::makeResult(request.id, QJsonObject{});
  }
  if (request.method == u"tools/list")
  {
    return listTools(request);
  }
  if (request.method == u"tools/call")
  {
    return callTool(request);
  }

  return jsonrpc::makeError(
    request.id,
    jsonrpc::MethodNotFound,
    QString{"method not found: %1"}.arg(request.method));
}

void Server::handleNotification(
  const jsonrpc::Notification& notification, Session& session)
{
  if (notification.method == u"notifications/initialized")
  {
    session.initialized = true;
  }
  // notifications/cancelled and all other notifications are ignored: calls are
  // synchronous, so there is nothing to cancel
}

QJsonObject Server::initialize(const jsonrpc::Request& request, QString& newSessionId)
{
  const auto requestedVersion = request.params.value("protocolVersion").toString();
  const auto protocolVersion = supportedProtocolVersions().contains(requestedVersion)
                                 ? requestedVersion
                                 : QString{LatestProtocolVersion};

  newSessionId = m_makeSessionId();
  m_sessions[newSessionId] = Session{
    .id = newSessionId,
    .protocolVersion = protocolVersion,
    .initialized = false,
    .lastUse = ++m_clock,
  };

  while (m_sessions.size() > MaxSessions)
  {
    const auto lru =
      std::ranges::min_element(m_sessions, [](const auto& lhs, const auto& rhs) {
        return lhs.second.lastUse < rhs.second.lastUse;
      });
    closeSession(lru->first);
  }

  auto result = QJsonObject{
    {"protocolVersion", protocolVersion},
    {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
    {"serverInfo",
     QJsonObject{
       {"name", m_info.name},
       {"title", m_info.title},
       {"version", m_info.version},
     }},
  };
  if (!m_info.instructions.isEmpty())
  {
    result.insert("instructions", m_info.instructions);
  }
  return jsonrpc::makeResult(request.id, std::move(result));
}

QJsonObject Server::listTools(const jsonrpc::Request& request) const
{
  auto tools = QJsonArray{};
  for (const auto& tool : m_tools.tools())
  {
    tools.push_back(QJsonObject{
      {"name", tool.name},
      {"title", tool.title},
      {"description", tool.description},
      {"inputSchema",
       tool.inputSchema.isEmpty() ? QJsonObject{{"type", "object"}} : tool.inputSchema},
      {"annotations",
       QJsonObject{
         {"title", tool.title},
         {"readOnlyHint", tool.annotations.readOnly},
         {"destructiveHint", tool.annotations.destructive},
         {"idempotentHint", tool.annotations.idempotent},
         {"openWorldHint", false},
       }},
    });
  }
  return jsonrpc::makeResult(request.id, QJsonObject{{"tools", tools}});
}

QJsonObject Server::callTool(const jsonrpc::Request& request)
{
  const auto nameValue = request.params.value("name");
  if (!nameValue.isString())
  {
    return jsonrpc::makeError(
      request.id, jsonrpc::InvalidParams, "tools/call requires a string 'name'");
  }

  const auto name = nameValue.toString();
  const auto* tool = m_tools.find(name);
  if (!tool)
  {
    const auto reportedName = name.size() > MaxReportedToolNameLength
                                ? name.left(MaxReportedToolNameLength) + u"..."
                                : name;
    return jsonrpc::makeError(
      request.id,
      jsonrpc::InvalidParams,
      QString{"unknown tool '%1'"}.arg(reportedName),
      QJsonObject{
        {"suggestions",
         suggestToolNames(m_tools, name.left(MaxReportedToolNameLength))}});
  }

  const auto argumentsValue = request.params.value("arguments");
  if (!argumentsValue.isUndefined() && !argumentsValue.isObject())
  {
    return jsonrpc::makeError(
      request.id, jsonrpc::InvalidParams, "tools/call 'arguments' must be an object");
  }

  if (m_dispatching)
  {
    return jsonrpc::makeResult(
      request.id,
      toolErrorToCallResult(ToolError{
        .code = "editor_busy",
        .message = "another tool call is still running",
        .hint = "retry in a moment",
        .details = {{"reason", "reentrant_call"}},
      }));
  }

  // copy the handler in case the registry changes while the handler runs
  const auto handler = tool->handler;
  const auto arguments = argumentsValue.toObject();

  try
  {
    const auto dispatching = kdl::set_temp{m_dispatching};
    return jsonrpc::makeResult(request.id, toolResultToCallResult(handler(arguments)));
  }
  catch (const std::exception& e)
  {
    return jsonrpc::makeError(
      request.id,
      jsonrpc::InternalError,
      QString{"internal error: %1"}.arg(QString::fromUtf8(e.what())));
  }
  catch (...)
  {
    return jsonrpc::makeError(
      request.id, jsonrpc::InternalError, "internal error: unknown exception");
  }
}

Session* Server::findSession(const QString& id)
{
  const auto it = m_sessions.find(id);
  return it != m_sessions.end() ? &it->second : nullptr;
}

void Server::closeSession(const QString id)
{
  if (m_sessions.erase(id) > 0 && sessionClosed)
  {
    sessionClosed(id);
  }
}

} // namespace tb::mcp
