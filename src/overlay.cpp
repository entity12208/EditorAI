// EditorAI overlay — the mod's primary UI (ImGui via gd-imgui-cocos).
// Two tabs: Chat (session list + conversations + the new-chat composer in
// one place) and Settings (full mod configuration, theme, OAuth).
// Opens with a user-bindable key on desktop (default E) or the floating
// bubble on touch devices; redrawn every frame, so nothing here can ever go
// stale. All inputs persist across restarts via Geode saved values.
#ifdef EDITORAI_HAS_IMGUI

#include "sessions.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <imgui-cocos.hpp>
#include <algorithm>
#include <array>
#include <chrono>
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
struct TextBuf { std::array<char, 256> buf{}; bool editing = false; int lastFrame = -1; };
std::unordered_map<std::string, TextBuf>& textBufs() {
    static std::unordered_map<std::string, TextBuf> b;
    return b;
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
    ImGui::SetNextItemWidth(280.f);
    ImGui::InputTextWithHint(fmt::format("{}##{}", label, id).c_str(), hint,
        tb.buf.data(), tb.buf.size(),
        secret ? ImGuiInputTextFlags_Password : 0);
    tb.editing = ImGui::IsItemActive();
    tipIfHovered(tip);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
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
    ImGui::SetNextItemWidth(210.f);
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

// ── Saved-level autocomplete ─────────────────────────────────────────────────
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

// Example-level-IDs editor: one box per ID with +/- row controls and the
// saved-level picker, stored back into the comma-joined setting (cap 5).
std::vector<std::array<char, 24>> g_exampleIdRows;
bool g_exampleIdsLoaded = false;

bool isAllDigits(const char* s) {
    if (!*s) return false;
    for (; *s; ++s)
        if (*s < '0' || *s > '9') return false;
    return true;
}

void exampleIdsWidget() {
    auto save = [] {
        std::string joined;
        for (auto& r : g_exampleIdRows) {
            if (!isAllDigits(r.data())) continue;  // names stay UI-only
            if (!joined.empty()) joined += ",";
            joined += r.data();
        }
        editoraiSetStr("example-level-ids", joined);
    };
    if (!g_exampleIdsLoaded) {
        g_exampleIdRows.clear();
        std::string raw = editoraiGetStr("example-level-ids");
        size_t start = 0;
        while (start < raw.size() && g_exampleIdRows.size() < 5) {
            size_t comma = raw.find(',', start);
            size_t len = (comma == std::string::npos ? raw.size() : comma) - start;
            std::string part = raw.substr(start, len);
            part.erase(0, part.find_first_not_of(" \t"));
            if (auto cut = part.find_last_not_of(" \t"); cut != std::string::npos)
                part.resize(cut + 1);
            if (!part.empty()) {
                std::array<char, 24> row{};
                snprintf(row.data(), row.size(), "%s", part.c_str());
                g_exampleIdRows.push_back(row);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (g_exampleIdRows.empty()) g_exampleIdRows.push_back({});
        g_exampleIdsLoaded = true;
    }

    ImGui::TextColored(COL_DIM, "example levels (the AI studies their style)");
    tipIfHovered("Up to 5 online level IDs the AI downloads and learns from. "
                 "Type an ID or a name to filter your saved levels, or pick "
                 "one from the dropdown.");
    int removeIdx = -1;
    for (int i = 0; i < (int)g_exampleIdRows.size(); ++i) {
        ImGui::PushID(7000 + i);
        ImGui::SetNextItemWidth(170.f);
        ImGui::InputTextWithHint("##exid", "ID, or name to search",
            g_exampleIdRows[i].data(), g_exampleIdRows[i].size());
        bool editedNow = ImGui::IsItemDeactivatedAfterEdit();
        bool rowActive = ImGui::IsItemActive();  // before suggest's Begin/End
        if (savedLevelSuggest(g_exampleIdRows[i].data(),
                              g_exampleIdRows[i].size()) || editedNow)
            save();
        if (g_exampleIdRows[i][0] && !isAllDigits(g_exampleIdRows[i].data()) &&
            !rowActive) {
            ImGui::SameLine();
            ImGui::TextColored(COL_WARN, "(pick from list - names don't save)");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(g_exampleIdRows.size() <= 1 && !g_exampleIdRows[0][0]);
        if (ImGui::SmallButton("-")) removeIdx = i;
        ImGui::EndDisabled();
        tipIfHovered("Remove this example level.");
        if (i + 1 == (int)g_exampleIdRows.size()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(g_exampleIdRows.size() >= 5);
            if (ImGui::SmallButton("+")) g_exampleIdRows.push_back({});
            ImGui::EndDisabled();
            tipIfHovered("Add another example level (max 5).");
        }
        ImGui::PopID();
    }
    if (removeIdx >= 0) {
        g_exampleIdRows.erase(g_exampleIdRows.begin() + removeIdx);
        if (g_exampleIdRows.empty()) g_exampleIdRows.push_back({});
        save();
    }
}

// Model field per provider: combo where mod.json constrains it (one-of),
// free text otherwise. Free-text model edits auto-enable the char bypasses.
void providerModelWidget(const std::string& p) {
    const char* tip = "Which model this provider runs. Bigger = better "
                      "levels, slower and pricier.";
    if (p == "gemini")
        settingCombo("model", "gemini-model", {"gemini-3-flash", "gemini-3-pro"}, tip);
    else if (p == "claude")
        settingCombo("model", "claude-model", {"claude-sonnet-4-6", "claude-opus-4-6"}, tip);
    else if (p == "openai")
        settingCombo("model", "openai-model", {"gpt-4o", "gpt-4.1-mini"}, tip);
    else if (p == "ministral")
        settingCombo("model", "ministral-model",
            {"ministral-3b-latest", "ministral-8b-latest", "mistral-small-latest",
             "mistral-medium-latest", "mistral-large-latest"}, tip);
    else if (p == "deepseek")
        settingCombo("model", "deepseek-model",
            {"deepseek-chat", "deepseek-reasoner", "deepseek-coder"}, tip);
    else if (p == "huggingface")
        settingText("model", "huggingface-model", "org/model-name", false, tip, true);
    else if (p == "openrouter")
        settingText("model", "openrouter-model", "vendor/model", false, tip, true);
    else if (p == "ollama")
        settingText("model", "ollama-model", "entity12208/editorai:deepseek", false, tip, true);
    else if (p == "lm-studio")
        settingText("model", "lm-studio-model", "default", false, tip, true);
    else if (p == "llama-cpp")
        settingText("model", "llama-cpp-model", "default", false, tip, true);
    else
        settingText("model", "custom-provider-model", "model name", false, tip, true);
}

// ── Tab: Sessions ─────────────────────────────────────────────────────────────
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
            if (ImGui::TreeNode("thinking...")) {
                ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
                ImGui::TextWrapped("%s", e.text.c_str());
                ImGui::PopStyleColor();
                ImGui::TreePop();
            }
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
            ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
            ImGui::TextWrapped("- %s", e.text.c_str());
            ImGui::PopStyleColor();
            break;
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
        if (pinBottom) ImGui::SetScrollHereY(1.0f);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
        ImGui::TextWrapped(g_st.pendingSelectAfter >= 0
            ? "Starting the generation - its conversation appears here the "
              "moment the editor opens..."
            : "Select a session to see its conversation.");
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
        ImGuiInputTextFlags_EnterReturnsTrue);
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
void composerBody(float dt) {
    if (!g_st.levelsFresh) {
        g_st.levels = editoraiListLocalLevels();
        g_st.levelsFresh = true;
        if (g_st.genTarget >= (int)g_st.levels.size()) g_st.genTarget = -1;
    }

    ImGui::TextColored(COL_ACCENT, "Target");
    tipIfHovered("Where the generated objects go.");
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh levels")) g_st.levelsFresh = false;
    tipIfHovered("Re-read your created-levels list.");

    std::string preview =
        g_st.genTarget == -1 ? "Current editor" :
        g_st.genTarget == -2 ? "+ New level"    :
        (g_st.genTarget < (int)g_st.levels.size()
            ? fmt::format("{} ({} obj)", g_st.levels[g_st.genTarget].name,
                          g_st.levels[g_st.genTarget].objectCount)
            : "?");
    ImGui::SetNextItemWidth(300.f);
    if (ImGui::BeginCombo("##target", preview.c_str())) {
        if (ImGui::Selectable("Current editor", g_st.genTarget == -1))
            g_st.genTarget = -1;
        if (ImGui::Selectable("+ New level", g_st.genTarget == -2))
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
        ImGui::EndCombo();
    }
    tipIfHovered("Current editor: build where you are.\n+ New level: create "
                 "and open a fresh level.\nOr pick any of your created levels.");

    if (g_st.genTarget == -2) {
        ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
        ImGui::TextUnformatted("A fresh level will be created and opened.");
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Checkbox("Replace level contents", &g_st.genReplace))
            editoraiSetSavedInt("ov-gen-replace", g_st.genReplace ? 1 : 0);
        tipIfHovered("On: the AI rebuilds the level from scratch.\n"
                     "Off: it adds to what's already there.");
    }

    ImGui::Spacing();
    ImGui::TextColored(COL_ACCENT, "Describe the level");
    ImGui::InputTextMultiline("##prompt", g_st.genPrompt, sizeof(g_st.genPrompt),
        ImVec2(-1.f, 110.f));
    tipIfHovered("Plain words: theme, gamemodes, pacing, song, anything. "
                 "Mention a BPM to sync to beats.");
    if (ImGui::IsItemDeactivatedAfterEdit())
        editoraiSetSavedStr("ov-gen-prompt", g_st.genPrompt);
    {
        size_t plen = strlen(g_st.genPrompt);
        if (plen > 0)
            ImGui::TextColored(plen > 400 ? COL_WARN : COL_DIM,
                "%zu chars%s", plen,
                plen > 400 ? "  (long prompts cost more and rarely help)" : "");
    }

    ImGui::Spacing();
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
                     "of your saved levels; 'custom...' takes any words.");
        if (isLevelId && !g_st.styleCustom) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(170.f);
            ImGui::InputTextWithHint("##stylelvl", "ID or saved-level name",
                g_st.styleLevelId, sizeof(g_st.styleLevelId));
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
                     "gets there. 0 = let the AI decide. Capped by the "
                     "'max objects' setting.");
    }

    ImGui::Spacing();
    bool can = g_st.genPrompt[0] != '\0';
    ImGui::BeginDisabled(!can);
    // Generate follows the user's accent color, not a hardcoded blue.
    ImGui::PushStyleColor(ImGuiCol_Button,
        ImVec4(COL_ACCENT.x * 0.55f, COL_ACCENT.y * 0.55f, COL_ACCENT.z * 0.55f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ImVec4(COL_ACCENT.x * 0.75f, COL_ACCENT.y * 0.75f, COL_ACCENT.z * 0.75f, 1.f));
    if (ImGui::Button("Generate", ImVec2(140, 32))) {
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
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    tipIfHovered("Start the generation. You can close this panel - or even "
                 "the editor - while it runs.");
    if (!can) {
        ImGui::SameLine();
        ImGui::TextColored(COL_DIM, "describe the level first");
    }

    if (!g_st.genError.empty()) {
        if (g_st.genErrorTtl > 0.f) {
            g_st.genErrorTtl -= dt;
            ImGui::TextColored(COL_ERR, "%s", g_st.genError.c_str());
        } else {
            g_st.genError.clear();  // keep state self-consistent post-TTL
        }
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
    ImGui::TextWrapped("Tips: mention a BPM ('140 bpm') to sync to beats; "
        "with Replace off the AI builds onto whatever is already there.");
    ImGui::PopStyleColor();
}

// ── Tab: Settings ─────────────────────────────────────────────────────────────
void tabSettings() {
    static const std::vector<const char*> PROVIDERS = {
        "gemini", "claude", "openai", "openrouter", "ministral",
        "huggingface", "deepseek", "ollama", "lm-studio", "llama-cpp", "custom"};
    static const std::vector<const char*> SUB_PROVIDERS = {
        "", "gemini", "claude", "openai", "openrouter", "ministral",
        "huggingface", "deepseek", "ollama", "lm-studio", "llama-cpp"};

    ImGui::BeginChild("settingsScroll", ImVec2(0, 0));

    if (ImGui::CollapsingHeader("Provider", ImGuiTreeNodeFlags_DefaultOpen)) {
        settingCombo("provider", "ai-provider", PROVIDERS,
            "Which AI service generates levels. Ollama runs locally (free); "
            "Platinum runs on community machines (free); the rest need an "
            "account with that provider.");
        std::string p = editoraiGetStr("ai-provider");
        providerModelWidget(p);
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
        } else {
            settingText("API key", p + "-api-key", "paste key", true,
                "Stored locally on this device and only sent to the "
                "provider itself.", true);
        }
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
        ImGui::TextColored(COL_DIM, "Subagent - a second model the AI can consult");
        settingCombo("subagent", "subagent-provider", SUB_PROVIDERS,
            "Optional second model the main AI can ask focused questions "
            "while building. Empty = disabled.");
        if (!editoraiGetStr("subagent-provider").empty())
            settingText("subagent model", "subagent-model",
                "empty = provider default", false,
                "Model the subagent uses.", true);
    }

    if (ImGui::CollapsingHeader("Generation")) {
        settingInt("max objects", "max-objects", 10, 1000000,
            "Hard ceiling on objects per generation. Higher = more detail, "
            "slower spawning.", ImGuiSliderFlags_Logarithmic);
        settingInt("spawn speed", "spawn-batch-size", 1, 100,
            "Ghost objects placed per tick while a blueprint appears. "
            "Higher = faster, choppier on weak devices.");
        settingInt("ground Y", "ai-ground-y", 15, 300,
            "The Y coordinate the AI treats as ground level (GD default 105).");
        exampleIdsWidget();
        settingToggle("compact prompts (cheaper)", "compact-prompts",
            "Sends a ~3 KB prompt instead of ~60 KB. Cheaper and faster; "
            "the full prompt knows more object names.");
        settingToggle("AI tools (search, level fetch, analysis)", "enable-ai-tools",
            "Lets the AI call tools mid-generation: web search, downloading "
            "reference levels, physics simulation, passability checks. The AI "
            "runs as many tool rounds as it needs - there is no limit.");
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
        settingInt("edit workload target", "edit-target-ops", 0, 5000,
            "When the AI reworks an existing level it must total at least "
            "this many edits (objects moved + deleted + restyled + added) - "
            "it is asked to continue until it gets there. Small follow-up "
            "tweaks (under 30 edits) stage immediately. 0 = off.");
    }

    if (ImGui::CollapsingHeader("Workflow")) {
        settingToggle("copilot mode (auto-fix while editing)", "copilot-mode",
            "While you edit, the AI watches for impossible sections and "
            "proposes fixes when the editor goes idle.");
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
        settingToggle("auto-share generations (opt-in telemetry)", "allow-telemetry",
            "While ON, EVERY generation uploads automatically to the "
            "community training collector: prompt + settings + objects + "
            "ratings (yours and the AI's own). Never identity, never keys. "
            "Highly-rated levels train the free community models.");
    }

    if (ImGui::CollapsingHeader("Theme")) {
        ImGui::TextColored(COL_DIM, "Drag the sliders - changes apply live.");
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
                tipIfHovered("Click, then press up to 3 keys in a row (e.g. "
                             "E, or G then D). That sequence opens/closes this "
                             "panel. Default is E.");
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
void drawOverlay() {
    float dt = ImGui::GetIO().DeltaTime;
    if (!g_themeLoaded) loadTheme();
    if (!g_st.persistedLoaded) loadPersistedInputs();
    applyThemeFrame();

    // Background ticks that must run even with the panel closed.
    editoraiOAuthTick(dt);
    g_persistTimer += dt;
    if (g_persistTimer > 5.f) {
        g_persistTimer = 0.f;
        editoraiPersistSessionsIfDirty();
    }

    // Floating bubble — the touch-device way in (no E key there). Hidden
    // during gameplay: an always-on window would eat taps in its rect, and
    // mid-attempt is never the moment.
    if (uiMobile() && !PlayLayer::get()) {
        ImGui::SetNextWindowPos(ImVec2(8, 60), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("##eaibubble", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoCollapse)) {
            if (ImGui::Button("AI", ImVec2(44, 44)))
                g_st.panelOpen = !g_st.panelOpen;
        }
        ImGui::End();
    }

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

    // Touch devices: never draw the big panel over live gameplay — the same
    // tap-eating rationale that hides the bubble, and the panel's rect is
    // 17× bigger. panelOpen survives; the panel returns after the attempt.
    if (uiMobile() && PlayLayer::get()) return;

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
        g_exampleIdsLoaded = false;
        g_keyCapture = false;
        return;
    }
    float alpha = g_animT * g_animT * (3.f - 2.f * g_animT);  // smoothstep
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

    ImGui::SetNextWindowSize(ImVec2(760, 460), ImGuiCond_FirstUseEver);
    bool open = true;
    // While fading OUT the window is visually gone but would still hit-test
    // (Alpha only affects rendering) — drop mouse input so it can't swallow
    // clicks meant for the game.
    ImGuiWindowFlags animFlags =
        !g_st.panelOpen ? ImGuiWindowFlags_NoMouseInputs : 0;
    if (!ImGui::Begin("EditorAI", &open, animFlags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    if (!open) {                         // animate out; keep rendering
        g_st.panelOpen = false;
        g_keyCapture   = false;          // never leave a capture armed
    }

    // Header strip: running indicator.
    int running = 0;
    for (auto& s : genSessions())
        if (s && s->state == GenSession::State::Running) ++running;
    if (running > 0)
        ImGui::TextColored(COL_WARN, "%d generation%s running",
                           running, running == 1 ? "" : "s");
    else if (uiMobile())
        ImGui::TextColored(COL_DIM, "idle  -  the AI bubble or X hides this panel");
    else
        ImGui::TextColored(COL_DIM, "idle  -  %s hides this panel",
                           keySeqDisplayName(overlayToggleSeq()).c_str());

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
            g_exampleIdsLoaded = false;
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace

#ifdef GEODE_IS_DESKTOP
class $modify(EAIOverlayKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool down, bool repeat, double time) {
        // ── Hotkey capture (Settings → Controls) ──────────────────────────
        // Each key pressed appends to the pending sequence (up to 3); the
        // 3rd auto-commits, fewer are saved via the UI button. Esc cancels.
        // Bare modifiers are ignored so a shifted choice doesn't bind Shift.
        // Only while the panel is OPEN — a capture armed through the close
        // fade would silently rebind+swallow the next editor keypress.
        if (g_keyCapture && !g_st.panelOpen) { g_keyCapture = false; g_keyCapturePending.clear(); }
        if (g_keyCapture && down && !repeat) {
            int k = (int)key;
            if (k == 27) { g_keyCapture = false; g_keyCapturePending.clear(); return true; }  // Esc
            // Editor-critical keys are refused (still capturing): binding
            // Tab/Space/Delete/Enter/Backspace/arrows would steal them from
            // GD's editor (Tab toggles the build menu).
            bool refused = k == 8 || k == 9 || k == 13 || k == 32 || k == 46 ||
                           (k >= 37 && k <= 40);
            if (refused) return true;
            if (k == 16 || k == 17 || k == 18) return true;          // bare modifier — ignore
            g_keyCapturePending.push_back(k);
            if (g_keyCapturePending.size() >= 3) {                   // full sequence — commit
                commitToggleSeq(g_keyCapturePending);
                g_keyCapturePending.clear();
                g_keyCapture = false;
            }
            return true;
        }

        // ── Hotkey match: a sequence of 1-3 keys pressed in a row ──────────
        // Keep a small ring of recent (key, time) presses; the toggle fires
        // when the ring's tail equals the configured sequence with each
        // consecutive gap inside a short window. A 1-key binding fires
        // immediately (exactly the old single-key behavior). Only built while
        // the guards pass, so keys typed into a text box never accumulate.
        if (down && !repeat) {
            bool guardsOk = ImGuiCocos::get().isInitialized() &&
                            !editoraiIsGDTextInputActive() &&
                            !ImGui::GetIO().WantCaptureKeyboard;
            if (guardsOk) {
                static std::vector<std::pair<int, double>> recent;  // (keycode, seconds)
                constexpr double SEQ_WINDOW = 1.2;  // max gap between keys in a row
                double now = nowSeconds();
                recent.push_back({(int)key, now});
                if (recent.size() > 3) recent.erase(recent.begin());

                const auto& seq = overlayToggleSeq();
                if (!seq.empty() && recent.size() >= seq.size()) {
                    size_t off = recent.size() - seq.size();
                    bool match = true;
                    for (size_t i = 0; i < seq.size() && match; ++i)
                        if (recent[off + i].first != seq[i]) match = false;
                    // For multi-key sequences every consecutive gap must be
                    // within the window (single-key has no gap to check).
                    for (size_t i = off + 1; i < recent.size() && match; ++i)
                        if (recent[i].second - recent[i - 1].second > SEQ_WINDOW) match = false;
                    if (match) {
                        g_st.panelOpen = !g_st.panelOpen;
                        if (!g_st.panelOpen) g_keyCapture = false;
                        recent.clear();             // don't double-fire on the next key
                        return true;                // swallow the completing key
                    }
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, time);
    }
};
#endif

static void editoraiOverlaySetup() {
    ImGuiCocos::get().setup([] {
        auto& style = ImGui::GetStyle();
        style.FrameRounding     = 5.f;
        style.WindowRounding    = 8.f;
        style.ChildRounding     = 8.f;
        style.PopupRounding     = 6.f;
        style.GrabRounding      = 4.f;
        style.TabRounding       = 5.f;
        style.ScrollbarRounding = 8.f;
        style.ScrollbarSize     = 12.f;
        style.FramePadding      = ImVec2(8, 5);
        style.ItemSpacing       = ImVec2(8, 7);
        style.WindowPadding     = ImVec2(12, 10);
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
