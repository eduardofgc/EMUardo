#pragma once

#include <SDL2/SDL.h>

#include <string>

namespace gba::frontend {

// Tiny built-in 5x7 dot-matrix font - hand-authored rather than pulling in
// SDL_ttf + a bundled font file, since neither is otherwise a dependency
// of this project and a splash/menu screen's text needs are modest
// (uppercase letters, digits, a handful of punctuation marks that show up
// in ROM filenames and UI labels). Unsupported characters (lowercase,
// anything outside the covered set) render as a blank cell rather than
// failing - see DrawText.
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;

// Draws `text` at (x, y) in `color`, each glyph pixel scaled up to a
// `scale`x`scale` block, left-to-right with a 1-glyph-wide gap between
// characters. Lowercase letters are upper-cased first (the font only
// defines uppercase glyphs - see the class comment). Returns the total
// pixel width drawn, so callers can center text if they want to.
int DrawText(SDL_Renderer* renderer, int x, int y, const std::string& text,
             int scale, SDL_Color color);

// Just the width DrawText() would use, without actually drawing -
// handy for centering text before you know where to start.
int MeasureText(const std::string& text, int scale);

} // namespace gba::frontend
