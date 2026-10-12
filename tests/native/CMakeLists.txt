cmake_minimum_required(VERSION 3.25)
project(LamaPonNativeTests LANGUAGES C CXX)
get_filename_component(engine_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LAMAPON_SDL_SOURCE_DIRECTORY "" CACHE PATH "Existing official SDL 3.4+ source directory")
set(LAMAPON_SDL_LICENSE_FILE "" CACHE FILEPATH "License for an existing SDL package")
include("${engine_root}/cmake/LamaPonNative.cmake")
option(LAMAPON_NATIVE_EXPORT_ONLY "Build the public Portable game without desktop integration probes" OFF)
if(LAMAPON_NATIVE_EXPORT_ONLY)
    lamapon_add_native_game(NativeExportSmoke
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ExportSmoke.cpp"
        ASSET_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        INPUT_ACTIONS_FILE "${engine_root}/tests/web/assets/lamapon-input-actions.json"
        GAME_NAME "LamaPon Native Export Smoke"
        SDL_SOURCE_DIRECTORY "${LAMAPON_SDL_SOURCE_DIRECTORY}"
        SDL_LICENSE_FILE "${LAMAPON_SDL_LICENSE_FILE}")
    return()
endif()
lamapon_add_native_game(NativeSmoke
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/NativeSmoke.cpp"
    ASSET_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    GAME_NAME "LamaPon Native Smoke"
    SCENE_PATH "/assets/scenes/起動.scene.json"
    INPUT_ACTIONS_FILE "${engine_root}/tests/web/assets/lamapon-input-actions.json"
    SDL_SOURCE_DIRECTORY "${LAMAPON_SDL_SOURCE_DIRECTORY}"
    SDL_LICENSE_FILE "${LAMAPON_SDL_LICENSE_FILE}")
target_compile_definitions(NativeSmoke PRIVATE LAMAPON_NATIVE_FRAME_PROBE=1)
add_executable(NativeSaveTests NativeSaveTests.cpp "${engine_root}/src/LamaPon/Native/NativeIo.cpp")
add_executable(NativeAudioTests NativeAudioTests.cpp
    "${engine_root}/src/LamaPon/Native/NativeAudio.cpp"
    "${engine_root}/src/LamaPon/Native/NativeIo.cpp")
target_include_directories(NativeAudioTests PRIVATE "${engine_root}/src"
    "${engine_root}/third_party/nlohmann" "${engine_root}/third_party/stb")
target_link_libraries(NativeAudioTests PRIVATE SDL3::SDL3)
target_compile_features(NativeAudioTests PRIVATE cxx_std_20)
target_compile_definitions(NativeAudioTests PRIVATE LAMAPON_NATIVE_AUDIO_PROBE=1
    $<$<PLATFORM_ID:Windows>:NOMINMAX>)
if(MSVC)
    target_compile_options(NativeAudioTests PRIVATE /utf-8 /EHsc /W3)
endif()
target_include_directories(NativeSaveTests PRIVATE "${engine_root}/src" "${engine_root}/third_party/nlohmann")
target_link_libraries(NativeSaveTests PRIVATE SDL3::SDL3)
target_compile_features(NativeSaveTests PRIVATE cxx_std_20)
target_compile_definitions(NativeSaveTests PRIVATE $<$<PLATFORM_ID:Windows>:NOMINMAX>)
if(MSVC)
    target_compile_options(NativeSaveTests PRIVATE /utf-8 /EHsc /W3)
endif()
add_custom_command(TARGET NativeSmoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NativeSmoke>/assets/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${engine_root}/tests/fixtures/models/embedded-data.gltf"
        "${engine_root}/tests/fixtures/models/embedded-buffer.glb"
        "$<TARGET_FILE_DIR:NativeSmoke>/assets/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes/Main.scene.json"
        "$<TARGET_FILE_DIR:NativeSmoke>/assets/scenes/起動.scene.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${engine_root}/tests/web/assets/scenes/MalformedType.scene.json"
        "$<TARGET_FILE_DIR:NativeSmoke>/assets/scenes/MalformedType.scene.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${engine_root}/tests/web/assets/scenes/MalformedEnvironment.scene.json"
        "$<TARGET_FILE_DIR:NativeSmoke>/assets/scenes/MalformedEnvironment.scene.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${engine_root}/tests/web/assets/scenes/UIButtonTarget.scene.json"
        "$<TARGET_FILE_DIR:NativeSmoke>/assets/scenes/UIButtonTarget.scene.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${engine_root}/tests/web/assets/scenes/UIButtonFinal.scene.json"
        "$<TARGET_FILE_DIR:NativeSmoke>/assets/scenes/UIButtonFinal.scene.json" VERBATIM)
set_property(TARGET NativeSmoke APPEND PROPERTY LINK_DEPENDS
    "${engine_root}/tests/fixtures/models/embedded-data.gltf"
    "${engine_root}/tests/fixtures/models/embedded-buffer.glb"
    "${engine_root}/tests/web/assets/scenes/MalformedType.scene.json"
    "${engine_root}/tests/web/assets/scenes/MalformedEnvironment.scene.json"
    "${engine_root}/tests/web/assets/scenes/UIButtonTarget.scene.json"
    "${engine_root}/tests/web/assets/scenes/UIButtonFinal.scene.json")

option(LAMAPON_CHECK_EDITOR_EXPORT "Compile Windows editor export code and process tests without the full editor" OFF)
if(LAMAPON_CHECK_EDITOR_EXPORT)
    if(NOT WIN32)
        message(FATAL_ERROR "The editor export process runs on Windows")
    endif()
    add_library(EditorExportCompileCheck OBJECT
        "${engine_root}/src/LamaPon/Editor/GameExportDialog.cpp")
    target_include_directories(EditorExportCompileCheck PRIVATE
        "${engine_root}/src" "${engine_root}/third_party/imgui"
        "${engine_root}/third_party/nlohmann"
        "${engine_root}/third_party/DirectXTK/include")
    add_executable(NativeExportJobTests
        "${engine_root}/tests/NativeExportJobTests.cpp"
        "${engine_root}/src/LamaPon/Editor/WebExportJob.cpp")
    target_include_directories(NativeExportJobTests PRIVATE
        "${engine_root}/src" "${engine_root}/third_party/nlohmann")
    foreach(target EditorExportCompileCheck NativeExportJobTests)
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_compile_definitions(${target} PRIVATE NOMINMAX)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8 /EHsc /W4)
        endif()
    endforeach()
endif()
