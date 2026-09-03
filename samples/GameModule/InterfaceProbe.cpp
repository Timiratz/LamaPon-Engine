// GetScript<T>()の検証用スクリプト2本。
//
// 継承順が要点です。LamaPon::Scriptを「先頭以外」に書くことで、
// Script*への上位変換にポインタ調整が必要な配置を意図的に作ります。
// instanceをvoid*からstatic_cast<Script*>すると壊れる並びなので、
// descriptorのasScript経由の変換が効いているかを試せます。
//
// 呼び出しはGame Module内から行います。利用者の実際の書き方と同じで、
// dynamic_castも同一モジュール内で完結します。
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
        void ApplyDamage(const int amount) override
        {
            m_health -= amount;
        }

        [[nodiscard]] int RemainingHealth() const override
        {
            return m_health;
        }

        // 残りHPをテストから読めるようにします。
        [[nodiscard]] std::string SaveProperties() const override
        {
            return nlohmann::json{
                { "health", m_health }
            }.dump();
        }

        void LoadProperties(
            const std::string_view json) override
        {
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
        int m_health{ 100 };
    };

    // インターフェース経由で相手を叩く側。型を知らずに
    // GetScript<IDamageable>()で引きます。
    class DamageDealerProbe final : public LamaPon::Script
    {
    public:
        void Start() override
        {
            if (auto* target =
                GetScript<SampleGame::IDamageable>())
            {
                target->ApplyDamage(25);
                m_dealt = true;
            }
        }

        [[nodiscard]] std::string SaveProperties() const override
        {
            return nlohmann::json{
                { "dealt", m_dealt }
            }.dump();
        }

    private:
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
