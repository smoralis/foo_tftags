// foo_tftags - publishes long titleformat scripts as global tag fields.
//
// Twenty slots. Each slot has a tag name and a titleformat script. The result of the script is
// exposed to every component that evaluates title formatting as %tf_<name>%, for example
//   slot name "albumartist"  ->  %tf_albumartist%
// Scripts may be written over several lines: line breaks and leading indentation are removed
// before compiling, and lines that start with // are dropped. Scripts may use the fields of other
// slots (%tf_artist% inside slot 3); a slot that (directly or indirectly) refers to itself
// evaluates to nothing instead of looping.
//
// Field names are read by foobar2000 at startup, so a changed tag name needs a restart.
// Script changes apply immediately.
//
// NOTE: keep this file pure ASCII (use \u / \x escapes). MSVC reads BOM-less
// sources in the ANSI code page unless /utf-8 is set, which garbles literals.
//
// Build: VS2022, x64, C++17, link foobar2000_SDK, foobar2000_component_client, pfc.

#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/helpers/helpers.h>
#include <foobar2000/SDK/coreDarkMode.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Comctl32.lib")

DECLARE_COMPONENT_VERSION("TF Tags", "0.1.0",
    "Publishes long titleformat scripts as global tag fields (%tf_<name>%).");
VALIDATE_COMPONENT_FILENAME("foo_tftags.dll");

namespace {

    constexpr size_t kSlots = 20;
    constexpr const char* kFieldPrefix = "tf_";

    GUID makeGuid(std::uint32_t data1, std::uint8_t last) {
        return GUID{ data1, 0x4f2a, 0x4c3b, { 0x9d, 0x71, 0x2a, 0x5c, 0x80, 0x6e, 0x11, last } };
    }

    const GUID guid_tftags_preferences = {
        0x2d9b5e10, 0x7c34, 0x4a68, { 0x8e, 0x15, 0x61, 0xb3, 0x0f, 0x9a, 0x27, 0xc4 }
    };

    struct SlotConfig {
        GUID nameGuid;
        GUID scriptGuid;
        cfg_string name;
        cfg_string script;
        SlotConfig(unsigned index, const char* defaultName, const char* defaultScript)
            : nameGuid(makeGuid(0x6f1c4a00u + index, 0x01)),
            scriptGuid(makeGuid(0x6f1c4b00u + index, 0x02)),
            name(nameGuid, defaultName),
            script(scriptGuid, defaultScript) {
        }
    };

    SlotConfig g_slots[kSlots] = {
        SlotConfig(0, "01", ""),
        SlotConfig(1, "02", ""),
        SlotConfig(2, "03", ""),
        SlotConfig(3, "04", ""),
        SlotConfig(4, "05", ""),
        SlotConfig(5, "06", ""),
        SlotConfig(6, "07", ""),
        SlotConfig(7, "08", ""),
        SlotConfig(8, "09", ""),
        SlotConfig(9, "10", ""),
        SlotConfig(10, "11", ""),
        SlotConfig(11, "12", ""),
        SlotConfig(12, "13", ""),
        SlotConfig(13, "14", ""),
        SlotConfig(14, "15", ""),
        SlotConfig(15, "16", ""),
        SlotConfig(16, "17", ""),
        SlotConfig(17, "18", ""),
        SlotConfig(18, "19", ""),
        SlotConfig(19, "20", "")
    };

    // ---------------------------------------------------------------- helpers

    std::string cleanName(const std::string& raw) {
        std::string out;
        for (unsigned char c : raw) {
            if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out += static_cast<char>(c);
        }
        if (out.rfind(kFieldPrefix, 0) == 0) out.erase(0, 3);   // the tf_ prefix is added automatically
        return out;
    }

    // Field name for a slot, e.g. "tf_albumartist". Empty or duplicate names get a slot number.
    std::string fieldNameFor(size_t index, const std::string* names /* kSlots entries, or null = saved */) {
        auto nameOf = [&](size_t i) {
            return cleanName(names ? names[i] : std::string(g_slots[i].name.get() ? g_slots[i].name.get() : ""));
            };
        std::string n = nameOf(index);
        if (n.empty()) n = "slot" + std::to_string(index + 1);
        for (size_t i = 0; i < index; ++i) {
            std::string other = nameOf(i);
            if (other.empty()) other = "slot" + std::to_string(i + 1);
            if (other == n) { n += "_" + std::to_string(index + 1); break; }
        }
        return std::string(kFieldPrefix) + n;
    }

    // Make a multi-line script into the one-liner foobar2000 compiles: drop whole-line //
    // comments, strip leading/trailing whitespace of every line, join the lines.
    std::string flattenScript(const std::string& text) {
        std::string out;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t first = 0;
            while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
            line.erase(0, first);
            if (!line.empty() && line.compare(0, 2, "//") != 0) out += line;
            if (end == text.size()) break;
            pos = end + 1;
        }
        return out;
    }

    // ---------------------------------------------------------------- dynamic (ICY) metadata overlay

    using MetaList = std::vector<std::pair<std::string, std::string>>;

    struct DynamicMetadata {
        std::mutex m;
        metadb_handle_ptr track;
        MetaList meta;
    };

    DynamicMetadata& dynamicMetadata() {
        static DynamicMetadata* d = new DynamicMetadata;
        return *d;
    }

    class dynamic_metadata_callback : public play_callback_static {
    public:
        unsigned get_flags() override {
            return flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_dynamic_info_track;
        }

        void on_playback_new_track(metadb_handle_ptr track) override {
            DynamicMetadata& d = dynamicMetadata();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.track = track;
                d.meta.clear();
            }
        }

        void on_playback_stop(play_control::t_stop_reason) override {
            DynamicMetadata& d = dynamicMetadata();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.track.release();
                d.meta.clear();
            }
        }

        void on_playback_dynamic_info_track(const file_info& info) override {
            MetaList meta;
            const t_size total = info.meta_get_count();
            for (t_size i = 0; i < total; ++i) {
                const t_size values = info.meta_enum_value_count(i);
                if (values == 0) continue;
                std::string name = info.meta_enum_name(i);
                for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                // Preserve all values in the same manner as normal metadata fields: the
                // first value is used by titleformat's %field% / $meta(field).
                meta.emplace_back(std::move(name), std::string(info.meta_enum_value(i, 0)));
            }
            DynamicMetadata& d = dynamicMetadata();
            {
                std::lock_guard<std::mutex> lock(d.m);
                d.meta = std::move(meta);
            }
        }

        void on_playback_starting(play_control::t_track_command, bool) override {}
        void on_playback_seek(double) override {}
        void on_playback_pause(bool) override {}
        void on_playback_edited(metadb_handle_ptr) override {}
        void on_playback_dynamic_info(const file_info&) override {}
        void on_playback_time(double) override {}
        void on_volume_change(float) override {}
    };

    play_callback_static_factory_t<dynamic_metadata_callback> g_dynamic_metadata_callback;

    bool dynamicOverlayFor(metadb_handle* handle, MetaList& out) {
        DynamicMetadata& d = dynamicMetadata();
        std::lock_guard<std::mutex> lock(d.m);
        if (!d.track.is_valid() || d.track.get_ptr() != handle) return false;
        out = d.meta;
        return true;
    }

    class dynamic_overlay_hook : public titleformat_hook {
    public:
        explicit dynamic_overlay_hook(MetaList items) : m_items(std::move(items)) {}

        bool process_field(titleformat_text_out* out, const char* name, t_size length,
            bool& found) override {
            if (length == 9 && _strnicmp(name, "isplaying", 9) == 0) {
                out->write(titleformat_inputtypes::meta, "1", 1);
                found = true;
                return true;
            }
            const std::string* value = find(std::string(name, length));
            if (!value) return false;
            out->write(titleformat_inputtypes::meta, value->c_str(), value->length());
            found = true;
            return true;
        }

        bool process_function(titleformat_text_out* out, const char* name, t_size length,
            titleformat_hook_function_params* params, bool& found) override {
            if (length != 4 || _strnicmp(name, "meta", 4) != 0 || params->get_param_count() < 1)
                return false;
            const char* field = nullptr;
            t_size fieldLength = 0;
            params->get_param(0, field, fieldLength);
            if (params->get_param_count() >= 2 && params->get_param_uint(1) != 0) return false;
            const std::string* value = find(std::string(field, fieldLength));
            if (!value) return false;
            out->write(titleformat_inputtypes::meta, value->c_str(), value->length());
            found = true;
            return true;
        }

    private:
        const std::string* find(const std::string& name) const {
            std::string key = name;
            for (auto& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            for (const auto& item : m_items) if (item.first == key) return &item.second;
            return nullptr;
        }
        MetaList m_items;
    };

    // ---------------------------------------------------------------- evaluation

    // Cache compiled titleformat objects only. The raw script is compared on every
    // request so preference edits take effect immediately; evaluated values are never cached.
    struct ScriptCache {
        std::mutex m;
        std::string raw;
        titleformat_object::ptr obj;
        bool init = false;
    };

    ScriptCache& scriptCache(size_t slot) {
        static ScriptCache* caches[kSlots] = {
            new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache,
            new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache,
            new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache,
            new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache, new ScriptCache
        };
        return *caches[slot];
    }

    titleformat_object::ptr compileScript(size_t slot) {
        const char* src = g_slots[slot].script.get();
        const std::string flat = flattenScript(src ? src : "");
        ScriptCache& c = scriptCache(slot);
        std::lock_guard<std::mutex> lock(c.m);
        if (!c.init || c.raw != flat) {
            titleformat_object::ptr obj;
            if (!flat.empty()) titleformat_compiler::get()->compile_safe(obj, flat.c_str());
            c.obj = obj;
            c.raw = flat;
            c.init = true;
        }
        return c.obj;
    }

    class script_cache_cleanup : public initquit {
    public:
        void on_quit() override {
            for (size_t i = 0; i < kSlots; ++i) {
                ScriptCache& c = scriptCache(i);
                std::lock_guard<std::mutex> lock(c.m);
                c.obj.release();
                c.init = false;
            }
        }
    };
    initquit_factory_t<script_cache_cleanup> g_script_cache_cleanup;

    // Evaluates one slot for one track. A slot that is already being evaluated on this thread
    // (a reference cycle) returns an empty string.
    std::string evalSlot(metadb_handle* h, size_t slot) {
        thread_local unsigned inProgress = 0;
        const unsigned bit = 1u << slot;
        if (inProgress & bit) return {};

        std::string value;
        titleformat_object::ptr script = compileScript(slot);
        if (script.is_valid()) {
            inProgress |= bit;
            pfc::string8 result;
            MetaList overlay;
            if (dynamicOverlayFor(h, overlay)) {
                dynamic_overlay_hook hook(std::move(overlay));
                h->format_title(&hook, result, script, nullptr);
            }
            else {
                h->format_title(nullptr, result, script, nullptr);
            }
            inProgress &= ~bit;
            value = result.c_str();
        }
        return value;
    }

    class tf_fields : public metadb_display_field_provider {
    public:
        t_uint32 get_field_count() override { return (t_uint32)kSlots; }

        void get_field_name(t_uint32 index, pfc::string_base& out) override {
            if (index >= kSlots) { out.reset(); return; }
            out.set_string(fieldNameFor(index, nullptr).c_str());
        }

        bool process_field(t_uint32 index, metadb_handle* handle, titleformat_text_out* out) override {
            if (index >= kSlots) return false;
            const std::string value = evalSlot(handle, index);
            if (value.empty()) return false;
            out->write(titleformat_inputtypes::meta, value.c_str(), value.length());
            return true;
        }
    };
    service_factory_single_t<tf_fields> g_tf_fields;

    // ---------------------------------------------------------------- preferences

    constexpr int ID_LIST = 2001;
    constexpr int ID_NAME = 2002;
    constexpr int ID_SCRIPT = 2003;
    constexpr int ID_TEST = 2004;
    constexpr int ID_RESULT = 2005;

    class tftags_preferences_instance : public preferences_page_instance {
    public:
        tftags_preferences_instance(HWND parent, preferences_page_callback::ptr callback)
            : m_callback(callback), m_wnd(nullptr), m_ready(false), m_current(0),
            m_list(nullptr), m_nameLabel(nullptr), m_hint(nullptr), m_scriptLabel(nullptr),
            m_name(nullptr), m_script(nullptr), m_test(nullptr), m_result(nullptr), m_note(nullptr),
            m_monoFont(nullptr), m_brDark(CreateSolidBrush(RGB(32, 32, 32))) {

            HINSTANCE hInstance = core_api::get_my_instance();
            WNDCLASSW wc{};
            if (!GetClassInfoW(hInstance, L"TfTagsPreferencesPage", &wc)) {
                wc.lpfnWndProc = WindowProc;
                wc.hInstance = hInstance;
                wc.lpszClassName = L"TfTagsPreferencesPage";
                wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                wc.hbrBackground = nullptr;
                RegisterClassW(&wc);
            }

            RECT rcParent{};
            GetClientRect(parent, &rcParent);
            const int initW = (rcParent.right > 0) ? rcParent.right : 640;
            const int initH = (rcParent.bottom > 0) ? rcParent.bottom : 520;

            m_wnd = CreateWindowExW(0, L"TfTagsPreferencesPage", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                0, 0, initW, initH, parent, nullptr, hInstance, this);
            if (!m_wnd) return;

            for (size_t i = 0; i < kSlots; ++i) {
                m_names[i] = g_slots[i].name.get() ? g_slots[i].name.get() : "";
                m_scripts[i] = g_slots[i].script.get() ? g_slots[i].script.get() : "";
            }

            m_monoFont = CreateFontW(-MulDiv(9, (int)dpiForWindow(m_wnd), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

            m_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), hInstance, nullptr);
            setFont(m_list, GetStockObject(DEFAULT_GUI_FONT));

            m_nameLabel = addLabel(L"Tag name (the field is always prefixed with tf_):");
            m_name = addEdit(ID_NAME, false);
            m_hint = addLabel(L"");
            m_scriptLabel = addLabel(L"Titleformat script (several lines are allowed):");
            m_script = addEdit(ID_SCRIPT, true);
            if (m_monoFont) setFont(m_script, m_monoFont);
            m_test = CreateWindowExW(0, L"BUTTON", L"Test on selected track", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TEST)), hInstance, nullptr);
            setFont(m_test, GetStockObject(DEFAULT_GUI_FONT));
            m_result = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RESULT)), hInstance, nullptr);
            setFont(m_result, GetStockObject(DEFAULT_GUI_FONT));
            m_note = addLabel(
                L"Scripts may use other slots (%tf_artist%); a slot that refers to itself evaluates to nothing.\n"
                L"Lines that start with // are ignored. Line breaks and indentation are removed before compiling.\n"
                L"Restart foobar2000 after changing a tag name; script changes apply at once.");

            for (size_t i = 0; i < kSlots; ++i) SendMessageW(m_list, LB_ADDSTRING, 0, 0);
            refreshList();
            SendMessageW(m_list, LB_SETCURSEL, 0, 0);
            loadCurrent();

            m_darkMode.AddDialogWithControls(m_wnd);
            m_ready = true;
            layoutControls(initW, initH);
        }

        ~tftags_preferences_instance() {
            if (m_monoFont) DeleteObject(m_monoFont);
            if (m_brDark) DeleteObject(m_brDark);
        }

        t_uint32 get_state() override {
            t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
            if (m_wnd && hasChanges()) state |= preferences_state::changed;
            return state;
        }

        fb2k::hwnd_t get_wnd() override { return m_wnd; }

        void apply() override {
            if (!m_wnd) return;
            storeCurrent();
            for (size_t i = 0; i < kSlots; ++i) {
                g_slots[i].name = cleanName(m_names[i]).c_str();
                g_slots[i].script = m_scripts[i].c_str();
                m_names[i] = cleanName(m_names[i]);
            }
            loadCurrent();
            refreshList();
            if (m_callback.is_valid()) m_callback->on_state_changed();
        }

        void reset() override {
            if (!m_wnd) return;
            for (size_t i = 0; i < kSlots; ++i) {
                std::string defaultName = std::to_string(i + 1);
                if (defaultName.size() < 2) defaultName.insert(defaultName.begin(), '0');
                m_names[i] = defaultName;
                m_scripts[i].clear();
            }
            loadCurrent();
            refreshList();
            if (m_callback.is_valid()) m_callback->on_state_changed();
        }

    private:
        static UINT dpiForWindow(HWND wnd) {
            using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
            static const auto fn = []() -> GetDpiForWindowFn {
                const HMODULE user32 = GetModuleHandleW(L"user32.dll");
                return user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
                }();
            if (fn && wnd) { const UINT dpi = fn(wnd); if (dpi) return dpi; }
            return 96;
        }
        int S(int v) const { return MulDiv(v, (int)dpiForWindow(m_wnd), 96); }

        static void setFont(HWND w, HGDIOBJ f) { SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE); }

        HWND addLabel(const wchar_t* text) const {
            HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                0, 0, 10, 10, m_wnd, nullptr, core_api::get_my_instance(), nullptr);
            setFont(c, GetStockObject(DEFAULT_GUI_FONT));
            return c;
        }

        HWND addEdit(int id, bool multiline) const {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
            style |= multiline ? (WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN) : ES_AUTOHSCROLL;
            HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style,
                0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), core_api::get_my_instance(), nullptr);
            setFont(c, GetStockObject(DEFAULT_GUI_FONT));
            return c;
        }

        static std::string readWindow(HWND w) {
            const int len = GetWindowTextLengthW(w);
            std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(w, wide.data(), len + 1);
            wide.resize(len);
            pfc::stringcvt::string_utf8_from_wide utf8(wide.c_str());
            return std::string(utf8.get_ptr());
        }

        static void writeWindow(HWND w, const std::string& s) {
            pfc::stringcvt::string_wide_from_utf8 wide(s.c_str());
            SetWindowTextW(w, wide.get_ptr());
        }

        void storeCurrent() {
            if (m_current >= kSlots) return;
            m_names[m_current] = readWindow(m_name);
            m_scripts[m_current] = readWindow(m_script);
        }

        void updateHint() {
            const std::string field = "Use in title formatting: %" + fieldNameFor(m_current, m_names) + "%";
            writeWindow(m_hint, field);
        }

        void loadCurrent() {
            m_loading = true;
            writeWindow(m_name, m_names[m_current]);
            writeWindow(m_script, m_scripts[m_current]);
            m_loading = false;
            updateHint();
        }

        void refreshList() {
            const int sel = (int)SendMessageW(m_list, LB_GETCURSEL, 0, 0);
            SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
            for (size_t i = 0; i < kSlots; ++i) {
                std::string slotNumber = std::to_string(i + 1);
                if (slotNumber.size() < 2) slotNumber.insert(slotNumber.begin(), '0');
                const std::string text = slotNumber + "   %" + fieldNameFor(i, m_names) + "%";
                pfc::stringcvt::string_wide_from_utf8 wide(text.c_str());
                SendMessageW(m_list, LB_DELETESTRING, i, 0);
                SendMessageW(m_list, LB_INSERTSTRING, i, reinterpret_cast<LPARAM>(wide.get_ptr()));
            }
            SendMessageW(m_list, LB_SETCURSEL, sel < 0 ? 0 : sel, 0);
            SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(m_list, nullptr, TRUE);
        }

        bool hasChanges() {
            storeCurrent();
            for (size_t i = 0; i < kSlots; ++i) {
                const char* n = g_slots[i].name.get();
                const char* s = g_slots[i].script.get();
                if (cleanName(m_names[i]) != cleanName(n ? n : "")) return true;
                if (m_scripts[i] != (s ? s : "")) return true;
            }
            return false;
        }

        void runTest() {
            storeCurrent();

            // Prefer the playing stream when it has live ICY metadata, even if a playlist
            // item is selected. Otherwise retain the usual selected-track test behavior.
            metadb_handle_ptr playing;
            playback_control::get()->get_now_playing(playing);
            MetaList overlay;
            const bool hasDynamic = !playing.is_empty() && dynamicOverlayFor(playing.get_ptr(), overlay);

            metadb_handle_ptr track;
            if (hasDynamic) {
                track = playing;
            }
            else {
                metadb_handle_list sel;
                playlist_manager::get()->activeplaylist_get_selected_items(sel);
                if (sel.get_count() > 0) track = sel[0];
                else track = playing;
            }
            if (track.is_empty()) {
                writeWindow(m_result, "Select a track in the playlist (or start playback) and try again.");
                return;
            }
            const std::string flat = flattenScript(m_scripts[m_current]);
            titleformat_object::ptr obj;
            if (flat.empty() || !titleformat_compiler::get()->compile(obj, flat.c_str())) {
                writeWindow(m_result, "(empty script or the script does not compile)");
                return;
            }
            pfc::string8 out;
            if (hasDynamic) {
                dynamic_overlay_hook hook(std::move(overlay));
                track->format_title(&hook, out, obj, nullptr);
            }
            else {
                track->format_title(nullptr, out, obj, nullptr);
            }
            writeWindow(m_result, out.is_empty() ? std::string("(empty result)") : std::string(out.c_str()));
        }

        void layoutControls(int w, int h) {
            if (!m_ready) return;
            const int margin = S(12), gap = S(8), listW = S(190), rowH = S(24), labelH = S(18);
            const int noteH = S(52), testH = S(26), resultH = S(56);
            const int rightX = margin + listW + gap;
            const int rightW = (std::max)(S(120), w - rightX - margin);
            auto place = [](HWND c, int x, int y, int cw, int ch) {
                SetWindowPos(c, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
                };
            const int bottom = h - margin;
            place(m_note, margin, bottom - noteH, w - 2 * margin, noteH);
            const int contentBottom = bottom - noteH - gap;
            place(m_list, margin, margin, listW, (std::max)(S(60), contentBottom - margin));

            int y = margin;
            place(m_nameLabel, rightX, y, rightW, labelH); y += labelH + S(2);
            place(m_name, rightX, y, (std::min)(rightW, S(260)), rowH);
            y += rowH + S(2);
            // Keep the generated titleformat field on its own full-width row.
            // Placing it beside the name edit clipped it on narrow Preferences pages.
            place(m_hint, rightX, y, rightW, labelH);
            y += labelH + gap;
            place(m_scriptLabel, rightX, y, rightW, labelH); y += labelH + S(2);
            const int lower = testH + gap + resultH;
            const int scriptH = (std::max)(S(80), contentBottom - y - lower - gap);
            place(m_script, rightX, y, rightW, scriptH); y += scriptH + gap;
            place(m_test, rightX, y, S(170), testH); y += testH + gap;
            place(m_result, rightX, y, rightW, (std::max)(S(30), contentBottom - y));
        }

        static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            auto* self = reinterpret_cast<tftags_preferences_instance*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
            if (msg == WM_NCCREATE) {
                auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                self = static_cast<tftags_preferences_instance*>(cs->lpCreateParams);
                SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (self) return self->handleMessage(wnd, msg, wp, lp);
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        LRESULT handleMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            const bool dark = m_darkMode;
            if (msg == WM_ERASEBKGND) {
                HDC dc = (HDC)wp;
                RECT rc{};
                GetClientRect(wnd, &rc);
                FillRect(dc, &rc, dark ? m_brDark : GetSysColorBrush(COLOR_BTNFACE));
                return 1;
            }
            if (msg == WM_CTLCOLORSTATIC) {
                HDC dc = (HDC)wp;
                if (dark) SetTextColor(dc, RGB(240, 240, 240));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<INT_PTR>(dark ? m_brDark : GetSysColorBrush(COLOR_BTNFACE));
            }
            if (wnd != m_wnd) return DefWindowProcW(wnd, msg, wp, lp);

            if (msg == WM_COMMAND) {
                const int id = LOWORD(wp);
                const int code = HIWORD(wp);
                if (id == ID_LIST && code == LBN_SELCHANGE) {
                    storeCurrent();
                    const int sel = (int)SendMessageW(m_list, LB_GETCURSEL, 0, 0);
                    if (sel >= 0) m_current = (size_t)sel;
                    loadCurrent();
                    return 0;
                }
                if (id == ID_TEST && code == BN_CLICKED) { runTest(); return 0; }
                if ((id == ID_NAME || id == ID_SCRIPT) && code == EN_CHANGE && m_ready && !m_loading) {
                    if (id == ID_NAME) {
                        storeCurrent();
                        updateHint();
                        refreshList();
                    }
                    if (m_callback.is_valid()) m_callback->on_state_changed();
                    return 0;
                }
            }
            if (msg == WM_SIZE) layoutControls(LOWORD(lp), HIWORD(lp));
            if (msg == WM_THEMECHANGED || msg == WM_SYSCOLORCHANGE || msg == WM_SETTINGCHANGE)
                InvalidateRect(m_wnd, nullptr, TRUE);
            if (msg == WM_DESTROY) m_wnd = nullptr;
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        preferences_page_callback::ptr m_callback;
        HWND m_wnd;
        bool m_ready;
        bool m_loading = false;
        size_t m_current;
        HWND m_list, m_nameLabel, m_hint, m_scriptLabel, m_name, m_script, m_test, m_result, m_note;
        HFONT m_monoFont;
        HBRUSH m_brDark;
        std::string m_names[kSlots];
        std::string m_scripts[kSlots];
        fb2k::CCoreDarkModeHooks m_darkMode;
    };

    class tftags_preferences_page : public preferences_page_v3 {
    public:
        const char* get_name() override { return "TF Tags"; }
        GUID get_guid() override { return guid_tftags_preferences; }
        GUID get_parent_guid() override { return preferences_page::guid_tools; }
        preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override {
            return new service_impl_t<tftags_preferences_instance>(reinterpret_cast<HWND>(parent), callback);
        }
    };
    preferences_page_factory_t<tftags_preferences_page> g_tftags_preferences_page;

} // namespace
