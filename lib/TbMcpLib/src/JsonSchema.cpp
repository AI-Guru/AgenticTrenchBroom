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

#include "mcp/JsonSchema.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>

namespace tb::mcp
{
namespace
{

constexpr auto TypeNames = std::array<QStringView, 7>{
  u"object", u"array", u"string", u"number", u"integer", u"boolean", u"null"};

bool isTypeName(const QJsonValue& value)
{
  return value.isString()
         && std::ranges::find(TypeNames, value.toString()) != TypeNames.end();
}

bool isIntegral(const QJsonValue& value)
{
  if (!value.isDouble())
  {
    return false;
  }
  const auto d = value.toDouble();
  return std::isfinite(d) && std::trunc(d) == d;
}

bool isNonNegativeInteger(const QJsonValue& value)
{
  return isIntegral(value) && value.toDouble() >= 0.0;
}

QString escapePointerToken(QString token)
{
  return token.replace(QStringLiteral("~"), QStringLiteral("~0"))
    .replace(QStringLiteral("/"), QStringLiteral("~1"));
}

QString childPointer(const QString& pointer, const QString& token)
{
  return pointer + u'/' + escapePointerToken(token);
}

QString childPointer(const QString& pointer, const qsizetype index)
{
  return pointer + u'/' + QString::number(index);
}

QString typeName(const QJsonValue& value)
{
  switch (value.type())
  {
  case QJsonValue::Null:
    return "null";
  case QJsonValue::Bool:
    return "boolean";
  case QJsonValue::Double:
    return "number";
  case QJsonValue::String:
    return "string";
  case QJsonValue::Array:
    return "array";
  case QJsonValue::Object:
    return "object";
  case QJsonValue::Undefined:
    break;
  }
  return "nothing";
}

bool matchesType(const QJsonValue& value, const QString& type)
{
  if (type == u"integer")
  {
    return isIntegral(value);
  }
  return typeName(value) == type;
}

QString display(const QJsonValue& value)
{
  switch (value.type())
  {
  case QJsonValue::Null:
    return "null";
  case QJsonValue::Bool:
    return value.toBool() ? "true" : "false";
  case QJsonValue::Double:
    if (isIntegral(value) && std::abs(value.toDouble()) < 1e15)
    {
      return QString::number(static_cast<qint64>(value.toDouble()));
    }
    return QString::number(value.toDouble(), 'g', 17);
  case QJsonValue::String:
    return value.toString();
  case QJsonValue::Array:
    return QString::fromUtf8(
      QJsonDocument{value.toArray()}.toJson(QJsonDocument::Compact));
  case QJsonValue::Object:
    return QString::fromUtf8(
      QJsonDocument{value.toObject()}.toJson(QJsonDocument::Compact));
  case QJsonValue::Undefined:
    break;
  }
  return "nothing";
}

qsizetype codePointCount(const QString& str)
{
  return std::ranges::count_if(str, [](const QChar c) { return !c.isLowSurrogate(); });
}

SchemaViolation violation(QString pointer, QString message, QJsonObject details = {})
{
  return SchemaViolation{std::move(pointer), std::move(message), std::move(details)};
}

std::optional<SchemaViolation> validateAt(
  const QJsonObject& schema, const QJsonValue& value, const QString& pointer);

std::optional<SchemaViolation> validateType(
  const QJsonObject& schema, const QJsonValue& value, const QString& pointer)
{
  if (!schema.contains("type"))
  {
    return std::nullopt;
  }

  const auto typeValue = schema.value("type");
  auto types = QStringList{};
  if (typeValue.isArray())
  {
    for (const auto& type : typeValue.toArray())
    {
      types.push_back(type.toString());
    }
  }
  else
  {
    types.push_back(typeValue.toString());
  }

  if (std::ranges::any_of(
        types, [&](const auto& type) { return matchesType(value, type); }))
  {
    return std::nullopt;
  }

  return violation(
    pointer,
    QString{"expected %1, got %2"}.arg(types.join(" or "), typeName(value)),
    {{"expected", QJsonArray::fromStringList(types)}, {"actual", typeName(value)}});
}

std::optional<SchemaViolation> validateConstAndEnum(
  const QJsonObject& schema, const QJsonValue& value, const QString& pointer)
{
  if (schema.contains("const") && schema.value("const") != value)
  {
    return violation(
      pointer,
      QString{"must be %1"}.arg(display(schema.value("const"))),
      {{"expected", schema.value("const")}});
  }

  if (schema.contains("enum"))
  {
    const auto allowed = schema.value("enum").toArray();
    if (std::ranges::find(allowed, value) == allowed.end())
    {
      auto allowedStrings = QStringList{};
      for (const auto& a : allowed)
      {
        allowedStrings.push_back(display(a));
      }
      return violation(
        pointer,
        QString{"must be one of: %1"}.arg(allowedStrings.join(", ")),
        {{"allowed", allowed}});
    }
  }

  return std::nullopt;
}

std::optional<SchemaViolation> validateObject(
  const QJsonObject& schema, const QJsonObject& object, const QString& pointer)
{
  const auto properties = schema.value("properties").toObject();

  for (const auto& required : schema.value("required").toArray())
  {
    const auto name = required.toString();
    if (!object.contains(name))
    {
      return violation(
        childPointer(pointer, name),
        QString{"missing required argument '%1'"}.arg(name),
        {{"missing", name}});
    }
  }

  if (schema.value("additionalProperties") == QJsonValue{false})
  {
    for (const auto& key : object.keys())
    {
      if (!properties.contains(key))
      {
        const auto allowed = properties.keys();
        return violation(
          childPointer(pointer, key),
          QString{"unknown argument '%1'; allowed: %2"}.arg(
            key, allowed.isEmpty() ? QString{"(none)"} : allowed.join(", ")),
          {{"allowed", QJsonArray::fromStringList(allowed)}});
      }
    }
  }

  for (const auto& key : object.keys())
  {
    if (properties.contains(key))
    {
      if (
        auto result = validateAt(
          properties.value(key).toObject(),
          object.value(key),
          childPointer(pointer, key)))
      {
        return result;
      }
    }
  }

  return std::nullopt;
}

std::optional<SchemaViolation> validateArray(
  const QJsonObject& schema, const QJsonArray& array, const QString& pointer)
{
  const auto size = static_cast<double>(array.size());
  if (schema.contains("minItems") && size < schema.value("minItems").toDouble())
  {
    return violation(
      pointer,
      QString{"must have at least %1 items, got %2"}.arg(
        display(schema.value("minItems")), QString::number(array.size())),
      {{"minItems", schema.value("minItems")}, {"actual", array.size()}});
  }
  if (schema.contains("maxItems") && size > schema.value("maxItems").toDouble())
  {
    return violation(
      pointer,
      QString{"must have at most %1 items, got %2"}.arg(
        display(schema.value("maxItems")), QString::number(array.size())),
      {{"maxItems", schema.value("maxItems")}, {"actual", array.size()}});
  }

  if (schema.contains("items"))
  {
    const auto items = schema.value("items").toObject();
    for (auto i = qsizetype{0}; i < array.size(); ++i)
    {
      if (auto result = validateAt(items, array.at(i), childPointer(pointer, i)))
      {
        return result;
      }
    }
  }

  return std::nullopt;
}

std::optional<SchemaViolation> validateString(
  const QJsonObject& schema, const QString& str, const QString& pointer)
{
  const auto length = static_cast<double>(codePointCount(str));
  if (schema.contains("minLength") && length < schema.value("minLength").toDouble())
  {
    return violation(
      pointer,
      QString{"must be at least %1 characters long"}.arg(
        display(schema.value("minLength"))),
      {{"minLength", schema.value("minLength")}});
  }
  if (schema.contains("maxLength") && length > schema.value("maxLength").toDouble())
  {
    return violation(
      pointer,
      QString{"must be at most %1 characters long"}.arg(
        display(schema.value("maxLength"))),
      {{"maxLength", schema.value("maxLength")}});
  }

  if (schema.contains("pattern"))
  {
    const auto pattern = schema.value("pattern").toString();
    const auto regex = QRegularExpression{pattern};
    if (!regex.isValid())
    {
      return violation(
        pointer,
        QString{"schema pattern '%1' is invalid"}.arg(pattern),
        {{"pattern", pattern}});
    }
    if (!regex.match(str).hasMatch())
    {
      return violation(
        pointer,
        QString{"must match the pattern %1"}.arg(pattern),
        {{"pattern", pattern}});
    }
  }

  return std::nullopt;
}

std::optional<SchemaViolation> validateNumber(
  const QJsonObject& schema, const double number, const QString& pointer)
{
  if (!std::isfinite(number))
  {
    return violation(pointer, "must be a finite number");
  }
  if (schema.contains("minimum") && number < schema.value("minimum").toDouble())
  {
    return violation(
      pointer,
      QString{"must be >= %1"}.arg(display(schema.value("minimum"))),
      {{"minimum", schema.value("minimum")}});
  }
  if (schema.contains("maximum") && number > schema.value("maximum").toDouble())
  {
    return violation(
      pointer,
      QString{"must be <= %1"}.arg(display(schema.value("maximum"))),
      {{"maximum", schema.value("maximum")}});
  }
  return std::nullopt;
}

std::optional<SchemaViolation> validateOneOf(
  const QJsonObject& schema, const QJsonValue& value, const QString& pointer)
{
  if (!schema.contains("oneOf"))
  {
    return std::nullopt;
  }

  const auto alternatives = schema.value("oneOf").toArray();
  auto matching = 0;
  auto failures = QJsonArray{};
  for (const auto& alternative : alternatives)
  {
    if (const auto result = validateAt(alternative.toObject(), value, pointer))
    {
      failures.push_back(QJsonObject{
        {"pointer", result->pointer},
        {"message", result->message},
      });
    }
    else
    {
      ++matching;
    }
  }

  if (matching == 0)
  {
    auto messages = QStringList{};
    for (const auto& failure : failures)
    {
      messages.push_back(failure.toObject().value("message").toString());
    }
    return violation(
      pointer,
      QString{"does not match any of the allowed alternatives (%1)"}.arg(
        messages.join("; ")),
      {{"alternatives", failures}});
  }
  if (matching > 1)
  {
    return violation(
      pointer,
      QString{"matches %1 alternatives, but must match exactly one"}.arg(matching),
      {{"matching", matching}});
  }
  return std::nullopt;
}

std::optional<SchemaViolation> validateAt(
  const QJsonObject& schema, const QJsonValue& value, const QString& pointer)
{
  if (auto result = validateType(schema, value, pointer))
  {
    return result;
  }
  if (auto result = validateConstAndEnum(schema, value, pointer))
  {
    return result;
  }

  auto result = std::optional<SchemaViolation>{};
  if (value.isObject())
  {
    result = validateObject(schema, value.toObject(), pointer);
  }
  else if (value.isArray())
  {
    result = validateArray(schema, value.toArray(), pointer);
  }
  else if (value.isString())
  {
    result = validateString(schema, value.toString(), pointer);
  }
  else if (value.isDouble())
  {
    result = validateNumber(schema, value.toDouble(), pointer);
  }

  if (result)
  {
    return result;
  }
  return validateOneOf(schema, value, pointer);
}

bool fail(QString* offending, const QString& pointer)
{
  if (offending)
  {
    *offending = pointer;
  }
  return false;
}

bool checkKeywords(const QJsonObject& schema, const QString& path, QString* offending)
{
  for (auto it = schema.begin(); it != schema.end(); ++it)
  {
    const auto key = it.key();
    const auto value = it.value();
    const auto keyPath = childPointer(path, key);

    if (key == u"type")
    {
      if (value.isArray())
      {
        const auto types = value.toArray();
        if (types.isEmpty() || !std::ranges::all_of(types, isTypeName))
        {
          return fail(offending, keyPath);
        }
      }
      else if (!isTypeName(value))
      {
        return fail(offending, keyPath);
      }
    }
    else if (key == u"properties")
    {
      if (!value.isObject())
      {
        return fail(offending, keyPath);
      }
      const auto properties = value.toObject();
      for (auto pit = properties.begin(); pit != properties.end(); ++pit)
      {
        const auto propertyPath = childPointer(keyPath, pit.key());
        if (!pit.value().isObject())
        {
          return fail(offending, propertyPath);
        }
        if (!checkKeywords(pit.value().toObject(), propertyPath, offending))
        {
          return false;
        }
      }
    }
    else if (key == u"required")
    {
      if (!value.isArray() || !std::ranges::all_of(value.toArray(), [](const auto& name) {
            return name.isString();
          }))
      {
        return fail(offending, keyPath);
      }
    }
    else if (key == u"additionalProperties")
    {
      if (value != QJsonValue{false})
      {
        return fail(offending, keyPath);
      }
    }
    else if (key == u"items")
    {
      if (!value.isObject())
      {
        return fail(offending, keyPath);
      }
      if (!checkKeywords(value.toObject(), keyPath, offending))
      {
        return false;
      }
    }
    else if (
      key == u"minItems" || key == u"maxItems" || key == u"minLength"
      || key == u"maxLength")
    {
      if (!isNonNegativeInteger(value))
      {
        return fail(offending, keyPath);
      }
    }
    else if (key == u"minimum" || key == u"maximum")
    {
      if (!value.isDouble())
      {
        return fail(offending, keyPath);
      }
    }
    else if (key == u"enum")
    {
      if (!value.isArray() || value.toArray().isEmpty())
      {
        return fail(offending, keyPath);
      }
    }
    else if (key == u"pattern")
    {
      if (!value.isString() || !QRegularExpression{value.toString()}.isValid())
      {
        return fail(offending, keyPath);
      }
    }
    else if (key == u"oneOf")
    {
      if (!value.isArray() || value.toArray().isEmpty())
      {
        return fail(offending, keyPath);
      }
      const auto alternatives = value.toArray();
      for (auto i = qsizetype{0}; i < alternatives.size(); ++i)
      {
        const auto alternativePath = childPointer(keyPath, i);
        if (!alternatives.at(i).isObject())
        {
          return fail(offending, alternativePath);
        }
        if (!checkKeywords(alternatives.at(i).toObject(), alternativePath, offending))
        {
          return false;
        }
      }
    }
    else if (key == u"description" || key == u"title")
    {
      if (!value.isString())
      {
        return fail(offending, keyPath);
      }
    }
    else if (key != u"const" && key != u"default")
    {
      return fail(offending, keyPath);
    }
  }
  return true;
}

} // namespace

std::optional<SchemaViolation> validate(
  const QJsonObject& schema, const QJsonValue& value)
{
  return validateAt(schema, value, QString{});
}

bool usesOnlySupportedKeywords(const QJsonObject& schema, QString* offending)
{
  return checkKeywords(schema, QString{}, offending);
}

} // namespace tb::mcp
