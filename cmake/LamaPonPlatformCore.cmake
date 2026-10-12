# 共通基盤だけを構成する。完成したゲームランタイムの出力先ではない。
function(lamapon_add_platform_core)
    if(APPLE OR EMSCRIPTEN OR (NOT WIN32 AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        message(FATAL_ERROR "Platform foundation currently targets Windows, Linux and Android NDK only")
    endif()
    get_filename_component(engine_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    find_package(Threads REQUIRED)
    add_library(LamaPonPlatformCore STATIC
        "${engine_root}/src/LamaPon/Portable/PortableInputState.cpp"
        "${engine_root}/src/LamaPon/Core/JobSystem.cpp"
        "${engine_root}/src/LamaPon/Core/VersionCompare.cpp"
        "${engine_root}/src/LamaPon/Core/Unicode.h"
        "${engine_root}/src/LamaPon/Core/PathUtils.h")
    add_library(LamaPon::PlatformCore ALIAS LamaPonPlatformCore)
    target_compile_features(LamaPonPlatformCore PUBLIC cxx_std_20)
    target_include_directories(LamaPonPlatformCore PUBLIC "${engine_root}/src")
    target_link_libraries(LamaPonPlatformCore PUBLIC Threads::Threads)
    set_target_properties(LamaPonPlatformCore PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

    if(BUILD_TESTING)
        add_executable(LamaPonPlatformCoreTests "${engine_root}/tests/PlatformCoreTests.cpp")
        target_link_libraries(LamaPonPlatformCoreTests PRIVATE LamaPon::PlatformCore)
        # Androidでもリンクを確認するが、ホスト上でクロスビルドしたexeを実行しない。
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME PlatformCore COMMAND LamaPonPlatformCoreTests)
            set_tests_properties(PlatformCore PROPERTIES TIMEOUT 30)
        endif()
    endif()
    foreach(target LamaPonPlatformCore LamaPonPlatformCoreTests)
        if(TARGET ${target})
            if(MSVC)
                target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /EHsc)
            else()
                target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
    endforeach()
endfunction()
