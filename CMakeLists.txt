cmake_minimum_required(VERSION 3.16)
project(LunarAutoClicker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(LunarAutoClicker WIN32 src/main.cpp src/clicker.cpp)
target_compile_definitions(LunarAutoClicker PRIVATE UNICODE _UNICODE)
target_link_libraries(LunarAutoClicker PRIVATE d2d1 dwrite dwmapi winmm shell32 ole32)

if(MSVC)
    target_compile_options(LunarAutoClicker PRIVATE /W4 /utf-8)
    set_property(TARGET LunarAutoClicker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
