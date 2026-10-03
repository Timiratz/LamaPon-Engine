#pragma once

#include <cstddef>
#include <cstdint>

namespace LamaPon
{
    class GameObject;
    class GraphicsDevice;
    class Script;
    struct CollisionEvent;

    // RuntimeとDLLの一致必須版
    inline constexpr std::uint32_t GameModuleApiVersion = 80;

    // DLL側でスクリプトを生成する(owner: 存続が必要な所有物体, graphics: 借用する描画サービス, propertiesJson: 初期設定JSON)。
    using NativeScriptCreateFunction = void* (*)(
        GameObject* owner,
        GraphicsDevice* graphics,
        const char* propertiesJson);
    // 生成元DLL側で破棄する(instance: 同じ型の生成済みインスタンス)。
    using NativeScriptDestroyFunction = void (*)(void* instance);
    // ゲーム時間で更新する(instance: 更新対象, deltaTime: 今回の経過秒数)。
    using NativeScriptUpdateFunction = void (*)(
        void* instance,
        float deltaTime);
    // 接触を同期通知する(instance: 通知対象, event: 通知中だけ有効な接触情報)。
    using NativeScriptCollisionFunction = void (*)(
        void* instance,
        const CollisionEvent* event);
    // 現状態をJSON文字列で返す(instance: 保存対象)。
    using NativeScriptSerializeFunction = const char* (*)(
        void* instance);
    // 最初の更新前に一度だけ開始する(instance: 開始対象)。
    using NativeScriptStartFunction = void (*)(void* instance);
    // 多重継承のポインター調整は具体型を知るDLL側で行う(instance: DLL側の具体型インスタンス)。
    using NativeScriptAsScriptFunction = Script* (*)(void* instance);
    // 実効アクティブ状態を反映する(instance: 対象, active: 有効状態)。
    using NativeScriptSetActiveFunction = void (*)(
        void* instance,
        bool active);

    struct NativeScriptTypeDescriptor final
    {
        // 登録を識別する型名
        const char* typeName{};
        // エディターの型表示名
        const char* displayName{};
        // 必須の生成処理
        NativeScriptCreateFunction create{};
        // 必須の破棄処理
        NativeScriptDestroyFunction destroy{};
        // 任意のフレーム更新
        NativeScriptUpdateFunction update{};
        // 接触開始の任意通知
        NativeScriptCollisionFunction collisionEnter{};
        // 接触継続の任意通知
        NativeScriptCollisionFunction collisionStay{};
        // 接触終了の任意通知
        NativeScriptCollisionFunction collisionExit{};
        // 任意の状態保存処理
        NativeScriptSerializeFunction serialize{};
        // 任意の固定刻み更新
        NativeScriptUpdateFunction fixedUpdate{};
        // 任意の型付き入力スキーマ
        const char* propertiesSchemaJson{};
        // 初回更新前の任意開始処理
        NativeScriptStartFunction start{};
        // 物理計算後の任意更新
        NativeScriptUpdateFunction lateUpdate{};
        // 有効状態遷移の任意通知
        NativeScriptSetActiveFunction setActive{};
        // トリガー開始の任意通知
        NativeScriptCollisionFunction triggerEnter{};
        // トリガー継続の任意通知
        NativeScriptCollisionFunction triggerStay{};
        // トリガー終了の任意通知
        NativeScriptCollisionFunction triggerExit{};
        // 未登録の型はGetScriptで基底・自作インターフェースを取得できない。
        // 具体型から基底への任意変換
        NativeScriptAsScriptFunction asScript{};
    };

    // 値の所有と読み書きはDataAssetに任せ、DLLには型の定義だけを持たせる。
    struct NativeDataAssetTypeDescriptor final
    {
        // JSONのtypeと対応する型名
        const char* typeName{};
        // 新規作成メニューの表示名
        const char* displayName{};
        // 型付き入力のJSONスキーマ
        const char* schemaJson{};
    };

    // 文字列・配列・関数をDLLに所有させ、読み込み中は存続させる。
    // 型登録は各256件までとし、型名・表示名は空白のみを除く1〜128バイトにする。
    struct GameModuleDescriptor final
    {
        // Runtimeと一致するABI版
        std::uint32_t apiVersion{};
        // DLLの表示名
        const char* moduleName{};
        // 登録スクリプト型の件数
        std::size_t componentCount{};
        // DLL所有のスクリプト型列
        const NativeScriptTypeDescriptor* components{};
        // 登録データ型の件数
        std::size_t dataAssetCount{};
        // DLL所有のデータ型列
        const NativeDataAssetTypeDescriptor* dataAssets{};
    };

    // DLLに存続する型記述を取得する。
    using GetGameModuleDescriptorFunction =
        const GameModuleDescriptor* (*)();
}

#define LAMAPON_GAME_MODULE_EXPORT \
    extern "C" __declspec(dllexport) \
    const LamaPon::GameModuleDescriptor* \
    LamaPonGetGameModule()
