#pragma once
#include <windows.h>
#include <GL/glcorearb.h>
#include <GL/wglext.h>

// Minimal OpenGL 3.3 core function loader (Win32 + WGL).
namespace gl {

extern PFNGLCREATESHADERPROC CreateShader;
extern PFNGLSHADERSOURCEPROC ShaderSource;
extern PFNGLCOMPILESHADERPROC CompileShader;
extern PFNGLGETSHADERIVPROC GetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
extern PFNGLDELETESHADERPROC DeleteShader;

extern PFNGLCREATEPROGRAMPROC CreateProgram;
extern PFNGLATTACHSHADERPROC AttachShader;
extern PFNGLLINKPROGRAMPROC LinkProgram;
extern PFNGLGETPROGRAMIVPROC GetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
extern PFNGLUSEPROGRAMPROC UseProgram;
extern PFNGLDELETEPROGRAMPROC DeleteProgram;
extern PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
extern PFNGLUNIFORM1IPROC Uniform1i;
extern PFNGLUNIFORM1FPROC Uniform1f;
extern PFNGLUNIFORM1FVPROC Uniform1fv;
extern PFNGLUNIFORM2FPROC Uniform2f;
extern PFNGLUNIFORM3FPROC Uniform3f;
extern PFNGLUNIFORM3FVPROC Uniform3fv;
extern PFNGLUNIFORM4FPROC Uniform4f;
extern PFNGLUNIFORM4FVPROC Uniform4fv;
extern PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv;

extern PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC BindVertexArray;
extern PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;

extern PFNGLGENBUFFERSPROC GenBuffers;
extern PFNGLBINDBUFFERPROC BindBuffer;
extern PFNGLBUFFERDATAPROC BufferData;
extern PFNGLBUFFERSUBDATAPROC BufferSubData;
extern PFNGLDELETEBUFFERSPROC DeleteBuffers;

extern PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;

extern PFNGLGENTEXTURESPROC GenTextures;
extern PFNGLBINDTEXTUREPROC BindTexture;
extern PFNGLTEXIMAGE2DPROC TexImage2D;
extern PFNGLTEXPARAMETERIPROC TexParameteri;
extern PFNGLGENERATEMIPMAPPROC GenerateMipmap;
extern PFNGLDELETETEXTURESPROC DeleteTextures;
extern PFNGLACTIVETEXTUREPROC ActiveTexture;
extern PFNGLPIXELSTOREIPROC PixelStorei;

extern PFNGLCLEARCOLORPROC ClearColor;
extern PFNGLCLEARPROC Clear;
extern PFNGLCLEARDEPTHPROC ClearDepth;
extern PFNGLVIEWPORTPROC Viewport;
extern PFNGLSCISSORPROC Scissor;
extern PFNGLENABLEPROC Enable;
extern PFNGLDISABLEPROC Disable;
extern PFNGLCULLFACEPROC CullFace;
extern PFNGLFRONTFACEPROC FrontFace;
extern PFNGLDEPTHFUNCPROC DepthFunc;
extern PFNGLDEPTHMASKPROC DepthMask;
extern PFNGLBLENDFUNCPROC BlendFunc;
extern PFNGLDRAWARRAYSPROC DrawArrays;
extern PFNGLDRAWELEMENTSPROC DrawElements;
extern PFNGLGETSTRINGPROC GetString;
extern PFNGLGETERRORPROC GetError;
extern PFNGLGETINTEGERVPROC GetIntegerv;
extern PFNGLGETFLOATVPROC GetFloatv;
extern PFNGLFINISHPROC Finish;

// WGL extensions
extern PFNWGLCREATECONTEXTATTRIBSARBPROC WglCreateContextAttribsARB;
extern PFNWGLCHOOSEPIXELFORMATARBPROC WglChoosePixelFormatARB;
extern PFNWGLSWAPINTERVALEXTPROC WglSwapIntervalEXT;

// Load every pointer above. Must be called with a current GL context.
// Returns false if any pointer failed to load (logs to stderr).
bool loadAll();

const char* errorString(GLenum e);

} // namespace gl
