# 原文のlicense群を配布物へ複製します。
# LAMAPON_DISTRIBUTION_DIR: license・noticeのstaging directory.
set(LAMAPON_DISTRIBUTION_DIR "${CMAKE_CURRENT_BINARY_DIR}/distribution")
file(MAKE_DIRECTORY "${LAMAPON_DISTRIBUTION_DIR}/licenses")
# lamapon_bundle_license(name: 出力名, source: 元file): licenseをdistributionへ複製します。
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
lamapon_bundle_license(ProggyClean third_party/imgui/misc/fonts/ProggyClean.LICENSE.txt)
configure_file(THIRD_PARTY_NOTICES.md
    "${LAMAPON_DISTRIBUTION_DIR}/THIRD_PARTY_NOTICES.md" COPYONLY)

# Runtime横へ通知文とlicenseだけ配置します。
add_custom_target(LamaPonDistributionFiles ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${LAMAPON_DISTRIBUTION_DIR}/licenses"
        "$<TARGET_FILE_DIR:LamaPonRuntime>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LAMAPON_DISTRIBUTION_DIR}/THIRD_PARTY_NOTICES.md"
        "$<TARGET_FILE_DIR:LamaPonRuntime>/THIRD_PARTY_NOTICES.md"
    VERBATIM)
add_dependencies(LamaPonRuntime LamaPonDistributionFiles)
install(DIRECTORY "${LAMAPON_DISTRIBUTION_DIR}/licenses/"
    DESTINATION "licenses")
install(FILES "${LAMAPON_DISTRIBUTION_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION ".")

# WebソースをSDKへ同梱し、Emscriptenは利用者のSDKを使います。
install(FILES tools/export_web.py tools/editor_web_export.py tools/embed_web_assets.py
    tools/export_native.py tools/build_native.py tools/editor_native_export.py
    tools/editor_linux_export.py tools/editor_linux_build.py tools/native_android.py
    tools/native_windows.py tools/native_linux.py DESTINATION tools)
install(FILES cmake/LamaPonWeb.cmake cmake/LamaPonNative.cmake DESTINATION cmake)
install(DIRECTORY src/LamaPon/Web src/LamaPon/Portable src/LamaPon/Native
    DESTINATION src/LamaPon)
install(FILES third_party/cgltf/cgltf.h third_party/cgltf/LICENSE.txt DESTINATION third_party/cgltf)
install(FILES third_party/nlohmann/LICENSE.MIT DESTINATION third_party/nlohmann)
install(FILES third_party/stb/stb_vorbis.c third_party/stb/LICENSE.txt DESTINATION third_party/stb)
install(FILES third_party/imgui/imstb_truetype.h third_party/imgui/LICENSE.txt DESTINATION third_party/imgui)
install(FILES third_party/imgui/misc/fonts/ProggyClean.ttf
    third_party/imgui/misc/fonts/ProggyClean.LICENSE.txt DESTINATION third_party/imgui/misc/fonts)
