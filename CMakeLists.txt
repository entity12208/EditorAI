cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(EditorAI VERSION 3.0.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

if (ANDROID)
    # NDK r28's libc++ (required: Geode 5.7+ needs clang >= 19) exposes a
    # missing <type_traits> include in the bundled nontype_functional
    # dependency. Force-include it for every TU; harmless otherwise.
    add_compile_options(-include type_traits)
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# ImGui overlay — the mod's primary UI (E key on desktop, floating bubble
# on mobile). Platforms without it fall back to the classic cocos popups
# (src/overlay.cpp compiles to nothing; main.cpp re-adds the AI button).
if (WIN32 OR APPLE OR ANDROID)
    # IMGUI_VERSION is read by gd-imgui-cocos's own CMakeLists (its CPM
    # fetch of dear imgui) — not dead, despite no reference in this file.
    # Main-HEAD backend needs >= 1.89.5 (AddMouseSourceEvent).
    set(IMGUI_VERSION "v1.92.4")
    # Pinned to main HEAD: the 1.0.0 tag (Dec 2023) predates Geode 5.x APIs.
    CPMAddPackage("gh:matcool/gd-imgui-cocos#b93f08ccef778a53ebba09b20c347f6a63980119")
    target_link_libraries(${PROJECT_NAME} imgui-cocos)
    target_compile_definitions(${PROJECT_NAME} PRIVATE EDITORAI_HAS_IMGUI=1)
endif()

setup_geode_mod(${PROJECT_NAME})
