cmake_minimum_required(VERSION 3.20)

project(vulkan-starter-app LANGUAGES C CXX)

add_executable(${PROJECT_NAME}
    source/main.cpp
    source/graphics_internal.cpp
    source/application.cpp

    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp
)

if(LINUX)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
elseif(APPLE)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /wd4201)
    target_link_options(${PROJECT_NAME} PRIVATE /subsystem:windows /entry:mainCRTStartup)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _USE_MATH_DEFINES)
else()
    message(FATAL_ERROR "${CMAKE_SYSTEM_NAME} is not supported")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD_REQUIRED TRUE
    CXX_STANDARD 20
)

find_package(Vulkan REQUIRED)

set(GLFW_LIBRARY_TYPE STATIC)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
add_subdirectory(third_party/vk-bootstrap EXCLUDE_FROM_ALL)

# Header-only local dependency: Vulkan Memory Allocator.
add_library(VulkanMemoryAllocator INTERFACE)
target_include_directories(VulkanMemoryAllocator INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/vma
)
add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)

# Header-only local dependency: GLM.
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/glm
)
add_library(glm::glm ALIAS glm)

target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glm::glm
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
)

# Compile shaders.
find_program(GLSLC_FOUND glslc)
if(GLSLC_FOUND)
    set(_SHADER_BINARIES)

    macro(compile_shader SHADER_FILE)
        set(SHADER_SOURCE ${CMAKE_SOURCE_DIR}/shaders/${SHADER_FILE})
        set(SHADER_BINARY ${SHADER_FILE}.spv)
        set(SHADER_BINARY_PATH ${CMAKE_SOURCE_DIR}/shaders/${SHADER_BINARY})

        add_custom_command(
            OUTPUT ${SHADER_BINARY_PATH}
            COMMAND glslc ${SHADER_SOURCE} -o ${SHADER_BINARY_PATH}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "Compiling ${SHADER_FILE} shader"
        )

        list(APPEND _SHADER_BINARIES ${SHADER_BINARY_PATH})
    endmacro()

    compile_shader(cube.vert)
    compile_shader(cube.frag)

    add_custom_target(shaders DEPENDS ${_SHADER_BINARIES})
    add_dependencies(${PROJECT_NAME} shaders)
else()
    message(FATAL_ERROR "glslc was not found. Make sure Vulkan SDK is installed and glslc is in PATH.")
endif()
