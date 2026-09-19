# plugin build file
cmake_minimum_required(VERSION 3.15)

project(NCLPIntanSource LANGUAGES CXX)

set(PLUGIN_GUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../plugin-GUI" CACHE PATH "Path to Open Ephys plugin-GUI checkout")
set(PLUGIN_HEADER_PATH "${PLUGIN_GUI_DIR}/Plugins/Headers")
set(JUCE_DIRECTORY "${PLUGIN_GUI_DIR}/JuceLibraryCode")
set(GUI_BASE_DIR "${PLUGIN_GUI_DIR}")
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(BUILD_TESTS OFF CACHE BOOL "Open Ephys GUI tests are not used by this standalone plugin" FORCE)

set(NCLP_EXPECTED_GUI_VERSION "1.1.0")

if(NOT EXISTS "${PLUGIN_GUI_DIR}/Plugins/PluginRules.cmake")
    message(FATAL_ERROR "Could not find Open Ephys PluginRules.cmake. Set -DPLUGIN_GUI_DIR=/path/to/plugin-GUI")
endif()

file(READ "${PLUGIN_GUI_DIR}/CMakeLists.txt" NCLP_PLUGIN_GUI_CMAKE)
string(REGEX MATCH "set\\([ \t]*GUI_VERSION[ \t]+([^ )]+)" _gui_version_match "${NCLP_PLUGIN_GUI_CMAKE}")
set(GUI_VERSION "${CMAKE_MATCH_1}")
if(GUI_VERSION STREQUAL "")
    message(FATAL_ERROR "Could not determine GUI_VERSION from ${PLUGIN_GUI_DIR}/CMakeLists.txt")
endif()
if(NOT GUI_VERSION STREQUAL NCLP_EXPECTED_GUI_VERSION)
    message(FATAL_ERROR
        "NCLPIntanSource targets Open Ephys GUI ${NCLP_EXPECTED_GUI_VERSION}, "
        "but ${PLUGIN_GUI_DIR} is GUI ${GUI_VERSION}")
endif()

# The official plugin-GUI v1.1.0 tag contains JuceHeader.h.in rather than the
# generated JuceHeader.h.  A standalone plugin build only needs this one
# configure step; configuring the full GUI would unnecessarily require its
# application-only dependencies (notably libcurl).
if(NOT EXISTS "${JUCE_DIRECTORY}/JuceHeader.h")
    if(NOT EXISTS "${JUCE_DIRECTORY}/JuceHeader.h.in")
        message(FATAL_ERROR "Missing ${JUCE_DIRECTORY}/JuceHeader.h.in")
    endif()

    string(REGEX MATCHALL "[0-9]+" NCLP_GUI_VERSION_PARTS "${GUI_VERSION}")
    list(GET NCLP_GUI_VERSION_PARTS 0 NCLP_GUI_VERSION_MAJOR)
    list(GET NCLP_GUI_VERSION_PARTS 1 NCLP_GUI_VERSION_MINOR)
    list(GET NCLP_GUI_VERSION_PARTS 2 NCLP_GUI_VERSION_PATCH)
    math(EXPR GUI_VERSION_HEX
        "(${NCLP_GUI_VERSION_MAJOR} << 16) + (${NCLP_GUI_VERSION_MINOR} << 8) + ${NCLP_GUI_VERSION_PATCH}"
        OUTPUT_FORMAT HEXADECIMAL)

    configure_file(
        "${JUCE_DIRECTORY}/JuceHeader.h.in"
        "${JUCE_DIRECTORY}/JuceHeader.h"
        @ONLY)
    message(STATUS "Generated Open Ephys ${GUI_VERSION} JuceHeader.h")
endif()

if(NOT EXISTS "${JUCE_DIRECTORY}/BinaryData.h")
    message(FATAL_ERROR "Missing ${JUCE_DIRECTORY}/BinaryData.h")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LINUX 1)
endif()

add_custom_target(open-ephys)

if(LINUX OR APPLE OR WIN32)
    set(DEFAULT_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins")
endif()

set(BIN_PLUGIN_DIR "${DEFAULT_PLUGIN_OUTPUT_DIR}" CACHE PATH "Directory where the plugin library is written")

include("${PLUGIN_GUI_DIR}/HelperFunctions.cmake")
include("${PLUGIN_GUI_DIR}/Plugins/PluginRules.cmake")

# Firmware and the host-side baseline analyzer share the exact FIR design and
# Q1.17 quantization implementation. Compile this C source as C++ because the
# standalone plugin project intentionally enables only the C++ language.
set(NCLP_RIPPLE_FILTER_DESIGN_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../software/main/src/drivers/ripple_filter_design.c")
set_source_files_properties(
    "${NCLP_RIPPLE_FILTER_DESIGN_SOURCE}" PROPERTIES LANGUAGE CXX)

add_sources(
    ${PLUGIN_NAME}
    Source/NCLPControlPage.cpp
    Source/NCLPControlPage.h
    Source/NCLPCommandClient.cpp
    Source/NCLPCommandClient.h
    Source/NCLPDataThread.cpp
    Source/NCLPDataThread.h
    Source/NCLPEditor.cpp
    Source/NCLPEditor.h
    Source/NCLPFrameParser.cpp
    Source/NCLPFrameParser.h
    Source/NCLPImpedanceCanvas.cpp
    Source/NCLPImpedanceCanvas.h
    Source/NCLPResultSession.cpp
    Source/NCLPRippleAnalysis.cpp
    Source/NCLPRippleAnalysis.h
    Source/NCLPStreamSession.cpp
    Source/NCLPOutputSession.cpp
    Source/NCLPRippleBaselinePanel.cpp
    Source/NCLPRippleBaselinePanel.h
    Source/NCLPSession.cpp
    Source/NCLPSession.h
    Source/NCLPTypes.h)

# add_sources() is a plugin-GUI helper that prepends this source directory, so
# add the already-absolute shared firmware source through CMake directly.
target_sources(${PLUGIN_NAME} PRIVATE "${NCLP_RIPPLE_FILTER_DESIGN_SOURCE}")

target_include_directories(${PLUGIN_NAME} PRIVATE "${PLUGIN_GUI_DIR}/Source" "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_compile_definitions(${PLUGIN_NAME} PRIVATE OEPLUGIN)

# PluginRules links these three JUCE runtime dependencies by their unversioned
# development names.  The packaged Open Ephys application can be present on a
# lean acquisition machine that has the runtime libraries but not the -dev
# symlinks.  In that case, create build-local linker aliases; no system files
# are modified and a normal development installation still takes precedence.
if(LINUX)
    function(nclp_add_runtime_link_fallback LINK_NAME VERSIONED_FILE)
        string(TOUPPER "${LINK_NAME}" NCLP_LINK_UPPER)
        set(NCLP_DEV_VAR "NCLP_${NCLP_LINK_UPPER}_DEV_LIBRARY")
        find_library(${NCLP_DEV_VAR} NAMES "${LINK_NAME}")
        if(NOT ${NCLP_DEV_VAR})
            set(NCLP_RUNTIME_VAR "NCLP_${NCLP_LINK_UPPER}_RUNTIME_LIBRARY")
            find_file(${NCLP_RUNTIME_VAR}
                NAMES "${VERSIONED_FILE}"
                PATHS /lib /usr/lib /usr/local/lib
                PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu)
            if(NOT ${NCLP_RUNTIME_VAR})
                message(FATAL_ERROR
                    "Missing ${LINK_NAME} development library and ${VERSIONED_FILE} runtime. "
                    "Install the corresponding Linux development package.")
            endif()

            set(NCLP_LINK_FALLBACK_DIR "${CMAKE_BINARY_DIR}/nclp-runtime-links")
            file(MAKE_DIRECTORY "${NCLP_LINK_FALLBACK_DIR}")
            if(NOT EXISTS "${NCLP_LINK_FALLBACK_DIR}/lib${LINK_NAME}.so")
                file(CREATE_LINK
                    "${${NCLP_RUNTIME_VAR}}"
                    "${NCLP_LINK_FALLBACK_DIR}/lib${LINK_NAME}.so"
                    SYMBOLIC)
            endif()
            target_link_directories(${PLUGIN_NAME} PRIVATE "${NCLP_LINK_FALLBACK_DIR}")
            message(STATUS
                "Using build-local ${LINK_NAME} linker alias -> ${${NCLP_RUNTIME_VAR}}")
        endif()
    endfunction()

    nclp_add_runtime_link_fallback(Xinerama libXinerama.so.1)
    nclp_add_runtime_link_fallback(asound libasound.so.2)
    nclp_add_runtime_link_fallback(freetype libfreetype.so.6)
    # Avoid embedding the workspace fallback directory in the copied plugin.
    # PluginRules supplies the required portable $ORIGIN/../shared runpath.
    set_property(TARGET ${PLUGIN_NAME} PROPERTY SKIP_BUILD_RPATH TRUE)
endif()

plugin_create_filters()

install(TARGETS ${PLUGIN_NAME}
        LIBRARY DESTINATION "${BIN_PLUGIN_DIR}"
        RUNTIME DESTINATION "${BIN_PLUGIN_DIR}")
