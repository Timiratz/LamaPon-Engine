cmake_minimum_required(VERSION 3.25)
project(LamaPonWebTests LANGUAGES CXX)
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake")
endif()
get_filename_component(engine_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(WebLifecycleTests
    WebLifecycleTests.cpp
    "${engine_root}/src/LamaPon/Web/WebInput.cpp"
    "${engine_root}/src/LamaPon/Portable/PortableLog.cpp")
target_compile_features(WebLifecycleTests PRIVATE cxx_std_20)
target_compile_options(WebLifecycleTests PRIVATE -Wall -Wextra -Werror -fexceptions)
target_include_directories(WebLifecycleTests PRIVATE
    "${engine_root}/src/LamaPon/Portable/include"
    "${engine_root}/src")
target_link_options(WebLifecycleTests PRIVATE
    "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/test-shell.html"
    "-sASSERTIONS=2" "-sSINGLE_FILE=1" -fexceptions)
set_target_properties(WebLifecycleTests PROPERTIES SUFFIX ".html")

# 実際のWebターゲット生成関数でも全Portableソースをビルドします。
include("${engine_root}/cmake/LamaPonWeb.cmake")
lamapon_add_web_game(PortableSmoke
    PORTABLE_GAME SINGLE_FILE
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/PortableSmoke.cpp"
    MODULES core input renderer2d renderer3d physics3d audio particles3d
    ASSET_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    SHELL_FILE "${engine_root}/src/LamaPon/Web/default-shell.html")
