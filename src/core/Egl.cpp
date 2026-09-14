#include "Egl.hpp"
#include "../helpers/Log.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include <stdexcept>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

static const EGLint WINDOW_CONFIG_ATTRIBS[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
};

static const EGLint GBM_CONFIG_ATTRIBS[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
};

static const EGLint CONTEXT_ATTRIBS[] = {
    EGL_CONTEXT_CLIENT_VERSION,
    2,
    EGL_NONE,
};

static const char* eglErrorToString(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_DEVICE_EXT: return "EGL_BAD_DEVICE_EXT";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    }
    return "Unknown";
}

bool CEGL::finishContext(const char* how) {
    EGLint matched = 0;
    const EGLint* attribs = m_usesGbm ? GBM_CONFIG_ATTRIBS : WINDOW_CONFIG_ATTRIBS;
    if (!eglChooseConfig(eglDisplay, attribs, &eglConfig, 1, &matched) || matched == 0) {
        Log::logger->log(Log::ERR, "eglChooseConfig failed ({})", eglErrorToString(eglGetError()));
        return false;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, CONTEXT_ATTRIBS);
    if (eglContext == EGL_NO_CONTEXT) {
        Log::logger->log(Log::ERR, "Failed to create EGL context ({})", eglErrorToString(eglGetError()));
        return false;
    }

    if (eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, eglContext) == EGL_FALSE) {
        Log::logger->log(Log::ERR, "surfaceless eglMakeCurrent failed ({})", eglErrorToString(eglGetError()));
        return false;
    }

    const char* vendorString = eglQueryString(eglDisplay, EGL_VENDOR);
    m_isNvidia               = vendorString && std::string{vendorString}.contains("NVIDIA");
    Log::logger->log(Log::INFO, "EGL vendor: {} ({})", vendorString ? vendorString : "(null)", how);
    return true;
}

bool CEGL::initWayland(wl_display* display) {
    eglDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, display, nullptr);
    if (eglDisplay == EGL_NO_DISPLAY) {
        Log::logger->log(Log::ERR, "Failed to create Wayland EGL display ({})", eglErrorToString(eglGetError()));
        return false;
    }

    if (eglInitialize(eglDisplay, nullptr, nullptr) == EGL_FALSE) {
        Log::logger->log(Log::ERR, "Failed to initialize Wayland EGL ({})", eglErrorToString(eglGetError()));
        eglTerminate(eglDisplay);
        eglDisplay = EGL_NO_DISPLAY;
        return false;
    }

    m_usesGbm = false;
    if (!finishContext("wayland")) {
        eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay, eglContext);
            eglContext = EGL_NO_CONTEXT;
        }
        eglTerminate(eglDisplay);
        eglDisplay = EGL_NO_DISPLAY;
        return false;
    }
    return true;
}

bool CEGL::initGbm(const char* renderNode) {
    m_gbmFd = open(renderNode, O_RDWR | O_CLOEXEC);
    if (m_gbmFd < 0) {
        Log::logger->log(Log::ERR, "Failed to open GBM render node {}", renderNode);
        return false;
    }

    m_gbm = gbm_create_device(m_gbmFd);
    if (!m_gbm) {
        Log::logger->log(Log::ERR, "gbm_create_device failed for {}", renderNode);
        close(m_gbmFd);
        m_gbmFd = -1;
        return false;
    }

    eglDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, m_gbm, nullptr);
    if (eglDisplay == EGL_NO_DISPLAY) {
        Log::logger->log(Log::ERR, "Failed to create GBM EGL display for {} ({})", renderNode, eglErrorToString(eglGetError()));
        gbm_device_destroy(m_gbm);
        m_gbm = nullptr;
        close(m_gbmFd);
        m_gbmFd = -1;
        return false;
    }

    if (eglInitialize(eglDisplay, nullptr, nullptr) == EGL_FALSE) {
        Log::logger->log(Log::ERR, "Failed to initialize GBM EGL for {} ({})", renderNode, eglErrorToString(eglGetError()));
        eglTerminate(eglDisplay);
        eglDisplay = EGL_NO_DISPLAY;
        gbm_device_destroy(m_gbm);
        m_gbm = nullptr;
        close(m_gbmFd);
        m_gbmFd = -1;
        return false;
    }

    m_usesGbm = true;
    if (!finishContext(renderNode)) {
        eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay, eglContext);
            eglContext = EGL_NO_CONTEXT;
        }
        eglTerminate(eglDisplay);
        eglDisplay = EGL_NO_DISPLAY;
        gbm_device_destroy(m_gbm);
        m_gbm = nullptr;
        close(m_gbmFd);
        m_gbmFd = -1;
        return false;
    }

    Log::logger->log(Log::INFO, "EGL using GBM render node {}", renderNode);
    return true;
}

CEGL::CEGL(wl_display* display) {
    const char* _EXTS = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!_EXTS) {
        if (eglGetError() == EGL_BAD_DISPLAY)
            throw std::runtime_error("EGL_EXT_client_extensions not supported");
        else
            throw std::runtime_error("Failed to query EGL client extensions");
    }

    std::string EXTS = _EXTS;

    if (!EXTS.contains("EGL_EXT_platform_base"))
        throw std::runtime_error("EGL_EXT_platform_base not supported");

    eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (eglGetPlatformDisplayEXT == nullptr)
        throw std::runtime_error("Failed to get eglGetPlatformDisplayEXT");

    if (EXTS.contains("EGL_EXT_platform_wayland") || EXTS.contains("EGL_KHR_platform_wayland")) {
        eglCreatePlatformWindowSurfaceEXT = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    }

    const char* gbmNode = getenv("HYPRLOCK_GBM_DEVICE");
    if (gbmNode && *gbmNode) {
        if (!initGbm(gbmNode))
            throw std::runtime_error("HYPRLOCK_GBM_DEVICE EGL init failed");
        return;
    }

    if (eglCreatePlatformWindowSurfaceEXT && initWayland(display))
        return;

    Log::logger->log(Log::WARN, "Wayland EGL init failed, trying Intel GBM");
    if (initGbm("/dev/dri/renderD128") || initGbm("/dev/dri/renderD129"))
        return;

    throw std::runtime_error("Failed to initialize EGL");
}

CEGL::~CEGL() {
    eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (eglContext != EGL_NO_CONTEXT)
        eglDestroyContext(eglDisplay, eglContext);

    if (eglDisplay != EGL_NO_DISPLAY)
        eglTerminate(eglDisplay);

    eglReleaseThread();

    if (m_gbm)
        gbm_device_destroy(m_gbm);
    if (m_gbmFd >= 0)
        close(m_gbmFd);
}

EGLSurface CEGL::createPlatformWindowSurfaceEXT(wl_egl_window* eglWindow) {
    if (!eglCreatePlatformWindowSurfaceEXT)
        return EGL_NO_SURFACE;

    EGLSurface eglSurface = eglCreatePlatformWindowSurfaceEXT(eglDisplay, eglConfig, eglWindow, nullptr);
    if (eglSurface == EGL_NO_SURFACE)
        Log::logger->log(Log::ERR, "Failed to allocate egl window surface, error: {}", eglErrorToString(eglGetError()));

    return eglSurface;
}

void CEGL::makeCurrent(EGLSurface surf) {
    const EGLSurface draw = surf ? surf : EGL_NO_SURFACE;
    if (eglMakeCurrent(eglDisplay, draw, draw, eglContext) == EGL_FALSE)
        Log::logger->log(Log::ERR, "Failed to eglMakeCurrent, error:  {}", eglErrorToString(eglGetError()));
}

bool CEGL::swapBuffers(EGLSurface surf) {
    if (eglSwapBuffers(eglDisplay, surf) == EGL_FALSE) {
        Log::logger->log(Log::ERR, "Failed to eglSwapBuffers, error:  {}", eglErrorToString(eglGetError()));
        return false;
    }

    return true;
}
