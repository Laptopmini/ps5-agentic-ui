// Canvas on SDL2's renderer (software on the PS5, Metal/GL on the Mac): GeometryCanvas's
// triangles go to SDL_RenderGeometry.
#pragma once

#include <SDL.h>

#include <string>
#include <vector>

#include "canvas_geometry.hpp"

namespace a2ui {

class SdlCanvas : public GeometryCanvas {
 public:
  /** `fontPath`: a TTF file. Throws std::runtime_error if it can't be read. */
  SdlCanvas(SDL_Renderer* renderer, const std::string& fontPath);
  /** A TTF already in memory (the console build links the font into the payload). */
  SdlCanvas(SDL_Renderer* renderer, std::vector<unsigned char> font);
  ~SdlCanvas() override;

  void clear(Color top, Color bottom) override;
  void clip(const Rect* r) override;

 protected:
  Texture createTexture(int w, int h, const uint32_t* rgba) override;
  void updateTexture(Texture t, int x, int y, int w, int h, const uint32_t* rgba) override;
  void drawTriangles(Texture t, const std::vector<Vertex>& v, const std::vector<int>& idx) override;

 private:
  SDL_Renderer* renderer_;
  std::vector<SDL_Texture*> textures_;
  std::vector<SDL_Vertex> scratch_;
};

}  // namespace a2ui
