/* GLES entry points used by the renderer; all resolve through the selected EGL implementation. */
#ifndef BZ_R_GL_API_H
#define BZ_R_GL_API_H
#include <GLES3/gl3.h>

typedef struct {
    PFNGLACTIVETEXTUREPROC ActiveTexture;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLBEGINQUERYPROC BeginQuery;
    PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBINDBUFFERBASEPROC BindBufferBase;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
    PFNGLBINDTEXTUREPROC BindTexture;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLBLENDEQUATIONPROC BlendEquation;
    PFNGLBLENDFUNCPROC BlendFunc;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLBUFFERSUBDATAPROC BufferSubData;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLCLEARPROC Clear;
    PFNGLCLEARCOLORPROC ClearColor;
    PFNGLCOLORMASKPROC ColorMask;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLCOMPRESSEDTEXIMAGE2DPROC CompressedTexImage2D;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLCULLFACEPROC CullFace;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLDELETEQUERIESPROC DeleteQueries;
    PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLDELETETEXTURESPROC DeleteTextures;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLDEPTHFUNCPROC DepthFunc;
    PFNGLDEPTHMASKPROC DepthMask;
    PFNGLDISABLEPROC Disable;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
    PFNGLDRAWARRAYSPROC DrawArrays;
    PFNGLDRAWARRAYSINSTANCEDPROC DrawArraysInstanced;
    PFNGLDRAWELEMENTSPROC DrawElements;
    PFNGLDRAWELEMENTSINSTANCEDPROC DrawElementsInstanced;
    PFNGLENABLEPROC Enable;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLENDQUERYPROC EndQuery;
    PFNGLFLUSHPROC Flush;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLGENQUERIESPROC GenQueries;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLGENTEXTURESPROC GenTextures;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLGETACTIVEUNIFORMBLOCKIVPROC GetActiveUniformBlockiv;
    PFNGLGETERRORPROC GetError;
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETQUERYOBJECTUIVPROC GetQueryObjectuiv;
    PFNGLGETQUERYIVPROC GetQueryiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETSTRINGPROC GetString;
    PFNGLGETSTRINGIPROC GetStringi;
    PFNGLGETUNIFORMBLOCKINDEXPROC GetUniformBlockIndex;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLISENABLEDPROC IsEnabled;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLMAPBUFFERRANGEPROC MapBufferRange;
    PFNGLPIXELSTOREIPROC PixelStorei;
    PFNGLPOLYGONOFFSETPROC PolygonOffset;
    PFNGLREADPIXELSPROC ReadPixels;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
    PFNGLSCISSORPROC Scissor;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLTEXIMAGE2DPROC TexImage2D;
    PFNGLTEXPARAMETERIPROC TexParameteri;
    PFNGLTEXSUBIMAGE2DPROC TexSubImage2D;
    PFNGLUNIFORM1FVPROC Uniform1fv;
    PFNGLUNIFORM1IVPROC Uniform1iv;
    PFNGLUNIFORM2FVPROC Uniform2fv;
    PFNGLUNIFORM2IVPROC Uniform2iv;
    PFNGLUNIFORM3FVPROC Uniform3fv;
    PFNGLUNIFORM4FVPROC Uniform4fv;
    PFNGLUNIFORMBLOCKBINDINGPROC UniformBlockBinding;
    PFNGLUNIFORMMATRIX3FVPROC UniformMatrix3fv;
    PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv;
    PFNGLUNMAPBUFFERPROC UnmapBuffer;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLVERTEXATTRIBDIVISORPROC VertexAttribDivisor;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLVIEWPORTPROC Viewport;
} glDispatch_t;
extern glDispatch_t bz_gl;

bool R_GLLoadDispatch(void);
void R_GLClearDispatch(void);

#define glActiveTexture bz_gl.ActiveTexture
#define glAttachShader bz_gl.AttachShader
#define glBeginQuery bz_gl.BeginQuery
#define glBindAttribLocation bz_gl.BindAttribLocation
#define glBindBuffer bz_gl.BindBuffer
#define glBindBufferBase bz_gl.BindBufferBase
#define glBindFramebuffer bz_gl.BindFramebuffer
#define glBindRenderbuffer bz_gl.BindRenderbuffer
#define glBindTexture bz_gl.BindTexture
#define glBindVertexArray bz_gl.BindVertexArray
#define glBlendEquation bz_gl.BlendEquation
#define glBlendFunc bz_gl.BlendFunc
#define glBlitFramebuffer bz_gl.BlitFramebuffer
#define glBufferData bz_gl.BufferData
#define glBufferSubData bz_gl.BufferSubData
#define glCheckFramebufferStatus bz_gl.CheckFramebufferStatus
#define glClear bz_gl.Clear
#define glClearColor bz_gl.ClearColor
#define glColorMask bz_gl.ColorMask
#define glCompileShader bz_gl.CompileShader
#define glCompressedTexImage2D bz_gl.CompressedTexImage2D
#define glCreateProgram bz_gl.CreateProgram
#define glCreateShader bz_gl.CreateShader
#define glCullFace bz_gl.CullFace
#define glDeleteBuffers bz_gl.DeleteBuffers
#define glDeleteFramebuffers bz_gl.DeleteFramebuffers
#define glDeleteProgram bz_gl.DeleteProgram
#define glDeleteQueries bz_gl.DeleteQueries
#define glDeleteRenderbuffers bz_gl.DeleteRenderbuffers
#define glDeleteShader bz_gl.DeleteShader
#define glDeleteTextures bz_gl.DeleteTextures
#define glDeleteVertexArrays bz_gl.DeleteVertexArrays
#define glDepthFunc bz_gl.DepthFunc
#define glDepthMask bz_gl.DepthMask
#define glDisable bz_gl.Disable
#define glDisableVertexAttribArray bz_gl.DisableVertexAttribArray
#define glDrawArrays bz_gl.DrawArrays
#define glDrawArraysInstanced bz_gl.DrawArraysInstanced
#define glDrawElements bz_gl.DrawElements
#define glDrawElementsInstanced bz_gl.DrawElementsInstanced
#define glEnable bz_gl.Enable
#define glEnableVertexAttribArray bz_gl.EnableVertexAttribArray
#define glEndQuery bz_gl.EndQuery
#define glFlush bz_gl.Flush
#define glFramebufferRenderbuffer bz_gl.FramebufferRenderbuffer
#define glFramebufferTexture2D bz_gl.FramebufferTexture2D
#define glGenBuffers bz_gl.GenBuffers
#define glGenFramebuffers bz_gl.GenFramebuffers
#define glGenQueries bz_gl.GenQueries
#define glGenRenderbuffers bz_gl.GenRenderbuffers
#define glGenTextures bz_gl.GenTextures
#define glGenVertexArrays bz_gl.GenVertexArrays
#define glGetActiveUniformBlockiv bz_gl.GetActiveUniformBlockiv
/* Keep the member separate from r_local.h's function-like GetError() macro. */
#define glGetError (bz_gl.GetError)
#define glGetIntegerv bz_gl.GetIntegerv
#define glGetProgramInfoLog bz_gl.GetProgramInfoLog
#define glGetProgramiv bz_gl.GetProgramiv
#define glGetQueryObjectuiv bz_gl.GetQueryObjectuiv
#define glGetQueryiv bz_gl.GetQueryiv
#define glGetShaderInfoLog bz_gl.GetShaderInfoLog
#define glGetShaderiv bz_gl.GetShaderiv
#define glGetString bz_gl.GetString
#define glGetStringi bz_gl.GetStringi
#define glGetUniformBlockIndex bz_gl.GetUniformBlockIndex
#define glGetUniformLocation bz_gl.GetUniformLocation
#define glIsEnabled bz_gl.IsEnabled
#define glLinkProgram bz_gl.LinkProgram
#define glMapBufferRange bz_gl.MapBufferRange
#define glPixelStorei bz_gl.PixelStorei
#define glPolygonOffset bz_gl.PolygonOffset
#define glReadPixels bz_gl.ReadPixels
#define glRenderbufferStorage bz_gl.RenderbufferStorage
#define glScissor bz_gl.Scissor
#define glShaderSource bz_gl.ShaderSource
#define glTexImage2D bz_gl.TexImage2D
#define glTexParameteri bz_gl.TexParameteri
#define glTexSubImage2D bz_gl.TexSubImage2D
#define glUniform1fv bz_gl.Uniform1fv
#define glUniform1iv bz_gl.Uniform1iv
#define glUniform2fv bz_gl.Uniform2fv
#define glUniform2iv bz_gl.Uniform2iv
#define glUniform3fv bz_gl.Uniform3fv
#define glUniform4fv bz_gl.Uniform4fv
#define glUniformBlockBinding bz_gl.UniformBlockBinding
#define glUniformMatrix3fv bz_gl.UniformMatrix3fv
#define glUniformMatrix4fv bz_gl.UniformMatrix4fv
#define glUnmapBuffer bz_gl.UnmapBuffer
#define glUseProgram bz_gl.UseProgram
#define glVertexAttribDivisor bz_gl.VertexAttribDivisor
#define glVertexAttribPointer bz_gl.VertexAttribPointer
#define glViewport bz_gl.Viewport

#endif
