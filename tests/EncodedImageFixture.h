#pragma once
#include <vector>

namespace LamaPonTest
{
    // Original one-pixel RGBA PNG, with the exact color (220, 30, 80, 255).
    inline std::vector<unsigned char> EncodedImage()
    {
        return {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
            0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0,
            13, 73, 68, 65, 84, 120, 156, 99, 184, 35, 23, 240, 31, 0, 5, 110,
            2, 74, 21, 150, 197, 173, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
    }
}
