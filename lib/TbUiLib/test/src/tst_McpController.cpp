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
#include <QDialog>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextEdit>

#include "McpAppTestUtils.h"
#include "McpTestUtils.h"
#include "base/PreferenceManager.h"
#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "mdl/TransactionScope.h"
#include "prefs/Preferences.h"
#include "ui/Console.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/McpController.h"

#include <chrono>
#include <future>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

using namespace std::chrono_literals;

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

/** Sends one HTTP request with Connection: close and returns the whole response. */
QByteArray httpRequest(const quint16 port, const QByteArray& request)
{
  auto socket = QTcpSocket{};
  socket.connectToHost(QHostAddress::LocalHost, port);
  if (!socket.waitForConnected(5000))
  {
    return {};
  }
  socket.write(request);
  socket.waitForBytesWritten(5000);

  auto response = QByteArray{};
  while (socket.state() == QAbstractSocket::ConnectedState
         && socket.waitForReadyRead(5000))
  {
    response += socket.readAll();
  }
  response += socket.readAll();
  return response;
}

/** Runs httpRequest on another thread while the main thread handles events. */
QByteArray httpRequestWithEventLoop(const quint16 port, const QByteArray& request)
{
  auto future =
    std::async(std::launch::async, [=]() { return httpRequest(port, request); });
  while (future.wait_for(1ms) != std::future_status::ready)
  {
    QApplication::processEvents();
  }
  return future.get();
}

QByteArray initializeRequest(const quint16 port)
{
  const auto body = QByteArray{
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})"};
  return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port)
         + "\r\nContent-Type: application/json\r\nAccept: application/json\r\n"
           "Connection: close\r\nContent-Length: "
         + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

QString consoleText(MapWindow& window)
{
  const auto* console = dynamic_cast<const Console*>(&window.logger());
  const auto* textEdit = console ? console->findChild<QTextEdit*>() : nullptr;
  return textEdit ? textEdit->toPlainText() : QString{};
}

bool waitForConsoleText(MapWindow& window, const QString& text)
{
  for (auto i = 0; i < 200; ++i)
  {
    if (consoleText(window).contains(text))
    {
      return true;
    }
    QApplication::processEvents();
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

} // namespace

TEST_CASE("McpController")
{
  auto resetPreferences = ResetMcpPreferences{};
  auto f = McpAppFixture{};
  auto& controller = f.controller;

  SECTION("start")
  {
    SECTION("does nothing if the server is disabled in the preferences")
    {
      controller.start();
      CHECK_FALSE(controller.isListening());
      CHECK(controller.port() == 0);
    }

    SECTION("listens on the given port and serves MCP requests")
    {
      controller.start(0);
      REQUIRE(controller.isListening());
      const auto port = controller.port();
      CHECK(port != 0);
      CHECK(controller.lastError().isEmpty());

      const auto response = httpRequestWithEventLoop(port, initializeRequest(port));
      CHECK(response.startsWith("HTTP/1.1 200"));
      CHECK(response.toLower().contains("mcp-session-id:"));
      const auto body = response.mid(response.indexOf("\r\n\r\n") + 4);
      const auto result =
        QJsonDocument::fromJson(body).object().value("result").toObject();
      CHECK(result.value("serverInfo").toObject().value("name") == "trenchbroom");
      CHECK(result.value("instructions").toString().contains("new_map"));
    }

    SECTION("does not change the preferences")
    {
      controller.start(0);
      REQUIRE(controller.isListening());
      CHECK_FALSE(pref(Preferences::McpServerEnabled));
      CHECK(pref(Preferences::McpServerPort) == 6767);
    }

    SECTION("reports a port that is in use")
    {
      auto blocker = QTcpServer{};
      REQUIRE(blocker.listen(QHostAddress::LocalHost, 0));

      controller.start(blocker.serverPort());
      CHECK_FALSE(controller.isListening());
      CHECK(controller.lastError().contains("not started"));
    }

    SECTION("can be called again while listening")
    {
      controller.start(0);
      const auto port = controller.port();
      controller.start(0);
      CHECK(controller.isListening());
      CHECK(controller.port() == port);
    }
  }

  SECTION("stop")
  {
    controller.start(0);
    REQUIRE(controller.isListening());
    controller.stop();
    CHECK_FALSE(controller.isListening());
    CHECK(controller.port() == 0);
  }

  SECTION("preferences")
  {
    SECTION("enabling and disabling the server starts and stops it")
    {
      setPref(Preferences::McpServerPort, 0);
      CHECK_FALSE(controller.isListening());

      setPref(Preferences::McpServerEnabled, true);
      CHECK(controller.isListening());
      CHECK(controller.port() != 0);

      setPref(Preferences::McpServerEnabled, false);
      CHECK_FALSE(controller.isListening());
    }

    SECTION("changing the port restarts the server on that port")
    {
      auto probe = QTcpServer{};
      REQUIRE(probe.listen(QHostAddress::LocalHost, 0));
      const auto freePort = probe.serverPort();
      probe.close();

      setPref(Preferences::McpServerPort, 0);
      setPref(Preferences::McpServerEnabled, true);
      REQUIRE(controller.isListening());

      setPref(Preferences::McpServerPort, int(freePort));
      CHECK(controller.isListening());
      CHECK(controller.port() == freePort);
    }

    SECTION("an invalid port is reported")
    {
      setPref(Preferences::McpServerPort, 70000);
      setPref(Preferences::McpServerEnabled, true);
      CHECK_FALSE(controller.isListening());
      CHECK(controller.lastError().contains("invalid port"));
    }

    SECTION("a preference change overrides the command line port")
    {
      controller.start(0);
      REQUIRE(controller.isListening());

      // the server is disabled in the preferences, so it stops
      setPref(Preferences::McpServerPort, 0);
      CHECK_FALSE(controller.isListening());
    }
  }

  SECTION("dispatcher")
  {
    CHECK(controller.dispatcher().find(u"new_map") != nullptr);
    CHECK(controller.dispatcher().find(u"get_map_info") != nullptr);
    CHECK(controller.dispatcher().find(u"screenshot") != nullptr);
  }

  SECTION("callTool")
  {
    SECTION("no document")
    {
      CHECK(f.err("get_map_info") == "no_document");
      CHECK(
        f.err("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})")
        == "no_document");
      CHECK(f.err("undo") == "no_document");
      CHECK(f.ok("list_documents").value("documents").toArray().isEmpty());
    }

    SECTION("busy gate")
    {
      auto& window = f.newMap();
      auto& map = window.document().map();

      SECTION("a modal dialog")
      {
        auto dialog = QDialog{};
        dialog.setModal(true);
        dialog.open();
        REQUIRE(QApplication::activeModalWidget() == &dialog);

        const auto error = f.error("get_map_info");
        CHECK(error.value("code") == "editor_busy");
        CHECK(error.value("details").toObject().value("reason") == "modal_dialog");
        CHECK(
          f.err("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})")
          == "editor_busy");
        CHECK(f.err("new_map", R"({"game":"Test"})") == "editor_busy");
        CHECK(f.err("save_map") == "editor_busy");

        dialog.close();
        f.ok("get_map_info");
      }

      SECTION("a popup")
      {
        auto menu = QMenu{};
        menu.addAction("item");
        menu.popup(QPoint{0, 0});
        if (QApplication::activePopupWidget() != &menu)
        {
          SKIP("this platform does not show popups");
        }

        const auto error = f.error("get_map_info");
        CHECK(error.value("code") == "editor_busy");
        CHECK(error.value("details").toObject().value("reason") == "popup");
        CHECK(f.err("new_map", R"({"game":"Test"})") == "editor_busy");

        menu.close();
        f.ok("get_map_info");
      }

      SECTION("a blocked selection")
      {
        map.editorContext().setBlockSelection(true);

        f.ok("get_map_info");
        const auto error =
          f.error("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
        CHECK(error.value("code") == "editor_busy");
        CHECK(error.value("details").toObject().value("reason") == "selection_blocked");
        CHECK(f.err("focus_nodes", R"({"ids":["@selection"]})") == "editor_busy");

        map.editorContext().setBlockSelection(false);
        f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
      }

      SECTION("a running transaction")
      {
        map.startTransaction("user drag", mdl::TransactionScope::LongRunning);

        f.ok("get_map_info");
        const auto error =
          f.error("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
        CHECK(error.value("code") == "editor_busy");
        CHECK(error.value("details").toObject().value("reason") == "transaction_active");
        CHECK(f.err("undo") == "editor_busy");
        CHECK(f.err("close_map") == "editor_busy");

        map.cancelTransaction();
        f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
      }

      SECTION("a modal tool")
      {
        const auto box =
          f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
        f.ok("select_nodes", QJsonObject{{"ids", QJsonArray{box.value("id")}}});
        REQUIRE(window.toolBox().canToggleAnyVertexTool());
        window.toolBox().toggleVertexTool();
        REQUIRE(window.toolBox().anyModalToolActive());

        const auto error =
          f.error("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
        CHECK(error.value("details").toObject().value("reason") == "modal_tool_active");
        f.ok("get_map_info");

        window.toolBox().toggleVertexTool();
        f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
      }
    }

    SECTION("log capture")
    {
      auto& window = f.newMap();

      const auto error = f.error("paste_map_text", R"({"text":"{ this is not a map"})");
      CHECK(error.value("code") == "parse_failed");
      const auto log = error.value("details").toObject().value("log").toArray();
      REQUIRE_FALSE(log.isEmpty());

      // the captured lines were also forwarded to the console
      const auto firstLine = log.first().toObject().value("text").toString();
      CHECK(waitForConsoleText(window, firstLine.left(20)));

      // the console is the document's target logger again
      window.document().logger().warn() << "logged after the call";
      CHECK(waitForConsoleText(window, "logged after the call"));

      const auto box =
        f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})");
      CHECK(box.value("log").toArray().isEmpty());
    }

    SECTION("reloading a document resets its ids")
    {
      auto& window = f.newMap();
      auto dir = QTemporaryDir{};

      const auto first =
        f.ok("create_box_brush", R"({"bounds":{"min":[0,0,0],"max":[8,8,8]}})")
          .value("id")
          .toString();
      const auto second =
        f.ok("create_box_brush", R"({"bounds":{"min":[16,0,0],"max":[24,8,8]}})")
          .value("id")
          .toString();
      const auto third =
        f.ok("create_box_brush", R"({"bounds":{"min":[32,0,0],"max":[40,8,8]}})")
          .value("id")
          .toString();
      f.ok("delete_nodes", QJsonObject{{"ids", QJsonArray{first, second}}});
      CHECK(f.err("get_nodes", QJsonObject{{"ids", QJsonArray{second}}}) == "stale_node");
      f.ok("get_nodes", QJsonObject{{"ids", QJsonArray{third}}});
      const auto brushCount =
        f.ok("find_nodes", R"({"types":["brush"]})").value("ids").toArray().size();

      f.ok("save_map", QJsonObject{{"path", dir.filePath("reload.map")}});
      const auto documentId = f.ok("get_map_info").value("document");

      REQUIRE(window.document().reload());

      // ids start again at 1, so the highest old ids are unknown now
      CHECK(
        f.err("get_nodes", QJsonObject{{"ids", QJsonArray{third}}}) == "unknown_node");
      const auto brushes =
        f.ok("find_nodes", R"({"types":["brush"]})").value("ids").toArray();
      CHECK(brushes.size() == brushCount);
      CHECK(f.ok("get_map_info").value("document") == documentId);
    }
  }
}

} // namespace tb::ui
