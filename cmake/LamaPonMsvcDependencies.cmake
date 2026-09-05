# CMakeの自動検出では、ローカライズされた/showIncludesの出力を
# 別のコードページとして変換する場合があります。Ninjaはコンパイラの
# 出力バイト列と接頭辞を比較するため、実際の出力を変換せず採取します。
# 呼び出しはproject(... LANGUAGES CXX)の後に置きます。
function(lamapon_configure_msvc_dependencies)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
       OR NOT CMAKE_GENERATOR MATCHES "^Ninja")
        return()
    endif()

    set(probe_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/LamaPonMsvcDependencies")
    file(MAKE_DIRECTORY "${probe_dir}")
    file(WRITE "${probe_dir}/lamapon-msvc-prefix.h" "#pragma once\n")
    file(WRITE "${probe_dir}/probe.cpp" "#include \"lamapon-msvc-prefix.h\"\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c
            "${probe_dir}/probe.cpp" "/Fo${probe_dir}/probe.obj"
        WORKING_DIRECTORY "${probe_dir}"
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_error
        RESULT_VARIABLE probe_result
        ENCODING NONE
        TIMEOUT 30
    )
    # ドライブ名のコロンの後には空白がないため、接頭辞の末尾と
    # 区別できます。先頭の改行は結果に含めず、末尾の空白は保持します。
    if(probe_result EQUAL 0
       AND "${probe_output}\n${probe_error}" MATCHES
           "(^|[\r\n])([^\r\n]+: +)[^\r\n]*lamapon-msvc-prefix\\.h")
        set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
        # 検出方式と接頭辞のハッシュをコンパイル定義へ含め、接頭辞が
        # 変わった場合にNinjaの依存情報を再生成します。
        string(SHA256 prefix_hash "1:${CMAKE_MATCH_2}")
        string(SUBSTRING "${prefix_hash}" 0 8 prefix_hash)
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:CXX>:/DLAMAPON_MSVC_DEPENDENCY_FORMAT=0x${prefix_hash}>")
    else()
        # 不明な接頭辞で構成を続けると、ヘッダー変更を取りこぼして
        # 成功扱いになるため、この組み合わせでは構成時に止めます。
        message(FATAL_ERROR
            "Could not detect the MSVC /showIncludes prefix for Ninja. "
            "Compiler probe result: ${probe_result}\n${probe_output}\n${probe_error}")
    endif()
endfunction()
