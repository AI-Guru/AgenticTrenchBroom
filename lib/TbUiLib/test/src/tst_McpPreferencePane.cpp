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

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHostAddress>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTcpServer>
#include <QToolBar>

#include "McpAppTestUtils.h"
#include "TestPreferenceStore.h"
#include "base/PreferenceManager.h"
#include "prefs/Preferences.h"
#include "ui/CatchConfig.h"
#include "ui/McpController.h"
#include "ui/McpPreferencePane.h"
#include "ui/PreferenceDialog.h"

#include "kd/k.h"

#include <memory>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/** Resets the MCP preferences when the test ends. */
class ResetMcpPreferences
{
public:
  ~ResetMcpPreferences()
  {
    auto& prefs = PreferenceManager::instance();
    prefs.resetToDefault(Preferences::McpServerEnabled);
    prefs.resetToDefault(Preferences::McpServerPort);
    prefs.saveChanges();
  }
};

template <typename T>
T& child(const QWidget& widget, const QString& name)
{
  auto* result = widget.findChild<T*>(name);
  REQUIRE(result != nullptr);
  return *result;
}

/** Returns a port that was free a moment ago. */
quint16 freePort()
{
  auto probe = QTcpServer{};
  REQUIRE(probe.listen(QHostAddress::LocalHost, 0));
  return probe.serverPort();
}

/**
 * Replaces the global PreferenceManager with one that doesn't save instantly (as on
 * Windows and Linux) and restores one equivalent to the one RunAllTests.cpp creates.
 */
class ScopedPendingPreferenceManager
{
public:
  ScopedPendingPreferenceManager()
  {
    PreferenceManager::createInstance(
      std::make_unique<TestPreferenceStore>(), !K(saveInstantly));
  }

  ~ScopedPendingPreferenceManager()
  {
    PreferenceManager::createInstance(
      std::make_unique<TestPreferenceStore>(), K(saveInstantly));
  }

  ScopedPendingPreferenceManager(const ScopedPendingPreferenceManager&) = delete;
  ScopedPendingPreferenceManager& operator=(const ScopedPendingPreferenceManager&) =
    delete;
};

} // namespace

TEST_CASE("McpPreferencePane")
{
  auto resetPreferences = ResetMcpPreferences{};
  auto f = McpAppFixture{};
  auto& controller = f.controller;

  // the server must not be started by the fixture
  REQUIRE_FALSE(controller.isListening());

  SECTION("constructor")
  {
    auto pane = McpPreferencePane{f.appController};

    CHECK_FALSE(child<QCheckBox>(pane, "mcpServerEnabled").isChecked());
    CHECK(child<QSpinBox>(pane, "mcpServerPort").value() == 6767);
    CHECK(child<QSpinBox>(pane, "mcpServerPort").minimum() == 0);
    CHECK(child<QSpinBox>(pane, "mcpServerPort").maximum() == 65535);
    CHECK_FALSE(child<QSpinBox>(pane, "mcpServerPort").keyboardTracking());
    CHECK(pane.statusText() == "Not running");
    CHECK(child<QLineEdit>(pane, "mcpServerUrl").isReadOnly());
    CHECK(child<QLineEdit>(pane, "mcpClaudeCommand").isReadOnly());

    // the configured port is shown before the server runs
    CHECK(pane.urlText() == "http://127.0.0.1:6767/mcp");
    CHECK(
      pane.commandText()
      == "claude mcp add --transport http trenchbroom http://127.0.0.1:6767/mcp");
  }

  SECTION("enabling the server")
  {
    setPref(Preferences::McpServerPort, int(freePort()));
    auto pane = McpPreferencePane{f.appController};
    auto& enabled = child<QCheckBox>(pane, "mcpServerEnabled");

    SECTION("starts and stops the server live")
    {
      enabled.setChecked(true);
      CHECK(pref(Preferences::McpServerEnabled));
      REQUIRE(controller.isListening());
      const auto url = mcpServerUrl(controller.port());
      CHECK(pane.statusText() == "Running at " + url);
      CHECK(pane.urlText() == url);
      CHECK(pane.commandText() == mcpClaudeAddCommand(controller.port()));

      enabled.setChecked(false);
      CHECK_FALSE(pref(Preferences::McpServerEnabled));
      CHECK_FALSE(controller.isListening());
      CHECK(pane.statusText() == "Not running");
    }

    SECTION("shows an error if the port is in use")
    {
      auto blocker = QTcpServer{};
      REQUIRE(blocker.listen(QHostAddress::LocalHost, 0));
      setPref(Preferences::McpServerPort, int(blocker.serverPort()));

      enabled.setChecked(true);
      CHECK_FALSE(controller.isListening());
      CHECK(pane.statusText().contains("not started"));
      CHECK(pane.statusText() == controller.lastError());

      // the error is cleared when the server is disabled
      enabled.setChecked(false);
      CHECK(pane.statusText() == "Not running");
    }
  }

  SECTION("changing the port")
  {
    auto pane = McpPreferencePane{f.appController};
    auto& port = child<QSpinBox>(pane, "mcpServerPort");

    SECTION("updates the preference and the shown URL")
    {
      port.setValue(7000);
      CHECK(pref(Preferences::McpServerPort) == 7000);
      CHECK(pane.urlText() == "http://127.0.0.1:7000/mcp");
      CHECK(pane.commandText().endsWith("http://127.0.0.1:7000/mcp"));
    }

    SECTION("restarts a running server on the new port")
    {
      setPref(Preferences::McpServerPort, int(freePort()));
      setPref(Preferences::McpServerEnabled, true);
      REQUIRE(controller.isListening());

      const auto newPort = freePort();
      port.setValue(int(newPort));
      CHECK(controller.isListening());
      CHECK(controller.port() == newPort);
      CHECK(pane.urlText() == mcpServerUrl(newPort));
    }

    SECTION("port 0 picks a free port and shows it once the server runs")
    {
      port.setValue(0);
      CHECK(port.text() == "Any free port");
      CHECK(pane.urlText().isEmpty());
      CHECK(pane.commandText().isEmpty());
      CHECK_FALSE(child<QPushButton>(pane, "mcpCopyUrl").isEnabled());
      CHECK_FALSE(child<QPushButton>(pane, "mcpCopyCommand").isEnabled());

      child<QCheckBox>(pane, "mcpServerEnabled").setChecked(true);
      REQUIRE(controller.isListening());
      CHECK(controller.port() != 0);
      CHECK(pane.urlText() == mcpServerUrl(controller.port()));
      CHECK(child<QPushButton>(pane, "mcpCopyUrl").isEnabled());
    }
  }

  SECTION("statusText")
  {
    SECTION("follows a server started on the command line")
    {
      auto pane = McpPreferencePane{f.appController};
      controller.start(0);
      REQUIRE(controller.isListening());
      CHECK(pane.statusText().startsWith("Running at " + controller.url()));
      CHECK(pane.statusText().contains("--mcp-port"));
      CHECK(pane.urlText() == controller.url());

      controller.stop();
      CHECK(pane.statusText() == "Not running");
    }

    SECTION("shows an invalid port preference")
    {
      setPref(Preferences::McpServerPort, 70000);
      setPref(Preferences::McpServerEnabled, true);
      auto pane = McpPreferencePane{f.appController};
      CHECK(pane.statusText().contains("invalid port"));
      // the spin box clamps the value
      CHECK(child<QSpinBox>(pane, "mcpServerPort").value() == 65535);
    }

    SECTION("says that the server is not available without a controller")
    {
      f.appController.setMcpController(nullptr);
      auto pane = McpPreferencePane{f.appController};
      CHECK(pane.statusText() == "The MCP server is not available.");
      CHECK(pane.urlText() == "http://127.0.0.1:6767/mcp");
      f.appController.setMcpController(&controller);
    }
  }

  SECTION("copy buttons")
  {
    setPref(Preferences::McpServerPort, int(freePort()));
    setPref(Preferences::McpServerEnabled, true);
    REQUIRE(controller.isListening());

    auto pane = McpPreferencePane{f.appController};
    auto* clipboard = QApplication::clipboard();
    REQUIRE(clipboard != nullptr);

    child<QPushButton>(pane, "mcpCopyUrl").click();
    CHECK(clipboard->text() == controller.url());

    child<QPushButton>(pane, "mcpCopyCommand").click();
    CHECK(clipboard->text() == mcpClaudeAddCommand(controller.port()));
  }

  SECTION("canResetToDefaults")
  {
    auto pane = McpPreferencePane{f.appController};
    CHECK(pane.canResetToDefaults());
  }

  SECTION("resetToDefaults")
  {
    setPref(Preferences::McpServerPort, int(freePort()));
    setPref(Preferences::McpServerEnabled, true);
    REQUIRE(controller.isListening());

    auto pane = McpPreferencePane{f.appController};
    pane.resetToDefaults();

    CHECK_FALSE(pref(Preferences::McpServerEnabled));
    CHECK(pref(Preferences::McpServerPort) == 6767);
    CHECK_FALSE(controller.isListening());
    CHECK_FALSE(child<QCheckBox>(pane, "mcpServerEnabled").isChecked());
    CHECK(child<QSpinBox>(pane, "mcpServerPort").value() == 6767);
  }

  SECTION("updateControls")
  {
    auto pane = McpPreferencePane{f.appController};

    // preference changes made elsewhere, e.g. by the PreferenceDialog
    setPref(Preferences::McpServerPort, 1234);
    pane.updateControls();
    CHECK(child<QSpinBox>(pane, "mcpServerPort").value() == 1234);
    CHECK(pane.urlText() == "http://127.0.0.1:1234/mcp");
  }

  SECTION("validate")
  {
    auto pane = McpPreferencePane{f.appController};
    CHECK(pane.validate());
  }

  SECTION("outlives the controller")
  {
    auto otherController = std::make_unique<McpController>(f.appController);
    REQUIRE(f.appController.mcpController() == otherController.get());
    auto pane = McpPreferencePane{f.appController};
    otherController->start(0);
    REQUIRE(otherController->isListening());
    otherController.reset();

    CHECK(f.appController.mcpController() == nullptr);
    pane.updateControls();
    CHECK(pane.statusText() == "The MCP server is not available.");
    f.appController.setMcpController(&controller);
  }
}

TEST_CASE("McpPreferencePane with pending preferences")
{
  // the controller must connect to the replaced preference manager, so it is created
  // after it and destroyed before it
  const auto pendingPreferences = ScopedPendingPreferenceManager{};
  auto f = McpAppFixture{};
  auto& controller = f.controller;
  auto& prefs = PreferenceManager::instance();
  REQUIRE_FALSE(prefs.saveInstantly());

  auto pane = McpPreferencePane{f.appController};
  auto& enabled = child<QCheckBox>(pane, "mcpServerEnabled");
  auto& port = child<QSpinBox>(pane, "mcpServerPort");
  constexpr auto pendingSuffix = ". Your changes take effect when you click Apply or OK.";

  const auto newPort = freePort();
  enabled.setChecked(true);
  port.setValue(int(newPort));

  // nothing happens until the changes are saved
  CHECK_FALSE(pref(Preferences::McpServerEnabled));
  CHECK_FALSE(controller.isListening());
  CHECK(enabled.isChecked());
  CHECK(port.value() == int(newPort));
  CHECK(pane.statusText() == QString{"Not running"} + pendingSuffix);
  CHECK(pane.urlText() == mcpServerUrl(newPort));

  SECTION("Apply starts the server on the new port")
  {
    prefs.saveChanges();
    REQUIRE(controller.isListening());
    CHECK(controller.port() == newPort);
    CHECK(pane.statusText() == "Running at " + mcpServerUrl(newPort));

    SECTION("and disabling is pending again")
    {
      enabled.setChecked(false);
      CHECK(controller.isListening());
      CHECK(pane.statusText() == "Running at " + mcpServerUrl(newPort) + pendingSuffix);

      prefs.saveChanges();
      CHECK_FALSE(controller.isListening());
      CHECK(pane.statusText() == "Not running");
    }
  }

  SECTION("Cancel keeps the server stopped")
  {
    prefs.discardChanges();
    pane.updateControls();
    CHECK_FALSE(controller.isListening());
    CHECK_FALSE(enabled.isChecked());
    CHECK(port.value() == 6767);
    CHECK(pane.statusText() == "Not running");
  }
}

TEST_CASE("PreferenceDialog")
{
  auto resetPreferences = ResetMcpPreferences{};
  auto f = McpAppFixture{};

  SECTION("has an MCP pane")
  {
    auto dialog = PreferenceDialog{f.appController, nullptr};
    auto* toolBar = dialog.findChild<QToolBar*>();
    REQUIRE(toolBar != nullptr);

    auto* mcpAction = static_cast<QAction*>(nullptr);
    for (auto* action : toolBar->actions())
    {
      if (action->text() == "MCP")
      {
        mcpAction = action;
      }
    }
    REQUIRE(mcpAction != nullptr);

    auto* stackedWidget = dialog.findChild<QStackedWidget*>();
    REQUIRE(stackedWidget != nullptr);
    mcpAction->trigger();
    CHECK(dynamic_cast<McpPreferencePane*>(stackedWidget->currentWidget()) != nullptr);
  }
}

} // namespace tb::ui
