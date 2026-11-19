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

#include <variant>

namespace tb::mcp::jsonrpc
{

enum ErrorCode : int
{
  ParseError = -32700,
  InvalidRequest = -32600,
  MethodNotFound = -32601,
  InvalidParams = -32602,
  InternalError = -32603,
};

/** A request, which must be answered with a result or an error. */
struct Request
{
  QJsonValue id;
  QString method;
  QJsonObject params;
};

/** A notification, which must not be answered. */
struct Notification
{
  QString method;
  QJsonObject params;
};

/** A client's response to a server request. It is accepted and ignored. */
struct Response
{
  QJsonValue id;
};

/**
 * A message that is not a valid JSON-RPC 2.0 message. The id is the message's id if it
 * could be determined and null otherwise.
 */
struct Invalid
{
  QJsonValue id;
  ErrorCode code;
  QString message;
};

using Message = std::variant<Request, Notification, Response, Invalid>;

/**
 * Classifies the given JSON value as a JSON-RPC 2.0 message.
 *
 * - jsonrpc must be "2.0"
 * - id must be a string or an integer valued number (null, fractional numbers and
 *   booleans are invalid)
 * - method must be a string
 * - params must be absent or an object
 * - an object without a method that has a result or an error and an id is a response
 * - arrays (batches) are not supported
 */
Message parse(const QJsonValue& value);

/** Returns {"jsonrpc":"2.0","id":id,"result":result}. */
QJsonObject makeResult(const QJsonValue& id, QJsonObject result);

/**
 * Returns {"jsonrpc":"2.0","id":id,"error":{"code":code,"message":message,"data":data}}.
 * The data member is omitted if data is null or undefined.
 */
QJsonObject makeError(
  const QJsonValue& id,
  ErrorCode code,
  const QString& message,
  const QJsonValue& data = {});

} // namespace tb::mcp::jsonrpc
