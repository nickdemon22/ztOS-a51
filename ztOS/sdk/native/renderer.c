// SPDX-License-Identifier: GPL-2.0-or-later
/* Qt owns the context and frame clock. C owns textures, geometry and GLES draws.
 * No Python callbacks or GL readbacks occur inside a render submission. */
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "zrnmetrorender.h"

struct renderer { GLuint program, vbo; GLint alpha; };
/* ABI 1: texture ID, normalized source rectangle, four homogeneous (x,y,w)
 * corners in logical points, opacity. TL, TR, BL, BR. */
struct quad { uint32_t texture; float src[4], corners[12], alpha; };
struct vertex { float p[4], uv[2]; };
unsigned zsdk_abi(void) { return 1; }
void *zsdk_create(void) {
    static const char vs[] = "attribute vec4 pos; attribute vec2 uv; varying vec2 v_uv;"
        "void main(){gl_Position=pos;v_uv=uv;}";
    static const char fs[] = "#ifdef GL_ES\nprecision mediump float;\n#endif\nvarying vec2 v_uv;"
        "uniform sampler2D tex; uniform float alpha;"
        "void main(){gl_FragColor=texture2D(tex,v_uv)*alpha;}";
    GLuint v = zmr_shader(GL_VERTEX_SHADER, vs), f = zmr_shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { if(v)glDeleteShader(v); if(f)glDeleteShader(f); return NULL; }
    struct renderer *r = calloc(1, sizeof(*r));
    if (!r) { glDeleteShader(v); glDeleteShader(f); return NULL; }
    r->program = glCreateProgram();
    glAttachShader(r->program,v); glAttachShader(r->program,f);
    glBindAttribLocation(r->program,0,"pos"); glBindAttribLocation(r->program,1,"uv");
    glLinkProgram(r->program); glDeleteShader(v); glDeleteShader(f);
    GLint ok; glGetProgramiv(r->program,GL_LINK_STATUS,&ok);
    if (!ok) { glDeleteProgram(r->program); free(r); return NULL; }
    glUseProgram(r->program);
    glUniform1i(glGetUniformLocation(r->program,"tex"),0);
    r->alpha = glGetUniformLocation(r->program,"alpha");
    glGenBuffers(1,&r->vbo);
    fprintf(stderr,"ztos: native C GLES renderer: %s (ABI 1)\n",glGetString(GL_RENDERER));
    return r;
}
void zsdk_destroy(void *ptr) {
    struct renderer *r=ptr; if(!r)return;
    glDeleteBuffers(1,&r->vbo); glDeleteProgram(r->program); free(r);
}
unsigned zsdk_max_texture(void) { GLint n=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&n);return n; }
unsigned zsdk_upload(int w,int h,int stride,const unsigned char *rgba) {
    if(w<=0 || h<=0 || w>(int)zsdk_max_texture() || h>(int)zsdk_max_texture() || stride<w*4)return 0;
    unsigned char *packed=NULL;
    if(stride!=w*4){
        packed=malloc((size_t)w*h*4);if(!packed)return 0;
        for(int y=0;y<h;y++)memcpy(packed+(size_t)y*w*4,rgba+(size_t)y*stride,(size_t)w*4);
        rgba=packed;
    }
    GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    free(packed);return t;
}
void zsdk_drop(unsigned t){glDeleteTextures(1,&t);}
void zsdk_draw(void *ptr,float w,float h,int pw,int ph,const struct quad *quads,unsigned n){
    struct renderer *r=ptr;
    glViewport(0,0,pw,ph);glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);
    glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    if(!r || w<=0 || h<=0 || !n)return;
    struct vertex *v=malloc((size_t)n*6*sizeof(*v));if(!v)return;
    static const unsigned order[]={0,1,2,2,1,3};
    for(unsigned i=0;i<n;i++)for(unsigned k=0;k<6;k++){
        unsigned c=order[k];const float *p=&quads[i].corners[c*3];
        struct vertex *a=&v[i*6+k];
        a->p[0]=2*p[0]/w-p[2];a->p[1]=p[2]-2*p[1]/h;a->p[2]=0;a->p[3]=p[2];
        a->uv[0]=quads[i].src[c%2?2:0];a->uv[1]=quads[i].src[c>=2?3:1];
    }
    glUseProgram(r->program);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);glBindBuffer(GL_ARRAY_BUFFER,r->vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)((size_t)n*6*sizeof(*v)),v,GL_STREAM_DRAW);free(v);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,sizeof(struct vertex),(void *)0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(struct vertex),(void *)(4*sizeof(float)));
    for(unsigned i=0;i<n;i++){
        if(!quads[i].texture || quads[i].alpha<=0.01f)continue;
        glUniform1f(r->alpha,fminf(1,quads[i].alpha));
        glBindTexture(GL_TEXTURE_2D,quads[i].texture);glDrawArrays(GL_TRIANGLES,(GLint)i*6,6);
    }
    glDisableVertexAttribArray(0);glDisableVertexAttribArray(1);glBindBuffer(GL_ARRAY_BUFFER,0);glUseProgram(0);
}
/* True perspective geometry reused for SDK turnstile rows. */
void zsdk_turn(float w,float h,float y0,float y1,float angle,float travel,float depth,float *corners){
    double eye=(h/2.0)/tan(34.0*3.141592653589793/360.0);
    double a=angle*0.017453292519943295,sa=sin(a),ca=cos(a),cx=w/2.0,cy=(y0+y1)/2.0;
    for(int i=0;i<4;i++){
        double x=(i%2)?w:0,y=(i>=2)?y1:y0,den=(eye-depth+x*sa)/eye;
        corners[i*3]=x*(cx*sa+eye*ca)/eye-cx*depth/eye+travel*den;
        corners[i*3+1]=(x*cy*sa+eye*y-cy*depth)/eye;
        corners[i*3+2]=den;
    }
}
void zsdk_entrance(double t,double h,double *state){
    double eye=(h/2.0)/tan(34.0*3.141592653589793/360.0);
    zmr_entrance(t,eye,&state[0],&state[1],&state[2],&state[3]);
}
