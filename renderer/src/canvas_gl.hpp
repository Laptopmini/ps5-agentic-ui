// Canvas on the GPU: GeometryCanvas's triangles drawn with OpenGL ES 2 (the PS5, through Mesa's
// zink on RADV) or desktop OpenGL 2.1 (the Mac). Functions are loaded with SDL_GL_GetProcAddress,
// so no GL headers or link libraries are needed. Draws are batched per texture; the view's
// 1920×1080 logical space is scaled to the drawable, letterboxed if the aspect differs.
#pragma once

#include <SDL.h>

#include <string>
#include <vector>

#include "canvas_geometry.hpp"

namespace a2ui {

class GlCanvas : public GeometryCanvas {
 public:
  /**
   * Needs a current GL context on `window`. `offscreen`: draw into a 1920×1080 framebuffer
   * object instead of the window (headless screenshots). Throws std::runtime_error on failure.
   */
  GlCanvas(SDL_Window* window, std::vector<unsigned char> font, bool offscreen = false);
  ~GlCanvas() override;

  void clear(Color top, Color bottom) override;
  void clip(const Rect* r) override;
  /** Flush and show the frame (swap, unless offscreen). */
  void present();
  /** The last frame as a 24-bit surface (caller frees), for screenshots. */
  SDL_Surface* readPixels();
  /** "GL_VENDOR / GL_RENDERER / GL_VERSION", for logs. */
  std::string info() const;

 protected:
  Texture createTexture(int w, int h, const uint32_t* rgba) override;
  void updateTexture(Texture t, int x, int y, int w, int h, const uint32_t* rgba) override;
  void drawTriangles(Texture t, const std::vector<Vertex>& v, const std::vector<int>& idx) override;

 private:
  struct Gl;
  void flush();
  void viewport();

  SDL_Window* window_;
  Gl* gl_;
  bool offscreen_;
  float vx_ = 0, vy_ = 0, vscale_ = 1;  // the viewport: logical space → drawable pixels
  unsigned program_ = 0, white_ = 0, fbo_ = 0, fboColor_ = 0;
  int uScale_ = -1, uTex_ = -1;
  std::vector<unsigned> textures_;
  // The batch: vertices and 16-bit indices for one texture.
  Texture batchTexture_ = 0;
  std::vector<Vertex> batch_;
  std::vector<uint16_t> batchIdx_;
};

}  // namespace a2ui
