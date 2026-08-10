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

string(REGEX MATCH "\"errors\":\\[[^]]*\\]" errors_json "${check_json}")
string(REGEX MATCH "\"warnings\":\\[[^]]*\\]" warnings_json "${check_json}")

string(REGEX MATCHALL "\"message\":" error_messages "${errors_json}")
list(LENGTH error_messages error_count)
if(NOT error_count EQUAL EXPECT_ERRORS)
  message(FATAL_ERROR
    "expected ${EXPECT_ERRORS} errors, got ${error_count}: ${check_json}")
endif()

string(REGEX MATCHALL "\"message\":" warning_messages "${warnings_json}")
list(LENGTH warning_messages warning_count)
if(NOT warning_count EQUAL EXPECT_WARNINGS)
  message(FATAL_ERROR
    "expected ${EXPECT_WARNINGS} warnings, got ${warning_count}: ${check_json}")
endif()

if(NOT "${EXPECT_MESSAGE}" STREQUAL "")
  string(FIND "${check_json}" "${EXPECT_MESSAGE}" message_position)
  if(message_position EQUAL -1)
    message(FATAL_ERROR "missing message '${EXPECT_MESSAGE}': ${check_json}")
  endif()
endif()

if(NOT DEFINED EXPECT_CODE OR "${EXPECT_CODE}" STREQUAL "")
  set(EXPECT_CODE "playback_unsupported_warning")
endif()

if(EXPECT_WARNINGS GREATER 0)
  set(expected_warning
    "{\"message\":\"${EXPECT_MESSAGE}\",\"path\":\"${FIXTURE}\",${EXPECT_POSITION},\"length\":1,\"code\":\"${EXPECT_CODE}\"}")
  string(FIND "${warnings_json}" "${expected_warning}" warning_shape_position)
  if(warning_shape_position EQUAL -1)
    message(FATAL_ERROR
      "missing exact warning entry '${expected_warning}': ${check_json}")
  endif()
endif()

if(NOT "${EXPECT_POSITION}" STREQUAL "")
  string(FIND "${check_json}" "${EXPECT_POSITION}" source_position)
  if(source_position EQUAL -1)
    message(FATAL_ERROR "missing position '${EXPECT_POSITION}': ${check_json}")
  endif()
endif()
