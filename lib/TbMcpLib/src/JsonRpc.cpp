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

#include "mcp/JsonRpc.h"

#include <cmath>

namespace tb::mcp::jsonrpc
{
namespace
{

bool isValidId(const QJsonValue& id)
{
  if (id.isString())
  {
    return true;
  }
  if (id.isDouble())
  {
    // integers up to 2^53 are represented exactly
    constexpr auto maxExactInteger = 9007199254740992.0;
    const auto d = id.toDouble();
    return std::isfinite(d) && std::trunc(d) == d && std::abs(d) <= maxExactInteger;
  }
  return false;
}

} // namespace

Message parse(const QJsonValue& value)
{
  if (value.isArray())
  {
    return Invalid{QJsonValue::Null, InvalidRequest, "batching is not supported"};
  }
  if (!value.isObject())
  {
    return Invalid{QJsonValue::Null, InvalidRequest, "message must be a JSON object"};
  }

  const auto object = value.toObject();
  const auto hasId = object.contains("id");
  const auto id = object.value("id");
  const auto validId = hasId && isValidId(id);
  const auto replyId = validId ? id : QJsonValue{QJsonValue::Null};

  if (object.value("jsonrpc") != QJsonValue{"2.0"})
  {
    return Invalid{replyId, InvalidRequest, R"(jsonrpc must be "2.0")"};
  }

  if (!object.contains("method"))
  {
    if (object.contains("result") || object.contains("error"))
    {
      if (hasId)
      {
        return Response{id};
      }
      return Invalid{QJsonValue::Null, InvalidRequest, "response is missing an id"};
    }
    return Invalid{replyId, InvalidRequest, "message is missing a method"};
  }

  if (hasId && !validId)
  {
    return Invalid{QJsonValue::Null, InvalidRequest, "id must be a string or an integer"};
  }

  const auto method = object.value("method");
  if (!method.isString())
  {
    return Invalid{replyId, InvalidRequest, "method must be a string"};
  }

  auto params = QJsonObject{};
  if (object.contains("params"))
  {
    const auto paramsValue = object.value("params");
    if (paramsValue.isArray())
    {
      return Invalid{replyId, InvalidParams, "params must be an object, not an array"};
    }
    if (!paramsValue.isObject())
    {
      return Invalid{replyId, InvalidRequest, "params must be an object"};
    }
    params = paramsValue.toObject();
  }

  if (hasId)
  {
    return Request{id, method.toString(), std::move(params)};
  }
  return Notification{method.toString(), std::move(params)};
}

QJsonObject makeResult(const QJsonValue& id, QJsonObject result)
{
  return QJsonObject{
    {"jsonrpc", "2.0"},
    {"id", id},
    {"result", std::move(result)},
  };
}

QJsonObject makeError(
  const QJsonValue& id,
  const ErrorCode code,
  const QString& message,
  const QJsonValue& data)
{
  auto error = QJsonObject{
    {"code", static_cast<int>(code)},
    {"message", message},
  };
  if (!data.isNull() && !data.isUndefined())
  {
    error.insert("data", data);
  }

  return QJsonObject{
    {"jsonrpc", "2.0"},
    {"id", id},
    {"error", std::move(error)},
  };
}

} // namespace tb::mcp::jsonrpc
