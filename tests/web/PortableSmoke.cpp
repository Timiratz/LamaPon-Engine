#include "LamaPon/LamaPon.h"

#include <stdexcept>

namespace
{
    class PortableStartupProbe final : public LamaPon::Script
    {
    public:
        void Start() override
        {
            // エンジン用CMake設定で例外のcatchが有効かを確認します。
            // 無効なら初期化中にabortし、running状態には到達しません。
            try
            {
                throw std::runtime_error("expected portable exception");
            }
            catch (const std::runtime_error&)
            {
                LamaPon::Logger::Instance().Info("Portable startup recovery passed.");
            }
        }
    };
}

LAMAPON_SCRIPT_NAMED(PortableStartupProbe, "Test.PortableStartup", "Portable startup probe");
