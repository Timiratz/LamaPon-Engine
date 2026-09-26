# EOS SDKは公式配布ページから取得します。SDKや資格情報を
# リポジトリへ同梱せず、未設定でもLANバックエンドはビルドできます。
set(LAMAPON_EOS_SDK_ROOT "" CACHE PATH "EOS SDK root containing Include, Lib and Bin")

if(LAMAPON_EOS_SDK_ROOT)
    # SDKの指定先を変更したとき、以前の検索結果を再利用しません。
    unset(LAMAPON_EOS_INCLUDE CACHE)
    unset(LAMAPON_EOS_LIBRARY CACHE)
    unset(LAMAPON_EOS_RUNTIME CACHE)
    find_path(LAMAPON_EOS_INCLUDE eos_sdk.h
        PATHS "${LAMAPON_EOS_SDK_ROOT}/Include" "${LAMAPON_EOS_SDK_ROOT}/SDK/Include"
        NO_DEFAULT_PATH REQUIRED)
    find_library(LAMAPON_EOS_LIBRARY NAMES EOSSDK-Win64-Shipping
        PATHS "${LAMAPON_EOS_SDK_ROOT}/Lib" "${LAMAPON_EOS_SDK_ROOT}/SDK/Lib"
        NO_DEFAULT_PATH REQUIRED)
    find_file(LAMAPON_EOS_RUNTIME EOSSDK-Win64-Shipping.dll
        PATHS "${LAMAPON_EOS_SDK_ROOT}/Bin" "${LAMAPON_EOS_SDK_ROOT}/SDK/Bin"
        NO_DEFAULT_PATH REQUIRED)
endif()

function(lamapon_configure_network_backend target)
    if(NOT LAMAPON_EOS_SDK_ROOT)
        return()
    endif()
    target_sources(${target} PRIVATE src/LamaPon/Online/EpicNetworkTransportSdk.cpp)
    target_include_directories(${target} PRIVATE "${LAMAPON_EOS_INCLUDE}")
    target_compile_definitions(${target} PRIVATE LAMAPON_WITH_EOS)
    target_link_libraries(${target} PRIVATE "${LAMAPON_EOS_LIBRARY}" bcrypt)
    # EOSを使わないゲームは、任意SDKのDLLなしでも起動できます。
    if(MSVC)
        target_link_libraries(${target} PRIVATE delayimp)
        target_link_options(${target} PRIVATE "/DELAYLOAD:EOSSDK-Win64-Shipping.dll")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LAMAPON_EOS_RUNTIME}" "$<TARGET_FILE_DIR:${target}>/"
        VERBATIM)
endfunction()
