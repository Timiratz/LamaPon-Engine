#pragma once

#include "LamaPon/Graphics/PrefilteredEnvironment.h"
#include "LamaPon/Scene/Component.h"

#include <algorithm>
#include <utility>

namespace LamaPon
{
    // 読み込み時はキャッシュ復元またはベイクを行い、シーン変更後は明示的な再ベイクが必要です。
    // 結果はディスクの環境キャッシュにも保存し、数値設定には有限値を指定します。
    class ReflectionProbeComponent final : public Component
    {
    public:
        // ローカル環境光プローブを作ります(range: 影響半径ワールド単位, intensity: 環境光強度)。
        explicit ReflectionProbeComponent(
            const float range = 10.0f,
            const float intensity = 1.0f) noexcept
        {
            SetRange(range);
            SetIntensity(intensity);
        }

        // 影響半径を0.1以上に制限して設定します(range: ワールド単位の半径)。
        void SetRange(const float range) noexcept
        {
            m_range = std::max(range, 0.1f);
        }
        // 影響半径をワールド単位で返します。
        [[nodiscard]] float Range() const noexcept
        {
            return m_range;
        }

        // 強度を非負に制限して設定します(intensity: 環境光強度)。
        void SetIntensity(const float intensity) noexcept
        {
            m_intensity = std::max(intensity, 0.0f);
        }
        // 環境光の強度を返します。
        [[nodiscard]] float Intensity() const noexcept
        {
            return m_intensity;
        }

        // ボックス射影の半幅を非負に制限して設定します(extents: ワールドXYZ半幅)。
        void SetBoxExtents(
            const DirectX::XMFLOAT3& extents) noexcept
        {
            m_boxExtents = {
                std::max(extents.x, 0.0f),
                std::max(extents.y, 0.0f),
                std::max(extents.z, 0.0f)
            };
        }
        // ボックス射影のワールドXYZ半幅を返します。
        [[nodiscard]] const DirectX::XMFLOAT3&
            BoxExtents() const noexcept
        {
            return m_boxExtents;
        }
        // XYZ半幅がすべて正でボックス射影を使うか返します。
        [[nodiscard]] bool
            UsesBoxProjection() const noexcept
        {
            return m_boxExtents.x > 0.0f
                && m_boxExtents.y > 0.0f
                && m_boxExtents.z > 0.0f;
        }

        // 混合を始める内側の厚みを設定します(distance: 非負に制限するワールド距離で0は混合なし)。
        void SetBlendDistance(const float distance) noexcept
        {
            m_blendDistance = std::max(distance, 0.0f);
        }
        // 影響半径以下に制限した混合距離を返します。
        [[nodiscard]] float BlendDistance() const noexcept
        {
            return std::min(m_blendDistance, m_range);
        }

        // 範囲外で0、混合域で線形に減る影響度を返します(position: 評価するワールド位置)。
        [[nodiscard]] float InfluenceAt(
            const DirectX::XMFLOAT3& position)
            const noexcept;

        // プローブ中心のワールド位置を返します。
        [[nodiscard]] DirectX::XMFLOAT3
            WorldPosition() const noexcept;

        // 次のシーン描画時のベイクを要求します。
        void RequestBake() noexcept
        {
            m_bakeRequested = true;
        }
        // ベイクが要求されているか返します。
        [[nodiscard]] bool IsBakeRequested() const noexcept
        {
            return m_bakeRequested;
        }
        // 保持する環境ビューが有効か返します。
        [[nodiscard]] bool IsBaked() const noexcept
        {
            return m_baked.IsValid();
        }

        // シーンのベイク結果を保持して要求を解除します(baked: 生成した環境ビュー)。
        void SetBakedEnvironment(
            PrefilteredEnvironmentViews baked) noexcept
        {
            m_baked = std::move(baked);
            m_bakeRequested = false;
        }
        // 保持する環境ビューへの参照を返します。
        [[nodiscard]] const PrefilteredEnvironmentViews&
            BakedEnvironment() const noexcept
        {
            return m_baked;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "ReflectionProbe";
        }

        // シーン読み込み由来と記録して初回キャッシュ復元を許可します。
        void MarkLoadedFromScene() noexcept
        {
            m_loadedFromScene = true;
        }
        // シーン読み込み由来か返します。
        [[nodiscard]] bool IsLoadedFromScene() const noexcept
        {
            return m_loadedFromScene;
        }

        // シーンが初回のキャッシュ復元を試みたと記録します。
        void MarkRestoreAttempted() noexcept
        {
            m_restoreAttempted = true;
        }
        // 初回のキャッシュ復元を試みたか返します。
        [[nodiscard]] bool RestoreAttempted() const noexcept
        {
            return m_restoreAttempted;
        }

    private:
        // 影響半径ワールド単位
        float m_range{ 10.0f };
        // 環境光の強度
        float m_intensity{ 1.0f };
        // 射影箱のワールドXYZ半幅
        DirectX::XMFLOAT3 m_boxExtents{};
        // 混合域の厚みワールド単位
        float m_blendDistance{};
        // ベイク要求の有無
        bool m_bakeRequested{ true };
        // シーン読み込み由来の指定
        bool m_loadedFromScene{};
        // 初回キャッシュ復元試行済み
        bool m_restoreAttempted{};
        // 保持するベイク環境ビュー
        PrefilteredEnvironmentViews m_baked;
    };
}
