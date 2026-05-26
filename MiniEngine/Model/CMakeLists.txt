# MiniEngine Model -> static library `mini_model`.

file(GLOB mini_model_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_library(mini_model STATIC ${mini_model_sources})

target_include_directories(mini_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mini_model PUBLIC mini_core)

# The offline mesh/texture path (MeshConvert.cpp, BuildH3D.cpp) uses DirectXMesh /
# DirectXTex. TODO(windows): add their include dirs + libs (MINI_DXMESH_DIR /
# MINI_DXTEX_DIR), or exclude the converter-only sources if ModelViewer needs only
# runtime .h3d loading.
miniengine_compile_shaders(mini_model ${CMAKE_CURRENT_SOURCE_DIR}/Shaders)
