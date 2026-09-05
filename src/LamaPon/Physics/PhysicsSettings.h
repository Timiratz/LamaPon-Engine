#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <string>

namespace LamaPon
{
    // 衝突レイヤーの数。コライダーのLayer()（0〜31のビット位置）と
    // CollisionMask()（32ビット）に合わせて固定です。
    inline constexpr std::size_t CollisionLayerCount = 32;

    namespace Detail
    {
        // 「全レイヤーが互いに当たる」マトリクス。既定値にすると
        // マトリクスを触らない限り従来と同じ挙動になります。
        [[nodiscard]] constexpr
            std::array<std::uint32_t, CollisionLayerCount>
            AllLayersCollide() noexcept
        {
            std::array<std::uint32_t, CollisionLayerCount>
                matrix{};
            for (auto& row : matrix)
            {
                row = 0xFFFFFFFFu;
            }
            return matrix;
        }
    }

    // プロジェクトと書き出したゲームで共有する物理設定です。
    // 値の範囲はValidateProjectSettingsが検証します。
    struct PhysicsSettings final
    {
        // 重力加速度（m/s²）。既定は地球のY下向き。
        // Rigidbodyの「重力を使う」がオンのものへ掛かります。
        DirectX::XMFLOAT3 gravity{ 0.0f, -9.81f, 0.0f };

        // 物理を1回進める時間（秒）。小さいほど正確ですが、
        // 1フレームあたりの回数が増えて重くなります。
        // FixedUpdateの間隔でもあります。
        float fixedTimeStep{ 1.0f / 60.0f };

        // 遅延を取り戻すために1フレームで実行する物理更新の上限です。
        // 大きな値は遅延時の処理負荷を増やします。
        std::uint32_t maximumCatchUpSteps{ 8 };

        // 接触の解決を繰り返す回数。多いほどめり込みや揺れが減り、
        // その分重くなります。積み上げた箱が沈むときに上げます。
        std::uint32_t solverIterations{ 8 };

        // ここまで遅ければ「止まっている」とみなす速さ（m/s）と
        // 角速度（rad/s）。小さくすると止まりにくくなり、
        // 大きくすると動いているのに寝てしまいます。
        float sleepLinearVelocity{ 0.25f };
        float sleepAngularVelocity{ 0.35f };

        // 上の速さを下回り続けて眠るまでの秒数。
        float sleepDelay{ 0.5f };

        // DCD（離散判定）で警告する速度のしきい値（m/s）。
        // 最も薄いコライダーの厚さを固定タイムステップで割った値が
        // 設定の目安です。
        float discreteSafeSpeed{ 40.0f };

        // trueなら上のしきい値で速度を制限します。falseなら挙動を
        // 変えず、しきい値を超えた物体をログで通知します。
        //
        // CCDを選んだ物体、キネマティック、眠っている物体は
        // どちらの対象にもなりません。
        bool clampDiscreteSpeed{ false };

        // ABI互換性を維持するため、新しいフィールドは末尾に追加します。
        // 構造体のサイズを変更した場合はGameModuleApiVersionも更新し、
        // 古いGame Module DLLの読み込みを拒否してください。

        // 衝突レイヤーの表示名。[0]は"Default"、残りは空（未使用）。
        // 名前はエディターの表示とスクリプトの名前引きに使うだけで、
        // 判定はあくまでコライダーのLayer()（番号）で行います。
        // 名前を変えても既存シーンの挙動は変わりません。
        std::array<std::string, CollisionLayerCount>
            layerNames{ "Default" };

        // 衝突マトリクス。[i]のビットjが立っていればレイヤーiとjは
        // 当たります。既定は全部当たる（＝マトリクスを触らない
        // 限り従来と同じ挙動）。
        //
        // このマトリクスはコライダーごとのCollisionMask()に追加で
        // 掛かります（両方を通ったペアだけが当たる）。
        //
        // 接触解決（3D/2Dの衝突・トリガー・Character Controllerの
        // 接地と壁当たり）にのみ適用します。Scene::Raycastや
        // Scene::OverlapBoxは、呼び出し時に渡されたマスクだけで
        // 絞り込みます。
        //
        // Character Controller内部のOverlapBox結果には、接触解決の
        // 一部としてこのマトリクスを適用します。
        //
        // 対称（[i]のj == [j]のi）が前提です。SetActivePhysicsSettingsが
        // 読み込み時に揃えるので、手で編集したJSONが非対称でも
        // 「両方が許可しているときだけ当たる」側へ丸まります。
        std::array<std::uint32_t, CollisionLayerCount>
            collisionMatrix{ Detail::AllLayersCollide() };
    };

    // いま効いている設定のマトリクスで、レイヤーaとbが当たるか。
    // 物理の接触ペア判定（3D/2D・トリガー・Character Controller）が
    // マスク判定とANDで使います。
    [[nodiscard]] bool LayersCanCollide(
        std::uint32_t layerA,
        std::uint32_t layerB) noexcept;

    // いま効いている物理設定。プロジェクト設定を適用した側が
    // SetActivePhysicsSettingsで入れ、Rigidbodyや当たり判定の解決が
    // ここから読みます。
    //
    // EXEとDLLで同じ設定を共有するため、実体はPhysicsSettings.cppに
    // 1つだけ定義します。
    [[nodiscard]] const PhysicsSettings&
        ActivePhysicsSettings() noexcept;
    // 壊れる値（0以下の時間刻みなど）はここで丸めます。
    void SetActivePhysicsSettings(
        const PhysicsSettings& settings) noexcept;
}
