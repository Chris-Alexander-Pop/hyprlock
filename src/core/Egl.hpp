#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-egl.h>
#include <gbm.h>

#include "../defines.hpp"

class CEGL {
  public:
    CEGL(wl_display*);
    ~CEGL();

    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig  eglConfig  = nullptr;
    EGLContext eglContext = EGL_NO_CONTEXT;

    EGLSurface createPlatformWindowSurfaceEXT(wl_egl_window* eglWindow);
    void       makeCurrent(EGLSurface surf);
    bool       swapBuffers(EGLSurface surf);

    bool       m_isNvidia = false;
    // NVIDIA wayland-egl window surfaces are incomplete on Intel-scanout
    // outputs. GBM-only contexts have no wl_egl_window at all.
    bool       needsShmPresent() const {
        return m_isNvidia || m_usesGbm;
    }

  private:
    bool initWayland(wl_display* display);
    bool initGbm(const char* renderNode);
    bool finishContext(const char* how);

    bool                                     m_usesGbm = false;
    int                                      m_gbmFd   = -1;
    gbm_device*                              m_gbm     = nullptr;
    PFNEGLGETPLATFORMDISPLAYEXTPROC          eglGetPlatformDisplayEXT          = nullptr;
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC eglCreatePlatformWindowSurfaceEXT = nullptr;
};

inline UP<CEGL> g_pEGL;
