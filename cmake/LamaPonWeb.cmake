include_guard(GLOBAL)

# lamapon_add_web_game(target: target name): Emscripten game targetを設定します。
function(lamapon_add_web_game target)
    # options: Boolean flags accepted by this function.
    set(options SINGLE_FILE PORTABLE_GAME)
    # oneValueArgs: options that take one value.
    set(oneValueArgs SHELL_FILE ASSET_DIRECTORY OUTPUT_NAME GAME_NAME SCENE_PATH)
    # multiValueArgs: options that take multiple values.
    set(multiValueArgs SOURCES MODULES)
    # cmake_parse_arguments stores parsed values in TWG_* variables.
    cmake_parse_arguments(TWG
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN}
    )

    # Emscripten以外のtoolchainを拒否します。
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR
            "lamapon_add_web_game requires the Emscripten CMake toolchain")
    endif()
    # Web targetにはsourceが必要です。
    if(NOT TWG_SOURCES)
        message(FATAL_ERROR
            "lamapon_add_web_game(${target}) requires SOURCES")
    endif()

    # TWG_MODULESが空なら必須runtime moduleを設定します。
    if(NOT TWG_MODULES)
        set(TWG_MODULES core input)
    endif()
    list(APPEND TWG_MODULES core input)
    list(REMOVE_DUPLICATES TWG_MODULES)

    # request一覧がある場合はlink内容と照合します。
    if(DEFINED LAMAPON_WEB_REQUESTED_MODULES)
        # requested_module: request側の現在module名。
        foreach(requested_module IN LISTS LAMAPON_WEB_REQUESTED_MODULES)
            # requestされた未link moduleを拒否します。
            if(NOT requested_module IN_LIST TWG_MODULES)
                message(FATAL_ERROR
                    "Web target ${target} does not link requested module: "
                    "${requested_module}")
            endif()
        endforeach()
        # linked_module: 検査中のtarget module。
        foreach(linked_module IN LISTS TWG_MODULES)
            # 宣言されていないlink moduleを拒否します。
            if(NOT linked_module IN_LIST LAMAPON_WEB_REQUESTED_MODULES)
                message(FATAL_ERROR
                    "Web target ${target} links undeclared module: "
                    "${linked_module}")
            endif()
        endforeach()
    endif()

    # LAMAPON_WEB_ROOT: Portable runtime repository root.
    get_filename_component(LAMAPON_WEB_ROOT
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.."
        ABSOLUTE
    )

    # runtime_sources: 選択moduleに必要なruntime source一覧.
    set(runtime_sources
        "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebApplication.cpp"
        "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebInput.cpp"
    )
    # Portable 2Dもframe消去とDOM合成にrenderer共通部を使います。
    if("renderer3d" IN_LIST TWG_MODULES
       OR (TWG_PORTABLE_GAME AND "renderer2d" IN_LIST TWG_MODULES))
        list(APPEND runtime_sources
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebRenderer3D.cpp")
    endif()
    # 要求時に3D physics runtimeを加えます。
    if("physics3d" IN_LIST TWG_MODULES)
        list(APPEND runtime_sources
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebPhysics3D.cpp")
    endif()
    # 要求時にaudio runtimeを加えます。
    if("audio" IN_LIST TWG_MODULES)
        list(APPEND runtime_sources
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Web/WebAudioRuntime.cpp")
    endif()
    # Portable gameに組み込みruntimeを加えます。
    if(TWG_PORTABLE_GAME)
        list(APPEND runtime_sources
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Portable/PortableRuntime.cpp"
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Portable/PortableCharacterRig2D.cpp"
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
    # Portable game用headerとgame情報を設定します。
    if(TWG_PORTABLE_GAME)
        target_include_directories(${target} BEFORE PRIVATE
            "${LAMAPON_WEB_ROOT}/src/LamaPon/Portable/include"
        )
        target_include_directories(${target} PRIVATE
            "${LAMAPON_WEB_ROOT}/third_party/nlohmann"
            "${LAMAPON_WEB_ROOT}/third_party/cgltf"
        )
        # TWG_GAME_NAMEが空なら既定のgame名を設定します。
        if(NOT TWG_GAME_NAME)
            set(TWG_GAME_NAME "LamaPon Portable Game")
        endif()
        # TWG_SCENE_PATHが空なら既定sceneを設定します。
        if(NOT TWG_SCENE_PATH)
            set(TWG_SCENE_PATH "/assets/scenes/Main.scene.json")
        endif()
        target_compile_definitions(${target} PRIVATE
            LAMAPON_PORTABLE_GAME_NAME="${TWG_GAME_NAME}"
            LAMAPON_PORTABLE_SCENE_PATH="${TWG_SCENE_PATH}"
        )
    endif()
    # particles3dはPortable game layerを必須とします。
    if("particles3d" IN_LIST TWG_MODULES AND NOT TWG_PORTABLE_GAME)
        message(FATAL_ERROR
            "The Web particles3d module currently requires PORTABLE_GAME")
    endif()
    target_compile_features(${target} PRIVATE cxx_std_20)
    # audio moduleの有無でcompile設定を切り替えます。
    if("audio" IN_LIST TWG_MODULES)
        target_compile_definitions(${target} PRIVATE
            LAMAPON_WEB_AUDIO_ENABLED=1
        )
    # audio moduleがない場合は無効にします。
    else()
        target_compile_definitions(${target} PRIVATE
            LAMAPON_WEB_AUDIO_ENABLED=0
        )
    endif()
    # Sceneの例外処理を使うためEmscripten例外を有効にします。
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -fexceptions
    )

    target_link_options(${target} PRIVATE
        -fexceptions
        "-sALLOW_MEMORY_GROWTH=1"
        "-sNO_EXIT_RUNTIME=1"
        "-sASSERTIONS=1"
    )
    # renderer moduleにWebGLを設定します。
    if("renderer2d" IN_LIST TWG_MODULES
       OR "renderer3d" IN_LIST TWG_MODULES)
        target_link_options(${target} PRIVATE
            "-sMIN_WEBGL_VERSION=1"
            "-sMAX_WEBGL_VERSION=2"
        )
    endif()
    # runtimeを単独HTMLへ埋め込みます。
    if(TWG_SINGLE_FILE)
        # file://で開ける単独HTML/Wasmを生成します。
        target_link_options(${target} PRIVATE
            "-sSINGLE_FILE=1"
            "-sSINGLE_FILE_BINARY_ENCODE=0"
        )
    endif()
    # TWG_SHELL_FILEがあればEmscriptenへ渡します。
    if(TWG_SHELL_FILE)
        # shell_file: targetへ渡すshell templateのabsolute path。
        get_filename_component(shell_file "${TWG_SHELL_FILE}" ABSOLUTE)
        target_link_options(${target} PRIVATE
            "--shell-file=${shell_file}"
        )
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${shell_file}")
    endif()
    # TWG_ASSET_DIRECTORYがあればembed fileとlink dependencyへ追加します。
    if(TWG_ASSET_DIRECTORY)
        # asset_directory: embedするasset directoryのabsolute path。
        get_filename_component(asset_directory
            "${TWG_ASSET_DIRECTORY}"
            ABSOLUTE
        )
        # 存在しないdirectoryを拒否します。
        if(NOT IS_DIRECTORY "${asset_directory}")
            message(FATAL_ERROR
                "Web asset directory was not found: ${asset_directory}")
        endif()
        target_link_options(${target} PRIVATE
            "--embed-file=${asset_directory}@/assets"
        )
        # lamapon_web_asset_files: embed対象asset file一覧。
        file(GLOB_RECURSE lamapon_web_asset_files
            CONFIGURE_DEPENDS
            LIST_DIRECTORIES false
            "${asset_directory}/*"
        )
        # 検出したassetをlink依存に加えます。
        if(lamapon_web_asset_files)
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
                ${lamapon_web_asset_files}
            )
        endif()
    endif()

    # LAMAPON_WEB_OUTPUT_NAMEがあればtarget引数より優先します。
    if(DEFINED LAMAPON_WEB_OUTPUT_NAME AND NOT LAMAPON_WEB_OUTPUT_NAME STREQUAL "")
        # output_name: globalで指定されたHTML file名。
        set(output_name "${LAMAPON_WEB_OUTPUT_NAME}")
    # global名がない場合はTWG_OUTPUT_NAMEを採用します。
    elseif(TWG_OUTPUT_NAME)
        # output_name: function optionで指定されたHTML file名。
        set(output_name "${TWG_OUTPUT_NAME}")
    # 両方未指定ならtarget名を使います。
    else()
        # output_name: target nameを使うdefault HTML file名。
        set(output_name "${target}")
    endif()
    # targetへ確定したHTML output nameとmodule listを設定します。
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        SUFFIX ".html"
        LAMAPON_WEB_MODULES "${TWG_MODULES}"
    )
endfunction()
