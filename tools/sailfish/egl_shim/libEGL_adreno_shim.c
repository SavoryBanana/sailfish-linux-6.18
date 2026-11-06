typedef __UINT8_TYPE__ uint8_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __SIZE_TYPE__ size_t;

#define NULL ((void*)0)

#define EGL_SUCCESS                     0x3000
#define EGL_NOT_INITIALIZED             0x3001
#define EGL_BAD_ACCESS                  0x3002
#define EGL_BAD_ALLOC                   0x3003
#define EGL_BAD_ATTRIBUTE               0x3004
#define EGL_BAD_CONFIG                  0x3005
#define EGL_BAD_CONTEXT                 0x3006
#define EGL_BAD_CURRENT_SURFACE         0x3007
#define EGL_BAD_DISPLAY                 0x3008
#define EGL_BAD_MATCH                   0x3009
#define EGL_BAD_NATIVE_PIXMAP           0x300A
#define EGL_BAD_NATIVE_WINDOW           0x300B
#define EGL_BAD_PARAMETER               0x300C
#define EGL_BAD_SURFACE                 0x300D
#define EGL_BUFFER_SIZE                 0x3020
#define EGL_ALPHA_SIZE                  0x3021
#define EGL_BLUE_SIZE                   0x3022
#define EGL_GREEN_SIZE                  0x3023
#define EGL_RED_SIZE                    0x3024
#define EGL_DEPTH_SIZE                  0x3025
#define EGL_STENCIL_SIZE                0x3026
#define EGL_CONFIG_CAVEAT               0x3027
#define EGL_CONFIG_ID                   0x3028
#define EGL_LEVEL                       0x3029
#define EGL_MAX_PBUFFER_HEIGHT          0x302A
#define EGL_MAX_PBUFFER_PIXELS          0x302B
#define EGL_MAX_PBUFFER_WIDTH           0x302C
#define EGL_NATIVE_RENDERABLE           0x302D
#define EGL_NATIVE_VISUAL_ID            0x302E
#define EGL_NATIVE_VISUAL_TYPE          0x302F
#define EGL_SAMPLES                     0x3031
#define EGL_SAMPLE_BUFFERS              0x3032
#define EGL_SURFACE_TYPE                0x3033
#define EGL_TRANSPARENT_TYPE            0x3034
#define EGL_NONE                        0x3038
#define EGL_BIND_TO_TEXTURE_RGB         0x3039
#define EGL_BIND_TO_TEXTURE_RGBA        0x303A
#define EGL_MIN_SWAP_INTERVAL           0x303B
#define EGL_MAX_SWAP_INTERVAL           0x303C
#define EGL_LUMINANCE_SIZE              0x303D
#define EGL_ALPHA_MASK_SIZE             0x303E
#define EGL_COLOR_BUFFER_TYPE           0x303F
#define EGL_RENDERABLE_TYPE             0x3040
#define EGL_MATCH_NATIVE_PIXMAP         0x3041
#define EGL_CONFORMANT                  0x3042
#define EGL_SLOW_CONFIG                 0x3050
#define EGL_NON_CONFORMANT_CONFIG       0x3051
#define EGL_TRANSPARENT_RGB             0x3052
#define EGL_VENDOR                      0x3053
#define EGL_VERSION                     0x3054
#define EGL_EXTENSIONS                  0x3055
#define EGL_HEIGHT                      0x3056
#define EGL_WIDTH                       0x3057
#define EGL_LARGEST_PBUFFER             0x3058
#define EGL_DRAW                        0x3059
#define EGL_READ                        0x305A
#define EGL_CORE_NATIVE_ENGINE          0x305B
#define EGL_CLIENT_APIS                 0x308D

#define EGL_PBUFFER_BIT                 0x0001
#define EGL_PIXMAP_BIT                  0x0002
#define EGL_WINDOW_BIT                  0x0004
#define EGL_OPENGL_ES_BIT               0x0001
#define EGL_OPENGL_ES2_BIT              0x0004
#define EGL_OPENGL_ES3_BIT              0x0040
#define EGL_RGB_BUFFER                  0x308E

typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);

#define EGL_TRUE 1
#define EGL_FALSE 0

static EGLint g_last_error = EGL_SUCCESS;

EGLint eglGetError(void) {
    EGLint err = g_last_error;
    g_last_error = EGL_SUCCESS;
    return err;
}

EGLDisplay eglGetDisplay(void *display_id) {
    return (EGLDisplay)(uintptr_t)0x1;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    if (major) *major = 1;
    if (minor) *minor = 5;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
    return EGL_TRUE;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name) {
    switch (name) {
        case EGL_VENDOR: return "Android";
        case EGL_VERSION: return "1.5 Android META-EGL";
        case EGL_EXTENSIONS:
            return "EGL_KHR_image_base EGL_KHR_gl_texture_2D_image EGL_ANDROID_image_native_buffer "
                   "EGL_KHR_fence_sync EGL_ANDROID_native_fence_sync EGL_KHR_wait_sync "
                   "EGL_KHR_get_all_proc_addresses EGL_ANDROID_presentation_time "
                   "EGL_KHR_swap_buffers_with_damage EGL_ANDROID_get_native_client_buffer "
                   "EGL_ANDROID_front_buffer_auto_refresh EGL_ANDROID_get_frame_timestamps "
                   "EGL_KHR_no_config_context EGL_KHR_surfaceless_context "
                   "EGL_IMG_context_priority EGL_EXT_protected_content";
        case EGL_CLIENT_APIS: return "OpenGL_ES";
        default: return "";
    }
}

EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    if (!num_config) return EGL_FALSE;
    *num_config = 1;
    if (configs && config_size > 0) {
        configs[0] = (EGLConfig)(uintptr_t)0x1;
    }
    return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    return eglGetConfigs(dpy, configs, config_size, num_config);
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    if (!value) return EGL_FALSE;
    switch (attribute) {
        case EGL_BUFFER_SIZE: *value = 32; break;
        case EGL_RED_SIZE: *value = 8; break;
        case EGL_GREEN_SIZE: *value = 8; break;
        case EGL_BLUE_SIZE: *value = 8; break;
        case EGL_ALPHA_SIZE: *value = 8; break;
        case EGL_DEPTH_SIZE: *value = 24; break;
        case EGL_STENCIL_SIZE: *value = 8; break;
        case EGL_CONFIG_CAVEAT: *value = EGL_NONE; break;
        case EGL_CONFIG_ID: *value = 1; break;
        case EGL_NATIVE_RENDERABLE: *value = EGL_TRUE; break;
        case EGL_NATIVE_VISUAL_ID: *value = 1; break; /* HAL_PIXEL_FORMAT_RGBA_8888 */
        case EGL_NATIVE_VISUAL_TYPE: *value = 0; break;
        case EGL_SURFACE_TYPE: *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
        case EGL_COLOR_BUFFER_TYPE: *value = EGL_RGB_BUFFER; break;
        case EGL_RENDERABLE_TYPE: *value = EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT; break;
        case EGL_CONFORMANT: *value = EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT; break;
        case EGL_MAX_PBUFFER_WIDTH: *value = 4096; break;
        case EGL_MAX_PBUFFER_HEIGHT: *value = 4096; break;
        case EGL_MAX_PBUFFER_PIXELS: *value = 4096 * 4096; break;
        case EGL_MIN_SWAP_INTERVAL: *value = 0; break;
        case EGL_MAX_SWAP_INTERVAL: *value = 1; break;
        default: *value = 0; break;
    }
    return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list) {
    return (EGLContext)(uintptr_t)0x1;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void *win, const EGLint *attrib_list) {
    return (EGLSurface)(uintptr_t)0x1;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
    return (EGLSurface)(uintptr_t)0x1;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    return EGL_TRUE;
}

EGLBoolean eglReleaseThread(void) {
    return EGL_TRUE;
}

EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value) {
    if (value) *value = 0;
    return EGL_TRUE;
}

EGLDisplay eglGetCurrentDisplay(void) {
    return (EGLDisplay)(uintptr_t)0x1;
}

EGLContext eglGetCurrentContext(void) {
    return (EGLContext)(uintptr_t)0x1;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw) {
    return (EGLSurface)(uintptr_t)0x1;
}

void *eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, unsigned int target, void *buffer, const EGLint *attrib_list) {
    return (void *)(uintptr_t)0x2;
}

void *eglCreateImage(EGLDisplay dpy, EGLContext ctx, unsigned int target, void *buffer, const EGLint *attrib_list) {
    return (void *)(uintptr_t)0x2;
}

EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, void *image) {
    return EGL_TRUE;
}

EGLBoolean eglDestroyImage(EGLDisplay dpy, void *image) {
    return EGL_TRUE;
}

void *eglCreateSyncKHR(EGLDisplay dpy, unsigned int type, const EGLint *attrib_list) {
    return (void *)(uintptr_t)0x1;
}

EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, void *sync) {
    return EGL_TRUE;
}

EGLint eglClientWaitSyncKHR(EGLDisplay dpy, void *sync, EGLint flags, uint64_t timeout) {
    return 0x30F6; /* EGL_CONDITION_SATISFIED_KHR */
}

EGLint eglWaitSyncKHR(EGLDisplay dpy, void *sync, EGLint flags) {
    return EGL_TRUE;
}

EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, void *sync, EGLint attribute, EGLint *value) {
    if (value && attribute == 0x30F1 /* EGL_SYNC_STATUS_KHR */) {
        *value = 0x30F2; /* EGL_SIGNALED_KHR */
    }
    return EGL_TRUE;
}

EGLint eglDupNativeFenceFDANDROID(EGLDisplay dpy, void *sync) {
    return -1;
}

void *eglGetNativeClientBufferANDROID(const void *buffer) {
    return (void *)buffer;
}

static const char *g_gl_extensions[] = {
    "GL_OES_EGL_image",
    "GL_OES_EGL_image_external",
    "GL_OES_EGL_image_external_essl3",
    "GL_OES_standard_derivatives",
    "GL_OES_texture_npot",
    "GL_EXT_texture_format_BGRA8888",
    "GL_EXT_read_format_bgra",
    "GL_OES_rgb8_rgba8",
    "GL_EXT_color_buffer_float",
    "GL_EXT_color_buffer_half_float",
    "GL_OES_depth24",
    "GL_OES_packed_depth_stencil",
    "GL_EXT_protected_content"
};
#define NUM_GL_EXTENSIONS ((int)(sizeof(g_gl_extensions) / sizeof(g_gl_extensions[0])))

/* OpenGL ES Stubs */
const char *glGetString(unsigned int name) {
    switch (name) {
        case 0x1F00: return "Google"; /* GL_VENDOR */
        case 0x1F01: return "Freedreno Adreno 530"; /* GL_RENDERER */
        case 0x1F02: return "OpenGL ES 3.2"; /* GL_VERSION */
        case 0x1F03: return "GL_OES_EGL_image GL_OES_EGL_image_external GL_OES_EGL_image_external_essl3 GL_OES_standard_derivatives GL_OES_texture_npot GL_EXT_texture_format_BGRA8888 GL_EXT_read_format_bgra GL_OES_rgb8_rgba8 GL_EXT_color_buffer_float GL_EXT_color_buffer_half_float GL_OES_depth24 GL_OES_packed_depth_stencil GL_EXT_protected_content";
        case 0x8B8C: return "OpenGL ES GLSL ES 3.20";
        default: return "";
    }
}

const uint8_t *glGetStringi(unsigned int name, unsigned int index) {
    if (name == 0x1F03 /* GL_EXTENSIONS */ && (int)index < NUM_GL_EXTENSIONS) {
        return (const uint8_t *)g_gl_extensions[index];
    }
    return (const uint8_t *)"";
}

void glGetIntegerv(unsigned int pname, int *params) {
    if (!params) return;
    switch (pname) {
        case 0x0D33: *params = 4096; break; /* GL_MAX_TEXTURE_SIZE */
        case 0x0D3A: /* GL_MAX_VIEWPORT_DIMS */
            params[0] = 4096;
            params[1] = 4096;
            break;
        case 0x84E8: *params = 4096; break; /* GL_MAX_RENDERBUFFER_SIZE */
        case 0x8D57: *params = 4; break;    /* GL_MAX_SAMPLES */
        case 0x0D57: *params = 8; break;    /* GL_STENCIL_BITS */
        case 0x0D52: *params = 8; break;    /* GL_RED_BITS */
        case 0x0D53: *params = 8; break;    /* GL_GREEN_BITS */
        case 0x0D54: *params = 8; break;    /* GL_BLUE_BITS */
        case 0x0D55: *params = 8; break;    /* GL_ALPHA_BITS */
        case 0x0D56: *params = 24; break;   /* GL_DEPTH_BITS */
        case 0x821D: *params = NUM_GL_EXTENSIONS; break; /* GL_NUM_EXTENSIONS */
        case 0x8869: *params = 32; break;   /* GL_MAX_VERTEX_ATTRIBS */
        case 0x8B4C: *params = 64; break;   /* GL_MAX_VERTEX_UNIFORM_VECTORS */
        case 0x8DF8: *params = 64; break;   /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */
        case 0x8872: *params = 16; break;   /* GL_MAX_TEXTURE_IMAGE_UNITS */
        case 0x8B4D: *params = 16; break;   /* GL_MAX_VARYING_VECTORS / GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
        case 0x84E2: *params = 0x84C0; break; /* GL_ACTIVE_TEXTURE */
        case 0x0BA2: /* GL_VIEWPORT */
            params[0] = 0;
            params[1] = 0;
            params[2] = 1080;
            params[3] = 1920;
            break;
        case 0x8824: *params = 8; break;    /* GL_MAX_DRAW_BUFFERS */
        case 0x8CDF: *params = 8; break;    /* GL_MAX_COLOR_ATTACHMENTS */
        case 0x8CA6: *params = 0; break;    /* GL_FRAMEBUFFER_BINDING */
        case 0x0DE0: *params = 4; break;    /* GL_SUBPIXEL_BITS */
        case 0x0BF1: *params = 4; break;    /* GL_MAX_SAMPLES */
        default: *params = 1; break;
    }
}

void glGetInternalformativ(unsigned int target, unsigned int internalformat, unsigned int pname, int count, int *params) {
    if (!params || count <= 0) return;
    for (int i = 0; i < count; i++) {
        params[i] = 1;
    }
}

void glGetTexParameteriv(unsigned int target, unsigned int pname, int *params) {
    if (params) *params = 1;
}

void glGetTexParameterfv(unsigned int target, unsigned int pname, float *params) {
    if (params) *params = 1.0f;
}

void glGetFloatv(unsigned int pname, float *params) {
    if (params) *params = 1.0f;
}

void glGetBooleanv(unsigned int pname, uint8_t *params) {
    if (params) *params = 1;
}

unsigned int glGetError(void) { return 0; }
unsigned int glCreateShader(unsigned int type) { return 1; }
unsigned int glCreateProgram(void) { return 1; }
unsigned int glCheckFramebufferStatus(unsigned int target) { return 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */ }
int glGetUniformLocation(unsigned int program, const char *name) { return 0; }
int glGetAttribLocation(unsigned int program, const char *name) { return 0; }

void glGetShaderiv(unsigned int shader, unsigned int pname, int *params) {
    if (params) *params = 1; /* GL_COMPILE_STATUS = GL_TRUE */
}

void glGetProgramiv(unsigned int program, unsigned int pname, int *params) {
    if (params) *params = 1; /* GL_LINK_STATUS = GL_TRUE */
}

void glGetShaderInfoLog(unsigned int shader, int maxLength, int *length, char *infoLog) {
    if (length) *length = 0;
    if (infoLog && maxLength > 0) infoLog[0] = '\0';
}

void glGetProgramInfoLog(unsigned int program, int maxLength, int *length, char *infoLog) {
    if (length) *length = 0;
    if (infoLog && maxLength > 0) infoLog[0] = '\0';
}

static unsigned int g_next_tex_id = 1;
void glGenTextures(int n, unsigned int *textures) {
    if (!textures) return;
    for (int i = 0; i < n; i++) {
        textures[i] = g_next_tex_id++;
    }
}
void glDeleteTextures(int n, const unsigned int *textures) {}
void glBindTexture(unsigned int target, unsigned int texture) {}

static unsigned int g_next_fb_id = 1;
void glGenFramebuffers(int n, unsigned int *framebuffers) {
    if (!framebuffers) return;
    for (int i = 0; i < n; i++) {
        framebuffers[i] = g_next_fb_id++;
    }
}
void glDeleteFramebuffers(int n, const unsigned int *framebuffers) {}
void glBindFramebuffer(unsigned int target, unsigned int framebuffer) {}

static unsigned int g_next_rb_id = 1;
void glGenRenderbuffers(int n, unsigned int *renderbuffers) {
    if (!renderbuffers) return;
    for (int i = 0; i < n; i++) {
        renderbuffers[i] = g_next_rb_id++;
    }
}
void glDeleteRenderbuffers(int n, const unsigned int *renderbuffers) {}
void glBindRenderbuffer(unsigned int target, unsigned int renderbuffer) {}

static unsigned int g_next_buf_id = 1;
void glGenBuffers(int n, unsigned int *buffers) {
    if (!buffers) return;
    for (int i = 0; i < n; i++) {
        buffers[i] = g_next_buf_id++;
    }
}
void glDeleteBuffers(int n, const unsigned int *buffers) {}
void glBindBuffer(unsigned int target, unsigned int buffer) {}

void glFramebufferTexture2D(unsigned int target, unsigned int attachment, unsigned int textarget, unsigned int texture, int level) {}
void glFramebufferRenderbuffer(unsigned int target, unsigned int attachment, unsigned int renderbuffertarget, unsigned int renderbuffer) {}
void glEGLImageTargetTexture2DOES(unsigned int target, void *image) {}
void glEGLImageTargetRenderbufferStorageOES(unsigned int target, void *image) {}
void glTexParameteri(unsigned int target, unsigned int pname, int param) {}
void glTexParameterf(unsigned int target, unsigned int pname, float param) {}
void glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border, unsigned int format, unsigned int type, const void *pixels) {}
void glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset, int width, int height, unsigned int format, unsigned int type, const void *pixels) {}
void glViewport(int x, int y, int width, int height) {}
void glClear(unsigned int mask) {}
void glClearColor(float red, float green, float blue, float alpha) {}
void glDrawArrays(unsigned int mode, int first, int count) {}
void glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices) {}
void glFlush(void) {}
void glFinish(void) {}

void glReadPixels(int x, int y, int width, int height, unsigned int format, unsigned int type, void *pixels) {
    if (!pixels || width <= 0 || height <= 0) return;
    unsigned char *p = (unsigned char *)pixels;
    int total = width * height * 4;
    for (int i = 0; i < total; i += 4) {
        p[i] = 0x20;     /* R */
        p[i + 1] = 0x20; /* G */
        p[i + 2] = 0x20; /* B */
        p[i + 3] = 0xff; /* A */
    }
}

void *glStub(void) {
    return (void *)1;
}

static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) {
    if (!procname) return NULL;
    if (my_strcmp(procname, "eglCreateImageKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglCreateImageKHR;
    if (my_strcmp(procname, "eglCreateImage") == 0) return (__eglMustCastToProperFunctionPointerType)eglCreateImage;
    if (my_strcmp(procname, "eglDestroyImageKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglDestroyImageKHR;
    if (my_strcmp(procname, "eglDestroyImage") == 0) return (__eglMustCastToProperFunctionPointerType)eglDestroyImage;
    if (my_strcmp(procname, "eglCreateSyncKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglCreateSyncKHR;
    if (my_strcmp(procname, "eglCreateSync") == 0) return (__eglMustCastToProperFunctionPointerType)eglCreateSyncKHR;
    if (my_strcmp(procname, "eglDestroySyncKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglDestroySyncKHR;
    if (my_strcmp(procname, "eglDestroySync") == 0) return (__eglMustCastToProperFunctionPointerType)eglDestroySyncKHR;
    if (my_strcmp(procname, "eglClientWaitSyncKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglClientWaitSyncKHR;
    if (my_strcmp(procname, "eglClientWaitSync") == 0) return (__eglMustCastToProperFunctionPointerType)eglClientWaitSyncKHR;
    if (my_strcmp(procname, "eglWaitSyncKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglWaitSyncKHR;
    if (my_strcmp(procname, "eglWaitSync") == 0) return (__eglMustCastToProperFunctionPointerType)eglWaitSyncKHR;
    if (my_strcmp(procname, "eglGetSyncAttribKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglGetSyncAttribKHR;
    if (my_strcmp(procname, "eglSwapInterval") == 0) return (__eglMustCastToProperFunctionPointerType)eglSwapInterval;
    if (my_strcmp(procname, "eglSwapBuffers") == 0) return (__eglMustCastToProperFunctionPointerType)eglSwapBuffers;
    if (my_strcmp(procname, "eglCreateWindowSurface") == 0) return (__eglMustCastToProperFunctionPointerType)eglCreateWindowSurface;
    if (my_strcmp(procname, "eglDupNativeFenceFDANDROID") == 0) return (__eglMustCastToProperFunctionPointerType)eglDupNativeFenceFDANDROID;
    if (my_strcmp(procname, "eglGetNativeClientBufferANDROID") == 0) return (__eglMustCastToProperFunctionPointerType)eglGetNativeClientBufferANDROID;
    if (my_strcmp(procname, "glGetString") == 0) return (__eglMustCastToProperFunctionPointerType)glGetString;
    if (my_strcmp(procname, "glGetStringi") == 0) return (__eglMustCastToProperFunctionPointerType)glGetStringi;
    if (my_strcmp(procname, "glGetIntegerv") == 0) return (__eglMustCastToProperFunctionPointerType)glGetIntegerv;
    if (my_strcmp(procname, "glGetInternalformativ") == 0) return (__eglMustCastToProperFunctionPointerType)glGetInternalformativ;
    if (my_strcmp(procname, "glGetTexParameteriv") == 0) return (__eglMustCastToProperFunctionPointerType)glGetTexParameteriv;
    if (my_strcmp(procname, "glGetTexParameterfv") == 0) return (__eglMustCastToProperFunctionPointerType)glGetTexParameterfv;
    if (my_strcmp(procname, "glGetFloatv") == 0) return (__eglMustCastToProperFunctionPointerType)glGetFloatv;
    if (my_strcmp(procname, "glGetBooleanv") == 0) return (__eglMustCastToProperFunctionPointerType)glGetBooleanv;
    if (my_strcmp(procname, "glGetError") == 0) return (__eglMustCastToProperFunctionPointerType)glGetError;
    if (my_strcmp(procname, "eglGetCurrentDisplay") == 0) return (__eglMustCastToProperFunctionPointerType)eglGetCurrentDisplay;
    if (my_strcmp(procname, "eglGetCurrentContext") == 0) return (__eglMustCastToProperFunctionPointerType)eglGetCurrentContext;
    if (my_strcmp(procname, "eglGetCurrentSurface") == 0) return (__eglMustCastToProperFunctionPointerType)eglGetCurrentSurface;
    if (my_strcmp(procname, "glGenTextures") == 0) return (__eglMustCastToProperFunctionPointerType)glGenTextures;
    if (my_strcmp(procname, "glDeleteTextures") == 0) return (__eglMustCastToProperFunctionPointerType)glDeleteTextures;
    if (my_strcmp(procname, "glBindTexture") == 0) return (__eglMustCastToProperFunctionPointerType)glBindTexture;
    if (my_strcmp(procname, "glGenFramebuffers") == 0) return (__eglMustCastToProperFunctionPointerType)glGenFramebuffers;
    if (my_strcmp(procname, "glDeleteFramebuffers") == 0) return (__eglMustCastToProperFunctionPointerType)glDeleteFramebuffers;
    if (my_strcmp(procname, "glBindFramebuffer") == 0) return (__eglMustCastToProperFunctionPointerType)glBindFramebuffer;
    if (my_strcmp(procname, "glCheckFramebufferStatus") == 0) return (__eglMustCastToProperFunctionPointerType)glCheckFramebufferStatus;
    if (my_strcmp(procname, "glReadPixels") == 0) return (__eglMustCastToProperFunctionPointerType)glReadPixels;
    if (my_strcmp(procname, "glGenRenderbuffers") == 0) return (__eglMustCastToProperFunctionPointerType)glGenRenderbuffers;
    if (my_strcmp(procname, "glDeleteRenderbuffers") == 0) return (__eglMustCastToProperFunctionPointerType)glDeleteRenderbuffers;
    if (my_strcmp(procname, "glBindRenderbuffer") == 0) return (__eglMustCastToProperFunctionPointerType)glBindRenderbuffer;
    if (my_strcmp(procname, "glGenBuffers") == 0) return (__eglMustCastToProperFunctionPointerType)glGenBuffers;
    if (my_strcmp(procname, "glDeleteBuffers") == 0) return (__eglMustCastToProperFunctionPointerType)glDeleteBuffers;
    if (my_strcmp(procname, "glBindBuffer") == 0) return (__eglMustCastToProperFunctionPointerType)glBindBuffer;
    if (my_strcmp(procname, "glFramebufferTexture2D") == 0) return (__eglMustCastToProperFunctionPointerType)glFramebufferTexture2D;
    if (my_strcmp(procname, "glFramebufferRenderbuffer") == 0) return (__eglMustCastToProperFunctionPointerType)glFramebufferRenderbuffer;
    if (my_strcmp(procname, "glEGLImageTargetTexture2DOES") == 0) return (__eglMustCastToProperFunctionPointerType)glEGLImageTargetTexture2DOES;
    if (my_strcmp(procname, "glEGLImageTargetRenderbufferStorageOES") == 0) return (__eglMustCastToProperFunctionPointerType)glEGLImageTargetRenderbufferStorageOES;
    if (my_strcmp(procname, "glTexParameteri") == 0) return (__eglMustCastToProperFunctionPointerType)glTexParameteri;
    if (my_strcmp(procname, "glTexParameterf") == 0) return (__eglMustCastToProperFunctionPointerType)glTexParameterf;
    if (my_strcmp(procname, "glTexImage2D") == 0) return (__eglMustCastToProperFunctionPointerType)glTexImage2D;
    if (my_strcmp(procname, "glTexSubImage2D") == 0) return (__eglMustCastToProperFunctionPointerType)glTexSubImage2D;
    if (my_strcmp(procname, "glViewport") == 0) return (__eglMustCastToProperFunctionPointerType)glViewport;
    if (my_strcmp(procname, "glClear") == 0) return (__eglMustCastToProperFunctionPointerType)glClear;
    if (my_strcmp(procname, "glClearColor") == 0) return (__eglMustCastToProperFunctionPointerType)glClearColor;
    if (my_strcmp(procname, "glDrawArrays") == 0) return (__eglMustCastToProperFunctionPointerType)glDrawArrays;
    if (my_strcmp(procname, "glDrawElements") == 0) return (__eglMustCastToProperFunctionPointerType)glDrawElements;
    if (my_strcmp(procname, "glFlush") == 0) return (__eglMustCastToProperFunctionPointerType)glFlush;
    if (my_strcmp(procname, "glFinish") == 0) return (__eglMustCastToProperFunctionPointerType)glFinish;
    return (__eglMustCastToProperFunctionPointerType)glStub;
}
