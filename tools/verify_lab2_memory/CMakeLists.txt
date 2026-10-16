cmake_minimum_required(VERSION 3.24)
project(TgeLab2Memory LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "This isolated sanitizer harness requires Linux with GCC or Clang.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(TGE_SOURCE_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TGE_SOURCE_ROOT "${TGE_SOURCE_DEFAULT}" CACHE PATH "Unmodified TurboGameEngine checkout")
set(TGE_DEPENDENCY_ROOT "" CACHE PATH "Existing CMake _deps directory; no downloads")
set(TGE_TEST_ASSET_ROOT "${TGE_SOURCE_ROOT}/assets" CACHE PATH "Unmodified test asset snapshot")

foreach(item IN ITEMS luau-src enkits-src spdlog-src glm-src entt-src nlohmann_json-src)
    if(NOT IS_DIRECTORY "${TGE_DEPENDENCY_ROOT}/${item}")
        message(FATAL_ERROR "Missing existing dependency: ${TGE_DEPENDENCY_ROOT}/${item}")
    endif()
endforeach()
if(NOT EXISTS "${TGE_SOURCE_ROOT}/tests/ScriptingSystemTests.cpp")
    message(FATAL_ERROR "TGE_SOURCE_ROOT does not contain the engine tests")
endif()

# Set these before dependencies: Luau, enkiTS and spdlog are instrumented too.
# Debug keeps the engine's existing 100 ms callback guard; -O1 avoids an
# unrepresentative unoptimized sanitizer build. No production flags are changed.
add_compile_options(-fsanitize=address,leak,undefined -fno-omit-frame-pointer
    -g -O1 -fno-sanitize-recover=all)
add_link_options(-fsanitize=address,leak,undefined -fno-omit-frame-pointer
    -fno-sanitize-recover=all)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory("${TGE_DEPENDENCY_ROOT}/luau-src" luau EXCLUDE_FROM_ALL)
set(ENKITS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_C_INTERFACE OFF CACHE BOOL "" FORCE)
set(ENKITS_TASK_PRIORITIES_NUM 3 CACHE STRING "" FORCE)
add_subdirectory("${TGE_DEPENDENCY_ROOT}/enkits-src" enkits EXCLUDE_FROM_ALL)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory("${TGE_DEPENDENCY_ROOT}/spdlog-src" spdlog EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)

add_library(tge_memory_runtime STATIC
    "${TGE_SOURCE_ROOT}/engine/core/JobSystem.cpp"
    "${TGE_SOURCE_ROOT}/engine/core/LiveFileWatcher.cpp"
    "${TGE_SOURCE_ROOT}/engine/core/Log.cpp"
    "${TGE_SOURCE_ROOT}/engine/ecs/world.cpp"
    "${TGE_SOURCE_ROOT}/engine/platform/Input.cpp"
    "${TGE_SOURCE_ROOT}/engine/scripting/ScriptingSystem.cpp"
    "${TGE_SOURCE_ROOT}/engine/scripting/react/ReactRuntime.cpp")
target_include_directories(tge_memory_runtime PUBLIC "${TGE_SOURCE_ROOT}"
    "${TGE_DEPENDENCY_ROOT}/glm-src"
    "${TGE_DEPENDENCY_ROOT}/entt-src/src"
    "${TGE_DEPENDENCY_ROOT}/nlohmann_json-src/include")
target_compile_definitions(tge_memory_runtime PUBLIC ENTT_NO_ETO PRIVATE
    ENGINE_REACT_PACKAGE_DIR="${TGE_SOURCE_ROOT}/third_party/react-luau")
target_link_libraries(tge_memory_runtime PUBLIC enkiTS Luau.VM Luau.Compiler
    spdlog::spdlog Threads::Threads)

add_executable(lab2_scripting_memory "${TGE_SOURCE_ROOT}/tests/ScriptingSystemTests.cpp")
target_link_libraries(lab2_scripting_memory PRIVATE tge_memory_runtime)
target_compile_definitions(lab2_scripting_memory PRIVATE
    ENGINE_TEST_ASSET_ROOT="${TGE_TEST_ASSET_ROOT}")
# The original fixture executes a 512-tween batch in a single guarded chunk.
# ASan/UBSan overhead can exceed its normal100ms budget. Adapt only this test
# driver's configured batch limit; its explicit5ms hostile-callback check stays.
set(TGE_REACT_BATCH_MILLISECONDS 1000 CACHE STRING "Sanitizer-only React fixture batch budget")
if(TGE_REACT_BATCH_MILLISECONDS LESS 100 OR TGE_REACT_BATCH_MILLISECONDS GREATER 5000)
    message(FATAL_ERROR "React diagnostic batch budget must be100..5000ms")
endif()
file(READ "${TGE_SOURCE_ROOT}/tests/ReactRuntimeTests.cpp" react_test_source)
set(budget_anchor "const auto originalLimits = scripting.limits();")
string(FIND "${react_test_source}" "${budget_anchor}" budget_anchor_offset)
if(budget_anchor_offset LESS 0)
    message(FATAL_ERROR "React test budget anchor changed; review the harness adaptation")
endif()
string(REPLACE "${budget_anchor}"
    "auto diagnosticLimits = scripting.limits(); diagnosticLimits.callbackMilliseconds = ${TGE_REACT_BATCH_MILLISECONDS}; scripting.setLimits(diagnosticLimits);\n        ${budget_anchor}"
    react_test_source "${react_test_source}")
file(WRITE "${CMAKE_BINARY_DIR}/ReactRuntimeSanitizerTests.cpp" "${react_test_source}")
add_executable(lab2_react_memory "${CMAKE_BINARY_DIR}/ReactRuntimeSanitizerTests.cpp")
target_link_libraries(lab2_react_memory PRIVATE tge_memory_runtime)
target_compile_definitions(lab2_react_memory PRIVATE
    ENGINE_REACT_TEST_DIR="${TGE_SOURCE_ROOT}/tests")
add_executable(lab2_lifecycle_memory MemoryLifecycle.cpp)
target_link_libraries(lab2_lifecycle_memory PRIVATE tge_memory_runtime)
target_compile_definitions(lab2_lifecycle_memory PRIVATE
    ENGINE_TEST_ASSET_ROOT="${TGE_TEST_ASSET_ROOT}")

# Keep the production checkout's logs clean. Tests read assets through this
# link, write their fixtures into the OS temp directory, and log under runtime/.
set(TGE_MEMORY_WORKDIR "${CMAKE_BINARY_DIR}/runtime" CACHE PATH "Isolated runtime directory")
file(MAKE_DIRECTORY "${TGE_MEMORY_WORKDIR}")
if(NOT EXISTS "${TGE_MEMORY_WORKDIR}/assets")
    file(CREATE_LINK "${TGE_TEST_ASSET_ROOT}" "${TGE_MEMORY_WORKDIR}/assets"
        SYMBOLIC RESULT asset_link_result)
    if(NOT asset_link_result STREQUAL "0")
        message(FATAL_ERROR "Cannot create read-only asset reference: ${asset_link_result}")
    endif()
endif()
enable_testing()
add_test(NAME lab2_scripting_memory COMMAND lab2_scripting_memory)
add_test(NAME lab2_react_memory COMMAND lab2_react_memory)
add_test(NAME lab2_lifecycle_memory COMMAND lab2_lifecycle_memory)
set_tests_properties(lab2_scripting_memory lab2_react_memory lab2_lifecycle_memory PROPERTIES
    WORKING_DIRECTORY "${TGE_MEMORY_WORKDIR}" TIMEOUT 300
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:symbolize=1;LSAN_OPTIONS=exitcode=23:report_objects=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
