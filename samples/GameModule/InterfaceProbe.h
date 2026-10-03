#pragma once

// Scriptとの多重継承で実装し、GetScriptによる自作Interface取得を検査する。
namespace SampleGame
{
    struct IDamageable
    {
        // Interfaceのポインターから派生Objectを安全に破棄する。
        virtual ~IDamageable() = default;
        // 指定HPのダメージを付与する(amount: 減らすHP)。
        virtual void ApplyDamage(int amount) = 0;
        // ダメージ付与後の残りHPを返す。
        [[nodiscard]] virtual int RemainingHealth() const = 0;
    };
}
