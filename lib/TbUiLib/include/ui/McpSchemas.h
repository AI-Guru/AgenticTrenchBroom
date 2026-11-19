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
#include <QString>
#include <QStringList>

#include <optional>

namespace tb::ui::schema
{

/** Matches world, default_layer, @selection and typed ids such as B12. */
inline constexpr auto NodeIdPattern =
  "^(world|default_layer|@selection|[LGEBP][1-9][0-9]{0,17})$";

inline constexpr int DefaultLimit = 500;
inline constexpr int MaxLimit = 5000;
inline constexpr int MaxNodeIds = 5000;

/**
 * Returns an object schema with the given properties, the given required properties and
 * additionalProperties set to false.
 */
QJsonObject object(QJsonObject properties, const QStringList& required = {});

/** Returns a copy of the given schema with its description set. */
QJsonObject describe(QJsonObject schema, const QString& description);

QJsonObject boolean(const QString& description = {});
QJsonObject number(
  std::optional<double> minimum = std::nullopt,
  std::optional<double> maximum = std::nullopt,
  const QString& description = {});
QJsonObject integer(
  std::optional<double> minimum = std::nullopt,
  std::optional<double> maximum = std::nullopt,
  const QString& description = {});
QJsonObject string(
  std::optional<int> minLength = std::nullopt,
  std::optional<int> maxLength = std::nullopt,
  const QString& description = {});
QJsonObject stringEnum(const QStringList& values, const QString& description = {});

/** An array with the given item schema and minItems 1 unless stated otherwise. */
QJsonObject array(
  QJsonObject items,
  std::optional<int> minItems = 1,
  std::optional<int> maxItems = std::nullopt,
  const QString& description = {});

/** [x, y, z] */
QJsonObject vec3(const QString& description = {});

/** [u, v] */
QJsonObject vec2(const QString& description = {});

/** {"min": Vec3, "max": Vec3} */
QJsonObject bounds(const QString& description = {});

/** A single node id. */
QJsonObject nodeId(const QString& description = {});

/** A list of node ids, which may contain "@selection". */
QJsonObject nodeIds(int maxItems = MaxNodeIds, const QString& description = {});

/** One of "+x", "-x", "+y", "-y", "+z", "-z". */
QJsonObject side(const QString& description = {});

/** "x", "y", "z" or a vector. */
QJsonObject axis(const QString& description = {});

/** {brush, index | side | normal} */
QJsonObject faceRef(const QString& description = {});

/** A material name. */
QJsonObject material(const QString& description = {});

/** A string, number, boolean or array value for an entity property. */
QJsonObject propValue(const QString& description = {});

/** Adds the paging properties offset and limit to the given properties. */
void addPaging(QJsonObject& properties);

} // namespace tb::ui::schema
