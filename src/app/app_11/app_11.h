/**
 * @file app_11.h
 * @author Mingo
 * @brief App11 — AC Remote (synthesised air-conditioner frames through IRac).
 *        Flipper-style TUI on hp_ui chrome, same shape as App09/App10.
 *        Design contract: docs/app11-ac-remote.md
 * @version 1.0
 * @date 2026-09-06
 * @copyright Copyright (c) 2025
 */
#pragma once
#include <mooncake.h>
#include "../../bsp/devices.h"
#include <IRac.h>
#include <IRutils.h>
#include <SD_MMC.h>
#include <FS.h>
#include <vector>

#include "../app_common/hp_ui.h"
#include "ac_store.h"
#include "app_11_hitachi.h"

using namespace mooncake;

namespace MOONCAKE::APPS
{
    /* ── Scene IDs ── */
    enum class AcScene : uint8_t {
        DeviceList,
        DeviceOptions,
        ProtocolPicker,   /* curated brands + "All supported protocols (N)" */
        AllProtocols,     /* every IRac-supported decode_type_t, A-Z        */
        ModelPicker,
        NameEditor,
        Confirm,
        Control,
        Advanced,
    };

    /* What the shared virtual-keyboard scene is naming. */
    enum class AcNameMode : uint8_t { NewDevice, RenameDevice };

    /* Why the protocol picker was opened. */
    enum class AcPickMode : uint8_t { NewDevice, ChangeProtocol };

    /* ── App class ── */
    class App11 : public AppAbility {
    public:
        App11(DEVICES* device);
        void onOpen() override;
        void onRunning() override;
        void onClose() override;

    private:
        DEVICES* _device = nullptr;

        /* ── Scene plumbing (same discipline as App09) ── */
        AcScene _scene      = AcScene::DeviceList;
        AcScene _prevScene  = AcScene::DeviceList;
        bool    _sceneDirty = true;
        /* True while the pending repaint only wipes an expired toast; enter
         * handlers must skip side effects (rescanning the card) when set. */
        bool    _repaintOnly = false;

        void _switchScene(AcScene s, bool resetSel = true);
        void _redraw() { _sceneDirty = true; }
        /* Scene whose list model was last built. A repaint can be queued for
         * scene A while a switch to B is already pending, so _repaintOnly
         * alone is not proof that the model on screen belongs to this scene. */
        AcScene _builtScene = AcScene::DeviceList;
        bool    _needRebuild(AcScene s);

        /* ── Shared list model ── */
        std::vector<String> _rows;
        std::vector<String> _subs;      /* empty → single-line list */
        int  _sel       = 0;
        int  _scrollTop = 0;

        /* Per-list remembered selection (_sel is shared with option menus). */
        int  _selDeviceList = 0;
        int  _selPicker     = 0;
        int  _selAll        = 0;
        int  _selModel      = 0;
        int  _selAdvanced   = 0;

        int  _visibleRows() const;
        void _drawRows();
        bool _navList();

        /* ── Shared drawing helpers ── */
        void _drawCentered(const char* l1, const char* l2 = nullptr,
                           const char* l3 = nullptr, const char* l4 = nullptr,
                           uint16_t color = hp::COL_FG);
        void _toast(const char* fmt, ...);
        void _serviceToast();
        void _paintToast();          /* scene-aware banner position */

        char     _toastMsg[64] = {0};
        uint32_t _toastUntil   = 0;

        /* ── Scene handlers ── */
        void _enterDeviceList();    void _runDeviceList();
        void _enterDeviceOptions(); void _runDeviceOptions();
        void _enterProtocolPicker();void _runProtocolPicker();
        void _enterAllProtocols();  void _runAllProtocols();
        void _enterModelPicker();   void _runModelPicker();
        void _enterNameEditor();    void _runNameEditor();
        void _enterConfirm();       void _runConfirm();
        void _enterControl();       void _runControl();
        void _enterAdvanced();      void _runAdvanced();

        /* ── Control screen ── */
        void _drawStatusPanel();
        void _drawUnsupportedPanel();
        void _drawPad(int only = -1);     /* only >= 0 redraws one button */
        void _pressPad(int index);
        void _cycleMode();
        void _cycleFan();
        void _cycleSwing();
        void _stepTemp(int delta);

        /* ── Transmit ── */
        stdAc::state_t _toState() const;
        /* Lights the LED for the send. `key` names the pad that was pressed;
         * it only matters on the Hitachi 424-family path, where the frame
         * carries the key code and the unit ignores fields it did not ask
         * about (docs/app11-ac-remote.md section 5). */
        bool _sendState(achitachi::Key key);
        /* Sends, persists on success, and records the banner text. Returns
         * false when nothing left the emitter, so the caller can roll its edit
         * back instead of showing a value the machine never received. */
        bool _sendAndReport(const char* label,
                            achitachi::Key key = achitachi::Key::PowerMode);

        /* ── Storage (Arduino side; ac_store stays pure) ── */
        void _devicePath(const char* stem, char* out, size_t outSize) const;
        void _listDevices();
        bool _loadDevice(const char* stem);   /* false → _loadError says why */
        bool _saveDevice() const;
        bool _writeBody(const char* path, const char* body, size_t len) const;
        bool _removeDevice(const char* stem) const;
        void _protocolLabel(char* out, size_t outSize) const;
        void _refreshProtocol();      /* recompute _protoName and _protoOk */

        /* ── Protocol / model tables ── */
        void _buildCurated();
        void _buildAllProtocols();
        /* Model ids the picker offers: the values whose modelToStr() differs
         * from the protocol's own "no model" text. Empty for a protocol
         * without models, and the picker is then skipped entirely. */
        void _collectModels(decode_type_t protocol);
        void _applyPickedProtocol();      /* picker + model → device, save    */
        std::vector<int16_t> _models;

        std::vector<decode_type_t> _curated;   /* parallel to the picker rows */
        std::vector<decode_type_t> _allProtos; /* alphabetical                */
        decode_type_t _pickProtocol = decode_type_t::UNKNOWN;
        int16_t       _pickModel    = -1;
        AcPickMode    _pickMode     = AcPickMode::NewDevice;
        /* Picker the name editor returns to on [B]. */
        AcScene       _lastPicker   = AcScene::ProtocolPicker;
        /* Scene a finished "Change protocol" returns to: the device list when
         * it started in DeviceOptions, Control when the Control screen itself
         * offered it because the stored protocol could not be driven. */
        AcScene       _pickReturn   = AcScene::DeviceList;

        /* ── Current device ── */
        acstore::Device _dev;
        char  _devStem[40]   = {0};
        char  _protoName[32] = {0};   /* typeToString cache, no String in draw */
        char  _selectStem[40] = {0};  /* row to highlight when the list rebuilds */
        /* The file behind _dev was read in full. Without this a transient SD
         * failure would let a later save overwrite a good file with defaults. */
        bool  _devLoaded = false;
        /* strToDecodeType(protocol) resolves AND IRac can drive it. */
        bool  _protoOk   = false;
        const char* _loadError = nullptr;
        std::vector<String> _fileList;

        /* ── Name editor ── */
        AcNameMode _nameMode    = AcNameMode::NewDevice;
        char       _editBuf[32] = {0};
        int        _vkSel       = 0;
        int        _vkCol       = 0;

        /* ── Confirm dialog ── */
        bool _confirmDelete    = false;
        char _confirmLine[48]  = {0};

        /* ── Control pad ── */
        int  _padSel  = 0;
        int  _padCol  = 0;    /* column to return to from the wide bottom key */

        /* ── IR ── */
        IRac* _ac = nullptr;
        /* Live protocol object for the Hitachi 424 family; open only while a
         * device using one of those protocols is on the Control screen. */
        achitachi::Direct _hitachi;
        bool _hitachiDirect = false;   /* the open device uses that path */
        /* Seed both transmitters from the stored state, without sending. */
        void _openTransmitter();

        /* Swallow button edges latched while a blocking send held the CPU. */
        void _drainInput();
        /* Drop pending direction edges so none leaks into the next scene. */
        void _eatDirections(bool includeUpDown = true);
    };
}
