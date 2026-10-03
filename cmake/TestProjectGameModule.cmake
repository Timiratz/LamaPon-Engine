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

file(REMOVE_RECURSE "${TEST_BUILD_DIR}" "${TEST_OUTPUT_DIR}")

# configureResult/Output/Error: configure exit codeとdiagnostics。
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${ENGINE_ROOT}/tools/ProjectGameModule"
        -B "${TEST_BUILD_DIR}"
        -G "${TEST_GENERATOR}"
        "-DCMAKE_BUILD_TYPE=${TEST_BUILD_TYPE}"
        "-DLAMAPON_ENGINE_ROOT:PATH=${ENGINE_ROOT}"
        "-DLAMAPON_PROJECT_ROOT:PATH=${PROJECT_ROOT}"
        "-DLAMAPON_RUNTIME_DIR:PATH=${RUNTIME_DIR}"
        "-DLAMAPON_MODULE_OUTPUT_DIR:PATH=${TEST_OUTPUT_DIR}"
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
        --build "${TEST_BUILD_DIR}"
        --target LamaPonGameModule
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

# modulePath: build artifactのexpected DLL path。
set(modulePath "${TEST_OUTPUT_DIR}/LamaPonGameModule.dll")
# module artifactの有無を確認します。
if(NOT EXISTS "${modulePath}")
    message(FATAL_ERROR "Project Game Module DLL was not generated.")
endif()

# loadResult/Output/Error: module load probeの終了codeとdiagnostics。
execute_process(
    COMMAND "${TEST_LOADER}" "${modulePath}"
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
