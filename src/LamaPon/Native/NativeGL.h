#pragma once
#include <SDL3/SDL.h>
#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#else
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <SDL3/SDL_opengl.h>
#endif

namespace LamaPon::Native::GL
{
    inline decltype(&::glActiveTexture) ActiveTexture{};
    inline decltype(&::glAttachShader) AttachShader{};
    inline decltype(&::glBindAttribLocation) BindAttribLocation{};
    inline decltype(&::glBindBuffer) BindBuffer{};
    inline decltype(&::glBindTexture) BindTexture{};
    inline decltype(&::glBindVertexArray) BindVertexArray{};
    inline decltype(&::glBlendFunc) BlendFunc{};
    inline decltype(&::glBufferData) BufferData{};
    inline decltype(&::glClear) Clear{};
    inline decltype(&::glClearColor) ClearColor{};
    inline decltype(&::glClearStencil) ClearStencil{};
    inline decltype(&::glColorMask) ColorMask{};
    inline decltype(&::glCompileShader) CompileShader{};
    inline decltype(&::glCreateProgram) CreateProgram{};
    inline decltype(&::glCreateShader) CreateShader{};
    inline decltype(&::glCullFace) CullFace{};
    inline decltype(&::glDeleteBuffers) DeleteBuffers{};
    inline decltype(&::glDeleteProgram) DeleteProgram{};
    inline decltype(&::glDeleteShader) DeleteShader{};
    inline decltype(&::glDeleteTextures) DeleteTextures{};
    inline decltype(&::glDeleteVertexArrays) DeleteVertexArrays{};
    inline decltype(&::glDepthMask) DepthMask{};
    inline decltype(&::glDisable) Disable{};
    inline decltype(&::glDisableVertexAttribArray) DisableVertexAttribArray{};
    inline decltype(&::glDrawArrays) DrawArrays{};
    inline decltype(&::glDrawElements) DrawElements{};
    inline decltype(&::glEnable) Enable{};
    inline decltype(&::glEnableVertexAttribArray) EnableVertexAttribArray{};
    inline decltype(&::glFrontFace) FrontFace{};
    inline decltype(&::glGenBuffers) GenBuffers{};
    inline decltype(&::glGenTextures) GenTextures{};
    inline decltype(&::glGenVertexArrays) GenVertexArrays{};
    inline decltype(&::glGetError) GetError{};
    inline decltype(&::glGetProgramInfoLog) GetProgramInfoLog{};
    inline decltype(&::glGetProgramiv) GetProgramiv{};
    inline decltype(&::glGetShaderInfoLog) GetShaderInfoLog{};
    inline decltype(&::glGetShaderiv) GetShaderiv{};
    inline decltype(&::glGetString) GetString{};
    inline decltype(&::glGetUniformLocation) GetUniformLocation{};
    inline decltype(&::glLinkProgram) LinkProgram{};
    inline decltype(&::glPixelStorei) PixelStorei{};
    inline decltype(&::glReadPixels) ReadPixels{};
    inline decltype(&::glScissor) Scissor{};
    inline decltype(&::glShaderSource) ShaderSource{};
    inline decltype(&::glStencilFunc) StencilFunc{};
    inline decltype(&::glStencilMask) StencilMask{};
    inline decltype(&::glStencilOp) StencilOp{};
    inline decltype(&::glTexImage2D) TexImage2D{};
    inline decltype(&::glTexParameteri) TexParameteri{};
    inline decltype(&::glUniform1f) Uniform1f{};
    inline decltype(&::glUniform1i) Uniform1i{};
    inline decltype(&::glUniform2f) Uniform2f{};
    inline decltype(&::glUniform2fv) Uniform2fv{};
    inline decltype(&::glUniform3f) Uniform3f{};
    inline decltype(&::glUniform4f) Uniform4f{};
    inline decltype(&::glUniform4fv) Uniform4fv{};
    inline decltype(&::glUniformMatrix4fv) UniformMatrix4fv{};
    inline decltype(&::glUseProgram) UseProgram{};
    inline decltype(&::glVertexAttribPointer) VertexAttribPointer{};
    inline decltype(&::glViewport) Viewport{};
    inline bool Load()
    {
        const auto nextActiveTexture = reinterpret_cast<decltype(ActiveTexture)>(SDL_GL_GetProcAddress("glActiveTexture"));
        if (!nextActiveTexture) { SDL_SetError("Missing OpenGL procedure: glActiveTexture"); return false; }
        const auto nextAttachShader = reinterpret_cast<decltype(AttachShader)>(SDL_GL_GetProcAddress("glAttachShader"));
        if (!nextAttachShader) { SDL_SetError("Missing OpenGL procedure: glAttachShader"); return false; }
        const auto nextBindAttribLocation = reinterpret_cast<decltype(BindAttribLocation)>(SDL_GL_GetProcAddress("glBindAttribLocation"));
        if (!nextBindAttribLocation) { SDL_SetError("Missing OpenGL procedure: glBindAttribLocation"); return false; }
        const auto nextBindBuffer = reinterpret_cast<decltype(BindBuffer)>(SDL_GL_GetProcAddress("glBindBuffer"));
        if (!nextBindBuffer) { SDL_SetError("Missing OpenGL procedure: glBindBuffer"); return false; }
        const auto nextBindTexture = reinterpret_cast<decltype(BindTexture)>(SDL_GL_GetProcAddress("glBindTexture"));
        if (!nextBindTexture) { SDL_SetError("Missing OpenGL procedure: glBindTexture"); return false; }
        const auto nextBindVertexArray = reinterpret_cast<decltype(BindVertexArray)>(SDL_GL_GetProcAddress("glBindVertexArray"));
        if (!nextBindVertexArray) { SDL_SetError("Missing OpenGL procedure: glBindVertexArray"); return false; }
        const auto nextBlendFunc = reinterpret_cast<decltype(BlendFunc)>(SDL_GL_GetProcAddress("glBlendFunc"));
        if (!nextBlendFunc) { SDL_SetError("Missing OpenGL procedure: glBlendFunc"); return false; }
        const auto nextBufferData = reinterpret_cast<decltype(BufferData)>(SDL_GL_GetProcAddress("glBufferData"));
        if (!nextBufferData) { SDL_SetError("Missing OpenGL procedure: glBufferData"); return false; }
        const auto nextClear = reinterpret_cast<decltype(Clear)>(SDL_GL_GetProcAddress("glClear"));
        if (!nextClear) { SDL_SetError("Missing OpenGL procedure: glClear"); return false; }
        const auto nextClearColor = reinterpret_cast<decltype(ClearColor)>(SDL_GL_GetProcAddress("glClearColor"));
        if (!nextClearColor) { SDL_SetError("Missing OpenGL procedure: glClearColor"); return false; }
        const auto nextClearStencil = reinterpret_cast<decltype(ClearStencil)>(SDL_GL_GetProcAddress("glClearStencil"));
        if (!nextClearStencil) { SDL_SetError("Missing OpenGL procedure: glClearStencil"); return false; }
        const auto nextColorMask = reinterpret_cast<decltype(ColorMask)>(SDL_GL_GetProcAddress("glColorMask"));
        if (!nextColorMask) { SDL_SetError("Missing OpenGL procedure: glColorMask"); return false; }
        const auto nextCompileShader = reinterpret_cast<decltype(CompileShader)>(SDL_GL_GetProcAddress("glCompileShader"));
        if (!nextCompileShader) { SDL_SetError("Missing OpenGL procedure: glCompileShader"); return false; }
        const auto nextCreateProgram = reinterpret_cast<decltype(CreateProgram)>(SDL_GL_GetProcAddress("glCreateProgram"));
        if (!nextCreateProgram) { SDL_SetError("Missing OpenGL procedure: glCreateProgram"); return false; }
        const auto nextCreateShader = reinterpret_cast<decltype(CreateShader)>(SDL_GL_GetProcAddress("glCreateShader"));
        if (!nextCreateShader) { SDL_SetError("Missing OpenGL procedure: glCreateShader"); return false; }
        const auto nextCullFace = reinterpret_cast<decltype(CullFace)>(SDL_GL_GetProcAddress("glCullFace"));
        if (!nextCullFace) { SDL_SetError("Missing OpenGL procedure: glCullFace"); return false; }
        const auto nextDeleteBuffers = reinterpret_cast<decltype(DeleteBuffers)>(SDL_GL_GetProcAddress("glDeleteBuffers"));
        if (!nextDeleteBuffers) { SDL_SetError("Missing OpenGL procedure: glDeleteBuffers"); return false; }
        const auto nextDeleteProgram = reinterpret_cast<decltype(DeleteProgram)>(SDL_GL_GetProcAddress("glDeleteProgram"));
        if (!nextDeleteProgram) { SDL_SetError("Missing OpenGL procedure: glDeleteProgram"); return false; }
        const auto nextDeleteShader = reinterpret_cast<decltype(DeleteShader)>(SDL_GL_GetProcAddress("glDeleteShader"));
        if (!nextDeleteShader) { SDL_SetError("Missing OpenGL procedure: glDeleteShader"); return false; }
        const auto nextDeleteTextures = reinterpret_cast<decltype(DeleteTextures)>(SDL_GL_GetProcAddress("glDeleteTextures"));
        if (!nextDeleteTextures) { SDL_SetError("Missing OpenGL procedure: glDeleteTextures"); return false; }
        const auto nextDeleteVertexArrays = reinterpret_cast<decltype(DeleteVertexArrays)>(SDL_GL_GetProcAddress("glDeleteVertexArrays"));
        if (!nextDeleteVertexArrays) { SDL_SetError("Missing OpenGL procedure: glDeleteVertexArrays"); return false; }
        const auto nextDepthMask = reinterpret_cast<decltype(DepthMask)>(SDL_GL_GetProcAddress("glDepthMask"));
        if (!nextDepthMask) { SDL_SetError("Missing OpenGL procedure: glDepthMask"); return false; }
        const auto nextDisable = reinterpret_cast<decltype(Disable)>(SDL_GL_GetProcAddress("glDisable"));
        if (!nextDisable) { SDL_SetError("Missing OpenGL procedure: glDisable"); return false; }
        const auto nextDisableVertexAttribArray = reinterpret_cast<decltype(DisableVertexAttribArray)>(SDL_GL_GetProcAddress("glDisableVertexAttribArray"));
        if (!nextDisableVertexAttribArray) { SDL_SetError("Missing OpenGL procedure: glDisableVertexAttribArray"); return false; }
        const auto nextDrawArrays = reinterpret_cast<decltype(DrawArrays)>(SDL_GL_GetProcAddress("glDrawArrays"));
        if (!nextDrawArrays) { SDL_SetError("Missing OpenGL procedure: glDrawArrays"); return false; }
        const auto nextDrawElements = reinterpret_cast<decltype(DrawElements)>(SDL_GL_GetProcAddress("glDrawElements"));
        if (!nextDrawElements) { SDL_SetError("Missing OpenGL procedure: glDrawElements"); return false; }
        const auto nextEnable = reinterpret_cast<decltype(Enable)>(SDL_GL_GetProcAddress("glEnable"));
        if (!nextEnable) { SDL_SetError("Missing OpenGL procedure: glEnable"); return false; }
        const auto nextEnableVertexAttribArray = reinterpret_cast<decltype(EnableVertexAttribArray)>(SDL_GL_GetProcAddress("glEnableVertexAttribArray"));
        if (!nextEnableVertexAttribArray) { SDL_SetError("Missing OpenGL procedure: glEnableVertexAttribArray"); return false; }
        const auto nextFrontFace = reinterpret_cast<decltype(FrontFace)>(SDL_GL_GetProcAddress("glFrontFace"));
        if (!nextFrontFace) { SDL_SetError("Missing OpenGL procedure: glFrontFace"); return false; }
        const auto nextGenBuffers = reinterpret_cast<decltype(GenBuffers)>(SDL_GL_GetProcAddress("glGenBuffers"));
        if (!nextGenBuffers) { SDL_SetError("Missing OpenGL procedure: glGenBuffers"); return false; }
        const auto nextGenTextures = reinterpret_cast<decltype(GenTextures)>(SDL_GL_GetProcAddress("glGenTextures"));
        if (!nextGenTextures) { SDL_SetError("Missing OpenGL procedure: glGenTextures"); return false; }
        const auto nextGenVertexArrays = reinterpret_cast<decltype(GenVertexArrays)>(SDL_GL_GetProcAddress("glGenVertexArrays"));
        if (!nextGenVertexArrays) { SDL_SetError("Missing OpenGL procedure: glGenVertexArrays"); return false; }
        const auto nextGetError = reinterpret_cast<decltype(GetError)>(SDL_GL_GetProcAddress("glGetError"));
        if (!nextGetError) { SDL_SetError("Missing OpenGL procedure: glGetError"); return false; }
        const auto nextGetProgramInfoLog = reinterpret_cast<decltype(GetProgramInfoLog)>(SDL_GL_GetProcAddress("glGetProgramInfoLog"));
        if (!nextGetProgramInfoLog) { SDL_SetError("Missing OpenGL procedure: glGetProgramInfoLog"); return false; }
        const auto nextGetProgramiv = reinterpret_cast<decltype(GetProgramiv)>(SDL_GL_GetProcAddress("glGetProgramiv"));
        if (!nextGetProgramiv) { SDL_SetError("Missing OpenGL procedure: glGetProgramiv"); return false; }
        const auto nextGetShaderInfoLog = reinterpret_cast<decltype(GetShaderInfoLog)>(SDL_GL_GetProcAddress("glGetShaderInfoLog"));
        if (!nextGetShaderInfoLog) { SDL_SetError("Missing OpenGL procedure: glGetShaderInfoLog"); return false; }
        const auto nextGetShaderiv = reinterpret_cast<decltype(GetShaderiv)>(SDL_GL_GetProcAddress("glGetShaderiv"));
        if (!nextGetShaderiv) { SDL_SetError("Missing OpenGL procedure: glGetShaderiv"); return false; }
        const auto nextGetString = reinterpret_cast<decltype(GetString)>(SDL_GL_GetProcAddress("glGetString"));
        if (!nextGetString) { SDL_SetError("Missing OpenGL procedure: glGetString"); return false; }
        const auto nextGetUniformLocation = reinterpret_cast<decltype(GetUniformLocation)>(SDL_GL_GetProcAddress("glGetUniformLocation"));
        if (!nextGetUniformLocation) { SDL_SetError("Missing OpenGL procedure: glGetUniformLocation"); return false; }
        const auto nextLinkProgram = reinterpret_cast<decltype(LinkProgram)>(SDL_GL_GetProcAddress("glLinkProgram"));
        if (!nextLinkProgram) { SDL_SetError("Missing OpenGL procedure: glLinkProgram"); return false; }
        const auto nextPixelStorei = reinterpret_cast<decltype(PixelStorei)>(SDL_GL_GetProcAddress("glPixelStorei"));
        if (!nextPixelStorei) { SDL_SetError("Missing OpenGL procedure: glPixelStorei"); return false; }
        const auto nextReadPixels = reinterpret_cast<decltype(ReadPixels)>(SDL_GL_GetProcAddress("glReadPixels"));
        if (!nextReadPixels) { SDL_SetError("Missing OpenGL procedure: glReadPixels"); return false; }
        const auto nextScissor = reinterpret_cast<decltype(Scissor)>(SDL_GL_GetProcAddress("glScissor"));
        if (!nextScissor) { SDL_SetError("Missing OpenGL procedure: glScissor"); return false; }
        const auto nextShaderSource = reinterpret_cast<decltype(ShaderSource)>(SDL_GL_GetProcAddress("glShaderSource"));
        if (!nextShaderSource) { SDL_SetError("Missing OpenGL procedure: glShaderSource"); return false; }
        const auto nextStencilFunc = reinterpret_cast<decltype(StencilFunc)>(SDL_GL_GetProcAddress("glStencilFunc"));
        if (!nextStencilFunc) { SDL_SetError("Missing OpenGL procedure: glStencilFunc"); return false; }
        const auto nextStencilMask = reinterpret_cast<decltype(StencilMask)>(SDL_GL_GetProcAddress("glStencilMask"));
        if (!nextStencilMask) { SDL_SetError("Missing OpenGL procedure: glStencilMask"); return false; }
        const auto nextStencilOp = reinterpret_cast<decltype(StencilOp)>(SDL_GL_GetProcAddress("glStencilOp"));
        if (!nextStencilOp) { SDL_SetError("Missing OpenGL procedure: glStencilOp"); return false; }
        const auto nextTexImage2D = reinterpret_cast<decltype(TexImage2D)>(SDL_GL_GetProcAddress("glTexImage2D"));
        if (!nextTexImage2D) { SDL_SetError("Missing OpenGL procedure: glTexImage2D"); return false; }
        const auto nextTexParameteri = reinterpret_cast<decltype(TexParameteri)>(SDL_GL_GetProcAddress("glTexParameteri"));
        if (!nextTexParameteri) { SDL_SetError("Missing OpenGL procedure: glTexParameteri"); return false; }
        const auto nextUniform1f = reinterpret_cast<decltype(Uniform1f)>(SDL_GL_GetProcAddress("glUniform1f"));
        if (!nextUniform1f) { SDL_SetError("Missing OpenGL procedure: glUniform1f"); return false; }
        const auto nextUniform1i = reinterpret_cast<decltype(Uniform1i)>(SDL_GL_GetProcAddress("glUniform1i"));
        if (!nextUniform1i) { SDL_SetError("Missing OpenGL procedure: glUniform1i"); return false; }
        const auto nextUniform2f = reinterpret_cast<decltype(Uniform2f)>(SDL_GL_GetProcAddress("glUniform2f"));
        if (!nextUniform2f) { SDL_SetError("Missing OpenGL procedure: glUniform2f"); return false; }
        const auto nextUniform2fv = reinterpret_cast<decltype(Uniform2fv)>(SDL_GL_GetProcAddress("glUniform2fv"));
        if (!nextUniform2fv) { SDL_SetError("Missing OpenGL procedure: glUniform2fv"); return false; }
        const auto nextUniform3f = reinterpret_cast<decltype(Uniform3f)>(SDL_GL_GetProcAddress("glUniform3f"));
        if (!nextUniform3f) { SDL_SetError("Missing OpenGL procedure: glUniform3f"); return false; }
        const auto nextUniform4f = reinterpret_cast<decltype(Uniform4f)>(SDL_GL_GetProcAddress("glUniform4f"));
        if (!nextUniform4f) { SDL_SetError("Missing OpenGL procedure: glUniform4f"); return false; }
        const auto nextUniform4fv = reinterpret_cast<decltype(Uniform4fv)>(SDL_GL_GetProcAddress("glUniform4fv"));
        if (!nextUniform4fv) { SDL_SetError("Missing OpenGL procedure: glUniform4fv"); return false; }
        const auto nextUniformMatrix4fv = reinterpret_cast<decltype(UniformMatrix4fv)>(SDL_GL_GetProcAddress("glUniformMatrix4fv"));
        if (!nextUniformMatrix4fv) { SDL_SetError("Missing OpenGL procedure: glUniformMatrix4fv"); return false; }
        const auto nextUseProgram = reinterpret_cast<decltype(UseProgram)>(SDL_GL_GetProcAddress("glUseProgram"));
        if (!nextUseProgram) { SDL_SetError("Missing OpenGL procedure: glUseProgram"); return false; }
        const auto nextVertexAttribPointer = reinterpret_cast<decltype(VertexAttribPointer)>(SDL_GL_GetProcAddress("glVertexAttribPointer"));
        if (!nextVertexAttribPointer) { SDL_SetError("Missing OpenGL procedure: glVertexAttribPointer"); return false; }
        const auto nextViewport = reinterpret_cast<decltype(Viewport)>(SDL_GL_GetProcAddress("glViewport"));
        if (!nextViewport) { SDL_SetError("Missing OpenGL procedure: glViewport"); return false; }
        // Commit only a complete procedure table; recovery failure must not leave null cleanup callbacks.
        ActiveTexture = nextActiveTexture;
        AttachShader = nextAttachShader;
        BindAttribLocation = nextBindAttribLocation;
        BindBuffer = nextBindBuffer;
        BindTexture = nextBindTexture;
        BindVertexArray = nextBindVertexArray;
        BlendFunc = nextBlendFunc;
        BufferData = nextBufferData;
        Clear = nextClear;
        ClearColor = nextClearColor;
        ClearStencil = nextClearStencil;
        ColorMask = nextColorMask;
        CompileShader = nextCompileShader;
        CreateProgram = nextCreateProgram;
        CreateShader = nextCreateShader;
        CullFace = nextCullFace;
        DeleteBuffers = nextDeleteBuffers;
        DeleteProgram = nextDeleteProgram;
        DeleteShader = nextDeleteShader;
        DeleteTextures = nextDeleteTextures;
        DeleteVertexArrays = nextDeleteVertexArrays;
        DepthMask = nextDepthMask;
        Disable = nextDisable;
        DisableVertexAttribArray = nextDisableVertexAttribArray;
        DrawArrays = nextDrawArrays;
        DrawElements = nextDrawElements;
        Enable = nextEnable;
        EnableVertexAttribArray = nextEnableVertexAttribArray;
        FrontFace = nextFrontFace;
        GenBuffers = nextGenBuffers;
        GenTextures = nextGenTextures;
        GenVertexArrays = nextGenVertexArrays;
        GetError = nextGetError;
        GetProgramInfoLog = nextGetProgramInfoLog;
        GetProgramiv = nextGetProgramiv;
        GetShaderInfoLog = nextGetShaderInfoLog;
        GetShaderiv = nextGetShaderiv;
        GetString = nextGetString;
        GetUniformLocation = nextGetUniformLocation;
        LinkProgram = nextLinkProgram;
        PixelStorei = nextPixelStorei;
        ReadPixels = nextReadPixels;
        Scissor = nextScissor;
        ShaderSource = nextShaderSource;
        StencilFunc = nextStencilFunc;
        StencilMask = nextStencilMask;
        StencilOp = nextStencilOp;
        TexImage2D = nextTexImage2D;
        TexParameteri = nextTexParameteri;
        Uniform1f = nextUniform1f;
        Uniform1i = nextUniform1i;
        Uniform2f = nextUniform2f;
        Uniform2fv = nextUniform2fv;
        Uniform3f = nextUniform3f;
        Uniform4f = nextUniform4f;
        Uniform4fv = nextUniform4fv;
        UniformMatrix4fv = nextUniformMatrix4fv;
        UseProgram = nextUseProgram;
        VertexAttribPointer = nextVertexAttribPointer;
        Viewport = nextViewport;
        return true;
    }
}

// 共有レンダラー内のGL呼び出しをロード済みの関数へ接続する。
#define glActiveTexture LamaPon::Native::GL::ActiveTexture
#define glAttachShader LamaPon::Native::GL::AttachShader
#define glBindAttribLocation LamaPon::Native::GL::BindAttribLocation
#define glBindBuffer LamaPon::Native::GL::BindBuffer
#define glBindTexture LamaPon::Native::GL::BindTexture
#define glBindVertexArray LamaPon::Native::GL::BindVertexArray
#define glBlendFunc LamaPon::Native::GL::BlendFunc
#define glBufferData LamaPon::Native::GL::BufferData
#define glClear LamaPon::Native::GL::Clear
#define glClearColor LamaPon::Native::GL::ClearColor
#define glClearStencil LamaPon::Native::GL::ClearStencil
#define glColorMask LamaPon::Native::GL::ColorMask
#define glCompileShader LamaPon::Native::GL::CompileShader
#define glCreateProgram LamaPon::Native::GL::CreateProgram
#define glCreateShader LamaPon::Native::GL::CreateShader
#define glCullFace LamaPon::Native::GL::CullFace
#define glDeleteBuffers LamaPon::Native::GL::DeleteBuffers
#define glDeleteProgram LamaPon::Native::GL::DeleteProgram
#define glDeleteShader LamaPon::Native::GL::DeleteShader
#define glDeleteTextures LamaPon::Native::GL::DeleteTextures
#define glDeleteVertexArrays LamaPon::Native::GL::DeleteVertexArrays
#define glDepthMask LamaPon::Native::GL::DepthMask
#define glDisable LamaPon::Native::GL::Disable
#define glDisableVertexAttribArray LamaPon::Native::GL::DisableVertexAttribArray
#define glDrawArrays LamaPon::Native::GL::DrawArrays
#define glDrawElements LamaPon::Native::GL::DrawElements
#define glEnable LamaPon::Native::GL::Enable
#define glEnableVertexAttribArray LamaPon::Native::GL::EnableVertexAttribArray
#define glFrontFace LamaPon::Native::GL::FrontFace
#define glGenBuffers LamaPon::Native::GL::GenBuffers
#define glGenTextures LamaPon::Native::GL::GenTextures
#define glGenVertexArrays LamaPon::Native::GL::GenVertexArrays
#define glGetError LamaPon::Native::GL::GetError
#define glGetProgramInfoLog LamaPon::Native::GL::GetProgramInfoLog
#define glGetProgramiv LamaPon::Native::GL::GetProgramiv
#define glGetShaderInfoLog LamaPon::Native::GL::GetShaderInfoLog
#define glGetShaderiv LamaPon::Native::GL::GetShaderiv
#define glGetString LamaPon::Native::GL::GetString
#define glGetUniformLocation LamaPon::Native::GL::GetUniformLocation
#define glLinkProgram LamaPon::Native::GL::LinkProgram
#define glPixelStorei LamaPon::Native::GL::PixelStorei
#define glReadPixels LamaPon::Native::GL::ReadPixels
#define glScissor LamaPon::Native::GL::Scissor
#define glShaderSource LamaPon::Native::GL::ShaderSource
#define glStencilFunc LamaPon::Native::GL::StencilFunc
#define glStencilMask LamaPon::Native::GL::StencilMask
#define glStencilOp LamaPon::Native::GL::StencilOp
#define glTexImage2D LamaPon::Native::GL::TexImage2D
#define glTexParameteri LamaPon::Native::GL::TexParameteri
#define glUniform1f LamaPon::Native::GL::Uniform1f
#define glUniform1i LamaPon::Native::GL::Uniform1i
#define glUniform2f LamaPon::Native::GL::Uniform2f
#define glUniform2fv LamaPon::Native::GL::Uniform2fv
#define glUniform3f LamaPon::Native::GL::Uniform3f
#define glUniform4f LamaPon::Native::GL::Uniform4f
#define glUniform4fv LamaPon::Native::GL::Uniform4fv
#define glUniformMatrix4fv LamaPon::Native::GL::UniformMatrix4fv
#define glUseProgram LamaPon::Native::GL::UseProgram
#define glVertexAttribPointer LamaPon::Native::GL::VertexAttribPointer
#define glViewport LamaPon::Native::GL::Viewport
