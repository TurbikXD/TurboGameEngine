cmake_minimum_required(VERSION 3.24)

file(LOCK "${CMAKE_BINARY_DIR}/tge-configure.lock" GUARD PROCESS TIMEOUT 120)

project(TurboGameEngine VERSION 0.1.0 LANGUAGES C CXX)
enable_testing()

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
option(ENGINE_ENABLE_TRACY "Enable Tracy instrumentation in engine targets" OFF)
option(ENGINE_PACKAGED_RUNTIME "Resolve runtime assets from the distribution" OFF)
set(ENGINE_BUILD_ID "local" CACHE STRING "Identifier embedded in the application and its crash reports")
set(ENGINE_BUILD_COMMIT "unknown" CACHE STRING "Source commit embedded in the application and its crash reports")
string(LENGTH "${ENGINE_BUILD_ID}" engine_build_id_length)
string(LENGTH "${ENGINE_BUILD_COMMIT}" engine_build_commit_length)
if(NOT ENGINE_BUILD_ID MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$" OR engine_build_id_length GREATER 80)
    message(FATAL_ERROR "ENGINE_BUILD_ID must be a filename-safe identifier of at most 80 characters")
endif()
if(NOT ENGINE_BUILD_COMMIT STREQUAL "unknown" AND
   (NOT ENGINE_BUILD_COMMIT MATCHES "^[0-9a-f]+$" OR NOT engine_build_commit_length EQUAL 40))
    message(FATAL_ERROR "ENGINE_BUILD_COMMIT must be a full lowercase Git SHA or unknown")
endif()
set(ENGINE_STEAMWORKS_SDK "" CACHE PATH "Optional Steamworks SDK directory containing public and redistributable_bin")
set(ENGINE_STEAMWORKS_DLL "" CACHE FILEPATH "Override steam_api64.dll path for a split Steamworks SDK install")
set(DILIGENT_SAMPLE_MODE "d3d12" CACHE STRING "Runtime backend mode for Diligent sample launch targets")
set_property(CACHE DILIGENT_SAMPLE_MODE PROPERTY STRINGS d3d11 d3d12 vk gl webgpu)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Disable remote updates for already populated dependencies" FORCE)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

set(ENKITS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_C_INTERFACE OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ENKITS_INSTALL OFF CACHE BOOL "" FORCE)
set(ENKITS_TASK_PRIORITIES_NUM 3 CACHE STRING "" FORCE)
FetchContent_Declare(
    enkits
    GIT_REPOSITORY https://github.com/dougbinks/enkiTS.git
    GIT_TAG 0289cf6ffce86697e1f46cb4eeca1c946bffec71
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(enkits)

if(ENGINE_ENABLE_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.13.1
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED ON
    )
    FetchContent_MakeAvailable(tracy)
endif()

function(enable_engine_tracy target_name)
    if(ENGINE_ENABLE_TRACY)
        target_link_libraries(${target_name} PRIVATE Tracy::TracyClient)
        target_compile_definitions(${target_name} PRIVATE ENGINE_TRACY_PROFILE=1)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE "$<$<CONFIG:Release>:/Zi>")
            target_link_options(${target_name} PRIVATE "$<$<CONFIG:Release>:/DEBUG>")
        endif()
    endif()
endfunction()

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

if(WIN32)
    find_package(Git REQUIRED)
    set(glfw_win32_patch "${CMAKE_CURRENT_SOURCE_DIR}/third_party/patches/glfw-win32-active-window.patch")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${glfw_SOURCE_DIR}" apply --reverse --check "${glfw_win32_patch}"
        RESULT_VARIABLE glfw_win32_patch_applied
        OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT glfw_win32_patch_applied EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${glfw_SOURCE_DIR}" apply "${glfw_win32_patch}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
endif()

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.15.0
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(entt)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG 2d88f9b5facaa9e4ec91247b39ae8251bd49a244
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(luau)

if(WIN32)
    FetchContent_Declare(stylua_runtime
        URL https://github.com/JohnnyMorganz/StyLua/releases/download/v2.5.2/stylua-windows-x86_64.zip
        URL_HASH SHA256=e77d0ea1226b8b389b43f702240091249a96eea25857281f90ea24d0eb9eb969
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(stylua_runtime)
endif()

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
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v5.4.3
    UPDATE_DISCONNECTED ON
)
if(DEFINED BUILD_SHARED_LIBS)
    set(_ENGINE_PREV_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
    set(_ENGINE_HAD_BUILD_SHARED_LIBS TRUE)
else()
    set(_ENGINE_HAD_BUILD_SHARED_LIBS FALSE)
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(assimp)
if(_ENGINE_HAD_BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS "${_ENGINE_PREV_BUILD_SHARED_LIBS}" CACHE BOOL "" FORCE)
else()
    unset(BUILD_SHARED_LIBS CACHE)
endif()
unset(_ENGINE_PREV_BUILD_SHARED_LIBS)
unset(_ENGINE_HAD_BUILD_SHARED_LIBS)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 664cf2d
    UPDATE_DISCONNECTED ON
)
FetchContent_GetProperties(imguizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(imguizmo)
endif()

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
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(DILIGENT_LOAD_PIX_EVENT_RUNTIME ON CACHE BOOL "" FORCE)
endif()
if(ENGINE_ENABLE_DILIGENT_WEBGPU)
    set(DILIGENT_NO_WEBGPU OFF CACHE BOOL "" FORCE)
else()
    set(DILIGENT_NO_WEBGPU ON CACHE BOOL "" FORCE)
endif()

add_subdirectory("${DILIGENT_ENGINE_DIR}")

set(ENGINE_SPIRV_TOOLS_SOURCE_DIR
    "${DILIGENT_ENGINE_DIR}/DiligentCore/ThirdParty/SPIRV-Tools/source")

function(engine_fix_spirv_tools_pch_includes target_name)
    if(TARGET ${target_name})
        target_include_directories(${target_name} PRIVATE ${ARGN})
    endif()
endfunction()

engine_fix_spirv_tools_pch_includes(
    SPIRV-Tools-shared
    "${ENGINE_SPIRV_TOOLS_SOURCE_DIR}"
)
engine_fix_spirv_tools_pch_includes(
    SPIRV-Tools-static
    "${ENGINE_SPIRV_TOOLS_SOURCE_DIR}"
)
engine_fix_spirv_tools_pch_includes(
    SPIRV-Tools-opt
    "${ENGINE_SPIRV_TOOLS_SOURCE_DIR}"
    "${ENGINE_SPIRV_TOOLS_SOURCE_DIR}/opt"
)
engine_fix_spirv_tools_pch_includes(
    SPIRV-Tools-reduce
    "${ENGINE_SPIRV_TOOLS_SOURCE_DIR}"
    "${ENGINE_SPIRV_TOOLS_SOURCE_DIR}/reduce"
)

if(MSVC AND TARGET SPIRV-Tools-opt)
    target_compile_options(SPIRV-Tools-opt PRIVATE /wd4717 /wd5232 /MP1)
endif()

function(set_engine_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
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

    get_target_property(_target_type ${target_name} TYPE)
    if(NOT _target_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY)$")
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
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" ENGINE_SOURCE_ROOT_CMAKE_PATH)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/engine/game/ImGuizmoCompat.h" ENGINE_IMGUIZMO_COMPAT_HEADER_CMAKE_PATH)

add_library(engine_core STATIC
    engine/core/Log.cpp
    engine/core/CrashReport.cpp
    engine/core/Time.cpp
    engine/core/Config.cpp
    engine/core/ProjectManager.cpp
    engine/core/AtomicFile.cpp
    engine/core/NetworkTestControl.cpp
    engine/core/NetworkTestCsv.cpp
    engine/core/PlaySession.cpp
    engine/core/LiveFileWatcher.cpp
    engine/core/JobSystem.cpp
)
target_include_directories(engine_core PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_core PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json PRIVATE enkiTS)
if(WIN32)
    target_link_libraries(engine_core PRIVATE shell32 ole32 uuid)
endif()
set_engine_warnings(engine_core)
enable_engine_tracy(engine_core)

add_library(engine_ecs STATIC engine/ecs/world.cpp)
target_include_directories(engine_ecs PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_ecs PUBLIC engine_core glm::glm EnTT::EnTT)
target_compile_definitions(engine_ecs PUBLIC ENTT_NO_ETO)
set_engine_warnings(engine_ecs)
enable_engine_tracy(engine_ecs)

add_library(engine_platform STATIC
    engine/platform/Window.cpp
    engine/platform/WindowGLFW.cpp
    engine/platform/Input.cpp
)
target_include_directories(engine_platform PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_platform PUBLIC engine_core glfw)
set_engine_warnings(engine_platform)

add_library(engine_scripting STATIC
    engine/scripting/ScriptAudio.cpp
    engine/scripting/ScriptingSystem.cpp
    engine/scripting/react/ReactRuntime.cpp
)
target_include_directories(engine_scripting PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_scripting PUBLIC engine_ecs engine_platform Luau.VM Luau.Compiler)
if(WIN32)
    target_link_libraries(engine_scripting PRIVATE xaudio2 ole32)
endif()
if(ENGINE_PACKAGED_RUNTIME)
    target_compile_definitions(engine_scripting PRIVATE ENGINE_PACKAGED_RUNTIME=1)
else()
    target_compile_definitions(engine_scripting PRIVATE ENGINE_REACT_PACKAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/react-luau")
endif()
set_engine_warnings(engine_scripting)
enable_engine_tracy(engine_scripting)

add_library(engine_text_editor STATIC third_party/ImGuiColorTextEdit/TextEditor.cpp)
target_include_directories(engine_text_editor PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}"
    "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui")
target_link_libraries(engine_text_editor PUBLIC Diligent-Imgui)

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
if(MSVC)
    set_property(SOURCE engine/rhi_diligent/DiligentDevice.cpp APPEND PROPERTY COMPILE_OPTIONS /GR)
endif()
set_engine_warnings(engine_rhi_diligent)
enable_engine_tracy(engine_rhi_diligent)

add_library(engine_rhi STATIC
    engine/rhi/RHI.cpp
)
target_include_directories(engine_rhi PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_rhi PUBLIC engine_core PRIVATE engine_rhi_diligent)
set_engine_warnings(engine_rhi)

add_library(engine_renderer STATIC
    engine/renderer/Renderer.cpp
    engine/editor/EditorTheme.cpp
    engine/renderer/UiCanvasCompositor.cpp
    engine/resources/resource_manager.cpp
    engine/resources/async_load_queue.cpp
    engine/resources/image_mips.cpp
    engine/resources/loaders.cpp
    engine/resources/material.cpp
    engine/resources/material_graph.cpp
    engine/ecs/render_system.cpp
    engine/ecs/transform_utils.cpp
    engine/ecs/transform_batch.cpp
    engine/ecs/collision_utils.cpp
)
target_include_directories(engine_renderer PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_include_directories(engine_renderer PRIVATE "${stb_SOURCE_DIR}")
target_compile_definitions(engine_renderer PUBLIC ENGINE_SOURCE_ROOT="${ENGINE_SOURCE_ROOT_CMAKE_PATH}")
if(ENGINE_PACKAGED_RUNTIME)
    target_compile_definitions(engine_renderer PRIVATE ENGINE_PACKAGED_RUNTIME=1)
endif()
target_link_libraries(
    engine_renderer
    PUBLIC
        engine_core
        engine_rhi
        engine_ecs
        glm::glm
        nlohmann_json::nlohmann_json
    PRIVATE
        assimp::assimp
)
if(TARGET Diligent-Imgui)
    target_link_libraries(engine_renderer PRIVATE Diligent-Imgui)
endif()
if(TARGET Diligent-BuildSettings)
    target_link_libraries(engine_renderer PRIVATE Diligent-BuildSettings)
endif()
if(MSVC)
    set_property(SOURCE engine/renderer/Renderer.cpp APPEND PROPERTY COMPILE_OPTIONS /GR)
endif()
if(EXISTS "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui/imgui_demo.cpp")
    target_sources(engine_renderer PRIVATE
        "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui/imgui_demo.cpp"
    )
endif()
set_engine_warnings(engine_renderer)
enable_engine_tracy(engine_renderer)

add_library(imguizmo STATIC
    "${imguizmo_SOURCE_DIR}/ImGuizmo.cpp"
)
target_include_directories(
    imguizmo
    PUBLIC
        "${imguizmo_SOURCE_DIR}"
    PRIVATE
        "${ENGINE_ROOT_INCLUDE_DIR}"
        "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui"
)
if(TARGET Diligent-Imgui)
    target_link_libraries(imguizmo PUBLIC Diligent-Imgui)
endif()
if(MSVC)
    target_compile_options(imguizmo PRIVATE "/FI${ENGINE_IMGUIZMO_COMPAT_HEADER_CMAKE_PATH}")
else()
    target_compile_options(imguizmo PRIVATE -include "${ENGINE_IMGUIZMO_COMPAT_HEADER_CMAKE_PATH}")
endif()
set_engine_warnings(imguizmo)

find_package(Threads REQUIRED)
add_library(engine_network STATIC
    engine/network/DatagramTransport.cpp
    engine/network/NetworkClient.cpp
    engine/network/NetworkServer.cpp
    engine/network/SteamTransport.cpp
)
target_include_directories(engine_network PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_network PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(engine_network PRIVATE ws2_32)
endif()
if(ENGINE_STEAMWORKS_SDK)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Steamworks support requires a Windows x64 MSVC build")
    endif()
    set(steamworks_include "${ENGINE_STEAMWORKS_SDK}/public")
    set(steamworks_library "${ENGINE_STEAMWORKS_SDK}/redistributable_bin/win64/steam_api64.lib")
    set(steamworks_runtime "${ENGINE_STEAMWORKS_SDK}/redistributable_bin/win64/steam_api64.dll")
    if(ENGINE_STEAMWORKS_DLL)
        set(steamworks_runtime "${ENGINE_STEAMWORKS_DLL}")
    endif()
    if(NOT EXISTS "${steamworks_include}/steam/steam_api.h" OR
       NOT EXISTS "${steamworks_library}" OR NOT EXISTS "${steamworks_runtime}")
        message(FATAL_ERROR "Steamworks SDK headers, steam_api64.lib or steam_api64.dll are missing")
    endif()
    target_include_directories(engine_network SYSTEM PRIVATE "${steamworks_include}")
    target_compile_definitions(engine_network PRIVATE ENGINE_ENABLE_STEAMWORKS=1)
    target_link_libraries(engine_network PRIVATE "${steamworks_library}" delayimp)
    target_link_options(engine_network INTERFACE "/DELAYLOAD:steam_api64.dll")
endif()
set_engine_warnings(engine_network)

add_library(engine_game STATIC
    engine/editor/EditorCommands.cpp
    engine/editor/EditorRibbon.cpp
    engine/editor/NetworkChartPng.cpp
    engine/editor/EditorIcons.cpp
    engine/editor/ScriptWorkspace.cpp
    engine/editor/ProjectLauncher.cpp
    engine/editor/CodexAssistant.cpp
    engine/editor/ScriptFormatter.cpp
    engine/editor/MaterialInspector.cpp
    engine/editor/MaterialGraphEditor.cpp
    engine/scripting/react/ReactUI.cpp
    engine/ecs/physics_system.cpp
    engine/ecs/debug_render_system.cpp
    engine/game/StateStack.cpp
    engine/game/FreeCameraController.cpp
    engine/game/MenuState.cpp
    engine/game/GameplayState.cpp
    engine/game/GameplayCommands.cpp
    engine/game/NetworkArena.cpp
    engine/game/NetworkArenaRules.cpp
    engine/game/SceneNetwork.cpp
    engine/game/SceneSteam.cpp
    engine/game/SceneNetworkServer.cpp
    engine/game/PauseState.cpp
    "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui/misc/cpp/imgui_stdlib.cpp"
)
target_include_directories(engine_game PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_include_directories(
    engine_game
    PRIVATE
        "${imguizmo_SOURCE_DIR}"
        "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui"
        "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui/misc/cpp"
)
target_link_libraries(engine_game PUBLIC engine_core engine_platform engine_renderer engine_scripting engine_network PRIVATE imguizmo engine_text_editor)
if(WIN32)
    target_link_libraries(engine_game PRIVATE shell32 ole32 uuid gdiplus)
    target_compile_definitions(engine_game PRIVATE ENGINE_STYLUA_EXECUTABLE="${stylua_runtime_SOURCE_DIR}/stylua.exe")
endif()
set_engine_warnings(engine_game)
enable_engine_tracy(engine_game)

add_executable(app
    app/main.cpp
    app/Lab5Session.cpp
    engine/core/Application.cpp
)
target_include_directories(app PRIVATE "${ENGINE_ROOT_INCLUDE_DIR}")
target_compile_definitions(app PRIVATE
    ENGINE_BACKEND_NAME="diligent"
    ENGINE_BUILD_ID="${ENGINE_BUILD_ID}"
    ENGINE_BUILD_COMMIT="${ENGINE_BUILD_COMMIT}"
)
target_link_libraries(app PRIVATE engine_core engine_platform engine_renderer engine_game)
if(ENGINE_PACKAGED_RUNTIME AND MSVC)
    target_link_options(app PRIVATE /DEBUG:FULL /INCREMENTAL:NO /PDBALTPATH:app.pdb)
endif()
set_engine_warnings(app)
enable_engine_tracy(app)
copy_pix_runtime_dll(app)
if(ENGINE_STEAMWORKS_SDK)
    add_custom_command(TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${steamworks_runtime}" "$<TARGET_FILE_DIR:app>/steam_api64.dll"
        VERBATIM
    )
endif()

add_executable(engine_network_tests tests/NetworkSessionTests.cpp)
target_link_libraries(engine_network_tests PRIVATE engine_network)
set_engine_warnings(engine_network_tests)
add_test(NAME engine_network_tests COMMAND engine_network_tests)
set_tests_properties(engine_network_tests PROPERTIES TIMEOUT 60)

add_executable(engine_physics_tests
    tests/PhysicsSystemTests.cpp
)
target_include_directories(engine_physics_tests PRIVATE "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_physics_tests PRIVATE engine_core engine_platform engine_renderer engine_game)
set_engine_warnings(engine_physics_tests)
add_test(NAME engine_physics_tests COMMAND engine_physics_tests)

add_executable(engine_job_system_tests
    tests/JobSystemTests.cpp
)
target_include_directories(engine_job_system_tests PRIVATE "${ENGINE_ROOT_INCLUDE_DIR}")
target_link_libraries(engine_job_system_tests PRIVATE engine_core)
set_engine_warnings(engine_job_system_tests)
add_test(NAME engine_job_system_tests COMMAND engine_job_system_tests)

add_executable(engine_async_load_tests tests/AsyncLoadQueueTests.cpp)
target_link_libraries(engine_async_load_tests PRIVATE engine_renderer)
set_engine_warnings(engine_async_load_tests)
add_test(NAME engine_async_load_tests COMMAND engine_async_load_tests)

add_executable(engine_world_tests tests/WorldTests.cpp)
target_link_libraries(engine_world_tests PRIVATE engine_ecs)
set_engine_warnings(engine_world_tests)
add_test(NAME engine_world_tests COMMAND engine_world_tests)

add_executable(engine_transform_batch_tests tests/TransformBatchTests.cpp)
target_link_libraries(engine_transform_batch_tests PRIVATE engine_renderer)
set_engine_warnings(engine_transform_batch_tests)
add_test(NAME engine_transform_batch_tests COMMAND engine_transform_batch_tests)

add_executable(engine_texture_loader_tests tests/TextureLoaderTests.cpp)
target_link_libraries(engine_texture_loader_tests PRIVATE engine_renderer)
set_engine_warnings(engine_texture_loader_tests)
add_test(NAME engine_texture_loader_tests COMMAND engine_texture_loader_tests)

add_executable(engine_material_tests tests/MaterialTests.cpp)
target_link_libraries(engine_material_tests PRIVATE engine_renderer)
set_engine_warnings(engine_material_tests)
add_test(NAME engine_material_tests COMMAND engine_material_tests)
set_tests_properties(engine_material_tests PROPERTIES TIMEOUT 60)

add_executable(engine_material_graph_tests tests/MaterialGraphTests.cpp)
target_link_libraries(engine_material_graph_tests PRIVATE engine_renderer)
set_engine_warnings(engine_material_graph_tests)
add_test(NAME engine_material_graph_tests COMMAND engine_material_graph_tests)
set_tests_properties(engine_material_graph_tests PROPERTIES TIMEOUT 60)

add_executable(engine_material_gpu_tests tests/MaterialGpuTests.cpp)
target_link_libraries(engine_material_gpu_tests PRIVATE engine_renderer engine_platform engine_rhi_diligent glfw)
if(WIN32)
    target_link_libraries(engine_material_gpu_tests PRIVATE Psapi)
endif()
set_engine_warnings(engine_material_gpu_tests)
copy_pix_runtime_dll(engine_material_gpu_tests)

add_executable(engine_lab3_demo_tests tests/Lab3DemoTests.cpp)
target_link_libraries(engine_lab3_demo_tests PRIVATE engine_scripting)
target_compile_definitions(engine_lab3_demo_tests PRIVATE ENGINE_LAB3_DEMO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/demos/lab3-materials")
set_engine_warnings(engine_lab3_demo_tests)
add_test(NAME engine_lab3_demo_tests COMMAND engine_lab3_demo_tests)
set_tests_properties(engine_lab3_demo_tests PROPERTIES TIMEOUT 60)

add_executable(engine_gameplay_state_tests tests/GameplayStateTests.cpp)
target_include_directories(engine_gameplay_state_tests PRIVATE "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui")
target_link_libraries(engine_gameplay_state_tests PRIVATE engine_game)
set_engine_warnings(engine_gameplay_state_tests)
add_test(NAME engine_gameplay_state_tests COMMAND engine_gameplay_state_tests)

add_executable(engine_editor_command_tests tests/EditorCommandTests.cpp)
target_include_directories(engine_editor_command_tests PRIVATE "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui")
target_link_libraries(engine_editor_command_tests PRIVATE engine_game)
set_engine_warnings(engine_editor_command_tests)
add_test(NAME engine_editor_command_tests COMMAND engine_editor_command_tests)
set_tests_properties(engine_editor_command_tests PROPERTIES TIMEOUT 60)

add_executable(engine_script_workspace_command_tests tests/ScriptWorkspaceCommandTests.cpp)
target_include_directories(engine_script_workspace_command_tests PRIVATE "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui")
target_link_libraries(engine_script_workspace_command_tests PRIVATE engine_game)
set_engine_warnings(engine_script_workspace_command_tests)
add_test(NAME engine_script_workspace_command_tests COMMAND engine_script_workspace_command_tests)
set_tests_properties(engine_script_workspace_command_tests PROPERTIES TIMEOUT 60)

add_executable(engine_scripting_tests tests/ScriptingSystemTests.cpp)
add_executable(engine_live_file_watcher_tests tests/LiveFileWatcherTests.cpp)
target_link_libraries(engine_live_file_watcher_tests PRIVATE engine_core)
set_engine_warnings(engine_live_file_watcher_tests)
add_test(NAME engine_live_file_watcher_tests COMMAND engine_live_file_watcher_tests)
target_link_libraries(engine_scripting_tests PRIVATE engine_scripting)
target_compile_definitions(engine_scripting_tests PRIVATE ENGINE_TEST_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets")
set_engine_warnings(engine_scripting_tests)
add_test(NAME engine_scripting_tests COMMAND engine_scripting_tests)
set_tests_properties(engine_scripting_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(engine_react_runtime_tests tests/ReactRuntimeTests.cpp)
target_link_libraries(engine_react_runtime_tests PRIVATE engine_scripting)
target_compile_definitions(engine_react_runtime_tests PRIVATE ENGINE_REACT_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
set_engine_warnings(engine_react_runtime_tests)
add_test(NAME engine_react_runtime_tests COMMAND engine_react_runtime_tests)
set_tests_properties(engine_react_runtime_tests PROPERTIES TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(engine_codex_assistant_tests tests/CodexAssistantTests.cpp)
target_include_directories(engine_codex_assistant_tests PRIVATE "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui")
target_link_libraries(engine_codex_assistant_tests PRIVATE engine_game)
set_engine_warnings(engine_codex_assistant_tests)
add_test(NAME engine_codex_assistant_tests COMMAND engine_codex_assistant_tests)
set_tests_properties(engine_scripting_tests engine_codex_assistant_tests PROPERTIES TIMEOUT 60)

add_executable(engine_script_formatter_tests tests/ScriptFormatterTests.cpp)
target_link_libraries(engine_script_formatter_tests PRIVATE engine_game)
set_engine_warnings(engine_script_formatter_tests)
add_test(NAME engine_script_formatter_tests COMMAND engine_script_formatter_tests)
set_tests_properties(engine_script_formatter_tests PROPERTIES TIMEOUT 60)

add_executable(engine_script_editor_tests tests/ScriptEditorTests.cpp)
target_link_libraries(engine_script_editor_tests PRIVATE engine_text_editor)
set_engine_warnings(engine_script_editor_tests)
add_test(NAME engine_script_editor_tests COMMAND engine_script_editor_tests)
set_tests_properties(engine_script_editor_tests PROPERTIES TIMEOUT 60)

add_executable(engine_project_manager_tests tests/ProjectManagerTests.cpp)
target_link_libraries(engine_project_manager_tests PRIVATE engine_core)
set_engine_warnings(engine_project_manager_tests)
add_test(NAME engine_project_manager_tests COMMAND engine_project_manager_tests)
set_tests_properties(engine_project_manager_tests PROPERTIES TIMEOUT 60)

add_executable(engine_atomic_file_tests tests/AtomicFileTests.cpp)
target_link_libraries(engine_atomic_file_tests PRIVATE engine_core)
set_engine_warnings(engine_atomic_file_tests)
add_test(NAME engine_atomic_file_tests COMMAND engine_atomic_file_tests)
set_tests_properties(engine_atomic_file_tests PROPERTIES TIMEOUT 60)

add_executable(engine_project_asset_tests tests/ProjectAssetTests.cpp)
target_link_libraries(engine_project_asset_tests PRIVATE engine_renderer)
set_engine_warnings(engine_project_asset_tests)
add_test(NAME engine_project_asset_tests COMMAND engine_project_asset_tests)
set_tests_properties(engine_project_asset_tests PROPERTIES TIMEOUT 60)

add_executable(engine_project_launcher_tests tests/ProjectLauncherTests.cpp)
target_include_directories(engine_project_launcher_tests PRIVATE "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui")
target_link_libraries(engine_project_launcher_tests PRIVATE engine_game)
set_engine_warnings(engine_project_launcher_tests)
add_test(NAME engine_project_launcher_tests COMMAND engine_project_launcher_tests)
set_tests_properties(engine_project_launcher_tests PROPERTIES TIMEOUT 60)

add_executable(engine_gpu_upload_tests tests/GpuUploadTests.cpp)
target_link_libraries(engine_gpu_upload_tests PRIVATE engine_renderer engine_platform engine_rhi_diligent glfw)
set_engine_warnings(engine_gpu_upload_tests)
copy_pix_runtime_dll(engine_gpu_upload_tests)

add_executable(engine_canvas_gpu_tests tests/CanvasGpuTests.cpp)
target_include_directories(engine_canvas_gpu_tests PRIVATE "${DILIGENT_ENGINE_DIR}/DiligentTools/ThirdParty/imgui")
target_link_libraries(engine_canvas_gpu_tests PRIVATE engine_renderer engine_platform engine_rhi_diligent glfw Diligent-Imgui)
set_engine_warnings(engine_canvas_gpu_tests)
copy_pix_runtime_dll(engine_canvas_gpu_tests)

set_tests_properties(engine_physics_tests engine_job_system_tests engine_async_load_tests engine_world_tests engine_transform_batch_tests engine_texture_loader_tests engine_gameplay_state_tests
    PROPERTIES TIMEOUT 60)

if(COMMAND copy_required_dlls)
    copy_required_dlls(app)
endif()

add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:app>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/react-luau"
        "$<TARGET_FILE_DIR:app>/assets/scripts/react-luau"
    COMMENT "Copying assets to runtime output directory"
)

if(WIN32)
    add_custom_command(TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:app>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${stylua_runtime_SOURCE_DIR}/stylua.exe" "$<TARGET_FILE_DIR:app>/tools/stylua.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/StyLua/LICENSE.md" "$<TARGET_FILE_DIR:app>/tools/StyLua-LICENSE.md"
    )
endif()

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
