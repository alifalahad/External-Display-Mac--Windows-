cmake_minimum_required(VERSION 3.20)
project(ExternalDisplayReceiver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows subsystem app (WinMain entry point)
add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    src/App/Window.cpp
    src/Renderer/D3D11Renderer.cpp
    src/Performance/FrameStats.cpp
    src/Network/StreamReceiver.cpp
    src/Decoder/H264Decoder.cpp
    src/Input/InputCapture.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Direct3D 11, DXGI, shader compiler
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    d3dcompiler
    d2d1
    dwrite
    ws2_32
    mfplat
    mfuuid
    ole32
)

# Enable UNICODE
target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# MSVC-specific settings
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive-)
endif()
