#ifndef BZ_R_GL_DISPATCH_H
#define BZ_R_GL_DISPATCH_H

#include "common/common.h"

typedef void (*glFunction_t)(void);
typedef struct { cstring_t name; size_t offset; } glFunctionDef_t;
typedef struct {
    void *table;
    size_t size;
    ARRAY(glFunctionDef_t const, entries);
    glFunction_t (*resolve)(cstring_t name);
} glFunctionLoad_t;

/* Owner-thread context initialization. Failure clears the whole table. */
bool R_GLLoadFunctions(glFunctionLoad_t const *load);

#endif
