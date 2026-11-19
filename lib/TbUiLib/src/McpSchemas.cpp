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

#include "ui/McpSchemas.h"

#include <QJsonArray>

namespace tb::ui::schema
{
namespace
{

QJsonObject withDescription(QJsonObject schema, const QString& description)
{
  if (!description.isEmpty())
  {
    schema["description"] = description;
  }
  return schema;
}

QJsonObject numeric(
  const char* type,
  const std::optional<double> minimum,
  const std::optional<double> maximum,
  const QString& description)
{
  auto result = QJsonObject{{"type", type}};
  if (minimum)
  {
    result["minimum"] = *minimum;
  }
  if (maximum)
  {
    result["maximum"] = *maximum;
  }
  return withDescription(std::move(result), description);
}

QJsonObject numberArray(const int size, const QString& description)
{
  return withDescription(
    {
      {"type", "array"},
      {"items", QJsonObject{{"type", "number"}}},
      {"minItems", size},
      {"maxItems", size},
    },
    description);
}

} // namespace

QJsonObject object(QJsonObject properties, const QStringList& required)
{
  auto result = QJsonObject{
    {"type", "object"},
    {"properties", std::move(properties)},
    {"additionalProperties", false},
  };
  if (!required.isEmpty())
  {
    result["required"] = QJsonArray::fromStringList(required);
  }
  return result;
}

QJsonObject describe(QJsonObject schema, const QString& description)
{
  return withDescription(std::move(schema), description);
}

QJsonObject boolean(const QString& description)
{
  return withDescription({{"type", "boolean"}}, description);
}

QJsonObject number(
  const std::optional<double> minimum,
  const std::optional<double> maximum,
  const QString& description)
{
  return numeric("number", minimum, maximum, description);
}

QJsonObject integer(
  const std::optional<double> minimum,
  const std::optional<double> maximum,
  const QString& description)
{
  return numeric("integer", minimum, maximum, description);
}

QJsonObject string(
  const std::optional<int> minLength,
  const std::optional<int> maxLength,
  const QString& description)
{
  auto result = QJsonObject{{"type", "string"}};
  if (minLength)
  {
    result["minLength"] = *minLength;
  }
  if (maxLength)
  {
    result["maxLength"] = *maxLength;
  }
  return withDescription(std::move(result), description);
}

QJsonObject stringEnum(const QStringList& values, const QString& description)
{
  return withDescription(
    {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}}, description);
}

QJsonObject array(
  QJsonObject items,
  const std::optional<int> minItems,
  const std::optional<int> maxItems,
  const QString& description)
{
  auto result = QJsonObject{{"type", "array"}, {"items", std::move(items)}};
  if (minItems)
  {
    result["minItems"] = *minItems;
  }
  if (maxItems)
  {
    result["maxItems"] = *maxItems;
  }
  return withDescription(std::move(result), description);
}

QJsonObject vec3(const QString& description)
{
  return numberArray(3, description);
}

QJsonObject vec2(const QString& description)
{
  return numberArray(2, description);
}

QJsonObject bounds(const QString& description)
{
  return withDescription(
    object({{"min", vec3()}, {"max", vec3()}}, {"min", "max"}), description);
}

QJsonObject nodeId(const QString& description)
{
  return withDescription(
    {{"type", "string"}, {"pattern", NodeIdPattern}},
    description.isEmpty() ? QStringLiteral("a node id such as B12, E3, G2, L1 or world")
                          : description);
}

QJsonObject nodeIds(const int maxItems, const QString& description)
{
  return array(
    {{"type", "string"}, {"pattern", NodeIdPattern}},
    1,
    maxItems,
    description.isEmpty()
      ? QStringLiteral("node ids; \"@selection\" expands to the user's selection")
      : description);
}

QJsonObject side(const QString& description)
{
  return stringEnum({"+x", "-x", "+y", "-y", "+z", "-z"}, description);
}

QJsonObject axis(const QString& description)
{
  return withDescription(
    {{"oneOf", QJsonArray{stringEnum({"x", "y", "z"}), vec3()}}}, description);
}

QJsonObject faceRef(const QString& description)
{
  return withDescription(
    object(
      {
        {"brush", nodeId()},
        {"index", integer(0)},
        {"side", side()},
        {"normal", vec3()},
      },
      {"brush"}),
    description.isEmpty()
      ? QStringLiteral("a brush face given by exactly one of index, side or normal")
      : description);
}

QJsonObject material(const QString& description)
{
  return string(1, 256, description);
}

QJsonObject propValue(const QString& description)
{
  return withDescription(
    {{"type", QJsonArray{"string", "number", "boolean", "array"}}}, description);
}

void addPaging(QJsonObject& properties)
{
  properties["offset"] = QJsonObject{
    {"type", "integer"},
    {"minimum", 0},
    {"default", 0},
    {"description", "index of the first result to return"},
  };
  properties["limit"] = QJsonObject{
    {"type", "integer"},
    {"minimum", 1},
    {"maximum", MaxLimit},
    {"default", DefaultLimit},
    {"description", "maximum number of results to return"},
  };
}

} // namespace tb::ui::schema
