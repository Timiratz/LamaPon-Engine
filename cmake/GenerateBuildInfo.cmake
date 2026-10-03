# 毎回Git情報を確認し、同じ内容の生成ファイルは書き換えません。
cmake_minimum_required(VERSION 3.25)

# required: 検証対象の必須CMake input名。
foreach(required IN ITEMS SOURCE_DIR BINARY_DIR PROJECT_VERSION)
    # 入力が指定されていない場合は構成を中止します。
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "GenerateBuildInfo.cmake requires -D${required}=...")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/LamaPonGitInfo.cmake")
lamapon_write_build_info("${SOURCE_DIR}" "${BINARY_DIR}")
