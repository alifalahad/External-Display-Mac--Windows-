cmake_minimum_required(VERSION 3.20)
project(ExternalDisplayReceiver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release build if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Windows subsystem app (WinMain entry point)
add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    src/App/Window.cpp
    src/App/CrashHandler.cpp
    src/App/Log.cpp
    src/App/Options.cpp
    src/App/StatusText.cpp
    src/App/MacFinder.cpp
    src/App/ReceiverApp.cpp
    src/Renderer/D3D11Renderer.cpp
    src/Performance/ClockSync.cpp
    src/Performance/PipelineMetrics.cpp
    src/Performance/SystemMonitor.cpp
    src/Network/StreamReceiver.cpp
    src/Decoder/H264Decoder.cpp
    src/Input/InputCapture.cpp
    src/Input/Pointer.cpp
    src/Input/PointerCursor.cpp
    src/Clipboard/ClipboardSync.cpp
    src/Discovery/ServiceDiscovery.cpp
    src/Security/PairingManager.cpp
    src/Security/Crypto.cpp
    src/Security/SecureChannel.cpp
    src/UI/PinPrompt.cpp
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
    bcrypt
    crypt32
    dbghelp
    pdh
    psapi
    user32
    gdi32
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
    # Release builds also get a .pdb so crash reports show function names + lines
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")
endif()

# Tests (Phase 20):
#   cmake -B build -DEXDP_BUILD_TESTS=ON && cmake --build build
#   .\build\PipelineMetricsTests.exe   metrics, clock sync, benchmark files, payload layout
#   .\build\AppLogicTests.exe          options, status texts, Mac choice, logging, replay window
#   .\build\SecureChannelTests.exe     protocol v2 crypto against the Mac's vectors (Windows only)
#   .\build\PointerTests.exe           pointer drawn here: wire layout, scaling, rules, Windows cursor
#   ctest --test-dir build               all of them
option(EXDP_BUILD_TESTS "Build unit tests" OFF)
if(EXDP_BUILD_TESTS)
    enable_testing()
    add_executable(PipelineMetricsTests
        tests/PipelineMetricsTests.cpp
        src/Performance/ClockSync.cpp
        src/Performance/PipelineMetrics.cpp
    )
    add_executable(AppLogicTests
        tests/AppLogicTests.cpp
        src/App/Options.cpp
        src/App/StatusText.cpp
        src/App/Log.cpp
    )
    add_executable(SecureChannelTests
        tests/SecureChannelTests.cpp
        src/Security/Crypto.cpp
        src/Security/SecureChannel.cpp
    )
    target_link_libraries(SecureChannelTests PRIVATE bcrypt crypt32)
    add_executable(PointerTests
        tests/PointerTests.cpp
        src/Input/Pointer.cpp
        src/Input/PointerCursor.cpp
    )
    target_link_libraries(PointerTests PRIVATE user32 gdi32)
    foreach(test PipelineMetricsTests AppLogicTests SecureChannelTests PointerTests)
        target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(${test} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(${test} PRIVATE /W4 /permissive-)
        endif()
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
endif()
