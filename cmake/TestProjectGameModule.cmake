# 必須CMake input名を順に検証します。
foreach(requiredVariable
    ENGINE_ROOT
    PROJECT_ROOT
    RUNTIME_DIR
    TEST_BUILD_DIR
    TEST_OUTPUT_DIR
    TEST_LOADER
    TEST_GENERATOR
    TEST_BUILD_TYPE
    GENERATED_INCLUDE_DIR)
    # 未指定のinputを検出します。
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} is required.")
    endif()
endforeach()

# Visual Studio等では親ビルドと同じCPUと構成を使います。
set(generatorPlatformArguments "")
if(DEFINED TEST_GENERATOR_PLATFORM AND NOT "${TEST_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND generatorPlatformArguments -A "${TEST_GENERATOR_PLATFORM}")
endif()

# seen_ON/seen_OFF: 今回検証したモード。
set(seen_ON FALSE)
set(seen_OFF FALSE)
# fastBuild: キャッシュを再利用して切り替えるモード。
foreach(fastBuild ON OFF ON OFF)
    # modeBuildDir/modeOutputDir: モード別の中間・DLL配置先。
    set(modeBuildDir "${TEST_BUILD_DIR}/${fastBuild}")
    set(modeOutputDir "${TEST_OUTPUT_DIR}/${fastBuild}")
    # modulePath: キャッシュに保持するDLL。
    set(modulePath "${modeOutputDir}/LamaPonGameModule.dll")
    if(TEST_MULTI_CONFIG)
        set(modulePath "${modeOutputDir}/${TEST_BUILD_TYPE}/LamaPonGameModule.dll")
    endif()
    # previousModeTime: 他モードのビルド前に保持した時刻。
    if(seen_${fastBuild})
        file(TIMESTAMP "${modulePath}" previousModeTime "%s.%f")
    endif()
    # configureResult/Output/Error: configure exit codeとdiagnostics。
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${ENGINE_ROOT}/tools/ProjectGameModule"
            -B "${modeBuildDir}"
            -G "${TEST_GENERATOR}"
            ${generatorPlatformArguments}
            "-DCMAKE_BUILD_TYPE=${TEST_BUILD_TYPE}"
            "-DLAMAPON_MODULE_FAST_BUILD:BOOL=${fastBuild}"
            "-DLAMAPON_ENGINE_ROOT:PATH=${ENGINE_ROOT}"
            "-DLAMAPON_PROJECT_ROOT:PATH=${PROJECT_ROOT}"
            "-DLAMAPON_RUNTIME_DIR:PATH=${RUNTIME_DIR}"
            "-DLAMAPON_MODULE_OUTPUT_DIR:PATH=${modeOutputDir}"
            "-DLAMAPON_GENERATED_INCLUDE_DIR:PATH=${GENERATED_INCLUDE_DIR}"
        RESULT_VARIABLE configureResult
        OUTPUT_VARIABLE configureOutput
        ERROR_VARIABLE configureError
    )
    # configure失敗時は詳細を返します。
    if(NOT configureResult EQUAL 0)
        message(FATAL_ERROR
            "Project Game Module configure failed:\n${configureOutput}\n${configureError}"
        )
    endif()

    # buildResult/Output/Error: module build exit codeとdiagnostics。
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            --build "${modeBuildDir}"
            --config "${TEST_BUILD_TYPE}"
            --target LamaPonGameModule --parallel 2
        RESULT_VARIABLE buildResult
        OUTPUT_VARIABLE buildOutput
        ERROR_VARIABLE buildError
    )
    # build失敗時は詳細を返します。
    if(NOT buildResult EQUAL 0)
        message(FATAL_ERROR
            "Project Game Module build failed:\n${buildOutput}\n${buildError}"
        )
    endif()

    # 他モードへの切り替えで再コンパイル・再リンクしないことを確認します。
    if(seen_${fastBuild})
        # reusedModeTime: 再利用後のDLL時刻。
        file(TIMESTAMP "${modulePath}" reusedModeTime "%s.%f")
        if(NOT previousModeTime STREQUAL reusedModeTime)
            message(FATAL_ERROR "Switching modes rebuilt an unchanged cached module.")
        endif()
    endif()
    set(seen_${fastBuild} TRUE)
    # 選択モードのDLLを既定配置先へ戻し、キャッシュとの一致を確認します。
    file(COPY_FILE "${modulePath}" "${TEST_OUTPUT_DIR}/LamaPonGameModule.dll" ONLY_IF_DIFFERENT)
    # cachedHash/deployedHash: キャッシュ・配置後の内容。
    file(SHA256 "${modulePath}" cachedHash)
    file(SHA256 "${TEST_OUTPUT_DIR}/LamaPonGameModule.dll" deployedHash)
    if(NOT cachedHash STREQUAL deployedHash)
        message(FATAL_ERROR "The deployed module does not match the selected mode.")
    endif()
    # module artifactの有無を確認します。
    if(NOT EXISTS "${modulePath}")
        message(FATAL_ERROR "Project Game Module DLL was not generated.")
    endif()

    # loadResult/Output/Error: module load probeの終了codeとdiagnostics。
    execute_process(
        COMMAND "${TEST_LOADER}" "${TEST_OUTPUT_DIR}/LamaPonGameModule.dll"
            "${ENGINE_ROOT}/packages/src/scene-transition-showcase"
            "${ENGINE_ROOT}/packages/src/network-session-workflow"
            "${ENGINE_ROOT}/packages/src/ollama-ai"
        RESULT_VARIABLE loadResult
        OUTPUT_VARIABLE loadOutput
        ERROR_VARIABLE loadError
    )
    # moduleのload失敗を報告します。
    if(NOT loadResult EQUAL 0)
        message(FATAL_ERROR
            "Project Game Module load failed:\n${loadOutput}\n${loadError}"
        )
    endif()

    message(STATUS "${loadOutput}")

    # 変更のない二回目のビルドはDLLを更新しないことを確認します。
    # moduleTimeBefore: 再ビルド前のDLL時刻。
    file(TIMESTAMP "${modulePath}" moduleTimeBefore "%s.%f")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${modeBuildDir}"
            --config "${TEST_BUILD_TYPE}"
            --target LamaPonGameModule --parallel 2
        RESULT_VARIABLE buildResult
        OUTPUT_VARIABLE buildOutput
        ERROR_VARIABLE buildError)
    if(NOT buildResult EQUAL 0)
        message(FATAL_ERROR "Repeated build failed:
    ${buildOutput}
    ${buildError}")
    endif()
    # moduleTimeAfter: 再ビルド後のDLL時刻。
    file(TIMESTAMP "${modulePath}" moduleTimeAfter "%s.%f")
    if(NOT moduleTimeBefore STREQUAL moduleTimeAfter)
        message(FATAL_ERROR
            "An unchanged Game Module was relinked "
            "(fastBuild=${fastBuild}, "
            "before=${moduleTimeBefore}, after=${moduleTimeAfter}).\n"
            "Build output:\n${buildOutput}\n${buildError}"
        )
    endif()
endforeach()
