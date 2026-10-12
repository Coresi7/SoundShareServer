cmake_minimum_required(VERSION 3.20)
project(SoundShareServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies (installed via vcpkg)
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)

add_executable(SoundShareServer
    SoundShareServer/main.cpp
    SoundShareServer/wasapi_capture.cpp
    SoundShareServer/http_server.cpp
    SoundShareServer/diagnostics.cpp
)

# ---- A33 (M-7): 诊断与故障注入双开关（互相独立）----
# 读（/_debug/sessions + stdin dump + DumpProbeJson）：默认 ON
option(SOUNDSHARE_ENABLE_DIAGNOSTICS "Enable read-only /_debug/sessions probe endpoint" ON)
# 写（/_debug/fault/set + session 侧注入检查点）：默认 OFF；关闭时端点返回 404，
# 注入检查点编译剔除 → 正常路径零开销。验收 AC-09/10/11 时由 tester 显式
# 以 -DSOUNDSHARE_ENABLE_FAULT_INJECTION=ON 构建。
option(SOUNDSHARE_ENABLE_FAULT_INJECTION "Enable /_debug/fault/set fault-injection write endpoint" OFF)

if(SOUNDSHARE_ENABLE_DIAGNOSTICS)
    target_compile_definitions(SoundShareServer PRIVATE SOUNDSHARE_ENABLE_DIAGNOSTICS)
endif()
if(SOUNDSHARE_ENABLE_FAULT_INJECTION)
    target_compile_definitions(SoundShareServer PRIVATE SOUNDSHARE_ENABLE_FAULT_INJECTION)
endif()

target_include_directories(SoundShareServer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/SoundShareServer
)

target_link_libraries(SoundShareServer PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    ole32
    avrt
)

add_executable(SoundShareClient
    SoundShareClient/main.cpp
    SoundShareClient/ws_client.cpp
    SoundShareClient/wasapi_render.cpp
)

target_include_directories(SoundShareClient PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/SoundShareClient
)

target_link_libraries(SoundShareClient PRIVATE
    Boost::system
    ole32
    avrt
)

# Windows-specific settings
if(MSVC)
    foreach(target SoundShareServer SoundShareClient)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
        target_compile_options(${target} PRIVATE /W3 /utf-8 /bigobj)
    endforeach()
endif()
