#include "canvas_sdl.hpp"

namespace a2ui {

SdlCanvas::SdlCanvas(SDL_Renderer* renderer, const std::string& fontPath) : SdlCanvas(renderer, readFont(fontPath)) {}

SdlCanvas::SdlCanvas(SDL_Renderer* renderer, std::vector<unsigned char> font) : GeometryCanvas(std::move(font)), renderer_(renderer) {
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

SdlCanvas::~SdlCanvas() {
  for (SDL_Texture* t : textures_) SDL_DestroyTexture(t);
}

SdlCanvas::Texture SdlCanvas::createTexture(int w, int h, const uint32_t* rgba) {
  SDL_Texture* t = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
  SDL_UpdateTexture(t, nullptr, rgba, w * 4);
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
  textures_.push_back(t);
  return reinterpret_cast<Texture>(t);
}

void SdlCanvas::updateTexture(Texture t, int x, int y, int w, int h, const uint32_t* rgba) {
  SDL_Rect rect{x, y, w, h};
  SDL_UpdateTexture(reinterpret_cast<SDL_Texture*>(t), &rect, rgba, w * 4);
}

void SdlCanvas::drawTriangles(Texture t, const std::vector<Vertex>& v, const std::vector<int>& idx) {
  scratch_.resize(v.size());
  for (size_t k = 0; k < v.size(); ++k) scratch_[k] = {{v[k].x, v[k].y}, {v[k].color.r, v[k].color.g, v[k].color.b, v[k].color.a}, {v[k].u, v[k].v}};
  SDL_RenderGeometry(renderer_, reinterpret_cast<SDL_Texture*>(t), scratch_.data(), int(scratch_.size()), idx.data(), int(idx.size()));
}

void SdlCanvas::clip(const Rect* r) {
  // In the renderer's logical coordinates, like everything drawn here.
  if (!r) {
    SDL_RenderSetClipRect(renderer_, nullptr);
    return;
  }
  SDL_Rect rect{int(r->x), int(r->y), int(r->w + 0.999f), int(r->h + 0.999f)};
  SDL_RenderSetClipRect(renderer_, &rect);
}

void SdlCanvas::clear(Color top, Color bottom) {
  SDL_RenderSetClipRect(renderer_, nullptr);
  int w, h;
  SDL_RenderGetLogicalSize(renderer_, &w, &h);
  if (w == 0) SDL_GetRendererOutputSize(renderer_, &w, &h);
  SDL_SetRenderDrawColor(renderer_, bottom.r, bottom.g, bottom.b, 255);
  SDL_RenderClear(renderer_);
  drawTriangles(0, {{0, 0, top, 0, 0}, {float(w), 0, top, 0, 0}, {float(w), float(h), bottom, 0, 0}, {0, float(h), bottom, 0, 0}}, {0, 1, 2, 0, 2, 3});
}

}  // namespace a2ui
