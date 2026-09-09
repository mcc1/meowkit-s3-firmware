#include "vu_face.h"
#include <math.h>
#include <stdio.h>

namespace {

/* Palette relevée sur les autres écrans du firmware. */
constexpr uint32_t COL_BG     = 0x0A0B09u;  /* fond            */
constexpr uint32_t COL_PANEL  = 0x141713u;  /* carte           */
constexpr uint32_t COL_LIME   = 0xBDE700u;  /* zone nominale   */
constexpr uint32_t COL_RED    = 0xF75129u;  /* zone de crête   */
constexpr uint32_t COL_TEXT   = 0xE9EFE2u;
constexpr uint32_t COL_MUTED  = 0x9AA394u;

/* Géométrie de l'aiguille — doit rester en accord avec vu_meter.h/.cpp. */
constexpr float PIVOT_X   = 160.0f;
constexpr float PIVOT_Y   = 239.0f;
/* Balayage symétrique : l'axe de l'arc tombe alors sur la verticale, au centre
 * de l'écran. L'app d'origine utilisait -45°/+35°, ce qui décalait le milieu de
 * l'arc de 9 px vers la gauche. Même amplitude (80°), donc même course. */
constexpr float ANGLE_MIN = -40.0f;   /* degrés, horaire depuis la verticale */
constexpr float ANGLE_MAX =  40.0f;
constexpr float R_SCALE   = 132.0f;   /* rayon de l'arc gradué  */
constexpr float R_TICK_IN = 118.0f;   /* base des graduations   */

/* Plage réellement parcourue par l'aiguille : l'app mappe linéairement
 * kDbFloor..kDbCeil sur ANGLE_MIN..ANGLE_MAX. La graduation reprend donc ces
 * valeurs plutôt que la convention VU de l'illustration d'origine, dont
 * l'échelle (30, 10, 7, 5, 3, 0, 3+) ne correspond pas au comportement réel. */
constexpr float DB_MIN = -36.0f;
constexpr float DB_MAX =  -6.0f;

/* Seuil de la zone de crête : le dernier cinquième du balayage. */
constexpr float DB_RED = -12.0f;

inline float dbToAngle(float db)
{
    const float t = (db - DB_MIN) / (DB_MAX - DB_MIN);
    return ANGLE_MIN + t * (ANGLE_MAX - ANGLE_MIN);
}

/* LovyanGFX compte les angles en degrés, 0° à l'est, sens horaire.
 * L'app les compte depuis la verticale : conversion. */
inline float toGfx(float a) { return a - 90.0f; }

inline void polar(float deg, float r, int32_t ox, int32_t oy, int32_t& x, int32_t& y)
{
    const float rad = deg * (float)M_PI / 180.0f;
    x = ox + (int32_t)lroundf(PIVOT_X + r * sinf(rad));
    y = oy + (int32_t)lroundf(PIVOT_Y - r * cosf(rad));
}

} // namespace

void vu_face_draw(LovyanGFX* gfx, int32_t ox, int32_t oy)
{
    if (!gfx) return;

    /* Fond, puis un panneau légèrement plus clair — même vocabulaire que les
     * cartes des réglages et de PC Monitor. */
    gfx->fillRect(ox, oy, 320, 240, gfx->color888(COL_BG >> 16 & 0xFF,
                                                  COL_BG >> 8  & 0xFF,
                                                  COL_BG       & 0xFF));

    /* L'arc gradué, en deux zones : nominale puis crête. */
    const int32_t cx = ox + (int32_t)PIVOT_X;
    const int32_t cy = oy + (int32_t)PIVOT_Y;
    gfx->fillArc(cx, cy, (int32_t)R_SCALE - 3, (int32_t)R_SCALE,
                 toGfx(ANGLE_MIN), toGfx(dbToAngle(DB_RED)),
                 gfx->color888(COL_LIME >> 16 & 0xFF, COL_LIME >> 8 & 0xFF, COL_LIME & 0xFF));
    gfx->fillArc(cx, cy, (int32_t)R_SCALE - 3, (int32_t)R_SCALE,
                 toGfx(dbToAngle(DB_RED)), toGfx(ANGLE_MAX),
                 gfx->color888(COL_RED >> 16 & 0xFF, COL_RED >> 8 & 0xFF, COL_RED & 0xFF));

    /* Graduations : une longue étiquetée tous les 6 dB, une courte tous les 2. */
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextFont(2);

    auto tickColor = [](float db) -> uint32_t {
        return (db >= DB_RED) ? COL_RED : COL_LIME;
    };

    for (int i = 0; i <= 15; ++i) {            /* -36 → -6 par pas de 2 dB */
        const float db    = DB_MIN + i * 2.0f;
        const bool  major = (i % 3) == 0;      /* tous les 6 dB */
        const uint32_t c  = tickColor(db);
        const uint16_t col = gfx->color888(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF);
        const float ang = dbToAngle(db);

        int32_t x0, y0, x1, y1;
        polar(ang, major ? R_TICK_IN : R_TICK_IN + 8.0f, ox, oy, x0, y0);
        polar(ang, R_SCALE - 4.0f, ox, oy, x1, y1);
        gfx->drawWideLine(x0, y0, x1, y1, major ? 2.5f : 1.2f, col);

        if (major) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", (int)-db);
            int32_t lx, ly;
            polar(ang, R_TICK_IN - 13.0f, ox, oy, lx, ly);
            /* Les chiffres restent clairs : le rouge de la zone de crête,
             * en petite police sur fond sombre, était illisible. */
            gfx->setTextColor(gfx->color888(COL_TEXT >> 16 & 0xFF,
                                            COL_TEXT >> 8  & 0xFF,
                                            COL_TEXT       & 0xFF));
            gfx->drawString(buf, lx, ly);
        }
    }

    /* Le balayage -45°..+35° est asymétrique : l'arc s'étend plus à gauche.
     * On centre donc l'unité sur son milieu réel, pas sur celui de l'écran. */
    int32_t xl, yl, xr, yr;
    polar(ANGLE_MIN, R_SCALE, ox, oy, xl, yl);
    polar(ANGLE_MAX, R_SCALE, ox, oy, xr, yr);
    const int32_t arcMidX = (xl + xr) / 2;

    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextColor(gfx->color888(COL_MUTED >> 16 & 0xFF, COL_MUTED >> 8 & 0xFF, COL_MUTED & 0xFF));
    gfx->drawString("dB", arcMidX, oy + 172);

    /* Libellés du bas, disposés symétriquement autour du milieu de l'arc et
     * non de l'écran : sans cela, l'asymétrie du balayage se voit. */
    constexpr int32_t kHalfSpan = 138;
    gfx->setTextDatum(textdatum_t::bottom_left);
    gfx->setTextColor(gfx->color888(COL_TEXT >> 16 & 0xFF, COL_TEXT >> 8 & 0xFF, COL_TEXT & 0xFF));
    gfx->drawString("VU METER", arcMidX - kHalfSpan, oy + 228);
    gfx->setTextDatum(textdatum_t::bottom_right);
    gfx->setTextColor(gfx->color888(COL_MUTED >> 16 & 0xFF, COL_MUTED >> 8 & 0xFF, COL_MUTED & 0xFF));
    gfx->drawString("PEAK", arcMidX + kHalfSpan, oy + 228);
}
