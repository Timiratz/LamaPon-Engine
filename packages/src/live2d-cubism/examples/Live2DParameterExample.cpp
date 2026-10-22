#include <LamaPonLive2D/Live2DModel.h>

#include <cmath>

class Live2DParameterExample final : public LamaPon::Script
{
public:
    // 時間に合わせてCubismの角度パラメータを動かします(deltaTime: 前フレームからの秒数)。
    void Update(const float deltaTime) override
    {
        m_elapsed += deltaTime;
        // 同じGameObjectにあるLive2Dモデル
        auto* model = GetScript<LamaPonLive2D::Live2DModel>();
        if (model != nullptr)
        {
            static_cast<void>(model->SetParameter(
                "ParamAngleX",
                std::sin(m_elapsed) * 20.0f));
        }
    }

private:
    // アニメーション用の経過秒数
    float m_elapsed{};
};

LAMAPON_SCRIPT(Live2DParameterExample);
