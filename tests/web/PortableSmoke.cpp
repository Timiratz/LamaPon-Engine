#include "LamaPon/LamaPon.h"

#include <stdexcept>

namespace
{
    class PortableStartupProbe final : public LamaPon::Script
    {
    public:
        // 初期化中の例外を捕捉してPortableの例外有効化を検査する。
        void Start() override
        {
            // 例外捕捉が無効なBuildではabortし、running状態へ到達できない。
            try
            {
                throw std::runtime_error("expected portable exception");
            }
            // Portable runtimeが捕捉した初期化例外を記録します。
            catch (const std::runtime_error&)
            {
                LamaPon::Logger::Instance().Info("Portable startup recovery passed.");
            }
        }
    };
}

LAMAPON_SCRIPT_NAMED(PortableStartupProbe, "Test.PortableStartup", "Portable startup probe");
