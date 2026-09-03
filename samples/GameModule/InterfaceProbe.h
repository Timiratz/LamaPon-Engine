#pragma once

// GetScript<T>()が自作インターフェースを引けることを確かめるための
// 最小のインターフェースです。Scriptを継承していない純粋な抽象基底で、
// スクリプト側はScriptとの多重継承で実装します。
namespace SampleGame
{
    struct IDamageable
    {
        virtual ~IDamageable() = default;
        virtual void ApplyDamage(int amount) = 0;
        [[nodiscard]] virtual int RemainingHealth() const = 0;
    };
}
