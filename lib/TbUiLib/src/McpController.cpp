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

#include "ui/McpController.h"

#include <QApplication>
#include <QObject>
#include <QPointer>
#include <QWidget>

#include "base/NotifierConnection.h"
#include "base/PreferenceManager.h"
#include "mcp/HttpTransport.h"
#include "mcp/Server.h"
#include "mcp/Tool.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "prefs/Preferences.h"
#include "ui/AppController.h"
#include "ui/GetVersion.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/McpAppTools.h"
#include "ui/McpLogCapture.h"
#include "ui/McpMapTools.h"
#include "ui/McpPreconditions.h"
#include "ui/McpToolDispatcher.h"
#include "ui/McpViewTools.h"

#include <cstdio>
#include <memory>
#include <unordered_map>

namespace tb::ui
{
namespace
{

const auto Instructions = QStringLiteral(
  R"(TrenchBroom level editor (MCP). Conventions:
- World units; Z up; right-handed; +X east, +Y north. Quake scale: player hull 32x32x56, step 18, doors >= 64 high,
  walls typically 8-16 thick. Vectors are [x,y,z]; bounds are {"min":[..],"max":[..]}; angles are DEGREES.
- Entity "angle" is yaw around +Z (0 = +X, 90 = +Y). Nothing is snapped to the grid unless you ask; keep brush vertices
  on integers (validate_map reports non-integer vertices).
- Start with list_games and new_map (game "Generic" needs no game files) or open_map; list_documents and
  set_active_document choose the map that the map tools act on. save_map writes .map files; close_map closes a saved map
  (close_map with discard_changes:true throws unsaved changes away without asking).
- Ids: world, default_layer, L<n> layer, G<n> group, E<n> entity, B<n> brush, P<n> patch; "@selection" = what the user
  selected. All types share one number sequence (a new map has L1 and B2, the next entity may be E4), so take ids
  from tool results or find_nodes instead of guessing them. Ids are stable across transforms, property/face edits, grouping and layer moves. They CHANGE after
  delete+undo, CSG, clip and paste; old ids return stale_node. New maps may contain a starter brush.
- Each mutating call is ONE undo step named "MCP: <tool>", leaves the user's selection as it was, and returns
  changes {created, removed, modified}, issues on touched nodes, warnings and log lines. undo only undoes MCP steps
  unless allow_user_commands is true.
- After new_map or open_map, call get_conventions for units, grid and typical sizes (player, doors, stairs).
- Block out with create_room (floor, ceiling and walls with door/window openings) and create_stairs; place things
  with drop_to_floor and align_nodes; describe_region summarizes an area. create_hollow_box, csg_subtract (touching
  brushes are never cut), clip_brush, create_primitive_brush, create_brush_from_points, duplicate_nodes and
  group_nodes (open_group/close_group to edit inside) cover the rest.
- checkpoint_create before big edits; checkpoint_revert undoes your steps back to it. Verify without screenshots:
  validate_map, query_point, check_brush_overlaps, checkpoint_diff, get_brush_geometry, find_nodes, get_map_stats.
  check_leaks flood fills from the point entities like a Quake compiler and returns the path of every leak; only
  world brushes seal, so run it after closing a room instead of reasoning about where brushes meet.
  screenshot and screenshot_set (views 3d, iso, top, front, side; frame_ids to look at nodes) return PNG images to
  show your work; set_camera and focus_nodes point the user's view at something.
- Use batch for multi-step edits that must succeed or fail together ("$0" refers to the first id created by
  operation 0, "$0.2" to the third); a failed batch changes nothing. batch with dry_run:true previews the results
  and issues and then rolls everything back.
- If you get editor_busy, the human is interacting (dialog open, dragging, modal tool); retry shortly.)");

const auto ModalHint = QStringLiteral("ask the user to close the dialog, then retry");

void printToStderr(const QString& message)
{
  std::fprintf(stderr, "%s\n", qPrintable(message));
  std::fflush(stderr);
}

} // namespace

class McpController::Impl : public McpAppHost
{
private:
  struct WindowRecord
  {
    QString documentId;
    std::unique_ptr<McpDocumentState> state;
    NotifierConnection notifierConnection;
  };

  AppController& m_appController;
  Notifier<>& m_statusDidChangeNotifier;
  // destroyed last, which disconnects the window signals
  QObject m_context;

  mcp::ToolRegistry m_registry;
  std::unique_ptr<McpToolDispatcher> m_dispatcher;
  std::unique_ptr<mcp::Server> m_server;
  std::unique_ptr<mcp::HttpTransport> m_transport;

  std::optional<quint16> m_commandLinePort;
  QString m_lastError;

  std::unordered_map<MapWindow*, WindowRecord> m_windows;
  quint64 m_nextDocumentId = 1;
  QPointer<MapWindow> m_pinned;

  std::unique_ptr<McpTeeLogger> m_teeLogger;
  QPointer<MapWindow> m_teeWindow;

  NotifierConnection m_notifierConnection;

public:
  Impl(AppController& appController, Notifier<>& statusNotifier)
    : m_appController{appController}
    , m_statusDidChangeNotifier{statusNotifier}
  {
    m_dispatcher = std::make_unique<McpToolDispatcher>(
      m_registry,
      [&](const McpToolKind kind) {
        return target(kind) | kdl::transform([](const McpTarget& t) { return t.doc; });
      },
      [&]() { return beginLogCapture(); },
      [&]() { endLogCapture(); });

    registerMcpAppTools(*m_dispatcher, *this);
    registerMcpMapTools(*m_dispatcher);
    registerMcpViewTools(*m_dispatcher, *this);

    m_server = std::make_unique<mcp::Server>(
      mcp::ServerInfo{
        .name = "trenchbroom",
        .title = "TrenchBroom",
        .version = getBuildVersion(),
        .instructions = Instructions,
      },
      m_registry);
    m_transport = std::make_unique<mcp::HttpTransport>(
      [&](const mcp::HttpRequest& request, const quint16 localPort) {
        return m_server->handle(request, localPort);
      });

    m_notifierConnection +=
      PreferenceManager::instance().preferenceDidChangeNotifier.connect(
        this, &Impl::preferenceDidChange);
  }

  ~Impl() override
  {
    // the server and transport use the dispatcher, the dispatcher uses the states
    m_transport.reset();
    m_server.reset();
    m_dispatcher.reset();
    m_windows.clear();
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void start(const std::optional<quint16> port)
  {
    if (port)
    {
      m_commandLinePort = port;
    }
    apply();
  }

  void stop()
  {
    m_commandLinePort.reset();
    m_transport->close();
    m_lastError.clear();
    m_statusDidChangeNotifier();
  }

  bool isListening() const { return m_transport->isListening(); }

  quint16 port() const { return m_transport->port(); }

  const QString& lastError() const { return m_lastError; }

  bool isCommandLineOverride() const { return m_commandLinePort.has_value(); }

  QJsonObject callTool(const QString& name, const QJsonObject& arguments)
  {
    return m_dispatcher->call(name, arguments);
  }

public: // McpAppHost
  AppController& appController() override { return m_appController; }

  McpToolDispatcher& dispatcher() override { return *m_dispatcher; }

  McpResult<McpTarget> target(const McpToolKind kind) override
  {
    auto* window = currentTarget();
    if (!window)
    {
      return toolError(
        "no_document", "No map is open. Call new_map or open_map.", "call new_map");
    }

    if (const auto busy = requireNoModalUi(); busy.is_error())
    {
      return errorOf(busy);
    }

    auto& map = window->document().map();
    if (kind == McpToolKind::Mutate || kind == McpToolKind::History)
    {
      if (map.commandProcessor().isTransactionActive())
      {
        return toolError(
          "editor_busy",
          "the user is editing the map",
          "retry in a moment",
          {{"reason", "transaction_active"}});
      }
      if (window->toolBox().anyModalToolActive())
      {
        return toolError(
          "editor_busy",
          "a modal tool is active",
          "ask the user to exit the vertex/clip tool",
          {{"reason", "modal_tool_active"}});
      }
      if (!map.editorContext().canChangeSelection())
      {
        return toolError(
          "editor_busy",
          "the selection cannot be changed right now",
          "ask the user to finish the current tool operation",
          {{"reason", "selection_blocked"}});
      }
    }

    return McpTarget{window, &stateFor(*window)};
  }

  McpResult<void> requireNoModalUi() override
  {
    if (QApplication::activeModalWidget())
    {
      return toolError(
        "editor_busy", "a modal dialog is open", ModalHint, {{"reason", "modal_dialog"}});
    }
    if (QApplication::activePopupWidget())
    {
      return toolError(
        "editor_busy", "a popup is open", ModalHint, {{"reason", "popup"}});
    }
    return kdl::void_success;
  }

  QString documentId(MapWindow& window) override { return record(window).documentId; }

  MapWindow* findDocument(const QString& documentId) override
  {
    for (auto* window : m_appController.mapWindowManager().mapWindows())
    {
      if (record(*window).documentId == documentId)
      {
        return window;
      }
    }
    return nullptr;
  }

  void pin(MapWindow* window) override { m_pinned = window; }

  MapWindow* currentTarget() override
  {
    const auto windows = m_appController.mapWindowManager().mapWindows();
    if (m_pinned && std::ranges::find(windows, m_pinned.data()) != windows.end())
    {
      return m_pinned.data();
    }
    return m_appController.mapWindowManager().topMapWindow();
  }

  void forgetDocumentState(MapWindow& window) override
  {
    if (const auto it = m_windows.find(&window); it != m_windows.end())
    {
      it->second.state.reset();
    }
  }

  const std::vector<McpLogLine>* capturedLog() override
  {
    return m_teeLogger ? &m_teeLogger->lines() : nullptr;
  }

private:
  WindowRecord& record(MapWindow& window)
  {
    if (const auto it = m_windows.find(&window); it != m_windows.end())
    {
      return it->second;
    }

    auto& record = m_windows[&window];
    record.documentId = QString{"d%1"}.arg(m_nextDocumentId++);
    // reloading or reverting replaces the map, so its ids start again
    record.notifierConnection += window.document().documentWasLoadedNotifier.connect(
      [this, windowPointer = &window]() { forgetDocumentState(*windowPointer); });
    QObject::connect(
      &window, &QObject::destroyed, &m_context, [this, windowPointer = &window]() {
        m_windows.erase(windowPointer);
      });
    return record;
  }

  McpDocumentState& stateFor(MapWindow& window)
  {
    auto& rec = record(window);
    auto& map = window.document().map();
    if (!rec.state || &rec.state->map != &map)
    {
      rec.state.reset();
      rec.state = std::make_unique<McpDocumentState>(map, rec.documentId);
    }
    return *rec.state;
  }

  std::vector<McpLogLine>* beginLogCapture()
  {
    auto* window = currentTarget();
    if (!window)
    {
      return nullptr;
    }

    m_teeLogger = std::make_unique<McpTeeLogger>(&window->logger());
    m_teeWindow = window;
    window->document().setTargetLogger(m_teeLogger.get());
    return &m_teeLogger->lines();
  }

  void endLogCapture()
  {
    if (m_teeWindow)
    {
      m_teeWindow->document().setTargetLogger(&m_teeWindow->logger());
    }
    m_teeWindow.clear();
    m_teeLogger.reset();
  }

  void apply()
  {
    doApply();
    m_statusDidChangeNotifier();
  }

  void doApply()
  {
    auto port = std::optional<quint16>{};
    if (m_commandLinePort)
    {
      port = m_commandLinePort;
    }
    else if (pref(Preferences::McpServerEnabled))
    {
      const auto prefPort = pref(Preferences::McpServerPort);
      if (prefPort < 0 || prefPort > 65535)
      {
        m_transport->close();
        m_lastError = QString{"MCP server not started: invalid port %1"}.arg(prefPort);
        printToStderr(m_lastError);
        return;
      }
      port = quint16(prefPort);
    }

    if (!port)
    {
      m_transport->close();
      m_lastError.clear();
      return;
    }

    if (m_transport->isListening() && (*port == 0 || m_transport->port() == *port))
    {
      return;
    }

    m_transport->close();
    if (const auto result = m_transport->listen(*port); result.is_success())
    {
      m_lastError.clear();
      printToStderr(
        QString{"MCP server listening on %1"}.arg(mcpServerUrl(result.value())));
    }
    else
    {
      m_lastError =
        QString{"MCP server not started: %1"}.arg(std::get<QString>(result.error()));
      printToStderr(m_lastError);
    }
  }

  void preferenceDidChange(const std::filesystem::path& path)
  {
    if (
      path == Preferences::McpServerEnabled.path
      || path == Preferences::McpServerPort.path)
    {
      // the user's choice takes over from the command line
      m_commandLinePort.reset();
      apply();
    }
  }
};

QString mcpServerUrl(const quint16 port)
{
  return QString{"http://127.0.0.1:%1/mcp"}.arg(port);
}

QString mcpClaudeAddCommand(const quint16 port)
{
  return QString{"claude mcp add --transport http trenchbroom %1"}.arg(
    mcpServerUrl(port));
}

McpController::McpController(AppController& appController)
  : m_appController{appController}
  , m_impl{std::make_unique<Impl>(appController, statusDidChangeNotifier)}
{
  m_appController.setMcpController(this);
}

McpController::~McpController()
{
  if (m_appController.mcpController() == this)
  {
    m_appController.setMcpController(nullptr);
  }
}

void McpController::start(const std::optional<quint16> port)
{
  m_impl->start(port);
}

void McpController::stop()
{
  m_impl->stop();
}

bool McpController::isListening() const
{
  return m_impl->isListening();
}

quint16 McpController::port() const
{
  return m_impl->port();
}

QString McpController::lastError() const
{
  return m_impl->lastError();
}

bool McpController::isCommandLineOverride() const
{
  return m_impl->isCommandLineOverride();
}

QString McpController::url() const
{
  return isListening() ? mcpServerUrl(port()) : QString{};
}

McpToolDispatcher& McpController::dispatcher()
{
  return m_impl->dispatcher();
}

QJsonObject McpController::callTool(const QString& name, const QJsonObject& arguments)
{
  return m_impl->callTool(name, arguments);
}

} // namespace tb::ui
