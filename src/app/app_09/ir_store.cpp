/**
 * @file ir_store.cpp
 * @brief App09 Infrared — Flipper `.ir` storage layer (see ir_store.h).
 */
#include "ir_store.h"

#include <cstdarg>
#include <cstring>
#include <cstdlib>

namespace irstore {

namespace {

/* Characters rejected by the name sanitiser (spec 2). */
static const char kBadNameChars[] = { '\\', '/', ':', '*', '?', '"', '<', '>', '|', '\0' };

/* Offset-tracking line reader. Reads the file in 512-byte chunks and reports
 * the absolute file offset of every line start, which is what the universal
 * index stores. Lines of any length are supported (raw `data:` lines are far
 * longer than one chunk). */
class LineReader {
public:
    /* maxKeep > 0 caps how many characters of a line are actually stored (the
     * rest is consumed and dropped). The index scan only needs the first few
     * characters of each line and must not grow a multi-KB String out of a raw
     * data line one reallocation at a time. */
    explicit LineReader(File& f, size_t maxKeep = 0) : _f(f), _maxKeep(maxKeep)
    {
        _reserve = (_maxKeep == 0) ? 512 : ((_maxKeep > 512) ? 512 : _maxKeep + 1);
    }

    /* false at EOF. Returned text has CR/LF stripped. Characters are staged in
     * a fixed buffer and appended in blocks with geometric growth: a 6 KB raw
     * data line costs a handful of reallocations, not one per 16 bytes. */
    bool next(String& line, uint32_t& startOff)
    {
        line = "";
        size_t cap = _reserve;
        line.reserve(cap);

        char   stage[128];
        size_t staged = 0;
        bool   any = false;
        startOff = _base + _pos;

        for (;;) {
            if (_pos >= _len) {
                if (!_fill()) {
                    _drain(line, stage, staged, cap);
                    return any;
                }
                if (!any) startOff = _base;
            }
            char c = _chunk[_pos++];
            if (c == '\n') {
                _drain(line, stage, staged, cap);
                return true;
            }
            any = true;
            if (c == '\r') continue;
            if (_maxKeep && line.length() + staged >= _maxKeep) continue;
            stage[staged++] = c;
            if (staged == sizeof(stage)) _drain(line, stage, staged, cap);
        }
    }

    uint32_t consumed() const { return _base + _pos; }

private:
    static void _drain(String& line, char* stage, size_t& staged, size_t& cap)
    {
        if (!staged) return;
        if (line.length() + staged > cap) {
            cap = (line.length() + staged) * 2;
            line.reserve(cap);
        }
        line.concat(stage, (unsigned int)staged);
        staged = 0;
    }

    bool _fill()
    {
        uint32_t at = (uint32_t)_f.position();
        int got = _f.read((uint8_t*)_chunk, sizeof(_chunk));
        if (got <= 0) { _len = 0; _pos = 0; return false; }
        _base = at;
        _len  = (size_t)got;
        _pos  = 0;
        return true;
    }

    File&    _f;
    size_t   _maxKeep = 0;
    size_t   _reserve = 96;
    char     _chunk[512];
    size_t   _len  = 0;
    size_t   _pos  = 0;
    uint32_t _base = 0;
};

/* Buffered, fully checked writer. Every byte handed to it is counted and a
 * short write latches an error the caller must inspect. */
class Writer {
public:
    explicit Writer(File& f) : _f(f) {}

    void put(const char* s)
    {
        for (const char* p = s; *p && _ok; ++p) {
            _buf[_n++] = *p;
            if (_n == sizeof(_buf)) flush();
        }
    }

    void putf(const char* fmt, ...)
    {
        char line[96];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        put(line);
    }

    bool flush()
    {
        if (!_ok) return false;
        if (_n) {
            if (_f.write((const uint8_t*)_buf, _n) != _n) { _ok = false; return false; }
            _total += _n;
            _n = 0;
        }
        return true;
    }

    bool   ok()    const { return _ok && !_f.getWriteError(); }
    size_t total() const { return _total; }

private:
    File&  _f;
    char   _buf[256];
    size_t _n     = 0;
    size_t _total = 0;
    bool   _ok    = true;
};

bool startsWith(const String& s, const char* p)
{
    size_t n = strlen(p);
    return s.length() >= n && strncmp(s.c_str(), p, n) == 0;
}

/* Apply one `key: value` line to the signal being built. */
void applyLine(Signal& s, const String& line, bool skipRawBody = false)
{
    if (startsWith(line, "type:")) {
        String t = line.substring(5); t.trim();
        s.isRaw = t.equalsIgnoreCase("raw");
    } else if (startsWith(line, "protocol:")) {
        String p = line.substring(9); p.trim();
        s.proto = irfc::protoFromName(p.c_str());
        strncpy(s.protoRaw, p.c_str(), sizeof(s.protoRaw) - 1);
        s.protoRaw[sizeof(s.protoRaw) - 1] = '\0';
    } else if (startsWith(line, "address:")) {
        s.address = irfc::parseHexBytes(line.c_str() + 8);
    } else if (startsWith(line, "command:")) {
        s.command = irfc::parseHexBytes(line.c_str() + 8);
    } else if (startsWith(line, "frequency:")) {
        long hz = line.substring(10).toInt();
        if (hz > 0) s.frequency = (uint32_t)hz;
    } else if (startsWith(line, "data:")) {
        if (skipRawBody) return;
        const char* p = line.c_str() + 5;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char* end = nullptr;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            s.raw.push_back((uint16_t)v);
            p = end;
        }
    }
}

void setName(Signal& s, const String& line)
{
    String n = line.substring(5); n.trim();
    strncpy(s.name, n.c_str(), sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = '\0';
}

/* Flipper files are LF-only; File::println() would emit CRLF. */
void writeEntry(Writer& w, const Signal& s)
{
    w.put("#\n");
    if (s.note[0]) w.putf("# %s\n", s.note);
    w.putf("name: %s\n", s.name);
    if (s.isRaw) {
        w.put("type: raw\n");
        w.putf("frequency: %lu\n", (unsigned long)s.frequency);
        w.put("duty_cycle: 0.330000\n");
        w.put("data:");
        for (size_t i = 0; i < s.raw.size(); i++) w.putf(" %u", (unsigned)s.raw[i]);
        w.put("\n");
    } else {
        char addr[16], cmd[16];
        irfc::formatHexBytes(s.address, addr, sizeof(addr));
        irfc::formatHexBytes(s.command, cmd, sizeof(cmd));
        /* An entry this firmware cannot map keeps its original protocol name. */
        const char* proto = (s.proto == irfc::Proto::Unknown && s.protoRaw[0])
                            ? s.protoRaw : irfc::protoName(s.proto);
        w.put("type: parsed\n");
        w.putf("protocol: %s\n", proto);
        w.putf("address: %s\n", addr);
        w.putf("command: %s\n", cmd);
    }
}

}  // namespace

/* ── Signal ───────────────────────────────────────────────────────────── */

void Signal::reset()
{
    /* Never memset: `raw` is a std::vector and memset orphans its heap block. */
    name[0]     = '\0';
    isRaw       = false;
    proto       = irfc::Proto::Unknown;
    protoRaw[0] = '\0';
    note[0]     = '\0';
    address   = 0;
    command   = 0;
    frequency = 38000;
    raw.clear();
}

/* ── Names ────────────────────────────────────────────────────────────── */

void sanitiseName(const char* in, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!in) { strncpy(out, "Remote", outSize - 1); out[outSize - 1] = '\0'; return; }

    const size_t kMax = 31;
    size_t limit = (outSize - 1 < kMax) ? (outSize - 1) : kMax;

    while (*in == ' ' || *in == '\t') in++;   /* trim leading */

    size_t n = 0;
    bool pendingSpace = false;
    for (const char* p = in; *p && n < limit; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') { pendingSpace = (n > 0); continue; }
        if (c < 0x20 || c == 0x7F) continue;
        if (strchr(kBadNameChars, (char)c) != nullptr) continue;
        if (pendingSpace) {
            out[n++] = '_';
            pendingSpace = false;
            if (n >= limit) break;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';

    /* Trim trailing separators, then guarantee non-empty. */
    while (n > 0 && (out[n - 1] == '_' || out[n - 1] == '-' || out[n - 1] == '.')) out[--n] = '\0';
    if (n == 0) { strncpy(out, "Remote", outSize - 1); out[outSize - 1] = '\0'; }
}

void devicePath(const char* stem, char* out, size_t outSize)
{
    snprintf(out, outSize, "%s/%s.ir", kIrDir, (stem && stem[0]) ? stem : "Remote");
}

/* ── Directory ────────────────────────────────────────────────────────── */

void listIrFiles(const char* dir, std::vector<String>& out)
{
    File root = SD_MMC.open(dir);
    if (!root) return;
    if (!root.isDirectory()) { root.close(); return; }

    std::vector<String> orphans;   /* "<name>.ir.bak" left by an interrupted write */

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String name = file.name();
            const char* slash = strrchr(name.c_str(), '/');
            String leaf = slash ? String(slash + 1) : name;
            if (leaf.endsWith(".ir") || leaf.endsWith(".IR")) out.push_back(leaf);
            else if (leaf.endsWith(".bak"))                   orphans.push_back(leaf);
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();

    /* A .bak whose real file is gone is the last complete copy: put it back. */
    for (const auto& leaf : orphans) {
        String real = leaf.substring(0, leaf.length() - 4);
        if (!real.endsWith(".ir") && !real.endsWith(".IR")) continue;
        char realPath[176], bakPath[176];
        snprintf(realPath, sizeof(realPath), "%s/%s", dir, real.c_str());
        snprintf(bakPath,  sizeof(bakPath),  "%s/%s", dir, leaf.c_str());
        if (SD_MMC.exists(realPath)) { SD_MMC.remove(bakPath); continue; }
        if (SD_MMC.rename(bakPath, realPath)) out.push_back(real);
    }
}

int countSignals(const char* path)
{
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return -1;
    LineReader lr(f);
    String line; uint32_t off;
    int n = 0;
    while (lr.next(line, off)) {
        if (startsWith(line, "name:")) n++;
    }
    f.close();
    return n;
}

/* ── Whole-file access ────────────────────────────────────────────────── */

bool loadFile(const char* path, std::vector<Signal>& out)
{
    out.clear();
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    LineReader lr(f);
    String line; uint32_t off;
    Signal sig;
    bool inSignal = false;

    while (lr.next(line, off)) {
        if (startsWith(line, "name:")) {
            if (inSignal) out.push_back(std::move(sig));
            sig.reset();
            setName(sig, line);
            inSignal = true;
        } else if (inSignal) {
            applyLine(sig, line);
        }
    }
    if (inSignal) out.push_back(std::move(sig));

    f.close();
    return true;
}

bool writeFile(const char* path, const std::vector<Signal>& sigs)
{
    char tmp[168];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (SD_MMC.exists(tmp)) SD_MMC.remove(tmp);

    File f = SD_MMC.open(tmp, FILE_WRITE);
    if (!f) return false;

    Writer w(f);
    w.put("Filetype: IR signals file\n");
    w.put("Version: 1\n");
    for (const auto& s : sigs) writeEntry(w, s);
    w.flush();

    const bool   wroteOk  = w.ok();
    const size_t expected = w.total();
    f.flush();
    f.close();

    if (!wroteOk || expected == 0) { SD_MMC.remove(tmp); return false; }

    /* Re-open and confirm every byte survived before the original is touched. */
    File check = SD_MMC.open(tmp, FILE_READ);
    if (!check) { SD_MMC.remove(tmp); return false; }
    const size_t onDisk = (size_t)check.size();
    check.close();
    if (onDisk != expected) { SD_MMC.remove(tmp); return false; }

    /* Only now is the original touched, and it is moved aside rather than
     * deleted: at every instant at least one complete copy exists on the card. */
    char bak[176];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    const bool hadOriginal = SD_MMC.exists(path);

    if (hadOriginal) {
        if (SD_MMC.exists(bak)) SD_MMC.remove(bak);
        if (!SD_MMC.rename(path, bak)) { SD_MMC.remove(tmp); return false; }
    }

    if (!SD_MMC.rename(tmp, path)) {
        if (hadOriginal) {
            if (SD_MMC.rename(bak, path)) SD_MMC.remove(tmp);  /* original restored */
            /* else: .bak is the only copy left; keep both it and the temp. */
        }
        return false;
    }

    if (hadOriginal) SD_MMC.remove(bak);
    return true;
}

int findSignal(const std::vector<Signal>& sigs, const char* name)
{
    if (!name) return -1;
    for (size_t i = 0; i < sigs.size(); i++)
        if (strcasecmp(sigs[i].name, name) == 0) return (int)i;
    return -1;
}

bool renameDevice(const char* oldPath, const char* newPath)
{
    if (SD_MMC.exists(newPath)) {
        /* FAT is case-insensitive, so TV -> tv reports the target as already
         * existing. Route a case-only change through a temporary name. */
        if (strcasecmp(oldPath, newPath) != 0) return false;
        char tmp[176];
        snprintf(tmp, sizeof(tmp), "%s.ren", oldPath);
        if (SD_MMC.exists(tmp)) SD_MMC.remove(tmp);
        if (!SD_MMC.rename(oldPath, tmp)) return false;
        if (!SD_MMC.rename(tmp, newPath)) { SD_MMC.rename(tmp, oldPath); return false; }
        return true;
    }
    return SD_MMC.rename(oldPath, newPath);
}

bool removeDevice(const char* path)
{
    return SD_MMC.remove(path);
}

/* ── Universal library ────────────────────────────────────────────────── */

void Index::clear()
{
    names.clear();
    offsets.clear();
    all.clear();
}

int Index::find(const char* name) const
{
    if (!name) return -1;
    for (size_t i = 0; i < names.size(); i++)
        if (strcasecmp(names[i].c_str(), name) == 0) return (int)i;
    return -1;
}

bool buildIndex(const char* path, Index& out, IndexTick tick, void* ctx)
{
    out.clear();
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    const uint32_t total = (uint32_t)f.size();
    LineReader lr(f, 40);          /* only the name: prefix matters here */
    String line; uint32_t off;
    uint32_t nextTick = 0;

    while (lr.next(line, off)) {
        if (tick && lr.consumed() >= nextTick) {
            nextTick = lr.consumed() + 4096;
            if (!tick(ctx, lr.consumed(), total)) {   /* aborted by the user */
                f.close();
                out.clear();
                return false;
            }
        }
        if (!startsWith(line, "name:")) continue;

        String n = line.substring(5); n.trim();
        int slot = out.find(n.c_str());
        if (slot < 0) {
            out.names.push_back(n);
            out.offsets.push_back(std::vector<uint32_t>());
            slot = (int)out.names.size() - 1;
        }
        out.offsets[slot].push_back(off);
        out.all.push_back(off);
    }

    f.close();
    if (tick) (void)tick(ctx, total, total);
    return true;
}

bool readEntryAt(const char* path, uint32_t offset, Signal& out)
{
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) { out.reset(); return false; }
    bool ok = readEntryAt(f, offset, out);
    f.close();
    return ok;
}

bool readEntryAt(File& f, uint32_t offset, Signal& out, bool skipRawBody)
{
    out.reset();
    if (!f.seek(offset)) return false;

    /* 64 characters hold any name/type/protocol/address/command line, so a
     * skipped raw body is dropped at read time instead of building a String. */
    LineReader lr(f, skipRawBody ? 64 : 0);
    String line; uint32_t off;

    if (!lr.next(line, off) || !startsWith(line, "name:")) return false;
    setName(out, line);

    while (lr.next(line, off)) {
        if (line.length() == 0) continue;
        if (line[0] == '#' || startsWith(line, "name:")) break;
        applyLine(out, line, skipRawBody);
    }
    return true;
}

}  // namespace irstore
