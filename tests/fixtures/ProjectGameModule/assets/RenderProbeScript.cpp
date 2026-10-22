#include "LamaPon/LamaPon.h"

#include <string>

class RenderProbeScript final : public LamaPon::Script
{
public:
    // 描画callbackの呼び出し数を保存します(sprites: 現在の描画窓口)。
    void OnRender2D(const LamaPon::SpriteDrawContext& sprites) override
    {
        if (sprites)
        {
            ++m_renderCount;
        }
    }

protected:
    // 描画回数をJSONへ保存します。
    std::string SaveProperties() const override
    {
        return "{\"renders\":" + std::to_string(m_renderCount) + "}";
    }

private:
    // 描画窓口が有効だった回数
    int m_renderCount{};
};

LAMAPON_SCRIPT(RenderProbeScript);
