#include "r_gl_dispatch.h"

#ifdef BZ_GL_DYNAMIC
#include <SDL2/SDL.h>
#include "r_gl_api.h"

glDispatch_t bz_gl;
static glFunctionDef_t const gl_functions[] = {
#define BZ_GL_ENTRY(NAME) { "gl" #NAME, offsetof(glDispatch_t, NAME) },
#include "r_gl_api.inc"
#undef BZ_GL_ENTRY
};
#endif

bool R_GLLoadFunctions(glFunctionLoad_t const *load) {
    if (!load || !load->table || !load->size || !load->resolve || IS_ARRAY_EMPTY(load->entries)) {
        fprintf(stderr, "Renderer: invalid GL function table\n"); return false;
    }
    memset(load->table, 0, load->size);
    FOR_EACH_ARRAY(glFunctionDef_t const, entry, load->entries) {
        glFunction_t function = load->resolve(entry->name);
        if (!function || load->size < sizeof(function) || entry->offset > load->size - sizeof(function)) {
            fprintf(stderr, "Renderer: required GL entry '%s' is unavailable\n", entry->name);
            memset(load->table, 0, load->size); return false;
        }
        memcpy((char *)load->table + entry->offset, &function, sizeof(function));
    }
    return true;
}

#ifdef BZ_GL_DYNAMIC
static glFunction_t R_GLResolveFunction(cstring_t name) { return (glFunction_t)SDL_GL_GetProcAddress(name); }

bool R_GLLoadDispatch(void) {
    return R_GLLoadFunctions(&(glFunctionLoad_t){
        .table = &bz_gl, .size = sizeof(bz_gl),
        .entries = gl_functions, .entries_count = sizeof(gl_functions) / sizeof(*gl_functions),
        .resolve = R_GLResolveFunction,
    });
}

void R_GLClearDispatch(void) { memset(&bz_gl, 0, sizeof(bz_gl)); }
#endif
