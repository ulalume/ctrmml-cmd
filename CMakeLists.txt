cmake_minimum_required(VERSION 3.13)
project(ctrmml_cmd LANGUAGES C CXX)

# Allow third-party projects with older cmake_minimum_required.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Emscripten exception handling — must be set BEFORE FetchContent so that
# dependent libraries (ctrmml, libvgm) are compiled with the same ABI.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  add_compile_options(-fexceptions)
endif()

set(CTRMML_TEMPLATE_ROM "${CMAKE_SOURCE_DIR}/assets/template.bin")
if(NOT EXISTS "${CTRMML_TEMPLATE_ROM}")
  message(FATAL_ERROR "assets/template.bin not found. A marker-enabled template ROM is required.")
endif()

set(CTRMML_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(CTRMML_TEMPLATE_SRC "${CTRMML_GENERATED_DIR}/template_rom_data.cpp")
file(MAKE_DIRECTORY "${CTRMML_GENERATED_DIR}")

add_custom_command(
  OUTPUT "${CTRMML_TEMPLATE_SRC}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE=${CTRMML_TEMPLATE_ROM}
    -DOUTPUT_FILE=${CTRMML_TEMPLATE_SRC}
    -P "${CMAKE_SOURCE_DIR}/cmake/embed_template.cmake"
  DEPENDS "${CTRMML_TEMPLATE_ROM}" "${CMAKE_SOURCE_DIR}/cmake/embed_template.cmake"
  COMMENT "Embedding assets/template.bin into ctrmml-cmd binary"
  VERBATIM)

set_source_files_properties("${CTRMML_TEMPLATE_SRC}" PROPERTIES GENERATED TRUE)

include(FetchContent)
FetchContent_Declare(
  ctrmml
  GIT_REPOSITORY https://github.com/superctr/ctrmml
  GIT_TAG 821d54b
)
FetchContent_Declare(
  libvgm
  GIT_REPOSITORY https://github.com/ValleyBell/libvgm
  GIT_TAG 0e349256338144205303a1495ddc788a854be1eb
)

# libvgm configuration
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(BUILD_LIBAUDIO OFF CACHE BOOL "" FORCE)
else()
  set(BUILD_LIBAUDIO ON CACHE BOOL "" FORCE)
endif()
set(BUILD_LIBEMU ON CACHE BOOL "" FORCE)
set(BUILD_LIBPLAYER ON CACHE BOOL "" FORCE)
set(BUILD_PLAYER OFF CACHE BOOL "" FORCE)
set(BUILD_VGM2WAV OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBRARY_TYPE "STATIC" CACHE STRING "STATIC" FORCE)
# trim utils features we don't need
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(UTIL_THREADING OFF CACHE BOOL "" FORCE)
  set(UTIL_CHARSET_CONV OFF CACHE BOOL "" FORCE)
else()
  set(UTIL_THREADING ON CACHE BOOL "" FORCE)
  set(UTIL_CHARSET_CONV ON CACHE BOOL "" FORCE)
endif()
# ensure required sound cores are built
set(SNDEMU__ALL ON CACHE BOOL "" FORCE)
set(SNDEMU_SN76496_ALL ON CACHE BOOL "" FORCE)
set(SNDEMU_YM2612_ALL ON CACHE BOOL "" FORCE)

if(WIN32)
  set(UTIL_CHARCNV_ICONV OFF CACHE BOOL "" FORCE)
  set(UTIL_CHARCNV_WINAPI ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(ctrmml libvgm)

if(TARGET ctrmml)
  if(MSVC)
    target_compile_options(ctrmml PRIVATE /FIstring)
  endif()
endif()

# Do not build native CLI tools from ctrmml if present.
foreach(tgt mmlc mdslink)
  if(TARGET ${tgt})
    set_property(TARGET ${tgt} PROPERTY EXCLUDE_FROM_ALL TRUE)
    set_property(TARGET ${tgt} PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
  endif()
endforeach()

set(CTRMML_CMD_CORE_SOURCES
  src/highlight_tracker.cpp
  src/mml_compile.cpp
  src/mdslink_tool.cpp
  src/rom_builder.cpp
  "${CTRMML_TEMPLATE_SRC}"
  src/vgm_audio_renderer.cpp
  src/vgm_export.cpp
  src/wav_export.cpp
  src/ctrmml_cmd.cpp
  src/ctrmml_cmd_c_api.cpp)

set(CTRMML_CMD_INCLUDE_DIRS
  src
  ${ctrmml_SOURCE_DIR}/src
  ${libvgm_SOURCE_DIR}
  ${libvgm_SOURCE_DIR}/emu
  ${libvgm_SOURCE_DIR}/utils
  ${libvgm_SOURCE_DIR}/player)

set(CTRMML_CMD_LINK_LIBS
  ctrmml
  vgm-utils
  vgm-emu
  vgm-player)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  # ---------- WASM build ----------

  # Emscripten zlib (shipped with emsdk)
  if(NOT TARGET ZLIB::ZLIB)
    if(DEFINED ENV{EM_CACHE})
      set(_EM_SYSROOT "$ENV{EM_CACHE}/sysroot")
    else()
      execute_process(COMMAND em-config CACHE OUTPUT_VARIABLE _EM_CACHE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
      set(_EM_SYSROOT "${_EM_CACHE_DIR}/sysroot")
    endif()
    set(_EM_ZLIB "${_EM_SYSROOT}/lib/wasm32-emscripten/libz.a")
    if(EXISTS "${_EM_ZLIB}")
      add_library(ZLIB::ZLIB UNKNOWN IMPORTED)
      set_target_properties(ZLIB::ZLIB PROPERTIES
        IMPORTED_LOCATION "${_EM_ZLIB}"
        INTERFACE_INCLUDE_DIRECTORIES ""
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ""
      )
    endif()
  endif()

  add_library(ctrmml-cmd-lib STATIC
    ${CTRMML_CMD_CORE_SOURCES}
    src/ctrmml_cmd_wasm_api.cpp)

  target_include_directories(ctrmml-cmd-lib PUBLIC ${CTRMML_CMD_INCLUDE_DIRS})
  target_link_libraries(ctrmml-cmd-lib PRIVATE ${CTRMML_CMD_LINK_LIBS})
  target_compile_definitions(ctrmml-cmd-lib PRIVATE -DLOCAL_LIBVGM)

  add_executable(ctrmml-cmd-wasm src/ctrmml_cmd_wasm_api.cpp)
  target_link_libraries(ctrmml-cmd-wasm PRIVATE ctrmml-cmd-lib)
  set_target_properties(ctrmml-cmd-wasm PROPERTIES
    OUTPUT_NAME "ctrmml_cmd"
    SUFFIX ".js")

  target_link_options(ctrmml-cmd-wasm PRIVATE
    -fexceptions
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sEXPORTED_FUNCTIONS=_malloc,_free,_ctrmml_cmd_wasm_compile,_ctrmml_cmd_wasm_get_compile_error,_ctrmml_cmd_wasm_check_json,_ctrmml_cmd_wasm_start_playback,_ctrmml_cmd_wasm_stop_playback,_ctrmml_cmd_wasm_is_playing,_ctrmml_cmd_wasm_render_audio,_ctrmml_cmd_wasm_get_player_ticks,_ctrmml_cmd_wasm_get_highlights,_ctrmml_cmd_wasm_export_vgm,_ctrmml_cmd_wasm_export_wav,_ctrmml_cmd_wasm_quickrom,_ctrmml_cmd_wasm_mdslink,_ctrmml_cmd_wasm_get_last_error,_ctrmml_cmd_wasm_get_track_data_json"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToUTF8,lengthBytesUTF8,UTF8ToString,HEAPF32,HEAPU32,FS"
    "SHELL:-sENVIRONMENT=web"
    "SHELL:-sMODULARIZE=1"
    "SHELL:-sEXPORT_NAME=createCtrmmlModule"
    "SHELL:-sASSERTIONS=1"
    "SHELL:-sSTRICT=1")

else()
  # ---------- Native build ----------
  add_library(ctrmml-cmd-lib STATIC
    ${CTRMML_CMD_CORE_SOURCES}
    src/audio_output.cpp)

  target_include_directories(ctrmml-cmd-lib PUBLIC
    ${CTRMML_CMD_INCLUDE_DIRS}
    ${libvgm_SOURCE_DIR}/audio)
  target_link_libraries(ctrmml-cmd-lib PRIVATE ${CTRMML_CMD_LINK_LIBS} vgm-audio)
  target_compile_definitions(ctrmml-cmd-lib PRIVATE -DLOCAL_LIBVGM)

  if(MSVC)
    target_compile_options(ctrmml-cmd-lib PRIVATE /FIstring)
  endif()

  add_executable(ctrmml-cmd src/main.cpp)
  target_link_libraries(ctrmml-cmd PRIVATE ctrmml-cmd-lib)

  if(WIN32)
    target_link_libraries(ctrmml-cmd PRIVATE winmm ole32 dsound uuid)
  endif()
endif()
