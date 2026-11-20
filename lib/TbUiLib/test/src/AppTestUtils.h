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

#include <QTimer>

namespace tb::ui
{
class WelcomeWindow;

/** Returns the welcome window, or nullptr if it was never created. */
WelcomeWindow* findWelcomeWindow();

bool isWelcomeWindowVisible();

/**
 * Detects modal dialogs while it exists. A modal dialog spins its own event loop, so the
 * timer fires while it is shown; the dialog is then closed so that the test does not
 * hang, and the dialog is counted.
 */
class ModalWatchdog
{
public:
  ModalWatchdog();

  int modalCount() const;

private:
  QTimer m_timer;
  int m_modalCount = 0;
};

} // namespace tb::ui
