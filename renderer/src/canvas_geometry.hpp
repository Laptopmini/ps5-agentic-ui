// The Canvas as triangles: rounded rects, strokes, circles and text (stb_truetype glyph atlases,
// one per pixel size) become vertex lists. Text: common Latin glyphs are packed up front; any other
// character is drawn on first use from the first font that has it (the main font, then the
// fallbacks: emoji, symbols), monochrome like the rest of the text. A backend only uploads textures and draws triangles:
// SdlCanvas (SDL_Renderer, software on the PS5) and GlCanvas (OpenGL ES 2 / desktop GL 2.1).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "a2ui/view.hpp"

namespace a2ui {

struct Vertex {
  float x, y;
  Color color;
  float u, v;
};

class GeometryCanvas : public Canvas {
 public:
  /** `font`: a TTF in memory. */
  explicit GeometryCanvas(std::vector<unsigned char> font);
  ~GeometryCanvas() override;

  void fillRect(Rect r, Color c, float radius) override;
  void fillRectCorners(Rect r, Color c, float tl, float tr, float br, float bl) override;
  void strokeRect(Rect r, Color c, float radius, float width) override;
  void fillCircle(float cx, float cy, float radius, Color c) override;
  void strokeCircle(float cx, float cy, float radius, Color c, float width) override;
  void fillTriangle(float x1, float y1, float x2, float y2, float x3, float y3, Color c) override;
  float text(float x, float y, const std::string& utf8, float size, Color c) override;
  float measure(const std::string& utf8, float size) override;
  float lineHeight(float size) override;
  /** Pictures are read on first use, halved until they're at most 256 px, and kept (up to 64). */
  bool image(Rect r, const std::string& source, float radius) override;

  /** A font to draw characters the main font lacks (emoji, symbols), tried in the order added. */
  void addFallbackFont(std::vector<unsigned char> font);

  /** Read a TTF file; throws std::runtime_error if it can't be read. */
  static std::vector<unsigned char> readFont(const std::string& path);

 protected:
  /** A texture the backend made; 0 means none (plain color). */
  using Texture = uintptr_t;
  /** White RGBA pixels with coverage in alpha (the glyph atlas). */
  virtual Texture createTexture(int w, int h, const uint32_t* rgba) = 0;  // the backend owns and frees it
  /** Replace a rectangle of a texture made by createTexture (`rgba` is w×h). */
  virtual void updateTexture(Texture t, int x, int y, int w, int h, const uint32_t* rgba) = 0;
  /** Draw indexed triangles (indices into `v`), textured with `t` times the vertex colors. */
  virtual void drawTriangles(Texture t, const std::vector<Vertex>& v, const std::vector<int>& idx) = 0;

 private:
  struct Atlas;
  struct Font;
  struct Glyph;
  Atlas& atlas(float size);
  /** A glyph for `cp` at this size, from any font; nullptr if none has it. */
  const Glyph* glyph(Atlas& a, int px, int cp);
  void outline(Rect r, const float radii[4], std::vector<Vertex>& out, Color c);
  void fillOutline(Rect r, const float radii[4], Color c);

  std::vector<unsigned char> font_;
  std::vector<std::unique_ptr<Font>> fonts_;  // the main font, then the fallbacks
  std::map<int, std::unique_ptr<Atlas>> atlases_;
  std::map<std::string, Texture> images_;  // by source; 0 = couldn't be read (not tried again)
};

}  // namespace a2ui
