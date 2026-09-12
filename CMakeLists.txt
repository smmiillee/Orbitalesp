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
)

# MSVC flags (what GitHub Actions windows-latest uses)
if(MSVC)
    target_compile_options(cs2external PRIVATE
        /O2
        /W3
        /EHsc
        /MP         # parallel compilation
    )
    target_compile_definitions(cs2external PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    # Fallback for local MinGW/Clang builds
    target_compile_options(cs2external PRIVATE -O2 -march=x86-64 -Wall)
endif()
