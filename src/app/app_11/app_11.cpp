/**
 * @file app_11.cpp
 * @author Mingo
 * @brief App11 — AC Remote (Flipper-style TUI on hp_ui chrome).
 *        DeviceList → Control (remote-style grid, every press transmits).
 *        Storage: /ac/<Name>.cfg key=value (ac_store.{h,cpp}, host-tested)
 *        Transmit: one IRac instance on HAL_PIN_IR_TX for the app session.
 *        Contract: docs/app11-ac-remote.md §3 §4 §5
 * @version 1.0
 * @date 2026-09-06
 * @copyright Copyright (c) 2025
 */
#include "app_11.h"
#include "../../bsp/config.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>

/* ── Storage ──────────────────────────────────────────────────────────── */

static constexpr const char* kAcDir = "/ac";
static constexpr size_t      kPathLen = 96;

/* Characters FAT rejects in a file name (same set as ir_store). */
static constexpr const char* kBadNameChars = "\\/:*?\"<>|";

/* Trim, collapse blanks to '_', drop the FAT-illegal and control characters,
 * clamp to 31 chars, never empty. Mirrors irstore::sanitiseName so the two IR
 * apps name their files by the same rules. */
static void acSanitiseName(const char* in, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!in) { strncpy(out, "AC", outSize - 1); out[outSize - 1] = '\0'; return; }

    const size_t kMax = 31;
    size_t limit = (outSize - 1 < kMax) ? (outSize - 1) : kMax;

    while (*in == ' ' || *in == '\t') in++;

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

    while (n > 0 && (out[n - 1] == '_' || out[n - 1] == '-' || out[n - 1] == '.')) out[--n] = '\0';
    if (n == 0) { strncpy(out, "AC", outSize - 1); out[outSize - 1] = '\0'; }
}

/* Strip the extension from a file name. */
static String acStemOf(const String& filename)
{
    int dot = filename.lastIndexOf('.');
    return (dot >= 0) ? filename.substring(0, dot) : filename;
}

/* Read a whole .cfg into `buf` (always NUL-terminated). A file that does not
 * fit is refused rather than truncated: half a config parses into a plausible
 * but wrong device, and the app would then write that back over the file. */
static constexpr int kReadFailed   = -1;
static constexpr int kReadTooLarge = -2;
/* Reads are given far more room than serialise() ever needs, so a file a
 * person expanded with comments still loads; anything past this is refused. */
static constexpr size_t kReadBufSize = 1024;

static int acReadBody(const char* path, char* buf, size_t bufSize)
{
    if (!buf || bufSize == 0) return kReadFailed;
    buf[0] = '\0';
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return kReadFailed;
    const size_t sz = (size_t)f.size();
    if (sz > bufSize - 1) { f.close(); return kReadTooLarge; }
    const size_t got = f.read((uint8_t*)buf, sz);
    f.close();
    buf[got] = '\0';
    return (int)got;
}

/* ── Curated protocol table (docs/app11-ac-remote.md DECISION 4A) ─────────
 * Rows the picker offers first; anything the build excludes is filtered out
 * by IRac::isProtocolSupported() when the list is assembled. */
struct AcBrandEntry {
    const char*   label;      /* brand-friendly row title  */
    decode_type_t type;
};

static const AcBrandEntry kCurated[] = {
    { "Hitachi AC",           decode_type_t::HITACHI_AC           },
    { "Hitachi AC1",          decode_type_t::HITACHI_AC1          },
    { "Hitachi AC264",        decode_type_t::HITACHI_AC264        },
    { "Hitachi AC296",        decode_type_t::HITACHI_AC296        },
    { "Hitachi AC344",        decode_type_t::HITACHI_AC344        },
    { "Hitachi AC424",        decode_type_t::HITACHI_AC424        },
    { "Daikin",               decode_type_t::DAIKIN               },
    { "Daikin 2",             decode_type_t::DAIKIN2              },
    { "Daikin 64",            decode_type_t::DAIKIN64             },
    { "Daikin 128",           decode_type_t::DAIKIN128            },
    { "Daikin 152",           decode_type_t::DAIKIN152            },
    { "Daikin 160",           decode_type_t::DAIKIN160            },
    { "Daikin 176",           decode_type_t::DAIKIN176            },
    { "Daikin 216",           decode_type_t::DAIKIN216            },
    /* DAIKIN200 and DAIKIN312 are decode-only in this library version:
     * IRac::isProtocolSupported() rejects both, so a curated row for them
     * would never render. They are left out rather than filtered silently. */
    { "Panasonic AC",         decode_type_t::PANASONIC_AC         },
    { "Panasonic AC32",       decode_type_t::PANASONIC_AC32       },
    { "Mitsubishi AC",        decode_type_t::MITSUBISHI_AC        },
    { "Mitsubishi 112",       decode_type_t::MITSUBISHI112        },
    { "Mitsubishi 136",       decode_type_t::MITSUBISHI136        },
    { "Mitsubishi Heavy 88",  decode_type_t::MITSUBISHI_HEAVY_88  },
    { "Mitsubishi Heavy 152", decode_type_t::MITSUBISHI_HEAVY_152 },
    { "Fujitsu AC",           decode_type_t::FUJITSU_AC           },
    { "Gree",                 decode_type_t::GREE                 },
    { "Toshiba AC",           decode_type_t::TOSHIBA_AC           },
    { "Sharp AC",             decode_type_t::SHARP_AC             },
    { "LG",                   decode_type_t::LG                   },
    { "LG2",                  decode_type_t::LG2                  },
    { "Samsung AC",           decode_type_t::SAMSUNG_AC           },
    { "Midea",                decode_type_t::MIDEA                },
    { "Carrier AC64",         decode_type_t::CARRIER_AC64         },
    { "Haier AC",             decode_type_t::HAIER_AC             },
    { "Haier AC176",          decode_type_t::HAIER_AC176          },
    { "Haier AC YRW02",       decode_type_t::HAIER_AC_YRW02       },
    { "Kelvinator",           decode_type_t::KELVINATOR           },
    { "Coolix",               decode_type_t::COOLIX               },
};
static constexpr int kCuratedCount = (int)(sizeof(kCurated) / sizeof(kCurated[0]));

/* How many decode_type_t values IRac can actually drive in this build. */
static int acCountSupported()
{
    int n = 0;
    for (int t = 1; t <= kLastDecodeType; t++)
        if (IRac::isProtocolSupported((decode_type_t)t)) n++;
    return n;
}

/* ── Shared virtual-keyboard keymap (4 x 10, same as App09) ── */
static const char* kAcKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3",
    "4", "5", "6", "7", "8", "9", "_", "-", ".", "[DEL]",
    "[OK]"
};
static constexpr int kAcCols     = 10;
static constexpr int kAcKeyCount = (int)(sizeof(kAcKeys) / sizeof(kAcKeys[0]));

/* ── Fixed menus ── */
static const char* kOptionItems[] = { "Rename", "Change protocol", "Delete" };
static constexpr int kOptionCount = 3;

static const char* kAdvancedLabels[] = { "Quiet", "Turbo", "Econo", "Light" };
static constexpr int kAdvancedCount  = 4;

/* ── Control screen layout ──────────────────────────────────────────────
 *   status panel   y = 28 .. 99      (LCD imitation)
 *   pad rows 0/1   y = 104 .. 172    (3 columns: Power/Temp-/Temp+, Mode/Fan/Swing)
 *   pad row 2      y = 176 .. 208    (full width: Advanced)                      */
static constexpr int PANEL_X = 4;
static constexpr int PANEL_Y = hp::CON_Y0 + 2;      /* 28  */
static constexpr int PANEL_W = hp::W - 8;           /* 312 */
static constexpr int PANEL_H = 72;

static constexpr int PAD_X0   = 5;
static constexpr int PAD_Y0   = 104;
static constexpr int PAD_W    = 99;
static constexpr int PAD_H    = 32;
static constexpr int PAD_DX   = 105;                /* PAD_W + 6 gap */
static constexpr int PAD_DY   = 36;                 /* PAD_H + 4 gap */
static constexpr int PAD_WIDE = 309;
static constexpr int kPadCount = 7;
static constexpr int kPadWide  = 6;                 /* index of the wide key */

static const char* const kPadLabels[kPadCount] = {
    "POWER", "TEMP -", "TEMP +", "MODE", "FAN", "SWING", "ADVANCED"
};

static void acPadRect(int idx, int& x, int& y, int& w, int& h)
{
    if (idx >= kPadWide) {
        x = PAD_X0; y = PAD_Y0 + 2 * PAD_DY; w = PAD_WIDE; h = PAD_H;
        return;
    }
    x = PAD_X0 + (idx % 3) * PAD_DX;
    y = PAD_Y0 + (idx / 3) * PAD_DY;
    w = PAD_W;
    h = PAD_H;
}

namespace MOONCAKE::APPS
{
    /* ════════════════════════════════════════════════════════════
     *  Constructor / Lifecycle
     * ════════════════════════════════════════════════════════════ */

    App11::App11(DEVICES* device) : _device(device)
    {
        setAppInfo().name = "AC Remote";
    }

    void App11::onOpen()
    {
        /* One IRac for the whole app session: it carries the previous state
         * that toggle-style protocols (Hitachi's button byte, Daikin's swing)
         * need to encode the next frame. Contract §5. A previous instance can
         * only exist if onClose was skipped; deleting it keeps the pin owned
         * by exactly one object. */
        if (_ac) delete _ac;
        _ac = new IRac(HAL_PIN_IR_TX);

        _scene       = AcScene::DeviceList;
        _prevScene   = AcScene::DeviceList;
        _repaintOnly = false;

        _rows.clear();
        _subs.clear();
        _fileList.clear();
        _curated.clear();
        _allProtos.clear();

        _sel = _scrollTop = 0;
        _selDeviceList = _selPicker = _selAll = _selModel = _selAdvanced = 0;

        _dev            = acstore::Device();
        _devStem[0]     = '\0';
        _protoName[0]   = '\0';
        _selectStem[0]  = '\0';
        _devLoaded      = false;
        _protoOk        = false;
        _hitachiDirect  = false;
        _hitachi.close();
        _loadError      = nullptr;
        _models.clear();
        _builtScene     = AcScene::DeviceList;
        _lastPicker     = AcScene::ProtocolPicker;

        _pickProtocol = decode_type_t::UNKNOWN;
        _pickModel    = -1;
        _pickMode     = AcPickMode::NewDevice;
        _pickReturn   = AcScene::DeviceList;

        _nameMode   = AcNameMode::NewDevice;
        _editBuf[0] = '\0';
        _vkSel      = 0;
        _vkCol      = 0;

        _confirmDelete   = false;
        _confirmLine[0]  = '\0';

        _padSel = 0;
        _padCol = 0;

        _toastMsg[0] = '\0';
        _toastUntil  = 0;

        if (!SD_MMC.exists(kAcDir)) SD_MMC.mkdir(kAcDir);

        /* The launcher's [A] is still latched when the app opens. */
        _drainInput();

        _switchScene(AcScene::DeviceList);
    }

    void App11::onRunning()
    {
        _device->button.update();
        _device->button.tick();

        /* Long-press B = exit app, from any screen. */
        if (_device->button.B.isLongPress()) {
            close();
            return;
        }

        _serviceToast();

        if (_sceneDirty) {
            _sceneDirty = false;
            switch (_scene) {
                case AcScene::DeviceList:     _enterDeviceList();     break;
                case AcScene::DeviceOptions:  _enterDeviceOptions();  break;
                case AcScene::ProtocolPicker: _enterProtocolPicker(); break;
                case AcScene::AllProtocols:   _enterAllProtocols();   break;
                case AcScene::ModelPicker:    _enterModelPicker();    break;
                case AcScene::NameEditor:     _enterNameEditor();     break;
                case AcScene::Confirm:        _enterConfirm();        break;
                case AcScene::Control:        _enterControl();        break;
                case AcScene::Advanced:       _enterAdvanced();       break;
            }
            /* A live toast survives a scene repaint. */
            if (_toastUntil && millis() < _toastUntil) _paintToast();
            _repaintOnly = false;
        }

        switch (_scene) {
            case AcScene::DeviceList:     _runDeviceList();     break;
            case AcScene::DeviceOptions:  _runDeviceOptions();  break;
            case AcScene::ProtocolPicker: _runProtocolPicker(); break;
            case AcScene::AllProtocols:   _runAllProtocols();   break;
            case AcScene::ModelPicker:    _runModelPicker();    break;
            case AcScene::NameEditor:     _runNameEditor();     break;
            case AcScene::Confirm:        _runConfirm();        break;
            case AcScene::Control:        _runControl();        break;
            case AcScene::Advanced:       _runAdvanced();       break;
        }
    }

    void App11::onClose()
    {
        if (_ac) { delete _ac; _ac = nullptr; }
        _hitachi.close();
        _hitachiDirect = false;
        _models.clear();
        _device->led.off();
        _rows.clear();
        _subs.clear();
        _fileList.clear();
        _curated.clear();
        _allProtos.clear();
    }

    /* ════════════════════════════════════════════════════════════
     *  Scene / list plumbing (same discipline as App09)
     * ════════════════════════════════════════════════════════════ */

    void App11::_switchScene(AcScene s, bool resetSel)
    {
        _prevScene   = _scene;
        _scene       = s;
        _sceneDirty  = true;
        /* A queued scene change always owns the next paint in full. */
        _repaintOnly = false;
        if (resetSel) { _sel = 0; _scrollTop = 0; }
    }

    /* True when the scene about to be drawn must rebuild its list model.
     * A bare toast-expiry repaint of the SAME scene reuses the cached rows;
     * anything else (including a repaint that arrived on top of a pending
     * scene switch) rebuilds, so _rows can never describe another scene. */
    bool App11::_needRebuild(AcScene s)
    {
        if (_repaintOnly && _builtScene == s) return false;
        _builtScene = s;
        return true;
    }

    int App11::_visibleRows() const
    {
        return _subs.empty() ? hp::LIST_VIS : hp::LIST2_VIS;
    }

    void App11::_drawRows()
    {
        auto& Lcd  = _device->Lcd;
        const int total = (int)_rows.size();
        const int vis   = _visibleRows();
        const bool two  = !_subs.empty();

        if (_sel >= total)  _sel = (total > 0) ? total - 1 : 0;
        if (_sel < 0)       _sel = 0;
        if (_sel < _scrollTop)          _scrollTop = _sel;
        if (_sel >= _scrollTop + vis)   _scrollTop = _sel - vis + 1;
        if (_scrollTop < 0)             _scrollTop = 0;

        for (int r = 0; r < vis; r++) {
            int idx = _scrollTop + r;
            if (idx < total) {
                if (two) {
                    const char* sub = (idx < (int)_subs.size()) ? _subs[idx].c_str() : "";
                    hp::drawListItemSub(Lcd, r, _rows[idx].c_str(), sub, idx == _sel);
                } else {
                    hp::drawListItem(Lcd, r, _rows[idx].c_str(), idx == _sel);
                }
            } else {
                if (two) hp::clearListRow2(Lcd, r);
                else     hp::clearListRow(Lcd, r);
            }
        }
        if (two) hp::drawScrollbar2(Lcd, total, _scrollTop, vis);
        else     hp::drawScrollbar(Lcd, total, _scrollTop, vis);
    }

    bool App11::_navList()
    {
        const int total = (int)_rows.size();
        if (total <= 0) return false;
        const int old = _sel;
        if (_device->button.Up.pressed()   && _sel > 0)         _sel--;
        if (_device->button.Down.pressed() && _sel < total - 1) _sel++;
        return _sel != old;
    }

    void App11::_drawCentered(const char* l1, const char* l2, const char* l3,
                              const char* l4, uint16_t color)
    {
        auto& Lcd = _device->Lcd;
        hp::clearContent(Lcd);

        const char* lines[4] = { l1, l2, l3, l4 };
        int n = 0;
        for (int i = 0; i < 4; i++) if (lines[i] && lines[i][0]) n++;
        if (n == 0) return;

        const int lh = 20;
        int y = hp::CON_Y0 + (hp::CON_H - n * lh) / 2;
        Lcd.setFont(&fonts::efontCN_16);
        Lcd.setTextColor(color, hp::COL_BG);
        for (int i = 0; i < 4; i++) {
            if (!lines[i] || !lines[i][0]) continue;
            int tw = (int)strlen(lines[i]) * 8;
            Lcd.setCursor((hp::W - tw) / 2, y);
            Lcd.print(lines[i]);
            y += lh;
        }
    }

    void App11::_toast(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(_toastMsg, sizeof(_toastMsg), fmt, ap);
        va_end(ap);
        _toastUntil = millis() + 600;
        if (!_sceneDirty) _paintToast();
    }

    /* The Control pad's wide bottom key sits where hp::drawToast paints, so
     * that scene gets its banner at the top of the content area instead. */
    void App11::_paintToast()
    {
        if (_scene != AcScene::Control) {
            hp::drawToast(_device->Lcd, _toastMsg);
            return;
        }
        auto& Lcd = _device->Lcd;
        const int bh = 18;
        const int by = hp::CON_Y0 + 1;
        int bw = (int)strlen(_toastMsg) * 8 + 16;
        if (bw > hp::W - 8) bw = hp::W - 8;
        const int bx = (hp::W - bw) / 2;
        Lcd.fillRect(bx, by, bw, bh, hp::COL_FG);
        Lcd.drawRect(bx, by, bw, bh, hp::COL_ACCENT);
        Lcd.setFont(&fonts::efontCN_16);
        Lcd.setTextColor(hp::COL_ACCENT, hp::COL_FG);
        Lcd.setCursor(bx + 8, by + 1);
        Lcd.print(_toastMsg);
    }

    void App11::_serviceToast()
    {
        if (_toastUntil && millis() >= _toastUntil) {
            _toastUntil = 0;
            /* Only claim the repaint when no scene change is already queued;
             * otherwise the new scene's enter handler would run believing its
             * model was already built for it. */
            if (!_sceneDirty) _repaintOnly = true;
            _sceneDirty = true;
        }
    }

    void App11::_eatDirections(bool includeUpDown)
    {
        if (includeUpDown) {
            (void)_device->button.Up.pressed();
            (void)_device->button.Down.pressed();
        }
        (void)_device->button.Left.pressed();
        (void)_device->button.Right.pressed();
    }

    void App11::_drainInput()
    {
        _device->button.update();
        _device->button.tick();
        (void)_device->button.A.pressed();     (void)_device->button.A.released();
        (void)_device->button.B.pressed();     (void)_device->button.B.released();
        (void)_device->button.Up.pressed();    (void)_device->button.Up.released();
        (void)_device->button.Down.pressed();  (void)_device->button.Down.released();
        (void)_device->button.Left.pressed();  (void)_device->button.Left.released();
        (void)_device->button.Right.pressed(); (void)_device->button.Right.released();
    }

    /* ════════════════════════════════════════════════════════════
     *  Storage (Arduino side; ac_store.cpp stays pure C++)
     * ════════════════════════════════════════════════════════════ */

    void App11::_devicePath(const char* stem, char* out, size_t outSize) const
    {
        snprintf(out, outSize, "%s/%s.cfg", kAcDir, (stem && stem[0]) ? stem : "AC");
    }

    void App11::_listDevices()
    {
        _fileList.clear();
        File root = SD_MMC.open(kAcDir);
        if (!root) return;
        if (!root.isDirectory()) { root.close(); return; }

        File file = root.openNextFile();
        while (file) {
            if (!file.isDirectory()) {
                String name = file.name();
                const char* slash = strrchr(name.c_str(), '/');
                String leaf = slash ? String(slash + 1) : name;
                if (leaf.endsWith(".cfg") || leaf.endsWith(".CFG")) _fileList.push_back(leaf);
            }
            file.close();
            file = root.openNextFile();
        }
        root.close();
    }

    bool App11::_loadDevice(const char* stem)
    {
        char path[kPathLen];
        _devicePath(stem, path, sizeof(path));

        char body[kReadBufSize];
        const int got = acReadBody(path, body, sizeof(body));

        _dev = acstore::Device();
        if (got > 0) acstore::parse(body, (size_t)got, _dev);

        strncpy(_devStem, stem, sizeof(_devStem) - 1);
        _devStem[sizeof(_devStem) - 1] = '\0';
        _refreshProtocol();

        _devLoaded = (got > 0);
        _loadError = _devLoaded ? nullptr
                   : (got == kReadTooLarge ? "Config too large" : "Read failed");
        return _devLoaded;
    }

    bool App11::_saveDevice() const
    {
        char path[kPathLen];
        _devicePath(_devStem, path, sizeof(path));

        char body[acstore::kFileBufSize];
        const size_t n = acstore::serialise(_dev, body, sizeof(body));
        if (n == 0) return false;
        return _writeBody(path, body, n);
    }

    /* Atomic rewrite, same discipline as irstore::writeFile: the temp file is
     * written and size-verified first, the original is moved to .bak rather
     * than deleted, and only then does the temp take its place. At every
     * instant at least one complete copy exists on the card. */
    bool App11::_writeBody(const char* path, const char* body, size_t len) const
    {
        char tmp[kPathLen + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        if (SD_MMC.exists(tmp)) SD_MMC.remove(tmp);

        File f = SD_MMC.open(tmp, FILE_WRITE);
        if (!f) return false;
        const size_t wrote = f.write((const uint8_t*)body, len);
        f.flush();
        f.close();
        if (wrote != len) { SD_MMC.remove(tmp); return false; }

        File check = SD_MMC.open(tmp, FILE_READ);
        if (!check) { SD_MMC.remove(tmp); return false; }
        const size_t onDisk = (size_t)check.size();
        check.close();
        if (onDisk != len) { SD_MMC.remove(tmp); return false; }

        char bak[kPathLen + 8];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        const bool hadOriginal = SD_MMC.exists(path);

        if (hadOriginal) {
            if (SD_MMC.exists(bak)) SD_MMC.remove(bak);
            if (!SD_MMC.rename(path, bak)) { SD_MMC.remove(tmp); return false; }
        }
        if (!SD_MMC.rename(tmp, path)) {
            if (hadOriginal && SD_MMC.rename(bak, path)) SD_MMC.remove(tmp);
            return false;
        }
        if (hadOriginal) SD_MMC.remove(bak);
        return true;
    }

    bool App11::_removeDevice(const char* stem) const
    {
        char path[kPathLen];
        _devicePath(stem, path, sizeof(path));
        return SD_MMC.remove(path);
    }

    /* Recompute everything derived from _dev.protocol / _dev.model. */
    void App11::_refreshProtocol()
    {
        const decode_type_t t = strToDecodeType(_dev.protocol);
        _protoOk = (t != decode_type_t::UNKNOWN) && IRac::isProtocolSupported(t);
        _hitachiDirect = achitachi::isSupported(t);
        /* The 424 family has no Auto mode: the class encodes it as Cool, so
         * the panel is corrected here rather than promising something the
         * frame will not carry. */
        if (_hitachiDirect && _dev.mode == acstore::kModeAuto)
            _dev.mode = acstore::kModeCool;
        _protocolLabel(_protoName, sizeof(_protoName));
    }

    /* "HITACHI_AC344" (+ " - <model>" when the device pins one). */
    void App11::_protocolLabel(char* out, size_t outSize) const
    {
        const decode_type_t t = strToDecodeType(_dev.protocol);
        const String name = typeToString(t);
        if (_dev.model >= 0) {
            const String model = irutils::modelToStr(t, _dev.model);
            snprintf(out, outSize, "%s - %s", name.c_str(), model.c_str());
        } else {
            snprintf(out, outSize, "%s", name.c_str());
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Device list
     * ════════════════════════════════════════════════════════════ */

    void App11::_enterDeviceList()
    {
        auto& Lcd = _device->Lcd;

        /* A bare repaint redraws the cached rows; only a real entry rescans. */
        if (_needRebuild(AcScene::DeviceList)) {
            _listDevices();
            _rows.clear();
            _subs.clear();

            char body[kReadBufSize];
            for (const auto& f : _fileList) {
                char path[kPathLen];
                snprintf(path, sizeof(path), "%s/%s", kAcDir, f.c_str());
                const int got = acReadBody(path, body, sizeof(body));

                acstore::Device d;
                if (got > 0) acstore::parse(body, (size_t)got, d);

                String sub;
                if (got == kReadTooLarge)   { sub = "Config too large"; }
                else if (got < 0)           { sub = "Unreadable"; }
                else {
                    const decode_type_t t = strToDecodeType(d.protocol);
                    sub = typeToString(t);
                    if (d.model >= 0) { sub += " - "; sub += irutils::modelToStr(t, d.model); }
                    if (t == decode_type_t::UNKNOWN || !IRac::isProtocolSupported(t))
                        sub += " (unsupported)";
                }

                _rows.push_back(acStemOf(f));
                _subs.push_back(sub);
            }
            if (!_rows.empty()) {
                _rows.push_back("+ New device...");
                _subs.push_back("Pick a protocol, then a name");
            }
        }

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "AC Remote", nullptr);
        hp::clearContent(Lcd);

        if (_rows.empty()) {
            hp::drawFooter4(Lcd, nullptr, "[A]Add", nullptr, "[B]Exit");
            _drawCentered("No air-conditioners yet", nullptr,
                          "Add one with + New device", nullptr, hp::COL_ACCENT);
            return;
        }

        hp::drawFooter4(Lcd, "[^v]Move", "[A]Open", "[>]Opts", "[B]Exit");
        if (_selectStem[0]) {
            for (size_t i = 0; i < _rows.size(); i++)
                if (strcasecmp(_rows[i].c_str(), _selectStem) == 0) {
                    _selDeviceList = (int)i;
                    break;
                }
            _selectStem[0] = '\0';
        }
        if (_selDeviceList >= (int)_rows.size()) _selDeviceList = (int)_rows.size() - 1;
        _sel = _selDeviceList;
        _drawRows();
    }

    /* The trailing "+ New device..." row is not a device. */
    static bool acIsNewRow(const std::vector<String>& rows, int sel)
    {
        return !rows.empty() && sel == (int)rows.size() - 1;
    }

    void App11::_runDeviceList()
    {
        /* Empty state: A starts the new-device flow, B leaves the app. */
        if (_rows.empty()) {
            _eatDirections();          /* no edge survives into the next scene */
            if (_device->button.A.pressed()) {
                _pickMode   = AcPickMode::NewDevice;
                _selPicker  = 0;
                _switchScene(AcScene::ProtocolPicker);
                return;
            }
            if (_device->button.B.pressed()) close();
            return;
        }

        if (_navList()) { _selDeviceList = _sel; _drawRows(); }

        if (_device->button.A.pressed()) {
            _selDeviceList = _sel;
            if (acIsNewRow(_rows, _sel)) {
                _pickMode  = AcPickMode::NewDevice;
                _selPicker = 0;
                _switchScene(AcScene::ProtocolPicker);
                return;
            }
            /* A file the app could not read in full must never reach the
             * Control screen: the defaults it would show are not the machine's
             * state, and the first successful press would persist them. */
            if (!_loadDevice(_rows[_sel].c_str())) {
                _toast("%s", _loadError ? _loadError : "Read failed");
                return;
            }
            _padSel = 0;
            _padCol = 0;
            /* Re-seed the library's "previous state" from the file so the
             * first frame of the session is a delta from what the machine
             * was last told, not from the library's defaults. */
            _openTransmitter();
            _switchScene(AcScene::Control);
            return;
        }

        if (_device->button.Right.pressed() && !acIsNewRow(_rows, _sel)) {
            _selDeviceList = _sel;
            _loadDevice(_rows[_sel].c_str());
            _switchScene(AcScene::DeviceOptions);
            return;
        }

        if (_device->button.B.pressed()) close();
    }

    /* ════════════════════════════════════════════════════════════
     *  Device options
     * ════════════════════════════════════════════════════════════ */

    void App11::_enterDeviceOptions()
    {
        auto& Lcd = _device->Lcd;
        _rows.clear();
        _subs.clear();
        for (int i = 0; i < kOptionCount; i++) _rows.push_back(kOptionItems[i]);

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, _devStem, "OPTS", hp::COL_FG);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Back");
        _drawRows();
    }

    void App11::_runDeviceOptions()
    {
        if (_navList()) _drawRows();
        _eatDirections(false);     /* Left/Right mean nothing here; drop them */

        if (_device->button.A.pressed()) {
            switch (_sel) {
            case 0:
                strncpy(_editBuf, _devStem, sizeof(_editBuf) - 1);
                _editBuf[sizeof(_editBuf) - 1] = '\0';
                _vkSel    = 0;
                _vkCol    = 0;
                _nameMode = AcNameMode::RenameDevice;
                _switchScene(AcScene::NameEditor);
                break;
            case 1:
                _pickMode   = AcPickMode::ChangeProtocol;
                _pickReturn = AcScene::DeviceList;
                _selPicker  = 0;
                _switchScene(AcScene::ProtocolPicker);
                break;
            default:
                _confirmDelete = true;
                snprintf(_confirmLine, sizeof(_confirmLine), "Delete %s?", _devStem);
                _switchScene(AcScene::Confirm);
                break;
            }
            return;
        }
        if (_device->button.B.pressed()) _switchScene(AcScene::DeviceList, false);
    }

    /* ════════════════════════════════════════════════════════════
     *  Protocol picker — curated first, everything behind the last row
     * ════════════════════════════════════════════════════════════ */

    void App11::_buildCurated()
    {
        _curated.clear();
        _rows.clear();
        _subs.clear();
        for (int i = 0; i < kCuratedCount; i++) {
            if (!IRac::isProtocolSupported(kCurated[i].type)) continue;
            _curated.push_back(kCurated[i].type);
            _rows.push_back(kCurated[i].label);
            _subs.push_back(typeToString(kCurated[i].type));
        }
        char all[40];
        snprintf(all, sizeof(all), "All supported protocols (%d)", acCountSupported());
        _rows.push_back(all);
        _subs.push_back("Every protocol IRac can drive");
    }

    void App11::_enterProtocolPicker()
    {
        auto& Lcd = _device->Lcd;
        if (_needRebuild(AcScene::ProtocolPicker)) _buildCurated();

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Protocol", nullptr);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Back");
        if (_selPicker >= (int)_rows.size()) _selPicker = 0;
        _sel = _selPicker;
        _drawRows();
    }

    void App11::_runProtocolPicker()
    {
        if (_navList()) { _selPicker = _sel; _drawRows(); }

        if (_device->button.A.pressed()) {
            _selPicker = _sel;
            if (_sel >= (int)_curated.size()) {           /* "All supported..." */
                _selAll = 0;
                _switchScene(AcScene::AllProtocols);
                return;
            }
            _pickProtocol = _curated[_sel];
            _pickModel    = -1;
            _collectModels(_pickProtocol);
            if (!_models.empty()) {
                _selModel = 0;
                _switchScene(AcScene::ModelPicker);
            } else {
                _applyPickedProtocol();
            }
            return;
        }
        if (_device->button.B.pressed()) {
            _switchScene(_pickMode == AcPickMode::ChangeProtocol ? AcScene::DeviceOptions
                                                                 : AcScene::DeviceList, false);
        }
    }

    /* ── Every supported protocol, alphabetical, Left/Right = letter jump ── */

    void App11::_buildAllProtocols()
    {
        struct Row { String name; decode_type_t type; };
        std::vector<Row> all;
        for (int t = 1; t <= kLastDecodeType; t++) {
            const decode_type_t type = (decode_type_t)t;
            if (!IRac::isProtocolSupported(type)) continue;
            all.push_back({ typeToString(type), type });
        }
        std::sort(all.begin(), all.end(), [](const Row& a, const Row& b) {
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });

        _allProtos.clear();
        _rows.clear();
        _subs.clear();
        for (const auto& r : all) {
            _allProtos.push_back(r.type);
            _rows.push_back(r.name);
        }
    }

    /* Uppercased first character of row `i`, 0 when out of range. */
    static char acRowLetter(const std::vector<String>& rows, int i)
    {
        if (i < 0 || i >= (int)rows.size() || rows[i].length() == 0) return 0;
        return (char)toupper((unsigned char)rows[i].charAt(0));
    }

    void App11::_enterAllProtocols()
    {
        auto& Lcd = _device->Lcd;
        if (_needRebuild(AcScene::AllProtocols)) _buildAllProtocols();

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "All protocols", nullptr);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", "[<>]A-Z", "[B]Back");
        if (_selAll >= (int)_rows.size()) _selAll = 0;
        _sel = _selAll;
        _drawRows();
    }

    void App11::_runAllProtocols()
    {
        bool moved = _navList();
        const int total = (int)_rows.size();

        /* Right jumps to the first row of the next initial letter. */
        if (_device->button.Right.pressed() && total > 0) {
            const char c = acRowLetter(_rows, _sel);
            int i = _sel;
            while (i < total - 1 && acRowLetter(_rows, i + 1) == c) i++;
            if (i < total - 1) { _sel = i + 1; moved = true; }
        }
        /* Left jumps to the first row of the previous initial letter. */
        if (_device->button.Left.pressed() && total > 0) {
            const char c = acRowLetter(_rows, _sel);
            int i = _sel;
            while (i > 0 && acRowLetter(_rows, i - 1) == c) i--;   /* block start */
            if (i > 0) {
                i--;
                const char p = acRowLetter(_rows, i);
                while (i > 0 && acRowLetter(_rows, i - 1) == p) i--;
            }
            if (i != _sel) { _sel = i; moved = true; }
        }

        if (moved) { _selAll = _sel; _drawRows(); }

        if (_device->button.A.pressed() && _sel < (int)_allProtos.size()) {
            _selAll       = _sel;
            _pickProtocol = _allProtos[_sel];
            _pickModel    = -1;
            _collectModels(_pickProtocol);
            if (!_models.empty()) {
                _selModel = 0;
                _switchScene(AcScene::ModelPicker);
            } else {
                _applyPickedProtocol();
            }
            return;
        }
        if (_device->button.B.pressed()) _switchScene(AcScene::ProtocolPicker, false);
    }

    /* ════════════════════════════════════════════════════════════
     *  Model picker
     * ════════════════════════════════════════════════════════════ */

    /* Model numbering is not uniform across the library: most enums start at
     * 1, toshiba_ac_remote_model_t starts at 0, and panasonic/voltas reserve 0
     * for "unknown". So the ids are not counted, they are collected: a value
     * belongs to the protocol when its modelToStr() differs from the text the
     * same protocol returns for "no model" (-1). Comparing against that string
     * instead of the literal "UNKNOWN" keeps this working whatever locale
     * IRremoteESP8266 is built with. */
    void App11::_collectModels(decode_type_t protocol)
    {
        _models.clear();
        const String none = irutils::modelToStr(protocol, (int16_t)-1);
        for (int16_t m = 0; m <= 15; m++) {
            if (irutils::modelToStr(protocol, m) == none) continue;
            _models.push_back(m);
        }
    }

    void App11::_enterModelPicker()
    {
        auto& Lcd = _device->Lcd;
        if (_needRebuild(AcScene::ModelPicker)) {
            _rows.clear();
            _subs.clear();
            _rows.push_back("Library default");
            for (int16_t m : _models)
                _rows.push_back(irutils::modelToStr(_pickProtocol, m));
        }

        char title[36];
        snprintf(title, sizeof(title), "%s model", typeToString(_pickProtocol).c_str());

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, title, nullptr);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Back");
        if (_selModel >= (int)_rows.size()) _selModel = 0;
        _sel = _selModel;
        _drawRows();
    }

    void App11::_runModelPicker()
    {
        if (_navList()) { _selModel = _sel; _drawRows(); }

        if (_device->button.A.pressed()) {
            _selModel  = _sel;
            _pickModel = (_sel >= 1 && _sel <= (int)_models.size())
                             ? _models[_sel - 1] : (int16_t)-1;
            _applyPickedProtocol();
            return;
        }
        if (_device->button.B.pressed()) _switchScene(_prevScene, false);
    }

    /* The picked protocol either names a brand-new device or replaces the
     * protocol of the one already open. */
    void App11::_applyPickedProtocol()
    {
        /* Remember which picker was on screen so [B] in the name editor walks
         * back one step rather than jumping over the model picker. */
        _lastPicker = _scene;

        if (_pickMode == AcPickMode::NewDevice) {
            _nameMode = AcNameMode::NewDevice;
            snprintf(_editBuf, sizeof(_editBuf), "%s", typeToString(_pickProtocol).c_str());
            _vkSel = 0;
            _vkCol = 0;
            _switchScene(AcScene::NameEditor);
            return;
        }

        /* Changing the protocol rewrites the whole file from _dev. If the file
         * was never read in full, _dev holds defaults, and saving would throw
         * away the state the machine is actually in. */
        if (!_devLoaded) {
            _switchScene(AcScene::DeviceList);
            _toast("%s - not saved", _loadError ? _loadError : "Read failed");
            return;
        }

        snprintf(_dev.protocol, sizeof(_dev.protocol), "%s",
                 typeToString(_pickProtocol).c_str());
        _dev.model = _pickModel;
        _refreshProtocol();

        const bool saved = _saveDevice();
        strncpy(_selectStem, _devStem, sizeof(_selectStem) - 1);
        _selectStem[sizeof(_selectStem) - 1] = '\0';
        /* The protocol changed under IRac, so its "previous state" describes a
         * different machine; re-seed it before the next frame is encoded. */
        _openTransmitter();
        _switchScene(_pickReturn);
        _toast(saved ? "Protocol changed" : "Write failed");
    }

    /* ════════════════════════════════════════════════════════════
     *  Name editor (shared virtual keyboard, same keymap as App09)
     * ════════════════════════════════════════════════════════════ */

    void App11::_enterNameEditor()
    {
        auto& Lcd = _device->Lcd;
        const char* title = (_nameMode == AcNameMode::RenameDevice) ? "Rename device"
                                                                    : "New device";
        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, title, nullptr);
        hp::drawFooter4(Lcd, "[^v<>]Move", "[A]Key", nullptr, "[B]Cancel");
        hp::drawVirtualKeyboard(Lcd, "Name:", _editBuf, (int)sizeof(_editBuf) - 1,
                                kAcKeys, kAcKeyCount, kAcCols, _vkSel);
    }

    void App11::_runNameEditor()
    {
        bool redraw = false;

        /* The last row is partial (only [OK]); it is reachable from every key
         * of the row above, and Up returns to the column the user came from. */
        const int lastRowStart = ((kAcKeyCount - 1) / kAcCols) * kAcCols;
        const bool onLastRow   = (_vkSel >= lastRowStart);

        if (_device->button.Up.pressed() && _vkSel >= kAcCols) {
            _vkSel = onLastRow ? (lastRowStart - kAcCols + _vkCol) : (_vkSel - kAcCols);
            redraw = true;
        }
        if (_device->button.Down.pressed() && !onLastRow) {
            _vkCol = _vkSel % kAcCols;
            int down = _vkSel + kAcCols;
            _vkSel  = (down < kAcKeyCount) ? down : lastRowStart;
            redraw  = true;
        }
        if (_device->button.Left.pressed()) {
            if (onLastRow)               { _vkSel = kAcKeyCount - 2; redraw = true; }
            else if (_vkSel % kAcCols)   { _vkSel--;                 redraw = true; }
        }
        if (_device->button.Right.pressed()) {
            if (onLastRow)                                        { _vkSel = 0; redraw = true; }
            else if ((_vkSel % kAcCols) < kAcCols - 1
                     && _vkSel + 1 < kAcKeyCount)                 { _vkSel++;   redraw = true; }
        }

        if (_device->button.A.pressed()) {
            const char* key = kAcKeys[_vkSel];
            int len = (int)strlen(_editBuf);
            if (strcmp(key, "[OK]") == 0) {
                char clean[40];
                acSanitiseName(_editBuf, clean, sizeof(clean));

                if (_nameMode == AcNameMode::NewDevice) {
                    char path[kPathLen];
                    _devicePath(clean, path, sizeof(path));
                    if (SD_MMC.exists(path)) { _toast("Name exists"); return; }

                    _dev = acstore::Device();
                    snprintf(_dev.protocol, sizeof(_dev.protocol), "%s",
                             typeToString(_pickProtocol).c_str());
                    _dev.model = _pickModel;
                    strncpy(_devStem, clean, sizeof(_devStem) - 1);
                    _devStem[sizeof(_devStem) - 1] = '\0';
                    _refreshProtocol();

                    if (!_saveDevice()) {
                        _toast("Save failed - check SD");
                        return;
                    }
                    /* The file on the card is exactly _dev from here on. */
                    _devLoaded = true;
                    _loadError = nullptr;
                    strncpy(_selectStem, _devStem, sizeof(_selectStem) - 1);
                    _selectStem[sizeof(_selectStem) - 1] = '\0';
                    _padSel = 0;
                    _padCol = 0;
                    _openTransmitter();
                    _switchScene(AcScene::Control);
                    return;
                }

                /* Rename: move the file, keep the state. */
                if (strcmp(clean, _devStem) == 0) {
                    _switchScene(AcScene::DeviceOptions, false);
                    return;
                }
                char oldPath[kPathLen], newPath[kPathLen];
                _devicePath(_devStem, oldPath, sizeof(oldPath));
                _devicePath(clean,    newPath, sizeof(newPath));
                if (SD_MMC.exists(newPath) || !SD_MMC.rename(oldPath, newPath)) {
                    _toast("Name exists");
                    return;
                }
                strncpy(_devStem, clean, sizeof(_devStem) - 1);
                _devStem[sizeof(_devStem) - 1] = '\0';
                strncpy(_selectStem, _devStem, sizeof(_selectStem) - 1);
                _selectStem[sizeof(_selectStem) - 1] = '\0';
                _switchScene(AcScene::DeviceList);
                _toast("Renamed");
                return;
            }
            if (strcmp(key, "[DEL]") == 0) {
                if (len > 0) { _editBuf[len - 1] = '\0'; redraw = true; }
            } else if (len < (int)sizeof(_editBuf) - 1) {
                _editBuf[len]     = key[0];
                _editBuf[len + 1] = '\0';
                redraw = true;
            }
        }

        if (redraw)
            hp::drawVirtualKeyboard(_device->Lcd, "Name:", _editBuf,
                                    (int)sizeof(_editBuf) - 1, kAcKeys,
                                    kAcKeyCount, kAcCols, _vkSel);

        if (_device->button.B.pressed()) {
            _switchScene(_nameMode == AcNameMode::RenameDevice ? AcScene::DeviceOptions
                                                               : _lastPicker, false);
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Confirm (delete)
     * ════════════════════════════════════════════════════════════ */

    void App11::_enterConfirm()
    {
        auto& Lcd = _device->Lcd;
        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Confirm", "!", hp::COL_WARN);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[A]Yes", nullptr, nullptr, "[B]No");
        hp::drawDialog(Lcd, _confirmLine, nullptr, hp::COL_WARN);
    }

    void App11::_runConfirm()
    {
        _eatDirections();          /* a dialog has no navigation of its own */

        if (_device->button.A.pressed()) {
            const bool gone = _confirmDelete ? _removeDevice(_devStem) : false;
            _confirmDelete  = false;
            _selDeviceList  = 0;
            _switchScene(AcScene::DeviceList);
            _toast(gone ? "Deleted" : "Delete failed");
            return;
        }
        if (_device->button.B.pressed()) {
            _confirmDelete = false;
            _switchScene(AcScene::DeviceOptions, false);
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Control — remote-style grid, every press transmits (§5 option A)
     * ════════════════════════════════════════════════════════════ */

    stdAc::state_t App11::_toState() const
    {
        stdAc::state_t s;
        IRac::initState(&s);
        s.protocol = strToDecodeType(_dev.protocol);
        s.model    = _dev.model;
        s.power    = _dev.power;
        s.mode     = (stdAc::opmode_t)_dev.mode;
        s.degrees  = (float)_dev.temp;
        s.celsius  = true;
        s.fanspeed = (stdAc::fanspeed_t)_dev.fan;
        s.swingv   = (stdAc::swingv_t)_dev.swingv;
        s.swingh   = (stdAc::swingh_t)_dev.swingh;
        s.quiet    = _dev.quiet;
        s.turbo    = _dev.turbo;
        s.econo    = _dev.econo;
        s.light    = _dev.light;
        return s;
    }

    /* Called whenever the device behind the Control screen changes. */
    void App11::_openTransmitter()
    {
        const stdAc::state_t seed = _toState();
        /* IRac keeps carrying the toggle history for every other protocol. */
        if (_ac) { _ac->next = seed; _ac->markAsSent(); }

        if (_hitachiDirect && _protoOk)
            _hitachiDirect = _hitachi.open(seed.protocol, HAL_PIN_IR_TX, seed);
        else
            _hitachi.close();
    }

    bool App11::_sendState(achitachi::Key key)
    {
        if (!_protoOk) return false;

        _device->led.setColor(WS2812B_Class::RED);
        bool ok = false;
        if (_hitachi.isOpen()) {
            /* Fields first, key code last, then the frame - the whole reason
             * this protocol family bypasses IRac. */
            ok = _hitachi.send(_toState(), key);
        } else if (_ac) {
            _ac->next = _toState();
            /* sendAc() calls markAsSent() itself when the frame went out,
             * which keeps _prev right for the toggle-style protocols. */
            ok = _ac->sendAc();
        }
        _device->led.off();
        /* The frame takes 100-500 ms; swallow the edges latched meanwhile. */
        _drainInput();
        return ok;
    }

    bool App11::_sendAndReport(const char* label, achitachi::Key key)
    {
        if (!_sendState(key)) {
            /* An unsupported protocol never reached the emitter; anything else
             * means IRac refused the frame it was handed. */
            _toast(_protoOk ? "Send failed" : "Not sent - protocol unsupported");
            return false;
        }
        /* Persist only what the machine was actually told. */
        if (!_saveDevice()) { _toast("Sent - save failed"); return true; }
        _toast("%s", label);
        return true;
    }

    /* Shown instead of the LCD imitation when the stored protocol name does
     * not resolve to something IRac can drive (a hand-edited .cfg, or a
     * protocol this build excludes). Sends stay disabled while it is up. */
    void App11::_drawUnsupportedPanel()
    {
        auto& Lcd = _device->Lcd;
        Lcd.fillRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, hp::COL_BG);
        Lcd.drawRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, hp::COL_ERR);
        Lcd.setFont(&fonts::efontCN_16);

        char line[40];
        snprintf(line, sizeof(line), "Protocol '%s'", _dev.protocol);
        Lcd.setTextColor(hp::COL_ACCENT, hp::COL_BG);
        Lcd.setCursor(PANEL_X + 8, PANEL_Y + 8);
        Lcd.print(line);
        Lcd.setCursor(PANEL_X + 8, PANEL_Y + 26);
        Lcd.print("is not supported - cannot send");
        Lcd.setTextColor(hp::COL_FG, hp::COL_BG);
        Lcd.setCursor(PANEL_X + 8, PANEL_Y + 48);
        Lcd.print("[B] Back, [>] Change protocol");
    }

    void App11::_drawStatusPanel()
    {
        if (!_protoOk) { _drawUnsupportedPanel(); return; }

        auto& Lcd = _device->Lcd;
        const uint16_t live = _dev.power ? hp::COL_ACCENT : hp::COL_DIM;

        Lcd.fillRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, hp::COL_BG);
        Lcd.drawRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, hp::COL_FG);
        Lcd.setFont(&fonts::efontCN_16);

        /* Big temperature (3x = 24x48 px per glyph). */
        char temp[6];
        snprintf(temp, sizeof(temp), "%d", (int)_dev.temp);
        Lcd.setTextSize(3);
        Lcd.setTextColor(live, hp::COL_BG);
        Lcd.setCursor(PANEL_X + 10, PANEL_Y + 6);
        Lcd.print(temp);
        Lcd.setTextSize(1);
        Lcd.setTextColor(hp::COL_FG, hp::COL_BG);
        Lcd.setCursor(PANEL_X + 12 + (int)strlen(temp) * 24, PANEL_Y + 38);
        Lcd.print("C");

        /* Mode word (2x). */
        const int mx = PANEL_X + 92;
        Lcd.setTextSize(2);
        Lcd.setTextColor(live, hp::COL_BG);
        Lcd.setCursor(mx, PANEL_Y + 6);
        Lcd.print(acstore::modeLabel(_dev.mode));
        Lcd.setTextSize(1);

        /* Fan word + five level bars. */
        char fanLine[16];
        snprintf(fanLine, sizeof(fanLine), "FAN %s", acstore::fanLabel(_dev.fan));
        Lcd.setTextColor(hp::COL_FG, hp::COL_BG);
        Lcd.setCursor(mx, PANEL_Y + 44);
        Lcd.print(fanLine);
        for (int i = 0; i < 5; i++) {
            const int bh = 4 + i * 2;
            const bool on = (_dev.fan == acstore::kFanAuto) || (i < _dev.fan);
            Lcd.fillRect(mx + 72 + i * 6, PANEL_Y + 44 + (12 - bh), 4, bh,
                         on ? hp::COL_FG : hp::COL_DIM);
        }

        /* Power state, right-aligned; the label is padded so both fit. */
        hp::drawBadge(Lcd, PANEL_X + PANEL_W - 38, PANEL_Y + 8,
                      _dev.power ? "ON " : "OFF",
                      _dev.power ? hp::COL_FG : hp::COL_DIM);

        /* Swing markers: '-' off, 'V'/'H' when that axis is set to auto. */
        char swing[10];
        snprintf(swing, sizeof(swing), "SWG %c%c",
                 (_dev.swingv == acstore::kSwingOff) ? '-' : 'V',
                 (_dev.swingh == acstore::kSwingOff) ? '-' : 'H');
        Lcd.setTextColor(hp::COL_FG, hp::COL_BG);
        Lcd.setCursor(PANEL_X + PANEL_W - 56, PANEL_Y + 44);
        Lcd.print(swing);
    }

    void App11::_drawPad(int only)
    {
        auto& Lcd = _device->Lcd;
        for (int i = 0; i < kPadCount; i++) {
            if (only >= 0 && i != only) continue;
            int x, y, w, h;
            acPadRect(i, x, y, w, h);
            hp::drawVirtualButton(Lcd, x, y, w, h, kPadLabels[i], i == _padSel);
        }
    }

    void App11::_enterControl()
    {
        auto& Lcd = _device->Lcd;
        hp::drawChrome(Lcd);
        /* The protocol name lives in the header badge: the status panel keeps
         * its room for the values that change on every press. */
        hp::drawHeader(Lcd, _devStem, _protoName, hp::COL_FG);
        hp::clearContent(Lcd);
        if (_protoOk) hp::drawFooter4(Lcd, "[^v<>]Move", "[A]Send", nullptr, "[B]Back");
        else          hp::drawFooter4(Lcd, nullptr, "[A/>]Protocol", nullptr, "[B]Back");
        _drawStatusPanel();
        _drawPad();
    }

    void App11::_cycleMode()
    {
        /* Auto -> Cool -> Heat -> Dry -> Fan -> Auto. kOff is never selected:
         * the Power key owns the on/off state. The Hitachi 424 family has no
         * Auto at all (the class folds it into Cool), so Auto is left out of
         * the cycle there and the panel always shows what is really sent. */
        switch (_dev.mode) {
            case acstore::kModeAuto: _dev.mode = acstore::kModeCool; break;
            case acstore::kModeCool: _dev.mode = acstore::kModeHeat; break;
            case acstore::kModeHeat: _dev.mode = acstore::kModeDry;  break;
            case acstore::kModeDry:  _dev.mode = acstore::kModeFan;  break;
            default:
                _dev.mode = _hitachiDirect ? acstore::kModeCool : acstore::kModeAuto;
                break;
        }
    }

    void App11::_cycleFan()
    {
        _dev.fan = (int8_t)((_dev.fan < acstore::kFanAuto ||
                             _dev.fan >= acstore::kFanMax)
                            ? acstore::kFanAuto : (_dev.fan + 1));
    }

    void App11::_cycleSwing()
    {
        /* Off/Off -> Auto/Off -> Auto/Auto -> Off/Off. Protocols without a
         * horizontal axis simply ignore swingh (IRac drops it). */
        if (_dev.swingv == acstore::kSwingOff) {
            _dev.swingv = acstore::kSwingAuto;
            _dev.swingh = acstore::kSwingOff;
        } else if (_dev.swingh == acstore::kSwingOff) {
            _dev.swingh = acstore::kSwingAuto;
        } else {
            _dev.swingv = acstore::kSwingOff;
            _dev.swingh = acstore::kSwingOff;
        }
    }

    void App11::_stepTemp(int delta)
    {
        _dev.temp = acstore::clampTemp((long)_dev.temp + delta);
    }

    void App11::_pressPad(int index)
    {
        if (index == kPadWide) {
            _selAdvanced = 0;
            _switchScene(AcScene::Advanced);
            return;
        }
        if (!_protoOk) {
            _toast("Not sent - protocol unsupported");
            return;
        }

        /* Every field edit below is provisional: the panel must never show a
         * value the air-conditioner was not actually told about, so a failed
         * send puts the whole state back. */
        const acstore::Device before = _dev;
        char label[24];
        /* The pad that was pressed travels with the frame on the Hitachi 424
         * family; every other protocol ignores it. */
        achitachi::Key key = achitachi::Key::PowerMode;
        switch (index) {
            case 0:
                _dev.power = !_dev.power;
                snprintf(label, sizeof(label), "Power %s", _dev.power ? "on" : "off");
                break;
            case 1:
            case 2:
                _stepTemp(index == 1 ? -1 : 1);
                key = (index == 1) ? achitachi::Key::TempDown : achitachi::Key::TempUp;
                snprintf(label, sizeof(label), "%d C", (int)_dev.temp);
                break;
            case 3:
                _cycleMode();
                snprintf(label, sizeof(label), "Mode %s", acstore::modeLabel(_dev.mode));
                break;
            case 4:
                _cycleFan();
                key = achitachi::Key::Fan;
                snprintf(label, sizeof(label), "Fan %s", acstore::fanLabel(_dev.fan));
                break;
            default:
                _cycleSwing();
                key = achitachi::Key::SwingV;
                snprintf(label, sizeof(label), "Swing %s%s",
                         (_dev.swingv == acstore::kSwingOff) ? "off" : "V",
                         (_dev.swingh == acstore::kSwingOff) ? ""    : "+H");
                break;
        }

        /* Queue the repaint before sending so the panel, the pad and the
         * banner are painted once, in that order, by the scene handler. */
        _redraw();
        if (!_sendAndReport(label, key)) _dev = before;
    }

    void App11::_runControl()
    {
        /* With a protocol IRac cannot drive there is nothing to send and
         * nothing to navigate, so the pad cursor stays put and [A] / [>] both
         * open the change-protocol flow the panel advertises. */
        if (!_protoOk) {
            const bool openPicker = _device->button.Right.pressed() ||
                                    _device->button.A.pressed();
            _eatDirections();
            if (openPicker) {
                _pickMode   = AcPickMode::ChangeProtocol;
                _pickReturn = AcScene::Control;
                _selPicker  = 0;
                _switchScene(AcScene::ProtocolPicker);
                return;
            }
            if (_device->button.B.pressed()) {
                strncpy(_selectStem, _devStem, sizeof(_selectStem) - 1);
                _selectStem[sizeof(_selectStem) - 1] = '\0';
                _switchScene(AcScene::DeviceList);
            }
            return;
        }

        const int old = _padSel;

        if (_device->button.Up.pressed()) {
            if (_padSel == kPadWide)      _padSel = 3 + _padCol;
            else if (_padSel >= 3)        _padSel -= 3;
        }
        if (_device->button.Down.pressed()) {
            if (_padSel < 3)              _padSel += 3;
            else if (_padSel < kPadWide) { _padCol = _padSel - 3; _padSel = kPadWide; }
        }
        if (_device->button.Left.pressed()  && _padSel < kPadWide && (_padSel % 3) > 0) _padSel--;
        if (_device->button.Right.pressed() && _padSel < kPadWide && (_padSel % 3) < 2) _padSel++;

        if (_padSel != old) {
            _drawPad(old);
            _drawPad(_padSel);
        }

        if (_device->button.A.pressed()) {
            _pressPad(_padSel);
            return;
        }
        if (_device->button.B.pressed()) {
            strncpy(_selectStem, _devStem, sizeof(_selectStem) - 1);
            _selectStem[sizeof(_selectStem) - 1] = '\0';
            _switchScene(AcScene::DeviceList);
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Advanced toggles — each one transmits at once
     * ════════════════════════════════════════════════════════════ */

    void App11::_enterAdvanced()
    {
        auto& Lcd = _device->Lcd;
        const bool values[kAdvancedCount] = { _dev.quiet, _dev.turbo, _dev.econo, _dev.light };

        _rows.clear();
        _subs.clear();
        for (int i = 0; i < kAdvancedCount; i++) {
            char row[32];
            /* IRHitachiAc424 and its subclasses expose none of these. */
            snprintf(row, sizeof(row), "%-6s %s", kAdvancedLabels[i],
                     _hitachiDirect ? "n/a" : acstore::boolName(values[i]));
            _rows.push_back(row);
        }

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Advanced", _protoName, hp::COL_FG);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", _hitachiDirect ? nullptr : "[A]Toggle",
                        nullptr, "[B]Back");
        if (_selAdvanced >= kAdvancedCount) _selAdvanced = 0;
        _sel = _selAdvanced;
        _drawRows();
    }

    void App11::_runAdvanced()
    {
        if (_navList()) { _selAdvanced = _sel; _drawRows(); }

        if (_device->button.A.pressed()) {
            _selAdvanced = _sel;
            if (_hitachiDirect) {
                _toast("Not supported by this protocol");
                return;
            }
            if (!_protoOk) {
                _toast("Not sent - protocol unsupported");
                return;
            }
            bool* target = nullptr;
            switch (_sel) {
                case 0:  target = &_dev.quiet; break;
                case 1:  target = &_dev.turbo; break;
                case 2:  target = &_dev.econo; break;
                default: target = &_dev.light; break;
            }
            const acstore::Device before = _dev;
            *target = !*target;

            char label[24];
            snprintf(label, sizeof(label), "%s %s", kAdvancedLabels[_sel],
                     acstore::boolName(*target));
            _redraw();                 /* rows carry the value, so rebuild them */
            if (!_sendAndReport(label)) _dev = before;
            return;
        }
        if (_device->button.B.pressed()) _switchScene(AcScene::Control, false);
    }
}
