# Linux対応の最初の段階として、OS・描画・音声・ウィンドウへ依存しない
# エンジン基盤だけをビルドします。Windowsデスクトップ版のターゲットは
# この構成には含めません。

find_package(Threads REQUIRED)

add_library(LamaPonHeadless STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/CrashSentinel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/CrashSentinel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/JobSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/JobSystem.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/Profiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/Profiler.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/Time.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/Time.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/VersionCompare.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Core/VersionCompare.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Reactive/Reactive.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LamaPon/Reactive/Reactive.h
)
add_library(LamaPon::Headless ALIAS LamaPonHeadless)

target_include_directories(LamaPonHeadless
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_features(LamaPonHeadless PUBLIC cxx_std_20)
target_compile_definitions(LamaPonHeadless PUBLIC LAMAPON_HEADLESS=1)
target_compile_options(LamaPonHeadless PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/permissive->
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wpedantic>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Werror>
)
target_link_libraries(LamaPonHeadless PUBLIC Threads::Threads)

include(CTest)
if(BUILD_TESTING)
    add_executable(LamaPonHeadlessCoreTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/HeadlessCoreTests.cpp
    )
    target_link_libraries(LamaPonHeadlessCoreTests PRIVATE LamaPon::Headless)

    add_executable(LamaPonHeadlessReactiveTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/ReactiveTests.cpp
    )
    target_link_libraries(LamaPonHeadlessReactiveTests PRIVATE LamaPon::Headless)

    add_executable(LamaPonHeadlessCrashSentinelTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/CrashSentinelTests.cpp
    )
    target_link_libraries(
        LamaPonHeadlessCrashSentinelTests
        PRIVATE LamaPon::Headless
    )

    foreach(headless_test IN ITEMS
        LamaPonHeadlessCoreTests
        LamaPonHeadlessReactiveTests
        LamaPonHeadlessCrashSentinelTests
    )
        target_compile_options(${headless_test} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4>
            $<$<CXX_COMPILER_ID:MSVC>:/permissive->
            $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall>
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wextra>
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wpedantic>
        )
    endforeach()

    add_test(NAME HeadlessCore COMMAND LamaPonHeadlessCoreTests)
    add_test(NAME HeadlessReactive COMMAND LamaPonHeadlessReactiveTests)
    add_test(
        NAME HeadlessCrashSentinel
        COMMAND LamaPonHeadlessCrashSentinelTests
    )
    set_tests_properties(
        HeadlessCore
        HeadlessReactive
        HeadlessCrashSentinel
        PROPERTIES LABELS "headless;linux"
    )
endif()
