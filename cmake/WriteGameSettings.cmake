# 入力設定fileがない場合は処理を止めます。
if(NOT DEFINED INPUT OR NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Project settings were not found: ${INPUT}")
endif()

# 出力pathがない場合は処理を止めます。
if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "Game settings output path was not specified.")
endif()

# project_settings: 入力Project設定JSON.
file(READ "${INPUT}" project_settings)
# project_format: 入力JSONのformat識別子.
string(JSON project_format GET "${project_settings}" format)
# project_version: 入力JSONのschema version.
string(JSON project_version GET "${project_settings}" version)

# 対応するProject設定だけを受け付けます。
if(
    NOT project_format STREQUAL "LamaPonProject"
    OR NOT project_version EQUAL 1
)
    message(FATAL_ERROR "Unsupported project settings format: ${INPUT}")
endif()

# game_settings: Game用formatへ変換したJSON.
string(
    JSON
    game_settings
    SET
    "${project_settings}"
    format
    "\"LamaPonGame\""
)
file(WRITE "${OUTPUT}" "${game_settings}\n")
