cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(EditorAI VERSION 2.1.10)

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

# Workaround for SDK 5.10.1 iOS bindings bug: CCKeyboardDispatcher modify has empty address
# Codegen generates `GEODE_APPLY_MODIFY_FOR_CONSTRUCTOR(, Default, ...)` which expands to
# `static auto address = ;` -> error: expected expression on iOS.
# Fix Modify.hpp to handle empty AddressInline_ as 0, plus patch generated file.
if (IOS OR "${CMAKE_SYSTEM_NAME}" STREQUAL "iOS")
    # Configure-time: patch Modify.hpp directly (present before codegen)
    set(_modify_hpp "$ENV{GEODE_SDK}/loader/include/Geode/modify/Modify.hpp")
    if (EXISTS "${_modify_hpp}")
        file(READ "${_modify_hpp}" _mod_content)
        if (_mod_content MATCHES "static auto address = AddressInline_;")
            string(REPLACE "static auto address = AddressInline_;" "static auto address = (AddressInline_ + 0);" _mod_content "${_mod_content}")
            file(WRITE "${_modify_hpp}" "${_mod_content}")
            message(STATUS "Patched Modify.hpp for iOS empty address bug (configure-time)")
        endif()
    endif()
    # Also try patching already-generated CCKeyboardDispatcher.hpp if present
    set(_ios_patch_file "${CMAKE_CURRENT_BINARY_DIR}/bindings/bindings/Geode/modify/CCKeyboardDispatcher.hpp")
    if (EXISTS "${_ios_patch_file}")
        file(READ "${_ios_patch_file}" _ios_content)
        string(REPLACE "GEODE_APPLY_MODIFY_FOR_CONSTRUCTOR(, Default, cocos2d::CCKeyboardDispatcher, )" "// PATCHED IOS: constructor disabled" _ios_content "${_ios_content}")
        string(REPLACE "GEODE_APPLY_MODIFY_FOR_DESTRUCTOR(, Default, cocos2d::CCKeyboardDispatcher)" "// PATCHED IOS: destructor disabled" _ios_content "${_ios_content}")
        file(WRITE "${_ios_patch_file}" "${_ios_content}")
        message(STATUS "Patched CCKeyboardDispatcher.hpp for iOS (configure-time)")
    endif()
    # Build-time patch via custom target (runs every build, after Codegen)
    add_custom_target(patch-ios-bindings ALL
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-ios.cmake"
        COMMENT "Patching iOS bindings CCKeyboardDispatcher if needed"
        VERBATIM
    )
    add_dependencies(${PROJECT_NAME} patch-ios-bindings)
    # Ensure Codegen runs before patch (patch must be after Codegen generates the file)
    if (TARGET GeodeCodegen)
        add_dependencies(patch-ios-bindings GeodeCodegen)
    endif()
    if (TARGET codegen)
        add_dependencies(patch-ios-bindings codegen)
    endif()
endif()

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

    # Geode's iOS bindings currently emit an invalid CCKeyboardDispatcher
    # constructor/destructor hook header. gd-imgui-cocos already disables its
    # keyboard hook on iOS, but its source still includes that header
    # unconditionally. Patch only that include in the fetched, pinned source
    # so iOS keeps touch/IME support without compiling a desktop-only binding.
    if (IOS OR "${CMAKE_SYSTEM_NAME}" STREQUAL "iOS")
        # Resolve the fetched source through the target instead of relying on
        # CPM's package-variable spelling (the repository and CMake project
        # use different names).
        get_target_property(_editorai_imgui_sources imgui-cocos INTERFACE_SOURCES)
        foreach (_editorai_imgui_source IN LISTS _editorai_imgui_sources)
            if (_editorai_imgui_source MATCHES "/src/hooks\\.cpp$")
                set(_editorai_imgui_hooks "${_editorai_imgui_source}")
            endif()
        endforeach()
        if (EXISTS "${_editorai_imgui_hooks}")
            file(READ "${_editorai_imgui_hooks}" _editorai_imgui_hooks_text)
            string(REPLACE
                "#include <Geode/modify/CCKeyboardDispatcher.hpp>"
                "#ifndef GEODE_IS_IOS\n#include <Geode/modify/CCKeyboardDispatcher.hpp>\n#endif"
                _editorai_imgui_hooks_text
                "${_editorai_imgui_hooks_text}")
            file(WRITE "${_editorai_imgui_hooks}" "${_editorai_imgui_hooks_text}")
        endif()
    endif()

    target_link_libraries(${PROJECT_NAME} imgui-cocos)
    target_compile_definitions(${PROJECT_NAME} PRIVATE EDITORAI_HAS_IMGUI=1)
endif()

setup_geode_mod(${PROJECT_NAME})
