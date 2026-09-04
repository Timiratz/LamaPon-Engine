# 出典ファイルをそのまま複製し、ソースビルド・SDK・ゲーム書き出しで
# 同じライセンス一式を使います。本文をCMakeや通知文へ転記しません。
set(LAMAPON_DISTRIBUTION_DIR "${CMAKE_CURRENT_BINARY_DIR}/distribution")
file(MAKE_DIRECTORY "${LAMAPON_DISTRIBUTION_DIR}/licenses")
function(lamapon_bundle_license name source)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        "${LAMAPON_DISTRIBUTION_DIR}/licenses/${name}.txt" COPYONLY)
endfunction()
lamapon_bundle_license(LamaPon LICENSE)
lamapon_bundle_license(DirectXTK third_party/DirectXTK/LICENSE)
lamapon_bundle_license(imgui third_party/imgui/LICENSE.txt)
lamapon_bundle_license(ImGuizmo third_party/ImGuizmo/LICENSE)
lamapon_bundle_license(nlohmann-json third_party/nlohmann/LICENSE.MIT)
lamapon_bundle_license(XAudio2Redist third_party/XAudio2Redist/LICENSE.txt)
lamapon_bundle_license(cgltf third_party/cgltf/LICENSE.txt)
lamapon_bundle_license(ufbx third_party/ufbx/LICENSE.txt)
lamapon_bundle_license(stb-vorbis third_party/stb/LICENSE.txt)
configure_file(THIRD_PARTY_NOTICES.md
    "${LAMAPON_DISTRIBUTION_DIR}/THIRD_PARTY_NOTICES.md" COPYONLY)

# Runtimeの隣へ常に揃えます。通知だけの変更でも再リンクは不要です。
add_custom_target(LamaPonDistributionFiles ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${LAMAPON_DISTRIBUTION_DIR}" "$<TARGET_FILE_DIR:LamaPonRuntime>"
    VERBATIM)
add_dependencies(LamaPonRuntime LamaPonDistributionFiles)
install(DIRECTORY "${LAMAPON_DISTRIBUTION_DIR}/" DESTINATION ".")

# Webビルドは配布SDKだけで生成できるよう、C++ソースも同梱します。
# Emscripten自体は利用者のSDKを使います。
install(FILES tools/export_web.py tools/editor_web_export.py DESTINATION tools)
install(FILES cmake/LamaPonWeb.cmake DESTINATION cmake)
install(DIRECTORY src/LamaPon/Web src/LamaPon/Portable
    DESTINATION src/LamaPon)
install(FILES third_party/cgltf/cgltf.h DESTINATION third_party/cgltf)
