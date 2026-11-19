/*
 Copyright (C) 2026 Kristian Duske

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
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>

#include "McpAppTestUtils.h"
#include "TestEnvironment.h"
#include "base/Result.h"
#include "gl/GlManager.h"
#include "gl/Resource.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MapWindow.h"

#include "kd/invoke.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

// Simple resource type for testing resource processing
struct TestResource
{
  void upload(gl::Gl&) const {}
  void drop(gl::Gl&) const {}
};

/** Ignores every close event of the objects it filters. */
class CloseEventRefuser : public QObject
{
public:
  int count() const { return m_count; }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override
  {
    if (event->type() == QEvent::Close)
    {
      ++m_count;
      event->ignore();
      return true;
    }
    return QObject::eventFilter(watched, event);
  }

private:
  int m_count = 0;
};

TEST_CASE("MapWindow")
{
  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  auto document = MapDocument::createDocument(
                    appController.environmentConfig(),
                    mdl::QuakeGameInfo,
                    mdl::MapFormat::Valve,
                    vm::bbox3d{8192.0},
                    appController.taskManager(),
                    appController.glManager().resourceManager())
                  | kdl::value();

  auto window = MapWindow{appController, std::move(document)};

  SECTION("load resets grid size dropdown")
  {
    auto* gridChoice = window.findChild<QComboBox*>("MapWindow_GridChoice");
    REQUIRE(gridChoice != nullptr);

    const auto changedGridSize = 5;
    const auto changedGridIndex = changedGridSize - mdl::Grid::MinSize;
    const auto path = getFixtureRoot() / "test/ui/MapDocument/emptyValveMap.map";

    window.setGridSize(changedGridSize);
    QApplication::processEvents();

    REQUIRE(window.document().map().grid().size() == changedGridSize);
    REQUIRE(gridChoice->currentIndex() == changedGridIndex);

    REQUIRE(window.document().load(
      appController.environmentConfig(),
      mdl::QuakeGameInfo,
      mdl::MapFormat::Unknown,
      vm::bbox3d{8192.0},
      path));

    QApplication::processEvents();

    const auto loadedGridSize = window.document().map().grid().size();
    const auto loadedGridIndex = loadedGridSize - mdl::Grid::MinSize;

    CHECK(loadedGridSize != changedGridSize);
    CHECK(gridChoice->currentIndex() == loadedGridIndex);
    CHECK(gridChoice->currentData().toInt() == loadedGridSize);
  }

  SECTION("canReloadAssets returns false when resources need processing")
  {
    using TestResourceT = gl::Resource<TestResource>;

    // Verify initial state: no resources pending processing
    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadAssets());

    // Add a resource that needs processing
    auto testResource = std::make_shared<TestResourceT>(
      []() { return Result<TestResource>{TestResource{}}; });
    appController.glManager().resourceManager().addResource(testResource);

    REQUIRE(appController.glManager().resourceManager().needsProcessing());
    CHECK(!window.canReloadAssets());

    // Process all resources synchronously
    auto testGl = gl::TestGl{};
    const auto processContext = gl::ProcessContext{testGl, [](auto, auto) {}};
    gl::processResourcesSync(appController.glManager().resourceManager(), processContext);

    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadAssets());
  }

  SECTION("closeDocumentDiscardingChanges")
  {
    // MapWindow uses Qt::WA_DeleteOnClose, so closing must not happen on a stack object
    auto closingWindow = QPointer<MapWindow>{new MapWindow{
      appController,
      MapDocument::createDocument(
        appController.environmentConfig(),
        mdl::QuakeGameInfo,
        mdl::MapFormat::Valve,
        vm::bbox3d{8192.0},
        appController.taskManager(),
        appController.glManager().resourceManager())
        | kdl::value()}};
    auto deleteWindow = kdl::invoke_later{[&]() { delete closingWindow.data(); }};

    const auto flushDeferredDelete = []() {
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };

    const auto watchdog = McpModalWatchdog{};

    SECTION("closes a modified window without asking")
    {
      closingWindow->document().map().incModificationCount();
      REQUIRE(closingWindow->document().map().modified());

      CHECK(closingWindow->closeDocumentDiscardingChanges());
      CHECK(watchdog.modalCount() == 0);

      flushDeferredDelete();
      CHECK(closingWindow.isNull());
    }

    SECTION("closes an unmodified window")
    {
      REQUIRE(!closingWindow->document().map().modified());

      CHECK(closingWindow->closeDocumentDiscardingChanges());
      CHECK(watchdog.modalCount() == 0);

      flushDeferredDelete();
      CHECK(closingWindow.isNull());
    }

    SECTION("close still asks to save a modified window")
    {
      closingWindow->document().map().incModificationCount();

      // The watchdog closes the question, which cancels the close
      CHECK(!closingWindow->close());
      CHECK(watchdog.modalCount() == 1);
      flushDeferredDelete();
      REQUIRE(!closingWindow.isNull());
      CHECK(closingWindow->document().map().modified());

      CHECK(closingWindow->closeDocumentDiscardingChanges());
      CHECK(watchdog.modalCount() == 1);
      flushDeferredDelete();
      CHECK(closingWindow.isNull());
    }

    SECTION("only skips the prompt during the call")
    {
      closingWindow->document().map().incModificationCount();

      // Refuse the close before MapWindow::closeEvent is called
      auto refuseClose = CloseEventRefuser{};
      closingWindow->installEventFilter(&refuseClose);

      CHECK(!closingWindow->closeDocumentDiscardingChanges());
      CHECK(refuseClose.count() == 1);
      CHECK(watchdog.modalCount() == 0);

      closingWindow->removeEventFilter(&refuseClose);

      // A later close asks again
      CHECK(!closingWindow->close());
      CHECK(watchdog.modalCount() == 1);
      flushDeferredDelete();
      CHECK(!closingWindow.isNull());
    }
  }
}

} // namespace tb::ui