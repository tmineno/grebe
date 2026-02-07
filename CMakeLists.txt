cmake_minimum_required(VERSION 3.24)
project(grebe VERSION 0.1.0 LANGUAGES CXX C)

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

# Dear ImGui library
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
# Common library (shared between grebe and future grebe-sg)
# =============================================================================

add_library(grebe_common STATIC
    src/app_command.cpp
    src/cli.cpp
    src/benchmark.cpp
    src/decimator.cpp
    src/data_generator.cpp
    src/decimation_thread.cpp
    src/profiler.cpp
    src/process_handle.cpp
)

target_include_directories(grebe_common PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(grebe_common PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(grebe_common PRIVATE /W4)
else()
    target_compile_options(grebe_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =============================================================================
# Main executable
# =============================================================================

add_executable(grebe
    src/main.cpp
    src/app_loop.cpp
    src/vulkan_context.cpp
    src/swapchain.cpp
    src/renderer.cpp
    src/buffer_manager.cpp
    src/hud.cpp
    src/microbench.cpp
    src/compute_decimator.cpp
    src/vma_impl.cpp
)

add_dependencies(grebe compile_shaders)

target_link_libraries(grebe PRIVATE
    grebe_common
    Vulkan::Vulkan
    glfw
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glm::glm
    stb_headers
    imgui_lib
)

target_compile_definitions(grebe PRIVATE
    SHADER_DIR="${SHADER_BINARY_DIR}"
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

if(MSVC)
    target_compile_options(grebe PRIVATE /W4)
else()
    target_compile_options(grebe PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =============================================================================
# Convenience targets (Linux)
# =============================================================================

add_custom_target(run
    COMMAND $<TARGET_FILE:grebe>
    DEPENDS grebe
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running grebe"
)

add_custom_target(profile
    COMMAND $<TARGET_FILE:grebe> --profile --ring-size=64M
    DEPENDS grebe
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running profile"
)

add_custom_target(bench
    COMMAND $<TARGET_FILE:grebe> --bench
    DEPENDS grebe
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running microbenchmarks"
)
