cmake_minimum_required(VERSION 3.16)
project(LunarAutoClicker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(LunarAutoClicker src/main.cpp)
target_compile_definitions(LunarAutoClicker PRIVATE UNICODE _UNICODE)
target_link_libraries(LunarAutoClicker PRIVATE winmm)

if(MSVC)
    target_compile_options(LunarAutoClicker PRIVATE /W4 /utf-8)
    set_property(TARGET LunarAutoClicker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
