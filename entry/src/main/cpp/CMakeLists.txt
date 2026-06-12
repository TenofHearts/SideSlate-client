cmake_minimum_required(VERSION 3.5.0)
project(Tablet2ScreenNative)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

add_library(h265receiver SHARED
    napi_init.cpp
    h265_receiver.cpp
)

target_include_directories(h265receiver PRIVATE
    ${NATIVERENDER_ROOT_PATH}
)

target_link_libraries(h265receiver PRIVATE
    libace_ndk.z.so
    libace_napi.z.so
    libhilog_ndk.z.so
    libnative_media_core.so
    libnative_media_codecbase.so
    libnative_media_vdec.so
    libnative_window.so
)
