cmake_minimum_required(VERSION 3.24)
project(mxl-scopes VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SCOPES_BUILD_TESTS "Build unit tests" ON)
option(SCOPES_WITH_NMOS "Link Sony nmos-cpp" OFF)
set(NMOS_CPP_DIR "" CACHE PATH "Path to sony/nmos-cpp Development/")

find_package(Threads REQUIRED)

set(SCOPES_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# scopes-core: config, scope engine, media, REST and metrics. It links neither libmxl nor
# nmos-cpp, so the bit-exact tests run anywhere (IMPLEMENTATION_PLAN.md).
add_library(scopes-core STATIC
    src/util/logging.cpp
    src/util/sha1.cpp
    src/util/uuid.cpp
    src/util/httpclient.cpp
    src/config/config.cpp
    src/config/store.cpp
    src/domain/scan.cpp
    src/nmos/ids.cpp
    src/media/v210.cpp
    src/media/patterns.cpp
    src/media/png.cpp
    src/media/timebase.cpp
    src/scope/analyse.cpp
    src/scope/render.cpp
    src/scope/engine.cpp
    src/scope/scheduler.cpp
    src/scope/cpu/dispatch.cpp
    src/app/state.cpp
    src/ops/metrics.cpp
    src/ops/httpserver.cpp
    src/ops/api.cpp
)
target_include_directories(scopes-core PUBLIC src third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(scopes-core PUBLIC Threads::Threads)
target_compile_options(scopes-core PRIVATE ${SCOPES_WARNINGS})

# The AVX2 and AVX-512 engines are one translation unit each with their own flags; the rest
# of the binary stays baseline x86-64, so the image starts on any x86-64 CPU (§3.1, §3.2).
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2 -mfma" SCOPES_HAVE_AVX2_FLAG)
check_cxx_compiler_flag("-mavx512f" SCOPES_HAVE_AVX512_FLAG)
if(SCOPES_HAVE_AVX2_FLAG AND SCOPES_HAVE_AVX512_FLAG AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_sources(scopes-core PRIVATE src/scope/cpu/avx2.cpp src/scope/cpu/avx512.cpp)
    set_source_files_properties(src/scope/cpu/avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
    set_source_files_properties(src/scope/cpu/avx512.cpp PROPERTIES COMPILE_OPTIONS "-mavx512f")
else()
    target_sources(scopes-core PRIVATE src/scope/cpu/noavx2.cpp)
endif()

# The web UI: one HTML file, embedded into the binary.
set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)
set(WEBUI_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/ops/webui_generated.hpp)
add_custom_command(
    OUTPUT ${WEBUI_GENERATED}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED} -DVAR=indexHtml
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
    COMMENT "Embedding the web UI"
)
add_custom_target(scopes-webui DEPENDS ${WEBUI_GENERATED})
add_dependencies(scopes-core scopes-webui)

# Micro-benchmark of the engine (IMPLEMENTATION_PLAN.md M0): generated frames, no libmxl.
add_executable(mxl-scopes-bench tools/bench.cpp)
target_link_libraries(mxl-scopes-bench PRIVATE scopes-core)
target_compile_options(mxl-scopes-bench PRIVATE ${SCOPES_WARNINGS})

if(SCOPES_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_config.cpp
        tests/unit/test_engine.cpp
        tests/unit/test_render.cpp
        tests/unit/test_scheduler.cpp
        tests/unit/test_api.cpp
        tests/unit/test_http.cpp
    )
    target_link_libraries(unit-tests PRIVATE scopes-core)
    target_compile_options(unit-tests PRIVATE ${SCOPES_WARNINGS})
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

find_package(mxl CONFIG QUIET)
if(mxl_FOUND)
    add_executable(mxl-scopes src/main.cpp src/mxlio/io.cpp src/nmos/node.cpp)
    target_link_libraries(mxl-scopes PRIVATE scopes-core mxl::mxl)
    target_compile_options(mxl-scopes PRIVATE ${SCOPES_WARNINGS})
    if(SCOPES_WITH_NMOS)
        if(NOT NMOS_CPP_DIR)
            message(FATAL_ERROR "SCOPES_WITH_NMOS=ON requires -DNMOS_CPP_DIR")
        endif()
        set(NMOS_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(NMOS_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_SUPPLIED_JSON_SCHEMA_VALIDATOR ON CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_SUPPLIED_JWT_CPP ON CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_AVAHI OFF CACHE BOOL "" FORCE)
        set(CMAKE_CXX_STANDARD 17)
        add_subdirectory(${NMOS_CPP_DIR} ${CMAKE_BINARY_DIR}/nmos-cpp EXCLUDE_FROM_ALL)
        set(CMAKE_CXX_STANDARD 20)
        target_compile_definitions(mxl-scopes PRIVATE SCOPES_WITH_NMOS=1)
        target_link_libraries(mxl-scopes PRIVATE nmos-cpp::nmos-cpp)
        set_source_files_properties(src/nmos/node.cpp PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            COMPILE_OPTIONS "-w")
    endif()
    # Test tools: a pattern writer (bars, ramp, flat, random) and an output sampler.
    add_executable(mxl-scopes-writer tests/tools/pattern_writer.cpp)
    target_link_libraries(mxl-scopes-writer PRIVATE scopes-core mxl::mxl)
    add_executable(mxl-scopes-sample tests/tools/sample_flow.cpp)
    target_link_libraries(mxl-scopes-sample PRIVATE mxl::mxl)
else()
    message(STATUS "libmxl not found: building scopes-core, unit tests and the benchmark only")
endif()
