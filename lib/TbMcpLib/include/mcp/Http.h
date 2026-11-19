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

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>

#include <optional>
#include <utility>
#include <vector>

namespace tb::mcp
{

/**
 * A parsed HTTP/1.x request.
 */
struct HttpRequest
{
  QByteArray method;
  QByteArray target;
  QByteArray version;

  /** Header names are stored in lower case, values are trimmed. */
  std::vector<std::pair<QByteArray, QByteArray>> headers;

  QByteArray body;

  /**
   * Returns the value of the first header with the given (lower case) name.
   */
  std::optional<QByteArray> header(QByteArrayView lowerName) const;

  /**
   * HTTP/1.1 connections are persistent unless a "Connection: close" header is present.
   * HTTP/1.0 connections are persistent only with "Connection: keep-alive".
   */
  bool keepAlive() const;
};

/**
 * An HTTP/1.1 response.
 */
struct HttpResponse
{
  int status = 200;
  std::vector<std::pair<QByteArray, QByteArray>> headers;
  QByteArray body;

  /**
   * Serializes the status line (with reason phrase), the headers, Content-Length (except
   * for 1xx, 204 and 304 responses), Connection and Cache-Control: no-store, followed by
   * the body.
   *
   * For a response to a HEAD request, pass headRequest = true: the Content-Length of the
   * body is still sent, but the body itself is not (RFC 9110 9.3.2). Otherwise a client
   * that keeps the connection open would read the body as the start of the next
   * response.
   */
  QByteArray serialize(bool keepAlive, bool headRequest = false) const;

  /**
   * Returns the value of the first header with the given name (case-insensitive).
   */
  std::optional<QByteArray> header(QByteArrayView name) const;

  /**
   * Creates a response with a compact JSON body and Content-Type: application/json.
   */
  static HttpResponse json(int status, const QJsonObject& body);

  /**
   * Creates a response without a body.
   */
  static HttpResponse empty(int status);
};

/**
 * Returns the reason phrase for the given status code, e.g. "Not Found" for 404.
 */
QByteArray reasonPhrase(int status);

/**
 * Size limits applied by HttpRequestParser.
 */
struct HttpRequestLimits
{
  qsizetype maxHeaderBytes = 32 * 1024;
  qsizetype maxBodyBytes = 16 * 1024 * 1024;
};

/**
 * An incremental HTTP/1.x request parser.
 *
 * Bytes are fed as they arrive. Once a request is complete, it can be taken from the
 * parser. Bytes following the request (pipelining) are kept and parsed when the request
 * is taken.
 *
 * Lines must end with CRLF, but a bare LF is tolerated. Chunked transfer encoding is not
 * supported.
 */
class HttpRequestParser
{
public:
  using Limits = HttpRequestLimits;

  enum class State
  {
    NeedMore,
    Complete,
    Error,
  };

  explicit HttpRequestParser(Limits limits = {});

  /**
   * Appends the given data and parses as far as possible. Once the parser is in the
   * Error state, all further data is ignored.
   */
  State feed(QByteArrayView data);

  State state() const;

  /**
   * Returns the completed request and starts parsing the remaining bytes.
   *
   * Precondition: state() == State::Complete
   */
  HttpRequest takeRequest();

  /**
   * Returns the HTTP status code describing the error if state() == State::Error, and 0
   * otherwise:
   *
   * - 400 for a malformed request line, header or Content-Length
   * - 411 for a POST request without Content-Length
   * - 413 if the body exceeds the limit
   * - 431 if the header section exceeds the limit
   * - 501 if a Transfer-Encoding header is present
   * - 505 if the version is not HTTP/1.x
   */
  int errorStatus() const;

private:
  void parse();
  void fail(int status);
  bool parseHead(QByteArrayView head);

  Limits m_limits;
  State m_state = State::NeedMore;
  int m_errorStatus = 0;

  QByteArray m_buffer;
  qsizetype m_searchFrom = 0;

  bool m_headParsed = false;
  qsizetype m_bodyStart = 0;
  qsizetype m_contentLength = 0;
  HttpRequest m_request;
};

} // namespace tb::mcp
