cmake_minimum_required(VERSION 3.16)
project(FolderSizeViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(FolderSizeViewer WIN32 FolderSizeViewer.cpp)

target_link_libraries(FolderSizeViewer PRIVATE
    comctl32
    shlwapi
    shell32
    user32
    gdi32
    comdlg32
)

if(MSVC)
    target_compile_options(FolderSizeViewer PRIVATE /W3 /O2)
    set_target_properties(FolderSizeViewer PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS")
endif()
