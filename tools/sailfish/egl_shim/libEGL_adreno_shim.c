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

void *eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, unsigned int target, void *buffer, const EGLint *attrib_list) {
    return (void *)(uintptr_t)0x1;
}

EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, void *image) {
    return EGL_TRUE;
}

void *eglCreateSyncKHR(EGLDisplay dpy, unsigned int type, const EGLint *attrib_list) {
    return (void *)(uintptr_t)0x1;
}

EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, void *sync) {
    return EGL_TRUE;
}

EGLint eglClientWaitSyncKHR(EGLDisplay dpy, void *sync, EGLint flags, uint64_t timeout) {
    return 0x30BC; /* EGL_CONDITION_SATISFIED_KHR */
}

EGLint eglWaitSyncKHR(EGLDisplay dpy, void *sync, EGLint flags) {
    return EGL_TRUE;
}

EGLint eglDupNativeFenceFDANDROID(EGLDisplay dpy, void *sync) {
    return -1;
}

void *eglGetNativeClientBufferANDROID(const void *buffer) {
    return (void *)buffer;
}

/* OpenGL ES Stubs */
const char *glGetString(unsigned int name) {
    switch (name) {
        case 0x1F00: return "Google"; /* GL_VENDOR */
        case 0x1F01: return "Freedreno Adreno 530"; /* GL_RENDERER */
        case 0x1F02: return "OpenGL ES 3.2"; /* GL_VERSION */
        case 0x1F03: return "GL_OES_EGL_image GL_OES_EGL_image_external GL_OES_EGL_image_external_essl3 GL_OES_standard_derivatives GL_OES_texture_npot";
        case 0x8B8C: return "OpenGL ES GLSL ES 3.20";
        default: return "";
    }
}

const uint8_t *glGetStringi(unsigned int name, unsigned int index) {
    return (const uint8_t*)"";
}

void glGetIntegerv(unsigned int pname, int *params) {
    if (!params) return;
    switch (pname) {
        case 0x0D33: *params = 4096; break; /* GL_MAX_TEXTURE_SIZE */
        case 0x8D57: *params = 4096; break; /* GL_MAX_RENDERBUFFER_SIZE */
        case 0x821D: *params = 5; break;    /* GL_NUM_EXTENSIONS */
        case 0x8869: *params = 32; break;   /* GL_MAX_VERTEX_ATTRIBS */
        case 0x8B4C: *params = 64; break;   /* GL_MAX_VERTEX_UNIFORM_VECTORS */
        case 0x8DF8: *params = 64; break;   /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */
        case 0x8872: *params = 16; break;   /* GL_MAX_TEXTURE_IMAGE_UNITS */
        case 0x8B4D: *params = 16; break;   /* GL_MAX_VARYING_VECTORS */
        case 0x84E2: *params = 0x84C0; break; /* GL_ACTIVE_TEXTURE */
        default: *params = 0; break;
    }
}

void glGetFloatv(unsigned int pname, float *params) {
    if (params) *params = 0.0f;
}

void glGetBooleanv(unsigned int pname, uint8_t *params) {
    if (params) *params = 0;
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

void glStub(void) {}

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
    if (my_strcmp(procname, "eglDestroyImageKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglDestroyImageKHR;
    if (my_strcmp(procname, "eglCreateSyncKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglCreateSyncKHR;
    if (my_strcmp(procname, "eglDestroySyncKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglDestroySyncKHR;
    if (my_strcmp(procname, "eglClientWaitSyncKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglClientWaitSyncKHR;
    if (my_strcmp(procname, "eglWaitSyncKHR") == 0) return (__eglMustCastToProperFunctionPointerType)eglWaitSyncKHR;
    if (my_strcmp(procname, "eglDupNativeFenceFDANDROID") == 0) return (__eglMustCastToProperFunctionPointerType)eglDupNativeFenceFDANDROID;
    if (my_strcmp(procname, "eglGetNativeClientBufferANDROID") == 0) return (__eglMustCastToProperFunctionPointerType)eglGetNativeClientBufferANDROID;
    if (my_strcmp(procname, "glGetString") == 0) return (__eglMustCastToProperFunctionPointerType)glGetString;
    if (my_strcmp(procname, "glGetStringi") == 0) return (__eglMustCastToProperFunctionPointerType)glGetStringi;
    if (my_strcmp(procname, "glGetIntegerv") == 0) return (__eglMustCastToProperFunctionPointerType)glGetIntegerv;
    if (my_strcmp(procname, "glGetError") == 0) return (__eglMustCastToProperFunctionPointerType)glGetError;
    return (__eglMustCastToProperFunctionPointerType)glStub;
}
