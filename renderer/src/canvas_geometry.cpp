#include "canvas_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
// Reading a font from a file pulls in C++ iostreams (and their locale/iconv machinery). A build
// that only ever uses in-memory fonts (the native title, which embeds its font) defines
// A2UI_NO_FONT_FILE to drop it and keep the binary free of that dependency.
#ifndef A2UI_NO_FONT_FILE
#include <fstream>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_WARNINGS
#include "stb/stb_image.h"

namespace a2ui {
namespace {

constexpr int kAtlasSize = 1024;
constexpr int kPageSize = 512;  // pages for glyphs drawn on first use
// Code point ranges baked into each atlas up front: ASCII, Latin-1, general punctuation, a few
// symbols. Anything else is drawn on first use (glyph()).
constexpr struct { int first, count; } kRanges[] = {{0x20, 0x5F}, {0xA0, 0x60}, {0x2010, 0x18}, {0x2030, 0x0B}, {0x20AC, 1}, {0x2122, 1}, {0x2190, 4},
                                                    {0x2600, 0x20}, {0x2713, 5}};

/** Decode one UTF-8 code point at `i` and advance `i`; returns U+FFFD on bad input. */
int nextCodepoint(const std::string& s, size_t& i) {
  unsigned char c = static_cast<unsigned char>(s[i++]);
  if (c < 0x80) return c;
  int extra = (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
  if (extra < 0) return 0xFFFD;
  int cp = c & (0x3F >> extra);
  for (int k = 0; k < extra; ++k) {
    if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
  }
  return cp;
}

}  // namespace

struct GeometryCanvas::Font {
  std::vector<unsigned char> data;
  stbtt_fontinfo info{};
};

/** A glyph's quad relative to the pen on the baseline, its texture coordinates, and its advance. */
struct GeometryCanvas::Glyph {
  Texture texture = 0;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0, s0 = 0, t0 = 0, s1 = 0, t1 = 0, advance = 0;
};

struct GeometryCanvas::Atlas {
  Texture texture = 0;
  std::map<int, Glyph> glyphs;
  std::set<int> missing;  // no font has it
  struct Page {
    Texture texture = 0;
    int x = 1, y = 1, rowHeight = 0;  // where the next glyph goes (shelf packing)
  };
  std::vector<Page> pages;
  float ascent = 0, lineHeight = 0;
};

GeometryCanvas::GeometryCanvas(std::vector<unsigned char> font) : font_(std::move(font)) {
  auto f = std::make_unique<Font>();
  stbtt_InitFont(&f->info, font_.data(), 0);
  fonts_.push_back(std::move(f));
}

void GeometryCanvas::addFallbackFont(std::vector<unsigned char> font) {
  auto f = std::make_unique<Font>();
  f->data = std::move(font);
  if (!stbtt_InitFont(&f->info, f->data.data(), stbtt_GetFontOffsetForIndex(f->data.data(), 0))) return;
  fonts_.push_back(std::move(f));
  for (auto& [px, a] : atlases_) a->missing.clear();  // the new font may have them
}

GeometryCanvas::~GeometryCanvas() = default;  // backends destroy their textures (see their destructors)

std::vector<unsigned char> GeometryCanvas::readFont(const std::string& path) {
#ifdef A2UI_NO_FONT_FILE
  (void)path;  // in-memory fonts only (the native title embeds its font)
  std::abort();
#else
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // Exceptions-optional, so this compiles for the native-title build (-fno-exceptions).
#if defined(__cpp_exceptions) && __cpp_exceptions
    throw std::runtime_error("cannot read font " + path);
#else
    std::fprintf(stderr, "cannot read font %s\n", path.c_str());
    std::abort();
#endif
  }
  return {std::istreambuf_iterator<char>(in), {}};
#endif
}

GeometryCanvas::Atlas& GeometryCanvas::atlas(float size) {
  int px = static_cast<int>(std::lround(size));
  auto it = atlases_.find(px);
  if (it != atlases_.end()) return *it->second;

  auto a = std::make_unique<Atlas>();
  std::vector<unsigned char> alpha(kAtlasSize * kAtlasSize);
  std::map<int, stbtt_packedchar> packed;
  stbtt_pack_context pc;
  stbtt_PackBegin(&pc, alpha.data(), kAtlasSize, kAtlasSize, 0, 1, nullptr);
  stbtt_PackSetOversampling(&pc, 2, 2);
  stbtt_fontinfo info;
  stbtt_InitFont(&info, font_.data(), 0);
  for (auto r : kRanges) {
    std::vector<stbtt_packedchar> chars(r.count);
    stbtt_PackFontRange(&pc, font_.data(), 0, static_cast<float>(px), r.first, r.count, chars.data());
    // Keep only glyphs the font really has; the others go through glyph() (fallback fonts).
    for (int k = 0; k < r.count; ++k)
      if (stbtt_FindGlyphIndex(&info, r.first + k) != 0) packed[r.first + k] = chars[k];
  }
  stbtt_PackEnd(&pc);

  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
  float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(px));
  a->ascent = ascent * scale;
  a->lineHeight = (ascent - descent + gap) * scale * 1.15f;

  // White glyphs with coverage in alpha; the color comes from the vertices. Byte order R,G,B,A.
  std::vector<uint32_t> rgba(kAtlasSize * kAtlasSize);
  for (size_t k = 0; k < alpha.size(); ++k) rgba[k] = (static_cast<uint32_t>(alpha[k]) << 24) | 0x00FFFFFF;
  a->texture = createTexture(kAtlasSize, kAtlasSize, rgba.data());
  for (auto& [cp, pchar] : packed) {
    stbtt_aligned_quad q;
    float qx = 0, qy = 0;
    stbtt_GetPackedQuad(&pchar, kAtlasSize, kAtlasSize, 0, &qx, &qy, &q, 0);
    a->glyphs[cp] = {a->texture, q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1, qx};
  }

  Atlas& ref = *a;
  atlases_[px] = std::move(a);
  return ref;
}

const GeometryCanvas::Glyph* GeometryCanvas::glyph(Atlas& a, int px, int cp) {
  if (auto g = a.glyphs.find(cp); g != a.glyphs.end()) return &g->second;
  if (a.missing.count(cp)) return nullptr;
  // Invisible on their own: variation selectors (emoji vs text style), the zero-width joiner,
  // skin-tone modifiers and tag characters. Drawn, they'd be boxes or swatches.
  if ((cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0000 && cp <= 0xE007F)) {
    a.missing.insert(cp);
    return nullptr;
  }
  const float emPixels = stbtt_ScaleForPixelHeight(&fonts_[0]->info, static_cast<float>(px)) / stbtt_ScaleForMappingEmToPixels(&fonts_[0]->info, 1);
  for (auto& f : fonts_) {
    int index = stbtt_FindGlyphIndex(&f->info, cp);
    if (index == 0) continue;
    // Same em size as the main font: fonts' line heights differ a lot (Noto Sans Symbols' is
    // nearly twice Noto Sans's), so scaling each by its own would shrink some glyphs.
    const float scale = stbtt_ScaleForMappingEmToPixels(&f->info, emPixels);
    int advance, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&f->info, index, &advance, &lsb);
    stbtt_GetGlyphBitmapBox(&f->info, index, scale, scale, &x0, &y0, &x1, &y1);
    Glyph g;
    g.advance = advance * scale;
    const int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && w < kPageSize - 2 && h < kPageSize - 2) {
      // Shelf packing, one pixel apart; a new page when this one is full.
      if (a.pages.empty() || a.pages.back().x + w + 1 > kPageSize) {
        if (!a.pages.empty()) {
          Atlas::Page& p = a.pages.back();
          p.x = 1;
          p.y += p.rowHeight + 1;
          p.rowHeight = 0;
        }
      }
      if (a.pages.empty() || a.pages.back().y + h + 1 > kPageSize) {
        std::vector<uint32_t> clear(kPageSize * kPageSize, 0x00FFFFFF);
        a.pages.push_back({createTexture(kPageSize, kPageSize, clear.data())});
      }
      Atlas::Page& p = a.pages.back();
      std::vector<unsigned char> alpha(static_cast<size_t>(w) * h);
      stbtt_MakeGlyphBitmap(&f->info, alpha.data(), w, h, w, scale, scale, index);
      std::vector<uint32_t> rgba(alpha.size());
      for (size_t k = 0; k < alpha.size(); ++k) rgba[k] = (static_cast<uint32_t>(alpha[k]) << 24) | 0x00FFFFFF;
      updateTexture(p.texture, p.x, p.y, w, h, rgba.data());
      g.texture = p.texture;
      g.x0 = static_cast<float>(x0);
      g.y0 = static_cast<float>(y0);
      g.x1 = static_cast<float>(x1);
      g.y1 = static_cast<float>(y1);
      g.s0 = static_cast<float>(p.x) / kPageSize;
      g.t0 = static_cast<float>(p.y) / kPageSize;
      g.s1 = static_cast<float>(p.x + w) / kPageSize;
      g.t1 = static_cast<float>(p.y + h) / kPageSize;
      p.x += w + 1;
      p.rowHeight = std::max(p.rowHeight, h);
    }
    return &(a.glyphs[cp] = g);
  }
  a.missing.insert(cp);
  return nullptr;  // in no font: skipped (no tofu, no '?')
}

float GeometryCanvas::text(float x, float y, const std::string& s, float size, Color c) {
  Atlas& a = atlas(size);
  const int px = static_cast<int>(std::lround(size));
  float pen = x, baseline = y + a.ascent;
  // One batch per texture: the packed atlas and the pages of glyphs drawn on first use.
  std::map<Texture, std::pair<std::vector<Vertex>, std::vector<int>>> batches;
  for (size_t i = 0; i < s.size();) {
    const Glyph* g = glyph(a, px, nextCodepoint(s, i));
    if (!g) continue;
    if (g->x1 > g->x0 && g->y1 > g->y0) {
      auto& [v, idx] = batches[g->texture];
      int b = static_cast<int>(v.size());
      v.push_back({pen + g->x0, baseline + g->y0, c, g->s0, g->t0});
      v.push_back({pen + g->x1, baseline + g->y0, c, g->s1, g->t0});
      v.push_back({pen + g->x1, baseline + g->y1, c, g->s1, g->t1});
      v.push_back({pen + g->x0, baseline + g->y1, c, g->s0, g->t1});
      idx.insert(idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    pen += g->advance;
  }
  for (auto& [texture, batch] : batches) drawTriangles(texture, batch.first, batch.second);
  return pen - x;
}

float GeometryCanvas::measure(const std::string& s, float size) {
  Atlas& a = atlas(size);
  const int px = static_cast<int>(std::lround(size));
  float w = 0;
  for (size_t i = 0; i < s.size();)
    if (const Glyph* g = glyph(a, px, nextCodepoint(s, i))) w += g->advance;
  return w;
}

float GeometryCanvas::lineHeight(float size) { return atlas(size).lineHeight; }

void GeometryCanvas::outline(Rect r, const float radii[4], std::vector<Vertex>& out, Color c) {
  // Corners clockwise from top-right (radii in that order); every corner gets the same number of
  // points so outlines of different radii can be stitched into strokes.
  const int seg = 8;
  const float limit = std::min(r.w, r.h) / 2;
  out.clear();
  const float start[4] = {-90, 0, 90, 180};
  for (int corner = 0; corner < 4; ++corner) {
    float rad = std::clamp(radii[corner], 0.0f, limit);
    float cx = corner < 2 ? r.x + r.w - rad : r.x + rad;
    float cy = corner == 0 || corner == 3 ? r.y + rad : r.y + r.h - rad;
    for (int k = 0; k <= seg; ++k) {
      float a = (start[corner] + 90.0f * k / seg) * float(M_PI) / 180;
      out.push_back({cx + rad * std::cos(a), cy + rad * std::sin(a), c, 0, 0});
    }
  }
}

void GeometryCanvas::fillOutline(Rect r, const float radii[4], Color c) {
  std::vector<Vertex> pts;
  outline(r, radii, pts, c);
  std::vector<Vertex> v{{r.x + r.w / 2, r.y + r.h / 2, c, 0, 0}};
  v.insert(v.end(), pts.begin(), pts.end());
  std::vector<int> idx;
  int n = static_cast<int>(pts.size());
  for (int k = 0; k < n; ++k) idx.insert(idx.end(), {0, 1 + k, 1 + (k + 1) % n});
  drawTriangles(0, v, idx);
}

bool GeometryCanvas::image(Rect r, const std::string& source, float radius) {
  auto it = images_.find(source);
  if (it == images_.end()) {
    Texture t = 0;
    std::string path = source.rfind("file://", 0) == 0 ? source.substr(7) : source;
    if (!path.empty() && path[0] == '/' && images_.size() < 64) {
      int w = 0, h = 0, n = 0;
      if (unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4)) {
        std::vector<uint32_t> rgba(reinterpret_cast<uint32_t*>(px), reinterpret_cast<uint32_t*>(px) + size_t(w) * h);
        stbi_image_free(px);
        // Halve (2×2 averages) until small: no mipmaps, so a big picture drawn small would shimmer.
        while (w > 256 && h > 256 && w % 2 == 0 && h % 2 == 0) {
          std::vector<uint32_t> half(size_t(w / 2) * (h / 2));
          for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w / 2; ++x) {
              uint32_t out = 0;
              for (int ch = 0; ch < 32; ch += 8) {
                uint32_t sum = 0;
                for (int k = 0; k < 4; ++k) sum += (rgba[size_t(2 * y + k / 2) * w + 2 * x + k % 2] >> ch) & 0xFF;
                out |= (sum / 4) << ch;
              }
              half[size_t(y) * (w / 2) + x] = out;
            }
          rgba.swap(half);
          w /= 2, h /= 2;
        }
        t = createTexture(w, h, rgba.data());
      }
    }
    it = images_.emplace(source, t).first;
  }
  if (!it->second) return false;
  // The rounded outline as a fan, each point's texture coordinate from where it is in `r`.
  const float radii[4] = {radius, radius, radius, radius};
  std::vector<Vertex> pts;
  const Color white{255, 255, 255, 255};
  outline(r, radii, pts, white);
  std::vector<Vertex> v{{r.x + r.w / 2, r.y + r.h / 2, white, 0.5f, 0.5f}};
  for (auto& p : pts) v.push_back({p.x, p.y, white, (p.x - r.x) / r.w, (p.y - r.y) / r.h});
  std::vector<int> idx;
  const int n = static_cast<int>(pts.size());
  for (int k = 0; k < n; ++k) idx.insert(idx.end(), {0, 1 + k, 1 + (k + 1) % n});
  drawTriangles(it->second, v, idx);
  return true;
}

void GeometryCanvas::fillRect(Rect r, Color c, float radius) {
  const float radii[4] = {radius, radius, radius, radius};
  fillOutline(r, radii, c);
}

void GeometryCanvas::fillRectCorners(Rect r, Color c, float tl, float tr, float br, float bl) {
  const float radii[4] = {tr, br, bl, tl};
  fillOutline(r, radii, c);
}

void GeometryCanvas::strokeRect(Rect r, Color c, float radius, float width) {
  const float outerR[4] = {radius, radius, radius, radius};
  const float in = std::max(0.0f, radius - width);
  const float innerR[4] = {in, in, in, in};
  std::vector<Vertex> outer, inner;
  outline(r, outerR, outer, c);
  outline({r.x + width, r.y + width, r.w - 2 * width, r.h - 2 * width}, innerR, inner, c);
  std::vector<Vertex> v;
  for (size_t k = 0; k < outer.size(); ++k) {
    v.push_back(outer[k]);
    v.push_back(inner[k]);
  }
  std::vector<int> idx;
  int n = static_cast<int>(outer.size());
  for (int k = 0; k < n; ++k) {
    int a = 2 * k, b = 2 * k + 1, c2 = 2 * ((k + 1) % n), d = 2 * ((k + 1) % n) + 1;
    idx.insert(idx.end(), {a, b, c2, b, d, c2});
  }
  drawTriangles(0, v, idx);
}

void GeometryCanvas::fillCircle(float cx, float cy, float radius, Color c) { fillRect({cx - radius, cy - radius, 2 * radius, 2 * radius}, c, radius); }

void GeometryCanvas::strokeCircle(float cx, float cy, float radius, Color c, float width) {
  strokeRect({cx - radius, cy - radius, 2 * radius, 2 * radius}, c, radius, width);
}

void GeometryCanvas::fillTriangle(float x1, float y1, float x2, float y2, float x3, float y3, Color c) {
  drawTriangles(0, {{x1, y1, c, 0, 0}, {x2, y2, c, 0, 0}, {x3, y3, c, 0, 0}}, {0, 1, 2});
}

}  // namespace a2ui
