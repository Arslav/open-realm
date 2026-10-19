#include "r_local.h"
#ifdef BZ_ANGLE_VULKAN
#include <vulkan/vulkan.h>
#endif

static bool graphics_vulkan;

#ifdef BZ_ANGLE_VULKAN
/* The optional choice must have a hardware device, not just a Vulkan loader. */
static bool R_HardwareVulkanAvailable(void) {
    VkInstance instance = VK_NULL_HANDLE;
    VkInstanceCreateInfo create = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    uint32_t count = 0;
    bool available = false;
    VkResult status = vkCreateInstance(&create, NULL, &instance);
    if (status != VK_SUCCESS) {
        fprintf(stderr, "Renderer: Vulkan inventory unavailable (vkCreateInstance=%d)\n", status); return false;
    }
    status = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (status != VK_SUCCESS) fprintf(stderr, "Renderer: Vulkan device count failed (%d)\n", status);
    if (status == VK_SUCCESS && count) {
        VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
        if (!devices) { fprintf(stderr, "Renderer: cannot allocate Vulkan device inventory\n"); vkDestroyInstance(instance, NULL); return false; }
        status = vkEnumeratePhysicalDevices(instance, &count, devices);
        if (status != VK_SUCCESS) fprintf(stderr, "Renderer: Vulkan device inventory failed (%d)\n", status);
        if (status == VK_SUCCESS) {
            FOR_LOOP(i, count) {
                VkPhysicalDeviceProperties properties;
                vkGetPhysicalDeviceProperties(devices[i], &properties);
                available |= properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                             properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            }
        }
        free(devices);
    }
    if (status == VK_SUCCESS && !available) fprintf(stderr, "Renderer: no hardware Vulkan device available\n");
    vkDestroyInstance(instance, NULL);
    return available;
}
#endif

/* Probe once on the owner thread; a missing optional package remains visible in diagnostics. */
uint32_t R_GraphicsBackends(void) {
    static uint32_t available;
    if (available) return available;
    available = GRAPHICS_BACKEND_GL;
#ifdef BZ_ANGLE_VULKAN
    void *gles = SDL_LoadObject("libGLESv2_angle.so");
    if (!gles) fprintf(stderr, "Renderer: packaged ANGLE GLES unavailable: %s\n", SDL_GetError());
    void *egl = gles ? SDL_LoadObject("libEGL_angle.so") : NULL;
    if (gles && !egl) fprintf(stderr, "Renderer: packaged ANGLE EGL unavailable: %s\n", SDL_GetError());
    if (gles && egl && R_HardwareVulkanAvailable()) available |= GRAPHICS_BACKEND_VULKAN;
    if (egl) SDL_UnloadObject(egl);
    if (gles) SDL_UnloadObject(gles);
    SDL_ClearError();
#endif
    return available;
}

/* SDL and all renderer entry points must use the same EGL/GLES implementation. */
bool R_SelectGraphicsBackend(void) {
    static struct { cstring_t name, egl, gles; uint32_t bit; } const backends[] = {
        { "gl", "libEGL.so", "libGLESv2.so", GRAPHICS_BACKEND_GL },
        { "vulkan", "libEGL_angle.so", "libGLESv2_angle.so", GRAPHICS_BACKEND_VULKAN },
    };
    cstring_t requested = ri.CvarString("r_backend", "gl");
    FOR_LOOP(i, sizeof(backends) / sizeof(*backends)) {
        if (strcmp(requested, backends[i].name)) continue;
        if (!(R_GraphicsBackends() & backends[i].bit)) {
            fprintf(stderr, "Renderer: requested backend '%s' is unavailable on this build/device\n", requested); return false;
        }
        graphics_vulkan = backends[i].bit == GRAPHICS_BACKEND_VULKAN;
#ifdef BZ_GL_DYNAMIC
        if (SDL_setenv("SDL_VIDEO_GL_DRIVER", backends[i].gles, 1) ||
            SDL_setenv("SDL_VIDEO_EGL_DRIVER", backends[i].egl, 1) ||
            (graphics_vulkan && SDL_setenv("ANGLE_DEFAULT_PLATFORM", "vulkan", 1))) {
            fprintf(stderr, "Renderer: cannot select graphics libraries: %s\n", SDL_GetError()); return false;
        }
#endif
        fprintf(stderr, "Renderer: requested backend %s\n", graphics_vulkan ? "Vulkan (ANGLE)" : "OpenGL/GLES");
        return true;
    }
    fprintf(stderr, "Renderer: unknown r_backend '%s' (expected gl or vulkan)\n", requested);
    return false;
}

/* A requested Vulkan path must not quietly restart as system GLES. */
bool R_VerifyGraphicsBackend(void) {
#ifdef BZ_GL_DYNAMIC
    if (!R_GLLoadDispatch()) return false;
#endif
    cstring_t renderer = (cstring_t)glGetString(GL_RENDERER);
    if (!renderer || (graphics_vulkan && (!strstr(renderer, "ANGLE") || !strstr(renderer, "Vulkan")))) {
        fprintf(stderr, "Renderer: backend verification failed: requested=%s GL_RENDERER=%s\n",
                graphics_vulkan ? "vulkan" : "gl", renderer ? renderer : "(null)"); return false;
    }
    return true;
}
