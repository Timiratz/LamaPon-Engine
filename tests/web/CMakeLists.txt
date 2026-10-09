cmake_minimum_required(VERSION 3.25)
project(LamaPonWebTests LANGUAGES CXX)
# EMSCRIPTENが無効ならWeb test構成を拒否します。
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake")
endif()
# engine_root: repository rootのabsolute path.
get_filename_component(engine_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(WebLifecycleTests
    "${engine_root}/src/LamaPon/Portable/PortableInputState.cpp"
    WebLifecycleTests.cpp
    "${engine_root}/src/LamaPon/Web/WebInput.cpp"
    "${engine_root}/src/LamaPon/Web/WebRenderer3D.cpp"
    "${engine_root}/src/LamaPon/Portable/PortableLog.cpp")
target_compile_features(WebLifecycleTests PRIVATE cxx_std_20)
target_compile_options(WebLifecycleTests PRIVATE -Wall -Wextra -Werror -fexceptions)
target_include_directories(WebLifecycleTests PRIVATE
    "${engine_root}/src/LamaPon/Portable/include"
    "${engine_root}/src")
target_link_options(WebLifecycleTests PRIVATE
    "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/test-shell.html"
    "-sASSERTIONS=2" "-sSINGLE_FILE=1" "-sMAX_WEBGL_VERSION=2" -fexceptions)
set_target_properties(WebLifecycleTests PROPERTIES SUFFIX ".html")

# 実際のWebターゲット生成関数でも全Portableソースをビルドします。
include("${engine_root}/cmake/LamaPonWeb.cmake")
lamapon_add_web_game(PortableSmoke
    PORTABLE_GAME SINGLE_FILE
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/PortableSmoke.cpp"
    MODULES core input renderer2d renderer3d physics3d audio particles3d
    ASSET_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    SHELL_FILE "${engine_root}/src/LamaPon/Web/default-shell.html")
target_compile_definitions(PortableSmoke PRIVATE LAMAPON_WEB_TEXTURE_PROBE=1)
target_link_options(PortableSmoke PRIVATE
    "--embed-file=${engine_root}/tests/fixtures/models@/assets/models")
set_property(TARGET PortableSmoke APPEND PROPERTY LINK_DEPENDS
    "${engine_root}/tests/fixtures/models/embedded-data.gltf"
    "${engine_root}/tests/fixtures/models/embedded-buffer.glb")
