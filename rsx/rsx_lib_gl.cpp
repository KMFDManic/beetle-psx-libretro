#include "rsx_lib_gl.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> /* exit() */
#include <stddef.h> /* offsetof() */

#include <stdexcept>

#include <glsm/glsmsym.h>

#include <boolean.h>

#include <glsm/glsmsym.h>
#include <vector>
#include <cstdio>
#include <stdint.h>

#include <map>
#include <string>

#include "mednafen/mednafen.h"
#include "mednafen/psx/gpu.h"
#include "libretro_options.h"

#define DRAWBUFFER_IS_EMPTY(x)           ((x)->map_index == 0)
#define DRAWBUFFER_REMAINING_CAPACITY(x) ((x)->capacity - (x)->map_index)
#define DRAWBUFFER_NEXT_INDEX(x)         ((x)->map_start + (x)->map_index)

#ifndef GL_MAP_INVALIDATE_RANGE_BIT
#define GL_MAP_INVALIDATE_RANGE_BIT       0x000
#endif

#include "../rustation-libretro/src/shaders/command_vertex.glsl.h"
#include "../rustation-libretro/src/shaders/command_fragment.glsl.h"
#define FILTER_XBR
#include "../rustation-libretro/src/shaders/command_fragment.glsl.h"
#include "../rustation-libretro/src/shaders/command_vertex.glsl.h"
#undef FILTER_XBR
#define FILTER_SABR
#include "../rustation-libretro/src/shaders/command_fragment.glsl.h"
#undef FILTER_SABR
#define FILTER_BILINEAR
#include "../rustation-libretro/src/shaders/command_fragment.glsl.h"
#undef FILTER_BILINEAR
#define FILTER_3POINT
#include "../rustation-libretro/src/shaders/command_fragment.glsl.h"
#undef FILTER_3POINT
#define FILTER_JINC2
#include "../rustation-libretro/src/shaders/command_fragment.glsl.h"
#undef FILTER_JINC2
#include "../rustation-libretro/src/shaders/output_vertex.glsl.h"
#include "../rustation-libretro/src/shaders/output_fragment.glsl.h"
#include "../rustation-libretro/src/shaders/image_load_vertex.glsl.h"
#include "../rustation-libretro/src/shaders/image_load_fragment.glsl.h"

#include "libretro.h"
#include "libretro_options.h"

#if 0 || defined(__APPLE__)
#define NEW_COPY_RECT
static const GLushort indices[6] = {0, 1, 2, 2, 1, 3};
#else
static const GLushort indices[6] = {0, 1, 2, 1, 2, 3};
#endif

enum VideoClock {
    VideoClock_Ntsc,
    VideoClock_Pal
};

enum FilterMode {
   FILTER_MODE_NEAREST,
   FILTER_MODE_SABR,
   FILTER_MODE_XBR,
   FILTER_MODE_BILINEAR,
   FILTER_MODE_3POINT,
   FILTER_MODE_JINC2
};

static bool has_software_fb = false;

extern "C" unsigned char widescreen_hack;


#ifdef __cplusplus
extern "C"
{
#endif
	extern retro_environment_t environ_cb;
	extern retro_video_refresh_t video_cb;
#ifdef __cplusplus
}
#endif

#define VRAM_WIDTH_PIXELS 1024
#define VRAM_HEIGHT 512
#define VRAM_PIXELS (VRAM_WIDTH_PIXELS * VRAM_HEIGHT)

/// How many vertices we buffer before forcing a draw. Since the
/// indexes are stored on 16bits we need to make sure that the length
/// multiplied by 3 (for triple buffering) doesn't overflow 0xffff.
static const unsigned int VERTEX_BUFFER_LEN = 0x4000;

/// Maximum number of indices for a vertex buffer. Since quads have
/// two duplicated vertices it can be up to 3/2 the vertex buffer
/// length
static const unsigned int INDEX_BUFFER_LEN = ((VERTEX_BUFFER_LEN * 3 + 1) / 2);

struct DrawConfig
{
    uint16_t display_top_left[2];
    uint16_t display_resolution[2];
    bool     display_24bpp;
    bool     display_off;
    int16_t  draw_offset[2];
    uint16_t draw_area_top_left[2];
    uint16_t draw_area_bot_right[2];
};


typedef std::map<std::string, GLint> UniformMap;

struct Program
{
    GLuint id;
    /// Hash map of all the active uniforms in this program
    UniformMap uniforms;
    char *info_log;
};

struct Shader
{
    GLuint id;
    char *info_log;
};

struct Attribute
{
   char name[32];
   size_t offset;
   /// Attribute type (BYTE, UNSIGNED_SHORT, FLOAT etc...)
   GLenum ty;
   GLint components;
};

struct CommandVertex {
    /// Position in PlayStation VRAM coordinates
    float position[4];
    /// RGB color, 8bits per component
    uint8_t color[3];
    /// Texture coordinates within the page
    uint16_t texture_coord[2];
    /// Texture page (base offset in VRAM used for texture lookup)
    uint16_t texture_page[2];
    /// Color Look-Up Table (palette) coordinates in VRAM
    uint16_t clut[2];
    /// Blending mode: 0: no texture, 1: raw-texture, 2: texture-blended
    uint8_t texture_blend_mode;
    /// Right shift from 16bits: 0 for 16bpp textures, 1 for 8bpp, 2
    /// for 4bpp
    uint8_t depth_shift;
    /// True if dithering is enabled for this primitive
    uint8_t dither;
    /// 0: primitive is opaque, 1: primitive is semi-transparent
    uint8_t semi_transparent;
    /// Texture window mask/OR values
    uint8_t texture_window[4];

    static std::vector<Attribute> attributes();
};

struct Texture
{
    GLuint id;
    uint32_t width;
    uint32_t height;
};

struct Framebuffer
{
    GLuint id;
    struct Texture _color_texture;
};

struct OutputVertex {
    /// Vertex position on the screen
    float position[2];
    /// Corresponding coordinate in the framebuffer
    uint16_t fb_coord[2];

    static std::vector<Attribute> attributes();
};

struct ImageLoadVertex {
    // Vertex position in VRAM
    uint16_t position[2];

    static std::vector<Attribute> attributes();
};

enum SemiTransparencyMode {
    /// Source / 2 + destination / 2
    SemiTransparencyMode_Average = 0,
    /// Source + destination
    SemiTransparencyMode_Add = 1,
    /// Destination - source
    SemiTransparencyMode_SubtractSource = 2,
    /// Destination + source / 4
    SemiTransparencyMode_AddQuarterSource = 3,
};

struct TransparencyIndex {
    SemiTransparencyMode transparency_mode;
    unsigned last_index;
    GLenum draw_mode;
};

template<typename T>
class DrawBuffer
{
public:
    /// OpenGL name for this buffer
    GLuint id;
    /// Vertex Array Object containing the bindings for this
    /// buffer. I'm assuming that each VAO will only use a single
    /// buffer for simplicity.
    GLuint vao;
    /// Program used to draw this buffer
    Program* program;
    /// Currently mapped buffer range (write-only)
    T *map;

    /// Number of elements T mapped at once in 'map'
    size_t capacity;
    /// Index one-past the last element stored in `map`, relative to
    /// the first element in `map`
    size_t map_index;
    /// Absolute offset of the 1st mapped element in the current
    /// buffer relative to the beginning of the GL storage.
    size_t map_start;

    DrawBuffer(size_t capacity, Program* program)
       :map(NULL)
    {
       GLuint id = 0;
       glGenVertexArrays(1, &id);

       this->vao      = id;

       id             = 0;

       // Generate the buffer object
       glGenBuffers(1, &id);

       this->program  = program;
       this->capacity = capacity;
       this->id       = id;

       // Create and map the buffer
       glBindBuffer(GL_ARRAY_BUFFER, id);

       size_t element_size = sizeof(T);
       // We allocate enough space for 3 times the buffer space and
       // we only remap one third of it at a time
       GLsizeiptr storage_size = this->capacity * element_size * 3;

       // Since we store indexes in unsigned shorts we want to make
       // sure the entire buffer is indexable.
       assert(this->capacity * 3 <= 0xffff);

       glBufferData(GL_ARRAY_BUFFER, storage_size, NULL, GL_DYNAMIC_DRAW);

       this->bind_attributes();

       this->map_index = 0;
       this->map_start = 0;

       this->map__no_bind();
    }

    ~DrawBuffer()
    {
       glBindBuffer(GL_ARRAY_BUFFER, this->id);

       this->unmap__no_bind();

       glDeleteBuffers(1, &this->id);
       glDeleteVertexArrays(1, &this->vao);
    }

    void bind_attributes()
    {
       glBindVertexArray(this->vao);

       // ARRAY_BUFFER is captured by VertexAttribPointer
       glBindBuffer(GL_ARRAY_BUFFER, this->id);

       std::vector<Attribute> attrs = T::attributes();

       GLint element_size = (GLint) sizeof( T );

       //speculative: attribs enabled on VAO=0 (disabled) get applied to the VAO when created initially
       //as a core, we don't control the state entirely at this point. frontend may have enabled attribs.
       //we need to make sure they're all disabled before then re-enabling the attribs we want
       //(solves crashes on some drivers/compilers due to accidentally enabled attribs)
       GLint nVertexAttribs;
       glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &nVertexAttribs);

       for (int i = 0; i < nVertexAttribs; i++)
          glDisableVertexAttribArray(i);

       for (std::vector<Attribute>::iterator it(attrs.begin()); it != attrs.end(); ++it)
       {
          Attribute& attr = *it;
          GLint index     = glGetAttribLocation(this->program->id, attr.name);

          // Don't error out if the shader doesn't use this
          // attribute, it could be caused by shader
          // optimization if the attribute is unused for
          // some reason.
          if (index < 0)
             continue;

          glEnableVertexAttribArray((GLuint) index);

          // This captures the buffer so that we don't have to bind it
          // when we draw later on, we'll just have to bind the vao
          switch (attr.ty)
          {
             case GL_BYTE:
             case GL_UNSIGNED_BYTE:
             case GL_SHORT:
             case GL_UNSIGNED_SHORT:
             case GL_INT:
             case GL_UNSIGNED_INT:
                glVertexAttribIPointer( index,
                      attr.components,
                      attr.ty,
                      element_size,
                      (GLvoid*)attr.offset);
                break;
             case GL_FLOAT:
                glVertexAttribPointer(  index,
                      attr.components,
                      attr.ty,
                      GL_FALSE,
                      element_size,
                      (GLvoid*)attr.offset);
                break;
             case GL_DOUBLE:
                glVertexAttribLPointer( index,
                      attr.components,
                      attr.ty,
                      element_size,
                      (GLvoid*)attr.offset);
                break;
          }
       }
    }


    // Map the buffer for write-only access
    void map__no_bind()
    {
       size_t element_size    = sizeof(T);
       GLsizeiptr buffer_size = this->capacity * element_size;
       GLintptr offset_bytes;
       void *m;

       glBindBuffer(GL_ARRAY_BUFFER, this->id);

       // If we're already mapped something's wrong
       assert(this->map == NULL);

       // We don't have enough room left to remap `capacity`,
       // start back from the beginning of the buffer.
       if (this->map_start > 2 * this->capacity)
          this->map_start = 0;

       offset_bytes = this->map_start * element_size;

#if 0
       printf("Remap %lu %lu\n", this->capacity, this->map_start);
#endif

       m = glMapBufferRange(GL_ARRAY_BUFFER,
             offset_bytes,
             buffer_size,
             GL_MAP_WRITE_BIT |
             GL_MAP_INVALIDATE_RANGE_BIT);

       // Just in case...
       assert(m != NULL);

       this->map = reinterpret_cast<T *>(m);
    }

    // Unmap the active buffer
    void unmap__no_bind()
    {
       assert(this->map != NULL);

       glBindBuffer(GL_ARRAY_BUFFER, this->id);

       glUnmapBuffer(GL_ARRAY_BUFFER);

       this->map = NULL;
    }

    void enable_attribute(const char* attr)
    {
       GLint index = glGetAttribLocation(this->program->id, attr);

       if (index < 0)
          return;

       glBindVertexArray(this->vao);

       glEnableVertexAttribArray(index);
    }

    void disable_attribute(const char* attr)
    {
       GLint index = glGetAttribLocation(this->program->id, attr);

       if (index < 0)
          return;

       glBindVertexArray(this->vao);

       glDisableVertexAttribArray(index);
    }


    void push_slice(T slice[], size_t n)
    {
       assert(n <= DRAWBUFFER_REMAINING_CAPACITY(this));
       assert(this->map != NULL);

       memcpy(this->map + this->map_index,
             slice,
             n * sizeof(T));

       this->map_index += n;
    }

    void prepare_draw()
    {
       glBindVertexArray(this->vao);
       glUseProgram(this->program->id);

       // I don't need to bind this to draw (it's captured by the
       // VAO) but I need it to map/unmap the storage.
       glBindBuffer(GL_ARRAY_BUFFER, this->id);

       /* Unmap the active buffer */
       glUnmapBuffer(GL_ARRAY_BUFFER);

       this->map = NULL;
    }

    /// Finalize the current buffer data and remap a fresh slice of
    /// the storage.
    void finalize_draw__no_bind()
    {
       this->map_start += this->map_index;
       this->map_index  = 0;

       this->map__no_bind();
    }

    void draw(GLenum mode)
    {

       this->prepare_draw();

       // Length in number of vertices
       glDrawArrays(mode, this->map_start, this->map_index);

       this->finalize_draw__no_bind();
    }

};

struct GlRenderer {
    /// Buffer used to handle PlayStation GPU draw commands
    DrawBuffer<CommandVertex>* command_buffer;
    GLushort opaque_triangle_indices[INDEX_BUFFER_LEN];
    GLushort opaque_line_indices[INDEX_BUFFER_LEN];
    GLushort semi_transparent_indices[INDEX_BUFFER_LEN];
    /// Primitive type for the vertices in the command buffers
    /// (TRIANGLES or LINES)
    GLenum command_draw_mode;
    unsigned opaque_triangle_index_pos;
    unsigned opaque_line_index_pos;
    unsigned semi_transparent_index_pos;
    /// Current semi-transparency mode
    SemiTransparencyMode semi_transparency_mode;
    std::vector<TransparencyIndex> transparency_mode_index;
    /// Polygon mode (for wireframe)
    GLenum command_polygon_mode;
    /// Buffer used to draw to the frontend's framebuffer
    DrawBuffer<OutputVertex>* output_buffer;
    /// Buffer used to copy textures from `fb_texture` to `fb_out`
    DrawBuffer<ImageLoadVertex>* image_load_buffer;
    /// Texture used to store the VRAM for texture mapping
    DrawConfig config;
    /// Framebuffer used as a shader input for texturing draw commands
    Texture fb_texture;
    /// Framebuffer used as an output when running draw commands
    Texture fb_out;
    /// Depth buffer for fb_out
    Texture fb_out_depth;
    /// Current resolution of the frontend's framebuffer
    uint32_t frontend_resolution[2];
    /// Current internal resolution upscaling factor
    uint32_t internal_upscaling;
    /// Current internal color depth
    uint8_t internal_color_depth;
    /// Counter for preserving primitive draw order in the z-buffer
    /// since we draw semi-transparent primitives out-of-order.
    int16_t primitive_ordering;
    /// Texture window mask/OR values
    uint8_t tex_x_mask;
    uint8_t tex_x_or;
    uint8_t tex_y_mask;
    uint8_t tex_y_or;

    uint32_t mask_set_or;
    uint32_t mask_eval_and;

    uint8_t filter_type;

    /// When true we display the entire VRAM buffer instead of just
    /// the visible area
    bool display_vram;
};


static void get_error(void)
{
#ifdef DEBUG
   GLenum error = glGetError();
   switch (error)
   {
      case GL_NO_ERROR:
         //puts("GL error flag: GL_NO_ERROR\n");
         return;
      case GL_INVALID_ENUM:
         puts("GL error flag: GL_INVALID_ENUM\n");
         break;
      case GL_INVALID_VALUE:
         puts("GL error flag: GL_INVALID_VALUE\n");
         break;
      case GL_INVALID_FRAMEBUFFER_OPERATION:
         puts("GL error flag: GL_INVALID_FRAMEBUFFER_OPERATION\n");
         break;
      case GL_OUT_OF_MEMORY:
         puts("GL error flag: GL_OUT_OF_MEMORY\n");
         break;
      case GL_STACK_UNDERFLOW:
         puts("GL error flag: GL_STACK_UNDERFLOW\n");
         break;
      case GL_STACK_OVERFLOW:
         puts("GL error flag: GL_STACK_OVERFLOW\n");
         break;
      case GL_INVALID_OPERATION:
         puts("GL error flag: GL_INVALID_OPERATION\n");
         break;
      default:
         printf("GL error flag: %d\n", (int) error);
         break;
   }

   assert(error == GL_NO_ERROR);
#endif
}

static void Framebuffer_init(struct Framebuffer *fb,
		struct Texture* color_texture)
{
   GLuint id = 0;
   glGenFramebuffers(1, &id);

   fb->id                    = id;

   fb->_color_texture.id     = color_texture->id;
   fb->_color_texture.width  = color_texture->width;
   fb->_color_texture.height = color_texture->height;

   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb->id);

   glFramebufferTexture(   GL_DRAW_FRAMEBUFFER,
         GL_COLOR_ATTACHMENT0,
         color_texture->id,
         0);

   GLenum col_attach_0 = GL_COLOR_ATTACHMENT0;
   glDrawBuffers(1, &col_attach_0);
   glViewport( 0,
         0,
         (GLsizei) color_texture->width,
         (GLsizei) color_texture->height);
}

/* forward decls */
static GLint Program_uniform(Program *program, const char* name);

static void program_uniform1i(Program *prog, const char *name, GLint i)
{
   GLint u;
   if (!prog)
      return;

   glUseProgram(prog->id);
   u = Program_uniform(prog, name);
   glUniform1i(u, i);
}

static void program_uniform1ui(Program *prog, const char *name, GLuint i)
{
   GLint u;
   if (!prog)
      return;

   glUseProgram(prog->id);
   u = Program_uniform(prog, name);
   glUniform1ui(u, i);
}

static void program_uniform2i(Program *prog, const char *name, GLint a, GLint b)
{
   GLint u;
   if (!prog)
      return;

   glUseProgram(prog->id);
   u = Program_uniform(prog, name);
   glUniform2i(u, a, b);
}

static void program_uniform2ui(Program *prog, const char *name, GLuint a, GLuint b)
{
   GLint u;
   if (!prog)
      return;

   glUseProgram(prog->id);
   u = Program_uniform(prog, name);
   glUniform2ui(u, a, b);
}

static void Shader_init(
      struct Shader *shader,
      const char* source,
      GLenum shader_type)
{
   GLint status;
   GLint log_len = 0;
   GLuint id;

   shader->info_log = NULL;
   id               = glCreateShader(shader_type);

   if (id == 0)
   {
      puts("An error occured creating the shader object\n");
      exit(EXIT_FAILURE);
   }

   glShaderSource( id,
         1,
         &source,
         NULL);
   glCompileShader(id);

   status = (GLint) GL_FALSE;
   glGetShaderiv(id, GL_COMPILE_STATUS, &status);
   glGetShaderiv(id, GL_INFO_LOG_LENGTH, &log_len);

   if (log_len > 0)
   {
      GLsizei len;

      shader->info_log = (char*)malloc(log_len);
      len              = (GLsizei) log_len;

      glGetShaderInfoLog(id,
            len,
            &log_len,
            (char*)shader->info_log);

      if (log_len > 0)
      {
         // The length returned by GetShaderInfoLog *excludes*
         // the ending \0 unlike the call to GetShaderiv above
         // so we can get rid of it by truncating here.
         /* log.truncate(log_len as usize); */
         /* Don't want to spend time thinking about the above, I'll just put a \0
            in the last index */
         shader->info_log[log_len - 1] = '\0';
      }
   }

   if (status != (GLint) GL_TRUE)
   {
      puts("Shader compilation failed:\n");

      /* print shader source */
      puts( source );

      puts("Shader info log:\n");
      puts(shader->info_log);

      exit(EXIT_FAILURE);
      return;
   }

   shader->id = id;
}

static void Shader_free(struct Shader *shader)
{
   if (shader)
   {
      glDeleteShader(shader->id);
      if (shader->info_log)
         free(shader->info_log);
   }
}

static UniformMap load_program_uniforms(GLuint program);

static void get_program_info_log(Program *pg, GLuint id)
{
   GLsizei len;
   GLint log_len = 0;

   glGetProgramiv(id, GL_INFO_LOG_LENGTH, &log_len);

   if (log_len <= 0)
      return;

   pg->info_log = (char*)malloc(log_len);
   len          = (GLsizei) log_len;

   glGetProgramInfoLog(id,
         len,
         &log_len,
         (char*)pg->info_log);

   if (log_len <= 0)
      return;

   // The length returned by GetShaderInfoLog *excludes*
   // the ending \0 unlike the call to GetShaderiv above
   // so we can get rid of it by truncating here.
   /* log.truncate(log_len as usize); */
   /* Don't want to spend time thinking about the above, I'll just put a \0
      in the last index */
   pg->info_log[log_len - 1] = '\0';
}

static void Program_init(
      Program *program,
      Shader* vertex_shader,
      Shader* fragment_shader)
{
   GLint status;
   GLuint id;

   program->info_log = NULL;

   id                = glCreateProgram();

   if (id == 0)
   {
      puts("An error occured creating the program object\n");
      exit(EXIT_FAILURE);
   }

   glAttachShader(id, vertex_shader->id);
   glAttachShader(id, fragment_shader->id);

   glLinkProgram(id);

   glDetachShader(id, vertex_shader->id);
   glDetachShader(id, fragment_shader->id);

   /* Program owns the two pointers, so we clean them up now */
   if (vertex_shader)
   {
      Shader_free(vertex_shader);
      delete vertex_shader;
      vertex_shader = NULL;
   }

   if (fragment_shader)
   {
      Shader_free(fragment_shader);
      delete fragment_shader;
      fragment_shader = NULL;
   }

   // Check if the program linking was successful
   status = (GLint) GL_FALSE;
   glGetProgramiv(id, GL_LINK_STATUS, &status);
   get_program_info_log(program, id);

   if (status != (GLint) GL_TRUE)
   {
      puts("OpenGL program linking failed\n");
      puts("Program info log:\n");
      puts(program->info_log );

      exit(EXIT_FAILURE);
      return;
   }

   /* Rust code has a try statement here, perhaps we should fail fast with
      exit(EXIT_FAILURE) ? */
   UniformMap uniforms = load_program_uniforms(id);

   program->id       = id;
   program->uniforms = uniforms;
}

static void Program_free(Program *program)
{
   if (!program)
      return;

   glDeleteProgram(program->id);
   if (program->info_log)
      free(program->info_log);
}


static GLint Program_uniform(Program *program, const char* name)
{
   bool found = program->uniforms.find(name) != program->uniforms.end();
   if (!found)
   {
      printf("Attempted to access unknown uniform %s\n", name);
      exit(EXIT_FAILURE);
   }

   return program->uniforms[name];
}

static UniformMap load_program_uniforms(GLuint program)
{
   size_t u;
   UniformMap uniforms;
   // Figure out how long a uniform name can be
   GLint max_name_len = 0;
   GLint n_uniforms   = 0;

   glGetProgramiv( program,
         GL_ACTIVE_UNIFORMS,
         &n_uniforms );

   glGetProgramiv( program,
         GL_ACTIVE_UNIFORM_MAX_LENGTH,
         &max_name_len);

   for (u = 0; u < n_uniforms; ++u)
   {
      // Retrieve the name of this uniform. Don't use the size we just fetched, because it's inconvenient. Use something monstrously large.
      char name[256];
      size_t name_len = max_name_len;
      GLsizei len     = 0;
      /* XXX we might want to validate those at some point */
      GLint size      = 0;
      GLenum ty       = 0;

      glGetActiveUniform( program,
            (GLuint) u,
            (GLsizei) name_len,
            &len,
            &size,
            &ty,
            (char*) name);

      if (len <= 0)
      {
         printf("Ignoring uniform name with size %d\n", len);
         continue;
      }

      // Retrieve the location of this uniform
      GLint location = glGetUniformLocation(program, (const char*) name);

      /* name.truncate(len as usize); */
      /* name[len - 1] = '\0'; */

      if (location < 0)
      {
         printf("Uniform \"%s\" doesn't have a location", name);
         continue;
      }

      uniforms[name] = location;
   }

   return uniforms;
}

static void Texture_init(
      struct Texture *tex,
      uint32_t width,
      uint32_t height,
      GLenum internal_format)
{
    GLuint id = 0;

    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexStorage2D(GL_TEXTURE_2D,
                    1,
                    internal_format,
                    (GLsizei) width,
                    (GLsizei) height);

    tex->id     = id;
    tex->width  = width;
    tex->height = height;
}

static void Texture_set_sub_image(
      struct Texture *tex,
      uint16_t top_left[2],
      uint16_t resolution[2],
      GLenum format,
      GLenum ty,
      uint16_t* data)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex->id);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    (GLint) top_left[0],
                    (GLint) top_left[1],
                    (GLsizei) resolution[0],
                    (GLsizei) resolution[1],
                    format,
                    ty,
                    (void*) data);
}

static void Texture_set_sub_image_window(
      struct Texture *tex,
      uint16_t top_left[2],
      uint16_t resolution[2],
      size_t row_len,
      GLenum format,
      GLenum ty,
      uint16_t* data)
{
   uint16_t x         = top_left[0];
   uint16_t y         = top_left[1];

   size_t index       = ((size_t) y) * row_len + ((size_t) x);

   /* TODO - Am I indexing data out of bounds? */
   uint16_t* sub_data = &( data[index] );

   glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint) row_len);

   Texture_set_sub_image(tex, top_left, resolution, format, ty, sub_data);

   glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

template<typename T>
static DrawBuffer<T>* build_buffer( const char* vertex_shader,
		const char* fragment_shader,
		size_t capacity)
{
   Shader *vs = new Shader;
   Shader *fs = new Shader;

   Shader_init(vs, vertex_shader, GL_VERTEX_SHADER);
   Shader_init(fs, fragment_shader, GL_FRAGMENT_SHADER);
   Program* program = new Program;

   Program_init(program, vs, fs);

   return new DrawBuffer<T>(capacity, program);
}

static void upload_textures(
      GlRenderer *renderer,
      uint16_t top_left[2],
      uint16_t dimensions[2],
      uint16_t pixel_buffer[VRAM_PIXELS]);

static bool GlRenderer_new(GlRenderer *renderer, DrawConfig config)
{
    struct retro_variable var = {0};

    if (!renderer)
       return false;

    var.key = option_internal_resolution;
    uint8_t upscaling = 1;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        /* Same limitations as libretro.cpp */
        upscaling = var.value[0] -'0';
	if (var.value[1] != 'x')
	{
	   upscaling  = (var.value[0] - '0') * 10;
	   upscaling += var.value[1] - '0';

	   if (upscaling == 32) /* Too high for GL */
              upscaling = 16;
	}
    }

    var.key = option_filter;
    uint8_t filter = FILTER_MODE_NEAREST;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
       if (!strcmp(var.value, "nearest"))
          filter = FILTER_MODE_NEAREST;
       else if (!strcmp(var.value, "SABR"))
          filter = FILTER_MODE_SABR;
       else if (!strcmp(var.value, "xBR"))
          filter = FILTER_MODE_XBR;
       else if (!strcmp(var.value, "bilinear"))
          filter = FILTER_MODE_BILINEAR;
       else if (!strcmp(var.value, "3-point"))
          filter = FILTER_MODE_3POINT;
       else if (!strcmp(var.value, "JINC2"))
          filter = FILTER_MODE_JINC2;

       renderer->filter_type = filter;
    }

    var.key = option_depth;
    uint8_t depth = 16;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
       if (!strcmp(var.value, "32bpp"))
          depth = 32;
       else
          depth = 16;
    }


    var.key = option_scale_dither;
    bool scale_dither = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
       if (!strcmp(var.value, "enabled"))
          scale_dither = true;
       else
          scale_dither = false;
    }

    var.key = option_wireframe;
    bool wireframe = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
       if (!strcmp(var.value, "enabled"))
          wireframe = true;
       else
          wireframe = false;
    }

    var.key = option_display_vram;
    bool display_vram = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (!strcmp(var.value, "enabled"))
	display_vram = true;
      else
	display_vram = false;
    }

    printf("Building OpenGL state (%dx internal res., %dbpp)\n", upscaling, depth);

    DrawBuffer<CommandVertex>* command_buffer;

    switch(renderer->filter_type)
    {
    case FILTER_MODE_SABR:
      command_buffer = build_buffer<CommandVertex>(
                           command_vertex_xbr,
                           command_fragment_sabr,
                           VERTEX_BUFFER_LEN);
      break;
    case FILTER_MODE_XBR:
      command_buffer = build_buffer<CommandVertex>(
                           command_vertex_xbr,
                           command_fragment_xbr,
                           VERTEX_BUFFER_LEN);
      break;
     case FILTER_MODE_BILINEAR:
      command_buffer = build_buffer<CommandVertex>(
                           command_vertex,
                           command_fragment_bilinear,
                           VERTEX_BUFFER_LEN);
      break;
     case FILTER_MODE_3POINT:
      command_buffer = build_buffer<CommandVertex>(
                           command_vertex,
                           command_fragment_3point,
                           VERTEX_BUFFER_LEN);
      break;
     case FILTER_MODE_JINC2:
      command_buffer = build_buffer<CommandVertex>(
                           command_vertex,
                           command_fragment_jinc2,
                           VERTEX_BUFFER_LEN);
      break;
    case FILTER_MODE_NEAREST:
    default:
       command_buffer = build_buffer<CommandVertex>(
                           command_vertex,
                           command_fragment,
                           VERTEX_BUFFER_LEN);
    }

    DrawBuffer<OutputVertex>* output_buffer =
        build_buffer<OutputVertex>(
            output_vertex,
            output_fragment,
            4);

    DrawBuffer<ImageLoadVertex>* image_load_buffer =
        build_buffer<ImageLoadVertex>(
            image_load_vertex,
            image_load_fragment,
            4);

    uint32_t native_width  = (uint32_t) VRAM_WIDTH_PIXELS;
    uint32_t native_height = (uint32_t) VRAM_HEIGHT;

    // Texture holding the raw VRAM texture contents. We can't
    // meaningfully upscale it since most games use paletted
    // textures.
    Texture_init(&renderer->fb_texture, native_width, native_height, GL_RGB5_A1);

    if (depth > 16) {
        // Dithering is superfluous when we increase the internal
        // color depth
        command_buffer->disable_attribute("dither");
    }

    uint32_t dither_scaling = scale_dither ? upscaling : 1;
    GLenum command_draw_mode = wireframe ? GL_LINE : GL_FILL;

    program_uniform1ui(command_buffer->program, "dither_scaling", dither_scaling);

    GLenum texture_storage = GL_RGB5_A1;
    switch (depth) {
    case 16:
        texture_storage = GL_RGB5_A1;
        break;
    case 32:
        texture_storage = GL_RGBA8;
        break;
    default:
        printf("Unsupported depth %d\n", depth);
        exit(EXIT_FAILURE);
    }

    Texture_init(
          &renderer->fb_out,
          native_width * upscaling,
          native_height * upscaling,
          texture_storage);

    Texture_init(
          &renderer->fb_out_depth,
          renderer->fb_out.width,
          renderer->fb_out.height,
          GL_DEPTH_COMPONENT32F);

    renderer->filter_type = filter;
    renderer->command_buffer = command_buffer;
    renderer->opaque_triangle_index_pos = INDEX_BUFFER_LEN - 1;
    renderer->opaque_line_index_pos = INDEX_BUFFER_LEN - 1;
    renderer->semi_transparent_index_pos = 0;
    renderer->command_draw_mode = GL_TRIANGLES;
    renderer->semi_transparency_mode =  SemiTransparencyMode_Average;
    renderer->command_polygon_mode = command_draw_mode;
    renderer->output_buffer = output_buffer;
    renderer->image_load_buffer = image_load_buffer;
    renderer->config = config;
    renderer->frontend_resolution[0] = 0;
    renderer->frontend_resolution[1] = 0;
    renderer->internal_upscaling = upscaling;
    renderer->internal_color_depth = depth;
    renderer->primitive_ordering = 0;
    renderer->tex_x_mask = 0;
    renderer->tex_x_or = 0;
    renderer->tex_y_mask = 0;
    renderer->tex_y_or = 0;
    renderer->display_vram = display_vram;
    renderer->mask_set_or  = 0;
    renderer->mask_eval_and = 0;

    uint16_t top_left[2] = {0, 0};
    uint16_t dimensions[2] = {(uint16_t) VRAM_WIDTH_PIXELS, (uint16_t) VRAM_HEIGHT};
    upload_textures(renderer, top_left, dimensions, GPU_get_vram());

    return true;
}

static void GlRenderer_free(GlRenderer *renderer)
{
   if (!renderer)
      return;

    if (renderer->command_buffer) {
        Program_free(renderer->command_buffer->program);
        delete renderer->command_buffer->program;
        delete renderer->command_buffer;
    }
    renderer->command_buffer = NULL;

    if (renderer->output_buffer)
        delete renderer->output_buffer;
    renderer->output_buffer = NULL;

    if (renderer->image_load_buffer)
        delete renderer->image_load_buffer;
    renderer->image_load_buffer = NULL;

    glDeleteTextures(1, &renderer->fb_texture.id);
    renderer->fb_texture.id     = 0;
    renderer->fb_texture.width  = 0;
    renderer->fb_texture.height = 0;

    glDeleteTextures(1, &renderer->fb_out.id);
    renderer->fb_out.id     = 0;
    renderer->fb_out.width  = 0;
    renderer->fb_out.height = 0;

    glDeleteTextures(1, &renderer->fb_out_depth.id);
    renderer->fb_out_depth.id     = 0;
    renderer->fb_out_depth.width  = 0;
    renderer->fb_out_depth.height = 0;
}

extern bool doCleanFrame;


static inline void apply_scissor(GlRenderer *renderer)
{
    uint16_t _x = renderer->config.draw_area_top_left[0];
    uint16_t _y = renderer->config.draw_area_top_left[1];
    int _w      = renderer->config.draw_area_bot_right[0] - _x;
    int _h      = renderer->config.draw_area_bot_right[1] - _y;

    if (_w < 0)
      _w = 0;

    if (_h < 0)
      _h = 0;

    GLsizei upscale = (GLsizei)renderer->internal_upscaling;

    // We need to scale those to match the internal resolution if
    // upscaling is enabled
    GLsizei x = (GLsizei) _x * upscale;
    GLsizei y = (GLsizei) _y * upscale;
    GLsizei w = (GLsizei) _w * upscale;
    GLsizei h = (GLsizei) _h * upscale;

    glScissor(x, y, w, h);
}

static void bind_libretro_framebuffer(GlRenderer *renderer)
{
    uint32_t f_w = renderer->frontend_resolution[0];
    uint32_t f_h = renderer->frontend_resolution[1];
    uint16_t _w;
    uint16_t _h;
    float    aspect_ratio;

    if (renderer->display_vram)
    {
      _w = VRAM_WIDTH_PIXELS;
      _h = VRAM_HEIGHT;
      // Is this accurate?
      aspect_ratio = 2.0 / 1.0;
    } else {
      _w = renderer->config.display_resolution[0];
      _h = renderer->config.display_resolution[1];
      aspect_ratio = widescreen_hack ? 16.0 / 9.0 : 4.0 / 3.0;
    }

    uint32_t upscale = renderer->internal_upscaling;

    // XXX scale w and h when implementing increased internal
    // resolution
    uint32_t w = (uint32_t) _w * upscale;
    uint32_t h = (uint32_t) _h * upscale;

    if (w != f_w || h != f_h) {
        // We need to change the frontend's resolution
        struct retro_game_geometry geometry;
        geometry.base_width  = w;
        geometry.base_height = h;
        // Max parameters are ignored by this call
        geometry.max_width  = 0;
        geometry.max_height = 0;

        geometry.aspect_ratio = aspect_ratio;

        //printf("Target framebuffer size: %dx%d\n", w, h);

        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);

        renderer->frontend_resolution[0] = w;
        renderer->frontend_resolution[1] = h;
    }

    // Bind the output framebuffer provided by the frontend
    /* TODO/FIXME - I think glsm_ctl(BIND) is the way to go here. Check with the libretro devs */
    GLuint fbo = glsm_get_current_framebuffer();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glViewport(0, 0, (GLsizei) w, (GLsizei) h);
}

static void GlRenderer_draw(GlRenderer *renderer);

static void upload_textures(
      GlRenderer *renderer,
      uint16_t top_left[2],
      uint16_t dimensions[2],
      uint16_t pixel_buffer[VRAM_PIXELS])
{
   if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
      GlRenderer_draw(renderer);

    Texture_set_sub_image(
          &renderer->fb_texture,
          top_left,
          dimensions,
          GL_RGBA,
          GL_UNSIGNED_SHORT_1_5_5_5_REV,
          pixel_buffer);

    uint16_t x_start    = top_left[0];
    uint16_t x_end      = x_start + dimensions[0];
    uint16_t y_start    = top_left[1];
    uint16_t y_end      = y_start + dimensions[1];

    const size_t slice_len = 4;
    ImageLoadVertex slice[slice_len] =
    {
        {   {x_start,   y_start }   },
        {   {x_end,     y_start }   },
        {   {x_start,   y_end   }   },
        {   {x_end,     y_end   }   }
    };

    renderer->image_load_buffer->push_slice(slice, slice_len);

    program_uniform1i(renderer->image_load_buffer->program, "fb_texture", 0);

    // fb_texture is always at 1x
    program_uniform1ui(renderer->image_load_buffer->program, "internal_upscaling", 1);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Bind the output framebuffer
    // let _fb = Framebuffer::new(&self.fb_out);
    Framebuffer _fb;
    Framebuffer_init(&_fb, &renderer->fb_out);

    if (!DRAWBUFFER_IS_EMPTY(renderer->image_load_buffer))
       renderer->image_load_buffer->draw(GL_TRIANGLE_STRIP);
    glPolygonMode(GL_FRONT_AND_BACK, renderer->command_polygon_mode);
    glEnable(GL_SCISSOR_TEST);

#ifdef DEBUG
    get_error();
#endif
    glDeleteFramebuffers(1, &_fb.id);
}

static bool retro_refresh_variables(GlRenderer *renderer)
{
    struct retro_variable var = {0};

    var.key = option_renderer_software_fb;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
       if (!strcmp(var.value, "enabled"))
          has_software_fb = true;
       else
          has_software_fb = false;
    }

    var.key = option_internal_resolution;
    uint8_t upscaling = 1;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        /* Same limitations as libretro.cpp */
        upscaling = var.value[0] -'0';
	if (var.value[1] != 'x')
	{
           upscaling  = (var.value[0] -'0') * 10;
           upscaling += (var.value[1] -'0');

	   if (upscaling == 32) /* Too high for GL */
	      upscaling = 16;
	}
    }

    var.key = option_filter;
    uint8_t filter = FILTER_MODE_NEAREST;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
       if (!strcmp(var.value, "nearest"))
          filter = FILTER_MODE_NEAREST;
       else if (!strcmp(var.value, "SABR"))
          filter = FILTER_MODE_SABR;
       else if (!strcmp(var.value, "xBR"))
          filter = FILTER_MODE_XBR;
       else if (!strcmp(var.value, "bilinear"))
          filter = FILTER_MODE_BILINEAR;
       else if (!strcmp(var.value, "3-point"))
          filter = FILTER_MODE_3POINT;
       else if (!strcmp(var.value, "JINC2"))
          filter = FILTER_MODE_JINC2;
    }

    var.key = option_depth;
    uint8_t depth = 16;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        depth = !strcmp(var.value, "32bpp") ? 32 : 16;
    }


    var.key = option_scale_dither;
    bool scale_dither = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
       if (!strcmp(var.value, "enabled"))
          scale_dither = true;
       else
          scale_dither = false;
    }

    var.key = option_wireframe;
    bool wireframe = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
       if (!strcmp(var.value, "enabled"))
          wireframe = true;
       else
          wireframe = false;
    }

    var.key = option_display_vram;
    bool display_vram = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (!strcmp(var.value, "enabled")) {
	display_vram = true;
      } else
	display_vram = false;
    }

    bool rebuild_fb_out =
      upscaling != renderer->internal_upscaling ||
      depth != renderer->internal_color_depth;

    if (rebuild_fb_out)
    {
        if (depth > 16)
            renderer->command_buffer->disable_attribute("dither");
        else
            renderer->command_buffer->enable_attribute("dither");

        uint32_t native_width = (uint32_t) VRAM_WIDTH_PIXELS;
        uint32_t native_height = (uint32_t) VRAM_HEIGHT;

        uint32_t w = native_width * upscaling;
        uint32_t h = native_height * upscaling;

        GLenum texture_storage = GL_RGB5_A1;
        switch (depth) {
        case 16:
            texture_storage = GL_RGB5_A1;
            break;
        case 32:
            texture_storage = GL_RGBA8;
            break;
        default:
            printf("Unsupported depth %d\n", depth);
            exit(EXIT_FAILURE);
        }

	glDeleteTextures(1, &renderer->fb_out.id);
	renderer->fb_out.id     = 0;
	renderer->fb_out.width  = 0;
	renderer->fb_out.height = 0;
        Texture_init(&renderer->fb_out, w, h, texture_storage);

        // This is a bit wasteful since it'll re-upload the data
        // to `fb_texture` even though we haven't touched it but
        // this code is not very performance-critical anyway.

        uint16_t top_left[2] = {0, 0};
        uint16_t dimensions[2] = {(uint16_t) VRAM_WIDTH_PIXELS, (uint16_t) VRAM_HEIGHT};

        upload_textures(renderer, top_left, dimensions,
			      GPU_get_vram());


	glDeleteTextures(1, &renderer->fb_out_depth.id);
	renderer->fb_out_depth.id     = 0;
	renderer->fb_out_depth.width  = 0;
	renderer->fb_out_depth.height = 0;
        Texture_init(&renderer->fb_out_depth, w, h, GL_DEPTH_COMPONENT32F);
    }

    uint32_t dither_scaling = scale_dither ? upscaling : 1;
    program_uniform1ui(renderer->command_buffer->program, "dither_scaling", (GLuint) dither_scaling);

    renderer->command_polygon_mode = wireframe ? GL_LINE : GL_FILL;

    glLineWidth((GLfloat) upscaling);

    // If the scaling factor has changed the frontend should be
    // reconfigured. We can't do that here because it could
    // destroy the OpenGL context which would destroy `self`
    //// r5 - replace 'self' by 'this'
    bool reconfigure_frontend =
      renderer->internal_upscaling != upscaling ||
      renderer->display_vram != display_vram ||
      renderer->filter_type != filter;

    renderer->internal_upscaling = upscaling;
    renderer->display_vram = display_vram;
    renderer->internal_color_depth = depth;
    renderer->filter_type = filter;

    return reconfigure_frontend;
}

static void vertex_preprocessing(
      GlRenderer *renderer,
      CommandVertex *v,
      unsigned count,
      GLenum mode,
      SemiTransparencyMode stm)
{
   bool is_semi_transparent = v[0].semi_transparent == 1;
   bool buffer_full         = DRAWBUFFER_REMAINING_CAPACITY(renderer->command_buffer) < count;

   if (buffer_full)
   {
      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         GlRenderer_draw(renderer);
   }

   int16_t z = renderer->primitive_ordering;
   renderer->primitive_ordering += 1;

   for (unsigned i = 0; i < count; i++)
   {
      v[i].position[2] = z;
      v[i].texture_window[0] = renderer->tex_x_mask;
      v[i].texture_window[1] = renderer->tex_x_or;
      v[i].texture_window[2] = renderer->tex_y_mask;
      v[i].texture_window[3] = renderer->tex_y_or;
   }

   if (is_semi_transparent &&
         (stm != renderer->semi_transparency_mode ||
          mode != renderer->command_draw_mode)) {
      // We're changing the transparency mode
      TransparencyIndex ti;
      ti.transparency_mode = renderer->semi_transparency_mode;
      ti.last_index        = renderer->semi_transparent_index_pos;
      ti.draw_mode         = renderer->command_draw_mode;

      renderer->transparency_mode_index.push_back(ti);
      renderer->semi_transparency_mode = stm;
      renderer->command_draw_mode = mode;
   }
}

static void push_primitive(
      GlRenderer *renderer,
      CommandVertex *v,
      unsigned count,
      GLenum mode,
      SemiTransparencyMode stm)
{
   bool is_semi_transparent = v[0].semi_transparent == 1;
   bool is_textured = v[0].texture_blend_mode != 0;
   // Textured semi-transparent polys can contain opaque texels (when
   // bit 15 of the color is set to 0). Therefore they're drawn twice,
   // once for the opaque texels and once for the semi-transparent
   // ones. Only untextured semi-transparent triangles don't need to be
   // drawn as opaque.
   bool is_opaque = !is_semi_transparent || is_textured;

   vertex_preprocessing(renderer, v, count, mode, stm);

   unsigned index = DRAWBUFFER_NEXT_INDEX(renderer->command_buffer);

   for (unsigned i = 0; i < count; i++)
   {
      if (is_opaque)
      {
         if (mode == GL_TRIANGLES)
         {
            renderer->opaque_triangle_indices[renderer->opaque_triangle_index_pos--] =
               index;
         }
         else
         {
            renderer->opaque_line_indices[renderer->opaque_line_index_pos--] =
               index;
         }
      }

      if (is_semi_transparent)
      {
         renderer->semi_transparent_indices[renderer->semi_transparent_index_pos++]
            = index;
      }

      index++;
   }

   renderer->command_buffer->push_slice(v, count);
}

std::vector<Attribute> CommandVertex::attributes()
{
    std::vector<Attribute> result;
    Attribute attr;

    strcpy(attr.name, "position");
    attr.offset     = offsetof(CommandVertex, position);
    attr.ty         = GL_FLOAT;
    attr.components = 4;

    result.push_back(attr);

    strcpy(attr.name, "color");
    attr.offset     = offsetof(CommandVertex, color);
    attr.ty         = GL_UNSIGNED_BYTE;
    attr.components = 3;

    result.push_back(attr);

    strcpy(attr.name, "texture_coord");
    attr.offset     = offsetof(CommandVertex, texture_coord);
    attr.ty         = GL_UNSIGNED_SHORT;
    attr.components = 2;

    result.push_back(attr);

    strcpy(attr.name, "texture_page");
    attr.offset     = offsetof(CommandVertex, texture_page);
    attr.ty         = GL_UNSIGNED_SHORT;
    attr.components = 2;

    result.push_back(attr);

    strcpy(attr.name, "clut");
    attr.offset     = offsetof(CommandVertex, clut);
    attr.ty         = GL_UNSIGNED_SHORT;
    attr.components = 2;

    result.push_back(attr);

    strcpy(attr.name, "texture_blend_mode");
    attr.offset     = offsetof(CommandVertex, texture_blend_mode);
    attr.ty         = GL_UNSIGNED_BYTE;
    attr.components = 1;

    result.push_back(attr);

    strcpy(attr.name, "depth_shift");
    attr.offset     = offsetof(CommandVertex, depth_shift);
    attr.ty         = GL_UNSIGNED_BYTE;
    attr.components = 1;

    result.push_back(attr);

    strcpy(attr.name, "dither");
    attr.offset     = offsetof(CommandVertex, dither);
    attr.ty         = GL_UNSIGNED_BYTE;
    attr.components = 1;

    result.push_back(attr);

    strcpy(attr.name, "semi_transparent");
    attr.offset     = offsetof(CommandVertex, semi_transparent);
    attr.ty         = GL_UNSIGNED_BYTE;
    attr.components = 1;

    result.push_back(attr);

    strcpy(attr.name, "texture_window");
    attr.offset     = offsetof(CommandVertex, texture_window);
    attr.ty         = GL_UNSIGNED_BYTE;
    attr.components = 4;

    result.push_back(attr);

    return result;
}

std::vector<Attribute> OutputVertex::attributes()
{
    std::vector<Attribute> result;
    Attribute attr;

    strcpy(attr.name, "position");
    attr.offset     = offsetof(OutputVertex, position);
    attr.ty         = GL_FLOAT;
    attr.components = 2;

    result.push_back(attr);

    strcpy(attr.name, "fb_coord");
    attr.offset     = offsetof(OutputVertex, fb_coord);
    attr.ty         = GL_UNSIGNED_SHORT;
    attr.components = 2;

    result.push_back(attr);

    return result;
}

std::vector<Attribute> ImageLoadVertex::attributes()
{
    std::vector<Attribute> result;
    Attribute attr;

    strcpy(attr.name, "position");
    attr.offset     = offsetof(ImageLoadVertex, position);
    attr.ty         = GL_UNSIGNED_SHORT;
    attr.components = 2;

    result.push_back(attr);

    return result;
}

/// State machine dealing with OpenGL context
/// destruction/reconstruction
enum GlState {
    // OpenGL context is ready
    GlState_Valid,
    /// OpenGL context has been destroyed (or is not created yet)
    GlState_Invalid
};

struct GlStateData {
    GlRenderer* r;
    DrawConfig c;
};

struct RetroGl {
    /*
    Rust's enums members can contain data. To emulate that,
    I'll use a helper struct to save the data.
    */
    GlStateData state_data;
    GlState state;
    VideoClock video_clock;
    bool inited;
};

/* This was originally in rustation-libretro/lib.rs */
retro_system_av_info get_av_info(VideoClock std);

static RetroGl static_renderer;

static void GlRenderer_draw(GlRenderer *renderer)
{
   int16_t x, y;

   if (!renderer || static_renderer.state == GlState_Invalid)
	   return;

   x = renderer->config.draw_offset[0];
   y = renderer->config.draw_offset[1];

   program_uniform2i(renderer->command_buffer->program, "offset", (GLint)x, (GLint)y);

   // We use texture unit 0
   program_uniform1i(renderer->command_buffer->program, "fb_texture", 0);

   // Bind the out framebuffer
   Framebuffer _fb;
   Framebuffer_init(&_fb, &renderer->fb_out);

   glFramebufferTexture(   GL_DRAW_FRAMEBUFFER,
         GL_DEPTH_ATTACHMENT,
         renderer->fb_out_depth.id,
         0);

   glClear(GL_DEPTH_BUFFER_BIT);

   // First we draw the opaque vertices
   glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
   glDisable(GL_BLEND);

   program_uniform1ui(renderer->command_buffer->program, "draw_semi_transparent", 0);

   renderer->command_buffer->prepare_draw();

   GLushort *opaque_triangle_indices =
      renderer->opaque_triangle_indices + renderer->opaque_triangle_index_pos + 1;
   GLsizei opaque_triangle_len =
      INDEX_BUFFER_LEN - renderer->opaque_triangle_index_pos - 1;

   if (opaque_triangle_len)
   {
      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
      {
	      /// This method doesn't call prepare_draw/finalize_draw itself, it
	      /// must be handled by the caller. This is because this command
	      /// can be called several times on the same buffer (i.e. multiple
	      /// draw calls between the prepare/finalize)
	      glBindBuffer(GL_ARRAY_BUFFER, renderer->command_buffer->id);
	      glDrawElements(GL_TRIANGLES, opaque_triangle_len, GL_UNSIGNED_SHORT, opaque_triangle_indices);
      }
   }

   GLushort *opaque_line_indices =
      renderer->opaque_line_indices + renderer->opaque_line_index_pos + 1;
   GLsizei opaque_line_len =
      INDEX_BUFFER_LEN - renderer->opaque_line_index_pos - 1;

   if (opaque_line_len)
   {
      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
      {
	      /// This method doesn't call prepare_draw/finalize_draw itself, it
	      /// must be handled by the caller. This is because this command
	      /// can be called several times on the same buffer (i.e. multiple
	      /// draw calls between the prepare/finalize)
	 glBindBuffer(GL_ARRAY_BUFFER, renderer->command_buffer->id);
	 glDrawElements(GL_LINES, opaque_line_len, GL_UNSIGNED_SHORT, opaque_line_indices);
      }
   }

   if (renderer->semi_transparent_index_pos > 0) {
      // Semi-transparency pass

      // Push the current semi-transparency mode
      TransparencyIndex ti;
      ti.transparency_mode = renderer->semi_transparency_mode;
      ti.last_index        = renderer->semi_transparent_index_pos;
      ti.draw_mode         = renderer->command_draw_mode;

      renderer->transparency_mode_index.push_back(ti);

      glEnable(GL_BLEND);
      program_uniform1ui(renderer->command_buffer->program, "draw_semi_transparent", 1);

      unsigned cur_index = 0;

      for (std::vector<TransparencyIndex>::iterator it =
            renderer->transparency_mode_index.begin();
            it != renderer->transparency_mode_index.end();
            ++it) {

         if (it->last_index == cur_index)
            continue;

         GLenum blend_func = GL_FUNC_ADD;
         GLenum blend_src = GL_CONSTANT_ALPHA;
         GLenum blend_dst = GL_CONSTANT_ALPHA;

         switch (it->transparency_mode) {
            /* 0.5xB + 0.5 x F */
            case SemiTransparencyMode_Average:
               blend_func = GL_FUNC_ADD;
               // Set to 0.5 with glBlendColor
               blend_src = GL_CONSTANT_ALPHA;
               blend_dst = GL_CONSTANT_ALPHA;
               break;
               /* 1.0xB + 1.0 x F */
            case SemiTransparencyMode_Add:
               blend_func = GL_FUNC_ADD;
               blend_src = GL_ONE;
               blend_dst = GL_ONE;
               break;
               /* 1.0xB - 1.0 x F */
            case SemiTransparencyMode_SubtractSource:
               blend_func = GL_FUNC_REVERSE_SUBTRACT;
               blend_src = GL_ONE;
               blend_dst = GL_ONE;
               break;
            case SemiTransparencyMode_AddQuarterSource:
               blend_func = GL_FUNC_ADD;
               blend_src = GL_CONSTANT_COLOR;
               blend_dst = GL_ONE;
               break;
         }

         glBlendFuncSeparate(blend_src, blend_dst, GL_ONE, GL_ZERO);
         glBlendEquationSeparate(blend_func, GL_FUNC_ADD);

         unsigned len = it->last_index - cur_index;
         GLushort *indices = renderer->semi_transparent_indices + cur_index;

         if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         {
	      /// This method doesn't call prepare_draw/finalize_draw itself, it
	      /// must be handled by the caller. This is because this command
	      /// can be called several times on the same buffer (i.e. multiple
	      /// draw calls between the prepare/finalize)
	    glBindBuffer(GL_ARRAY_BUFFER, renderer->command_buffer->id);
	    glDrawElements(it->draw_mode, len, GL_UNSIGNED_SHORT, indices);
         }

         cur_index = it->last_index;
      }
   }

   renderer->command_buffer->finalize_draw__no_bind();

   renderer->primitive_ordering = 0;
   renderer->opaque_triangle_index_pos = INDEX_BUFFER_LEN - 1;
   renderer->opaque_line_index_pos = INDEX_BUFFER_LEN - 1;
   renderer->semi_transparent_index_pos = 0;
   renderer->transparency_mode_index.clear();

   glDeleteFramebuffers(1, &_fb.id);
}

static void gl_context_reset(void)
{
    static DrawConfig config;
    puts("OpenGL context reset\n");
    glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);

    if (!glsm_ctl(GLSM_CTL_STATE_SETUP, NULL))
        return;

    /* Save this on the stack, I'm unsure if saving a ptr would
    would cause trouble because of the 'delete' below  */

    if (!static_renderer.inited)
       return;

    switch (static_renderer.state)
    {
    case GlState_Valid:
        config = static_renderer.state_data.r->config;
        break;
    case GlState_Invalid:
        config = static_renderer.state_data.c;
        break;
    }

    static_renderer.state_data.r = new GlRenderer();

    if (GlRenderer_new(static_renderer.state_data.r, config))
       static_renderer.state = GlState_Valid;
}

static void gl_context_destroy(void)
{
    glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);

    GlRenderer_free(static_renderer.state_data.r);

    if (static_renderer.state_data.r)
        delete static_renderer.state_data.r;
    static_renderer.state_data.r = NULL;

    if (static_renderer.inited)
    {
        static_renderer.state        = GlState_Invalid;
        static_renderer.state_data.c = static_renderer.state_data.r->config;
    }

    static_renderer.inited       = false;
}

static bool gl_context_framebuffer_lock(void* data)
{
    return false;
}

static bool RetroGl_alloc(VideoClock video_clock)
{
    glsm_ctx_params_t params = {0};
    retro_pixel_format f = RETRO_PIXEL_FORMAT_XRGB8888;

    if ( !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f) )
    {
        puts("Can't set pixel format\n");
	return false;
    }

    /* glsm related setup */
    params.context_reset         = gl_context_reset;
    params.context_destroy       = gl_context_destroy;
    params.framebuffer_lock      = gl_context_framebuffer_lock;
    params.environ_cb            = environ_cb;
    params.stencil               = false;
    params.imm_vbo_draw          = NULL;
    params.imm_vbo_disable       = NULL;

    if ( !glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params) ) {
        puts("Failed to init hardware context\n");
	return false;
    }

    static DrawConfig config = {
        {0, 0},         // display_top_left
        {1024, 512},    // display_resolution
        false,          // display_24bpp
	true,           // display_off
        {0, 0},         // draw_area_top_left
        {0, 0},         // draw_area_dimensions
        {0, 0},         // draw_offset
    };

    // No context until `context_reset` is called
    static_renderer.state        = GlState_Invalid;
    static_renderer.state_data.c = config;
    static_renderer.state_data.r = NULL;

    static_renderer.video_clock  = video_clock;

    return true;
}

void RetroGl_free()
{
    if (static_renderer.state_data.r)
        delete static_renderer.state_data.r;
    static_renderer.state_data.r = NULL;

    memset(&static_renderer.state_data.c, 0, sizeof(DrawConfig));
    static_renderer.state       = GlState_Invalid;
    static_renderer.video_clock = VideoClock_Ntsc;
}

struct retro_system_av_info get_av_info(VideoClock std)
{
    struct retro_variable var = {0};

    var.key = option_internal_resolution;
    uint8_t upscaling = 1;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      /* Same limitations as libretro.cpp */
      upscaling = var.value[0] -'0';
      if (var.value[1] != 'x')
      {
         upscaling  = (var.value[0] -'0') * 10;
         upscaling += (var.value[1] -'0');

	 if (upscaling == 32) /* Too high for GL */
            upscaling = 16;
      }
    }

    var.key = option_display_vram;
    bool display_vram = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (!strcmp(var.value, "enabled"))
	display_vram = true;
      else
	display_vram = false;
    }

    var.key = option_widescreen_hack;
    bool widescreen_hack = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "enabled"))
            widescreen_hack = true;
        else if (!strcmp(var.value, "disabled"))
            widescreen_hack = false;
    }

    unsigned int max_width = 0;
    unsigned int max_height = 0;

    if (display_vram) {
      max_width = 1024;
      max_height = 512;
    } else {
      // Maximum resolution supported by the PlayStation video
      // output is 640x480
      max_width = 640;
      max_height = 480;
    }

    max_width *= upscaling;
    max_height *= upscaling;

    struct retro_system_av_info info;
    memset(&info, 0, sizeof(info));

    // The base resolution will be overriden using
    // ENVIRONMENT_SET_GEOMETRY before rendering a frame so
    // this base value is not really important
    info.geometry.base_width    = max_width;
    info.geometry.base_height   = max_height;
    info.geometry.max_width     = max_width;
    info.geometry.max_height    = max_height;
    if (display_vram) {
      info.geometry.aspect_ratio = 2./1.;
    } else {
      /* TODO: Replace 4/3 with MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO */
      info.geometry.aspect_ratio  = widescreen_hack ? 16.0/9.0 : 4.0/3.0;
    }
    info.timing.sample_rate     = 44100;

    // Precise FPS values for the video output for the given
    // VideoClock. It's actually possible to configure the PlayStation GPU
    // to output with NTSC timings with the PAL clock (and vice-versa)
    // which would make this code invalid but it wouldn't make a lot of
    // sense for a game to do that.
    switch (std) {
    case VideoClock_Ntsc:
        info.timing.fps = 59.941;
        break;
    case VideoClock_Pal:
        info.timing.fps = 49.76;
        break;
    }

    return info;
}

void rsx_gl_init(void)
{
}

bool rsx_gl_open(bool is_pal)
{
   VideoClock clock = is_pal ? VideoClock_Pal : VideoClock_Ntsc;
   bool ret         = RetroGl_alloc(clock);
   static_renderer.inited = ret ? true : false; 
   return ret;
}

void rsx_gl_close(void)
{
   RetroGl_free();
   static_renderer.inited = false;
}

void rsx_gl_refresh_variables(void)
{
    GlRenderer* renderer = NULL;
    if (!static_renderer.inited)
	    return;

    switch (static_renderer.state)
    {
       case GlState_Valid:
          renderer = static_renderer.state_data.r;
          break;
       case GlState_Invalid:
          // Nothing to be done if we don't have a GL context
          return;
    }

    bool reconfigure_frontend = retro_refresh_variables(renderer);

#if 0
    if (reconfigure_frontend)
    {
        // The resolution has changed, we must tell the frontend
        // to change its format
        struct retro_variable var = {0};

        struct retro_system_av_info av_info = get_av_info(static_renderer.video_clock);

        // This call can potentially (but not necessarily) call
        // `context_destroy` and `context_reset` to reinitialize
        bool ok = environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);

        if (!ok)
        {
            puts("Couldn't change frontend resolution\n");
            puts("Try resetting to enable the new configuration\n");
        }
    }
#endif
}

bool rsx_gl_has_software_renderer(void)
{
   if (!static_renderer.inited)
      return false;
   return has_software_fb;
}

void rsx_gl_prepare_frame(void)
{
   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;

      // In case we're upscaling we need to increase the line width
      // proportionally
      glLineWidth((GLfloat)renderer->internal_upscaling);
      glPolygonMode(GL_FRONT_AND_BACK, renderer->command_polygon_mode);
      glEnable(GL_SCISSOR_TEST);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LEQUAL);
      // Used for PSX GPU command blending
      glBlendColor(0.25, 0.25, 0.25, 0.5);

      apply_scissor(renderer);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, renderer->fb_texture.id);
   }
}

void rsx_gl_finalize_frame(const void *fb, unsigned width,
      unsigned height, unsigned pitch)
{
   /* Setup 2 triangles that cover the entire framebuffer
      then copy the displayed portion of the screen from fb_out */

   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;
      // Draw pending commands
      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         GlRenderer_draw(renderer);

      // We can now render to the frontend's buffer
      bind_libretro_framebuffer(renderer);

      glDisable(GL_SCISSOR_TEST);
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_BLEND);

      /* If the display is off, just clear the screen */
      if (renderer->config.display_off && !renderer->display_vram)
      {
         if (doCleanFrame)
         {
            glClearColor(0.0, 0.0, 0.0, 0.0);
            glClear(GL_COLOR_BUFFER_BIT);
            doCleanFrame = false;
         }
      }
      else
      {
         // Bind 'fb_out' to texture unit 1
	 glActiveTexture(GL_TEXTURE1);
	 glBindTexture(GL_TEXTURE_2D, renderer->fb_out.id);

         // First we draw the visible part of fb_out
         uint16_t fb_x_start = renderer->config.display_top_left[0];
         uint16_t fb_y_start = renderer->config.display_top_left[1];
         uint16_t fb_width   = renderer->config.display_resolution[0];
         uint16_t fb_height  = renderer->config.display_resolution[1];

         GLint depth_24bpp   = (GLint) renderer->config.display_24bpp;

         if (renderer->display_vram)
         {
            // Display the entire VRAM as a 16bpp buffer
            fb_x_start = 0;
            fb_y_start = 0;
            fb_width = VRAM_WIDTH_PIXELS;
            fb_height = VRAM_HEIGHT;

            depth_24bpp = 0;
         }

	 if (renderer->output_buffer)
	 {
		 OutputVertex slice[4] =
		 {
			 { {-1.0, -1.0}, {0,         fb_height}   },
			 { { 1.0, -1.0}, {fb_width , fb_height}   },
			 { {-1.0,  1.0}, {0,         0} },
			 { { 1.0,  1.0}, {fb_width,  0} }
		 };
		 renderer->output_buffer->push_slice(slice, 4);

		 program_uniform1i(renderer->output_buffer->program, "fb", 1);
		 program_uniform2ui(renderer->output_buffer->program, "offset", fb_x_start, fb_y_start);
		 program_uniform1i(renderer->output_buffer->program,  "depth_24bpp", depth_24bpp);
		 program_uniform1ui(renderer->output_buffer->program, "internal_upscaling",
				 renderer->internal_upscaling);

		 if (!DRAWBUFFER_IS_EMPTY(renderer->output_buffer))
			 renderer->output_buffer->draw(GL_TRIANGLE_STRIP);
	 }
      }

      // Hack: copy fb_out back into fb_texture at the end of every
      // frame to make offscreen rendering kinda sorta work. Very messy
      // and slow.
      {
         ImageLoadVertex slice[4] =
         {
            {   {0,    0 }   },
            {   {1023, 0 }   },
            {   {0,    511   }   },
            {   {1023, 511   }   },
         };

         renderer->image_load_buffer->push_slice(slice, 4);

         program_uniform1i(renderer->image_load_buffer->program, "fb_texture", 1);

         glDisable(GL_SCISSOR_TEST);
         glDisable(GL_BLEND);
         glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

         Framebuffer _fb;
         Framebuffer_init(&_fb, &renderer->fb_texture);

         program_uniform1ui(renderer->image_load_buffer->program, "internal_upscaling",
               renderer->internal_upscaling);

         if (!DRAWBUFFER_IS_EMPTY(renderer->image_load_buffer))
            renderer->image_load_buffer->draw(GL_TRIANGLE_STRIP);

	 glDeleteFramebuffers(1, &_fb.id);
      }

      // Cleanup OpenGL context before returning to the frontend
      /* All of these GL calls are also done in glsm_ctl(UNBIND) */
      glDisable(GL_BLEND);
      glBlendColor(0.0, 0.0, 0.0, 0.0);
      glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
      glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, 0);
      glUseProgram(0);
      glBindVertexArray(0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glLineWidth(1.0);
      glClearColor(0.0, 0.0, 0.0, 0.0);

      // When using a hardware renderer we set the data pointer to
      // -1 to notify the frontend that the frame has been rendered
      // in the framebuffer.
      video_cb(   RETRO_HW_FRAME_BUFFER_VALID,
            renderer->frontend_resolution[0],
            renderer->frontend_resolution[1], 0);
   }
}

void rsx_gl_set_environment(retro_environment_t callback)
{
}

void rsx_gl_set_video_refresh(retro_video_refresh_t callback)
{
}

void rsx_gl_get_system_av_info(struct retro_system_av_info *info)
{
   /* TODO/FIXME - This definition seems very backwards and duplicating work */

   /* This will possibly trigger the frontend to reconfigure itself */
   rsx_gl_refresh_variables();

   struct retro_system_av_info result = get_av_info(static_renderer.video_clock);
   memcpy(info, &result, sizeof(result));
}

/* Draw commands */

void rsx_gl_set_mask_setting(uint32_t mask_set_or, uint32_t mask_eval_and)
{
   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;

      // Finish drawing anything with the current offset
      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         GlRenderer_draw(renderer);
      renderer->mask_set_or   = mask_set_or;
      renderer->mask_eval_and = mask_eval_and;
   }
}

void rsx_gl_set_draw_offset(int16_t x, int16_t y)
{
   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;

      // Finish drawing anything with the current offset
      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         GlRenderer_draw(renderer);
      renderer->config.draw_offset[0] = x;
      renderer->config.draw_offset[1] = y;
   }
}

void rsx_gl_set_tex_window(uint8_t tww, uint8_t twh,
      uint8_t twx, uint8_t twy)
{
   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;
      renderer->tex_x_mask = ~(tww << 3);
      renderer->tex_x_or   = (twx & tww) << 3;
      renderer->tex_y_mask = ~(twh << 3);
      renderer->tex_y_or   = (twy & twh) << 3;
   }
}

void  rsx_gl_set_draw_area(uint16_t x0,
			   uint16_t y0,
			   uint16_t x1,
			   uint16_t y1)
{
   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;

      // Finish drawing anything in the current area
      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         GlRenderer_draw(renderer);

      renderer->config.draw_area_top_left[0] = x0;
      renderer->config.draw_area_top_left[1] = y0;
      // Draw area coordinates are inclusive
      renderer->config.draw_area_bot_right[0] = x1 + 1;
      renderer->config.draw_area_bot_right[1] = y1 + 1;

      apply_scissor(renderer);
   }
}

void rsx_gl_set_display_mode(uint16_t x,
      uint16_t y,
      uint16_t w,
      uint16_t h,
      bool depth_24bpp)
{
   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer                    = static_renderer.state_data.r;

      renderer->config.display_top_left[0]   = x;
      renderer->config.display_top_left[1]   = y;

      renderer->config.display_resolution[0] = w;
      renderer->config.display_resolution[1] = h;
      renderer->config.display_24bpp = depth_24bpp;
   }
}


void rsx_gl_push_quad(
      float p0x,
      float p0y,
	  float p0w,
      float p1x,
      float p1y,
	  float p1w,
      float p2x,
      float p2y,
	  float p2w,
      float p3x,
      float p3y,
	  float p3w,
      uint32_t c0,
      uint32_t c1,
      uint32_t c2,
      uint32_t c3,
      uint16_t t0x,
      uint16_t t0y,
      uint16_t t1x,
      uint16_t t1y,
      uint16_t t2x,
      uint16_t t2y,
      uint16_t t3x,
      uint16_t t3y,
      uint16_t texpage_x,
      uint16_t texpage_y,
      uint16_t clut_x,
      uint16_t clut_y,
      uint8_t texture_blend_mode,
      uint8_t depth_shift,
      bool dither,
      int blend_mode)
{
   SemiTransparencyMode semi_transparency_mode = SemiTransparencyMode_Add;
   bool semi_transparent = false;
   switch (blend_mode) {
   case -1:
      semi_transparent = false;
      semi_transparency_mode = SemiTransparencyMode_Add;
      break;
   case 0:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode_Average;
      break;
   case 1:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode_Add;
      break;
   case 2:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode_SubtractSource;
      break;
   case 3:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode_AddQuarterSource;
      break;
   default:
      exit(EXIT_FAILURE);
   }

   CommandVertex v[4] =
   {
      {
          {p0x, p0y, 0.95, p0w},   /* position */
          {(uint8_t) c0, (uint8_t) (c0 >> 8), (uint8_t) (c0 >> 16)}, /* color */
          {t0x, t0y},   /* texture_coord */
          {texpage_x, texpage_y},
          {clut_x, clut_y},
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither,
          semi_transparent,
      },
      {
          {p1x, p1y, 0.95, p1w }, /* position */
          {(uint8_t) c1, (uint8_t) (c1 >> 8), (uint8_t) (c1 >> 16)}, /* color */
          {t1x, t1y}, /* texture_coord */
          {texpage_x, texpage_y},
          {clut_x, clut_y},
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither,
          semi_transparent,
      },
      {
          {p2x, p2y, 0.95, p2w }, /* position */
          {(uint8_t) c2, (uint8_t) (c2 >> 8), (uint8_t) (c2 >> 16)}, /* color */
          {t2x, t2y}, /* texture_coord */
          {texpage_x, texpage_y},
          {clut_x, clut_y},
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither,
          semi_transparent,
      },
      {
          {p3x, p3y, 0.95, p3w }, /* position */
          {(uint8_t) c3, (uint8_t) (c3 >> 8), (uint8_t) (c3 >> 16)}, /* color */
          {t3x, t3y}, /* texture_coord */
          {texpage_x, texpage_y},
          {clut_x, clut_y},
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither,
          semi_transparent,
      },
   };

   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer     = static_renderer.state_data.r;

      bool is_semi_transparent = v[0].semi_transparent == 1;
      bool is_textured         = v[0].texture_blend_mode != 0;
      // Textured semi-transparent polys can contain opaque texels (when
      // bit 15 of the color is set to 0). Therefore they're drawn twice,
      // once for the opaque texels and once for the semi-transparent
      // ones. Only untextured semi-transparent triangles don't need to be
      // drawn as opaque.
      bool is_opaque           = !is_semi_transparent || is_textured;

      vertex_preprocessing(renderer, v, 4, GL_TRIANGLES, semi_transparency_mode);

      unsigned index = DRAWBUFFER_NEXT_INDEX(renderer->command_buffer);

      for (unsigned i = 0; i < 6; i++)
      {
         if (is_opaque)
         {
            renderer->opaque_triangle_indices[renderer->opaque_triangle_index_pos--] =
               index + indices[i];
         }

         if (is_semi_transparent)
         {
            renderer->semi_transparent_indices[renderer->semi_transparent_index_pos++]
               = index + indices[i];
         }
      }

      renderer->command_buffer->push_slice(v, 4);
   }
}

void rsx_gl_push_triangle(
	  float p0x,
	  float p0y,
	  float p0w,
	  float p1x,
      float p1y,
	  float p1w,
      float p2x,
      float p2y,
	  float p2w,
      uint32_t c0,
      uint32_t c1,
      uint32_t c2,
      uint16_t t0x,
      uint16_t t0y,
      uint16_t t1x,
      uint16_t t1y,
      uint16_t t2x,
      uint16_t t2y,
      uint16_t texpage_x,
      uint16_t texpage_y,
      uint16_t clut_x,
      uint16_t clut_y,
      uint8_t texture_blend_mode,
      uint8_t depth_shift,
      bool dither,
      int blend_mode)
{
   SemiTransparencyMode semi_transparency_mode = SemiTransparencyMode_Add;
   bool semi_transparent = false;
   switch (blend_mode) {
   case -1:
      semi_transparent = false;
      semi_transparency_mode = SemiTransparencyMode_Add;
      break;
   case 0:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode_Average;
      break;
   case 1:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode_Add;
      break;
   case 2:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode_SubtractSource;
      break;
   case 3:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode_AddQuarterSource;
      break;
   default:
      exit(EXIT_FAILURE);
   }

   CommandVertex v[3] =
   {
      {
          {p0x, p0y, 0.95, p0w},   /* position */
          {(uint8_t) c0, (uint8_t) (c0 >> 8), (uint8_t) (c0 >> 16)}, /* color */
          {t0x, t0y},   /* texture_coord */
          {texpage_x, texpage_y},
          {clut_x, clut_y},
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither,
          semi_transparent,
      },
      {
          {p1x, p1y, 0.95, p1w }, /* position */
          {(uint8_t) c1, (uint8_t) (c1 >> 8), (uint8_t) (c1 >> 16)}, /* color */
          {t1x, t1y}, /* texture_coord */
          {texpage_x, texpage_y},
          {clut_x, clut_y},
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither,
          semi_transparent,
      },
      {
          {p2x, p2y, 0.95, p2w }, /* position */
          {(uint8_t) c2, (uint8_t) (c2 >> 8), (uint8_t) (c2 >> 16)}, /* color */
          {t2x, t2y}, /* texture_coord */
          {texpage_x, texpage_y},
          {clut_x, clut_y},
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither,
          semi_transparent,
      }
   };

   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;
      push_primitive(renderer, v, 3, GL_TRIANGLES, semi_transparency_mode);
   }
}

void rsx_gl_fill_rect(uint32_t color,
      uint16_t x, uint16_t y,
      uint16_t w, uint16_t h)
{

   uint16_t top_left[2]   = {x, y};
   uint16_t dimensions[2] = {w, h};
   uint8_t col[3] = {(uint8_t) color, (uint8_t) (color >> 8), (uint8_t) (color >> 16)};

   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;

      // Draw pending commands
      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         GlRenderer_draw(renderer);

      // Fill rect ignores the draw area. Save the previous value
      // and reconfigure the scissor box to the fill rectangle
      // instead.
      uint16_t draw_area_top_left[2] = {
         renderer->config.draw_area_top_left[0],
         renderer->config.draw_area_top_left[1]
      };
      uint16_t draw_area_bot_right[2] = {
         renderer->config.draw_area_bot_right[0],
         renderer->config.draw_area_bot_right[1]
      };

      renderer->config.draw_area_top_left[0]  = top_left[0];
      renderer->config.draw_area_top_left[1]  = top_left[1];
      renderer->config.draw_area_bot_right[0] = top_left[0] + dimensions[0];
      renderer->config.draw_area_bot_right[1] = top_left[1] + dimensions[1];

      apply_scissor(renderer);

      /* This scope is intentional, just like in the Rust version */
      {
         // Bind the out framebuffer
         Framebuffer _fb;
         Framebuffer_init(&_fb, &renderer->fb_out);

         if (doCleanFrame)
         {
            glClearColor(   (float) col[0] / 255.0,
                  (float) col[1] / 255.0,
                  (float) col[2] / 255.0,
                  // XXX Not entirely sure what happens to
                  // the mask bit in fill_rect commands
                  0.0);
            glClear(GL_COLOR_BUFFER_BIT);
            doCleanFrame = false;
         }

	 glDeleteFramebuffers(1, &_fb.id);
      }

      // Reconfigure the draw area
      renderer->config.draw_area_top_left[0]    = draw_area_top_left[0];
      renderer->config.draw_area_top_left[1]    = draw_area_top_left[1];
      renderer->config.draw_area_bot_right[0]   = draw_area_bot_right[0];
      renderer->config.draw_area_bot_right[1]   = draw_area_bot_right[1];

      apply_scissor(renderer);
   }
}


void rsx_gl_copy_rect(
      uint16_t src_x, uint16_t src_y,
      uint16_t dst_x, uint16_t dst_y,
      uint16_t w, uint16_t h)
{

    if (static_renderer.state == GlState_Valid)
    {
       GlRenderer *renderer        = static_renderer.state_data.r;
       uint16_t source_top_left[2] = {src_x, src_y};
       uint16_t target_top_left[2] = {dst_x, dst_y};
       uint16_t dimensions[2]      = {w, h};

       // Draw pending commands
       if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         GlRenderer_draw(renderer);

       uint32_t upscale = renderer->internal_upscaling;

       GLint src_x = (GLint) source_top_left[0] * (GLint) upscale;
       GLint src_y = (GLint) source_top_left[1] * (GLint) upscale;
       GLint dst_x = (GLint) target_top_left[0] * (GLint) upscale;
       GLint dst_y = (GLint) target_top_left[1] * (GLint) upscale;

       GLsizei w = (GLsizei) dimensions[0] * (GLsizei) upscale;
       GLsizei h = (GLsizei) dimensions[1] * (GLsizei) upscale;

#ifdef NEW_COPY_RECT
       /* TODO/FIXME - buggy code!
        *
        * Dead or Alive/Tekken 3 (high-res interlaced game) has screen
        * flickering issues with this code! */

       // The diagonal is duplicated. I originally used "1, 2, 1, 2" to
       // duplicate the diagonal but I believe it was incorrect because of
       // the OpenGL filling convention. At least it's what TinyTiger told
       // me...

       GLuint fb;

       glGenFramebuffers(1, &fb);
       glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
       glFramebufferTexture(GL_READ_FRAMEBUFFER,
             GL_COLOR_ATTACHMENT0,
             renderer->fb_out->id,
             0);

       glReadBuffer(GL_COLOR_ATTACHMENT0);

       // Can I bind the same texture to the framebuffer and
       // GL_TEXTURE_2D? Something tells me this is undefined
       // behaviour. I could use glReadPixels and glWritePixels instead
       // or something like that.
       glBindTexture(GL_TEXTURE_2D, renderer->fb_out->id);

       glCopyTexSubImage2D(GL_TEXTURE_2D, 0, dst_x, dst_y, src_x, src_y, w, h);

       glDeleteFramebuffers(1, &fb);
#else

       // The diagonal is duplicated

       // XXX CopyImageSubData gives undefined results if the source
       // and target area overlap, this should be handled
       // explicitely
       /* TODO - OpenGL 4.3 and GLES 3.2 requirement! FIXME! */
       glCopyImageSubData( renderer->fb_out.id, GL_TEXTURE_2D, 0, src_x, src_y, 0,
             renderer->fb_out.id, GL_TEXTURE_2D, 0, dst_x, dst_y, 0,
             w, h, 1 );
#endif

#ifdef DEBUG
       get_error();
#endif
    }
}

void rsx_gl_push_line(int16_t p0x,
		      int16_t p0y,
		      int16_t p1x,
		      int16_t p1y,
		      uint32_t c0,
		      uint32_t c1,
		      bool dither,
		      int blend_mode)
{
   SemiTransparencyMode semi_transparency_mode = SemiTransparencyMode_Add;
   bool semi_transparent = false;

   switch (blend_mode)
   {
      case -1:
         semi_transparent = false;
         semi_transparency_mode = SemiTransparencyMode_Add;
         break;
      case 0:
         semi_transparent = true;
         semi_transparency_mode = SemiTransparencyMode_Average;
         break;
      case 1:
         semi_transparent = true;
         semi_transparency_mode = SemiTransparencyMode_Add;
         break;
      case 2:
         semi_transparent = true;
         semi_transparency_mode = SemiTransparencyMode_SubtractSource;
         break;
      case 3:
         semi_transparent = true;
         semi_transparency_mode = SemiTransparencyMode_AddQuarterSource;
         break;
      default:
         exit(EXIT_FAILURE);
   }


   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer = static_renderer.state_data.r;

      CommandVertex v[2] = {
	      {
		      {(float)p0x, (float)p0y, 0., 1.0}, /* position */
		      {(uint8_t) c0, (uint8_t) (c0 >> 8), (uint8_t) (c0 >> 16)}, /* color */
		      {0, 0}, /* texture_coord */
		      {0, 0}, /* texture_page */
		      {0, 0}, /* clut */
		      0,      /* texture_blend_mode */
		      0,      /* depth_shift */
		      (uint8_t) dither,
		      semi_transparent,
	      },
	      {
		      {(float)p1x, (float)p1y, 0., 1.0}, /* position */
		      {(uint8_t) c1, (uint8_t) (c1 >> 8), (uint8_t) (c1 >> 16)}, /* color */
		      {0, 0}, /* texture_coord */
		      {0, 0}, /* texture_page */
		      {0, 0}, /* clut */
		      0,      /* texture_blend_mode */
		      0,      /* depth_shift */
		      (uint8_t) dither,
		      semi_transparent,
	      }
      };
      push_primitive(renderer, v, 2, GL_LINES, semi_transparency_mode);
   }
}

void rsx_gl_load_image(uint16_t x, uint16_t y,
      uint16_t w, uint16_t h,
      uint16_t *vram)
{

   /* TODO FIXME - upload_vram_window expects a
      uint16_t[VRAM_HEIGHT*VRAM_WIDTH_PIXELS] array arg instead of a ptr */

   if (static_renderer.state == GlState_Valid)
   {
      uint16_t top_left[2];
      uint16_t dimensions[2];
      GlRenderer *renderer   = static_renderer.state_data.r;

      top_left[0]            = x;
      top_left[1]            = y;
      dimensions[0]          = w;
      dimensions[1]          = h;

      if (!DRAWBUFFER_IS_EMPTY(renderer->command_buffer))
         GlRenderer_draw(renderer);

      Texture_set_sub_image_window(
            &renderer->fb_texture,
            top_left,
            dimensions,
            (size_t) VRAM_WIDTH_PIXELS,
            GL_RGBA,
            GL_UNSIGNED_SHORT_1_5_5_5_REV,
            vram);

      uint16_t x_start    = top_left[0];
      uint16_t x_end      = x_start + dimensions[0];
      uint16_t y_start    = top_left[1];
      uint16_t y_end      = y_start + dimensions[1];

      const size_t slice_len = 4;
      ImageLoadVertex slice[slice_len] =
      {
         {   {x_start,   y_start }   },
         {   {x_end,     y_start }   },
         {   {x_start,   y_end   }   },
         {   {x_end,     y_end   }   }
      };
      renderer->image_load_buffer->push_slice(slice, slice_len);

      program_uniform1i(renderer->image_load_buffer->program, "fb_texture", 0);
      // fb_texture is always at 1x
      program_uniform1ui(renderer->image_load_buffer->program, "internal_upscaling", 1);

      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_BLEND);
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

      // Bind the output framebuffer
      Framebuffer _fb;

      Framebuffer_init(&_fb, &renderer->fb_out);

      if (!DRAWBUFFER_IS_EMPTY(renderer->image_load_buffer))
         renderer->image_load_buffer->draw(GL_TRIANGLE_STRIP);
      glPolygonMode(GL_FRONT_AND_BACK, renderer->command_polygon_mode);
      glEnable(GL_SCISSOR_TEST);

#ifdef DEBUG
      get_error();
#endif

      glDeleteFramebuffers(1, &_fb.id);
   }
}

void rsx_gl_toggle_display(bool status)
{
   if (static_renderer.state == GlState_Valid)
   {
      GlRenderer *renderer          = static_renderer.state_data.r;
      renderer->config.display_off = status;
   }
}
