cmake_minimum_required(VERSION 3.27)

# CMake build for the MiniEngine ModelViewer sample (Core + Model + ModelViewer),
# replacing ModelViewer.sln / the .vcxproj files. Windows + D3D12 + MSVC only;
# the engine source is left pristine -- only build files are added.
#
# Configure/build (Windows):
#   cmake --preset build-debug-msvc
#   cmake --build build/build-debug-msvc
# Produces ModelViewer.exe, the injection target for vr-test-framework.

project(MiniEngine LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "MiniEngine is Windows + D3D12 + MSVC only and cannot build on this platform.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(MiniEngineDependencies)
include(CompileShaders)

add_subdirectory(Core)
add_subdirectory(Model)
add_subdirectory(ModelViewer)
