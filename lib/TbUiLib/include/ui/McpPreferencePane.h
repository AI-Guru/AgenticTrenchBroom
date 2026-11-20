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

#include "base/NotifierConnection.h"
#include "ui/PreferencePane.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace tb::ui
{
class AppController;

/**
 * Lets the user enable the MCP server, choose its port and copy the URL and the command
 * that connects Claude Code to it. Shows whether the server is running.
 *
 * The server follows the preferences, so it is started and stopped as soon as the
 * preferences change (immediately on macOS, on Apply or OK elsewhere).
 */
class McpPreferencePane : public PreferencePane
{
  Q_OBJECT
private:
  AppController& m_appController;
  QCheckBox* m_enabled = nullptr;
  QSpinBox* m_port = nullptr;
  QLabel* m_status = nullptr;
  QLineEdit* m_url = nullptr;
  QPushButton* m_copyUrl = nullptr;
  QLineEdit* m_command = nullptr;
  QPushButton* m_copyCommand = nullptr;

  NotifierConnection m_notifierConnection;

public:
  explicit McpPreferencePane(AppController& appController, QWidget* parent = nullptr);

  /** The status text, e.g. "Running at http://127.0.0.1:6767/mcp". */
  QString statusText() const;

  /** The URL shown in the pane. */
  QString urlText() const;

  /** The claude mcp add command shown in the pane. */
  QString commandText() const;

  bool canResetToDefaults() override;
  void updateControls() override;
  bool validate() override;

private:
  void createGui();
  QWidget* createMcpPreferences();
  void connectObservers();

  /** The port the server listens on, or else the port from the preferences. */
  quint16 displayedPort() const;

  void doResetToDefaults() override;
};

} // namespace tb::ui
