include_guard(GLOBAL)

function(lamapon_add_web_game target)
    set(options SINGLE_FILE PORTABLE_GAME)
    set(oneValueArgs SHELL_FILE ASSET_DIRECTORY OUTPUT_NAME GAME_NAME SCENE_PATH)
    set(multiValueArgs SOURCES MODULES)
    cmake_parse_arguments(TWG
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN}
    )

    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR
            "lamapon_add_web_game requires the Emscripten CMake toolchain")
    endif()
    if(NOT TWG_SOURCES)
        message(FATAL_ERROR
            "lamapon_add_web_game(${target}) requires SOURCES")
    endif()

    if(NOT TWG_MODULES)
        set(TWG_MODULES core input)
    endif()
    list(APPEND TWG_MODULES core input)
    list(REMOVE_DUPLICATES TWG_MODULES)

    if(DEFINED LAMAPON_WEB_REQUESTED_MODULES)
        foreach(requested_module IN LISTS LAMAPON_WEB_REQUESTED_MODULES)
            if(NOT requested_module IN_LIST TWG_MODULES)
                message(FATAL_ERROR
                    "Web target ${target} does not link requested module: "
                    "${requested_module}")
            endif()
        endforeach()
        foreach(linked_module IN LISTS TWG_MODULES)
            if(NOT linked_module IN_LIST LAMAPON_WEB_REQUESTED_MODULES)
                message(FATAL_ERROR
                    "Web target ${target} links undeclared module: "
                    "${linked_module}")
            endif()
        endforeach()
    endif()

    get_filename_component(LAMAPON_WEB_ROOT
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.."
        ABSOLUTE
    )

    set(runtime_sources
        "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebApplication.cpp"
        "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebInput.cpp"
    )
    # Portable 2Dと3Dは、キャンバスとコンテキストを管理する小さな実装を
    # 共有します。2D専用ターゲットはModel/Meshコンポーネントを公開も使用も
    # しませんが、DOMのスプライトとテキストを合成する前にフレームの消去と
    # 表示処理が必要です。
    if("renderer3d" IN_LIST TWG_MODULES
       OR (TWG_PORTABLE_GAME AND "renderer2d" IN_LIST TWG_MODULES))
        list(APPEND runtime_sources
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebRenderer3D.cpp")
    endif()
    if("physics3d" IN_LIST TWG_MODULES)
        list(APPEND runtime_sources
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebPhysics3D.cpp")
    endif()
    if("audio" IN_LIST TWG_MODULES)
        list(APPEND runtime_sources
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebAudioRuntime.cpp")
    endif()
    if(TWG_PORTABLE_GAME)
        list(APPEND runtime_sources
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Portable/PortableRuntime.cpp"
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Portable/PortableLog.cpp"
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Portable/PortableWebGame.cpp"
        )
    endif()

    add_executable(${target}
        ${TWG_SOURCES}
        ${runtime_sources}
    )
    target_include_directories(${target} PRIVATE
        "${LAMAPON_WEB_ROOT}/src"
    )
    if(TWG_PORTABLE_GAME)
        target_include_directories(${target} BEFORE PRIVATE
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Portable/include"
        )
        target_include_directories(${target} PRIVATE
            "${LAMAPON_WEB_ROOT}/third_party/nlohmann"
            "${LAMAPON_WEB_ROOT}/third_party/cgltf"
        )
        if(NOT TWG_GAME_NAME)
            set(TWG_GAME_NAME "LamaPon Portable Game")
        endif()
        if(NOT TWG_SCENE_PATH)
            set(TWG_SCENE_PATH "/assets/scenes/Main.scene.json")
        endif()
        target_compile_definitions(${target} PRIVATE
            LAMAPON_PORTABLE_GAME_NAME="${TWG_GAME_NAME}"
            LAMAPON_PORTABLE_SCENE_PATH="${TWG_SCENE_PATH}"
        )
    endif()
    if("particles3d" IN_LIST TWG_MODULES AND NOT TWG_PORTABLE_GAME)
        message(FATAL_ERROR
            "The Web particles3d module currently requires PORTABLE_GAME")
    endif()
    target_compile_features(${target} PRIVATE cxx_std_20)
    if("audio" IN_LIST TWG_MODULES)
        target_compile_definitions(${target} PRIVATE
            LAMAPON_WEB_AUDIO_ENABLED=1
        )
    else()
        target_compile_definitions(${target} PRIVATE
            LAMAPON_WEB_AUDIO_ENABLED=0
        )
    endif()
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        # シーン／スクリプトにある既存の例外処理を有効にし、回復可能な例外で
        # ブラウザーランタイム全体が強制終了しないようにします。
        -fexceptions
    )

    target_link_options(${target} PRIVATE
        -fexceptions
        "-sALLOW_MEMORY_GROWTH=1"
        "-sNO_EXIT_RUNTIME=1"
        "-sASSERTIONS=1"
    )
    if("renderer2d" IN_LIST TWG_MODULES
       OR "renderer3d" IN_LIST TWG_MODULES)
        target_link_options(${target} PRIVATE
            "-sMIN_WEBGL_VERSION=1"
            "-sMAX_WEBGL_VERSION=2"
        )
    endif()
    if(TWG_SINGLE_FILE)
        target_link_options(${target} PRIVATE
            "-sSINGLE_FILE=1"
            # Base64にすると、ローカルWebサーバーを使わずfile://から直接
            # 開いた場合も、自己完結型のHTML/Wasmパッケージとして動作します。
            "-sSINGLE_FILE_BINARY_ENCODE=0"
        )
    endif()
    if(TWG_SHELL_FILE)
        get_filename_component(shell_file "${TWG_SHELL_FILE}" ABSOLUTE)
        target_link_options(${target} PRIVATE
            "--shell-file=${shell_file}"
        )
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${shell_file}")
    endif()
    if(TWG_ASSET_DIRECTORY)
        get_filename_component(asset_directory
            "${TWG_ASSET_DIRECTORY}"
            ABSOLUTE
        )
        if(NOT IS_DIRECTORY "${asset_directory}")
            message(FATAL_ERROR
                "Web asset directory was not found: ${asset_directory}")
        endif()
        target_link_options(${target} PRIVATE
            "--embed-file=${asset_directory}@/assets"
        )
        # Emscriptenはリンク時にディレクトリを埋め込みます。CMakeは
        # --embed-fileから個々のファイルを推測しないため、ステージングした
        # 全アセットを明示的にリンク依存へ加えます。C++に変更がなくても、
        # PNGからWebPへの再変換後は配布用HTMLを必ず作り直します。
        file(GLOB_RECURSE lamapon_web_asset_files
            CONFIGURE_DEPENDS
            LIST_DIRECTORIES false
            "${asset_directory}/*"
        )
        if(lamapon_web_asset_files)
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
                ${lamapon_web_asset_files}
            )
        endif()
    endif()

    if(DEFINED LAMAPON_WEB_OUTPUT_NAME AND NOT LAMAPON_WEB_OUTPUT_NAME STREQUAL "")
        set(output_name "${LAMAPON_WEB_OUTPUT_NAME}")
    elseif(TWG_OUTPUT_NAME)
        set(output_name "${TWG_OUTPUT_NAME}")
    else()
        set(output_name "${target}")
    endif()
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        SUFFIX ".html"
        LAMAPON_WEB_MODULES "${TWG_MODULES}"
    )
endfunction()
