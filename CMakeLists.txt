cmake_minimum_required(VERSION 3.20)
project(cs2external CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Collect ImGui sources
file(GLOB IMGUI_SOURCES
    "${CMAKE_SOURCE_DIR}/imgui/*.cpp"
)

# Collect project sources
file(GLOB PROJECT_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

add_executable(cs2external WIN32
    ${PROJECT_SOURCES}
    ${IMGUI_SOURCES}
    "${CMAKE_SOURCE_DIR}/imgui/backends/imgui_impl_win32.cpp"
    "${CMAKE_SOURCE_DIR}/imgui/backends/imgui_impl_dx11.cpp"
)

target_include_directories(cs2external PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/imgui"
    "${CMAKE_SOURCE_DIR}/imgui/backends"
)

target_link_libraries(cs2external PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    ntdll
)

target_compile_options(cs2external PRIVATE
    -O2
    -march=x86-64
    -Wall
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(cs2external PRIVATE NDEBUG)
    set_target_properties(cs2external PROPERTIES LINK_FLAGS "-mwindows")
endif()
