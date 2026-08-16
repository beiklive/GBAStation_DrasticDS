/* egl_vk_stubs.c -- the few EGL/GLES symbols NVK does NOT already stub, for the
 * VULKAN build only.
 *
 * The Vulkan (NVK) build does NOT link switch-mesa's libEGL/libGLESv2 (they and
 * NVK both bundle mesa util/nir/compiler object code and can't co-link). But
 * the shared import layer references EGL entry points even when the Vulkan host
 * is selected. NVK's own
 * rust_switch_stubs already provides most egl* symbols (eglGetDisplay,
 * eglSwapBuffers, ...); this file adds only the handful it does not, so the link
 * resolves. The Vulkan presenter uses DraStic's renderFrame only to select and
 * consume its completed screen buffer; glBindTexture/glTexSubImage2D are
 * redirected to the CPU capture bridge and no GLES context is created. These
 * remaining EGL queries therefore fail benignly. Compiled to nothing in the
 * OpenGL build (real switch-mesa symbols linked instead).
 */
#ifdef USE_VULKAN

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stddef.h>

EGLDisplay eglGetDisplay(EGLNativeDisplayType display) {
  (void)display; return (EGLDisplay)1;
}
EGLBoolean eglInitialize(EGLDisplay display, EGLint *major, EGLint *minor) {
  (void)display; if (major) *major = 1; if (minor) *minor = 5; return EGL_TRUE;
}
EGLBoolean eglBindAPI(EGLenum api) { (void)api; return EGL_TRUE; }
const char *eglQueryString(EGLDisplay display, EGLint name) {
  (void)display; (void)name; return "";
}
EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint *attributes,
                           EGLConfig *config, EGLint size, EGLint *count) {
  (void)display; (void)attributes;
  if (config && size) *config = (EGLConfig)1;
  if (count) *count = 1;
  return EGL_TRUE;
}
EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
                              EGLint attribute, EGLint *value) {
  (void)display; (void)config; (void)attribute; if (value) *value = 0;
  return EGL_TRUE;
}
EGLContext eglCreateContext(EGLDisplay display, EGLConfig config,
                            EGLContext share, const EGLint *attributes) {
  (void)display; (void)config; (void)share; (void)attributes; return (EGLContext)1;
}
EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                  EGLNativeWindowType window,
                                  const EGLint *attributes) {
  (void)display; (void)config; (void)window; (void)attributes; return (EGLSurface)1;
}
EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config,
                                   const EGLint *attributes) {
  (void)display; (void)config; (void)attributes; return (EGLSurface)1;
}
EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
  (void)display; (void)surface; return EGL_TRUE;
}
EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context) {
  (void)display; (void)context; return EGL_TRUE;
}
EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read,
                          EGLContext context) {
  (void)display; (void)draw; (void)read; (void)context; return EGL_TRUE;
}
EGLint eglGetError(void) { return EGL_SUCCESS; }
EGLBoolean eglSwapInterval(EGLDisplay display, EGLint interval) {
  (void)display; (void)interval; return EGL_TRUE;
}
EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
  (void)display; (void)surface; return EGL_TRUE;
}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
  (void)name; return NULL;
}
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v) {
  (void)d; (void)s; (void)a; if (v) *v = 0; return EGL_FALSE; }
EGLContext eglGetCurrentContext(void) { return EGL_NO_CONTEXT; }
EGLSurface eglGetCurrentSurface(EGLint rw) { (void)rw; return EGL_NO_SURFACE; }
const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)""; }

#endif // USE_VULKAN
