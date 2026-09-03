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

    // 物理・当たり判定の調整値。プロジェクト設定に入り、
    // 書き出したゲームにも付いていきます。
    //
    // ここへ集める前は、どの値もコードへ直書きでした（重力9.81、
    // 固定60Hz、反復8回など）。**既定値は直書きのときと同じ**なので、
    // 触らなければ挙動は変わりません。
    //
    // 値の意味と「どこまで下げると何が壊れるか」はdocs/physics.mdに
    // まとめてあります。範囲の検査はValidateProjectSettingsが行います。
    struct PhysicsSettings final
    {
        // 重力加速度（m/s²）。既定は地球のY下向き。
        // Rigidbodyの「重力を使う」がオンのものへ掛かります。
        DirectX::XMFLOAT3 gravity{ 0.0f, -9.81f, 0.0f };

        // 物理を1回進める時間（秒）。小さいほど正確ですが、
        // 1フレームあたりの回数が増えて重くなります。
        // FixedUpdateの間隔でもあります。
        float fixedTimeStep{ 1.0f / 60.0f };

        // 1フレームで進める最大回数。描画が重くて遅れたとき、
        // ここまでしか取り戻しません。**上げると「重いほどさらに
        // 重くなる」悪循環に入ります**（進める回数が増えて、
        // さらに遅れる）。
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

        // DCD（既定の離散判定）で安全に扱える速さ（m/s）。
        //
        // すり抜けが起きる条件は
        // 「速さ × 固定タイムステップ > コライダーの薄さ」です。
        // 60Hzで薄さ0.5mの壁なら 0.5 / (1/60) = 30 m/s が境目なので、
        // **自分のゲームで一番薄い当たり判定 ÷ 固定タイムステップ**を
        // 入れてください。
        //
        // 既定の40は「普通の落下では出ないが、弾や高速移動では出る」
        // あたりに置いています（60Hzで1歩あたり約0.67m）。
        float discreteSafeSpeed{ 40.0f };

        // trueにすると、上の速さを超えないよう速度を頭打ちにします。
        // **既定はfalse**で、超えた物体をログで知らせるだけです
        // （挙動を勝手に変えないため）。オンにするとCCD無しでも
        // すり抜けにくくなりますが、落下が途中で頭打ちになるなど
        // 見た目が変わります。
        //
        // CCDを選んだ物体、キネマティック、眠っている物体は
        // どちらの対象にもなりません。
        bool clampDiscreteSpeed{ false };

        // ---- ここから下は後から足した項目です。**必ず末尾へ足す**
        // こと（途中へ入れるとProjectSettingsのレイアウトがずれ、
        // 作り直していないGame Module DLLが壊れます）。
        //
        // 末尾へ足しても、**それだけでは足りません**。この構造体は
        // Game Moduleから見える（エンジンのヘッダを直接includeする）
        // ため、sizeofが変わったら GameModule.h の
        // GameModuleApiVersion も上げて古いDLLを弾いてください。
        // 古いDLLがSetActivePhysicsSettingsを呼ぶと、runtime側は
        // 新しいレイアウトで読み、足したぶんが未初期化のゴミに
        // なります（今はstd::stringが入っているので落ちます）。----

        // 衝突レイヤーの表示名。[0]は"Default"、残りは空（未使用）。
        // 名前はエディターの表示とスクリプトの名前引きに使うだけで、
        // 判定はあくまでコライダーのLayer()（番号）で行います。
        // 名前を変えても既存シーンの挙動は変わりません。
        std::array<std::string, CollisionLayerCount>
            layerNames{ "Default" };

        // 衝突マトリクス。[i]のビットjが立っていればレイヤーiとjは
        // 当たります。**既定は全部当たる**（＝マトリクスを触らない
        // 限り従来と同じ挙動）。
        //
        // このマトリクスはコライダーごとのCollisionMask()に**追加で**
        // 掛かります（両方を通ったペアだけが当たる）。
        //
        // 掛かるのは**接触の解決**（3D/2Dの衝突・トリガー・Character
        // Controllerの接地と壁当たり）だけです。Scene::Raycastや
        // Scene::OverlapBoxのような**呼び出し側が使う問い合わせ**には
        // 掛かりません——問い合わせは渡されたマスクだけで絞ります
        // 問い合わせは渡されたマスクだけで絞ります。
        //
        // 紛らわしい点: Character Controllerは内部でOverlapBoxを
        // 使いますが、あれは「接地しているか」を自分で解決するための
        // 呼び出しなので、結果へこのマトリクスを後掛けします。
        // 「問い合わせには掛からない」の例外ではなく、**問い合わせの
        // 結果を使って接触を解決している側**だと考えてください。
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
    // **実体はPhysicsSettings.cppに1つだけ**あります。ヘッダで
    // inline staticにするとEXEとDLLで別物になり、設定が無言で
    // 無視されます（[[dll-boundary-inline-static]]の件）。
    [[nodiscard]] const PhysicsSettings&
        ActivePhysicsSettings() noexcept;
    // 壊れる値（0以下の時間刻みなど）はここで丸めます。
    void SetActivePhysicsSettings(
        const PhysicsSettings& settings) noexcept;
}
