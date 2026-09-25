# ビルドのたびに実行し、表示用のブランチ名とコミットを最新に保ちます。
#
#   cmake -DSOURCE_DIR=<src> -DBINARY_DIR=<build>
#         -DPROJECT_VERSION=<x.y.z> -DCMAKE_GENERATOR=<...>
#         -DCMAKE_CXX_COMPILER_ID=<...>
#         -DCMAKE_CXX_COMPILER_VERSION=<...>
#         -DCMAKE_SYSTEM_PROCESSOR=<...>
#         -P cmake/GenerateBuildInfo.cmake
#
# 構成時にだけGitを読むと、再構成しない限りコミットを重ねても古い
# 値が表示され続けます。出力は内容が変わったときだけ書き換えるため、
# 変更が無いビルドではBuildInfoData.cppも再コンパイルされません。
cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS SOURCE_DIR BINARY_DIR PROJECT_VERSION)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "GenerateBuildInfo.cmake requires -D${required}=...")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/LamaPonGitInfo.cmake")
lamapon_write_build_info("${SOURCE_DIR}" "${BINARY_DIR}")
