/**
 * @file ir_raw_tools.h
 * @brief Post-processing for raw IR captures: AGC fade detection and
 *        period-preserving mark normalisation.
 *
 * Pure C++17, no Arduino / IRremoteESP8266 headers, so it can be unit-tested
 * on the host.
 *
 * A capture is an alternating mark/space list in microseconds, exactly as
 * IRrecv reports it: index 0 is the header mark, index 1 the header space,
 * then one (mark, space) pair per bit. When the length is odd the final entry
 * is a lone trailing mark.
 *
 * Why this exists: on a long air-conditioner frame (280-424 bits) a receiver
 * whose AGC winds down reports bit marks that shrink monotonically - measured
 * 450 us at the start, 100 us at the end of a Hitachi RAS-22NK frame - while
 * mark + space per bit stays constant. Replaying those decaying marks does not
 * drive the target. Rewriting every mark to the (well-measured) leading value
 * and giving the difference back to the following space keeps the bit periods
 * and restores a sendable frame.
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace irraw {

/* -- Tunables (documented so the firmware and the host tests agree) -- */
constexpr float  kFadeRatioLimit = 0.70f;  /* tailMean/headMean below this = faded */
constexpr float  kFadeMarkFactor = 0.75f;  /* "first faded mark" threshold vs head  */
constexpr float  kHeadFraction   = 0.15f;  /* head/tail window for the fade ratio   */
constexpr float  kNormFraction   = 0.20f;  /* window used to measure a clean mark   */
constexpr float  kMaxStdevRatio  = 0.12f;  /* head marks must be this tight         */
constexpr float  kClusterSepMin  = 0.60f;  /* long space >= (1+this) * short space  */
constexpr int    kMinWindow      = 8;      /* never average fewer than this many    */
constexpr int    kMinBitMarks    = 40;     /* below this nothing is judged          */
constexpr uint16_t kMinSpaceUs   = 100;    /* never shrink a space below this       */
constexpr uint32_t kGapFactor    = 8;      /* space > this x median = frame gap     */

struct FadeInfo {
    bool     faded    = false;
    float    ratio    = 1.0f;   /* tailMean / headMean */
    uint32_t fadeUs   = 0;      /* when the first clearly-shrunk mark arrives */
    float    headMean = 0.0f;
    float    tailMean = 0.0f;
    int      bitMarks = 0;
};

/* Measure how much the bit marks shrink from the start to the end of a
 * capture. The header mark and any lone trailing mark are excluded. */
FadeInfo analyseFade(const uint16_t* data, size_t len);

/* True when the capture looks like a pulse-distance frame whose leading marks
 * are uniform, i.e. when normaliseMarks() is meaningful. `markUsOut` receives
 * the measured clean mark length. Pulse-width codings (Sony, RC5/RC6) fail the
 * bimodal-space test and are never normalised. */
bool canNormalise(const uint16_t* data, size_t len, uint16_t& markUsOut);

/* Rewrite every bit mark to `markUs` and give the difference back to the
 * following space, so each bit keeps its period. The header pair is left
 * alone; a space wider than kGapFactor x the median (an inter-frame gap) keeps
 * its length; a pair whose space would fall below kMinSpaceUs is skipped.
 * A lone trailing mark is set to `markUs`. Returns the number of pairs
 * rewritten. */
int normaliseMarks(uint16_t* data, size_t len, uint16_t markUs);

}  // namespace irraw
