#pragma once

#include <cstddef>
#include <cstdint>

namespace LamaPon
{
    class GameObject;
    class GraphicsDevice;
    class Script;
    struct CollisionEvent;

    // PhysicsSettingsにlayerNames（std::string×32）と
    // collisionMatrixを足したため6へ上げます。
    //
    // 上げる基準は「NativeScriptTypeDescriptorが変わったとき」だけ
    // ではありません。**Game Moduleから見える構造体のレイアウトが
    // 変わったら上げます**。Game Moduleはエンジンのヘッダを直接
    // includeしてLamaPonRuntime.libとリンクするので、古いDLLは
    // 古いsizeofのまま動きます。値渡し・参照渡しでその構造体を
    // runtime側へ渡すと、runtimeは新しいレイアウトで読み、
    // 足したぶんが未初期化のゴミになります。POD同士なら変な数値で
    // 済みましたが、std::stringが入った今はゴミのポインタを
    // 解放しにいって落ちます（例: 古いDLLからの
    // SetActivePhysicsSettings）。
    //
    // 5はasScriptの追加で上げたものです。NativeScriptTypeDescriptorは
    // 配列で受け渡すため、フィールドを足すと要素サイズが変わり、
    // 古いモジュールでは要素境界がずれて別のフィールドを読みます。
    // GameModuleHostがapiVersionの完全一致を要求して古いDLLを
    // 弾くので、どちらの状態にもなりません（保存時の自動ビルドで
    // 作り直されます）。
    // 8はMeshRendererComponentへProcedural Mesh用の公開APIと保持
    // データを追加したときに上げました。Game Module側がComponentを
    // 生成するため、クラスの大きさが違う古いDLLは混在できません。
    // 9はMeshRendererComponentへカリングモードの上書きを追加した
    // ときに上げました。古いDLLとはComponentのサイズが異なります。
    // 10はAudioSourceComponentへ任意区間ループの保持データを
    // 追加したときに上げました。これを忘れていた間、古いDLLが
    // 弾かれずに読み込まれ、エディターがログを1行も書かないまま
    // アクセス違反で落ちていました。
    //
    // 7はAssetManager::LoadTextureへ用途（TextureUsage）の引数を
    // 足したときに上げました。既定値付きなので書き換えは不要ですが、
    // 既定引数は呼ぶ側で埋まる＝エクスポート名が変わるので、古い
    // DLLはこの関数を解決できません。構造体のレイアウトだけでなく、
    // **公開関数のシグネチャを変えたときも上げます**。
    //
    // 11はUIButtonComponentへ円形当たり判定フラグ
    // （m_circularHitArea）を足してsizeofが変わったときに上げました。
    // 12はデータアセット（ScriptableObject相当）を足したときに
    // 上げました。GameModuleDescriptorの末尾へ2フィールド増えた
    // ため、古いDLLのディスクリプタを新しいレイアウトで読むと
    // 存在しない配列を辿ってしまいます。AssetManagerにも
    // データアセットのキャッシュが増えてsizeofが変わりました。
    inline constexpr std::uint32_t GameModuleApiVersion = 14;

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
        // Optional 60 Hz callback for physics and continuous forces.
        NativeScriptUpdateFunction fixedUpdate{};
        // Optional JSON schema used to draw typed fields in the Inspector.
        const char* propertiesSchemaJson{};
        // 任意: 生成後、最初のUpdateの直前に一度だけ呼ばれます。
        NativeScriptStartFunction start{};
        // 任意: 毎フレーム、全ComponentのUpdate後に呼ばれます。
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
        // 古いGame Moduleではnullptrになるため、呼び出し側で確認します。
        NativeScriptAsScriptFunction asScript{};
    };

    // データアセット（`*.asset.json`）の型宣言です。値そのものは
    // 持たず、「どんなフィールドがあるか」だけをエディターへ
    // 伝えます。読み書きはエンジン側（DataAsset）が行うため、
    // Hot Reloadで型が消えてもゲーム中のデータは無効になりません。
    struct NativeDataAssetTypeDescriptor final
    {
        // 例: "Game.CardData"。`*.asset.json`の"type"と対応します。
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
        // 任意: データアセットの型。古いモジュールは0のままです。
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
