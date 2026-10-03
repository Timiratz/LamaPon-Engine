# LAMAPON_DIRECTXTK_ROOT: DirectXTK binary packageのroot.
set(
    LAMAPON_DIRECTXTK_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/DirectXTK"
    CACHE PATH
    "Path to a DirectXTK binary package containing include/ and native/lib/."
)
# LAMAPON_XAUDIO2_REDIST_ROOT: XAudio2 Redistributableのroot.
set(
    LAMAPON_XAUDIO2_REDIST_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/XAudio2Redist"
    CACHE PATH
    "Path to the Microsoft XAudio2 Redistributable package."
)

# pointer幅に対応するlibrary architectureを選びます。
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # _lamapon_directxtk_arch: 64-bit targetのlibrary architecture。
    set(_lamapon_directxtk_arch x64)
# 32-bit targetにはx86 libraryを選びます。
else()
    # _lamapon_directxtk_arch: 32-bit targetのlibrary architecture。
    set(_lamapon_directxtk_arch x86)
endif()

# _lamapon_directxtk_include: DirectXTK header directory.
set(_lamapon_directxtk_include "${LAMAPON_DIRECTXTK_ROOT}/include")
# _lamapon_directxtk_lib_root: architecture別library root.
set(_lamapon_directxtk_lib_root "${LAMAPON_DIRECTXTK_ROOT}/native/lib/${_lamapon_directxtk_arch}")
# _lamapon_xaudio2_include: XAudio2 header directory.
set(_lamapon_xaudio2_include "${LAMAPON_XAUDIO2_REDIST_ROOT}/include")
# _lamapon_xaudio2_release_root: Release file root.
set(_lamapon_xaudio2_release_root "${LAMAPON_XAUDIO2_REDIST_ROOT}/release")
# _lamapon_xaudio2_debug_root: Debug file root.
set(_lamapon_xaudio2_debug_root "${LAMAPON_XAUDIO2_REDIST_ROOT}/debug")
# LAMAPON_XAUDIO2_REDIST_DLL: 配布するXAudio2 runtime DLL path.
set(LAMAPON_XAUDIO2_REDIST_DLL
    "${_lamapon_xaudio2_release_root}/bin/${_lamapon_directxtk_arch}/xaudio2_9redist.dll"
)

# headerと全構成のlibraryが揃った場合だけ登録します。
if(
    EXISTS "${_lamapon_directxtk_include}/SpriteBatch.h"
    AND EXISTS "${_lamapon_directxtk_lib_root}/Debug/DirectXTK.lib"
    AND EXISTS "${_lamapon_directxtk_lib_root}/Release/DirectXTK.lib"
    AND EXISTS "${_lamapon_directxtk_lib_root}/Debug/DirectXTKAudioWin7.lib"
    AND EXISTS "${_lamapon_directxtk_lib_root}/Release/DirectXTKAudioWin7.lib"
)
    add_library(LamaPonDirectXTKBinary STATIC IMPORTED GLOBAL)
    set_target_properties(LamaPonDirectXTKBinary PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
        IMPORTED_LOCATION_DEBUG "${_lamapon_directxtk_lib_root}/Debug/DirectXTK.lib"
        IMPORTED_LOCATION_RELEASE "${_lamapon_directxtk_lib_root}/Release/DirectXTK.lib"
        IMPORTED_LOCATION_RELWITHDEBINFO "${_lamapon_directxtk_lib_root}/Release/DirectXTK.lib"
        IMPORTED_LOCATION_MINSIZEREL "${_lamapon_directxtk_lib_root}/Release/DirectXTK.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_lamapon_directxtk_include}"
    )

    add_library(LamaPon::DirectXTK ALIAS LamaPonDirectXTKBinary)

    add_library(LamaPonDirectXTKAudioBinary STATIC IMPORTED GLOBAL)
    set_target_properties(LamaPonDirectXTKAudioBinary PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
        IMPORTED_LOCATION_DEBUG "${_lamapon_directxtk_lib_root}/Debug/DirectXTKAudioWin7.lib"
        IMPORTED_LOCATION_RELEASE "${_lamapon_directxtk_lib_root}/Release/DirectXTKAudioWin7.lib"
        IMPORTED_LOCATION_RELWITHDEBINFO "${_lamapon_directxtk_lib_root}/Release/DirectXTKAudioWin7.lib"
        IMPORTED_LOCATION_MINSIZEREL "${_lamapon_directxtk_lib_root}/Release/DirectXTKAudioWin7.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_lamapon_directxtk_include}"
    )
    add_library(LamaPon::DirectXTKAudio ALIAS LamaPonDirectXTKAudioBinary)
    # 必要なheaderまたはlibraryがない場合は構成を拒否します。
else()
    message(FATAL_ERROR
        "DirectXTK was not found at:\n"
        "  ${LAMAPON_DIRECTXTK_ROOT}\n"
        "Expected headers plus DirectXTK.lib and DirectXTKAudioWin7.lib."
    )
endif()

# header・library・runtime DLLが揃った場合だけ登録します。
if(
    EXISTS "${_lamapon_xaudio2_include}/xaudio2.h"
    AND EXISTS "${_lamapon_xaudio2_release_root}/lib/${_lamapon_directxtk_arch}/xaudio2_9redist.lib"
    AND EXISTS "${_lamapon_xaudio2_debug_root}/lib/${_lamapon_directxtk_arch}/xapobaseredist_md.lib"
    AND EXISTS "${_lamapon_xaudio2_release_root}/lib/${_lamapon_directxtk_arch}/xapobaseredist_md.lib"
    AND EXISTS "${LAMAPON_XAUDIO2_REDIST_DLL}"
)
    add_library(LamaPonXAudio2Redist INTERFACE)
    target_include_directories(LamaPonXAudio2Redist INTERFACE
        "${_lamapon_xaudio2_include}"
    )
    target_compile_definitions(LamaPonXAudio2Redist INTERFACE
        USING_XAUDIO2_REDIST
    )
    target_link_libraries(LamaPonXAudio2Redist INTERFACE
        "${_lamapon_xaudio2_release_root}/lib/${_lamapon_directxtk_arch}/xaudio2_9redist.lib"
        "$<$<CONFIG:Debug>:${_lamapon_xaudio2_debug_root}/lib/${_lamapon_directxtk_arch}/xapobaseredist_md.lib>"
        "$<$<NOT:$<CONFIG:Debug>>:${_lamapon_xaudio2_release_root}/lib/${_lamapon_directxtk_arch}/xapobaseredist_md.lib>"
    )
    add_library(LamaPon::XAudio2Redist ALIAS LamaPonXAudio2Redist)
    # 必要なheader・library・DLLがない場合は構成を拒否します。
else()
    message(FATAL_ERROR
        "Microsoft XAudio2 Redistributable was not found at:\n"
        "  ${LAMAPON_XAUDIO2_REDIST_ROOT}"
    )
endif()
