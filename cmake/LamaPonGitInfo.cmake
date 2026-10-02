# lamapon_escape_build_string(output: 出力変数, value: 入力文字列): C++・JSON用にescapeします。
function(lamapon_escape_build_string output value)
    # escaped: C++・JSONへ安全に埋め込む文字列。
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REGEX REPLACE "[\r\n\t]" " " escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

# lamapon_run_git(output: 出力変数, root: repository, ARGN: Git引数): Git commandのstdoutを返します。
function(lamapon_run_git output root)
    # git_output/git_result: Git stdoutとprocess exit code。
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${root}" ${ARGN}
        OUTPUT_VARIABLE git_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_result
        ERROR_QUIET
    )
    # 成功時は標準出力を返します。
    if(git_result EQUAL 0)
        set(${output} "${git_output}" PARENT_SCOPE)
    # 失敗時は空文字を返します。
    else()
        set(${output} "" PARENT_SCOPE)
    endif()
endfunction()

# lamapon_read_git_info(root: repository, prefix: output name): Git metadataとdirty状態を設定します。
function(lamapon_read_git_info root prefix)
    # 表示用branch名
    set(branch "unknown")
    # 短縮commit hash
    set(commit "unknown")
    # 完全なcommit hash
    set(commit_full "unknown")
    # 最新commitのsubject
    set(subject "")
    # 作業treeの変更有無
    set(dirty OFF)

    # Git executable未検出なら検索します。
    if(NOT GIT_EXECUTABLE)
        find_package(Git QUIET)
    endif()
    # Git管理treeならcommit情報を読みます。
    if(GIT_EXECUTABLE AND EXISTS "${root}/.git")
        # 短縮commit hashの取得
        lamapon_run_git(git_commit "${root}" rev-parse --short=12 HEAD)
        # commit hashを取得できた場合だけ詳細を読みます。
        if(NOT git_commit STREQUAL "")
            set(commit "${git_commit}")
            # 完全なcommit hashの取得
            lamapon_run_git(git_commit_full "${root}" rev-parse HEAD)
            # 完全hashが取れた場合だけ置き換えます。
            if(NOT git_commit_full STREQUAL "")
                set(commit_full "${git_commit_full}")
            endif()
            # 最新commit subjectの取得
            lamapon_run_git(git_subject "${root}" log -1 --format=%s)
            set(subject "${git_subject}")

            # 現在のbranch名を取得します。
            lamapon_run_git(git_branch "${root}"
                rev-parse --abbrev-ref HEAD)
            # detached HEADならCIのref情報を調べます。
            if(git_branch STREQUAL "HEAD")
                # PRのhead branchを優先します。
                if(NOT "$ENV{GITHUB_HEAD_REF}" STREQUAL "")
                    set(git_branch "$ENV{GITHUB_HEAD_REF}")
                # 通常のCI ref名を次に使います。
                elseif(NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
                    set(git_branch "$ENV{GITHUB_REF_NAME}")
                # refを特定できない場合の表示名
                else()
                    set(git_branch "detached")
                endif()
            endif()
            # 取得できたbranch名を採用します。
            if(NOT git_branch STREQUAL "")
                set(branch "${git_branch}")
            endif()

            # git_status: generated filesを除くGit working tree状態。
            lamapon_run_git(git_status "${root}" status --porcelain)
            # 未commitの変更がある場合はdirtyを記録します。
            if(NOT git_status STREQUAL "")
                set(dirty ON)
            endif()
        endif()
    endif()

    # 明示branch指定は検出結果より優先します。
    if(NOT "$ENV{LAMAPON_BUILD_BRANCH}" STREQUAL "")
        set(branch "$ENV{LAMAPON_BUILD_BRANCH}")
    endif()

    # Revision表示文字列
    set(revision "${commit}")
    # dirty treeはrevisionへ印を付けます。
    if(dirty)
        string(APPEND revision "-dirty")
    endif()

    # 呼び出し側へbranch名を返します。
    set(${prefix}_BRANCH "${branch}" PARENT_SCOPE)
    # 呼び出し側へ短縮hashを返します。
    set(${prefix}_COMMIT "${commit}" PARENT_SCOPE)
    # 呼び出し側へ完全hashを返します。
    set(${prefix}_COMMIT_FULL "${commit_full}" PARENT_SCOPE)
    # 呼び出し側へcommit subjectを返します。
    set(${prefix}_SUBJECT "${subject}" PARENT_SCOPE)
    # 呼び出し側へdirty状態を返します。
    set(${prefix}_DIRTY "${dirty}" PARENT_SCOPE)
    # 呼び出し側へrevisionを返します。
    set(${prefix}_REVISION "${revision}" PARENT_SCOPE)
endfunction()

# lamapon_write_build_info(source_dir: source root, binary_dir: build root): C++・JSON BuildInfoを生成します。
function(lamapon_write_build_info source_dir binary_dir)
    lamapon_read_git_info("${source_dir}" LAMAPON_GIT)

    # dirty状態をC++とJSON用の文字列へ変換します。
    if(LAMAPON_GIT_DIRTY)
        # C++用dirty literal
        set(LAMAPON_BUILD_DIRTY_CPP "true")
        # JSON用dirty literal
        set(LAMAPON_BUILD_DIRTY_JSON "true")
    # clean時はfalseを生成します。
    else()
        set(LAMAPON_BUILD_DIRTY_CPP "false")
        set(LAMAPON_BUILD_DIRTY_JSON "false")
    endif()
    # LAMAPON_BUILD_BRANCH/COMMIT/COMMIT_FULL/COMMIT_SUBJECT/REVISION: escape済みBuildInfo文字列。
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
