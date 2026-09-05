/**
 * @file ir_store.h
 * @brief App09 Infrared — Flipper `.ir` file storage layer.
 *
 * One device = one file = N signals (see docs/app09-infrared-redesign.md §2).
 * Nothing here knows about the screen; everything here knows about SD_MMC.
 *
 * On-disk grammar (unchanged, Flipper compatible):
 *     #
 *     name: <button>
 *     type: parsed
 *     protocol: <FlipperProtoName>
 *     address: XX XX XX XX
 *     command: XX XX XX XX
 * or
 *     #
 *     name: <button>
 *     type: raw
 *     frequency: 38000
 *     duty_cycle: 0.330000
 *     data: <space separated us timings>
 *
 * `value` / `bits` are never persisted — they are derived from
 * (protocol, address, command) by ir_flipper_codec at send time.
 */
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <vector>
#include "ir_flipper_codec.h"

namespace irstore {

static constexpr const char* kIrDir   = "/infrared";
static constexpr const char* kUnivDir = "/infrared/universal";

/* One button of one device. */
struct Signal {
    char        name[32]     = {0};
    bool        isRaw        = false;
    irfc::Proto proto        = irfc::Proto::Unknown;
    /* Verbatim `protocol:` text. Kept so that rewriting a file never destroys
     * an entry whose protocol this firmware cannot map (proto == Unknown):
     * the original spelling is written back unchanged. */
    char        protoRaw[20] = {0};
    uint32_t    address      = 0;
    uint32_t    command      = 0;
    uint32_t    frequency    = 38000;       /* carrier, raw entries only */
    /* Optional provenance comment, written as "# <note>" right after the
     * entry's "#" separator. Flipper and the reader below both ignore
     * comment lines, so this never changes how the entry is parsed. */
    char        note[64]     = {0};
    std::vector<uint16_t> raw;              /* us timings, raw entries only */

    void reset();
};

/* ── Names ────────────────────────────────────────────────────────────── */

/* Trim, collapse runs of spaces to '_', drop \ / : * ? " < > | and any other
 * control character, clamp to 31 chars, never empty (falls back to "Remote"). */
void sanitiseName(const char* in, char* out, size_t outSize);

/* "<kIrDir>/<stem>.ir" */
void devicePath(const char* stem, char* out, size_t outSize);

/* ── Directory ────────────────────────────────────────────────────────── */

/* File names (with extension) of every *.ir / *.IR in `dir`; skips dirs. */
void listIrFiles(const char* dir, std::vector<String>& out);

/* Number of `name:` entries in a file (cheap chunked scan). -1 = unreadable. */
int countSignals(const char* path);

/* ── Whole-file access (device files; they hold a handful of signals) ──── */

bool loadFile(const char* path, std::vector<Signal>& out);

/* Atomic rewrite: write "<path>.tmp", verify every byte landed (write counts
 * plus a re-opened size check), only then remove the original and rename the
 * temp over it. Any failure deletes the temp and leaves the original intact. */
bool writeFile(const char* path, const std::vector<Signal>& sigs);

/* Case-insensitive lookup; -1 when absent. */
int findSignal(const std::vector<Signal>& sigs, const char* name);

/* false if the target exists. A case-only change ("tv" -> "TV") is legal even
 * though FAT reports the target as existing: it goes through a temp name. */
bool renameDevice(const char* oldPath, const char* newPath);
bool removeDevice(const char* path);

/* ── Universal library (big multi-brand files, never fully loaded) ─────── */

/* name -> every file offset holding an entry with that name, plus the
 * file-ordered list of all entries (TV-B-Gone). */
struct Index {
    std::vector<String>                names;
    std::vector<std::vector<uint32_t>> offsets;   /* parallel to names */
    std::vector<uint32_t>              all;

    void clear();
    int  find(const char* name) const;            /* -1 when absent */
};

/* Progress callback for the 512-byte chunked scan: drives the spinner and
 * keeps the UI alive. Return false to abort the scan (buildIndex then fails). */
using IndexTick = bool (*)(void* ctx, uint32_t done, uint32_t total);

bool buildIndex(const char* path, Index& out, IndexTick tick = nullptr, void* ctx = nullptr);

/* Parse exactly one entry whose `name:` line starts at `offset`. The File&
 * overload lets a caller sweep many entries of one file without reopening it;
 * memory stays bounded by a single signal either way. */
/* skipRawBody: parse the entry's metadata but drop the (multi-KB) `data:`
 * payload of a raw entry. A scan that only groups parsed entries must not
 * build a 6 KB vector per raw entry just to throw it away. */
bool readEntryAt(const char* path, uint32_t offset, Signal& out);
bool readEntryAt(File& f, uint32_t offset, Signal& out, bool skipRawBody = false);

}  // namespace irstore
