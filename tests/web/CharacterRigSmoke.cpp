#include "LamaPon/LamaPon.h"

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

// Web版の2Dキャラクター部品(揺れ物・瞬き・スキン・パラメータ・メッシュ描画)をブラウザーで検証します。
namespace
{
    // 差が許容内か返します(left: 比較元, right: 比較先, tolerance: 許容差)。
    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance)
    {
        return std::abs(left - right) <= tolerance;
    }

    // 検証結果をbodyのdata-test-statusへ書きます(status: passedか失敗理由)。
    EM_JS(void, PublishStatus, (const char* status), {
        // 公開する検証結果
        const text = UTF8ToString(status);
        document.body.dataset.testStatus = text;
        console.log("CharacterRig2D web check: " + text);
    });

    // 同じフレームの描画が終わった直後にDOMを検証して結果を書きます(strandId: メッシュの房, hairId: 通常の髪, noseId: 姿勢だけ動く鼻)。
    // 描画は全ての更新の後に行われるため、次の描画フレームを待たずにsetTimeoutで検証します。
    EM_JS(void, ScheduleDomCheck, (double strandId, double hairId, double noseId), {
        setTimeout(() => {
            // 指定物体のIDから要素を探します(prefix: 要素IDの接頭辞, id: 物体のID)。
            const find = (prefix, id) => document.getElementById(prefix + String(Math.floor(id)));
            // 房のメッシュ要素
            const strand = find("lamapon-portable-mesh-", strandId);
            // 結果の文
            let status = "passed";
            if (!strand || strand.style.display === "none" || strand.children.length !== 8
                || !strand.firstElementChild.style.transform.startsWith("matrix(")) {
                status = "the mesh was not drawn as eight DOM triangles";
            }
            else if (!strand.firstElementChild.style.backgroundImage.includes("data:image/png")) {
                status = "the mesh triangles did not use the sprite texture";
            }
            else if (!find("lamapon-portable-sprite-", hairId)) {
                status = "a plain sprite stopped using the sprite element";
            }
            else if (!find("lamapon-portable-sprite-", noseId)
                || find("lamapon-portable-mesh-", noseId)) {
                status = "a pose-only keyform turned its sprite into a mesh";
            }
            document.body.dataset.testStatus = status;
            console.log("CharacterRig2D web check: " + status);
        }, 0);
    });

    class CharacterRig2DProbe final : public LamaPon::Script
    {
    public:
        // 最初の更新の中でシーンを60Hzで12回進めて状態を検証し、DOMの検証を予約します(deltaTime: 経過秒数)。
        // ヘッドレスのブラウザーは描画フレームの数と間隔が揃わないため、検証に使う時間はここで固定します。
        void Update(float) override
        {
            if (m_finished || m_stepping)
            {
                return;
            }
            m_stepping = true;
            // 固定で進めた回数
            for (int step = 1; step <= SimulatedSteps && !m_finished; ++step)
            {
                Step(step);
                GetScene().Update(1.0f / 60.0f);
            }
            m_stepping = false;
            if (!m_finished)
            {
                Verify();
            }
        }

    private:
        // 固定で進める回数
        static constexpr int SimulatedSteps = 12;

        // 1回分の入力を与え、揺れと瞬きの観測値を記録します(step: 1から数えた回数)。
        void Step(const int step)
        {
            // パラメータを持つ根元
            auto* character = Find("Character");
            // 動かす頭
            auto* head = Find("Head");
            // 揺れる髪
            auto* hair = Find("Hair");
            // 瞬きする目
            auto* eye = Find("Eye");
            // 瞬きの親
            auto* eyes = Find("Eyes");
            // パラメータで動く鼻
            auto* nose = Find("Nose");
            // スキンで曲がる房
            auto* strand = Find("Strand");
            // 房の下のボーン
            auto* bone = Find("StrandBone1");
            if (character == nullptr || head == nullptr || hair == nullptr
                || eye == nullptr || eyes == nullptr || nose == nullptr
                || strand == nullptr || bone == nullptr)
            {
                Fail("a scene object was not loaded");
                return;
            }
            if (step == 1)
            {
                character->GetComponent<LamaPon::Rig2DComponent>()->SetParameter("AngleX", 30.0f);
                bone->GetTransform().SetEulerAngles(0.0f, 0.0f, std::numbers::pi_v<float> * 0.5f);
            }
            if (step <= 6)
            {
                head->GetTransform().position.x += 20.0f;
            }
            // 髪の揺れ物
            const auto* sway = hair->GetComponent<LamaPon::Sway2DComponent>();
            m_maximumSway = std::max(m_maximumSway, sway != nullptr ? sway->CurrentAngle() : 0.0f);
            // 目の瞬き
            const auto* blink = eyes->GetComponent<LamaPon::Blink2DComponent>();
            m_blinkSeen = m_blinkSeen || (blink != nullptr && blink->IsBlinking());
            m_eyeFrameChanged = m_eyeFrameChanged
                || eye->GetComponent<LamaPon::SpriteRendererComponent>()->SourceRect().x > 0.0f;
        }

        // 進めた後の状態を検証し、成功ならDOMの検証を予約します。
        void Verify()
        {
            // 揺れる髪
            auto* hair = Find("Hair");
            // パラメータで動く鼻
            auto* nose = Find("Nose");
            // スキンで曲がる房
            auto* strand = Find("Strand");
            // 髪の揺れ物
            const auto* sway = hair->GetComponent<LamaPon::Sway2DComponent>();
            if (sway == nullptr || m_maximumSway < 0.01f)
            {
                Fail("Sway2D did not swing");
                return;
            }
            if (!NearlyEqual(nose->GetTransform().position.x, 112.0f, 0.01f)
                || !NearlyEqual(
                    nose->GetTransform().rotation.z,
                    6.0f * std::numbers::pi_v<float> / 180.0f,
                    0.001f))
            {
                Fail("Keyform2D did not move the nose");
                return;
            }
            if (!m_blinkSeen || !m_eyeFrameChanged)
            {
                Fail("Blink2D did not switch frames");
                return;
            }
            // スキンで曲げた房の頂点
            const auto positions =
                strand->GetComponent<LamaPon::SpriteRendererComponent>()->DeformedMeshPositions();
            if (positions.size() != 10
                || !NearlyEqual(positions[8].x, -50.0f, 1.5f)
                || !NearlyEqual(positions[8].y, 40.0f, 1.5f))
            {
                Fail("SpriteSkin2D did not bend the mesh");
                return;
            }
            m_finished = true;
            ScheduleDomCheck(
                static_cast<double>(strand->Id()),
                static_cast<double>(hair->Id()),
                static_cast<double>(nose->Id()));
        }

        // 失敗を一度だけ公開します(reason: 失敗理由)。
        void Fail(const std::string& reason)
        {
            m_finished = true;
            PublishStatus(reason.c_str());
        }

        // 固定の更新中で、入れ子の呼び出しを無視するか
        bool m_stepping{};
        // 観測した揺れの最大角度ラジアン
        float m_maximumSway{};
        // 瞬き中の状態を観測したか
        bool m_blinkSeen{};
        // 目のコマが切り替わったか
        bool m_eyeFrameChanged{};
        // 結果を公開したか検証を予約したか
        bool m_finished{};
    };
}

LAMAPON_SCRIPT_NAMED(CharacterRig2DProbe, "Test.CharacterRig2D", "Character rig 2D probe");
