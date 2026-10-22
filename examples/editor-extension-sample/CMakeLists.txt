cmake_minimum_required(VERSION 3.25)

project(LamaPonEditorExtensionSample LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "LamaPon Editor extensions currently require MSVC on Windows.")
endif()

set(LAMAPON_EDITOR_EXTENSION_INCLUDE_DIR "" CACHE PATH
    "Directory containing LamaPon/Editor/EditorExtensionApi.h")
if(NOT EXISTS "${LAMAPON_EDITOR_EXTENSION_INCLUDE_DIR}/LamaPon/Editor/EditorExtensionApi.h")
    message(FATAL_ERROR
        "Set LAMAPON_EDITOR_EXTENSION_INCLUDE_DIR to the LamaPon SDK include or src directory.")
endif()

add_library(SampleEditorExtension SHARED SampleEditorExtension.cpp)
target_include_directories(SampleEditorExtension PRIVATE
    "${LAMAPON_EDITOR_EXTENSION_INCLUDE_DIR}")
target_compile_features(SampleEditorExtension PRIVATE cxx_std_20)
target_compile_options(SampleEditorExtension PRIVATE /W4 /permissive-)
set_target_properties(SampleEditorExtension PROPERTIES
    OUTPUT_NAME "SampleEditorExtension"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/runtime")

foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" configurationUpper)
    set_target_properties(SampleEditorExtension PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${configurationUpper}"
        "${CMAKE_BINARY_DIR}/package/runtime")
endforeach()
