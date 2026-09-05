#pragma once

#include <cstddef>
#include <cstdint>

namespace LamaPon
{
    class GameObject;
    class GraphicsDevice;
    class Script;
    struct CollisionEvent;

    // Game ModuleとRuntimeのABIを識別します。公開構造体のレイアウトや
    // 公開関数のシグネチャを変更した場合は、この値も更新してください。
    // GameModuleHostは完全一致を要求し、互換性のないDLLを読み込みません。
    // API 16ではSceneの物理時計を専用クラスへ移し、Sceneのレイアウトが
    // 変わったため、ゲーム用DLLの再ビルドが必要です。
    inline constexpr std::uint32_t GameModuleApiVersion = 16;

    using NativeScriptCreateFunction = void* (*)(
        GameObject* owner,
        GraphicsDevice* graphics,
        const char* propertiesJson);
    using NativeScriptDestroyFunction = void (*)(void* instance);
    using NativeScriptUpdateFunction = void (*)(
        void* instance,
        float deltaTime);
    using NativeScriptCollisionFunction = void (*)(
        void* instance,
        const CollisionEvent* event);
    using NativeScriptSerializeFunction = const char* (*)(
        void* instance);
    using NativeScriptStartFunction = void (*)(void* instance);
    // instanceをScript*へ上位変換します。呼び出し側でこれをやると、
    // instanceがvoid*で型を失っているためポインタ調整が入らず、
    // Scriptを先頭以外に継承したスクリプト（インターフェースとの
    // 多重継承）で静かに壊れます。具体型が分かるGame Module側で
    // 変換させるための関数です。
    using NativeScriptAsScriptFunction = Script* (*)(void* instance);
    using NativeScriptSetActiveFunction = void (*)(
        void* instance,
        bool active);

    struct NativeScriptTypeDescriptor final
    {
        const char* typeName{};
        const char* displayName{};
        NativeScriptCreateFunction create{};
        NativeScriptDestroyFunction destroy{};
        NativeScriptUpdateFunction update{};
        NativeScriptCollisionFunction collisionEnter{};
        NativeScriptCollisionFunction collisionStay{};
        NativeScriptCollisionFunction collisionExit{};
        NativeScriptSerializeFunction serialize{};
        // 任意: プロジェクトで設定した固定時間刻みごとに呼ばれます。
        NativeScriptUpdateFunction fixedUpdate{};
        // 任意: Inspectorへ型付きフィールドを表示するJSONスキーマ。
        const char* propertiesSchemaJson{};
        // 任意: 生成後、最初のUpdateの直前に一度だけ呼ばれます。
        NativeScriptStartFunction start{};
        // 任意: 毎フレーム、Update・固定更新・物理計算の後に呼ばれます。
        NativeScriptUpdateFunction lateUpdate{};
        // 任意: 実効アクティブ状態の遷移（OnEnable/OnDisable）。
        NativeScriptSetActiveFunction setActive{};
        // 任意: トリガー接触はCollisionコールバックの代わりに
        // こちらへ届きます。
        NativeScriptCollisionFunction triggerEnter{};
        NativeScriptCollisionFunction triggerStay{};
        NativeScriptCollisionFunction triggerExit{};
        // 任意: instanceをScript*として取り出します。これがあると
        // GameObject::GetScript<T>()で自作インターフェースを引けます。
        // この変換を登録しない型ではnullptrになるため、呼び出し側で確認します。
        NativeScriptAsScriptFunction asScript{};
    };

    // データアセット（*.asset.json）の型宣言です。値そのものは
    // 持たず、「どんなフィールドがあるか」だけをエディターへ
    // 伝えます。読み書きはエンジン側（DataAsset）が行うため、
    // Hot Reloadで型が消えてもゲーム中のデータは無効になりません。
    struct NativeDataAssetTypeDescriptor final
    {
        // 例: "Game.CardData"。*.asset.jsonの"type"と対応します。
        const char* typeName{};
        // 「新規データアセット」メニューへ出す表示名。
        const char* displayName{};
        // Inspectorへ型付きの入力欄を出すためのJSONスキーマ。
        // NativeScriptTypeDescriptor::propertiesSchemaJsonと
        // 同じ書式です。
        const char* schemaJson{};
    };

    struct GameModuleDescriptor final
    {
        std::uint32_t apiVersion{};
        const char* moduleName{};
        std::size_t componentCount{};
        const NativeScriptTypeDescriptor* components{};
        // データアセット型の数。未登録の場合は0です。
        std::size_t dataAssetCount{};
        const NativeDataAssetTypeDescriptor* dataAssets{};
    };

    using GetGameModuleDescriptorFunction =
        const GameModuleDescriptor* (*)();
}

#define LAMAPON_GAME_MODULE_EXPORT \
    extern "C" __declspec(dllexport) \
    const LamaPon::GameModuleDescriptor* \
    LamaPonGetGameModule()
