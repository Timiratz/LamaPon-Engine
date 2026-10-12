cmake_minimum_required(VERSION 3.25)

project(LamaPonProjectGameModule LANGUAGES CXX)

# LAMAPON_MODULE_FAST_BUILD: 編集時のコンパイルとリンクを短縮するか。
option(LAMAPON_MODULE_FAST_BUILD "Use fast builds for editor iteration" OFF)

# Windows/MSVC以外のtargetはまだサポートしません。
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "LamaPon project modules currently require MSVC on Windows.")
endif()

# requiredVariable: 検証中の必須project・SDK root名.
foreach(requiredVariable
    LAMAPON_ENGINE_ROOT
    LAMAPON_PROJECT_ROOT
    LAMAPON_RUNTIME_DIR)
# 未指定rootを検出した場合はconfigureを停止します。
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} must be specified.")
    endif()
endforeach()

# LAMAPON_ENGINE_ROOT・LAMAPON_PROJECT_ROOT・LAMAPON_RUNTIME_DIRを正規化します。
cmake_path(ABSOLUTE_PATH LAMAPON_ENGINE_ROOT NORMALIZE)
cmake_path(ABSOLUTE_PATH LAMAPON_PROJECT_ROOT NORMALIZE)
cmake_path(ABSOLUTE_PATH LAMAPON_RUNTIME_DIR NORMALIZE)

include("${LAMAPON_ENGINE_ROOT}/cmake/LamaPonMsvcDependencies.cmake")
lamapon_configure_msvc_dependencies()

# SDK source headerの有無を検証します。
if(NOT EXISTS "${LAMAPON_ENGINE_ROOT}/src/LamaPon/LamaPon.h")
    message(FATAL_ERROR "LAMAPON_ENGINE_ROOT does not contain LamaPon sources.")
endif()
# project内のassets directoryを検証します。
if(NOT EXISTS "${LAMAPON_PROJECT_ROOT}/assets")
    message(FATAL_ERROR "LAMAPON_PROJECT_ROOT does not contain an assets folder.")
endif()
# runtime import libraryの有無を検証します。
if(NOT EXISTS "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.lib")
    message(FATAL_ERROR "LamaPonRuntime.lib was not found in LAMAPON_RUNTIME_DIR.")
endif()

# compile対象SDK headerからdescriptor用API versionを読みます。
# runtime versionが渡された場合はSDKとの不一致をconfigure時に拒否します。
# LAMAPON_GAME_MODULE_API_HEADER: API version定義を持つSDK header.
set(LAMAPON_GAME_MODULE_API_HEADER
    "${LAMAPON_ENGINE_ROOT}/src/LamaPon/Scripting/GameModule.h")
# SDK headerがなければmoduleを構成できません。
if(NOT EXISTS "${LAMAPON_GAME_MODULE_API_HEADER}")
    message(FATAL_ERROR
        "LAMAPON_ENGINE_ROOT does not contain GameModule.h: "
        "${LAMAPON_GAME_MODULE_API_HEADER}")
endif()
# API version headerの変更でconfigureを再実行します。
set_property(DIRECTORY APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS "${LAMAPON_GAME_MODULE_API_HEADER}")
# LAMAPON_GAME_MODULE_API_VERSION_LINE: SDK header内のAPI version宣言.
file(STRINGS "${LAMAPON_GAME_MODULE_API_HEADER}"
    LAMAPON_GAME_MODULE_API_VERSION_LINE
    REGEX "GameModuleApiVersion[ \\t]*=[ \\t]*[0-9]+"
    LIMIT_COUNT 1)
# API version宣言を抽出できなければconfigureを止めます。
if(NOT LAMAPON_GAME_MODULE_API_VERSION_LINE MATCHES
    "GameModuleApiVersion[ \\t]*=[ \\t]*([0-9]+)")
    message(FATAL_ERROR
        "Could not read GameModuleApiVersion from: "
        "${LAMAPON_GAME_MODULE_API_HEADER}")
endif()
# LAMAPON_GAME_MODULE_API_VERSION: SDK headerから読み取ったversion.
set(LAMAPON_GAME_MODULE_API_VERSION "${CMAKE_MATCH_1}")
# runtime API versionが指定された場合にSDK versionと比較します。
if(DEFINED LAMAPON_RUNTIME_API_VERSION
    AND NOT "${LAMAPON_RUNTIME_API_VERSION}" STREQUAL ""
    AND NOT "${LAMAPON_RUNTIME_API_VERSION}" STREQUAL
        "${LAMAPON_GAME_MODULE_API_VERSION}")
    message(FATAL_ERROR
        "The installed Game Module SDK is API "
        "${LAMAPON_GAME_MODULE_API_VERSION}, but LamaPonRuntime is API "
        "${LAMAPON_RUNTIME_API_VERSION}. Rebuild and reinstall LamaPon "
        "so the Runtime and SDK are from the same version.")
endif()

# CMAKE_CXX_STANDARD: moduleをcompileするC++ standard.
set(CMAKE_CXX_STANDARD 20)
# CMAKE_CXX_STANDARD_REQUIRED: toolchain defaultへのfallbackを禁止します。
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CMAKE_CXX_EXTENSIONS: compiler固有のextensionを無効にします。
set(CMAKE_CXX_EXTENSIONS OFF)

# LAMAPON_PACKAGE_NATIVE_FILE: 検証済みpackage依存設定.
set(LAMAPON_PACKAGE_NATIVE_FILE
    "${LAMAPON_PROJECT_ROOT}/.lamapon/package-native.cmake")
# ファイルの追加・変更でconfigureを再実行します。
set_property(DIRECTORY APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS "${LAMAPON_PACKAGE_NATIVE_FILE}")
# LAMAPON_PACKAGE_INCLUDE_DIRECTORIES: package header search path一覧.
set(LAMAPON_PACKAGE_INCLUDE_DIRECTORIES)
# LAMAPON_PACKAGE_LIBRARIES: package link library一覧.
set(LAMAPON_PACKAGE_LIBRARIES)
# LAMAPON_PACKAGE_DEFINES: package compile definition一覧.
set(LAMAPON_PACKAGE_DEFINES)
# 生成済みpackage native設定がある場合だけ取り込みます。
if(EXISTS "${LAMAPON_PACKAGE_NATIVE_FILE}")
    include("${LAMAPON_PACKAGE_NATIVE_FILE}")
endif()

# LAMAPON_PROJECT_SCRIPT_SOURCES: assets内のproject script一覧.
file(GLOB_RECURSE LAMAPON_PROJECT_SCRIPT_SOURCES CONFIGURE_DEPENDS
    "${LAMAPON_PROJECT_ROOT}/assets/*.cpp"
)

add_library(LamaPonRuntime SHARED IMPORTED GLOBAL)
set_target_properties(LamaPonRuntime PROPERTIES
    IMPORTED_LOCATION "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.dll"
    IMPORTED_IMPLIB "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.lib"
)

# generated descriptorはSDK API version変更時に再compileします。
# stale NMake依存情報から旧API DLLが再利用されることを防ぎます。
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/ProjectGameModule.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ProjectGameModule.cpp"
    @ONLY)

# generated descriptorとproject scriptsからGame Module DLLを作ります。
add_library(LamaPonGameModule SHARED
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ProjectGameModule.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ScriptRegistry.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ScriptRegistry.h"
    ${LAMAPON_PROJECT_SCRIPT_SOURCES}
)

target_include_directories(LamaPonGameModule PRIVATE
    "${LAMAPON_ENGINE_ROOT}/src"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${LAMAPON_ENGINE_ROOT}/third_party/nlohmann"
    "${LAMAPON_ENGINE_ROOT}/third_party/DirectXTK/include"
    "${LAMAPON_ENGINE_ROOT}/third_party/XAudio2Redist/include"
)
# SDK source treeにVersion.hがない場合のfallbackを生成します。
# engine project versionからmodule用headerを生成します。
if(NOT EXISTS "${LAMAPON_ENGINE_ROOT}/src/LamaPon/Core/Version.h")
    # LAMAPON_MODULE_VERSION_LINE: engine CMakeLists.txtのproject version行.
    # module headerへ渡すversion宣言行です。
    file(STRINGS
        "${LAMAPON_ENGINE_ROOT}/CMakeLists.txt"
        LAMAPON_MODULE_VERSION_LINE
        REGEX "project\\(LamaPon VERSION"
        LIMIT_COUNT 1
    )
    # project version行からsemver 3成分を抽出します。
    if(LAMAPON_MODULE_VERSION_LINE MATCHES
        "VERSION +([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        # PROJECT_VERSION_MAJOR: engineのmajor version.
        set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
        # PROJECT_VERSION_MINOR: engineのminor version.
        set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
        # PROJECT_VERSION_PATCH: engineのpatch version.
        set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
    # version行が読めない場合は0.0.0を使います。
    else()
        # PROJECT_VERSION_MAJOR: fallback major version.
        set(PROJECT_VERSION_MAJOR 0)
        # PROJECT_VERSION_MINOR: fallback minor version.
        set(PROJECT_VERSION_MINOR 0)
        # PROJECT_VERSION_PATCH: fallback patch version.
        set(PROJECT_VERSION_PATCH 0)
    endif()
    # PROJECT_VERSION: Version.h.inへ渡すsemver文字列.
    set(PROJECT_VERSION
        "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

    configure_file(
        "${LAMAPON_ENGINE_ROOT}/cmake/Version.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/LamaPon/Core/Version.h"
        @ONLY
    )
    target_include_directories(LamaPonGameModule PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
endif()
target_compile_definitions(LamaPonGameModule PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    USING_XAUDIO2_REDIST
)
target_compile_options(LamaPonGameModule PRIVATE
    /W4
    /permissive-
    /utf-8
    $<$<CONFIG:Release>:/Gy>
)
target_link_options(LamaPonGameModule PRIVATE
    $<$<CONFIG:Release>:/PDBALTPATH:LamaPonGameModule.pdb>
)
if(LAMAPON_MODULE_FAST_BUILD)
    # configuration: 編集時に最適化を省く構成。
    foreach(configuration RELEASE RELWITHDEBINFO MINSIZEREL)
        string(REGEX REPLACE "(^|[ \t])[/-]O[12xs]([ \t]|$)" "\\1\\2"
            CMAKE_CXX_FLAGS_${configuration} "${CMAKE_CXX_FLAGS_${configuration}}")
        string(REGEX REPLACE "(^|[ \t])[/-]Ob[123]([ \t]|$)" "\\1\\2"
            CMAKE_CXX_FLAGS_${configuration} "${CMAKE_CXX_FLAGS_${configuration}}")
    endforeach()
    # RuntimeとのABIを維持し、Scriptだけ最適化を省きます。
    target_compile_options(LamaPonGameModule PRIVATE /Od /Ob0)
    target_precompile_headers(LamaPonGameModule PRIVATE
        "${LAMAPON_ENGINE_ROOT}/src/LamaPon/LamaPon.h")
    target_link_options(LamaPonGameModule PRIVATE
        /DEBUG:FULL /INCREMENTAL /OPT:NOREF /OPT:NOICF)
else()
    target_compile_options(LamaPonGameModule PRIVATE
        $<$<CONFIG:Release>:/GL>)
    target_link_options(LamaPonGameModule PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Release>:/LTCG>)
endif()

target_link_libraries(LamaPonGameModule PRIVATE LamaPonRuntime)

# LAMAPON_PACKAGE_INCLUDE_DIRECTORIESだけをinclude search pathへ追加します。
if(LAMAPON_PACKAGE_INCLUDE_DIRECTORIES)
    target_include_directories(LamaPonGameModule PRIVATE
        ${LAMAPON_PACKAGE_INCLUDE_DIRECTORIES})
endif()
# LAMAPON_PACKAGE_LIBRARIESをlink対象へ追加します。
if(LAMAPON_PACKAGE_LIBRARIES)
    target_link_libraries(LamaPonGameModule PRIVATE
        ${LAMAPON_PACKAGE_LIBRARIES})
    set_property(TARGET LamaPonGameModule APPEND PROPERTY LINK_DEPENDS
        ${LAMAPON_PACKAGE_LIBRARIES})
endif()
# LAMAPON_PACKAGE_DEFINESをcompile設定へ追加します。
if(LAMAPON_PACKAGE_DEFINES)
    target_compile_definitions(LamaPonGameModule PRIVATE
        ${LAMAPON_PACKAGE_DEFINES})
endif()

# runtimeのlib/dllを明示link依存にしstale NMake DLLを防ぎます。
set_property(TARGET LamaPonGameModule APPEND PROPERTY LINK_DEPENDS
    "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.lib"
    "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.dll"
)

# LAMAPON_MODULE_OUTPUT_DIR未指定ならproject module directoryを使います。
if(NOT DEFINED LAMAPON_MODULE_OUTPUT_DIR
    OR "${LAMAPON_MODULE_OUTPUT_DIR}" STREQUAL "")
    # LAMAPON_MODULE_OUTPUT_DIR: DLLとimport libraryの出力directory.
    set(LAMAPON_MODULE_OUTPUT_DIR
        "${LAMAPON_PROJECT_ROOT}/.lamapon/bin"
    )
endif()
# LAMAPON_MODULE_OUTPUT_DIRをabsolute pathへ正規化します。
cmake_path(ABSOLUTE_PATH LAMAPON_MODULE_OUTPUT_DIR NORMALIZE)
set_target_properties(LamaPonGameModule PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LamaPonGameModule"
    RUNTIME_OUTPUT_DIRECTORY "${LAMAPON_MODULE_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${LAMAPON_MODULE_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${LAMAPON_MODULE_OUTPUT_DIR}"
)

# LAMAPON_MODULE_DEPLOY_DIR指定時だけlink後のDLLを配置します。
# copy_if_differentで共有先の不要なtimestamp更新を防ぎます。
if(DEFINED LAMAPON_MODULE_DEPLOY_DIR
    AND NOT "${LAMAPON_MODULE_DEPLOY_DIR}" STREQUAL "")
    # LAMAPON_MODULE_DEPLOY_DIR: network projectへのdeploy先.
    cmake_path(ABSOLUTE_PATH LAMAPON_MODULE_DEPLOY_DIR NORMALIZE)
    add_custom_command(TARGET LamaPonGameModule POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${LAMAPON_MODULE_DEPLOY_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:LamaPonGameModule>"
            "${LAMAPON_MODULE_DEPLOY_DIR}/LamaPonGameModule.dll"
        VERBATIM
    )
endif()
