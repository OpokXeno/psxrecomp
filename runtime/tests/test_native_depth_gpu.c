/* Exercise production FP32 depth helpers and capture resolve shaders with a
 * hidden GL context, without including private renderer implementation. */
#define GL_GLEXT_PROTOTYPES 1
#include "psx_sdl.h"
#ifdef PSX_SDL3
#include <SDL3/SDL_opengl.h>
#else
#include <SDL2/SDL_opengl.h>
#endif
#include "native_depth.h"
#include "native_vk_depth_bounds.h"
#include "depth_test_shaders.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static GLuint compile(GLenum stage, const char *source) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
    GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[8192]; glGetShaderInfoLog(shader, sizeof(log), NULL, log); fprintf(stderr, "%s\n", log); }
    assert(ok); return shader;
}
static GLuint program(const char *vertex, const char *fragment) {
    GLuint p = glCreateProgram();
    GLuint a = compile(vertex ? GL_VERTEX_SHADER : GL_COMPUTE_SHADER, vertex ? vertex : fragment);
    glAttachShader(p, a); glDeleteShader(a);
    if (vertex) { a = compile(GL_FRAGMENT_SHADER, fragment); glAttachShader(p, a); glDeleteShader(a); }
    glLinkProgram(p); GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[8192]; glGetProgramInfoLog(p, sizeof(log), NULL, log); fprintf(stderr, "%s\n", log); }
    assert(ok); return p;
}
static GLuint image(GLenum format, GLenum external, GLenum type, int w, int h, const void *data) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, external, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); return t;
}
static void check_planes(void) {
    const char *vertex = "#version 330\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
    GLuint p = program(vertex, glsl_native_depth_probe_frag), fbo, vao;
    enum { W = 24, H = 16 };
    GLuint t = image(GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, W, H, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao); glViewport(0, 0, W, H); glUseProgram(p);
    const float planes[][3] = {{1.0f/60000.0f,0,0}, {1.0f/60001.0f,0,0}, {0.001f,0.00003f,-0.00001f}, {0.1f,10.0f,-20.0f}};
    const unsigned biases[] = {0, 5, 6, 16};
    unsigned samples = 0;
    for (int scale = 1; scale <= 8; ++scale) for (unsigned n = 0; n < 4; ++n) for (unsigned b = 0; b < 4; ++b) {
        const float seed = planes[n][0], dx = planes[n][1]/scale, dy = planes[n][2]/scale;
        const int ox = -3, oy = 2;
        const uint32_t stored = n % 2 ? native_depth_encode(1.0f/60000.0f) : 0u;
        glUniform4i(glGetUniformLocation(p, "plane"), (int32_t)native_depth_encode(seed),
                    (int32_t)native_depth_encode(dx), (int32_t)native_depth_encode(dy), scale);
        glUniform2i(glGetUniformLocation(p, "origin"), ox*scale, oy*scale);
        glUniform1ui(glGetUniformLocation(p, "stored"), stored);
        glUniform1ui(glGetUniformLocation(p, "bias"), biases[b]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        uint32_t actual[H][W][4]; glReadPixels(0, 0, W, H, GL_RGBA_INTEGER, GL_UNSIGNED_INT, actual);
        assert(glGetError() == GL_NO_ERROR);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const uint32_t key = actual[y][x][0];
            const uint32_t reference = native_depth_evaluate(seed, dx, dy, x-ox*scale, y-oy*scale);
            const uint32_t tested = native_depth_biased(key, biases[b]), threshold = native_depth_threshold(stored);
            uint32_t low, high;
            assert(nvk_depth_interval(seed, planes[n][1], planes[n][2], ox, oy, scale, x, y, x+1, y+1, &low, &high));
            assert(key >= low && key <= high);
            /* GL permits a different FMA rounding than C's fmaf. Flat
             * distant surfaces must still retain the exact FP32 value. */
            if (n < 2) assert(key == reference);
            assert(actual[y][x][1] == tested);
            assert(actual[y][x][2] == threshold);
            assert(actual[y][x][3] == (uint32_t)(tested >= threshold)); ++samples;
        }
    }
    glDeleteVertexArrays(1, &vao); glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &t); glDeleteProgram(p);
    printf("PASS %u actual GPU FP32 plane/bias/tie samples, scales 1..8\n", samples);
}

static void check_setup(void) {
    GLuint p = program(NULL, glsl_native_setup_comp), buffers[2]; glGenBuffers(2, buffers);
    struct { int32_t xyuv[3][4], rgbz[3][4], config[4], depth[4]; } triangle = {
        {{-229376,81920,0,0}, {294912,81920,0,0}, {-229376,606208,0,0}},
        {{128,128,128,512*4096}, {128,128,128,1024*4096}, {128,128,128,2048*4096}},
        {1,0,0,0}, {2,6,1,0}
    };
    glUseProgram(p); glUniform1i(glGetUniformLocation(p, "triangle_count"), 1);
    for (int scale = 1; scale <= 8; ++scale) for (int missing = 0; missing <= 1; ++missing) {
        triangle.config[0] = scale; triangle.rgbz[2][3] = missing ? 0 : 2048*4096;
        uint32_t parameters[20][4] = {{0}};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(triangle), &triangle, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[0]);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(parameters), parameters, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers[1]);
        glDispatchCompute(1, 1, 1); glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(parameters), parameters);
        assert(glGetError() == GL_NO_ERROR);
        assert(parameters[2][0] == (uint32_t)!missing && parameters[2][1] == (missing ? 1u : 2u));
        if (!missing) {
            /* Analytic plane through the three reciprocal depths. Its slope
             * exceeded the old fixed-point limit and used to become flat. */
            const float seed = 1.0f/512 + 1.0f/16384 + 3.0f/65536;
            assert(parameters[4][0] == native_depth_encode(seed));
            assert(parameters[4][1] == native_depth_encode((-1.0f/8192)/scale));
            assert(parameters[4][2] == native_depth_encode((-3.0f/16384)/scale));
            assert(parameters[4][3] == (uint32_t)scale);
            assert((int32_t)parameters[8][1] == -4*scale && parameters[8][2] == (uint32_t)scale);
        }
    }
    glDeleteBuffers(2, buffers); glDeleteProgram(p);
    puts("PASS GPU triangle setup: FP32 seed/gradients, steep/subpixel triangle, missing depth, scales 1..8");
}

static void check_capture(const char *source, int linked) {
    GLuint p = program(NULL, source), buffers[2]; glGenBuffers(2, buffers);
    glUseProgram(p); glUniform4i(glGetUniformLocation(p, "rect"), 0, 0, 1, 1);
    glUniform4ui(glGetUniformLocation(p, "info"), 2, 4, 0, 1);
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        const uint32_t initial = native_depth_encode(1.0f/1000.0f);
        const uint32_t near = native_depth_encode(1.0f/900.0f), far = native_depth_encode(1.0f/1100.0f);
        const uint32_t tie = native_depth_encode(1.0f/1000.5f);
        const uint32_t key = scenario == 0 ? far : scenario == 1 ? tie : near;
        const unsigned bias = scenario == 4 ? 5 : 0;
        const uint32_t draw_key = scenario == 4 ? native_depth_encode(1.0f/1020.0f) : key;
        /* Reject far; accept OT ties; write opaque near; blended near keeps
         * the underlying key; a biased farther decal passes but writes unbiased. */
        const uint32_t policy = (1u<<21) | (2u<<22) | (1u<<25) | (scenario == 3 ? 1u<<16 : 0u) | 31u;
        uint32_t meta[8] = {0}; meta[4] = linked ? 0u : 1u; meta[5] = 1u;
        uint32_t fragment[4] = {1, draw_key, linked ? policy | (bias<<26) : native_depth_biased(draw_key, bias), linked ? UINT32_MAX : policy};
        for (unsigned b = 0; b < 2; ++b) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[b]);
            glBufferData(GL_SHADER_STORAGE_BUFFER, b ? sizeof(fragment) : sizeof(meta), b ? fragment : meta, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, buffers[b]);
        }
        const uint32_t black = 0;
        GLuint color = image(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 1, &black);
        GLuint depth = image(GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 1, 1, &initial);
        glBindImageTexture(2, color, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
        glBindImageTexture(3, depth, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glDispatchCompute(1, 1, 1); glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        uint32_t actual; glBindTexture(GL_TEXTURE_2D, depth); glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &actual);
        assert(actual == (scenario == 0 || scenario == 3 ? initial : draw_key));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]); glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(meta), meta);
        assert(meta[0] == 0 && glGetError() == GL_NO_ERROR);
        glDeleteTextures(1, &color); glDeleteTextures(1, &depth);
    }
    glDeleteBuffers(2, buffers); glDeleteProgram(p);
    printf("PASS %s capture: occlusion, OT ties, opacity, blending, bias\n", linked ? "linked" : "contiguous");
}
int main(void) {
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window *window = SDL_CreateWindow("FP32 depth regression", 0, 0, 32, 32, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    assert(window); SDL_GLContext context = SDL_GL_CreateContext(window); assert(context);
    printf("GPU: %s (%s)\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    /* Compile/link the production renderer and compute setup, too. */
    GLuint p = program(glsl_native_gpu_vert, glsl_native_gpu_frag); glDeleteProgram(p);
    check_setup(); check_planes();
    check_capture(glsl_native_capture_resolve_comp, 0); check_capture(glsl_native_capture_linked_comp, 1);
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit(); return 0;
}
