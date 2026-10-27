// GL. The game dlopen()s libGLESv2 and dlsym()s every function by name: dlsym answers with
// table thunks, and the GLES2 entries below forward to the real driver. Guest pointers are
// host pointers, so texture uploads and client-side vertex arrays need no copying.
#include <GLES2/gl2.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "rr2.h"

static GLuint g_fb;  // framebuffer currently bound by the game

// With supersampling on, every clear goes to the 2x target, so the screen's depth buffer keeps
// stale values. The engine then copies the target to the screen with depth test still on, and
// the copy fails wherever old depth is nearer (black, with noise). So clear the screen's depth
// whenever the engine switches to the screen while supersampling; with it on, only MSAA_End does.
static void clear_screen_depth() {
    GLboolean depth_mask, scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint stencil_mask;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask), glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil_mask);
    glDepthMask(GL_TRUE), glStencilMask(~0u), glDisable(GL_SCISSOR_TEST);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDepthMask(depth_mask), glStencilMask(GLuint(stencil_mask));
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

#define W(f) {#f, wrap<f>},
// What the guest sees from glGetString. The game picks texture files by GPU vendor (Imagination
// -> .pvr, NVIDIA -> .dds, others -> .etc). If a driver lacks its vendor's format (e.g. newer
// PowerVR without PVRTC), report a neutral vendor so the game uses .etc. If ETC1 itself isn't
// advertised but the context is GLES 3, upload ETC1 data as ETC2, which decodes it identically.
static bool g_etc1_via_etc2;
static std::string guest_string(GLenum name) {
    auto str = [](GLenum n) { auto s = (const char*)glGetString(n); return std::string(s ? s : ""); };
    std::string s = str(name), ext = str(GL_EXTENSIONS);
    auto has = [&](const char* e) { return ext.find(e) != std::string::npos; };
    if (name == GL_VENDOR && ((s.find("Imagination") != s.npos && !has("GL_IMG_texture_compression_pvrtc")) ||
                              (s.find("NVIDIA") != s.npos && !has("GL_EXT_texture_compression_s3tc"))))
        return LOG("textures: %s lacks its own format, using ETC1", s.c_str()), "ARM";
    if (name == GL_EXTENSIONS && !has("GL_OES_compressed_ETC1_RGB8_texture") && str(GL_VERSION).find("OpenGL ES 3") != s.npos)
        return LOG("textures: no ETC1 extension, uploading as ETC2"), g_etc1_via_etc2 = true, s + " GL_OES_compressed_ETC1_RGB8_texture";
    return s;
}

static Shims gl = {
    W(glActiveTexture) W(glAttachShader) W(glBindAttribLocation) W(glBindBuffer)
    W(glBindRenderbuffer) W(glBindTexture) W(glBlendColor) W(glBlendEquation) W(glBlendEquationSeparate)
    W(glBlendFunc) W(glBlendFuncSeparate) W(glBufferData) W(glBufferSubData) W(glCheckFramebufferStatus)
    W(glClear) W(glClearColor) W(glClearDepthf) W(glClearStencil) W(glColorMask) W(glCompileShader)
    W(glCompressedTexSubImage2D) W(glCopyTexImage2D) W(glCopyTexSubImage2D)
    W(glCreateProgram) W(glCreateShader) W(glCullFace) W(glDeleteBuffers) W(glDeleteFramebuffers)
    W(glDeleteProgram) W(glDeleteRenderbuffers) W(glDeleteShader) W(glDeleteTextures) W(glDepthFunc)
    W(glDepthMask) W(glDepthRangef) W(glDetachShader) W(glDisable) W(glDisableVertexAttribArray)
    W(glEnable) W(glEnableVertexAttribArray) W(glFinish) W(glFlush)
    W(glFramebufferRenderbuffer) W(glFramebufferTexture2D) W(glFrontFace) W(glGenBuffers)
    W(glGenFramebuffers) W(glGenRenderbuffers) W(glGenTextures) W(glGenerateMipmap) W(glGetActiveAttrib)
    W(glGetActiveUniform) W(glGetAttachedShaders) W(glGetAttribLocation) W(glGetBooleanv)
    W(glGetBufferParameteriv) W(glGetError) W(glGetFloatv) W(glGetFramebufferAttachmentParameteriv)
    W(glGetIntegerv) W(glGetProgramInfoLog) W(glGetProgramiv) W(glGetRenderbufferParameteriv)
    W(glGetShaderInfoLog) W(glGetShaderPrecisionFormat) W(glGetShaderSource) W(glGetShaderiv)
    W(glGetTexParameterfv) W(glGetTexParameteriv) W(glGetUniformLocation) W(glGetUniformfv)
    W(glGetUniformiv) W(glGetVertexAttribfv) W(glGetVertexAttribiv) W(glHint) W(glIsBuffer) W(glIsEnabled)
    W(glIsFramebuffer) W(glIsProgram) W(glIsRenderbuffer) W(glIsShader) W(glIsTexture) W(glLineWidth)
    W(glLinkProgram) W(glPixelStorei) W(glPolygonOffset) W(glReadPixels) W(glReleaseShaderCompiler)
    W(glRenderbufferStorage) W(glSampleCoverage) W(glShaderBinary) W(glStencilFunc)
    W(glStencilFuncSeparate) W(glStencilMask) W(glStencilMaskSeparate) W(glStencilOp)
    W(glStencilOpSeparate) W(glTexImage2D) W(glTexParameterf) W(glTexParameterfv) W(glTexParameteri)
    W(glTexParameteriv) W(glTexSubImage2D) W(glUniform1f) W(glUniform1fv) W(glUniform1i) W(glUniform1iv)
    W(glUniform2f) W(glUniform2fv) W(glUniform2i) W(glUniform2iv) W(glUniform3f) W(glUniform3fv)
    W(glUniform3i) W(glUniform3iv) W(glUniform4f) W(glUniform4fv) W(glUniform4i) W(glUniform4iv)
    W(glUniformMatrix2fv) W(glUniformMatrix3fv) W(glUniformMatrix4fv) W(glUseProgram) W(glValidateProgram)
    W(glVertexAttrib1f) W(glVertexAttrib1fv) W(glVertexAttrib2f) W(glVertexAttrib2fv) W(glVertexAttrib3f)
    W(glVertexAttrib3fv) W(glVertexAttrib4f) W(glVertexAttrib4fv) W(glVertexAttribPointer)
    {"glBindFramebuffer", [](Ctx& c) {
         glBindFramebuffer(c.arg(0), g_fb = c.arg(1));
         if (g_fb == 0 && aa_rendering()) clear_screen_depth();
     }},
    // Menus clip with scissor rectangles in screen pixels (e.g. CGameMessageBox, the pause
    // menu). The engine resizes its viewport for the 2x supersampling target but not these,
    // so scale them while that target is what's being drawn into.
    {"glScissor", [](Ctx& c) {
         int k = g_fb != 0 && aa_target_bound() ? 2 : 1;
         glScissor(int(c.arg(0)) * k, int(c.arg(1)) * k, int(c.arg(2)) * k, int(c.arg(3)) * k);
     }},
    W(glViewport) W(glDrawArrays) W(glDrawElements)
    // the three that pass pointers to pointers or return host memory
    {"glShaderSource", [](Ctx& c) {
         std::vector<const GLchar*> src(c.arg(1));
         for (u32 i = 0; i < src.size(); i++) src[i] = ptr<GLchar>(ptr<u32>(c.arg(2))[i]);
         glShaderSource(c.arg(0), GLsizei(src.size()), src.data(), ptr<GLint>(c.arg(3)));
     }},
    {"glCompressedTexImage2D", [](Ctx& c) {
         GLenum fmt = c.arg(2) == 0x8D64 && g_etc1_via_etc2 ? 0x9274 : c.arg(2);  // ETC1 -> COMPRESSED_RGB8_ETC2
         glCompressedTexImage2D(c.arg(0), int(c.arg(1)), fmt, int(c.arg(3)), int(c.arg(4)), int(c.arg(5)),
                                int(c.arg(6)), ptr(c.arg(7)));
     }},
    {"glGetString", [](Ctx& c) {
         static std::unordered_map<GLenum, u32> cache;
         if (cache.empty()) {  // first GL call with a live context: record what surface we got
             GLint b[7];
             const GLenum q[7] = {GL_RED_BITS, GL_GREEN_BITS, GL_BLUE_BITS, GL_ALPHA_BITS,
                                  GL_DEPTH_BITS, GL_STENCIL_BITS, GL_SAMPLES};
             for (int i = 0; i < 7; i++) glGetIntegerv(q[i], &b[i]);
             LOG("GL %s | %s | RGBA %d%d%d%d depth %d stencil %d samples %d", glGetString(GL_RENDERER),
                 glGetString(GL_VERSION), b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
             GLint vp[4], tex, rb;  // the initial viewport is the surface size
             glGetIntegerv(GL_VIEWPORT, vp), glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tex), glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &rb);
             check_supersampling(vp[2], vp[3], std::min(tex, rb));
         }
         auto [it, fresh] = cache.try_emplace(c.arg(0), 0);
         if (fresh) {
             std::string s = guest_string(c.arg(0));
             it->second = gaddr(memcpy(guest_malloc(s.size() + 1), s.c_str(), s.size() + 1));
         }
         c.ret(it->second);
     }},
    // dynamic loading: accept the GL/EGL libraries (Skia is a private system lib: refuse)
    {"dlopen", [](Ctx& c) {
         const char* name = ptr<char>(c.arg(0));
         bool ok = strstr(name, "GLES") || strstr(name, "EGL");
         LOG("dlopen %s -> %s", name, ok ? "ok" : "refused");
         c.ret(ok);
     }},
    {"dlsym", [](Ctx& c) { c.ret(thunk_for(ptr<char>(c.arg(1)))); }},
    {"dlclose", [](Ctx& c) { c.ret(0); }},
};
#undef W
