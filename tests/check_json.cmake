execute_process(
  COMMAND "${CTRMML_CMD}" check --json "${FIXTURE}"
  RESULT_VARIABLE check_result
  OUTPUT_VARIABLE check_json
  ERROR_VARIABLE check_stderr
  OUTPUT_STRIP_TRAILING_WHITESPACE)

if(EXPECT_OK)
  if(NOT check_result EQUAL 0)
    message(FATAL_ERROR "check unexpectedly failed (${check_result}): ${check_json}\n${check_stderr}")
  endif()
  string(FIND "${check_json}" "\"ok\":true" ok_position)
else()
  if(check_result EQUAL 0)
    message(FATAL_ERROR "check unexpectedly succeeded: ${check_json}")
  endif()
  string(FIND "${check_json}" "\"ok\":false" ok_position)
endif()

if(ok_position EQUAL -1)
  message(FATAL_ERROR "unexpected check JSON status: ${check_json}")
endif()

string(REGEX MATCHALL "\"code\":\"playback_error\"" playback_errors "${check_json}")
list(LENGTH playback_errors playback_error_count)
if(NOT playback_error_count EQUAL EXPECT_ERRORS)
  message(FATAL_ERROR
    "expected ${EXPECT_ERRORS} playback errors, got ${playback_error_count}: ${check_json}")
endif()

if(NOT "${EXPECT_MESSAGE}" STREQUAL "")
  string(FIND "${check_json}" "${EXPECT_MESSAGE}" message_position)
  if(message_position EQUAL -1)
    message(FATAL_ERROR "missing message '${EXPECT_MESSAGE}': ${check_json}")
  endif()
endif()

if(NOT "${EXPECT_POSITION}" STREQUAL "")
  string(FIND "${check_json}" "${EXPECT_POSITION}" source_position)
  if(source_position EQUAL -1)
    message(FATAL_ERROR "missing position '${EXPECT_POSITION}': ${check_json}")
  endif()
endif()
