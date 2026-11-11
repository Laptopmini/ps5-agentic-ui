#include "canvas_gl.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>

#include "a2ui/view.hpp"

namespace a2ui {
namespace {

// The few GL types and constants used (identical in GL 2.1 and GLES 2).
using GLenum = unsigned;
using GLuint = unsigned;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLbitfield = unsigned;
using GLchar = char;
constexpr GLenum TRIANGLES = 0x0004, UNSIGNED_BYTE = 0x1401, UNSIGNED_SHORT = 0x1403, FLOAT = 0x1406, RGBA = 0x1908;
constexpr GLenum TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, LINEAR = 0x2601;
constexpr GLenum TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, CLAMP_TO_EDGE = 0x812F;
constexpr GLenum SCISSOR_TEST = 0x0C11;
constexpr GLenum BLEND = 0x0BE2, SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303, ONE = 1;
constexpr GLenum VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82;
constexpr GLenum COLOR_BUFFER_BIT = 0x4000, FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum PACK_ALIGNMENT = 0x0D05, UNPACK_ALIGNMENT = 0x0CF5, VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02;
constexpr GLboolean TRUE_ = 1, FALSE_ = 0;

// No #version: GLSL 1.10 on desktop GL 2.1 and GLSL ES 1.00 on GLES 2 both accept this.
const char* kVertex = R"(
attribute vec2 a_pos;
attribute vec4 a_color;
attribute vec2 a_uv;
uniform vec2 u_scale;
varying vec4 v_color;
varying vec2 v_uv;
void main() {
  gl_Position = vec4(a_pos.x * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y, 0.0, 1.0);
  v_color = a_color;
  v_uv = a_uv;
}
)";
const char* kFragment = R"(
#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D u_tex;
varying vec4 v_color;
varying vec2 v_uv;
void main() { gl_FragColor = texture2D(u_tex, v_uv) * v_color; }
)";

}  // namespace

struct GlCanvas::Gl {
#define GLFN(ret, name, ...) ret (*name)(__VA_ARGS__) = nullptr;
#include "canvas_gl_functions.inc"
#undef GLFN
};

GlCanvas::GlCanvas(SDL_Window* window, std::vector<unsigned char> font, bool offscreen)
    : GeometryCanvas(std::move(font)), window_(window), gl_(new Gl), offscreen_(offscreen) {
  Gl& g = *gl_;
#define GLFN(ret, name, ...) \
  g.name = reinterpret_cast<ret (*)(__VA_ARGS__)>(SDL_GL_GetProcAddress("gl" #name)); \
  if (!g.name) throw std::runtime_error("missing GL function gl" #name);
#include "canvas_gl_functions.inc"
#undef GLFN

  auto compile = [&](GLenum type, const char* src) {
    GLuint s = g.CreateShader(type);
    g.ShaderSource(s, 1, &src, nullptr);
    g.CompileShader(s);
    GLint ok = 0;
    g.GetShaderiv(s, COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512] = {};
      g.GetShaderInfoLog(s, sizeof log, nullptr, log);
      throw std::runtime_error(std::string("shader: ") + log);
    }
    return s;
  };
  program_ = g.CreateProgram();
  g.AttachShader(program_, compile(VERTEX_SHADER, kVertex));
  g.AttachShader(program_, compile(FRAGMENT_SHADER, kFragment));
  g.BindAttribLocation(program_, 0, "a_pos");
  g.BindAttribLocation(program_, 1, "a_color");
  g.BindAttribLocation(program_, 2, "a_uv");
  g.LinkProgram(program_);
  GLint ok = 0;
  g.GetProgramiv(program_, LINK_STATUS, &ok);
  if (!ok) throw std::runtime_error("shader program did not link");
  uScale_ = g.GetUniformLocation(program_, "u_scale");
  uTex_ = g.GetUniformLocation(program_, "u_tex");

  const uint32_t white = 0xFFFFFFFF;  // plain color = white texture times the vertex color
  white_ = static_cast<unsigned>(createTexture(1, 1, &white));

  if (offscreen_) {
    g.GenTextures(1, &fboColor_);
    g.BindTexture(TEXTURE_2D, fboColor_);
    g.TexImage2D(TEXTURE_2D, 0, RGBA, int(View::kWidth), int(View::kHeight), 0, RGBA, UNSIGNED_BYTE, nullptr);
    g.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
    g.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    g.GenFramebuffers(1, &fbo_);
    g.BindFramebuffer(FRAMEBUFFER, fbo_);
    g.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, fboColor_, 0);
    if (g.CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) throw std::runtime_error("offscreen framebuffer incomplete");
  }
}

GlCanvas::~GlCanvas() {
  Gl& g = *gl_;
  if (!textures_.empty()) g.DeleteTextures(GLsizei(textures_.size()), textures_.data());
  if (fbo_) g.DeleteFramebuffers(1, &fbo_);
  if (fboColor_) g.DeleteTextures(1, &fboColor_);
  if (program_) g.DeleteProgram(program_);
  delete gl_;
}

std::string GlCanvas::info() const {
  auto str = [&](GLenum e) {
    const unsigned char* s = gl_->GetString(e);
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string("?");
  };
  return str(VENDOR) + " / " + str(RENDERER) + " / " + str(VERSION);
}

void GlCanvas::updateTexture(Texture t, int x, int y, int w, int h, const uint32_t* rgba) {
  Gl& g = *gl_;
  flush();  // batched triangles may use this texture as it was
  g.BindTexture(TEXTURE_2D, GLuint(t));
  g.PixelStorei(UNPACK_ALIGNMENT, 4);
  g.TexSubImage2D(TEXTURE_2D, 0, x, y, w, h, RGBA, UNSIGNED_BYTE, rgba);
}

GlCanvas::Texture GlCanvas::createTexture(int w, int h, const uint32_t* rgba) {
  Gl& g = *gl_;
  flush();
  GLuint t = 0;
  g.GenTextures(1, &t);
  g.BindTexture(TEXTURE_2D, t);
  g.PixelStorei(UNPACK_ALIGNMENT, 4);
  g.TexImage2D(TEXTURE_2D, 0, RGBA, w, h, 0, RGBA, UNSIGNED_BYTE, rgba);
  g.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
  g.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
  g.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
  g.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
  textures_.push_back(t);
  return t;
}

void GlCanvas::viewport() {
  Gl& g = *gl_;
  if (offscreen_) {
    g.BindFramebuffer(FRAMEBUFFER, fbo_);
    g.Viewport(0, 0, int(View::kWidth), int(View::kHeight));
    vx_ = vy_ = 0;
    vscale_ = 1;
    return;
  }
  // Fit 16:9 into the drawable (HiDPI-aware), letterboxed.
  int dw = 0, dh = 0;
  SDL_GL_GetDrawableSize(window_, &dw, &dh);
  float scale = std::min(dw / View::kWidth, dh / View::kHeight);
  int vw = int(View::kWidth * scale), vh = int(View::kHeight * scale);
  g.Viewport((dw - vw) / 2, (dh - vh) / 2, vw, vh);
  vx_ = float((dw - vw) / 2);
  vy_ = float((dh - vh) / 2);
  vscale_ = scale;
}

void GlCanvas::clip(const Rect* r) {
  Gl& g = *gl_;
  flush();  // what's batched was drawn under the previous clip
  if (!r) {
    g.Disable(SCISSOR_TEST);
    return;
  }
  // GL counts from the bottom-left, in drawable pixels.
  const float x0 = vx_ + r->x * vscale_, y0 = vy_ + (View::kHeight - r->y - r->h) * vscale_;
  g.Enable(SCISSOR_TEST);
  g.Scissor(int(std::floor(x0)), int(std::floor(y0)), int(std::ceil(r->w * vscale_)), int(std::ceil(r->h * vscale_)));
}

void GlCanvas::clear(Color top, Color bottom) {
  Gl& g = *gl_;
  batch_.clear();
  batchIdx_.clear();
  viewport();
  g.Disable(SCISSOR_TEST);
  g.ClearColor(0, 0, 0, 1);
  g.Clear(COLOR_BUFFER_BIT);
  g.UseProgram(program_);
  g.Uniform2f(uScale_, 2.0f / View::kWidth, 2.0f / View::kHeight);
  g.Uniform1i(uTex_, 0);
  g.ActiveTexture(TEXTURE0);
  g.Enable(BLEND);
  // Straight alpha for color; keep the destination opaque (screenshots and the console's display).
  g.BlendFuncSeparate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA);
  drawTriangles(0, {{0, 0, top, 0, 0}, {View::kWidth, 0, top, 0, 0}, {View::kWidth, View::kHeight, bottom, 0, 0}, {0, View::kHeight, bottom, 0, 0}}, {0, 1, 2, 0, 2, 3});
}

void GlCanvas::drawTriangles(Texture t, const std::vector<Vertex>& v, const std::vector<int>& idx) {
  if (t == 0) t = white_;
  if (t != batchTexture_ || batch_.size() + v.size() > 65000) flush();
  batchTexture_ = t;
  uint16_t base = static_cast<uint16_t>(batch_.size());
  batch_.insert(batch_.end(), v.begin(), v.end());
  for (int i : idx) batchIdx_.push_back(static_cast<uint16_t>(base + i));
}

void GlCanvas::flush() {
  if (batchIdx_.empty()) return;
  Gl& g = *gl_;
  g.BindTexture(TEXTURE_2D, GLuint(batchTexture_));
  const char* base = reinterpret_cast<const char*>(batch_.data());
  g.EnableVertexAttribArray(0);
  g.EnableVertexAttribArray(1);
  g.EnableVertexAttribArray(2);
  g.VertexAttribPointer(0, 2, FLOAT, FALSE_, sizeof(Vertex), base + offsetof(Vertex, x));
  g.VertexAttribPointer(1, 4, UNSIGNED_BYTE, TRUE_, sizeof(Vertex), base + offsetof(Vertex, color));
  g.VertexAttribPointer(2, 2, FLOAT, FALSE_, sizeof(Vertex), base + offsetof(Vertex, u));
  g.DrawElements(TRIANGLES, GLsizei(batchIdx_.size()), UNSIGNED_SHORT, batchIdx_.data());
  batch_.clear();
  batchIdx_.clear();
}

void GlCanvas::present() {
  flush();
  if (!offscreen_) SDL_GL_SwapWindow(window_);
}

SDL_Surface* GlCanvas::readPixels() {
  flush();
  Gl& g = *gl_;
  const int w = int(View::kWidth), h = int(View::kHeight);
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  if (offscreen_) g.BindFramebuffer(FRAMEBUFFER, fbo_);
  g.PixelStorei(PACK_ALIGNMENT, 4);
  g.ReadPixels(0, 0, w, h, RGBA, UNSIGNED_BYTE, rgba.data());
  SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
  for (int y = 0; y < h; ++y) {  // GL rows go bottom-up
    const uint8_t* src = &rgba[size_t(h - 1 - y) * w * 4];
    uint8_t* dst = static_cast<uint8_t*>(out->pixels) + size_t(y) * out->pitch;
    for (int x = 0; x < w; ++x) {
      dst[x * 3] = src[x * 4];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }
  return out;
}

}  // namespace a2ui
