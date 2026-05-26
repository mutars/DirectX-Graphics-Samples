# Toolchain + third-party wiring for the MiniEngine CMake build. Windows + MSVC
# only. Authored from the .vcxproj/.props spec. The NuGet-sourced dependencies
# cannot be fetched or built on Linux -- they are located here from a restored
# packages directory, with TODOs for the Windows build machine.

# --- dxc (HLSL 2021, shader model 6.2) ---------------------------------------
find_program(DXC_EXECUTABLE NAMES dxc
  HINTS
    "$ENV{WindowsSdkVerBinPath}x64"
    "$ENV{WindowsSdkDir}bin/x64"
  DOC "DirectX Shader Compiler (dxc.exe)")
if(NOT DXC_EXECUTABLE)
  message(WARNING "dxc.exe not found -- set -DDXC_EXECUTABLE=<path>. "
                  "Shader compilation will fail until then.")
endif()

# --- NuGet packages ----------------------------------------------------------
# MiniEngine restores these via packages.config; CMake does not run NuGet. Restore
# once, then point MINIENGINE_PACKAGES_DIR at the folder. Expected ids/versions:
#   Microsoft.Direct3D.D3D12       1.618.3      (Agility SDK headers + D3D12Core redist)
#   WinPixEventRuntime             1.0.240308001
#   directxmesh_desktop_win10      2024.10.29.1
#   directxtex_desktop_win10       2024.10.29.1
#   zlib-msvc-x64                  1.2.11.8900
# Source-available alternative: FetchContent DirectXMesh / DirectXTex / zlib (all
# ship CMake) instead of restoring those three packages.
set(MINIENGINE_PACKAGES_DIR "${CMAKE_CURRENT_LIST_DIR}/../../Packages" CACHE PATH
    "Root of restored NuGet packages")

# Resolve a versioned package dir by glob (tolerant of version bumps).
function(_mini_pkg_dir out_var glob)
  file(GLOB _hits "${MINIENGINE_PACKAGES_DIR}/${glob}")
  if(_hits)
    list(GET _hits 0 _first)
    set(${out_var} "${_first}" PARENT_SCOPE)
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()

_mini_pkg_dir(MINI_AGILITY_DIR "Microsoft.Direct3D.D3D12.*")
_mini_pkg_dir(MINI_WINPIX_DIR  "WinPixEventRuntime.*")
_mini_pkg_dir(MINI_DXMESH_DIR  "directxmesh_desktop_win10.*")
_mini_pkg_dir(MINI_DXTEX_DIR   "directxtex_desktop_win10.*")
_mini_pkg_dir(MINI_ZLIB_DIR    "zlib-msvc-x64.*")

# Common third-party + system libs shared by all MiniEngine targets.
add_library(miniengine_thirdparty INTERFACE)
target_link_libraries(miniengine_thirdparty INTERFACE
  d3d12 dxgi dxguid d3d11 winmm comctl32)

if(MINI_AGILITY_DIR)
  target_include_directories(miniengine_thirdparty INTERFACE
    "${MINI_AGILITY_DIR}/build/native/include")
endif()
if(MINI_WINPIX_DIR)
  target_include_directories(miniengine_thirdparty INTERFACE
    "${MINI_WINPIX_DIR}/Include/WinPixEventRuntime")
  target_link_libraries(miniengine_thirdparty INTERFACE
    "${MINI_WINPIX_DIR}/bin/x64/WinPixEventRuntime.lib")
endif()

# TODO(windows):
#  - DirectXMesh / DirectXTex include + lib (used by Model and ModelViewer) from
#    MINI_DXMESH_DIR / MINI_DXTEX_DIR (or FetchContent the source repos).
#  - zlibstatic.lib from MINI_ZLIB_DIR for ModelViewer.
#  - Stage runtime DLLs next to ModelViewer.exe at build time: the Agility SDK
#    D3D12Core.dll (+ d3d12SDKLayers.dll) under a D3D12/ subfolder, and
#    WinPixEventRuntime.dll.
