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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <string>

#include <catch2/catch_tostring.hpp>

namespace tb::mcp
{

/** Parses the given JSON text, which must be an object or an array. */
inline QJsonValue json(const char* text)
{
  const auto document = QJsonDocument::fromJson(QByteArray{text});
  return document.isArray() ? QJsonValue{document.array()}
                            : QJsonValue{document.object()};
}

/** Parses the given JSON object text. */
inline QJsonObject jsonObject(const char* text)
{
  return QJsonDocument::fromJson(QByteArray{text}).object();
}

inline QByteArray compact(const QJsonObject& object)
{
  return QJsonDocument{object}.toJson(QJsonDocument::Compact);
}

} // namespace tb::mcp

namespace Catch
{

template <>
struct StringMaker<QString>
{
  static std::string convert(const QString& str) { return '"' + str.toStdString() + '"'; }
};

template <>
struct StringMaker<QByteArray>
{
  static std::string convert(const QByteArray& str)
  {
    return '"' + str.toStdString() + '"';
  }
};

template <>
struct StringMaker<QJsonObject>
{
  static std::string convert(const QJsonObject& object)
  {
    return QJsonDocument{object}.toJson(QJsonDocument::Compact).toStdString();
  }
};

template <>
struct StringMaker<QJsonArray>
{
  static std::string convert(const QJsonArray& array)
  {
    return QJsonDocument{array}.toJson(QJsonDocument::Compact).toStdString();
  }
};

template <>
struct StringMaker<QJsonValue>
{
  static std::string convert(const QJsonValue& value)
  {
    if (value.isObject())
    {
      return StringMaker<QJsonObject>::convert(value.toObject());
    }
    if (value.isArray())
    {
      return StringMaker<QJsonArray>::convert(value.toArray());
    }
    if (value.isUndefined())
    {
      return "undefined";
    }
    // wrap scalars in an array to serialize them
    const auto text =
      QJsonDocument{QJsonArray{value}}.toJson(QJsonDocument::Compact).toStdString();
    return text.substr(1, text.size() - 2);
  }
};

} // namespace Catch
