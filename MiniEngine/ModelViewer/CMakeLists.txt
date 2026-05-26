# MiniEngine ModelViewer -> the windowed application `ModelViewer.exe`.
# This is the target the vr-test-framework harness injects into.

add_executable(ModelViewer WIN32 ${CMAKE_CURRENT_SOURCE_DIR}/ModelViewer.cpp)

# Core/Model headers + the third-party stack arrive transitively through
# mini_model (PUBLIC -> mini_core -> miniengine_thirdparty).
target_link_libraries(ModelViewer PRIVATE mini_model)

target_link_libraries(ModelViewer PRIVATE
  kernel32 user32 gdi32 winspool comdlg32 advapi32 shell32 ole32 oleaut32 uuid)

# TODO(windows):
#  - link zlibstatic.lib (MINI_ZLIB_DIR); ModelViewer loads zlib-compressed assets.
#  - copy runtime assets beside the exe: ModelViewer/Sponza/ and ModelViewer/Textures/.
#  - stage the Agility SDK D3D12Core.dll (+ d3d12SDKLayers.dll) under a D3D12/
#    subfolder and WinPixEventRuntime.dll next to the exe.
