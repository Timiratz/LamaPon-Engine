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
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} is required.")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_BUILD_DIR}" "${TEST_OUTPUT_DIR}")

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
if(NOT configureResult EQUAL 0)
    message(FATAL_ERROR
        "Project Game Module configure failed:\n${configureOutput}\n${configureError}"
    )
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        --build "${TEST_BUILD_DIR}"
        --target LamaPonGameModule
    RESULT_VARIABLE buildResult
    OUTPUT_VARIABLE buildOutput
    ERROR_VARIABLE buildError
)
if(NOT buildResult EQUAL 0)
    message(FATAL_ERROR
        "Project Game Module build failed:\n${buildOutput}\n${buildError}"
    )
endif()

set(modulePath "${TEST_OUTPUT_DIR}/LamaPonGameModule.dll")
if(NOT EXISTS "${modulePath}")
    message(FATAL_ERROR "Project Game Module DLL was not generated.")
endif()

execute_process(
    COMMAND "${TEST_LOADER}" "${modulePath}"
    RESULT_VARIABLE loadResult
    OUTPUT_VARIABLE loadOutput
    ERROR_VARIABLE loadError
)
if(NOT loadResult EQUAL 0)
    message(FATAL_ERROR
        "Project Game Module load failed:\n${loadOutput}\n${loadError}"
    )
endif()

message(STATUS "${loadOutput}")
