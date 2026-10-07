#pragma once

#include "LamaPon/Scene/Component.h"

#include <string>
#include <string_view>

namespace LamaPon
{
    class GameModuleHost;
    struct NativeScriptTypeDescriptor;

    // Game Moduleの実体を所有し、更新コールバックの失敗後は再生成まで通常の更新・接触通知を抑止する。
    class NativeScriptComponent final : public Component
    {
    public:
        // Game Moduleの登録型とプロパティを持つスクリプト部品を作る(scriptType: 1〜128バイトの登録型名, propertiesJson: 64KiB以下のJSONオブジェクト)。
        explicit NativeScriptComponent(
            std::string scriptType,
            std::string propertiesJson = "{}");
        // 実体を無効化・破棄してホスト登録を解除する。
        ~NativeScriptComponent() override;

        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "NativeScript";
        }
        // Game Moduleに登録された型名を取得する。
        [[nodiscard]] const std::string&
            ScriptType() const noexcept
        {
            return m_scriptType;
        }
        // 登録済みの表示名を取得し、表示名がなければ型名を返す。
        [[nodiscard]] std::string DisplayName() const;
        // 生成時に渡す保存済みのプロパティJSONを取得する。
        [[nodiscard]] const std::string&
            PropertiesJson() const noexcept
        {
            return m_propertiesJson;
        }
        // 実体から有効なJSONを取得し、未生成や失敗時は保存済みのJSONを返す。
        [[nodiscard]] std::string SerializedProperties() const;
        // 登録型のプロパティスキーマを借用し、取得できなければ空を返す。
        // 返す参照はGame Moduleの再読み込みや解放をまたいで保持しない。
        [[nodiscard]] std::string_view
            PropertiesSchemaJson() const noexcept;
        // JSONを検証して旧実体を破棄し、新しいプロパティで再生成する(propertiesJson: 64KiB以下のJSONオブジェクト)。
        void SetPropertiesJson(std::string propertiesJson);
        // Apply an opted-in script's values without recreating its instance or calling Start.
        // LoadProperties must validate before modifying script state.
        void ApplyPropertiesJsonLive(std::string propertiesJson);

        // 登録型がホストに見つかるか確認する。
        [[nodiscard]] bool IsResolved() const noexcept;
        // 実体をGame Module側で基底Scriptへ変換し、未生成や変換未対応ならnullptrを返す。
        // 返すポインターはプロパティ変更やGame Moduleの再読み込み・解放をまたいで保持しない。
        [[nodiscard]] Script* ScriptInstance() const noexcept override;
        // 最後の生成失敗やコールバック失敗の説明を取得する。
        [[nodiscard]] const std::string&
            LastError() const noexcept
        {
            return m_lastError;
        }

    protected:
        // 描画機器を借用し、現在のホストに登録して実体を生成する(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 実体を確保し、初回のStartと通常更新を呼ぶ(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 失敗が記録されていない実体へ後段更新を渡す(deltaTime: 経過秒数)。
        void OnLateUpdate(float deltaTime) override;
        // 実体を確保して固定更新を呼び、Startは通常更新まで呼ばない(fixedDeltaTime: 固定更新の経過秒数)。
        void OnFixedUpdate(float fixedDeltaTime) override;
        // 実体へ衝突開始を渡す(event: 衝突相手と接触情報)。
        void OnCollisionEnter(const CollisionEvent& event) override;
        // 実体へ継続中の衝突を渡す(event: 衝突相手と接触情報)。
        void OnCollisionStay(const CollisionEvent& event) override;
        // 実体へ衝突終了を渡す(event: 衝突相手と接触情報)。
        void OnCollisionExit(const CollisionEvent& event) override;
        // 実体へトリガー進入を渡す(event: トリガー相手と接触情報)。
        void OnTriggerEnter(const CollisionEvent& event) override;
        // 実体へトリガー内の継続接触を渡す(event: トリガー相手と接触情報)。
        void OnTriggerStay(const CollisionEvent& event) override;
        // 実体へトリガー退出を渡す(event: トリガー相手と接触情報)。
        void OnTriggerExit(const CollisionEvent& event) override;
        // 実体の有効状態を変更して通知する(active: 新しい有効状態)。
        void OnActiveStateChanged(bool active) override;

    private:
        friend class GameModuleHost;

        // 非空で64KiB以下のJSONオブジェクトか検証し、不正なら例外を返す(propertiesJson: 検証するJSON文字列)。
        static void ValidateProperties(
            std::string_view propertiesJson);
        // 描画機器と登録型が揃えば実体を生成し、生成失敗をLastErrorに残す。
        void EnsureInstance();
        // 実体を無効化して破棄を呼び、診断失敗も含め例外を外へ出さない。
        void DestroyInstance() noexcept;
        // Game Moduleの解放前にプロパティを保存して実体を破棄する。
        void BeforeModuleUnload();
        // Game Moduleの読み込み後に実体を再生成する。
        void AfterModuleLoad();
        // 有効状態が変わった実体へ有効化・無効化を通知する(active: 通知する有効状態)。
        void NotifyInstanceActive(bool active) noexcept;

        // Game Moduleの登録型名
        std::string m_scriptType;
        // 生成に使うプロパティJSON
        std::string m_propertiesJson;
        // 最後に記録した失敗の説明
        std::string m_lastError;
        // 借用した描画機器
        GraphicsDevice* m_graphics{};
        // 登録先のGame Moduleホスト
        GameModuleHost* m_host{};
        // 読込中のモジュールの型情報
        const NativeScriptTypeDescriptor* m_descriptor{};
        // 所有するスクリプト実体
        void* m_instance{};
        // Startの呼び出し完了状態
        bool m_started{};
        // 実体へ通知済みの有効状態
        bool m_instanceActive{};
    };
}
