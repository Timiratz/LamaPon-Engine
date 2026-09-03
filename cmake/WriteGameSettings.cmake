if(NOT DEFINED INPUT OR NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Project settings were not found: ${INPUT}")
endif()

if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "Game settings output path was not specified.")
endif()

file(READ "${INPUT}" project_settings)
string(JSON project_format GET "${project_settings}" format)
string(JSON project_version GET "${project_settings}" version)

if(
    NOT project_format STREQUAL "LamaPonProject"
    OR NOT project_version EQUAL 1
)
    message(FATAL_ERROR "Unsupported project settings format: ${INPUT}")
endif()

string(
    JSON
    game_settings
    SET
    "${project_settings}"
    format
    "\"LamaPonGame\""
)
file(WRITE "${OUTPUT}" "${game_settings}\n")
