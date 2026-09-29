// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

#ifndef NOODLES_RENDER_NOODLES_GL_H
#define NOODLES_RENDER_NOODLES_GL_H

// Keep all platform GL selection in one public header. The matching CMake
// target links OpenGLES on iOS/iPadOS, OpenGL on macOS, GLESv3 on Android,
// and OpenGL plus GLEW on other desktop platforms.
#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IPHONE
    #define NOODLES_GLES 1
    #include <OpenGLES/ES3/gl.h>
    #include <OpenGLES/ES3/glext.h>
  #else
    #define GL_SILENCE_DEPRECATION 1
    #include <OpenGL/gl3.h>
  #endif
#elif defined(__ANDROID__)
  #define NOODLES_GLES 1
  #include <GLES3/gl3.h>
#else
  #define NOODLES_USE_GLEW 1
  #include <GL/glew.h>
#endif

#ifndef NOODLES_GLES
  #define NOODLES_GLES 0
#endif
#ifndef NOODLES_USE_GLEW
  #define NOODLES_USE_GLEW 0
#endif

#endif // NOODLES_RENDER_NOODLES_GL_H
