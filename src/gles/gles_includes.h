#pragma once
// OpenGL ES 2 headers: vitaGL on the PS Vita, the Khronos headers (Mesa) elsewhere.
#if defined(__vita__)
#include <vitaGL.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif
