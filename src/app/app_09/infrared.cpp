/**
 * @file infrared.cpp
 * @author Mingo
 * @brief App09 — Infrared (Flipper-style TUI)
 *          MainMenu → Learn / Remotes / Universal (all brands)
 *        A device file is a container; its signals are its buttons.
 *        Storage: Flipper-compatible .ir (ir_store.{h,cpp})
 *        Codec  : (protocol, address, command) <-> IRsend (ir_flipper_codec.h)
 *        Contract: docs/app09-infrared-redesign.md §2 §4 §5
 * @version 2.0
 * @date 2026-09-05
 * @copyright Copyright (c) 2025
 */
#include "infrared.h"
#include "../../bsp/config.h"
#include "../app_common/hp_ui.h"
#include "ir_raw_tools.h"

#include <cstdarg>

/* ── Layout constants (bound to hp_ui shared chrome) ── */
static constexpr int MENU_Y0 = hp::CON_Y0 + 2;

static constexpr const char* IR_LEARN_ICON_PATH = "/assets/ir_icon.png";

/* Known category count — indices 0-8 map to kVBtnsDefs[], projectors alias=2 */
static constexpr int kUnivKnownCount = 9;

/* Map filename stem → category index; -1 = generic */
static int univCatIndex(const char* stem) {
    if (strcasecmp(stem, "tv")           == 0) return 0;
    if (strcasecmp(stem, "ac")           == 0) return 1;
    if (strcasecmp(stem, "projector")    == 0) return 2;
    if (strcasecmp(stem, "audio")        == 0) return 3;
    if (strcasecmp(stem, "bluray_dvd")   == 0) return 4;
    if (strcasecmp(stem, "digital_sign") == 0) return 5;
    if (strcasecmp(stem, "fans")         == 0) return 6;
    if (strcasecmp(stem, "leds")         == 0) return 7;
    if (strcasecmp(stem, "monitor")      == 0) return 8;
    if (strcasecmp(stem, "projectors")   == 0) return 2; /* same layout */
    return -1;
}

/* Strip the extension from a file name. */
static String stemOf(const String& filename) {
    int dot = filename.lastIndexOf('.');
    return (dot >= 0) ? filename.substring(0, dot) : filename;
}

/* Make a display label from a filename: strip .ir, replace _ with space,
 * short names (≤3 chars) are fully uppercased, longer ones capitalize first. */
static String univDisplayName(const String& filename) {
    String n = stemOf(filename);
    n.replace("_", " ");
    if (n.length() <= 3) { n.toUpperCase(); }
    else if (n.length() > 0) { n.setCharAt(0, toupper((unsigned char)n.charAt(0))); }
    return n;
}

/* ── Virtual remote button definitions
 *    Layout: 2-column grid, rows = ceil(N/2).
 *    Index matches univCatIndex(): TV=0 AC=1 Proj=2 Audio=3 BDvd=4
 *                                  Sign=5 Fan=6 LED=7 Mon=8
 * ── */
struct VBtnDef {
    const char* label;
    const char* names[3];   /* candidate signal names, nullptr-terminated */
};

/* Each button carries up to three candidate signal names; the first one the
 * indexed file actually contains wins (case-insensitive). The bundled library
 * spells the same function differently per category (Eject vs Open_Close,
 * Rotate vs Swing, SOURCE vs Input_next), and a button with no counterpart in
 * its file was dropped or relabelled — every button below resolves against
 * "sd files/infrared/universal/". */

/* 0 — TV  (tv.ir: Power Mute Vol_up Vol_dn Ch_next Ch_prev) */
static const VBtnDef kVBtns_TV[] = {
    {"POWER",  {"Power",   nullptr, nullptr}}, {"MUTE",  {"Mute",    nullptr, nullptr}},
    {"VOL +",  {"Vol_up",  nullptr, nullptr}}, {"CH +",  {"Ch_next", "Ch_up",   nullptr}},
    {"VOL -",  {"Vol_dn",  nullptr, nullptr}}, {"CH -",  {"Ch_prev", "Ch_dn",   nullptr}},
};
static constexpr int kVBtns_TV_N = 6;

/* 1 — AC  (ac.ir: Off Dh Cool_hi Cool_lo Heat_hi Heat_lo) */
static const VBtnDef kVBtns_AC[] = {
    {"OFF",    {"Off",     nullptr, nullptr}}, {"DRY",    {"Dh",      "Dry",  nullptr}},
    {"COOL ^", {"Cool_hi", nullptr, nullptr}}, {"HEAT ^", {"Heat_hi", nullptr, nullptr}},
    {"COOL v", {"Cool_lo", nullptr, nullptr}}, {"HEAT v", {"Heat_lo", nullptr, nullptr}},
};
static constexpr int kVBtns_AC_N = 6;

/* 2 — Projector  (projector.ir / projectors.ir: Power Mute Vol_up Vol_dn) */
static const VBtnDef kVBtns_Proj[] = {
    {"POWER",  {"Power",   nullptr, nullptr}}, {"MUTE",  {"Mute",   nullptr, nullptr}},
    {"VOL +",  {"Vol_up",  nullptr, nullptr}}, {"VOL -", {"Vol_dn", nullptr, nullptr}},
};
static constexpr int kVBtns_Proj_N = 4;

/* 3 — Audio system  (audio.ir: 8/8 present) */
static const VBtnDef kVBtns_Audio[] = {
    {"POWER",  {"Power", nullptr, nullptr}}, {"MUTE",  {"Mute",   nullptr, nullptr}},
    {"VOL +",  {"Vol_up", nullptr, nullptr}}, {"VOL -", {"Vol_dn", nullptr, nullptr}},
    {"PLAY",   {"Play",  nullptr, nullptr}}, {"PAUSE", {"Pause",  nullptr, nullptr}},
    {"NEXT",   {"Next",  nullptr, nullptr}}, {"PREV",  {"Prev",   nullptr, nullptr}},
};
static constexpr int kVBtns_Audio_N = 8;

/* 4 — Blu-ray / DVD  (bluray_dvd.ir: Power Eject Play Pause Fast_fo Fast_ba Ok
 *   Subtitle — Stop/Mute/Next/Prev do not exist in the file and were replaced) */
static const VBtnDef kVBtns_BDvd[] = {
    {"POWER",  {"Power",   nullptr,      nullptr}}, {"EJECT", {"Eject",   "Open_Close", nullptr}},
    {"PLAY",   {"Play",    nullptr,      nullptr}}, {"PAUSE", {"Pause",   nullptr,      nullptr}},
    {"FWD",    {"Fast_fo", "Next",       nullptr}}, {"REW",   {"Fast_ba", "Prev",       nullptr}},
    {"OK",     {"Ok",      "Enter",      nullptr}}, {"SUB",   {"Subtitle", nullptr,     nullptr}},
};
static constexpr int kVBtns_BDvd_N = 8;

/* 5 — Digital signage  (digital_sign.ir: POWER SOURCE PLAY STOP) */
static const VBtnDef kVBtns_Sign[] = {
    {"POWER",  {"Power", nullptr,      nullptr}}, {"SOURCE", {"Source", "Input_next", "Input"}},
    {"PLAY",   {"Play",  nullptr,      nullptr}}, {"STOP",   {"Stop",   nullptr,      nullptr}},
};
static constexpr int kVBtns_Sign_N = 4;

/* 6 — Fans  (fans.ir: Power Rotate Speed_up Speed_dn Mode Timer) */
static const VBtnDef kVBtns_Fan[] = {
    {"POWER",  {"Power",    nullptr,  nullptr}}, {"SWING", {"Rotate", "Swing",  nullptr}},
    {"SPD +",  {"Speed_up", nullptr,  nullptr}}, {"SPD -", {"Speed_dn", nullptr, nullptr}},
    {"MODE",   {"Mode",     "Sleep",  nullptr}}, {"TIMER", {"Timer",  nullptr,  nullptr}},
};
static constexpr int kVBtns_Fan_N = 6;

/* 7 — LED strips  (leds.ir: Power_on Power_off Brightness_up/dn Red Green Blue White) */
static const VBtnDef kVBtns_LED[] = {
    {"ON",     {"Power_on",       "Power", nullptr}}, {"OFF",   {"Power_off", nullptr, nullptr}},
    {"BRT +",  {"Brightness_up",  nullptr, nullptr}}, {"BRT -", {"Brightness_dn", nullptr, nullptr}},
    {"RED",    {"Red",            nullptr, nullptr}}, {"GREEN", {"Green",     nullptr, nullptr}},
    {"BLUE",   {"Blue",           nullptr, nullptr}}, {"WHITE", {"White",     nullptr, nullptr}},
};
static constexpr int kVBtns_LED_N = 8;

/* 8 — PC monitor  (monitor.ir: POWER SOURCE MENU EXIT) */
static const VBtnDef kVBtns_Mon[] = {
    {"POWER",  {"Power", nullptr,      nullptr}}, {"SOURCE", {"Source", "Input_next", "Input"}},
    {"MENU",   {"Menu",  nullptr,      nullptr}}, {"EXIT",   {"Exit",   "Back",       nullptr}},
};
static constexpr int kVBtns_Mon_N = 4;

/* Indexed by univCatIndex() return value (0-8) */
static const VBtnDef* const kVBtnsDefs[] = {
    kVBtns_TV, kVBtns_AC, kVBtns_Proj,
    kVBtns_Audio, kVBtns_BDvd, kVBtns_Sign,
    kVBtns_Fan, kVBtns_LED, kVBtns_Mon,
};
static const int kVBtnsCount[] = {
    kVBtns_TV_N, kVBtns_AC_N, kVBtns_Proj_N,
    kVBtns_Audio_N, kVBtns_BDvd_N, kVBtns_Sign_N,
    kVBtns_Fan_N, kVBtns_LED_N, kVBtns_Mon_N,
};

/* First alias present in the index wins; -1 when the file has none of them. */
static int univFindAlias(const irstore::Index& index, const VBtnDef& def)
{
    for (int i = 0; i < 3; i++) {
        if (!def.names[i]) break;
        int slot = index.find(def.names[i]);
        if (slot >= 0) return slot;
    }
    return -1;
}

/* TV-B-Gone is the extra last button of the TV category. */
static constexpr int kUnivCatTV     = 0;
static constexpr const char* kTvbgLabel = "TV-B-GONE";

/* Compute button rect for a 2-column grid within the content area.
 *   Area: x=5..314, y=CON_Y0+4..CON_Y1-4.  GAP=6 between buttons. */
static void _getVBtnRect(int idx, int numBtns,
                         int& bx, int& by, int& bw, int& bh)
{
    const int GAP = 6, AX = 5, AY = hp::CON_Y0 + 4;
    const int AW  = hp::W - 10;
    const int AH  = hp::CON_Y1 - hp::CON_Y0 - 8;
    int rows = (numBtns + 1) / 2;
    if (rows < 1) rows = 1;
    bw = (AW - GAP) / 2;
    bh = (AH - (rows - 1) * GAP) / rows;
    bx = AX + (idx % 2) * (bw + GAP);
    by = AY + (idx / 2) * (bh + GAP);
}

/* ── Shared virtual-keyboard keymap (4 x 10) ── */
static const char* kNameKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3",
    "4", "5", "6", "7", "8", "9", "_", "-", ".", "[DEL]",
    "[OK]"
};
static constexpr int kNameCols     = 10;
static constexpr int kNameKeyCount = (int)(sizeof(kNameKeys) / sizeof(kNameKeys[0]));

/* ── Fixed menus ── */
static const char* kMainItems[]  = { "Learn", "Remotes", "Universal (all brands)" };
static constexpr int kMainCount  = 3;

static const char* kLearnItems[] = { "Send test", "Save...", "Learn again" };
static constexpr int kLearnCount = 3;

/* Identify (a paused sweep). Previous/Next step the cursor and send at once so
 * the user can walk back to the code the device actually reacted to. */
static const char* kIdentItems[] = {
    "Resend this", "Previous", "Next", "Save as device...", "Resume sweep", "Stop"
};
static constexpr int kIdentCount = 6;

/* Load a file from SD into PSRAM. Caller owns the returned buffer. */
static uint8_t* _irLoadSdFile(const char* path, size_t& outLen)
{
    outLen = 0;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return nullptr;
    size_t sz = f.size();
    if (sz == 0) { f.close(); return nullptr; }
    uint8_t* buf = (uint8_t*)ps_malloc(sz);
    if (!buf) { f.close(); return nullptr; }
    size_t got = f.read(buf, sz);
    f.close();
    if (got != sz) { free(buf); return nullptr; }
    outLen = sz;
    return buf;
}

/* Raw captures are replayed at 38 kHz unless the decoder recognised a family
 * that the library itself modulates differently (checked against the kXxxFreq
 * constants in lib/IRremoteESP8266/src/ir_*.h: every AC family listed in the
 * task is 38 kHz except the Panasonic and Daikin2 frames at 36.7 kHz). */
static uint32_t carrierForDecode(decode_type_t t)
{
    switch (t) {
        case PANASONIC:
        case PANASONIC_AC:
        case PANASONIC_AC32:
        case DAIKIN2:
            return 36700;
        default:
            return 38000;
    }
}

/* Progress callback for the chunked universal-library scan. Keeps the button
 * driver alive so a long-press B still aborts and exits during a 350 KB scan. */
static bool _irIndexTick(void* ctx, uint32_t done, uint32_t total)
{
    (void)done; (void)total;
    DEVICES* dev = static_cast<DEVICES*>(ctx);
    dev->button.update();
    dev->button.tick();
    if (dev->button.B.isLongPress()) return false;
    static uint32_t phase = 0;
    hp::drawLoadingTick(dev->Lcd, phase += 4);
    return true;
}

/* Input-box heading for the shared name editor. */
static const char* _nameHeading(MOONCAKE::APPS::IrNameMode m)
{
    using M = MOONCAKE::APPS::IrNameMode;
    return (m == M::NewDevice || m == M::RenameDevice || m == M::SaveAsDevice)
           ? "Device:" : "Name:";
}

/* Draw the 3 fixed LearnResult rows below the summary line. */
static void _drawLearnRows(LGFX_Class& lcd, int sel)
{
    for (int i = 0; i < kLearnCount; i++)
        hp::drawListItem(lcd, i + 2, kLearnItems[i], i == sel);
}

/* Identify rows start one slot below the identified-signal summary. */
static void _drawIdentRows(LGFX_Class& lcd, int sel)
{
    for (int i = 0; i < kIdentCount; i++)
        hp::drawListItem(lcd, i + 1, kIdentItems[i], i == sel);
}

namespace MOONCAKE::APPS
{
    /* ════════════════════════════════════════════════════════════
     *  Constructor / Lifecycle
     * ════════════════════════════════════════════════════════════ */

    App09::App09(DEVICES* device) : _device(device)
    {
        setAppInfo().name = "Infrared";
    }

    void App09::onOpen()
    {
        _irSend = new IRsend(HAL_PIN_IR_TX);
        _irSend->begin();
        _irRecv = nullptr;

        /* Never memset structs holding std::vector members. */
        _learned.reset();
        _learnSummary[0]   = '\0';
        _learnFaded        = false;
        _learnFadeMs       = 0;
        _learnNormUs       = 0;
        _learnDevice[0]    = '\0';
        _learnReturnsToView = false;

        _devStem[0] = '\0';
        _devPath[0] = '\0';
        _devSignals.clear();
        _fileList.clear();
        _univCloseFile();
        _univPath[0]  = '\0';
        _univIndex.clear();
        _blastOffsets.clear();
        _lastSent.reset();
        _sweepGapMs   = kGapSlow;      /* the gap choice lives for one app session */
        _blastIsTvbg  = false;
        _blastResume  = false;
        _blastCursor  = 0;

        _editBuf[0]     = '\0';
        _vkSel          = 0;
        _saveDevStem[0] = '\0';
        _saveBtnName[0] = '\0';
        _toastMsg[0]    = '\0';
        _toastUntil     = 0;

        _learnIconTried = false;
        _learnIconLen   = 0;
        _learnIconPng   = nullptr;

        /* Per-list selections and editor/confirm state must not survive a
         * previous run of the app. */
        _selRemoteList = _selSaveTarget = _selRemoteView = 0;
        _selUnivMenu   = _selUnivCat    = 0;
        _vkCol         = 0;
        _nameMode      = IrNameMode::Button;
        _nameFromDup   = false;
        _selectStem[0] = '\0';
        _confirmKind   = IrConfirmKind::None;
        _confirmIndex  = 0;
        _univCat       = -1;
        _vbSel         = 0;
        _rcToggle      = false;
        _repaintOnly   = false;

        if (!SD_MMC.exists(irstore::kIrDir)) SD_MMC.mkdir(irstore::kIrDir);

        _switchScene(IrScene::MainMenu);
    }

    void App09::onRunning()
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
                case IrScene::MainMenu:      _enterMainMenu();      break;
                case IrScene::LearnWait:     _enterLearnWait();     break;
                case IrScene::LearnResult:   _enterLearnResult();   break;
                case IrScene::SaveTarget:    _enterSaveTarget();    break;
                case IrScene::NameEditor:    _enterNameEditor();    break;
                case IrScene::DupResolve:    _enterDupResolve();    break;
                case IrScene::RemoteList:    _enterRemoteList();    break;
                case IrScene::DeviceOptions: _enterDeviceOptions(); break;
                case IrScene::RemoteView:    _enterRemoteView();    break;
                case IrScene::SignalOptions: _enterSignalOptions(); break;
                case IrScene::Confirm:       _enterConfirm();       break;
                case IrScene::UniversalMenu: _enterUniversalMenu(); break;
                case IrScene::UniversalCat:  _enterUniversalCat();  break;
                case IrScene::Blast:         _enterBlast();         break;
                case IrScene::Identify:      _enterIdentify();      break;
            }
            /* A live toast survives a scene repaint. */
            if (_toastUntil && millis() < _toastUntil)
                hp::drawToast(_device->Lcd, _toastMsg);
            _repaintOnly = false;
        }

        switch (_scene) {
            case IrScene::MainMenu:      _runMainMenu();      break;
            case IrScene::LearnWait:     _runLearnWait();     break;
            case IrScene::LearnResult:   _runLearnResult();   break;
            case IrScene::SaveTarget:    _runSaveTarget();    break;
            case IrScene::NameEditor:    _runNameEditor();    break;
            case IrScene::DupResolve:    _runDupResolve();    break;
            case IrScene::RemoteList:    _runRemoteList();    break;
            case IrScene::DeviceOptions: _runDeviceOptions(); break;
            case IrScene::RemoteView:    _runRemoteView();    break;
            case IrScene::SignalOptions: _runSignalOptions(); break;
            case IrScene::Confirm:       _runConfirm();       break;
            case IrScene::UniversalMenu: _runUniversalMenu(); break;
            case IrScene::UniversalCat:  _runUniversalCat();  break;
            case IrScene::Blast:         _runBlast();         break;
            case IrScene::Identify:      _runIdentify();      break;
        }
    }

    void App09::onClose()
    {
        _stopRx();
        if (_irSend) { delete _irSend; _irSend = nullptr; }
        _device->led.off();
        _fileList.clear();
        _devSignals.clear();
        _univCloseFile();
        _univIndex.clear();
        _blastOffsets.clear();
        _rows.clear();
        _subs.clear();
        if (_learnIconPng) { free(_learnIconPng); _learnIconPng = nullptr; }
        _learnIconLen   = 0;
        _learnIconTried = false;
    }

    /* ════════════════════════════════════════════════════════════
     *  Scene / list plumbing
     * ════════════════════════════════════════════════════════════ */

    void App09::_switchScene(IrScene s, bool resetSel)
    {
        _prevScene  = _scene;
        _scene      = s;
        _sceneDirty = true;
        if (resetSel) { _sel = 0; _scrollTop = 0; }
    }

    int App09::_visibleRows() const
    {
        return _subs.empty() ? hp::LIST_VIS : hp::LIST2_VIS;
    }

    void App09::_drawRows()
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

    bool App09::_navList()
    {
        const int total = (int)_rows.size();
        if (total <= 0) return false;
        const int old = _sel;
        if (_device->button.Up.pressed()   && _sel > 0)         _sel--;
        if (_device->button.Down.pressed() && _sel < total - 1) _sel++;
        return _sel != old;
    }

    void App09::_drawCentered(const char* l1, const char* l2, const char* l3,
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

    void App09::_toast(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(_toastMsg, sizeof(_toastMsg), fmt, ap);
        va_end(ap);
        _toastUntil = millis() + 600;
        /* With a repaint already queued the banner is drawn after the new
         * scene, never for one frame on top of the old one. */
        if (!_sceneDirty) hp::drawToast(_device->Lcd, _toastMsg);
    }

    void App09::_serviceToast()
    {
        if (_toastUntil && millis() >= _toastUntil) {
            _toastUntil  = 0;
            _repaintOnly = true;     /* repaint wipes the banner, nothing else */
            _sceneDirty  = true;
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Main menu
     * ════════════════════════════════════════════════════════════ */

    void App09::_enterMainMenu()
    {
        auto& Lcd = _device->Lcd;
        _rows.clear();
        _subs.clear();
        for (int i = 0; i < kMainCount; i++) _rows.push_back(kMainItems[i]);

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Infrared");
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Exit");
        _drawRows();
    }

    void App09::_runMainMenu()
    {
        if (_navList()) _drawRows();

        if (_device->button.A.pressed()) {
            switch (_sel) {
                case 0: _startLearn(nullptr); break;
                case 1: _selRemoteList = 0; _switchScene(IrScene::RemoteList);    break;
                case 2: _selUnivMenu   = 0; _switchScene(IrScene::UniversalMenu); break;
            }
            return;
        }
        if (_device->button.B.pressed()) close();
    }

    /* ════════════════════════════════════════════════════════════
     *  Learn
     * ════════════════════════════════════════════════════════════ */

    void App09::_startLearn(const char* deviceStem)
    {
        if (deviceStem && deviceStem[0]) {
            strncpy(_learnDevice, deviceStem, sizeof(_learnDevice) - 1);
            _learnDevice[sizeof(_learnDevice) - 1] = '\0';
            _learnReturnsToView = true;
        } else {
            _learnDevice[0] = '\0';
            _learnReturnsToView = false;
        }
        _switchScene(IrScene::LearnWait);
    }

    void App09::_enterLearnWait()
    {
        auto& Lcd = _device->Lcd;
        if (!_learnIconTried) {
            _learnIconTried = true;
            _learnIconPng   = _irLoadSdFile(IR_LEARN_ICON_PATH, _learnIconLen);
        }

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Learn", "RX", hp::COL_WARN);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, nullptr, nullptr, nullptr, "[B]Back");

        Lcd.setFont(&fonts::efontCN_16);
        Lcd.setTextColor(hp::COL_FG, hp::COL_BG);
        Lcd.setCursor(16, hp::CON_Y0 + 8);
        Lcd.print("Point the remote at MeowKit");
        Lcd.setCursor(16, hp::CON_Y0 + 26);
        Lcd.print("and press a button");

        if (_learnIconPng && _learnIconLen > 0)
            hp::drawPng(Lcd, 78, hp::CON_Y0 + 52, _learnIconPng, _learnIconLen);

        Lcd.setTextColor(hp::COL_DIM, hp::COL_BG);
        Lcd.setCursor(16, hp::CON_Y1 - 22);
        if (_learnDevice[0]) Lcd.printf("Saving into: %s", _learnDevice);
        else                 Lcd.print("Waiting for IR signal...");

        /* A repaint (expiring toast) must not abort a capture in progress. */
        if (!_repaintOnly) _startRx();
    }

    void App09::_runLearnWait()
    {
        if (_device->button.B.pressed()) {
            _stopRx();
            if (_learnReturnsToView && _learnDevice[0]) _openDevice(_learnDevice);
            else                                       _switchScene(IrScene::MainMenu);
            return;
        }

        if (!_irRecv) return;

        decode_results results;
        if (!_irRecv->decode(&results)) return;

        /* IRrecv was built with save_buffer = true, so decode() copies the
         * capture and rearms the receiver itself; no resume() call is needed. */

        /* Build the timing list first: the fade measurement describes the
         * capture itself and is worth logging even for a frame we discard. */
        std::vector<uint16_t> raw;
        raw.reserve(results.rawlen);
        for (uint16_t i = 1; i < results.rawlen; i++) {
            uint32_t us = (uint32_t)results.rawbuf[i] * kRawTick;
            raw.push_back(us > 65535 ? 65535 : (uint16_t)us);
        }
        const irraw::FadeInfo fade = irraw::analyseFade(raw.data(), raw.size());

        Serial.printf("IR_LEARN,decode_type=%s,bits=%u,rawlen=%u,overflow=%d,"
                      "fade=%.2f,fade_ms=%lu\n",
                      typeToString(results.decode_type, false).c_str(),
                      (unsigned)results.bits, (unsigned)results.rawlen,
                      results.overflow ? 1 : 0,
                      (double)fade.ratio, (unsigned long)(fade.fadeUs / 1000));

        /* A truncated capture can never be replayed; do not keep it. */
        if (results.overflow) {
            _toast("Capture overflow - press again");
            return;
        }

        /* A repeat / ditto frame carries no payload. Storing it would produce
         * a garbage RAW blob, so drop it and keep listening. */
        if (results.repeat || results.bits == 0) {
            _toast("Repeat frame ignored - press again");
            return;
        }

        _learned.reset();
        _learnFaded  = false;
        _learnFadeMs = 0;
        _learnNormUs = 0;

        /* decode_results → irfc::TxKind (spec §3). Anything not listed is raw. */
        irfc::TxKind kind = irfc::TxKind::None;
        switch (results.decode_type) {
            case NEC:       kind = irfc::TxKind::NEC;          break;
            case SAMSUNG:   kind = irfc::TxKind::SAMSUNG;      break;
            case SONY:      kind = irfc::TxKind::SONY;         break;
            case RC5:       kind = irfc::TxKind::RC5;          break;
            case RC6:       kind = irfc::TxKind::RC6;          break;
            case PANASONIC: kind = irfc::TxKind::PANASONIC64;  break;
            case PIONEER:   kind = irfc::TxKind::PIONEER;      break;
            case SANYO_LC7461: kind = irfc::TxKind::SANYO_LC7461; break;
            default:        kind = irfc::TxKind::None;         break;
        }

        bool parsed = false;
        if (kind != irfc::TxKind::None) {
            irfc::Flipper fl;
            irfc::TxFrame tf;
            /* Learn-side self-check: only store parsed when the round trip
             * reproduces exactly what the receiver reported. */
            /* The toggle bit of RC5 (bit 11) / RC6 (bit 16) is not part of the
             * stored signal, so it is masked out of the comparison. */
            uint64_t toggleMask = 0;
            if (kind == irfc::TxKind::RC5) toggleMask = 1ULL << 11;
            if (kind == irfc::TxKind::RC6) toggleMask = 1ULL << 16;
            if (irfc::fromDecode(kind, results.value, results.bits, fl) &&
                irfc::toTx(fl, tf) &&
                (tf.data & ~toggleMask) == (results.value & ~toggleMask) &&
                tf.nbits == results.bits) {
                _learned.isRaw   = false;
                _learned.proto   = fl.proto;
                _learned.address = fl.address;
                _learned.command = fl.command;
                parsed = true;
                snprintf(_learnSummary, sizeof(_learnSummary), "%s · A:%02X C:%02X",
                         irfc::protoName(fl.proto),
                         (unsigned)fl.address, (unsigned)fl.command);
            }
        }

        if (!parsed) {
            /* Anything shorter than a real frame is noise or a stray edge. */
            static constexpr int kMinRawSamples = 8;
            if ((int)raw.size() < kMinRawSamples) {
                _toast("Signal too short - press again");
                return;
            }

            _learnFaded  = fade.faded;
            _learnFadeMs = fade.fadeUs / 1000;

            /* An unrecognised pulse-distance frame whose marks decayed is
             * rebuilt: every mark back to the clean leading value, the
             * difference given to its space so the bit periods survive. */
            uint16_t markUs = 0;
            if (results.decode_type == UNKNOWN &&
                irraw::canNormalise(raw.data(), raw.size(), markUs)) {
                irraw::normaliseMarks(raw.data(), raw.size(), markUs);
                _learnNormUs = markUs;
            }

            _learned.isRaw     = true;
            _learned.frequency = carrierForDecode(results.decode_type);
            _learned.raw.swap(raw);
            if (_learnNormUs) {
                snprintf(_learned.note, sizeof(_learned.note),
                         "meowkit: marks normalised to %u us (fade %.2f)",
                         (unsigned)_learnNormUs, (double)fade.ratio);
            }
            _buildRawSummary(results.decode_type, results.bits);

            if (_learnFaded)
                _toast("Faded at %lu ms - move 20-50 cm",
                       (unsigned long)_learnFadeMs);
        }

        _stopRx();
        _switchScene(IrScene::LearnResult);
    }

    /* "RAW · 407 · FADED norm" / "RAW (HITACHI_AC 280b) · 583".
     * The protocol block and "norm" are mutually exclusive (normalisation only
     * runs on UNKNOWN), but the length is still checked before it is used. */
    void App09::_buildRawSummary(decode_type_t type, uint16_t bits)
    {
        const int n = (int)_learned.raw.size();

        char flags[16] = {0};
        if (_learnFaded && _learnNormUs) snprintf(flags, sizeof(flags), " · FADED norm");
        else if (_learnFaded)            snprintf(flags, sizeof(flags), " · FADED");
        else if (_learnNormUs)           snprintf(flags, sizeof(flags), " · norm");

        char body[64];
        if (type != UNKNOWN) {
            /* bits == 0 was rejected earlier, so the count is always known. */
            String proto = typeToString(type, false);
            snprintf(body, sizeof(body), "RAW (%s %ub) · %d", proto.c_str(),
                     (unsigned)bits, n);
        } else if (flags[0]) {
            snprintf(body, sizeof(body), "RAW · %d", n);
        } else {
            snprintf(body, sizeof(body), "RAW · %d samples", n);
        }

        if (strlen(body) + strlen(flags) < sizeof(_learnSummary))
            snprintf(_learnSummary, sizeof(_learnSummary), "%s%s", body, flags);
        else    /* a very long protocol name: drop the detail, keep the flags */
            snprintf(_learnSummary, sizeof(_learnSummary), "RAW · %d%s", n, flags);
    }

    void App09::_enterLearnResult()
    {
        auto& Lcd = _device->Lcd;
        _rows.clear();
        _subs.clear();
        for (int i = 0; i < kLearnCount; i++) _rows.push_back(kLearnItems[i]);

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Signal", "OK", hp::COL_FG);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Back");

        Lcd.setFont(&fonts::efontCN_16);
        Lcd.setTextColor(hp::COL_ACCENT, hp::COL_BG);
        Lcd.setCursor(hp::PAD_X + 8, MENU_Y0 + 6);
        Lcd.print(_learnSummary);

        /* One extra line explaining what the post-processing did. */
        if (_learnFaded || _learnNormUs) {
            char info[48];
            if (_learnFaded)
                snprintf(info, sizeof(info), "Signal faded at %lu ms",
                         (unsigned long)_learnFadeMs);
            else
                snprintf(info, sizeof(info), "Marks normalised to %u us",
                         (unsigned)_learnNormUs);
            Lcd.setTextColor(_learnFaded ? hp::COL_WARN : hp::COL_DIM, hp::COL_BG);
            Lcd.setCursor(hp::PAD_X + 8, MENU_Y0 + 6 + hp::ITEM_H);
            Lcd.print(info);
        }

        _drawLearnRows(Lcd, _sel);
    }

    void App09::_runLearnResult()
    {
        if (_navList()) _drawLearnRows(_device->Lcd, _sel);

        if (_device->button.A.pressed()) {
            switch (_sel) {
                case 0:                       /* Send test — through the codec */
                    _sendOrToast(_learned, "Sent test");
                    break;
                case 1:                       /* Save... */
                    if (_learnDevice[0]) {
                        strncpy(_saveDevStem, _learnDevice, sizeof(_saveDevStem) - 1);
                        _saveDevStem[sizeof(_saveDevStem) - 1] = '\0';
                        char path[168];
                        irstore::devicePath(_saveDevStem, path, sizeof(path));
                        int n = irstore::countSignals(path);
                        snprintf(_editBuf, sizeof(_editBuf), "BTN_%d", (n > 0 ? n : 0) + 1);
                        _nameMode    = IrNameMode::Button;
                        _vkSel       = 0;
                        _nameFromDup = false;
                        _switchScene(IrScene::NameEditor);
                    } else {
                        _switchScene(IrScene::SaveTarget);
                    }
                    break;
                case 2:                       /* Learn again */
                    _switchScene(IrScene::LearnWait);
                    break;
            }
            return;
        }

        if (_device->button.B.pressed()) {
            if (_learnReturnsToView && _learnDevice[0]) _openDevice(_learnDevice);
            else                                       _switchScene(IrScene::MainMenu);
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Save target picker
     * ════════════════════════════════════════════════════════════ */

    void App09::_enterSaveTarget()
    {
        auto& Lcd = _device->Lcd;
        /* A bare repaint redraws the cached rows; only a real entry rescans. */
        if (!_repaintOnly) {
            _fileList.clear();
            irstore::listIrFiles(irstore::kIrDir, _fileList);

            _rows.clear();
            _subs.clear();
            _rows.push_back("+ New device...");
            _subs.push_back("create a new .ir file");
            for (const auto& f : _fileList) {
                char path[168];
                snprintf(path, sizeof(path), "%s/%s", irstore::kIrDir, f.c_str());
                int n = irstore::countSignals(path);
                char sub[24];
                snprintf(sub, sizeof(sub), "%d buttons", n < 0 ? 0 : n);
                _rows.push_back(stemOf(f));
                _subs.push_back(sub);
            }
        }

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Save to", nullptr);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Back");
        _sel = _selSaveTarget;
        _drawRows();
    }

    void App09::_runSaveTarget()
    {
        if (_navList()) { _selSaveTarget = _sel; _drawRows(); }

        if (_device->button.A.pressed()) {
            _selSaveTarget = _sel;
            if (_sel == 0) {
                _editBuf[0]  = '\0';
                _vkSel       = 0;
                _nameMode    = IrNameMode::NewDevice;
                _nameFromDup = false;
                _switchScene(IrScene::NameEditor);
            } else {
                String stem = _rows[_sel];
                strncpy(_saveDevStem, stem.c_str(), sizeof(_saveDevStem) - 1);
                _saveDevStem[sizeof(_saveDevStem) - 1] = '\0';
                char path[168];
                irstore::devicePath(_saveDevStem, path, sizeof(path));
                int n = irstore::countSignals(path);
                snprintf(_editBuf, sizeof(_editBuf), "BTN_%d", (n > 0 ? n : 0) + 1);
                _vkSel       = 0;
                _nameMode    = IrNameMode::Button;
                _nameFromDup = false;
                _switchScene(IrScene::NameEditor);
            }
            return;
        }
        if (_device->button.B.pressed()) _switchScene(IrScene::LearnResult);
    }

    /* ════════════════════════════════════════════════════════════
     *  Name editor (shared virtual keyboard)
     * ════════════════════════════════════════════════════════════ */

    void App09::_enterNameEditor()
    {
        auto& Lcd = _device->Lcd;
        const char* title = "Button name";
        switch (_nameMode) {
            case IrNameMode::NewDevice:    title = "Device name";   break;
            case IrNameMode::RenameDevice: title = "Rename device"; break;
            case IrNameMode::RenameSignal: title = "Rename button"; break;
            case IrNameMode::SaveAsDevice: title = "Save as device"; break;
            default: break;
        }

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, title, nullptr);
        hp::drawFooter4(Lcd, "[^v<>]Move", "[A]Key", nullptr, "[B]Cancel");
        hp::drawVirtualKeyboard(Lcd, _nameHeading(_nameMode), _editBuf, (int)sizeof(_editBuf) - 1,
                                kNameKeys, kNameKeyCount, kNameCols, _vkSel);
    }

    void App09::_runNameEditor()
    {
        bool redraw = false;

        /* The last row is partial (only [OK]); it is reachable from every key
         * of the row above, and Up returns to the column the user came from. */
        const int lastRowStart = ((kNameKeyCount - 1) / kNameCols) * kNameCols;
        const bool onLastRow   = (_vkSel >= lastRowStart);

        if (_device->button.Up.pressed() && _vkSel >= kNameCols) {
            _vkSel = onLastRow ? (lastRowStart - kNameCols + _vkCol) : (_vkSel - kNameCols);
            redraw = true;
        }
        if (_device->button.Down.pressed() && !onLastRow) {
            _vkCol = _vkSel % kNameCols;
            int down = _vkSel + kNameCols;
            _vkSel  = (down < kNameKeyCount) ? down : lastRowStart;
            redraw  = true;
        }
        if (_device->button.Left.pressed()) {
            if (onLastRow)                 { _vkSel = kNameKeyCount - 2; redraw = true; }
            else if (_vkSel % kNameCols)   { _vkSel--;                   redraw = true; }
        }
        if (_device->button.Right.pressed()) {
            if (onLastRow)                                            { _vkSel = 0;  redraw = true; }
            else if ((_vkSel % kNameCols) < kNameCols - 1
                     && _vkSel + 1 < kNameKeyCount)                   { _vkSel++;    redraw = true; }
        }

        if (_device->button.A.pressed()) {
            const char* key = kNameKeys[_vkSel];
            int len = (int)strlen(_editBuf);
            if (strcmp(key, "[OK]") == 0) {
                _finishNameEditor();
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
            hp::drawVirtualKeyboard(_device->Lcd, _nameHeading(_nameMode), _editBuf,
                                    (int)sizeof(_editBuf) - 1, kNameKeys,
                                    kNameKeyCount, kNameCols, _vkSel);

        if (_device->button.B.pressed()) _cancelNameEditor();
    }

    void App09::_drainInput()
    {
        _device->button.update();
        /* tick() advances the long-press detector; it only clears the flag once
         * the button has been released (see _waitButtonsReleased). */
        _device->button.tick();
        (void)_device->button.A.pressed();     (void)_device->button.A.released();
        (void)_device->button.B.pressed();     (void)_device->button.B.released();
        (void)_device->button.Up.pressed();    (void)_device->button.Up.released();
        (void)_device->button.Down.pressed();  (void)_device->button.Down.released();
        (void)_device->button.Left.pressed();  (void)_device->button.Left.released();
        (void)_device->button.Right.pressed(); (void)_device->button.Right.released();
    }

    /* Button_Class clears its long-press flag in tick(), but only after the
     * button goes back up. A scan aborted with a long press therefore has to
     * wait for the release before the app's global exit check runs again. */
    void App09::_waitButtonsReleased()
    {
        const uint32_t deadline = millis() + 3000;   /* never hang on a stuck pin */
        for (;;) {
            _device->button.update();
            _device->button.tick();
            const bool up =
                _device->button.A.state()    == Button_Class::RELEASED &&
                _device->button.B.state()    == Button_Class::RELEASED &&
                _device->button.Up.state()   == Button_Class::RELEASED &&
                _device->button.Down.state() == Button_Class::RELEASED &&
                _device->button.Left.state() == Button_Class::RELEASED &&
                _device->button.Right.state()== Button_Class::RELEASED;
            if (up || (int32_t)(millis() - deadline) >= 0) break;
        }
        _drainInput();
    }

    /* B leaves the editor without writing anything. */
    void App09::_cancelNameEditor()
    {
        switch (_nameMode) {
            case IrNameMode::NewDevice:
                _switchScene(IrScene::SaveTarget, false);
                break;
            case IrNameMode::Button:
                if (_nameFromDup) _switchScene(IrScene::DupResolve, false);
                else _switchScene(_learnDevice[0] ? IrScene::LearnResult
                                                  : IrScene::SaveTarget, false);
                break;
            case IrNameMode::RenameDevice:
                _switchScene(IrScene::DeviceOptions, false);
                break;
            case IrNameMode::RenameSignal:
                _switchScene(IrScene::SignalOptions, false);
                break;
            case IrNameMode::SaveAsDevice:
                _switchScene(IrScene::Identify, false);
                break;
        }
    }

    void App09::_finishNameEditor()
    {
        char clean[40];
        irstore::sanitiseName(_editBuf, clean, sizeof(clean));

        switch (_nameMode) {
        case IrNameMode::NewDevice: {
            strncpy(_saveDevStem, clean, sizeof(_saveDevStem) - 1);
            _saveDevStem[sizeof(_saveDevStem) - 1] = '\0';
            char path[168];
            irstore::devicePath(_saveDevStem, path, sizeof(path));
            int n = irstore::countSignals(path);
            snprintf(_editBuf, sizeof(_editBuf), "BTN_%d", (n > 0 ? n : 0) + 1);
            _vkSel    = 0;
            _nameMode = IrNameMode::Button;
            _redraw();
            break;
        }
        case IrNameMode::Button:
            strncpy(_saveBtnName, clean, sizeof(_saveBtnName) - 1);
            _saveBtnName[sizeof(_saveBtnName) - 1] = '\0';
            _commitSave(false);
            break;

        case IrNameMode::RenameDevice: {
            char newPath[168];
            irstore::devicePath(clean, newPath, sizeof(newPath));
            if (strcmp(clean, _devStem) == 0) {           /* unchanged */
                _switchScene(IrScene::RemoteList, false);
                break;
            }
            if (!irstore::renameDevice(_devPath, newPath)) {
                _toast("Name exists");
                break;                                     /* stay in the editor */
            }
            strncpy(_devStem, clean, sizeof(_devStem) - 1);
            _devStem[sizeof(_devStem) - 1] = '\0';
            strncpy(_devPath, newPath, sizeof(_devPath) - 1);
            _devPath[sizeof(_devPath) - 1] = '\0';
            /* Follow the device to its new position in the refreshed list. */
            strncpy(_selectStem, _devStem, sizeof(_selectStem) - 1);
            _selectStem[sizeof(_selectStem) - 1] = '\0';
            _switchScene(IrScene::RemoteList, false);
            _toast("Renamed");
            break;
        }
        case IrNameMode::SaveAsDevice:
            _saveIdentifiedDevice(clean);
            break;

        case IrNameMode::RenameSignal: {
            int idx = _confirmIndex;
            if (idx < 0 || idx >= (int)_devSignals.size()) {
                _switchScene(IrScene::RemoteView, false);
                break;
            }
            int dup = irstore::findSignal(_devSignals, clean);
            if (dup >= 0 && dup != idx) { _toast("Name exists"); break; }
            strncpy(_devSignals[idx].name, clean, sizeof(_devSignals[idx].name) - 1);
            _devSignals[idx].name[sizeof(_devSignals[idx].name) - 1] = '\0';
            bool written = irstore::writeFile(_devPath, _devSignals);
            if (!written) irstore::loadFile(_devPath, _devSignals);
            _switchScene(IrScene::RemoteView, false);
            _toast(written ? "Renamed" : "Write failed");
            break;
        }
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Save commit + duplicate resolution
     * ════════════════════════════════════════════════════════════ */

    void App09::_commitSave(bool replaceExisting)
    {
        char path[168];
        irstore::devicePath(_saveDevStem, path, sizeof(path));

        std::vector<irstore::Signal> sigs;
        if (SD_MMC.exists(path)) irstore::loadFile(path, sigs);

        int dup = irstore::findSignal(sigs, _saveBtnName);
        if (dup >= 0 && !replaceExisting) {
            _switchScene(IrScene::DupResolve);
            return;
        }

        irstore::Signal s = _learned;
        strncpy(s.name, _saveBtnName, sizeof(s.name) - 1);
        s.name[sizeof(s.name) - 1] = '\0';

        int target;
        if (dup >= 0) { sigs[dup] = s; target = dup; }
        else          { sigs.push_back(s); target = (int)sigs.size() - 1; }

        if (!irstore::writeFile(path, sigs)) {
            _toast("Save failed - check SD");
            return;
        }

        _openDevice(_saveDevStem, target);
        _toast("Saved: %s / %s", _saveDevStem, _saveBtnName);
    }

    void App09::_enterDupResolve()
    {
        auto& Lcd = _device->Lcd;
        _rows.clear();
        _subs.clear();
        _rows.push_back("Replace");
        _rows.push_back("Rename");
        _rows.push_back("Cancel");

        char title[40];
        snprintf(title, sizeof(title), "%s exists", _saveBtnName);
        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, title, nullptr);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Cancel");
        _drawRows();
    }

    void App09::_runDupResolve()
    {
        if (_navList()) _drawRows();

        if (_device->button.A.pressed()) {
            switch (_sel) {
                case 0: _commitSave(true); break;
                case 1:
                    strncpy(_editBuf, _saveBtnName, sizeof(_editBuf) - 1);
                    _editBuf[sizeof(_editBuf) - 1] = '\0';
                    _vkSel       = 0;
                    _nameMode    = IrNameMode::Button;
                    _nameFromDup = true;
                    _switchScene(IrScene::NameEditor);
                    break;
                default: _switchScene(IrScene::LearnResult); break;
            }
            return;
        }
        if (_device->button.B.pressed()) _switchScene(IrScene::LearnResult);
    }

    /* ════════════════════════════════════════════════════════════
     *  Remotes — device list
     * ════════════════════════════════════════════════════════════ */

    void App09::_enterRemoteList()
    {
        auto& Lcd = _device->Lcd;
        /* A bare repaint redraws the cached rows; only a real entry rescans. */
        if (!_repaintOnly) {
            _fileList.clear();
            irstore::listIrFiles(irstore::kIrDir, _fileList);

            _rows.clear();
            _subs.clear();
            for (const auto& f : _fileList) {
                char path[168];
                snprintf(path, sizeof(path), "%s/%s", irstore::kIrDir, f.c_str());
                int n = irstore::countSignals(path);
                char sub[24];
                snprintf(sub, sizeof(sub), "%d buttons", n < 0 ? 0 : n);
                _rows.push_back(stemOf(f));
                _subs.push_back(sub);
            }
        }

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Remotes", nullptr);
        hp::clearContent(Lcd);

        if (_rows.empty()) {
            hp::drawFooter4(Lcd, nullptr, nullptr, nullptr, "[B]Back");
            _drawCentered("No saved remotes", "Use Learn to add one",
                          nullptr, nullptr, hp::COL_FG);
        } else {
            hp::drawFooter4(Lcd, "[^v]Move", "[A]Open", "[>]Opts", "[B]Back");
            if (_selectStem[0]) {
                for (size_t i = 0; i < _rows.size(); i++)
                    if (strcasecmp(_rows[i].c_str(), _selectStem) == 0) {
                        _selRemoteList = (int)i;
                        break;
                    }
                _selectStem[0] = '\0';
            }
            _sel = _selRemoteList;
            _drawRows();
        }
    }

    void App09::_runRemoteList()
    {
        if (_rows.empty()) {
            if (_device->button.B.pressed()) _switchScene(IrScene::MainMenu);
            return;
        }

        if (_navList()) { _selRemoteList = _sel; _drawRows(); }

        if (_device->button.A.pressed()) {
            _selRemoteList = _sel;
            _openDevice(_rows[_sel].c_str());
            return;
        }
        if (_device->button.Right.pressed()) {
            _selRemoteList = _sel;
            strncpy(_devStem, _rows[_sel].c_str(), sizeof(_devStem) - 1);
            _devStem[sizeof(_devStem) - 1] = '\0';
            irstore::devicePath(_devStem, _devPath, sizeof(_devPath));
            _switchScene(IrScene::DeviceOptions);
            return;
        }
        if (_device->button.B.pressed()) _switchScene(IrScene::MainMenu);
    }

    void App09::_enterDeviceOptions()
    {
        auto& Lcd = _device->Lcd;
        _rows.clear();
        _subs.clear();
        _rows.push_back("Learn new button");
        _rows.push_back("Rename");
        _rows.push_back("Delete");

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, _devStem, "OPTS", hp::COL_FG);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Back");
        _drawRows();
    }

    void App09::_runDeviceOptions()
    {
        if (_navList()) _drawRows();

        if (_device->button.A.pressed()) {
            switch (_sel) {
            case 0:
                _startLearn(_devStem);
                break;
            case 1:
                strncpy(_editBuf, _devStem, sizeof(_editBuf) - 1);
                _editBuf[sizeof(_editBuf) - 1] = '\0';
                _vkSel    = 0;
                _nameMode = IrNameMode::RenameDevice;
                _switchScene(IrScene::NameEditor);
                break;
            default:
                _confirmKind = IrConfirmKind::DeleteDevice;
                snprintf(_confirmLine, sizeof(_confirmLine), "Delete %s?", _devStem);
                _switchScene(IrScene::Confirm);
                break;
            }
            return;
        }
        if (_device->button.B.pressed()) _switchScene(IrScene::RemoteList, false);
    }

    /* ════════════════════════════════════════════════════════════
     *  Remotes — device view (buttons)
     * ════════════════════════════════════════════════════════════ */

    void App09::_openDevice(const char* stem, int selectSignal)
    {
        strncpy(_devStem, stem, sizeof(_devStem) - 1);
        _devStem[sizeof(_devStem) - 1] = '\0';
        irstore::devicePath(_devStem, _devPath, sizeof(_devPath));
        _devSignals.clear();
        irstore::loadFile(_devPath, _devSignals);

        _switchScene(IrScene::RemoteView);
        if (selectSignal < 0) selectSignal = 0;
        if (selectSignal > (int)_devSignals.size()) selectSignal = (int)_devSignals.size();
        _selRemoteView = selectSignal;
        _sel           = selectSignal;
    }

    void App09::_enterRemoteView()
    {
        auto& Lcd = _device->Lcd;
        _rows.clear();
        _subs.clear();
        for (const auto& s : _devSignals) {
            char sub[40];
            if (s.isRaw) snprintf(sub, sizeof(sub), "RAW %d", (int)s.raw.size());
            else         snprintf(sub, sizeof(sub), "%s A:%02X C:%02X",
                                  (s.proto == irfc::Proto::Unknown && s.protoRaw[0])
                                      ? s.protoRaw : irfc::protoName(s.proto),
                                  (unsigned)s.address,
                                  (unsigned)s.command);
            _rows.push_back(s.name);
            _subs.push_back(sub);
        }
        _rows.push_back("+ Learn new button");
        _subs.push_back("");

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, _devStem, nullptr);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Send", "[>]Opts", "[B]Back");
        _sel = _selRemoteView;
        _drawRows();
    }

    void App09::_runRemoteView()
    {
        if (_navList()) { _selRemoteView = _sel; _drawRows(); }

        const int nSig    = (int)_devSignals.size();
        const bool onLearn = (_sel >= nSig);

        if (_device->button.A.pressed()) {
            if (onLearn) {
                _startLearn(_devStem);
            } else {
                char label[48];
                snprintf(label, sizeof(label), "Sent %s", _devSignals[_sel].name);
                _sendOrToast(_devSignals[_sel], label);
            }
            return;
        }
        if (_device->button.Right.pressed() && !onLearn) {
            _selRemoteView = _sel;
            _confirmIndex  = _sel;
            _switchScene(IrScene::SignalOptions);
            return;
        }
        if (_device->button.B.pressed()) _switchScene(IrScene::RemoteList);
    }

    void App09::_enterSignalOptions()
    {
        auto& Lcd = _device->Lcd;
        _rows.clear();
        _subs.clear();
        _rows.push_back("Rename");
        _rows.push_back("Delete");

        const char* name = (_confirmIndex >= 0 && _confirmIndex < (int)_devSignals.size())
                           ? _devSignals[_confirmIndex].name : "Signal";
        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, name, "OPTS", hp::COL_FG);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Back");
        _drawRows();
    }

    void App09::_runSignalOptions()
    {
        if (_navList()) _drawRows();

        if (_device->button.A.pressed()) {
            if (_confirmIndex < 0 || _confirmIndex >= (int)_devSignals.size()) {
                _switchScene(IrScene::RemoteView, false);
                return;
            }
            if (_sel == 0) {
                strncpy(_editBuf, _devSignals[_confirmIndex].name, sizeof(_editBuf) - 1);
                _editBuf[sizeof(_editBuf) - 1] = '\0';
                _vkSel    = 0;
                _nameMode = IrNameMode::RenameSignal;
                _switchScene(IrScene::NameEditor);
            } else {
                _confirmKind = IrConfirmKind::DeleteSignal;
                snprintf(_confirmLine, sizeof(_confirmLine), "Delete %s?",
                         _devSignals[_confirmIndex].name);
                _switchScene(IrScene::Confirm);
            }
            return;
        }
        if (_device->button.B.pressed()) _switchScene(IrScene::RemoteView, false);
    }

    /* ════════════════════════════════════════════════════════════
     *  Confirm dialog
     * ════════════════════════════════════════════════════════════ */

    void App09::_enterConfirm()
    {
        auto& Lcd = _device->Lcd;
        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Confirm", "!", hp::COL_WARN);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[A]Yes", nullptr, nullptr, "[B]No");
        hp::drawDialog(Lcd, _confirmLine, nullptr, hp::COL_WARN);
    }

    void App09::_runConfirm()
    {
        if (_device->button.A.pressed()) {
            if (_confirmKind == IrConfirmKind::DeleteDevice) {
                const bool gone = irstore::removeDevice(_devPath);
                _confirmKind   = IrConfirmKind::None;
                _selRemoteList = 0;
                _switchScene(IrScene::RemoteList);
                _toast(gone ? "Deleted" : "Delete failed");
            } else if (_confirmKind == IrConfirmKind::DeleteSignal) {
                bool ok = true;
                if (_confirmIndex >= 0 && _confirmIndex < (int)_devSignals.size()) {
                    _devSignals.erase(_devSignals.begin() + _confirmIndex);
                    ok = irstore::writeFile(_devPath, _devSignals);
                    if (!ok) irstore::loadFile(_devPath, _devSignals);
                    /* Keep the highlight on a real button, not on the trailing
                     * "+ Learn new button" row, after the list shrank. */
                    if (!_devSignals.empty() && _selRemoteView >= (int)_devSignals.size())
                        _selRemoteView = (int)_devSignals.size() - 1;
                }
                _confirmKind = IrConfirmKind::None;
                _switchScene(IrScene::RemoteView);
                _toast(ok ? "Deleted" : "Write failed");
            } else {
                _switchScene(_prevScene, false);
            }
            return;
        }
        if (_device->button.B.pressed()) {
            IrConfirmKind k = _confirmKind;
            _confirmKind = IrConfirmKind::None;
            _switchScene(k == IrConfirmKind::DeleteDevice ? IrScene::DeviceOptions
                                                          : IrScene::SignalOptions, false);
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Universal (all brands)
     * ════════════════════════════════════════════════════════════ */

    void App09::_enterUniversalMenu()
    {
        auto& Lcd = _device->Lcd;
        if (!_repaintOnly) {
            _fileList.clear();
            irstore::listIrFiles(irstore::kUnivDir, _fileList);

            _rows.clear();
            _subs.clear();
            for (const auto& f : _fileList) {
                _rows.push_back(univDisplayName(f));
                _subs.push_back(f);
            }
        }

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, "Universal · all brands", nullptr);
        hp::clearContent(Lcd);

        if (_rows.empty()) {
            hp::drawFooter4(Lcd, nullptr, nullptr, nullptr, "[B]Back");
            _drawCentered("Universal library not on SD card",
                          "Copy  sd files/infrared/universal/",
                          "from the firmware repo to",
                          "SD:/infrared/universal/",
                          hp::COL_FG);
        } else {
            hp::drawFooter4(Lcd, "[^v]Move", "[A]Open", nullptr, "[B]Back");
            _sel = _selUnivMenu;
            _drawRows();
        }
    }

    void App09::_runUniversalMenu()
    {
        if (_rows.empty()) {
            if (_device->button.B.pressed()) _switchScene(IrScene::MainMenu);
            return;
        }

        if (_navList()) { _selUnivMenu = _sel; _drawRows(); }

        if (_device->button.A.pressed()) {
            auto& Lcd = _device->Lcd;
            _selUnivMenu = _sel;
            const String& file = _fileList[_sel];
            String stem = stemOf(file);

            snprintf(_univPath, sizeof(_univPath), "%s/%s", irstore::kUnivDir, file.c_str());
            strncpy(_univTitle, univDisplayName(file).c_str(), sizeof(_univTitle) - 1);
            _univTitle[sizeof(_univTitle) - 1] = '\0';
            _univCat = univCatIndex(stem.c_str());

            /* One chunked pass builds name → offsets; nothing else reads the
             * whole file, so memory stays bounded by one signal afterwards. */
            hp::drawLoadingBegin(Lcd, "Indexing library...", file.c_str());
            bool ok = irstore::buildIndex(_univPath, _univIndex, _irIndexTick, _device);
            /* _irIndexTick polled the buttons; discard the edges it latched so
             * a press made during indexing cannot fire a blast on entry. */
            _drainInput();
            if (!ok || _univIndex.all.empty()) {
                /* A long-press B abort is handled by onRunning on the next frame. */
                if (!_device->button.B.isLongPress())
                    _toast("Cannot read %s", file.c_str());
                _redraw();
                return;
            }
            _vbSel       = 0;
            _selUnivCat  = 0;
            _switchScene(IrScene::UniversalCat);
            return;
        }
        if (_device->button.B.pressed()) _switchScene(IrScene::MainMenu);
    }

    void App09::_enterUniversalCat()
    {
        auto& Lcd = _device->Lcd;
        if (!_repaintOnly) _univCloseFile();
        char title[48];
        snprintf(title, sizeof(title), "%s · all brands", _univTitle);

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, title, nullptr);
        hp::clearContent(Lcd);
        _rows.clear();
        _subs.clear();

        if (_univCat >= 0 && _univCat < kUnivKnownCount) {
            hp::drawFooter4(Lcd, "[^v<>]Move", "[A]Blast", nullptr, "[B]Back");
            const VBtnDef* btns = kVBtnsDefs[_univCat];
            int n = kVBtnsCount[_univCat];
            if (_univCat == kUnivCatTV) n++;                 /* + TV-B-Gone */
            if (_vbSel < 0 || _vbSel >= n) _vbSel = 0;
            for (int i = 0; i < n; i++) {
                int bx, by, bw, bh;
                _getVBtnRect(i, n, bx, by, bw, bh);
                const char* label = (_univCat == kUnivCatTV && i == n - 1)
                                    ? kTvbgLabel : btns[i].label;
                hp::drawVirtualButton(Lcd, bx, by, bw, bh, label, i == _vbSel);
            }
        } else {
            hp::drawFooter4(Lcd, "[^v]Move", "[A]Blast", nullptr, "[B]Back");
            for (const auto& n : _univIndex.names) _rows.push_back(n);
            _sel   = _selUnivCat;                             /* restore selection */
            _vbSel = _sel;
            _drawRows();
        }
    }

    void App09::_runUniversalCat()
    {
        auto& Lcd = _device->Lcd;
        const bool known = (_univCat >= 0 && _univCat < kUnivKnownCount);

        if (_device->button.B.pressed()) {
            _switchScene(IrScene::UniversalMenu, false);
            return;
        }

        int n = 0;
        if (known) {
            n = kVBtnsCount[_univCat];
            if (_univCat == kUnivCatTV) n++;
        } else {
            n = (int)_rows.size();
        }
        if (n <= 0) return;

        if (known) {
            const VBtnDef* btns = kVBtnsDefs[_univCat];
            int rows = (n + 1) / 2;
            int old  = _vbSel;
            int row  = _vbSel / 2;
            int col  = _vbSel % 2;
            if (_device->button.Up.pressed())    row = (row - 1 + rows) % rows;
            if (_device->button.Down.pressed())  row = (row + 1) % rows;
            if (_device->button.Left.pressed())  col = (col - 1 + 2) % 2;
            if (_device->button.Right.pressed()) col = (col + 1) % 2;
            int sel = row * 2 + col;
            if (sel >= n) sel = n - 1;
            if (sel != old) {
                _vbSel = sel;
                int bx, by, bw, bh;
                const char* lo = (_univCat == kUnivCatTV && old == n - 1) ? kTvbgLabel : btns[old].label;
                const char* ln = (_univCat == kUnivCatTV && sel == n - 1) ? kTvbgLabel : btns[sel].label;
                _getVBtnRect(old, n, bx, by, bw, bh);
                hp::drawVirtualButton(Lcd, bx, by, bw, bh, lo, false);
                _getVBtnRect(sel, n, bx, by, bw, bh);
                hp::drawVirtualButton(Lcd, bx, by, bw, bh, ln, true);
            }
        } else {
            if (_navList()) { _vbSel = _sel; _selUnivCat = _sel; _drawRows(); }
        }

        if (!_device->button.A.pressed()) return;

        /* Build the blast queue from the index — one entry parsed at a time. */
        _blastOffsets.clear();
        _blastIsTvbg = false;
        if (known && _univCat == kUnivCatTV && _vbSel == n - 1) {
            _blastOffsets = _univIndex.all;                  /* TV-B-Gone */
            _blastIsTvbg  = true;
            strncpy(_blastName, kTvbgLabel, sizeof(_blastName) - 1);
        } else {
            const char* label;
            int slot;
            if (known) {
                const VBtnDef& def = kVBtnsDefs[_univCat][_vbSel];
                label = def.label;
                slot  = univFindAlias(_univIndex, def);
            } else {
                label = _rows[_sel].c_str();
                slot  = _univIndex.find(label);
            }
            if (slot < 0) {
                _toast("No signals: %s", label);
                return;
            }
            _blastOffsets = _univIndex.offsets[slot];
            strncpy(_blastName, label, sizeof(_blastName) - 1);
        }
        _blastName[sizeof(_blastName) - 1] = '\0';
        _switchScene(IrScene::Blast);
    }

    /* ════════════════════════════════════════════════════════════
     *  Blast — iterate offsets, one signal in memory at a time
     * ════════════════════════════════════════════════════════════ */

    /* One line inside a 268 px popup: "12/815 brands · gap 1.0s". */
    void App09::_blastDetail(char* out, size_t n) const
    {
        const int total = (int)_blastOffsets.size();
        const char* gap = _blastIsTvbg ? "0.25"
                                       : ((_sweepGapMs == kGapFast) ? "0.25" : "1.0");
        if (_blastSkipped > 0)
            snprintf(out, n, "%d/%d · skip %d · gap %ss",
                     _blastPos, total, _blastSkipped, gap);
        else
            snprintf(out, n, "%d/%d brands · gap %ss", _blastPos, total, gap);
    }

    void App09::_enterBlast()
    {
        auto& Lcd = _device->Lcd;
        /* A real entry rearms the queue; a repaint (expiring toast) or a resume
         * from Identify only redraws, so a sweep is never restarted mid-flight. */
        if (!_repaintOnly && !_blastResume) {
            _blastPos     = 0;
            _blastSkipped = 0;
            _blastDone    = false;
            _blastDoneAt  = 0;
            _blastLast    = 0;
            _blastCursor  = 0;
        }
        _blastResume = false;

        if (_blastIsTvbg) hp::drawFooter4(Lcd, nullptr, nullptr, nullptr, "[B]Pause");
        else              hp::drawFooter4(Lcd, "[<>]Gap", nullptr, nullptr, "[B]Pause");
        if (!_blastDone) _device->led.setColor(WS2812B_Class::RED);

        const int total = (int)_blastOffsets.size();
        char detail[48];
        _blastDetail(detail, sizeof(detail));
        hp::drawProgressPopup(Lcd, _blastName, detail,
                              total > 0 ? (float)_blastPos / (float)total : 0.0f,
                              _blastDone ? hp::COL_FG : hp::COL_WARN);
    }

    void App09::_runBlast()
    {
        auto& Lcd = _device->Lcd;
        const int total = (int)_blastOffsets.size();

        if (_device->button.B.pressed()) {          /* pause -> Identify */
            if (_blastDone) {
                _device->led.off();
                _univCloseFile();
                _switchScene(IrScene::UniversalCat, false);
                return;
            }
            if (_blastPos == 0) {          /* nothing sent yet: plain stop */
                _device->led.off();
                _univCloseFile();
                _switchScene(IrScene::UniversalCat, false);
                return;
            }
            _blastCursor = _blastPos - 1;   /* the entry that was just sent */
            _switchScene(IrScene::Identify);
            return;
        }

        /* Left/Right retune the sweep while it runs; the choice sticks for the
         * rest of the app session. TV-B-Gone stays at its fixed 250 ms. */
        if (!_blastDone && !_blastIsTvbg &&
            (_device->button.Left.pressed() || _device->button.Right.pressed())) {
            _sweepGapMs = (_sweepGapMs == kGapSlow) ? kGapFast : kGapSlow;
            char detail[48];
            _blastDetail(detail, sizeof(detail));
            hp::drawProgressPopup(Lcd, _blastName, detail,
                                  total > 0 ? (float)_blastPos / (float)total : 0.0f,
                                  hp::COL_WARN);
        }

        if (_blastDone) {
            if (millis() - _blastDoneAt > 1200) {
                _device->led.off();
                _univCloseFile();
                _switchScene(IrScene::UniversalCat, false);
            }
            return;
        }

        if (_blastPos >= total) {
            _blastDone   = true;
            _blastDoneAt = millis();
            _device->led.setColor(WS2812B_Class::GREEN);
            hp::drawProgressPopup(Lcd, _blastName, "No more brands", 1.0f, hp::COL_FG);
            return;
        }

        const uint32_t gapMs = _blastIsTvbg ? kTvbgGapMs : _sweepGapMs;
        uint32_t now = millis();
        if (_blastLast != 0 && now - _blastLast < gapMs) return;
        _blastLast = now;

        if (_univRead(_blastOffsets[_blastPos], _lastSent)) {
            if (!_txSignal(_lastSent)) _blastSkipped++;
        } else {
            _blastSkipped++;
        }
        _blastCursor = _blastPos;
        _blastPos++;

        char detail[48];
        _blastDetail(detail, sizeof(detail));
        hp::drawProgressPopup(Lcd, _blastName, detail,
                              (float)_blastPos / (float)(total > 0 ? total : 1),
                              hp::COL_WARN);
    }

    /* One handle serves a whole sweep / Identify session: the alternative is
     * an SD open+close for every transmitted entry. */
    bool App09::_univRead(uint32_t offset, irstore::Signal& out, bool skipRawBody)
    {
        if (!_univFileOpen) {
            _univFile = SD_MMC.open(_univPath, FILE_READ);
            if (!_univFile) { out.reset(); return false; }
            _univFileOpen = true;
        }
        return irstore::readEntryAt(_univFile, offset, out, skipRawBody);
    }

    void App09::_univCloseFile()
    {
        if (_univFileOpen) {
            _univFile.close();
            _univFileOpen = false;
        }
    }

    /* ════════════════════════════════════════════════════════════
     *  Identify — the sweep is paused on one entry
     * ════════════════════════════════════════════════════════════ */

    void App09::_identifySummary(char* out, size_t n) const
    {
        if (_lastSent.isRaw) {
            snprintf(out, n, "RAW · %d samples", (int)_lastSent.raw.size());
            return;
        }
        const char* proto = (_lastSent.proto == irfc::Proto::Unknown && _lastSent.protoRaw[0])
                            ? _lastSent.protoRaw : irfc::protoName(_lastSent.proto);
        snprintf(out, n, "%s  A:%02lX  C:%02lX", proto,
                 (unsigned long)_lastSent.address, (unsigned long)_lastSent.command);
    }

    void App09::_enterIdentify()
    {
        auto& Lcd = _device->Lcd;
        _rows.clear();
        _subs.clear();
        for (int i = 0; i < kIdentCount; i++) _rows.push_back(kIdentItems[i]);

        char title[48];
        snprintf(title, sizeof(title), "%s · #%d/%d", _blastName,
                 _blastCursor + 1, (int)_blastOffsets.size());

        hp::drawChrome(Lcd);
        hp::drawHeader(Lcd, title, "HOLD", hp::COL_WARN);
        hp::clearContent(Lcd);
        hp::drawFooter4(Lcd, "[^v]Move", "[A]Select", nullptr, "[B]Stop");
        if (!_repaintOnly) _device->led.setColor(WS2812B_Class::YELLOW);

        char summary[48];
        _identifySummary(summary, sizeof(summary));
        Lcd.setFont(&fonts::efontCN_16);
        Lcd.setTextColor(hp::COL_ACCENT, hp::COL_BG);
        Lcd.setCursor(hp::PAD_X + 8, MENU_Y0 + 4);
        Lcd.print(summary);

        _drawIdentRows(Lcd, _sel);
    }

    /* Move the cursor and (optionally) transmit what it now points at. */
    void App09::_identifyGoto(int index, bool send)
    {
        const int total = (int)_blastOffsets.size();
        if (total <= 0) return;
        if (index < 0) index = 0;
        if (index >= total) index = total - 1;
        /* Commit the move only when the entry could actually be read, so a
         * failed read never leaves the cursor on a blank _lastSent. */
        irstore::Signal next;
        if (!_univRead(_blastOffsets[index], next)) {
            _toast("Read failed");
            _redraw();
            return;
        }
        _blastCursor = index;
        _lastSent    = std::move(next);
        if (send) _sendOrToast(_lastSent, "Sent");
        _redraw();
    }

    void App09::_runIdentify()
    {
        if (_navList()) _drawIdentRows(_device->Lcd, _sel);

        if (_device->button.A.pressed()) {
            switch (_sel) {
                case 0: _sendOrToast(_lastSent, "Sent");               break;
                case 1: _identifyGoto(_blastCursor - 1, true);         break;
                case 2: _identifyGoto(_blastCursor + 1, true);         break;
                case 3: {
                    _device->led.off();          /* the sweep is over for now */
                    _defaultDeviceName(_editBuf, sizeof(_editBuf));
                    _vkSel       = 0;
                    _nameMode    = IrNameMode::SaveAsDevice;
                    _nameFromDup = false;
                    _switchScene(IrScene::NameEditor);
                    break;
                }
                case 4:                                  /* resume from cursor */
                    _blastPos    = _blastCursor + 1;
                    _blastLast   = millis();
                    _blastResume = true;
                    _switchScene(IrScene::Blast, false);
                    break;
                default:
                    _device->led.off();
                    _univCloseFile();
                    _switchScene(IrScene::UniversalCat, false);
                    break;
            }
            return;
        }

        if (_device->button.B.pressed()) {
            _device->led.off();
            _univCloseFile();
            _switchScene(IrScene::UniversalCat, false);
        }
    }

    /* "TV_NEC_04" — category, protocol, address. */
    void App09::_defaultDeviceName(char* out, size_t n) const
    {
        char raw[64];
        if (_lastSent.isRaw) {
            snprintf(raw, sizeof(raw), "%s_RAW", _univTitle);
        } else {
            const char* proto = (_lastSent.proto == irfc::Proto::Unknown && _lastSent.protoRaw[0])
                                ? _lastSent.protoRaw : irfc::protoName(_lastSent.proto);
            snprintf(raw, sizeof(raw), "%s_%s_%02lX", _univTitle, proto,
                     (unsigned long)_lastSent.address);
        }
        irstore::sanitiseName(raw, out, n);
    }

    /* Collect every entry of the category file that shares the identified
     * entry's protocol AND address — in a brand-less library that is "the same
     * remote" — keeping the first occurrence of each button name. */
    void App09::_saveIdentifiedDevice(const char* stem)
    {
        auto& Lcd = _device->Lcd;
        char path[168];
        irstore::devicePath(stem, path, sizeof(path));
        if (SD_MMC.exists(path)) { _toast("Name exists"); return; }

        std::vector<irstore::Signal> out;

        /* Name it before anything else: the dedupe below matches on the name. */
        if (!_lastSent.name[0])
            strncpy(_lastSent.name, "BTN_1", sizeof(_lastSent.name) - 1);

        if (_lastSent.isRaw) {
            out.push_back(_lastSent);
            _univCloseFile();
            if (!irstore::writeFile(path, out)) { _toast("Save failed"); return; }
            _openDevice(stem, 0);
            _toast("RAW: only this button saved");
            return;
        }

        hp::drawLoadingBegin(Lcd, "Scanning library...", _univTitle);

        /* The confirmed entry goes in first, so it — not an earlier same-named
         * entry of the same (protocol, address) group carrying a different
         * command — is the one that keeps its button name. */
        out.push_back(_lastSent);

        bool aborted = false;
        const size_t total = _univIndex.all.size();
        for (size_t i = 0; i < total; i++) {
            if ((i & 0x0F) == 0) {
                _device->button.update();
                _device->button.tick();
                if (_device->button.B.isLongPress()) { aborted = true; break; }
                hp::drawLoadingTick(Lcd, (uint32_t)i);
            }
            irstore::Signal sig;
            if (!_univRead(_univIndex.all[i], sig, true))  continue;  /* skip raw bodies */
            if (sig.isRaw)                                continue;
            if (sig.proto   != _lastSent.proto)           continue;
            if (sig.address != _lastSent.address)         continue;
            /* Two different unmapped protocol names both parse to Unknown. */
            if (sig.proto == irfc::Proto::Unknown &&
                strcasecmp(sig.protoRaw, _lastSent.protoRaw) != 0) continue;
            if (irstore::findSignal(out, sig.name) >= 0)  continue;  /* first wins */
            out.push_back(std::move(sig));
        }
        _univCloseFile();
        _drainInput();

        if (aborted) {
            /* The abort press must not also trip the app-wide exit check. */
            _waitButtonsReleased();
            _redraw();
            return;
        }
        if (out.empty()) { _toast("Nothing to save"); _redraw(); return; }
        if (!irstore::writeFile(path, out)) { _toast("Save failed"); _redraw(); return; }

        _openDevice(stem, 0);
        _toast("Saved %d buttons", (int)out.size());
    }

    /* ════════════════════════════════════════════════════════════
     *  IR hardware
     * ════════════════════════════════════════════════════════════ */

    void App09::_startRx()
    {
        _stopRx();
        /* 2048 entries is the raw capture limit: an AC frame pair
         * (2 x ~850 timings) must fit inside the 50 ms idle timeout. */
        _irRecv = new IRrecv(HAL_PIN_IR_RX, 2048, 50, true);
        _irRecv->enableIRIn();
    }

    void App09::_stopRx()
    {
        if (_irRecv) {
            _irRecv->disableIRIn();
            delete _irRecv;
            _irRecv = nullptr;
        }
    }

    /* The single transmit funnel: every parsed signal leaves through here. */
    void App09::_txFrame(const irfc::TxFrame& f)
    {
        if (!_irSend) return;
        /* .ir files carry no RC5/RC6 toggle bit; like Flipper's encoder we flip
         * it on every transmission so receivers see distinct key presses
         * (protocol map §3.9/§3.11: RC5/RC5X bit 11, RC6 mode-0 bit 16). */
        const uint64_t rcToggle = _rcToggle ? 1 : 0;
        _rcToggle = !_rcToggle;
        /* Repeat counts follow Flipper's per-protocol minimum frame count
         * (protocol map §0.5): SIRC 3 frames = library default repeat 2,
         * Pioneer 2 frames = repeat 1, everything else a single frame. */
        switch (f.kind) {
            case irfc::TxKind::NEC:         _irSend->sendNEC(f.data, f.nbits);         break;
            case irfc::TxKind::SAMSUNG:     _irSend->sendSAMSUNG(f.data, f.nbits);     break;
            case irfc::TxKind::SONY:        _irSend->sendSony(f.data, f.nbits, kSonyMinRepeat); break;
            case irfc::TxKind::RC5:         _irSend->sendRC5(f.data | (rcToggle << 11), f.nbits); break;
            case irfc::TxKind::RC6:         _irSend->sendRC6(f.data | (rcToggle << 16), f.nbits); break;
            case irfc::TxKind::PANASONIC64: _irSend->sendPanasonic64(f.data, f.nbits); break;
            case irfc::TxKind::PIONEER:     _irSend->sendPioneer(f.data, f.nbits, 1);  break;
            case irfc::TxKind::SANYO_LC7461:
                _irSend->sendSanyoLC7461(f.data, f.nbits);                             break;
            case irfc::TxKind::GENERIC_RCA: {
                const irfc::GenericTiming& t = irfc::rcaTiming();
                _irSend->sendGeneric(t.hdrMark, t.hdrSpace,
                                     t.oneMark, t.oneSpace,
                                     t.zeroMark, t.zeroSpace,
                                     t.footerMark, t.gapUs,
                                     f.data, f.nbits, t.khz, t.msbFirst,
                                     (uint16_t)0, (uint8_t)33);
                break;
            }
            default: break;
        }
    }

    bool App09::_txSignal(const irstore::Signal& sig)
    {
        if (!_irSend) return false;

        if (sig.isRaw) {
            if (sig.raw.empty()) return false;
            /* enableIROut() accepts Hz as well as kHz, so the stored carrier
             * is passed through unrounded (36700 Hz would become 36 kHz). */
            uint32_t hz = sig.frequency ? sig.frequency : 38000;
            _irSend->sendRaw(sig.raw.data(), (uint16_t)sig.raw.size(), (uint16_t)hz);
            return true;
        }

        irfc::Flipper fl;
        fl.proto   = sig.proto;
        fl.address = sig.address;
        fl.command = sig.command;

        irfc::TxFrame frame;
        if (!irfc::toTx(fl, frame)) return false;   /* no codec mapping */
        _txFrame(frame);
        return true;
    }

    void App09::_sendOrToast(const irstore::Signal& sig, const char* sentLabel)
    {
        if (_txSignal(sig)) { _toast("%s", sentLabel); return; }
        if (sig.isRaw) { _toast("Empty signal"); return; }
        _toast("Unsupported: %s",
               (sig.proto == irfc::Proto::Unknown && sig.protoRaw[0])
                   ? sig.protoRaw : irfc::protoName(sig.proto));
    }
}  /* namespace MOONCAKE::APPS */
