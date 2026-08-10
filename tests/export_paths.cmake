if(NOT DEFINED CTRMML_CMD OR NOT DEFINED BAD_FIXTURE OR NOT DEFINED CLEAN_FIXTURE OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "missing export path test arguments")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

function(expect_export_failure format extension)
  set(output "${OUTPUT_DIR}/bad.${extension}")
  file(REMOVE "${output}")
  execute_process(
    COMMAND "${CTRMML_CMD}" export "${BAD_FIXTURE}" "--${format}" --out "${output}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(rc EQUAL 0)
    message(FATAL_ERROR "${format} export unexpectedly succeeded")
  endif()
  if(NOT stderr MATCHES "Panning not supported for PSG channels")
    message(FATAL_ERROR "${format} error did not contain renderer message: ${stderr}")
  endif()
  if(EXISTS "${output}")
    message(FATAL_ERROR "${format} failure left output behind: ${output}")
  endif()
endfunction()

function(expect_export_success format extension)
  set(output "${OUTPUT_DIR}/clean.${extension}")
  file(REMOVE "${output}")
  execute_process(
    COMMAND "${CTRMML_CMD}" export "${CLEAN_FIXTURE}" "--${format}" --out "${output}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "clean ${format} export failed (${rc}): ${stderr}")
  endif()
  if(NOT EXISTS "${output}")
    message(FATAL_ERROR "clean ${format} export did not create ${output}")
  endif()
endfunction()

expect_export_failure(wav wav)
expect_export_failure(vgm vgm)
expect_export_success(wav wav)
expect_export_success(vgm vgm)

file(REMOVE
  "${OUTPUT_DIR}/bad.wav"
  "${OUTPUT_DIR}/bad.vgm"
  "${OUTPUT_DIR}/clean.wav"
  "${OUTPUT_DIR}/clean.vgm")
