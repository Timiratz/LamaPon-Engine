# lamapon_configure_msvc_dependencies(): Ninja/MSVCのshowIncludes prefixを設定します。
function(lamapon_configure_msvc_dependencies)
    # MSVC/Ninja以外ではCMake標準の依存検出を使います。
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
       OR NOT CMAKE_GENERATOR MATCHES "^Ninja")
        return()
    endif()

    # probe_dir: showIncludes検出用source・object directory。
    set(probe_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/LamaPonMsvcDependencies")
    file(MAKE_DIRECTORY "${probe_dir}")
    file(WRITE "${probe_dir}/lamapon-msvc-prefix.h" "#pragma once\n")
    file(WRITE "${probe_dir}/probe.cpp" "#include \"lamapon-msvc-prefix.h\"\n")
    # probe_output・probe_error・probe_result: compiler出力と終了状態。
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
    # drive文字の後の空白で接頭辞とpathを区別します。
    # 末尾の空白も接頭辞の一部として保持します。
    # 成功したprobe出力からMSVCのinclude prefixを抽出します。
    if(probe_result EQUAL 0
       AND "${probe_output}\n${probe_error}" MATCHES
           "(^|[\r\n])([^\r\n]+: +)[^\r\n]*lamapon-msvc-prefix\\.h")
        set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
        # 接頭辞変更時にNinjaの依存情報を再生成します。
        # prefix_hash: 検出したinclude prefixのbuild識別hash。
        string(SHA256 prefix_hash "1:${CMAKE_MATCH_2}")
        string(SUBSTRING "${prefix_hash}" 0 8 prefix_hash)
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:CXX>:/DLAMAPON_MSVC_DEPENDENCY_FORMAT=0x${prefix_hash}>")
    # probe失敗時はheader依存を保証できないため構成を止めます。
    else()
        message(FATAL_ERROR
            "Could not detect the MSVC /showIncludes prefix for Ninja. "
            "Compiler probe result: ${probe_result}\n${probe_output}\n${probe_error}")
    endif()
endfunction()
