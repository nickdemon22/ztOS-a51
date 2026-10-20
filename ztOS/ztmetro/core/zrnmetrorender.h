// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ZRNMETRORENDER_H
#define ZRNMETRORENDER_H
#include <math.h>
#include <stdio.h>
#include <GLES2/gl2.h>
/* Shared by the native apps and the Python SDK's C renderer. */
static inline GLuint zmr_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "ztOS GLES shader: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
static inline void zmr_entrance(double t, double eye, double *angle,
                               double *depth, double *travel, double *alpha) {
    t = fmax(0.0, fmin(1.0, t));
    double left = 1.0 - t;
    *angle = 69.8 * exp(-2.649 * t) * pow(left, 0.165);
    *depth = -0.182 * eye * exp(-1.656 * t) * left;
    *travel = 110.0 * (-0.476 * exp(-7.157 * t) + 0.193 * exp(-0.849 * t)) * sqrt(left);
    *alpha = (0.7 + 0.3 * cos(*angle * 0.017453292519943295)) * fmin(1.0, t / 0.08);
}
#endif
