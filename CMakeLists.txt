cmake_minimum_required(VERSION 3.13)
project(ctrmml_cmd LANGUAGES C CXX)

# Allow third-party projects with older cmake_minimum_required.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# libvgm configuration: enable audio + emulation for native playback
set(BUILD_LIBAUDIO ON CACHE BOOL "" FORCE)
set(BUILD_LIBEMU ON CACHE BOOL "" FORCE)
set(BUILD_LIBPLAYER ON CACHE BOOL "" FORCE)
set(BUILD_PLAYER OFF CACHE BOOL "" FORCE)
set(BUILD_VGM2WAV OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBRARY_TYPE "STATIC" CACHE STRING "STATIC" FORCE)
# trim utils features we don't need
set(UTIL_THREADING ON CACHE BOOL "" FORCE)
set(UTIL_CHARSET_CONV ON CACHE BOOL "" FORCE)
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

set(CTRMML_CMD_LIB_SOURCES
  src/highlight_tracker.cpp
  src/mml_compile.cpp
  src/mdslink_tool.cpp
  src/rom_builder.cpp
  "${CTRMML_TEMPLATE_SRC}"
  src/vgm_audio_renderer.cpp
  src/audio_output.cpp
  src/vgm_export.cpp
  src/wav_export.cpp
  src/ctrmml_cmd.cpp
  src/ctrmml_cmd_c_api.cpp)

add_library(ctrmml-cmd-lib STATIC ${CTRMML_CMD_LIB_SOURCES})

target_include_directories(ctrmml-cmd-lib PUBLIC
  src
  ${ctrmml_SOURCE_DIR}/src
  ${libvgm_SOURCE_DIR}
  ${libvgm_SOURCE_DIR}/audio
  ${libvgm_SOURCE_DIR}/emu
  ${libvgm_SOURCE_DIR}/utils
  ${libvgm_SOURCE_DIR}/player)

target_link_libraries(ctrmml-cmd-lib PRIVATE
  ctrmml
  vgm-utils
  vgm-audio
  vgm-emu
  vgm-player)

target_compile_definitions(ctrmml-cmd-lib PRIVATE -DLOCAL_LIBVGM)

if(MSVC)
  target_compile_options(ctrmml-cmd-lib PRIVATE /FIstring)
endif()

add_executable(ctrmml-cmd
  src/main.cpp)

target_link_libraries(ctrmml-cmd PRIVATE ctrmml-cmd-lib)

if(WIN32)
  target_link_libraries(ctrmml-cmd PRIVATE winmm ole32 dsound uuid)
endif()
