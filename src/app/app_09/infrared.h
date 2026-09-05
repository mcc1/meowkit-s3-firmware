/**
 * @file infrared.h
 * @author Mingo
 * @brief App09 — Infrared (Learn / Remotes / Universal all-brands)
 *        Flipper-style TUI on hp_ui chrome.
 *        Design contract: docs/app09-infrared-redesign.md
 * @version 2.0
 * @date 2026-09-05
 * @copyright Copyright (c) 2025
 */
#pragma once
#include <mooncake.h>
#include "../../bsp/devices.h"
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <SD_MMC.h>
#include <FS.h>
#include <vector>

#include "../app_common/hp_ui.h"
#include "ir_flipper_codec.h"
#include "ir_store.h"

using namespace mooncake;

namespace MOONCAKE::APPS
{
    /* ── Scene IDs ── */
    enum class IrScene : uint8_t {
        MainMenu,
        LearnWait,
        LearnResult,
        SaveTarget,
        NameEditor,
        DupResolve,
        RemoteList,
        DeviceOptions,
        RemoteView,
        SignalOptions,
        Confirm,
        UniversalMenu,
        UniversalCat,
        Blast,
        Identify,
    };

    /* What the shared virtual-keyboard scene is naming. */
    enum class IrNameMode : uint8_t {
        NewDevice,      /* Learn → Save… → + New device… */
        Button,         /* Learn → button name */
        RenameDevice,
        RenameSignal,
        SaveAsDevice,   /* Identify -> "Save as device..." */
    };

    /* What the shared confirm dialog will do on [A]. */
    enum class IrConfirmKind : uint8_t { None, DeleteDevice, DeleteSignal };

    /* ── App class ── */
    class App09 : public AppAbility {
    public:
        App09(DEVICES* device);
        void onOpen() override;
        void onRunning() override;
        void onClose() override;

    private:
        DEVICES* _device = nullptr;

        /* ── Scene stack ── */
        IrScene _scene      = IrScene::MainMenu;
        IrScene _prevScene  = IrScene::MainMenu;
        bool    _sceneDirty = true;

        void _switchScene(IrScene s, bool resetSel = true);
        void _redraw() { _sceneDirty = true; }

        /* True while the pending repaint is only refreshing pixels (a toast
         * expired). Enter handlers must skip side effects such as restarting
         * the receiver or rearming a blast when this is set. */
        bool _repaintOnly = false;

        /* ── Shared list model (every list scene fills these) ── */
        std::vector<String> _rows;
        std::vector<String> _subs;      /* empty → single-line list */
        int  _sel       = 0;
        int  _scrollTop = 0;

        /* Per-list remembered selection: _sel is shared with options menus and
         * dialogs, so each list restores its own highlight when re-entered. */
        int  _selRemoteList = 0;
        int  _selSaveTarget = 0;
        int  _selRemoteView = 0;
        int  _selUnivMenu   = 0;
        int  _selUnivCat    = 0;

        int  _visibleRows() const;
        void _drawRows();
        bool _navList();                /* Up/Down; true when the selection moved */

        /* ── Shared drawing helpers ── */
        void _drawCentered(const char* l1, const char* l2 = nullptr,
                           const char* l3 = nullptr, const char* l4 = nullptr,
                           uint16_t color = hp::COL_FG);
        void _toast(const char* fmt, ...);
        void _serviceToast();

        char     _toastMsg[64] = {0};
        uint32_t _toastUntil   = 0;

        /* ── Scene handlers ── */
        void _enterMainMenu();      void _runMainMenu();
        void _enterLearnWait();     void _runLearnWait();
        void _enterLearnResult();   void _runLearnResult();
        void _buildRawSummary(decode_type_t type, uint16_t bits);
        void _enterSaveTarget();    void _runSaveTarget();
        void _enterNameEditor();    void _runNameEditor();
        void _enterDupResolve();    void _runDupResolve();
        void _enterRemoteList();    void _runRemoteList();
        void _enterDeviceOptions(); void _runDeviceOptions();
        void _enterRemoteView();    void _runRemoteView();
        void _enterSignalOptions(); void _runSignalOptions();
        void _enterConfirm();       void _runConfirm();
        void _enterUniversalMenu(); void _runUniversalMenu();
        void _enterUniversalCat();  void _runUniversalCat();
        void _enterBlast();         void _runBlast();
        void _enterIdentify();      void _runIdentify();

        /* Identify / sweep helpers */
        void _blastDetail(char* out, size_t n) const;
        void _identifySummary(char* out, size_t n) const;
        void _identifyGoto(int index, bool send);
        void _saveIdentifiedDevice(const char* stem);
        /* One open handle for the whole sweep / Identify session. */
        bool _univRead(uint32_t offset, irstore::Signal& out, bool skipRawBody = false);
        void _univCloseFile();
        void _defaultDeviceName(char* out, size_t n) const;

        /* ── Flows ── */
        void _openDevice(const char* stem, int selectSignal = 0);
        void _startLearn(const char* deviceStem);   /* nullptr → no preselection */
        void _commitSave(bool replaceExisting);
        void _finishNameEditor();
        void _cancelNameEditor();
        /* Swallow button edges latched while a blocking scan held the CPU. */
        void _drainInput();
        /* Block until every button is physically released, so the long press
         * that aborted a scan cannot also be read as "exit the app". */
        void _waitButtonsReleased();

        /* ── IR hardware ──
         * IRrecv buffer = 2048 entries: that is the hard raw-capture limit,
         * i.e. the longest learnable capture is 2047 mark/space timings.
         * 1024 was not enough for air-conditioner remotes, whose 280-424 bit
         * frames are usually sent twice inside the 50 ms idle timeout and so
         * overflowed the buffer (the capture was then silently truncated). */
        IRrecv* _irRecv = nullptr;
        IRsend* _irSend = nullptr;
        bool    _rcToggle = false;      /* RC5/RC6 toggle bit, flipped per send */

        void _startRx();
        void _stopRx();
        void _txFrame(const irfc::TxFrame& frame);
        bool _txSignal(const irstore::Signal& sig);   /* false = no codec mapping */
        void _sendOrToast(const irstore::Signal& sig, const char* sentLabel);

        /* ── Learn state ── */
        irstore::Signal _learned;
        /* Longest form: "RAW (MITSUBISHI_HEAVY_152 424b) · 1234" */
        char            _learnSummary[48] = {0};
        /* Raw post-processing flags for the capture on screen (see
         * ir_raw_tools.h): AGC fade and period-preserving normalisation. */
        bool            _learnFaded     = false;
        uint32_t        _learnFadeMs    = 0;
        uint16_t        _learnNormUs    = 0;   /* 0 = not normalised */
        char            _learnDevice[40]  = {0};   /* preselected device stem, "" = none */
        bool            _learnReturnsToView = false;

        /* ── Save / name-editor state ── */
        IrNameMode _nameMode      = IrNameMode::Button;
        char       _editBuf[32]   = {0};
        int        _vkSel         = 0;
        int        _vkCol         = 0;   /* column to return to from the [OK] row */
        bool       _nameFromDup   = false;
        char       _saveDevStem[40] = {0};
        char       _saveBtnName[32] = {0};

        /* ── Current device ── */
        char                         _selectStem[40] = {0}; /* row to highlight next */
        char                         _devStem[40]  = {0};
        char                         _devPath[168] = {0};
        std::vector<irstore::Signal> _devSignals;
        std::vector<String>          _fileList;

        /* ── Confirm dialog ── */
        IrConfirmKind _confirmKind = IrConfirmKind::None;
        char          _confirmLine[48] = {0};
        int           _confirmIndex    = 0;

        /* ── Universal ── */
        irstore::Index _univIndex;
        char           _univPath[168] = {0};
        char           _univTitle[32] = {0};
        int            _univCat       = -1;      /* known-category index, -1 = generic */
        int            _vbSel         = 0;

        /* ── Blast ── */
        std::vector<uint32_t> _blastOffsets;
        char          _blastName[32] = {0};
        int           _blastPos      = 0;
        int           _blastSkipped  = 0;
        bool          _blastDone     = false;
        bool          _blastIsTvbg   = false;    /* TV-B-Gone sweep: fixed gap */
        bool          _blastResume   = false;    /* re-entry must not rearm */
        uint32_t      _blastDoneAt   = 0;
        uint32_t      _blastLast     = 0;
        static constexpr uint32_t kTvbgGapMs = 250;   /* TV-B-Gone, not adjustable */
        static constexpr uint32_t kGapSlow   = 1000;  /* sweep default */
        static constexpr uint32_t kGapFast   = 250;
        uint32_t      _sweepGapMs    = kGapSlow;  /* kept for the app session */

        /* ── Identify (sweep paused) ── */
        irstore::Signal _lastSent;               /* entry the cursor points at */
        int             _blastCursor = 0;
        File            _univFile;               /* category file, kept open */
        bool            _univFileOpen = false;

        /* Learn page icon cache (PNG bytes loaded from SD once) */
        uint8_t* _learnIconPng   = nullptr;
        size_t   _learnIconLen   = 0;
        bool     _learnIconTried = false;
    };
}
