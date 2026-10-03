#include "LamaPon/LamaPon.h"

#include "InterfaceProbe.h"

#include <nlohmann/json.hpp>

namespace
{
    // インターフェースを実装する側。
    class DamageableProbe final
        : public SampleGame::IDamageable
        , public LamaPon::Script
    {
    public:
        // Interface経由で受けたダメージを残りHPから引く(amount: 減らすHP)。
        void ApplyDamage(const int amount) override
        {
            m_health -= amount;
        }

        // 検査用に現在の残りHPを返す。
        [[nodiscard]] int RemainingHealth() const override
        {
            return m_health;
        }

        // 検査側で読めるよう残りHPをJSONで保存する。
        [[nodiscard]] std::string SaveProperties() const override
        {
            return nlohmann::json{
                { "health", m_health }
            }.dump();
        }

        // JSON Objectのhealthがあれば残りHPへ戻す(json: 読み込む保存JSON)。
        void LoadProperties(
            const std::string_view json) override
        {
            // 例外を出さず解析した保存JSON
            const auto document = nlohmann::json::parse(
                json,
                nullptr,
                false);
            if (document.is_object())
            {
                m_health = document.value("health", m_health);
            }
        }

    private:
        // ダメージ後の残りHP
        int m_health{ 100 };
    };

    // 具体的なScript型を使わずInterface経由でダメージを付与する。
    class DamageDealerProbe final : public LamaPon::Script
    {
    public:
        // 同じObjectのIDamageableへ25ダメージを与え成功を記録する。
        void Start() override
        {
            // 同じObjectから取得する借用Interface
            if (auto* target =
                GetScript<SampleGame::IDamageable>())
            {
                target->ApplyDamage(25);
                m_dealt = true;
            }
        }

        // ダメージ付与の成功状態を検査用のJSONで保存する。
        [[nodiscard]] std::string SaveProperties() const override
        {
            return nlohmann::json{
                { "dealt", m_dealt }
            }.dump();
        }

    private:
        // 起動時にダメージ付与できたか
        bool m_dealt{};
    };
}

LAMAPON_SCRIPT_NAMED(
    DamageableProbe,
    "Sample.DamageableProbe",
    "被ダメージ確認用");

LAMAPON_SCRIPT_NAMED(
    DamageDealerProbe,
    "Sample.DamageDealerProbe",
    "ダメージ付与確認用");
