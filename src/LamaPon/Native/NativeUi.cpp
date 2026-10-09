#include "LamaPon/Native/NativeBridge.h"
#include "LamaPon/Native/NativeServices.h"
#include "LamaPon/Native/NativeGL.h"
#include "LamaPon/Core/Unicode.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace LamaPon::Native
{
    namespace
    {
        struct Quad
        {
            double id{};
            std::uint32_t texture{};
            std::array<float, 4> color{};
            float x{}, y{}, width{}, height{}, pivotX{}, pivotY{}, rotation{};
            std::array<float, 4> uv{0, 0, 1, 1};
            int order{}, mask{};
        };
        struct Mask { std::array<float, 4> rect; int shape{}; };
        std::vector<Quad> quads;
        std::vector<Mask> masks;
        GLuint program{}, buffer{};
        std::unordered_map<std::string, std::vector<unsigned char>> fonts;
        struct TextTexture { std::uint32_t texture{}; int width{}, height{}; std::string key; std::uint64_t frame{}; };
        std::unordered_map<double, TextTexture> textTextures;
        std::uint64_t frame{};

        GLuint Shader(const GLenum type, const char* body)
        {
#if defined(__ANDROID__)
            const std::string source = std::string("#version 300 es\nprecision mediump float;\n") + body;
#else
            const std::string source = std::string("#version 330 core\n") + body;
#endif
            const char* data = source.c_str();
            GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &data, nullptr); glCompileShader(shader);
            GLint success{}; glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
            if (!success)
            {
                std::array<char, 2048> message{};
                glGetShaderInfoLog(shader, static_cast<GLsizei>(message.size()), nullptr, message.data());
                glDeleteShader(shader); throw std::runtime_error(message.data());
            }
            return shader;
        }
        void Initialize()
        {
            if (program) return;
            const auto vertex = Shader(GL_VERTEX_SHADER, R"(
layout(location=0) in vec2 position; layout(location=2) in vec2 uv;
uniform vec2 viewport; out vec2 texcoord; out vec2 pixel;
void main(){pixel=position; texcoord=uv; vec2 p=position/viewport*2.0-1.0; gl_Position=vec4(p.x,-p.y,0,1);})");
            GLuint fragment{};
            try
            {
                fragment = Shader(GL_FRAGMENT_SHADER, R"(
in vec2 texcoord; in vec2 pixel; out vec4 result;
uniform sampler2D image; uniform vec4 color; uniform int textured;
uniform int maskCount; uniform int interaction; uniform vec4 maskRects[8]; uniform vec4 maskShapes[8];
void main(){bool inside=false;
 for(int i=0;i<8;i++){if(i>=maskCount)break; vec4 r=maskRects[i]; vec2 p=(pixel-r.xy)/max(r.zw,vec2(0.001));
 bool hit=all(greaterThanEqual(p,vec2(0)))&&all(lessThanEqual(p,vec2(1)));
 if(maskShapes[i].x>0.5){vec2 q=p*2.0-1.0;hit=hit&&dot(q,q)<=1.0;} inside=inside||hit;}
 if((interaction==1&&!inside)||(interaction==2&&inside))discard;
 result=color; if(textured!=0)result*=texture(image,texcoord);})");
                program = glCreateProgram(); glAttachShader(program, vertex); glAttachShader(program, fragment);
                glLinkProgram(program); GLint linked{}; glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (!linked) throw std::runtime_error("Native UI shader link failed");
                glGenBuffers(1, &buffer);
            }
            catch (...) { glDeleteShader(vertex); if (fragment) glDeleteShader(fragment); if (program) glDeleteProgram(program); program = 0; throw; }
            glDeleteShader(vertex); glDeleteShader(fragment);
        }

        TextTexture RasterText(const std::string& text, const std::string& fontPath, const float size,
            const float boundsWidth, const bool wrap, const int alignment)
        {
            auto found = fonts.find(fontPath);
            if (found == fonts.end()) found = fonts.emplace(fontPath, ReadAsset(fontPath.c_str())).first;
            const auto& bytes = found->second;
            if (bytes.size() < 12) throw std::runtime_error("Font asset is empty or truncated");
            stbtt_fontinfo font{};
            const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
            if (offset < 0 || !stbtt_InitFont(&font, bytes.data(), offset)) throw std::runtime_error("Cannot initialize font");
            const auto codepoints = Detail::Utf8ToCodeUnits<char32_t>(text);
            const float scale = stbtt_ScaleForPixelHeight(&font, size);
            int ascent{}, descent{}, gap{};
            stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
            const float lineHeight = std::max(1.0f, (ascent - descent + gap) * scale);
            std::vector<std::u32string> lines(1);
            std::vector<float> widths(1);
            for (const auto cp : codepoints)
            {
                if (cp == U'\r') continue;
                if (cp == U'\n') { lines.emplace_back(); widths.push_back(0); continue; }
                int advance{}, bearing{};
                stbtt_GetCodepointHMetrics(&font, static_cast<int>(cp), &advance, &bearing);
                if (wrap && boundsWidth > 0 && !lines.back().empty() && widths.back() + advance * scale > boundsWidth)
                { lines.emplace_back(); widths.push_back(0); }
                if (!lines.back().empty()) widths.back() += stbtt_GetCodepointKernAdvance(&font,
                    static_cast<int>(lines.back().back()), static_cast<int>(cp)) * scale;
                widths.back() += advance * scale; lines.back().push_back(cp);
            }
            const float maximumWidth = *std::max_element(widths.begin(), widths.end());
            const int width = std::clamp(static_cast<int>(std::ceil(wrap && boundsWidth > 0 ? boundsWidth : maximumWidth)) + 2, 1, 8192);
            const int height = std::clamp(static_cast<int>(std::ceil(lineHeight * lines.size())) + 2, 1, 8192);
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4, 0);
            for (std::size_t line = 0; line < lines.size(); ++line)
            {
                float x = 1 + (alignment == 1 ? (width - widths[line]) * 0.5f : alignment == 2 ? width - widths[line] : 0);
                const float baseline = 1 + ascent * scale + static_cast<float>(line) * lineHeight;
                char32_t previous{};
                for (const auto cp : lines[line])
                {
                    if (previous) x += stbtt_GetCodepointKernAdvance(&font, static_cast<int>(previous), static_cast<int>(cp)) * scale;
                    int glyphWidth{}, glyphHeight{}, dx{}, dy{};
                    unsigned char* glyph = stbtt_GetCodepointBitmap(&font, 0, scale, static_cast<int>(cp), &glyphWidth, &glyphHeight, &dx, &dy);
                    for (int row = 0; row < glyphHeight; ++row) for (int column = 0; column < glyphWidth; ++column)
                    {
                        const int px = static_cast<int>(std::floor(x)) + dx + column;
                        const int py = static_cast<int>(std::floor(baseline)) + dy + row;
                        if (px < 0 || py < 0 || px >= width || py >= height) continue;
                        const auto target = (static_cast<std::size_t>(py) * width + px) * 4;
                        pixels[target] = pixels[target + 1] = pixels[target + 2] = 255;
                        pixels[target + 3] = std::max(pixels[target + 3], glyph[row * glyphWidth + column]);
                    }
                    stbtt_FreeBitmap(glyph, nullptr);
                    int advance{}, bearing{}; stbtt_GetCodepointHMetrics(&font, static_cast<int>(cp), &advance, &bearing);
                    x += advance * scale; previous = cp;
                }
            }
            return {UploadTexture(pixels.data(), width, height), width, height, {}};
        }
    }

    void BeginPortableUiFrame() { ++frame; quads.clear(); masks.clear(); }
    void RenderPortableMask(double, float x, float y, float width, float height, int shape)
    { if (masks.size() < 8) masks.push_back({{x, y, width, height}, shape}); }
    void HidePortableObjectUi(const double id)
    { std::erase_if(quads, [id](const Quad& quad) { return quad.id == id; }); }
    void RenderPortableSprite(const char*, double id, const char* path, float r, float g, float b, float a,
        float x, float y, float width, float height, float pivotX, float pivotY, float rotation, int order,
        float sourceX, float sourceY, float sourceWidth, float sourceHeight, int mask)
    {
        Quad quad; quad.id = id; quad.texture = AssetTexture(path); quad.color = {r, g, b, a};
        quad.x = x; quad.y = y; quad.width = width; quad.height = height; quad.pivotX = pivotX; quad.pivotY = pivotY;
        quad.rotation = rotation; quad.order = order; quad.mask = mask; quad.uv = {sourceX, sourceY, sourceWidth, sourceHeight};
        quads.push_back(quad);
    }
    void RenderPortableText(const char*, double id, const char* text, const char*, const char* asset, float size,
        float r, float g, float b, float a, float x, float y, float width, float height, int wrap, int horizontal, int vertical, int order)
    {
        if (!text || !*text || !std::isfinite(size)) return;
        try
        {
            const std::string fontPath = asset && *asset ? asset : "/assets/lamapon-default-font.ttf";
            const std::string key = fontPath + '\n' + text + '\n' + std::to_string(size) + '\n'
                + std::to_string(width) + '\n' + std::to_string(wrap) + '\n' + std::to_string(horizontal);
            auto& texture = textTextures[id];
            if (texture.key != key)
            {
                auto replacement = RasterText(text, fontPath, std::clamp(size, 1.0f, 512.0f), width, wrap != 0, horizontal);
                if (!replacement.texture) throw std::runtime_error("Cannot upload text texture");
                DestroyTexture(texture.texture);
                texture = std::move(replacement);
                texture.key = key;
            }
            texture.frame = frame;
            Quad quad; quad.id = id; quad.texture = texture.texture; quad.color = {r, g, b, a};
            quad.width = static_cast<float>(texture.width); quad.height = static_cast<float>(texture.height);
            quad.x = x + (horizontal == 1 ? (width - quad.width) * 0.5f : horizontal == 2 ? width - quad.width : 0);
            quad.y = y + (vertical == 1 ? (height - quad.height) * 0.5f : vertical == 2 ? height - quad.height : 0);
            quad.order = order; quads.push_back(quad);
        }
        catch (const std::exception& error) { SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Text: %s", error.what()); }
    }
    void EndPortableUiFrame()
    {
        std::erase_if(textTextures, [](const auto& entry)
        {
            if (entry.second.frame == frame) return false;
            DestroyTexture(entry.second.texture);
            return true;
        });
        if (quads.empty()) return;
        Initialize();
        int width{}, height{}; SDL_GetWindowSizeInPixels(GameWindow(), &width, &height);
        if (width <= 0 || height <= 0) return;
        std::stable_sort(quads.begin(), quads.end(), [](const Quad& left, const Quad& right) { return left.order < right.order; });
        glUseProgram(program); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glActiveTexture(GL_TEXTURE0);
        glUniform2f(glGetUniformLocation(program, "viewport"), static_cast<float>(width), static_cast<float>(height));
        glUniform1i(glGetUniformLocation(program, "image"), 0);
        std::array<float, 32> rectangles{}, shapes{};
        for (std::size_t index = 0; index < masks.size(); ++index)
        { std::copy(masks[index].rect.begin(), masks[index].rect.end(), rectangles.begin() + index * 4); shapes[index * 4] = static_cast<float>(masks[index].shape); }
        glUniform1i(glGetUniformLocation(program, "maskCount"), static_cast<int>(masks.size()));
        glUniform4fv(glGetUniformLocation(program, "maskRects[0]"), static_cast<GLsizei>(masks.size()), rectangles.data());
        glUniform4fv(glGetUniformLocation(program, "maskShapes[0]"), static_cast<GLsizei>(masks.size()), shapes.data());
        glBindBuffer(GL_ARRAY_BUFFER, buffer); glEnableVertexAttribArray(0); glDisableVertexAttribArray(1); glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, nullptr);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, reinterpret_cast<void*>(sizeof(float) * 2));
        for (const auto& quad : quads)
        {
            std::array<float, 24> vertices{};
            constexpr std::array<int, 6> corners{0, 1, 2, 0, 2, 3};
            constexpr std::array<float, 4> xs{0, 1, 1, 0}, ys{0, 0, 1, 1};
            const float cosine = std::cos(quad.rotation), sine = std::sin(quad.rotation);
            for (std::size_t index = 0; index < corners.size(); ++index)
            {
                const auto corner = static_cast<std::size_t>(corners[index]);
                const float px = (xs[corner] - quad.pivotX) * quad.width, py = (ys[corner] - quad.pivotY) * quad.height;
                vertices[index * 4] = quad.x + px * cosine - py * sine;
                vertices[index * 4 + 1] = quad.y + px * sine + py * cosine;
                vertices[index * 4 + 2] = quad.uv[0] + xs[corner] * quad.uv[2];
                vertices[index * 4 + 3] = quad.uv[1] + ys[corner] * quad.uv[3];
            }
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_DYNAMIC_DRAW);
            BindTexture(quad.texture);
            glUniform1i(glGetUniformLocation(program, "textured"), quad.texture ? 1 : 0);
            glUniform1i(glGetUniformLocation(program, "interaction"), quad.mask);
            glUniform4fv(glGetUniformLocation(program, "color"), 1, quad.color.data());
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        glEnable(GL_DEPTH_TEST); glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    void ShutdownUi() noexcept
    {
        if (buffer) glDeleteBuffers(1, &buffer);
        if (program) glDeleteProgram(program);
        buffer = program = 0; quads.clear(); masks.clear(); fonts.clear(); textTextures.clear();
    }
    void ForgetUiGraphics() noexcept
    {
        // Old names belong to the lost context. Texture handles remain stable
        // through RestoreTextures, including the cached rasterized text.
        buffer = program = 0;
        quads.clear(); masks.clear();
    }
}
