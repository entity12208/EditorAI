// EditorAI overlay — the mod's primary UI (ImGui via gd-imgui-cocos).
// Two tabs: Chat (session list + conversations + the new-chat composer in
// one place) and Settings (full mod configuration, theme, OAuth).
// Opens with a user-bindable key on desktop (default E) or the floating
// bubble on touch devices; redrawn every frame, so nothing here can ever go
// stale. All inputs persist across restarts via Geode saved values.
#ifdef EDITORAI_HAS_IMGUI

#include "sessions.hpp"
#include <Geode/Geode.hpp>
#ifdef GEODE_IS_DESKTOP
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#endif
#include <imgui-cocos.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_set>
#include <cstring>
#include <unordered_map>

using namespace geode::prelude;

namespace {

// ── Theme ────────────────────────────────────────────────────────────────────
// User-tunable via Settings → Theme (persisted as hex in saved values).
// The non-themed semantic colors (dim/error/ok/warn) stay fixed.
ImVec4 COL_ACCENT {0.36f, 0.69f, 1.00f, 1.f};
ImVec4 COL_BG     {0.09f, 0.10f, 0.12f, 1.f};
ImVec4 COL_USER   {0.45f, 0.85f, 1.00f, 1.f};
ImVec4 COL_AI     {0.45f, 0.95f, 0.55f, 1.f};
ImVec4 COL_TOOL   {1.00f, 0.78f, 0.30f, 1.f};
constexpr ImVec4 COL_DIM  {0.62f, 0.62f, 0.66f, 1.f};
constexpr ImVec4 COL_ERR  {1.00f, 0.38f, 0.38f, 1.f};
constexpr ImVec4 COL_OK   {0.40f, 0.95f, 0.45f, 1.f};
constexpr ImVec4 COL_WARN {1.00f, 0.85f, 0.35f, 1.f};

struct ThemeSlot { const char* key; const char* label; ImVec4* col; ImVec4 def; };
ThemeSlot THEME_SLOTS[] = {
    {"ui-col-accent", "accent",      &COL_ACCENT, {0.36f, 0.69f, 1.00f, 1.f}},
    {"ui-col-bg",     "background",  &COL_BG,     {0.09f, 0.10f, 0.12f, 1.f}},
    {"ui-col-user",   "your messages", &COL_USER, {0.45f, 0.85f, 1.00f, 1.f}},
    {"ui-col-ai",     "AI messages", &COL_AI,     {0.45f, 0.95f, 0.55f, 1.f}},
    {"ui-col-tool",   "tool calls",  &COL_TOOL,   {1.00f, 0.78f, 0.30f, 1.f}},
};
bool g_themeLoaded = false;

std::string vecToHex(const ImVec4& v) {
    return fmt::format("{:02X}{:02X}{:02X}",
        (int)std::clamp(v.x * 255.f, 0.f, 255.f),
        (int)std::clamp(v.y * 255.f, 0.f, 255.f),
        (int)std::clamp(v.z * 255.f, 0.f, 255.f));
}
ImVec4 hexToVec(const std::string& hex, const ImVec4& def) {
    if (hex.size() != 6) return def;
    int v[3];
    for (int i = 0; i < 3; ++i) {
        auto r = geode::utils::numFromString<int>(hex.substr(i * 2, 2), 16);
        if (!r) return def;
        v[i] = r.unwrap();
    }
    return ImVec4(v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, 1.f);
}
void loadTheme() {
    for (auto& s : THEME_SLOTS)
        *s.col = hexToVec(editoraiGetSavedStr(s.key, vecToHex(s.def)), s.def);
    g_themeLoaded = true;
}

// Derive every ImGui chrome color from the two user choices (bg + accent)
// each frame — keeps the whole window coherent however they tint it.
void applyThemeFrame() {
    auto lift = [](const ImVec4& c, float d, float a = 1.f) {
        return ImVec4(std::min(c.x + d, 1.f), std::min(c.y + d, 1.f),
                      std::min(c.z + d, 1.f), a);
    };
    auto dim = [](const ImVec4& c, float f, float a = 1.f) {
        return ImVec4(c.x * f, c.y * f, c.z * f, a);
    };
    auto* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_WindowBg]       = ImVec4(COL_BG.x, COL_BG.y, COL_BG.z, 0.97f);
    c[ImGuiCol_ChildBg]        = lift(COL_BG, 0.02f, 0.60f);
    c[ImGuiCol_PopupBg]        = lift(COL_BG, 0.03f, 0.98f);
    c[ImGuiCol_FrameBg]        = lift(COL_BG, 0.07f);
    c[ImGuiCol_FrameBgHovered] = lift(COL_BG, 0.11f);
    c[ImGuiCol_FrameBgActive]  = lift(COL_BG, 0.14f);
    c[ImGuiCol_Button]         = lift(COL_BG, 0.09f);
    c[ImGuiCol_ButtonHovered]  = lift(COL_BG, 0.15f);
    c[ImGuiCol_ButtonActive]   = dim(COL_ACCENT, 0.55f);
    c[ImGuiCol_Header]         = dim(COL_ACCENT, 0.38f);
    c[ImGuiCol_HeaderHovered]  = dim(COL_ACCENT, 0.50f);
    c[ImGuiCol_HeaderActive]   = dim(COL_ACCENT, 0.60f);
    c[ImGuiCol_Tab]            = lift(COL_BG, 0.05f);
    c[ImGuiCol_TabHovered]     = dim(COL_ACCENT, 0.55f);
    c[ImGuiCol_TabSelected]    = dim(COL_ACCENT, 0.45f);
    c[ImGuiCol_TitleBgActive]  = dim(COL_ACCENT, 0.30f);
    c[ImGuiCol_CheckMark]      = COL_ACCENT;
    c[ImGuiCol_SliderGrab]     = COL_ACCENT;
    c[ImGuiCol_SliderGrabActive] = lift(COL_ACCENT, 0.15f);
}

// Every EditorAI ImGui text box opts into this callback. ImGui normally owns
// Ctrl+A and Shift-selection itself, but the game/backend can consume the
// physical keyboard event before the focused widget observes the modifier.
// The keyboard hook below mirrors modifiers into ImGui explicitly; this
// callback makes Select All deterministic for whichever field is active.
int textSelectionCallback(ImGuiInputTextCallbackData* data) {
    if (data && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_A, false))
        data->SelectAll();
    return 0;
}

constexpr ImGuiInputTextFlags TEXT_SELECTION_FLAGS =
    ImGuiInputTextFlags_CallbackAlways;

// ── State ────────────────────────────────────────────────────────────────────
struct OverlayState {
    bool panelOpen = false;
    int  selectedSessionId = -1;
    char chatInput[2048] = {};
    int  chatMode = 0;              // 0 edit, 1 plan, 2 chat
    bool composing = false;         // right pane shows the new-chat composer
    // Composer ("+ new chat")
    char genPrompt[4096] = {};
    int  genTarget = -1;            // -1 current, -2 new, >=0 local index
    bool genReplace = false;
    std::string genError;
    float genErrorTtl = 0.f;        // seconds the error line stays visible
    std::vector<LocalLevelInfo> levels;
    bool levelsFresh = false;
    char styleLevelId[24] = {};     // style = "levelID" reference level
    bool diffCustom  = false;       // "custom..." picked in difficulty combo
    bool styleCustom = false;       // "custom..." picked in style combo
    bool persistedLoaded = false;   // saved-value inputs loaded once
    // Generate with a deferred target (new level / picked level) creates its
    // session only once the editor opens — until a session with id beyond
    // this marker appears, the chat pane must not latch onto an OLD session.
    int  pendingSelectAfter = -1;
};
OverlayState g_st;
float g_animT = 0.f;                // panel open/close animation (0..1)
float g_persistTimer = 0.f;         // session-persist throttle
bool  g_keyCapture = false;            // Settings is waiting for a new hotkey
std::vector<int> g_keyCapturePending;  // keys collected so far this capture (<=3)
std::vector<int> g_toggleSeqCache;     // hotkey sequence, re-read at most 1x/s
double g_toggleSeqCacheAt = -1e9;      // last cache refresh (steady seconds)

// The panel hotkey is user-bindable (Settings → Controls). cocos keycodes
// mirror Windows VK codes, so names derive from ranges + a few specials.
std::string keyDisplayName(int k) {
    if (k >= 'A' && k <= 'Z') return std::string(1, (char)k);
    if (k >= '0' && k <= '9') return std::string(1, (char)k);
    if (k >= 112 && k <= 123) return fmt::format("F{}", k - 111);
    switch (k) {
        case 8:   return "Backspace";
        case 9:   return "Tab";
        case 13:  return "Enter";
        case 32:  return "Space";
        case 35:  return "End";
        case 36:  return "Home";
        case 45:  return "Insert";
        case 46:  return "Delete";
        case 192: return "`";
    }
    return fmt::format("key {}", k);
}
std::string keySeqDisplayName(const std::vector<int>& seq) {
    if (seq.empty()) return "(none)";
    std::string out;
    for (size_t i = 0; i < seq.size(); ++i) {
        if (i) out += " > ";
        out += keyDisplayName(seq[i]);
    }
    return out;
}
double nowSeconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// The panel toggle is a SEQUENCE of 1-3 keys pressed in a row (default: just
// E). Stored as three saved ints (ov-toggle-key / -key2 / -key3; 0 = unused).
// Cached (read per-keypress AND per-frame) and refreshed at most 1x/s; the
// capture path rewrites the cache directly so a rebind takes effect at once.
const std::vector<int>& overlayToggleSeq() {
    double now = nowSeconds();
    if (g_toggleSeqCache.empty() || now - g_toggleSeqCacheAt > 1.0) {
        g_toggleSeqCacheAt = now;
        g_toggleSeqCache.clear();
        int k1 = (int)editoraiGetSavedInt("ov-toggle-key",
                    (int)cocos2d::enumKeyCodes::KEY_E);
        int k2 = (int)editoraiGetSavedInt("ov-toggle-key2", 0);
        int k3 = (int)editoraiGetSavedInt("ov-toggle-key3", 0);
        if (k1 > 0) g_toggleSeqCache.push_back(k1);
        if (k2 > 0) g_toggleSeqCache.push_back(k2);
        if (k3 > 0) g_toggleSeqCache.push_back(k3);
        if (g_toggleSeqCache.empty())   // never leave it un-toggleable
            g_toggleSeqCache.push_back((int)cocos2d::enumKeyCodes::KEY_E);
    }
    return g_toggleSeqCache;
}
// Persist a captured sequence (1-3 keys) and refresh the cache immediately.
void commitToggleSeq(const std::vector<int>& seq) {
    editoraiSetSavedInt("ov-toggle-key",  seq.size() > 0 ? seq[0] : (int)cocos2d::enumKeyCodes::KEY_E);
    editoraiSetSavedInt("ov-toggle-key2", seq.size() > 1 ? seq[1] : 0);
    editoraiSetSavedInt("ov-toggle-key3", seq.size() > 2 ? seq[2] : 0);
    g_toggleSeqCache = seq.empty()
        ? std::vector<int>{ (int)cocos2d::enumKeyCodes::KEY_E } : seq;
    g_toggleSeqCacheAt = nowSeconds();
}

// Inputs the user typed survive restarts (Geode saved values).
void loadPersistedInputs() {
    snprintf(g_st.genPrompt, sizeof(g_st.genPrompt), "%s",
             editoraiGetSavedStr("ov-gen-prompt").c_str());
    snprintf(g_st.styleLevelId, sizeof(g_st.styleLevelId), "%s",
             editoraiGetSavedStr("ov-style-id").c_str());
    g_st.genReplace = editoraiGetSavedInt("ov-gen-replace", 0) != 0;
    g_st.chatMode   = (int)std::clamp<int64_t>(
        editoraiGetSavedInt("ov-chat-mode", 0), 0, 2);
    g_st.persistedLoaded = true;
}

// Mobile-style UI? True only on real touch devices (compile-time). The
// floating-bubble layout and the gameplay guards key off this.
bool uiMobile() {
#ifdef GEODE_IS_MOBILE
    return true;
#else
    return false;
#endif
}

ImVec4 stateColor(GenSession::State s) {
    switch (s) {
        case GenSession::State::Running:        return COL_WARN;
        case GenSession::State::AwaitingEditor: return COL_ACCENT;
        case GenSession::State::Staged:         return COL_OK;
        case GenSession::State::Done:           return COL_OK;
        case GenSession::State::Failed:         return COL_ERR;
    }
    return ImVec4(1, 1, 1, 1);
}

void tipIfHovered(const char* tip) {
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tip);
}

// ── Settings widgets (write-through to mod settings; every one has a tip) ───
void settingToggle(const char* label, const char* id, const char* tip = nullptr) {
    bool v = editoraiGetBool(id);
    if (ImGui::Checkbox(fmt::format("{}##{}", label, id).c_str(), &v))
        editoraiSetBool(id, v);
    tipIfHovered(tip);
}

void settingInt(const char* label, const char* id, int mn, int mx,
                const char* tip = nullptr, ImGuiSliderFlags flags = 0) {
    int v = (int)editoraiGetInt(id);
    ImGui::SetNextItemWidth(170.f);
    if (ImGui::SliderInt(fmt::format("{}##{}", label, id).c_str(), &v, mn, mx,
                         "%d", flags))
        editoraiSetInt(id, (int64_t)v);
    tipIfHovered(tip);
}

// Text settings keep a per-id edit buffer; the live value re-syncs whenever
// the field is not focused and writes back when editing ends — so external
// changes show up, but typing is never clobbered mid-keystroke.
// lastFrame: stamped when the field renders. The tab-bar cleanup only clears
// editing flags for fields NOT rendered this frame — settingText is used in
// BOTH tabs (composer's custom difficulty/style), and a blanket clear while
// the Chat tab renders one would clobber in-progress typing every frame.
struct TextBuf { std::array<char, 1024> buf{}; bool editing = false; int lastFrame = -1; };
std::unordered_map<std::string, TextBuf>& textBufs() {
    static std::unordered_map<std::string, TextBuf> b;
    return b;
}

// Flush staged ImGui text buffers into Geode settings. settingText() only
// writes back on IsItemDeactivatedAfterEdit, so a user who types an API key
// and immediately hits "Test connection" would otherwise test the OLD key
// (stale buffer → spurious HTTP 401). Call before any network probe that
// reads the key / URL / model from settings.
// Only flush fields rendered this frame: a hidden custom-model buffer must
// not clobber a freshly picked preset (preset pick hides the text field,
// leaving a stale buf behind).
void flushTextBufs() {
    int curFrame = ImGui::GetFrameCount();
    for (auto& [id, tb] : textBufs()) {
        if (tb.lastFrame != curFrame) continue;
        editoraiSetStr(id.c_str(), tb.buf.data());
    }
}

// Named BYOPAK profiles. Geode saved values live in this mod's local save
// directory; nothing here is synced or sent anywhere. A profile includes the
// key because endpoints commonly use different credentials, and loading a
// URL with the previous endpoint's key is both confusing and unsafe.
struct EndpointProfile {
    std::string name, url, model, auth, key;
};

std::vector<EndpointProfile> loadEndpointProfiles() {
    std::vector<EndpointProfile> out;
    auto raw = editoraiGetSavedStr("custom-endpoint-profiles", "");
    if (raw.empty()) return out;
    auto parsed = matjson::parse(raw);
    if (!parsed) return out;
    const auto root = parsed.unwrap();
    if (!root.isArray()) return out;
    for (size_t i = 0; i < root.size() && out.size() < 20; ++i) {
        const auto e = root[i];
        if (!e.isObject()) continue;
        EndpointProfile p;
        p.name  = e["name"].asString().unwrapOr("");
        p.url   = e["url"].asString().unwrapOr("");
        p.model = e["model"].asString().unwrapOr("");
        p.auth  = e["auth"].asString().unwrapOr("");
        p.key   = e["key"].asString().unwrapOr("");
        if (!p.name.empty() && !p.url.empty()) out.push_back(std::move(p));
    }
    return out;
}

void saveEndpointProfiles(const std::vector<EndpointProfile>& profiles) {
    auto arr = matjson::Value::array();
    for (auto& p : profiles) {
        auto e = matjson::Value::object();
        e["name"] = p.name; e["url"] = p.url; e["model"] = p.model;
        e["auth"] = p.auth; e["key"] = p.key;
        arr.push(std::move(e));
    }
    editoraiSetSavedStr("custom-endpoint-profiles", arr.dump());
}

void endpointProfilesWidget() {
    static std::string selected;
    static std::array<char, 64> newName{};
    auto profiles = loadEndpointProfiles();
    if (!selected.empty() && std::none_of(profiles.begin(), profiles.end(),
            [&](const EndpointProfile& p) { return p.name == selected; }))
        selected.clear();

    ImGui::TextColored(COL_DIM, "saved locally");
    ImGui::SetNextItemWidth(210.f);
    if (ImGui::BeginCombo("endpoint profile##custom-profiles",
                          selected.empty() ? "(none)" : selected.c_str())) {
        for (auto& p : profiles) {
            bool isSelected = selected == p.name;
            if (ImGui::Selectable(p.name.c_str(), isSelected)) selected = p.name;
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected.empty());
    if (ImGui::SmallButton("Load")) {
        for (auto& p : profiles) if (p.name == selected) {
            editoraiSetStr("custom-provider-name", p.name);
            editoraiSetStr("custom-provider-url", p.url);
            editoraiSetStr("custom-provider-model", p.model);
            editoraiSetStr("custom-provider-auth", p.auth);
            editoraiSetStr("custom-provider-api-key", p.key);
            for (auto& [id, tb] : textBufs()) tb.editing = false;
            break;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) {
        profiles.erase(std::remove_if(profiles.begin(), profiles.end(),
            [&](const EndpointProfile& p) { return p.name == selected; }), profiles.end());
        saveEndpointProfiles(profiles);
        selected.clear();
    }
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(150.f);
    ImGui::InputTextWithHint("##endpoint-profile-name", "profile name",
        newName.data(), newName.size(), TEXT_SELECTION_FLAGS, textSelectionCallback);
    ImGui::SameLine();
    if (ImGui::SmallButton("Save current")) {
        std::string name = newName.data();
        if (name.empty()) name = editoraiGetStr("custom-provider-name");
        std::string url = editoraiGetStr("custom-provider-url");
        if (!name.empty() && !url.empty()) {
            EndpointProfile current{name, url,
                editoraiGetStr("custom-provider-model"),
                editoraiGetStr("custom-provider-auth"),
                editoraiGetStr("custom-provider-api-key")};
            auto it = std::find_if(profiles.begin(), profiles.end(),
                [&](const EndpointProfile& p) { return p.name == name; });
            if (it == profiles.end()) {
                if (profiles.size() < 20) profiles.push_back(std::move(current));
            } else *it = std::move(current);
            saveEndpointProfiles(profiles);
            selected = name;
            newName.fill(0);
        }
    }
    tipIfHovered("Saves name, URL, model, auth template and key only in this "
                 "mod's local Geode save data. Up to 20 profiles.");
}

// autoBypass: pasting an API key or model ID often contains characters GD's
// own text inputs would reject — turn both bypass settings on automatically
// so nothing downstream mangles them.
void settingText(const char* label, const std::string& id,
                 const char* hint = "", bool secret = false,
                 const char* tip = nullptr, bool autoBypass = false) {
    auto& tb = textBufs()[id];
    tb.lastFrame = ImGui::GetFrameCount();
    if (!tb.editing) {
        std::string cur = editoraiGetStr(id.c_str());
        snprintf(tb.buf.data(), tb.buf.size(), "%s", cur.c_str());
    }
    // Per-field key reveal (masked fields can't be proofread by hand).
    // Defaults to hidden every restart; the buffer itself always holds the
    // real text, only the display is masked.
    static std::unordered_map<std::string, bool> revealed;
    bool show = secret && revealed[id];
    ImGui::SetNextItemWidth(std::min(280.f, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint(fmt::format("{}##{}", label, id).c_str(), hint,
        tb.buf.data(), tb.buf.size(), TEXT_SELECTION_FLAGS |
        (secret && !show ? ImGuiInputTextFlags_Password : ImGuiInputTextFlags_None),
        textSelectionCallback);
    tb.editing = ImGui::IsItemActive();
    tipIfHovered(tip);
    // Capture BEFORE the Show/Hide button below: afterwards "last item" is
    // the button, and this query would read the wrong widget.
    bool finished = ImGui::IsItemDeactivatedAfterEdit();
    if (secret) {
        ImGui::SameLine();
        if (ImGui::SmallButton(fmt::format("{}##show-{}",
                show ? "Hide" : "Show", id).c_str())) {
            revealed[id] = !show;
            // Force the buffer back through the widget so it redraws in the
            // new mode even while it keeps focus.
            tb.editing = false;
        }
        tipIfHovered("Reveal the key to proofread or repair it by hand.");
    }
    // Mobile keyboards (especially on iOS) often offer no paste action at
    // all, so every settings field gets its own Paste button that pulls the
    // system clipboard straight into the field.
    if (uiMobile()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(fmt::format("Paste##paste-{}", id).c_str())) {
            if (const char* clip = ImGui::GetClipboardText()) {
                std::string s = clip;
                auto a = s.find_first_not_of(" \t\r\n");
                if (a != std::string::npos) {
                    auto b = s.find_last_not_of(" \t\r\n");
                    s = s.substr(a, b - a + 1);
                    snprintf(tb.buf.data(), tb.buf.size(), "%s", s.c_str());
                    tb.editing = false;
                    editoraiSetStr(id.c_str(), tb.buf.data());
                    if (autoBypass && tb.buf[0]) {
                        editoraiSetBool("bypass-char-filter", true);
                        editoraiSetBool("bypass-char-limit", true);
                    }
                }
            }
        }
        tipIfHovered("Paste from the system clipboard.");
    }
    if (finished) {
        editoraiSetStr(id.c_str(), tb.buf.data());
        if (autoBypass && tb.buf[0]) {
            editoraiSetBool("bypass-char-filter", true);
            editoraiSetBool("bypass-char-limit", true);
        }
    }
}

void settingCombo(const char* label, const char* id,
                  const std::vector<const char*>& options,
                  const char* tip = nullptr) {
    std::string cur = editoraiGetStr(id);
    const char* preview = cur.empty() ? "(none)" : cur.c_str();
    ImGui::SetNextItemWidth(std::min(210.f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo(fmt::format("{}##{}", label, id).c_str(), preview)) {
        for (auto* opt : options) {
            bool sel = cur == opt;
            if (ImGui::Selectable(*opt ? opt : "(none)", sel))
                editoraiSetStr(id, opt);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    tipIfHovered(tip);
}

// ── Quality presets ───────────────────────────────────────────────────────────
// One-click profiles that set the refinement / vision / self-check / two-pass
// options together so a new user never has to understand each knob. Picking a
// profile writes the underlying settings immediately; the individual toggles
// below stay fully adjustable afterward.
void applyQualityProfile(const std::string& name) {
    if (name == "fast") {
        editoraiSetInt("refinement-rounds", 1);
        editoraiSetBool("refine-until-done", false);
        editoraiSetBool("two-pass-generation", false);
        editoraiSetBool("enable-vision", false);
        editoraiSetBool("enable-self-critique", false);
    } else if (name == "premium") {
        editoraiSetInt("refinement-rounds", 5);
        editoraiSetBool("refine-until-done", true);
        editoraiSetBool("two-pass-generation", true);
        editoraiSetBool("enable-vision", true);
        editoraiSetBool("enable-self-critique", true);
    } else {  // balanced — a user-selected quicker compromise
        editoraiSetInt("refinement-rounds", 3);
        editoraiSetBool("refine-until-done", true);
        editoraiSetBool("two-pass-generation", true);
        editoraiSetBool("enable-vision", true);
        editoraiSetBool("enable-self-critique", true);
    }
    editoraiSetStr("quality-profile", name);
}

bool qualityProfileMatches(const std::string& name) {
    int rounds = (int)editoraiGetInt("refinement-rounds");
    bool untilDone = editoraiGetBool("refine-until-done");
    bool twoPass = editoraiGetBool("two-pass-generation");
    bool vision = editoraiGetBool("enable-vision");
    bool critique = editoraiGetBool("enable-self-critique");
    if (name == "fast")
        return rounds == 1 && !untilDone && !twoPass && !vision && !critique;
    if (name == "premium")
        return rounds == 5 && untilDone && twoPass && vision && critique;
    return rounds == 3 && untilDone && twoPass && vision && critique;
}

// Combo + tip for the quality preset row.
void qualityProfileWidget() {
    static const std::vector<const char*> PROFILES = {"premium", "balanced", "fast"};
    std::string cur = editoraiGetStr("quality-profile");
    if (cur != "fast" && cur != "premium") cur = "balanced";
    if (!qualityProfileMatches(cur)) cur = "custom";
    const char* preview =
        cur == "fast"    ? "fast (cheaper)" :
        cur == "premium" ? "premium (best, default)" :
        cur == "custom"  ? "custom" : "balanced";
    ImGui::SetNextItemWidth(std::min(210.f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("quality preset##qprofile", preview)) {
        for (auto* p : PROFILES) {
            bool sel = cur == p;
            const char* lbl =
                std::strcmp(p, "fast") == 0 ? "fast (cheaper)" :
                std::strcmp(p, "premium") == 0 ? "premium (best, default)" :
                "balanced";
            if (ImGui::Selectable(lbl, sel)) applyQualityProfile(p);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    tipIfHovered("Premium does the most work. Choose a lighter preset only "
                 "when speed or cost matters more.");
}

// Getting-started hint: the ONE action still needed for the current provider,
// so a new user never has to read the whole settings page. Returns
// (message, ready) — ready=false renders it in the warning color.
std::pair<std::string, bool> setupHintFor(const std::string& p) {
    auto hasKey = [&](const char* keyId) {
        return !editoraiGetStr(keyId).empty();
    };
    if (p == "manual")
        return {"Manual: copy prompt, paste reply.", true};
    if (p == "ollama") {
        if (editoraiGetBool("use-platinum"))
            return {"Platinum selected. Pick a model and test it.", true};
        std::string det = editoraiDetectedBackends();
        if (det.find("ollama") != std::string::npos)
            return {"Ollama found. Pick a model and test it.", true};
        return {"No local Ollama found. Install it, run `ollama pull <model>`, "
                "or switch on Platinum above.", false};
    }
    if (p == "lm-studio" || p == "llama-cpp") {
        std::string det = editoraiDetectedBackends();
        if (det.find(p) != std::string::npos)
            return {p == "lm-studio" ? "LM Studio detected - set the model and "
                                       "press Test connection."
                                     : "llama.cpp detected - set the model and "
                                       "press Test connection.", true};
        return {p == "lm-studio" ? "LM Studio not detected - start its server "
                                   "(default port 1234), then Test connection."
                                 : "llama.cpp not detected - start its server "
                                   "(default port 8080), then Test connection.", false};
    }
    if (p == "custom") {
        bool haveUrl = !editoraiGetStr("custom-provider-url").empty();
        bool haveModel = !editoraiGetStr("custom-provider-model").empty();
        if (haveUrl && haveModel)
            return {"Endpoint ready. Test it.", true};
        return {"Enter an endpoint and model. Key optional.", false};
    }
    // Hosted providers — key check mirrors the *-api-key setting names.
    std::string keyId = p + "-api-key";
    if (hasKey(keyId.c_str()))
        return {"Key set. Test it.", true};
    return {"Paste your " + p + " API key below (or use 'Sign in with browser' "
            "where available).", false};
}


// Call IMMEDIATELY after an InputText holding a level ID: shows the user's
// saved (online) levels in a dropdown under the box. Typing an ID or a name
// narrows the list live; clicking an entry writes its ID into the buffer.
// Returns true the frame a level is picked.
bool savedLevelSuggest(char* buf, size_t bufSize) {
    // Never while the panel is fading out — this is a separate top-level
    // window, so the main panel's NoMouseInputs wouldn't cover it.
    if (!g_st.panelOpen) return false;
    static std::vector<SavedLevelInfo> s_cache;
    static double s_cacheAt = -1e9;
    static ImGuiID s_owner = 0;
    ImGuiID itemId = ImGui::GetItemID();
    bool active = ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) {
        s_owner = itemId;
        if (ImGui::GetTime() - s_cacheAt > 5.0) {
            s_cache = editoraiListSavedLevels();
            s_cacheAt = ImGui::GetTime();
        }
    }
    if (s_owner != itemId) return false;

    std::string q = buf;
    for (auto& ch : q) ch = (char)std::tolower((unsigned char)ch);
    std::vector<const SavedLevelInfo*> matches;
    for (auto& l : s_cache) {
        if (!q.empty()) {
            std::string name = l.name;
            for (auto& ch : name) ch = (char)std::tolower((unsigned char)ch);
            if (name.find(q) == std::string::npos &&
                std::to_string(l.levelId).rfind(q, 0) != 0)
                continue;
        }
        matches.push_back(&l);
        if (matches.size() >= 8) break;
    }
    if (matches.empty()) {
        if (!active) s_owner = 0;
        return false;
    }

    ImVec2 pos = ImGui::GetItemRectMin();
    pos.y += ImGui::GetItemRectSize().y + 2.f;
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(
        ImVec2(std::max(ImGui::GetItemRectSize().x, 260.f), 0.f));
    bool picked = false;
    bool winHovered = false;
    if (ImGui::Begin("##lvlsuggest", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(COL_DIM, "your saved levels:");
        for (auto* l : matches) {
            ImGui::PushID(l->levelId);
            if (ImGui::Selectable(
                    fmt::format("{}  ({})", l->name, l->levelId).c_str())) {
                snprintf(buf, bufSize, "%d", l->levelId);
                picked = true;
                s_owner = 0;
            }
            ImGui::PopID();
        }
        winHovered = ImGui::IsWindowHovered(
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
            ImGuiHoveredFlags_ChildWindows);
    }
    ImGui::End();
    if (!active && !winHovered && !picked) s_owner = 0;
    return picked;
}

// ── Markdown rendering ───────────────────────────────────────────────────────
// ImGui has no markdown renderer, so this is a small purpose-built one for
// exactly the subset LLMs emit: ATX headings, bullet/numbered lists, fenced
// code blocks, blockquotes, horizontal rules, and inline `code`/**bold**/
// *italic*. It is written to be safe on PARTIAL input (a stream can cut off
// mid-token) — every unterminated construct simply renders as plain text.
//
// Wrapping is done by hand instead of via TextWrapped because a single logical
// line mixes styles (bold spans etc.), and ImGui's wrapping applies per call.
namespace md {

// One styled run of text produced by the inline parser.
struct Span {
    std::string text;
    bool bold   = false;
    bool italic = false;
    bool code   = false;
};

// Split a line into styled spans. Unterminated markers are emitted literally,
// which is what makes this safe mid-stream.
std::vector<Span> inlineSpans(std::string_view line) {
    std::vector<Span> out;
    Span cur;
    bool bold = false, italic = false, code = false;
    auto flush = [&] {
        if (!cur.text.empty()) { out.push_back(cur); cur.text.clear(); }
    };
    auto restyle = [&] { cur.bold = bold; cur.italic = italic; cur.code = code; };
    restyle();
    size_t i = 0;
    while (i < line.size()) {
        // Inline code wins over emphasis (markdown rule) — inside a code span
        // asterisks are literal.
        if (line[i] == '`') {
            // Only open a code span if there is a closing backtick on this line.
            if (!code && line.find('`', i + 1) == std::string_view::npos) {
                cur.text += '`'; ++i; continue;
            }
            flush(); code = !code; restyle(); ++i; continue;
        }
        if (!code && line.compare(i, 2, "**") == 0) {
            if (!bold && line.find("**", i + 2) == std::string_view::npos) {
                cur.text += "**"; i += 2; continue;
            }
            flush(); bold = !bold; restyle(); i += 2; continue;
        }
        if (!code && (line[i] == '*' || line[i] == '_')) {
            char m = line[i];
            // Not emphasis when it's part of a word (snake_case identifiers).
            bool wordInner = i > 0 && (std::isalnum((unsigned char)line[i - 1]) ||
                                       line[i - 1] == '_');
            if (m == '_' && wordInner) { cur.text += m; ++i; continue; }
            if (!italic && line.find(m, i + 1) == std::string_view::npos) {
                cur.text += m; ++i; continue;
            }
            flush(); italic = !italic; restyle(); ++i; continue;
        }
        cur.text += line[i];
        ++i;
    }
    flush();
    return out;
}

// Draw one span run with manual word wrapping inside `wrapW`.
// `x0` is the left edge; the caller has already indented.
void drawSpans(const std::vector<Span>& spans, float wrapW,
               const ImVec4& baseCol)
{
    const ImVec4 codeCol {0.85f, 0.92f, 1.00f, 1.f};
    float startX = ImGui::GetCursorPosX();
    float lineLeft = wrapW;
    bool  atLineStart = true;

    auto newline = [&] {
        // NOTE: no ImGui::NewLine() here. After any Text* call ImGui has
        // already advanced the cursor to the next line (SameLine is what keeps
        // items together), so calling NewLine would insert a BLANK line and
        // double-space every wrapped paragraph. All we need is to restore the
        // left edge — Text resets CursorPos.x to the indent, which is not
        // necessarily our hanging-indent start.
        ImGui::SetCursorPosX(startX);
        lineLeft = wrapW;
        atLineStart = true;
    };

    for (auto& sp : spans) {
        // Split into words so wrapping can happen between them.
        size_t p = 0;
        while (p < sp.text.size()) {
            size_t sp1 = sp.text.find(' ', p);
            std::string word = sp.text.substr(
                p, sp1 == std::string::npos ? std::string::npos : sp1 - p);
            bool trailingSpace = sp1 != std::string::npos;
            p = (sp1 == std::string::npos) ? sp.text.size() : sp1 + 1;
            if (word.empty()) {
                if (trailingSpace && !atLineStart) {
                    float w = ImGui::CalcTextSize(" ").x;
                    if (lineLeft - w <= 0.f) { newline(); }
                    else {
                        ImGui::SameLine(0.f, 0.f);
                        ImGui::TextUnformatted(" ");
                        lineLeft -= w;
                    }
                }
                continue;
            }
            std::string draw = word + (trailingSpace ? " " : "");
            float w = ImGui::CalcTextSize(draw.c_str()).x;
            if (!atLineStart && w > lineLeft) newline();
            if (!atLineStart) ImGui::SameLine(0.f, 0.f);
            // Long unbreakable token (a URL or a base64 blob): hard-slice it
            // so it can never push the layout past the panel edge.
            if (w > wrapW && wrapW > 8.f) {
                std::string chunk;
                for (char ch : draw) {
                    chunk += ch;
                    if (ImGui::CalcTextSize(chunk.c_str()).x >= wrapW - 4.f) {
                        if (sp.code) {
                            ImGui::PushStyleColor(ImGuiCol_Text, codeCol);
                            ImGui::TextUnformatted(chunk.c_str());
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextColored(baseCol, "%s", chunk.c_str());
                        }
                        ImGui::SetCursorPosX(startX);
                        chunk.clear();
                    }
                }
                if (!chunk.empty()) {
                    if (sp.code) {
                        ImGui::PushStyleColor(ImGuiCol_Text, codeCol);
                        ImGui::TextUnformatted(chunk.c_str());
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::TextColored(baseCol, "%s", chunk.c_str());
                    }
                    lineLeft = wrapW - ImGui::CalcTextSize(chunk.c_str()).x;
                    atLineStart = false;
                } else {
                    lineLeft = wrapW;
                    atLineStart = true;
                }
                continue;
            }
            // Bold has no separate font here (one atlas), so it renders as a
            // brighter tint; italic dims slightly. Cheap, but readable.
            ImVec4 col = baseCol;
            if (sp.code) col = codeCol;
            else if (sp.bold) col = ImVec4(std::min(baseCol.x * 1.25f, 1.f),
                                           std::min(baseCol.y * 1.25f, 1.f),
                                           std::min(baseCol.z * 1.25f, 1.f), 1.f);
            else if (sp.italic) col = ImVec4(baseCol.x * 0.85f, baseCol.y * 0.85f,
                                             baseCol.z * 0.92f, 1.f);
            if (sp.code) {
                // Subtle plate behind inline code so it reads as code.
                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImVec2 sz  = ImGui::CalcTextSize(draw.c_str());
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(pos.x - 1.f, pos.y),
                    ImVec2(pos.x + sz.x + 1.f, pos.y + sz.y),
                    ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.09f)), 2.f);
            }
            ImGui::TextColored(col, "%s", draw.c_str());
            lineLeft -= w;
            atLineStart = false;
            if (lineLeft <= 0.f) newline();
        }
    }
    // Nothing to close out: the final Text call already ended the line.
}

// Render a whole markdown document (or a partial one, mid-stream).
void render(std::string_view text, const ImVec4& baseCol, float wrapW) {
    if (wrapW < 40.f) wrapW = 40.f;
    bool inFence = false;
    std::string fenceLang;
    std::string codeAcc;
    float baseX = ImGui::GetCursorPosX();
    int fenceIdx = 0;   // distinct child IDs when a message has several blocks

    auto flushFence = [&] {
        if (codeAcc.empty() && fenceLang.empty()) return;
        // Code block: monospace-ish plate. One child so it scrolls its own
        // horizontal overflow instead of stretching the transcript.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.f, 1.f, 1.f, 0.05f));
        int lines = 1;
        for (char c : codeAcc) if (c == '\n') ++lines;
        float h = std::min((float)lines, 18.f) * ImGui::GetTextLineHeight()
                + ImGui::GetStyle().WindowPadding.y * 2.f;
        // EndChild is called unconditionally — required since ImGui 1.90 even
        // when BeginChild returns false (culled/collapsed).
        ImGui::BeginChild(ImGui::GetID(&codeAcc) + (ImGuiID)(++fenceIdx),
                          ImVec2(wrapW, h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        if (!fenceLang.empty())
            ImGui::TextColored(COL_DIM, "%s", fenceLang.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.94f, 0.82f, 1.f));
        ImGui::TextUnformatted(codeAcc.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        codeAcc.clear();
        fenceLang.clear();
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        std::string_view line = text.substr(
            pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        bool last = (eol == std::string_view::npos);
        pos = last ? text.size() + 1 : eol + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        // Fence toggling.
        std::string_view trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        if (trimmed.rfind("```", 0) == 0 || trimmed.rfind("~~~", 0) == 0) {
            if (inFence) { flushFence(); inFence = false; }
            else {
                inFence = true;
                fenceLang = std::string(trimmed.substr(3));
            }
            continue;
        }
        if (inFence) {
            codeAcc.append(line);
            codeAcc += '\n';
            if (last) flushFence();   // unterminated fence mid-stream
            continue;
        }

        if (trimmed.empty()) { ImGui::Spacing(); continue; }

        // Horizontal rule.
        if (trimmed.size() >= 3 &&
            (trimmed.find_first_not_of('-') == std::string_view::npos ||
             trimmed.find_first_not_of('*') == std::string_view::npos ||
             trimmed.find_first_not_of('_') == std::string_view::npos)) {
            ImGui::Separator();
            continue;
        }

        // Headings: bigger visual weight via the accent color + spacing.
        int hashes = 0;
        while (hashes < (int)trimmed.size() && trimmed[hashes] == '#') ++hashes;
        if (hashes > 0 && hashes <= 6 && hashes < (int)trimmed.size() &&
            trimmed[hashes] == ' ') {
            std::string_view body = trimmed.substr(hashes + 1);
            ImGui::Spacing();
            ImGui::SetCursorPosX(baseX);
            auto spans = inlineSpans(body);
            for (auto& s : spans) s.bold = true;
            drawSpans(spans, wrapW, COL_ACCENT);
            if (hashes <= 2) {
                // Underline for h1/h2 so structure reads at a glance.
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(p.x, p.y - 2.f), ImVec2(p.x + wrapW, p.y - 2.f),
                    ImGui::GetColorU32(ImVec4(COL_ACCENT.x, COL_ACCENT.y,
                                              COL_ACCENT.z, 0.35f)));
                ImGui::Spacing();
            }
            continue;
        }

        // Blockquote.
        if (trimmed.front() == '>') {
            std::string_view body = trimmed.substr(1);
            while (!body.empty() && body.front() == ' ') body.remove_prefix(1);
            ImVec2 top = ImGui::GetCursorScreenPos();
            ImGui::Indent(10.f);
            ImGui::SetCursorPosX(baseX + 10.f);
            drawSpans(inlineSpans(body), wrapW - 10.f, COL_DIM);
            ImGui::Unindent(10.f);
            float bot = ImGui::GetCursorScreenPos().y;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(top.x, top.y), ImVec2(top.x + 2.f, bot),
                ImGui::GetColorU32(COL_DIM), 1.f);
            continue;
        }

        // Bulleted / numbered list item.
        bool bullet = (trimmed.size() >= 2 &&
                       (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
                       trimmed[1] == ' ');
        size_t numLen = 0;
        while (numLen < trimmed.size() && std::isdigit((unsigned char)trimmed[numLen]))
            ++numLen;
        bool numbered = numLen > 0 && numLen + 1 < trimmed.size() &&
                        (trimmed[numLen] == '.' || trimmed[numLen] == ')') &&
                        trimmed[numLen + 1] == ' ';
        if (bullet || numbered) {
            std::string marker = bullet
                ? std::string("\u2022 ")
                : std::string(trimmed.substr(0, numLen + 1)) + " ";
            std::string_view body = trimmed.substr(bullet ? 2 : numLen + 2);
            ImGui::SetCursorPosX(baseX);
            ImGui::TextColored(COL_DIM, "%s", marker.c_str());
            ImGui::SameLine(0.f, 0.f);
            float markerW = ImGui::CalcTextSize(marker.c_str()).x;
            drawSpans(inlineSpans(body), std::max(wrapW - markerW, 40.f), baseCol);
            continue;
        }

        ImGui::SetCursorPosX(baseX);
        drawSpans(inlineSpans(trimmed), wrapW, baseCol);
    }
    if (inFence) flushFence();
}

} // namespace md

// (No example-level-IDs editor anymore: pinning reference level IDs by hand is
// gone. The AI finds references itself with the search_levels tool — ask for a
// style in words, e.g. "like Nine Circles", and it searches GD by title.)
bool isAllDigits(const char* s) {
    if (!*s) return false;
    for (; *s; ++s)
        if (*s < '0' || *s > '9') return false;
    return true;
}

// Model picker: a combo of presets PLUS a "custom..." entry. Picking a preset
// writes it immediately; picking "custom..." (or already holding a value that
// isn't a preset) reveals a text field so ANY model id can be typed. Gives
// every provider both quick presets and free custom entry.
void settingModelCombo(const char* id,
                       const std::vector<const char*>& presets,
                       const char* tip) {
    std::string cur = editoraiGetStr(id);
    bool curIsPreset = false;
    for (auto* opt : presets) if (cur == opt) { curIsPreset = true; break; }
    // "custom" sticky state: on once the value leaves the preset set, until a
    // preset is picked again. Per-setting so switching provider resets it.
    static std::unordered_map<std::string, bool> customMode;
    bool& custom = customMode[id];
    if (!cur.empty() && !curIsPreset) custom = true;

    const char* preview = custom ? "custom..."
                        : (cur.empty() ? "(none)" : cur.c_str());
    ImGui::SetNextItemWidth(210.f);
    if (ImGui::BeginCombo(fmt::format("model##{}", id).c_str(), preview)) {
        for (auto* opt : presets) {
            bool sel = !custom && cur == opt;
            if (ImGui::Selectable(opt, sel)) {
                editoraiSetStr(id, opt);
                custom = false;
                // Sync the text buffer immediately: otherwise the hidden
                // custom-model buffer still holds the old free-text value and
                // a later flushTextBufs() (Test connection) would undo this pick.
                auto& tb = textBufs()[id];
                snprintf(tb.buf.data(), tb.buf.size(), "%s", opt);
                tb.editing = false;
                tb.lastFrame = ImGui::GetFrameCount();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        if (ImGui::Selectable("custom...", custom)) custom = true;
        ImGui::EndCombo();
    }
    tipIfHovered(tip);
    if (custom)
        settingText("custom id", id, "type any model id", false,
                    "Any model id this provider accepts.", true);
}

// Ollama/Platinum model selector. A dropdown of the models the server
// actually has (fetched live) instead of a text box — with a loading state,
// an explicit "none available" message, and a Refresh button. Auto-fetches
// the first time it's shown. A custom-tag text field stays available for
// anyone pointing at a model the list doesn't surface.
void ollamaModelSelector() {
    static bool s_autoFetched = false;
    if (!s_autoFetched) { s_autoFetched = true; editoraiRefreshOllamaModels(); }

    bool platinum = editoraiGetBool("use-platinum");
    std::vector<std::string> models;
    int state = editoraiGetOllamaModels(models);   // 0 none,1 loading,2 empty,3 have,4 error
    std::string cur = editoraiGetStr("ollama-model");

    const char* preview = cur.empty() ? "(pick a model)" : cur.c_str();
    ImGui::SetNextItemWidth(210.f);
    if (ImGui::BeginCombo("model##ollama-model", preview)) {
        if (state == 3) {
            for (auto& m : models) {
                bool sel = (cur == m);
                if (ImGui::Selectable(m.c_str(), sel)) editoraiSetStr("ollama-model", m);
                if (sel) ImGui::SetItemDefaultFocus();
            }
        } else {
            ImGui::TextDisabled("%s",
                state == 1 ? "loading..." :
                state == 2 ? (platinum ? "no Platinum models available"
                                       : "no local models installed") :
                state == 4 ? "couldn't reach the server" :
                             "tap Refresh to load");
        }
        ImGui::EndCombo();
    }
    tipIfHovered("Which model to run. The list is fetched live from the "
                 "server - pick one, or type a custom tag below.");

    ImGui::SameLine();
    if (ImGui::SmallButton(state == 1 ? "..." : "Refresh"))
        editoraiRefreshOllamaModels();
    tipIfHovered("Re-fetch the model list from the server.");

    // Explicit empty/error line under the combo so it's visible without
    // opening the dropdown.
    if (state == 2)
        ImGui::TextColored(COL_WARN, platinum
            ? "No Platinum models available - the server may be down or idle."
            : "No local models - run `ollama pull <model>` first.");
    else if (state == 4)
        ImGui::TextColored(COL_ERR, "Couldn't reach the server. Check it's "
            "running, then Refresh.");

    // Custom tag escape hatch (auto-bypass on, like other model fields).
    settingText("custom tag", "ollama-model", "or type a model tag",
                false, "Use any tag the server has, even if it's not in the "
                "list above.", true);
}

// Model field per provider: preset combo + custom entry for hosted providers,
// free text for local/BYOPAK. Free-text model edits auto-enable char bypasses.
void providerModelWidget(const std::string& p) {
    const char* tip = "Which model this provider runs. Bigger = better "
                      "levels, slower and pricier. Pick 'custom...' to type "
                      "any model id.";
    if (p == "gemini")
        settingModelCombo("gemini-model",
            {"gemini-3.8-flash", "gemini-3.5-flash-lite", "gemini-3.1-pro", "gemma-4-31b-it"}, tip);
    else if (p == "claude")
        settingModelCombo("claude-model",
            {"claude-sonnet-5-5", "claude-opus-5-5", "claude-haiku-4-5"}, tip);
    else if (p == "openai")
        settingModelCombo("openai-model",
            {"gpt-6-astra", "gpt-6.1-sol", "gpt-6-luna"}, tip);
    else if (p == "ministral")
        settingModelCombo("ministral-model",
            {"ministral-3b-latest", "ministral-8b-latest", "mistral-small-latest",
             "mistral-medium-latest", "mistral-large-latest"}, tip);
    else if (p == "deepseek")
        settingModelCombo("deepseek-model",
            {"deepseek-chat", "deepseek-reasoner", "deepseek-coder"}, tip);
    else if (p == "groq")
        settingModelCombo("groq-model",
            {"openai/gpt-oss-120b", "openai/gpt-oss-20b",
             "qwen/qwen3.8-27b"}, tip);
    else if (p == "huggingface")
        settingModelCombo("huggingface-model",
            {"meta-llama/Llama-3.1-8B-Instruct", "Qwen/Qwen2.5-7B-Instruct"}, tip);
    else if (p == "openrouter")
        settingModelCombo("openrouter-model",
            {"google/gemini-3.8-flash", "google/gemini-3.5-flash-lite", "google/gemma-4-31b-it", "anthropic/claude-sonnet-5-5",
             "openai/gpt-6-astra", "meta-llama/llama-3.3-70b-instruct"}, tip);
    else if (p == "ollama")
        ollamaModelSelector();
    else if (p == "lm-studio")
        settingText("model", "lm-studio-model", "default", false, tip, true);
    else if (p == "llama-cpp")
        settingText("model", "llama-cpp-model", "default", false, tip, true);
    else if (p == "manual")
        ImGui::TextColored(COL_DIM, "Copy prompt, paste reply.");
    else
        settingText("model", "custom-provider-model", "model name", false, tip, true);
}

// ── Tab: Sessions ─────────────────────────────────────────────────────────────
// Width available for wrapped message bodies inside the transcript child.
float transcriptWrapW(float indent) {
    return std::max(ImGui::GetContentRegionAvail().x - indent - 6.f, 60.f);
}

// A "thinking" disclosure: the AI's reasoning, collapsed by default, rendered
// dim and in the same markdown as everything else. Used for both finished
// entries and the live stream.
//   forceOpen: nullptr = remember the user's click (finished entries).
//              non-null = drive the state every frame (the live block opens
//              while the model is only reasoning, then folds away once the
//              actual answer starts arriving).
void renderThinkingBlock(const char* label, const std::string& text,
                         const bool* forceOpen = nullptr)
{
    if (forceOpen) ImGui::SetNextItemOpen(*forceOpen);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
    bool open = ImGui::TreeNodeEx(label);
    ImGui::PopStyleColor();
    if (!open) return;
    md::render(text, COL_DIM, transcriptWrapW(ImGui::GetTreeNodeToLabelSpacing()));
    ImGui::TreePop();
}

void renderEntry(const GenSession::Entry& e, int idx) {
    using K = GenSession::Entry::Kind;
    ImGui::PushID(idx);
    switch (e.kind) {
        case K::User: case K::Assistant: {
            // Chat card: thin accent bar down the message's left edge —
            // role-colored, reads like a conversation instead of a log.
            const ImVec4& roleCol = e.kind == K::User ? COL_USER : COL_AI;
            ImGui::Spacing();
            ImVec2 barTop = ImGui::GetCursorScreenPos();
            ImGui::Indent(10.f);
            ImGui::TextColored(roleCol, e.kind == K::User ? "You" : "AI");
            // The AI writes markdown; the user's own text is shown verbatim.
            if (e.kind == K::Assistant)
                md::render(e.text, ImVec4(0.90f, 0.90f, 0.92f, 1.f),
                           transcriptWrapW(10.f));
            else
                ImGui::TextWrapped("%s", e.text.c_str());
            ImGui::Unindent(10.f);
            float barBot = ImGui::GetCursorScreenPos().y -
                           ImGui::GetStyle().ItemSpacing.y;
            if (barBot > barTop.y) {
                ImVec4 barCol = roleCol; barCol.w = 0.65f;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(barTop.x + 1.f, barTop.y),
                    ImVec2(barTop.x + 4.f, barBot),
                    ImGui::GetColorU32(barCol), 2.f);
            }
            break;
        }
        case K::Thinking:
            renderThinkingBlock("thinking", e.text);
            break;
        case K::ToolCall:
            ImGui::TextColored(COL_TOOL, "> %s", e.text.substr(0, 90).c_str());
            if (ImGui::IsItemHovered() && e.text.size() > 90)
                ImGui::SetTooltip("%s", e.text.c_str());
            break;
        case K::ToolResult:
            if (ImGui::TreeNode(fmt::format("result ({} chars)", e.text.size()).c_str())) {
                ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
                ImGui::TextWrapped("%s", e.text.c_str());
                ImGui::PopStyleColor();
                ImGui::TreePop();
            }
            break;
        case K::Status:
        {
            std::string brief = e.text;
            if (brief.size() > 140) {
                size_t cut = brief.find_first_of(".\n", 80);
                if (cut == std::string::npos || cut > 140) cut = 137;
                brief.resize(cut);
                brief += "...";
            }
            ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
            ImGui::TextWrapped("- %s", brief.c_str());
            ImGui::PopStyleColor();
            if (brief != e.text && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", e.text.c_str());
            break;
        }
        case K::Error:
            ImGui::TextColored(COL_ERR, "%s", e.text.c_str());
            break;
    }
    ImGui::PopID();
}

void composerBody(float dt);   // the "+ new chat" pane (defined below)

void tabChat(float dt) {
    auto& sessions = genSessions();
    // No sessions → the composer IS the view. Sync the flag (not just a
    // local) so the first session appearing from any source — copilot
    // included — doesn't silently flip the pane.
    if (sessions.empty()) g_st.composing = true;

    // A deferred-target Generate (new level / picked level) creates its
    // session only when the editor opens: jump to it the moment it exists,
    // and never latch onto an older session in the meantime.
    if (g_st.pendingSelectAfter >= 0) {
        for (int i = (int)sessions.size() - 1; i >= 0; --i) {
            if (sessions[i] && sessions[i]->id > g_st.pendingSelectAfter) {
                g_st.selectedSessionId  = sessions[i]->id;
                g_st.pendingSelectAfter = -1;
                g_st.composing = false;
                memset(g_st.chatInput, 0, sizeof(g_st.chatInput));
                break;
            }
        }
    }
    bool composing = g_st.composing;

    ImGui::BeginChild("list", ImVec2(210, 0), ImGuiChildFlags_None);
    {
        // New chat — selecting it swaps the right pane to the composer.
        ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
        if (ImGui::Selectable("+  new chat", composing))
            g_st.composing = true;
        ImGui::PopStyleColor();
        tipIfHovered("Describe a level and pick where it goes - the "
                     "conversation continues right here once it starts.");
        ImGui::Separator();
    }
    if (!sessions.empty()) {
        bool anyFinished = false;
        for (auto& s : sessions)
            if (s && (s->state == GenSession::State::Done ||
                      s->state == GenSession::State::Failed)) { anyFinished = true; break; }
        if (anyFinished) {
            if (ImGui::SmallButton("clear finished")) {
                auto& all = genSessions();
                all.erase(std::remove_if(all.begin(), all.end(), [](auto& s) {
                    return !s || s->state == GenSession::State::Done ||
                                 s->state == GenSession::State::Failed;
                }), all.end());
                editoraiMarkSessionsDirty();
            }
            tipIfHovered("Remove all finished/failed sessions from this list "
                         "(running ones stay).");
            ImGui::Separator();
        }
    }
    int64_t nowSecs = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (int i = (int)sessions.size() - 1; i >= 0; --i) {
        auto& s = sessions[i];
        if (!s) continue;
        ImGui::PushID(i);
        bool sel = !composing && g_st.selectedSessionId == s->id;
        std::string label = fmt::format("#{}  {}", s->id,
            s->title.empty() ? "(untitled)" : s->title);
        if (ImGui::Selectable(label.c_str(), sel)) {
            g_st.composing = false;
            g_st.pendingSelectAfter = -1;   // explicit pick beats auto-jump
            if (g_st.selectedSessionId != s->id) {
                g_st.selectedSessionId = s->id;
                memset(g_st.chatInput, 0, sizeof(g_st.chatInput));
            }
        }
        ImGui::Indent(14.f);
        {
            // Status dot + state text — a colored dot reads faster than
            // colored words.
            ImVec2 c = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(c.x + 4.f, c.y + ImGui::GetTextLineHeight() * 0.55f),
                3.5f, ImGui::GetColorU32(stateColor(s->state)));
            ImGui::Dummy(ImVec2(11.f, 0.f));
            ImGui::SameLine(0.f, 0.f);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, stateColor(s->state));
        ImGui::TextUnformatted(s->stateName());
        ImGui::PopStyleColor();
        if (s->startedAt > 0 && nowSecs > s->startedAt) {
            int64_t age = nowSecs - s->startedAt;
            std::string ageStr =
                age < 60      ? std::string("just now") :
                age < 3600    ? fmt::format("{}m ago", age / 60) :
                age < 86400   ? fmt::format("{}h ago", age / 3600) :
                                fmt::format("{}d ago", age / 86400);
            ImGui::SameLine();
            ImGui::TextColored(COL_DIM, "%s", ageStr.c_str());
        }
        if (s->restored) {
            ImGui::SameLine();
            ImGui::TextColored(COL_DIM, "(restored)");
        }
        if (!s->flowPhase.empty() && s->flowPhase != "idle") {
            ImGui::Indent(14.f);
            ImGui::TextColored(COL_DIM, "%s", s->flowPhase.c_str());
            ImGui::Unindent(14.f);
        }
        ImGui::Unindent(14.f);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Composer pane — "new chat" picked (or nothing exists yet).
    if (composing) {
        ImGui::BeginChild("composer", ImVec2(0, 0), ImGuiChildFlags_None);
        composerBody(dt);
        ImGui::EndChild();
        return;
    }

    // Selected session (auto-select newest when nothing valid is selected —
    // but never while a deferred Generate is waiting for ITS session).
    std::shared_ptr<GenSession> sel;
    for (auto& s : sessions)
        if (s && s->id == g_st.selectedSessionId) { sel = s; break; }
    if (!sel && g_st.pendingSelectAfter < 0)
        for (int i = (int)sessions.size() - 1; i >= 0; --i)
            if (sessions[i]) {
                sel = sessions[i];
                g_st.selectedSessionId = sel->id;
                // Auto-select is still a selection change — never let typed
                // text carry over to a different session.
                memset(g_st.chatInput, 0, sizeof(g_st.chatInput));
                break;
            }

    bool showRating = sel && sel->needsRating;
    // (No Share button anymore: with telemetry on, every output uploads
    // automatically — once at completion and again with the rating.)
    bool showRetry  = sel && !showRating && !sel->fbPrompt.empty() &&
                      (sel->state == GenSession::State::Done ||
                       sel->state == GenSession::State::Failed);

    ImGui::BeginChild("chatcol", ImVec2(0, 0), ImGuiChildFlags_None);
    float footerH = ImGui::GetFrameHeightWithSpacing() +
        ((showRating || showRetry)
            ? ImGui::GetFrameHeightWithSpacing() : 0.f);
    ImGui::BeginChild("transcript", ImVec2(0, -footerH), ImGuiChildFlags_Borders);
    if (sel) {
        bool pinBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f;
        // Render the newest 150 entries — full 400-entry transcripts cost
        // real CPU every frame and nobody scrolls that far back.
        size_t start = sel->transcript.size() > 150
            ? sel->transcript.size() - 150 : 0;
        if (start > 0)
            ImGui::TextColored(COL_DIM, "(%d older entries not shown)",
                               (int)start);
        for (size_t i = start; i < sel->transcript.size(); ++i)
            renderEntry(sel->transcript[i], (int)i);
        // Show an elapsed-time heartbeat while the complete response is
        // pending, so a slow model does not look like a frozen mod.
        if (sel->state == GenSession::State::Running && !sel->liveStatus.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(COL_DIM, "AI %s", sel->liveStatus.c_str());
        }
        if (pinBottom) ImGui::SetScrollHereY(1.0f);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
        ImGui::TextWrapped(g_st.pendingSelectAfter >= 0
            ? "Starting..."
            : "Select a session.");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    if (showRating) {
        ImGui::TextColored(COL_ACCENT, "Rate it:");
        tipIfHovered("Ratings teach the AI your taste - high-rated levels "
                     "become few-shot examples for future generations.");
        for (int r = 1; r <= 10; ++r) {
            ImGui::SameLine();
            ImGui::PushID(900 + r);
            if (ImGui::SmallButton(std::to_string(r).c_str()))
                editoraiRateSession(sel, r);
            ImGui::PopID();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("skip")) {
            sel->needsRating = false;
            editoraiMarkSessionsDirty();  // skip must survive restarts too
        }
    } else if (showRetry) {
        if (ImGui::SmallButton("retry with same prompt")) {
            snprintf(g_st.genPrompt, sizeof(g_st.genPrompt), "%s",
                     sel->fbPrompt.c_str());
            editoraiSetSavedStr("ov-gen-prompt", g_st.genPrompt);
            g_st.composing = true;
        }
        tipIfHovered("Copies this session's prompt into a new chat so you "
                     "can tweak and re-run it.");
    }

    // Export the full conversation to a text file (debugging / sharing).
    if (sel) {
        if (ImGui::SmallButton("export")) {
            std::string err;
            if (editoraiExportSession(sel, err))
                ImGui::SetTooltip("%s", err.c_str());
        }
        tipIfHovered("Writes this session's full transcript (prompts, AI "
                     "replies, tool calls, thinking) to session-<id>.txt in "
                     "the mod's save folder.");
        ImGui::SameLine();
    }

    // Chat mode: how the AI treats your next message.
    static const char* MODES[] = {"Edit", "Plan", "Chat"};
    ImGui::SetNextItemWidth(64.f);
    if (ImGui::Combo("##chatmode", &g_st.chatMode, MODES, 3))
        editoraiSetSavedInt("ov-chat-mode", g_st.chatMode);
    tipIfHovered("Edit: the AI changes the level.\n"
                 "Plan: it only writes a build plan - nothing is placed.\n"
                 "Chat: it just answers - no script, no changes.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 76.f);
    bool enter = ImGui::InputTextWithHint("##chat", "message the AI...",
        g_st.chatInput, sizeof(g_st.chatInput),
        ImGuiInputTextFlags_EnterReturnsTrue | TEXT_SELECTION_FLAGS,
        textSelectionCallback);
    ImGui::SameLine();
    if (sel && sel->state == GenSession::State::Running) {
        if (ImGui::Button("Cancel", ImVec2(68, 0)))
            editoraiCancelSession(sel);
        tipIfHovered("Stop this generation now.");
    } else {
        // No read-only sessions: an engineless (restored) session rebuilds
        // its AI context from the saved conversation on the next Send.
        bool can = sel && g_st.chatInput[0] != '\0';
        ImGui::BeginDisabled(!can);
        if ((ImGui::Button("Send", ImVec2(68, 0)) || enter) && can) {
            editoraiSendFollowUp(sel, g_st.chatInput, g_st.chatMode);
            memset(g_st.chatInput, 0, sizeof(g_st.chatInput));
        }
        ImGui::EndDisabled();
        if (sel && !sel->enginePtr)
            tipIfHovered("Restored session - sending rebuilds the AI context "
                         "from the saved conversation. Edit messages may ask "
                         "you to open the session's level first.");
        else
            tipIfHovered("Send a follow-up. The current mode (left) decides "
                         "whether it edits the level, plans, or just chats.");
    }
    ImGui::EndChild();
}

// ── New-chat composer (the right pane of the Chat tab) ──────────────────────
// Chat-first: the message box is the primary control and everything else is a
// compact strip under it. You type what you want and press Ctrl+Enter (or the
// send button); the knobs are there when you want them, not in your way.
void composerBody(float dt) {
    if (!g_st.levelsFresh) {
        g_st.levels = editoraiListLocalLevels();
        g_st.levelsFresh = true;
        if (g_st.genTarget >= (int)g_st.levels.size()) g_st.genTarget = -1;
    }

    bool manualProv = editoraiGetStr("ai-provider") == "manual";

    // ── The prompt IS the interface ─────────────────────────────────────────
    // Reserve room for the strip + button row below, and give the rest to the
    // message box so it feels like a chat composer rather than a form field.
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.f + 34.f;
    float inputH = std::max(ImGui::GetContentRegionAvail().y - footer, 90.f);
    // Ctrl+Enter sends; plain Enter inserts a newline (multi-line prompts are
    // common, so Enter must not fire the generation).
    bool sendNow = ImGui::InputTextMultiline("##prompt",
        g_st.genPrompt, sizeof(g_st.genPrompt), ImVec2(-1.f, inputH),
        ImGuiInputTextFlags_EnterReturnsTrue | TEXT_SELECTION_FLAGS,
        textSelectionCallback) &&
        ImGui::GetIO().KeyCtrl;
    bool typing = ImGui::IsItemActive();
    if (g_st.genPrompt[0] == '\0' && !typing) {
        // Hint drawn inside the empty box (InputTextMultiline has no hint API).
        ImVec2 r = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(r.x + 6.f, r.y + 5.f), ImGui::GetColorU32(COL_DIM),
            "Describe the level you want - theme, gamemodes, pacing, a song, "
            "a level to imitate...");
    }
    tipIfHovered("Plain words work best. Mention a BPM ('140 bpm') to sync to "
                 "beats, or name a level ('like Nine Circles') and the AI will "
                 "look it up. Ctrl+Enter sends.");
    if (ImGui::IsItemDeactivatedAfterEdit())
        editoraiSetSavedStr("ov-gen-prompt", g_st.genPrompt);

    // ── One-line context strip: where it goes + the knobs, inline ───────────
    std::string preview =
        g_st.genTarget == -1 ? "current editor" :
        g_st.genTarget == -2 ? "new level"      :
        (g_st.genTarget < (int)g_st.levels.size()
            ? g_st.levels[g_st.genTarget].name
            : "?");
    ImGui::TextColored(COL_DIM, "to");
    ImGui::SameLine(0.f, 4.f);
    ImGui::SetNextItemWidth(150.f);
    if (ImGui::BeginCombo("##target", preview.c_str())) {
        if (ImGui::Selectable("current editor", g_st.genTarget == -1))
            g_st.genTarget = -1;
        if (ImGui::Selectable("+ new level", g_st.genTarget == -2))
            g_st.genTarget = -2;
        if (!g_st.levels.empty()) ImGui::Separator();
        for (int i = 0; i < (int)g_st.levels.size(); ++i) {
            ImGui::PushID(i);
            std::string lbl = fmt::format("{} ({} obj)",
                g_st.levels[i].name, g_st.levels[i].objectCount);
            if (ImGui::Selectable(lbl.c_str(), g_st.genTarget == i))
                g_st.genTarget = i;
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Selectable("refresh list")) g_st.levelsFresh = false;
        ImGui::EndCombo();
    }
    tipIfHovered("Where the objects go: the level you're in, a brand-new "
                 "level, or any of your created levels.");
    if (g_st.genTarget != -2) {
        ImGui::SameLine();
        if (ImGui::Checkbox("replace", &g_st.genReplace))
            editoraiSetSavedInt("ov-gen-replace", g_st.genReplace ? 1 : 0);
        tipIfHovered("On: rebuild the level from scratch.\n"
                     "Off: add to what's already there.");
    }
    ImGui::SameLine();
    // Live summary of the knobs, click to expand.
    static bool s_optionsOpen = false;
    {
        std::string diff = editoraiGetStr("difficulty");
        std::string len  = editoraiGetStr("length");
        std::string sty  = editoraiGetStr("style");
        std::string sum  = fmt::format("{} | {} | {}",
            diff.empty() ? "?" : diff, len.empty() ? "?" : len,
            sty == "levelID" ? "reference" : (sty.empty() ? "?" : sty));
        if (ImGui::SmallButton(fmt::format("{}  {}",
                s_optionsOpen ? "v" : ">", sum).c_str()))
            s_optionsOpen = !s_optionsOpen;
        tipIfHovered("Difficulty, length and style. Click to change them.");
    }

    if (s_optionsOpen) {
        ImGui::Indent(8.f);
        // Difficulty: presets or a free-typed custom word.
        {
            static const std::vector<const char*> PRESETS =
                {"easy", "medium", "hard", "extreme"};
            std::string cur = editoraiGetStr("difficulty");
            bool isPreset = std::find_if(PRESETS.begin(), PRESETS.end(),
                [&](const char* p) { return cur == p; }) != PRESETS.end();
            ImGui::SetNextItemWidth(150.f);
            if (ImGui::BeginCombo("difficulty##diffsel",
                    (!isPreset || g_st.diffCustom) ? "custom..." : cur.c_str())) {
                for (auto* p : PRESETS)
                    if (ImGui::Selectable(p, cur == p)) {
                        editoraiSetStr("difficulty", p);
                        g_st.diffCustom = false;
                    }
                if (ImGui::Selectable("custom...", g_st.diffCustom || !isPreset))
                    g_st.diffCustom = true;
                ImGui::EndCombo();
            }
            tipIfHovered("How hard the level should be. 'custom...' lets you "
                         "type anything - e.g. 'insane demon' or 'chill auto'.");
            if (g_st.diffCustom || !isPreset) {
                ImGui::SameLine();
                settingText("##customdiff", "difficulty", "your difficulty",
                            false, "Free-form difficulty the AI aims for.");
            }
        }
        // Style: presets, a reference level, or a free-typed custom word.
        {
            static const std::vector<const char*> PRESETS =
                {"modern", "retro", "flow", "memory"};
            std::string cur = editoraiGetStr("style");
            bool isPreset = std::find_if(PRESETS.begin(), PRESETS.end(),
                [&](const char* p) { return cur == p; }) != PRESETS.end();
            bool isLevelId = cur == "levelID";
            ImGui::SetNextItemWidth(150.f);
            const char* stylePreview =
                isLevelId ? "levelID"
                          : ((g_st.styleCustom || !isPreset) ? "custom..." : cur.c_str());
            if (ImGui::BeginCombo("style##stylesel", stylePreview)) {
                for (auto* p : PRESETS)
                    if (ImGui::Selectable(p, cur == p)) {
                        editoraiSetStr("style", p);
                        g_st.styleCustom = false;
                    }
                if (ImGui::Selectable("levelID", isLevelId)) {
                    editoraiSetStr("style", "levelID");
                    g_st.styleCustom = false;
                }
                if (ImGui::Selectable("custom...", g_st.styleCustom))
                    g_st.styleCustom = true;
                ImGui::EndCombo();
            }
            tipIfHovered("Visual/gameplay style. 'levelID' copies the look of one "
                         "of your saved levels; 'custom...' takes any words. Or "
                         "just name a level in the prompt - the AI can search "
                         "for it itself.");
            if (isLevelId && !g_st.styleCustom) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(170.f);
                ImGui::InputTextWithHint("##stylelvl", "ID or saved-level name",
                    g_st.styleLevelId, sizeof(g_st.styleLevelId),
                    TEXT_SELECTION_FLAGS, textSelectionCallback);
                bool edited  = ImGui::IsItemDeactivatedAfterEdit();
                // Capture BEFORE the suggest window's Begin/End clobbers
                // LastItemData — else this tooltip can never fire.
                bool hovered = ImGui::IsItemHovered();
                if (savedLevelSuggest(g_st.styleLevelId, sizeof(g_st.styleLevelId))
                    || edited)
                    editoraiSetSavedStr("ov-style-id", g_st.styleLevelId);
                if (hovered)
                    ImGui::SetTooltip("Type to filter your saved levels, or pick "
                                      "from the dropdown. The AI downloads it "
                                      "and matches its style.");
            } else if (g_st.styleCustom || (!isPreset && !isLevelId)) {
                ImGui::SameLine();
                settingText("##customstyle", "style", "your style", false,
                            "Free-form style words the AI aims for.");
            } else if (g_st.styleLevelId[0] && !isLevelId) {
                // Style switched away — clear the reference so it can't silently
                // resurface on a later levelID generation.
                memset(g_st.styleLevelId, 0, sizeof(g_st.styleLevelId));
                editoraiSetSavedStr("ov-style-id", "");
            }
        }
        settingCombo("length", "length", {"short", "medium", "long", "xl", "xxl"},
            "Target level length. The mod enforces it - too-short drafts get "
            "extension rounds automatically.");
        {
            int v = (int)editoraiGetInt("target-object-count");
            ImGui::SetNextItemWidth(150.f);
            if (ImGui::InputInt("target objects##tgtobj", &v, 100, 1000))
                editoraiSetInt("target-object-count",
                               (int64_t)std::clamp(v, 0, 20000));
            tipIfHovered("Minimum total objects the level must reach - the AI "
                         "keeps building (densifying and decorating) until it "
                         "gets there. 0 = let the AI decide.");
        }
        ImGui::Unindent(8.f);
    }

    // ── Send row ────────────────────────────────────────────────────────────
    bool can = g_st.genPrompt[0] != '\0';
    ImGui::BeginDisabled(!can);
    // Send follows the user's accent color, not a hardcoded blue.
    ImGui::PushStyleColor(ImGuiCol_Button,
        ImVec4(COL_ACCENT.x * 0.55f, COL_ACCENT.y * 0.55f, COL_ACCENT.z * 0.55f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ImVec4(COL_ACCENT.x * 0.75f, COL_ACCENT.y * 0.75f, COL_ACCENT.z * 0.75f, 1.f));
    // Manual provider: "Send" becomes "Copy prompt for AI" (no network).
    const char* genLabel = manualProv ? "Copy prompt for AI" : "Send";
    bool clicked = ImGui::Button(genLabel, ImVec2(manualProv ? 180 : 110, 30));
    if ((clicked || (sendNow && can)) && can) {
        std::string err;
        bool replace = g_st.genTarget == -2 ? true : g_st.genReplace;
        std::string expectName = g_st.genTarget >= 0 &&
            g_st.genTarget < (int)g_st.levels.size()
                ? g_st.levels[g_st.genTarget].name : std::string();
        std::string prompt = g_st.genPrompt;
        // style = "levelID": route the reference through the existing
        // "style: <id>" prompt directive (the engine pulls it back out and
        // injects the downloaded level's style brief). Skip when the user
        // already typed their own "style:" directive — no duplicates.
        if (g_st.styleLevelId[0] && isAllDigits(g_st.styleLevelId) &&
            editoraiGetStr("style") == "levelID" &&
            prompt.find("style:") == std::string::npos)
            prompt += fmt::format("\nstyle: {}", g_st.styleLevelId);
        if (manualProv) {
            // Copy the full prompt to the clipboard for the current editor; a
            // "Build from clipboard" button appears below once it's pending.
            if (editoraiManualCopy(prompt, replace, err)) g_st.genError.clear();
            else { g_st.genError = err; g_st.genErrorTtl = 6.f; }
        } else {
            // Snapshot the newest session id BEFORE starting: the "Current
            // editor" target creates its session SYNCHRONOUSLY inside
            // editoraiStartGeneration, so computing this afterward would equal
            // the new session's id and the "id > pendingSelectAfter" jump would
            // never match — leaving the chat pane stuck on the placeholder.
            int maxIdBefore = 0;
            for (auto& s : genSessions()) if (s) maxIdBefore = std::max(maxIdBefore, s->id);
            if (editoraiStartGeneration(g_st.genTarget, prompt, replace,
                                        err, expectName)) {
                g_st.genError.clear();
                memset(g_st.genPrompt, 0, sizeof(g_st.genPrompt));
                editoraiSetSavedStr("ov-gen-prompt", "");
                // Select whatever session appears after the pre-call newest id —
                // works for both the synchronous current-editor path (already in
                // the list) and deferred targets (created when the editor opens).
                g_st.pendingSelectAfter = maxIdBefore;
                g_st.selectedSessionId  = -1;
                g_st.levelsFresh = false;          // a level was created/changed
                g_st.composing = false;            // jump to the live conversation
            } else {
                g_st.genError = err;
                g_st.genErrorTtl = 6.f;
            }
        }
    }
    ImGui::PopStyleColor(2);
    // Manual: once a prompt is copied, offer Build-from-clipboard.
    if (manualProv && editoraiManualPending()) {
        ImGui::SameLine();
        if (ImGui::Button("Build from clipboard", ImVec2(180, 30))) {
            std::string err;
            if (editoraiManualBuild(err)) {
                g_st.pendingSelectAfter = 0;
                g_st.selectedSessionId  = -1;
                g_st.composing = false;            // jump to the conversation / preview
            } else { g_st.genError = err; g_st.genErrorTtl = 6.f; }
        }
        tipIfHovered("Reads your AI's reply from the clipboard and builds the level.");
    }
    ImGui::EndDisabled();
    tipIfHovered("Start building. You can close this panel - or even the "
                 "editor - while it runs. (Ctrl+Enter also sends.)");
    ImGui::SameLine();
    if (!can) {
        ImGui::TextColored(COL_DIM, "describe the level first");
    } else {
        size_t plen = strlen(g_st.genPrompt);
        ImGui::TextColored(plen > 400 ? COL_WARN : COL_DIM,
            "%zu chars%s", plen,
            plen > 400 ? "  (long prompts rarely help)" : "");
    }

    if (!g_st.genError.empty()) {
        if (g_st.genErrorTtl > 0.f) {
            g_st.genErrorTtl -= dt;
            ImGui::TextColored(COL_ERR, "%s", g_st.genError.c_str());
        } else {
            g_st.genError.clear();  // keep state self-consistent post-TTL
        }
    }
}

// ── Tab: Settings ─────────────────────────────────────────────────────────────
void tabSettings() {
    static const std::vector<const char*> PROVIDERS = {
        "gemini", "claude", "openai", "openrouter", "ministral",
        "huggingface", "deepseek", "groq", "ollama", "lm-studio", "llama-cpp",
        "custom", "manual"};
    static const std::vector<const char*> SUB_PROVIDERS = {
        "same", "platinum", "gemini", "claude", "openai", "openrouter",
        "ministral", "huggingface", "deepseek", "groq", "ollama",
        "lm-studio", "llama-cpp", "custom"};

    ImGui::BeginChild("settingsScroll", ImVec2(0, 0));

    if (ImGui::CollapsingHeader("Provider", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Provider picker with local-backend auto-detect tags.
        {
            std::string detected = editoraiDetectedBackends();
            std::string curP = editoraiGetStr("ai-provider");
            const char* preview = curP.empty() ? "(none)" : curP.c_str();
            ImGui::SetNextItemWidth(210.f);
            if (ImGui::BeginCombo("provider##provider", preview)) {
                for (auto* opt : PROVIDERS) {
                    bool sel = curP == opt;
                    std::string label = opt;
                    if (detected.find(opt) != std::string::npos)
                        label += "  (detected)";
                    if (ImGui::Selectable(label.c_str(), sel))
                        editoraiSetStr("ai-provider", opt);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            tipIfHovered("Which AI service generates levels. Ollama runs "
                         "locally (free); Platinum runs on community machines "
                         "(free); the rest need an account with that provider. "
                         "Local servers found on this machine are tagged "
                         "(detected).");
        }
        std::string p = editoraiGetStr("ai-provider");
        {
            // Getting-started nudge: the single action still needed for this
            // provider, so setup is one glance instead of a settings maze.
            auto [hint, ready] = setupHintFor(p);
            if (!hint.empty()) {
                ImGui::TextColored(ready ? COL_DIM : COL_WARN, "%s", hint.c_str());
                tipIfHovered("The one thing left to do before you can generate.");
                ImGui::Spacing();
            }
        }
        providerModelWidget(p);
        {
            // Test connection — a real authenticated probe of the current
            // provider (the same endpoints the AI generation calls).
            ImGui::SameLine();
            if (ImGui::SmallButton("Test connection")) { flushTextBufs(); editoraiTestProvider(); }
            tipIfHovered("Sends a tiny authenticated request to this provider "
                         "to confirm the key/URL work before you generate.");
            std::string tstat = editoraiTestStatus();
            if (!tstat.empty()) {
                ImGui::SameLine();
                bool ok = tstat.rfind("✓", 0) == 0;
                ImGui::TextColored(ok ? COL_OK : COL_DIM, "%s", tstat.c_str());
            }
        }
        if (p == "ollama") {
            settingToggle("Use Platinum (community cloud)", "use-platinum",
                "Routes through the VLT GG-hosted volunteer network instead "
                "of localhost. Prompts run on donor machines - don't put "
                "personal info in them.");
            settingInt("timeout (s)", "ollama-timeout", 60, 1800,
                "How long to wait for the local/Platinum model before "
                "giving up. Big models on slow hardware need more.");
        } else if (p == "lm-studio") {
            settingText("server URL", "lm-studio-url", "http://localhost:1234",
                false, "Where your LM Studio server listens.", true);
        } else if (p == "llama-cpp") {
            settingText("server URL", "llama-cpp-url", "http://localhost:8080",
                false, "Where your llama.cpp server listens.", true);
        } else if (p == "custom") {
            endpointProfilesWidget();
            settingText("name", "custom-provider-name", "My Provider", false,
                "Display name for your endpoint.");
            settingText("endpoint URL", "custom-provider-url",
                "https://host/v1/chat/completions", false,
                "Any OpenAI-compatible chat-completions endpoint.", true);
            settingText("API key", "custom-provider-api-key", "", true,
                "Stored locally, only ever sent to YOUR endpoint.", true);
            settingText("auth header", "custom-provider-auth",
                "Authorization: Bearer ${KEY}", false,
                "How the key is attached. ${KEY} is replaced with it.");
        } else if (p == "manual") {
            ImGui::TextColored(COL_DIM, "Copy prompt, paste reply, then Build.");
        } else {
            settingText("API key", p + "-api-key", "paste key", true,
                "Stored locally on this device and only sent to the "
                "provider itself.", true);
        }
        if (p == "gemini")
            settingToggle("disable thinking (faster, shallower)", "disable-thinking",
                "Skips the thinking phase on Flash models. Pro models can't "
                "disable thinking and ignore this.");
        // One-click sign-in where the provider supports it — no key-copying.
        if (editoraiOAuthAvailable(p)) {
            ImGui::BeginDisabled(editoraiOAuthActive());
            if (ImGui::Button("Sign in with browser"))
                editoraiOAuthStart(p);
            ImGui::EndDisabled();
            tipIfHovered("Opens the provider's login page; the key/token is "
                         "fetched and saved automatically. No copy-pasting.");
            std::string st = editoraiOAuthStatus();
            if (!st.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(editoraiOAuthActive() ? COL_WARN : COL_DIM,
                                   "%s", st.c_str());
            }
        }
        ImGui::Separator();
        ImGui::TextColored(COL_DIM, "Assistant AI");
        settingCombo("assistant", "subagent-provider", SUB_PROVIDERS,
            "Same copies the main AI. Platinum uses the community network.");
        std::string subProvider = editoraiGetStr("subagent-provider");
        if (!subProvider.empty() && subProvider != "same" && subProvider != "platinum")
            settingText("assistant model", "subagent-model",
                "provider default", false, "Assistant model.", true);
    }

    if (ImGui::CollapsingHeader("Generation")) {
        // No max-objects setting: the AI sizes to the request. The user can
        // say "about 500 objects" in their prompt if they care.
        ImGui::TextColored(COL_DIM, "Name a reference level; the AI finds it.");
    }

    if (ImGui::CollapsingHeader("Placement & Editor")) {
        settingToggle("live block placement", "live-placement",
            "Blocks appear in the editor as the AI produces each round - "
            "you watch the level grow instead of waiting for the end. "
            "Accept/Deny still reviews the whole build when it finishes.");
        settingInt("spawn speed", "spawn-batch-size", 1, 100,
            "Ghost objects placed per tick while a blueprint appears. "
            "Higher = faster, choppier on weak devices.");
        settingInt("ground Y", "ai-ground-y", 15, 300,
            "The Y coordinate the AI treats as ground level (GD default 105).");
        settingInt("edit workload target", "edit-target-ops", 0, 5000,
            "When the AI reworks an existing level it must total at least "
            "this many edits (objects moved + deleted + restyled + added) - "
            "it is asked to continue until it gets there. Small follow-up "
            "tweaks (under 30 edits) stage immediately. 0 = off.");
    }

    if (ImGui::CollapsingHeader("AI Behavior")) {
        qualityProfileWidget();
        ImGui::Separator();
        settingToggle("AI tools (search, level fetch, analysis)", "enable-ai-tools",
            "Lets the AI call tools mid-generation: web search, downloading "
            "reference levels, physics simulation, passability checks, song "
            "BPM/waveform analysis, scratch memory, and self-set goals. The "
            "AI runs as many tool rounds as it needs - there is no limit.");
        settingToggle("show AI goal & tasks", "show-goal-tasks",
            "When the AI sets itself a goal (set_goal tool), mirror its goal "
            "and task checklist in the status line and session log.");
        settingToggle("refine until done", "refine-until-done",
            "Keep polishing until the AI itself says the level is finished, "
            "instead of a fixed number of passes. There is no pass limit; "
            "Cancel remains available throughout.");
        if (!editoraiGetBool("refine-until-done"))
            settingInt("refinement rounds", "refinement-rounds", 0, 10,
                "Extra self-review passes after the first draft. More rounds = "
                "better quality, more tokens.");
        settingToggle("triggers & colors", "enable-advanced-features",
            "Allows the AI to place triggers (move, color, pulse, camera...) "
            "and assign color channels / groups.");
        settingToggle("two-pass (structure, then decoration)", "two-pass-generation",
            "First pass builds gameplay, second pass decorates. Slower, "
            "usually prettier.");
        settingToggle("final self-check", "enable-self-critique",
            "The AI rates its own level 1-10 before staging and applies "
            "fixes for every issue it finds. One extra AI call.");
        settingToggle("vision (model sees the level)", "enable-vision",
            "Image-capable models (Claude, Gemini, GPT-4o, LLaVA...) get a "
            "rendered snapshot of the level on review and follow-up turns - "
            "they fix what they can SEE, not just coordinates.");
        settingToggle("AI playtest & watch", "ai-playtest",
            "Lets the AI call an unlimited repeatable playtest pass combining "
            "physics, passability, difficulty analysis and a fresh visual render. "
            "The real user playtest remains authoritative.");
        settingToggle("copilot mode (auto-fix while editing)", "copilot-mode",
            "While you edit, the AI watches for impossible sections and "
            "proposes fixes when the editor goes idle.");
    }

    if (ImGui::CollapsingHeader("Workflow")) {
        settingToggle("check for updates", "check-updates",
            "Once a day, check GitHub for a newer EditorAI release and offer "
            "a one-click install. Nothing installs without confirmation.");
        settingToggle("rate generations", "enable-rating",
            "Ask for a 1-10 rating after each generation. Ratings become "
            "few-shot examples that teach the AI your taste.");
        settingInt("feedback examples", "max-feedback-examples", 1, 10,
            "How many of your past rated generations are shown to the AI "
            "as examples each time.");
        settingToggle("save AI output to file", "show-ai-output",
            "Writes each raw AI response to the mod folder - useful for "
            "debugging weird generations.");
        settingToggle("rate limiting", "enable-rate-limiting",
            "Minimum delay between generations, so a double-click can't "
            "fire two API calls.");
        settingInt("rate limit (s)", "rate-limit-seconds", 1, 60,
            "Seconds between allowed generations.");
    }

    if (ImGui::CollapsingHeader("Privacy")) {
        settingToggle("auto-share generations (opt-in telemetry)", "allow-telemetry",
            "While ON, EVERY generation uploads automatically to the "
            "community training collector: prompt + settings + objects + "
            "ratings (yours and the AI's own). Never identity, never keys. "
            "Highly-rated levels train the free community models.");
    }

    if (ImGui::CollapsingHeader("Theme")) {
        ImGui::TextColored(COL_DIM, "Changes apply live.");
        for (auto& s : THEME_SLOTS) {
            float col[3] = {s.col->x, s.col->y, s.col->z};
            if (ImGui::ColorEdit3(fmt::format("{}##{}", s.label, s.key).c_str(),
                    col, ImGuiColorEditFlags_DisplayRGB)) {
                *s.col = ImVec4(col[0], col[1], col[2], 1.f);
                // Persist on every change: edits made inside the picker
                // POPUP never fire IsItemDeactivatedAfterEdit on this row,
                // so deferred saving would lose them. Saved values are
                // in-memory until Geode's save — per-change cost is nil.
                editoraiSetSavedStr(s.key, vecToHex(*s.col));
            }
            tipIfHovered("Click the swatch for a picker, or drag the R/G/B "
                         "numbers like sliders.");
        }
        if (ImGui::SmallButton("reset theme")) {
            for (auto& s : THEME_SLOTS) {
                *s.col = s.def;
                editoraiSetSavedStr(s.key, vecToHex(s.def));
            }
        }
        tipIfHovered("Back to the default EditorAI look.");
        settingInt("animation speed", "ui-anim-speed", 1, 10,
            "How fast this panel opens/closes. 1 = slow fade (2 s), "
            "10 = instant.");
    }

    if (ImGui::CollapsingHeader("Misc")) {
#ifdef GEODE_IS_DESKTOP
        ImGui::TextColored(COL_DIM, "Controls");
        {
            // The hotkey is a sequence of up to 3 keys pressed in a row.
            // While capturing, the keyboard hook appends each key into
            // g_keyCapturePending; this UI shows progress and commits.
            if (!g_keyCapture) {
                if (ImGui::Button(fmt::format("panel hotkey:  {}",
                        keySeqDisplayName(overlayToggleSeq())).c_str())) {
                    g_keyCapture = true;
                    g_keyCapturePending.clear();
                }
                tipIfHovered("Click, then press up to 3 keys (e.g. E, or G "
                             "then D). Multi-key bindings fire when tapped in "
                             "a row OR held together. Default is E.");
                settingToggle("hotkey priority", "hotkey-priority",
                    "The panel hotkey wins over the editor's own keybinds and "
                    "other mods - EditorAI consumes the key. Text boxes still "
                    "type normally. Turn off to let the key also trigger "
                    "whatever else is bound to it.");
            } else {
                ImGui::TextColored(COL_ACCENT, "press up to 3 keys in a row...");
                ImGui::Text("so far:  %s",
                    g_keyCapturePending.empty()
                        ? "(none)" : keySeqDisplayName(g_keyCapturePending).c_str());
                // A 3-key sequence auto-commits in the hook; for 1-2 keys the
                // user clicks save. Esc (in the hook) or Cancel here aborts.
                ImGui::BeginDisabled(g_keyCapturePending.empty());
                if (ImGui::SmallButton("save")) {
                    commitToggleSeq(g_keyCapturePending);
                    g_keyCapturePending.clear();
                    g_keyCapture = false;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("cancel")) {
                    g_keyCapturePending.clear();
                    g_keyCapture = false;
                }
            }
        }
        ImGui::Separator();
#endif
        settingToggle("character filter bypass", "bypass-char-filter",
            "Lets GD text boxes accept characters they normally reject "
            "(auto-enabled when you paste API keys or model IDs).");
        settingToggle("character limit bypass", "bypass-char-limit",
            "Removes GD's length caps on text boxes (auto-enabled when you "
            "paste API keys or model IDs).");
    }

    ImGui::EndChild();
}

// ── Main draw ─────────────────────────────────────────────────────────────────
#ifdef GEODE_IS_MOBILE
// ── Native cocos floating bubble (mobile) ────────────────────────────────────
// Eclipse-style: a real CCMenu targeted touch delegate, NOT an ImGui window.
// ImGui's touch emulation only delivers hover on the first tap, which forced
// a double-press before any drag could start — raw touches have no such
// problem: press-and-move drags from the very first touch.
//
// Behavior: round ~62 px bubble; drag freely (position saved across restarts);
// a quick tap toggles the panel; auto-dims to 50% after 5 s untouched;
// hidden completely inside any level (PlayLayer exists).
//
// The circle art is baked into a runtime texture once so a plain CCSprite
// shows it — sprites honor setOpacity reliably.
CCTexture2D* eaiMakeBubbleTexture() {
    constexpr int S = 112;
    constexpr float R = 54.f;
    constexpr float RING = 6.f;
    auto* data = new unsigned char[(size_t)S * S * 4];
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            float dx = (float)x + 0.5f - S / 2.f;
            float dy = (float)y + 0.5f - S / 2.f;
            float d = std::sqrt(dx * dx + dy * dy);
            float a = std::clamp(R - d, 0.f, 1.f);
            unsigned char r, g, b;
            // Match inner panel: dark navy fill (COL_BG ~ #171A1F) + accent ring
            // (#5CB0FF = COL_ACCENT). Was: white ring + light-blue fill, which
            // clashed with the dark UI.
            if (d > R - RING) { r = 92; g = 176; b = 255; }   // accent ring
            else { r = 22; g = 25; b = 31; }                  // dark fill
            size_t i = ((size_t)y * S + x) * 4;
            data[i] = r; data[i + 1] = g; data[i + 2] = b;
            data[i + 3] = (unsigned char)(a * 255.f);
        }
    }
    auto* tex = new CCTexture2D();
    tex->initWithData(data, kCCTexture2DPixelFormat_RGBA8888, S, S, CCSize(S, S));
    delete[] data;
    tex->autorelease();
    return tex;
}

class EAIBubble : public CCMenu {
protected:
    constexpr static float DRAG_SLOP = 8.f;       // finger travel before a press counts as a drag (tap vs drag)
    constexpr static float TOUCH_RADIUS = 40.f;   // round hit area (bubble visual radius is ~31 px)
    constexpr static float DIM_OPACITY = 0.5f;    // idle dim target
    constexpr static float IDLE_DELAY = 5.f;      // seconds untouched before dimming

    CCSprite* m_sprite = nullptr;
    CCLabelBMFont* m_label = nullptr;
    CCPoint m_grabOff{};   // finger-minus-sprite offset, so the bubble never jumps
    CCPoint m_pressPos{};  // finger position at press time (for the slop check)
    CCPoint m_homePos{};   // sprite position at press time (restored if it was just a tap)
    float m_opacity = 1.f; // current opacity, eased toward m_opacityTarget
    float m_opacityTarget = 1.f;
    float m_idleT = 0.f;   // seconds since last touch
    bool m_haveMoved = false;

public:
    // Central rule: the bubble is COMPLETELY hidden while a level is active
    // (PlayLayer exists) and reappears only after the player exits the level.
    // Called every frame from both get() (ImGui tick) and update() (cocos
    // tick) so a missed scheduler tick or a scene-change flash can never
    // leave it visible mid-attempt.
    void refreshForScene() {
        bool inLevel = PlayLayer::get() != nullptr;
        if (inLevel) {
            if (isVisible()) setVisible(false);
            // Reset idle so it comes back fully opaque after the level ends.
            m_idleT = 0.f;
            m_opacityTarget = 1.f;
            m_opacity = 1.f;
            applyOpacity();
        } else {
            if (!isVisible()) {
                setVisible(true);
                poke();
            }
        }
    }

    static EAIBubble* get() {
        static EAIBubble* s_inst = nullptr;
        if (!s_inst) {
            s_inst = new EAIBubble();
            if (s_inst->init()) {
                // Geode >= 5.10 removed SceneManager/keepAcrossScenes.
                // Retain the bubble for the app lifetime and re-parent it
                // to the current scene whenever the scene changes.
                s_inst->retain();
            } else {
                delete s_inst;
                s_inst = nullptr;
            }
        }
        if (s_inst) {
            if (auto* cur = CCScene::get()) {
                if (s_inst->getParent() != cur) {
                    if (s_inst->getParent())
                        s_inst->removeFromParentAndCleanup(false);
                    cur->addChild(s_inst);
                }
            }
            // Enforce hidden-in-level immediately (no 1-frame flash while
            // waiting for the cocos update() tick).
            s_inst->refreshForScene();
        }
        return s_inst;
    }

protected:
    bool init() override {
        if (!CCMenu::init()) return false;
        setZOrder(100);
        setPosition(CCPoint(0, 0));
        setID("eai-bubble"_spr);
        // Explicit: this node lives or dies by raw touches, make sure no
        // GD/CCMenu default ever leaves it touch-disabled on some version.
        setTouchEnabled(true);
        setEnabled(true);
        scheduleUpdate();

        m_sprite = CCSprite::createWithTexture(eaiMakeBubbleTexture());
        m_sprite->setScale(0.55f);   // 112 px art -> ~62 px on screen (compact, in the 55-65 px band)
        CCSize win = CCDirector::get()->getWinSize();
        m_sprite->setPosition(clampPos(CCPoint(
            (float)editoraiGetSavedInt("eai-bubble-cx", 40),
            (float)editoraiGetSavedInt("eai-bubble-cy",
                (int64_t)(win.height - 120))
        )));
        this->addChild(m_sprite);

        m_label = CCLabelBMFont::create("AI", "bigFont.fnt");
        m_label->setScale(0.8f);
        CCSize ss = m_sprite->getContentSize();
        m_label->setPosition(CCPoint(ss.width / 2.f, ss.height / 2.f));
        m_sprite->addChild(m_label);

        if (auto* scene = CCScene::get())
            scene->addChild(this);
        return true;
    }

    CCPoint clampPos(CCPoint p) {
        CCSize win = CCDirector::get()->getWinSize();
        constexpr float R = 33.f;   // visual radius ~31 + 2 px margin
        p.x = std::clamp(p.x, R, std::max(R, win.width - R));
        p.y = std::clamp(p.y, R, std::max(R, win.height - R));
        return p;
    }

    // Any touch (re)starts the idle clock at full opacity.
    void poke() {
        m_idleT = 0.f;
        m_opacityTarget = 1.f;
    }

    void applyOpacity() {
        auto o = (GLubyte)(m_opacity * 255);
        m_sprite->setOpacity(o);
        m_label->setOpacity(o);
    }

    void update(float dt) override {
        // Hidden completely inside any level — invisible, eats no taps.
        // refreshForScene() is the single source of truth (also called from
        // get() every ImGui frame); update() just drives the idle-dim when
        // visible.
        refreshForScene();
        if (!isVisible()) return;
        m_idleT += dt;
        if (m_idleT >= IDLE_DELAY) m_opacityTarget = DIM_OPACITY;
        if (m_opacity != m_opacityTarget) {
            float step = dt * 3.f;
            m_opacity += std::clamp(m_opacityTarget - m_opacity, -step, step);
            applyOpacity();
        }
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!isVisible()) return false;
        if (PlayLayer::get()) return false; // belt & suspenders: never eat taps in-level
        CCPoint p = convertToNodeSpace(touch->getLocation());
        CCPoint sp = m_sprite->getPosition();
        if (ccpDistance(p, sp) > TOUCH_RADIUS) return false;
        m_haveMoved = false;
        m_pressPos = p;
        m_homePos = sp;   // restore point if the finger lifts as a tap (kills tremble jump)
        m_grabOff = CCPoint(p.x - sp.x, p.y - sp.y);
        poke();
        return true;   // claim the touch — it started inside the bubble
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        // Follow the finger from the very first pixel so drag feels instant
        // (single press-and-move, no double-tap). The slop only decides at
        // release time whether this gesture was a tap or a drag.
        CCPoint p = convertToNodeSpace(touch->getLocation());
        if (!m_haveMoved && ccpDistance(p, m_pressPos) >= DRAG_SLOP)
            m_haveMoved = true;
        poke();
        m_sprite->setPosition(clampPos(CCPoint(p.x - m_grabOff.x, p.y - m_grabOff.y)));
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        if (m_haveMoved) {
            CCPoint sp = m_sprite->getPosition();
            editoraiSetSavedInt("eai-bubble-cx", (int64_t)sp.x);
            editoraiSetSavedInt("eai-bubble-cy", (int64_t)sp.y);
        } else {
            // Pure tap: undo any sub-slop tremble shift, then toggle panel.
            m_sprite->setPosition(m_homePos);
            g_st.panelOpen = !g_st.panelOpen;
        }
        poke();
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        // A cancelled gesture (system interruption, notification shade,
        // multi-touch preemption, ...) is NOT a tap and NOT a completed
        // drag: never toggle the panel and never persist a mid-drag bubble
        // position. Just roll back to the pre-press position and clear the
        // active-touch state.
        if (m_sprite) m_sprite->setPosition(m_homePos);
        m_haveMoved = false;
        poke();
    }

    void registerWithTouchDispatcher() override {
        // Very high priority + swallow: touches starting on the bubble reach
        // it first; anything outside passes through untouched.
        CCTouchDispatcher::get()->addTargetedDelegate(this, -1000, true);
    }
};
#endif

void drawOverlay() {
    float dt = ImGui::GetIO().DeltaTime;
    if (!g_themeLoaded) loadTheme();
    if (!g_st.persistedLoaded) loadPersistedInputs();
    applyThemeFrame();

    // Background ticks that must run even with the panel closed.
    editoraiOAuthTick(dt);
    editoraiProbeLocalBackends();   // once — tags local servers in the picker
    g_persistTimer += dt;
    if (g_persistTimer > 5.f) {
        g_persistTimer = 0.f;
        editoraiPersistSessionsIfDirty();
    }

    // Floating bubble — the touch-device way in (no E key there). This is a
    // NATIVE cocos button (Eclipse-style targeted touch delegate, created
    // below), not an ImGui window: ImGui's touch emulation only delivers
    // hover on the first tap, which forced a double-press before any drag.
    // Raw touches drag from the very first press. Hidden completely inside
    // levels, draggable (position saved), auto-dims to 50% after 5 s idle.
#ifdef GEODE_IS_MOBILE
    EAIBubble::get();
#endif

    // "Go to level" — a finished generation is waiting for its target level.
    // Tiny, pinned bottom-right, never over GD's own corner buttons; inert
    // (greyed) while the user is inside a level or another editor. On touch
    // devices it is suppressed during gameplay (same tap-eating rationale as
    // the bubble — the bottom-right corner is a jump-input area mid-attempt).
    if (!(uiMobile() && PlayLayer::get())) {
        std::shared_ptr<GenSession> waiting;
        auto& sessions = genSessions();
        for (int i = (int)sessions.size() - 1; i >= 0; --i)
            if (sessions[i] &&
                sessions[i]->state == GenSession::State::AwaitingEditor &&
                sessions[i]->targetLevel) { waiting = sessions[i]; break; }
        if (waiting) {
            auto& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 6.f, io.DisplaySize.y - 6.f),
                                    ImGuiCond_Always, ImVec2(1.f, 1.f));
            ImGui::SetNextWindowBgAlpha(0.85f);
            if (ImGui::Begin("##eaigoto", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoFocusOnAppearing)) {
                bool blocked = PlayLayer::get() != nullptr ||
                               LevelEditorLayer::get() != nullptr;
                ImGui::BeginDisabled(blocked);
                if (ImGui::SmallButton("go to level"))
                    editoraiGoToSessionLevel(waiting);
                ImGui::EndDisabled();
                if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Leave this level/editor first");
            }
            ImGui::End();
        }
    }

    // Never draw the big panel over LIVE gameplay on any platform — it eats
    // input mid-attempt and its rect is huge. Paused levels are fine (the
    // pause menu is exactly when you'd read/queue a generation). panelOpen
    // survives; the panel returns when the level pauses or ends.
    if (auto* pl = PlayLayer::get(); pl && !pl->m_isPaused) return;

    // Open/close animation: alpha fade driven by the ui-anim-speed setting
    // (1 = 2 s fade, 10 = instant). The setting is re-read at most once a
    // second — not 120x/s.
    {
        static int   s_spd = 8;
        static float s_spdAge = 99.f;
        s_spdAge += dt;
        if (s_spdAge > 1.f) {
            s_spdAge = 0.f;
            s_spd = (int)std::clamp<int64_t>(editoraiGetInt("ui-anim-speed"), 1, 10);
        }
        float dur = s_spd >= 10 ? 0.f : 2.f * (float)(10 - s_spd) / 9.f;
        float target = g_st.panelOpen ? 1.f : 0.f;
        if (dur <= 0.f) g_animT = target;
        else {
            float step = dt / dur;
            g_animT += g_animT < target ? step : -step;
            g_animT = std::clamp(g_animT, 0.f, 1.f);
        }
    }
    if (g_animT <= 0.05f && !g_st.panelOpen) {
        // Fully hidden — drop any mid-edit flags so Settings fields re-sync
        // from live values when it reopens. A hotkey capture left dangling
        // would silently eat the next keypress, so it dies here too.
        for (auto& [id, tb] : textBufs()) tb.editing = false;
        g_keyCapture = false;
        return;
    }
    float alpha = g_animT * g_animT * (3.f - 2.f * g_animT);  // smoothstep
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

    // Desktop keeps a movable working panel. On phones, make this a genuine
    // full-screen surface: no tiny floating controls, no accidental editor
    // taps around the edges, and enough vertical room above the software
    // keyboard for the composer and settings controls.
    const auto* viewport = ImGui::GetMainViewport();
    bool mobile = uiMobile();
    if (mobile) {
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowSize(ImVec2(760, 460), ImGuiCond_FirstUseEver);
    }
    // Center the window every time it (re)appears. The window is only
    // submitted while open, so Appearing fires on each open — this makes a
    // lost/off-screen position self-heal instead of rendering into nowhere.
    if (!mobile)
        ImGui::SetNextWindowPos(viewport->GetCenter(),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    // While fading OUT the window is visually gone but would still hit-test
    // (Alpha only affects rendering) — drop mouse input so it can't swallow
    // clicks meant for the game.
    ImGuiWindowFlags animFlags = !g_st.panelOpen ? ImGuiWindowFlags_NoMouseInputs : 0;
    if (mobile) animFlags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (!ImGui::Begin("EditorAI", &open, animFlags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    if (!open) {                         // animate out; keep rendering
        g_st.panelOpen = false;
        g_keyCapture   = false;          // never leave a capture armed
    }

    // ── Header bar: status beacon + state text + right-aligned hide hint ──
    {
        int running = 0;
        for (auto& s : genSessions())
            if (s && s->state == GenSession::State::Running) ++running;

        // Status beacon: steady dim dot when idle, pulsing warm dot while
        // generating — readable at a glance without parsing any text.
        ImVec2 p = ImGui::GetCursorScreenPos();
        float  h = ImGui::GetTextLineHeight();
        float  r = h * 0.32f;
        ImU32  dotCol;
        if (running > 0) {
            float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 4.f);
            dotCol = ImGui::GetColorU32(ImVec4(COL_WARN.x, COL_WARN.y,
                                               COL_WARN.z, pulse));
        } else {
            dotCol = ImGui::GetColorU32(ImVec4(COL_DIM.x, COL_DIM.y,
                                               COL_DIM.z, 0.9f));
        }
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + r + 1.f, p.y + h * 0.55f), r, dotCol);
        ImGui::Dummy(ImVec2(r * 2.f + 7.f, h));
        ImGui::SameLine(0.f, 4.f);

        if (running > 0)
            ImGui::TextColored(COL_WARN, "%d generation%s running",
                               running, running == 1 ? "" : "s");
        else
            ImGui::TextColored(COL_DIM, "idle");

        // Right-aligned dismiss hint, out of the reading path. On touch
        // screens the title-bar X is too small to hit reliably and the
        // full-screen panel has no other way out, so mobile also gets a
        // big dedicated close button.
        std::string hint = mobile
            ? std::string("bubble hides")
            : fmt::format("{} hides", keySeqDisplayName(overlayToggleSeq()));
        float btnW = mobile ? 56.f : 0.f;
        float gap  = mobile ? 8.f : 0.f;
        float hw = ImGui::CalcTextSize(hint.c_str()).x + btnW + gap;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20.f,
                                 ImGui::GetContentRegionMax().x - hw));
        if (mobile) {
            if (ImGui::Button("X##eai-close", ImVec2(btnW, 34.f))) {
                g_st.panelOpen = false;
                g_keyCapture = false;
            }
            tipIfHovered("Hide the panel (the AI bubble brings it back).");
            ImGui::SameLine(0.f, gap);
        }
        ImGui::TextColored(ImVec4(COL_DIM.x, COL_DIM.y, COL_DIM.z, 0.65f),
                           "%s", hint.c_str());
    }

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Chat")) {
            tabChat(dt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            tabSettings();
            ImGui::EndTabItem();
        } else {
            // A field left mid-edit when the tab was switched away would keep
            // its editing flag set and block live re-sync forever. Clear the
            // flags ONLY for fields that did not render this frame — the
            // Chat tab's composer uses settingText too (custom difficulty/
            // style), and clearing those mid-typing clobbers keystrokes.
            int nowFrame = ImGui::GetFrameCount();
            for (auto& [id, tb] : textBufs())
                if (tb.lastFrame < nowFrame) tb.editing = false;
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace

#ifdef GEODE_IS_DESKTOP
// Keys currently held (cocos keycode set). Maintained unconditionally —
// including while text boxes are focused — so a key released inside an input
// can never get stuck "held"; it is only READ when the text-input guards pass.
static std::unordered_set<int> g_heldKeys;

// The single hotkey brain: capture flow + sequence/chord matching + toggle.
// Returns true when the key event must be CONSUMED (nothing else — GD editor,
// Custom Keybinds, other mods — may see it). Called from the raw GLFW hook on
// Windows and from the keyboard-dispatcher hook elsewhere / for keys the GLFW
// map doesn't cover.
static bool editoraiOverlayHandleKey(int key, bool down, bool repeat) {
    // Feed modifier state explicitly before the modal overlay consumes the
    // dispatcher event. gd-imgui-cocos also translates named keys, but on
    // some platform/input paths its per-frame modifier snapshot arrives too
    // late for Ctrl+A or Shift+Arrow in the currently focused text field.
    if (ImGuiCocos::get().isInitialized()) {
        auto k = static_cast<cocos2d::enumKeyCodes>(key);
        auto& io = ImGui::GetIO();
        if (k == cocos2d::enumKeyCodes::KEY_Control ||
            k == cocos2d::enumKeyCodes::KEY_LeftControl ||
            k == cocos2d::enumKeyCodes::KEY_RightContol)
            io.AddKeyEvent(ImGuiMod_Ctrl, down);
        if (k == cocos2d::enumKeyCodes::KEY_Shift ||
            k == cocos2d::enumKeyCodes::KEY_LeftShift ||
            k == cocos2d::enumKeyCodes::KEY_RightShift)
            io.AddKeyEvent(ImGuiMod_Shift, down);

        // Some gd-imgui-cocos/Proton paths deliver printable text through IME
        // but lose the named Backspace event before ImGui sees it. Mirror the
        // destructive/navigation keys while our modal panel is open so every
        // InputText (composer, chat, settings, endpoint profiles) edits
        // consistently. Repeated same-state AddKeyEvent calls are deduplicated
        // by ImGui, so this is safe even when the backend also supplies them.
        if (g_st.panelOpen || io.WantCaptureKeyboard) {
            if (key == 8)  io.AddKeyEvent(ImGuiKey_Backspace, down);
            if (key == 46) io.AddKeyEvent(ImGuiKey_Delete, down);
            if (key == 35) io.AddKeyEvent(ImGuiKey_End, down);
            if (key == 36) io.AddKeyEvent(ImGuiKey_Home, down);
            if (key == 37) io.AddKeyEvent(ImGuiKey_LeftArrow, down);
            if (key == 38) io.AddKeyEvent(ImGuiKey_UpArrow, down);
            if (key == 39) io.AddKeyEvent(ImGuiKey_RightArrow, down);
            if (key == 40) io.AddKeyEvent(ImGuiKey_DownArrow, down);
        }
    }

    // Held-key ledger for the chord matcher ("pressed together").
    if (!repeat) {
        if (down) g_heldKeys.insert(key);
        else      g_heldKeys.erase(key);
    }

    // ── Hotkey capture (Settings → Controls) ──────────────────────────────
    // Each key pressed appends to the pending sequence (up to 3); the 3rd
    // auto-commits, fewer are saved via the UI button. Esc cancels. Bare
    // modifiers are ignored so a shifted choice doesn't bind Shift. Only
    // while the panel is OPEN — a capture armed through the close fade would
    // silently rebind+swallow the next editor keypress.
    if (g_keyCapture && !g_st.panelOpen) { g_keyCapture = false; g_keyCapturePending.clear(); }
    if (g_keyCapture && down && !repeat) {
        if (key == 27) { g_keyCapture = false; g_keyCapturePending.clear(); return true; }  // Esc
        // Editor-critical keys are refused (still capturing): binding
        // Tab/Space/Delete/Enter/Backspace/arrows would steal them from
        // GD's editor (Tab toggles the build menu).
        bool refused = key == 8 || key == 9 || key == 13 || key == 32 || key == 46 ||
                       (key >= 37 && key <= 40);
        if (refused) return true;
        if (key == 16 || key == 17 || key == 18) return true;    // bare modifier — ignore
        g_keyCapturePending.push_back(key);
        if (g_keyCapturePending.size() >= 3) {                   // full sequence — commit
            commitToggleSeq(g_keyCapturePending);
            g_keyCapturePending.clear();
            g_keyCapture = false;
        }
        return true;
    }

    // ── Hotkey match: sequence OR chord ────────────────────────────────────
    // Two ways a multi-key binding fires:
    //   - sequence: the keys tapped in order, each gap under 1.2 s
    //     (ring of recent presses, tail-compared)
    //   - chord: all binding keys physically held at the same time, in ANY
    //     press order — "both pressed at once" just works
    // A 1-key binding fires immediately via the sequence path. Only evaluated
    // while the guards pass, so keys typed into a text box never accumulate
    // or toggle.
    if (down && !repeat) {
        bool guardsOk = ImGuiCocos::get().isInitialized() &&
                        !editoraiIsGDTextInputActive() &&
                        !ImGui::GetIO().WantCaptureKeyboard;
        if (guardsOk) {
            static std::vector<std::pair<int, double>> recent;  // (keycode, seconds)
            constexpr double SEQ_WINDOW = 1.2;  // max gap between keys in a row
            double now = nowSeconds();
            recent.push_back({key, now});
            if (recent.size() > 3) recent.erase(recent.begin());

            const auto& seq = overlayToggleSeq();
            bool fired = false;

            // Chord: this press is part of the binding and every binding key
            // is currently down.
            if (seq.size() >= 2) {
                bool keyInSeq = false, allHeld = true;
                for (int k : seq) {
                    if (k == key) keyInSeq = true;
                    if (!g_heldKeys.count(k)) allHeld = false;
                }
                fired = keyInSeq && allHeld;
            }

            // Sequence: the ring's tail equals the binding in order.
            if (!fired && !seq.empty() && recent.size() >= seq.size()) {
                size_t off = recent.size() - seq.size();
                bool match = true;
                for (size_t i = 0; i < seq.size() && match; ++i)
                    if (recent[off + i].first != seq[i]) match = false;
                // For multi-key sequences every consecutive gap must be
                // within the window (single-key has no gap to check).
                for (size_t i = off + 1; i < recent.size() && match; ++i)
                    if (recent[i].second - recent[i - 1].second > SEQ_WINDOW) match = false;
                fired = match;
            }

            if (fired) {
                // Refractory: GD can dispatch one physical press through here
                // more than once in some scenes — a re-fire within the window
                // would toggle the panel open and instantly closed again.
                static double s_lastFire = -10.0;
                if (now - s_lastFire > 0.25) {
                    s_lastFire = now;
                    g_st.panelOpen = !g_st.panelOpen;
                    if (!g_st.panelOpen) g_keyCapture = false;
                }
                recent.clear();             // don't double-fire on the next key
                // Priority ON: consume the completing key so GD's editor and
                // other mods never act on it. OFF: toggle but let the key
                // flow through to everything downstream.
                return editoraiGetBool("hotkey-priority");
            }
        }
    }
    // The overlay receives ImGui input independently, so when it is visible
    // the game must not also react to the same keystrokes. This makes typing,
    // shortcuts, and navigation truly modal instead of leaking into the
    // editor behind the window.
    return g_st.panelOpen;
}

// The hotkey hook. Runs at essentially-first priority so a matched toggle
// key is consumed before GD's editor keybinds and mods like Custom Keybinds
// (all of which live downstream of this function) can act on it.
class $modify(EAIOverlayKeys, CCKeyboardDispatcher) {
    static void onModify(auto self) {
        if (!self.setHookPriority(
                "cocos2d::CCKeyboardDispatcher::dispatchKeyboardMSG", -1'000'000'000))
            geode::log::warn("EditorAI: failed to prioritize the hotkey hook");
    }

    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool down, bool repeat, double time) {
        if (editoraiOverlayHandleKey((int)key, down, repeat))
            return true;
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, time);
    }
};
#endif

static void editoraiOverlaySetup() {
    ImGuiCocos::get().setup([] {
        auto& style = ImGui::GetStyle();
        // Card-like chrome: generous rounding, soft padding, no hard borders.
        style.FrameRounding     = 6.f;
        style.WindowRounding    = 12.f;
        style.ChildRounding     = 10.f;
        style.PopupRounding     = 8.f;
        style.GrabRounding      = 6.f;
        style.TabRounding       = 6.f;
        style.ScrollbarRounding = 10.f;
        style.ScrollbarSize     = 11.f;
        style.FramePadding      = ImVec2(10, 6);
        style.ItemSpacing       = ImVec2(10, 8);
        style.ItemInnerSpacing  = ImVec2(8, 6);
        style.WindowPadding     = ImVec2(14, 12);
        style.CellPadding       = ImVec2(8, 5);
        style.GrabMinSize       = 12.f;
        style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
        style.SeparatorTextBorderSize = 2.f;
        style.SeparatorTextPadding    = ImVec2(18, 2);
        style.WindowBorderSize  = 0.f;
        style.ChildBorderSize   = 1.f;
        style.PopupBorderSize   = 0.f;
        style.FrameBorderSize   = 0.f;
        style.TabBarBorderSize  = 0.f;
        // Colors come from applyThemeFrame() every frame (user-tunable).
    }).draw([] {
        drawOverlay();
    });
    ImGuiCocos::get().setInputMode(ImGuiCocos::InputMode::Default);
    // The ImGuiCocos layer itself stays visible; panel visibility is our own
    // flag so the mobile bubble can render while the panel is closed.
    ImGuiCocos::get().setVisible(true);
}

$execute {
    editoraiOverlaySetup();
    geode::log::info("EditorAI overlay registered (E key / AI bubble)");
}

#endif
