cmake_minimum_required(VERSION 3.24)

project(TurboGameEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENGINE_BACKEND "diligent" CACHE STRING "Runtime backend (only diligent is supported)" FORCE)
set_property(CACHE ENGINE_BACKEND PROPERTY STRINGS diligent)
option(ENGINE_BUILD_DILIGENT_SAMPLES "Build DiligentEngine samples/tutorials" ON)
option(ENGINE_BUILD_APP_LAUNCHER_SAMPLES "Build selected standalone Diligent samples alongside app" OFF)
option(ENGINE_ENABLE_DILIGENT_WEBGPU "Build WebGPU backend in Diligent" OFF)
set(DILIGENT_SAMPLE_MODE "d3d12" CACHE STRING "Runtime backend mode for Diligent sample launch targets")
set_property(CACHE DILIGENT_SAMPLE_MODE PROPERTY STRINGS d3d11 d3d12 vk gl webgpu)

string(TOLOWER "${ENGINE_BACKEND}" ENGINE_BACKEND_NORMALIZED)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Disable remote updates for already populated dependencies" FORCE)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(glm)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG v2.0.0rc13
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(tinyobjloader)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(stb)

set(DILIGENT_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/DiligentEngine")
if(NOT EXISTS "${DILIGENT_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "DiligentEngine sources were not found at ${DILIGENT_ENGINE_DIR}. Clone with submodules or run: git submodule update --init --recursive")
endif()

set(DILIGENT_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_FX ON CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_SAMPLES ${ENGINE_BUILD_DILIGENT_SAMPLES} CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_SAMPLE_BASE_ONLY OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_CORE_TESTS OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_TOOLS_TESTS OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_FX_TESTS OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_SAMPLES_TESTS OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_TOOLS_INCLUDE_TEST OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_FX_INCLUDE_TEST OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_SAMPLES_INCLUDE_TEST OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(DILIGENT_LOAD_PIX_EVENT_RUNTIME ON CACHE BOOL "" FORCE)
endif()
if(ENGINE_ENABLE_DILIGENT_WEBGPU)
    set(DILIGENT_NO_WEBGPU OFF CACHE BOOL "" FORCE)
else()
    set(DILIGENT_NO_WEBGPU ON CACHE BOOL "" FORCE)
endif()

add_subdirectory("${DILIGENT_ENGINE_DIR}")

function(set_engine_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(get_pix_copy_helper_target_name target_name out_var)
    string(MAKE_C_IDENTIFIER "${target_name}" _target_identifier)
    set(${out_var} "copy_pix_runtime_${_target_identifier}" PARENT_SCOPE)
endfunction()

function(copy_pix_runtime_dll target_name)
    if(NOT WIN32 OR
       NOT TARGET ${target_name} OR
       NOT DEFINED DILIGENT_PIX_EVENT_RUNTIME_DLL_PATH OR
       NOT EXISTS "${DILIGENT_PIX_EVENT_RUNTIME_DLL_PATH}")
        return()
    endif()

    get_target_property(_target_source_dir ${target_name} SOURCE_DIR)
    if(_target_source_dir STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DILIGENT_PIX_EVENT_RUNTIME_DLL_PATH}"
                "$<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Copying WinPixEventRuntime.dll for ${target_name}"
        )
        return()
    endif()

    get_pix_copy_helper_target_name(${target_name} _helper_target)
    if(NOT TARGET ${_helper_target})
        add_custom_target(${_helper_target}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DILIGENT_PIX_EVENT_RUNTIME_DLL_PATH}"
                "$<TARGET_FILE_DIR:${target_name}>"
            DEPENDS ${target_name}
            COMMENT "Copying WinPixEventRuntime.dll for ${target_name}"
        )
    endif()
    set_property(GLOBAL APPEND PROPERTY ENGINE_PIX_COPY_TARGETS ${_helper_target})
endfunction()

copy_pix_runtime_dll(Diligent-RenderStatePackager)
get_pix_copy_helper_target_name(Diligent-RenderStatePackager DILIGENT_RENDER_STATE_PACKAGER_PIX_COPY_TARGET)

set(ENGINE_ROOT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(engine_core STATIC
    engine/core/Log.cpp
    engine/core/Time.cpp
    engine/core/Config.cpp
)
target_include_directories(engine_core PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_core PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json)
set_engine_warnings(engine_core)

add_library(engine_platform STATIC
    engine/platform/Window.cpp
    engine/platform/WindowGLFW.cpp
    engine/platform/Input.cpp
)
target_include_directories(engine_platform PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_platform PUBLIC engine_core glfw)
set_engine_warnings(engine_platform)

add_library(engine_rhi_diligent STATIC
    engine/rhi_diligent/DiligentDevice.cpp
)
target_include_directories(engine_rhi_diligent PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")

set(DILIGENT_RHI_LIBS)
foreach(_diligent_target IN ITEMS
    Diligent-GraphicsEngineD3D11-static
    Diligent-GraphicsEngineD3D12-static
    Diligent-GraphicsEngineOpenGL-static
    Diligent-GraphicsEngineVk-static
    Diligent-GraphicsEngineWebGPU-static
)
    if(TARGET ${_diligent_target})
        list(APPEND DILIGENT_RHI_LIBS ${_diligent_target})
    endif()
endforeach()

if(DILIGENT_RHI_LIBS STREQUAL "")
    message(FATAL_ERROR "No Diligent backend libraries were generated")
endif()

target_link_libraries(engine_rhi_diligent
PUBLIC
    engine_core
    Diligent-BuildSettings
PRIVATE
    glfw
    ${DILIGENT_RHI_LIBS}
)
target_compile_definitions(engine_rhi_diligent PRIVATE ENGINE_ENABLE_DILIGENT_BACKEND=1)
set_engine_warnings(engine_rhi_diligent)

add_library(engine_rhi STATIC
    engine/rhi/RHI.cpp
)
target_include_directories(engine_rhi PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_compile_definitions(engine_rhi PRIVATE ENGINE_ENABLE_DILIGENT_BACKEND=1)
target_link_libraries(engine_rhi PUBLIC engine_core PRIVATE engine_rhi_diligent)
set_engine_warnings(engine_rhi)

add_library(engine_renderer STATIC
    engine/renderer/Renderer.cpp
    engine/resources/resource_manager.cpp
    engine/resources/loaders.cpp
    engine/ecs/render_system.cpp
)
target_include_directories(engine_renderer PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_include_directories(engine_renderer PRIVATE "${tinyobjloader_SOURCE_DIR}" "${stb_SOURCE_DIR}")
target_link_libraries(engine_renderer PUBLIC engine_core engine_rhi glm::glm nlohmann_json::nlohmann_json)
if(TARGET Diligent-Imgui)
    target_link_libraries(engine_renderer PRIVATE Diligent-Imgui)
endif()
if(TARGET Diligent-BuildSettings)
    target_link_libraries(engine_renderer PRIVATE Diligent-BuildSettings)
endif()
if(EXISTS "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui/imgui_demo.cpp")
    target_sources(engine_renderer PRIVATE
        "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui/imgui_demo.cpp"
    )
endif()
set_engine_warnings(engine_renderer)

add_library(engine_game STATIC
    engine/ecs/world.cpp
    engine/game/StateStack.cpp
    engine/game/MenuState.cpp
    engine/game/GameplayState.cpp
    engine/game/PauseState.cpp
)
target_include_directories(engine_game PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_game PUBLIC engine_core engine_platform engine_renderer)
set_engine_warnings(engine_game)

add_executable(app
    app/main.cpp
    engine/core/Application.cpp
)
target_include_directories(app PRIVATE "${ENGINE_ROOT_INCLUDE_DIR}")
target_compile_definitions(app PRIVATE ENGINE_BACKEND_NAME="diligent")
target_link_libraries(app PRIVATE engine_core engine_platform engine_renderer engine_game)
set_engine_warnings(app)
copy_pix_runtime_dll(app)

if(COMMAND copy_required_dlls)
    copy_required_dlls(app)
endif()

add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:app>/assets"
    COMMENT "Copying assets to runtime output directory"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/CMakeLists.txt")
    add_subdirectory(tools/shader_compiler)
endif()

set(DILIGENT_SAMPLES_ROOT "${DILIGENT_ENGINE_DIR}/DiligentSamples")
set(ENGINE_APP_SAMPLE_TARGETS)

if(EXISTS "${DILIGENT_SAMPLES_ROOT}/Samples")
    file(GLOB _diligent_sample_dirs RELATIVE "${DILIGENT_SAMPLES_ROOT}/Samples" "${DILIGENT_SAMPLES_ROOT}/Samples/*")
    foreach(_sample_dir IN LISTS _diligent_sample_dirs)
        if(IS_DIRECTORY "${DILIGENT_SAMPLES_ROOT}/Samples/${_sample_dir}" AND
           EXISTS "${DILIGENT_SAMPLES_ROOT}/Samples/${_sample_dir}/CMakeLists.txt" AND
           TARGET ${_sample_dir})
            list(APPEND ENGINE_APP_SAMPLE_TARGETS ${_sample_dir})
        endif()
    endforeach()
endif()

if(EXISTS "${DILIGENT_SAMPLES_ROOT}/Tutorials")
    file(GLOB _diligent_tutorial_dirs RELATIVE "${DILIGENT_SAMPLES_ROOT}/Tutorials" "${DILIGENT_SAMPLES_ROOT}/Tutorials/*")
    foreach(_tutorial_dir IN LISTS _diligent_tutorial_dirs)
        if(IS_DIRECTORY "${DILIGENT_SAMPLES_ROOT}/Tutorials/${_tutorial_dir}" AND
           _tutorial_dir MATCHES "^Tutorial" AND
           EXISTS "${DILIGENT_SAMPLES_ROOT}/Tutorials/${_tutorial_dir}/CMakeLists.txt" AND
           TARGET ${_tutorial_dir})
            list(APPEND ENGINE_APP_SAMPLE_TARGETS ${_tutorial_dir})
        endif()
    endforeach()
endif()

if(ENGINE_APP_SAMPLE_TARGETS)
    list(REMOVE_DUPLICATES ENGINE_APP_SAMPLE_TARGETS)
    list(SORT ENGINE_APP_SAMPLE_TARGETS)

    foreach(_sample_target IN LISTS ENGINE_APP_SAMPLE_TARGETS)
        copy_pix_runtime_dll(${_sample_target})
    endforeach()

    if(TARGET Tutorial25_StatePackager AND TARGET ${DILIGENT_RENDER_STATE_PACKAGER_PIX_COPY_TARGET})
        add_dependencies(Tutorial25_StatePackager ${DILIGENT_RENDER_STATE_PACKAGER_PIX_COPY_TARGET})
    endif()

    get_property(_engine_pix_copy_targets GLOBAL PROPERTY ENGINE_PIX_COPY_TARGETS)
    if(_engine_pix_copy_targets)
        list(REMOVE_DUPLICATES _engine_pix_copy_targets)
    endif()

    add_custom_target(build_diligent_all_samples
        DEPENDS ${ENGINE_APP_SAMPLE_TARGETS} ${_engine_pix_copy_targets}
        COMMENT "Build all available Diligent samples and tutorials"
    )

    add_custom_target(build_diligent_app_samples
        DEPENDS build_diligent_all_samples
        COMMENT "Compatibility alias: build all available Diligent samples"
    )

    if(ENGINE_BUILD_APP_LAUNCHER_SAMPLES)
        add_dependencies(app build_diligent_all_samples)
    endif()
endif()

function(add_diligent_run_target target_name run_target_name)
    if(TARGET ${target_name})
        add_custom_target(${run_target_name}
            COMMAND $<TARGET_FILE:${target_name}> --mode ${DILIGENT_SAMPLE_MODE}
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${target_name}>
            DEPENDS ${target_name}
            COMMENT "Run Diligent ${target_name} with mode=${DILIGENT_SAMPLE_MODE}"
        )
    endif()
endfunction()

add_diligent_run_target(GLFWDemo run_diligent_glfw_demo)
add_diligent_run_target(Tutorial01_HelloTriangle run_diligent_tutorial01)
add_diligent_run_target(ImguiDemo run_diligent_imgui_demo)
add_diligent_run_target(Tutorial03_Texturing run_diligent_tutorial03)
