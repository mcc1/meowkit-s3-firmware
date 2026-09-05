/**
 * @file ir_raw_tools.cpp
 * @brief Implementation of the raw-capture fade / normalisation helpers.
 */
#include "ir_raw_tools.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace irraw {

namespace {

/* Indices of the bit marks: 2, 4, 6 ... The header mark (0) and a lone
 * trailing mark (len-1 when len is odd) are not bit marks. */
void collectBitMarkIndices(size_t len, std::vector<size_t>& out)
{
    out.clear();
    if (len < 4) return;
    for (size_t i = 2; i + 1 < len; i += 2) out.push_back(i);
}

int windowSize(int count, float fraction)
{
    int w = (int)(count * fraction);
    if (w < kMinWindow) w = kMinWindow;
    if (w > count)      w = count;
    return w;
}

double meanOf(const uint16_t* data, const std::vector<size_t>& idx,
              int from, int to)
{
    double sum = 0.0;
    int    n   = 0;
    for (int i = from; i < to; ++i) { sum += data[idx[(size_t)i]]; ++n; }
    return n ? sum / n : 0.0;
}

}  // namespace

FadeInfo analyseFade(const uint16_t* data, size_t len)
{
    FadeInfo info;
    if (!data || len < 4) return info;

    std::vector<size_t> idx;
    collectBitMarkIndices(len, idx);
    info.bitMarks = (int)idx.size();
    if (info.bitMarks < 2) return info;

    const int w = windowSize(info.bitMarks, kHeadFraction);
    info.headMean = (float)meanOf(data, idx, 0, w);
    info.tailMean = (float)meanOf(data, idx, info.bitMarks - w, info.bitMarks);
    if (info.headMean <= 0.0f) return info;

    info.ratio = info.tailMean / info.headMean;
    info.faded = (info.bitMarks >= kMinBitMarks) && (info.ratio < kFadeRatioLimit);

    /* Where does the decay first become visible? Sum every timing up to the
     * first bit mark that has dropped below kFadeMarkFactor of the head. */
    const double threshold = kFadeMarkFactor * info.headMean;
    uint32_t     elapsed   = 0;
    size_t       next      = 0;
    for (size_t i = 0; i < len; ++i) {
        if (next < idx.size() && idx[next] == i) {
            if ((double)data[i] < threshold) { info.fadeUs = elapsed; break; }
            ++next;
        }
        elapsed += data[i];
    }
    return info;
}

bool canNormalise(const uint16_t* data, size_t len, uint16_t& markUsOut)
{
    markUsOut = 0;
    if (!data || (int)len < kMinBitMarks) return false;

    std::vector<size_t> idx;
    collectBitMarkIndices(len, idx);
    const int count = (int)idx.size();
    if (count < kMinBitMarks) return false;

    const int w = windowSize(count, kNormFraction);

    /* (b) the leading marks must be tight around one value. */
    const double mean = meanOf(data, idx, 0, w);
    if (mean <= 0.0) return false;
    double var = 0.0;
    for (int i = 0; i < w; ++i) {
        const double d = (double)data[idx[(size_t)i]] - mean;
        var += d * d;
    }
    const double stdev = std::sqrt(var / w);
    if (stdev > kMaxStdevRatio * mean) return false;

    /* (c) the spaces of the same window must be bimodal: a pulse-distance
     * coding carries its bits in the space length. A pulse-width coding
     * (Sony, RC5/RC6) has one space cluster and is rejected here. */
    uint16_t lo = 0xFFFF, hi = 0;
    for (int i = 0; i < w; ++i) {
        const uint16_t sp = data[idx[(size_t)i] + 1];
        lo = std::min(lo, sp);
        hi = std::max(hi, sp);
    }
    if (lo == 0 || hi <= lo) return false;

    const uint32_t split = ((uint32_t)lo + hi) / 2;
    double  loSum = 0, hiSum = 0;
    int     loN = 0, hiN = 0;
    for (int i = 0; i < w; ++i) {
        const uint16_t sp = data[idx[(size_t)i] + 1];
        if (sp < split) { loSum += sp; ++loN; }
        else            { hiSum += sp; ++hiN; }
    }
    if (loN == 0 || hiN == 0) return false;

    const double loC = loSum / loN;
    const double hiC = hiSum / hiN;
    /* "centres differ by >= 60 %" measured against the short cluster, i.e. the
     * long space is at least 1.6x the short one. */
    if (hiC < (1.0 + kClusterSepMin) * loC) return false;

    markUsOut = (uint16_t)(mean + 0.5);
    return markUsOut > 0;
}

int normaliseMarks(uint16_t* data, size_t len, uint16_t markUs)
{
    if (!data || len < 4 || markUs == 0) return 0;

    std::vector<size_t> idx;
    collectBitMarkIndices(len, idx);
    if (idx.empty()) return 0;

    /* Median bit space, used to recognise inter-frame gaps. */
    std::vector<uint16_t> spaces;
    spaces.reserve(idx.size());
    for (size_t i : idx) spaces.push_back(data[i + 1]);
    std::sort(spaces.begin(), spaces.end());
    const uint32_t median = spaces[spaces.size() / 2];

    int changed = 0;
    for (size_t i : idx) {
        const uint32_t space = data[i + 1];
        if (median > 0 && space > kGapFactor * median) {
            data[i] = markUs;              /* frame gap: only the mark moves */
            continue;
        }
        const uint32_t period = (uint32_t)data[i] + space;
        if (period <= markUs || period - markUs < kMinSpaceUs) continue;
        uint32_t newSpace = period - markUs;
        if (newSpace > 65535u) newSpace = 65535u;
        data[i]     = markUs;
        data[i + 1] = (uint16_t)newSpace;
        ++changed;
    }

    /* A lone trailing mark closes the frame and fades just like the others. */
    if (len % 2 == 1) data[len - 1] = markUs;

    return changed;
}

}  // namespace irraw
