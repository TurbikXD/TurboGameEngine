cmake_minimum_required(VERSION 3.24)

project(TurboGameEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENGINE_BACKEND "opengl" CACHE STRING "Default runtime backend (opengl|vulkan|d3d12)")
set_property(CACHE ENGINE_BACKEND PROPERTY STRINGS opengl vulkan d3d12)

option(ENGINE_ENABLE_VULKAN "Build Vulkan backend target" ON)
if(WIN32)
    option(ENGINE_ENABLE_D3D12 "Build D3D12 backend target" ON)
else()
    option(ENGINE_ENABLE_D3D12 "Build D3D12 backend target" OFF)
endif()

string(TOLOWER "${ENGINE_BACKEND}" ENGINE_BACKEND_NORMALIZED)
set(_valid_backends opengl vulkan d3d12)
list(FIND _valid_backends "${ENGINE_BACKEND_NORMALIZED}" _backend_index)
if(_backend_index EQUAL -1)
    message(FATAL_ERROR "ENGINE_BACKEND must be one of: opengl, vulkan, d3d12")
endif()

if(ENGINE_BACKEND_NORMALIZED STREQUAL "vulkan" AND NOT ENGINE_ENABLE_VULKAN)
    message(FATAL_ERROR "ENGINE_BACKEND=vulkan requires ENGINE_ENABLE_VULKAN=ON")
endif()

if(ENGINE_BACKEND_NORMALIZED STREQUAL "d3d12")
    if(NOT WIN32)
        message(FATAL_ERROR "ENGINE_BACKEND=d3d12 is only supported on Windows")
    endif()
    if(NOT ENGINE_ENABLE_D3D12)
        message(FATAL_ERROR "ENGINE_BACKEND=d3d12 requires ENGINE_ENABLE_D3D12=ON")
    endif()
endif()

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
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
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.8
)
FetchContent_MakeAvailable(glad)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GLAD_GENERATED_DIR "${CMAKE_BINARY_DIR}/_deps/glad-generated")
add_custom_command(
    OUTPUT
        "${GLAD_GENERATED_DIR}/src/gl.c"
        "${GLAD_GENERATED_DIR}/include/glad/gl.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLAD_GENERATED_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${glad_SOURCE_DIR}
        "${Python3_EXECUTABLE}" -m glad --api=gl:core=3.3 --out-path "${GLAD_GENERATED_DIR}" --reproducible c
    DEPENDS "${glad_SOURCE_DIR}/glad/__main__.py"
    COMMENT "Generating glad OpenGL 3.3 loader"
    VERBATIM
)
add_library(glad_gl_core_33 STATIC "${GLAD_GENERATED_DIR}/src/gl.c")
target_include_directories(glad_gl_core_33 PUBLIC "${GLAD_GENERATED_DIR}/include")

if(ENGINE_ENABLE_VULKAN)
    FetchContent_Declare(
        vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.3.296
    )
    FetchContent_MakeAvailable(vulkan_headers)
endif()

function(set_engine_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

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

add_library(engine_rhi_opengl STATIC
    engine/rhi_opengl/GLDevice.cpp
    engine/rhi_opengl/GLSwapchain.cpp
    engine/rhi_opengl/GLCommandBuffer.cpp
    engine/rhi_opengl/GLPipeline.cpp
    engine/rhi_opengl/GLResources.cpp
    engine/rhi_opengl/GLShader.cpp
    engine/rhi_opengl/GLUtils.cpp
)
target_include_directories(engine_rhi_opengl PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_rhi_opengl PUBLIC engine_core engine_platform PRIVATE glad_gl_core_33 glfw glm::glm)
target_compile_definitions(engine_rhi_opengl PRIVATE GLFW_INCLUDE_NONE=1)
set_engine_warnings(engine_rhi_opengl)

if(ENGINE_ENABLE_VULKAN)
    add_library(engine_rhi_vulkan STATIC
        engine/rhi_vulkan/VkDevice.cpp
        engine/rhi_vulkan/VkSwapchain.cpp
        engine/rhi_vulkan/VkCommandBuffer.cpp
        engine/rhi_vulkan/VkPipeline.cpp
        engine/rhi_vulkan/VkResources.cpp
        engine/rhi_vulkan/VkSync.cpp
    )
    target_include_directories(engine_rhi_vulkan PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
    if(TARGET Vulkan::Headers)
        target_link_libraries(engine_rhi_vulkan PUBLIC engine_core Vulkan::Headers)
    else()
        target_link_libraries(engine_rhi_vulkan PUBLIC engine_core)
    endif()
    target_compile_definitions(engine_rhi_vulkan PRIVATE ENGINE_ENABLE_VULKAN_BACKEND=1)
    set_engine_warnings(engine_rhi_vulkan)
endif()

if(WIN32 AND ENGINE_ENABLE_D3D12)
    add_library(engine_rhi_d3d12 STATIC
        engine/rhi_d3d12/DxDevice.cpp
        engine/rhi_d3d12/DxSwapchain.cpp
        engine/rhi_d3d12/DxCommandBuffer.cpp
        engine/rhi_d3d12/DxPipeline.cpp
        engine/rhi_d3d12/DxResources.cpp
        engine/rhi_d3d12/DxSync.cpp
    )
    target_include_directories(engine_rhi_d3d12 PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
    target_link_libraries(engine_rhi_d3d12 PUBLIC engine_core)
    target_compile_definitions(engine_rhi_d3d12 PRIVATE ENGINE_ENABLE_D3D12_BACKEND=1)
    set_engine_warnings(engine_rhi_d3d12)
endif()

add_library(engine_rhi STATIC
    engine/rhi/RHI.cpp
)
target_include_directories(engine_rhi PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_compile_definitions(engine_rhi PRIVATE
    ENGINE_ENABLE_OPENGL_BACKEND=1
    ENGINE_ENABLE_VULKAN_BACKEND=$<BOOL:${ENGINE_ENABLE_VULKAN}>
    ENGINE_ENABLE_D3D12_BACKEND=$<BOOL:${ENGINE_ENABLE_D3D12}>
)
target_link_libraries(engine_rhi PUBLIC engine_core PRIVATE engine_rhi_opengl)
if(TARGET engine_rhi_vulkan)
    target_link_libraries(engine_rhi PRIVATE engine_rhi_vulkan)
endif()
if(TARGET engine_rhi_d3d12)
    target_link_libraries(engine_rhi PRIVATE engine_rhi_d3d12)
endif()
set_engine_warnings(engine_rhi)

add_library(engine_renderer STATIC
    engine/renderer/Renderer.cpp
)
target_include_directories(engine_renderer PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_renderer PUBLIC engine_core engine_rhi glm::glm)
set_engine_warnings(engine_renderer)

add_library(engine_game STATIC
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
target_compile_definitions(app PRIVATE ENGINE_BACKEND_NAME="${ENGINE_BACKEND_NORMALIZED}")
target_link_libraries(app PRIVATE engine_core engine_platform engine_renderer engine_game)
set_engine_warnings(app)

add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:app>/assets"
    COMMENT "Copying assets to runtime output directory"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/CMakeLists.txt")
    add_subdirectory(tools/shader_compiler)
endif()
