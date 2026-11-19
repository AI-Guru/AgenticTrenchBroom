add_library(TbMcpLib STATIC)

target_sources(TbMcpLib
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Http.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/JsonRpc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/JsonSchema.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Tool.cpp

  PUBLIC FILE_SET headers TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/Http.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/JsonRpc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/JsonSchema.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/Server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mcp/Tool.h
)

target_link_libraries(TbMcpLib
  PRIVATE
    CompilerConfig
    Qt6::Network
  PUBLIC
    Qt6::Core
    KdLib
)

set_target_properties(TbMcpLib PROPERTIES AUTOMOC TRUE)

add_subdirectory(test)
