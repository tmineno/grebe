cmake_minimum_required(VERSION 3.24)
project(grebe VERSION 2.0.0 LANGUAGES CXX C)

# =============================================================================
# Windows Cross-Build from WSL2
# =============================================================================
# When WINDOWS_CROSS_BUILD=ON, skip all normal build logic and only define
# custom targets that invoke the Windows build/run scripts via MSVC.
# Usage: cmake --preset windows-release && cmake --build --preset windows-release

option(WINDOWS_CROSS_BUILD "Cross-build for Windows via MSVC from WSL2" OFF)

if(WINDOWS_CROSS_BUILD)
    add_custom_target(windows-build ALL
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/build-windows.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Building Windows native .exe (MSVC)"
    )
    add_custom_target(run
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/run-windows.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running Windows native .exe"
    )
    add_custom_target(profile
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/run-windows.sh --profile --ring-size=64M
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running profile (Windows native)"
    )
    add_custom_target(bench
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/run-windows.sh --bench
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running microbenchmarks (Windows native)"
    )
    return()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Dependencies
# =============================================================================

include(FetchContent)

# Vulkan SDK (must be installed)
find_package(Vulkan REQUIRED)

# OpenGL (for grebe-sg signal gateway UI)
find_package(OpenGL REQUIRED)

# GLFW
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)

# vk-bootstrap
FetchContent_Declare(
    vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG        v1.3.290
    GIT_SHALLOW    TRUE
)

# VMA (Vulkan Memory Allocator)
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0
    GIT_SHALLOW    TRUE
)

# glm
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# stb (no CMake support - create interface library)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)

# Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(glfw vk-bootstrap VulkanMemoryAllocator glm spdlog json stb imgui)

# stb interface library
add_library(stb_headers INTERFACE)
target_include_directories(stb_headers INTERFACE ${stb_SOURCE_DIR})

# Dear ImGui library (Vulkan backend)
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw Vulkan::Vulkan)

# Dear ImGui library (OpenGL3 backend, for grebe-sg)
add_library(imgui_opengl_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_opengl_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_opengl_lib PUBLIC glfw OpenGL::GL)

# =============================================================================
# Shader compilation
# =============================================================================

find_program(GLSLC glslc HINTS
    ${Vulkan_GLSLC_EXECUTABLE}
    $ENV{VULKAN_SDK}/bin
    $ENV{VULKAN_SDK}/Bin
)

if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found. Ensure Vulkan SDK is installed and glslc is on PATH.")
endif()

set(SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)

file(GLOB SHADER_SOURCES
    ${SHADER_SOURCE_DIR}/*.vert
    ${SHADER_SOURCE_DIR}/*.frag
    ${SHADER_SOURCE_DIR}/*.comp
)

file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

set(SHADER_SPIRV_FILES "")
foreach(SHADER_SRC ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER_SRC} NAME)
    set(SHADER_SPV ${SHADER_BINARY_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SHADER_SPV}
        COMMAND ${GLSLC} ${SHADER_SRC} -o ${SHADER_SPV}
        DEPENDS ${SHADER_SRC}
        COMMENT "Compiling shader: ${SHADER_NAME}"
    )
    list(APPEND SHADER_SPIRV_FILES ${SHADER_SPV})
endforeach()

add_custom_target(compile_shaders DEPENDS ${SHADER_SPIRV_FILES})

# =============================================================================
# libgrebe — Core library
# =============================================================================

add_library(grebe STATIC
    # Data pipeline core
    src/decimator.cpp
    src/data_generator.cpp
    src/decimation_thread.cpp
    src/decimation_engine.cpp
    src/synthetic_source.cpp
    src/ingestion_thread.cpp
)

target_include_directories(grebe PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(grebe PUBLIC
    spdlog::spdlog
)

if(MSVC)
    target_compile_options(grebe PRIVATE /W4)
else()
    target_compile_options(grebe PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =============================================================================
# grebe-viewer — Reference viewer application
# =============================================================================

add_executable(grebe-viewer
    apps/viewer/main.cpp
    apps/viewer/app_loop.cpp
    apps/viewer/cli.cpp
    apps/viewer/microbench.cpp
    # App-only modules (moved from libgrebe Phase 6)
    apps/viewer/app_command.cpp
    apps/viewer/hud.cpp
    apps/viewer/profiler.cpp
    apps/viewer/envelope_verifier.cpp
    apps/viewer/process_handle.cpp
    # Vulkan rendering + benchmark (moved from libgrebe Phase 7)
    apps/viewer/vulkan_context.cpp
    apps/viewer/swapchain.cpp
    apps/viewer/renderer.cpp
    apps/viewer/buffer_manager.cpp
    apps/viewer/vulkan_renderer.cpp
    apps/viewer/compute_decimator.cpp
    apps/viewer/vma_impl.cpp
    apps/viewer/benchmark.cpp
    # IPC transport (moved from libgrebe Phase 7.2)
    apps/viewer/ipc_source.cpp
    apps/common/ipc/pipe_transport.cpp
)

add_dependencies(grebe-viewer compile_shaders)

target_include_directories(grebe-viewer PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/viewer
    ${CMAKE_SOURCE_DIR}/apps/common
)

target_compile_definitions(grebe-viewer PRIVATE
    SHADER_DIR="${SHADER_BINARY_DIR}/"
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

target_link_libraries(grebe-viewer PRIVATE
    grebe
    imgui_lib
    Vulkan::Vulkan
    glfw
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glm::glm
    nlohmann_json::nlohmann_json
    stb_headers
)

if(MSVC)
    target_compile_options(grebe-viewer PRIVATE /W4)
else()
    target_compile_options(grebe-viewer PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =============================================================================
# grebe-sg — Signal generator executable
# =============================================================================

add_executable(grebe-sg
    apps/sg/sg_main.cpp
    # IPC transport (shared with grebe-viewer, Phase 7.2)
    apps/common/ipc/pipe_transport.cpp
)

target_include_directories(grebe-sg PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/common
)

target_link_libraries(grebe-sg PRIVATE
    grebe
    imgui_opengl_lib
    OpenGL::GL
)

if(MSVC)
    target_compile_options(grebe-sg PRIVATE /W4)
else()
    target_compile_options(grebe-sg PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =============================================================================
# grebe-bench — Benchmark suite (stub)
# =============================================================================

add_executable(grebe-bench
    apps/bench/main.cpp
)

target_link_libraries(grebe-bench PRIVATE grebe)

# =============================================================================
# Convenience targets (Linux)
# =============================================================================

add_custom_target(run
    COMMAND $<TARGET_FILE:grebe-viewer>
    DEPENDS grebe-viewer
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running grebe-viewer"
)

add_custom_target(profile
    COMMAND $<TARGET_FILE:grebe-viewer> --profile --ring-size=64M
    DEPENDS grebe-viewer
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running profile"
)

add_custom_target(bench-run
    COMMAND $<TARGET_FILE:grebe-viewer> --bench
    DEPENDS grebe-viewer
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running microbenchmarks"
)
