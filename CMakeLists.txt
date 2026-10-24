cmake_minimum_required(VERSION 3.28)
project(Bxsim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put the finished .exe in build/bin so it's easy to find.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Static everything on Windows: one .exe you can hand to anyone, no DLLs.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- SFML (downloaded and built automatically the first time you configure) ----
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SFML_USE_STATIC_STD_LIBS ON CACHE BOOL "" FORCE)
set(SFML_BUILD_NETWORK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SFML
    GIT_REPOSITORY https://github.com/SFML/SFML.git
    GIT_TAG 3.1.0
    GIT_SHALLOW ON
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(SFML)

# ---- The app ----
add_executable(Bxsim
    src/main.cpp
    src/Spectrum.cpp
    src/Renderer.cpp
    src/Platform.cpp
)
target_include_directories(Bxsim PRIVATE src)
target_link_libraries(Bxsim PRIVATE SFML::Graphics SFML::Audio)

if(WIN32)
    # Release builds open without a console window; Debug keeps it for log output.
    set_target_properties(Bxsim PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
    target_link_libraries(Bxsim PRIVATE SFML::Main comdlg32 shell32)
    target_compile_definitions(Bxsim PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

if(MSVC)
    target_compile_options(Bxsim PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(Bxsim PRIVATE -Wall -Wextra)
endif()
