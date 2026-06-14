#pragma once
// Shared seam between main.cpp (the engine) and overlay.cpp (the ImGui UI).
// Keep this header lean: GenSession + the few calls the overlay needs. The
// engine (AIGeneratorPopup) stays private to main.cpp.

#include <Geode/Geode.hpp>
#include <memory>
#include <string>
#include <vector>

struct GenSession {
    enum class State { Running, AwaitingEditor, Staged, Done, Failed };
    struct Entry {
        enum class Kind { User, Assistant, Thinking, ToolCall, ToolResult, Status, Error };
        Kind kind;
        std::string text;
    };
    // Durable conversation memory — unlike `transcript` (a display log with
    // status/tool noise), `chat` holds only the user/assistant turns and is
    // what a resumed session's AI context is rebuilt from after a restart.
    // chatPush() folds overflow into `chatSummary` so context never explodes.
    struct ChatMsg { int role = 0; std::string text; };  // 0 = user, 1 = assistant
    int                          id = 0;
    std::string                  title;
    State                        state = State::Running;
    std::vector<Entry>           transcript;
    std::vector<ChatMsg>         chat;
    std::string                  chatSummary;     // digest of folded-away turns
    std::string                  targetLevelName; // persisted; re-resolves targetLevel
    std::string                  pendingEdit;     // edit follow-up waiting for its editor
    int                          pendingEditMode = 0;
    // The engine popup, type-erased to CCNode (complete type for Ref);
    // enginePtr is the typed alias — both always point at the same node.
    geode::Ref<cocos2d::CCNode>  engineRef;
    void*                        enginePtr = nullptr;
    // The level this generation targets. Adoption (and the go-to-level
    // button) match on this — entering any OTHER level's editor does
    // nothing; the session keeps waiting for its own level.
    geode::Ref<GJGameLevel>      targetLevel;
    int64_t                      startedAt = 0;
    bool                         needsRating = false;  // overlay shows 1-10 row

    // Feedback snapshot captured the moment the rating row is armed, so
    // rating an older session never picks up a newer generation's data
    // (the s_last* globals are last-write-wins across sessions).
    std::string fbPrompt, fbDifficulty, fbStyle, fbLength;
    std::string fbObjectsJson, fbEditedObjectsJson, fbEditSummary;
    bool        fbAccepted = false;
    int         fbRating   = 0;      // set by editoraiRateSession
    bool        fbShared   = false;  // telemetry Share already sent
    bool        restored   = false;  // loaded from disk — engine context gone

    void push(Entry::Kind k, std::string text) {
        // Rolling window: long conversations keep flowing — the OLDEST
        // entries fall off (a hard "stop recording" cap would silently
        // freeze the transcript mid-conversation).
        if (transcript.size() >= 400)
            transcript.erase(transcript.begin(),
                             transcript.begin() + (transcript.size() - 399));
        if (text.size() > 1200) { utf8Trim(text, 1200); text += " [...]"; }
        transcript.push_back({k, std::move(text)});
        void editoraiMarkSessionsDirty();      // fwd-decl; defined in main.cpp
        editoraiMarkSessionsDirty();
    }

    // The mod's own context manager: append a turn, then keep the live tail
    // under a fixed character budget by folding the oldest turns into
    // `chatSummary` (one digest line each). The summary itself is bounded by
    // keeping its head (the original request era) plus its newest tail —
    // so an arbitrarily long conversation always rebuilds into a prompt of
    // roughly summary + last-N-turns, never the whole history.
    // Trim to at most `cap` bytes without splitting a UTF-8 codepoint (a
    // half-codepoint garbles both the ImGui transcript and API payloads).
    static void utf8Trim(std::string& s, size_t cap) {
        if (s.size() <= cap) return;
        s.resize(cap);
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80)
            s.pop_back();
        if (!s.empty() && (unsigned char)s.back() >= 0xC0)
            s.pop_back();
    }

    void chatPush(int role, std::string text) {
        if (text.size() > 4000) { utf8Trim(text, 4000); text += " [...]"; }
        chat.push_back({role, std::move(text)});
        size_t total = 0;
        for (auto& m : chat) total += m.text.size() + 16;
        while (total > 36000 && chat.size() > 8) {
            auto& old = chat.front();
            total -= old.text.size() + 16;
            std::string line = old.role == 0 ? "User: " : "AI: ";
            std::string digest = old.text;
            bool cut = digest.size() > 220;
            utf8Trim(digest, 220);
            line += digest;
            if (cut) line += " ...";
            line += "\n";
            chatSummary += line;
            chat.erase(chat.begin());
        }
        if (chatSummary.size() > 8000) {
            std::string head = chatSummary.substr(0, 2000);
            std::string tail = chatSummary.substr(chatSummary.size() - 5000);
            utf8Trim(head, 2000);
            // Drop the tail's possibly-split leading codepoint too.
            while (!tail.empty() && ((unsigned char)tail.front() & 0xC0) == 0x80)
                tail.erase(tail.begin());
            chatSummary = head + "[... earlier turns condensed ...]\n" + tail;
        }
        void editoraiMarkSessionsDirty();
        editoraiMarkSessionsDirty();
    }
    const char* stateName() const {
        switch (state) {
            case State::Running:        return "running";
            case State::AwaitingEditor: return "ready - open an editor";
            case State::Staged:         return "staged for review";
            case State::Done:           return "done";
            case State::Failed:         return "failed";
        }
        return "?";
    }
};

// Registry (defined in main.cpp).
std::vector<std::shared_ptr<GenSession>>& genSessions();

// Engine actions the overlay can invoke (defined in main.cpp; safe no-ops
// when the session/engine is gone). mode: 0 = edit (change the level),
// 1 = plan (plan only, no objects), 2 = chat (answer only, no script).
void editoraiSendFollowUp(const std::shared_ptr<GenSession>& session,
                          const std::string& text, int mode = 0);
void editoraiCancelSession(const std::shared_ptr<GenSession>& session);
// Opens a fresh generator session: only valid inside an editor; returns
// false (overlay shows a hint) otherwise.
bool editoraiOpenGenerator();

// True while a GD text box has keyboard focus (tracked in main.cpp) — the
// overlay hotkey must not fire mid-typing.
bool editoraiIsGDTextInputActive();

// ── Overlay-initiated generation (level targeting) ──────────────────────────
// target: -2 = create a brand-new level, -1 = current editor,
//         >= 0 = index into the local-levels list below.
struct LocalLevelInfo { std::string name; int objectCount = 0; };
std::vector<LocalLevelInfo> editoraiListLocalLevels();
// Starts (or queues) a generation against the chosen target. Opens the
// editor scene when needed; generation begins once the editor is ready.
// Returns false with a human-readable reason in err. For target >= 0 pass
// the level name shown in the list — the local-levels array can reorder
// between listing and use, so the name re-validates (and re-finds) the
// target instead of trusting a stale index.
bool editoraiStartGeneration(int target, const std::string& prompt,
                             bool replaceContents, std::string& err,
                             const std::string& expectedName = "");

// Inline rating from the overlay (replaces the old RatingPopup flow).
void editoraiRateSession(const std::shared_ptr<GenSession>& session, int rating);

// "Go to level" (the small bottom-right overlay button): opens the editor
// for the session's target level so a waiting blueprint can stage. Returns
// false when travel is not possible right now (already inside a level or
// another editor, or the target is gone).
bool editoraiGoToSessionLevel(const std::shared_ptr<GenSession>& session);

// Settings bridge for the overlay's Settings tab (thin wrappers over
// Mod::get()->getSettingValue/setSettingValue; defined in main.cpp).
std::string editoraiGetStr(const char* id);
void        editoraiSetStr(const char* id, const std::string& v);
bool        editoraiGetBool(const char* id);
void        editoraiSetBool(const char* id, bool v);
int64_t     editoraiGetInt(const char* id);
void        editoraiSetInt(const char* id, int64_t v);

// Saved-value bridge (Geode saved.json — persists overlay inputs, theme
// colors, and anything else that isn't a mod.json setting).
std::string editoraiGetSavedStr(const char* key, const std::string& def = "");
void        editoraiSetSavedStr(const char* key, const std::string& v);
int64_t     editoraiGetSavedInt(const char* key, int64_t def = 0);
void        editoraiSetSavedInt(const char* key, int64_t v);

// ── Session persistence. Sessions survive game restarts fully conversable:
// the chat memory (chat + chatSummary) is serialized, and the engine is
// rebuilt lazily the next time the user sends a message ─────────────────────
void editoraiMarkSessionsDirty();
void editoraiPersistSessionsIfDirty();   // throttled; overlay ticks this

// Telemetry share (the overlay rating row's Share button; opt-in, 8+ only).
bool editoraiShareSession(const std::shared_ptr<GenSession>& session,
                          std::string& err);

// ── Saved (online) levels — example/style reference pickers ────────────────
struct SavedLevelInfo { std::string name; int levelId = 0; };
std::vector<SavedLevelInfo> editoraiListSavedLevels();

// ── Headless OAuth (driven from the overlay's per-frame tick) ───────────────
bool        editoraiOAuthAvailable(const std::string& provider);
bool        editoraiOAuthStart(const std::string& provider);
void        editoraiOAuthTick(float dt);
std::string editoraiOAuthStatus();
bool        editoraiOAuthActive();
