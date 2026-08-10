cmake_minimum_required(VERSION 3.13)
project(ctrmml_cmd LANGUAGES C CXX)

include(CTest)

# Allow third-party projects with older cmake_minimum_required.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Emscripten exception handling — must be set BEFORE FetchContent so that
# dependent libraries (ctrmml) are compiled with the same ABI.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  add_compile_options(-fexceptions)
endif()

set(CTRMML_TEMPLATE_ROM "${CMAKE_CURRENT_SOURCE_DIR}/assets/template.bin")
if(NOT EXISTS "${CTRMML_TEMPLATE_ROM}")
  message(FATAL_ERROR "assets/template.bin not found. A marker-enabled template ROM is required.")
endif()

set(CTRMML_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CTRMML_TEMPLATE_SRC "${CTRMML_GENERATED_DIR}/template_rom_data.cpp")
file(MAKE_DIRECTORY "${CTRMML_GENERATED_DIR}")

add_custom_command(
  OUTPUT "${CTRMML_TEMPLATE_SRC}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE=${CTRMML_TEMPLATE_ROM}
    -DOUTPUT_FILE=${CTRMML_TEMPLATE_SRC}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_template.cmake"
  DEPENDS "${CTRMML_TEMPLATE_ROM}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_template.cmake"
  COMMENT "Embedding assets/template.bin into ctrmml-cmd binary"
  VERBATIM)

set_source_files_properties("${CTRMML_TEMPLATE_SRC}" PROPERTIES GENERATED TRUE)

include(FetchContent)
FetchContent_Declare(
  ctrmml
  GIT_REPOSITORY https://github.com/ulalume/ctrmml.git
  GIT_TAG afc89d3
)
FetchContent_Declare(
  ymfm
  GIT_REPOSITORY https://github.com/aaronsgiles/ymfm
  GIT_TAG 17decfae857b92ab55fbb30ade2287ace095a381
)
FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio
  GIT_TAG 0.11.21
)

FetchContent_MakeAvailable(ctrmml)
FetchContent_GetProperties(ymfm)
if(NOT ymfm_POPULATED)
  FetchContent_Populate(ymfm)
endif()
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
  FetchContent_Populate(miniaudio)
endif()

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
  src/check_channel_events.cpp
  src/check_supplemental.cpp
  src/highlight_tracker.cpp
  src/mml_compile.cpp
  src/mdslink_tool.cpp
  src/rom_builder.cpp
  "${CTRMML_TEMPLATE_SRC}"
  src/vgm_audio_renderer.cpp
  src/vgm_export.cpp
  src/wav_export.cpp
  src/ymfm_ym2612_device.cpp
  src/mame_sn76496_device.cpp
  src/ctrmml_cmd.cpp
  src/ctrmml_cmd_c_api.cpp)

# Keep the C sources in their own STATIC library so the project-wide
# CMAKE_CXX_STANDARD does not leak C++ flags onto C compilations. MSBuild
# applies /std:c++17 across mixed-language targets, which causes MSVC's
# corecrt headers to drag yvals_core.h into the C translation unit and
# fail with STL1003 ("Unexpected compiler, expected C++ compiler").
add_library(ctrmml-cmd-csources STATIC
  src/sn76496/sn76496.c
  src/resampler/Resampler.c
  src/resampler/logging.c)
target_include_directories(ctrmml-cmd-csources PUBLIC src)

set(CTRMML_CMD_INCLUDE_DIRS
  src
  ${ctrmml_SOURCE_DIR}/src
  ${ymfm_SOURCE_DIR}/src)

add_library(ymfm-core STATIC
  ${ymfm_SOURCE_DIR}/src/ymfm_opn.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_adpcm.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_ssg.cpp
  src/ymfm_explicit_inst.cpp)
target_include_directories(ymfm-core PUBLIC ${ymfm_SOURCE_DIR}/src)

set(CTRMML_CMD_LINK_LIBS
  ctrmml
  ymfm-core
  ctrmml-cmd-csources)

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
    src/ctrmml_cmd_wasm_api.cpp
    src/preview_synth.cpp)

  target_include_directories(ctrmml-cmd-lib PUBLIC ${CTRMML_CMD_INCLUDE_DIRS})
  target_link_libraries(ctrmml-cmd-lib PRIVATE ${CTRMML_CMD_LINK_LIBS})

  add_executable(ctrmml-cmd-wasm src/ctrmml_cmd_wasm_api.cpp)
  target_link_libraries(ctrmml-cmd-wasm PRIVATE ctrmml-cmd-lib)
  set_target_properties(ctrmml-cmd-wasm PROPERTIES
    OUTPUT_NAME "ctrmml_cmd"
    SUFFIX ".js")

  option(CTRMML_WASM_ASSERTIONS
    "Enable Emscripten runtime assertions in ctrmml wasm build." OFF)
  if(CTRMML_WASM_ASSERTIONS)
    set(_CTRMML_WASM_ASSERTIONS "1")
  else()
    set(_CTRMML_WASM_ASSERTIONS "0")
  endif()

  target_link_options(ctrmml-cmd-wasm PRIVATE
    -fexceptions
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sEXPORTED_FUNCTIONS=_malloc,_free,_ctrmml_cmd_wasm_compile,_ctrmml_cmd_wasm_get_compile_error,_ctrmml_cmd_wasm_check_json,_ctrmml_cmd_wasm_start_playback,_ctrmml_cmd_wasm_stop_playback,_ctrmml_cmd_wasm_is_playing,_ctrmml_cmd_wasm_render_audio,_ctrmml_cmd_wasm_set_mute_mask,_ctrmml_cmd_wasm_set_lowpass_filter,_ctrmml_cmd_wasm_compile_and_relink,_ctrmml_cmd_wasm_get_player_ticks,_ctrmml_cmd_wasm_get_highlights,_ctrmml_cmd_wasm_export_vgm,_ctrmml_cmd_wasm_export_wav,_ctrmml_cmd_wasm_quickrom,_ctrmml_cmd_wasm_mdslink,_ctrmml_cmd_wasm_get_last_error,_ctrmml_cmd_wasm_get_track_data_json,_ctrmml_cmd_wasm_find_cursor_tick,_ctrmml_cmd_wasm_find_cursor_channel_json,_ctrmml_cmd_wasm_get_instrument_data_json,_ctrmml_cmd_wasm_get_quickrom_info,_ctrmml_cmd_wasm_get_mdslink_info,_preview_init,_preview_deinit,_preview_load_fm,_preview_load_psg,_preview_set_mode,_preview_note_on,_preview_note_off,_preview_all_notes_off,_preview_set_fm_op_mask"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToUTF8,lengthBytesUTF8,UTF8ToString,HEAPF32,HEAPU32,FS"
    "SHELL:-sENVIRONMENT=web"
    "SHELL:-sMODULARIZE=1"
    "SHELL:-sEXPORT_NAME=createCtrmmlModule"
    "SHELL:-sASSERTIONS=${_CTRMML_WASM_ASSERTIONS}"
    "SHELL:-sSTRICT=1")

else()
  # ---------- Native build ----------
  add_library(ctrmml-cmd-lib STATIC
    ${CTRMML_CMD_CORE_SOURCES}
    src/audio_output.cpp)

  target_include_directories(ctrmml-cmd-lib PUBLIC
    ${CTRMML_CMD_INCLUDE_DIRS}
    ${miniaudio_SOURCE_DIR})
  target_link_libraries(ctrmml-cmd-lib PRIVATE ${CTRMML_CMD_LINK_LIBS})

  if(MSVC)
    target_compile_options(ctrmml-cmd-lib PRIVATE /FIstring)
  endif()

  add_executable(ctrmml-cmd src/main.cpp)
  target_link_libraries(ctrmml-cmd PRIVATE ctrmml-cmd-lib)

  if(BUILD_TESTING)
    function(add_check_json_test name fixture expected_ok expected_errors expected_warnings expected_message expected_position)
      add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
          "-DCTRMML_CMD=$<TARGET_FILE:ctrmml-cmd>"
          "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${fixture}"
          "-DEXPECT_OK=${expected_ok}"
          "-DEXPECT_ERRORS=${expected_errors}"
          "-DEXPECT_WARNINGS=${expected_warnings}"
          "-DEXPECT_MESSAGE=${expected_message}"
          "-DEXPECT_POSITION=${expected_position}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_json.cmake")
      set_tests_properties(${name} PROPERTIES TIMEOUT 5)
    endfunction()

    add_check_json_test(check_channel_macro_repro channel_macro_repro.mml TRUE 0 1
      "Panning is not supported for PSG channels: in-app playback will stop here (MDSDRV hardware ignores it) (channel G, macro *401)"
      "\"line\":4,\"col\":9")
    add_check_json_test(check_channel_macro_fm_only channel_macro_fm_only.mml TRUE 0 0 "" "")
    add_check_json_test(check_channel_direct_psg channel_direct_psg.mml FALSE 1 0
      "Panning not supported for PSG channels" "\"line\":2,\"col\":6")
    add_check_json_test(check_channel_pan_clear channel_pan_clear.mml TRUE 0 0 "" "")
    add_check_json_test(check_channel_nested_loop channel_nested_loop.mml TRUE 0 1
      "Panning is not supported for PSG channels: in-app playback will stop here (MDSDRV hardware ignores it) (channel G, macro *403)"
      "\"line\":4,\"col\":6")
    add_check_json_test(check_channel_lfo channel_lfo.mml TRUE 0 1
      "LFO is not supported for PSG channels: in-app playback will stop here (MDSDRV hardware interprets it as a noise-mode command) (channel G)"
      "\"line\":2,\"col\":6")

    add_test(NAME export_paths_native
      COMMAND ${CMAKE_COMMAND}
        "-DCTRMML_CMD=$<TARGET_FILE:ctrmml-cmd>"
        "-DBAD_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/channel_macro_repro.mml"
        "-DCLEAN_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/channel_pan_clear.mml"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/export-path-test-native"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/export_paths.cmake")
    set_tests_properties(export_paths_native PROPERTIES TIMEOUT 20)

    add_executable(wasm-export-api-test
      tests/wasm_export_api_test.cpp
      src/ctrmml_cmd_wasm_api.cpp
      src/preview_synth.cpp)
    target_include_directories(wasm-export-api-test PRIVATE ${CTRMML_CMD_INCLUDE_DIRS})
    target_link_libraries(wasm-export-api-test PRIVATE ctrmml-cmd-lib)

    add_test(NAME export_paths_wasm_api
      COMMAND wasm-export-api-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/channel_macro_repro.mml"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/channel_pan_clear.mml"
        "${CMAKE_CURRENT_BINARY_DIR}/export-path-test-wasm-api")
    set_tests_properties(export_paths_wasm_api PROPERTIES TIMEOUT 20)
  endif()

  if(APPLE)
    find_library(COREAUDIO_FRAMEWORK CoreAudio)
    find_library(AUDIOUNIT_FRAMEWORK AudioUnit)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    target_link_libraries(ctrmml-cmd PRIVATE
      ${COREAUDIO_FRAMEWORK} ${AUDIOUNIT_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  elseif(WIN32)
    target_link_libraries(ctrmml-cmd PRIVATE winmm ole32 dsound uuid)
  endif()
endif()
