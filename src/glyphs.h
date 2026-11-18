#ifndef GLYPHS_H
#define GLYPHS_H

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

class QPainter;

// Monochrome glyphs for the condition types and the view and dimension
// layers that are not Minecraft structures (the structures keep their
// texture icons in rc/icons). They are drawn in code on a 20x20 unit grid with
// one stroke weight and round joins, in the manner of the tool rail icons
// (chromeIcon), and tinted from the theme when painted.
//
// Names replace the former bitmaps of the same name ("overworld", "nether",
// "the_end", "map", "tempcat", "height", "quad", "reference", "grid") and
// add a few ("terrain", "climate", "biome_center", "logic_or", "logic_not",
// "lua").

// Whether a glyph of that name exists (otherwise use the bitmap icon).
bool hasGlyph(const QString& name);

// All glyph names, in contact sheet order.
QStringList glyphNames();

// Draws a glyph onto a transparent layer scaled to 20x20 units. Some glyphs
// cut holes with QPainter::CompositionMode_Clear, so draw into a pixmap of
// its own, never directly onto a widget.
void drawGlyph(QPainter& p, const QString& name, const QColor& color);

// A square pixmap of the glyph, side in logical pixels at the given device
// pixel ratio (cached).
QPixmap glyphPixmap(const QString& name, const QColor& color, int side, qreal dpr = 1.0);

// An icon that takes its colors from the application palette whenever it is
// painted, so it follows theme changes: muted (PlaceholderText) normally,
// Text when selected, a dimmer muted when disabled. A toggle icon (layer
// buttons) also shows the checked ("On") state in the Text color.
QIcon glyphIcon(const QString& name, bool toggle = false);

// Writes a contact sheet of all glyphs (18 and 36 px, dark and light theme,
// muted and text color) to path. Returns false on failure.
bool renderGlyphSheet(const QString& path, qreal dpr);

#endif // GLYPHS_H
