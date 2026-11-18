#include "dpinternal.h"

#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace dp
{

QString biomeDisplayName(const QString& id)
{
    // the path without the namespace, folders shown as "A / B"
    QString path = id.mid(id.indexOf(QLatin1Char(':')) + 1);
    QString out;
    bool upper = true;
    for (QChar c : path)
    {
        if (c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.'))
        {
            out += QLatin1Char(' ');
            upper = true;
        }
        else if (c == QLatin1Char('/'))
        {
            out += QLatin1String(" / ");
            upper = true;
        }
        else
        {
            out += upper ? c.toUpper() : c;
            upper = false;
        }
    }
    return out;
}

// hue in degrees, saturation and lightness in 0 .. 1 (no QColor: Qt Core only)
static QRgb hslToRgb(double h, double s, double l)
{
    const double c = (1.0 - std::fabs(2.0 * l - 1.0)) * s;
    const double hp = std::fmod(h, 360.0) / 60.0;
    const double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    if (hp < 1)      { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    const double m = l - c / 2.0;
    auto ch = [m](double v) { return (int) std::lround(std::min(1.0, std::max(0.0, v + m)) * 255.0); };
    return qRgb(ch(r), ch(g), ch(b));
}

static void rgbToHsl(QRgb rgb, double *h, double *s, double *l)
{
    const double r = qRed(rgb) / 255.0, g = qGreen(rgb) / 255.0, b = qBlue(rgb) / 255.0;
    const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    const double d = mx - mn;
    *l = (mx + mn) / 2.0;
    *s = d == 0 ? 0 : d / (1.0 - std::fabs(2.0 * *l - 1.0));
    if (d == 0)
        *h = 0;
    else if (mx == r)
        *h = 60.0 * std::fmod((g - b) / d + 6.0, 6.0);
    else if (mx == g)
        *h = 60.0 * ((b - r) / d + 2.0);
    else
        *h = 60.0 * ((r - g) / d + 4.0);
}

static uint32_t fnv1a(const QString& s)
{
    uint32_t h = 2166136261u;
    for (char c : s.toUtf8())
        h = (h ^ (unsigned char) c) * 16777619u;
    // final mix, so that ids that differ in one character spread out
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

// The grass or foliage colour a biome sets for itself: a number, or
// "#rrggbb" from 26.1 on (the same biome of both versions gets the same tint)
static bool biomeTint(const QJsonObject& biome, QRgb *rgb)
{
    const QJsonObject effects = biome.value(QLatin1String("effects")).toObject();
    for (const char *key : {"grass_color", "foliage_color"})
    {
        const QJsonValue v = effects.value(QLatin1String(key));
        if (v.isDouble())
        {
            *rgb = 0xff000000u | ((uint32_t) v.toInt() & 0xffffffu);
            return true;
        }
        const QString str = v.toString();
        bool ok = false;
        const uint32_t c = str.mid(1).toUInt(&ok, 16);
        if (str.size() == 7 && str.startsWith(QLatin1Char('#')) && ok)
        {
            *rgb = 0xff000000u | c;
            return true;
        }
    }
    return false;
}

QRgb fallbackBiomeColor(const QString& id, const QJsonObject& biome, int variant)
{
    // variant 0 is the colour of the id; the others hash the id with the
    // variant number, and spread wider around the biome's tint
    const uint32_t h = fnv1a(variant == 0 ? id : id + QLatin1Char('#') + QString::number(variant));
    // three independent fractions in 0 .. 1
    const double u0 = (h & 0x3ff) / 1023.0;
    const double u1 = ((h >> 10) & 0x3ff) / 1023.0;
    const double u2 = ((h >> 20) & 0x3ff) / 1023.0;

    // Without anything known about the biome: any hue, with a saturation and
    // lightness in a middle band, so the colours stay apart from each other
    // without being glaring or muddy.
    double hue = u0 * 360.0;
    double sat = 0.38 + 0.30 * u1;
    double lig = 0.42 + 0.24 * u2;

    // A biome that sets its own grass or foliage colour looks like that in
    // the game: stay near that hue. The hash still moves hue and lightness a
    // little, because many biomes of a pack share such a colour.
    QRgb tint;
    if (biomeTint(biome, &tint))
    {
        double th, ts, tl;
        rgbToHsl(tint, &th, &ts, &tl);
        if (ts > 0.08)
        {
            const int w = std::min(variant, 6);
            hue = std::fmod(th + (u0 - 0.5) * (36.0 + 20.0 * w) + 360.0, 360.0);
            if (w == 0)
            {
                sat = std::min(0.75, std::max(0.30, ts * 0.8 + (u1 - 0.5) * 0.16));
                lig = std::min(0.68, std::max(0.34, tl + (u2 - 0.5) * 0.20));
            }
            else
            {
                sat = std::min(0.80, std::max(0.25, ts * 0.8 + (u1 - 0.5) * (0.16 + 0.05 * w)));
                lig = std::min(0.74, std::max(0.28, tl + (u2 - 0.5) * (0.20 + 0.06 * w)));
            }
        }
    }
    return hslToRgb(hue, sat, lig);
}

double colorDistance(QRgb a, QRgb b)
{
    // weighted by the mean red ("redmean"), a cheap perceptual distance
    const double r = (qRed(a) + qRed(b)) / 2.0;
    const double dr = qRed(a) - qRed(b), dg = qGreen(a) - qGreen(b), db = qBlue(a) - qBlue(b);
    return std::sqrt((2.0 + r / 256.0) * dr * dr + 4.0 * dg * dg + (2.0 + (255.0 - r) / 256.0) * db * db);
}

} // namespace dp
