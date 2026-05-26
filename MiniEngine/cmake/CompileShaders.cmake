# Reproduces MiniEngine's FxCompile build step in CMake.
#
# Each .hlsl in SHADER_DIR is compiled with dxc to a C header
#   ${CMAKE_CURRENT_BINARY_DIR}/CompiledShaders/<name>.h
# exposing a byte array named g_p<name>, which the C++ includes as
#   #include "CompiledShaders/<name>.h"
#
# Shader profile is inferred from the filename suffix (MiniEngine convention):
# *PS -> ps, *VS -> vs, *GS/*HS/*DS/*MS/*AS likewise; everything else defaults to
# compute (cs), matching Build.props. Settings come from the .props: shader model
# 6.2, HLSL 2021 (-HV 2021), entry point `main`. .hlsli are includes and are not
# compiled (the *.hlsl glob excludes them).
#
# TODO(windows): a handful of shaders that do not follow the *XS suffix convention,
# or that the .vcxproj compiles multiple times with different -D defines, need
# explicit handling -- cross-check against Core.vcxproj / Model.vcxproj FxCompile
# entries and add overrides here.

function(miniengine_compile_shaders TARGET SHADER_DIR)
  file(GLOB _shaders CONFIGURE_DEPENDS ${SHADER_DIR}/*.hlsl)
  set(_out_root ${CMAKE_CURRENT_BINARY_DIR})
  set(_headers "")

  foreach(_shader ${_shaders})
    get_filename_component(_name ${_shader} NAME_WE)

    if(_name MATCHES "PS$")
      set(_profile "ps_6_2")
    elseif(_name MATCHES "VS$")
      set(_profile "vs_6_2")
    elseif(_name MATCHES "GS$")
      set(_profile "gs_6_2")
    elseif(_name MATCHES "HS$")
      set(_profile "hs_6_2")
    elseif(_name MATCHES "DS$")
      set(_profile "ds_6_2")
    elseif(_name MATCHES "MS$")
      set(_profile "ms_6_2")
    elseif(_name MATCHES "AS$")
      set(_profile "as_6_2")
    else()
      set(_profile "cs_6_2")
    endif()

    set(_header "${_out_root}/CompiledShaders/${_name}.h")
    add_custom_command(
      OUTPUT ${_header}
      COMMAND ${DXC_EXECUTABLE} -nologo -T ${_profile} -E main -HV 2021
              -I ${SHADER_DIR}
              $<$<CONFIG:Debug>:-Zi> $<$<CONFIG:Debug>:-Qembed_debug>
              -Fh ${_header} -Vn g_p${_name} ${_shader}
      MAIN_DEPENDENCY ${_shader}
      COMMENT "dxc ${_name} (${_profile})"
      VERBATIM)
    list(APPEND _headers ${_header})
  endforeach()

  add_custom_target(${TARGET}_shaders DEPENDS ${_headers})
  add_dependencies(${TARGET} ${TARGET}_shaders)
  target_include_directories(${TARGET} PRIVATE ${_out_root})
endfunction()
