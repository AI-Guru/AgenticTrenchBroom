add_executable(TbMcpLibTest)
EMBED_UTF8_MANIFEST(TbMcpLibTest)
add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

target_sources(TbMcpLibTest PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Http.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tst_JsonRpc.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tst_JsonSchema.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Server.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Tool.cpp"
)

target_link_libraries(TbMcpLibTest
  PRIVATE
    CompilerConfig
    TbMcpLib
    Catch2::Catch2WithMain
    Qt6::Network
    Qt6::Test
)

set_target_properties(TbMcpLibTest PROPERTIES AUTOMOC TRUE)

include(Catch)
catch_discover_tests(TbMcpLibTest)
