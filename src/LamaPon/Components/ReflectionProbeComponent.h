#pragma once

#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Scene/Component.h"

#include <algorithm>

namespace LamaPon
{
    // リフレクションプローブ（ローカルIBL）。
    //
    // Skyboxの環境反射はシーン全体で1つなので、屋内の金属が屋外の
    // 空を映してしまいます。プローブを置くと、その位置から見た
    // シーンをキューブマップへ焼き（ベイク）、範囲内のオブジェクトの
    // 反射と環境光がそれに置き換わります。
    //
    // ベイクはシーン読み込み後に自動で1回走ります。物を動かしたら
    // Inspectorの「ベイクし直す」を押してください（実行中の自動
    // 再ベイクはしません。6面ぶんの再描画が走るためです）。
    // 焼いた結果はGPUメモリー上だけに持ち、ファイルへは保存しません。
    class ReflectionProbeComponent final : public Component
    {
    public:
        explicit ReflectionProbeComponent(
            const float range = 10.0f,
            const float intensity = 1.0f) noexcept
        {
            SetRange(range);
            SetIntensity(intensity);
        }

        // 影響範囲（半径、ワールド単位）。中心がこの範囲に入っている
        // オブジェクトへ適用されます。
        void SetRange(const float range) noexcept
        {
            m_range = std::max(range, 0.1f);
        }
        [[nodiscard]] float Range() const noexcept
        {
            return m_range;
        }

        // 反射の強さ（SkyのIBL強度と同じ扱い）。
        void SetIntensity(const float intensity) noexcept
        {
            m_intensity = std::max(intensity, 0.0f);
        }
        [[nodiscard]] float Intensity() const noexcept
        {
            return m_intensity;
        }

        // ボックス射影の箱の半径（各軸、プローブ中心から）。
        //
        // キューブマップは「無限遠の景色」として作られるので、
        // これを指定しないと部屋の壁が無限に遠くにあるように映り、
        // カメラを動かしても壁の映り込みが動きません。部屋の
        // 大きさを入れると、壁・床・天井が正しい距離で映ります。
        // 0を含む場合は補正しません（屋外や空を映す用途）。
        void SetBoxExtents(
            const DirectX::XMFLOAT3& extents) noexcept
        {
            m_boxExtents = {
                std::max(extents.x, 0.0f),
                std::max(extents.y, 0.0f),
                std::max(extents.z, 0.0f)
            };
        }
        [[nodiscard]] const DirectX::XMFLOAT3&
            BoxExtents() const noexcept
        {
            return m_boxExtents;
        }
        // 3軸すべてが正のときだけボックス射影が効きます。
        [[nodiscard]] bool
            UsesBoxProjection() const noexcept
        {
            return m_boxExtents.x > 0.0f
                && m_boxExtents.y > 0.0f
                && m_boxExtents.z > 0.0f;
        }

        // 隣のプローブと混ぜ始める距離（範囲の内側から測った厚み）。
        //
        // 0にすると混ぜません（境界で切り替わる従来の挙動）。値を
        // 入れると、範囲の縁からこの厚みのあいだで少しずつ隣の
        // プローブへ移るので、境界をまたいだ瞬間に映り込みが飛ぶのを
        // 防げます。範囲より大きい値を入れても範囲までに収めます。
        void SetBlendDistance(const float distance) noexcept
        {
            m_blendDistance = std::max(distance, 0.0f);
        }
        [[nodiscard]] float BlendDistance() const noexcept
        {
            return std::min(m_blendDistance, m_range);
        }

        // この位置に対する影響度（0＝範囲の外、1＝混ぜ始める距離より
        // 内側）。プローブを選ぶときと混ぜる比率の両方に使います。
        [[nodiscard]] float InfluenceAt(
            const DirectX::XMFLOAT3& position)
            const noexcept;

        // 箱の中心にも使うワールド位置。
        [[nodiscard]] DirectX::XMFLOAT3
            WorldPosition() const noexcept;

        // ベイクを要求します（次の描画フレームの頭で実行されます）。
        void RequestBake() noexcept
        {
            m_bakeRequested = true;
        }
        [[nodiscard]] bool IsBakeRequested() const noexcept
        {
            return m_bakeRequested;
        }
        [[nodiscard]] bool IsBaked() const noexcept
        {
            return m_baked.IsValid();
        }

        // Sceneのベイク処理だけが使います。
        void SetBakedEnvironment(
            EnvironmentRenderer::OwnedPrefilteredEnvironment
                baked) noexcept
        {
            m_baked = std::move(baked);
            m_bakeRequested = false;
        }
        [[nodiscard]] const EnvironmentRenderer::
            OwnedPrefilteredEnvironment&
            BakedEnvironment() const noexcept
        {
            return m_baked;
        }

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "ReflectionProbe";
        }

        // シーンファイルから読み込まれたプローブであることの印。
        // ディスクの環境キャッシュから復元してよいのは、この印が
        // あるものだけです（スクリプトが実行中に作ったプローブは、
        // 過去の別の絵を映すべきではないため）。
        void MarkLoadedFromScene() noexcept
        {
            m_loadedFromScene = true;
        }
        [[nodiscard]] bool IsLoadedFromScene() const noexcept
        {
            return m_loadedFromScene;
        }

        // ディスクからの復元を一度試したかどうか。外れても毎フレーム
        // ファイルを開き直さないための印で、Sceneのベイク処理だけが
        // 使います。
        void MarkRestoreAttempted() noexcept
        {
            m_restoreAttempted = true;
        }
        [[nodiscard]] bool RestoreAttempted() const noexcept
        {
            return m_restoreAttempted;
        }

    private:
        float m_range{ 10.0f };
        float m_intensity{ 1.0f };
        // 既定は0（補正なし＝空や屋外を映す従来の挙動）。
        DirectX::XMFLOAT3 m_boxExtents{};
        // 既定は0（混ぜない＝境界で切り替わる従来の挙動）。
        float m_blendDistance{};
        // 初期状態でベイク待ち。シーン読み込みでコンポーネントが
        // 作り直されると自動的にベイク待ちへ戻ります（その最初の
        // 1回は、ディスクのキャッシュに残っていれば復元で済みます）。
        bool m_bakeRequested{ true };
        bool m_loadedFromScene{};
        bool m_restoreAttempted{};
        EnvironmentRenderer::OwnedPrefilteredEnvironment
            m_baked;
    };
}
