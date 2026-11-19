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

#include "base/Logger.h"
#include "ui/CatchConfig.h" // IWYU pragma: keep
#include "ui/McpLogCapture.h"

#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

class RecordingLogger : public Logger
{
public:
  std::vector<std::pair<LogLevel, std::string>> messages;

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    messages.emplace_back(level, std::string{message});
  }
};

} // namespace

TEST_CASE("McpTeeLogger")
{
  auto target = RecordingLogger{};
  auto tee = McpTeeLogger{&target};

  SECTION("doLog")
  {
    SECTION("forwards every message to the target")
    {
      tee.debug() << "debug";
      tee.info() << "info";
      tee.warn() << "warn";
      tee.error() << "error";

      CHECK(
        target.messages
        == std::vector<std::pair<LogLevel, std::string>>{
          {LogLevel::Debug, "debug"},
          {LogLevel::Info, "info"},
          {LogLevel::Warn, "warn"},
          {LogLevel::Error, "error"},
        });
    }

    SECTION("works without a target")
    {
      auto standalone = McpTeeLogger{};
      standalone.error() << "lost";
      REQUIRE(standalone.lines().size() == 1);
      CHECK(standalone.lines().front().text == "lost");
    }
  }

  SECTION("lines")
  {
    tee.info() << "info";
    tee.warn() << "a warning";
    tee.debug() << "debug";
    tee.error() << "an error with \xc3\xa4";

    const auto& lines = std::as_const(tee).lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].level == LogLevel::Warn);
    CHECK(lines[0].text == "a warning");
    CHECK(lines[1].level == LogLevel::Error);
    CHECK(lines[1].text == QString::fromUtf8("an error with \xc3\xa4"));

    tee.lines().clear();
    CHECK(tee.lines().empty());
  }

  SECTION("hasErrors")
  {
    CHECK_FALSE(tee.hasErrors());
    tee.warn() << "warning";
    CHECK_FALSE(tee.hasErrors());
    tee.error() << "error";
    CHECK(tee.hasErrors());
  }
}

} // namespace tb::ui
