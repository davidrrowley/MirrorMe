cmake_minimum_required(VERSION 3.20)

project(MirrorMe LANGUAGES CXX)

add_executable(MirrorMe WIN32
    src/main.cpp
    src/app.rc
)

target_compile_features(MirrorMe PRIVATE cxx_std_20)
target_compile_definitions(MirrorMe PRIVATE UNICODE _UNICODE)
target_link_libraries(MirrorMe PRIVATE user32 gdi32 d3d11 dxgi windowsapp dwmapi shell32)

if(MSVC)
    target_compile_options(MirrorMe PRIVATE /W4 /permissive-)
endif()
