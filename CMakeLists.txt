cmake_minimum_required(VERSION 3.24)
project(mxl-scopes VERSION 1.1.0 LANGUAGES CXX)

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

# Graticule labels, caption and state texts with Blend2D and DejaVu Sans (§3.1), the pin of
# mxl-multiviewer; OFF draws them with the 8×8 bitmap font (an offline build).
option(SCOPES_WITH_BLEND2D "Draw texts with Blend2D" ON)
if(SCOPES_WITH_BLEND2D)
    include(FetchContent)
    FetchContent_Declare(blend2d_src
        URL https://blend2d.com/download/blend2d-0.21.2.tar.gz
        URL_HASH SHA256=234ba57a64153c96885a6271f15fdcf6c594ac8cb03adaad19abb8d3b2b533fa
    )
    FetchContent_GetProperties(blend2d_src)
    if(NOT blend2d_src_POPULATED)
        FetchContent_Populate(blend2d_src)
    endif()
    set(BLEND2D_STATIC ON CACHE BOOL "" FORCE)
    set(BLEND2D_TEST OFF CACHE BOOL "" FORCE)
    set(BLEND2D_DEMOS OFF CACHE BOOL "" FORCE)
    set(BLEND2D_NO_INSTALL ON CACHE BOOL "" FORCE)
    set(BLEND2D_NO_JIT_LOGGING ON CACHE BOOL "" FORCE)
    add_subdirectory(${blend2d_src_SOURCE_DIR} ${CMAKE_BINARY_DIR}/blend2d EXCLUDE_FROM_ALL)
endif()

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
    src/scope/workers.cpp
    src/scope/measure.cpp
    src/preview/plan.cpp
    src/preview/child.cpp
    src/scope/scheduler.cpp
    src/scope/cpu/dispatch.cpp
    src/app/state.cpp
    src/ops/metrics.cpp
    src/ops/httpserver.cpp
    src/ops/api.cpp
    src/ops/widgets.cpp
)
target_include_directories(scopes-core PUBLIC src third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(scopes-core PUBLIC Threads::Threads)
target_compile_options(scopes-core PRIVATE ${SCOPES_WARNINGS})
if(SCOPES_WITH_BLEND2D)
    set(DEJAVU_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/scope/dejavu_sans.hpp)
    add_custom_command(
        OUTPUT ${DEJAVU_HEADER}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/third_party/dejavu/DejaVuSans.ttf
                -DOUTPUT=${DEJAVU_HEADER}
                -DNAMESPACE=scopes::font
                -DVAR=dejavuSans
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBytes.cmake
        DEPENDS third_party/dejavu/DejaVuSans.ttf cmake/EmbedBytes.cmake
        COMMENT "Embedding DejaVu Sans"
    )
    add_custom_target(scopes-font DEPENDS ${DEJAVU_HEADER})
    add_dependencies(scopes-core scopes-font)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/scope/render.cpp PROPERTIES OBJECT_DEPENDS "${DEJAVU_HEADER}")
    target_link_libraries(scopes-core PUBLIC blend2d::blend2d)
    target_compile_definitions(scopes-core PUBLIC SCOPES_WITH_BLEND2D=1)
endif()

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
        tests/unit/test_modes.cpp
        tests/unit/test_render.cpp
        tests/unit/test_scheduler.cpp
        tests/unit/test_threads.cpp
        tests/unit/test_measure.cpp
        tests/unit/test_key.cpp
        tests/unit/test_preview.cpp
        tests/unit/test_widgets.cpp
        tests/unit/test_api.cpp
        tests/unit/test_http.cpp
    )
    target_link_libraries(unit-tests PRIVATE scopes-core)
    target_compile_options(unit-tests PRIVATE ${SCOPES_WARNINGS})
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

find_package(mxl CONFIG QUIET)
if(mxl_FOUND)
    # The WebRTC preview's H.264 encoder and RTSP publisher (§2.5): the system FFmpeg.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)
    # The web UI (§9.1): the Vue 3 app built by Vite into one HTML file (web/), embedded into the
    # binary. npm builds it here; without npm a web/dist/index.html built before (the image's Node
    # stage) is embedded; without either a short page says so. As mxl-multiviewer.
    set(WEBUI_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/ops/webui_generated.hpp)
    find_program(NPM_EXECUTABLE npm)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html AND NOT NPM_EXECUTABLE)
        set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html)
        set(WEBUI_DEPENDS ${WEBUI_HTML})
    elseif(NPM_EXECUTABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/web/package.json)
        set(WEBUI_HTML ${CMAKE_CURRENT_BINARY_DIR}/webui/index.html)
        file(GLOB_RECURSE SCOPES_WEB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/src/*)
        add_custom_command(
            OUTPUT ${WEBUI_HTML}
            COMMAND ${NPM_EXECUTABLE} ci --no-fund --no-audit
            COMMAND ${NPM_EXECUTABLE} run build
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/webui
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html ${WEBUI_HTML}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
            DEPENDS web/package.json web/package-lock.json web/index.html web/vite.config.js ${SCOPES_WEB_SOURCES}
            COMMENT "Building the Vue web UI"
            VERBATIM
        )
        set(WEBUI_DEPENDS ${WEBUI_HTML})
    else()
        set(WEBUI_HTML ${CMAKE_CURRENT_BINARY_DIR}/webui/placeholder.html)
        file(WRITE ${WEBUI_HTML} "<!doctype html><title>mxl-scopes</title><p>This build has no web UI (no npm, no web/dist). See /api/v1/info.</p>\n")
        set(WEBUI_DEPENDS "")
    endif()
    add_custom_command(
        OUTPUT ${WEBUI_GENERATED}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED} -DVAR=indexHtml
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
        DEPENDS ${WEBUI_DEPENDS} cmake/EmbedFile.cmake
        COMMENT "Embedding the web UI"
    )
    add_executable(mxl-scopes src/main.cpp src/mxlio/io.cpp src/nmos/node.cpp src/preview/publisher.cpp ${WEBUI_GENERATED})
    target_link_libraries(mxl-scopes PRIVATE scopes-core mxl::mxl PkgConfig::AV)
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
