#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define LAMAPON_EDITOR_EXTENSION_CALL __cdecl
#define LAMAPON_EDITOR_EXTENSION_EXPORT __declspec(dllexport)
#else
#define LAMAPON_EDITOR_EXTENSION_CALL
#define LAMAPON_EDITOR_EXTENSION_EXPORT
#endif

#define LAMAPON_EDITOR_EXTENSION_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LamaPonEditorUiApiV1
{
    /* ABI確認用の関数表サイズ */
    uint32_t structSize;
    /* ホスト所有のUI状態。各UI関数へ渡します。 */
    void* userData;
    /* UTF-8文字列を装飾せずに表示します。 */
    void (LAMAPON_EDITOR_EXTENSION_CALL* text)(
        void* userData,
        const char* text);
    /* ボタンを表示し、押されたフレームに非0を返します。 */
    int32_t (LAMAPON_EDITOR_EXTENSION_CALL* button)(
        void* userData,
        const char* label);
    /* 0または1の値を編集し、変更時は非0を返します。 */
    int32_t (LAMAPON_EDITOR_EXTENSION_CALL* checkbox)(
        void* userData,
        const char* label,
        int32_t* value);
    /* 範囲付き浮動小数を編集し、変更時は非0を返します。 */
    int32_t (LAMAPON_EDITOR_EXTENSION_CALL* sliderFloat)(
        void* userData,
        const char* label,
        float* value,
        float minimum,
        float maximum);
    /* 浮動小数を編集し、変更時は非0を返します。 */
    int32_t (LAMAPON_EDITOR_EXTENSION_CALL* inputFloat)(
        void* userData,
        const char* label,
        float* value);
    /* 水平の区切り線を表示します。 */
    void (LAMAPON_EDITOR_EXTENSION_CALL* separator)(void* userData);
} LamaPonEditorUiApiV1;

typedef struct LamaPonEditorExtensionHostApiV1
{
    /* ABI確認用の関数表サイズ */
    uint32_t structSize;
    /* ホスト側ABI版 */
    uint32_t abiVersion;
    /* ホスト所有の状態。ホスト関数へ渡します。 */
    void* userData;
    /* 診断を記録します。levelは0=情報、1=警告、2=エラーです。 */
    void (LAMAPON_EDITOR_EXTENSION_CALL* log)(
        void* userData,
        uint32_t level,
        const char* message);
    /* パネル描画中に使えるEditor UI関数 */
    const LamaPonEditorUiApiV1* ui;
} LamaPonEditorExtensionHostApiV1;

typedef struct LamaPonEditorPanelV1
{
    /* ABI確認用のパネル記述サイズ */
    uint32_t structSize;
    /* 一意なUTF-8パネルID */
    const char* id;
    /* Editorへ表示するUTF-8タイトル */
    const char* displayName;
    /* 任意のUTF-8ウィンドウメニュー分類 */
    const char* windowMenuGroup;
    /* LAMAPON_EDITOR_PANEL_*フラグ */
    uint32_t flags;
    /* 拡張所有のパネル状態 */
    void* userData;
    /* パネル内容を描画します。openは入出力とも0または1です。 */
    void (LAMAPON_EDITOR_EXTENSION_CALL* draw)(
        void* extensionUserData,
        void* panelUserData,
        const LamaPonEditorUiApiV1* ui,
        int32_t* open);
} LamaPonEditorPanelV1;

typedef struct LamaPonEditorExtensionV1
{
    /* ABI確認用の記述サイズ */
    uint32_t structSize;
    /* LAMAPON_EDITOR_EXTENSION_ABI_VERSIONと一致させます。 */
    uint32_t abiVersion;
    /* 一意なUTF-8拡張ID */
    const char* id;
    /* UTF-8拡張表示名 */
    const char* displayName;
    /* 拡張DLLが所有するパネル記述配列 */
    const LamaPonEditorPanelV1* panels;
    /* パネル記述数 */
    uint32_t panelCount;
    /* 拡張を初期化し、不透明な状態を出力します。 */
    int32_t (LAMAPON_EDITOR_EXTENSION_CALL* initialize)(
        const LamaPonEditorExtensionHostApiV1* host,
        void** extensionUserData);
    /* Editorの各フレームで拡張を更新します。 */
    void (LAMAPON_EDITOR_EXTENSION_CALL* update)(
        void* extensionUserData);
    /* 拡張所有のリソースを解放します。 */
    void (LAMAPON_EDITOR_EXTENSION_CALL* shutdown)(
        void* extensionUserData);
} LamaPonEditorExtensionV1;

/* DLLが所有する静的な拡張記述へのポインターを返します。 */
typedef const LamaPonEditorExtensionV1* (
    LAMAPON_EDITOR_EXTENSION_CALL* LamaPonGetEditorExtensionV1Function)(
        void);

#ifdef __cplusplus
}
#endif

#define LAMAPON_EDITOR_PANEL_DEFAULT_OPEN 1u
#define LAMAPON_EDITOR_PANEL_SHOW_IN_WINDOW_MENU 2u
