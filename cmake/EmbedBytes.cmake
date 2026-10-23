if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED NAMESPACE OR NOT DEFINED VAR)
    message(FATAL_ERROR "INPUT, OUTPUT, NAMESPACE, and VAR are required")
endif()
get_filename_component(_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}" "// Generated — do not edit.
#pragma once
#include <cstddef>
namespace ${NAMESPACE} {
inline constexpr unsigned char ${VAR}[] = {${bytes}};
inline constexpr std::size_t ${VAR}Size = sizeof(${VAR});
}
")
