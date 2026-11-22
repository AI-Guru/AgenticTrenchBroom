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

#include <optional>

namespace tb::mcp
{

/**
 * Describes why a value does not match a schema.
 *
 * The pointer is an RFC 6901 JSON pointer to the offending value ("" is the root, e.g.
 * "/bounds/min/0"). For a missing required property, it points to the missing property.
 */
struct SchemaViolation
{
  QString pointer;
  QString message;
  QJsonObject details;
};

/**
 * Validates the given value against a subset of JSON Schema. The keywords are checked
 * in a fixed order and the first violation is returned, so the result is deterministic.
 *
 * Supported keywords:
 * - type (a string or an array of strings among object, array, string, number,
 *   integer, boolean, null)
 * - properties, required, additionalProperties (only false; absent means allowed)
 * - items (a single schema), minItems, maxItems
 * - enum, const
 * - minimum, maximum
 * - minLength, maxLength (in Unicode code points), pattern (not implicitly anchored)
 * - oneOf (exactly one alternative must match)
 * - description, title, default (ignored)
 *
 * Returns nullopt if the value matches.
 */
std::optional<SchemaViolation> validate(
  const QJsonObject& schema, const QJsonValue& value);

/**
 * Returns whether the given schema only uses the keywords supported by validate() with
 * well-formed values (e.g. valid regular expressions). If not and offending is not null,
 * it is set to a JSON pointer to the offending keyword within the schema.
 */
bool usesOnlySupportedKeywords(const QJsonObject& schema, QString* offending = nullptr);

} // namespace tb::mcp
