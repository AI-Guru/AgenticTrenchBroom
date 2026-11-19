add_executable(TbUiLibTest)
EMBED_UTF8_MANIFEST(TbUiLibTest)

target_sources(TbUiLibTest
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpAppTestUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpTestUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RunAllTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ActionContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ActionInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_AppController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Actions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompilationDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompilationProfileManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompilationRunner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CompilationTaskListBox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityDefinitionFileChooser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityPropertyGrid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityPropertyModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_InputEvent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_IssueBrowserView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_KeyboardShortcutUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LaunchGameEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LaunchGameEngineDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LayerListBox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapViewBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpAppTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpCheckpoints.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpDeterminism.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpDocumentState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpLogCapture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpMapCreateTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpMapEditTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpMapEvalTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpMapHistoryTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpMapReadTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpMapStructureTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpNodeRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpPreconditions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpSchemas.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpSelectionGuard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpToolDispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpToolFuzz.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_McpViewTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MultiCompletionLineEdit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QPathUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QPreferenceStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QPreferenceStoreUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QStringUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_QVecUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_RecentDocuments.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ScaleToolPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SearchPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SignalDelayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_SystemPaths.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TextOutputAdapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_UpdateVersion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ViewEditor.cpp

  PUBLIC FILE_SET cmd_tool TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

target_link_libraries(TbUiLibTest PRIVATE 
  CompilerConfig
  PrecompileQtHeaders
  PrecompileStdHeaders
  Catch2::Catch2WithMain
  Qt6::Network
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  Qt6::Svg
  Qt6::Test
  Qt6::Widgets
  TbAppTestUtilsLib
  TbBaseTestUtilsLib
  TbElLib
  TbElTestUtilsLib
  TbFsTestUtilsLib
  TbMcpLib
  TbMdlTestUtilsLib
  TbPreferencesLib
  TbRenderLib
  TbUiLib
  TbUiTestUtilsLib
  UpdateLib
)

set_target_properties(TbUiLibTest PROPERTIES AUTOMOC TRUE)

add_dependencies(TbUiLibTest CmdTool)
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/CmdTool.h"
  CONTENT "#define CMD_TOOL_PATH \"$<TARGET_FILE:CmdTool>\"")

# Copy fixtures
set(TEST_FIXTURE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixture)
set(TEST_FIXTURE_DEST_DIR $<TARGET_FILE_DIR:TbUiLibTest>/fixture)

add_custom_command(TARGET TbUiLibTest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${TEST_FIXTURE_DEST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TEST_FIXTURE_SOURCE_DIR}" "${TEST_FIXTURE_DEST_DIR}/test"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games" "${TEST_FIXTURE_DEST_DIR}/games"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games-testing" "${TEST_FIXTURE_DEST_DIR}/games"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/graphics/images" "$<TARGET_FILE_DIR:TbUiLibTest>/images"
  # tests that show map windows initialize OpenGL, which loads shaders and fonts
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/shader" "$<TARGET_FILE_DIR:TbUiLibTest>/shader"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/fonts" "$<TARGET_FILE_DIR:TbUiLibTest>/fonts"
)

include(Catch)
catch_discover_tests(TbUiLibTest)
