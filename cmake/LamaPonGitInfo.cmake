# ビルドしたソースを特定するためのGit情報を集めます。
#
# LamaPonの表示用バージョンは「ブランチ名 @ コミット」です。
# MAJOR.MINOR.PATCHはパッケージやプロジェクトの互換判定だけに使い、
# どのソースから作ったかはここで集めた値で示します。
#
# 通常のCMake構成と、ビルドのたびに実行するスクリプトモード
# （GenerateBuildInfo.cmake）の両方から使います。

# C++文字列リテラルとJSON文字列のどちらへ埋め込んでも壊れないよう、
# バックスラッシュと二重引用符をエスケープし、制御文字は空白へ寄せます。
function(lamapon_escape_build_string output value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REGEX REPLACE "[\r\n\t]" " " escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

function(lamapon_run_git output root)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${root}" ${ARGN}
        OUTPUT_VARIABLE git_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_result
        ERROR_QUIET
    )
    if(git_result EQUAL 0)
        set(${output} "${git_output}" PARENT_SCOPE)
    else()
        set(${output} "" PARENT_SCOPE)
    endif()
endfunction()

# <prefix>_BRANCH / _COMMIT / _COMMIT_FULL / _SUBJECT / _DIRTY /
# _REVISION を設定します。Gitが無い、またはGit管理外のソース
# （配布zipなど）では "unknown" になります。
#
# ブランチ名は次の順で決めます。
#   1. 環境変数 LAMAPON_BUILD_BRANCH（CIや手元での明示指定）
#   2. git rev-parse --abbrev-ref HEAD
#   3. detached HEADなら GitHub Actions の GITHUB_HEAD_REF /
#      GITHUB_REF_NAME
#   4. それも無ければ "detached"
function(lamapon_read_git_info root prefix)
    set(branch "unknown")
    set(commit "unknown")
    set(commit_full "unknown")
    set(subject "")
    set(dirty OFF)

    if(NOT GIT_EXECUTABLE)
        find_package(Git QUIET)
    endif()
    if(GIT_EXECUTABLE AND EXISTS "${root}/.git")
        lamapon_run_git(git_commit "${root}" rev-parse --short=12 HEAD)
        if(NOT git_commit STREQUAL "")
            set(commit "${git_commit}")
            lamapon_run_git(git_commit_full "${root}" rev-parse HEAD)
            if(NOT git_commit_full STREQUAL "")
                set(commit_full "${git_commit_full}")
            endif()
            lamapon_run_git(git_subject "${root}" log -1 --format=%s)
            set(subject "${git_subject}")

            lamapon_run_git(git_branch "${root}"
                rev-parse --abbrev-ref HEAD)
            if(git_branch STREQUAL "HEAD")
                if(NOT "$ENV{GITHUB_HEAD_REF}" STREQUAL "")
                    set(git_branch "$ENV{GITHUB_HEAD_REF}")
                elseif(NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
                    set(git_branch "$ENV{GITHUB_REF_NAME}")
                else()
                    set(git_branch "detached")
                endif()
            endif()
            if(NOT git_branch STREQUAL "")
                set(branch "${git_branch}")
            endif()

            # 生成物（build/やout/）は.gitignore済みのため、
            # 追跡ファイルと未追跡ソースの変更だけを見ます。
            lamapon_run_git(git_status "${root}" status --porcelain)
            if(NOT git_status STREQUAL "")
                set(dirty ON)
            endif()
        endif()
    endif()

    if(NOT "$ENV{LAMAPON_BUILD_BRANCH}" STREQUAL "")
        set(branch "$ENV{LAMAPON_BUILD_BRANCH}")
    endif()

    set(revision "${commit}")
    if(dirty)
        string(APPEND revision "-dirty")
    endif()

    set(${prefix}_BRANCH "${branch}" PARENT_SCOPE)
    set(${prefix}_COMMIT "${commit}" PARENT_SCOPE)
    set(${prefix}_COMMIT_FULL "${commit_full}" PARENT_SCOPE)
    set(${prefix}_SUBJECT "${subject}" PARENT_SCOPE)
    set(${prefix}_DIRTY "${dirty}" PARENT_SCOPE)
    set(${prefix}_REVISION "${revision}" PARENT_SCOPE)
endfunction()

# BuildInfoData.cpp.in / BuildInfo.json.in を展開します。
# configure_fileは内容が同じなら出力を書き換えないため、
# コミットが変わらない限り再コンパイルは起きません。
function(lamapon_write_build_info source_dir binary_dir)
    lamapon_read_git_info("${source_dir}" LAMAPON_GIT)

    if(LAMAPON_GIT_DIRTY)
        set(LAMAPON_BUILD_DIRTY_CPP "true")
        set(LAMAPON_BUILD_DIRTY_JSON "true")
    else()
        set(LAMAPON_BUILD_DIRTY_CPP "false")
        set(LAMAPON_BUILD_DIRTY_JSON "false")
    endif()
    lamapon_escape_build_string(LAMAPON_BUILD_BRANCH
        "${LAMAPON_GIT_BRANCH}")
    lamapon_escape_build_string(LAMAPON_BUILD_COMMIT
        "${LAMAPON_GIT_COMMIT}")
    lamapon_escape_build_string(LAMAPON_BUILD_COMMIT_FULL
        "${LAMAPON_GIT_COMMIT_FULL}")
    lamapon_escape_build_string(LAMAPON_BUILD_COMMIT_SUBJECT
        "${LAMAPON_GIT_SUBJECT}")
    lamapon_escape_build_string(LAMAPON_BUILD_REVISION
        "${LAMAPON_GIT_REVISION}")

    configure_file(
        "${source_dir}/cmake/BuildInfoData.cpp.in"
        "${binary_dir}/generated/LamaPon/Core/BuildInfoData.cpp"
        @ONLY
    )
    configure_file(
        "${source_dir}/cmake/BuildInfo.json.in"
        "${binary_dir}/generated/build-info.json"
        @ONLY
    )
endfunction()
