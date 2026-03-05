if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "INPUT_FILE and OUTPUT_FILE must be set")
endif()

if(NOT EXISTS "${INPUT_FILE}")
  message(FATAL_ERROR "Template ROM not found: ${INPUT_FILE}")
endif()

file(READ "${INPUT_FILE}" TEMPLATE_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," TEMPLATE_ARRAY "${TEMPLATE_HEX}")

set(CONTENT
"#include <cstddef>
#include <cstdint>

namespace ctrmml_embedded
{
extern const uint8_t kTemplateRomData[] = {${TEMPLATE_ARRAY}};
extern const size_t kTemplateRomSize = sizeof(kTemplateRomData);
}
")

file(WRITE "${OUTPUT_FILE}" "${CONTENT}")
