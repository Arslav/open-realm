/* Compile the real GLES dispatch with checked calls too: GetError() must not
 * expand a GL dispatch member into a statement. This object is never linked. */
#include "renderer/r_local.h"

GLenum BZ_CheckGLDispatch(void) {
    R_Call(glBindBuffer, GL_ARRAY_BUFFER, 0);
    return glGetError();
}
