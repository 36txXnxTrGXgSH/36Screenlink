cmake_minimum_required(VERSION 3.25)
project(Screenlink LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Runtime statique : l'exe reste portable, sans redistribuable VC++ sur le PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(ScreenlinkAgent
    agent/src/main.cpp
    agent/src/audio.cpp
    agent/src/capture.cpp
    agent/src/channel.cpp
    agent/src/crypto.cpp
    agent/src/desktop.cpp
    agent/src/device_helper.cpp
    agent/src/discovery.cpp
    agent/src/encoder_mf.cpp
    agent/src/encoder_nvenc.cpp
    agent/src/input.cpp
    agent/src/monitor.cpp
    agent/src/store.cpp
    agent/src/tray.cpp
    agent/src/window_follow.cpp
    agent/res/app.rc
)

set_target_properties(ScreenlinkAgent PROPERTIES OUTPUT_NAME "36Screenlink")
target_include_directories(ScreenlinkAgent PRIVATE third_party/nvenc)
target_compile_definitions(ScreenlinkAgent PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ScreenlinkAgent PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ScreenlinkAgent PRIVATE
    d3d11 dxgi ws2_32 bcrypt crypt32 dnsapi user32 shell32 shcore wtsapi32 ole32 advapi32 oleaut32 mf mfplat mfuuid strmiids dwmapi)

add_executable(ScreenlinkService service/36ScreenlinkService.cpp)
set_target_properties(ScreenlinkService PROPERTIES OUTPUT_NAME "36ScreenlinkService")
target_compile_definitions(ScreenlinkService PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ScreenlinkService PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ScreenlinkService PRIVATE advapi32 wtsapi32)
