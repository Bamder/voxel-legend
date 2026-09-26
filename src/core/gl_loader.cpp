#include "gl.hpp"
#include <cstdio>

namespace gl {

#define GLPTR(T, v) PFNGL##T##PROC v = nullptr
GLPTR(CREATESHADER, CreateShader); GLPTR(SHADERSOURCE, ShaderSource); GLPTR(COMPILESHADER, CompileShader);
GLPTR(GETSHADERIV, GetShaderiv); GLPTR(GETSHADERINFOLOG, GetShaderInfoLog); GLPTR(DELETESHADER, DeleteShader);
GLPTR(CREATEPROGRAM, CreateProgram); GLPTR(ATTACHSHADER, AttachShader); GLPTR(LINKPROGRAM, LinkProgram);
GLPTR(GETPROGRAMIV, GetProgramiv); GLPTR(GETPROGRAMINFOLOG, GetProgramInfoLog); GLPTR(USEPROGRAM, UseProgram);
GLPTR(DELETEPROGRAM, DeleteProgram); GLPTR(GETUNIFORMLOCATION, GetUniformLocation); GLPTR(UNIFORM1I, Uniform1i);
GLPTR(UNIFORM1F, Uniform1f); GLPTR(UNIFORM1FV, Uniform1fv);
GLPTR(UNIFORM2F, Uniform2f); GLPTR(UNIFORM3F, Uniform3f);
GLPTR(UNIFORM3FV, Uniform3fv); GLPTR(UNIFORM4F, Uniform4f); GLPTR(UNIFORM4FV, Uniform4fv);
GLPTR(UNIFORMMATRIX4FV, UniformMatrix4fv); GLPTR(GENVERTEXARRAYS, GenVertexArrays);
GLPTR(BINDVERTEXARRAY, BindVertexArray); GLPTR(DELETEVERTEXARRAYS, DeleteVertexArrays);
GLPTR(GENBUFFERS, GenBuffers); GLPTR(BINDBUFFER, BindBuffer); GLPTR(BUFFERDATA, BufferData);
GLPTR(BUFFERSUBDATA, BufferSubData); GLPTR(DELETEBUFFERS, DeleteBuffers);
GLPTR(VERTEXATTRIBPOINTER, VertexAttribPointer); GLPTR(ENABLEVERTEXATTRIBARRAY, EnableVertexAttribArray);
GLPTR(DISABLEVERTEXATTRIBARRAY, DisableVertexAttribArray); GLPTR(GENTEXTURES, GenTextures);
GLPTR(BINDTEXTURE, BindTexture); GLPTR(TEXIMAGE2D, TexImage2D); GLPTR(TEXPARAMETERI, TexParameteri);
GLPTR(GENERATEMIPMAP, GenerateMipmap); GLPTR(DELETETEXTURES, DeleteTextures);
GLPTR(ACTIVETEXTURE, ActiveTexture); GLPTR(PIXELSTOREI, PixelStorei); GLPTR(CLEARCOLOR, ClearColor);
GLPTR(CLEAR, Clear); GLPTR(CLEARDEPTH, ClearDepth); GLPTR(VIEWPORT, Viewport); GLPTR(SCISSOR, Scissor); GLPTR(ENABLE, Enable);
GLPTR(DISABLE, Disable); GLPTR(CULLFACE, CullFace); GLPTR(FRONTFACE, FrontFace);
GLPTR(DEPTHFUNC, DepthFunc); GLPTR(DEPTHMASK, DepthMask); GLPTR(BLENDFUNC, BlendFunc);
GLPTR(DRAWARRAYS, DrawArrays); GLPTR(DRAWELEMENTS, DrawElements); GLPTR(GETSTRING, GetString);
GLPTR(GETERROR, GetError); GLPTR(GETINTEGERV, GetIntegerv); GLPTR(GETFLOATV, GetFloatv);
GLPTR(FINISH, Finish);
#undef GLPTR

PFNWGLCREATECONTEXTATTRIBSARBPROC WglCreateContextAttribsARB = nullptr;
PFNWGLCHOOSEPIXELFORMATARBPROC WglChoosePixelFormatARB = nullptr;
PFNWGLSWAPINTERVALEXTPROC WglSwapIntervalEXT = nullptr;

static int loadErrorCount = 0;
static const char* lastMissing = nullptr;

static void* resolve(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    if (!p) {
        HMODULE ogl = GetModuleHandleA("opengl32.dll");
        p = (void*)GetProcAddress(ogl, name);
    }
    if (!p) {
        loadErrorCount++;
        if (!lastMissing) lastMissing = name;
    }
    return p;
}

bool loadAll() {
    loadErrorCount = 0;
    lastMissing = nullptr;

    WglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)resolve("wglCreateContextAttribsARB");
    WglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)resolve("wglChoosePixelFormatARB");
    WglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)resolve("wglSwapIntervalEXT");

#define L(fn) fn = (decltype(fn))resolve("gl" #fn)
    L(CreateShader); L(ShaderSource); L(CompileShader); L(GetShaderiv); L(GetShaderInfoLog);
    L(DeleteShader); L(CreateProgram); L(AttachShader); L(LinkProgram); L(GetProgramiv);
    L(GetProgramInfoLog); L(UseProgram); L(DeleteProgram); L(GetUniformLocation); L(Uniform1i);
    L(Uniform1f); L(Uniform1fv); L(Uniform2f); L(Uniform3f); L(Uniform3fv); L(Uniform4f); L(Uniform4fv);
    L(UniformMatrix4fv); L(GenVertexArrays); L(BindVertexArray); L(DeleteVertexArrays);
    L(GenBuffers); L(BindBuffer); L(BufferData); L(BufferSubData); L(DeleteBuffers);
    L(VertexAttribPointer); L(EnableVertexAttribArray); L(DisableVertexAttribArray);
    L(GenTextures); L(BindTexture); L(TexImage2D); L(TexParameteri); L(GenerateMipmap);
    L(DeleteTextures); L(ActiveTexture); L(PixelStorei); L(ClearColor); L(Clear); L(ClearDepth);
    L(Viewport); L(Scissor); L(Enable); L(Disable); L(CullFace); L(FrontFace); L(DepthFunc); L(DepthMask);
    L(BlendFunc); L(DrawArrays); L(DrawElements); L(GetString); L(GetError); L(GetIntegerv);
    L(GetFloatv); L(Finish);
#undef L

    if (loadErrorCount > 0) {
        fprintf(stderr, "[gl] failed to load %d GL function(s); first missing: %s\n",
                loadErrorCount, lastMissing ? lastMissing : "?");
        return false;
    }
    return true;
}

const char* errorString(GLenum e) {
    switch (e) {
        case GL_NO_ERROR: return "GL_NO_ERROR";
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default: return "GL_UNKNOWN_ERROR";
    }
}

} // namespace gl
