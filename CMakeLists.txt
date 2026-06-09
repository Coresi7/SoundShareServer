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
)

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
        target_compile_options(${target} PRIVATE /W3 /utf-8)
    endforeach()
endif()
