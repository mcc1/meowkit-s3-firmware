/**
 * @file ac_store.cpp
 * @brief App11 AC Remote - `/ac/<Name>.cfg` parse / serialise (host-testable).
 *
 * See ac_store.h for the grammar. Nothing here touches the filesystem: the
 * app hands the whole file body in and takes the whole file body out, which
 * is what keeps the atomic tmp -> .bak -> rename writer on the Arduino side.
 */
#include "ac_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acstore {

namespace {

/* Case-insensitive compare that does not depend on <cctype>'s locale. */
int lowerByte(char c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : (unsigned char)c;
}

bool sameToken(const char* a, const char* b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (lowerByte(*a) != lowerByte(*b)) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Copy src[0..len) into out, trimming blanks at both ends. */
void copyTrimmed(const char* src, size_t len, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!src) return;
    size_t begin = 0, end = len;
    while (begin < end && isBlank(src[begin]))     ++begin;
    while (end > begin && isBlank(src[end - 1]))   --end;
    size_t n = end - begin;
    if (n > outSize - 1) n = outSize - 1;
    memcpy(out, src + begin, n);
    out[n] = '\0';
}

/* strtol that reports whether the whole token was a number. */
bool parseLong(const char* text, long& out)
{
    if (!text || !text[0]) return false;
    char* stop = nullptr;
    long v = strtol(text, &stop, 10);
    if (!stop || stop == text) return false;
    while (*stop && isBlank(*stop)) ++stop;
    if (*stop != '\0') return false;
    out = v;
    return true;
}

/* One key -> field assignment. Unknown keys fall through silently. */
void applyPair(const char* key, const char* value, Device& d)
{
    long num = 0;
    if (sameToken(key, "protocol")) {
        size_t n = strlen(value);
        if (n > kProtocolLen - 1) n = kProtocolLen - 1;
        memcpy(d.protocol, value, n);
        d.protocol[n] = '\0';
    } else if (sameToken(key, "model")) {
        if (parseLong(value, num)) {
            if (num < -1)    num = -1;
            if (num > 32767) num = 32767;
            d.model = (int16_t)num;
        }
    } else if (sameToken(key, "power"))  { d.power  = parseBool(value, d.power);
    } else if (sameToken(key, "mode"))   { d.mode   = parseMode(value, d.mode);
    } else if (sameToken(key, "temp"))   { if (parseLong(value, num)) d.temp = clampTemp(num);
    } else if (sameToken(key, "fan"))    { d.fan    = parseFan(value, d.fan);
    } else if (sameToken(key, "swingv")) { d.swingv = parseSwing(value, d.swingv);
    } else if (sameToken(key, "swingh")) { d.swingh = parseSwing(value, d.swingh);
    } else if (sameToken(key, "quiet"))  { d.quiet  = parseBool(value, d.quiet);
    } else if (sameToken(key, "turbo"))  { d.turbo  = parseBool(value, d.turbo);
    } else if (sameToken(key, "econo"))  { d.econo  = parseBool(value, d.econo);
    } else if (sameToken(key, "light"))  { d.light  = parseBool(value, d.light);
    }
}

}  // namespace

/* -- Value tokens ------------------------------------------------------- */

const char* modeName(int8_t mode)
{
    switch (mode) {
        case kModeOff:  return "off";
        case kModeCool: return "cool";
        case kModeHeat: return "heat";
        case kModeDry:  return "dry";
        case kModeFan:  return "fan";
        default:        return "auto";
    }
}

const char* fanName(int8_t fan)
{
    switch (fan) {
        case kFanMin:    return "min";
        case kFanLow:    return "low";
        case kFanMedium: return "medium";
        case kFanHigh:   return "high";
        case kFanMax:    return "max";
        default:         return "auto";
    }
}

const char* swingName(int8_t swing)
{
    return (swing == kSwingOff) ? "off" : "auto";
}

const char* boolName(bool value) { return value ? "on" : "off"; }

const char* modeLabel(int8_t mode)
{
    switch (mode) {
        case kModeOff:  return "OFF";
        case kModeCool: return "COOL";
        case kModeHeat: return "HEAT";
        case kModeDry:  return "DRY";
        case kModeFan:  return "FAN";
        default:        return "AUTO";
    }
}

const char* fanLabel(int8_t fan)
{
    switch (fan) {
        case kFanMin:    return "MIN";
        case kFanLow:    return "LOW";
        case kFanMedium: return "MED";
        case kFanHigh:   return "HIGH";
        case kFanMax:    return "MAX";
        default:         return "AUTO";
    }
}

int8_t parseMode(const char* text, int8_t def)
{
    if (sameToken(text, "off"))  return kModeOff;
    if (sameToken(text, "auto")) return kModeAuto;
    if (sameToken(text, "cool")) return kModeCool;
    if (sameToken(text, "heat")) return kModeHeat;
    if (sameToken(text, "dry"))  return kModeDry;
    if (sameToken(text, "fan"))  return kModeFan;
    return def;
}

int8_t parseFan(const char* text, int8_t def)
{
    if (sameToken(text, "auto"))   return kFanAuto;
    if (sameToken(text, "min"))    return kFanMin;
    if (sameToken(text, "low"))    return kFanLow;
    if (sameToken(text, "medium")) return kFanMedium;
    if (sameToken(text, "high"))   return kFanHigh;
    if (sameToken(text, "max"))    return kFanMax;
    return def;
}

int8_t parseSwing(const char* text, int8_t def)
{
    if (sameToken(text, "off"))  return kSwingOff;
    if (sameToken(text, "auto")) return kSwingAuto;
    if (sameToken(text, "on"))   return kSwingAuto;
    return def;
}

bool parseBool(const char* text, bool def)
{
    if (sameToken(text, "on")  || sameToken(text, "true")  || sameToken(text, "1"))
        return true;
    if (sameToken(text, "off") || sameToken(text, "false") || sameToken(text, "0"))
        return false;
    return def;
}

int8_t clampTemp(long celsius)
{
    if (celsius < kTempMin) return kTempMin;
    if (celsius > kTempMax) return kTempMax;
    return (int8_t)celsius;
}

/* -- File body ---------------------------------------------------------- */

bool parse(const char* text, size_t len, Device& out)
{
    out = Device();                 /* every absent key keeps its default */
    if (!text) return false;

    size_t i = 0;
    while (i < len) {
        size_t lineEnd = i;
        while (lineEnd < len && text[lineEnd] != '\n') ++lineEnd;

        /* Split on the first ':'; a line without one is not a pair. */
        size_t colon = i;
        while (colon < lineEnd && text[colon] != ':') ++colon;
        if (colon < lineEnd) {
            char key[24], value[kProtocolLen];
            copyTrimmed(text + i, colon - i, key, sizeof(key));
            copyTrimmed(text + colon + 1, lineEnd - colon - 1, value, sizeof(value));
            if (key[0] && key[0] != '#') applyPair(key, value, out);
        }
        i = (lineEnd < len) ? lineEnd + 1 : len;
    }
    return true;
}

const char kFileHeader[] =
    "# MeowKit AC Remote device. Saving from the handheld rewrites this file\n"
    "# from the keys below; any other key you add here is discarded.\n";

size_t serialise(const Device& d, char* out, size_t outSize)
{
    if (!out || outSize == 0) return 0;
    out[0] = '\0';

    int n = snprintf(out, outSize,
                     "%s"
                     "protocol: %s\n"
                     "model: %d\n"
                     "power: %s\n"
                     "mode: %s\n"
                     "temp: %d\n"
                     "fan: %s\n"
                     "swingv: %s\n"
                     "swingh: %s\n"
                     "quiet: %s\n"
                     "turbo: %s\n"
                     "econo: %s\n"
                     "light: %s\n",
                     kFileHeader,
                     d.protocol[0] ? d.protocol : "UNKNOWN",
                     (int)d.model,
                     boolName(d.power),
                     modeName(d.mode),
                     (int)d.temp,
                     fanName(d.fan),
                     swingName(d.swingv),
                     swingName(d.swingh),
                     boolName(d.quiet),
                     boolName(d.turbo),
                     boolName(d.econo),
                     boolName(d.light));

    if (n <= 0 || (size_t)n >= outSize) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

}  // namespace acstore
