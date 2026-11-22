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

#include "mcp/Http.h"

#include <QJsonDocument>

#include "kd/contracts.h"

#include <algorithm>
#include <limits>

namespace tb::mcp
{
namespace
{

bool isTokenChar(const char c)
{
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
  {
    return true;
  }
  return QByteArrayView{"!#$%&'*+-.^_`|~"}.contains(c);
}

bool isToken(const QByteArrayView str)
{
  return !str.isEmpty() && std::ranges::all_of(str, isTokenChar);
}

bool isControlChar(const char c)
{
  const auto u = static_cast<unsigned char>(c);
  return u < 0x20 || u == 0x7f;
}

bool isDigit(const char c)
{
  return c >= '0' && c <= '9';
}

bool hasConnectionToken(const HttpRequest& request, const QByteArrayView token)
{
  for (const auto& [name, value] : request.headers)
  {
    if (name == "connection")
    {
      for (const auto& part : value.split(','))
      {
        if (part.trimmed().compare(token, Qt::CaseInsensitive) == 0)
        {
          return true;
        }
      }
    }
  }
  return false;
}

/**
 * Parses a decimal Content-Length value. Returns nullopt if the value is malformed or
 * does not fit into a qint64.
 */
std::optional<qint64> parseContentLength(const QByteArrayView str)
{
  if (str.isEmpty() || !std::ranges::all_of(str, isDigit))
  {
    return std::nullopt;
  }

  constexpr auto max = std::numeric_limits<qint64>::max();
  auto result = qint64{0};
  for (const auto c : str)
  {
    const auto digit = qint64{c - '0'};
    if (result > (max - digit) / 10)
    {
      return std::nullopt;
    }
    result = result * 10 + digit;
  }
  return result;
}

bool hasNoContentLength(const int status)
{
  return (status >= 100 && status < 200) || status == 204 || status == 304;
}

} // namespace

std::optional<QByteArray> HttpRequest::header(const QByteArrayView lowerName) const
{
  const auto it = std::ranges::find_if(
    headers, [&](const auto& header) { return header.first == lowerName; });
  return it != headers.end() ? std::optional{it->second} : std::nullopt;
}

bool HttpRequest::keepAlive() const
{
  if (version == "HTTP/1.0")
  {
    return hasConnectionToken(*this, "keep-alive");
  }
  return !hasConnectionToken(*this, "close");
}

QByteArray HttpResponse::serialize(const bool keepAlive, const bool headRequest) const
{
  auto result = QByteArray{};
  result += "HTTP/1.1 ";
  result += QByteArray::number(status);
  result += ' ';
  result += reasonPhrase(status);
  result += "\r\n";

  for (const auto& [name, value] : headers)
  {
    if (
      name.compare("content-length", Qt::CaseInsensitive) == 0
      || name.compare("connection", Qt::CaseInsensitive) == 0)
    {
      continue;
    }
    result += name;
    result += ": ";
    result += value;
    result += "\r\n";
  }

  const auto hasBody = !hasNoContentLength(status);
  if (hasBody)
  {
    result += "Content-Length: ";
    result += QByteArray::number(body.size());
    result += "\r\n";
  }
  result += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  result += "Cache-Control: no-store\r\n";
  result += "\r\n";

  if (hasBody && !headRequest)
  {
    result += body;
  }
  return result;
}

std::optional<QByteArray> HttpResponse::header(const QByteArrayView name) const
{
  const auto it = std::ranges::find_if(headers, [&](const auto& header) {
    return header.first.compare(name, Qt::CaseInsensitive) == 0;
  });
  return it != headers.end() ? std::optional{it->second} : std::nullopt;
}

HttpResponse HttpResponse::json(const int status, const QJsonObject& body)
{
  return HttpResponse{
    .status = status,
    .headers = {{"Content-Type", "application/json"}},
    .body = QJsonDocument{body}.toJson(QJsonDocument::Compact),
  };
}

HttpResponse HttpResponse::empty(const int status)
{
  return HttpResponse{.status = status, .headers = {}, .body = {}};
}

QByteArray reasonPhrase(const int status)
{
  switch (status)
  {
  case 200:
    return "OK";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 406:
    return "Not Acceptable";
  case 408:
    return "Request Timeout";
  case 411:
    return "Length Required";
  case 413:
    return "Content Too Large";
  case 415:
    return "Unsupported Media Type";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 503:
    return "Service Unavailable";
  case 505:
    return "HTTP Version Not Supported";
  default:
    return "Unknown";
  }
}

HttpRequestParser::HttpRequestParser(const Limits limits)
  : m_limits{limits}
{
}

HttpRequestParser::State HttpRequestParser::feed(const QByteArrayView data)
{
  if (m_state != State::Error)
  {
    m_buffer.append(data);
    parse();
  }
  return m_state;
}

HttpRequestParser::State HttpRequestParser::state() const
{
  return m_state;
}

HttpRequest HttpRequestParser::takeRequest()
{
  contract_pre(m_state == State::Complete);

  auto request = std::move(m_request);
  m_buffer.remove(0, m_bodyStart + m_contentLength);

  m_request = HttpRequest{};
  m_state = State::NeedMore;
  m_searchFrom = 0;
  m_headParsed = false;
  m_bodyStart = 0;
  m_contentLength = 0;

  parse();
  return request;
}

int HttpRequestParser::errorStatus() const
{
  return m_state == State::Error ? m_errorStatus : 0;
}

void HttpRequestParser::parse()
{
  if (m_state != State::NeedMore)
  {
    return;
  }

  if (!m_headParsed)
  {
    // RFC 9112 2.2: ignore empty lines preceding the request line
    auto skipped = qsizetype{0};
    while (skipped < m_buffer.size())
    {
      if (m_buffer.at(skipped) == '\n')
      {
        skipped += 1;
      }
      else if (
        m_buffer.at(skipped) == '\r' && skipped + 1 < m_buffer.size()
        && m_buffer.at(skipped + 1) == '\n')
      {
        skipped += 2;
      }
      else
      {
        break;
      }
    }
    if (skipped > 0)
    {
      m_buffer.remove(0, skipped);
      m_searchFrom = 0;
    }

    const auto lfLf = m_buffer.indexOf("\n\n", m_searchFrom);
    const auto lfCrLf = m_buffer.indexOf("\n\r\n", m_searchFrom);

    auto headEnd = qsizetype{-1};
    auto bodyStart = qsizetype{-1};
    if (lfLf >= 0 && (lfCrLf < 0 || lfLf < lfCrLf))
    {
      headEnd = lfLf + 1;
      bodyStart = lfLf + 2;
    }
    else if (lfCrLf >= 0)
    {
      headEnd = lfCrLf + 1;
      bodyStart = lfCrLf + 3;
    }

    if (bodyStart < 0)
    {
      if (m_buffer.size() > m_limits.maxHeaderBytes)
      {
        fail(431);
      }
      else
      {
        m_searchFrom = std::max(qsizetype{0}, m_buffer.size() - 2);
      }
      return;
    }

    if (bodyStart > m_limits.maxHeaderBytes)
    {
      fail(431);
      return;
    }

    if (!parseHead(QByteArrayView{m_buffer}.first(headEnd)))
    {
      return;
    }

    m_headParsed = true;
    m_bodyStart = bodyStart;
  }

  if (m_buffer.size() - m_bodyStart >= m_contentLength)
  {
    m_request.body = m_buffer.mid(m_bodyStart, m_contentLength);
    m_state = State::Complete;
  }
}

void HttpRequestParser::fail(const int status)
{
  m_state = State::Error;
  m_errorStatus = status;
  m_buffer.clear();
}

bool HttpRequestParser::parseHead(const QByteArrayView head)
{
  auto lines = std::vector<QByteArrayView>{};
  auto lineStart = qsizetype{0};
  while (lineStart < head.size())
  {
    auto lineEnd = head.indexOf('\n', lineStart);
    if (lineEnd < 0)
    {
      lineEnd = head.size();
    }
    auto line = head.sliced(lineStart, lineEnd - lineStart);
    if (line.endsWith('\r'))
    {
      line.chop(1);
    }
    lines.push_back(line);
    lineStart = lineEnd + 1;
  }

  if (lines.empty())
  {
    fail(400);
    return false;
  }

  // request line
  const auto requestLine = QByteArray{lines.front().data(), lines.front().size()};
  const auto parts = requestLine.split(' ');
  if (parts.size() != 3 || !isToken(parts[0]) || parts[1].isEmpty())
  {
    fail(400);
    return false;
  }
  if (std::ranges::any_of(parts[1], isControlChar))
  {
    fail(400);
    return false;
  }

  const auto& version = parts[2];
  if (
    version.size() != 8 || !version.startsWith("HTTP/") || !isDigit(version[5])
    || version[6] != '.' || !isDigit(version[7]))
  {
    fail(400);
    return false;
  }
  if (version[5] != '1')
  {
    fail(505);
    return false;
  }

  m_request.method = parts[0];
  m_request.target = parts[1];
  m_request.version = version;

  // header fields
  for (auto i = size_t{1}; i < lines.size(); ++i)
  {
    const auto line = lines[i];
    if (line.isEmpty())
    {
      continue;
    }

    // obsolete line folding is rejected (RFC 9112 5.2)
    if (line.front() == ' ' || line.front() == '\t')
    {
      fail(400);
      return false;
    }

    const auto colon = line.indexOf(':');
    if (colon <= 0 || !isToken(line.first(colon)))
    {
      fail(400);
      return false;
    }

    const auto value = line.sliced(colon + 1).trimmed();
    if (std::ranges::any_of(
          value, [](const char c) { return c != '\t' && isControlChar(c); }))
    {
      fail(400);
      return false;
    }

    m_request.headers.emplace_back(
      QByteArray{line.first(colon).data(), colon}.toLower(),
      QByteArray{value.data(), value.size()});
  }

  if (m_request.header("transfer-encoding"))
  {
    fail(501);
    return false;
  }

  auto contentLength = std::optional<qint64>{};
  for (const auto& [name, value] : m_request.headers)
  {
    if (name == "content-length")
    {
      const auto length = parseContentLength(value);
      if (!length || (contentLength && *contentLength != *length))
      {
        fail(400);
        return false;
      }
      contentLength = length;
    }
  }

  if (!contentLength)
  {
    if (m_request.method == "POST")
    {
      fail(411);
      return false;
    }
    contentLength = 0;
  }

  if (*contentLength > m_limits.maxBodyBytes)
  {
    fail(413);
    return false;
  }

  m_contentLength = static_cast<qsizetype>(*contentLength);
  return true;
}

} // namespace tb::mcp
