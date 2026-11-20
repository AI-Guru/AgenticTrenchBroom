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

#include "ui/McpPreferencePane.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

#include "base/PreferenceManager.h"
#include "prefs/Preferences.h"
#include "ui/AppController.h"
#include "ui/FormWithSectionsLayout.h"
#include "ui/McpController.h"
#include "ui/QStyleUtils.h"
#include "ui/ViewConstants.h"

#include <algorithm>

namespace tb::ui
{
namespace
{

constexpr auto MaxPort = 65535;

QHBoxLayout* makeCopyRow(QLineEdit* lineEdit, QPushButton* button)
{
  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(LayoutConstants::MediumHMargin);
  layout->addWidget(lineEdit, 1);
  layout->addWidget(button);
  return layout;
}

QLineEdit* makeReadOnlyLineEdit()
{
  auto* lineEdit = new QLineEdit{};
  lineEdit->setReadOnly(true);
  return lineEdit;
}

} // namespace

McpPreferencePane::McpPreferencePane(AppController& appController, QWidget* parent)
  : PreferencePane{parent}
  , m_appController{appController}
{
  createGui();
  connectObservers();
  updateControls();
}

QString McpPreferencePane::statusText() const
{
  return m_status->text();
}

QString McpPreferencePane::urlText() const
{
  return m_url->text();
}

QString McpPreferencePane::commandText() const
{
  return m_command->text();
}

void McpPreferencePane::createGui()
{
  auto* mcpPreferences = createMcpPreferences();

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(QMargins{});
  layout->setSpacing(0);
  layout->addSpacing(LayoutConstants::NarrowVMargin);
  layout->addWidget(mcpPreferences, 1);
  layout->addSpacing(LayoutConstants::MediumVMargin);

  createScrollableContent(layout);
}

QWidget* McpPreferencePane::createMcpPreferences()
{
  auto* info = new QLabel{tr(
    R"(The MCP server lets AI agents such as Claude Code inspect and edit your maps through the Model Context Protocol.
It only accepts connections from this computer, but any program running on this computer can connect while the server is enabled.
Agent edits are regular undo steps named "MCP: ...". Agents cannot use the editor while a dialog is open.)")};
  info->setWordWrap(true);
  setInfoStyle(info);

  m_enabled = new QCheckBox{};
  m_enabled->setObjectName("mcpServerEnabled");
  connect(m_enabled, &QCheckBox::checkStateChanged, this, [&](const auto state) {
    auto& prefs = PreferenceManager::instance();
    prefs.set(Preferences::McpServerEnabled, state == Qt::Checked);
    updateControls();
  });

  m_port = new QSpinBox{};
  m_port->setObjectName("mcpServerPort");
  m_port->setRange(0, MaxPort);
  m_port->setSpecialValueText(tr("Any free port"));
  // only apply the port when the user is done typing it, not for every digit
  m_port->setKeyboardTracking(false);
  connect(m_port, &QSpinBox::valueChanged, this, [&](const int value) {
    auto& prefs = PreferenceManager::instance();
    prefs.set(Preferences::McpServerPort, value);
    updateControls();
  });

  m_status = new QLabel{};
  m_status->setObjectName("mcpServerStatus");
  m_status->setWordWrap(true);
  // the error comes from the network layer and must not be interpreted as rich text
  m_status->setTextFormat(Qt::PlainText);
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

  m_url = makeReadOnlyLineEdit();
  m_url->setObjectName("mcpServerUrl");
  m_copyUrl = new QPushButton{tr("Copy")};
  m_copyUrl->setObjectName("mcpCopyUrl");
  connect(m_copyUrl, &QPushButton::clicked, this, [&]() {
    QGuiApplication::clipboard()->setText(m_url->text());
  });

  m_command = makeReadOnlyLineEdit();
  m_command->setObjectName("mcpClaudeCommand");
  m_copyCommand = new QPushButton{tr("Copy")};
  m_copyCommand->setObjectName("mcpCopyCommand");
  connect(m_copyCommand, &QPushButton::clicked, this, [&]() {
    QGuiApplication::clipboard()->setText(m_command->text());
  });

  auto* commandInfo = new QLabel{tr(
    R"(Run this command in a terminal to add TrenchBroom to Claude Code. Other MCP clients need the URL and the transport "Streamable HTTP".)")};
  commandInfo->setWordWrap(true);
  setInfoStyle(commandInfo);

  auto* layout = new FormWithSectionsLayout{};
  layout->setContentsMargins(
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin);
  layout->setVerticalSpacing(LayoutConstants::WideVMargin);

  layout->addSection(tr("MCP Server"));
  layout->addRow(info);
  layout->addRow(tr("Enable MCP server"), m_enabled);
  layout->addRow(tr("Port"), m_port);
  layout->addRow(tr("Status"), m_status);

  layout->addSection(tr("Connecting an Agent"));
  layout->addRow(tr("URL"), makeCopyRow(m_url, m_copyUrl));
  layout->addRow(tr("Claude Code"), makeCopyRow(m_command, m_copyCommand));
  layout->addRow(commandInfo);

  auto* widget = new QWidget{};
  widget->setLayout(layout);
  return widget;
}

void McpPreferencePane::connectObservers()
{
  if (auto* controller = m_appController.mcpController())
  {
    m_notifierConnection +=
      controller->statusDidChangeNotifier.connect([&]() { updateControls(); });
  }
}

quint16 McpPreferencePane::displayedPort() const
{
  if (const auto* controller = m_appController.mcpController();
      controller && controller->isListening())
  {
    return controller->port();
  }

  auto& prefs = PreferenceManager::instance();
  return quint16(
    std::clamp(prefs.getPendingValue(Preferences::McpServerPort), 0, MaxPort));
}

bool McpPreferencePane::canResetToDefaults()
{
  return true;
}

void McpPreferencePane::doResetToDefaults()
{
  auto& prefs = PreferenceManager::instance();
  prefs.resetToDefault(Preferences::McpServerEnabled);
  prefs.resetToDefault(Preferences::McpServerPort);
  updateControls();
}

void McpPreferencePane::updateControls()
{
  const auto enabledBlocker = QSignalBlocker{m_enabled};
  const auto portBlocker = QSignalBlocker{m_port};

  auto& prefs = PreferenceManager::instance();
  const auto pendingEnabled = prefs.getPendingValue(Preferences::McpServerEnabled);
  const auto pendingPort = prefs.getPendingValue(Preferences::McpServerPort);
  const auto hasPendingChanges =
    pendingEnabled != prefs.get(Preferences::McpServerEnabled)
    || pendingPort != prefs.get(Preferences::McpServerPort);

  m_enabled->setChecked(pendingEnabled);
  m_port->setValue(std::clamp(pendingPort, 0, MaxPort));

  const auto* controller = m_appController.mcpController();
  const auto listening = controller && controller->isListening();

  auto status = QString{};
  if (!controller)
  {
    status = tr("The MCP server is not available.");
  }
  else if (listening)
  {
    status = tr("Running at %1").arg(controller->url());
    if (controller->isCommandLineOverride())
    {
      status += tr(" (started with --mcp-port; changing a setting here takes over)");
    }
  }
  else if (!controller->lastError().isEmpty())
  {
    status = controller->lastError();
  }
  else
  {
    status = tr("Not running");
  }
  if (hasPendingChanges)
  {
    status += tr(". Your changes take effect when you click Apply or OK.");
  }
  m_status->setText(status);
  if (controller && !listening && !controller->lastError().isEmpty())
  {
    setErrorStyle(m_status);
  }
  else
  {
    setDefaultStyle(m_status);
  }

  const auto port = displayedPort();
  const auto hasPort = listening || port != 0;
  m_url->setText(hasPort ? mcpServerUrl(port) : QString{});
  m_url->setPlaceholderText(tr("Start the server to see the port"));
  m_command->setText(hasPort ? mcpClaudeAddCommand(port) : QString{});
  m_command->setPlaceholderText(tr("Start the server to see the port"));
  m_copyUrl->setEnabled(hasPort);
  m_copyCommand->setEnabled(hasPort);
}

bool McpPreferencePane::validate()
{
  return true;
}

} // namespace tb::ui
