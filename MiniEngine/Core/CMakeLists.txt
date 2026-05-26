# MiniEngine Core -> static library `mini_core`.

file(GLOB mini_core_sources CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Math/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Util/*.cpp)
# pch.cpp is the MSVC PCH generator; CMake drives the PCH itself (below).
list(REMOVE_ITEM mini_core_sources ${CMAKE_CURRENT_SOURCE_DIR}/pch.cpp)

add_library(mini_core STATIC ${mini_core_sources})

target_include_directories(mini_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/Math
  ${CMAKE_CURRENT_SOURCE_DIR}/Util)

target_compile_definitions(mini_core PUBLIC
  _GAMING_DESKTOP=1
  __WRL_NO_DEFAULT_LIB__
  WIN32
  _WINDOWS
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG RELEASE>)

target_link_libraries(mini_core PUBLIC miniengine_thirdparty)

target_precompile_headers(mini_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pch.h)

miniengine_compile_shaders(mini_core ${CMAKE_CURRENT_SOURCE_DIR}/Shaders)
