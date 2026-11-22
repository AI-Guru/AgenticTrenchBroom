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

#include "TestUtils.h"
#include "mcp/Tool.h"

#include "kd/contracts.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

struct ContractViolation : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

/**
 * Turns contract violations into exceptions while in scope, so that violations can be
 * tested without aborting the test process.
 */
struct ThrowOnContractViolation
{
  ThrowOnContractViolation()
  {
    kd::set_contract_violation_handler([](
                                         const std::string_view,
                                         const int,
                                         const std::string_view type,
                                         const std::string_view condition) {
      throw ContractViolation{std::string{type} + ": " + std::string{condition}};
    });
  }

  ~ThrowOnContractViolation()
  {
    kd::set_contract_violation_handler([](
                                         const std::string_view,
                                         const int,
                                         const std::string_view,
                                         const std::string_view) { std::abort(); });
  }

  ThrowOnContractViolation(const ThrowOnContractViolation&) = delete;
  ThrowOnContractViolation& operator=(const ThrowOnContractViolation&) = delete;
};

ToolSpec makeTool(QString name, QJsonObject inputSchema = {{"type", "object"}})
{
  return ToolSpec{
    .name = std::move(name),
    .title = "Title",
    .description = "Description",
    .inputSchema = std::move(inputSchema),
    .annotations = {},
    .handler = [](const QJsonObject&) -> ToolResult { return ToolOutput{}; },
  };
}

} // namespace

TEST_CASE("ToolRegistry")
{
  auto registry = ToolRegistry{};

  SECTION("add")
  {
    registry.add(makeTool("b"));
    registry.add(makeTool("a"));
    REQUIRE(registry.tools().size() == 2);

    SECTION("duplicate names are rejected")
    {
      const auto handler = ThrowOnContractViolation{};
      CHECK_THROWS_AS(registry.add(makeTool("a")), ContractViolation);
      CHECK(registry.tools().size() == 2);
    }

    SECTION("unsupported schema keywords are rejected")
    {
      const auto handler = ThrowOnContractViolation{};
      CHECK_THROWS_AS(
        registry.add(makeTool("c", QJsonObject{{"type", "object"}, {"format", "x"}})),
        ContractViolation);
      CHECK(registry.tools().size() == 2);
    }

    SECTION("a handler is required")
    {
      const auto handler = ThrowOnContractViolation{};
      auto tool = makeTool("c");
      tool.handler = nullptr;
      CHECK_THROWS_AS(registry.add(std::move(tool)), ContractViolation);
      CHECK(registry.tools().size() == 2);
    }
  }

  SECTION("find")
  {
    registry.add(makeTool("first"));
    registry.add(makeTool("second"));

    const auto* first = registry.find(u"first");
    REQUIRE(first != nullptr);
    CHECK(first->name == "first");

    const auto* second = registry.find(u"second");
    REQUIRE(second != nullptr);
    CHECK(second->name == "second");

    CHECK(registry.find(u"First") == nullptr);
    CHECK(registry.find(u"third") == nullptr);
    CHECK(registry.find(u"") == nullptr);
  }

  SECTION("tools")
  {
    CHECK(registry.tools().empty());

    registry.add(makeTool("zeta"));
    registry.add(makeTool("alpha"));
    registry.add(makeTool("mu"));

    auto names = std::vector<QString>{};
    for (const auto& tool : registry.tools())
    {
      names.push_back(tool.name);
    }
    CHECK(names == std::vector<QString>{"zeta", "alpha", "mu"});
  }
}

TEST_CASE("Tool")
{
  SECTION("toolErrorToCallResult")
  {
    SECTION("with hint")
    {
      const auto result = toolErrorToCallResult(ToolError{
        .code = "stale_node",
        .message = "B5 was removed",
        .hint = "call find_nodes for current ids",
        .details = {{"removed_by", "MCP: Delete Nodes"}},
      });
      CHECK(result == jsonObject(R"({
          "content": [{"type": "text", "text": "stale_node: B5 was removed Hint: call find_nodes for current ids"}],
          "structuredContent": {"error": {
            "code": "stale_node",
            "message": "B5 was removed",
            "hint": "call find_nodes for current ids",
            "details": {"removed_by": "MCP: Delete Nodes"}
          }},
          "isError": true
        })"));
    }

    SECTION("without hint")
    {
      const auto result = toolErrorToCallResult(ToolError{
        .code = "invalid_argument",
        .message = "bad",
        .hint = {},
        .details = {},
      });
      CHECK(result == jsonObject(R"({
          "content": [{"type": "text", "text": "invalid_argument: bad"}],
          "structuredContent": {"error": {
            "code": "invalid_argument",
            "message": "bad",
            "hint": "",
            "details": {}
          }},
          "isError": true
        })"));
    }
  }

  SECTION("toolOutputToCallResult")
  {
    SECTION("structured only")
    {
      const auto result = toolOutputToCallResult(ToolOutput{
        .structured = {{"id", "B1"}, {"bounds", QJsonArray{1, 2.5}}},
        .extraContent = {},
      });
      CHECK(result == jsonObject(R"({
          "content": [{"type": "text", "text": "{\"bounds\":[1,2.5],\"id\":\"B1\"}"}],
          "structuredContent": {"id": "B1", "bounds": [1, 2.5]},
          "isError": false
        })"));
    }

    SECTION("with extra content")
    {
      const auto image =
        QJsonObject{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}};
      const auto result = toolOutputToCallResult(ToolOutput{
        .structured = {{"width", 1}},
        .extraContent = {image},
      });
      CHECK(result == jsonObject(R"({
          "content": [
            {"type": "text", "text": "{\"width\":1}"},
            {"type": "image", "data": "AAAA", "mimeType": "image/png"}
          ],
          "structuredContent": {"width": 1},
          "isError": false
        })"));
    }

    SECTION("empty output")
    {
      const auto result = toolOutputToCallResult(ToolOutput{});
      CHECK(result["content"][0]["text"] == "{}");
      CHECK(result["structuredContent"] == QJsonObject{});
      CHECK(result["isError"] == false);
    }
  }

  SECTION("toolResultToCallResult")
  {
    const auto output = ToolOutput{.structured = {{"a", 1}}, .extraContent = {}};
    CHECK(toolResultToCallResult(ToolResult{output}) == toolOutputToCallResult(output));

    const auto error = ToolError{.code = "x", .message = "y", .hint = {}, .details = {}};
    CHECK(toolResultToCallResult(ToolResult{error}) == toolErrorToCallResult(error));
  }
}

} // namespace tb::mcp
