cmake_minimum_required(VERSION 3.20)
project(DisplayTranslationServer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(dt_server
    src/main.cpp
    src/config.cpp
    src/capture.cpp
    src/h264_encoder.cpp
    src/tcp_server.cpp
)

target_include_directories(dt_server PRIVATE src)

target_link_libraries(dt_server PRIVATE
    d3d11
    dxgi
    windowscodecs
    ws2_32
    ole32
    shlwapi
    oleaut32
    mfplat
    mfuuid
    strmiids
)

# Static-link the mingw runtime so the exe doesn't need libstdc++/libgcc DLLs alongside it
target_link_options(dt_server PRIVATE -static -static-libgcc -static-libstdc++)
