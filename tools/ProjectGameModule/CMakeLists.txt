cmake_minimum_required(VERSION 3.25)

project(LamaPonProjectGameModule LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "LamaPon project modules currently require MSVC on Windows.")
endif()

foreach(requiredVariable
    LAMAPON_ENGINE_ROOT
    LAMAPON_PROJECT_ROOT
    LAMAPON_RUNTIME_DIR)
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} must be specified.")
    endif()
endforeach()

cmake_path(ABSOLUTE_PATH LAMAPON_ENGINE_ROOT NORMALIZE)
cmake_path(ABSOLUTE_PATH LAMAPON_PROJECT_ROOT NORMALIZE)
cmake_path(ABSOLUTE_PATH LAMAPON_RUNTIME_DIR NORMALIZE)

include("${LAMAPON_ENGINE_ROOT}/cmake/LamaPonMsvcDependencies.cmake")
lamapon_configure_msvc_dependencies()

if(NOT EXISTS "${LAMAPON_ENGINE_ROOT}/src/LamaPon/LamaPon.h")
    message(FATAL_ERROR "LAMAPON_ENGINE_ROOT does not contain LamaPon sources.")
endif()
if(NOT EXISTS "${LAMAPON_PROJECT_ROOT}/assets")
    message(FATAL_ERROR "LAMAPON_PROJECT_ROOT does not contain an assets folder.")
endif()
if(NOT EXISTS "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.lib")
    message(FATAL_ERROR "LamaPonRuntime.lib was not found in LAMAPON_RUNTIME_DIR.")
endif()

# Game Moduleのdescriptorへ埋め込むAPI版数を、実際にコンパイルするSDK
# ヘッダーから読み取る。ビルドを起動したRuntimeの版数も渡されている場合は
# ここで照合するため、Runtimeだけ更新されSDKが古い状態を明確に検出できる。
set(LAMAPON_GAME_MODULE_API_HEADER
    "${LAMAPON_ENGINE_ROOT}/src/LamaPon/Scripting/GameModule.h")
if(NOT EXISTS "${LAMAPON_GAME_MODULE_API_HEADER}")
    message(FATAL_ERROR
        "LAMAPON_ENGINE_ROOT does not contain GameModule.h: "
        "${LAMAPON_GAME_MODULE_API_HEADER}")
endif()
set_property(DIRECTORY APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS "${LAMAPON_GAME_MODULE_API_HEADER}")
file(STRINGS "${LAMAPON_GAME_MODULE_API_HEADER}"
    LAMAPON_GAME_MODULE_API_VERSION_LINE
    REGEX "GameModuleApiVersion[ \\t]*=[ \\t]*[0-9]+"
    LIMIT_COUNT 1)
if(NOT LAMAPON_GAME_MODULE_API_VERSION_LINE MATCHES
    "GameModuleApiVersion[ \\t]*=[ \\t]*([0-9]+)")
    message(FATAL_ERROR
        "Could not read GameModuleApiVersion from: "
        "${LAMAPON_GAME_MODULE_API_HEADER}")
endif()
set(LAMAPON_GAME_MODULE_API_VERSION "${CMAKE_MATCH_1}")
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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# パッケージが持ち込むネイティブ依存です。assets/packages/<名前>/
# package.json の native を LamaPon が検証して生成します。ここでは
# 生成済みの変数を読み込むだけで、パッケージ側が任意のコンパイル／
# リンクオプションを差し込めないようにしています。
# ファイルが無い状態から現れた場合にも再configureできるよう、
# 存在の有無に関わらず依存として登録します。
set(LAMAPON_PACKAGE_NATIVE_FILE
    "${LAMAPON_PROJECT_ROOT}/.lamapon/package-native.cmake")
set_property(DIRECTORY APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS "${LAMAPON_PACKAGE_NATIVE_FILE}")
set(LAMAPON_PACKAGE_INCLUDE_DIRECTORIES)
set(LAMAPON_PACKAGE_LIBRARIES)
set(LAMAPON_PACKAGE_DEFINES)
if(EXISTS "${LAMAPON_PACKAGE_NATIVE_FILE}")
    include("${LAMAPON_PACKAGE_NATIVE_FILE}")
endif()

file(GLOB_RECURSE LAMAPON_PROJECT_SCRIPT_SOURCES CONFIGURE_DEPENDS
    "${LAMAPON_PROJECT_ROOT}/assets/*.cpp"
)

add_library(LamaPonRuntime SHARED IMPORTED GLOBAL)
set_target_properties(LamaPonRuntime PROPERTIES
    IMPORTED_LOCATION "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.dll"
    IMPORTED_IMPLIB "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.lib"
)

# descriptorを生成済みソースにすることで、API番号が変わった場合は
# CMakeの全ジェネレーター（NMakeを含む）で必ず再コンパイルされる。
# ヘッダー依存だけに任せると、NMakeやネットワークドライブの古い依存情報が
# ProjectGameModule.cppを再利用し、旧API番号のDLLを作ることがある。
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/ProjectGameModule.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ProjectGameModule.cpp"
    @ONLY)

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
# 配布パッケージにはsrc/LamaPon/Core/Version.hが含まれます。エンジンの
# ソースツリーをLAMAPON_ENGINE_ROOTへ直接指定した場合は、本体のビルド前でも
# 解決できるよう、このモジュールのビルドディレクトリにも同じ内容を生成します。
if(NOT EXISTS "${LAMAPON_ENGINE_ROOT}/src/LamaPon/Core/Version.h")
    set(LAMAPON_BUILD_REVISION "unknown")
    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${LAMAPON_ENGINE_ROOT}/.git")
        execute_process(
            COMMAND
                "${GIT_EXECUTABLE}"
                -C "${LAMAPON_ENGINE_ROOT}"
                rev-parse --short=12 HEAD
            OUTPUT_VARIABLE LAMAPON_MODULE_GIT_REVISION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE LAMAPON_MODULE_GIT_REVISION_RESULT
            ERROR_QUIET
        )
        if(LAMAPON_MODULE_GIT_REVISION_RESULT EQUAL 0
            AND NOT LAMAPON_MODULE_GIT_REVISION STREQUAL "")
            set(LAMAPON_BUILD_REVISION "${LAMAPON_MODULE_GIT_REVISION}")
        endif()
    endif()

    # バージョン番号はエンジン本体のCMakeLists.txtを正本とし、
    # project()のVERSIONから読み取ります。
    file(STRINGS
        "${LAMAPON_ENGINE_ROOT}/CMakeLists.txt"
        LAMAPON_MODULE_VERSION_LINE
        REGEX "project\\(LamaPon VERSION"
        LIMIT_COUNT 1
    )
    if(LAMAPON_MODULE_VERSION_LINE MATCHES
        "VERSION +([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
        set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
        set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
    else()
        set(PROJECT_VERSION_MAJOR 0)
        set(PROJECT_VERSION_MINOR 0)
        set(PROJECT_VERSION_PATCH 0)
    endif()
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
    $<$<CONFIG:Release>:/GL>
)
target_link_options(LamaPonGameModule PRIVATE
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
    $<$<CONFIG:Release>:/LTCG>
    $<$<CONFIG:Release>:/PDBALTPATH:LamaPonGameModule.pdb>
)
target_link_libraries(LamaPonGameModule PRIVATE LamaPonRuntime)

if(LAMAPON_PACKAGE_INCLUDE_DIRECTORIES)
    target_include_directories(LamaPonGameModule PRIVATE
        ${LAMAPON_PACKAGE_INCLUDE_DIRECTORIES})
endif()
if(LAMAPON_PACKAGE_LIBRARIES)
    target_link_libraries(LamaPonGameModule PRIVATE
        ${LAMAPON_PACKAGE_LIBRARIES})
    set_property(TARGET LamaPonGameModule APPEND PROPERTY LINK_DEPENDS
        ${LAMAPON_PACKAGE_LIBRARIES})
endif()
if(LAMAPON_PACKAGE_DEFINES)
    target_compile_definitions(LamaPonGameModule PRIVATE
        ${LAMAPON_PACKAGE_DEFINES})
endif()

# IMPORTED_IMPLIBをリンクしていても、NMakeでは外部Runtimeを建て直した
# だけのケースがGame Moduleの再リンク条件へ入らないことがあります。
# そのままbuildが成功扱いになると、Runtimeより古いDLLが残り、起動時の
# 互換性ガードに拒否されます。libとdllの両方を明示的なLink依存にします。
set_property(TARGET LamaPonGameModule APPEND PROPERTY LINK_DEPENDS
    "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.lib"
    "${LAMAPON_RUNTIME_DIR}/LamaPonRuntime.dll"
)

if(NOT DEFINED LAMAPON_MODULE_OUTPUT_DIR
    OR "${LAMAPON_MODULE_OUTPUT_DIR}" STREQUAL "")
    set(LAMAPON_MODULE_OUTPUT_DIR
        "${LAMAPON_PROJECT_ROOT}/.lamapon/bin"
    )
endif()
cmake_path(ABSOLUTE_PATH LAMAPON_MODULE_OUTPUT_DIR NORMALIZE)
set_target_properties(LamaPonGameModule PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LamaPonGameModule"
    RUNTIME_OUTPUT_DIRECTORY "${LAMAPON_MODULE_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${LAMAPON_MODULE_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${LAMAPON_MODULE_OUTPUT_DIR}"
)

# ネットワーク上のプロジェクトでは、中間生成物をローカルへ置き、リンクが
# 完了したDLLだけを共有先へ配置します。copy_if_differentなので、変更が
# 無い再ビルドでWebDAVのタイムスタンプを無駄に更新しません。
if(DEFINED LAMAPON_MODULE_DEPLOY_DIR
    AND NOT "${LAMAPON_MODULE_DEPLOY_DIR}" STREQUAL "")
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
