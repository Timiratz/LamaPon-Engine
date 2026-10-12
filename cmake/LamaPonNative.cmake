include_guard(GLOBAL)

# ネイティブPortableゲーム。既存のWindowsエディターとは別の出力ランタイム。
function(lamapon_add_native_game target)
    if(APPLE OR EMSCRIPTEN OR (NOT WIN32 AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        message(FATAL_ERROR "Native portable games target Windows, Linux and Android only")
    endif()
    cmake_parse_arguments(NG "" "ASSET_DIRECTORY;GAME_NAME;SCENE_PATH;SDL_SOURCE_DIRECTORY;SDL_LICENSE_FILE;INPUT_ACTIONS_FILE;ANDROID_PACKAGE_LIBS_DIRECTORY" "SOURCES;ASSET_INCLUDE_PATHS;PACKAGE_INCLUDE_DIRECTORIES;PACKAGE_SOURCES;PACKAGE_LIBRARIES;PACKAGE_DEFINES;PACKAGE_RUNTIME_FILES;PACKAGE_LICENSE_FILES" ${ARGN})
    if(NG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown native game arguments: ${NG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT IS_DIRECTORY "${NG_ASSET_DIRECTORY}")
        message(FATAL_ERROR "Native game requires an existing ASSET_DIRECTORY")
    endif()
    get_filename_component(engine_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(engine_license "${engine_root}/LICENSE")
    if(NOT EXISTS "${engine_license}")
        set(engine_license "${engine_root}/licenses/LamaPon.txt")
    endif()
    if(NOT EXISTS "${engine_license}")
        message(FATAL_ERROR "Native output requires the engine license from the source tree or installed SDK")
    endif()
    if(NOT TARGET SDL3::SDL3)
        if(NG_SDL_SOURCE_DIRECTORY)
            if(NOT EXISTS "${NG_SDL_SOURCE_DIRECTORY}/CMakeLists.txt")
                message(FATAL_ERROR "SDL_SOURCE_DIRECTORY must contain official SDL 3.4+ sources")
            endif()
            file(READ "${NG_SDL_SOURCE_DIRECTORY}/include/SDL3/SDL_version.h" sdl_version_header)
            string(REGEX MATCH "SDL_MAJOR_VERSION[ \t]+([0-9]+)" sdl_major_match "${sdl_version_header}")
            set(sdl_major "${CMAKE_MATCH_1}")
            string(REGEX MATCH "SDL_MINOR_VERSION[ \t]+([0-9]+)" sdl_minor_match "${sdl_version_header}")
            set(sdl_minor "${CMAKE_MATCH_1}")
            if(NOT sdl_major STREQUAL "3" OR NOT sdl_minor_match OR sdl_minor LESS 4)
                message(FATAL_ERROR "Native images require SDL 3.4 or newer SDL 3.x sources")
            endif()
            if(ANDROID)
                # SDLActivity loads libSDL3.so before libmain.so.
                set(SDL_SHARED ON CACHE BOOL "" FORCE)
                set(SDL_STATIC OFF CACHE BOOL "" FORCE)
            else()
                set(SDL_SHARED OFF CACHE BOOL "" FORCE)
                set(SDL_STATIC ON CACHE BOOL "" FORCE)
            endif()
            set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
            set(SDL_TESTS OFF CACHE BOOL "" FORCE)
            set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
            add_subdirectory("${NG_SDL_SOURCE_DIRECTORY}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
        else()
            find_package(SDL3 3.4 CONFIG REQUIRED)
        endif()
    endif()
    if(ANDROID)
        # SDLActivityがlibmain.soをロードする。
        add_library(${target} SHARED ${NG_SOURCES})
        set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
    else()
        add_executable(${target} ${NG_SOURCES})
    endif()
    target_sources(${target} PRIVATE
        "${engine_root}/src/LamaPon/Native/NativeGame.cpp"
        "${engine_root}/src/LamaPon/Native/NativeInput.cpp"
        "${engine_root}/src/LamaPon/Native/NativeIo.cpp"
        "${engine_root}/src/LamaPon/Native/NativeAudio.cpp"
        "${engine_root}/src/LamaPon/Native/NativeTextures.cpp"
        "${engine_root}/src/LamaPon/Native/NativeUi.cpp"
        "${engine_root}/src/LamaPon/Portable/PortableInputState.cpp"
        "${engine_root}/src/LamaPon/Portable/PortableRuntime.cpp"
        "${engine_root}/src/LamaPon/Portable/PortableCharacterRig2D.cpp"
        "${engine_root}/src/LamaPon/Portable/PortableLog.cpp"
        "${engine_root}/src/LamaPon/Scene/EventBus.cpp"
        "${engine_root}/src/LamaPon/Web/WebRenderer3D.cpp")
    if(NG_PACKAGE_SOURCES)
        target_sources(${target} PRIVATE ${NG_PACKAGE_SOURCES})
    endif()
    target_include_directories(${target} BEFORE PRIVATE
        "${engine_root}/src/LamaPon/Portable/include"
        "${engine_root}/src"
        "${engine_root}/third_party/nlohmann"
        "${engine_root}/third_party/cgltf"
        "${engine_root}/third_party/stb"
        "${engine_root}/third_party/imgui")
    if(NG_PACKAGE_INCLUDE_DIRECTORIES)
        target_include_directories(${target} PRIVATE ${NG_PACKAGE_INCLUDE_DIRECTORIES})
    endif()
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(NOT NG_GAME_NAME)
        set(NG_GAME_NAME "LamaPon Game")
    endif()
    if(NOT NG_SCENE_PATH)
        set(NG_SCENE_PATH "/assets/scenes/Main.scene.json")
    endif()
    foreach(value IN ITEMS NG_GAME_NAME NG_SCENE_PATH)
        string(REPLACE "\\" "\\\\" ${value} "${${value}}")
        string(REPLACE "\"" "\\\"" ${value} "${${value}}")
    endforeach()
    target_compile_definitions(${target} PRIVATE
        LAMAPON_NATIVE_RUNTIME=1 LAMAPON_WEB_AUDIO_ENABLED=1
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        "LAMAPON_PORTABLE_GAME_NAME=\"${NG_GAME_NAME}\""
        "LAMAPON_PORTABLE_SCENE_PATH=\"${NG_SCENE_PATH}\"")
    if(NG_PACKAGE_DEFINES)
        target_compile_definitions(${target} PRIVATE ${NG_PACKAGE_DEFINES})
    endif()
    target_link_libraries(${target} PRIVATE SDL3::SDL3 ${NG_PACKAGE_LIBRARIES})
    if(ANDROID)
        if(NG_PACKAGE_RUNTIME_FILES OR NG_PACKAGE_LIBRARIES)
            if(NOT NG_ANDROID_PACKAGE_LIBS_DIRECTORY)
                message(FATAL_ERROR "Android package dependencies require ANDROID_PACKAGE_LIBS_DIRECTORY")
            endif()
            set(android_package_runtime_files ${NG_PACKAGE_RUNTIME_FILES})
            foreach(package_library IN LISTS NG_PACKAGE_LIBRARIES)
                if(package_library MATCHES "\\.so(\\.[0-9]+)*$")
                    list(APPEND android_package_runtime_files "${package_library}")
                endif()
            endforeach()
            list(REMOVE_DUPLICATES android_package_runtime_files)
            foreach(package_file IN LISTS android_package_runtime_files)
                if(NOT package_file MATCHES "\\.so$")
                    message(FATAL_ERROR "Android package runtime files must be unversioned .so libraries: ${package_file}")
                endif()
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${NG_ANDROID_PACKAGE_LIBS_DIRECTORY}/${CMAKE_ANDROID_ARCH_ABI}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${package_file}"
                        "${NG_ANDROID_PACKAGE_LIBS_DIRECTORY}/${CMAKE_ANDROID_ARCH_ABI}" VERBATIM)
            endforeach()
        endif()
        target_link_libraries(${target} PRIVATE GLESv3)
        target_link_options(${target} PRIVATE
            "LINKER:-z,max-page-size=16384" "LINKER:-z,common-page-size=16384"
            "LINKER:--no-undefined")
    endif()
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc /W3)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -fexceptions)
    endif()
    if(NOT ANDROID)
        # Assets can change without recompiling the game. Run staging on every build.
        set(stage_target "${target}_assets")
        add_custom_target(${stage_target})
        add_dependencies(${target} ${stage_target})
        if(MSVC)
            set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
            set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
            set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY FALSE)
            set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
            include(InstallRequiredSystemLibraries)
            add_custom_command(TARGET ${stage_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${engine_root}/src/LamaPon/Native/WindowsRuntime.NOTICE.txt"
                    "$<TARGET_FILE_DIR:${target}>/licenses/WindowsRuntime.txt" VERBATIM)
            foreach(runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
                add_custom_command(TARGET ${stage_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime}"
                        "$<TARGET_FILE_DIR:${target}>" VERBATIM)
            endforeach()
        endif()
        if(WIN32)
            get_target_property(sdl_kind SDL3::SDL3 TYPE)
            if(sdl_kind STREQUAL "SHARED_LIBRARY")
                add_custom_command(TARGET ${stage_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>"
                        "$<TARGET_FILE_DIR:${target}>" VERBATIM)
                add_dependencies(${stage_target} SDL3::SDL3)
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # 配布フォルダーを移動しても、隣の共有SDLを検索します。
            set_target_properties(${target} PROPERTIES
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH "$ORIGIN"
                INSTALL_RPATH_USE_LINK_PATH FALSE)
            get_target_property(sdl_kind SDL3::SDL3 TYPE)
            if(sdl_kind STREQUAL "SHARED_LIBRARY")
                # ELFが要求するSONAMEで実体を配置し、外部SDKへのリンクを残しません。
                add_custom_command(TARGET ${stage_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>"
                        "$<TARGET_FILE_DIR:${target}>/$<TARGET_SONAME_FILE_NAME:SDL3::SDL3>" VERBATIM)
                add_dependencies(${stage_target} SDL3::SDL3)
            endif()
        endif()
        set(package_runtime_files ${NG_PACKAGE_RUNTIME_FILES})
        foreach(package_library IN LISTS NG_PACKAGE_LIBRARIES)
            if(package_library MATCHES "\\.dll$" OR package_library MATCHES "\\.so(\\.[0-9]+)*$")
                list(APPEND package_runtime_files "${package_library}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES package_runtime_files)
        foreach(package_file IN LISTS package_runtime_files)
            add_custom_command(TARGET ${stage_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${package_file}"
                    "$<TARGET_FILE_DIR:${target}>" VERBATIM)
        endforeach()
        add_custom_command(TARGET ${stage_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/assets"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${engine_root}/third_party/imgui/misc/fonts/ProggyClean.ttf"
                "$<TARGET_FILE_DIR:${target}>/assets/lamapon-default-font.ttf"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${engine_license}"
                "$<TARGET_FILE_DIR:${target}>/licenses/LamaPon.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${engine_root}/third_party/imgui/misc/fonts/ProggyClean.LICENSE.txt"
                "$<TARGET_FILE_DIR:${target}>/licenses/ProggyClean.txt"
            VERBATIM)
        if(NOT NG_ASSET_INCLUDE_PATHS)
            set(NG_ASSET_INCLUDE_PATHS ".")
        endif()
        file(REAL_PATH "${NG_ASSET_DIRECTORY}" asset_root)
        foreach(relative IN LISTS NG_ASSET_INCLUDE_PATHS)
            file(REAL_PATH "${NG_ASSET_DIRECTORY}/${relative}" included)
            cmake_path(IS_PREFIX asset_root "${included}" NORMALIZE stays_inside)
            if(IS_ABSOLUTE "${relative}" OR relative MATCHES "(^|[/\\])\\.\\.([/\\]|$)" OR NOT stays_inside OR NOT EXISTS "${included}")
                message(FATAL_ERROR "ASSET_INCLUDE_PATHS must name existing relative paths inside assets")
            endif()
            if(IS_DIRECTORY "${included}")
                add_custom_command(TARGET ${stage_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_directory "${included}"
                        "$<TARGET_FILE_DIR:${target}>/assets/${relative}" VERBATIM)
            else()
                get_filename_component(parent "${relative}" DIRECTORY)
                add_custom_command(TARGET ${stage_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/assets/${parent}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${included}"
                        "$<TARGET_FILE_DIR:${target}>/assets/${relative}" VERBATIM)
            endif()
        endforeach()
        if(NG_INPUT_ACTIONS_FILE)
            if(NOT EXISTS "${NG_INPUT_ACTIONS_FILE}")
                message(FATAL_ERROR "INPUT_ACTIONS_FILE does not exist")
            endif()
            add_custom_command(TARGET ${stage_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NG_INPUT_ACTIONS_FILE}"
                    "$<TARGET_FILE_DIR:${target}>/assets/lamapon-input-actions.json" VERBATIM)
        endif()
        foreach(dependency IN ITEMS nlohmann cgltf stb imgui)
            if(dependency STREQUAL "nlohmann")
                set(license_name LICENSE.MIT)
            else()
                set(license_name LICENSE.txt)
            endif()
            add_custom_command(TARGET ${stage_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${engine_root}/third_party/${dependency}/${license_name}"
                    "$<TARGET_FILE_DIR:${target}>/licenses/${dependency}.txt" VERBATIM)
        endforeach()
        if(NOT NG_SDL_LICENSE_FILE AND NG_SDL_SOURCE_DIRECTORY)
            set(NG_SDL_LICENSE_FILE "${NG_SDL_SOURCE_DIRECTORY}/LICENSE.txt")
        endif()
        if(NG_SDL_LICENSE_FILE AND EXISTS "${NG_SDL_LICENSE_FILE}")
            add_custom_command(TARGET ${stage_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NG_SDL_LICENSE_FILE}"
                    "$<TARGET_FILE_DIR:${target}>/licenses/SDL3.txt" VERBATIM)
        else()
            message(WARNING "SDL3 license is not staged; set SDL_LICENSE_FILE before distributing the game")
        endif()
        list(LENGTH NG_PACKAGE_LICENSE_FILES package_license_value_count)
        math(EXPR package_license_pair_count "${package_license_value_count} / 2")
        if(package_license_pair_count GREATER 0)
            math(EXPR package_license_last_pair "${package_license_pair_count} - 1")
            foreach(pair_index RANGE 0 ${package_license_last_pair})
                math(EXPR source_index "${pair_index} * 2")
                math(EXPR destination_index "${source_index} + 1")
                list(GET NG_PACKAGE_LICENSE_FILES ${source_index} package_license_source)
                list(GET NG_PACKAGE_LICENSE_FILES ${destination_index} package_license_destination)
                if(NOT EXISTS "${package_license_source}" OR IS_DIRECTORY "${package_license_source}")
                    message(FATAL_ERROR "Package license file is missing: ${package_license_source}")
                endif()
                get_filename_component(package_license_parent "${package_license_destination}" DIRECTORY)
                get_filename_component(package_license_name "${package_license_destination}" NAME)
                add_custom_command(TARGET ${stage_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:${target}>/licenses/${package_license_parent}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${package_license_source}"
                        "$<TARGET_FILE_DIR:${target}>/licenses/${package_license_parent}/${package_license_name}" VERBATIM)
            endforeach()
        endif()
    endif()
endfunction()
