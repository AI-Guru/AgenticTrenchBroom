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

#include "AppTestUtils.h"

#include <QApplication>
#include <QWidget>

#include "ui/WelcomeWindow.h"

#include <chrono>

namespace tb::ui
{

WelcomeWindow* findWelcomeWindow()
{
  for (auto* widget : QApplication::topLevelWidgets())
  {
    if (auto* welcomeWindow = dynamic_cast<WelcomeWindow*>(widget))
    {
      return welcomeWindow;
    }
  }
  return nullptr;
}

bool isWelcomeWindowVisible()
{
  const auto* welcomeWindow = findWelcomeWindow();
  return welcomeWindow != nullptr && welcomeWindow->isVisible();
}

ModalWatchdog::ModalWatchdog()
{
  using namespace std::chrono_literals;

  QObject::connect(&m_timer, &QTimer::timeout, [&]() {
    if (auto* modal = QApplication::activeModalWidget())
    {
      ++m_modalCount;
      modal->close();
    }
  });
  m_timer.start(10ms);
}

int ModalWatchdog::modalCount() const
{
  return m_modalCount;
}

} // namespace tb::ui
