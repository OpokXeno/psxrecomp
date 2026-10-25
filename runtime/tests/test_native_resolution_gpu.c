/* Render production geometry at physical pixel sizes, including non-integer
 * X/Y scales. A diagonal must advance at host-pixel precision, rather than
 * repeat the rows of an enlarged guest-resolution image. */
#define GL_GLEXT_PROTOTYPES 1
#include "psx_sdl.h"
#ifdef PSX_SDL3
#include <SDL3/SDL_opengl.h>
#else
#include <SDL2/SDL_opengl.h>
#endif
#include "native_resolution.h"
#include "depth_test_shaders.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static GLuint compile(GLenum stage, const char *source) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
    GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[8192]; glGetShaderInfoLog(shader, sizeof(log), NULL, log); fprintf(stderr, "%s\n", log); }
    assert(ok); return shader;
}

static void check_spans(void) {
    /* Adjacent scanout spans partition every output pixel, including 224
     * active lines mapped to 2160 rows and odd window dimensions. */
    const uint32_t heights[]={601,1080,2160};
    for (unsigned i=0;i<3;++i) {
        uint32_t total=0;
        for(uint32_t y=0;y<224;++y)
            total+=native_resolution_edge(y+1,224,heights[i])-native_resolution_edge(y,224,heights[i]);
        assert(total==heights[i]);
    }
}

static GLuint texture(GLenum format, GLenum external, GLenum type, int w, int h, const void *data) {
    GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
    glTexImage2D(GL_TEXTURE_2D,0,format,w,h,0,external,type,data);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    return t;
}

static void check_raster(GLuint program, unsigned w, unsigned h, unsigned manual_scale) {
    glActiveTexture(GL_TEXTURE0+12);
    GLuint target=texture(GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,w,h,NULL),fbo;
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glViewport(0,0,w,h); glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    const float scale=manual_scale?manual_scale:1.f;
    glUniform4f(glGetUniformLocation(program,"size"),320,224,0.5f/scale-1.f/64.f,scale);
    glUniform4f(glGetUniformLocation(program,"raster"),manual_scale?0.f:320.f/w,manual_scale?0.f:224.f/h,0,0);
    glDrawArrays(GL_TRIANGLES,0,3);
    uint32_t *pixels=malloc((size_t)w*h*sizeof(*pixels)); assert(pixels);
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    assert(glGetError()==GL_NO_ERROR);
    assert((pixels[(h/5)*w+w/5]&0xffffffu)==0xffu);
    assert((pixels[(h*4/5)*w+w*4/5]&0xffffffu)==0u);
    unsigned last=w,plateau=0,max_plateau=0,edges=0;
    for(unsigned y=h/4;y<h*3/4;++y) {
        unsigned right=0;
        for(unsigned x=0;x<w;++x) {
            const uint32_t color=pixels[(size_t)y*w+x]&0xffffffu;
            assert(color==0u||color==0xffu); /* no spatial interpolation */
            if(color)right=x;
        }
        assert(right>0);
        plateau=right==last?plateau+1:1;
        if(plateau>max_plateau)max_plateau=plateau;
        edges+=right!=last; last=right;
    }
    assert(max_plateau<=2u&&edges>h/4);
    printf("PASS %s %ux%u: diagonal resolved on the physical pixel grid\n",manual_scale?"manual":"fixed",w,h);
    free(pixels); glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&target);
}

/* Render one frozen HUD into both framebuffer aliases. These must be the
 * same bytes after scanout, including fractional 480p/720p/1440p row scales. */
static void check_hud_alias(GLuint program, GLuint vbo, unsigned w, unsigned h, unsigned page_rows) {
    const float ry=(float)h/216.f;
    const unsigned pitch=native_resolution_page_pixels(page_rows,h,216u);
    const unsigned storage_h=(512u/page_rows)*pitch+(unsigned)ceilf((512u%page_rows)*ry);
    glActiveTexture(GL_TEXTURE0+12);
    GLuint target=texture(GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,w,storage_h,NULL),fbo;
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    uint32_t glyphs[32*32];
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
        glyphs[y*32+x]=((y%7)<2||(x%9)<3)?0xffffffffu:0xff204060u;
    glActiveTexture(GL_TEXTURE0+2);
    GLuint hud=texture(GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,32,32,glyphs);
    glUniform4i(glGetUniformLocation(program,"flags"),0,0,0,2);
    glUniform4f(glGetUniformLocation(program,"source_raster"),1,1,0,0);
    const size_t bytes=(size_t)w*h*4u;
    uint32_t *pixels[2]={malloc(bytes),malloc(bytes)};assert(pixels[0]&&pixels[1]);
    for(unsigned alias=0;alias<2;++alias){
        const float origin=(float)(alias*page_rows),pixel=(float)(alias*pitch);
        const float v[6][9]={{12,14+origin,0,0},{306,14+origin,1,0},{12,47+origin,0,1},
            {12,47+origin,0,1},{306,14+origin,1,0},{306,47+origin,1,1}};
        glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STATIC_DRAW);
        glUniform4f(glGetUniformLocation(program,"size"),320,512,0,1);
        glUniform4f(glGetUniformLocation(program,"raster"),320.f/w,1.f/ry,0,pixel);
        glUniform4f(glGetUniformLocation(program,"raster_pages"),origin,1,0,0);
        glViewportIndexedf(0,0,pixel,(float)w,512.f*ry);
        glDrawArrays(GL_TRIANGLES,0,6);
        glReadPixels(0,(int)pixel,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels[alias]);
    }
    if(memcmp(pixels[0],pixels[1],bytes)) {
        unsigned count=0;
        for(size_t i=0;i<bytes/4u;++i)if(pixels[0][i]!=pixels[1][i]) {
            if(count<4)fprintf(stderr,"HUD %ux%u page %u mismatch %zu,%zu: %08x/%08x\n",w,h,page_rows,i%w,i/w,pixels[0][i],pixels[1][i]);
            ++count;
        }
        fprintf(stderr,"HUD mismatches: %u\n",count);
        assert(0);
    }
    /* Framebuffer-texture transfers must preserve the same page alignment. */
    glActiveTexture(GL_TEXTURE0+12);
    GLuint copied=texture(GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,w,h,NULL),copy_fbo;
    glGenFramebuffers(1,&copy_fbo);glBindFramebuffer(GL_FRAMEBUFFER,copy_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,copied,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glActiveTexture(GL_TEXTURE0+2);glBindTexture(GL_TEXTURE_2D,target);
    glUniform4f(glGetUniformLocation(program,"size"),320,216,0,1);
    glUniform4f(glGetUniformLocation(program,"raster"),320.f/w,1.f/ry,0,0);
    glUniform4f(glGetUniformLocation(program,"source_raster"),1,512.f*ry/storage_h,0,0);
    glUniform4f(glGetUniformLocation(program,"raster_pages"),0,0,512.f/page_rows,(pitch-page_rows*ry)/storage_h);
    glViewportIndexedf(0,0,0,(float)w,(float)h);
    for(unsigned alias=0;alias<2;++alias){
        const float t=(float)(alias*page_rows)/512.f,b=(float)(alias*page_rows+216u)/512.f;
        const float v[6][9]={{0,0,0,t},{320,0,1,t},{0,216,0,b},{0,216,0,b},{320,0,1,t},{320,216,1,b}};
        glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STATIC_DRAW);
        glDrawArrays(GL_TRIANGLES,0,6);
        glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels[alias]);
    }
    assert(memcmp(pixels[0],pixels[1],bytes)==0);
    assert(glGetError()==GL_NO_ERROR);
    printf("PASS HUD %ux%u, page %u: identical alias and framebuffer-copy pixels\n",w,h,page_rows);
    if(w==640u&&h==480u&&page_rows==224u) {
        /* Negative control: the former global viewport places buffer 224 at
         * a fractional row. The fixture must detect its visible HUD shift. */
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0+2);glBindTexture(GL_TEXTURE_2D,hud);
        glUniform4f(glGetUniformLocation(program,"source_raster"),1,1,0,0);
        glUniform4f(glGetUniformLocation(program,"size"),320,512,0,1);
        glUniform4f(glGetUniformLocation(program,"raster"),320.f/w,1.f/ry,0,0);
        glUniform4f(glGetUniformLocation(program,"raster_pages"),0,1,0,0);
        glViewportIndexedf(0,0,0,(float)w,512.f*ry);
        for(unsigned alias=0;alias<2;++alias) {
            const float origin=(float)(alias*page_rows);
            const float v[6][9]={{12,14+origin,0,0},{306,14+origin,1,0},{12,47+origin,0,1},
                {12,47+origin,0,1},{306,14+origin,1,0},{306,47+origin,1,1}};
            glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STATIC_DRAW);
            glDrawArrays(GL_TRIANGLES,0,6);
            glReadPixels(0,(int)floorf(origin*ry+.5f),w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels[alias]);
        }
        assert(memcmp(pixels[0],pixels[1],bytes)!=0);
        puts("PASS HUD regression: the former unaligned viewport reproduces the pixel shift");
    }
    free(pixels[0]);free(pixels[1]);
    glDeleteFramebuffers(1,&fbo);glDeleteFramebuffers(1,&copy_fbo);
    glDeleteTextures(1,&target);glDeleteTextures(1,&hud);glDeleteTextures(1,&copied);
    glUniform4i(glGetUniformLocation(program,"flags"),0,0,0,0);
    glUniform4f(glGetUniformLocation(program,"raster_pages"),0,0,0,0);
    glUniform4f(glGetUniformLocation(program,"source_raster"),0,0,0,0);
}

int main(void) {
    check_spans(); assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,4); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_Window *window=SDL_CreateWindow("Native resolution regression",0,0,32,32,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    assert(window); SDL_GLContext context=SDL_GL_CreateContext(window); assert(context);
    GLuint program=glCreateProgram(),v=compile(GL_VERTEX_SHADER,glsl_native_gpu_vert),f=compile(GL_FRAGMENT_SHADER,glsl_native_gpu_frag);
    glAttachShader(program,v); glAttachShader(program,f); glLinkProgram(program);
    GLint ok; glGetProgramiv(program,GL_LINK_STATUS,&ok); assert(ok); glUseProgram(program);
    glDeleteShader(v); glDeleteShader(f);
    const char *samplers[]={"words","destination","source_image","destination_depth","source_depth","hd_image","mip_image"};
    uint32_t zero=0; GLuint textures[7];
    for(unsigned i=0;i<7;++i) {
        glUniform1i(glGetUniformLocation(program,samplers[i]),i); glActiveTexture(GL_TEXTURE0+i);
        const int integral=i==0||i==3||i==4;
        textures[i]=texture(integral?GL_R32UI:GL_RGBA8,integral?GL_RED_INTEGER:GL_RGBA,integral?GL_UNSIGNED_INT:GL_UNSIGNED_BYTE,1,1,&zero);
    }
    GLuint words,words_texture; glGenBuffers(1,&words); glBindBuffer(GL_TEXTURE_BUFFER,words);
    glBufferData(GL_TEXTURE_BUFFER,sizeof(zero),&zero,GL_STATIC_DRAW);
    glActiveTexture(GL_TEXTURE0+8); glGenTextures(1,&words_texture); glBindTexture(GL_TEXTURE_BUFFER,words_texture);
    glTexBuffer(GL_TEXTURE_BUFFER,GL_R16UI,words); glUniform1i(glGetUniformLocation(program,"recipe_words"),8);
    glUniform4f(glGetUniformLocation(program,"attribute_origin"),0,0,0,1);
    const float vertices[3][9]={{16,16,0,0,255,0,0,0,0},{304,16,0,0,255,0,0,0,0},{16,208,0,0,255,0,0,0,0}};
    GLuint vao,vbo; glGenVertexArrays(1,&vao); glBindVertexArray(vao); glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);
    const unsigned sizes[]={2,2,4,1},offsets[]={0,2,4,8};
    for(unsigned i=0;i<4;++i) {glEnableVertexAttribArray(i);glVertexAttribPointer(i,sizes[i],GL_FLOAT,GL_FALSE,sizeof(vertices[0]),(void *)(uintptr_t)(offsets[i]*sizeof(float)));}
    const unsigned resolutions[][2]={{801,601},{1280,720},{1920,1080},{2880,2160},{3840,2160}};
    for(unsigned i=0;i<sizeof(resolutions)/sizeof(*resolutions);++i)check_raster(program,resolutions[i][0],resolutions[i][1],0);
    for(unsigned scale=1;scale<=8;scale*=2)check_raster(program,320*scale,224*scale,scale);
    const unsigned hud_sizes[][2]={{640,480},{1280,720},{1920,1080},{2560,1440},{3840,2160}};
    for(unsigned i=0;i<sizeof(hud_sizes)/sizeof(*hud_sizes);++i)
        check_hud_alias(program,vbo,hud_sizes[i][0],hud_sizes[i][1],224u);
    check_hud_alias(program,vbo,640,480,216u);
    check_hud_alias(program,vbo,640,480,256u);
    glDeleteBuffers(1,&vbo);glDeleteBuffers(1,&words);glDeleteTextures(1,&words_texture);
    glDeleteTextures(7,textures);glDeleteVertexArrays(1,&vao);glDeleteProgram(program);
    SDL_GL_DeleteContext(context);SDL_DestroyWindow(window);SDL_Quit();return 0;
}
