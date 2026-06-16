#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <random>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
// cocos2d's pre-linked zlib wrapper (ZipUtils::ccInflateMemory) is used to
// decompress gzip-encoded level data fetched from the GD servers.
// Base64 goes through Geode's own utils/base64 (GEODE_DLL, exported from the
// loader on EVERY platform) — cocos2d::base64Decode is NOT exported on
// macOS/iOS and fails to link there.
#include <Geode/cocos/support/zip_support/ZipUtils.h>
#include <Geode/utils/base64.hpp>
#include <span>
// ── BEGIN inlined headers (formerly src/json_lenient.hpp, src/example_sections_data.hpp, src/tool_use.hpp) ─────────
// Lenient JSON parser for AI output.
//
// Small LLMs (esp. our 1.5B fine-tune) occasionally emit JSON with one of
// these recoverable mistakes:
//   - line comments (// ...) and block comments (/* ... */)
//   - trailing commas before } or ]
//   - single-quoted strings ('foo')
//   - bare (unquoted) object keys ({foo: 1})
//   - missing trailing braces/brackets (output cut off)
//
// parseJsonLenient() tries strict matjson::parse first; on failure, applies
// progressively more aggressive normalizations and retries. Every transform
// is STRING-AWARE — characters inside JSON string literals are never modified.
//
// All transforms are pure; the original string is never mutated. If every
// normalization fails, returns the ORIGINAL matjson error so callers can log
// the real reason.

#include <Geode/loader/Log.hpp>
#include <matjson.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace editorai::json_lenient {

// ── Internal helpers ─────────────────────────────────────────────────────────

// Strip /* ... */ and // ... comments. String-aware. Block-comment terminator
// is "*/"; line-comment terminator is newline or EOF.
inline std::string stripComments(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool inStr = false, esc = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (esc) { esc = false; out.push_back(c); continue; }
        if (inStr) {
            if (c == '\\') { esc = true; out.push_back(c); continue; }
            if (c == '"')  { inStr = false; out.push_back(c); continue; }
            out.push_back(c);
            continue;
        }
        // not in string
        if (c == '"') { inStr = true; out.push_back(c); continue; }
        // detect "//" line comment
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            i += 2;
            while (i < in.size() && in[i] != '\n') ++i;
            // keep the newline so line-based parsers still work
            if (i < in.size()) out.push_back('\n');
            continue;
        }
        // detect "/* */" block comment
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            if (i + 1 < in.size()) i += 1; // jump to the '/' of "*/"
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// Remove trailing commas: "...,}" → "...}" and "...,]" → "...]".
// Only when the comma is the LAST non-whitespace character before } or ].
// String-aware.
inline std::string stripTrailingCommas(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool inStr = false, esc = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (esc) { esc = false; out.push_back(c); continue; }
        if (inStr) {
            if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            out.push_back(c);
            continue;
        }
        if (c == '"') { inStr = true; out.push_back(c); continue; }
        if (c == ',') {
            // peek forward through whitespace for the next non-ws char
            size_t k = i + 1;
            while (k < in.size() && (in[k] == ' ' || in[k] == '\t' ||
                                     in[k] == '\n' || in[k] == '\r')) ++k;
            if (k < in.size() && (in[k] == '}' || in[k] == ']')) {
                // drop the comma; keep trailing whitespace as-is
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

// Convert single-quoted strings to double-quoted strings. We only touch a
// run that starts with ' OUTSIDE a double-quoted string and contains no
// double quote. (If it does contain a double quote, transformation would
// produce invalid JSON; leave it alone and let it fail.)
inline std::string singleToDoubleQuotes(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool inDouble = false, esc = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (esc) { esc = false; out.push_back(c); continue; }
        if (inDouble) {
            if (c == '\\') esc = true;
            else if (c == '"') inDouble = false;
            out.push_back(c);
            continue;
        }
        if (c == '"') { inDouble = true; out.push_back(c); continue; }
        if (c == '\'') {
            // scan forward for the closing single quote
            size_t k = i + 1;
            bool hasDoubleQuote = false;
            bool sawEnd = false;
            while (k < in.size()) {
                if (in[k] == '\\' && k + 1 < in.size()) { k += 2; continue; }
                if (in[k] == '"') hasDoubleQuote = true;
                if (in[k] == '\'') { sawEnd = true; break; }
                ++k;
            }
            if (sawEnd && !hasDoubleQuote) {
                out.push_back('"');
                for (size_t j = i + 1; j < k; ++j) out.push_back(in[j]);
                out.push_back('"');
                i = k;
                continue;
            }
            // else: bail, keep verbatim
        }
        out.push_back(c);
    }
    return out;
}

// Quote bare object keys: `{ foo: 1 }` → `{ "foo": 1 }`. Only triggers when
// we see an identifier (alphanumeric/underscore) followed by optional
// whitespace and a colon, after a `{` or `,` (key position). String-aware.
inline std::string quoteBareKeys(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 16);
    bool inStr = false, esc = false;
    // Track whether we're at a position where a key would appear
    // (right after '{' or ',' inside an object).
    bool keyPosition = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (esc) { esc = false; out.push_back(c); continue; }
        if (inStr) {
            if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            out.push_back(c);
            continue;
        }
        if (c == '"') { inStr = true; out.push_back(c); keyPosition = false; continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            out.push_back(c);
            continue;
        }
        if (c == '{' || c == ',') { keyPosition = true; out.push_back(c); continue; }
        if (c == '}' || c == ']' || c == '[' || c == ':') {
            keyPosition = false;
            out.push_back(c);
            continue;
        }
        if (keyPosition && (std::isalpha((unsigned char)c) || c == '_' || c == '$')) {
            // collect identifier chars
            size_t k = i;
            while (k < in.size() && (std::isalnum((unsigned char)in[k]) ||
                                     in[k] == '_' || in[k] == '$')) ++k;
            // peek for ':' after optional ws
            size_t p = k;
            while (p < in.size() && (in[p] == ' ' || in[p] == '\t')) ++p;
            if (p < in.size() && in[p] == ':') {
                out.push_back('"');
                for (size_t j = i; j < k; ++j) out.push_back(in[j]);
                out.push_back('"');
                i = k - 1;          // for-loop ++ will land on k
                keyPosition = false;
                continue;
            }
            // not a key, keep as-is
            for (size_t j = i; j < k; ++j) out.push_back(in[j]);
            i = k - 1;
            keyPosition = false;
            continue;
        }
        keyPosition = false;
        out.push_back(c);
    }
    return out;
}

// Auto-close unbalanced { [ by counting. String-aware. If we end with N
// unclosed structures, append the appropriate sequence. We never REMOVE
// existing closers — only add.
inline std::string autoCloseBrackets(std::string_view in) {
    // Track structure stack to know whether each unclosed thing was { or [
    std::vector<char> stack;
    bool inStr = false, esc = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (esc) { esc = false; continue; }
        if (inStr) {
            if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{' || c == '[') stack.push_back(c);
        else if (c == '}' && !stack.empty() && stack.back() == '{') stack.pop_back();
        else if (c == ']' && !stack.empty() && stack.back() == '[') stack.pop_back();
        else if ((c == '}' || c == ']') && !stack.empty()) {
            // Mismatched closer (e.g. '}' against '['): treat it as closing
            // the open structure anyway. Leaving the opener on the stack
            // would append a SECOND closer at the end — the repair would
            // worsen already-broken input instead of giving the lenient
            // parser its best shot.
            stack.pop_back();
        }
    }
    if (inStr) {
        // unterminated string — close it
        std::string out(in);
        out.push_back('"');
        // and close remaining structures
        while (!stack.empty()) {
            out.push_back(stack.back() == '{' ? '}' : ']');
            stack.pop_back();
        }
        return out;
    }
    if (stack.empty()) return std::string(in);
    std::string out(in);
    // Strip any trailing partial token (last comma/colon would leave invalid input)
    while (!out.empty() && (out.back() == ',' || out.back() == ':' || out.back() == ' '
                            || out.back() == '\t' || out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    while (!stack.empty()) {
        out.push_back(stack.back() == '{' ? '}' : ']');
        stack.pop_back();
    }
    return out;
}

// ── Public entry point ──────────────────────────────────────────────────────

struct Result {
    bool ok = false;
    matjson::Value value;
    std::string error;        // last error from matjson if all fail
    std::string transformed;  // final string we actually parsed (for debugging)
    int fixesApplied = 0;     // count of normalization passes that helped
};

// Try strict parse, then progressive normalizations. Each transform is added
// CUMULATIVELY (each step inherits the previous step's output) so we can fix
// multiple co-occurring issues.
inline Result parse(std::string_view input) {
    Result r;
    // 0. strict — matjson::parse takes a string_view, no copy needed
    {
        auto p = matjson::parse(input);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = input; return r; }
        r.error = "strict parse failed";
    }
    // Cumulative fix passes share one rolling buffer: each transform reads
    // the previous result and replaces it, so at most two copies of the text
    // are alive at any moment (the old chain kept all five).
    std::string work = stripComments(input);                       // 1
    {
        auto p = matjson::parse(work);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = work; r.fixesApplied = 1; return r; }
    }
    work = stripTrailingCommas(work);                              // 2
    {
        auto p = matjson::parse(work);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = work; r.fixesApplied = 2; return r; }
    }
    work = singleToDoubleQuotes(work);                             // 3
    {
        auto p = matjson::parse(work);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = work; r.fixesApplied = 3; return r; }
    }
    work = quoteBareKeys(work);                                    // 4
    {
        auto p = matjson::parse(work);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = work; r.fixesApplied = 4; return r; }
    }
    work = autoCloseBrackets(work);  // 5 — last because it's the most destructive
    {
        auto p = matjson::parse(work);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = work; r.fixesApplied = 5; return r; }
    }
    // give up
    r.transformed = std::move(work);
    return r;
}

} // namespace editorai::json_lenient

// AUTO-GENERATED FROM resources/example_sections.json (v2 — 21 sections, 7 levels)
//
// Source levels:
//   - Back on Track v2 by IIINePtunEIII   (retro / medium)
//   - Stereo Madness v2 by Sumsar          (retro / easy)
//   - Cataclysm by Ggb0y                   (dark-decorated / extreme demon)
//   - Aquarius by Skitten                  (neon-modern / hard, pulse-heavy)
//   - Acid Factory by UsernameDefault      (industrial-modern / hard)
//   - Promises                             (clean-modern / medium)
//   - Adrift                               (ambient-calm / medium, flow)
//
// Each section is ~500 X-units wide, X normalized to 0, up to ~75 known objects.
// Each example includes a hand-written description so the AI sees more than
// just "decorated/hard" — it gets concrete style notes.

#include <string_view>

namespace editorai {

inline constexpr std::string_view EXAMPLE_SECTIONS_JSON = R"eai_v2(
{
  "_comment": "EditorAI few-shot examples. X normalized to start at 0 per section.",
  "_version": 2,
  "examples": [
    {"level_name":"Back on Track v2","author":"IIINePtunEIII","theme":"classic-retro","difficulty":"medium","tags":["retro","easy","platforming","classic"],"description":"A clean retro-style remake of the classic GD level 'Back on Track'. Spike trains, simple jump arcs, gradient blocks, occasional dual portals. Very symmetric and rhythmic; ideal reference for medium-difficulty cube gameplay.","orig_x_range":[13500,14000],"object_count":75,"objects":[{"type":"block_black_gradient_square","x":15,"y":195},{"type":"block_black_gradient_square","x":45,"y":165},{"type":"block_black_gradient_square","x":75,"y":135},{"type":"block_grid_patterned_inner_square","x":15,"y":285},{"type":"block_grid_patterned_inner_square","x":45,"y":285},{"type":"block_grid_patterned_inner_square","x":75,"y":285},{"type":"block_grid_patterned_inner_square","x":15,"y":255},{"type":"block_grid_patterned_inner_square","x":45,"y":255},{"type":"block_grid_patterned_inner_square","x":75,"y":255},{"type":"block_grid_patterned_inner_square","x":75,"y":165},{"type":"block_grid_patterned_inner_square","x":75,"y":195},{"type":"block_grid_patterned_inner_square","x":75,"y":225},{"type":"block_grid_patterned_inner_square","x":45,"y":225},{"type":"block_grid_patterned_inner_square","x":45,"y":195},{"type":"block_grid_patterned_inner_square","x":15,"y":225},{"type":"decor_medium_rod","x":75,"y":106},{"type":"block_black_gradient_square","x":75,"y":165},{"type":"block_black_gradient_square","x":45,"y":195},{"type":"block_black_gradient_square","x":15,"y":225},{"type":"block_black_gradient_square","x":105,"y":135},{"type":"block_black_gradient_square","x":135,"y":135},{"type":"block_black_gradient_square","x":165,"y":135},{"type":"block_black_gradient_square","x":195,"y":135},{"type":"block_grid_patterned_inner_square","x":105,"y":285},{"type":"block_grid_patterned_inner_square","x":135,"y":285},{"type":"block_grid_patterned_inner_square","x":165,"y":285},{"type":"block_grid_patterned_inner_square","x":195,"y":285},{"type":"block_grid_patterned_inner_square","x":105,"y":255},{"type":"block_grid_patterned_inner_square","x":135,"y":255},{"type":"block_grid_patterned_inner_square","x":165,"y":255},{"type":"block_grid_patterned_inner_square","x":195,"y":255},{"type":"block_grid_patterned_inner_square","x":105,"y":225},{"type":"block_grid_patterned_inner_square","x":135,"y":225},{"type":"block_grid_patterned_inner_square","x":135,"y":195},{"type":"block_grid_patterned_inner_square","x":165,"y":195},{"type":"block_grid_patterned_inner_square","x":195,"y":195},{"type":"block_grid_patterned_inner_square","x":105,"y":195},{"type":"block_grid_patterned_inner_square","x":135,"y":165},{"type":"block_grid_patterned_inner_square","x":105,"y":165},{"type":"block_grid_patterned_inner_square","x":195,"y":165},{"type":"block_grid_patterned_inner_square","x":165,"y":165},{"type":"block_grid_patterned_inner_square","x":195,"y":225},{"type":"block_grid_patterned_inner_square","x":165,"y":225},{"type":"block_black_gradient_square","x":225,"y":135},{"type":"block_black_gradient_square","x":255,"y":135},{"type":"block_black_gradient_square","x":285,"y":135},{"type":"block_grid_patterned_inner_square","x":225,"y":285},{"type":"block_grid_patterned_inner_square","x":255,"y":285},{"type":"block_grid_patterned_inner_square","x":285,"y":285},{"type":"block_grid_patterned_inner_square","x":225,"y":255},{"type":"block_grid_patterned_inner_square","x":255,"y":255},{"type":"block_grid_patterned_inner_square","x":285,"y":255},{"type":"block_grid_patterned_inner_square","x":225,"y":195},{"type":"block_grid_patterned_inner_square","x":255,"y":225},{"type":"block_grid_patterned_inner_square","x":225,"y":165},{"type":"block_grid_patterned_inner_square","x":255,"y":165},{"type":"block_grid_patterned_inner_square","x":255,"y":195},{"type":"block_grid_patterned_inner_square","x":225,"y":225},{"type":"block_grid_patterned_inner_square","x":285,"y":165},{"type":"block_grid_patterned_inner_square","x":285,"y":195},{"type":"block_grid_patterned_inner_square","x":285,"y":225},{"type":"decor_medium_rod","x":285,"y":106},{"type":"block_black_gradient_square","x":285,"y":165},{"type":"block_black_gradient_square","x":315,"y":165},{"type":"block_black_gradient_square","x":345,"y":195},{"type":"block_black_gradient_square","x":375,"y":225},{"type":"block_grid_patterned_inner_square","x":315,"y":285},{"type":"block_grid_patterned_inner_square","x":345,"y":285},{"type":"block_grid_patterned_inner_square","x":375,"y":285},{"type":"block_grid_patterned_inner_square","x":315,"y":255},{"type":"block_grid_patterned_inner_square","x":345,"y":225},{"type":"block_grid_patterned_inner_square","x":315,"y":225},{"type":"block_grid_patterned_inner_square","x":315,"y":195},{"type":"block_grid_patterned_inner_square","x":345,"y":255},{"type":"block_grid_patterned_inner_square","x":375,"y":255}]},
    {"level_name":"Back on Track v2","author":"IIINePtunEIII","theme":"classic-retro","difficulty":"medium","tags":["retro","easy","platforming","classic"],"description":"A clean retro-style remake of the classic GD level 'Back on Track'. Spike trains, simple jump arcs, gradient blocks, occasional dual portals. Very symmetric and rhythmic; ideal reference for medium-difficulty cube gameplay.","orig_x_range":[14500,15000],"object_count":75,"objects":[{"type":"block_black_gradient_square","x":35,"y":75},{"type":"block_black_gradient_square","x":65,"y":75},{"type":"block_black_gradient_square","x":95,"y":75},{"type":"block_grid_patterned_inner_square","x":5,"y":285},{"type":"block_grid_patterned_inner_square","x":35,"y":285},{"type":"block_grid_patterned_inner_square","x":65,"y":285},{"type":"block_grid_patterned_inner_square","x":95,"y":285},{"type":"block_grid_patterned_inner_square","x":95,"y":255},{"type":"block_grid_patterned_inner_square","x":65,"y":255},{"type":"block_grid_patterned_inner_square","x":35,"y":255},{"type":"block_grid_patterned_inner_square","x":5,"y":255},{"type":"block_grid_patterned_inner_square","x":5,"y":225},{"type":"block_grid_patterned_inner_square","x":35,"y":225},{"type":"block_grid_patterned_inner_square","x":65,"y":225},{"type":"block_grid_patterned_inner_square","x":95,"y":225},{"type":"block_grid_patterned_inner_square","x":5,"y":195},{"type":"block_grid_patterned_inner_square","x":95,"y":195},{"type":"block_grid_patterned_inner_square","x":65,"y":195},{"type":"block_grid_patterned_inner_square","x":35,"y":195},{"type":"block_grid_patterned_inner_square","x":5,"y":165},{"type":"block_grid_patterned_inner_square","x":35,"y":165},{"type":"block_grid_patterned_inner_square","x":65,"y":165},{"type":"block_grid_patterned_inner_square","x":95,"y":165},{"type":"block_grid_patterned_inner_square","x":35,"y":135},{"type":"block_grid_patterned_inner_square","x":65,"y":135},{"type":"block_grid_patterned_inner_square","x":95,"y":135},{"type":"block_grid_patterned_inner_square","x":95,"y":105},{"type":"block_grid_patterned_inner_square","x":35,"y":105},{"type":"block_grid_patterned_inner_square","x":65,"y":105},{"type":"block_grid_patterned_inner_square","x":5,"y":135},{"type":"block_black_gradient_square","x":5,"y":105},{"type":"decor_medium_rod","x":35,"y":46},{"type":"block_black_gradient_square","x":35,"y":105},{"type":"block_black_gradient_square","x":5,"y":135},{"type":"block_black_gradient_square","x":125,"y":75},{"type":"block_black_gradient_square","x":155,"y":75},{"type":"block_grid_patterned_inner_square","x":125,"y":285},{"type":"block_grid_patterned_inner_square","x":155,"y":285},{"type":"block_grid_patterned_inner_square","x":155,"y":255},{"type":"block_grid_patterned_inner_square","x":125,"y":255},{"type":"block_grid_patterned_inner_square","x":125,"y":225},{"type":"block_grid_patterned_inner_square","x":155,"y":225},{"type":"block_grid_patterned_inner_square","x":155,"y":195},{"type":"block_grid_patterned_inner_square","x":125,"y":195},{"type":"block_grid_patterned_inner_square","x":125,"y":165},{"type":"block_grid_patterned_inner_square","x":155,"y":165},{"type":"block_grid_patterned_inner_square","x":125,"y":135},{"type":"block_grid_patterned_inner_square","x":155,"y":135},{"type":"block_grid_patterned_inner_square","x":155,"y":105},{"type":"block_grid_patterned_inner_square","x":125,"y":105},{"type":"block_black_gradient_square","x":185,"y":75},{"type":"block_grid_patterned_inner_square","x":185,"y":285},{"type":"block_grid_patterned_inner_square","x":185,"y":255},{"type":"block_grid_patterned_inner_square","x":185,"y":225},{"type":"block_grid_patterned_inner_square","x":185,"y":195},{"type":"block_grid_patterned_inner_square","x":185,"y":165},{"type":"block_grid_patterned_inner_square","x":185,"y":135},{"type":"block_grid_patterned_inner_square","x":185,"y":105},{"type":"decor_medium_rod","x":125,"y":46},{"type":"block_black_gradient_square","x":215,"y":75},{"type":"block_grid_patterned_inner_square","x":215,"y":285},{"type":"block_grid_patterned_inner_square","x":215,"y":255},{"type":"block_grid_patterned_inner_square","x":215,"y":225},{"type":"block_grid_patterned_inner_square","x":215,"y":195},{"type":"block_grid_patterned_inner_square","x":215,"y":165},{"type":"block_grid_patterned_inner_square","x":215,"y":135},{"type":"block_grid_patterned_inner_square","x":215,"y":105},{"type":"block_black_gradient_square","x":215,"y":105},{"type":"block_black_gradient_square","x":245,"y":105},{"type":"block_grid_patterned_inner_square","x":245,"y":285},{"type":"block_grid_patterned_inner_square","x":275,"y":285},{"type":"block_grid_patterned_inner_square","x":275,"y":255},{"type":"block_grid_patterned_inner_square","x":245,"y":255},{"type":"block_grid_patterned_inner_square","x":245,"y":225},{"type":"block_grid_patterned_inner_square","x":275,"y":225}]},
    {"level_name":"Back on Track v2","author":"IIINePtunEIII","theme":"classic-retro","difficulty":"medium","tags":["retro","easy","platforming","classic"],"description":"A clean retro-style remake of the classic GD level 'Back on Track'. Spike trains, simple jump arcs, gradient blocks, occasional dual portals. Very symmetric and rhythmic; ideal reference for medium-difficulty cube gameplay.","orig_x_range":[15500,16000],"object_count":75,"objects":[{"type":"block_black_gradient_square","x":25,"y":135},{"type":"block_black_gradient_square","x":55,"y":135},{"type":"block_black_gradient_square","x":85,"y":135},{"type":"block_grid_patterned_inner_square","x":25,"y":15},{"type":"block_grid_patterned_inner_square","x":55,"y":15},{"type":"block_grid_patterned_inner_square","x":85,"y":15},{"type":"block_grid_patterned_inner_square","x":25,"y":45},{"type":"block_grid_patterned_inner_square","x":55,"y":45},{"type":"block_grid_patterned_inner_square","x":85,"y":45},{"type":"block_grid_patterned_inner_square","x":25,"y":75},{"type":"block_grid_patterned_inner_square","x":55,"y":75},{"type":"block_grid_patterned_inner_square","x":85,"y":75},{"type":"block_grid_patterned_inner_square","x":25,"y":105},{"type":"block_grid_patterned_inner_square","x":55,"y":105},{"type":"block_grid_patterned_inner_square","x":85,"y":105},{"type":"block_grid_patterned_inner_square","x":25,"y":285},{"type":"block_grid_patterned_inner_square","x":55,"y":285},{"type":"block_grid_patterned_inner_square","x":85,"y":285},{"type":"block_black_gradient_square","x":25,"y":255},{"type":"block_black_gradient_square","x":85,"y":255},{"type":"block_black_gradient_square","x":55,"y":255},{"type":"spike_black_gradient_spike","x":85,"y":165},{"type":"decor_medium_rod","x":55,"y":226},{"type":"block_black_gradient_square","x":115,"y":135},{"type":"block_black_gradient_square","x":145,"y":135},{"type":"block_black_gradient_square","x":175,"y":135},{"type":"block_grid_patterned_inner_square","x":115,"y":15},{"type":"block_grid_patterned_inner_square","x":145,"y":15},{"type":"block_grid_patterned_inner_square","x":175,"y":15},{"type":"block_grid_patterned_inner_square","x":115,"y":45},{"type":"block_grid_patterned_inner_square","x":145,"y":45},{"type":"block_grid_patterned_inner_square","x":175,"y":45},{"type":"block_grid_patterned_inner_square","x":115,"y":75},{"type":"block_grid_patterned_inner_square","x":145,"y":75},{"type":"block_grid_patterned_inner_square","x":115,"y":105},{"type":"block_grid_patterned_inner_square","x":145,"y":105},{"type":"block_grid_patterned_inner_square","x":175,"y":105},{"type":"block_grid_patterned_inner_square","x":175,"y":75},{"type":"block_grid_patterned_inner_square","x":115,"y":285},{"type":"block_grid_patterned_inner_square","x":145,"y":285},{"type":"block_grid_patterned_inner_square","x":175,"y":285},{"type":"block_black_gradient_square","x":115,"y":255},{"type":"block_black_gradient_square","x":145,"y":255},{"type":"block_black_gradient_square","x":175,"y":255},{"type":"spike_black_gradient_spike","x":115,"y":165},{"type":"spike_black_gradient_spike","x":145,"y":165},{"type":"spike_black_gradient_spike","x":175,"y":165},{"type":"block_black_gradient_square","x":205,"y":135},{"type":"block_black_gradient_square","x":235,"y":135},{"type":"block_black_gradient_square","x":265,"y":135},{"type":"block_black_gradient_square","x":295,"y":135},{"type":"block_grid_patterned_inner_square","x":205,"y":15},{"type":"block_grid_patterned_inner_square","x":235,"y":15},{"type":"block_grid_patterned_inner_square","x":265,"y":15},{"type":"block_grid_patterned_inner_square","x":295,"y":15},{"type":"block_grid_patterned_inner_square","x":205,"y":45},{"type":"block_grid_patterned_inner_square","x":235,"y":45},{"type":"block_grid_patterned_inner_square","x":265,"y":45},{"type":"block_grid_patterned_inner_square","x":295,"y":45},{"type":"block_grid_patterned_inner_square","x":205,"y":105},{"type":"block_grid_patterned_inner_square","x":235,"y":105},{"type":"block_grid_patterned_inner_square","x":265,"y":105},{"type":"block_grid_patterned_inner_square","x":295,"y":105},{"type":"block_grid_patterned_inner_square","x":205,"y":75},{"type":"block_grid_patterned_inner_square","x":235,"y":75},{"type":"block_grid_patterned_inner_square","x":265,"y":75},{"type":"block_grid_patterned_inner_square","x":295,"y":75},{"type":"block_grid_patterned_inner_square","x":205,"y":285},{"type":"block_grid_patterned_inner_square","x":235,"y":285},{"type":"block_grid_patterned_inner_square","x":265,"y":285},{"type":"block_grid_patterned_inner_square","x":295,"y":285},{"type":"block_black_gradient_square","x":205,"y":255},{"type":"block_black_gradient_square","x":235,"y":255},{"type":"block_black_gradient_square","x":265,"y":255},{"type":"block_black_gradient_square","x":295,"y":255}]},
    {"level_name":"Stereo Madness v2","author":"Sumsar","theme":"classic-retro","difficulty":"easy","tags":["retro","easy","classic","cube-heavy"],"description":"Modern remake of Stereo Madness with grid-patterned blocks, lots of decoration clouds, and gentle spike/orb progression. Showcases legible cube gameplay with classic GD vibe \u2014 heavy on platform stacks at fixed heights.","orig_x_range":[4500,5000],"object_count":75,"objects":[{"type":"block_grid_patterned_top_square","x":15,"y":135},{"type":"block_grid_patterned_top_square","x":45,"y":135},{"type":"block_grid_patterned_top_square","x":75,"y":135},{"type":"block_grid_patterned_inner_square","x":15,"y":105},{"type":"block_grid_patterned_inner_square","x":15,"y":75},{"type":"block_grid_patterned_inner_square","x":15,"y":45},{"type":"block_grid_patterned_inner_square","x":15,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":75,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":45,"y":4},{"type":"block_grid_patterned_inner_square","x":75,"y":105},{"type":"block_grid_patterned_inner_square","x":75,"y":75},{"type":"block_grid_patterned_inner_square","x":75,"y":45},{"type":"block_grid_patterned_inner_square","x":75,"y":15},{"type":"decor_short_rod","x":34,"y":161},{"type":"block_grid_patterned_top_square","x":105,"y":135},{"type":"block_grid_patterned_top_square","x":135,"y":135},{"type":"spike_black_gradient_spike","x":105,"y":165},{"type":"spike_black_gradient_spike","x":135,"y":165},{"type":"block_grid_patterned_top_square","x":165,"y":135},{"type":"spike_black_gradient_spike","x":165,"y":165},{"type":"block_grid_patterned_top_square","x":195,"y":135},{"type":"spike_black_gradient_spike","x":195,"y":165},{"type":"block_grid_patterned_inner_square","x":165,"y":105},{"type":"block_grid_patterned_inner_square","x":165,"y":75},{"type":"block_grid_patterned_inner_square","x":165,"y":45},{"type":"block_grid_patterned_inner_square","x":165,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":105,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":135,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":195,"y":4},{"type":"block_grid_patterned_inner_square","x":105,"y":105},{"type":"block_grid_patterned_inner_square","x":105,"y":75},{"type":"block_grid_patterned_inner_square","x":105,"y":45},{"type":"block_grid_patterned_inner_square","x":105,"y":15},{"type":"decor_short_rod","x":187,"y":203,"rotation":90.0},{"type":"decor_short_rod","x":113,"y":203,"rotation":270.0},{"type":"block_grid_patterned_top_square","x":225,"y":135},{"type":"block_grid_patterned_top_square","x":255,"y":135},{"type":"block_grid_patterned_top_square","x":285,"y":135},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":225,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":255,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":285,"y":4},{"type":"block_grid_patterned_inner_square","x":225,"y":105},{"type":"block_grid_patterned_inner_square","x":225,"y":75},{"type":"block_grid_patterned_inner_square","x":225,"y":45},{"type":"block_grid_patterned_inner_square","x":225,"y":15},{"type":"block_grid_patterned_inner_square","x":255,"y":75},{"type":"block_grid_patterned_inner_square","x":255,"y":45},{"type":"block_grid_patterned_inner_square","x":255,"y":15},{"type":"block_grid_patterned_inner_square","x":255,"y":105},{"type":"block_grid_patterned_top_square","x":315,"y":135},{"type":"block_grid_patterned_outer_corner_square","x":375,"y":135,"rotation":90.0},{"type":"block_grid_patterned_inner_corner_square","x":375,"y":105,"rotation":90.0},{"type":"block_grid_patterned_top_square","x":345,"y":135},{"type":"block_grid_patterned_inner_square","x":315,"y":105},{"type":"block_grid_patterned_inner_square","x":315,"y":75},{"type":"block_grid_patterned_inner_square","x":315,"y":45},{"type":"block_grid_patterned_inner_square","x":315,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":375,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":345,"y":4},{"type":"decor_medium_rod","x":364,"y":168},{"type":"block_grid_patterned_inner_square","x":375,"y":75},{"type":"block_grid_patterned_inner_square","x":375,"y":45},{"type":"block_grid_patterned_inner_square","x":375,"y":15},{"type":"decor_short_rod","x":319,"y":161},{"type":"block_grid_patterned_top_square","x":435,"y":105},{"type":"block_grid_patterned_top_square","x":465,"y":105},{"type":"block_grid_patterned_top_square","x":405,"y":105},{"type":"block_grid_patterned_top_square","x":495,"y":105},{"type":"block_black_gradient_single_slab","x":495,"y":165},{"type":"spike_black_gradient_spike","x":495,"y":187},{"type":"block_grid_patterned_inner_square","x":465,"y":75},{"type":"block_grid_patterned_inner_square","x":465,"y":45},{"type":"block_grid_patterned_inner_square","x":465,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":405,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":435,"y":4}]},
    {"level_name":"Stereo Madness v2","author":"Sumsar","theme":"classic-retro","difficulty":"easy","tags":["retro","easy","classic","cube-heavy"],"description":"Modern remake of Stereo Madness with grid-patterned blocks, lots of decoration clouds, and gentle spike/orb progression. Showcases legible cube gameplay with classic GD vibe \u2014 heavy on platform stacks at fixed heights.","orig_x_range":[11500,12000],"object_count":75,"objects":[{"type":"block_grid_patterned_outer_corner_square","x":65,"y":165},{"type":"block_grid_patterned_top_square","x":65,"y":135,"rotation":-90.0},{"type":"block_grid_patterned_top_square","x":65,"y":105,"rotation":-90.0},{"type":"block_grid_patterned_outer_corner_square","x":65,"y":315,"rotation":-90.0},{"type":"block_grid_patterned_top_square","x":65,"y":345,"rotation":-90.0},{"type":"block_grid_patterned_top_square","x":65,"y":375,"rotation":-90.0},{"type":"block_grid_patterned_top_square","x":65,"y":405,"rotation":-90.0},{"type":"block_grid_patterned_top_square","x":65,"y":435,"rotation":-90.0},{"type":"block_grid_patterned_top_square","x":95,"y":165},{"type":"block_grid_patterned_top_square","x":95,"y":315},{"type":"block_grid_patterned_inner_square","x":95,"y":105},{"type":"block_grid_patterned_inner_square","x":95,"y":135},{"type":"block_grid_patterned_inner_square","x":95,"y":345},{"type":"block_grid_patterned_inner_square","x":95,"y":375},{"type":"block_grid_patterned_inner_square","x":95,"y":405},{"type":"block_grid_patterned_inner_square","x":95,"y":435},{"type":"block_grid_patterned_inner_square","x":95,"y":75},{"type":"decor_short_rod","x":5,"y":95},{"type":"decor_short_rod","x":5,"y":385},{"type":"block_grid_patterned_top_square","x":125,"y":165},{"type":"block_grid_patterned_top_square","x":125,"y":315},{"type":"block_grid_patterned_inner_square","x":125,"y":105},{"type":"block_grid_patterned_inner_square","x":125,"y":135},{"type":"block_grid_patterned_inner_square","x":125,"y":345},{"type":"block_grid_patterned_inner_square","x":125,"y":375},{"type":"block_grid_patterned_inner_square","x":125,"y":405},{"type":"block_grid_patterned_inner_square","x":125,"y":435},{"type":"block_grid_patterned_top_square","x":155,"y":165},{"type":"block_grid_patterned_top_square","x":155,"y":315},{"type":"block_grid_patterned_inner_square","x":155,"y":105},{"type":"block_grid_patterned_inner_square","x":155,"y":135},{"type":"block_grid_patterned_inner_square","x":155,"y":345},{"type":"block_grid_patterned_inner_square","x":155,"y":375},{"type":"block_grid_patterned_inner_square","x":155,"y":405},{"type":"block_grid_patterned_inner_square","x":155,"y":435},{"type":"block_grid_patterned_top_square","x":185,"y":315},{"type":"block_grid_patterned_inner_square","x":185,"y":345},{"type":"block_grid_patterned_inner_square","x":185,"y":375},{"type":"block_grid_patterned_inner_square","x":185,"y":405},{"type":"block_grid_patterned_inner_square","x":185,"y":435},{"type":"block_grid_patterned_top_square","x":185,"y":165},{"type":"block_grid_patterned_inner_square","x":185,"y":105},{"type":"block_grid_patterned_inner_square","x":185,"y":135},{"type":"block_grid_patterned_inner_square","x":125,"y":75},{"type":"block_grid_patterned_inner_square","x":155,"y":75},{"type":"block_grid_patterned_inner_square","x":185,"y":75},{"type":"block_grid_patterned_inner_square","x":155,"y":45},{"type":"block_grid_patterned_inner_square","x":185,"y":45},{"type":"block_grid_patterned_inner_square","x":155,"y":15},{"type":"block_grid_patterned_inner_square","x":185,"y":15},{"type":"block_grid_patterned_inner_square","x":125,"y":45},{"type":"block_grid_patterned_top_square","x":215,"y":165},{"type":"block_grid_patterned_top_square","x":245,"y":165},{"type":"block_grid_patterned_top_square","x":215,"y":315},{"type":"block_grid_patterned_top_square","x":245,"y":315},{"type":"block_grid_patterned_inner_square","x":215,"y":105},{"type":"block_grid_patterned_inner_square","x":245,"y":105},{"type":"block_grid_patterned_inner_square","x":215,"y":135},{"type":"block_grid_patterned_inner_square","x":245,"y":135},{"type":"block_grid_patterned_inner_square","x":215,"y":345},{"type":"block_grid_patterned_inner_square","x":245,"y":345},{"type":"block_grid_patterned_inner_square","x":215,"y":375},{"type":"block_grid_patterned_inner_square","x":245,"y":375},{"type":"block_grid_patterned_inner_square","x":215,"y":405},{"type":"block_grid_patterned_inner_square","x":245,"y":405},{"type":"block_grid_patterned_inner_square","x":215,"y":435},{"type":"block_grid_patterned_inner_square","x":245,"y":435},{"type":"block_grid_patterned_top_square","x":275,"y":165},{"type":"block_grid_patterned_top_square","x":275,"y":315},{"type":"block_grid_patterned_inner_square","x":275,"y":105},{"type":"block_grid_patterned_inner_square","x":275,"y":135},{"type":"block_grid_patterned_inner_square","x":275,"y":345},{"type":"block_grid_patterned_inner_square","x":275,"y":375},{"type":"block_grid_patterned_inner_square","x":275,"y":405},{"type":"block_grid_patterned_inner_square","x":275,"y":435}]},
    {"level_name":"Stereo Madness v2","author":"Sumsar","theme":"classic-retro","difficulty":"easy","tags":["retro","easy","classic","cube-heavy"],"description":"Modern remake of Stereo Madness with grid-patterned blocks, lots of decoration clouds, and gentle spike/orb progression. Showcases legible cube gameplay with classic GD vibe \u2014 heavy on platform stacks at fixed heights.","orig_x_range":[25500,26000],"object_count":73,"objects":[{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":15,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":45,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":75,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":15,"y":296},{"type":"spike_small_decorative_spikes","x":45,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":45,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":75,"y":296},{"type":"block_black_gradient_square","x":195,"y":135},{"type":"block_black_gradient_square","x":135,"y":45},{"type":"block_black_gradient_square","x":135,"y":75},{"type":"block_black_gradient_square","x":135,"y":15},{"type":"block_grid_patterned_inner_square","x":165,"y":45},{"type":"block_black_gradient_square","x":165,"y":105},{"type":"block_grid_patterned_inner_square","x":165,"y":75},{"type":"block_grid_patterned_inner_square","x":165,"y":15},{"type":"block_grid_patterned_inner_square","x":195,"y":45},{"type":"block_grid_patterned_inner_square","x":195,"y":75},{"type":"block_grid_patterned_inner_square","x":195,"y":105},{"type":"block_grid_patterned_inner_square","x":195,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":105,"y":4},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":105,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":135,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":165,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":195,"y":296},{"type":"spike_medium_decorative_spikes","x":101,"y":279},{"type":"block_black_gradient_square","x":255,"y":135},{"type":"block_black_gradient_square","x":225,"y":135},{"type":"block_black_gradient_square","x":285,"y":135},{"type":"block_grid_patterned_inner_square","x":255,"y":45},{"type":"block_grid_patterned_inner_square","x":255,"y":75},{"type":"block_grid_patterned_inner_square","x":255,"y":105},{"type":"block_grid_patterned_inner_square","x":255,"y":15},{"type":"block_grid_patterned_inner_square","x":225,"y":45},{"type":"block_grid_patterned_inner_square","x":225,"y":75},{"type":"block_grid_patterned_inner_square","x":225,"y":105},{"type":"block_grid_patterned_inner_square","x":225,"y":15},{"type":"block_grid_patterned_inner_square","x":285,"y":45},{"type":"block_grid_patterned_inner_square","x":285,"y":75},{"type":"block_grid_patterned_inner_square","x":285,"y":105},{"type":"block_grid_patterned_inner_square","x":285,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":255,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":225,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":285,"y":296},{"type":"spike_small_decorative_spikes","x":271,"y":285,"rotation":180.0},{"type":"block_black_gradient_square","x":315,"y":135},{"type":"block_black_gradient_square","x":345,"y":135},{"type":"block_grid_patterned_inner_square","x":315,"y":45},{"type":"block_grid_patterned_inner_square","x":315,"y":75},{"type":"block_grid_patterned_inner_square","x":315,"y":105},{"type":"block_grid_patterned_inner_square","x":315,"y":15},{"type":"block_grid_patterned_inner_square","x":345,"y":45},{"type":"block_grid_patterned_inner_square","x":345,"y":75},{"type":"block_grid_patterned_inner_square","x":345,"y":105},{"type":"block_grid_patterned_inner_square","x":345,"y":15},{"type":"block_grid_patterned_inner_square","x":375,"y":75},{"type":"block_grid_patterned_inner_square","x":375,"y":15},{"type":"block_grid_patterned_inner_square","x":375,"y":45},{"type":"block_black_gradient_square","x":375,"y":105},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":345,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":315,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":375,"y":296},{"type":"block_black_gradient_square","x":405,"y":45},{"type":"block_black_gradient_square","x":405,"y":15},{"type":"block_black_gradient_square","x":405,"y":75},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":405,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":435,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":465,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":495,"y":296},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":465,"y":6},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":495,"y":6},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":435,"y":6},{"type":"spike_large_decorative_spikes","x":455,"y":279,"rotation":180.0},{"type":"spike_small_decorative_spikes","x":489,"y":15}]},
    {"level_name":"Cataclysm","author":"Ggb0y","theme":"dark-decorated","difficulty":"extreme","tags":["demon","extreme","decorated","dark","saw-heavy"],"description":"A famous Insane Demon. Dark palette dominated by black and red. Sawblades EVERYWHERE, tight spike corridors, lots of decoration_cloud at varying scales for ambience. Heavy use of color triggers (~500) to flash the palette during intense segments.","orig_x_range":[4500,5000],"object_count":75,"objects":[{"type":"obj_blue_gravity_pad","x":75,"y":207,"rotation":180.0},{"type":"obj_blue_gravity_pad","x":75,"y":153},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":15,"y":34},{"type":"block_black_inner_square","x":45,"y":15},{"type":"block_black_inner_square","x":15,"y":15},{"type":"portal_cube_portal","x":39,"y":181},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":15,"y":326,"rotation":180.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":45,"y":326,"rotation":180.0},{"type":"block_black_inner_square","x":45,"y":345},{"type":"block_black_inner_square","x":15,"y":345},{"type":"portal_normal_gravity_portal","x":47,"y":181},{"type":"block_black_inner_square","x":45,"y":375},{"type":"block_black_inner_square","x":45,"y":405},{"type":"block_black_inner_square","x":45,"y":435},{"type":"block_black_inner_square","x":15,"y":375},{"type":"block_black_inner_square","x":15,"y":405},{"type":"block_black_inner_square","x":15,"y":435},{"type":"obj_blue_gravity_pad","x":105,"y":207,"rotation":180.0},{"type":"obj_blue_gravity_pad","x":105,"y":153},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":135,"y":34},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":165,"y":34},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":195,"y":34},{"type":"block_black_inner_square","x":135,"y":15},{"type":"block_black_inner_square","x":165,"y":15},{"type":"block_black_inner_square","x":195,"y":15},{"type":"speed_portal_half","x":141,"y":179},{"type":"portal_normal_gravity_portal","x":141,"y":181},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":195,"y":326,"rotation":180.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":165,"y":326,"rotation":180.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":135,"y":326,"rotation":180.0},{"type":"block_black_inner_square","x":135,"y":345},{"type":"block_black_inner_square","x":165,"y":345},{"type":"block_black_inner_square","x":195,"y":345},{"type":"block_black_inner_square","x":135,"y":375},{"type":"block_black_inner_square","x":165,"y":375},{"type":"block_black_inner_square","x":195,"y":375},{"type":"block_black_inner_square","x":165,"y":405},{"type":"block_black_inner_square","x":195,"y":405},{"type":"block_black_inner_square","x":135,"y":405},{"type":"block_black_inner_square","x":195,"y":435},{"type":"block_black_inner_square","x":165,"y":435},{"type":"block_black_inner_square","x":135,"y":435},{"type":"pulse_trigger","x":135,"y":645},{"type":"pulse_trigger","x":135,"y":675},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":225,"y":34},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":255,"y":34},{"type":"block_black_inner_square","x":225,"y":15},{"type":"block_black_inner_square","x":255,"y":15},{"type":"block_black_inner_square","x":285,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":285,"y":34,"rotation":180.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":296,"y":45,"rotation":90.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":285,"y":54,"rotation":123.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":273,"y":30,"rotation":115.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":296,"y":315,"rotation":270.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":255,"y":326,"rotation":180.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":285,"y":326,"rotation":180.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":225,"y":326,"rotation":180.0},{"type":"block_black_inner_square","x":225,"y":345},{"type":"block_black_inner_square","x":255,"y":345},{"type":"block_black_inner_square","x":285,"y":345},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":281,"y":302,"rotation":210.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":263,"y":320,"rotation":236.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":292,"y":317,"rotation":270.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":284,"y":319,"rotation":270.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":278,"y":323,"rotation":270.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":272,"y":327,"rotation":270.0},{"type":"block_black_inner_square","x":225,"y":375},{"type":"block_black_inner_square","x":255,"y":375},{"type":"block_black_inner_square","x":285,"y":375},{"type":"block_black_inner_square","x":225,"y":405},{"type":"block_black_inner_square","x":255,"y":405},{"type":"block_black_inner_square","x":285,"y":405},{"type":"block_black_inner_square","x":285,"y":435},{"type":"block_black_inner_square","x":255,"y":435},{"type":"block_black_inner_square","x":225,"y":435}]},
    {"level_name":"Cataclysm","author":"Ggb0y","theme":"dark-decorated","difficulty":"extreme","tags":["demon","extreme","decorated","dark","saw-heavy"],"description":"A famous Insane Demon. Dark palette dominated by black and red. Sawblades EVERYWHERE, tight spike corridors, lots of decoration_cloud at varying scales for ambience. Heavy use of color triggers (~500) to flash the palette during intense segments.","orig_x_range":[6000,6500],"object_count":75,"objects":[{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":15,"y":64},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":11,"y":86,"rotation":-13.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":37,"y":94,"rotation":-22.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":63,"y":104,"rotation":-16.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":91,"y":112,"rotation":-14.0},{"type":"block_black_inner_square","x":15,"y":45},{"type":"block_black_inner_square","x":45,"y":45},{"type":"block_black_inner_square","x":75,"y":45},{"type":"block_black_inner_square","x":45,"y":75},{"type":"block_black_inner_square","x":15,"y":75},{"type":"block_black_inner_square","x":75,"y":75},{"type":"block_black_inner_square","x":75,"y":15},{"type":"block_black_inner_square","x":45,"y":15},{"type":"block_black_inner_square","x":15,"y":15},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":13,"y":188,"rotation":163.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":39,"y":198,"rotation":153.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":65,"y":208,"rotation":160.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":91,"y":216,"rotation":166.0},{"type":"block_black_inner_square","x":15,"y":255},{"type":"block_black_inner_square","x":45,"y":255},{"type":"block_black_inner_square","x":75,"y":255},{"type":"block_black_inner_square","x":75,"y":285},{"type":"block_black_inner_square","x":45,"y":285},{"type":"block_black_inner_square","x":15,"y":285},{"type":"block_black_inner_square","x":15,"y":225},{"type":"block_black_inner_square","x":45,"y":225},{"type":"block_black_inner_square","x":75,"y":89},{"type":"block_black_inner_square","x":45,"y":81},{"type":"block_black_inner_square","x":75,"y":231},{"type":"block_black_inner_square","x":15,"y":207},{"type":"block_black_inner_square","x":35,"y":215},{"type":"block_black_inner_square","x":55,"y":221},{"type":"spike_large_black_sawblade","x":45,"y":225},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":119,"y":120,"rotation":-21.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":145,"y":130,"rotation":-28.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":171,"y":138,"rotation":-8.0},{"type":"block_black_inner_square","x":195,"y":135},{"type":"block_black_inner_square","x":195,"y":105},{"type":"block_black_inner_square","x":105,"y":45},{"type":"block_black_inner_square","x":135,"y":45},{"type":"block_black_inner_square","x":135,"y":75},{"type":"block_black_inner_square","x":165,"y":75},{"type":"block_black_inner_square","x":105,"y":75},{"type":"block_black_inner_square","x":195,"y":75},{"type":"block_black_inner_square","x":165,"y":105},{"type":"block_black_inner_square","x":135,"y":105},{"type":"block_black_inner_square","x":195,"y":45},{"type":"block_black_inner_square","x":165,"y":45},{"type":"block_black_inner_square","x":165,"y":15},{"type":"block_black_inner_square","x":195,"y":15},{"type":"block_black_inner_square","x":135,"y":15},{"type":"block_black_inner_square","x":105,"y":15},{"type":"block_black_inner_square","x":195,"y":255},{"type":"block_black_inner_square","x":195,"y":285},{"type":"block_black_inner_square","x":195,"y":315},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":117,"y":226,"rotation":150.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":141,"y":240,"rotation":150.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":169,"y":240,"rotation":168.0},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":141,"y":236,"rotation":168.0},{"type":"block_black_inner_square","x":105,"y":255},{"type":"block_black_inner_square","x":135,"y":255},{"type":"block_black_inner_square","x":165,"y":255},{"type":"block_black_inner_square","x":165,"y":285},{"type":"block_black_inner_square","x":135,"y":285},{"type":"block_black_inner_square","x":105,"y":285},{"type":"block_black_inner_square","x":105,"y":99},{"type":"block_black_inner_square","x":165,"y":117},{"type":"block_black_inner_square","x":105,"y":241},{"type":"block_black_inner_square","x":161,"y":111},{"type":"block_black_inner_square","x":113,"y":243},{"type":"spike_large_black_sawblade","x":165,"y":113},{"type":"block_black_inner_square","x":225,"y":135},{"type":"block_black_inner_square","x":255,"y":135},{"type":"block_black_inner_square","x":285,"y":135},{"type":"block_black_inner_square","x":285,"y":105}]},
    {"level_name":"Cataclysm","author":"Ggb0y","theme":"dark-decorated","difficulty":"extreme","tags":["demon","extreme","decorated","dark","saw-heavy"],"description":"A famous Insane Demon. Dark palette dominated by black and red. Sawblades EVERYWHERE, tight spike corridors, lots of decoration_cloud at varying scales for ambience. Heavy use of color triggers (~500) to flash the palette during intense segments.","orig_x_range":[13000,13500],"object_count":75,"objects":[{"type":"spike_black_gradient_spike","x":35,"y":375,"rotation":180.0},{"type":"obj_blue_gravity_orb","x":5,"y":465},{"type":"spike_black_gradient_spike","x":95,"y":405,"rotation":180.0},{"type":"obj_blue_gravity_pad","x":95,"y":453},{"type":"obj_blue_gravity_pad","x":95,"y":507,"rotation":180.0},{"type":"spike_black_gradient_spike","x":5,"y":525},{"type":"spike_black_gradient_spike","x":95,"y":555},{"type":"spike_black_gradient_spike","x":35,"y":315},{"type":"spike_black_gradient_spike","x":95,"y":345},{"type":"block_black_outer_corner_square","x":35,"y":255},{"type":"block_black_outer_corner_square","x":5,"y":165},{"type":"block_black_top_pillar_square","x":95,"y":285},{"type":"block_black_top_square","x":95,"y":255,"rotation":90.0},{"type":"block_black_top_square","x":95,"y":225,"rotation":90.0},{"type":"block_black_top_square","x":95,"y":195,"rotation":90.0},{"type":"block_black_top_square","x":95,"y":105,"rotation":90.0},{"type":"block_black_top_square","x":95,"y":135,"rotation":90.0},{"type":"block_black_top_square","x":95,"y":165,"rotation":90.0},{"type":"block_black_top_square","x":95,"y":75,"rotation":90.0},{"type":"block_black_top_square","x":95,"y":45,"rotation":90.0},{"type":"block_black_top_square","x":95,"y":15,"rotation":90.0},{"type":"block_black_top_square","x":35,"y":195,"rotation":270.0},{"type":"block_black_top_square","x":35,"y":225,"rotation":270.0},{"type":"block_black_top_square","x":65,"y":255},{"type":"block_black_inner_corner_square","x":5,"y":135},{"type":"block_black_inner_corner_square","x":35,"y":165},{"type":"block_black_inner_square","x":5,"y":105},{"type":"block_black_inner_square","x":5,"y":75},{"type":"block_black_inner_square","x":5,"y":45},{"type":"block_black_inner_square","x":35,"y":135},{"type":"block_black_inner_square","x":35,"y":105},{"type":"block_black_inner_square","x":35,"y":75},{"type":"block_black_inner_square","x":35,"y":45},{"type":"block_black_inner_square","x":35,"y":15},{"type":"block_black_inner_square","x":5,"y":15},{"type":"block_black_inner_square","x":65,"y":15},{"type":"block_black_inner_square","x":65,"y":45},{"type":"block_black_inner_square","x":65,"y":75},{"type":"block_black_inner_square","x":65,"y":105},{"type":"block_black_inner_square","x":65,"y":135},{"type":"block_black_inner_square","x":65,"y":165},{"type":"block_black_inner_square","x":65,"y":195},{"type":"block_black_inner_square","x":65,"y":225},{"type":"spike_half_black_gradient_spike","x":14,"y":405,"rotation":270.0},{"type":"spike_black_gradient_spike","x":125,"y":405,"rotation":180.0},{"type":"spike_black_gradient_spike","x":109,"y":405},{"type":"spike_black_gradient_spike","x":139,"y":405},{"type":"spike_black_gradient_spike","x":155,"y":405,"rotation":180.0},{"type":"obj_blue_gravity_pad","x":125,"y":507,"rotation":180.0},{"type":"obj_blue_gravity_pad","x":155,"y":453},{"type":"obj_blue_gravity_pad","x":125,"y":453},{"type":"obj_blue_gravity_pad","x":155,"y":507,"rotation":180.0},{"type":"obj_blue_gravity_pad","x":185,"y":507,"rotation":180.0},{"type":"spike_black_gradient_spike","x":125,"y":555},{"type":"spike_black_gradient_spike","x":155,"y":555},{"type":"spike_black_gradient_spike","x":185,"y":555},{"type":"spike_black_gradient_spike","x":171,"y":555,"rotation":180.0},{"type":"spike_black_gradient_spike","x":141,"y":555,"rotation":180.0},{"type":"spike_black_gradient_spike","x":111,"y":555,"rotation":180.0},{"type":"spike_black_gradient_spike","x":125,"y":345},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":155,"y":304},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":185,"y":304},{"type":"block_black_inner_square","x":155,"y":255},{"type":"block_black_inner_square","x":155,"y":225},{"type":"block_black_inner_square","x":155,"y":195},{"type":"block_black_inner_square","x":155,"y":165},{"type":"block_black_inner_square","x":155,"y":285},{"type":"block_black_inner_square","x":185,"y":285},{"type":"block_black_inner_square","x":185,"y":255},{"type":"block_black_inner_square","x":185,"y":225},{"type":"block_black_inner_square","x":185,"y":195},{"type":"block_black_inner_square","x":185,"y":165},{"type":"block_black_inner_square","x":155,"y":135},{"type":"block_black_inner_square","x":155,"y":105},{"type":"block_black_inner_square","x":155,"y":75}]},
    {"level_name":"Aquarius","author":"Skitten","theme":"neon-modern","difficulty":"hard","tags":["modern","hard","triggers","neon","pulse-heavy"],"description":"Neon-modern style with lots of block_outline silhouettes, ~300 pulse triggers driving background flashes, and ~270 move triggers animating platforms. Color palette skews bright blue / pink / cyan. Wave segments alternate with ship sections.","orig_x_range":[6500,7000],"object_count":69,"objects":[{"type":"portal_flipped_gravity_portal","x":85,"y":75},{"type":"block_black_pillar_square","x":25,"y":45},{"type":"block_black_pillar_square","x":25,"y":15},{"type":"spike_large_decorative_spikes","x":25,"y":221,"rotation":-180.0},{"type":"block_black_inner_square","x":25,"y":255},{"type":"block_black_inner_square","x":85,"y":315},{"type":"block_black_inner_square","x":85,"y":255},{"type":"block_black_inner_square","x":25,"y":285},{"type":"block_black_inner_square","x":25,"y":315},{"type":"block_black_inner_square","x":85,"y":285},{"type":"block_black_inner_square","x":55,"y":255},{"type":"block_black_inner_square","x":55,"y":285},{"type":"block_black_inner_square","x":55,"y":315},{"type":"spike_large_decorative_spikes","x":25,"y":221,"rotation":-180.0},{"type":"spike_small_decorative_spikes","x":85,"y":13},{"type":"spike_small_decorative_spikes","x":85,"y":13},{"type":"block_black_gradient_square","x":25,"y":135},{"type":"pulse_trigger","x":25,"y":-105},{"type":"toggle_trigger","x":145,"y":-45},{"type":"block_black_inner_square","x":145,"y":315},{"type":"block_black_inner_square","x":115,"y":255},{"type":"block_black_inner_square","x":145,"y":285},{"type":"block_black_inner_square","x":145,"y":255},{"type":"block_black_inner_square","x":115,"y":285},{"type":"block_black_inner_square","x":115,"y":315},{"type":"toggle_trigger","x":145,"y":-15},{"type":"block_black_inner_square","x":175,"y":315},{"type":"block_black_inner_square","x":175,"y":255},{"type":"block_black_inner_square","x":175,"y":285},{"type":"spike_small_decorative_spikes","x":175,"y":227},{"type":"spike_small_decorative_spikes","x":175,"y":227},{"type":"portal_normal_gravity_portal","x":205,"y":375,"rotation":-90.0},{"type":"rotate_trigger","x":205,"y":465},{"type":"spike_medium_decorative_spikes","x":205,"y":19},{"type":"block_black_inner_square","x":265,"y":255},{"type":"block_black_inner_square","x":235,"y":255},{"type":"spike_medium_decorative_spikes","x":205,"y":19},{"type":"block_black_inner_square","x":295,"y":285},{"type":"block_black_inner_square","x":235,"y":285},{"type":"block_black_inner_square","x":235,"y":315},{"type":"block_black_inner_square","x":295,"y":255},{"type":"block_black_inner_square","x":295,"y":315},{"type":"block_black_inner_square","x":205,"y":315},{"type":"block_black_inner_square","x":205,"y":285},{"type":"block_black_inner_square","x":205,"y":255},{"type":"block_black_inner_square","x":265,"y":315},{"type":"block_black_inner_square","x":265,"y":285},{"type":"block_black_gradient_square","x":295,"y":135},{"type":"block_black_gradient_square","x":385,"y":135},{"type":"block_black_gradient_square","x":355,"y":135},{"type":"block_black_inner_square","x":355,"y":255},{"type":"block_black_inner_square","x":385,"y":315},{"type":"block_black_inner_square","x":385,"y":255},{"type":"block_black_inner_square","x":355,"y":285},{"type":"block_black_inner_square","x":325,"y":315},{"type":"block_black_inner_square","x":325,"y":285},{"type":"block_black_inner_square","x":355,"y":315},{"type":"block_black_inner_square","x":325,"y":255},{"type":"block_black_inner_square","x":385,"y":285},{"type":"block_black_gradient_square","x":325,"y":135},{"type":"block_black_gradient_square","x":415,"y":135},{"type":"block_black_gradient_square","x":475,"y":135},{"type":"block_black_gradient_square","x":445,"y":135},{"type":"pulse_trigger","x":445,"y":465},{"type":"block_black_inner_square","x":445,"y":15},{"type":"spike_medium_decorative_spikes","x":475,"y":49},{"type":"block_black_inner_square","x":445,"y":345},{"type":"block_black_inner_square","x":475,"y":345},{"type":"block_black_inner_square","x":475,"y":15}]},
    {"level_name":"Aquarius","author":"Skitten","theme":"neon-modern","difficulty":"hard","tags":["modern","hard","triggers","neon","pulse-heavy"],"description":"Neon-modern style with lots of block_outline silhouettes, ~300 pulse triggers driving background flashes, and ~270 move triggers animating platforms. Color palette skews bright blue / pink / cyan. Wave segments alternate with ship sections.","orig_x_range":[22500,23000],"object_count":66,"objects":[{"type":"block_black_inner_square","x":45,"y":45},{"type":"block_black_inner_square","x":45,"y":15},{"type":"block_black_gradient_square","x":75,"y":285},{"type":"toggle_trigger","x":45,"y":-75},{"type":"block_black_inner_square","x":75,"y":15},{"type":"block_black_inner_square","x":75,"y":45},{"type":"block_black_inner_square","x":45,"y":75},{"type":"block_black_inner_square","x":75,"y":75},{"type":"block_black_inner_square","x":45,"y":105},{"type":"block_black_inner_square","x":75,"y":105},{"type":"pulse_trigger","x":30,"y":-45},{"type":"pulse_trigger","x":30,"y":-75},{"type":"pulse_trigger","x":30,"y":-135},{"type":"block_black_inner_square","x":135,"y":105},{"type":"block_black_gradient_square","x":135,"y":285},{"type":"block_black_gradient_square","x":105,"y":285},{"type":"block_black_gradient_square","x":165,"y":285},{"type":"large_fading_cloud","x":135,"y":137},{"type":"block_black_inner_square","x":105,"y":45},{"type":"block_black_inner_square","x":135,"y":45},{"type":"block_black_inner_square","x":165,"y":45},{"type":"block_black_inner_square","x":195,"y":45},{"type":"block_black_inner_square","x":195,"y":15},{"type":"block_black_inner_square","x":165,"y":15},{"type":"block_black_inner_square","x":135,"y":15},{"type":"block_black_inner_square","x":105,"y":15},{"type":"block_black_inner_square","x":105,"y":75},{"type":"block_black_inner_square","x":135,"y":75},{"type":"block_black_inner_square","x":165,"y":75},{"type":"block_black_inner_square","x":195,"y":75},{"type":"block_black_inner_square","x":195,"y":105},{"type":"block_black_inner_square","x":165,"y":105},{"type":"block_black_inner_square","x":105,"y":105},{"type":"block_black_gradient_square","x":195,"y":285},{"type":"block_black_inner_square","x":225,"y":105},{"type":"block_black_inner_square","x":225,"y":75},{"type":"block_black_inner_square","x":225,"y":45},{"type":"block_black_inner_square","x":225,"y":15},{"type":"alpha_trigger","x":225,"y":675},{"type":"block_black_inner_square","x":315,"y":75},{"type":"block_black_inner_square","x":345,"y":75},{"type":"block_black_inner_square","x":345,"y":45},{"type":"block_black_inner_square","x":315,"y":45},{"type":"block_black_inner_square","x":315,"y":15},{"type":"block_black_inner_square","x":345,"y":15},{"type":"block_black_inner_square","x":375,"y":15},{"type":"block_black_inner_square","x":375,"y":45},{"type":"block_black_inner_square","x":375,"y":75},{"type":"pulse_trigger","x":345,"y":-45},{"type":"pulse_trigger","x":345,"y":-75},{"type":"pulse_trigger","x":345,"y":-135},{"type":"block_black_inner_square","x":495,"y":75},{"type":"small_fading_cloud","x":405,"y":103},{"type":"block_black_inner_square","x":405,"y":75},{"type":"block_black_inner_square","x":405,"y":15},{"type":"block_black_inner_square","x":435,"y":15},{"type":"block_black_inner_square","x":465,"y":15},{"type":"block_black_inner_square","x":435,"y":75},{"type":"block_black_inner_square","x":405,"y":45},{"type":"block_black_inner_square","x":435,"y":45},{"type":"block_black_inner_square","x":465,"y":45},{"type":"block_black_inner_square","x":465,"y":75},{"type":"block_black_inner_square","x":495,"y":15},{"type":"block_black_inner_square","x":495,"y":45},{"type":"effect_pulsing_filled_circle","x":482,"y":265,"rotation":11.0},{"type":"effect_pulsing_filled_circle","x":465,"y":-45}]},
    {"level_name":"Aquarius","author":"Skitten","theme":"neon-modern","difficulty":"hard","tags":["modern","hard","triggers","neon","pulse-heavy"],"description":"Neon-modern style with lots of block_outline silhouettes, ~300 pulse triggers driving background flashes, and ~270 move triggers animating platforms. Color palette skews bright blue / pink / cyan. Wave segments alternate with ship sections.","orig_x_range":[32000,32500],"object_count":71,"objects":[{"type":"obj_blue_gravity_pad","x":55,"y":327},{"type":"obj_blue_gravity_pad","x":25,"y":288},{"type":"obj_blue_gravity_pad","x":25,"y":327},{"type":"obj_blue_gravity_pad","x":55,"y":243},{"type":"obj_blue_gravity_pad","x":85,"y":288},{"type":"obj_blue_gravity_pad","x":85,"y":282,"rotation":-180.0},{"type":"obj_blue_gravity_pad","x":55,"y":282,"rotation":-180.0},{"type":"obj_blue_gravity_pad","x":55,"y":288},{"type":"obj_blue_gravity_pad","x":85,"y":243},{"type":"obj_blue_gravity_pad","x":85,"y":327},{"type":"obj_blue_gravity_pad","x":25,"y":282,"rotation":-180.0},{"type":"obj_blue_gravity_pad","x":25,"y":243},{"type":"block_black_gradient_square","x":25,"y":225},{"type":"block_black_gradient_square","x":55,"y":225},{"type":"block_black_gradient_square","x":85,"y":225},{"type":"block_black_gradient_square","x":85,"y":345},{"type":"block_black_gradient_square","x":55,"y":345},{"type":"block_black_gradient_square","x":25,"y":345},{"type":"obj_blue_gravity_pad","x":145,"y":327},{"type":"obj_blue_gravity_pad","x":175,"y":243},{"type":"obj_blue_gravity_pad","x":145,"y":243},{"type":"obj_blue_gravity_pad","x":175,"y":327},{"type":"obj_blue_gravity_pad","x":115,"y":282,"rotation":-180.0},{"type":"obj_blue_gravity_pad","x":115,"y":288},{"type":"obj_blue_gravity_pad","x":115,"y":243},{"type":"obj_blue_gravity_pad","x":115,"y":327},{"type":"block_black_gradient_square","x":115,"y":225},{"type":"block_black_gradient_square","x":145,"y":225},{"type":"block_black_gradient_square","x":175,"y":225},{"type":"block_black_gradient_square","x":175,"y":345},{"type":"block_black_gradient_square","x":145,"y":345},{"type":"block_black_gradient_square","x":115,"y":345},{"type":"obj_blue_gravity_pad","x":265,"y":243},{"type":"obj_blue_gravity_pad","x":235,"y":243},{"type":"obj_blue_gravity_pad","x":235,"y":327},{"type":"obj_blue_gravity_pad","x":295,"y":243},{"type":"obj_blue_gravity_pad","x":265,"y":327},{"type":"obj_blue_gravity_pad","x":295,"y":327},{"type":"obj_blue_gravity_pad","x":205,"y":243},{"type":"obj_blue_gravity_pad","x":205,"y":327},{"type":"block_black_gradient_square","x":205,"y":225},{"type":"block_black_gradient_square","x":235,"y":225},{"type":"block_black_gradient_square","x":265,"y":225},{"type":"block_black_gradient_square","x":295,"y":225},{"type":"block_black_gradient_square","x":295,"y":345},{"type":"block_black_gradient_square","x":265,"y":345},{"type":"block_black_gradient_square","x":205,"y":345},{"type":"block_black_gradient_square","x":235,"y":345},{"type":"speed_portal_double","x":355,"y":255},{"type":"block_black_gradient_square","x":385,"y":345},{"type":"block_black_gradient_square","x":385,"y":195},{"type":"obj_blue_gravity_pad","x":367,"y":285,"rotation":-270.0},{"type":"obj_blue_gravity_pad","x":355,"y":327},{"type":"obj_blue_gravity_pad","x":367,"y":315,"rotation":-270.0},{"type":"obj_blue_gravity_pad","x":325,"y":327},{"type":"block_black_gradient_square","x":325,"y":195},{"type":"portal_cube_portal","x":355,"y":255},{"type":"block_black_gradient_square","x":355,"y":195},{"type":"obj_blue_gravity_pad","x":325,"y":243},{"type":"block_black_gradient_square","x":325,"y":345},{"type":"block_black_gradient_square","x":355,"y":345},{"type":"block_black_gradient_square","x":415,"y":195},{"type":"block_black_gradient_square","x":490,"y":232},{"type":"block_black_gradient_square","x":415,"y":345},{"type":"block_black_gradient_square","x":432,"y":278},{"type":"block_black_gradient_square","x":432,"y":308},{"type":"block_black_gradient_square","x":415,"y":315},{"type":"block_black_gradient_square","x":446,"y":268},{"type":"block_black_gradient_square","x":460,"y":262},{"type":"block_black_gradient_square","x":469,"y":242},{"type":"block_black_gradient_square","x":452,"y":252}]},
    {"level_name":"Acid Factory","author":"UsernameDefault","theme":"industrial-modern","difficulty":"hard","tags":["hard","industrial","triggers","modern"],"description":"Industrial-themed Hard demon. Mostly block_outline silhouettes with green/yellow pulse highlights. Lots of ball-mode segments via portal_ball. Spike fields tight, decoration sparse but coordinated.","orig_x_range":[2500,3000],"object_count":55,"objects":[{"type":"block_black_inner_square","x":35,"y":45},{"type":"block_black_inner_square","x":35,"y":75},{"type":"block_black_inner_square","x":5,"y":75},{"type":"block_black_inner_square","x":5,"y":45},{"type":"block_black_inner_square","x":65,"y":45},{"type":"block_black_inner_square","x":65,"y":75},{"type":"block_black_inner_square","x":35,"y":15},{"type":"block_black_inner_square","x":5,"y":15},{"type":"block_black_inner_square","x":65,"y":15},{"type":"block_black_inner_square","x":125,"y":15},{"type":"block_black_inner_square","x":125,"y":45},{"type":"block_black_inner_square","x":125,"y":-15},{"type":"block_black_inner_square","x":155,"y":15},{"type":"block_black_inner_square","x":155,"y":45},{"type":"block_black_inner_square","x":155,"y":-15},{"type":"block_black_inner_square","x":185,"y":15},{"type":"block_black_inner_square","x":185,"y":45},{"type":"block_black_inner_square","x":185,"y":-15},{"type":"pulse_trigger","x":125,"y":-147},{"type":"pulse_trigger","x":125,"y":-180},{"type":"pulse_trigger","x":125,"y":-120},{"type":"pulse_trigger","x":125,"y":-90},{"type":"jump_orb_yellow_jump_orb","x":215,"y":135},{"type":"block_black_inner_square","x":215,"y":15},{"type":"block_black_inner_square","x":215,"y":45},{"type":"block_black_inner_square","x":215,"y":-15},{"type":"block_black_inner_square","x":245,"y":15},{"type":"block_black_inner_square","x":245,"y":45},{"type":"block_black_inner_square","x":245,"y":-15},{"type":"block_black_inner_square","x":275,"y":15},{"type":"block_black_inner_square","x":275,"y":45},{"type":"block_black_inner_square","x":275,"y":-15},{"type":"pulse_trigger","x":245,"y":-147},{"type":"pulse_trigger","x":245,"y":-180},{"type":"pulse_trigger","x":245,"y":-120},{"type":"pulse_trigger","x":245,"y":-90},{"type":"jump_orb_yellow_jump_orb","x":305,"y":165},{"type":"block_black_inner_square","x":305,"y":15},{"type":"block_black_inner_square","x":305,"y":45},{"type":"block_black_inner_square","x":305,"y":-15},{"type":"block_black_inner_square","x":335,"y":15},{"type":"block_black_inner_square","x":335,"y":45},{"type":"block_black_inner_square","x":335,"y":-15},{"type":"block_black_inner_square","x":365,"y":15},{"type":"block_black_inner_square","x":365,"y":45},{"type":"block_black_inner_square","x":365,"y":-15},{"type":"block_black_inner_square","x":425,"y":45},{"type":"block_black_inner_square","x":425,"y":75},{"type":"block_black_inner_square","x":425,"y":15},{"type":"block_black_inner_square","x":455,"y":45},{"type":"block_black_inner_square","x":455,"y":75},{"type":"block_black_inner_square","x":455,"y":15},{"type":"block_black_inner_square","x":485,"y":45},{"type":"block_black_inner_square","x":485,"y":75},{"type":"block_black_inner_square","x":485,"y":15}]},
    {"level_name":"Acid Factory","author":"UsernameDefault","theme":"industrial-modern","difficulty":"hard","tags":["hard","industrial","triggers","modern"],"description":"Industrial-themed Hard demon. Mostly block_outline silhouettes with green/yellow pulse highlights. Lots of ball-mode segments via portal_ball. Spike fields tight, decoration sparse but coordinated.","orig_x_range":[4500,5000],"object_count":75,"objects":[{"type":"block_black_inner_square","x":15,"y":405},{"type":"block_black_inner_square","x":75,"y":45},{"type":"block_black_inner_square","x":45,"y":45},{"type":"block_black_inner_square","x":15,"y":45},{"type":"block_black_inner_square","x":15,"y":15},{"type":"block_black_inner_square","x":45,"y":15},{"type":"block_black_inner_square","x":75,"y":15},{"type":"block_black_inner_square","x":75,"y":75},{"type":"block_black_inner_square","x":45,"y":75},{"type":"block_black_inner_square","x":15,"y":75},{"type":"alpha_trigger","x":75,"y":555},{"type":"alpha_trigger","x":75,"y":585},{"type":"alpha_trigger","x":15,"y":555},{"type":"alpha_trigger","x":15,"y":585},{"type":"block_black_inner_square","x":75,"y":405},{"type":"block_black_inner_square","x":45,"y":405},{"type":"pulse_trigger","x":135,"y":495},{"type":"effect_pulsing_diamond","x":135,"y":135},{"type":"alpha_trigger","x":195,"y":555},{"type":"alpha_trigger","x":195,"y":585},{"type":"alpha_trigger","x":135,"y":555},{"type":"alpha_trigger","x":135,"y":585},{"type":"block_black_inner_square","x":195,"y":405},{"type":"block_black_inner_square","x":165,"y":405},{"type":"block_black_inner_square","x":135,"y":405},{"type":"block_black_inner_square","x":105,"y":405},{"type":"block_black_inner_square","x":135,"y":45},{"type":"block_black_inner_square","x":195,"y":45},{"type":"block_black_inner_square","x":165,"y":45},{"type":"block_black_inner_square","x":105,"y":45},{"type":"block_black_inner_square","x":105,"y":15},{"type":"block_black_inner_square","x":135,"y":15},{"type":"block_black_inner_square","x":165,"y":15},{"type":"block_black_inner_square","x":195,"y":15},{"type":"block_black_inner_square","x":195,"y":75},{"type":"block_black_inner_square","x":165,"y":75},{"type":"block_black_inner_square","x":135,"y":75},{"type":"block_black_inner_square","x":105,"y":75},{"type":"pulse_trigger","x":285,"y":495},{"type":"alpha_trigger","x":255,"y":585},{"type":"alpha_trigger","x":255,"y":555},{"type":"block_black_inner_square","x":285,"y":405},{"type":"block_black_inner_square","x":255,"y":405},{"type":"block_black_inner_square","x":225,"y":405},{"type":"block_black_inner_square","x":255,"y":45},{"type":"block_black_inner_square","x":225,"y":45},{"type":"block_black_inner_square","x":285,"y":45},{"type":"block_black_inner_square","x":225,"y":75},{"type":"block_black_inner_square","x":255,"y":75},{"type":"block_black_inner_square","x":285,"y":75},{"type":"block_black_inner_square","x":285,"y":15},{"type":"block_black_inner_square","x":255,"y":15},{"type":"block_black_inner_square","x":225,"y":15},{"type":"portal_green_size_portal","x":315,"y":225},{"type":"effect_pulsing_diamond","x":315,"y":135},{"type":"alpha_trigger","x":315,"y":555},{"type":"alpha_trigger","x":315,"y":585},{"type":"alpha_trigger","x":375,"y":585},{"type":"alpha_trigger","x":375,"y":555},{"type":"block_black_inner_square","x":315,"y":75},{"type":"block_black_inner_square","x":345,"y":75},{"type":"block_black_inner_square","x":375,"y":75},{"type":"block_black_inner_square","x":375,"y":15},{"type":"block_black_inner_square","x":345,"y":15},{"type":"block_black_inner_square","x":315,"y":15},{"type":"block_black_inner_square","x":315,"y":45},{"type":"block_black_inner_square","x":345,"y":45},{"type":"block_black_inner_square","x":375,"y":45},{"type":"block_black_inner_square","x":315,"y":405},{"type":"block_black_inner_square","x":345,"y":405},{"type":"block_black_inner_square","x":375,"y":405},{"type":"portal_green_size_portal","x":495,"y":225},{"type":"pulse_trigger","x":435,"y":495},{"type":"pulse_trigger","x":495,"y":495},{"type":"alpha_trigger","x":435,"y":555}]},
    {"level_name":"Acid Factory","author":"UsernameDefault","theme":"industrial-modern","difficulty":"hard","tags":["hard","industrial","triggers","modern"],"description":"Industrial-themed Hard demon. Mostly block_outline silhouettes with green/yellow pulse highlights. Lots of ball-mode segments via portal_ball. Spike fields tight, decoration sparse but coordinated.","orig_x_range":[11500,12000],"object_count":39,"objects":[{"type":"spike_non_colorable_spike_black_pit_hazard","x":95,"y":106},{"type":"speed_portal_normal","x":5,"y":197},{"type":"spike_non_colorable_spike_black_pit_hazard","x":35,"y":106},{"type":"spike_non_colorable_spike_black_pit_hazard","x":65,"y":106},{"type":"spike_non_colorable_spike_black_pit_hazard","x":5,"y":309,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":65,"y":317,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":95,"y":317,"rotation":-180.0},{"type":"pulse_trigger","x":65,"y":555},{"type":"pulse_trigger","x":65,"y":585},{"type":"speed_portal_half","x":155,"y":267},{"type":"spike_non_colorable_spike_black_pit_hazard","x":125,"y":106},{"type":"spike_non_colorable_spike_black_pit_hazard","x":185,"y":106},{"type":"spike_non_colorable_spike_black_pit_hazard","x":155,"y":106},{"type":"spike_non_colorable_spike_black_pit_hazard","x":155,"y":327,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":185,"y":327,"rotation":-180.0},{"type":"spike_medium_decorative_spikes","x":105,"y":123},{"type":"spike_large_decorative_spikes","x":113,"y":123},{"type":"decor_tall_rod","x":125,"y":279,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":215,"y":327,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":245,"y":327,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":275,"y":327,"rotation":-180.0},{"type":"speed_portal_normal","x":335,"y":223},{"type":"spike_non_colorable_spike_black_pit_hazard","x":365,"y":92},{"type":"spike_non_colorable_spike_black_pit_hazard","x":305,"y":327,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":335,"y":327,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":365,"y":327,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":395,"y":327,"rotation":-180.0},{"type":"pulse_trigger","x":335,"y":555},{"type":"pulse_trigger","x":335,"y":585},{"type":"speed_portal_half","x":485,"y":153},{"type":"spike_non_colorable_spike_black_pit_hazard","x":455,"y":92},{"type":"spike_non_colorable_spike_black_pit_hazard","x":425,"y":92},{"type":"spike_non_colorable_spike_black_pit_hazard","x":425,"y":327,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":455,"y":327,"rotation":-180.0},{"type":"spike_non_colorable_spike_black_pit_hazard","x":485,"y":327,"rotation":-180.0},{"type":"spike_small_decorative_spikes","x":485,"y":103},{"type":"effect_pulsing_diamond","x":441,"y":209},{"type":"effect_pulsing_diamond","x":441,"y":209},{"type":"decor_tall_rod","x":485,"y":141}]},
    {"level_name":"Promises","author":"Adiale","theme":"clean-modern","difficulty":"medium","tags":["modern","medium","clean","decorated"],"description":"A clean, well-paced modern level. Smooth color transitions between sections, tasteful decoration without overload. Good reference for 'modern but not flashy' design \u2014 readable gameplay with subtle visual polish.","orig_x_range":[7500,8000],"object_count":75,"objects":[{"type":"block_black_inner_square","x":15,"y":15},{"type":"block_black_inner_square","x":45,"y":15},{"type":"block_black_inner_square","x":75,"y":15},{"type":"block_black_inner_square","x":75,"y":45},{"type":"block_black_inner_square","x":45,"y":45},{"type":"block_black_inner_square","x":15,"y":45},{"type":"block_black_inner_square","x":15,"y":75},{"type":"block_black_inner_square","x":75,"y":75},{"type":"block_black_inner_square","x":45,"y":75},{"type":"block_black_inner_square","x":68,"y":344},{"type":"block_black_inner_square","x":45,"y":-15},{"type":"block_black_inner_square","x":75,"y":-15},{"type":"block_black_inner_square","x":15,"y":-15},{"type":"effect_pulsing_music_note","x":38,"y":178},{"type":"effect_pulsing_music_note","x":36,"y":177},{"type":"jump_orb_yellow_jump_orb","x":105,"y":195},{"type":"block_black_inner_square","x":105,"y":15},{"type":"block_black_inner_square","x":135,"y":15},{"type":"block_black_inner_square","x":165,"y":15},{"type":"block_black_inner_square","x":195,"y":15},{"type":"block_black_inner_square","x":135,"y":45},{"type":"block_black_inner_square","x":105,"y":45},{"type":"block_black_inner_square","x":165,"y":45},{"type":"block_black_inner_square","x":195,"y":45},{"type":"block_black_inner_square","x":195,"y":75},{"type":"block_black_inner_square","x":165,"y":75},{"type":"block_black_inner_square","x":135,"y":75},{"type":"block_black_inner_square","x":105,"y":75},{"type":"block_black_inner_square","x":195,"y":105},{"type":"block_black_inner_square","x":165,"y":105},{"type":"pulse_trigger","x":143,"y":735},{"type":"pulse_trigger","x":143,"y":705},{"type":"pulse_trigger","x":143,"y":615},{"type":"pulse_trigger","x":143,"y":645},{"type":"pulse_trigger","x":143,"y":765},{"type":"pulse_trigger","x":143,"y":675},{"type":"pulse_trigger","x":143,"y":585},{"type":"effect_pulsing_music_note","x":185,"y":344},{"type":"effect_pulsing_music_note","x":183,"y":342},{"type":"block_black_inner_square","x":105,"y":-15},{"type":"block_black_inner_square","x":195,"y":-15},{"type":"block_black_inner_square","x":165,"y":-15},{"type":"block_black_inner_square","x":135,"y":-15},{"type":"decor_medium_decorative_gear","x":165,"y":285},{"type":"block_black_inner_square","x":225,"y":135},{"type":"block_black_inner_square","x":225,"y":105},{"type":"block_black_inner_square","x":225,"y":75},{"type":"block_black_inner_square","x":225,"y":45},{"type":"block_black_inner_square","x":225,"y":15},{"type":"block_black_inner_square","x":255,"y":15},{"type":"block_black_inner_square","x":255,"y":45},{"type":"block_black_inner_square","x":255,"y":75},{"type":"block_black_inner_square","x":255,"y":105},{"type":"block_black_inner_square","x":285,"y":15},{"type":"block_black_inner_square","x":285,"y":75},{"type":"block_black_inner_square","x":285,"y":45},{"type":"block_black_inner_square","x":285,"y":105},{"type":"block_black_inner_square","x":255,"y":135},{"type":"block_black_inner_square","x":285,"y":135},{"type":"block_black_inner_square","x":225,"y":-15},{"type":"block_black_inner_square","x":268,"y":333},{"type":"block_black_inner_square","x":255,"y":-15},{"type":"block_black_inner_square","x":285,"y":-15},{"type":"block_black_inner_square","x":315,"y":15},{"type":"block_black_inner_square","x":315,"y":45},{"type":"block_black_inner_square","x":315,"y":75},{"type":"block_black_inner_square","x":315,"y":105},{"type":"block_black_inner_square","x":345,"y":135},{"type":"block_black_inner_square","x":375,"y":135},{"type":"block_black_inner_square","x":375,"y":45},{"type":"block_black_inner_square","x":375,"y":75},{"type":"block_black_inner_square","x":345,"y":45},{"type":"block_black_inner_square","x":345,"y":75},{"type":"block_black_inner_square","x":345,"y":105},{"type":"block_black_inner_square","x":375,"y":105}]},
    {"level_name":"Promises","author":"Adiale","theme":"clean-modern","difficulty":"medium","tags":["modern","medium","clean","decorated"],"description":"A clean, well-paced modern level. Smooth color transitions between sections, tasteful decoration without overload. Good reference for 'modern but not flashy' design \u2014 readable gameplay with subtle visual polish.","orig_x_range":[8500,9000],"object_count":75,"objects":[{"type":"pulse_trigger","x":13,"y":585},{"type":"pulse_trigger","x":13,"y":615},{"type":"pulse_trigger","x":13,"y":645},{"type":"pulse_trigger","x":13,"y":675},{"type":"pulse_trigger","x":13,"y":705},{"type":"pulse_trigger","x":13,"y":735},{"type":"pulse_trigger","x":13,"y":765},{"type":"block_black_inner_square","x":65,"y":105},{"type":"block_black_inner_square","x":95,"y":135},{"type":"block_black_inner_square","x":5,"y":15},{"type":"block_black_inner_square","x":5,"y":105},{"type":"block_black_inner_square","x":35,"y":105},{"type":"block_black_inner_square","x":35,"y":75},{"type":"block_black_inner_square","x":5,"y":75},{"type":"block_black_inner_square","x":5,"y":45},{"type":"block_black_inner_square","x":35,"y":45},{"type":"block_black_inner_square","x":65,"y":75},{"type":"block_black_inner_square","x":65,"y":45},{"type":"block_black_inner_square","x":95,"y":75},{"type":"block_black_inner_square","x":95,"y":105},{"type":"block_black_inner_square","x":95,"y":45},{"type":"block_black_inner_square","x":95,"y":15},{"type":"block_black_inner_square","x":65,"y":-15},{"type":"block_black_inner_square","x":35,"y":-15},{"type":"block_black_inner_square","x":5,"y":-15},{"type":"block_black_inner_square","x":35,"y":15},{"type":"block_black_inner_square","x":65,"y":15},{"type":"effect_pulsing_music_note","x":58,"y":355},{"type":"effect_pulsing_music_note","x":56,"y":353},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"alpha_trigger","x":95,"y":855},{"type":"decor_medium_decorative_gear","x":5,"y":285},{"type":"block_black_inner_square","x":125,"y":15},{"type":"block_black_inner_square","x":155,"y":15},{"type":"block_black_inner_square","x":185,"y":15},{"type":"block_black_inner_square","x":155,"y":45},{"type":"block_black_inner_square","x":185,"y":45},{"type":"block_black_inner_square","x":125,"y":45},{"type":"block_black_inner_square","x":125,"y":75},{"type":"block_black_inner_square","x":155,"y":75},{"type":"block_black_inner_square","x":185,"y":75},{"type":"effect_pulsing_music_note","x":111,"y":214},{"type":"effect_pulsing_music_note","x":110,"y":213},{"type":"block_black_inner_square","x":125,"y":315},{"type":"block_black_inner_square","x":125,"y":345},{"type":"block_black_inner_square","x":125,"y":375},{"type":"block_black_inner_square","x":125,"y":405},{"type":"block_black_inner_square","x":125,"y":435},{"type":"block_black_inner_square","x":125,"y":465},{"type":"block_black_inner_square","x":125,"y":495},{"type":"block_black_inner_square","x":125,"y":525},{"type":"block_black_inner_square","x":125,"y":555},{"type":"block_black_inner_square","x":155,"y":315},{"type":"block_black_inner_square","x":155,"y":555},{"type":"block_black_inner_square","x":155,"y":525},{"type":"block_black_inner_square","x":155,"y":495},{"type":"block_black_inner_square","x":155,"y":465},{"type":"block_black_inner_square","x":155,"y":435},{"type":"block_black_inner_square","x":155,"y":405},{"type":"block_black_inner_square","x":155,"y":375},{"type":"block_black_inner_square","x":155,"y":345},{"type":"block_black_inner_square","x":275,"y":75},{"type":"block_black_inner_square","x":275,"y":45},{"type":"block_black_inner_square","x":275,"y":105},{"type":"block_black_inner_square","x":275,"y":15}]},
    {"level_name":"Promises","author":"Adiale","theme":"clean-modern","difficulty":"medium","tags":["modern","medium","clean","decorated"],"description":"A clean, well-paced modern level. Smooth color transitions between sections, tasteful decoration without overload. Good reference for 'modern but not flashy' design \u2014 readable gameplay with subtle visual polish.","orig_x_range":[10000,10500],"object_count":75,"objects":[{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":5,"y":124},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":35,"y":124},{"type":"block_black_inner_square","x":5,"y":105},{"type":"block_black_inner_square","x":35,"y":105},{"type":"block_black_inner_square","x":35,"y":75},{"type":"block_black_inner_square","x":5,"y":75},{"type":"block_black_inner_square","x":5,"y":15},{"type":"block_black_inner_square","x":35,"y":15},{"type":"block_black_inner_square","x":35,"y":45},{"type":"block_black_inner_square","x":5,"y":45},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":95,"y":356},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":65,"y":356},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":35,"y":356},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":5,"y":356},{"type":"block_black_inner_square","x":95,"y":405},{"type":"block_black_inner_square","x":65,"y":405},{"type":"block_black_inner_square","x":35,"y":405},{"type":"block_black_inner_square","x":5,"y":405},{"type":"block_black_inner_square","x":5,"y":375},{"type":"block_black_inner_square","x":35,"y":375},{"type":"block_black_inner_square","x":65,"y":375},{"type":"block_black_inner_square","x":95,"y":375},{"type":"alpha_trigger","x":95,"y":645},{"type":"jump_orb_yellow_jump_orb","x":185,"y":285},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":185,"y":356},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":155,"y":356},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":125,"y":356},{"type":"block_black_inner_square","x":185,"y":405},{"type":"block_black_inner_square","x":155,"y":405},{"type":"block_black_inner_square","x":125,"y":405},{"type":"block_black_inner_square","x":125,"y":375},{"type":"block_black_inner_square","x":155,"y":375},{"type":"block_black_inner_square","x":185,"y":375},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":185,"y":124},{"type":"block_black_inner_square","x":185,"y":105},{"type":"block_black_inner_square","x":185,"y":75},{"type":"block_black_inner_square","x":185,"y":45},{"type":"block_black_inner_square","x":185,"y":15},{"type":"decor_medium_decorative_gear","x":166,"y":364},{"type":"decor_very_small_decorative_gear","x":185,"y":285},{"type":"alpha_trigger","x":125,"y":645},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":245,"y":356},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":215,"y":356},{"type":"block_black_inner_square","x":245,"y":375},{"type":"block_black_inner_square","x":245,"y":405},{"type":"block_black_inner_square","x":215,"y":405},{"type":"block_black_inner_square","x":215,"y":375},{"type":"pulse_trigger","x":255,"y":645},{"type":"pulse_trigger","x":255,"y":675},{"type":"pulse_trigger","x":255,"y":705},{"type":"pulse_trigger","x":255,"y":735},{"type":"pulse_trigger","x":255,"y":765},{"type":"pulse_trigger","x":255,"y":795},{"type":"pulse_trigger","x":255,"y":825},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":275,"y":124},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":245,"y":124},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":215,"y":124},{"type":"block_black_inner_square","x":215,"y":45},{"type":"block_black_inner_square","x":215,"y":15},{"type":"block_black_inner_square","x":245,"y":15},{"type":"block_black_inner_square","x":275,"y":15},{"type":"block_black_inner_square","x":275,"y":45},{"type":"block_black_inner_square","x":245,"y":45},{"type":"block_black_inner_square","x":215,"y":75},{"type":"block_black_inner_square","x":245,"y":75},{"type":"block_black_inner_square","x":275,"y":75},{"type":"block_black_inner_square","x":215,"y":105},{"type":"block_black_inner_square","x":245,"y":105},{"type":"block_black_inner_square","x":275,"y":105},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":395,"y":124},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":365,"y":124},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":335,"y":124},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":305,"y":124},{"type":"block_black_inner_square","x":305,"y":15},{"type":"block_black_inner_square","x":335,"y":15}]},
    {"level_name":"Adrift","author":"TamaN","theme":"ambient-calm","difficulty":"medium","tags":["modern","medium","ambient","calm","flow"],"description":"Ambient/floaty atmosphere with smooth flow gameplay. Subtle alpha fades, slow color drifts, low-intensity hazards. Lots of negative space; objects positioned for visual breathing room rather than dense gameplay.","orig_x_range":[13500,14000],"object_count":75,"objects":[{"type":"block_black_inner_square","x":45,"y":255},{"type":"block_black_inner_square","x":45,"y":225},{"type":"block_black_inner_square","x":45,"y":195},{"type":"block_black_inner_square","x":45,"y":165},{"type":"block_black_inner_square","x":45,"y":135},{"type":"block_black_inner_square","x":75,"y":255},{"type":"block_black_inner_square","x":75,"y":195},{"type":"block_black_inner_square","x":75,"y":165},{"type":"block_black_inner_square","x":75,"y":225},{"type":"block_black_inner_square","x":75,"y":135},{"type":"block_black_inner_square","x":45,"y":105},{"type":"block_black_inner_square","x":75,"y":105},{"type":"block_black_inner_square","x":75,"y":75},{"type":"block_black_inner_square","x":45,"y":75},{"type":"block_black_inner_square","x":45,"y":45},{"type":"block_black_inner_square","x":75,"y":45},{"type":"effect_pulsing_filled_circle","x":13,"y":483},{"type":"effect_pulsing_filled_circle","x":20,"y":407},{"type":"block_black_gradient_single_slab","x":105,"y":353},{"type":"spike_small_decorative_spikes","x":195,"y":343},{"type":"block_black_inner_square","x":105,"y":225},{"type":"block_black_inner_square","x":105,"y":195},{"type":"block_black_inner_square","x":105,"y":165},{"type":"block_black_inner_square","x":105,"y":135},{"type":"block_black_inner_square","x":135,"y":195},{"type":"block_black_inner_square","x":135,"y":165},{"type":"block_black_inner_square","x":135,"y":135},{"type":"block_black_inner_square","x":105,"y":105},{"type":"block_black_inner_square","x":135,"y":105},{"type":"block_black_inner_square","x":135,"y":75},{"type":"block_black_inner_square","x":105,"y":75},{"type":"block_black_inner_square","x":105,"y":45},{"type":"block_black_inner_square","x":135,"y":45},{"type":"small_fading_cloud","x":197,"y":193,"rotation":90.0},{"type":"small_fading_cloud","x":197,"y":117,"rotation":90.0},{"type":"block_black_gradient_single_slab","x":285,"y":353},{"type":"block_black_pillar_square","x":285,"y":225,"rotation":90.0},{"type":"block_mechanical_inner_square","x":285,"y":105},{"type":"block_mechanical_inner_square","x":285,"y":195},{"type":"block_mechanical_inner_square","x":285,"y":135},{"type":"block_mechanical_inner_square","x":285,"y":165},{"type":"block_black_outer_corner_square","x":225,"y":225},{"type":"block_black_pillar_square","x":225,"y":195},{"type":"block_black_pillar_square","x":225,"y":165},{"type":"block_black_pillar_square","x":225,"y":135},{"type":"block_black_pillar_square","x":225,"y":105},{"type":"block_black_pillar_square","x":255,"y":225,"rotation":90.0},{"type":"block_mechanical_inner_square","x":255,"y":195},{"type":"block_mechanical_inner_square","x":255,"y":165},{"type":"block_mechanical_inner_square","x":255,"y":135},{"type":"block_mechanical_inner_square","x":255,"y":105},{"type":"effect_pulsing_filled_circle","x":282,"y":289},{"type":"effect_pulsing_filled_circle","x":271,"y":457},{"type":"effect_pulsing_filled_circle","x":202,"y":273},{"type":"block_black_gradient_single_slab","x":345,"y":323},{"type":"block_black_pillar_square","x":315,"y":225,"rotation":90.0},{"type":"block_black_pillar_square","x":345,"y":225,"rotation":90.0},{"type":"block_black_pillar_square","x":375,"y":195},{"type":"block_black_pillar_square","x":375,"y":165},{"type":"block_black_pillar_square","x":375,"y":135},{"type":"block_black_pillar_square","x":375,"y":105},{"type":"block_black_outer_corner_square","x":375,"y":225,"rotation":90.0},{"type":"block_mechanical_inner_square","x":315,"y":105},{"type":"block_mechanical_inner_square","x":345,"y":105},{"type":"block_mechanical_inner_square","x":345,"y":135},{"type":"block_mechanical_inner_square","x":345,"y":165},{"type":"block_mechanical_inner_square","x":345,"y":195},{"type":"block_mechanical_inner_square","x":315,"y":195},{"type":"block_mechanical_inner_square","x":315,"y":135},{"type":"block_mechanical_inner_square","x":315,"y":165},{"type":"alpha_trigger","x":315,"y":405},{"type":"block_black_gradient_single_slab","x":405,"y":353},{"type":"block_black_gradient_single_slab","x":465,"y":323},{"type":"block_black_inner_square","x":495,"y":225},{"type":"block_black_inner_square","x":495,"y":195}]},
    {"level_name":"Adrift","author":"TamaN","theme":"ambient-calm","difficulty":"medium","tags":["modern","medium","ambient","calm","flow"],"description":"Ambient/floaty atmosphere with smooth flow gameplay. Subtle alpha fades, slow color drifts, low-intensity hazards. Lots of negative space; objects positioned for visual breathing room rather than dense gameplay.","orig_x_range":[18500,19000],"object_count":75,"objects":[{"type":"spike_medium_decorative_spikes","x":25,"y":259},{"type":"block_black_inner_square","x":85,"y":405},{"type":"block_black_inner_square","x":55,"y":405},{"type":"block_black_inner_square","x":25,"y":405},{"type":"block_black_inner_square","x":25,"y":435},{"type":"block_black_inner_square","x":55,"y":435},{"type":"block_black_inner_square","x":85,"y":435},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":25,"y":154},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":55,"y":154},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":85,"y":154},{"type":"block_black_inner_square","x":85,"y":135},{"type":"block_black_inner_square","x":55,"y":135},{"type":"block_black_inner_square","x":25,"y":135},{"type":"block_black_inner_square","x":25,"y":15},{"type":"block_black_inner_square","x":25,"y":45},{"type":"block_black_inner_square","x":55,"y":45},{"type":"block_black_inner_square","x":55,"y":15},{"type":"block_black_inner_square","x":85,"y":15},{"type":"block_black_inner_square","x":85,"y":45},{"type":"block_black_inner_square","x":55,"y":75},{"type":"block_black_inner_square","x":25,"y":75},{"type":"block_black_inner_square","x":85,"y":75},{"type":"block_black_inner_square","x":55,"y":105},{"type":"block_black_inner_square","x":25,"y":105},{"type":"block_black_inner_square","x":85,"y":105},{"type":"large_fading_cloud","x":85,"y":167},{"type":"large_fading_cloud","x":25,"y":373,"rotation":180.0},{"type":"spike_very_small_decorative_spikes","x":163,"y":217},{"type":"spike_medium_decorative_spikes","x":115,"y":311,"rotation":180.0},{"type":"block_black_inner_square","x":175,"y":405},{"type":"block_black_inner_square","x":145,"y":405},{"type":"block_black_inner_square","x":115,"y":405},{"type":"block_black_inner_square","x":115,"y":435},{"type":"block_black_inner_square","x":145,"y":435},{"type":"block_black_inner_square","x":175,"y":435},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":115,"y":154},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":145,"y":154},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":175,"y":154},{"type":"block_black_inner_square","x":175,"y":135},{"type":"block_black_inner_square","x":145,"y":135},{"type":"block_black_inner_square","x":145,"y":105},{"type":"block_black_inner_square","x":145,"y":75},{"type":"block_black_inner_square","x":145,"y":45},{"type":"block_black_inner_square","x":145,"y":15},{"type":"block_black_inner_square","x":115,"y":15},{"type":"block_black_inner_square","x":175,"y":105},{"type":"block_black_inner_square","x":175,"y":75},{"type":"block_black_inner_square","x":175,"y":45},{"type":"block_black_inner_square","x":175,"y":15},{"type":"block_black_inner_square","x":115,"y":45},{"type":"block_black_inner_square","x":115,"y":75},{"type":"block_black_inner_square","x":115,"y":105},{"type":"block_black_inner_square","x":115,"y":135},{"type":"small_fading_cloud","x":145,"y":377,"rotation":180.0},{"type":"portal_cube_portal","x":205,"y":255},{"type":"block_black_gradient_single_slab","x":235,"y":173},{"type":"block_black_inner_square","x":205,"y":435},{"type":"block_black_inner_square","x":235,"y":465},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":205,"y":154},{"type":"hazard_non_colorable_wavy_black_pit_hazard","x":235,"y":154},{"type":"block_black_inner_square","x":295,"y":15},{"type":"block_black_inner_square","x":295,"y":45},{"type":"block_black_inner_square","x":295,"y":75},{"type":"block_black_inner_square","x":265,"y":15},{"type":"block_black_inner_square","x":265,"y":45},{"type":"block_black_inner_square","x":265,"y":75},{"type":"block_black_inner_square","x":235,"y":45},{"type":"block_black_inner_square","x":235,"y":15},{"type":"block_black_inner_square","x":235,"y":75},{"type":"block_black_inner_square","x":235,"y":105},{"type":"block_black_inner_square","x":235,"y":135},{"type":"block_black_inner_square","x":205,"y":135},{"type":"block_black_inner_square","x":205,"y":75},{"type":"block_black_inner_square","x":205,"y":105},{"type":"block_black_inner_square","x":205,"y":15}]},
    {"level_name":"Adrift","author":"TamaN","theme":"ambient-calm","difficulty":"medium","tags":["modern","medium","ambient","calm","flow"],"description":"Ambient/floaty atmosphere with smooth flow gameplay. Subtle alpha fades, slow color drifts, low-intensity hazards. Lots of negative space; objects positioned for visual breathing room rather than dense gameplay.","orig_x_range":[28000,28500],"object_count":75,"objects":[{"type":"block_mechanical_pillar_square","x":5,"y":45},{"type":"block_mechanical_pillar_square","x":5,"y":15},{"type":"block_mechanical_pillar_square","x":65,"y":75},{"type":"block_mechanical_pillar_square","x":65,"y":45},{"type":"block_mechanical_pillar_square","x":65,"y":15},{"type":"block_mechanical_pillar_square","x":95,"y":105,"rotation":90.0},{"type":"block_mechanical_pillar_square","x":65,"y":495,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":65,"y":465,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":65,"y":435,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":95,"y":405,"rotation":270.0},{"type":"effect_pulsing_filled_circle","x":5,"y":435},{"type":"effect_pulsing_filled_circle","x":5,"y":315},{"type":"effect_pulsing_filled_circle","x":5,"y":195},{"type":"block_mechanical_pillar_square","x":125,"y":105,"rotation":90.0},{"type":"block_mechanical_pillar_square","x":155,"y":105,"rotation":90.0},{"type":"block_mechanical_pillar_square","x":185,"y":75,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":185,"y":45,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":185,"y":15,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":185,"y":435,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":185,"y":465,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":185,"y":495,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":155,"y":405,"rotation":270.0},{"type":"block_mechanical_pillar_square","x":125,"y":405,"rotation":270.0},{"type":"effect_pulsing_filled_circle","x":129,"y":46},{"type":"effect_pulsing_filled_circle","x":129,"y":486},{"type":"block_mechanical_pillar_square","x":275,"y":105,"rotation":90.0},{"type":"block_mechanical_pillar_square","x":245,"y":105,"rotation":90.0},{"type":"block_mechanical_pillar_square","x":215,"y":105,"rotation":90.0},{"type":"block_mechanical_pillar_square","x":245,"y":165,"rotation":90.0},{"type":"block_mechanical_pillar_square","x":275,"y":165,"rotation":90.0},{"type":"block_mechanical_pillar_square","x":275,"y":345,"rotation":270.0},{"type":"block_mechanical_pillar_square","x":245,"y":345,"rotation":270.0},{"type":"block_mechanical_pillar_square","x":245,"y":405,"rotation":270.0},{"type":"block_mechanical_pillar_square","x":215,"y":405,"rotation":270.0},{"type":"block_mechanical_pillar_square","x":275,"y":405,"rotation":270.0},{"type":"effect_pulsing_filled_circle","x":250,"y":42},{"type":"effect_pulsing_filled_circle","x":250,"y":467},{"type":"effect_pulsing_filled_circle","x":239,"y":315},{"type":"effect_pulsing_filled_circle","x":239,"y":225},{"type":"block_mechanical_pillar_square","x":305,"y":135,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":305,"y":75,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":305,"y":45,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":305,"y":15,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":305,"y":495,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":305,"y":465,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":305,"y":435,"rotation":180.0},{"type":"block_mechanical_pillar_square","x":305,"y":375,"rotation":180.0},{"type":"block_black_inner_square","x":335,"y":105},{"type":"block_black_inner_square","x":335,"y":75},{"type":"block_black_inner_square","x":335,"y":45},{"type":"block_black_inner_square","x":365,"y":15},{"type":"block_black_inner_square","x":335,"y":15},{"type":"block_black_inner_square","x":365,"y":105},{"type":"block_black_inner_square","x":365,"y":75},{"type":"block_black_inner_square","x":365,"y":45},{"type":"block_black_inner_square","x":335,"y":495},{"type":"block_black_inner_square","x":335,"y":465},{"type":"block_black_inner_square","x":335,"y":435},{"type":"block_black_inner_square","x":335,"y":405},{"type":"block_black_inner_square","x":365,"y":495},{"type":"block_black_inner_square","x":365,"y":465},{"type":"block_black_inner_square","x":365,"y":435},{"type":"block_black_inner_square","x":365,"y":405},{"type":"block_mechanical_pillar_square","x":395,"y":135},{"type":"block_mechanical_pillar_square","x":395,"y":105},{"type":"block_mechanical_pillar_square","x":395,"y":75},{"type":"block_mechanical_pillar_square","x":395,"y":45},{"type":"block_mechanical_pillar_square","x":395,"y":15},{"type":"block_mechanical_pillar_square","x":395,"y":375},{"type":"block_mechanical_pillar_square","x":395,"y":405},{"type":"block_mechanical_pillar_square","x":395,"y":435},{"type":"block_mechanical_pillar_square","x":395,"y":465},{"type":"block_mechanical_pillar_square","x":395,"y":495},{"type":"portal_flipped_gravity_portal","x":350,"y":255},{"type":"block_black_gradient_square","x":365,"y":345}]}
  ]
}

)eai_v2";

// ── GD Creator School design knowledge ──────────────────────────────────────
// Ultra-compact distillation of community level-design guidance from
// https://www.gdcreatorschool.com. The full text lives in
// resources/gdcs_design.txt; this constant is the in-prompt digest
// (~700 bytes / ~180 tokens, down from ~4 KB) so it fits cleanly inside
// any model's context.
inline constexpr std::string_view GDCS_DESIGN_TIPS = R"gdcs(
DESIGN TIPS (GD Creator School digest):
- Pick difficulty first; pace to match. One theme per section.
- Layers: gameplay → decoration → triggers (decoration follows layout).
- Vary segment lengths; alternate tense bursts with breathers.
- Place orbs/pads 30-60u BEFORE the obstacle they bypass.
- Ramp difficulty: ~30% intro / ~50% main / ~20% climax.
- ≤10 color channels per section; backgrounds complement gameplay color.
- Z LAYER: T1-T4 above player, B1-B4 below (blocks default B2, decor T1/T2 or B3/B4).
- Triggers fire when X is crossed; place slightly BEFORE the visible effect.
- Avoid: spammed identical objects, sub-90u jump gaps, decoration covering hazards.
- Macros (FLOOR/CORRIDOR/PILLAR/SPIKE-TRAIN/PLATFORM-RUN) expand from one line — prefer them for repetitive shapes.
)gdcs";

} // namespace editorai

// ─── OAuth plumbing ──────────────────────────────────────────────────────────
//
// One-click sign-in. The only provider with a working flow is:
//   • HuggingFace → OAuth 2.0 + PKCE with loopback redirect.
// (Gemini device flow and OpenRouter PKCE were investigated and abandoned —
// see the notes near HF_CLIENT_ID below.)
//
// Everything else still uses the manual paste-an-API-key path; this
// namespace ships only what's needed for the three above. Tokens land in
// Mod::get()->setSavedValue() (not user-facing settings) so the per-provider
// API key fields in mod.json keep their "user types it" semantics. The
// API-call paths prefer a saved OAuth token if one exists, fall through to
// the manual key otherwise.
//
// Tokens are stored in plaintext in Geode save data — same posture as the
// existing manual API keys. Not worse, not better.

#include <Geode/Geode.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include "sessions.hpp"
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#ifdef _WIN32
  // Geode's Windows toolchain already pulls in <winsock.h> (v1) via
  // <windows.h>. Re-including <winsock2.h> here would cause hostent /
  // WSAData / etc. redefinitions. winsock.h has every API we need
  // (socket/bind/listen/accept/recv/send/WSAStartup) so we just rely on
  // whatever Geode already brought in and tell the linker to pull Ws2_32.lib.
  #pragma comment(lib, "Ws2_32.lib")
  using socklen_compat = int;
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socklen_compat = socklen_t;
#endif

namespace oauth {

// ── OAuth app credentials (placeholders — replace before shipping) ──────────
//
// HuggingFace: register at https://huggingface.co/settings/applications/new
//   Redirect URI: http://127.0.0.1:<port>/cb (any port, loopback is allowed
//   by the HF console). Scopes: `inference-api` at minimum.
//
// Google (Gemini): register at https://console.cloud.google.com/apis/credentials
//   App type: "TVs and Limited Input devices" (enables device flow).
//   Required scope: https://www.googleapis.com/auth/generative-language
//
// OpenRouter: no registration needed — the public auth endpoint works for
//   any caller via PKCE. callback_url just has to be reachable; we use
//   http://127.0.0.1:<port>/cb.
//
// When a client_id is left as the empty string the matching flow surfaces
// a friendly "developer hasn't registered an app yet" error instead of
// silently failing.
inline constexpr const char* HF_CLIENT_ID       = "b603d351-80a3-4969-b9eb-e1be97f3aa50";
// HuggingFace requires the redirect_uri to be an exact match for one of the
// URIs registered on the OAuth app — they don't accept port ranges. Pin a
// single high-numbered port so the registered URI stays stable. If the port
// is taken at runtime the listener fails with a clear error rather than
// silently rebinding to a random port the OAuth registration wouldn't accept.
inline constexpr int         HF_REDIRECT_PORT   = 53947;
inline constexpr const char* HF_SCOPES          = "inference-api";
// Google (Gemini) OAuth intentionally NOT supported: the only valid scope
// forces a GCP-project header, breaking the "click a button, done" promise.

// ── SHA-256 (public domain — minimal RFC 6234 implementation) ──────────────
// Just enough to compute the PKCE S256 challenge from a verifier string.
// We don't need a streaming API; one-shot hash is fine.
struct Sha256 {
    static constexpr uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
        0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
        0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
        0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
        0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
        0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    static std::vector<uint8_t> hash(const std::string& in) {
        uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                         0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        std::vector<uint8_t> msg(in.begin(), in.end());
        uint64_t bitLen = (uint64_t)msg.size() * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0);
        for (int i = 7; i >= 0; --i) msg.push_back((uint8_t)(bitLen >> (i * 8)));

        for (size_t off = 0; off < msg.size(); off += 64) {
            uint32_t w[64] = {0};
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t)msg[off + i*4] << 24 | (uint32_t)msg[off + i*4 + 1] << 16
                     | (uint32_t)msg[off + i*4 + 2] << 8  | (uint32_t)msg[off + i*4 + 3];
            for (int i = 16; i < 64; ++i) {
                uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
                uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19)  ^ (w[i-2] >> 10);
                w[i] = w[i-16] + s0 + w[i-7] + s1;
            }
            uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
            for (int i = 0; i < 64; ++i) {
                uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
                uint32_t ch = (e & f) ^ (~e & g);
                uint32_t t1 = hh + S1 + ch + K[i] + w[i];
                uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
                uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t t2 = S0 + mj;
                hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
            }
            h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
        }
        std::vector<uint8_t> out(32);
        for (int i = 0; i < 8; ++i) {
            out[i*4]   = (uint8_t)(h[i] >> 24);
            out[i*4+1] = (uint8_t)(h[i] >> 16);
            out[i*4+2] = (uint8_t)(h[i] >> 8);
            out[i*4+3] = (uint8_t)h[i];
        }
        return out;
    }
};

// ── base64url-without-padding ──────────────────────────────────────────────
inline std::string b64url(const uint8_t* data, size_t len) {
    static constexpr char ALPH[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) v |= ((uint32_t)data[i+1]) << 8;
        if (i + 2 < len) v |= (uint32_t)data[i+2];
        out.push_back(ALPH[(v >> 18) & 0x3F]);
        out.push_back(ALPH[(v >> 12) & 0x3F]);
        if (i + 1 < len) out.push_back(ALPH[(v >> 6) & 0x3F]);
        if (i + 2 < len) out.push_back(ALPH[v & 0x3F]);
    }
    return out;
}

// 32 random bytes → 43-character base64url string. Used as the PKCE
// code_verifier. std::random_device is good enough on win/mac/linux.
inline std::string randomVerifier() {
    std::random_device rd;
    std::mt19937_64 gen(((uint64_t)rd() << 32) | rd());
    uint8_t buf[32];
    for (size_t i = 0; i < sizeof buf; i += 8) {
        uint64_t v = gen();
        for (int j = 0; j < 8 && i + j < sizeof buf; ++j)
            buf[i + j] = (uint8_t)(v >> (j * 8));
    }
    return b64url(buf, sizeof buf);
}

inline std::string s256Challenge(const std::string& verifier) {
    auto h = Sha256::hash(verifier);
    return b64url(h.data(), h.size());
}

// ── Minimal local HTTP listener ────────────────────────────────────────────
// One-shot server bound to 127.0.0.1. Accepts a single HTTP request, captures
// its query string ("code=...&state=..."), sends a "you can close this tab"
// HTML response, and shuts down. Runs on a worker thread; the calling popup
// polls completion via a flag.
//
// `desiredPort` lets the caller pin a specific port (required for OAuth
// providers that demand an exact-match registered redirect URI — Hugging
// Face does). Pass 0 to let the OS pick an ephemeral port.
class LocalCallback {
public:
    bool start(int desiredPort, int& portOut) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;
#endif
        m_sock = (int)::socket(AF_INET, SOCK_STREAM, 0);
        if (m_sock < 0) return false;
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons((unsigned short)desiredPort);  // 0 = OS picks
        if (::bind(m_sock, (sockaddr*)&addr, sizeof addr) != 0) { close(); return false; }
        socklen_compat len = sizeof addr;
        if (::getsockname(m_sock, (sockaddr*)&addr, &len) != 0) { close(); return false; }
        portOut = ntohs(addr.sin_port);
        if (::listen(m_sock, 1) != 0) { close(); return false; }
        m_thread = std::thread([this]{ runOnce(); });
        return true;
    }

    // True once a request has been received (or the listener errored/stopped).
    bool done() const { return m_done.load(); }
    std::string query() const { return m_query; }

    void stop() {
        m_stopRequested = true;
        // Force-close the listening socket so the accept() returns.
        close();
        if (m_thread.joinable()) m_thread.join();
    }

    ~LocalCallback() { stop(); }

private:
    // Wait until the socket is readable or stop is requested. Closing a socket
    // that another thread is blocked in accept()/recv() on does not reliably
    // wake it on POSIX, so poll in short slices instead of blocking forever.
    bool waitReadable(int sock) {
        while (!m_stopRequested) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            timeval tv{0, 250000};  // 250 ms slices
            int r = ::select(sock + 1, &fds, nullptr, nullptr, &tv);
            if (r > 0)  return true;
            if (r < 0)  return false;  // socket closed/errored
        }
        return false;
    }

    void runOnce() {
        if (!waitReadable(m_sock)) { m_done = true; return; }
        sockaddr_in clientAddr{};
        socklen_compat clen = sizeof clientAddr;
        int client = (int)::accept(m_sock, (sockaddr*)&clientAddr, &clen);
        if (client < 0) { m_done = true; return; }

        // Read the request line. HTTP GET; we only need "GET /cb?<query> HTTP/1.1"
        char buf[4096];
        if (!waitReadable(client)) {
#ifdef _WIN32
            ::closesocket(client);
#else
            ::close(client);
#endif
            m_done = true;
            return;
        }
#ifdef _WIN32
        int n = ::recv(client, buf, sizeof buf - 1, 0);
#else
        ssize_t n = ::recv(client, buf, sizeof buf - 1, 0);
#endif
        if (n > 0) {
            buf[n] = 0;
            std::string req(buf);
            // Find "GET <path> HTTP/"
            size_t sp1 = req.find(' ');
            size_t sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
                auto q = path.find('?');
                if (q != std::string::npos) m_query = path.substr(q + 1);
            }
        }
        const char* body =
            "<html><body style='font-family:sans-serif;text-align:center;padding:48px'>"
            "<h2>You can close this tab.</h2>"
            "<p>Geometry Dash will pick it up from here.</p>"
            "</body></html>";
        char resp[512];
        int rlen = std::snprintf(resp, sizeof resp,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s",
            std::strlen(body), body);
        if (rlen > 0) ::send(client, resp, rlen, 0);
#ifdef _WIN32
        ::closesocket(client);
#else
        ::close(client);
#endif
        m_done = true;
    }

    void close() {
        if (m_sock >= 0) {
#ifdef _WIN32
            ::closesocket((SOCKET)m_sock);
            WSACleanup();
#else
            ::close(m_sock);
#endif
            m_sock = -1;
        }
    }

    int  m_sock = -1;
    std::string m_query;
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;
};

// Stored credential helpers — saved value, not setting. Settings stay
// user-editable for the manual key path; OAuth tokens are mod-managed.
// We fully qualify `geode::Mod` because this namespace is defined BEFORE
// the file's `using namespace geode::prelude;`.
inline int64_t nowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline std::string savedToken(const std::string& provider) {
    auto tok = geode::Mod::get()->getSavedValue<std::string>(provider + "-oauth-token");
    if (tok.empty()) return tok;
    // An expired token must not shadow a manually pasted API key —
    // getProviderApiKey prefers the OAuth token when present.
    int64_t exp = geode::Mod::get()->getSavedValue<int64_t>(provider + "-oauth-expires-at");
    if (exp != 0 && nowEpochSeconds() >= exp) return "";
    return tok;
}
inline void setSavedToken(const std::string& provider, const std::string& token) {
    geode::Mod::get()->setSavedValue<std::string>(provider + "-oauth-token", token);
}
// expiresIn = seconds from now; 0 = never expires.
inline void setSavedExpiry(const std::string& provider, int64_t expiresIn) {
    geode::Mod::get()->setSavedValue<int64_t>(provider + "-oauth-expires-at",
        expiresIn > 0 ? nowEpochSeconds() + expiresIn : 0);
}
inline void clearSavedToken(const std::string& provider) {
    setSavedToken(provider, "");
    geode::Mod::get()->setSavedValue<std::string>(provider + "-oauth-refresh", "");
    geode::Mod::get()->setSavedValue<int64_t>(provider + "-oauth-expires-at", 0);
}

} // namespace oauth

// Multi-turn tool-use support for every supported AI provider EXCEPT custom.
//
// Architecture:
//   1. We define a small, fixed catalog of tools the AI can call (web_search,
//      download_level, search_newgrounds, get_newgrounds_song, analyze_level,
//      ...). Each tool has a JSON schema the providers understand.
//   2. We maintain a provider-agnostic conversation history (toolUse::Message).
//   3. For each turn:
//        - Build the provider-specific request body from the history.
//        - POST it to the provider.
//        - Parse the response into ParsedResponse: either text (final answer)
//          or a list of ToolCall structs.
//        - If tool calls: execute each (via callbacks the popup wires up),
//          append both the assistant turn and the tool-results turn to the
//          history, loop. If text: that's our final aiResponse — same path
//          as the existing single-shot.
//
// The loop is UNBOUNDED: the AI runs as many tool rounds — and as many calls
// to any single tool — as it wants. There is no round-count setting and no
// per-tool "you've called this N times, stop" cap. The only guards are an
// exact-duplicate guard (a repeat of the SAME query just reuses the prior
// result, never a stop signal) and a high anti-runaway backstop that
// finalizes gracefully (see doToolRound).
//
// Each provider has its own native tool-use format:
//   - OpenAI / Mistral / HuggingFace / OpenRouter / DeepSeek / LM Studio /
//     llama.cpp / Ollama (/api/chat) all share the OpenAI tools schema.
//   - Anthropic Claude uses content blocks with type=tool_use / tool_result.
//   - Gemini uses functionDeclarations + functionCall / functionResponse parts.
//
// "custom" provider is intentionally skipped: we don't know what tool-use
// format the user's endpoint speaks. The single-shot path still works for it.

#include <Geode/Geode.hpp>
#include <array>
#include <functional>
#include <string>
#include <vector>

// Defined later in this file; needed by the request builders below to skip
// the temperature param on OpenAI o-series reasoning models.
static bool isOSeriesModel(const std::string& model);

namespace toolUse {

// Tool-call rounds are unbounded — there is no round budget. See the loop
// in AIGeneratorPopup::doToolRound for the structural runaway guards.

// Tools the AI can call. Anything outside this list is rejected during dispatch.
inline constexpr std::array<std::string_view, 12> KNOWN_TOOL_NAMES = {
    "web_search",
    "download_level",
    "search_newgrounds",
    "get_newgrounds_song",
    "analyze_level",
    "get_level_length",
    "search_objects",
    "check_passability",
    "analyze_difficulty_curve",
    "simulate_physics",
    "ask_subagent",
    "get_level_region",
};

// ── JSON schema for the tool catalog ─────────────────────────────────────────
// Returned in OpenAI-compatible "tools" array form. Claude/Gemini have
// slightly different wrappers but the same parameter shapes; their builders
// translate this representation on the fly.
inline matjson::Value openAIToolSchema(const std::string& name,
                                       const std::string& description,
                                       std::vector<std::pair<std::string, std::pair<std::string, std::string>>> params,
                                       std::vector<std::string> required)
{
    // params: (name, (type, description))
    auto props = matjson::Value::object();
    for (auto& [pname, ptype] : params) {
        auto p = matjson::Value::object();
        p["type"]        = ptype.first;
        p["description"] = ptype.second;
        props[pname] = p;
    }
    auto paramSchema = matjson::Value::object();
    paramSchema["type"]       = "object";
    paramSchema["properties"] = props;
    if (!required.empty()) {
        auto arr = matjson::Value::array();
        for (auto& r : required) arr.push(r);
        paramSchema["required"] = arr;
    }

    auto fn = matjson::Value::object();
    fn["name"]        = name;
    fn["description"] = description;
    fn["parameters"]  = paramSchema;

    auto wrap = matjson::Value::object();
    wrap["type"]     = "function";
    wrap["function"] = fn;
    return wrap;
}

// The full tool catalog (OpenAI-compat form). Builders for other providers
// can re-shape these on demand.
inline matjson::Value buildToolCatalog() {
    auto arr = matjson::Value::array();
    arr.push(openAIToolSchema(
        "web_search",
        "Search the public web for design inspiration, GD level references, "
        "song info, or anything else useful before composing the level. Returns "
        "the top 3 result titles and snippets.",
        {{"query", {"string", "What to search for. Keep concise."}}},
        {"query"}
    ));
    arr.push(openAIToolSchema(
        "download_level",
        "Download an existing Geometry Dash level by numeric ID. Returns a "
        "compact EAI-format summary (name, author, X-scale, top object types, "
        "sample of first 25 objects). Use this when the user mentions a "
        "specific GD level you want to draw inspiration from.",
        {{"level_id", {"integer", "Numeric GD level ID (e.g. 128 for Bloodbath)."}}},
        {"level_id"}
    ));
    arr.push(openAIToolSchema(
        "search_newgrounds",
        "Search Newgrounds Audio for a song by name/keyword. Returns the song "
        "title, artist, and the Newgrounds song ID (which you can pass to "
        "level_metadata.song_id in your final output).",
        {{"query", {"string", "Song title, artist, or keyword."}}},
        {"query"}
    ));
    arr.push(openAIToolSchema(
        "get_newgrounds_song",
        "Fetch metadata for a specific Newgrounds song by its numeric ID. Use "
        "when the user provides a song ID directly, or after search_newgrounds "
        "returns one.",
        {{"song_id", {"integer", "Newgrounds song ID."}}},
        {"song_id"}
    ));
    arr.push(openAIToolSchema(
        "analyze_level",
        "Inspect the editor's current level: object count, X span, dominant "
        "object types, current song. No parameters. Returns a JSON summary.",
        {},
        {}
    ));
    arr.push(openAIToolSchema(
        "get_level_length",
        "Return how long (in seconds at 1× speed) the level is RIGHT NOW, "
        "what category GD will classify it as (Tiny/Short/Medium/Long/XL), "
        "the target length range (from the user's \"length\" setting), and "
        "the X coordinate at which to KEEP BUILDING from. Call this often "
        "while composing — the mod will REJECT any final answer that's "
        "shorter than the target.",
        {},
        {}
    ));
    arr.push(openAIToolSchema(
        "search_objects",
        "Search the object catalog by substring (case-insensitive). Returns up "
        "to 30 matching object type names you can use in OBJ/JSON output. Use "
        "this to discover decoration, hazard, or themed block names that are "
        "not listed in your prompt — the full catalog has ~4000 entries.",
        {{"query", {"string", "Substring to match, e.g. 'saw', 'deco', 'glow', 'pillar'."}}},
        {"query"}
    ));
    arr.push(openAIToolSchema(
        "check_passability",
        "Run the mod's fly-anywhere pathfinder over the objects from your "
        "ACCEPTED DRAFTS so far (it sees nothing until after your first JSON "
        "answer — use it between EXTEND rounds, not before the first draft). "
        "Returns the passability percentage and a list of fully-blocked X "
        "zones (death zones) so you can fix impossible sections early — the "
        "mod will otherwise bounce them back to you for fixes.",
        {},
        {}
    ));
    if (!geode::Mod::get()->getSettingValue<std::string>("subagent-provider").empty()) {
        arr.push(openAIToolSchema(
            "ask_subagent",
            "Consult the user's configured second model (a different "
            "provider) with a focused question — e.g. ask a local model for "
            "themed object combinations, or a big model to sanity-check your "
            "plan. One question per call; answers are advisory.",
            {{"question", {"string", "A single focused question (max ~1500 chars)."}}},
            {"question"}
        ));
    }
    arr.push(openAIToolSchema(
        "simulate_physics",
        "Run a cube-physics bot through your ACCEPTED DRAFT: it jumps "
        "greedily over hazards and reports exactly where it dies (un-jumpable "
        "spike spacing, walls, bad landings). Much stricter than "
        "check_passability for cube sections. Works after your first draft.",
        {},
        {}
    ));
    arr.push(openAIToolSchema(
        "analyze_difficulty_curve",
        "Histogram of hazard density across your ACCEPTED DRAFT so far, in "
        "1200-unit windows (units: hazards per 1000u). Use it between EXTEND "
        "rounds to verify your pacing matches the requested difficulty ramp — "
        "sudden spikes or dead-flat stretches show up immediately.",
        {},
        {}
    ));
    arr.push(openAIToolSchema(
        "get_level_region",
        "List the CURRENT editor level's objects whose X falls inside "
        "[x_start, x_end] (type, x, y; capped at 80 objects). Much cheaper "
        "than analyze_level when you only need one section — ideal in edit "
        "mode or when extending an existing level.",
        {{"x_start", {"integer", "Left edge of the X range."}},
         {"x_end",   {"integer", "Right edge of the X range."}}},
        {"x_start", "x_end"}
    ));
    // (No "think" tool: reasoning belongs in plain assistant text, which the
    // overlay streams to the user live. A think tool just burned rounds.)
    return arr;
}

// ── Conversation types ─────────────────────────────────────────────────────
struct ToolCall {
    std::string id;    // tool_call_id (used by OpenAI / Claude; we synthesize for Gemini)
    std::string name;  // one of KNOWN_TOOL_NAMES
    matjson::Value args = matjson::Value::object();
    // Gemini 3: opaque signature attached to the functionCall PART; must be
    // replayed verbatim when the call is echoed back in history, or the API
    // 400s with "Function call is missing a thought_signature". Empty for
    // every other provider (and for unsigned parallel calls) — only emitted
    // when non-empty.
    std::string thoughtSignature;
};

struct ToolResult {
    std::string toolCallId;
    std::string content;
    bool isError = false;
};

enum class MessageRole {
    System,
    User,
    Assistant,
    ToolResults,
};

struct Message {
    MessageRole role = MessageRole::User;
    std::string text;                          // for system/user/assistant text
    std::vector<ToolCall>   toolCalls;         // when an assistant turn calls tools
    std::vector<ToolResult> toolResults;       // when delivering tool outputs
    // Optional inline image on USER turns (vision models see the level as
    // it builds). Base64 of the encoded image; empty = text-only.
    std::string imageB64;
    std::string imageMime = "image/png";
};

// ── Parsed-response abstraction ────────────────────────────────────────────
struct ParsedResponse {
    bool ok = false;
    std::string finalText;                     // present iff no tool calls
    std::vector<ToolCall> toolCalls;
    std::string assistantTextWithCalls;        // text the assistant emitted alongside the tool calls (rare but possible)
    std::string reasoningText;                 // provider-reported reasoning (DeepSeek reasoning_content, Claude thinking blocks) — display-only
    std::string errorMessage;                  // user-visible error string
};

// Which providers we route through the tool-use loop. "custom" intentionally
// excluded — we don't know what its tool-use API looks like.
inline bool supportsToolUse(const std::string& provider) {
    return provider == "openai"     || provider == "ministral"  ||
           provider == "huggingface"|| provider == "openrouter" ||
           provider == "deepseek"   || provider == "lm-studio"  ||
           provider == "llama-cpp"  ||
           provider == "claude"     ||
           provider == "gemini"     ||
           provider == "ollama";
}

// Vision: can this provider+model accept inline images? Claude and Gemini
// always can; everywhere else, sniff the model name for known vision
// families (conservative — a false negative just means text-only rounds).
inline bool supportsVision(const std::string& provider, const std::string& model) {
    if (provider == "claude" || provider == "gemini") return true;
    auto has = [&](const char* s) {
        return model.find(s) != std::string::npos;
    };
    if (provider == "openai")
        return has("4o") || has("4.1") || has("vision");
    return has("gpt-4o") || has("claude") || has("gemini") || has("llava") ||
           has("pixtral") || has("-vl") || has("vision") || has("4o") ||
           has("minicpm-v") || has("moondream");
}

// Endpoint URL for the chat/tool-use call. For most providers it's the same
// chat-completions URL we already use; Ollama needs /api/chat (not generate);
// Gemini needs the v1beta path with model in it.
std::string urlFor(const std::string& provider, const std::string& model);

// True for providers whose tools dialect is OpenAI's. These all share the same
// request builder and the same response parser.
inline bool isOpenAICompat(const std::string& provider) {
    return provider == "openai"     || provider == "ministral"  ||
           provider == "huggingface"|| provider == "openrouter" ||
           provider == "deepseek"   || provider == "lm-studio"  ||
           provider == "llama-cpp"  || provider == "ollama";
}

// Which token-limit field each OpenAI-compatible provider accepts, and how
// many tokens to ask for. Strict servers — HuggingFace's router and OpenAI
// itself among them — return HTTP 400 when BOTH max_tokens and
// max_completion_tokens appear in one request, so every body must carry
// exactly one. Single source of truth for the single-shot path (callAPI)
// and the tool-use loop (buildOpenAICompatRequest).
struct TokenLimitSpec { const char* field; int limit; };
inline TokenLimitSpec tokenLimitSpec(const std::string& provider) {
    if (provider == "openai" || provider == "lm-studio" || provider == "llama-cpp")
        return {"max_completion_tokens", 16384};
    if (provider == "ministral" || provider == "openrouter")
        return {"max_tokens", 16384};
    // huggingface, deepseek, custom, ollama (ignores it), anything else
    return {"max_tokens", 8192};
}

// ── Per-provider request builders ──────────────────────────────────────────
// Each takes the conversation so far and returns the provider's expected JSON
// body. The OpenAI-compat builder takes the provider too because Ollama —
// which speaks the same dialect for tools — has two quirks that need
// branching: it defaults to stream:true (which gives back NDJSON instead of
// a single JSON object, breaking the parser) and it expects
// tool_call.function.arguments to be an OBJECT, not a JSON-encoded string.
matjson::Value buildOpenAICompatRequest(const std::string& provider,
                                        const std::vector<Message>& history,
                                        const std::string& model);
matjson::Value buildClaudeRequest      (const std::vector<Message>& history,
                                        const std::string& model);
matjson::Value buildGeminiRequest      (const std::vector<Message>& history,
                                        const std::string& model);

// ── Per-provider response parsers ──────────────────────────────────────────
ParsedResponse parseOpenAICompatResponse(const matjson::Value& json);
ParsedResponse parseClaudeResponse      (const matjson::Value& json);
ParsedResponse parseGeminiResponse      (const matjson::Value& json);

// ── Dispatch by provider name ──────────────────────────────────────────────
matjson::Value buildRequest (const std::string& provider,
                             const std::vector<Message>& history,
                             const std::string& model);
ParsedResponse parseResponse(const std::string& provider,
                             const matjson::Value& json);

// ─────────────────────────────────────────────────────────────────────────
// Inline implementations
// ─────────────────────────────────────────────────────────────────────────

inline std::string urlFor(const std::string& provider, const std::string& model) {
    if (provider == "openai")      return "https://api.openai.com/v1/chat/completions";
    if (provider == "ministral")   return "https://api.mistral.ai/v1/chat/completions";
    if (provider == "huggingface") return "https://router.huggingface.co/v1/chat/completions";
    if (provider == "openrouter")  return "https://openrouter.ai/api/v1/chat/completions";
    if (provider == "deepseek")    return "https://api.deepseek.com/v1/chat/completions";
    if (provider == "lm-studio") {
        std::string base = geode::Mod::get()->getSettingValue<std::string>("lm-studio-url");
        return base + "/v1/chat/completions";
    }
    if (provider == "llama-cpp") {
        std::string base = geode::Mod::get()->getSettingValue<std::string>("llama-cpp-url");
        return base + "/v1/chat/completions";
    }
    if (provider == "ollama") {
        bool platinum = geode::Mod::get()->getSettingValue<bool>("use-platinum");
        // Mirror getOllamaUrl() in main.cpp. /api/chat supports tools where
        // /api/generate doesn't, so the tool-use loop hits the chat endpoint.
        // Platinum uses a single unified API endpoint at :21800/api/* — the
        // coordinator transparently routes /api/tags, /api/generate, and
        // /api/chat through to a worker. (If /api/chat is not yet wired up
        // on the coordinator, tool-use mode will return 404 on Platinum;
        // basic /api/generate generation still works in that case.)
        std::string base = platinum
            ? std::string("http://sn-1.vltgg.net:21800")
            : std::string("http://localhost:11434");
        return base + "/api/chat";
    }
    if (provider == "claude")      return "https://api.anthropic.com/v1/messages";
    if (provider == "gemini") {
        return fmt::format(
            "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent",
            model
        );
    }
    return "";
}

// ── OpenAI-compatible: tools + messages array, role=tool for tool results ─
// Provider is needed so we can branch on Ollama, which has two divergences
// from the rest of the OpenAI-compat family: (1) it streams by default, so
// /api/chat must be told stream:false explicitly to get a single JSON
// object back, and (2) it expects tool_call function arguments as a JSON
// object rather than a JSON-encoded string.
inline matjson::Value buildOpenAICompatRequest(const std::string& provider,
                                               const std::vector<Message>& history,
                                               const std::string& model)
{
    const bool isOllama = (provider == "ollama");

    auto messages = matjson::Value::array();
    for (const auto& m : history) {
        switch (m.role) {
            case MessageRole::System: {
                auto msg = matjson::Value::object();
                msg["role"]    = "system";
                msg["content"] = m.text;
                messages.push(msg);
                break;
            }
            case MessageRole::User: {
                auto msg = matjson::Value::object();
                msg["role"] = "user";
                if (!m.imageB64.empty() && !isOllama) {
                    // Vision: content becomes a parts array. (Ollama's chat
                    // API wants a separate "images" field instead.)
                    auto parts = matjson::Value::array();
                    auto tp = matjson::Value::object();
                    tp["type"] = "text";
                    tp["text"] = m.text;
                    parts.push(tp);
                    auto ip = matjson::Value::object();
                    ip["type"] = "image_url";
                    auto iu = matjson::Value::object();
                    iu["url"] = fmt::format("data:{};base64,{}",
                                            m.imageMime, m.imageB64);
                    ip["image_url"] = iu;
                    parts.push(ip);
                    msg["content"] = parts;
                } else {
                    msg["content"] = m.text;
                    if (!m.imageB64.empty() && isOllama) {
                        auto imgs = matjson::Value::array();
                        imgs.push(m.imageB64);
                        msg["images"] = imgs;
                    }
                }
                messages.push(msg);
                break;
            }
            case MessageRole::Assistant: {
                auto msg = matjson::Value::object();
                msg["role"]    = "assistant";
                if (!m.text.empty()) msg["content"] = m.text;
                if (!m.toolCalls.empty()) {
                    auto tcs = matjson::Value::array();
                    for (const auto& tc : m.toolCalls) {
                        auto call = matjson::Value::object();
                        call["id"]   = tc.id;
                        call["type"] = "function";
                        auto fn = matjson::Value::object();
                        fn["name"] = tc.name;
                        if (isOllama) {
                            // Ollama: arguments stays a JSON object.
                            fn["arguments"] = tc.args;
                        } else {
                            // OpenAI / Mistral / DeepSeek / etc: arguments is a string.
                            fn["arguments"] = tc.args.dump();
                        }
                        call["function"] = fn;
                        tcs.push(call);
                    }
                    msg["tool_calls"] = tcs;
                    if (!msg.contains("content")) msg["content"] = matjson::Value(nullptr);
                }
                messages.push(msg);
                break;
            }
            case MessageRole::ToolResults: {
                // Each tool result is its own role=tool message.
                for (const auto& tr : m.toolResults) {
                    auto msg = matjson::Value::object();
                    msg["role"]            = "tool";
                    msg["tool_call_id"]    = tr.toolCallId;
                    msg["content"]         = tr.content;
                    messages.push(msg);
                }
                break;
            }
        }
    }

    auto body = matjson::Value::object();
    body["model"]    = model;
    body["messages"] = messages;
    body["tools"]    = buildToolCatalog();
    body["tool_choice"] = "auto";
    if (isOllama) {
        // Ollama's NATIVE /api/chat ignores OpenAI-style top-level
        // temperature/max_tokens — its knobs live in "options".
        auto options = matjson::Value::object();
        options["temperature"] = 0.7;
        options["num_predict"] = tokenLimitSpec(provider).limit;
        body["options"] = options;
    } else {
        // Exactly ONE token-limit field — sending both max_tokens and
        // max_completion_tokens makes strict servers (HuggingFace router,
        // OpenAI) reject the request with HTTP 400.
        auto spec = tokenLimitSpec(provider);
        body[spec.field] = spec.limit;
        // OpenAI o-series reasoning models reject a temperature param (400).
        if (!(provider == "openai" && isOSeriesModel(model)))
            body["temperature"] = 0.7;
    }
    // CRITICAL for Ollama: without stream:false, /api/chat returns
    // newline-delimited JSON (one object per chunk), which fails as a single
    // JSON parse. Setting it explicitly is harmless for every other
    // OpenAI-compat provider (they all treat unset and false the same).
    body["stream"] = false;
    return body;
}

// `arguments` arrives either as a JSON-encoded STRING (OpenAI dialect) or as
// a JSON OBJECT (Ollama / some fine-tunes). Normalize to an object; an empty
// object on parse failure matches ToolCall's default.
inline matjson::Value parseToolArgs(const matjson::Value& argsField) {
    if (auto argsStrRes = argsField.asString(); argsStrRes) {
        auto p = editorai::json_lenient::parse(argsStrRes.unwrap());
        if (p.ok) return std::move(p.value);
        return matjson::Value::object();
    }
    if (argsField.isObject()) return argsField;
    return matjson::Value::object();
}

inline ParsedResponse parseOpenAICompatResponse(const matjson::Value& json) {
    ParsedResponse out;

    // Accept two shapes:
    //   OpenAI-style: { "choices": [{ "message": {...} }] }
    //   Ollama-style: { "message": {...} }
    matjson::Value msg;
    if (json.contains("choices") && json["choices"].isArray() && json["choices"].size() > 0) {
        msg = json["choices"][0]["message"];
    } else if (json.contains("message") && json["message"].isObject()) {
        msg = json["message"];
    } else {
        // If the body is an outright error from the provider, surface it.
        if (json.contains("error")) {
            auto errStr = json["error"]["message"].asString();
            if (errStr) {
                out.errorMessage = errStr.unwrap();
                return out;
            }
        }
        out.errorMessage = "Response had neither 'choices[0].message' nor 'message'";
        return out;
    }

    if (!msg.isObject()) {
        out.errorMessage = "'message' field is not an object";
        return out;
    }

    // Capture any plain text the assistant emitted (some models put a brief
    // reasoning trace alongside tool calls).
    auto contentRes = msg["content"].asString();
    std::string text = contentRes ? contentRes.unwrap() : "";

    // Reasoning models expose their chain of thought in a sibling field:
    // DeepSeek uses "reasoning_content", OpenRouter normalizes to
    // "reasoning". Display-only — never fed back into history.
    {
        const auto& cmsg = msg;
        for (const char* key : {"reasoning_content", "reasoning"}) {
            if (cmsg.contains(key)) {
                auto rr = cmsg[key].asString();
                if (rr && !rr.unwrap().empty()) { out.reasoningText = rr.unwrap(); break; }
            }
        }
    }

    // Tool calls? Both OpenAI and Ollama use `tool_calls` at the message level.
    if (msg.contains("tool_calls") && msg["tool_calls"].isArray()
        && msg["tool_calls"].size() > 0) {
        auto& tcs = msg["tool_calls"];
        for (size_t i = 0; i < tcs.size(); ++i) {
            // Const ref: every key access below must hit the non-inserting
            // const operator[] (the non-const one inserts null on miss).
            const auto& tc = tcs[i];
            ToolCall call;
            // Ollama sometimes omits the id. Synthesize a stable one so the
            // tool_call → tool_result pairing still works.
            auto idRes = tc["id"].asString();
            call.id = idRes ? idRes.unwrap() : fmt::format("call_{}", i);
            const auto& fn = tc["function"];
            auto nameRes = fn["name"].asString();
            call.name = nameRes ? nameRes.unwrap() : "";
            call.args = parseToolArgs(fn["arguments"]);
            out.toolCalls.push_back(std::move(call));
        }
        out.assistantTextWithCalls = text;
        out.ok = true;
        return out;
    }

    // ── Fallback A: bare top-level {"name":..., "arguments":...} in content ─
    // Ollama 0.3+ pre-strips the <tool_call> wrapper before failing to parse
    // string-typed arguments, leaving the bare JSON in `content`. The shape
    // is unambiguous: our level-output JSON has top-level keys analysis/objects/
    // level_metadata, so anything with name+arguments at the top is a tool
    // call we should recover.
    {
        std::string trimmed = text;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(0, 1);
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))  trimmed.pop_back();
        if (!trimmed.empty() && trimmed.front() == '{') {
            auto parsed = editorai::json_lenient::parse(trimmed);
            if (parsed.ok) {
                auto& obj = parsed.value;
                if (obj.isObject() && obj.contains("name") && obj.contains("arguments")
                    && !obj.contains("objects") && !obj.contains("analysis")) {
                    ToolCall call;
                    call.id = "call_xb_0";
                    auto nameRes = obj["name"].asString();
                    if (nameRes) call.name = nameRes.unwrap();
                    call.args = parseToolArgs(obj["arguments"]);
                    if (!call.name.empty()) {
                        out.toolCalls.push_back(std::move(call));
                        out.assistantTextWithCalls = "";
                        out.ok = true;
                        return out;
                    }
                }
            }
        }
    }

    // ── Fallback B: <tool_call> XML in content ──────────────────────────────
    // Some Qwen fine-tunes emit
    //     <tool_call>{"name":"X","arguments":"{}"}</tool_call>
    // without Ollama stripping the wrapper. Recover via tag scan.
    if (text.find("<tool_call>") != std::string::npos) {
        size_t pos = 0;
        size_t synthId = 0;
        while ((pos = text.find("<tool_call>", pos)) != std::string::npos) {
            size_t end = text.find("</tool_call>", pos);
            if (end == std::string::npos) break;
            std::string body = text.substr(pos + 11, end - (pos + 11));
            // trim ws
            while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) body.erase(0, 1);
            while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())))  body.pop_back();
            auto parsed = editorai::json_lenient::parse(body);
            if (parsed.ok) {
                auto& obj = parsed.value;
                if (obj.isObject() && obj.contains("name")) {
                    ToolCall call;
                    call.id = fmt::format("call_xt_{}", synthId++);
                    auto nameRes = obj["name"].asString();
                    if (nameRes) call.name = nameRes.unwrap();
                    if (obj.contains("arguments"))
                        call.args = parseToolArgs(obj["arguments"]);
                    if (!call.name.empty()) out.toolCalls.push_back(std::move(call));
                }
            }
            pos = end + 12; // strlen("</tool_call>")
        }
        if (!out.toolCalls.empty()) {
            // Strip the recovered tool_call blocks from the narrative.
            std::string clean = text;
            size_t p = 0;
            while ((p = clean.find("<tool_call>", p)) != std::string::npos) {
                size_t e = clean.find("</tool_call>", p);
                if (e == std::string::npos) break;
                clean.erase(p, (e + 12) - p);
            }
            while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.front()))) clean.erase(0, 1);
            while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.back())))  clean.pop_back();
            out.assistantTextWithCalls = clean;
            out.ok = true;
            return out;
        }
    }

    // No tool calls — text is the final answer.
    out.finalText = text;
    out.ok = !text.empty();
    if (!out.ok) out.errorMessage = "Empty final response";
    return out;
}

// ── Anthropic Claude: tool_use / tool_result content blocks ───────────────
inline matjson::Value buildClaudeRequest(const std::vector<Message>& history,
                                         const std::string& model)
{
    // Claude wants system prompt as a top-level field, NOT a message.
    std::string systemText;
    auto messages = matjson::Value::array();

    for (const auto& m : history) {
        if (m.role == MessageRole::System) {
            if (!systemText.empty()) systemText += "\n\n";
            systemText += m.text;
            continue;
        }
        if (m.role == MessageRole::User) {
            auto msg = matjson::Value::object();
            msg["role"] = "user";
            if (!m.imageB64.empty()) {
                // Vision: image block + text block.
                auto content = matjson::Value::array();
                auto img = matjson::Value::object();
                img["type"] = "image";
                auto srcObj = matjson::Value::object();
                srcObj["type"]       = "base64";
                srcObj["media_type"] = m.imageMime;
                srcObj["data"]       = m.imageB64;
                img["source"] = srcObj;
                content.push(img);
                auto txt = matjson::Value::object();
                txt["type"] = "text";
                txt["text"] = m.text;
                content.push(txt);
                msg["content"] = content;
            } else {
                msg["content"] = m.text;   // simple string content
            }
            messages.push(msg);
            continue;
        }
        if (m.role == MessageRole::Assistant) {
            auto msg = matjson::Value::object();
            msg["role"]    = "assistant";
            // If there are tool calls, content MUST be an array of content blocks.
            if (!m.toolCalls.empty()) {
                auto content = matjson::Value::array();
                if (!m.text.empty()) {
                    auto txt = matjson::Value::object();
                    txt["type"] = "text";
                    txt["text"] = m.text;
                    content.push(txt);
                }
                for (const auto& tc : m.toolCalls) {
                    auto block = matjson::Value::object();
                    block["type"]  = "tool_use";
                    block["id"]    = tc.id;
                    block["name"]  = tc.name;
                    block["input"] = tc.args;
                    content.push(block);
                }
                msg["content"] = content;
            } else {
                msg["content"] = m.text;
            }
            messages.push(msg);
            continue;
        }
        if (m.role == MessageRole::ToolResults) {
            // Tool results in Claude are a USER message with tool_result blocks.
            auto msg = matjson::Value::object();
            msg["role"] = "user";
            auto content = matjson::Value::array();
            for (const auto& tr : m.toolResults) {
                auto block = matjson::Value::object();
                block["type"]         = "tool_result";
                block["tool_use_id"]  = tr.toolCallId;
                block["content"]      = tr.content;
                if (tr.isError) block["is_error"] = true;
                content.push(block);
            }
            msg["content"] = content;
            messages.push(msg);
            continue;
        }
    }

    // Claude expects an array of {name, description, input_schema}
    auto tools = matjson::Value::array();
    auto catalog = buildToolCatalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        const auto& catEntry = catalog[i];
        const auto& fn = catEntry["function"];
        auto t = matjson::Value::object();
        auto nameRes = fn["name"].asString();
        auto descRes = fn["description"].asString();
        t["name"]         = nameRes ? nameRes.unwrap() : "";
        t["description"]  = descRes ? descRes.unwrap() : "";
        t["input_schema"] = fn["parameters"];
        tools.push(t);
    }

    auto body = matjson::Value::object();
    body["model"]       = model;
    body["max_tokens"]  = 8192;
    body["temperature"] = 0.7;
    if (!systemText.empty()) {
        // System as a content-block array with cache_control: Anthropic
        // caches the prefix up to the marker (tools render before system, so
        // this covers both). Tool rounds, extension rounds, and refinement
        // rounds all re-send the identical prefix — those reads bill at ~10%
        // of input price. Below the minimum cacheable size this is silently
        // ignored; never an error.
        auto sysBlock = matjson::Value::object();
        sysBlock["type"] = "text";
        sysBlock["text"] = systemText;
        auto cache = matjson::Value::object();
        cache["type"] = "ephemeral";
        sysBlock["cache_control"] = cache;
        auto sysArr = matjson::Value::array();
        sysArr.push(sysBlock);
        body["system"] = sysArr;
    }
    body["messages"]    = messages;
    body["tools"]       = tools;
    return body;
}

inline ParsedResponse parseClaudeResponse(const matjson::Value& json) {
    ParsedResponse out;
    if (!json.contains("content") || !json["content"].isArray()) {
        out.errorMessage = "Claude response missing content array";
        return out;
    }
    auto& content = json["content"];
    std::string text;
    for (size_t i = 0; i < content.size(); ++i) {
        auto& block = content[i];
        auto typeRes = block["type"].asString();
        if (!typeRes) continue;
        std::string type = typeRes.unwrap();
        if (type == "text") {
            auto t = block["text"].asString();
            if (t) text += t.unwrap();
        } else if (type == "thinking") {
            // Present when the model thinks (e.g. adaptive thinking on).
            // Display-only; never replayed into history.
            auto t = block["thinking"].asString();
            if (t) {
                if (!out.reasoningText.empty()) out.reasoningText += "\n";
                out.reasoningText += t.unwrap();
            }
        } else if (type == "tool_use") {
            ToolCall call;
            auto idRes   = block["id"].asString();
            auto nameRes = block["name"].asString();
            call.id   = idRes   ? idRes.unwrap()   : fmt::format("call_{}", i);
            call.name = nameRes ? nameRes.unwrap() : "";
            // Claude provides input as a JSON object directly (not a string)
            call.args = block["input"];
            out.toolCalls.push_back(std::move(call));
        }
    }

    if (!out.toolCalls.empty()) {
        out.assistantTextWithCalls = text;
        out.ok = true;
    } else {
        out.finalText = text;
        out.ok = !text.empty();
        if (!out.ok) out.errorMessage = "Empty Claude response";
    }
    return out;
}

// ── Gemini: contents array with parts, functionDeclarations in tools ──────
inline matjson::Value buildGeminiRequest(const std::vector<Message>& history,
                                         const std::string& model)
{
    (void)model;  // model is in the URL, not the body, for Gemini

    std::string systemText;
    auto contents = matjson::Value::array();

    for (const auto& m : history) {
        if (m.role == MessageRole::System) {
            if (!systemText.empty()) systemText += "\n\n";
            systemText += m.text;
            continue;
        }
        if (m.role == MessageRole::User) {
            auto msg = matjson::Value::object();
            msg["role"] = "user";
            auto parts = matjson::Value::array();
            auto p = matjson::Value::object();
            p["text"] = m.text;
            parts.push(p);
            if (!m.imageB64.empty()) {
                auto ip = matjson::Value::object();
                auto inline_ = matjson::Value::object();
                inline_["mimeType"] = m.imageMime;
                inline_["data"]     = m.imageB64;
                ip["inlineData"] = inline_;
                parts.push(ip);
            }
            msg["parts"] = parts;
            contents.push(msg);
            continue;
        }
        if (m.role == MessageRole::Assistant) {
            auto msg = matjson::Value::object();
            msg["role"] = "model";  // Gemini uses "model" not "assistant"
            auto parts = matjson::Value::array();
            if (!m.text.empty()) {
                auto p = matjson::Value::object();
                p["text"] = m.text;
                parts.push(p);
            }
            for (const auto& tc : m.toolCalls) {
                auto p = matjson::Value::object();
                auto fc = matjson::Value::object();
                fc["name"] = tc.name;
                fc["args"] = tc.args;
                p["functionCall"] = fc;
                // Echo the Gemini 3 thought signature byte-for-byte on the
                // same part it arrived on (see ToolCall::thoughtSignature).
                if (!tc.thoughtSignature.empty())
                    p["thoughtSignature"] = tc.thoughtSignature;
                parts.push(p);
            }
            msg["parts"] = parts;
            contents.push(msg);
            continue;
        }
        if (m.role == MessageRole::ToolResults) {
            // Gemini: function responses come back as a "user" turn with
            // functionResponse parts (one per tool result).
            auto msg = matjson::Value::object();
            msg["role"] = "user";
            auto parts = matjson::Value::array();
            for (const auto& tr : m.toolResults) {
                auto p = matjson::Value::object();
                auto fr = matjson::Value::object();
                // Tool name isn't carried in our ToolResult, but Gemini wants it.
                // We stash it in toolCallId after a ':' sentinel — see executor.
                std::string callId = tr.toolCallId;
                std::string toolName;
                auto sep = callId.find(':');
                if (sep != std::string::npos) {
                    toolName = callId.substr(0, sep);
                    // callId remains as-is — Gemini ignores the id field
                }
                fr["name"] = toolName;
                auto respObj = matjson::Value::object();
                respObj["result"] = tr.content;
                fr["response"] = respObj;
                p["functionResponse"] = fr;
                parts.push(p);
            }
            msg["parts"] = parts;
            contents.push(msg);
            continue;
        }
    }

    // Function declarations. Gemini's v1beta API rejects OBJECT-typed
    // parameter schemas whose "properties" map is empty (400 INVALID_ARGUMENT
    // "...parameters.properties: should be non-empty for OBJECT type"), so
    // no-arg tools (analyze_level, get_level_length) must omit "parameters"
    // entirely — that is Google's documented way to declare no-arg functions.
    // OpenAI and Claude both accept empty properties; only this builder
    // needs the sanitization.
    auto fns = matjson::Value::array();
    auto catalog = buildToolCatalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        const auto& catEntry = catalog[i];
        const auto& fn = catEntry["function"];
        auto decl = matjson::Value::object();
        decl["name"]        = fn["name"];
        decl["description"] = fn["description"];
        const auto& params  = fn["parameters"];
        if (params.isObject() && params.contains("properties")
            && params["properties"].isObject() && params["properties"].size() > 0)
        {
            decl["parameters"] = params;
        }
        fns.push(std::move(decl));
    }
    auto toolBlock = matjson::Value::object();
    toolBlock["functionDeclarations"] = fns;
    auto tools = matjson::Value::array();
    tools.push(toolBlock);

    auto body = matjson::Value::object();
    if (!systemText.empty()) {
        auto sysObj = matjson::Value::object();
        auto sysParts = matjson::Value::array();
        auto sp = matjson::Value::object();
        sp["text"] = systemText;
        sysParts.push(sp);
        sysObj["parts"] = sysParts;
        body["systemInstruction"] = sysObj;
    }
    body["contents"] = contents;
    body["tools"]    = tools;

    auto genConfig = matjson::Value::object();
    genConfig["temperature"]     = 0.7;
    genConfig["maxOutputTokens"] = 32768;
    // Disable the thinking budget for latency — but ONLY on Flash models.
    // Pro models cannot disable thinking and reject thinkingBudget: 0 with
    // HTTP 400 INVALID_ARGUMENT.
    if (model.find("flash") != std::string::npos) {
        auto thinkConfig = matjson::Value::object();
        thinkConfig["thinkingBudget"] = 0;
        genConfig["thinkingConfig"] = thinkConfig;
    }
    body["generationConfig"] = genConfig;
    return body;
}

inline ParsedResponse parseGeminiResponse(const matjson::Value& json) {
    ParsedResponse out;
    if (!json.contains("candidates") || !json["candidates"].isArray()
        || json["candidates"].size() == 0)
    {
        out.errorMessage = "Gemini response: no candidates";
        return out;
    }
    auto cand = json["candidates"][0];
    auto content = cand["content"];
    if (!content.isObject() || !content.contains("parts") || !content["parts"].isArray()) {
        // Empty content with finishReason = ok happens sometimes; just treat as empty
        auto finishRes = cand["finishReason"].asString();
        if (finishRes && finishRes.unwrap() == "SAFETY") {
            out.errorMessage = "Gemini safety filter blocked the response";
        } else {
            out.errorMessage = "Gemini response missing content.parts";
        }
        return out;
    }
    auto& parts = content["parts"];
    std::string text;
    int synthId = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        auto& p = parts[i];
        if (p.contains("text")) {
            auto t = p["text"].asString();
            if (t) text += t.unwrap();
        }
        if (p.contains("functionCall")) {
            auto fc = p["functionCall"];
            ToolCall call;
            auto nameRes = fc["name"].asString();
            call.name = nameRes ? nameRes.unwrap() : "";
            // Synthesize an id like "<name>:<n>" so the matching tool_result
            // can be re-associated when we feed it back to Gemini (see builder).
            call.id   = fmt::format("{}:{}", call.name, synthId++);
            if (fc.contains("args")) call.args = fc["args"];
            // Gemini 3 signs functionCall parts; the signature must come
            // back with the replayed call or the next round 400s. It sits
            // on the PART, not inside functionCall.
            if (p.contains("thoughtSignature")) {
                if (auto sig = p["thoughtSignature"].asString())
                    call.thoughtSignature = sig.unwrap();
            }
            out.toolCalls.push_back(std::move(call));
        }
    }
    if (!out.toolCalls.empty()) {
        out.assistantTextWithCalls = text;
        out.ok = true;
    } else {
        out.finalText = text;
        out.ok = !text.empty();
        if (!out.ok) out.errorMessage = "Empty Gemini response";
    }
    return out;
}

// ── Dispatch ───────────────────────────────────────────────────────────────
inline matjson::Value buildRequest(const std::string& provider,
                                   const std::vector<Message>& history,
                                   const std::string& model)
{
    if (provider == "claude")  return buildClaudeRequest(history, model);
    if (provider == "gemini")  return buildGeminiRequest(history, model);
    if (isOpenAICompat(provider))
        return buildOpenAICompatRequest(provider, history, model);
    return matjson::Value::object();  // empty -- caller should check supportsToolUse
}

inline ParsedResponse parseResponse(const std::string& provider,
                                    const matjson::Value& json)
{
    if (provider == "claude") return parseClaudeResponse(json);
    if (provider == "gemini") return parseGeminiResponse(json);
    if (isOpenAICompat(provider)) return parseOpenAICompatResponse(json);
    ParsedResponse out;
    out.errorMessage = "Unsupported provider for tool use";
    return out;
}

} // namespace toolUse

// ── END inlined headers ──────────────────────────────────────

#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/CCTextInputNode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/ui/TextInput.hpp>
// NodeIDs utilities: NodeIDs::provideFor, switchToMenu, etc.
#include <Geode/utils/NodeIDs.hpp>

using namespace geode::prelude;

// ─── Shared UI theme ─────────────────────────────────────────────────────────
//
// "GD-native polish": every EditorAI surface uses GD's own visual language —
// brown GJ_square01 popup chrome (Geode's default Popup bg, untinted),
// goldFont titles, GJ_checkOn/Off togglers, black square02b wells, striped
// comment-cell rows, and embossed groove dividers. Zero custom art assets.
//
// Spacing rhythm: outer popup padding 16 · well padding 10 · card gap 8 ·
// settings row height 26 · footer zone 34.
// Font floors (Android BMFont mip smear): chatFont >= 0.45, bigFont >= 0.22.
// Every dynamic label gets limitLabelWidth().
namespace ui {

// Palette. (Names suffixed _COL to dodge platform macros like ERROR.)
constexpr ccColor3B TEXT_PRIMARY   {255, 255, 255};  // bigFont labels
constexpr ccColor3B TEXT_SECONDARY {255, 235, 205};  // chatFont hints / neutral status
constexpr ccColor3B TEXT_DIM       {224, 174, 126};  // range hints, "(optional)"
constexpr ccColor3B TEXT_INACTIVE  {140, 140, 140};  // inactive tabs, unselected ratings
constexpr ccColor3B SUCCESS_COL    { 64, 227,  72};
constexpr ccColor3B ERROR_COL      {255,  90,  90};
constexpr ccColor3B WARN_COL       {255, 165,  75};
constexpr ccColor3B BUSY_COL       { 96, 171, 239};  // transient "Testing..." status
constexpr ccColor3B CELL_COL       {161,  88,  44};  // settings row cell (dark brown)
constexpr ccColor3B CELL_ALT_COL   {194, 114,  62};  // alternating row cell
constexpr ccColor3B TAB_OFF_COL    {120,  66,  33};  // inactive-tab fallback plate

// Recessed black inset (square02b tints from near-white, so tints land exact).
inline CCScale9Sprite* makeWell(float w, float h, GLubyte opacity = 90) {
    auto s = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    s->setContentSize({w, h});
    s->setColor({0, 0, 0});
    s->setOpacity(opacity);
    return s;
}

// Brown settings-row cell, striped via `alt`.
inline CCScale9Sprite* makeCell(float w, float h, bool alt) {
    auto s = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    s->setContentSize({w, h});
    s->setColor(alt ? CELL_ALT_COL : CELL_COL);
    return s;
}

// Embossed divider: 1px dark line with a 1px light line right below.
// Heights set via setContentSize, never setScale (sub-pixel smear).
inline void addGroove(CCNode* parent, float w, float cx, float cy, int z = 0) {
    auto dark = CCLayerColor::create({0, 0, 0, 80});
    dark->setContentSize({w, 1.f});
    dark->ignoreAnchorPointForPosition(false);
    dark->setAnchorPoint({0.5f, 0.5f});
    dark->setPosition({cx, cy});
    parent->addChild(dark, z);
    auto light = CCLayerColor::create({255, 255, 255, 28});
    light->setContentSize({w, 1.f});
    light->ignoreAnchorPointForPosition(false);
    light->setAnchorPoint({0.5f, 0.5f});
    light->setPosition({cx, cy - 1.f});
    parent->addChild(light, z);
}

// Small tintable status dot (auth state, provider chip).
inline CCSprite* makeStatusDot(ccColor3B col, float scale = 0.6f) {
    auto d = CCSprite::create("smallDot.png");
    if (!d) d = CCSprite::create();  // degrade invisibly if the sprite moves
    d->setScale(scale);
    d->setColor(col);
    return d;
}

// Re-apply title styling. MUST be called after every setTitle() — Geode
// rebuilds the title label there.
inline void styleTitle(CCLabelBMFont* title, float scale) {
    if (title) title->setScale(scale);
}

} // namespace ui

// ─── Logging helper — prevents Geode's spdlog from silently truncating long
//     strings. Splits anything over 1800 characters into numbered chunks.
// ─────────────────────────────────────────────────────────────────────────────
static void logLong(const std::string& label, const std::string& content) {
    // Full payloads (system prompts, level JSON, AI responses — easily 100KB+
    // per generation) go to DEBUG so default INFO logs stay readable and the
    // main thread isn't flushing hundreds of KB to disk every generation.
    log::info("{}: {} chars (full text at DEBUG log level)", label, content.size());
    constexpr size_t CHUNK = 1800;
    if (content.size() <= CHUNK) {
        log::debug("{}: {}", label, content);
        return;
    }
    size_t total = (content.size() + CHUNK - 1) / CHUNK;
    for (size_t i = 0; i < content.size(); i += CHUNK) {
        size_t part = i / CHUNK + 1;
        log::debug("[{} {}/{}] {}", label, part, total, content.substr(i, CHUNK));
    }
}

// ─── Object ID registry ──────────────────────────────────────────────────────

static std::unordered_map<std::string, int> parseObjectIDs(const std::string& jsonContent, const std::string& source) {
    std::unordered_map<std::string, int> ids;
    auto json = matjson::parse(jsonContent);
    if (!json) {
        log::error("Failed to parse {} object_ids.json: {}", source, json.unwrapErr());
        return ids;
    }
    auto obj = json.unwrap();
    if (!obj.isObject()) {
        log::error("{} object_ids.json root is not an object", source);
        return ids;
    }
    for (auto& [key, value] : obj) {
        auto intVal = value.asInt();
        if (intVal) ids[key] = intVal.unwrap();
    }
    log::info("Loaded {} object IDs from {}", ids.size(), source);
    return ids;
}

// Short aliases. The full catalog uses verbose names like
// `effect_pulse_trigger`, but the AI (and most modders) reach for short forms:
// `pulse_trigger`. Inject the short forms at load time so BOTH resolve to the
// same numeric ID without bloating object_ids.json.
//
// Add new aliases here as a {short_alias, canonical_name} pair. The
// canonical_name must exist in object_ids.json or the alias is silently
// dropped. Aliases never overwrite an existing entry in the catalog.
static void injectShortAliases(std::unordered_map<std::string, int>& ids) {
    static const std::vector<std::pair<const char*, const char*>> ALIASES = {
        // ── Effect triggers (short ↔ canonical) ────────────────────────────
        {"color_trigger",       "effect_color_trigger"},
        {"alpha_trigger",       "effect_alpha_trigger"},
        {"move_trigger",        "effect_move_trigger"},
        {"pulse_trigger",       "effect_pulse_trigger"},
        {"rotate_trigger",      "effect_rotate_trigger"},
        {"scale_trigger",       "effect_scale_trigger"},
        {"shake_trigger",       "effect_shake_trigger"},
        {"spawn_trigger",       "effect_spawn_trigger"},
        {"stop_trigger",        "effect_stop_trigger"},
        {"toggle_trigger",      "effect_toggle_trigger"},
        {"end_trigger",         "effect_10_level_end_trigger"},
        // ── Speed portals (short ↔ canonical by color) ─────────────────────
        {"speed_portal_half",       "portal_yellow_slow_speed_portal"},   // 0.5×
        {"speed_portal_normal",     "portal_blue_normal_speed_portal"},   // 1×
        {"speed_portal_double",     "portal_green_fast_speed_portal"},    // 2×
        {"speed_portal_triple",     "portal_red_fast_speed_portal"},      // 3×
        {"speed_portal_quadruple",  "portal_pink_fast_speed_portal"},     // 4×
        // ── Gamemode portals (short ↔ canonical) ───────────────────────────
        {"portal_cube",         "portal_cube_portal"},
        {"portal_ship",         "portal_ship_portal"},
        {"portal_ball",         "portal_ball_portal"},
        {"portal_ufo",          "portal_ufo_portal"},
        {"portal_wave",         "portal_wave_portal"},
        {"portal_robot",        "portal_robot_portal"},
        {"portal_spider",       "portal_spider_portal"},
        {"portal_swing",        "portal_swing_portal"},
        {"portal_gravity_up",   "portal_flipped_gravity_portal"},
        {"portal_gravity_down", "portal_normal_gravity_portal"},
        {"portal_gravity_reverse", "portal_reverse_gravity_portal"},
        {"portal_size_mini",    "portal_green_size_portal"},
        {"portal_size_normal",  "portal_pink_size_portal"},
        {"portal_mirror_on",    "portal_blue_mirror_portal"},
        {"portal_mirror_off",   "portal_orange_mirror_portal"},
        // ── Pads / orbs (compact-friendly aliases) ─────────────────────────
        {"pad_yellow",  "jump_pad_yellow_jump_pad"},
        {"pad_pink",    "jump_pad_pink_jump_pad"},
        {"pad_red",     "jump_pad_red_jump_pad"},
        {"orb_yellow",  "jump_orb_yellow_jump_orb"},
        {"orb_pink",    "jump_orb_pink_jump_orb"},
        {"orb_red",     "jump_orb_red_jump_orb"},
        {"orb_blue",    "obj_blue_gravity_orb"},
        {"orb_green",   "obj_green_dash_orb"},
        {"orb_black",   "obj_black_drop_orb"},
    };
    int added = 0;
    for (auto [alias, canonical] : ALIASES) {
        if (ids.count(alias)) continue;                  // never clobber
        auto it = ids.find(canonical);
        if (it == ids.end()) continue;                   // canonical missing
        ids[alias] = it->second;
        ++added;
    }
    // Also inject pure-numeric-ID aliases for trigger names the prompt
    // promises but no canonical exists for under that short name.
    static const std::vector<std::pair<const char*, int>> NUMERIC_ALIASES = {
        {"show_player_trigger", 1613},
        {"hide_player_trigger", 1612},
        {"show_trail_trigger",  32},
        {"hide_trail_trigger",  33},
    };
    for (auto [alias, id] : NUMERIC_ALIASES) {
        if (!ids.count(alias)) { ids[alias] = id; ++added; }
    }
    log::info("Injected {} short aliases into OBJECT_IDS", added);
}

static std::unordered_map<std::string, int> loadObjectIDs() {
    auto path = Mod::get()->getResourcesDir() / "object_ids.json";
    std::unordered_map<std::string, int> ids;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        auto content = geode::utils::file::readString(path);
        if (content) ids = parseObjectIDs(content.unwrap(), "local file");
        else log::error("Error reading local object_ids.json: {}", content.unwrapErr());
    } else {
        log::warn("Local object_ids.json not found");
    }

    if (ids.empty()) {
        log::warn("Using default object IDs (5 objects only)");
        ids = {
            {"block_black_gradient_square", 1},
            {"spike_black_gradient_spike", 8},
            {"jump_pad_yellow_jump_pad",   35},
            {"jump_orb_yellow_jump_orb",   36},
            {"portal_cube_portal",         13},
        };
    }
    injectShortAliases(ids);

    // Token diet: 71% of catalog names carry a meaningless "obj_" prefix
    // (4 chars × ~2800 names ≈ 11KB of prompt). Register the stripped form
    // as an alias wherever it doesn't collide with an existing name, so the
    // AI can use (and be shown) the short form while long names keep
    // resolving. Two-pass so insertion order can't make collisions racy.
    {
        std::vector<std::pair<std::string, int>> stripped;
        for (auto& [name, id] : ids) {
            if (name.rfind("obj_", 0) == 0 && name.size() > 4)
                stripped.emplace_back(name.substr(4), id);
        }
        int added = 0;
        for (auto& [shortName, id] : stripped) {
            if (ids.emplace(shortName, id).second) ++added;
        }
        log::info("Registered {} obj_-stripped short aliases", added);
    }
    return ids;
}

static std::unordered_map<std::string, int> OBJECT_IDS = loadObjectIDs();

// Shared id→name reverse map. Picks the SHORTEST name per ID (ties broken
// lexicographically for determinism) — short aliases beat verbose canonical
// names, so everything shown to the AI (level JSON, region queries, level
// summaries, prompt catalog) costs fewer tokens. OBJECT_IDS is immutable
// after startup, so this builds exactly once.
static const std::unordered_map<int, std::string>& objectIdToName() {
    static const std::unordered_map<int, std::string> map = [] {
        std::unordered_map<int, std::string> m;
        m.reserve(OBJECT_IDS.size());
        for (auto& [name, id] : OBJECT_IDS) {
            auto it = m.find(id);
            if (it == m.end()
                || name.size() < it->second.size()
                || (name.size() == it->second.size() && name < it->second))
            {
                m[id] = name;
            }
        }
        return m;
    }();
    return map;
}

// ─── Expert example sections (bundled .gmd extracts) ────────────────────────
// Built offline from a handful of well-known levels (Back on Track v2, Stereo
// Madness v2, Cataclysm, Aquarius, Acid Factory). Each entry is one ~500-X-wide
// slice of objects with X coordinates normalized to start at 0.
//
// At prompt-build time we pick 1-2 of these matching the user's requested
// difficulty/style and paste them into the system prompt as concrete few-shot
// examples. Anchors the AI's sense of "what a real level looks like" instead
// of relying purely on its training-data priors.

struct ExampleSection {
    std::string levelName;
    std::string author;
    std::string theme;        // e.g. "classic-retro", "dark-decorated"
    std::string difficulty;   // easy / medium / hard / extreme
    std::string description;  // hand-written style notes for this level (v2+)
    std::vector<std::string> tags;
    // Up to ~30 objects packed as JSON-array string ready to inject.
    // Pre-stringified at load time so the prompt builder doesn't re-encode it
    // on every generation. Source JSON has up to 75 per section; we trim
    // when stringifying so each example is ~1.5KB max.
    std::string objectsJson;
    int origXStart = 0;
    int origXEnd   = 0;
    int objectCount = 0;
};

static std::vector<ExampleSection> loadExampleSections() {
    std::vector<ExampleSection> out;
    // Data is embedded above as the EXAMPLE_SECTIONS_JSON raw string literal,
    // so there's no disk read and the examples ship with the binary itself.
    // No fallback path, no GitHub fetch, no missing-resource warning — the
    // JSON is part of the compiled mod.
    {
        std::string_view content = editorai::EXAMPLE_SECTIONS_JSON;
        auto parsed = matjson::parse(std::string(content));
        if (!parsed) {
            log::error("Failed to parse embedded EXAMPLE_SECTIONS_JSON: {}", parsed.unwrapErr());
            return out;
        }
        auto root = parsed.unwrap();
        if (!root.contains("examples") || !root["examples"].isArray()) {
            log::error("example_sections.json missing 'examples' array");
            return out;
        }
        auto& arr = root["examples"];
        for (size_t i = 0; i < arr.size(); ++i) {
            auto& ex = arr[i];
            if (!ex.isObject()) continue;
            ExampleSection s;
            if (auto r = ex["level_name"].asString())  s.levelName  = r.unwrap();
            if (auto r = ex["author"].asString())      s.author     = r.unwrap();
            if (auto r = ex["theme"].asString())       s.theme      = r.unwrap();
            if (auto r = ex["difficulty"].asString())  s.difficulty = r.unwrap();
            if (auto r = ex["description"].asString()) s.description= r.unwrap();
            if (auto r = ex["object_count"].asInt())   s.objectCount = (int)r.unwrap();
            if (ex.contains("tags") && ex["tags"].isArray()) {
                auto& tags = ex["tags"];
                for (size_t t = 0; t < tags.size(); ++t) {
                    auto tr = tags[t].asString();
                    if (tr) s.tags.push_back(tr.unwrap());
                }
            }
            if (ex.contains("orig_x_range") && ex["orig_x_range"].isArray()
                && ex["orig_x_range"].size() == 2) {
                auto x0 = ex["orig_x_range"][0].asInt();
                auto x1 = ex["orig_x_range"][1].asInt();
                if (x0) s.origXStart = (int)x0.unwrap();
                if (x1) s.origXEnd   = (int)x1.unwrap();
            }
            // Cap to ~30 objects when stringifying so a single example doesn't
            // eat too much prompt budget. Full 75 stays in source for future use.
            // Bumped from 20 → 30 (v2) so the AI sees a denser slice per level.
            if (ex.contains("objects") && ex["objects"].isArray()) {
                auto& src = ex["objects"];
                constexpr size_t MAX_PER_EXAMPLE = 30;
                auto trimmed = matjson::Value::array();
                size_t take = std::min(src.size(), MAX_PER_EXAMPLE);
                for (size_t k = 0; k < take; ++k) {
                    trimmed.push(src[k]);
                }
                s.objectsJson = trimmed.dump();
            }
            if (!s.objectsJson.empty()) out.push_back(std::move(s));
        }
        log::info("Loaded {} expert example sections (embedded in binary)", out.size());
    }
    return out;
}

static std::vector<ExampleSection> EXAMPLE_SECTIONS = loadExampleSections();

// Score an example against the requested difficulty/style/tags. Higher = better
// fit. Used for nearest-neighbor sampling so the AI sees inspirations that
// resemble what the user is asking for.
static int scoreExample(const ExampleSection& ex,
                        const std::string& difficulty,
                        const std::string& style)
{
    int score = 0;
    if (!difficulty.empty() && ex.difficulty == difficulty) score += 3;
    for (const auto& tag : ex.tags) {
        if (tag == difficulty) score += 1;
        if (tag == style)      score += 2;
    }
    if (ex.theme.find(style) != std::string::npos) score += 1;
    return score;
}

// Pick up to `count` example indices, prioritizing tag matches but shuffling
// equal scores so the AI sees different examples across generations.
static std::vector<size_t> pickExampleIndices(const std::string& difficulty,
                                              const std::string& style,
                                              int count)
{
    std::vector<size_t> picked;
    if (EXAMPLE_SECTIONS.empty() || count <= 0) return picked;

    std::vector<std::pair<int, size_t>> scored;
    scored.reserve(EXAMPLE_SECTIONS.size());
    for (size_t i = 0; i < EXAMPLE_SECTIONS.size(); ++i)
        scored.emplace_back(scoreExample(EXAMPLE_SECTIONS[i], difficulty, style), i);

    // Shuffle then stable-sort by score desc — randomizes ties on each call.
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::shuffle(scored.begin(), scored.end(), gen);
    std::stable_sort(scored.begin(), scored.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    for (int i = 0; i < count && i < (int)scored.size(); ++i)
        picked.push_back(scored[i].second);
    return picked;
}

// object_ids.json is shipped INSIDE the .geode bundle (see mod.json `resources`),
// so there's no startup GitHub fetch and no need to fall back to a remote
// source. The bundled file is the source of truth; users who want updates
// install a newer mod release. If somehow the bundle is missing, loadObjectIDs
// degrades to a minimal hardcoded fallback (5 entries) and logs a warning.

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Returns true if the model name is an o-series reasoning model.
// These models reject the "temperature" parameter and return HTTP 400 if sent.
static bool isOSeriesModel(const std::string& model) {
    if (model.size() < 2) return false;
    return (model[0] == 'o' && model[1] >= '1' && model[1] <= '9');
}

// Parse a 6-digit hex color string "#RRGGBB" or "RRGGBB" into r, g, b.
// Returns false if the string is invalid.
static bool parseHexColor(const std::string& hex, GLubyte& r, GLubyte& g, GLubyte& b) {
    std::string_view h = hex;
    if (!h.empty() && h[0] == '#') h.remove_prefix(1);
    if (h.length() < 6) return false;
    auto rr = utils::numFromString<GLubyte>(h.substr(0, 2), 16);
    auto gg = utils::numFromString<GLubyte>(h.substr(2, 2), 16);
    auto bb = utils::numFromString<GLubyte>(h.substr(4, 2), 16);
    if (!rr || !gg || !bb) return false;
    r = rr.unwrap(); g = gg.unwrap(); b = bb.unwrap();
    return true;
}

// ─── Codec / network helpers (used by the AI's optional tools) ──────────────
// Decode URL-safe base64 (used by GD's level-string encoding). Returns empty
// vector on failure. The input may use - and _ in place of + and /; pad chars
// can be missing.
static std::vector<unsigned char> urlSafeBase64Decode(std::string input) {
    if (input.empty()) return {};
    // Normalize any standard-alphabet chars to URL-safe so either form decodes
    // (callers pass URL-safe, but be lenient). Geode's Url variant reads the
    // - / _ alphabet and ignores padding, so no manual '=' padding is needed.
    for (auto& c : input) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    // Geode's base64 is implemented in the loader and links on every platform
    // (unlike cocos2d::base64Decode, which is missing on macOS/iOS).
    auto res = geode::utils::base64::decode(
        input, geode::utils::base64::Base64Variant::Url);
    if (!res) return {};                      // never unwrap an unchecked Result
    return std::move(res).unwrap();           // vector<uint8_t> == vector<unsigned char>
}

// Inflate a gzip-or-zlib buffer using cocos2d-x's pre-linked zlib wrapper.
// (Linking against zlib directly would require teaching CMake about the GD-
// bundled zlib library — calling ZipUtils::ccInflateMemory sidesteps that
// since cocos2d-x already has zlib linked into the GD executable.)
static std::string zlibInflateBytes(const unsigned char* data, size_t len) {
    if (!data || len == 0) return "";
    unsigned char* out = nullptr;
    int outLen = cocos2d::ZipUtils::ccInflateMemory(
        const_cast<unsigned char*>(data), (unsigned int)len, &out);
    if (outLen <= 0 || !out) {
        if (out) std::free(out);
        return "";
    }
    std::string s(reinterpret_cast<const char*>(out), (size_t)outLen);
    std::free(out);
    return s;
}

// HTML-decode the bare entities GD's "description" field uses. Boomlings
// returns level descriptions base64-encoded URL-safe; this is the pre-step.
static std::string base64UrlDecodeText(const std::string& enc) {
    auto bytes = urlSafeBase64Decode(enc);
    return std::string(bytes.begin(), bytes.end());
}

// Form-URL-encode a query value so we can build POST bodies cleanly.
static std::string urlFormEncode(std::string_view s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        bool unreserved = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
                       || (c >= 'a' && c <= 'z') || c == '-' || c == '_'
                       || c == '.' || c == '~';
        if (unreserved) out += (char)c;
        else if (c == ' ') out += '+';
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
    }
    return out;
}

// Split a colon-separated key:value:key:value GD response into pairs.
static std::unordered_map<std::string, std::string> parseColonKV(std::string_view body) {
    std::unordered_map<std::string, std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        size_t colon = body.find(':', i);
        if (colon == std::string_view::npos) break;
        std::string key(body.substr(i, colon - i));
        size_t next = body.find(':', colon + 1);
        std::string val = (next == std::string_view::npos)
            ? std::string(body.substr(colon + 1))
            : std::string(body.substr(colon + 1, next - colon - 1));
        out[key] = val;
        if (next == std::string_view::npos) break;
        i = next + 1;
    }
    return out;
}

// Strip simple HTML tags and decode the most common entities. Good enough for
// scraping titles/snippets out of NG / DuckDuckGo HTML; not a full HTML parser.
static std::string stripHtmlBasic(std::string s) {
    // Remove tags
    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) out += c;
    }
    // Decode common entities
    struct Ent { const char* from; const char* to; };
    static const Ent ents[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"},
        {"&nbsp;", " "}, {"&apos;", "'"},
    };
    for (auto& e : ents) {
        size_t p = 0;
        while ((p = out.find(e.from, p)) != std::string::npos) {
            out.replace(p, std::strlen(e.from), e.to);
            p += std::strlen(e.to);
        }
    }
    // Collapse whitespace
    std::string compact;
    compact.reserve(out.size());
    bool lastWs = false;
    for (char c : out) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ') {
            if (!lastWs && !compact.empty()) compact += ' ';
            lastWs = true;
        } else {
            compact += c;
            lastWs = false;
        }
    }
    while (!compact.empty() && compact.back() == ' ') compact.pop_back();
    return compact;
}

// ─── Error code system ────────────────────────────────────────────────────────
// Format: EAI-CCSS where CC = category, SS = specific error
//
// Categories:
//   10 = Connection errors (can't reach server)
//   20 = Authentication errors (bad API key)
//   30 = Permission/quota errors (valid key, no access)
//   40 = Request errors (bad model, bad request body)
//   50 = Server errors (provider is down)
//   60 = Response parsing errors (AI returned bad data)
//   70 = Generation errors (valid data but unusable)
//   80 = Ollama-specific errors
//
// Provider prefixes:
//   G = Gemini, C = Claude, O = OpenAI, M = Mistral, H = HuggingFace,
//   D = DeepSeek, R = OpenRouter, S = LM Studio, Y = llama.cpp, L = Ollama
//
// The error code encodes: provider initial + category + specific error

static std::string makeErrorCode(const std::string& provider, int category, int specific) {
    // Provider prefix: G=gemini, C=claude, O=openai, M=mistral, H=huggingface,
    //                  D=deepseek, L=ollama, S=lm-studio, Y=llama-cpp, R=openrouter, X=unknown
    char p = 'X';
    if (provider == "gemini")      p = 'G';
    else if (provider == "claude")      p = 'C';
    else if (provider == "openai")      p = 'O';
    else if (provider == "ministral")   p = 'M';
    else if (provider == "huggingface") p = 'H';
    else if (provider == "deepseek")    p = 'D';
    else if (provider == "ollama")      p = 'L';
    else if (provider == "lm-studio")   p = 'S';
    else if (provider == "llama-cpp")   p = 'Y';
    else if (provider == "openrouter")  p = 'R';
    else if (provider == "custom")      p = 'U';   // U for User-defined
    return fmt::format("EAI-{}{:02d}{:02d}", p, category, specific);
}

// Convenience: generate error code from the current provider setting
static std::string autoErrorCode(int category, int specific) {
    std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
    return makeErrorCode(provider, category, specific);
}

// Format helper for the new error convention:
//   "<plain-English what + why>. <How to fix it>. (EAI-XYZZ)"
// Append a short upstream-detail clause if present.
static std::string fmtUserError(const std::string& description,
                                const std::string& fix,
                                const std::string& code,
                                const std::string& detail = "") {
    std::string out = description;
    if (!fix.empty()) out += " " + fix;
    if (!detail.empty()) {
        std::string trimmed = detail.substr(0, 140);
        out += " (upstream said: " + trimmed + ")";
    }
    out += " (" + code + ")";
    return out;
}

static std::pair<std::string, std::string> parseAPIError(const std::string& errorBody, int statusCode) {
    std::string title = "API Error";
    std::string code, msg;
    {
        auto json = matjson::parse(errorBody);
        std::string errorMsg, errorStatus;
        if (json) {
            auto error = json.unwrap();
            // Standard format: {"error": {"message": "...", "status": "..."}}
            if (error.contains("error")) {
                const auto errorObj = error["error"];  // const: reads must not insert
                if (errorObj.isObject()) {
                    if (auto m = errorObj["message"].asString(); m) errorMsg = m.unwrap();
                    if (auto s = errorObj["status"].asString();  s) errorStatus = s.unwrap();
                } else if (auto d = errorObj.asString(); d) {
                    // HuggingFace shape: {"error": "message"}
                    errorMsg = d.unwrap();
                }
            }
        }

        auto isKeyError = [&]() {
            return errorMsg.find("API key") != std::string::npos
                || errorMsg.find("api key") != std::string::npos
                || errorMsg.find("API_KEY") != std::string::npos
                || errorStatus == "API_KEY_INVALID"
                || errorMsg.find("invalid key") != std::string::npos
                || errorMsg.find("authentication") != std::string::npos
                || errorMsg.find("Unauthorized") != std::string::npos;
        };

        if (statusCode == 401) {
            title = "Invalid API Key";
            code  = autoErrorCode(20, 1);
            msg = fmtUserError(
                "Your API key was rejected by the provider (HTTP 401).",
                "Open settings (gear icon), paste a fresh key from the provider's dashboard, and save.",
                code, errorMsg);

        } else if (statusCode == 403) {
            if (isKeyError()) {
                title = "Invalid API Key";
                code  = autoErrorCode(20, 3);
                msg = fmtUserError(
                    "Your API key doesn't have permission for this model.",
                    "Either pick a model the key has access to, or generate a new key with the right scopes in the provider dashboard.",
                    code, errorMsg);
            } else {
                title = "Access Denied";
                code  = autoErrorCode(30, 3);
                msg = fmtUserError(
                    "The provider denied the request (HTTP 403).",
                    "This usually means the model requires a paid plan, your free-tier quota is exhausted, or the API isn't enabled in your provider dashboard.",
                    code, errorMsg);
            }

        } else if (statusCode == 404) {
            title = "Model Not Found";
            code  = autoErrorCode(40, 4);
            msg = fmtUserError(
                "The provider doesn't recognize the model name you configured.",
                "Open settings and double-check the exact model name (capitalization and version matter).",
                code, errorMsg);

        } else if (statusCode == 429) {
            bool isQuota = errorMsg.find("quota") != std::string::npos || errorMsg.find("Quota") != std::string::npos;
            title = "Rate Limit Exceeded";
            code  = autoErrorCode(30, isQuota ? 29 : 9);
            msg = isQuota
                ? fmtUserError(
                    "You've hit the provider's quota for this billing period.",
                    "Wait until the quota resets, upgrade your plan, or switch to a different provider in settings.",
                    code, errorMsg)
                : fmtUserError(
                    "You're sending requests faster than the provider allows.",
                    "Wait 20-30 seconds and try again, or raise \"rate-limit-seconds\" in settings.",
                    code, errorMsg);

        } else if (statusCode == 400) {
            if (isKeyError()) {
                title = "Invalid API Key";
                code  = autoErrorCode(20, 0);
                msg = fmtUserError(
                    "The provider rejected your API key (HTTP 400 with key-error message).",
                    "Open settings and paste a fresh key.",
                    code, errorMsg);
            } else if (errorMsg.find("model") != std::string::npos || errorStatus == "NOT_FOUND") {
                title = "Invalid Model";
                code  = autoErrorCode(40, 0);
                msg = fmtUserError(
                    "The model name you picked isn't valid for this provider.",
                    "Check the model name in settings (some providers use \"models/foo\", others just \"foo\").",
                    code, errorMsg);
            } else {
                title = "Invalid Request";
                code  = autoErrorCode(40, 99);
                msg = fmtUserError(
                    "The provider rejected the request as malformed (HTTP 400).",
                    "This usually means an outdated model or a setting the provider no longer supports. Try a different model or re-install the mod.",
                    code, errorMsg);
            }

        } else if (statusCode >= 500) {
            title = fmt::format("Service Error (HTTP {})", statusCode);
            code  = autoErrorCode(50, std::min(statusCode - 500, 99));
            msg = fmtUserError(
                "The provider is having an outage.",
                "Wait a couple of minutes and retry, or switch to a different provider in settings.",
                code, errorMsg);
        } else if (statusCode == 0) {
            title = "Connection Failed";
            code  = autoErrorCode(10, 1);
            msg = fmtUserError(
                "The mod couldn't reach the provider at all (no HTTP response).",
                "Check your internet, your firewall, and (if using Ollama/LM Studio) that the local server is running.",
                code);
        } else if (!errorMsg.empty()) {
            title = fmt::format("API Error (HTTP {})", statusCode);
            code  = autoErrorCode(40, 50);
            msg = fmtUserError(
                "The provider returned an error.",
                "Open Geode log for the full details. If this persists, try a different provider in settings.",
                code, errorMsg);
        } else {
            title = fmt::format("API Error (HTTP {})", statusCode);
            code  = autoErrorCode(40, 98);
            msg = fmtUserError(
                fmt::format("The provider returned an unexpected HTTP {}.", statusCode),
                "Check the provider's status page, then try again or switch providers.",
                code, errorBody.substr(0, 140));
        }
    }
    return {title, msg};
}

// ─── Per-provider API key / model helpers ─────────────────────────────────────

static std::string getProviderApiKey(const std::string& provider) {
    // Prefer a Sign-In OAuth token when one is saved. Only HuggingFace ships
    // a true button-only OAuth flow that works from a desktop mod with a
    // loopback listener. OpenRouter requires https://*:443/3000 callbacks
    // (per their docs), so its PKCE flow is incompatible — paste-key path
    // only. Gemini intentionally stays paste-only too.
    if (provider == "huggingface") {
        std::string t = oauth::savedToken(provider);
        if (!t.empty()) return t;
    }
    if (provider == "gemini")       return Mod::get()->getSettingValue<std::string>("gemini-api-key");
    if (provider == "claude")       return Mod::get()->getSettingValue<std::string>("claude-api-key");
    if (provider == "openai")       return Mod::get()->getSettingValue<std::string>("openai-api-key");
    if (provider == "ministral")    return Mod::get()->getSettingValue<std::string>("ministral-api-key");
    if (provider == "huggingface")  return Mod::get()->getSettingValue<std::string>("huggingface-api-key");
    if (provider == "openrouter")   return Mod::get()->getSettingValue<std::string>("openrouter-api-key");
    if (provider == "deepseek")     return Mod::get()->getSettingValue<std::string>("deepseek-api-key");
    if (provider == "custom")       return Mod::get()->getSettingValue<std::string>("custom-provider-api-key");
    return ""; // ollama / local — no key needed
}

static std::string getProviderModel(const std::string& provider) {
    if (provider == "gemini")       return Mod::get()->getSettingValue<std::string>("gemini-model");
    if (provider == "claude")       return Mod::get()->getSettingValue<std::string>("claude-model");
    if (provider == "openai")       return Mod::get()->getSettingValue<std::string>("openai-model");
    if (provider == "ministral")    return Mod::get()->getSettingValue<std::string>("ministral-model");
    if (provider == "huggingface")  return Mod::get()->getSettingValue<std::string>("huggingface-model");
    if (provider == "ollama")       return Mod::get()->getSettingValue<std::string>("ollama-model");
    if (provider == "lm-studio")    return Mod::get()->getSettingValue<std::string>("lm-studio-model");
    if (provider == "llama-cpp")    return Mod::get()->getSettingValue<std::string>("llama-cpp-model");
    if (provider == "openrouter")   return Mod::get()->getSettingValue<std::string>("openrouter-model");
    if (provider == "deepseek")     return Mod::get()->getSettingValue<std::string>("deepseek-model");
    if (provider == "custom")       return Mod::get()->getSettingValue<std::string>("custom-provider-model");
    return "unknown";
}

// ─── Custom provider (BYOPAK — Bring Your Own Provider And Key) ─────────────
// Parses the user's "Authorization: Bearer ${KEY}" template into a (name, value)
// pair, substituting ${KEY} with the actual key. Empty header template (no
// colon) means no auth header at all (useful for local self-hosted servers).
// Returns nullopt if the user left the auth template blank.
static std::optional<std::pair<std::string, std::string>> parseCustomAuthHeader(const std::string& apiKey) {
    std::string tmpl = Mod::get()->getSettingValue<std::string>("custom-provider-auth");
    if (tmpl.empty()) return std::nullopt;
    // No key + a template that wants one would produce a malformed
    // "Authorization: Bearer" header — local no-auth servers (the only
    // reason the key may legitimately be empty) want NO header instead.
    if (apiKey.empty() && tmpl.find("${KEY}") != std::string::npos)
        return std::nullopt;

    // Substitute ${KEY} placeholder. Length is fixed at 6 chars.
    constexpr std::string_view PLACEHOLDER = "${KEY}";
    size_t pos = tmpl.find(PLACEHOLDER);
    while (pos != std::string::npos) {
        tmpl.replace(pos, PLACEHOLDER.size(), apiKey);
        pos = tmpl.find(PLACEHOLDER, pos + apiKey.size());
    }

    // Split on first colon: "Name: Value"
    size_t colon = tmpl.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string name  = tmpl.substr(0, colon);
    std::string value = tmpl.substr(colon + 1);
    // Trim leading whitespace from value
    while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
    if (name.empty() || value.empty()) return std::nullopt;
    return std::make_pair(name, value);
}

// Parse the "example-level-ids" setting into at most 5 numeric GD level IDs.
// Accepts commas/spaces/semicolons as separators; ignores junk tokens.
// Scan a prompt for "<digits> bpm" (case-insensitive); 0 when absent.
// Manual digit scan — same exception-free pattern as parseExampleLevelIds.
static int parseBpmFromPrompt(const std::string& prompt) {
    for (size_t i = 0; i + 2 < prompt.size(); ++i) {
        char a = (char)std::tolower((unsigned char)prompt[i]);
        if (a != 'b') continue;
        char b = (char)std::tolower((unsigned char)prompt[i + 1]);
        char c = (char)std::tolower((unsigned char)prompt[i + 2]);
        if (b != 'p' || c != 'm') continue;
        // Walk backwards over optional space(s), then digits.
        size_t j = i;
        while (j > 0 && prompt[j - 1] == ' ') --j;
        size_t end = j;
        while (j > 0 && prompt[j - 1] >= '0' && prompt[j - 1] <= '9') --j;
        if (j == end) continue;
        auto v = geode::utils::numFromString<int>(prompt.substr(j, end - j));
        if (v && v.unwrap() >= 30 && v.unwrap() <= 300) return v.unwrap();
    }
    return 0;
}

// Beat grid note for the prompt: BPM -> units-per-beat at 1x player speed.
// Empty when the prompt names no BPM.
static std::string buildBeatGridNote(const std::string& prompt) {
    int bpm = parseBpmFromPrompt(prompt);
    if (!bpm) return {};
    constexpr float SPEED_1X = 311.58f;
    float unitsPerBeat = SPEED_1X * 60.f / (float)bpm;
    std::string marks;
    for (int m = 0; m < 8; ++m) {
        if (m) marks += ", ";
        marks += fmt::format("{:.0f}", m * unitsPerBeat * 4.f);
    }
    return fmt::format(
        "\n\nBEAT GRID ({} BPM at 1x speed): one beat = {:.0f} units. Place "
        "orbs, pads, and key obstacles ON beat multiples; start sections on "
        "measure marks (4 beats): {} ... Scale spacing if you change speed "
        "portals ({:.0f}u at 0.5x, {:.0f}u at 2x).",
        bpm, unitsPerBeat, marks,
        251.16f * 60.f / bpm, 387.42f * 60.f / bpm);
}

// Pull a "style: <levelID>" directive out of the prompt (case-insensitive,
// removes it in place). Returns the ID digits, or empty.
static std::string extractStyleId(std::string& prompt) {
    static const char* KEY = "style:";
    for (size_t i = 0; i + 6 <= prompt.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < 6; ++k)
            if ((char)std::tolower((unsigned char)prompt[i + k]) != KEY[k]) { match = false; break; }
        if (!match) continue;
        size_t j = i + 6;
        while (j < prompt.size() && prompt[j] == ' ') ++j;
        size_t start = j;
        while (j < prompt.size() && prompt[j] >= '0' && prompt[j] <= '9') ++j;
        if (j - start < 4) continue;  // level IDs are >=4 digits; "style: dark" is prose
        std::string id = prompt.substr(start, j - start);
        prompt.erase(i, j - i);
        // Tidy doubled spaces left behind.
        if (i < prompt.size() && i > 0 && prompt[i] == ' ' && prompt[i - 1] == ' ')
            prompt.erase(i, 1);
        return id;
    }
    return {};
}

// Region rebuild (mutation mode): "region 300-900 ..." prefix marks an X
// range whose ORIGINAL objects get deleted when the replacement blueprint is
// accepted. Parsed out of the prompt like extractStyleId.
struct PendingRegionDelete { float x0 = 0, x1 = 0; bool active = false; };
static PendingRegionDelete s_pendingRegionDelete;

static bool extractRegionRange(std::string& prompt, float& outX0, float& outX1) {
    static const char* KEY = "region ";
    for (size_t i = 0; i + 7 <= prompt.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < 7; ++k)
            if ((char)std::tolower((unsigned char)prompt[i + k]) != KEY[k]) { match = false; break; }
        if (!match) continue;
        size_t j = i + 7;
        size_t s0 = j;
        while (j < prompt.size() && prompt[j] >= '0' && prompt[j] <= '9') ++j;
        if (j == s0) continue;
        auto x0v = geode::utils::numFromString<float>(prompt.substr(s0, j - s0));
        float x0 = x0v ? x0v.unwrap() : -1.f;
        // separator: '-', "..", or " to "
        size_t sep = j;
        if (sep < prompt.size() && prompt[sep] == '-') j = sep + 1;
        else if (sep + 1 < prompt.size() && prompt[sep] == '.' && prompt[sep + 1] == '.') j = sep + 2;
        else if (sep + 3 < prompt.size() && prompt.compare(sep, 4, " to ") == 0) j = sep + 4;
        else continue;
        size_t s1 = j;
        while (j < prompt.size() && prompt[j] >= '0' && prompt[j] <= '9') ++j;
        if (j == s1) continue;
        auto x1v = geode::utils::numFromString<float>(prompt.substr(s1, j - s1));
        float x1 = x1v ? x1v.unwrap() : -1.f;
        if (x0 < 0.f || x1 <= x0) continue;
        prompt.erase(i, j - i);
        outX0 = x0;
        outX1 = x1;
        return true;
    }
    return false;
}

static std::vector<int> parseExampleLevelIds() {
    std::string raw = Mod::get()->getSettingValue<std::string>("example-level-ids");
    std::vector<int> ids;
    std::string tok;
    auto flush = [&] {
        if (!tok.empty()) {
            if (auto n = geode::utils::numFromString<int>(tok)) {
                if (n.unwrap() > 0 && ids.size() < 5) ids.push_back(n.unwrap());
            }
            tok.clear();
        }
    };
    for (char c : raw) {
        if (c >= '0' && c <= '9') tok += c;
        else flush();
    }
    flush();
    return ids;
}

static std::string getOllamaUrl() {
    bool usePlatinum = Mod::get()->getSettingValue<bool>("use-platinum");
    return usePlatinum
        ? "http://sn-1.vltgg.net:21800"
        : "http://localhost:11434";
}

// Apply the per-provider authentication header(s) to a WebRequest. One source
// of truth for the single-shot path, the tool loop, AND the settings popup's
// key validator.
static void applyProviderAuth(web::WebRequest& req,
                              const std::string& provider,
                              const std::string& apiKey)
{
    if (provider == "gemini") {
        req.header("x-goog-api-key", apiKey);
    } else if (provider == "claude") {
        req.header("x-api-key", apiKey);
        req.header("anthropic-version", "2023-06-01");
    } else if (provider == "openai"     || provider == "ministral" ||
               provider == "huggingface"|| provider == "deepseek")
    {
        req.header("Authorization", fmt::format("Bearer {}", apiKey));
    } else if (provider == "openrouter") {
        req.header("Authorization", fmt::format("Bearer {}", apiKey));
        req.header("Referer", "https://editorai.pages.dev");
        req.header("X-Title", "EditorAI");
    } else if (provider == "custom") {
        // BYOPAK: apply the user's auth header template (with ${KEY}
        // subst). Skip entirely if the template is empty or malformed
        // (local servers may need no auth header at all).
        if (auto header = parseCustomAuthHeader(apiKey)) {
            req.header(header->first.c_str(), header->second);
            log::info("Custom provider: auth header '{}' set ({} chars)",
                header->first, header->second.size());
        } else {
            log::info("Custom provider: no auth header (template empty or no colon)");
        }
    }
    // ollama / lm-studio / llama-cpp: no auth header
}

// Per-provider request timeout, shared by the single-shot and tool-loop
// paths. Ollama gets the user-configured timeout (large local models with
// partial GPU offload can be very slow); other local servers get 5 min;
// hosted APIs get 3 min so a dead connection can't hang a generation
// forever.
static std::chrono::seconds providerTimeout(const std::string& provider) {
    if (provider == "ollama") {
        return std::chrono::seconds(
            (int)Mod::get()->getSettingValue<int64_t>("ollama-timeout"));
    }
    if (provider == "lm-studio" || provider == "llama-cpp")
        return std::chrono::seconds(300);
    // Hosted APIs: generous — reasoning models with 16K-token outputs can
    // legitimately run minutes; this exists only so a dead connection can't
    // hang a generation forever.
    return std::chrono::seconds(300);
}

// ─── Deferred object struct ───────────────────────────────────────────────────

struct DeferredObject {
    int objectID;
    CCPoint position;
    matjson::Value data;
};

// ─── GD coordinate constants ────────────────────────────────────────────────
// GD's editor uses a 30-unit grid. Block CENTERS sit on the grid at Y =
// 15 + 30*n. The exact Y where the lowest block should land depends on the
// editor's visible ground sprite — GD 2.2's default ground sprite is ~90
// units tall, so blocks placed at Y=15 (the level-data "ground row") render
// fully INSIDE the ground sprite and look 2 cells underground.
//
// The setting "ai-ground-y" lets the user pick the Y the AI's lowest object
// lands on. Default 105 = "one cell above a 90-unit ground sprite", which is
// the typical player-spawn Y in vanilla GD. If blocks still look wrong the
// user can fine-tune in mod settings without rebuilding.
// Helper: read the configured ground Y as a float for arithmetic. Clamped to a
// sane range so a bad setting can't cause underflow.
static float getGroundY() {
    int v = (int)Mod::get()->getSettingValue<int64_t>("ai-ground-y");
    return (float)std::clamp(v, 0, 1000);
}

// ─── GD length classification ──────────────────────────────────────────────
// Geometry Dash classifies levels by their duration at the START speed:
//
//   Tiny    : 0–10 s
//   Short   : 10–30 s
//   Medium  : 30–60 s
//   Long    : 60–120 s
//   XL      : 120 s+
//   (XXL : the mod's own informal extension for very long levels.)
//
// The player moves 311.58 units / second at 1× speed (verified against the
// constants the Pathfinding-Bot project uses). So at the default speed:
//   1 s  ≈ 311 units of X
//   10 s ≈ 3115 units
//   30 s ≈ 9347 units
//   60 s ≈ 18695 units
//   120 s ≈ 37389 units
//
// The mod's "length" setting uses the labels short / medium / long / xl / xxl.
// We map each to a (target_min_seconds, target_max_seconds) tuple so the AI
// has a concrete duration to hit. The enforcement loop in runToolLoop rejects
// final answers shorter than target_min and asks the AI for more.
static constexpr float GD_PLAYER_SPEED_1X = 311.58f;

struct LengthTarget {
    const char* label;
    float minSeconds;
    float maxSeconds;  // upper bound for prompt hint; not enforced
};

static LengthTarget lengthTargetForSetting(const std::string& setting) {
    if (setting == "short")  return {"Short",  10.f,  30.f};
    if (setting == "medium") return {"Medium", 30.f,  60.f};
    if (setting == "long")   return {"Long",   60.f, 120.f};
    if (setting == "xl")     return {"XL",    120.f, 180.f};
    if (setting == "xxl")    return {"XXL",   180.f, 300.f};
    // Anything else (including "tiny" or custom user text) → medium-ish default
    return {"Medium", 30.f, 60.f};
}

// Convert max X to a (seconds, category-name) pair.
static std::pair<float, std::string> describeLengthByX(float maxX) {
    float secs = maxX / GD_PLAYER_SPEED_1X;
    const char* cat;
    if      (secs < 10.f)  cat = "Tiny";
    else if (secs < 30.f)  cat = "Short";
    else if (secs < 60.f)  cat = "Medium";
    else if (secs < 120.f) cat = "Long";
    else                   cat = "XL";
    return {secs, std::string(cat)};
}

// ── EditorAI Script (EAS) parser ──────────────────────────────────────────
//
// EAS is a line-based DSL that replaces JSON for AI output. Each line is one
// verb plus key=value args. The parser tokenizes, dispatches by verb, and
// produces the same intermediate matjson::Value the existing JSON path expects
// (so downstream code — applyLevelMetadata, prepareObjects, macros — is reused
// unchanged).
//
// See the system prompt for the full spec. Brief grammar:
//
//   # comment line OR plan markdown — ignored
//   META key=value ...                — name/desc/song/bg/ground/platformer/audio_track
//   COLOR ch=N hex=RRGGBB [blend] [player_color]
//   SECTION x0..x1 difficulty=... mode=...
//   OBJ <type> x y [k=v ...]
//   SPIKE x [y=Y] [variant=...]
//   BLOCK x y [variant=...]
//   SAW x y [size=small|medium|large]
//   ORB <color> x y                   — yellow/pink/red/blue/green/black/dash
//   PAD <color> x y                   — yellow/pink/red
//   PORTAL <kind> x [y=Y]             — cube/ship/ball/ufo/wave/robot/spider/swing
//                                       /mini/normal/mirror-on/mirror-off
//                                       /speed-half/-normal/-double/-triple/-quad
//                                       /gravity-up/-down/-reverse
//                                       /teleport-blue/-orange
//   FLOOR x0..x1 [y=Y] [type=...]     — solid ground row across X range
//   CORRIDOR x0..x1 ceiling=Y floor=Y — ship/wave channel
//   PLATFORM-RUN x0..x1 y=Y [gap=G] [gap-every=E]
//   SPIKE-TRAIN x count=N [spacing=S]
//   STAIR-UP x steps=N [step-w=W] [step-h=H]
//   STAIR-DOWN x y_top=Y steps=N
//   PILLAR x y_bot=Y y_top=Y
//   BLOCK-WALL x y_bot=Y y_top=Y
//   BLOCK-STACK x y count=N
//   ARC-ORBS x y count=N [radius=R] [orb=yellow]
//   MIRROR axis=X [from=x0..x1]
//   COPY from=x0..x1 offset=DX
//   TRIGGER color   ch=N hex=RRGGBB at=X [duration=T] [blend]
//   TRIGGER alpha   groups=g at=X to=A duration=T
//   TRIGGER move    groups=g at=X dx=DX dy=DY duration=T
//   TRIGGER toggle  groups=g at=X on=BOOL
//   TRIGGER pulse   ch=N hex=RRGGBB at=X duration=T
//   TRIGGER rotate  groups=g at=X degrees=D duration=T
//   TRIGGER spawn   target=N at=X [delay=T]
//   TRIGGER stop    groups=g at=X
//   TRIGGER end     at=X
//
// All triggers default to X-position-triggered. Touch triggers require explicit
// `touch=true` (this is rare and a deliberate design choice).

namespace eas {

inline std::string trim(std::string s) {
    // Single erase for the leading run — erase(0,1) in a loop is O(k*n)
    // because each call shifts the whole remainder left.
    auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    if (first > 0) s.erase(0, first);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Safe stof-equivalent — returns `dflt` on bad input, no exceptions (the
// Geode index rejects exception use outright). Mirrors std::stof's
// parse-the-leading-prefix behaviour ("30," → 30) so messy AI tokens still
// land: a single garbage token must not kill the whole EAS line, and
// `SPIKE basic 105 180` and friends must not vanish on some models.
inline float tryFloat(const std::string& s, float dflt) {
    size_t i = 0, n = s.size();
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    // numFromString (from_chars) rejects a leading '+' that stof accepted —
    // skip it here so "+30" still parses; keep '-' in the slice.
    if (i < n && s[i] == '+') ++i;
    size_t start = i;
    if (i < n && s[i] == '-') ++i;
    bool digits = false, dot = false;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (std::isdigit(c)) { digits = true; ++i; continue; }
        if (c == '.' && !dot) { dot = true; ++i; continue; }
        break;
    }
    if (digits && i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
        size_t k = j;
        while (k < n && std::isdigit((unsigned char)s[k])) ++k;
        if (k > j) i = k;
    }
    if (!digits) return dflt;
    auto res = geode::utils::numFromString<float>(
        std::string_view(s).substr(start, i - start));
    return res ? res.unwrap() : dflt;
}

// Safe stoi-equivalent with the same leading-prefix semantics as tryFloat.
inline int tryInt(const std::string& s, int dflt) {
    size_t i = 0, n = s.size();
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    // Skip a leading '+' — numFromString rejects it (see tryFloat).
    if (i < n && s[i] == '+') ++i;
    size_t start = i;
    if (i < n && s[i] == '-') ++i;
    size_t d = i;
    while (i < n && std::isdigit((unsigned char)s[i])) ++i;
    if (i == d) return dflt;
    auto res = geode::utils::numFromString<int>(
        std::string_view(s).substr(start, i - start));
    return res ? res.unwrap() : dflt;
}

// True iff `s` parses cleanly as a leading number. Lets us probe a positional
// token: if it's numeric, treat it as x/y; if not, it's a variant/color/kind
// keyword. Mirrors the try/catch dance the SAW handler did inline.
inline bool isNumericTok(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    bool sawDigit = false, sawDot = false;
    for (; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (std::isdigit(c)) { sawDigit = true; continue; }
        if (c == '.' && !sawDot) { sawDot = true; continue; }
        if (c == 'e' || c == 'E') { return sawDigit; }
        return false;
    }
    return sawDigit;
}

// First kv key (in order) that's set on the line — returns its float value,
// or `dflt` if none of the aliases are present. Lets EAS handlers accept
// `start=`, `from=`, `x0=` as synonyms for the canonical `x_start=` without
// nine ternaries per handler.
struct Line;  // forward, defined below
template <class L>
inline float fnumAlias(const L& ln, std::initializer_list<const char*> keys, float dflt) {
    for (auto k : keys) if (ln.kv.count(k)) return ln.fnum(k, dflt);
    return dflt;
}
template <class L>
inline std::string strAlias(const L& ln, std::initializer_list<const char*> keys,
                            const std::string& dflt = "") {
    for (auto k : keys) if (ln.kv.count(k)) return ln.str(k, dflt);
    return dflt;
}

// Tokenize one line into a verb + a map of key=value args + a list of positional
// args (the part between verb and first key=value). Quoted strings are unescaped.
struct Line {
    std::string verb;                              // first whitespace-separated token
    std::vector<std::string> pos;                  // positional args (before key=value pairs)
    std::unordered_map<std::string, std::string> kv;
    bool flag(const std::string& k) const {
        auto it = kv.find(k);
        return it != kv.end() && (it->second.empty() || it->second == "true" || it->second == "1");
    }
    std::string str(const std::string& k, const std::string& dflt = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? dflt : it->second;
    }
    float fnum(const std::string& k, float dflt) const {
        auto it = kv.find(k);
        return it == kv.end() ? dflt : tryFloat(it->second, dflt);
    }
    int inum(const std::string& k, int dflt) const {
        auto it = kv.find(k);
        return it == kv.end() ? dflt : tryInt(it->second, dflt);
    }
};

inline Line tokenize(const std::string& raw) {
    Line out;
    std::string s = trim(raw);
    if (s.empty() || s[0] == '#' || (s.size() >= 2 && s[0] == '/' && s[1] == '/')) return out;
    // First token is the verb.
    size_t i = 0;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    out.verb = lower(s.substr(0, i));
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;

    // Parse remaining tokens. A token is either:
    //   - bare positional (no '=' in it before any whitespace)
    //   - key=value where value may be quoted ("..."), bare, or contain '..'
    while (i < s.size()) {
        // Skip whitespace
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        // Read until whitespace or '='.
        size_t start = i;
        size_t eq = std::string::npos;
        // Find the equals or end of the bare token (no spaces inside it)
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            if (s[i] == '=' && eq == std::string::npos) { eq = i; }
            if (s[i] == '"') {
                // Quoted segment: scan to matching close quote
                size_t qend = i + 1;
                while (qend < s.size() && s[qend] != '"') {
                    if (s[qend] == '\\' && qend + 1 < s.size()) qend += 2;
                    else                                       ++qend;
                }
                if (qend >= s.size()) { i = qend; break; }
                i = qend + 1;
                continue;
            }
            ++i;
        }
        std::string tok = s.substr(start, i - start);
        if (eq == std::string::npos) {
            // Bare token: either a positional (variant/x/y) OR a known flag
            // word. The system prompt teaches bare flags (`SPIKE 600 notouch`,
            // `BLOCK 900 105 passable`, `COLOR ... blend`, `TRIGGER ... multi_activate`),
            // but flag()/applyCommonFields/triggerObj only read kv — so a bare
            // flag would otherwise fall into `pos`, where positional consumers
            // silently eat it (and the flag never applies). Promote a recognized
            // flag word into kv as "true"; non-flag bare tokens (variants like
            // `small`/`ship`/`yellow`) stay positional as before.
            static const std::unordered_set<std::string> kBareFlags = {
                "passable", "notouch", "no_touch", "hide", "noglow", "no_glow",
                "nofade", "dont_fade", "dontenter", "dont_enter", "highdetail",
                "high_detail", "noeffects", "no_effects", "blend", "blending",
                "multi_activate", "multi", "touch", "spawn_triggered",
                "flip_x", "flip_y", "player_color", "exclusive", "lock_rotation",
                "lock_object_rotation", "lock_to_player_x", "lock_to_player_y",
                "activate", "hold", "editor_disable", "exit",
            };
            std::string lo = lower(tok);
            if (kBareFlags.count(lo)) out.kv[lo] = "true";
            else                      out.pos.push_back(tok);
        } else {
            size_t rel = eq - start;
            std::string k = lower(tok.substr(0, rel));
            std::string v = tok.substr(rel + 1);
            // Unwrap quotes + un-escape
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                v = v.substr(1, v.size() - 2);
                std::string unesc; unesc.reserve(v.size());
                for (size_t j = 0; j < v.size(); ++j) {
                    if (v[j] == '\\' && j + 1 < v.size()) { unesc.push_back(v[j+1]); ++j; }
                    else                                   unesc.push_back(v[j]);
                }
                v = unesc;
            }
            out.kv[k] = v;
        }
    }
    return out;
}

// Parse "x0..x1" range. Returns (x0, x1, ok).
inline std::tuple<float, float, bool> parseRange(const std::string& s) {
    auto dd = s.find("..");
    if (dd == std::string::npos) return {0, 0, false};
    float a = tryFloat(s.substr(0, dd), NAN);
    float b = tryFloat(s.substr(dd + 2), NAN);
    if (std::isnan(a) || std::isnan(b)) return {0, 0, false};
    return {a, b, true};
}

// Hex string → RGB triplet matjson array. Supports "RRGGBB" with optional "#".
inline matjson::Value hexToRGBArray(const std::string& hex) {
    auto h = hex;
    if (!h.empty() && h.front() == '#') h.erase(0, 1);
    auto arr = matjson::Value::array();
    if (h.size() != 6) { arr.push(255); arr.push(255); arr.push(255); return arr; }
    GLubyte r = 255, g = 255, b = 255;
    parseHexColor(h, r, g, b);  // leaves 255,255,255 on failure
    arr.push((int)r); arr.push((int)g); arr.push((int)b);
    return arr;
}

// ── ORB / PAD / PORTAL color → type-name resolvers ─────────────────────────
// Translate the short EAS keywords (yellow/pink/blue/cube/ship/...) into the
// canonical object_ids.json names so downstream code resolves them correctly.
inline std::string orbType(const std::string& color) {
    auto c = lower(color);
    if (c == "yellow")   return "jump_orb_yellow_jump_orb";
    if (c == "pink")     return "jump_orb_pink_jump_orb";
    if (c == "red")      return "jump_orb_red_jump_orb";
    if (c == "blue")     return "obj_blue_gravity_orb";
    if (c == "green")    return "obj_green_dash_orb";
    if (c == "black")    return "obj_black_drop_orb";
    if (c == "dash")     return "obj_green_dash_orb";
    if (c == "gravity")  return "obj_blue_gravity_orb";
    if (c == "teleport") return "obj_teleport_orb";
    if (c == "toggle")   return "obj_toggle_orb";
    return "jump_orb_yellow_jump_orb";
}
inline std::string padType(const std::string& color) {
    auto c = lower(color);
    if (c == "yellow") return "jump_pad_yellow_jump_pad";
    if (c == "pink")   return "jump_pad_pink_jump_pad";
    if (c == "red")    return "jump_pad_red_jump_pad";
    if (c == "blue")   return "obj_blue_gravity_pad";
    if (c == "spider") return "obj_spider_pad";
    return "jump_pad_yellow_jump_pad";
}
inline std::string portalType(const std::string& kind) {
    auto c = lower(kind);
    if (c == "cube")             return "portal_cube_portal";
    if (c == "ship")             return "portal_ship_portal";
    if (c == "ball")             return "portal_ball_portal";
    if (c == "ufo")              return "portal_ufo_portal";
    if (c == "wave")             return "portal_wave_portal";
    if (c == "robot")            return "portal_robot_portal";
    if (c == "spider")           return "portal_spider_portal";
    if (c == "swing")            return "portal_swing_portal";
    if (c == "mini")             return "portal_green_size_portal";
    if (c == "normal" || c == "size-normal") return "portal_pink_size_portal";
    if (c == "mirror-on")        return "portal_blue_mirror_portal";
    if (c == "mirror-off")       return "portal_orange_mirror_portal";
    if (c == "speed-half" || c == "0.5x") return "portal_yellow_slow_speed_portal";
    if (c == "speed-normal" || c == "1x") return "portal_blue_normal_speed_portal";
    if (c == "speed-double" || c == "2x") return "portal_green_fast_speed_portal";
    if (c == "speed-triple" || c == "3x") return "portal_red_fast_speed_portal";
    if (c == "speed-quad"   || c == "4x") return "portal_pink_fast_speed_portal";
    if (c == "gravity-up")       return "portal_flipped_gravity_portal";
    if (c == "gravity-down")     return "portal_normal_gravity_portal";
    if (c == "gravity-reverse")  return "portal_reverse_gravity_portal";
    if (c == "teleport-blue")    return "portal_unlinked_blue_teleport_portal";
    if (c == "teleport-orange")  return "portal_unlinked_orange_teleport_portal";
    if (c == "teleport-linked")  return "portal_linked_teleport_portals";
    if (c == "dual")             return "portal_dual_portal";
    return "portal_cube_portal";
}
inline std::string spikeVariant(const std::string& v) {
    auto c = lower(v);
    if (c == "tiny")    return "spike_black_gradient_tiny_spike";
    if (c == "small")   return "spike_colored_small_spike";
    if (c == "half")    return "spike_colored_half_spike";
    if (c == "colored") return "spike_colored_spike";
    if (c == "pit")     return "spike_black_pit_hazard";
    if (c == "slope")   return "spike_black_slope_hazard";
    return "spike_black_gradient_spike";
}
inline std::string blockVariant(const std::string& v) {
    auto c = lower(v);
    if (c == "small")        return "block_black_gradient_small_square";
    if (c == "slab")         return "block_black_gradient_single_slab";
    if (c == "slab-middle")  return "block_black_gradient_slab_middle";
    if (c == "slab-side")    return "block_black_gradient_slab_side";
    if (c == "grid")         return "block_grid_patterned_inner_square";
    if (c == "grid-top")     return "block_grid_patterned_top_square";
    if (c == "grid-corner")  return "block_grid_patterned_inner_corner_square";
    return "block_black_gradient_square";
}
inline std::string sawVariant(const std::string& sz) {
    auto c = lower(sz);
    if (c == "large" || c == "big")   return "spike_large_black_sawblade";
    if (c == "medium" || c == "med")  return "spike_medium_black_sawblade";
    return "spike_small_black_sawblade";
}

// ── Macro emitter helpers ──────────────────────────────────────────────────

inline matjson::Value makeMacro(const std::string& name) {
    auto m = matjson::Value::object();
    m["name"] = name;
    return m;
}
inline matjson::Value makeObj(const std::string& type, float x, float y) {
    auto o = matjson::Value::object();
    o["type"] = type;
    o["x"] = (double)x;
    o["y"] = (double)y;
    return o;
}

// Parse a comma-separated list of ints from a string into a matjson array.
// Used for `groups=1,2,3` and other multi-value fields.
inline matjson::Value parseIntList(const std::string& s) {
    auto arr = matjson::Value::array();
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        while (j < s.size() && s[j] != ',') ++j;
        std::string tok = s.substr(i, j - i);
        // trim whitespace
        while (!tok.empty() && std::isspace((unsigned char)tok.front())) tok.erase(0, 1);
        while (!tok.empty() && std::isspace((unsigned char)tok.back()))  tok.pop_back();
        if (!tok.empty()) {
            if (auto num = geode::utils::numFromString<int>(tok))
                arr.push((double)num.unwrap());
        }
        i = j + 1;
    }
    return arr;
}

// Apply the fields every kind of object can carry: groups, color channels,
// rotation, scale, multi_activate, etc. Called from every object emitter so
// the AI can attach these to spikes, blocks, orbs, portals, triggers — all
// consistent.
//
// Also called (in spirit) when generating MACRO params: a macro line like
// `FLOOR 0..900 color=4 groups=1 scale=1.2` needs those fields stored in the
// macro params so the downstream macro expander can re-apply them to every
// emitted block. applyMacroPassthroughs (in main.cpp) does that on the
// receiving side; this function does the emitting side for both object lines
// AND macro lines (the field names are intentionally identical).
//
// Per-axis aliases:
//   color | color_channel  → color_channel
//   detail | detail_color  → detail_color_channel
//   rot | rotation         → rotation
//   multi | multi_activate → multi_activate
inline void applyCommonFields(matjson::Value& obj, const Line& ln) {
    if (ln.kv.count("groups"))        obj["groups"]               = parseIntList(ln.str("groups"));
    if (ln.kv.count("color"))         obj["color_channel"]        = (double)ln.inum("color", 1);
    if (ln.kv.count("color_channel")) obj["color_channel"]        = (double)ln.inum("color_channel", 1);
    if (ln.kv.count("detail"))        obj["detail_color_channel"] = (double)ln.inum("detail", 1);
    if (ln.kv.count("detail_color"))  obj["detail_color_channel"] = (double)ln.inum("detail_color", 1);
    if (ln.kv.count("scale"))         obj["scale"]    = (double)ln.fnum("scale", 1.f);
    if (ln.kv.count("rot"))           obj["rotation"] = (double)ln.fnum("rot", 0.f);
    if (ln.kv.count("rotation"))      obj["rotation"] = (double)ln.fnum("rotation", 0.f);
    if (ln.kv.count("flip_x") && ln.flag("flip_x"))   obj["flip_x"] = true;
    if (ln.kv.count("flip_y") && ln.flag("flip_y"))   obj["flip_y"] = true;
    if (ln.flag("multi_activate"))    obj["multi_activate"] = true;
    if (ln.flag("multi"))             obj["multi_activate"] = true;
    // 2.2 editor flags (any object): solid-looking decoration, hitbox-less
    // hazards, hidden helper geometry, glow control.
    if (ln.flag("passable"))          obj["passable"]    = true;
    if (ln.flag("notouch") || ln.flag("no_touch"))
                                      obj["no_touch"]    = true;
    if (ln.flag("hide"))              obj["hide"]        = true;
    if (ln.flag("noglow") || ln.flag("no_glow"))
                                      obj["no_glow"]     = true;
    if (ln.flag("nofade") || ln.flag("dont_fade"))
                                      obj["dont_fade"]   = true;
    if (ln.flag("dontenter") || ln.flag("dont_enter"))
                                      obj["dont_enter"]  = true;
    if (ln.flag("highdetail") || ln.flag("high_detail"))
                                      obj["high_detail"] = true;
    if (ln.flag("noeffects") || ln.flag("no_effects"))
                                      obj["no_effects"]  = true;
    if (ln.kv.count("z_layer"))       obj["z_layer"]       = (double)ln.inum("z_layer", 0);
    if (ln.kv.count("z_order"))       obj["z_order"]       = (double)ln.inum("z_order", 0);
    if (ln.kv.count("editor_layer"))  obj["editor_layer"]  = (double)ln.inum("editor_layer", 0);
    if (ln.kv.count("editor_layer_2"))obj["editor_layer_2"]= (double)ln.inum("editor_layer_2", 0);
}

// ── Trigger emitter (always position-triggered, never touch-triggered) ─────
//
// The mod's downstream code treats `{"type":"<trigger_name>", "x":..., "y":...,
// ...trigger_specific_fields}` as a trigger placement. We emit it that way.
// Touch-trigger mode is OFF by default; only emitted if EAS author writes
// touch=true explicitly.
//
// Common trigger fields supported across variants:
//   touch=true            — touch-triggered (rare; default off)
//   multi_activate=true   — fire every pass, not just first
//   easing=N              — 0-18 easing curve (see GD enum)
//   easing_rate=F         — curve sharpness
inline matjson::Value triggerObj(const std::string& type, float x, float y,
                                 const Line& ln) {
    auto t = makeObj(type, x, y);
    if (ln.flag("touch"))           t["touch_triggered"] = true;
    if (ln.flag("spawn_triggered")) t["spawn_triggered"] = true;
    if (ln.flag("multi_activate") || ln.flag("multi"))
                                    t["multi_activate"] = true;
    if (ln.kv.count("easing"))      t["easing"]      = (double)ln.inum("easing", 0);
    if (ln.kv.count("easing_rate")) t["easing_rate"] = (double)ln.fnum("easing_rate", 2.f);
    // own_groups: the trigger's OWN group membership (so spawn chains can
    // point at it) — `groups=` on trigger lines means the TARGET instead.
    if (ln.kv.count("own_groups"))  t["groups"] = parseIntList(ln.str("own_groups"));
    return t;
}

// ── Main parse entry point ─────────────────────────────────────────────────

struct ParseResult {
    bool   ok = false;
    std::string error;
    matjson::Value root;        // {analysis, objects, macros, level_metadata}
};

inline ParseResult parse(std::string_view text) {
    ParseResult r;
    auto root      = matjson::Value::object();
    auto objects   = matjson::Value::array();
    auto macros    = matjson::Value::array();
    auto metadata  = matjson::Value::object();
    auto defaultColors = matjson::Value::array();
    bool metaSeen = false;

    // Stride through lines
    std::string cur;
    auto handle_inner = [&](const std::string& raw) {
        auto ln = tokenize(raw);
        if (ln.verb.empty()) return;

        // ── Edit operations on EXISTING level objects ────────────────────
        // MOVE/DELETE/EDIT travel through the same objects array as pseudo-
        // entries carrying an "op" key; the engine splits them out before
        // spawning and executes them against the live editor (journaled, so
        // Deny rolls them back). Selector forms:
        //   #12        one object by inventory index
        //   #5-40      inventory index range (inclusive)
        //   rect:x1,y1,x2,y2   every object inside that box
        //   id:spike / id:8    every object of that type (name or numeric)
        // MOVE takes dx=/dy= deltas; EDIT applies rot=, scale=, color=,
        // detail=, flip_x, flip_y (the fields applyEditOps executes —
        // anything else parsed by applyCommonFields is currently ignored).
        if (ln.verb == "move" || ln.verb == "delete" || ln.verb == "edit") {
            std::string sel = ln.str("sel");
            if (sel.empty() && !ln.pos.empty()) sel = ln.pos[0];
            if (sel.empty()) {
                geode::log::warn("EAS: {} line without a selector - skipped", ln.verb);
                return;
            }
            auto op = matjson::Value::object();
            op["op"]  = ln.verb;
            op["sel"] = sel;
            // Optional extra filter (combines with rect selectors).
            if (ln.kv.count("type")) op["filter_type"] = ln.str("type");
            if (ln.kv.count("id"))   op["filter_id"]   = (double)ln.inum("id", 0);
            if (ln.verb == "move") {
                op["dx"] = (double)ln.fnum("dx", 0.f);
                op["dy"] = (double)ln.fnum("dy", 0.f);
            } else if (ln.verb == "edit") {
                applyCommonFields(op, ln);
            }
            objects.push(std::move(op));
            return;
        }

        // ── META ─────────────────────────────────────────────────────────
        if (ln.verb == "meta") {
            if (!ln.str("name").empty())   metadata["name"]         = ln.str("name");
            if (!ln.str("desc").empty())   metadata["description"]  = ln.str("desc");
            if (!ln.str("description").empty()) metadata["description"] = ln.str("description");
            if (!ln.str("song_id").empty()) metadata["song_id"]     = (double)ln.inum("song_id", 0);
            if (!ln.str("audio_track").empty()) metadata["audio_track"] = (double)ln.inum("audio_track", 0);
            if (!ln.str("bg").empty())     metadata["background_id"] = (double)ln.inum("bg", 1);
            if (!ln.str("ground").empty()) metadata["ground_id"]     = (double)ln.inum("ground", 1);
            if (!ln.str("middle").empty()) metadata["middle_ground_id"] = (double)ln.inum("middle", 1);
            if (!ln.str("ground_line").empty()) metadata["ground_line_id"] = (double)ln.inum("ground_line", 1);
            if (!ln.str("font").empty())   metadata["font_id"]       = (double)ln.inum("font", 0);
            if (ln.kv.count("platformer"))
                metadata["platformer_mode"] = ln.flag("platformer");
            metaSeen = true;
            return;
        }

        // ── COLOR (default channel colors, applied via metadata.default_colors) ─
        if (ln.verb == "color") {
            // If the line has "at=X", it's a runtime color trigger, fall through
            if (ln.kv.count("at")) {
                // Color trigger — emit as effect_color_trigger object
                auto t = triggerObj("effect_color_trigger", ln.fnum("at", 0), 0, ln);
                if (ln.kv.count("ch"))  t["color_channel"] = (double)ln.inum("ch", 1);
                if (ln.kv.count("hex")) t["color"]         = hexToRGBArray(ln.str("hex"));
                if (ln.kv.count("duration")) t["duration"] = (double)ln.fnum("duration", 0.5f);
                if (ln.flag("blend"))        t["blending"] = true;
                objects.push(t);
                return;
            }
            // Default color set at level start
            auto c = matjson::Value::object();
            c["channel"] = (double)ln.inum("ch", 1);
            c["color"]   = "#" + ln.str("hex", "ffffff");
            if (ln.flag("blend"))         c["blending"]     = true;
            if (ln.flag("player_color"))  c["player_color"] = true;
            defaultColors.push(c);
            return;
        }

        // ── SECTION (informational; auto-emits a section break marker the
        //    mod could use later. For now, we just track it via comments.) ──
        if (ln.verb == "section") {
            // Currently a no-op for layout. Could later emit a section
            // marker for analytics or visual checking.
            return;
        }

        // ── Primitive placements ────────────────────────────────────────
        if (ln.verb == "obj") {
            // OBJ <type> <x> <y> — type is always positional. tryFloat guards
            // against `OBJ block_x 105 nan` and friends.
            std::string type = ln.pos.empty() ? ln.str("type", "") : ln.pos[0];
            if (type.empty()) return;
            float x = ln.pos.size() > 1 ? tryFloat(ln.pos[1], ln.fnum("x", 0))   : ln.fnum("x", 0);
            float y = ln.pos.size() > 2 ? tryFloat(ln.pos[2], ln.fnum("y", 105)) : ln.fnum("y", 105);
            auto o = makeObj(type, x, y);
            applyCommonFields(o, ln);
            objects.push(o);
            return;
        }
        if (ln.verb == "spike") {
            // Accept both `SPIKE x y` (numeric-first) AND `SPIKE variant x y`
            // (variant-first, mirroring SAW). Falls back to kv `variant=`/`x=`/`y=`
            // when positionals are partial. tryFloat protects against garbage.
            std::string variant = ln.str("variant", "basic");
            float x = ln.fnum("x", 0), y = ln.fnum("y", 105);
            size_t pi = 0;
            if (pi < ln.pos.size() && !isNumericTok(ln.pos[pi])) {
                variant = ln.pos[pi]; ++pi;
            }
            if (pi < ln.pos.size()) { x = tryFloat(ln.pos[pi], x); ++pi; }
            if (pi < ln.pos.size()) { y = tryFloat(ln.pos[pi], y); ++pi; }
            auto o = makeObj(spikeVariant(variant), x, y);
            applyCommonFields(o, ln);
            objects.push(o);
            return;
        }
        if (ln.verb == "block") {
            // Same shape as SPIKE — variant-tolerant positional parsing.
            std::string variant = ln.str("variant", "basic");
            float x = ln.fnum("x", 0), y = ln.fnum("y", 105);
            size_t pi = 0;
            if (pi < ln.pos.size() && !isNumericTok(ln.pos[pi])) {
                variant = ln.pos[pi]; ++pi;
            }
            if (pi < ln.pos.size()) { x = tryFloat(ln.pos[pi], x); ++pi; }
            if (pi < ln.pos.size()) { y = tryFloat(ln.pos[pi], y); ++pi; }
            auto o = makeObj(blockVariant(variant), x, y);
            applyCommonFields(o, ln);
            objects.push(o);
            return;
        }
        if (ln.verb == "row") {
            // ROW <type> x0..x1 [y=105] [step=30] — run-length placement of
            // ANY catalog object. One line replaces dozens of OBJ lines for
            // the repetitive rows that dominate output tokens (deco strips,
            // coin lines, chain fences...). Type goes through the same
            // resolution as OBJ, so block/spike aliases work too.
            std::string type = ln.pos.empty() ? ln.str("type", "") : ln.pos[0];
            if (type.empty()) return;
            float x0 = 0, x1 = 0; bool ok = false;
            if (ln.pos.size() > 1) std::tie(x0, x1, ok) = parseRange(ln.pos[1]);
            if (ok && x1 < x0) ok = false;  // reject reversed range like siblings
            if (!ok) {
                x0 = fnumAlias(ln, {"x_start","start","from","x0"}, 0);
                x1 = fnumAlias(ln, {"x_end","end","to","x1"},       0);
                ok = (x1 >= x0) && (ln.kv.count("x_start") || ln.kv.count("start") ||
                                    ln.kv.count("from")    || ln.kv.count("x0"));
            }
            if (!ok) return;
            float y    = ln.fnum("y", 105);
            float step = ln.fnum("step", 30);
            if (!(step >= 5.f)) step = 30;        // also catches NaN
            // Cap like every macro does — a hallucinated range can't bomb
            // the editor.
            int count = (int)((x1 - x0) / step) + 1;
            count = std::clamp(count, 1, 300);
            for (int i = 0; i < count; ++i) {
                auto o = makeObj(type, x0 + i * step, y);
                applyCommonFields(o, ln);
                objects.push(o);
            }
            return;
        }
        if (ln.verb == "saw") {
            // Two accepted shapes (consistent w/ ORB/PAD/PORTAL):
            //   SAW <size> x y       — size as positional, e.g. "SAW small 2550 180"
            //   SAW x y size=<size>  — size as keyword
            std::string size = ln.str("size", "small");
            float x = 0, y = 105;
            if (!ln.pos.empty()) {
                // Probe: is pos[0] numeric (treat as x), or a size word?
                if (isNumericTok(ln.pos[0])) {
                    x = tryFloat(ln.pos[0], 0);
                    if (ln.pos.size() > 1) {
                        y = tryFloat(ln.pos[1], y);
                    } else if (ln.kv.count("y")) {
                        y = ln.fnum("y", 105);
                    }
                } else {
                    // Non-numeric → it's the size descriptor
                    size = ln.pos[0];
                    if (ln.pos.size() > 1) x = tryFloat(ln.pos[1], x);
                    if (ln.pos.size() > 2)      y = tryFloat(ln.pos[2], y);
                    else if (ln.kv.count("y"))  y = ln.fnum("y", 105);
                }
            }
            auto o = makeObj(sawVariant(size), x, y);
            applyCommonFields(o, ln);
            objects.push(o);
            return;
        }
        if (ln.verb == "orb") {
            // Accept `ORB <color> <x> <y>` positional, OR mixed (color
            // positional + x/y kv), OR fully kv (`ORB color=yellow x=105 y=180`).
            std::string color = ln.str("color", "yellow");
            float x = ln.fnum("x", 0), y = ln.fnum("y", 105);
            size_t pi = 0;
            if (pi < ln.pos.size() && !isNumericTok(ln.pos[pi])) {
                color = ln.pos[pi]; ++pi;
            }
            if (pi < ln.pos.size()) { x = tryFloat(ln.pos[pi], x); ++pi; }
            if (pi < ln.pos.size()) { y = tryFloat(ln.pos[pi], y); ++pi; }
            if (color.empty()) return;
            auto o = makeObj(orbType(color), x, y);
            applyCommonFields(o, ln);
            objects.push(o);
            return;
        }
        if (ln.verb == "pad") {
            std::string color = ln.str("color", "yellow");
            float x = ln.fnum("x", 0), y = ln.fnum("y", 105);
            size_t pi = 0;
            if (pi < ln.pos.size() && !isNumericTok(ln.pos[pi])) {
                color = ln.pos[pi]; ++pi;
            }
            if (pi < ln.pos.size()) { x = tryFloat(ln.pos[pi], x); ++pi; }
            if (pi < ln.pos.size()) { y = tryFloat(ln.pos[pi], y); ++pi; }
            if (color.empty()) return;
            auto o = makeObj(padType(color), x, y);
            applyCommonFields(o, ln);
            objects.push(o);
            return;
        }
        if (ln.verb == "portal") {
            std::string kind = ln.str("kind", ln.str("type", ""));
            float x = ln.fnum("x", 0), y = ln.fnum("y", 165);
            size_t pi = 0;
            if (pi < ln.pos.size() && !isNumericTok(ln.pos[pi])) {
                kind = ln.pos[pi]; ++pi;
            }
            if (pi < ln.pos.size()) { x = tryFloat(ln.pos[pi], x); ++pi; }
            if (pi < ln.pos.size()) { y = tryFloat(ln.pos[pi], y); ++pi; }
            if (kind.empty()) return;
            auto o = makeObj(portalType(kind), x, y);
            applyCommonFields(o, ln);
            objects.push(o);
            return;
        }

        // ── Structural macros (delegate to the existing macro system) ───
        // Each macro line can also carry the same common fields a single
        // object can (color, groups, scale, rotation, etc.) — those are
        // stored in the macro params and propagated to every emitted child
        // object by applyMacroPassthroughs downstream.
        if (ln.verb == "floor" || ln.verb == "block-floor" || ln.verb == "block_floor") {
            // Accept "FLOOR x0..x1" positional OR x_start/x_end/start/end/
            // from/to/x0/x1 kv aliases. >= so a single-block floor still
            // emits (FLOOR 100..100 → one block at x=100).
            float x0 = 0, x1 = 0; bool ok = false;
            if (!ln.pos.empty()) std::tie(x0, x1, ok) = parseRange(ln.pos[0]);
            if (!ok) {
                x0 = fnumAlias(ln, {"x_start","start","from","x0"}, 0);
                x1 = fnumAlias(ln, {"x_end","end","to","x1"},       0);
                ok = (x1 >= x0) && (ln.kv.count("x_start") || ln.kv.count("start") ||
                                    ln.kv.count("from")    || ln.kv.count("x0"));
            }
            if (!ok) return;
            auto m = makeMacro("block_floor");
            m["x_start"] = (double)x0;
            m["x_end"]   = (double)x1;
            if (ln.kv.count("y"))    m["y"]          = (double)ln.fnum("y", 105);
            std::string bt = strAlias(ln, {"type","block-type","block_type"});
            if (!bt.empty()) m["block_type"] = bt;
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "platform-run" || ln.verb == "platform_run") {
            float x0 = 0, x1 = 0; bool ok = false;
            if (!ln.pos.empty()) std::tie(x0, x1, ok) = parseRange(ln.pos[0]);
            if (!ok) {
                x0 = fnumAlias(ln, {"x_start","start","from","x0"}, 0);
                x1 = fnumAlias(ln, {"x_end","end","to","x1"},       0);
                ok = (x1 >= x0) && (ln.kv.count("x_start") || ln.kv.count("start") ||
                                    ln.kv.count("from")    || ln.kv.count("x0"));
            }
            if (!ok) return;
            auto m = makeMacro("platform_run");
            m["x_start"] = (double)x0;
            m["x_end"]   = (double)x1;
            if (ln.kv.count("y"))         m["y"]         = (double)ln.fnum("y", 0);
            if (ln.kv.count("gap"))       m["gap_size"]  = (double)ln.fnum("gap", 0);
            if (ln.kv.count("gap-every")) m["gap_every"] = (double)ln.fnum("gap-every", 120);
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "corridor") {
            // CORRIDOR x0..x1 ceiling=Y floor=Y — expand to two block_floors
            // (one at the floor Y, one at the ceiling Y). Common fields are
            // applied to BOTH rows so a corridor's color/groups propagate to
            // every block in the channel.
            float x0 = 0, x1 = 0; bool ok = false;
            if (!ln.pos.empty()) std::tie(x0, x1, ok) = parseRange(ln.pos[0]);
            if (!ok) {
                x0 = fnumAlias(ln, {"x_start","start","from","x0"}, 0);
                x1 = fnumAlias(ln, {"x_end","end","to","x1"},       0);
                ok = (x1 >= x0) && (ln.kv.count("x_start") || ln.kv.count("start") ||
                                    ln.kv.count("from")    || ln.kv.count("x0"));
            }
            if (!ok) return;
            float floorY = ln.fnum("floor", 105);
            float ceilY  = ln.fnum("ceiling", floorY + 240);
            for (auto y : { floorY, ceilY }) {
                auto m = makeMacro("block_floor");
                m["x_start"] = (double)x0; m["x_end"] = (double)x1; m["y"] = (double)y;
                applyCommonFields(m, ln);
                macros.push(m);
            }
            return;
        }
        if (ln.verb == "spike-train" || ln.verb == "spike_train") {
            float x = ln.pos.empty() ? ln.fnum("x", 0) : tryFloat(ln.pos[0], ln.fnum("x", 0));
            auto m = makeMacro("spike_train");
            m["x"] = (double)x;
            m["count"]   = (double)ln.inum("count", 3);
            m["spacing"] = (double)ln.fnum("spacing", 30);
            if (ln.kv.count("y"))         m["y"]         = (double)ln.fnum("y", 105);
            std::string st = strAlias(ln, {"spike-type","spike_type","type","variant"});
            if (!st.empty()) m["spike_type"] = spikeVariant(st);
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "stair-up"   || ln.verb == "stair_up" ||
            ln.verb == "stair-down" || ln.verb == "stair_down") {
            float x = ln.pos.empty() ? ln.fnum("x", 0) : tryFloat(ln.pos[0], ln.fnum("x", 0));
            auto m = makeMacro(ln.verb.find("up") != std::string::npos ? "stair_up" : "stair_down");
            m["x"]     = (double)x;
            m["steps"] = (double)ln.inum("steps", 3);
            if (ln.kv.count("step-w") || ln.kv.count("step_w"))
                m["step_width"]  = (double)fnumAlias(ln, {"step-w","step_w","step_width"}, 30);
            if (ln.kv.count("step-h") || ln.kv.count("step_h"))
                m["step_height"] = (double)fnumAlias(ln, {"step-h","step_h","step_height"}, 30);
            if (ln.kv.count("y_top") || ln.kv.count("y"))
                m["y"] = (double)fnumAlias(ln, {"y_top","y"}, 0);
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "pillar") {
            float x = ln.pos.empty() ? ln.fnum("x", 0) : tryFloat(ln.pos[0], ln.fnum("x", 0));
            auto m = makeMacro("pillar");
            m["x"]       = (double)x;
            m["y_start"] = (double)fnumAlias(ln, {"y_bot","y_start","y0","bottom"}, 105);
            m["y_end"]   = (double)fnumAlias(ln, {"y_top","y_end","y1","top"},       105 + 90);
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "block-wall" || ln.verb == "block_wall") {
            float x = ln.pos.empty() ? ln.fnum("x", 0) : tryFloat(ln.pos[0], ln.fnum("x", 0));
            auto m = makeMacro("pillar");   // reuse pillar macro for vertical wall
            m["x"]       = (double)x;
            m["y_start"] = (double)fnumAlias(ln, {"y_bot","y_start","y0","bottom"}, 105);
            m["y_end"]   = (double)fnumAlias(ln, {"y_top","y_end","y1","top"},       105 + 90);
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "block-stack" || ln.verb == "block_stack") {
            // BLOCK-STACK is emitted directly as objects (not as a downstream
            // macro), so applyCommonFields is called on each block as it's
            // created. This keeps the same observable behavior — color/groups
            // attach to every block in the stack.
            float x = ln.pos.empty() ? ln.fnum("x", 0) : tryFloat(ln.pos[0], ln.fnum("x", 0));
            float y = ln.pos.size() > 1 ? tryFloat(ln.pos[1], ln.fnum("y", 105)) : ln.fnum("y", 105);
            int count = std::clamp(ln.inum("count", 2), 1, 200);  // match spike_train's cap
            std::string type = blockVariant(ln.str("variant", "basic"));
            for (int i = 0; i < count; ++i) {
                auto o = makeObj(type, x, y + i * 30);
                applyCommonFields(o, ln);
                objects.push(o);
            }
            return;
        }
        if (ln.verb == "arc-orbs" || ln.verb == "arc_orbs" || ln.verb == "orb-arc" || ln.verb == "orb_arc") {
            float x = ln.pos.empty() ? ln.fnum("x", 0) : tryFloat(ln.pos[0], ln.fnum("x", 0));
            float y = ln.pos.size() > 1 ? tryFloat(ln.pos[1], ln.fnum("y", 135)) : ln.fnum("y", 135);
            auto m = makeMacro("orb_arc");
            m["x"]     = (double)x;
            m["y"]     = (double)y;
            m["count"] = (double)ln.inum("count", 3);
            if (ln.kv.count("spacing")) m["spacing"] = (double)ln.fnum("spacing", 60);
            if (ln.kv.count("orb"))     m["orb_type"] = orbType(ln.str("orb", "yellow"));
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "pyramid") {
            // PYRAMID <x> [base=5] [y=<groundY>] [block=<type>]
            float x = ln.pos.empty() ? fnumAlias(ln, {"x", "center"}, 0)
                                     : tryFloat(ln.pos[0], fnumAlias(ln, {"x", "center"}, 0));
            auto m = makeMacro("pyramid");
            m["x"]    = (double)x;
            m["base"] = (double)ln.inum("base", 5);
            if (ln.kv.count("y"))     m["y"] = (double)ln.fnum("y", 105);
            if (ln.kv.count("block")) m["block_type"] = blockVariant(ln.str("block"));
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "ceiling-spikes" || ln.verb == "ceiling_spikes") {
            // CEILING-SPIKES x0..x1 [y=375] [spacing=30] [spike=<type>]
            float x0 = 0, x1 = 0; bool ok = false;
            if (!ln.pos.empty()) std::tie(x0, x1, ok) = parseRange(ln.pos[0]);
            if (!ok) {
                x0 = fnumAlias(ln, {"x_start","start","from","x0"}, 0);
                x1 = fnumAlias(ln, {"x_end","end","to","x1"},       x0 + 120);
                ok = (x1 >= x0) && (ln.kv.count("x_start") || ln.kv.count("start") ||
                                    ln.kv.count("from")    || ln.kv.count("x0"));
            }
            if (!ok) return;
            auto m = makeMacro("ceiling_spikes");
            m["x_start"] = (double)x0;
            m["x_end"]   = (double)x1;
            if (ln.kv.count("y"))       m["y"] = (double)ln.fnum("y", 375);
            if (ln.kv.count("spacing")) m["spacing"] = (double)ln.fnum("spacing", 30);
            if (ln.kv.count("spike"))   m["spike_type"] = spikeVariant(ln.str("spike"));
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "saw-gauntlet" || ln.verb == "saw_gauntlet") {
            // SAW-GAUNTLET x0..x1 [y=<ground+60>] [spacing=120] [size=small] [weave=0]
            float x0 = 0, x1 = 0; bool ok = false;
            if (!ln.pos.empty()) std::tie(x0, x1, ok) = parseRange(ln.pos[0]);
            if (!ok) {
                x0 = fnumAlias(ln, {"x_start","start","from","x0"}, 0);
                x1 = fnumAlias(ln, {"x_end","end","to","x1"},       x0 + 360);
                ok = (x1 >= x0) && (ln.kv.count("x_start") || ln.kv.count("start") ||
                                    ln.kv.count("from")    || ln.kv.count("x0"));
            }
            if (!ok) return;
            auto m = makeMacro("saw_gauntlet");
            m["x_start"] = (double)x0;
            m["x_end"]   = (double)x1;
            if (ln.kv.count("y"))       m["y"]       = (double)ln.fnum("y", 165);
            if (ln.kv.count("spacing")) m["spacing"] = (double)ln.fnum("spacing", 120);
            if (ln.kv.count("size"))    m["size"]    = ln.str("size");
            if (ln.kv.count("weave"))   m["weave"]   = (double)ln.fnum("weave", 0);
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "dual") {
            // DUAL x0..x1 [floor=Y gap=240 block=<variant>] — full dual-mode
            // scaffold (entry portal + mirrored corridor + exit portal).
            float x0 = 0, x1 = 0; bool ok = false;
            if (!ln.pos.empty()) std::tie(x0, x1, ok) = parseRange(ln.pos[0]);
            if (!ok) {
                x0 = fnumAlias(ln, {"x_start","start","from","x0"}, 0);
                x1 = fnumAlias(ln, {"x_end","end","to","x1"},       x0 + 600);
                ok = (x1 >= x0) && (ln.kv.count("x_start") || ln.kv.count("start") ||
                                    ln.kv.count("from")    || ln.kv.count("x0"));
            }
            if (!ok) return;
            auto m = makeMacro("dual_section");
            m["x_start"] = (double)x0;
            m["x_end"]   = (double)x1;
            if (ln.kv.count("floor")) m["floor"] = (double)ln.fnum("floor", 105);
            if (ln.kv.count("gap"))   m["gap"]   = (double)ln.fnum("gap", 240);
            if (ln.kv.count("block")) m["block_type"] = blockVariant(ln.str("block"));
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "teleport") {
            // TELEPORT x [y=Y y_offset=DY] — linked teleport pair (one object,
            // both ends; DY is the vertical jump, default +150).
            float x = ln.pos.empty() ? ln.fnum("x", 0) : tryFloat(ln.pos[0], ln.fnum("x", 0));
            auto m = makeMacro("teleport_pair");
            m["x"] = (double)x;
            if (ln.kv.count("y"))        m["y"]        = (double)ln.fnum("y", 165);
            if (ln.kv.count("y_offset")) m["y_offset"] = (double)ln.fnum("y_offset", 150);
            applyCommonFields(m, ln);
            macros.push(m);
            return;
        }
        if (ln.verb == "mirror") {
            auto m = makeMacro("mirror_horizontal");
            m["axis_x"] = (double)ln.fnum("axis", 0);
            if (ln.kv.count("from")) {
                float x0 = 0, x1 = 0; bool ok = false;
                std::tie(x0, x1, ok) = parseRange(ln.str("from"));
                if (ok) { m["from_x_start"] = (double)x0; m["from_x_end"] = (double)x1; }
            }
            macros.push(m);
            return;
        }
        if (ln.verb == "copy") {
            auto m = makeMacro("copy_paste");
            if (ln.kv.count("from")) {
                float x0 = 0, x1 = 0; bool ok = false;
                std::tie(x0, x1, ok) = parseRange(ln.str("from"));
                if (ok) { m["from_x_start"] = (double)x0; m["from_x_end"] = (double)x1; }
            }
            if (ln.kv.count("offset")) m["to_x_offset"] = (double)ln.fnum("offset", 0);
            macros.push(m);
            return;
        }

        // ── TRIGGER (all default to X-position-triggered) ───────────────
        if (ln.verb == "trigger") {
            if (ln.pos.empty()) return;
            std::string kind = lower(ln.pos[0]);
            float at = ln.fnum("at", 0);
            if (kind == "color") {
                auto t = triggerObj("effect_color_trigger", at, 0, ln);
                if (ln.kv.count("ch"))       t["color_channel"] = (double)ln.inum("ch", 1);
                if (ln.kv.count("channel"))  t["color_channel"] = (double)ln.inum("channel", 1);
                if (ln.kv.count("hex"))      t["color"]         = hexToRGBArray(ln.str("hex"));
                if (ln.kv.count("duration")) t["duration"]      = (double)ln.fnum("duration", 0.5f);
                if (ln.kv.count("opacity"))  t["opacity"]       = (double)ln.fnum("opacity", 1.f);
                if (ln.flag("blend"))        t["blending"]      = true;
                if (ln.flag("blending"))     t["blending"]      = true;
                objects.push(t);
            } else if (kind == "alpha") {
                auto t = triggerObj("effect_alpha_trigger", at, 0, ln);
                if (ln.kv.count("groups")) t["target_group"]  = (double)ln.inum("groups", 1);
                if (ln.kv.count("target")) t["target_group"]  = (double)ln.inum("target", 1);
                if (ln.kv.count("to"))     t["opacity"]       = (double)ln.fnum("to", 1.f);
                if (ln.kv.count("opacity")) t["opacity"]      = (double)ln.fnum("opacity", 1.f);
                if (ln.kv.count("duration")) t["duration"]    = (double)ln.fnum("duration", 0.5f);
                objects.push(t);
            } else if (kind == "move") {
                auto t = triggerObj("effect_move_trigger", at, 0, ln);
                if (ln.kv.count("groups")) t["target_group"]  = (double)ln.inum("groups", 1);
                if (ln.kv.count("target")) t["target_group"]  = (double)ln.inum("target", 1);
                if (ln.kv.count("dx"))     t["move_x"]        = (double)ln.fnum("dx", 0);
                if (ln.kv.count("dy"))     t["move_y"]        = (double)ln.fnum("dy", 0);
                if (ln.kv.count("move_x")) t["move_x"]        = (double)ln.fnum("move_x", 0);
                if (ln.kv.count("move_y")) t["move_y"]        = (double)ln.fnum("move_y", 0);
                if (ln.kv.count("duration")) t["duration"]    = (double)ln.fnum("duration", 0.5f);
                if (ln.flag("lock_to_player_x")) t["lock_to_player_x"] = true;
                if (ln.flag("lock_to_player_y")) t["lock_to_player_y"] = true;
                objects.push(t);
            } else if (kind == "toggle") {
                auto t = triggerObj("effect_toggle_trigger", at, 0, ln);
                if (ln.kv.count("groups")) t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target")) t["target_group"] = (double)ln.inum("target", 1);
                t["activate_group"] = ln.flag("on") || ln.flag("activate");
                objects.push(t);
            } else if (kind == "pulse") {
                auto t = triggerObj("effect_pulse_trigger", at, 0, ln);
                // Pulse targets EITHER a color channel OR a group, not both.
                if (ln.kv.count("ch"))       t["target_color_channel"] = (double)ln.inum("ch", 1);
                if (ln.kv.count("channel"))  t["target_color_channel"] = (double)ln.inum("channel", 1);
                if (ln.kv.count("groups"))   t["target_group"]         = (double)ln.inum("groups", 1);
                if (ln.kv.count("target"))   t["target_group"]         = (double)ln.inum("target", 1);
                if (ln.kv.count("hex"))      t["color"]                = hexToRGBArray(ln.str("hex"));
                if (ln.kv.count("duration")) t["duration"]             = (double)ln.fnum("duration", 0.5f);
                if (ln.kv.count("fade_in"))  t["fade_in"]              = (double)ln.fnum("fade_in", 0.f);
                if (ln.kv.count("hold"))     t["hold"]                 = (double)ln.fnum("hold", 0.f);
                if (ln.kv.count("fade_out")) t["fade_out"]             = (double)ln.fnum("fade_out", 0.f);
                if (ln.flag("exclusive"))    t["exclusive"]            = true;
                objects.push(t);
            } else if (kind == "rotate") {
                auto t = triggerObj("effect_rotate_trigger", at, 0, ln);
                if (ln.kv.count("groups"))  t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target"))  t["target_group"] = (double)ln.inum("target", 1);
                if (ln.kv.count("center"))  t["center_group"] = (double)ln.inum("center", 1);
                if (ln.kv.count("degrees")) t["degrees"]      = (double)ln.fnum("degrees", 360);
                if (ln.kv.count("duration")) t["duration"]    = (double)ln.fnum("duration", 1);
                if (ln.flag("lock_rotation") || ln.flag("lock_object_rotation"))
                                            t["lock_object_rotation"] = true;
                objects.push(t);
            } else if (kind == "spawn") {
                auto t = triggerObj("effect_spawn_trigger", at, 0, ln);
                if (ln.kv.count("target")) t["target_group"]   = (double)ln.inum("target", 1);
                if (ln.kv.count("groups")) t["target_group"]   = (double)ln.inum("groups", 1);
                if (ln.kv.count("delay"))  t["delay"]          = (double)ln.fnum("delay", 0);
                if (ln.flag("editor_disable")) t["editor_disable"] = true;
                objects.push(t);
            } else if (kind == "stop") {
                auto t = triggerObj("effect_stop_trigger", at, 0, ln);
                if (ln.kv.count("groups")) t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target")) t["target_group"] = (double)ln.inum("target", 1);
                objects.push(t);
            } else if (kind == "scale") {
                auto t = triggerObj("effect_scale_trigger", at, 0, ln);
                if (ln.kv.count("groups"))   t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target"))   t["target_group"] = (double)ln.inum("target", 1);
                if (ln.kv.count("to"))       t["scale"]        = (double)ln.fnum("to", 1);
                if (ln.kv.count("duration")) t["duration"]     = (double)ln.fnum("duration", 0.5f);
                objects.push(t);
            } else if (kind == "shake") {
                auto t = triggerObj("effect_shake_trigger", at, 0, ln);
                if (ln.kv.count("duration")) t["duration"]  = (double)ln.fnum("duration", 1);
                if (ln.kv.count("strength")) t["strength"]  = (double)ln.fnum("strength", 1);
                if (ln.kv.count("interval")) t["interval"]  = (double)ln.fnum("interval", 0);
                objects.push(t);
            } else if (kind == "zoom") {
                // TRIGGER zoom at=X zoom=1.5 [duration=T] — 2.2 camera zoom
                auto t = triggerObj("effect_zoom_camera_trigger", at, 0, ln);
                if (ln.kv.count("zoom"))     t["zoom"]     = (double)ln.fnum("zoom", 1);
                if (ln.kv.count("duration")) t["duration"] = (double)ln.fnum("duration", 0.5f);
                objects.push(t);
            } else if (kind == "static-cam" || kind == "static_cam" || kind == "camera-static") {
                // TRIGGER static-cam at=X target=G [duration=T] [exit] — lock camera to group
                auto t = triggerObj("effect_static_camera_trigger", at, 0, ln);
                if (ln.kv.count("target"))   t["target_group"] = (double)ln.inum("target", 1);
                if (ln.kv.count("groups"))   t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("duration")) t["duration"]     = (double)ln.fnum("duration", 0.5f);
                if (ln.flag("exit"))         t["exit"]         = true;
                objects.push(t);
            } else if (kind == "offset-cam" || kind == "offset_cam" || kind == "camera-offset") {
                // TRIGGER offset-cam at=X x=DX y=DY [duration=T] — pan the camera
                auto t = triggerObj("effect_offset_camera_trigger", at, 0, ln);
                if (ln.kv.count("x"))        t["move_x"]   = (double)ln.fnum("x", 0);
                if (ln.kv.count("y"))        t["move_y"]   = (double)ln.fnum("y", 0);
                if (ln.kv.count("duration")) t["duration"] = (double)ln.fnum("duration", 0.5f);
                objects.push(t);
            } else if (kind == "timewarp") {
                // TRIGGER timewarp at=X mod=0.5 — slow-mo / speed-up (2.2)
                auto t = triggerObj("effect_timewarp_trigger", at, 0, ln);
                if (ln.kv.count("mod"))      t["mod"] = (double)ln.fnum("mod", 1);
                objects.push(t);
            } else if (kind == "song") {
                // TRIGGER song at=X [sound_id=N channel=C volume=V]
                auto t = triggerObj("effect_song_trigger", at, 0, ln);
                if (ln.kv.count("sound_id")) t["sound_id"] = (double)ln.inum("sound_id", 0);
                if (ln.kv.count("channel"))  t["channel"]  = (double)ln.inum("channel", 0);
                if (ln.kv.count("volume"))   t["volume"]   = (double)ln.fnum("volume", 1);
                objects.push(t);
            } else if (kind == "sfx") {
                // TRIGGER sfx at=X sound_id=N [volume=V pitch=P]
                auto t = triggerObj("effect_sfx_trigger", at, 0, ln);
                if (ln.kv.count("sound_id")) t["sound_id"] = (double)ln.inum("sound_id", 0);
                if (ln.kv.count("volume"))   t["volume"]   = (double)ln.fnum("volume", 1);
                if (ln.kv.count("pitch"))    t["pitch"]    = (double)ln.fnum("pitch", 0);
                objects.push(t);
            } else if (kind == "follow") {
                // TRIGGER follow at=X target=G follow=G2 [x_mod=1 y_mod=1 duration=T]
                auto t = triggerObj("effect_follow_trigger", at, 0, ln);
                if (ln.kv.count("groups"))   t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target"))   t["target_group"] = (double)ln.inum("target", 1);
                if (ln.kv.count("follow"))   t["follow_group"] = (double)ln.inum("follow", 1);
                if (ln.kv.count("x_mod"))    t["x_mod"]        = (double)ln.fnum("x_mod", 1);
                if (ln.kv.count("y_mod"))    t["y_mod"]        = (double)ln.fnum("y_mod", 1);
                if (ln.kv.count("duration")) t["duration"]     = (double)ln.fnum("duration", 10);
                objects.push(t);
            } else if (kind == "follow-y" || kind == "follow_y" || kind == "follow-player-y") {
                // TRIGGER follow-y at=X target=G [speed=S delay=D offset=O max_speed=M duration=T]
                auto t = triggerObj("effect_follow_player_y_trigger", at, 0, ln);
                if (ln.kv.count("groups"))    t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target"))    t["target_group"] = (double)ln.inum("target", 1);
                if (ln.kv.count("speed"))     t["speed"]        = (double)ln.fnum("speed", 1);
                if (ln.kv.count("delay"))     t["delay"]        = (double)ln.fnum("delay", 0);
                if (ln.kv.count("offset"))    t["offset"]       = (double)ln.inum("offset", 0);
                if (ln.kv.count("max_speed")) t["max_speed"]    = (double)ln.fnum("max_speed", 0);
                if (ln.kv.count("duration"))  t["duration"]     = (double)ln.fnum("duration", 10);
                objects.push(t);
            } else if (kind == "touch") {
                // TRIGGER touch at=X target=G [activate] [hold] — player-tap activation
                auto t = triggerObj("effect_touch_trigger", at, 0, ln);
                if (ln.kv.count("groups")) t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target")) t["target_group"] = (double)ln.inum("target", 1);
                if (ln.flag("activate"))   t["activate"]     = true;
                if (ln.flag("hold"))       t["hold"]         = true;
                objects.push(t);
            } else if (kind == "count" || kind == "instant-count" || kind == "instant_count") {
                // TRIGGER count at=X item_id=N count=C target=G [activate]
                auto t = triggerObj(kind == "count" ? "effect_count_trigger"
                                                    : "effect_instant_count_trigger",
                                    at, 0, ln);
                if (ln.kv.count("item_id")) t["item_id"]      = (double)ln.inum("item_id", 1);
                if (ln.kv.count("count"))   t["count"]        = (double)ln.inum("count", 1);
                if (ln.kv.count("groups"))  t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target"))  t["target_group"] = (double)ln.inum("target", 1);
                if (ln.flag("activate"))    t["activate"]     = true;
                objects.push(t);
            } else if (kind == "pickup") {
                // TRIGGER pickup at=X item_id=N count=C — adjust a counter
                auto t = triggerObj("effect_pickup_trigger", at, 0, ln);
                if (ln.kv.count("item_id")) t["item_id"] = (double)ln.inum("item_id", 1);
                if (ln.kv.count("count"))   t["count"]   = (double)ln.inum("count", 1);
                objects.push(t);
            } else if (kind == "on-death" || kind == "on_death") {
                // TRIGGER on-death target=G [activate] — fires when the player dies
                auto t = triggerObj("effect_on_death_trigger", at, 0, ln);
                if (ln.kv.count("groups")) t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target")) t["target_group"] = (double)ln.inum("target", 1);
                if (ln.flag("activate"))   t["activate"]     = true;
                objects.push(t);
            } else if (kind == "end") {
                auto t = triggerObj("effect_10_level_end_trigger", at, 0, ln);
                objects.push(t);
            } else if (kind == "animate") {
                // TRIGGER animate groups=G anim=N at=X — play animation N on
                // the animated objects in group G
                auto t = triggerObj("effect_animate_trigger", at, 0, ln);
                if (ln.kv.count("groups")) t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target")) t["target_group"] = (double)ln.inum("target", 1);
                if (ln.kv.count("anim"))   t["animation_id"] = (double)ln.inum("anim", 0);
                if (ln.kv.count("animation_id"))
                    t["animation_id"] = (double)ln.inum("animation_id", 0);
                objects.push(t);
            } else if (kind == "gravity") {
                // TRIGGER gravity g=F at=X [duration=T] — change gravity
                // multiplier (1 = normal, 0.5 = floaty, 2 = heavy)
                auto t = triggerObj("effect_gravity_trigger", at, 0, ln);
                if (ln.kv.count("g"))        t["gravity"]  = (double)ln.fnum("g", 1.f);
                if (ln.kv.count("gravity"))  t["gravity"]  = (double)ln.fnum("gravity", 1.f);
                if (ln.kv.count("duration")) t["duration"] = (double)ln.fnum("duration", 0.5f);
                objects.push(t);
            } else if (kind == "teleport") {
                // TRIGGER teleport groups=G at=X — teleport the player to
                // group G's location (2.2 teleport trigger)
                auto t = triggerObj("effect_teleport_trigger", at, 0, ln);
                if (ln.kv.count("groups")) t["target_group"] = (double)ln.inum("groups", 1);
                if (ln.kv.count("target")) t["target_group"] = (double)ln.inum("target", 1);
                objects.push(t);
            } else if (kind == "reverse") {
                objects.push(triggerObj("effect_reverse_trigger", at, 0, ln));
            } else if (kind == "bg-on" || kind == "bg_on") {
                objects.push(triggerObj("effect_background_effect_on_trigger", at, 0, ln));
            } else if (kind == "bg-off" || kind == "bg_off") {
                objects.push(triggerObj("effect_background_effect_off_trigger", at, 0, ln));
            } else if (kind == "no-enter-fx" || kind == "no_enter_fx") {
                objects.push(triggerObj("effect_no_enter_effect_trigger", at, 0, ln));
            } else if (kind == "show-player" || kind == "show_player") {
                objects.push(triggerObj("show_player_trigger", at, 0, ln));
            } else if (kind == "hide-player" || kind == "hide_player") {
                objects.push(triggerObj("hide_player_trigger", at, 0, ln));
            } else if (kind == "show-trail" || kind == "show_trail") {
                objects.push(triggerObj("show_trail_trigger", at, 0, ln));
            } else if (kind == "hide-trail" || kind == "hide_trail") {
                objects.push(triggerObj("hide_trail_trigger", at, 0, ln));
            } else {
                // A typo'd kind must not vanish silently — the macros
                // dispatcher warns on unknown names; match it.
                geode::log::warn("EAS: unknown TRIGGER kind '{}' - line skipped", kind);
            }
            return;
        }
    };
    // All per-line parsing is exception-free (tryFloat/tryInt/numFromString
    // return defaults on bad input), so a malformed line degrades to default
    // values instead of needing a catch-all here.
    auto handle = [&](const std::string& raw) { handle_inner(raw); };
    // Stream through lines
    for (size_t i = 0; i <= text.size(); ++i) {
        char c = (i < text.size()) ? text[i] : '\n';
        if (c == '\n' || c == '\r') {
            if (!cur.empty()) handle(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }

    // Assemble
    if (defaultColors.size() > 0) metadata["default_colors"] = defaultColors;
    if (metaSeen)                 root["level_metadata"]      = metadata;
    root["objects"] = objects;
    root["macros"]  = macros;
    root["analysis"] = std::string("Auto-generated from EAS script.");
    r.ok = true;
    r.root = root;
    return r;
}

// Detect whether `text` is EAS or JSON by inspecting the first non-comment,
// non-whitespace line. EAS verbs are recognized; otherwise assume JSON.
inline bool looksLikeEAS(const std::string& text) {
    static const std::unordered_set<std::string> EAS_VERBS = {
        "meta","section","color","obj","spike","block","saw","orb","pad","portal",
        "floor","block-floor","block_floor","platform-run","platform_run",
        "corridor","spike-train","spike_train",
        "stair-up","stair_up","stair-down","stair_down",
        "pillar","block-wall","block_wall","block-stack","block_stack",
        "arc-orbs","arc_orbs","orb-arc","orb_arc",
        "pyramid","ceiling-spikes","ceiling_spikes","saw-gauntlet","saw_gauntlet",
        "mirror","copy","trigger","row","dual","teleport",
        "move","delete","edit",   // edit ops on existing objects
    };
    // In-place line walk — istringstream would copy the whole (potentially
    // 60 KB+) response just to inspect the first non-comment line.
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string_view line(text.data() + pos, eol - pos);
        pos = eol + 1;

        // Trim the view (same chars as eas::trim)
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        if (line.empty()) continue;
        if (line[0] == '#' || (line.size() >= 2 && line[0] == '/' && line[1] == '/')) continue;

        // First real line — check its first word
        size_t sp = line.find_first_of(" \t");
        std::string word = lower(std::string(line.substr(0, sp)));
        return EAS_VERBS.count(word) > 0;
    }
    return false;
}

// Extract the EAS section from a model response. The model emits a Plan
// (markdown) then "## Level Script\n" then the EAS. If "## Level Script" is
// present, return everything after it; otherwise return the whole text (EAS
// authors may skip the plan).
inline std::string extractScript(const std::string& text) {
    static const std::string MARK = "## Level Script";
    auto pos = text.rfind(MARK);
    if (pos == std::string::npos) return text;
    auto start = pos + MARK.size();
    while (start < text.size() && (text[start] == '\n' || text[start] == '\r')) ++start;
    return text.substr(start);
}

// ── EAS serializer (the parser's inverse) ───────────────────────────────────
// Turns a mod-format objects array back into EAS text. Three consumers:
// the Mutation Engine (current level as prompt context), .eas blueprint
// export, and recipe/debug surfaces. Deliberately simple — one line per
// object, no pattern mining — because EAS OBJ lines are already ~6-8 tokens
// and correctness beats cleverness when the output is fed back to a model.
inline std::string objectsToEAS(const matjson::Value& objectsArray,
                                float groundY = 105.f) {
    if (!objectsArray.isArray()) return {};

    // JSON trigger type → EAS TRIGGER kind. Mirrors the TRIGGER dispatcher.
    static const std::unordered_map<std::string, std::string> TRIGGER_KINDS = {
        {"color_trigger", "color"},   {"effect_color_trigger",  "color"},
        {"move_trigger", "move"},     {"effect_move_trigger",   "move"},
        {"alpha_trigger", "alpha"},   {"effect_alpha_trigger",  "alpha"},
        {"rotate_trigger", "rotate"}, {"effect_rotate_trigger", "rotate"},
        {"toggle_trigger", "toggle"}, {"effect_toggle_trigger", "toggle"},
        {"pulse_trigger", "pulse"},   {"effect_pulse_trigger",  "pulse"},
        {"spawn_trigger", "spawn"},   {"effect_spawn_trigger",  "spawn"},
        {"stop_trigger", "stop"},     {"effect_stop_trigger",   "stop"},
        {"end_trigger", "end"},       {"effect_10_level_end_trigger", "end"},
        {"effect_scale_trigger", "scale"},     {"effect_shake_trigger", "shake"},
        {"effect_zoom_camera_trigger", "zoom"},
        {"effect_static_camera_trigger", "static-cam"},
        {"effect_offset_camera_trigger", "offset-cam"},
        {"effect_timewarp_trigger", "timewarp"},
        {"effect_song_trigger", "song"},       {"effect_sfx_trigger", "sfx"},
        {"effect_follow_trigger", "follow"},
        {"effect_follow_player_y_trigger", "follow-y"},
        {"effect_touch_trigger", "touch"},     {"effect_count_trigger", "count"},
        {"effect_instant_count_trigger", "instant-count"},
        {"effect_pickup_trigger", "pickup"},
        {"effect_on_death_trigger", "on-death"},
        {"effect_animate_trigger", "animate"},
        {"effect_gravity_trigger", "gravity"},
        {"effect_teleport_trigger", "teleport"},
        {"effect_reverse_trigger", "reverse"},
        {"effect_background_effect_on_trigger", "bg-on"},
        {"effect_background_effect_off_trigger", "bg-off"},
        {"effect_no_enter_effect_trigger", "no-enter-fx"},
        {"show_player_trigger", "show-player"},
        {"hide_player_trigger", "hide-player"},
        {"show_trail_trigger", "show-trail"},
        {"hide_trail_trigger", "hide-trail"},
    };
    // JSON field → EAS keyword, shared across trigger kinds. Only fields the
    // TRIGGER dispatcher actually round-trips.
    static const std::vector<std::pair<const char*, const char*>> TRIG_FIELDS = {
        {"target_group", "target"}, {"duration", "duration"}, {"easing", "easing"},
        {"move_x", "dx"},           {"move_y", "dy"},         {"degrees", "degrees"},
        {"strength", "strength"},   {"interval", "interval"}, {"zoom", "zoom"},
        {"mod", "mod"},             {"sound_id", "sound_id"}, {"channel", "channel"},
        {"volume", "volume"},       {"pitch", "pitch"},       {"follow_group", "follow"},
        {"x_mod", "x_mod"},         {"y_mod", "y_mod"},       {"speed", "speed"},
        {"delay", "delay"},         {"offset", "offset"},     {"max_speed", "max_speed"},
        {"item_id", "item_id"},     {"count", "count"},       {"opacity", "to"},
        {"scale", "to"},
        {"fade_in", "fade_in"},     {"hold", "hold"},         {"fade_out", "fade_out"},
        {"target_color_channel", "ch"},
        {"animation_id", "anim"},   {"gravity", "g"},
        // Bool flags ride the same loop (numeric parse fails → flag emit).
        {"easing_rate", "easing_rate"},
        {"blending", "blend"},          {"exclusive", "exclusive"},
        {"lock_object_rotation", "lock_rotation"},
        {"lock_to_player_x", "lock_to_player_x"},
        {"lock_to_player_y", "lock_to_player_y"},
        {"touch_triggered", "touch"},   {"spawn_triggered", "spawn_triggered"},
        {"editor_disable", "editor_disable"},
        {"activate_group", "on"},
    };

    auto fmtNum = [](double v) -> std::string {
        double r = std::round(v);
        if (std::abs(v - r) < 0.01) return fmt::format("{}", (long long)r);
        return fmt::format("{:.1f}", v);
    };

    std::string out;
    out.reserve(objectsArray.size() * 40);
    for (size_t i = 0; i < objectsArray.size(); ++i) {
        const auto& o = objectsArray[i];
        if (!o.isObject()) continue;
        auto typeRes = o["type"].asString();
        if (!typeRes) continue;
        const std::string& type = typeRes.unwrap();
        auto xRes = o["x"].asDouble();
        double x = xRes ? xRes.unwrap() : 0.0;

        // Common suffix fields shared by both shapes
        std::string suffix;
        {
            auto col = o["color_channel"].asInt();
            if (col) suffix += fmt::format(" color={}", col.unwrap());
            if (o.contains("groups") && o["groups"].isArray() && o["groups"].size() > 0) {
                suffix += " groups=";
                const auto& gs = o["groups"];
                for (size_t g = 0; g < gs.size(); ++g) {
                    auto gi = gs[g].asInt();
                    if (!gi) continue;
                    if (g) suffix += ",";
                    suffix += fmt::format("{}", gi.unwrap());
                }
            }
            auto sc = o["scale"].asDouble();
            if (sc && std::abs(sc.unwrap() - 1.0) > 0.01)
                suffix += fmt::format(" scale={:.2f}", sc.unwrap());
            auto rot = o["rotation"].asDouble();
            if (rot && std::abs(rot.unwrap()) > 0.01)
                suffix += fmt::format(" rot={}", (int)rot.unwrap());
            auto fx = o["flip_x"].asBool();
            if (fx && fx.unwrap()) suffix += " flip_x";
            auto fy = o["flip_y"].asBool();
            if (fy && fy.unwrap()) suffix += " flip_y";
            auto zl = o["z_layer"].asInt();
            if (zl) suffix += fmt::format(" z_layer={}", zl.unwrap());
            auto zo = o["z_order"].asInt();
            if (zo) suffix += fmt::format(" z_order={}", zo.unwrap());
            auto ma = o["multi_activate"].asBool();
            if (ma && ma.unwrap()) suffix += " multi_activate";
        }

        auto trigIt = TRIGGER_KINDS.find(type);
        if (trigIt != TRIGGER_KINDS.end()) {
            // Triggers do NOT take the generic suffix: on TRIGGER lines
            // `color=` / `groups=` carry different meanings (channel / target
            // group), so each field maps explicitly.
            out += fmt::format("TRIGGER {} at={}", trigIt->second, fmtNum(x));
            // color may be a hex string (JSON origin) or an RGB array (the
            // EAS parser stores hexToRGBArray output) — handle both.
            auto hex = o["color"].asString();
            if (hex) {
                std::string h = hex.unwrap();
                if (!h.empty() && h[0] == '#') h.erase(0, 1);
                out += fmt::format(" hex={}", h);
            } else if (o.contains("color") && o["color"].isArray() &&
                       o["color"].size() >= 3) {
                const auto& carr = o["color"];
                auto rr = carr[0].asInt(); auto gg = carr[1].asInt(); auto bb = carr[2].asInt();
                out += fmt::format(" hex={:02x}{:02x}{:02x}",
                    rr ? std::clamp((int)rr.unwrap(), 0, 255) : 255,
                    gg ? std::clamp((int)gg.unwrap(), 0, 255) : 255,
                    bb ? std::clamp((int)bb.unwrap(), 0, 255) : 255);
            }
            auto trigCh = o["color_channel"].asInt();
            if (trigCh) out += fmt::format(" ch={}", trigCh.unwrap());
            for (auto& [jk, ek] : TRIG_FIELDS) {
                if (!o.contains(jk)) continue;
                auto num = o[jk].asDouble();
                if (num) { out += fmt::format(" {}={}", ek, fmtNum(num.unwrap())); continue; }
                auto b = o[jk].asBool();
                if (b && b.unwrap()) out += fmt::format(" {}", ek);
            }
            auto act = o["activate"].asBool();
            if (act && act.unwrap()) out += " activate";
            auto ma = o["multi_activate"].asBool();
            if (ma && ma.unwrap()) out += " multi_activate";
            // The trigger's OWN group membership (spawn-chain targets) uses
            // own_groups= on TRIGGER lines — groups= means the TARGET there.
            if (o.contains("groups") && o["groups"].isArray() &&
                o["groups"].size() > 0) {
                std::string gl;
                const auto& garr = o["groups"];
                for (size_t gi = 0; gi < garr.size(); ++gi) {
                    auto gv = garr[gi].asInt();
                    if (!gv) continue;
                    if (!gl.empty()) gl += ",";
                    gl += std::to_string(gv.unwrap());
                }
                if (!gl.empty()) out += fmt::format(" own_groups={}", gl);
            }
            out += '\n';
            continue;
        }

        auto yRes = o["y"].asDouble();
        double y = yRes ? yRes.unwrap() : groundY;
        // Omit Y at ground — the parser defaults to it, and this is the
        // documented token-economy convention.
        if (std::abs(y - groundY) < 0.01)
            out += fmt::format("OBJ {} {}{}\n", type, fmtNum(x), suffix);
        else
            out += fmt::format("OBJ {} {} {}{}\n", type, fmtNum(x), fmtNum(y), suffix);
    }
    return out;
}

} // namespace eas

// Scan a free-form text response and extract the LAST valid JSON object
// that looks like our EAI level shape (has "objects", "macros", or
// "level_metadata"). Used after a tool-loop final answer because some models
// emit narration alongside the JSON, or include earlier draft JSON blocks
// before the real one. Returns empty string if nothing usable is found.
static std::string extractLastEAIJsonBlock(const std::string& text) {
    // Pass 1: collect top-level balanced {...} spans with a cheap
    // string-aware brace counter. No parsing yet — the lenient parser makes
    // up to 5 string copies per attempt, so it must only run on the few
    // candidates that can actually win.
    std::vector<std::pair<size_t, size_t>> spans;  // [openPos, closePos]
    size_t i = 0;
    int unbalancedRetries = 0;
    while (i < text.size()) {
        size_t openPos = text.find('{', i);
        if (openPos == std::string::npos) break;

        int depth = 0;
        size_t closePos = std::string::npos;
        bool inString = false;
        bool escape   = false;
        for (size_t j = openPos; j < text.size(); ++j) {
            char c = text[j];
            if (escape) { escape = false; continue; }
            if (inString) {
                if (c == '\\') { escape = true; }
                else if (c == '"') { inString = false; }
                continue;
            }
            if (c == '"') { inString = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) { closePos = j; break; }
            }
        }
        if (closePos == std::string::npos) {
            // Unbalanced from THIS opener — but a later independent block can
            // still be balanced (e.g. an unclosed brace in prose followed by
            // the real JSON payload). Re-scan from the next char. Capped:
            // each retry is an O(n) scan, and the input is model-controlled.
            if (++unbalancedRetries > 64) break;
            i = openPos + 1;
            continue;
        }
        spans.emplace_back(openPos, closePos);
        unbalancedRetries = 0;  // budget is per contiguous unbalanced run —
                                // a shared budget made 65 stray '{' in prose
                                // abort the scan before the real JSON block
        i = closePos + 1;
    }

    // Pass 2: the LAST EAI-shaped block wins (else the last valid JSON
    // object). Walking the spans backwards finds it after parsing only the
    // candidates positioned after it — typically a single parse, where the
    // old forward walk lenient-parsed every block in the response.
    std::string fallback;
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        std::string candidate = text.substr(it->first, it->second - it->first + 1);
        // Lenient parser: finds JSON blocks even when the model emitted a
        // trailing comma, comment, or single-quoted string.
        auto lenient = editorai::json_lenient::parse(candidate);
        if (!lenient.ok || !lenient.value.isObject()) continue;
        bool eaiShaped = lenient.value.contains("objects")
                      || lenient.value.contains("macros")
                      || lenient.value.contains("level_metadata");
        if (eaiShaped) return candidate;
        if (fallback.empty()) fallback = std::move(candidate);
    }
    return fallback;
}

// ─── Level passability checker ───────────────────────────────────────────────
//
// Simple but thorough pathfinding: assume the player can FLY anywhere within
// the playable vertical range. A "death" is a column of X where EVERY Y row
// is blocked by a hazard or solid block — there's literally no airspace to
// fly through.
//
// "If a wall MOVES before the player reaches it, ignore it" — implemented by
// checking each blocking object's group membership against move/toggle/alpha
// triggers that fire (via X position) BEFORE the player would reach the
// object. If so, the object is treated as "moved/gone" and excluded from
// blocking checks.
//
// What blocks the player: `block_*`, `spike_*`, `hazard_*`, `*sawblade*`.
// What does NOT block: portals, orbs, pads, decor, triggers, effects, ground-
// line cosmetics. Touch orbs/pads pass through harmlessly in this model.
//
// Player flight range: Y in [15, 540]. That's roughly the playable airspace
// in standard GD. Cells are 30 units (one grid square).
namespace levelcheck {

struct DeathZone {
    float x_start;
    float x_end;
};

struct Result {
    float pass_rate = 1.f;       // 0.0 - 1.0; fraction of columns with at least one passable Y
    int   total_columns   = 0;
    int   blocked_columns = 0;
    std::vector<DeathZone> deaths;
    std::string summary;         // human-readable for AI/user feedback
};

// Object types that physically block the player. We only need substring
// matching here — anything containing these prefixes counts.
inline bool isBlockingType(const std::string& type) {
    if (type.empty()) return false;
    // Solid blocks
    if (type.rfind("block_", 0) == 0) return true;
    // Spikes (deadly + decorative; both block via collision damage / solid)
    if (type.rfind("spike_", 0) == 0 && type.find("fake") == std::string::npos)
        return true;
    // Generic hazards (pit, animated, etc.)
    if (type.rfind("hazard_", 0) == 0) return true;
    // Sawblades
    if (type.find("sawblade") != std::string::npos) return true;
    if (type.find("saw_blade") != std::string::npos) return true;
    return false;
}

// Triggers that "remove" objects (effectively) when fired. Recognise both the
// legacy short names (used by JSON output) and the `effect_*_trigger` names
// the EAS parser emits — same trigger, two name conventions.
inline bool isMovingTrigger(const std::string& type) {
    if (type == "effect_move_trigger"   || type == "move_trigger")   return true;
    if (type == "effect_alpha_trigger"  || type == "alpha_trigger")  return true;
    if (type == "effect_toggle_trigger" || type == "toggle_trigger") return true;
    if (type == "effect_stop_trigger"   || type == "stop_trigger")   return true;
    return false;
}

// Shared matjson accessors with defaults. matjson's typed accessors return
// Result<T>; these wrap them with fallbacks so call sites stay compact. Used
// by the level checker and every macro expander.
inline float getFloat(const matjson::Value& v, const std::string& key, float dflt) {
    if (!v.contains(key)) return dflt;
    auto d = v[key].asDouble();
    if (d) return (float)d.unwrap();
    auto i = v[key].asInt();
    if (i) return (float)i.unwrap();
    return dflt;
}
inline int getInt(const matjson::Value& v, const std::string& key, int dflt) {
    if (!v.contains(key)) return dflt;
    auto r = v[key].asInt();
    return r ? (int)r.unwrap() : dflt;
}
inline std::string getStr(const matjson::Value& v, const std::string& key, const char* dflt) {
    if (!v.contains(key)) return dflt;
    auto r = v[key].asString();
    return r ? r.unwrap() : std::string(dflt);
}
// True only when the key is present AND truthy — so a JSON `flip_x:false`
// does NOT trigger a flip (EAS only ever emits the key when true).
inline bool getBool(const matjson::Value& v, const std::string& key, bool dflt) {
    if (!v.contains(key)) return dflt;
    auto b = v[key].asBool();
    if (b) return b.unwrap();
    auto i = v[key].asInt();
    if (i) return i.unwrap() != 0;
    auto s = v[key].asString();
    if (s) { const auto& t = s.unwrap(); return t == "true" || t == "1"; }
    return dflt;
}

inline std::set<int> parseGroups(const matjson::Value& v) {
    std::set<int> out;
    if (!v.contains("groups")) return out;
    auto& g = v["groups"];
    if (g.isArray()) {
        for (size_t i = 0; i < g.size(); ++i) {
            auto n = g[i].asInt();
            if (n) out.insert((int)n.unwrap());
        }
    } else {
        auto n = g.asInt();
        if (n) out.insert((int)n.unwrap());
    }
    return out;
}

inline int parseSingleGroup(const matjson::Value& v, const char* key) {
    if (!v.contains(key)) return 0;
    auto n = v[key].asInt();
    return n ? (int)n.unwrap() : 0;
}

inline Result check(const matjson::Value& objectsArray) {
    Result r;
    if (!objectsArray.isArray() || objectsArray.size() == 0) {
        r.pass_rate = 1.f;
        r.summary = "Empty level (nothing to check).";
        return r;
    }

    // ── Stage 1: build trigger index — which groups get moved/hidden, by what X ──
    // group_id → earliest trigger X that affects it
    std::unordered_map<int, float> groupRemovedAtX;

    // ── Stage 2: scan objects, building (a) blocker list, (b) max X ──
    struct Obj { float x, y, half_w, half_h; std::set<int> groups; };
    std::vector<Obj> blockers;
    float maxX = 0.f;
    int skippedGarbage = 0;

    // First pass: collect triggers (so we know which groups are "moved away")
    for (size_t i = 0; i < objectsArray.size(); ++i) {
        const auto& o = objectsArray[i];
        if (!o.isObject()) continue;
        auto typeRes = o["type"].asString();
        if (!typeRes) continue;
        std::string type = typeRes.unwrap();
        if (!isMovingTrigger(type)) continue;
        // Direction matters: a toggle that turns a group ON adds blockers,
        // it doesn't remove them — treating it as removal hides real death
        // zones. Same for alpha fading TO visible (opacity > 0).
        if (type.find("toggle") != std::string::npos) {
            auto act = o["activate_group"].asBool();
            if (act && act.unwrap()) continue;
        }
        if (type.find("alpha") != std::string::npos) {
            float to = getFloat(o, "opacity", 0.f);
            if (to > 0.05f) continue;
        }
        float trigX = getFloat(o, "x", 0.f);
        int target = parseSingleGroup(o, "target_group");
        if (target == 0) {
            // legacy "groups" field on triggers
            auto gs = parseGroups(o);
            for (int g : gs) {
                auto it = groupRemovedAtX.find(g);
                if (it == groupRemovedAtX.end() || trigX < it->second)
                    groupRemovedAtX[g] = trigX;
            }
        } else {
            auto it = groupRemovedAtX.find(target);
            if (it == groupRemovedAtX.end() || trigX < it->second)
                groupRemovedAtX[target] = trigX;
        }
    }

    // Second pass: collect blockers, filtering out "moved before player reaches" ones
    for (size_t i = 0; i < objectsArray.size(); ++i) {
        const auto& o = objectsArray[i];
        if (!o.isObject()) continue;
        auto typeRes = o["type"].asString();
        if (!typeRes) continue;
        std::string type = typeRes.unwrap();
        if (!isBlockingType(type)) continue;
        // passable / no_touch objects have no collision — fake walls and
        // decorative hazards must not register as blockers.
        {
            auto pr = o["passable"].asBool();
            if (pr && pr.unwrap()) continue;
            auto nt = o["no_touch"].asBool();
            if (nt && nt.unwrap()) continue;
        }

        float x = getFloat(o, "x", 0.f);
        float y = getFloat(o, "y", 0.f);
        // Model output is untrusted: a garbage X (1e9, NaN, inf) would blow up
        // the column-bucket allocation below or hit float→int overflow UB.
        if (!std::isfinite(x) || !std::isfinite(y)
            || x < -10000.f || x > 250000.f || y < -10000.f || y > 100000.f) {
            ++skippedGarbage;
            continue;
        }
        if (x > maxX) maxX = x;

        auto groups = parseGroups(o);
        bool movedBefore = false;
        for (int g : groups) {
            auto it = groupRemovedAtX.find(g);
            if (it != groupRemovedAtX.end() && it->second < x) {
                movedBefore = true;
                break;
            }
        }
        if (movedBefore) continue;   // object is gone by the time player arrives

        float scale = getFloat(o, "scale", 1.f);
        if (!std::isfinite(scale)) scale = 1.f;
        scale = std::clamp(scale, 0.05f, 50.f);
        // Standard GD cell is 30×30; spikes are roughly the same bounding box.
        float half = 15.f * scale;
        blockers.push_back({x, y, half, half, std::move(groups)});
    }

    if (blockers.empty()) {
        r.pass_rate = 1.f;
        r.summary = "No blocking objects — level is trivially passable.";
        return r;
    }

    // Build a column-indexed spatial map for fast lookup. Each blocker spans
    // (x-half_w, x+half_w) → bucketize into 30-unit X columns.
    constexpr float COL_STEP = 30.f;
    constexpr float Y_MIN    = 15.f;   // ground sprite top
    constexpr float Y_MAX    = 540.f;  // top of standard playable airspace
    constexpr float ROW_STEP = 30.f;

    // Player detection square — 0.75 × grid cell = 22.5 units. The pathfinder
    // treats the player as this AABB (not a point) when testing collisions:
    // a cell is marked blocked iff a 22.5×22.5 box centered in it would
    // intersect the blocker. Implemented as Minkowski sum — inflate each
    // blocker's half-extent by half the player size before column/row
    // bucketing. Matches GD's actual cube hitbox more closely than a point.
    constexpr float PLAYER_DETECTION_HALF = 0.75f * 30.f * 0.5f;  // = 11.25

    int totalCols = (int)((maxX + 60.f) / COL_STEP) + 1;
    int totalRows = (int)((Y_MAX - Y_MIN) / ROW_STEP) + 1;

    // For each column, a bitmask of blocked Y rows. totalRows is 18 with the
    // constants above, so a uint32 covers the playfield — far cheaper than
    // the old per-column std::set<int> (node allocation per blocked cell).
    const uint32_t fullMask = totalRows >= 32 ? ~0u : ((1u << totalRows) - 1u);
    std::vector<uint32_t> blockedRows(totalCols, 0u);
    for (const auto& b : blockers) {
        float effHalfW = b.half_w + PLAYER_DETECTION_HALF;
        float effHalfH = b.half_h + PLAYER_DETECTION_HALF;
        int colStart = std::max(0, (int)((b.x - effHalfW) / COL_STEP));
        int colEnd   = std::min(totalCols - 1, (int)((b.x + effHalfW) / COL_STEP));
        int rowStart = std::max(0, (int)((b.y - effHalfH - Y_MIN) / ROW_STEP));
        int rowEnd   = std::min(totalRows - 1, (int)((b.y + effHalfH - Y_MIN) / ROW_STEP));
        if (rowEnd < rowStart) continue;
        uint32_t rowMask = (rowEnd - rowStart + 1 >= 32)
            ? ~0u
            : (((1u << (rowEnd - rowStart + 1)) - 1u) << rowStart);
        for (int c = colStart; c <= colEnd; ++c)
            blockedRows[c] |= rowMask;
    }

    // Walk columns; a column is "dead" if EVERY row is blocked.
    std::vector<bool> dead(totalCols, false);
    int deadCount = 0;
    for (int c = 0; c < totalCols; ++c) {
        if (blockedRows[c] == fullMask) {
            dead[c] = true;
            ++deadCount;
        }
    }

    // Cluster adjacent dead columns into zones.
    int i = 0;
    while (i < totalCols) {
        if (!dead[i]) { ++i; continue; }
        int j = i;
        while (j + 1 < totalCols && dead[j + 1]) ++j;
        r.deaths.push_back({i * COL_STEP, (j + 1) * COL_STEP});
        i = j + 1;
    }

    // ── Gamemode-aware advisories (v1) ──────────────────────────────────────
    // The dead-column check above assumes fly-anywhere, which is only true
    // for flight modes. Segment the level by gamemode portals and emit
    // WARNINGS (not death zones — these are heuristics, and false positives
    // must not trigger forced fix rounds):
    //   • cube/ball/robot/spider: all airspace above ~row 12 (Y>375) is
    //     unreachable without an orb/pad — warn if a column's only free rows
    //     are up there and no orb/pad is nearby.
    //   • ship/ufo/wave/swing: warn if the largest contiguous free gap in a
    //     column is a single row (30u) — threadable only at demon precision.
    std::string advisories;
    {
        // Gamemode segments from portal placements (start defaults to cube).
        static const std::unordered_map<std::string, bool> FLIGHT_MODE = {
            {"portal_cube_portal", false}, {"portal_ball_portal",   false},
            {"portal_robot_portal", false}, {"portal_spider_portal", false},
            {"portal_ship_portal", true},  {"portal_ufo_portal",    true},
            {"portal_wave_portal", true},  {"portal_swing_portal",  true},
        };
        std::vector<std::pair<float, bool>> modeChanges;  // x → isFlight
        std::vector<float> boostXs;                       // orbs/pads (cube reach extenders)
        for (size_t oi = 0; oi < objectsArray.size(); ++oi) {
            const auto& o = objectsArray[oi];
            if (!o.isObject()) continue;
            auto typeRes = o["type"].asString();
            if (!typeRes) continue;
            const std::string& type = typeRes.unwrap();
            float ox = getFloat(o, "x", 0.f);
            if (!std::isfinite(ox) || ox < 0.f || ox > 250000.f) continue;
            auto it = FLIGHT_MODE.find(type);
            if (it != FLIGHT_MODE.end()) modeChanges.emplace_back(ox, it->second);
            // Catalog orb/pad names: jump_orb_*, jump_pad_*, obj_*_orb,
            // obj_*_pad — substring match covers all spellings; a stray
            // decorative "_orb" only suppresses a warning (safe direction).
            else if (type.find("_orb") != std::string::npos ||
                     type.find("_pad") != std::string::npos)
                boostXs.push_back(ox);
        }
        std::sort(modeChanges.begin(), modeChanges.end());
        std::sort(boostXs.begin(), boostXs.end());

        auto isFlightAt = [&](float x) {
            bool flight = false;  // levels start in cube
            for (auto& [px, f] : modeChanges) { if (px <= x) flight = f; else break; }
            return flight;
        };
        auto boostNear = [&](float x) {
            auto lo = std::lower_bound(boostXs.begin(), boostXs.end(), x - 120.f);
            return lo != boostXs.end() && *lo <= x + 120.f;
        };

        // Cube reach ceiling: row 12 ≈ Y 375 (generous — includes orb chains).
        const int CUBE_MAX_ROW = (int)((375.f - Y_MIN) / ROW_STEP);
        const uint32_t cubeBand = (CUBE_MAX_ROW + 1 >= 32)
            ? ~0u : ((1u << (CUBE_MAX_ROW + 1)) - 1u);

        int cubeHighOnly = 0, flightTight = 0;
        float firstCubeX = -1.f, firstTightX = -1.f;
        for (int c = 0; c < totalCols; ++c) {
            if (dead[c] || blockedRows[c] == 0) continue;
            float colX = c * COL_STEP;
            if (!isFlightAt(colX)) {
                // Free somewhere, but the reachable cube band is fully blocked?
                if ((blockedRows[c] & cubeBand) == cubeBand && !boostNear(colX)) {
                    ++cubeHighOnly;
                    if (firstCubeX < 0) firstCubeX = colX;
                }
            } else {
                // Largest contiguous run of free rows in this column.
                uint32_t freeMask = ~blockedRows[c] & fullMask;
                int best = 0, run = 0;
                for (int rrow = 0; rrow < totalRows; ++rrow) {
                    if (freeMask & (1u << rrow)) { if (++run > best) best = run; }
                    else run = 0;
                }
                if (best == 1) {
                    ++flightTight;
                    if (firstTightX < 0) firstTightX = colX;
                }
            }
        }
        if (cubeHighOnly > 0)
            advisories += fmt::format(
                " WARNING: {} column(s) (first at X={:.0f}) are in a CUBE-family "
                "segment but only have free space above Y=375 with no orb/pad "
                "nearby — the player likely cannot reach it.",
                cubeHighOnly, firstCubeX);
        if (flightTight > 0)
            advisories += fmt::format(
                " WARNING: {} column(s) (first at X={:.0f}) in a flight segment "
                "have only a 1-row (30u) gap — extremely tight; widen to 60u+ "
                "unless aiming for demon difficulty.",
                flightTight, firstTightX);
    }

    r.total_columns   = totalCols;
    r.blocked_columns = deadCount;
    r.pass_rate       = totalCols == 0 ? 1.f
                       : 1.f - (float)deadCount / (float)totalCols;

    if (r.deaths.empty()) {
        r.summary = fmt::format("Level is 100% passable across {} columns.", totalCols);
    } else {
        std::string zones;
        for (size_t k = 0; k < r.deaths.size() && k < 8; ++k) {
            if (k) zones += ", ";
            zones += fmt::format("X={:.0f}-{:.0f}",
                                 r.deaths[k].x_start, r.deaths[k].x_end);
        }
        if (r.deaths.size() > 8)
            zones += fmt::format(" (+{} more)", r.deaths.size() - 8);
        r.summary = fmt::format(
            "Level is {:.1f}% passable ({} dead columns out of {}). Death zones: {}",
            r.pass_rate * 100.f, deadCount, totalCols, zones);
    }
    if (skippedGarbage > 0)
        r.summary += fmt::format(
            " ({} objects with out-of-range coordinates were ignored)", skippedGarbage);
    r.summary += advisories;
    return r;
}

// ── Greedy-bot cube simulator ───────────────────────────────────────────────
// Traces an actual cube trajectory through the draft: gravity, jump arcs, a
// greedy "jump when a hazard/wall approaches" policy, AABB death checks.
// Catches the classic AI failure — a spike at exactly un-jumpable distance —
// that column-occupancy checks can't see. Constants are community-measured
// GD values at 1x; this is an ADVISORY sim, not a frame-perfect replica.
struct SimDeath { float x, y; std::string reason; };
struct SimResult {
    std::vector<cocos2d::CCPoint> path;   // sampled trajectory for the overlay
    std::vector<SimDeath> deaths;
    float reachedX = 0.f;
    bool  finished = false;
};

inline SimResult simulateCube(const matjson::Value& objectsArray, float groundY) {
    SimResult res;
    if (!objectsArray.isArray() || objectsArray.size() == 0) return res;

    struct Box { float x, y, halfW, halfH; };
    std::vector<Box> solids, hazards;
    // v2: gamemode segments, speed portals, and jump orbs/pads. The bot is
    // mode-aware — cube/robot run the jump simulation; flight modes run a
    // corridor-gap check (a cube model in a ship tunnel reports nonsense).
    enum class SimMode { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };
    struct ModeChange  { float x; SimMode m; };
    struct SpeedChange { float x; float mult; };
    struct Booster     { float x, y, impulse; bool isPad; };
    std::vector<ModeChange>  modes;
    std::vector<SpeedChange> speeds;
    std::vector<Booster>     boosters;
    float maxX = 0.f;
    for (size_t i = 0; i < objectsArray.size(); ++i) {
        const auto& o = objectsArray[i];
        if (!o.isObject()) continue;
        auto typeRes = o["type"].asString();
        if (!typeRes) continue;
        const std::string& t = typeRes.unwrap();
        float x = getFloat(o, "x", 0.f);
        float y = getFloat(o, "y", 0.f);
        if (!std::isfinite(x) || !std::isfinite(y) ||
            x < 0.f || x > 250000.f || y < -1000.f || y > 5000.f) continue;
        float scale = getFloat(o, "scale", 1.f);
        if (!std::isfinite(scale)) scale = 1.f;
        scale = std::clamp(scale, 0.05f, 50.f);
        // Gamemode portals.
        if      (t == "portal_cube_portal")   { modes.push_back({x, SimMode::Cube});   continue; }
        else if (t == "portal_ship_portal")   { modes.push_back({x, SimMode::Ship});   continue; }
        else if (t == "portal_ball_portal")   { modes.push_back({x, SimMode::Ball});   continue; }
        else if (t == "portal_ufo_portal")    { modes.push_back({x, SimMode::Ufo});    continue; }
        else if (t == "portal_wave_portal")   { modes.push_back({x, SimMode::Wave});   continue; }
        else if (t == "portal_robot_portal")  { modes.push_back({x, SimMode::Robot});  continue; }
        else if (t == "portal_spider_portal") { modes.push_back({x, SimMode::Spider}); continue; }
        else if (t == "portal_swing_portal")  { modes.push_back({x, SimMode::Swing});  continue; }
        // Speed portals (community-measured ratios vs 311.58 u/s).
        else if (t == "portal_yellow_slow_speed_portal") { speeds.push_back({x, 0.8061f}); continue; }
        else if (t == "portal_blue_normal_speed_portal") { speeds.push_back({x, 1.0f});    continue; }
        else if (t == "portal_green_fast_speed_portal")  { speeds.push_back({x, 1.2434f}); continue; }
        else if (t == "portal_pink_fast_speed_portal")   { speeds.push_back({x, 1.5020f}); continue; }
        else if (t == "portal_red_fast_speed_portal")    { speeds.push_back({x, 1.8486f}); continue; }
        // Jump orbs / pads (gravity & dash variants excluded — the bot
        // can't reason about them; better blind than wrong).
        else if (t.rfind("jump_orb_", 0) == 0 || t.rfind("jump_pad_", 0) == 0) {
            float imp = t.find("red") != std::string::npos    ? 1.35f
                      : t.find("pink") != std::string::npos   ? 0.75f
                                                              : 1.0f;
            boosters.push_back({x, y, imp, t.rfind("jump_pad_", 0) == 0});
            continue;
        }
        bool hazard = t.rfind("spike_", 0) == 0 || t.rfind("hazard_", 0) == 0 ||
                      t.find("_saw") != std::string::npos || t.find("saw_") != std::string::npos;
        bool solid  = !hazard && isBlockingType(t);
        if (!hazard && !solid) continue;
        // Flagged-off collision: no_touch hazards can't kill, passable
        // solids can't block — skip both so the bot sees the real level.
        if (hazard) {
            auto nt = o["no_touch"].asBool();
            if (nt && nt.unwrap()) continue;
        }
        if (solid) {
            auto pr = o["passable"].asBool();
            if (pr && pr.unwrap()) continue;
        }
        // Hazard hitboxes in GD are forgiving (~40-60% of the sprite).
        float half = 15.f * scale * (hazard ? 0.55f : 1.f);
        (hazard ? hazards : solids).push_back({x, y, half, half});
        if (x > maxX) maxX = x;
    }
    if (maxX <= 0.f) return res;
    auto byX = [](const Box& a, const Box& b) { return a.x < b.x; };
    std::sort(solids.begin(),  solids.end(),  byX);
    std::sort(hazards.begin(), hazards.end(), byX);
    std::sort(modes.begin(),  modes.end(),
              [](const ModeChange& a,  const ModeChange& b)  { return a.x < b.x; });
    std::sort(speeds.begin(), speeds.end(),
              [](const SpeedChange& a, const SpeedChange& b) { return a.x < b.x; });
    std::sort(boosters.begin(), boosters.end(),
              [](const Booster& a, const Booster& b) { return a.x < b.x; });

    // 1x physics. Jump: v0≈603.7 u/s, g≈2794 u/s² → apex ≈ 65u, length ≈ 134u.
    constexpr float VX = 311.58f, V_JUMP = 603.72f, GRAV = 2794.11f;
    constexpr float DT = 1.f / 60.f;
    constexpr float HALF = 15.f;                  // player cube half-extent
    const float floorTop = groundY + 15.f;        // top surface of the ground row

    float x = 0.f, y = floorTop + HALF, vy = 0.f;
    bool grounded = true;
    size_t sWin = 0, hWin = 0;                    // two-pointer sweep windows
    int sinceSample = 0;
    SimMode mode = SimMode::Cube;
    float   vxMult = 1.f;
    size_t  mWin = 0, spWin = 0, bWin = 0;
    float   lastCorridorX = -1e9f;
    auto isCorridorMode = [](SimMode m) {
        return m == SimMode::Ship || m == SimMode::Ufo || m == SimMode::Wave ||
               m == SimMode::Ball || m == SimMode::Spider || m == SimMode::Swing;
    };
    auto modeName = [](SimMode m) -> const char* {
        switch (m) {
            case SimMode::Ship:   return "ship";
            case SimMode::Ufo:    return "ufo";
            case SimMode::Wave:   return "wave";
            case SimMode::Ball:   return "ball";
            case SimMode::Spider: return "spider";
            case SimMode::Swing:  return "swing";
            case SimMode::Robot:  return "robot";
            default:              return "cube";
        }
    };

    // Standing surface under the player: ground, or the highest block top
    // whose span contains px.
    auto surfaceAt = [&](float px, float py) {
        float best = floorTop;
        for (size_t i = sWin; i < solids.size() && solids[i].x - solids[i].halfW <= px + HALF; ++i) {
            const auto& b = solids[i];
            if (px + HALF < b.x - b.halfW || px - HALF > b.x + b.halfW) continue;
            float top = b.y + b.halfH;
            if (top <= py - HALF + 6.f && top > best) best = top;
        }
        return best;
    };

    res.path.reserve(2048);
    float lastPadX = -1e9f;        // a pad fires once per pass, not per tick
    for (int tick = 0; tick < 12000; ++tick) {
        // Mode / speed segment advancement. The multiplier in effect BEFORE
        // crossing a portal moves this tick — GD applies the new speed from
        // the next frame, and matching that avoids stepping over a spike
        // sitting right on the portal.
        float stepMult = vxMult;
        while (mWin < modes.size() && modes[mWin].x <= x) {
            SimMode prev = mode;
            mode = modes[mWin++].m;
            if (isCorridorMode(mode) != isCorridorMode(prev))
                lastCorridorX = -1e9f;
            if (!isCorridorMode(mode)) {
                vy = 0.f;
                if (isCorridorMode(prev)) {
                    // Leaving flight: snap to the standing surface instead
                    // of free-falling from corridor-center height (a ceiling
                    // at that height would false-flag a wall slam).
                    y = surfaceAt(x, y) + HALF;
                    grounded = true;
                } else {
                    grounded = false;
                }
            }
        }
        while (spWin < speeds.size() && speeds[spWin].x <= x)
            vxMult = speeds[spWin++].mult;
        while (bWin < boosters.size() && boosters[bWin].x < x - 100.f) ++bWin;

        x += VX * stepMult * DT;
        if (x > maxX + 200.f) { res.finished = true; break; }
        // Advance sweep windows past geometry far behind the player.
        while (sWin < solids.size()  && solids[sWin].x  + solids[sWin].halfW  < x - 200.f) ++sWin;
        while (hWin < hazards.size() && hazards[hWin].x + hazards[hWin].halfW < x - 200.f) ++hWin;

        // ── Flight/corridor modes: vertical freedom, so the question is
        // only "does an adequately tall free gap exist at this X?" ──────────
        if (isCorridorMode(mode)) {
            if (x - lastCorridorX >= 15.f) {
                lastCorridorX = x;
                // Occupied vertical intervals at this column (solids AND
                // hazards both block a flight path).
                std::vector<std::pair<float, float>> occ;
                for (size_t i = sWin; i < solids.size() &&
                     solids[i].x - solids[i].halfW <= x + HALF; ++i) {
                    const auto& b = solids[i];
                    if (x + HALF < b.x - b.halfW || x - HALF > b.x + b.halfW) continue;
                    occ.push_back({b.y - b.halfH, b.y + b.halfH});
                }
                for (size_t i = hWin; i < hazards.size() &&
                     hazards[i].x - hazards[i].halfW <= x + HALF; ++i) {
                    const auto& h = hazards[i];
                    if (x + HALF < h.x - h.halfW || x - HALF > h.x + h.halfW) continue;
                    occ.push_back({h.y - h.halfH, h.y + h.halfH});
                }
                std::sort(occ.begin(), occ.end());
                float lo = floorTop, bestGap = 0.f, bestLo = floorTop;
                constexpr float CEIL = 540.f;
                for (auto& iv : occ) {
                    if (iv.first > lo) {
                        float gap = std::min(iv.first, CEIL) - lo;
                        if (gap > bestGap) { bestGap = gap; bestLo = lo; }
                    }
                    lo = std::max(lo, iv.second);
                    if (lo >= CEIL) break;
                }
                if (CEIL > lo && CEIL - lo > bestGap) {
                    bestGap = CEIL - lo;
                    bestLo  = lo;
                }
                // Below 40u even a wave can't thread it. (60u+ is comfy;
                // 40-60 is tight-but-fair, not flagged.)
                if (bestGap < 40.f) {
                    res.deaths.push_back({x, y,
                        fmt::format("corridor too tight for {} ({}u free)",
                                    modeName(mode), (int)bestGap)});
                    if (res.deaths.size() >= 8) break;
                    x += 60.f;
                    sinceSample = 0;     // settle before sampling again
                    while (sWin < solids.size()  &&
                           solids[sWin].x  + solids[sWin].halfW  < x - 200.f) ++sWin;
                    while (hWin < hazards.size() &&
                           hazards[hWin].x + hazards[hWin].halfW < x - 200.f) ++hWin;
                } else {
                    y = bestLo + bestGap * 0.5f;   // fly the widest gap
                }
            }
            if (++sinceSample >= 3) {
                sinceSample = 0;
                res.path.push_back({x, y});
            }
            res.reachedX = x;
            continue;
        }

        // ── Cube / robot: jump physics ──────────────────────────────────────
        // Pads fire on contact, no input needed — but only ONCE per pass
        // (the contact window spans several ticks; re-firing every tick
        // would relaunch mid-arc and double the apex).
        for (size_t i = bWin; i < boosters.size() && boosters[i].x <= x + 20.f; ++i) {
            const auto& bo = boosters[i];
            if (bo.isPad && bo.x != lastPadX &&
                std::abs(bo.x - x) < 20.f && std::abs(bo.y - y) < 30.f) {
                vy = V_JUMP * bo.impulse * 1.15f;  // pads kick harder than taps
                grounded = false;
                lastPadX = bo.x;
                break;
            }
        }
        if (grounded) {
            // Greedy jump policy: hazard in the next ~90u at foot level, or a
            // wall (solid whose top is above standable step) in the next 50u.
            bool wantJump = false;
            for (size_t i = hWin; i < hazards.size() && hazards[i].x < x + 95.f; ++i) {
                const auto& h = hazards[i];
                if (h.x > x + 20.f && h.y - h.halfH < y + 25.f && h.y + h.halfH > y - HALF - 25.f) {
                    wantJump = true; break;
                }
            }
            if (!wantJump) {
                for (size_t i = sWin; i < solids.size() && solids[i].x < x + 55.f; ++i) {
                    const auto& b = solids[i];
                    if (b.x > x + HALF && b.y + b.halfH > y - HALF + 6.f &&
                        b.y - b.halfH < y + HALF) { wantJump = true; break; }
                }
            }
            if (wantJump) { vy = V_JUMP; grounded = false; }
        }
        if (!grounded) {
            vy -= GRAV * DT;
            y += vy * DT;
            // surfaceAt(x, y): with py=y the filter accepts only tops at or
            // below the player's center — passing y+30 let the bot snap UP
            // onto blocks above itself, sailing over real death zones.
            // Computed once; the orb scan below reuses it.
            float surf = surfaceAt(x, y);
            // Mid-air orb: falling toward trouble? A jump orb within reach
            // resets the jump — exactly how players chain orbs over pits.
            if (vy < 0.f) {
                for (size_t i = bWin; i < boosters.size() &&
                     boosters[i].x <= x + 30.f; ++i) {
                    const auto& bo = boosters[i];
                    if (bo.isPad) continue;
                    if (std::abs(bo.x - x) < 28.f && std::abs(bo.y - y) < 45.f) {
                        bool danger = surf <= floorTop + 1.f;  // pit below
                        for (size_t k = hWin; !danger && k < hazards.size() &&
                             hazards[k].x < x + 100.f; ++k)
                            if (hazards[k].x > x - 20.f &&
                                std::abs(hazards[k].y - y) < 90.f) danger = true;
                        if (danger) { vy = V_JUMP * bo.impulse; break; }
                    }
                }
            }
            if (vy <= 0.f && y - HALF <= surf) {
                y = surf + HALF; vy = 0.f; grounded = true;
                lastPadX = -1e9f;          // pads re-arm on landing
            }
        } else {
            float surf = surfaceAt(x, y);
            if (surf < y - HALF - 1.f) { grounded = false; vy = 0.f; }  // walked off an edge
            else y = surf + HALF;
        }

        // Wall slam: grounded into a solid's face.
        for (size_t i = sWin; i < solids.size() && solids[i].x - solids[i].halfW <= x + HALF; ++i) {
            const auto& b = solids[i];
            if (x + HALF > b.x - b.halfW && x - HALF < b.x + b.halfW &&
                y + HALF - 6.f > b.y - b.halfH && y - HALF + 6.f < b.y + b.halfH) {
                res.deaths.push_back({x, y, "ran into a wall"});
                goto died;
            }
        }
        // Hazard intersect.
        for (size_t i = hWin; i < hazards.size() && hazards[i].x - hazards[i].halfW <= x + HALF; ++i) {
            const auto& h = hazards[i];
            if (x + HALF * 0.7f > h.x - h.halfW && x - HALF * 0.7f < h.x + h.halfW &&
                y + HALF * 0.7f > h.y - h.halfH && y - HALF * 0.7f < h.y + h.halfH) {
                res.deaths.push_back({x, y, "hit a hazard"});
                goto died;
            }
        }
        goto survived;
    died:
        if (res.deaths.size() >= 8) break;
        x += 60.f;                       // respawn-skip past the killer
        // Catch the sweep windows up to the teleported x before surfaceAt.
        while (sWin < solids.size()  && solids[sWin].x  + solids[sWin].halfW  < x - 200.f) ++sWin;
        while (hWin < hazards.size() && hazards[hWin].x + hazards[hWin].halfW < x - 200.f) ++hWin;
        y = surfaceAt(x, floorTop + 90.f) + HALF;
        vy = 0.f;
        grounded = true;
        sinceSample = 0;                 // settle before sampling again
        res.reachedX = x;
        continue;                        // no path point on the death tick
    survived:
        if (++sinceSample >= 3) {
            sinceSample = 0;
            res.path.push_back({x, y});
        }
        res.reachedX = x;
    }
    return res;
}

// ── Difficulty-curve histogram ──────────────────────────────────────────────
// Bins hazard density into fixed X windows so the AI can SEE its own pacing.
// Pure array math over the draft — no allocation surprises, same coordinate
// sanitation as check().
struct CurveWindow { float x0, x1, density; };
inline std::vector<CurveWindow> difficultyHistogram(const matjson::Value& objectsArray,
                                                    float windowWidth = 1200.f) {
    std::vector<CurveWindow> out;
    if (!objectsArray.isArray() || objectsArray.size() == 0) return out;
    float maxX = 0.f;
    std::vector<float> hazardXs;
    hazardXs.reserve(objectsArray.size() / 4);
    for (size_t i = 0; i < objectsArray.size(); ++i) {
        const auto& o = objectsArray[i];
        if (!o.isObject()) continue;
        auto typeRes = o["type"].asString();
        if (!typeRes) continue;
        const std::string& t = typeRes.unwrap();
        float x = getFloat(o, "x", 0.f);
        if (!std::isfinite(x) || x < 0.f || x > 250000.f) continue;
        bool hazard = t.rfind("spike_", 0) == 0 || t.rfind("hazard_", 0) == 0 ||
                      t.find("_saw") != std::string::npos ||
                      t.find("saw_") != std::string::npos;
        if (hazard) hazardXs.push_back(x);
        if (isBlockingType(t) && x > maxX) maxX = x;
    }
    if (maxX <= 0.f) return out;
    int windows = std::clamp((int)(maxX / windowWidth) + 1, 1, 64);
    std::vector<int> counts(windows, 0);
    for (float x : hazardXs) {
        int w = (int)(x / windowWidth);
        if (w >= 0 && w < windows) ++counts[w];
    }
    out.reserve(windows);
    for (int w = 0; w < windows; ++w)
        out.push_back({w * windowWidth, (w + 1) * windowWidth,
                       counts[w] / windowWidth * 1000.f});
    return out;
}

} // namespace levelcheck

// Walk an EAI objects array and return the maximum X. Filters out triggers
// (which can sit at very large X without affecting gameplay length).
static float computeMaxXFromObjects(const matjson::Value& objectsArray) {
    if (!objectsArray.isArray()) return 0.f;
    static const std::unordered_set<std::string> triggerTypes = {
        "color_trigger","move_trigger","pulse_trigger","alpha_trigger",
        "toggle_trigger","spawn_trigger","stop_trigger","rotate_trigger",
        "end_trigger","show_trail_trigger","hide_trail_trigger",
        "show_player_trigger","hide_player_trigger","collision_trigger",
        "on_death_trigger","count_trigger",
        // EAS triggers go through `effect_*_trigger` aliases
        "effect_color_trigger","effect_move_trigger","effect_pulse_trigger",
        "effect_alpha_trigger","effect_toggle_trigger","effect_spawn_trigger",
        "effect_stop_trigger","effect_rotate_trigger","effect_scale_trigger",
        "effect_shake_trigger","effect_10_level_end_trigger",
        // 2.2 trigger surface (camera / time / audio / interactivity)
        "effect_zoom_camera_trigger","effect_static_camera_trigger",
        "effect_offset_camera_trigger","effect_rotate_camera_trigger",
        "effect_edge_camera_trigger","effect_timewarp_trigger",
        "effect_song_trigger","effect_sfx_trigger",
        "effect_follow_trigger","effect_follow_player_y_trigger",
        "effect_touch_trigger","effect_count_trigger",
        "effect_instant_count_trigger","effect_pickup_trigger",
        "effect_on_death_trigger",
    };
    float maxX = 0.f;
    for (size_t i = 0; i < objectsArray.size(); ++i) {
        const auto& o = objectsArray[i];
        if (!o.isObject()) continue;
        auto xRes = o["x"].asDouble();
        if (!xRes) continue;
        float x = (float)xRes.unwrap();
        auto typeRes = o["type"].asString();
        if (typeRes) {
            const auto& type = typeRes.unwrap();
            // Skip triggers — their X is just where they fire.
            if (triggerTypes.count(type)) continue;
            // Skip pure-decoration objects (decor_*, parallax_*) — they can
            // sit far past the gameplay end purely for visual atmosphere and
            // shouldn't be counted as gameplay length.
            if (type.rfind("decor_", 0)    == 0) continue;
            if (type.rfind("parallax_", 0) == 0) continue;
            if (type == "effect_pulsing_music_note") continue;
            // Skip standalone visual effects (particle objects, etc.)
            if (type.rfind("particle_", 0) == 0) continue;
        }
        if (x > maxX) maxX = x;
    }
    return maxX;
}

// ─── Macros (SPWN/G.js-inspired pattern shortcuts) ──────────────────────────
// Macros let the AI emit ONE compact JSON entry that expands to many real
// object entries. They exist to (1) cut tokens dramatically when generating
// repetitive structures (spike walls, stair patterns, color cycles) and (2)
// guarantee that common patterns are placed with correct math (spacing,
// alignment, trigger sequencing) regardless of how dumb the model is.
//
// Each macro is identified by a "name" field on the macro object. Other
// fields are macro-specific parameters; missing optional fields fall back
// to sensible defaults. The expander returns plain object dicts that get
// merged into the regular `objects` array and processed by prepareObjects.
//
// Safety caps: every macro caps its output (typically at 200 objects) so a
// hallucinated count=999999 can't OOM the editor.

namespace macros {
    // Shared matjson accessors — single definitions live in levelcheck above.
    using levelcheck::getInt;
    using levelcheck::getFloat;
    using levelcheck::getStr;

    // Look up an object name in OBJECT_IDS; if missing, fall back to the
    // provided default so the macro still produces something.
    static std::string resolveType(const std::string& wanted, const std::string& fallback) {
        if (OBJECT_IDS.find(wanted) != OBJECT_IDS.end()) return wanted;
        return fallback;
    }

    static matjson::Value makeObj(const std::string& type, float x, float y) {
        auto o = matjson::Value::object();
        o["type"] = type;
        o["x"] = (double)x;
        o["y"] = (double)y;
        return o;
    }
    static matjson::Value makeObj(const std::string& type, float x, float y, float scale) {
        auto o = makeObj(type, x, y);
        if (scale != 1.0f) o["scale"] = (double)scale;
        return o;
    }

    // Macros call this on every object they emit so per-object fields the AI
    // attached to the macro line (color_channel, groups, scale, rotation,
    // etc.) propagate to every block / spike / orb the macro produces.
    //
    // Per-object x/y are NEVER copied (those come from the macro's own math).
    // type isn't copied either (each macro chooses its own type per-step).
    //
    // The keys mirror what EAS's applyCommonFields attaches to single objects
    // — so a `FLOOR 0..900 color=4 scale=1.2` line and a row of individual
    // `BLOCK x y color=4 scale=1.2` lines produce identical objects.
    static void applyMacroPassthroughs(matjson::Value& obj, const matjson::Value& p) {
        static const std::vector<const char*> COPY_KEYS = {
            "color_channel", "detail_color_channel",
            "groups",
            "scale", "rotation",
            "z_layer", "z_order",
            "editor_layer", "editor_layer_2",
            "multi_activate",
            "flip_x", "flip_y",
            "main_color", "detail_color",
            "copy_color_channel", "copy_color_hsv",
            // 2.2 editor flags — `FLOOR 0..900 passable` must reach every
            // child block the macro emits.
            "passable", "no_touch", "hide", "no_glow",
            "dont_fade", "dont_enter", "high_detail", "no_effects",
        };
        if (!p.isObject()) return;
        for (auto k : COPY_KEYS) {
            if (!p.contains(k)) continue;
            // Per-object fields the macro set on its own (rare) take priority.
            if (obj.contains(k)) continue;
            obj[k] = p[k];
        }
    }
    static void applyMacroPassthroughs(std::vector<matjson::Value>& out,
                                       const matjson::Value& p) {
        for (auto& o : out) applyMacroPassthroughs(o, p);
    }

    // ── pyramid ─────────────────────────────────────────────────────────────
    //   x (center), base? (blocks across, default 5), y? (base-row center Y,
    //   default ground), block_type?
    // Centered block pyramid: each row two blocks narrower than the one
    // below. Capped so a hallucinated base can't OOM the editor.
    static std::vector<matjson::Value> pyramid(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        int   base = std::clamp(getInt(p, "base", 5), 1, 21);
        float cx   = getFloat(p, "x", 0.f);
        float y    = getFloat(p, "y", getGroundY());
        std::string type = resolveType(getStr(p, "block_type", "block_black_gradient_square"),
                                       "block_black_gradient_square");
        for (int row = 0; base - 2 * row >= 1 && (int)out.size() < 200; ++row) {
            int   width = base - 2 * row;
            float left  = cx - (width - 1) * 15.f;  // 30-unit cells, centered
            for (int i = 0; i < width && (int)out.size() < 200; ++i)
                out.push_back(makeObj(type, left + i * 30.f, y + row * 30.f));
        }
        return out;
    }

    // ── ceiling_spikes ──────────────────────────────────────────────────────
    //   x_start, x_end, y? (375 — a high corridor ceiling), spacing? (30),
    //   spike_type?
    // Downward-pointing spike row (rotation 180) hanging from a ceiling.
    static std::vector<matjson::Value> ceiling_spikes(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x0 = getFloat(p, "x_start", 0.f);
        float x1 = getFloat(p, "x_end", x0 + 120.f);
        if (x1 < x0) std::swap(x0, x1);
        float y       = getFloat(p, "y", 375.f);
        float spacing = std::max(15.f, getFloat(p, "spacing", 30.f));
        std::string type = resolveType(getStr(p, "spike_type", "spike_black_gradient_spike"),
                                       "spike_black_gradient_spike");
        for (float x = x0; x <= x1 + 0.1f && (int)out.size() < 200; x += spacing) {
            auto o = makeObj(type, x, y);
            o["rotation"] = 180.0;
            out.push_back(std::move(o));
        }
        return out;
    }

    // ── saw_gauntlet ────────────────────────────────────────────────────────
    //   x_start, x_end, y? (ground+60), spacing? (120), size?
    //   (small/medium/large), weave? (alternate saws ±weave units in Y)
    // Evenly spaced sawblades across a range; weave staggers heights so the
    // player threads between them.
    static std::vector<matjson::Value> saw_gauntlet(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x0 = getFloat(p, "x_start", 0.f);
        float x1 = getFloat(p, "x_end", x0 + 360.f);
        if (x1 < x0) std::swap(x0, x1);
        float y       = getFloat(p, "y", getGroundY() + 60.f);
        float spacing = std::max(45.f, getFloat(p, "spacing", 120.f));
        float weave   = getFloat(p, "weave", 0.f);
        std::string type = eas::sawVariant(getStr(p, "size", "small"));
        int i = 0;
        for (float x = x0; x <= x1 + 0.1f && (int)out.size() < 200; x += spacing, ++i)
            out.push_back(makeObj(type, x, y + ((i % 2) ? -weave : weave)));
        return out;
    }

    // ── dual_section ────────────────────────────────────────────────────────
    //   x_start, x_end, floor? (ground), gap? (240), block_type?
    // A well-formed dual-mode scaffold: entry dual portal, mirrored
    // floor+ceiling corridor (the top lane is the gravity-flipped twin), and
    // an exit dual portal. Getting this trio right by hand is exactly what
    // models get wrong — the macro guarantees the portals bracket the lane.
    static std::vector<matjson::Value> dual_section(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x0 = getFloat(p, "x_start", 0.f);
        float x1 = getFloat(p, "x_end", x0 + 600.f);
        if (x1 < x0) std::swap(x0, x1);
        float floorY = getFloat(p, "floor", getGroundY());
        float gap    = std::clamp(getFloat(p, "gap", 240.f), 120.f, 450.f);
        float ceilY  = floorY + gap;
        float midY   = floorY + gap * 0.5f;
        std::string type = resolveType(getStr(p, "block_type", "block_black_gradient_square"),
                                       "block_black_gradient_square");
        out.push_back(makeObj("portal_dual_portal",      x0 - 45.f, midY));
        out.push_back(makeObj("portal_exit_dual_portal", x1 + 45.f, midY));
        for (float x = x0; x <= x1 + 0.1f && (int)out.size() < 400; x += 30.f) {
            out.push_back(makeObj(type, x, floorY));
            out.push_back(makeObj(type, x, ceilY));
        }
        return out;
    }

    // ── teleport_pair ───────────────────────────────────────────────────────
    //   x, y? (entry portal Y), y_offset? (vertical jump, default +150)
    // The classic linked teleport (one object carries both ends). The offset
    // rides along as teleport_y_offset and is applied to the
    // TeleportPortalObject in applyObjectProperties.
    static std::vector<matjson::Value> teleport_pair(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x    = getFloat(p, "x", 0.f);
        float y    = getFloat(p, "y", getGroundY() + 60.f);
        float yOff = std::clamp(getFloat(p, "y_offset", 150.f), -450.f, 450.f);
        auto o = makeObj("portal_linked_teleport_portals", x, y);
        o["teleport_y_offset"] = (double)yOff;
        out.push_back(std::move(o));
        return out;
    }

    // ── spike_train ─────────────────────────────────────────────────────────
    //   x, y? (ground), count, spacing? (30), spike_type? (basic spike)
    // Expands to `count` spikes in a horizontal row.
    static std::vector<matjson::Value> spike_train(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        int   count   = std::clamp(getInt(p, "count", 5), 1, 200);
        float x       = getFloat(p, "x", 0.f);
        float y       = getFloat(p, "y", getGroundY());
        float spacing = std::max(15.f, getFloat(p, "spacing", 30.f));
        std::string type = resolveType(getStr(p, "spike_type", "spike_black_gradient_spike"),
                                       "spike_black_gradient_spike");
        for (int i = 0; i < count; ++i)
            out.push_back(makeObj(type, x + i * spacing, y));
        return out;
    }

    // ── stair_up ────────────────────────────────────────────────────────────
    //   x, y? (ground), steps, step_width? (30), step_height? (30), block_type?
    // Ascending stairway. Each step is one block; step N is at
    //   x + N*step_width, y + N*step_height.
    static std::vector<matjson::Value> stair_up(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        int   steps      = std::clamp(getInt(p, "steps", 4), 1, 50);
        float x          = getFloat(p, "x", 0.f);
        float y          = getFloat(p, "y", getGroundY());
        float stepW      = std::max(15.f, getFloat(p, "step_width", 30.f));
        float stepH      = std::max(15.f, getFloat(p, "step_height", 30.f));
        std::string type = resolveType(getStr(p, "block_type", "block_black_gradient_square"),
                                       "block_black_gradient_square");
        for (int i = 0; i < steps; ++i)
            out.push_back(makeObj(type, x + i * stepW, y + i * stepH));
        return out;
    }

    // ── stair_down ──────────────────────────────────────────────────────────
    // Mirror of stair_up; step N is at x + N*step_width, y - N*step_height.
    // y is the TOP of the staircase; floors out at ground row.
    static std::vector<matjson::Value> stair_down(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        int   steps      = std::clamp(getInt(p, "steps", 4), 1, 50);
        float x          = getFloat(p, "x", 0.f);
        float y          = getFloat(p, "y", getGroundY() + 30.f * steps);
        float stepW      = std::max(15.f, getFloat(p, "step_width", 30.f));
        float stepH      = std::max(15.f, getFloat(p, "step_height", 30.f));
        float groundY    = getGroundY();
        std::string type = resolveType(getStr(p, "block_type", "block_black_gradient_square"),
                                       "block_black_gradient_square");
        for (int i = 0; i < steps; ++i) {
            float yy = std::max(groundY, y - i * stepH);
            out.push_back(makeObj(type, x + i * stepW, yy));
        }
        return out;
    }

    // ── block_floor ─────────────────────────────────────────────────────────
    //   x_start, x_end, y? (ground), block_type?
    // Solid floor of blocks from x_start to x_end (step = 30).
    static std::vector<matjson::Value> block_floor(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x0 = getFloat(p, "x_start", 0.f);
        float x1 = getFloat(p, "x_end",   x0 + 300.f);
        float y  = getFloat(p, "y", getGroundY());
        if (x1 < x0) std::swap(x0, x1);
        // Cap at 200 blocks so a runaway range doesn't fill the editor.
        const int maxBlocks = 200;
        int n = std::min(maxBlocks, (int)((x1 - x0) / 30.f) + 1);
        std::string type = resolveType(getStr(p, "block_type", "block_black_gradient_square"),
                                       "block_black_gradient_square");
        for (int i = 0; i < n; ++i)
            out.push_back(makeObj(type, x0 + i * 30.f, y));
        return out;
    }

    // ── pillar ──────────────────────────────────────────────────────────────
    //   x, y_start? (ground), y_end, block_type?
    // Vertical column of blocks. Useful for walls, decoration spires, etc.
    static std::vector<matjson::Value> pillar(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x  = getFloat(p, "x", 0.f);
        float y0 = getFloat(p, "y_start", getGroundY());
        float y1 = getFloat(p, "y_end", y0 + 120.f);
        if (y1 < y0) std::swap(y0, y1);
        const int maxBlocks = 50;
        int n = std::min(maxBlocks, (int)((y1 - y0) / 30.f) + 1);
        std::string type = resolveType(getStr(p, "block_type", "block_black_gradient_square"),
                                       "block_black_gradient_square");
        for (int i = 0; i < n; ++i)
            out.push_back(makeObj(type, x, y0 + i * 30.f));
        return out;
    }

    // ── platform_run ────────────────────────────────────────────────────────
    //   x_start, x_end, y? (one row above ground), block_type?,
    //   gap_size? (default 0 = solid platform), gap_every? (only if gap_size>0)
    // A horizontal strip of blocks, optionally with regular gaps. Use for
    // hovering platforms the player jumps between.
    static std::vector<matjson::Value> platform_run(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x0 = getFloat(p, "x_start", 0.f);
        float x1 = getFloat(p, "x_end",   x0 + 300.f);
        float y  = getFloat(p, "y", getGroundY() + 90.f);  // 3 rows up by default
        float gapSize  = getFloat(p, "gap_size", 0.f);
        float gapEvery = std::max(60.f, getFloat(p, "gap_every", 120.f));
        if (x1 < x0) std::swap(x0, x1);
        // platform_run defaults: AI usually wants a small floating slab. Use the
        // single-slab catalog name if it exists (it's the closest visual to a
        // standalone platform); resolveType falls back to the basic block.
        std::string type = resolveType(
            getStr(p, "block_type", "block_black_gradient_single_slab"),
            "block_black_gradient_square");

        const int maxBlocks = 200;
        int produced = 0;
        for (float x = x0; x <= x1 && produced < maxBlocks; x += 30.f) {
            // Skip blocks inside a gap window
            if (gapSize > 0.f) {
                float modX = std::fmod(x - x0, gapEvery);
                if (modX >= gapEvery - gapSize) continue;
            }
            out.push_back(makeObj(type, x, y));
            ++produced;
        }
        return out;
    }

    // ── color_pulse ─────────────────────────────────────────────────────────
    //   x, channel? (1), colors? (["#ff0000","#00ff00","#0000ff"]),
    //   duration_per? (0.4), spacing? (10)
    // Series of color triggers that cycle a channel through `colors`. The
    // triggers are spread along X by `spacing` so they fire in sequence as
    // the player runs through. Bidirectional: same channel pulses through
    // the array, then stops on the final color.
    static std::vector<matjson::Value> color_pulse(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x        = getFloat(p, "x", 0.f);
        int   channel  = std::clamp(getInt(p, "channel", 1), 1, 1015);
        float duration = std::clamp(getFloat(p, "duration_per", 0.4f), 0.05f, 30.f);
        float spacing  = std::max(0.f, getFloat(p, "spacing", 10.f));

        // Default rainbow if no colors array passed
        std::vector<std::string> colors;
        const auto& colorsArr = p["colors"];
        if (colorsArr.isArray()) {
            for (size_t i = 0; i < colorsArr.size() && i < 12; ++i) {
                auto cr = colorsArr[i].asString();
                if (cr) colors.push_back(cr.unwrap());
            }
        }
        if (colors.empty()) {
            colors = {"#ff0040", "#ff8000", "#ffff00", "#00ff40", "#00aaff", "#a040ff"};
        }

        const double groundY = (double)getGroundY();  // settings lookup — hoist out of loop
        for (size_t i = 0; i < colors.size(); ++i) {
            auto o = matjson::Value::object();
            o["type"] = "color_trigger";
            o["x"] = (double)(x + i * spacing);
            o["y"] = groundY;
            o["color_channel"] = channel;
            o["color"] = colors[i];
            o["duration"] = (double)duration;
            out.push_back(std::move(o));
        }
        return out;
    }

    // ── trigger_chain ───────────────────────────────────────────────────────
    //   x, groups (array of ints), delays? (array of floats, default 0.5 each)
    // Emits one spawn_trigger per (group, delay) pair, all at the same X.
    // Lets the AI fire several effects in a timed cascade without writing
    // separate triggers for each.
    static std::vector<matjson::Value> trigger_chain(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float x = getFloat(p, "x", 0.f);

        std::vector<int>   groups;
        std::vector<float> delays;
        auto g = p["groups"];
        if (g.isArray()) {
            for (size_t i = 0; i < g.size() && i < 32; ++i) {
                auto gr = g[i].asInt();
                if (gr) groups.push_back(std::clamp((int)gr.unwrap(), 1, 9999));
            }
        }
        auto d = p["delays"];
        if (d.isArray()) {
            for (size_t i = 0; i < d.size() && i < 32; ++i) {
                auto dr = d[i].asDouble();
                delays.push_back(dr ? (float)dr.unwrap() : 0.5f);
            }
        }

        float groundY = getGroundY();
        for (size_t i = 0; i < groups.size(); ++i) {
            float delay = (i < delays.size()) ? delays[i] : (float)(0.5 * (i + 1));
            auto o = matjson::Value::object();
            o["type"] = "spawn_trigger";
            o["x"] = (double)x;
            o["y"] = (double)groundY;
            o["target_group"] = groups[i];
            o["delay"] = (double)std::clamp(delay, 0.f, 30.f);
            out.push_back(o);
        }
        return out;
    }

    // ── orb_arc ─────────────────────────────────────────────────────────────
    //   x, y? (mid-air), count? (3), spacing? (60), orb_type? (yellow)
    // A small horizontal line of orbs for jump chains.
    static std::vector<matjson::Value> orb_arc(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        int   count   = std::clamp(getInt(p, "count", 3), 1, 30);
        float x       = getFloat(p, "x", 0.f);
        float y       = getFloat(p, "y", getGroundY() + 60.f);
        float spacing = std::max(15.f, getFloat(p, "spacing", 60.f));
        std::string type = resolveType(getStr(p, "orb_type", "orb_yellow"), "orb_yellow");
        for (int i = 0; i < count; ++i)
            out.push_back(makeObj(type, x + i * spacing, y));
        return out;
    }

    // ── copy_paste ──────────────────────────────────────────────────────────
    //   from_x_start, from_x_end, to_x_offset (or to_x_start)
    //   y_filter_min? (-inf), y_filter_max? (+inf)
    // Copies every object emitted SO FAR (raw objects + earlier macros) whose
    // X falls in [from_x_start, from_x_end] into a new range offset by
    // to_x_offset. Useful for repeating a motif without re-listing all its
    // objects. Optional y_filter_min/max trims the source vertically.
    //
    // Note: this reads from `current` (the objects already appended), so
    // copy_paste must come AFTER the source objects in the macros array.
    static std::vector<matjson::Value> copy_paste(const matjson::Value& p,
                                                  const std::vector<matjson::Value>& current)
    {
        std::vector<matjson::Value> out;
        float fxs = getFloat(p, "from_x_start", 0.f);
        float fxe = getFloat(p, "from_x_end",   fxs + 300.f);
        if (fxe < fxs) std::swap(fxs, fxe);

        // Support either to_x_offset (delta) or to_x_start (absolute).
        float dx;
        auto offRes = p["to_x_offset"].asDouble();
        if (offRes) {
            dx = (float)offRes.unwrap();
        } else {
            float toStart = getFloat(p, "to_x_start", fxs + (fxe - fxs));
            dx = toStart - fxs;
        }

        float yMin = getFloat(p, "y_filter_min", -1e9f);
        float yMax = getFloat(p, "y_filter_max",  1e9f);

        const int maxCopies = 200;
        for (const auto& src : current) {
            if (!src.isObject()) continue;
            auto xRes = src["x"].asDouble();
            auto yRes = src["y"].asDouble();
            if (!xRes || !yRes) continue;
            float sx = (float)xRes.unwrap();
            float sy = (float)yRes.unwrap();
            if (sx < fxs || sx > fxe) continue;
            if (sy < yMin || sy > yMax) continue;
            auto copy = src;  // matjson::Value supports copy
            copy["x"] = (double)(sx + dx);
            out.push_back(std::move(copy));
            if ((int)out.size() >= maxCopies) break;
        }
        return out;
    }

    // ── mirror_horizontal ───────────────────────────────────────────────────
    //   axis_x — the X position to mirror around
    //   from_x_start?, from_x_end? — limit source range (default: everything left of axis)
    //   y_filter_min?, y_filter_max?
    //   flip_x? — also flip each object visually (true by default)
    // Reflects matching objects across `axis_x`: an object at (axis_x - d, y)
    // produces a copy at (axis_x + d, y). Use to mirror an intro section,
    // build a symmetrical boss arena, etc.
    static std::vector<matjson::Value> mirror_horizontal(const matjson::Value& p,
                                                         const std::vector<matjson::Value>& current)
    {
        std::vector<matjson::Value> out;
        float axisX = getFloat(p, "axis_x", 0.f);
        float fxs   = getFloat(p, "from_x_start", -1e9f);
        float fxe   = getFloat(p, "from_x_end",   axisX);
        if (fxe < fxs) std::swap(fxs, fxe);
        float yMin  = getFloat(p, "y_filter_min", -1e9f);
        float yMax  = getFloat(p, "y_filter_max",  1e9f);

        bool flipX = true;
        auto flipRes = p["flip_x"].asBool();
        if (flipRes) flipX = flipRes.unwrap();

        const int maxOut = 200;
        for (const auto& src : current) {
            if (!src.isObject()) continue;
            auto xRes = src["x"].asDouble();
            auto yRes = src["y"].asDouble();
            if (!xRes || !yRes) continue;
            float sx = (float)xRes.unwrap();
            float sy = (float)yRes.unwrap();
            if (sx < fxs || sx > fxe) continue;
            if (sy < yMin || sy > yMax) continue;
            auto copy = src;
            float mirroredX = axisX + (axisX - sx);
            copy["x"] = (double)mirroredX;
            if (flipX) copy["flip_x"] = true;
            out.push_back(std::move(copy));
            if ((int)out.size() >= maxOut) break;
        }
        return out;
    }

    // ── beat_sync ───────────────────────────────────────────────────────────
    //   bpm (required, 30-300), x_start, x_end, channel? (1),
    //   color? ("#ffffff"), speed_tier? (1 = 1x), fade_in? (0.05),
    //   hold? (0.05), fade_out? (0.20)
    // Places one pulse trigger per beat over the X range. X-per-beat is
    // computed from BPM and GD's player-speed-per-second constant.
    //
    // GD speed constants (units / second):
    //   0=Slow 0.5x = 251.16,  1=Normal 1x = 311.58,  2=2x = 387.42,
    //   3=3x = 478.00,         4=4x = 583.00
    // Beat distance = speed × (60 / BPM).
    static std::vector<matjson::Value> beat_sync(const matjson::Value& p) {
        std::vector<matjson::Value> out;
        float bpm = std::clamp(getFloat(p, "bpm", 120.f), 30.f, 300.f);
        float x0  = getFloat(p, "x_start", 0.f);
        float x1  = getFloat(p, "x_end",   x0 + 1500.f);
        if (x1 < x0) std::swap(x0, x1);
        int channel = std::clamp(getInt(p, "channel", 1), 1, 1015);
        std::string color = getStr(p, "color", "#ffffff");

        // Resolve speed tier → units/second.
        int speedTier = std::clamp(getInt(p, "speed_tier", 1), 0, 4);
        static constexpr float SPEEDS[5] = {251.16f, 311.58f, 387.42f, 478.f, 583.f};
        float secondsPerBeat = 60.f / bpm;

        // Optional speeds="0:1,3000:2" — x:tier pairs marking where the AI
        // placed speed portals. Beat spacing then adapts per segment instead
        // of drifting off-beat after every portal.
        std::vector<std::pair<float, int>> speedMap;
        std::string speedsStr = getStr(p, "speeds", "");
        size_t segStart = 0;
        while (segStart < speedsStr.size()) {
            size_t comma = speedsStr.find(',', segStart);
            if (comma == std::string::npos) comma = speedsStr.size();
            std::string pairStr = speedsStr.substr(segStart, comma - segStart);
            segStart = comma + 1;
            size_t colon = pairStr.find(':');
            if (colon == std::string::npos) continue;
            float px = eas::tryFloat(pairStr.substr(0, colon), -1.f);
            int   pt = (int)eas::tryFloat(pairStr.substr(colon + 1), -1.f);
            if (px >= 0.f && pt >= 0 && pt <= 4) speedMap.emplace_back(px, pt);
        }
        std::sort(speedMap.begin(), speedMap.end());

        auto unitsPerBeatAt = [&](float x) {
            int tier = speedTier;
            for (auto& [sx, st] : speedMap)
                if (sx <= x) tier = st; else break;
            float upb = SPEEDS[tier] * secondsPerBeat;
            return upb < 15.f ? 15.f : upb;  // safety floor
        };

        float fadeIn  = std::clamp(getFloat(p, "fade_in",  0.05f), 0.f, 5.f);
        float hold    = std::clamp(getFloat(p, "hold",     0.05f), 0.f, 5.f);
        float fadeOut = std::clamp(getFloat(p, "fade_out", 0.20f), 0.f, 5.f);

        const int maxBeats = 200;
        int produced = 0;
        for (float x = x0; x <= x1 && produced < maxBeats; x += unitsPerBeatAt(x), ++produced) {
            auto o = matjson::Value::object();
            o["type"]                = "pulse_trigger";
            o["x"]                   = (double)x;
            o["y"]                   = (double)getGroundY();
            o["target_group"]        = 0;            // pulse channel, not group
            o["target_color_channel"]= channel;
            o["color"]               = color;
            o["fade_in"]             = (double)fadeIn;
            o["hold"]                = (double)hold;
            o["fade_out"]            = (double)fadeOut;
            out.push_back(std::move(o));
        }
        log::info("beat_sync: BPM={} base_tier={} ({} speed changes) → {} pulses over X=[{:.0f},{:.0f}]",
                  bpm, speedTier, speedMap.size(), produced, x0, x1);
        return out;
    }
} // namespace macros (closing here; reopened below for block_template + dispatch)

// ─── Block templates (AI-set, applied to every matching object) ─────────────
// When the AI emits {"name":"block_template","type":"<name>","properties":{...}}
// we store the property dict here. Then in applyObjectProperties we merge
// these properties INTO each spawned object of that type before per-object
// fields are applied. The per-object fields still win on conflicts so the AI
// can override the template ad-hoc.
//
// Keyed by the same "type" name the AI uses in objects (e.g.
// "block_black_gradient_square"). Empty map = no templates.
static std::unordered_map<std::string, matjson::Value> s_blockTemplates;

// Reset templates whenever a generation starts so old templates don't bleed
// into a fresh prompt. Called from prepareObjects.
static void resetBlockTemplates() {
    if (!s_blockTemplates.empty()) {
        log::info("Resetting {} block templates from previous generation", s_blockTemplates.size());
        s_blockTemplates.clear();
    }
}

// Merge a template's properties into an object Value WITHOUT overwriting any
// keys that are already set on the object. Returns the count of merged keys.
static int applyBlockTemplateToObject(matjson::Value& obj) {
    auto typeRes = obj["type"].asString();
    if (!typeRes) return 0;
    auto it = s_blockTemplates.find(typeRes.unwrap());
    if (it == s_blockTemplates.end()) return 0;
    int merged = 0;
    if (!it->second.isObject()) return 0;
    for (auto& [k, v] : it->second) {
        if (!obj.contains(k)) {
            obj[k] = v;
            ++merged;
        }
    }
    return merged;
}

namespace macros {
    // ── Dispatcher ──────────────────────────────────────────────────────────
    // Returns true if name matched a known macro. out_objects is appended to
    // (not cleared). For "regional" macros (copy_paste, mirror_horizontal)
    // we pass the CURRENT out_objects so they can read what was emitted
    // earlier in the same macros array.
    static bool expand(const std::string& name,
                       const matjson::Value& params,
                       std::vector<matjson::Value>& out_objects)
    {
        // block_template doesn't produce objects — it stores a template
        // that applies to all future objects of the given type. Accept the
        // kebab and bare variants for robustness.
        if (name == "block_template" || name == "block-template" || name == "template") {
            auto typeRes = params["type"].asString();
            if (!typeRes) {
                log::warn("block_template: missing 'type' — skipping");
                return false;
            }
            auto props = params["properties"];
            if (!props.isObject()) {
                log::warn("block_template: 'properties' is not an object — skipping");
                return false;
            }
            s_blockTemplates[typeRes.unwrap()] = props;
            log::info("Block template set for type '{}' ({} props)",
                      typeRes.unwrap(), (int)props.size());
            return true;
        }

        // Normalize the macro name: lowercase + kebab→snake. Lets the AI emit
        // either "spike-train" or "spike_train"; either "Spike_Train" or "SPIKE_TRAIN".
        // Also folds short aliases (floor → block_floor, wall → pillar) so the
        // AI doesn't have to remember the canonical name.
        std::string canon = name;
        for (auto& c : canon) {
            c = (char)std::tolower((unsigned char)c);
            if (c == '-') c = '_';
        }
        if      (canon == "floor")      canon = "block_floor";
        else if (canon == "wall")       canon = "pillar";
        else if (canon == "block_wall") canon = "pillar";
        else if (canon == "platform")   canon = "platform_run";
        else if (canon == "arc_orbs" || canon == "orb-arc") canon = "orb_arc";
        else if (canon == "stairs_up" || canon == "stairup")     canon = "stair_up";
        else if (canon == "stairs_down" || canon == "stairdown") canon = "stair_down";
        else if (canon == "mirror")     canon = "mirror_horizontal";
        else if (canon == "copy")       canon = "copy_paste";
        else if (canon == "block_pyramid")               canon = "pyramid";
        else if (canon == "spike_ceiling")               canon = "ceiling_spikes";
        else if (canon == "saw_run" || canon == "saws")  canon = "saw_gauntlet";
        else if (canon == "dual" || canon == "dual_corridor")     canon = "dual_section";
        else if (canon == "teleport" || canon == "tp_pair")       canon = "teleport_pair";

        std::vector<matjson::Value> r;
        if      (canon == "spike_train")       r = spike_train(params);
        else if (canon == "stair_up")          r = stair_up(params);
        else if (canon == "stair_down")        r = stair_down(params);
        else if (canon == "block_floor")       r = block_floor(params);
        else if (canon == "pillar")            r = pillar(params);
        else if (canon == "platform_run")      r = platform_run(params);
        else if (canon == "color_pulse")       r = color_pulse(params);
        else if (canon == "trigger_chain")     r = trigger_chain(params);
        else if (canon == "orb_arc")           r = orb_arc(params);
        else if (canon == "beat_sync")         r = beat_sync(params);
        else if (canon == "copy_paste")        r = copy_paste(params, out_objects);
        else if (canon == "mirror_horizontal") r = mirror_horizontal(params, out_objects);
        else if (canon == "pyramid")           r = pyramid(params);
        else if (canon == "ceiling_spikes")    r = ceiling_spikes(params);
        else if (canon == "saw_gauntlet")      r = saw_gauntlet(params);
        else if (canon == "dual_section")      r = dual_section(params);
        else if (canon == "teleport_pair")     r = teleport_pair(params);
        else {
            log::warn("EditorAI: unknown macro '{}' (canon '{}') — skipping", name, canon);
            return false;
        }
        // Propagate per-object fields from the macro's params (color_channel,
        // groups, scale, rotation, etc.) onto every object the macro just
        // produced. This is what lets `FLOOR 0..900 color=4 scale=1.2 groups=1,2`
        // attach the requested color channel, scale, and groups to every
        // block the floor macro emits — same as if the AI had written them
        // out as individual `BLOCK x y color=4 scale=1.2 groups=1,2` lines.
        // copy_paste and mirror_horizontal are deliberately skipped because
        // they re-emit existing objects that already have fields set.
        if (canon != "copy_paste" && canon != "mirror_horizontal") {
            // canon, not name: the alias table maps "copy"→copy_paste and
            // "mirror"→mirror_horizontal — raw-name compares let aliases
            // sneak past and overwrite fields on re-emitted objects.
            applyMacroPassthroughs(r, params);
        }
        log::info("Macro '{}' expanded to {} objects", name, r.size());
        for (auto& obj : r) out_objects.push_back(std::move(obj));
        return true;
    }

    // Iterates a "macros" array from the AI response and appends every
    // expansion into out_objects. Each entry must be an object with a "name"
    // string; any other fields are passed verbatim as params.
    static void expandAll(const matjson::Value& macrosArray,
                          std::vector<matjson::Value>& out_objects)
    {
        if (!macrosArray.isArray()) return;
        int totalBefore = (int)out_objects.size();
        for (size_t i = 0; i < macrosArray.size(); ++i) {
            const auto& entry = macrosArray[i];
            if (!entry.isObject()) continue;
            auto nameRes = entry["name"].asString();
            if (!nameRes) {
                log::warn("EditorAI: macro entry {} has no 'name' — skipping", i);
                continue;
            }
            expand(nameRes.unwrap(), entry, out_objects);
        }
        log::info("Macros: expanded {} objects total", (int)out_objects.size() - totalBefore);
    }
} // namespace macros

// ─── Blueprint preview shared state ─────────────────────────────────────────
// These statics are shared between AIGeneratorPopup (which creates ghost objects)
// and AIEditorUI (which shows accept/deny buttons). Using statics avoids a
// circular dependency between the two classes.

static bool s_inPreviewMode = false;
static bool s_inEditMode   = false;  // user is editing accepted objects before clicking Done
static std::vector<Ref<GameObject>> s_previewObjects;
// Layer-based preview: generated objects land on their OWN editor layer and
// the editor switches to it — GD fades every other layer, so the new build
// shows in full color against the dimmed level (no tints, no opacity hacks).
// Per preview object: the editor layer(s) it should END on after Accept
// (the AI-assigned editor_layer, captured before the preview override).
static std::vector<std::pair<short, short>> s_previewIntendedLayers;
static short s_previewLayer            = -1;  // layer the preview lives on
static short s_editorLayerBeforePreview = -1; // restored on accept/deny/edit

// Switch the editor's visible layer + keep GD's own label in sync.
static void setEditorCurrentLayer(LevelEditorLayer* lel, short layer) {
    if (!lel) return;
    lel->m_currentLayer = layer;
    lel->updateOptions();
    if (lel->m_editorUI && lel->m_editorUI->m_currentLayerLabel) {
        lel->m_editorUI->m_currentLayerLabel->setString(
            layer < 0 ? "ALL" : fmt::format("{}", layer).c_str());
        lel->m_editorUI->m_currentLayerLabel->setVisible(layer >= 0);
    }
}

// ── Edit-op journal ─────────────────────────────────────────────────────────
// MOVE/DELETE/EDIT ops execute against LIVE editor objects the moment a
// result stages, but stay reversible until the user decides: every touched
// object's original transform is journaled here. Deletes are soft (hidden)
// until Accept makes them real; Deny restores everything.
struct EditOpRecord {
    Ref<GameObject>  obj;
    cocos2d::CCPoint pos;
    float rot     = 0.f;
    float scaleX  = 1.f;
    float scaleY  = 1.f;
    bool  hadBaseColor   = false;  // object has an m_baseColor sprite
    bool  hadDetailColor = false;  // object has an m_detailColor sprite
    int   baseColorID   = 0;       // original channel (valid iff hadBaseColor)
    int   detailColorID = 0;       // original channel (valid iff hadDetailColor)
    bool  wasVisible = true;
    bool  deleted    = false;   // soft-deleted (hidden); removed on Accept
};
static std::vector<EditOpRecord> s_editOpJournal;
static std::unordered_set<GameObject*> s_editOpDeleted;  // pending soft deletes

// Accept / Done: make soft deletes real, drop the journal.
static void finalizeEditOps(LevelEditorLayer* lel) {
    int removed = 0;
    for (auto& rec : s_editOpJournal) {
        GameObject* obj = rec.obj;
        if (!obj || !rec.deleted) continue;
        if (lel && obj->getParent()) {
            lel->removeObject(obj, true);
            ++removed;
        }
    }
    if (removed > 0)
        log::info("EditorAI: finalized {} AI deletions", removed);
    s_editOpJournal.clear();
    s_editOpDeleted.clear();
}

// Deny: restore every touched object to its journaled state.
static void rollbackEditOps(LevelEditorLayer* lel) {
    int restored = 0;
    for (auto it = s_editOpJournal.rbegin(); it != s_editOpJournal.rend(); ++it) {
        GameObject* obj = it->obj;
        if (!obj || !obj->getParent()) continue;
        if (obj->getPosition() != it->pos) {
            // moveObject keeps the editor's spatial sections consistent.
            if (lel && lel->m_editorUI)
                lel->m_editorUI->moveObject(obj, it->pos - obj->getPosition());
            else
                obj->setPosition(it->pos);
        }
        obj->setRotation(it->rot);
        obj->setScaleX(it->scaleX);
        obj->setScaleY(it->scaleY);
        // Presence-flag gated (channel 0 is a legitimate value, so ">0"
        // would silently fail to restore an object whose original was 0).
        if (it->hadBaseColor && obj->m_baseColor)
            obj->m_baseColor->m_colorID   = it->baseColorID;
        if (it->hadDetailColor && obj->m_detailColor)
            obj->m_detailColor->m_colorID = it->detailColorID;
        obj->setVisible(it->wasVisible);
        ++restored;
    }
    if (restored > 0)
        log::info("EditorAI: rolled back AI edits on {} objects", restored);
    s_editOpJournal.clear();
    s_editOpDeleted.clear();
}

// Active rating popup, if any. Set by RatingPopup::create, cleared in its
// onClose. Lets the AI button cancel a pending feedback prompt instead of
// being blocked by it. Typed as CCNode to avoid a forward-declaration chain
// (RatingPopup is defined much later).
static Ref<CCNode> s_activeRatingPopup;

// Forward declaration — defined after AIEditorUI
static void showPreviewButtonsOnEditorUI(EditorUI* ui);

// ─── Feedback / rating persistence ──────────────────────────────────────────
// Stores the last generation context so the rating popup (shown after
// accept/deny) can save it alongside the user's 1-10 score.

// ─── Prompt history ──────────────────────────────────────────────────────────
static std::vector<std::string> s_promptHistory;
static int s_promptHistoryIndex = -1;
static constexpr int MAX_PROMPT_HISTORY = 20;

static void addToPromptHistory(const std::string& prompt) {
    // Don't add duplicates of the most recent entry
    if (!s_promptHistory.empty() && s_promptHistory.back() == prompt) return;
    s_promptHistory.push_back(prompt);
    if ((int)s_promptHistory.size() > MAX_PROMPT_HISTORY)
        s_promptHistory.erase(s_promptHistory.begin());
    s_promptHistoryIndex = (int)s_promptHistory.size();  // past the end = "current"
}

// Playtest-ghost overlay (CCDrawNode polyline of the simulated cube run).
// Lives on the editor's object layer; cleared on accept/deny/edit/new spawn.
static Ref<cocos2d::CCDrawNode> s_playtestGhost;

static void removePlaytestGhost() {
    if (s_playtestGhost) {
        s_playtestGhost->removeFromParent();
        s_playtestGhost = nullptr;
    }
}

// Set by the editor's Mutate button just before opening the generator popup;
// init() consumes it and switches the popup into mutation mode.
static bool s_openInMutationMode = false;

static std::string s_lastUserPrompt;    // the raw prompt the user typed
static std::string s_lastAINarration;   // AI's plan prose, shown by the Why? button
static std::string s_lastDifficulty;
static std::string s_lastStyle;
static std::string s_lastLength;
static bool        s_lastWasAccepted = false;
static std::string s_lastGeneratedJson;  // raw JSON objects array from the AI
static std::string s_lastEditSummary;       // edit diff from Edit mode (set by onDoneEditing)
static std::string s_lastEditedObjectsJson; // objects after user edits (set by onDoneEditing)
static int         s_lastSelfRating = 0;    // AI's own RATING: n/10 from self-review

// Auto-telemetry: with allow-telemetry ON, every completed generation is
// uploaded to the community collector (and re-sent once the user rates it,
// so the training filter sees the human verdict). Defined near the other
// collector plumbing; forward-declared here for processFinalResponse.
static void autoContributeGeneration(int userRating);

// Edit tracking: snapshot of accepted objects for implicit feedback
struct AcceptedObjectSnapshot {
    int objectID;
    float x, y;
};
static std::vector<AcceptedObjectSnapshot> s_acceptedSnapshot;
static std::string s_snapshotPrompt;

// Each feedback entry saved to disk
struct FeedbackEntry {
    std::string prompt;
    std::string difficulty;
    std::string style;
    std::string length;
    std::string feedback;          // optional free-text from the user
    std::string objectsJson;       // the AI's generated objects array (compact JSON)
    std::string editedObjectsJson; // objects after user edits (via Edit mode)
    std::string editSummary;       // implicit feedback: what the user changed after accepting
    int         rating;            // 1-10
    bool        accepted;          // true = accepted, false = denied
    int64_t     timestamp = 0;     // unix seconds; 0 on pre-existing entries
};

static std::filesystem::path getFeedbackPath() {
    return Mod::get()->getSaveDir() / "feedback.json";
}

// All rated generations. Loaded from disk once per session, then kept in
// sync in memory; saves serialize on the calling thread and write on a
// background thread so the frame never blocks on disk.
static std::vector<FeedbackEntry>& loadFeedback() {
    static std::vector<FeedbackEntry> s_cache = [] {
        std::vector<FeedbackEntry> entries;
        auto path = getFeedbackPath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return entries;
        auto content = utils::file::readString(path);
        if (!content) {
            log::error("Failed to read feedback.json: {}", content.unwrapErr());
            return entries;
        }
        auto json = matjson::parse(content.unwrap());
        if (!json || !json.unwrap().isArray()) return entries;
        for (auto& item : json.unwrap()) {
            FeedbackEntry e;
            auto p = item["prompt"].asString();     if (p) e.prompt     = p.unwrap();
            auto d = item["difficulty"].asString();  if (d) e.difficulty = d.unwrap();
            auto s = item["style"].asString();       if (s) e.style     = s.unwrap();
            auto l = item["length"].asString();      if (l) e.length    = l.unwrap();
            auto f = item["feedback"].asString();    if (f) e.feedback    = f.unwrap();
            auto o = item["objectsJson"].asString();       if (o)  e.objectsJson       = o.unwrap();
            auto eo = item["editedObjectsJson"].asString(); if (eo) e.editedObjectsJson = eo.unwrap();
            auto es = item["editSummary"].asString();       if (es) e.editSummary       = es.unwrap();
            auto r = item["rating"].asInt();         if (r) e.rating    = r.unwrap();
            auto a = item["accepted"].asBool();      if (a) e.accepted  = a.unwrap();
            auto ts = item["timestamp"].asInt();     if (ts) e.timestamp = ts.unwrap();
            if (!e.prompt.empty() && e.rating >= 1 && e.rating <= 10)
                entries.push_back(std::move(e));
        }
        return entries;
    }();
    return s_cache;
}

// Serialize the in-memory cache and write it out on a background thread.
static void persistFeedback() {
    auto& entries = loadFeedback();
    auto arr = matjson::Value::array();
    for (auto& e : entries) {
        auto obj = matjson::Value::object();
        obj["prompt"]     = e.prompt;
        obj["difficulty"] = e.difficulty;
        obj["style"]      = e.style;
        obj["length"]     = e.length;
        if (!e.feedback.empty())
            obj["feedback"] = e.feedback;
        if (!e.objectsJson.empty())
            obj["objectsJson"] = e.objectsJson;
        if (!e.editedObjectsJson.empty())
            obj["editedObjectsJson"] = e.editedObjectsJson;
        if (!e.editSummary.empty())
            obj["editSummary"] = e.editSummary;
        obj["rating"]     = e.rating;
        obj["accepted"]   = e.accepted;
        if (e.timestamp) obj["timestamp"] = e.timestamp;
        arr.push(obj);
    }

    // Move the matjson tree into the worker and dump() there — entries hold
    // full level dumps, so serialization itself is the expensive part.
    // Snapshots carry a sequence number so two in-flight writers can't land
    // out of order (last-SNAPSHOT-wins, not last-thread-wins), and the write
    // goes through a temp file + rename so a crash mid-write can't tear
    // feedback.json.
    static std::atomic<uint64_t> s_writeGen{0};
    uint64_t gen = ++s_writeGen;
    std::thread([path = getFeedbackPath(), arr = std::move(arr), gen] {
        static std::mutex s_writeMutex;
        static uint64_t   s_lastWritten = 0;
        std::lock_guard<std::mutex> lock(s_writeMutex);
        if (gen <= s_lastWritten) return;  // a newer snapshot already landed
        auto tmp = path;
        tmp += ".tmp";
        auto res = utils::file::writeString(tmp, arr.dump());
        if (!res) {
            log::error("Failed to save feedback.json: {}", res.unwrapErr());
            return;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) log::error("Failed to commit feedback.json: {}", ec.message());
        else    s_lastWritten = gen;
    }).detach();
}

static void saveFeedbackEntry(const FeedbackEntry& entry) {
    auto& entries = loadFeedback();
    entries.push_back(entry);

    // Keep at most 200 entries (oldest dropped). Few-shot selection samples
    // from this pool — a bigger pool means better-matched examples; disk
    // cost is bounded by the sampleJson truncation.
    while (entries.size() > 200)
        entries.erase(entries.begin());

    persistFeedback();
    log::info("Saved feedback entry (rating={}, accepted={})", entry.rating, entry.accepted);
}

// Compute similarity score (0.0-1.0) between a feedback entry and the current request.
// Matching difficulty/style/length each contribute 0.33, so identical settings = 1.0.
static float feedbackSimilarity(const FeedbackEntry& e,
                                 const std::string& difficulty,
                                 const std::string& style,
                                 const std::string& length) {
    float score = 0.0f;
    if (e.difficulty == difficulty) score += 0.33f;
    if (e.style == style)          score += 0.33f;
    if (e.length == length)        score += 0.34f;
    return score;
}

// Returns the top-N highest-rated accepted entries, prioritized by similarity
// to the current request, then by rating.
// Shared selector for the few-shot pickers below. Entries carry full level
// dumps (objectsJson can be hundreds of KB), so this works with POINTERS into
// the cache — never copy the store. `betterRating` orders ties after the
// similarity sort: top picks want highest-rated first, bottom picks lowest.
template <class Filter, class Better>
static std::vector<const FeedbackEntry*> pickFeedback(int maxCount,
                                                      const std::string& difficulty,
                                                      const std::string& style,
                                                      const std::string& length,
                                                      Filter filter, Better betterRating) {
    auto& entries = loadFeedback();
    bool hasCurrent = !difficulty.empty() || !style.empty() || !length.empty();

    // Score each entry ONCE up front — computing similarity inside the sort
    // comparator re-ran the string comparisons O(N log N) times.
    std::vector<std::pair<float, const FeedbackEntry*>> scored;
    for (auto& e : entries)
        if (filter(e))
            scored.emplace_back(
                hasCurrent ? feedbackSimilarity(e, difficulty, style, length) : 0.f, &e);

    std::sort(scored.begin(), scored.end(),
        [&](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return betterRating(a.second->rating, b.second->rating);
        });

    std::vector<const FeedbackEntry*> picked;
    picked.reserve(std::min((size_t)std::max(maxCount, 0), scored.size()));
    for (auto& [sim, e] : scored) {
        if ((int)picked.size() >= maxCount) break;
        picked.push_back(e);
    }
    return picked;
}

// Top-N highest-rated accepted entries, most similar to the current request.
static std::vector<const FeedbackEntry*> getTopFeedback(int maxCount,
                                                        const std::string& difficulty = "",
                                                        const std::string& style = "",
                                                        const std::string& length = "") {
    return pickFeedback(maxCount, difficulty, style, length,
        [](const FeedbackEntry& e) { return e.accepted && e.rating >= 6; },
        [](int a, int b) { return a > b; });
}

// N lowest-rated entries with feedback text, most similar first.
static std::vector<const FeedbackEntry*> getBottomFeedback(int maxCount,
                                                           const std::string& difficulty = "",
                                                           const std::string& style = "",
                                                           const std::string& length = "") {
    return pickFeedback(maxCount, difficulty, style, length,
        [](const FeedbackEntry& e) {
            return e.rating <= 5 && (!e.feedback.empty() || !e.editSummary.empty());
        },
        [](int a, int b) { return a < b; });
}

// Diff accepted snapshot vs current level state. Returns a human-readable
// summary of what the user changed (deleted objects, moved objects).
static std::string computeEditSummary(LevelEditorLayer* editor) {
    if (s_acceptedSnapshot.empty() || !editor) return "";

    // Build a set of current object positions from the editor
    std::unordered_map<int, std::vector<std::pair<float,float>>> currentObjs;
    auto* objects = editor->m_objects;
    if (objects) {
        for (int i = 0; i < objects->count(); ++i) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (obj) {
                currentObjs[obj->m_objectID].push_back({obj->getPositionX(), obj->getPositionY()});
            }
        }
    }

    int deleted = 0;
    int moved = 0;
    int kept = 0;
    for (auto& snap : s_acceptedSnapshot) {
        auto it = currentObjs.find(snap.objectID);
        if (it == currentObjs.end() || it->second.empty()) {
            ++deleted;
            continue;
        }
        // Find closest match for this snapshot position
        float bestDist = 999999.f;
        int bestIdx = -1;
        for (int j = 0; j < (int)it->second.size(); ++j) {
            float dx = it->second[j].first - snap.x;
            float dy = it->second[j].second - snap.y;
            float dist = dx*dx + dy*dy;
            if (dist < bestDist) { bestDist = dist; bestIdx = j; }
        }
        if (bestDist < 1.0f) {
            ++kept;
            it->second.erase(it->second.begin() + bestIdx);
        } else if (bestDist < 10000.f) {
            ++moved;
            it->second.erase(it->second.begin() + bestIdx);
        } else {
            ++deleted;
        }
    }

    int total = (int)s_acceptedSnapshot.size();
    if (deleted == 0 && moved == 0) return "";  // user kept everything as-is

    std::string summary;
    if (deleted > 0)
        summary += fmt::format("deleted {}/{} objects", deleted, total);
    if (moved > 0) {
        if (!summary.empty()) summary += ", ";
        summary += fmt::format("moved {}/{} objects", moved, total);
    }
    if (kept > 0) {
        if (!summary.empty()) summary += ", ";
        summary += fmt::format("kept {}/{} unchanged", kept, total);
    }
    return summary;
}

// Capture the current editor objects within the snapshot's X-range as JSON.
// This gives us the "after editing" state to pair with the original generation.
static std::string captureEditedObjects(LevelEditorLayer* editor) {
    if (s_acceptedSnapshot.empty() || !editor) return "";

    // Find the X-range of the original generation
    float minX = s_acceptedSnapshot[0].x, maxX = s_acceptedSnapshot[0].x;
    for (auto& snap : s_acceptedSnapshot) {
        if (snap.x < minX) minX = snap.x;
        if (snap.x > maxX) maxX = snap.x;
    }
    // Pad the range slightly to catch objects the user moved near the edges
    minX -= 30.f;
    maxX += 30.f;

    // Build reverse lookup: object ID -> type name
    const auto& idToName = objectIdToName();

    auto arr = matjson::Value::array();
    auto* objects = editor->m_objects;
    if (objects) {
        for (int i = 0; i < objects->count(); ++i) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!obj) continue;
            float ox = obj->getPositionX();
            if (ox < minX || ox > maxX) continue;

            auto entry = matjson::Value::object();
            auto nameIt = idToName.find(obj->m_objectID);
            if (nameIt != idToName.end())
                entry["type"] = nameIt->second;
            else
                entry["id"] = obj->m_objectID;
            entry["x"] = (int)obj->getPositionX();
            entry["y"] = (int)obj->getPositionY();
            arr.push(entry);
        }
    }

    return arr.size() > 0 ? arr.dump() : "";
}

// ─── Rating popup ────────────────────────────────────────────────────────────

class RatingPopup : public Popup {
protected:
    int m_selectedRating = 0;
    std::vector<CCMenuItemSpriteExtra*> m_ratingButtons;
    CCMenuItemSpriteExtra* m_shareBtn = nullptr;       // telemetry opt-in (8+ only)
    bool m_shared = false;                              // one share per rating
    async::TaskHolder<web::WebResponse> m_shareTask;
    CCLabelBMFont* m_ratingLabel     = nullptr;
    CCLabelBMFont* m_feedbackHint    = nullptr;
    TextInput*     m_feedbackInput   = nullptr;
    // Ref: the ring lives unparented until first selection and gets
    // reparented between buttons — a raw pointer would dangle.
    Ref<CCSprite>  m_selRing;

    bool init() override {
        constexpr float W = 380.f, H = 280.f;
        if (!Popup::init(W, H))
            return false;

        this->setID("rating-popup"_spr);
        this->setTitle("Rate This Generation");
        ui::styleTitle(m_title, 0.8f);
        ui::addGroove(m_mainLayer, 330.f, W / 2.f, H - 36.f);

        auto descLabel = CCLabelBMFont::create(
            s_lastWasAccepted ? "How good was this generation?" : "How was this generation?",
            "bigFont.fnt");
        descLabel->limitLabelWidth(340.f, 0.4f, 0.1f);
        descLabel->setColor(ui::TEXT_PRIMARY);
        descLabel->setPosition({W / 2.f, 226.f});
        m_mainLayer->addChild(descLabel);

        auto hintLabel = CCLabelBMFont::create(
            "Your ratings help the AI learn your style", "chatFont.fnt");
        hintLabel->setScale(0.55f);
        hintLabel->setColor(ui::TEXT_SECONDARY);
        hintLabel->setPosition({W / 2.f, 208.f});
        m_mainLayer->addChild(hintLabel);

        // 1-10 rating grid: two rows of five slate digit buttons.
        auto ratingMenu = CCMenu::create();
        ratingMenu->setContentSize({280.f, 100.f});
        ratingMenu->ignoreAnchorPointForPosition(false);
        ratingMenu->setAnchorPoint({0.5f, 0.5f});
        ratingMenu->setPosition({W / 2.f, 166.f});
        for (int i = 1; i <= 10; ++i) {
            auto label = fmt::format("{}", i);
            auto btn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create(label.c_str(), "bigFont.fnt", "GJ_button_04.png", 0.6f),
                this, menu_selector(RatingPopup::onRatingButton)
            );
            btn->setTag(i);
            // Unselected = dimmed; selection brightens + rings (see below).
            btn->setColor(ui::TEXT_INACTIVE);
            int col = (i - 1) % 5;
            btn->setPosition({140.f + (float)(col - 2) * 52.f,
                              i <= 5 ? 73.f : 27.f});
            ratingMenu->addChild(btn);
            m_ratingButtons.push_back(btn);
        }
        m_mainLayer->addChild(ratingMenu);

        // Icon-kit selection ring, reparented onto the picked digit.
        m_selRing = CCSprite::createWithSpriteFrameName("GJ_select_001.png");
        if (m_selRing) m_selRing->setVisible(false);

        m_ratingLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_ratingLabel->setScale(0.5f);
        m_ratingLabel->setPosition({W / 2.f, 116.f});
        m_ratingLabel->setVisible(false);
        m_mainLayer->addChild(m_ratingLabel);

        // Feedback hint — changes text based on selected rating
        m_feedbackHint = CCLabelBMFont::create("(optional)", "chatFont.fnt");
        m_feedbackHint->setScale(0.5f);
        m_feedbackHint->setColor(ui::TEXT_DIM);
        m_feedbackHint->setPosition({W / 2.f, 98.f});
        m_mainLayer->addChild(m_feedbackHint);

        // Feedback well + input (the well IS the input bg).
        auto well = ui::makeWell(320.f, 36.f);
        well->setPosition({W / 2.f, 70.f});
        m_mainLayer->addChild(well);

        // Width is PRE-scale: 500 × 0.6 = 300pt touch target, matching the well.
        m_feedbackInput = TextInput::create(500, "What could be improved?", "bigFont.fnt");
        m_feedbackInput->setPosition({W / 2.f, 70.f});
        m_feedbackInput->setScale(0.6f);
        if (auto bg = m_feedbackInput->getBGSprite()) bg->setVisible(false);
        // No setMaxCharCount / setAllowedChars — the global CCTextInputNode
        // bypass hook below makes both dead code. Free-text feedback can be
        // any length and any character set.
        m_mainLayer->addChild(m_feedbackInput);

        // Bottom row: [No Feedback] [Submit]
        // - Submit (right, gold/green): saves the chosen rating to feedback.json
        // - No Feedback (left, red): dismisses without saving. The previous
        //   generation's accept/deny still counts; only the rating is skipped.
        auto submitMenu = CCMenu::create();
        submitMenu->setContentSize({340.f, 40.f});
        submitMenu->ignoreAnchorPointForPosition(false);
        submitMenu->setAnchorPoint({0.5f, 0.5f});
        submitMenu->setPosition({W / 2.f, 30.f});

        auto skipBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("No Feedback", "goldFont.fnt", "GJ_button_06.png", 0.6f),
            this, menu_selector(RatingPopup::onSkipFeedback)
        );
        skipBtn->setPosition({90.f, 20.f});
        submitMenu->addChild(skipBtn);

        auto submitBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Submit", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(RatingPopup::onSubmit)
        );
        submitBtn->setPosition({244.f, 20.f});
        submitMenu->addChild(submitBtn);

        // "Share" — opt-in community dataset donation. Hidden until the user
        // picks a rating of 8+ AND has allow-telemetry on; nothing ever
        // uploads without this explicit tap.
        m_shareBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Share", "goldFont.fnt", "GJ_button_03.png", 0.6f),
            this, menu_selector(RatingPopup::onShareTelemetry));
        m_shareBtn->setPosition({320.f, 20.f});
        m_shareBtn->setVisible(false);
        submitMenu->addChild(m_shareBtn);

        m_mainLayer->addChild(submitMenu);

        return true;
    }

    // One POST to the Platinum collector: prompt + settings + rating + the
    // generated level. No keys, no identity — see the setting description.
    void onShareTelemetry(CCObject*) {
        if (m_shared || m_selectedRating < 8) return;
        if (s_lastGeneratedJson.empty() || s_lastUserPrompt.empty()) {
            Notification::create("Nothing to share for this generation",
                                 NotificationIcon::Warning)->show();
            return;
        }
        m_shared = true;
        if (m_shareBtn) m_shareBtn->setVisible(false);

        auto body = matjson::Value::object();
        body["v"]          = 1;
        body["rating"]     = m_selectedRating;
        body["prompt"]     = s_lastUserPrompt;
        body["difficulty"] = s_lastDifficulty;
        body["style"]      = s_lastStyle;
        body["length"]     = s_lastLength;
        body["objects"]    = s_lastGeneratedJson;   // compact JSON string

        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");
        request.timeout(std::chrono::seconds(10));
        request.bodyString(body.dump());
        m_shareTask.spawn(
            request.post("http://sn-1.vltgg.net:21800/api/contribute"),
            [](web::WebResponse resp) {
                Notification::create(
                    resp.ok() ? "Shared - thanks for improving the model!"
                              : "Share failed (collector unreachable)",
                    resp.ok() ? NotificationIcon::Success
                              : NotificationIcon::Warning)->show();
            });
    }

    // Closes the popup without saving any feedback. Lets the user dismiss
    // the prompt when they don't want to rate this particular generation.
    void onSkipFeedback(CCObject*) {
        log::info("EditorAI: user skipped feedback for this generation");
        Notification::create("Feedback skipped.", NotificationIcon::Info)->show();
        this->onClose(nullptr);
    }

    void onRatingButton(CCObject* sender) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        m_selectedRating = btn->getTag();

        // Brighten the pick, dim the rest, and move the icon-kit selection
        // ring onto it. No scale pop — the 52px grid pitch would overlap.
        for (auto* b : m_ratingButtons) {
            bool selected = (b->getTag() == m_selectedRating);
            b->setColor(selected ? ui::TEXT_PRIMARY : ui::TEXT_INACTIVE);
            if (selected && m_selRing) {
                if (auto img = b->getNormalImage()) {
                    m_selRing->removeFromParent();
                    auto size = img->getContentSize();
                    m_selRing->setPosition({size.width / 2.f, size.height / 2.f});
                    float target = std::max(size.width, size.height) * 1.25f;
                    m_selRing->setScale(target / m_selRing->getContentSize().width);
                    m_selRing->setVisible(true);
                    img->addChild(m_selRing, 10);
                }
            }
        }

        m_ratingLabel->setString(fmt::format("Rating: {}/10", m_selectedRating).c_str());
        m_ratingLabel->setVisible(true);
        if (m_shareBtn)
            m_shareBtn->setVisible(m_selectedRating >= 8 && !m_shared &&
                Mod::get()->getSettingValue<bool>("allow-telemetry"));

        // Update feedback hint and placeholder based on rating range
        if (m_selectedRating <= 4) {
            m_feedbackHint->setString("What went wrong? (optional)");
            m_feedbackInput->setString("");
        } else if (m_selectedRating <= 7) {
            m_feedbackHint->setString("What could be better? (optional)");
            m_feedbackInput->setString("");
        } else {
            m_feedbackHint->setString("What did you like? (optional)");
            m_feedbackInput->setString("");
        }
    }

    void onSubmit(CCObject*) {
        if (m_selectedRating == 0) {
            FLAlertLayer::create("No Rating", gd::string("Please select a rating first."), "OK")->show();
            return;
        }

        FeedbackEntry entry;
        entry.prompt      = s_lastUserPrompt;
        entry.difficulty  = s_lastDifficulty;
        entry.style       = s_lastStyle;
        entry.length      = s_lastLength;
        entry.feedback    = m_feedbackInput->getString();
        entry.objectsJson       = s_lastGeneratedJson;
        entry.editedObjectsJson = s_lastEditedObjectsJson;
        entry.editSummary       = s_lastEditSummary;
        entry.rating            = m_selectedRating;
        entry.accepted          = s_lastWasAccepted;
        entry.timestamp         = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        saveFeedbackEntry(entry);
        s_lastEditSummary.clear();
        s_lastEditedObjectsJson.clear();

        Notification::create(
            fmt::format("Rated {}/10 — thanks!", m_selectedRating),
            NotificationIcon::Success
        )->show();

        this->onClose(nullptr);
    }

    // Override to clear the active-popup pointer so the AI button knows the
    // feedback prompt is no longer up. Called on the X-close, on submit, and
    // when the AI button cancels the feedback prompt.
    void onClose(CCObject* sender) override {
        if (s_activeRatingPopup && s_activeRatingPopup.data() == static_cast<CCNode*>(this)) {
            s_activeRatingPopup = nullptr;
        }
        Popup::onClose(sender);
    }

public:
    static RatingPopup* create() {
        auto ret = new RatingPopup();
        if (ret->init()) {
            ret->autorelease();
            // Remember this popup so clicking the AI button can cancel it.
            s_activeRatingPopup = ret;
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Cancel the rating prompt if one is showing. Called from the AI button so the
// user can start a new generation without first having to rate the previous
// one. The rating is effectively discarded (not saved).
static void cancelActiveRatingPopup() {
    if (!s_activeRatingPopup) return;
    auto popup = s_activeRatingPopup;       // keep alive across removal
    s_activeRatingPopup = nullptr;          // prevent re-entry from popup's own onClose
    log::info("EditorAI: cancelling pending rating prompt — user clicked AI button instead");
    popup->removeFromParentAndCleanup(true);
}

// Forward declaration — defined after AIEditorUI
static void showRatingIfEnabled();

// ─── In-mod settings popup (replaces the trip to Geode's settings page) ─────
//
// Three tabs — General / Provider / Advanced — covering every mod.json
// setting. The Provider tab folds in the sign-in flow: paste-key, OAuth
// (PKCE+loopback, HuggingFace only), test, sign-out.
// One popup, one mental model, no more "open Geode settings".
//
// Visual language: GD-native (see the ui:: namespace) — striped comment-cell
// rows inside a recessed well, garage-style GJ_tab plates, real GD checkbox
// togglers, goldFont headers, chunky GD action buttons.

// Defined after DebugInspectorPopup (which needs the request log statics
// declared further down); the settings popup only needs to launch it.
static void openDebugInspector();

// Maps a provider id to its model-setting key in mod.json.
static const char* providerModelSettingKey(const std::string& p) {
    if (p == "gemini")      return "gemini-model";
    if (p == "claude")      return "claude-model";
    if (p == "openai")      return "openai-model";
    if (p == "ministral")   return "ministral-model";
    if (p == "huggingface") return "huggingface-model";
    if (p == "openrouter")  return "openrouter-model";
    if (p == "deepseek")    return "deepseek-model";
    if (p == "ollama")      return "ollama-model";
    if (p == "lm-studio")   return "lm-studio-model";
    if (p == "llama-cpp")   return "llama-cpp-model";
    return "custom-provider-model";
}

class AISettingsPopup : public Popup {
public:
    enum class Tab { General, Provider, Advanced };

protected:
    Tab m_tab = Tab::General;
    CCNode* m_content = nullptr;
    std::array<CCMenuItemSpriteExtra*, 3> m_tabBtns{nullptr, nullptr, nullptr};
    // Per-tab restyle handles: GJ_tabOn/GJ_tabOff plates + label. Avoids the
    // old getNormalImage()->getChildren() walking on tab switch.
    struct TabUI { CCNode* on = nullptr; CCNode* off = nullptr; CCLabelBMFont* label = nullptr; };
    std::array<TabUI, 3> m_tabUI{};

    struct TextRow  { std::string sid; TextInput* in; bool password; };
    struct IntRow   { std::string sid; TextInput* in; int64_t min, max, def; };
    // CycleRow stores a pointer to the value-display label so the arrow
    // handlers can update its text in place — no full tab rebuild on cycle.
    struct CycleRow { std::string sid; std::vector<std::string> opts; CCLabelBMFont* lbl; };
    std::vector<TextRow>  m_texts;
    std::vector<IntRow>   m_ints;
    std::vector<CycleRow> m_cycles;

    // Provider-tab OAuth flow state (PKCE — HuggingFace only).
    std::unique_ptr<oauth::LocalCallback> m_pkceListener;
    std::string m_pkceVerifier, m_pkceState;
    int         m_pkcePort = 0;
    float       m_pkceElapsed = 0.f;  // seconds since the browser was opened
    async::TaskHolder<web::WebResponse> m_authNet;
    async::TaskHolder<web::WebResponse> m_modelNet;   // dynamic model-list fetch
    CCLabelBMFont* m_authStatus = nullptr;
    CCSprite*      m_authDot    = nullptr;  // status pill dot, tinted with each setAuthStatus

    // Width lint: GD's design height is locked at 320pt, so the 4:3 design
    // width floor is ~426.7 — the old 480 overflowed iPads. 410 max.
    static constexpr float W = 410.f;
    static constexpr float H = 290.f;

    // Row engine state: rows append to the active tab's scroll content.
    CCNode* m_rows     = nullptr;   // ScrollLayer's m_contentLayer
    int     m_rowIndex = 0;         // stripe counter (CELL vs CELL_ALT)
    static constexpr float ROW_W = 364.f;
    static constexpr float ROW_H = 26.f;
    // Cycler value-label fit, shared by addCycler and cycleStep so the
    // build-time and update-time fits can't drift.
    static constexpr float CYCLE_LBL_W   = 120.f;
    static constexpr float CYCLE_LBL_SC  = 0.4f;
    static constexpr float CYCLE_LBL_MIN = 0.2f;

    static const char* tabName(Tab t) {
        switch (t) {
            case Tab::General:  return "General";
            case Tab::Provider: return "Provider";
            case Tab::Advanced: return "Advanced";
        }
        return "?";
    }

    bool init(Tab startTab) {
        m_tab = startTab;
        if (!Popup::init(W, H)) return false;
        this->setID("settings-popup"_spr);
        this->setTitle("AI Settings");
        ui::styleTitle(m_title, 0.75f);

        // Recessed black content well spanning the body, with a raised
        // table-top lip the tab plates visually connect to.
        auto well = ui::makeWell(384.f, 200.f);
        well->setPosition({W / 2.f, 118.f});
        m_mainLayer->addChild(well, 0);

        // Tab strip — garage-style GJ_tabOn/tabOff plates whose bottoms
        // overlap the lip by 2px so the active tab reads as "connected".
        auto tabMenu = CCMenu::create();
        tabMenu->setContentSize({W, 34.f});
        tabMenu->ignoreAnchorPointForPosition(false);
        tabMenu->setAnchorPoint({0.5f, 0.5f});
        tabMenu->setPosition({W / 2.f, 233.f});
        for (int i = 0; i < 3; ++i) {
            Tab t = (Tab)i;
            constexpr float TAB_W = 92.f, TAB_H = 30.f;
            auto stack = CCNode::create();
            stack->setContentSize({TAB_W, TAB_H});

            // GJ_tab frames can fail/garble on rotated atlas frames — null-
            // check and fall back to tinted square02b plates.
            CCNode* on  = CCScale9Sprite::createWithSpriteFrameName("GJ_tabOn_001.png");
            CCNode* off = CCScale9Sprite::createWithSpriteFrameName("GJ_tabOff_001.png");
            if (!on || !off) {
                auto onS  = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
                onS->setColor(ui::CELL_COL);
                auto offS = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
                offS->setColor(ui::TAB_OFF_COL);
                on = onS; off = offS;
            }
            static_cast<CCScale9Sprite*>(on)->setContentSize({TAB_W, TAB_H});
            static_cast<CCScale9Sprite*>(off)->setContentSize({TAB_W, TAB_H});
            on->setPosition({TAB_W / 2.f, TAB_H / 2.f});
            off->setPosition({TAB_W / 2.f, TAB_H / 2.f});
            stack->addChild(on);
            stack->addChild(off);

            auto lbl = CCLabelBMFont::create(tabName(t), "bigFont.fnt");
            lbl->setScale(0.4f);
            lbl->setPosition({TAB_W / 2.f, TAB_H / 2.f + 1.f});
            stack->addChild(lbl, 1);

            m_tabUI[i] = {on, off, lbl};

            auto btn = CCMenuItemSpriteExtra::create(stack, this,
                menu_selector(AISettingsPopup::onTabSwitch));
            btn->setTag(i);
            btn->setPosition({W / 2.f + (float)(i - 1) * 96.f, 17.f});
            tabMenu->addChild(btn);
            m_tabBtns[i] = btn;
        }
        m_mainLayer->addChild(tabMenu, 3);
        restyleTabs();

        m_content = CCNode::create();
        m_content->setPosition({0, 0});
        m_mainLayer->addChild(m_content, 1);

        buildTab();
        return true;
    }

    // Active tab shows its GJ_tabOn plate + bright label; inactive show
    // tabOff + dimmed label. Pure visibility/color swap, no rebuilds.
    void restyleTabs() {
        for (int i = 0; i < 3; ++i) {
            bool act = ((int)m_tab == i);
            if (m_tabUI[i].on)    m_tabUI[i].on->setVisible(act);
            if (m_tabUI[i].off)   m_tabUI[i].off->setVisible(!act);
            if (m_tabUI[i].label) m_tabUI[i].label->setColor(
                act ? ui::TEXT_PRIMARY : ui::TEXT_INACTIVE);
        }
    }

    void onClose(CCObject* o) override { flushInputs(); Popup::onClose(o); }

    void flushInputs() {
        for (auto& r : m_texts)
            geode::Mod::get()->setSettingValue<std::string>(r.sid,
                std::string(r.in->getString()));
        for (auto& r : m_ints) {
            std::string s = r.in->getString();
            int64_t v = r.def;
            if (!s.empty()) {
                if (auto num = geode::utils::numFromString<int64_t>(s)) v = num.unwrap();
            }
            v = std::clamp(v, r.min, r.max);
            geode::Mod::get()->setSettingValue<int64_t>(r.sid, v);
        }
    }

    void onTabSwitch(CCObject* sender) {
        flushInputs();
        Tab t = (Tab)static_cast<CCNode*>(sender)->getTag();
        if (t == m_tab) return;
        m_tab = t;
        restyleTabs();
        buildTab();
    }

    void buildTab() {
        // Persist typed-but-unsaved TextInput contents before destroying
        // them. Covers every rebuild path (provider cycle, sign-out, PKCE
        // completion) — not just tab switch and close. No-op at init
        // (vectors empty); idempotent on the already-flushed paths; cyclers
        // are never in m_texts/m_ints so this can't clobber a just-cycled
        // setting.
        flushInputs();
        m_content->removeAllChildren();
        m_texts.clear(); m_ints.clear(); m_cycles.clear();
        m_authStatus = nullptr;
        m_authDot    = nullptr;
        m_rowIndex   = 0;

        // Fresh scroll container per tab. General/Provider fit without
        // scrolling (scrollbar sits idle); Advanced scrolls.
        auto scroll = ScrollLayer::create(CCSize{372.f, 188.f});
        scroll->setID("settings-scroll"_spr);
        scroll->setPosition({19.f, 24.f});
        scroll->m_contentLayer->setLayout(
            ColumnLayout::create()
                ->setAxisReverse(true)
                ->setAxisAlignment(AxisAlignment::End)
                ->setAutoGrowAxis(188.f)
                ->setGap(3.f)
                ->setAutoScale(false));
        m_rows = scroll->m_contentLayer;
        m_content->addChild(scroll);

        auto bar = Scrollbar::create(scroll);
        bar->setPosition({398.f, 118.f});
        m_content->addChild(bar);

        switch (m_tab) {
            case Tab::General:  buildGeneral();  break;
            case Tab::Provider: buildProvider(); break;
            case Tab::Advanced: buildAdvanced(); break;
        }

        // Order matters: layout first, then snap to top.
        m_rows->updateLayout();
        scroll->scrollToTop();
    }

    // ── Row engine ───────────────────────────────────────────────────────
    // Every settings row is a 364×26 node appended to the scroll content:
    // striped brown comment-cell background, white bigFont label on the
    // left, control on the right. Children use ROW-LOCAL coords (origin
    // bottom-left).

    // Base row: striped cell + left label. Caller attaches the control and
    // MUST call pushRow(row).
    CCNode* makeRow(const char* lbl) {
        auto row = CCNode::create();
        row->setContentSize({ROW_W, ROW_H});
        auto cell = ui::makeCell(ROW_W, ROW_H, (m_rowIndex++ % 2) == 1);
        cell->setPosition({ROW_W / 2.f, ROW_H / 2.f});
        row->addChild(cell);

        auto l = CCLabelBMFont::create(lbl, "bigFont.fnt");
        l->limitLabelWidth(180.f, 0.35f, 0.1f);
        l->setColor(ui::TEXT_PRIMARY);
        l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({10.f, ROW_H / 2.f});
        row->addChild(l);
        return row;
    }
    void pushRow(CCNode* row) { m_rows->addChild(row); }

    // Bare (cell-less) row of arbitrary height, for notes/headers/pills.
    CCNode* makeBareRow(float h) {
        auto row = CCNode::create();
        row->setContentSize({ROW_W, h});
        return row;
    }

    // ── Cycler — ◀ value ▶. The value label must stay a SIBLING of the
    // arrow CCMenu (CCMenu touch dispatch assumes CCMenuItem children —
    // a label inside crashes it).
    void addCycler(const char* lbl, const char* sid,
                   const std::vector<std::string>& opts) {
        std::string cur = geode::Mod::get()->getSettingValue<std::string>(sid);
        if (cur.empty() && !opts.empty()) cur = opts[0];
        if (cur.empty()) cur = "-";

        int tag = (int)m_cycles.size();
        m_cycles.push_back({sid, opts, nullptr});

        auto row = makeRow(lbl);

        auto valLbl = CCLabelBMFont::create(cur.c_str(), "bigFont.fnt");
        valLbl->limitLabelWidth(CYCLE_LBL_W, CYCLE_LBL_SC, CYCLE_LBL_MIN);
        valLbl->setColor(ui::TEXT_PRIMARY);
        valLbl->setPosition({287.f, ROW_H / 2.f});
        row->addChild(valLbl);
        m_cycles.back().lbl = valLbl;

        auto arrowMenu = CCMenu::create();
        arrowMenu->setContentSize({ROW_W, ROW_H});
        arrowMenu->ignoreAnchorPointForPosition(false);
        arrowMenu->setAnchorPoint({0.5f, 0.5f});
        arrowMenu->setPosition({ROW_W / 2.f, ROW_H / 2.f});
        auto mkArrow = [this, tag](float rotation, SEL_MenuHandler sel, float xx) {
            auto sprite = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
            if (!sprite) return (CCMenuItemSpriteExtra*)nullptr;
            sprite->setScale(0.45f);
            sprite->setRotation(rotation);
            auto btn = CCMenuItemSpriteExtra::create(sprite, this, sel);
            btn->setTag(tag);
            btn->setPosition({xx, ROW_H / 2.f});
            return btn;
        };
        if (auto* a = mkArrow(180.f, menu_selector(AISettingsPopup::onCyclePrev), 222.f))
            arrowMenu->addChild(a);
        if (auto* a = mkArrow(  0.f, menu_selector(AISettingsPopup::onCycleNext), 352.f))
            arrowMenu->addChild(a);
        row->addChild(arrowMenu);
        pushRow(row);
    }

    // ── Toggle — real GD checkbox (GJ_checkOn/Off via the standard-sprites
    // toggler). The sid travels in the userObject; the toggler swaps its
    // own sprite, so onToggle just flips the setting.
    void addToggle(const char* lbl, const char* sid) {
        auto row = makeRow(lbl);

        auto menu = CCMenu::create();
        menu->setContentSize({30.f, ROW_H});
        menu->ignoreAnchorPointForPosition(false);
        menu->setAnchorPoint({0.5f, 0.5f});
        menu->setPosition({340.f, ROW_H / 2.f});

        auto tog = CCMenuItemToggler::createWithStandardSprites(this,
            menu_selector(AISettingsPopup::onToggle), 0.5f);
        tog->toggle(geode::Mod::get()->getSettingValue<bool>(sid));
        tog->setUserObject(CCString::create(sid));
        tog->setPosition({15.f, ROW_H / 2.f});
        menu->addChild(tog);
        row->addChild(menu);
        pushRow(row);
    }

    // ── Text input — keeps TextInput's stock dark bg (sits fine on the
    // brown cell; no extra well here).
    void addText(const char* lbl, const char* sid, const char* ph,
                 int maxLen = 200, bool password = false) {
        auto row = makeRow(lbl);
        float w = 150.f;
        auto in = TextInput::create(w, ph, "bigFont.fnt");
        in->setScale(0.6f);
        // Right edge pinned at x=354.
        in->setPosition({354.f - (w * 0.6f) / 2.f, ROW_H / 2.f});
        in->setMaxCharCount(maxLen);
        if (password) in->setPasswordMode(true);
        std::string cur = geode::Mod::get()->getSettingValue<std::string>(sid);
        if (!cur.empty()) in->setString(cur);
        row->addChild(in);
        m_texts.push_back({sid, in, password});
        pushRow(row);
    }

    void addInt(const char* lbl, const char* sid,
                int64_t mn, int64_t mx, int64_t df) {
        auto row = makeRow(lbl);

        std::string hintStr = fmt::format("{}-{}", mn, mx);
        auto hint = CCLabelBMFont::create(hintStr.c_str(), "chatFont.fnt");
        hint->setScale(0.45f);  // chatFont hard floor
        hint->setColor(ui::TEXT_DIM);
        hint->setAnchorPoint({1.f, 0.5f});
        hint->setPosition({292.f, ROW_H / 2.f});
        row->addChild(hint);

        // Materialise the format strings into named locals so the c_str
        // pointer outlives any synchronous internal copy TextInput may do.
        // Width 56 ≥ the documented ~50px TextInput crash floor.
        std::string phStr = fmt::format("{}", df);
        auto in = TextInput::create(56.f, phStr.c_str(), "bigFont.fnt");
        in->setScale(0.6f);
        in->setPosition({326.f, ROW_H / 2.f});
        in->setCommonFilter(geode::CommonFilter::Uint);
        in->setMaxCharCount(7);
        int64_t cur = geode::Mod::get()->getSettingValue<int64_t>(sid);
        in->setString(fmt::format("{}", cur));
        row->addChild(in);
        m_ints.push_back({sid, in, mn, mx, df});
        pushRow(row);
    }

    void addNote(const std::string& txt) {
        auto row = makeBareRow(18.f);
        auto n = CCLabelBMFont::create(txt.c_str(), "chatFont.fnt");
        n->limitLabelWidth(340.f, 0.5f, 0.45f);
        n->setColor(ui::TEXT_SECONDARY);
        n->setPosition({ROW_W / 2.f, 9.f});
        row->addChild(n);
        pushRow(row);
    }

    // goldFont section header flanked by short hairlines.
    void addHeader(const char* txt) {
        auto row = makeBareRow(22.f);
        auto h = CCLabelBMFont::create(txt, "goldFont.fnt");
        h->setScale(0.4f);
        h->setPosition({ROW_W / 2.f, 11.f});
        row->addChild(h);
        float half = h->getScaledContentSize().width / 2.f;
        for (float dir : {-1.f, 1.f}) {
            auto line = CCLayerColor::create({0, 0, 0, 80});
            line->setContentSize({50.f, 1.f});
            line->ignoreAnchorPointForPosition(false);
            line->setAnchorPoint({0.5f, 0.5f});
            line->setPosition({ROW_W / 2.f + dir * (half + 8.f + 25.f), 11.f});
            row->addChild(line);
        }
        pushRow(row);
    }

    // ── Cycler arrow handlers ────────────────────────────────────────────
    // Left arrow steps back, right arrow steps forward; both share
    // cycleStep which updates the saved setting and rewrites the value
    // label's string in place — no widget rebuild needed. ai-provider on
    // the Provider tab triggers a full tab rebuild because the rest of
    // the Provider body depends on the active provider.
    void onCyclePrev(CCObject* sender) { cycleStep(sender, -1); }
    void onCycleNext(CCObject* sender) { cycleStep(sender, +1); }
    void cycleStep(CCObject* sender, int delta) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        int tag = btn->getTag();
        if (tag < 0 || tag >= (int)m_cycles.size()) return;
        auto& info = m_cycles[tag];
        if (info.opts.empty()) return;

        std::string cur = geode::Mod::get()->getSettingValue<std::string>(info.sid);
        int idx = 0;
        for (int i = 0; i < (int)info.opts.size(); ++i)
            if (info.opts[i] == cur) { idx = i; break; }
        int n   = (int)info.opts.size();
        int nxt = ((idx + delta) % n + n) % n;
        const std::string& v = info.opts[nxt];
        geode::Mod::get()->setSettingValue(info.sid, v);

        if (info.lbl) {
            info.lbl->setString(v.c_str());
            // Re-fit: the build-time fit was computed for the previous
            // value; a longer option would overflow into the arrows.
            info.lbl->limitLabelWidth(CYCLE_LBL_W, CYCLE_LBL_SC, CYCLE_LBL_MIN);
        }
        if (info.sid == "ai-provider" && m_tab == Tab::Provider) buildTab();
    }

    void onToggle(CCObject* sender) {
        // CCMenuItemToggler swaps its own checkbox sprite; we only flip the
        // setting. Read the SETTING (not isToggled(), which reports the
        // pre-click state inside the callback) and negate.
        auto btn = static_cast<CCMenuItemToggler*>(sender);
        auto obj = static_cast<CCString*>(btn->getUserObject());
        if (!obj) return;
        std::string sid = obj->getCString();
        bool v = !geode::Mod::get()->getSettingValue<bool>(sid);
        geode::Mod::get()->setSettingValue(sid, v);
    }

    void buildGeneral() {
        // No provider cycler here — it lives on the Provider tab (and the
        // generator's chip jumps straight there). Keeps General scroll-free.
        addText  ("Difficulty", "difficulty", "easy/medium/hard/extreme", 30);
        addText  ("Style",      "style",      "modern/retro/flow/memory", 30);
        addText  ("Length",     "length",     "short/medium/long/xl/xxl", 30);
        addInt   ("Max objects","max-objects", 10, 1000000, 500);
        addInt   ("Spawn speed","spawn-batch-size", 1, 100, 8);
        addInt   ("Ground Y",   "ai-ground-y", 15, 300, 105);
        addText  ("Example IDs","example-level-ids", "up to 5 level IDs, comma-sep", 100);
        addNote  ("Free-form text is OK for difficulty/style/length.");
    }

    // ── Advanced tab — one readable striped column; the ScrollLayer takes
    // care of the overflow (this is the only tab that scrolls).
    void buildAdvanced() {
        addHeader("Prompting");
        addToggle("Enable AI tools",   "enable-ai-tools");
        addToggle("Triggers & colors", "enable-advanced-features");
        addInt   ("Refinement rounds", "refinement-rounds", 0, 10, 3);

        addHeader("Rate & Feedback");
        addToggle("Rate limiting",     "enable-rate-limiting");
        addInt   ("Rate limit (s)",    "rate-limit-seconds", 1, 60, 3);
        addToggle("Save AI output",    "show-ai-output");
        addToggle("Rate generations",  "enable-rating");
        addInt   ("Feedback examples", "max-feedback-examples", 1, 10, 3);

        addHeader("Bypass");
        addToggle("Character filter bypass", "bypass-char-filter");
        addToggle("Character limit bypass",  "bypass-char-limit");
        addNote("Bypasses apply to every text box in the game while enabled.");

        addHeader("Debug");
        {
            auto row = makeRow("Request inspector");
            auto menu = CCMenu::create();
            menu->setContentSize({80.f, ROW_H});
            menu->ignoreAnchorPointForPosition(false);
            menu->setAnchorPoint({0.5f, 0.5f});
            menu->setPosition({330.f, ROW_H / 2.f});
            auto lbl = CCLabelBMFont::create("Open", "goldFont.fnt");
            lbl->setScale(0.45f);
            auto btn = CCMenuItemSpriteExtra::create(lbl, this,
                menu_selector(AISettingsPopup::onOpenInspector));
            btn->setPosition({40.f, ROW_H / 2.f});
            menu->addChild(btn);
            row->addChild(menu);
        }
        addNote("Last 10 API exchanges with latency + bodies; also validates clipboard EAS.");
    }

    void onOpenInspector(CCObject*) { openDebugInspector(); }

    // ── Provider profiles: 3 save/load slots ─────────────────────────────
    void addProfileRows() {
        addHeader("Profiles");
        auto mkSlotRow = [this](const char* label, bool isSave) {
            auto row = makeRow(label);
            auto menu = CCMenu::create();
            menu->setContentSize({110.f, ROW_H});
            menu->ignoreAnchorPointForPosition(false);
            menu->setAnchorPoint({0.5f, 0.5f});
            menu->setPosition({318.f, ROW_H / 2.f});
            for (int slot = 0; slot < 3; ++slot) {
                bool exists = !Mod::get()->getSavedValue<std::string>(
                    fmt::format("provider-profile-{}", slot), "").empty();
                auto lbl = CCLabelBMFont::create(
                    fmt::format("{}", slot + 1).c_str(), "goldFont.fnt");
                lbl->setScale(0.5f);
                if (!isSave && !exists) lbl->setColor({120, 120, 120});
                auto btn = CCMenuItemSpriteExtra::create(lbl, this,
                    isSave ? menu_selector(AISettingsPopup::onProfileSave)
                           : menu_selector(AISettingsPopup::onProfileLoad));
                btn->setTag(slot);
                btn->setPosition({20.f + slot * 35.f, ROW_H / 2.f});
                menu->addChild(btn);
            }
            row->addChild(menu);
        };
        mkSlotRow("Load profile",    false);
        mkSlotRow("Save to profile", true);
        addNote("A profile = provider + model + prompt/tool settings. Save your 'fast draft' and 'best quality' setups.");
    }

    void onProfileSave(CCObject* sender) {
        int slot = static_cast<CCNode*>(sender)->getTag();
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        auto o = matjson::Value::object();
        o["provider"] = provider;
        o["model"]    = Mod::get()->getSettingValue<std::string>(providerModelSettingKey(provider));
        o["tools"]    = Mod::get()->getSettingValue<bool>("enable-ai-tools");
        o["refine"]   = (int)Mod::get()->getSettingValue<int64_t>("refinement-rounds");
        Mod::get()->setSavedValue<std::string>(
            fmt::format("provider-profile-{}", slot), o.dump());
        Notification::create(fmt::format("Profile {} saved ({})", slot + 1, provider),
                             NotificationIcon::Success)->show();
        buildTab();
    }

    void onProfileLoad(CCObject* sender) {
        int slot = static_cast<CCNode*>(sender)->getTag();
        auto raw = Mod::get()->getSavedValue<std::string>(
            fmt::format("provider-profile-{}", slot), "");
        if (raw.empty()) {
            Notification::create("Empty slot - save a profile first",
                                 NotificationIcon::Warning)->show();
            return;
        }
        auto parsed = matjson::parse(raw);
        if (!parsed || !parsed.unwrap().isObject()) {
            Notification::create("Profile data corrupted", NotificationIcon::Error)->show();
            return;
        }
        const auto p = parsed.unwrap();
        auto prov = p["provider"].asString();
        if (prov) {
            Mod::get()->setSettingValue<std::string>("ai-provider", prov.unwrap());
            auto model = p["model"].asString();
            if (model)
                Mod::get()->setSettingValue<std::string>(
                    providerModelSettingKey(prov.unwrap()), model.unwrap());
        }
        auto tools = p["tools"].asBool();
        if (tools) Mod::get()->setSettingValue<bool>("enable-ai-tools", tools.unwrap());
        auto refine = p["refine"].asInt();
        if (refine) Mod::get()->setSettingValue<int64_t>("refinement-rounds", refine.unwrap());
        Notification::create(fmt::format("Profile {} loaded", slot + 1),
                             NotificationIcon::Success)->show();
        buildTab();
    }

    void buildProvider() {
        addProfileRows();
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        // Only HuggingFace ships a button-only OAuth that works from a
        // loopback HTTP listener. OpenRouter's PKCE flow rejects non-https
        // callbacks (per their docs — only https on ports 443/3000); Gemini's
        // OAuth needs a GCP project step. Both stay on the paste-key path.
        bool isOAuth  = (p == "huggingface");
        bool isLocal  = (p == "ollama" || p == "lm-studio"  || p == "llama-cpp");
        bool isCustom = (p == "custom");
        bool isManual = (p == "manual");

        addHeader("Provider");
        addCycler("Provider", "ai-provider",
            {"gemini","claude","openai","openrouter","ministral","huggingface",
             "deepseek","ollama","lm-studio","llama-cpp","custom","manual"});

        addHeader("Model");
        if (p == "gemini")
            // Keep in sync with the one-of list in mod.json.
            addCycler("Model", "gemini-model", {"gemini-3-flash","gemini-3-pro"});
        else if (p == "claude")
            addCycler("Model", "claude-model", {"claude-sonnet-4-6","claude-opus-4-6"});
        else if (p == "openai")
            addCycler("Model", "openai-model", {"gpt-4o","gpt-4.1-mini"});
        else if (p == "ministral")
            addCycler("Model", "ministral-model",
                {"ministral-3b-latest","ministral-8b-latest","mistral-small-latest",
                 "mistral-medium-latest","mistral-large-latest"});
        else if (p == "deepseek")
            addCycler("Model", "deepseek-model",
                {"deepseek-chat","deepseek-reasoner","deepseek-coder"});
        else if (p == "openrouter")
            addText("Model", "openrouter-model", "vendor/model-name", 100);
        else if (p == "huggingface")
            addText("Model", "huggingface-model", "owner/repo", 100);
        else if (p == "ollama") {
            addToggle("Platinum", "use-platinum");
            addText("Tag",         "ollama-model", "entity12208/editorai:v3-7b", 100);
            addInt ("Timeout (s)", "ollama-timeout", 60, 1800, 600);
        } else if (p == "lm-studio") {
            addText("URL",   "lm-studio-url",   "http://localhost:1234", 100);
            addText("Model", "lm-studio-model", "loaded model name",     100);
        } else if (p == "llama-cpp") {
            addText("URL",   "llama-cpp-url",   "http://localhost:8080", 100);
            addText("Model", "llama-cpp-model", "default",               100);
        } else if (isCustom) {
            addText("Name",  "custom-provider-name", "Display name", 60);
            addText("URL",   "custom-provider-url",  "https://...",  150);
            addText("Model", "custom-provider-model","model name",   100);
            addText("Auth",  "custom-provider-auth", "Authorization: Bearer ${KEY}", 150);
        } else if (isManual) {
            addNote("Manual / copy-paste — no key or model. Generate copies a");
            addNote("prompt; paste it into any AI, then paste the reply back.");
        }

        // ── Dynamic model picker (skipped for the manual copy-paste provider) ─
        // Fetch the provider's real model list (no hardcoded options) and show
        // each as a tappable row that writes the per-provider model setting.
        if (!isManual) {
            auto row = makeBareRow(30.f);
            auto menu = CCMenu::create();
            menu->setContentSize({ROW_W, 30.f});
            menu->ignoreAnchorPointForPosition(false);
            menu->setAnchorPoint({0.5f, 0.5f});
            menu->setPosition({ROW_W / 2.f, 15.f});
            auto spr = ButtonSprite::create("Fetch model list", "bigFont.fnt",
                                            "GJ_button_04.png", 0.5f);
            spr->setScale(0.62f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this,
                           menu_selector(AISettingsPopup::onFetchModels));
            btn->setPosition({ROW_W / 2.f, 15.f});
            menu->addChild(btn);
            row->addChild(menu);
            pushRow(row);
        }
        if (auto it = modelCache().find(p); it != modelCache().end() && !it->second.empty()) {
            std::string curModel =
                geode::Mod::get()->getSettingValue<std::string>(providerModelSettingKey(p));
            addNote(fmt::format("{} models - tap to select:", it->second.size()));
            for (const auto& m : it->second) addPickRow(m, m == curModel);
        }

        addHeader("Authentication");
        if (!isLocal && !isManual) {
            std::string keySid = isCustom ? std::string("custom-provider-api-key")
                                          : (p + "-api-key");
            bool hasOAuth = !oauth::savedToken(p).empty();
            const char* ph = hasOAuth ? "(signed in — paste to override)"
                                      : "paste API key";
            addText("API key", keySid.c_str(), ph, 220, true);
        }

        // ── Auth status pill: recessed well + tinted dot + status text ──
        {
            auto row = makeBareRow(22.f);
            auto pill = ui::makeWell(340.f, 18.f, 70);
            pill->setPosition({ROW_W / 2.f, 11.f});
            row->addChild(pill);

            m_authDot = ui::makeStatusDot(ui::TEXT_SECONDARY);
            m_authDot->setPosition({ROW_W / 2.f - 158.f, 11.f});
            row->addChild(m_authDot);

            m_authStatus = CCLabelBMFont::create("", "bigFont.fnt");
            m_authStatus->setScale(0.3f);
            m_authStatus->setColor(ui::TEXT_SECONDARY);
            m_authStatus->setAnchorPoint({0.f, 0.5f});
            m_authStatus->setPosition({ROW_W / 2.f - 146.f, 11.f});
            row->addChild(m_authStatus);
            pushRow(row);
        }

        // ── Action row: the conditional buttons share one RowLayout menu ──
        {
            auto row = makeBareRow(34.f);
            auto menu = CCMenu::create();
            menu->setContentSize({ROW_W, 34.f});
            menu->ignoreAnchorPointForPosition(false);
            menu->setAnchorPoint({0.5f, 0.5f});
            menu->setPosition({ROW_W / 2.f, 17.f});
            menu->setLayout(RowLayout::create()
                ->setGap(8.f)
                ->setAutoScale(false));

            auto mkBtn = [this, menu](const char* lbl, SEL_MenuHandler sel,
                                      const char* tex, const char* font, float s) {
                menu->addChild(CCMenuItemSpriteExtra::create(
                    ButtonSprite::create(lbl, font, tex, s), this, sel));
            };
            if (isOAuth)
                mkBtn("Sign In", menu_selector(AISettingsPopup::onSignIn),
                      "GJ_button_01.png", "goldFont.fnt", 0.5f);
            if (!isLocal && !isCustom && !isManual)
                mkBtn("Key page", menu_selector(AISettingsPopup::onOpenKeyPage),
                      "GJ_button_04.png", "bigFont.fnt", 0.4f);
            if (isLocal)
                mkBtn("Test connection", menu_selector(AISettingsPopup::onTestKey),
                      "GJ_button_01.png", "goldFont.fnt", 0.5f);
            if (!isLocal && !isManual)
                mkBtn("Save & test", menu_selector(AISettingsPopup::onSaveAndTest),
                      "GJ_button_02.png", "goldFont.fnt", 0.5f);
            if (!oauth::savedToken(p).empty())
                mkBtn("Sign out", menu_selector(AISettingsPopup::onSignOut),
                      "GJ_button_06.png", "goldFont.fnt", 0.45f);
            menu->updateLayout();
            row->addChild(menu);
            pushRow(row);
        }

        bool hasOAuth  = !oauth::savedToken(p).empty();
        bool hasManual = !isLocal && !isCustom && !isManual &&
            !geode::Mod::get()->getSettingValue<std::string>(p + "-api-key").empty();
        if      (hasOAuth)  setAuthStatus("✓ Signed in via OAuth.",          ui::SUCCESS_COL);
        else if (hasManual) setAuthStatus("Manual API key set.",             ui::TEXT_SECONDARY);
        else if (isManual)  setAuthStatus("No key needed — copy-paste flow.",ui::TEXT_SECONDARY);
        else if (isLocal)   setAuthStatus("No auth needed — Test to verify.",ui::TEXT_SECONDARY);
        else                setAuthStatus("Not configured.",                 ui::ERROR_COL);
    }

    void setAuthStatus(const std::string& msg, ccColor3B col) {
        if (!m_authStatus) return;
        m_authStatus->setString(msg.c_str());
        m_authStatus->limitLabelWidth(300.f, 0.3f, 0.1f);
        m_authStatus->setColor(col);
        // The dot carries the state too — text glyphs like ✓/✗ don't exist
        // in bigFont and drop silently.
        if (m_authDot) m_authDot->setColor(col);
    }

    void onOpenKeyPage(CCObject*) {
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        const char* url = nullptr;
        if      (p == "gemini")      url = "https://aistudio.google.com/app/apikey";
        else if (p == "claude")      url = "https://console.anthropic.com/settings/keys";
        else if (p == "openai")      url = "https://platform.openai.com/api-keys";
        else if (p == "ministral")   url = "https://console.mistral.ai/api-keys";
        else if (p == "huggingface") url = "https://huggingface.co/settings/tokens";
        else if (p == "openrouter")  url = "https://openrouter.ai/keys";
        else if (p == "deepseek")    url = "https://platform.deepseek.com/api_keys";
        if (url) {
            geode::utils::web::openLinkInBrowser(url);
            setAuthStatus("Browser opened. Paste & Save & test when done.",
                          ui::BUSY_COL);
        }
    }
    void onSaveAndTest(CCObject*) {
        flushInputs();
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string key = geode::Mod::get()->getSettingValue<std::string>(
            p == "custom" ? "custom-provider-api-key" : (p + "-api-key"));
        if (key.empty()) {
            setAuthStatus("No key to test.", ui::ERROR_COL); return;
        }
        oauth::clearSavedToken(p);
        setAuthStatus("Saved. Testing...", ui::BUSY_COL);
        runValidate(p, key);
    }
    void onTestKey(CCObject*) {
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string key = getProviderApiKey(p);
        setAuthStatus("Testing...", ui::BUSY_COL);
        runValidate(p, key);
    }
    void onSignOut(CCObject*) {
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        oauth::clearSavedToken(p);
        setAuthStatus("Signed out.", ui::BUSY_COL);
        buildTab();
    }
    void runValidate(const std::string& provider, const std::string& key) {
        std::string url;
        if      (provider == "ollama")    url = getOllamaUrl() + "/api/tags";
        else if (provider == "lm-studio") url = geode::Mod::get()->getSettingValue<std::string>("lm-studio-url") + "/v1/models";
        else if (provider == "llama-cpp") url = geode::Mod::get()->getSettingValue<std::string>("llama-cpp-url") + "/v1/models";
        else if (provider == "openai")    url = "https://api.openai.com/v1/models";
        else if (provider == "claude")    url = "https://api.anthropic.com/v1/models";
        else if (provider == "ministral") url = "https://api.mistral.ai/v1/models";
        else if (provider == "deepseek")  url = "https://api.deepseek.com/v1/models";
        else if (provider == "huggingface") url = "https://huggingface.co/api/whoami-v2";
        else if (provider == "openrouter")  url = "https://openrouter.ai/api/v1/auth/key";
        else if (provider == "gemini")
            url = "https://generativelanguage.googleapis.com/v1beta/models";
        else { setAuthStatus("✓ Saved.", ui::SUCCESS_COL); return; }

        auto req = web::WebRequest();
        req.timeout(std::chrono::seconds(10));
        // Same headers the real generation requests use.
        applyProviderAuth(req, provider, key);
        m_authNet.spawn(req.get(url),
            [this](web::WebResponse resp) {
                if (resp.ok()) setAuthStatus("✓ Connected.", ui::SUCCESS_COL);
                else setAuthStatus(fmt::format("✗ HTTP {}.", resp.code()),
                                   ui::ERROR_COL);
            });
    }

    // ── Dynamic model list ────────────────────────────────────────────────
    // Fetches the provider's REAL available models from its own API instead of
    // a hardcoded list: /api/tags for Ollama/Platinum, /v1/models for every
    // OpenAI-compatible provider + Claude, /v1beta/models for Gemini, and the
    // trending text-generation list for HuggingFace. Results are cached for the
    // session; the Provider tab renders each as a tappable row.
    static std::unordered_map<std::string, std::vector<std::string>>& modelCache() {
        static std::unordered_map<std::string, std::vector<std::string>> c;
        return c;
    }

    static std::string modelListUrl(const std::string& provider) {
        if (provider == "ollama")     return getOllamaUrl() + "/api/tags";
        if (provider == "lm-studio")  return geode::Mod::get()->getSettingValue<std::string>("lm-studio-url") + "/v1/models";
        if (provider == "llama-cpp")  return geode::Mod::get()->getSettingValue<std::string>("llama-cpp-url") + "/v1/models";
        if (provider == "openai")     return "https://api.openai.com/v1/models";
        if (provider == "claude")     return "https://api.anthropic.com/v1/models";
        if (provider == "ministral")  return "https://api.mistral.ai/v1/models";
        if (provider == "deepseek")   return "https://api.deepseek.com/v1/models";
        if (provider == "openrouter") return "https://openrouter.ai/api/v1/models";
        if (provider == "gemini")     return "https://generativelanguage.googleapis.com/v1beta/models";
        if (provider == "huggingface")
            return "https://huggingface.co/api/models?pipeline_tag=text-generation&sort=trending&limit=60";
        if (provider == "custom") {
            std::string base = geode::Mod::get()->getSettingValue<std::string>("custom-provider-url");
            if (base.empty()) return "";
            auto pos = base.find("/chat/completions");
            if (pos != std::string::npos) base = base.substr(0, pos);
            while (!base.empty() && base.back() == '/') base.pop_back();
            auto endsWith = [&](const char* s){ std::string t = s; return base.size() >= t.size() && base.compare(base.size() - t.size(), t.size(), t) == 0; };
            if (endsWith("/v1/models")) return base;
            if (endsWith("/v1"))        return base + "/models";
            return base + "/v1/models";
        }
        return "";
    }

    // Normalize a model-list response into a deduped, sorted, capped id list.
    // Passed by value so internal access is non-const (matjson operator[]).
    static std::vector<std::string> parseModelList(matjson::Value json) {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        auto add = [&](std::string id) {
            if (id.empty()) return;
            if (id.rfind("models/", 0) == 0) id = id.substr(7);  // gemini: "models/gemini-…"
            if (seen.insert(id).second) out.push_back(std::move(id));
        };
        auto scanArray = [&](matjson::Value& arr, const char* field) {
            for (size_t i = 0; i < arr.size(); ++i) {
                // const ref → const operator[] returns null on a missing key
                // instead of INSERTING into the parsed response (house rule).
                const matjson::Value& e = arr[i];
                if (e.isString()) { add(e.asString().unwrapOr("")); continue; }
                if (!e.isObject()) continue;
                if (auto r = e[field].asString()) add(r.unwrap());
            }
        };
        if (json.contains("data") && json["data"].isArray())          // OpenAI-compat + Claude + OpenRouter
            scanArray(json["data"], "id");
        else if (json.contains("models") && json["models"].isArray()) // Ollama (name) + Gemini (name)
            scanArray(json["models"], "name");
        else if (json.isArray())                                      // HuggingFace trending
            scanArray(json, "id");
        std::sort(out.begin(), out.end());
        if (out.size() > 80) out.resize(80);  // keep the picker scrollable, not endless
        return out;
    }

    void onFetchModels(CCObject*) {
        flushInputs();   // persist typed key/url/model before any rebuild
        std::string provider = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string url = modelListUrl(provider);
        if (url.empty()) {
            setAuthStatus("Set the endpoint URL first.", ui::ERROR_COL); return;
        }
        std::string key = getProviderApiKey(provider);
        setAuthStatus("Fetching models…", ui::BUSY_COL);
        auto req = web::WebRequest();
        req.timeout(std::chrono::seconds(15));
        applyProviderAuth(req, provider, key);
        m_modelNet.spawn(req.get(url),
            [this, provider](web::WebResponse resp) {
                if (!resp.ok()) {
                    setAuthStatus(fmt::format("✗ Models HTTP {}.", resp.code()), ui::ERROR_COL);
                    return;
                }
                auto jr = resp.json();
                if (!jr) { setAuthStatus("✗ Bad model-list JSON.", ui::ERROR_COL); return; }
                auto list = parseModelList(std::move(jr).unwrap());
                if (list.empty()) { setAuthStatus("No models returned.", ui::ERROR_COL); return; }
                size_t n = list.size();
                modelCache()[provider] = std::move(list);
                buildTab();   // re-render the Provider tab with the selectable rows
                setAuthStatus(fmt::format("✓ {} models — tap one to use it.", n), ui::SUCCESS_COL);
            });
    }

    void onPickModel(CCObject* sender) {
        auto* node = static_cast<CCNode*>(sender);
        auto* str  = static_cast<CCString*>(node->getUserObject());
        if (!str) return;
        std::string model = str->getCString();
        flushInputs();   // save other fields first…
        std::string provider = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        geode::Mod::get()->setSettingValue<std::string>(
            providerModelSettingKey(provider), model);   // …then override the model
        buildTab();
        setAuthStatus(fmt::format("Model set: {}", model), ui::SUCCESS_COL);
    }

    // One tappable model row — full name in the userObject, a bullet on the
    // currently-selected one, display truncated so long ids don't overflow.
    void addPickRow(const std::string& model, bool current) {
        auto row = makeRow("");
        auto menu = CCMenu::create();
        menu->setContentSize({ROW_W, ROW_H});
        menu->ignoreAnchorPointForPosition(false);
        menu->setAnchorPoint({0.5f, 0.5f});
        menu->setPosition({ROW_W / 2.f, ROW_H / 2.f});
        std::string shown = model.size() > 34 ? model.substr(0, 33) + "…" : model;
        if (current) shown = "● " + shown;
        auto spr = ButtonSprite::create(shown.c_str(), "bigFont.fnt",
                       current ? "GJ_button_02.png" : "GJ_button_05.png", 0.5f);
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this,
                       menu_selector(AISettingsPopup::onPickModel));
        btn->setUserObject(CCString::create(model));
        btn->setPosition({ROW_W / 2.f, ROW_H / 2.f});
        menu->addChild(btn);
        row->addChild(menu);
        pushRow(row);
    }

    void onSignIn(CCObject*) {
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        if (p == "huggingface") startPKCE(p);
        else setAuthStatus("This provider has no OAuth flow.", ui::ERROR_COL);
    }

    void startPKCE(const std::string& provider) {
        if (provider == "huggingface" && std::string(oauth::HF_CLIENT_ID).empty()) {
            setAuthStatus("Add an HF client_id in oauth namespace first.",
                          ui::ERROR_COL); return;
        }
        m_pkceListener = std::make_unique<oauth::LocalCallback>();
        int port = 0;
        // HuggingFace demands the exact redirect_uri it has on file. Bind to
        // the registered port (53947) and surface a clear "another app is
        // using it" message if the bind fails, rather than silently picking
        // a random port the OAuth registration wouldn't accept.
        if (!m_pkceListener->start(oauth::HF_REDIRECT_PORT, port)) {
            setAuthStatus(fmt::format(
                "Port {} is busy — close whatever app is using it and retry.",
                oauth::HF_REDIRECT_PORT), ui::ERROR_COL);
            m_pkceListener.reset(); return;
        }
        m_pkcePort     = port;
        m_pkceVerifier = oauth::randomVerifier();
        m_pkceState    = oauth::randomVerifier().substr(0, 16);
        std::string redirect  = fmt::format("http://127.0.0.1:{}/cb", port);
        std::string challenge = oauth::s256Challenge(m_pkceVerifier);
        // HuggingFace is the only provider here — kept the function name
        // generic in case we add another PKCE provider later.
        std::string authUrl = fmt::format(
            "https://huggingface.co/oauth/authorize?client_id={}"
            "&redirect_uri={}&response_type=code&scope={}"
            "&state={}&code_challenge={}&code_challenge_method=S256",
            oauth::HF_CLIENT_ID, redirect, oauth::HF_SCOPES,
            m_pkceState, challenge);
        (void)provider;  // kept in signature for future PKCE additions
        setAuthStatus("Authorise in browser...", ui::BUSY_COL);
        geode::utils::web::openLinkInBrowser(authUrl);
        m_pkceElapsed = 0.f;
        this->schedule(schedule_selector(AISettingsPopup::tickPkce), 0.5f);
    }
    void tickPkce(float dt) {
        // Give up after 5 minutes — otherwise an abandoned browser tab leaves
        // the loopback server and this tick running until the popup closes.
        m_pkceElapsed += dt;
        if (m_pkceElapsed > 300.f) {
            if (m_pkceListener) { m_pkceListener->stop(); m_pkceListener.reset(); }
            this->unschedule(schedule_selector(AISettingsPopup::tickPkce));
            setAuthStatus("Sign-in timed out — try again.", ui::ERROR_COL);
            Notification::create("HuggingFace sign-in timed out.",
                                 NotificationIcon::Error)->show();
            return;
        }
        if (!m_pkceListener || !m_pkceListener->done()) return;
        std::string q = m_pkceListener->query();
        m_pkceListener->stop();
        m_pkceListener.reset();
        this->unschedule(schedule_selector(AISettingsPopup::tickPkce));
        auto getQ = [&](const char* k)->std::string {
            std::string key = std::string(k) + "=";
            auto pos = q.find(key); if (pos == std::string::npos) return "";
            auto end = q.find('&', pos);
            return q.substr(pos + key.size(),
                end == std::string::npos ? std::string::npos : end - pos - key.size());
        };
        std::string code  = getQ("code");
        std::string state = getQ("state");
        if (code.empty()) { setAuthStatus("No code returned.", ui::ERROR_COL); return; }
        if (state != m_pkceState) {
            setAuthStatus("State mismatch.", ui::ERROR_COL); return;
        }
        setAuthStatus("Exchanging code...", ui::BUSY_COL);
        std::string redirect = fmt::format("http://127.0.0.1:{}/cb", m_pkcePort);
        std::string body = fmt::format(
            "client_id={}&grant_type=authorization_code&code={}"
            "&redirect_uri={}&code_verifier={}",
            oauth::HF_CLIENT_ID, code, redirect, m_pkceVerifier);
        auto req = web::WebRequest();
        req.header("Content-Type", "application/x-www-form-urlencoded");
        req.timeout(std::chrono::seconds(30));  // the one request that had no timeout
        req.bodyString(body);
        m_authNet.spawn(req.post("https://huggingface.co/oauth/token"),
            [this](web::WebResponse resp) {
                auto jr = resp.json();
                if (!resp.ok() || !jr) {
                    setAuthStatus(fmt::format("HTTP {}.", resp.code()),
                                  ui::ERROR_COL); return;
                }
                const auto json = jr.unwrap();
                std::string t = json["access_token"].asString().unwrapOr("");
                if (t.empty()) { setAuthStatus("Empty token.", ui::ERROR_COL); return; }
                oauth::setSavedToken("huggingface", t);
                // Record expiry (if the server reports one) so an expired
                // token stops shadowing a manually pasted API key.
                int64_t expiresIn = json["expires_in"].asInt().unwrapOr(0);
                oauth::setSavedExpiry("huggingface", expiresIn);
                setAuthStatus("✓ Signed in.", ui::SUCCESS_COL);
                // The status label is Provider-tab-local (null elsewhere) —
                // a global toast makes the outcome visible from any tab.
                Notification::create("Signed in to HuggingFace!",
                                     NotificationIcon::Success)->show();
                buildTab();
            });
    }

public:
    static AISettingsPopup* create(Tab startTab = Tab::General) {
        auto p = new AISettingsPopup();
        if (p->init(startTab)) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }
};


// ─── Main generation popup ────────────────────────────────────────────────────

// ─── Request inspector log ───────────────────────────────────────────────────
// Ring buffer of the last API exchanges (request + response, truncated).
// Filled by the generator's send/receive paths; surfaced by the debug popup.
struct RequestLogEntry {
    std::string provider, model, url;
    std::string requestBody;   // truncated to 4 KB
    std::string responseBody;  // truncated to 4 KB
    int         httpCode  = 0;
    float       latencyMs = 0.f;
    int64_t     timestamp = 0;
};
static std::deque<RequestLogEntry> s_requestLog;
static std::chrono::steady_clock::time_point s_requestStart;
static bool s_requestPending = false;

static void logApiRequest(const std::string& provider, const std::string& model,
                          const std::string& url, const std::string& body) {
    RequestLogEntry e;
    e.provider = provider;
    e.model    = model;
    e.url      = url;
    e.requestBody = body.size() > 4096 ? body.substr(0, 4096) + "\n...(truncated)" : body;
    e.timestamp = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    s_requestLog.push_back(std::move(e));
    // 7 = what the inspector popup's touch rect fits (178px / 24px rows).
    while (s_requestLog.size() > 7) s_requestLog.pop_front();
    s_requestStart   = std::chrono::steady_clock::now();
    s_requestPending = true;
}

static void logApiResponse(int code, const std::string& body) {
    if (!s_requestPending || s_requestLog.empty()) return;
    s_requestPending = false;
    auto& e = s_requestLog.back();
    e.httpCode  = code;
    e.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - s_requestStart).count();
    e.responseBody = body.size() > 4096 ? body.substr(0, 4096) + "\n...(truncated)" : body;
}

// ─── EAS dry-run validation (no editor mutation, no preview) ─────────────────
// Parses + macro-expands + sanity-checks an EAS buffer and reports without
// touching the editor. Powers the "Validate clipboard EAS" debug action.
static std::string validateEASReport(const std::string& src) {
    auto er = eas::parse(eas::extractScript(src));
    if (!er.ok)
        return fmt::format("PARSE FAILED: {}", er.error.empty() ? "unknown error" : er.error);

    auto objectsArray = er.root.contains("objects") && er.root["objects"].isArray()
        ? er.root["objects"] : matjson::Value::array();
    size_t directObjects = objectsArray.size();
    size_t macroCount = 0;
    if (er.root.contains("macros") && er.root["macros"].isArray()) {
        macroCount = er.root["macros"].size();
        std::vector<matjson::Value> expanded;
        macros::expandAll(er.root["macros"], expanded);
        for (auto& obj : expanded) objectsArray.push(std::move(obj));
    }

    // Type resolution sweep.
    size_t unknown = 0;
    std::string unknownSample;
    for (size_t i = 0; i < objectsArray.size(); ++i) {
        const auto& elem = objectsArray[i];  // const: key reads must not insert
        auto t = elem["type"].asString();
        if (!t) continue;
        const std::string& name = t.unwrap();
        bool known = OBJECT_IDS.count(name) > 0
                  || name.find("_trigger") != std::string::npos;
        if (!known) {
            ++unknown;
            if (unknownSample.size() < 120) {
                if (!unknownSample.empty()) unknownSample += ", ";
                unknownSample += name;
            }
        }
    }

    float maxX = computeMaxXFromObjects(objectsArray);
    auto pass = levelcheck::check(objectsArray);
    const char* tier = maxX < 3115 ? "Tiny" : maxX < 9347 ? "Short"
                     : maxX < 18693 ? "Medium" : maxX < 37387 ? "Long" : "XL";

    std::string report = fmt::format(
        "<cg>Parsed OK.</c>\n"
        "Objects: {} direct + {} macro(s) -> {} total\n"
        "Max X: {:.0f} ({} tier, ~{:.0f}s at 1x)\n"
        "Passability: {:.1f}%",
        directObjects, macroCount, objectsArray.size(),
        maxX, tier, maxX / 311.58f, pass.pass_rate * 100.f);
    if (unknown > 0)
        report += fmt::format("\n<cr>{} unknown type(s):</c> {}", unknown, unknownSample);
    if (!pass.deaths.empty())
        report += fmt::format("\n<cy>{} death zone(s)</c> - first at X={:.0f}",
                              pass.deaths.size(), pass.deaths[0].x_start);
    return report;
}

// ─── Prompt presets (saved) + built-in templates ─────────────────────────────
// Presets persist through Geode saved values (no manual file I/O; Geode
// serializes the container off the hot path). Capped at 6 — a curated strip,
// not a database.
struct PromptPreset {
    std::string name, prompt, difficulty, style, length;
};

static std::vector<PromptPreset> loadPromptPresets() {
    std::vector<PromptPreset> out;
    auto raw = Mod::get()->getSavedValue<std::string>("prompt-presets", "");
    if (raw.empty()) return out;
    auto parsed = matjson::parse(raw);
    if (!parsed || !parsed.unwrap().isArray()) return out;
    const auto arr = parsed.unwrap();
    for (size_t i = 0; i < arr.size() && out.size() < 6; ++i) {
        const auto& it = arr[i];
        PromptPreset p;
        auto n = it["name"].asString();       if (n) p.name       = n.unwrap();
        auto q = it["prompt"].asString();     if (q) p.prompt     = q.unwrap();
        auto d = it["difficulty"].asString(); if (d) p.difficulty = d.unwrap();
        auto s = it["style"].asString();      if (s) p.style      = s.unwrap();
        auto l = it["length"].asString();     if (l) p.length     = l.unwrap();
        if (!p.prompt.empty()) out.push_back(std::move(p));
    }
    return out;
}

static void savePromptPresets(const std::vector<PromptPreset>& presets) {
    auto arr = matjson::Value::array();
    for (auto& p : presets) {
        auto o = matjson::Value::object();
        o["name"] = p.name;         o["prompt"] = p.prompt;
        o["difficulty"] = p.difficulty;
        o["style"] = p.style;       o["length"] = p.length;
        arr.push(o);
    }
    Mod::get()->setSavedValue<std::string>("prompt-presets", arr.dump());
}

// base64url decoder (inverse of oauth::b64url). Returns empty on any
// malformed input — never throws.
static std::vector<uint8_t> b64urlDecode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) return {};
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// ── Recipe cards: prompt+settings bundled into one shareable code ───────────
static constexpr const char* RECIPE_PREFIX = "EAIR1:";

static std::string buildRecipeCode(const std::string& prompt) {
    auto o = matjson::Value::object();
    o["v"]          = 1;
    o["prompt"]     = prompt;
    o["difficulty"] = Mod::get()->getSettingValue<std::string>("difficulty");
    o["style"]      = Mod::get()->getSettingValue<std::string>("style");
    o["length"]     = Mod::get()->getSettingValue<std::string>("length");
    auto ids = Mod::get()->getSettingValue<std::string>("example-level-ids");
    if (!ids.empty()) o["example_ids"] = ids;
    std::string json = o.dump();
    return std::string(RECIPE_PREFIX)
         + oauth::b64url((const uint8_t*)json.data(), json.size());
}

// Applies a recipe code; returns false if it isn't one / can't be decoded.
static bool applyRecipeCode(const std::string& code, std::string& outPrompt) {
    if (code.rfind(RECIPE_PREFIX, 0) != 0) return false;
    auto bytes = b64urlDecode(code.substr(strlen(RECIPE_PREFIX)));
    if (bytes.empty()) return false;
    auto parsed = matjson::parse(std::string_view((const char*)bytes.data(), bytes.size()));
    if (!parsed || !parsed.unwrap().isObject()) return false;
    const auto r = parsed.unwrap();
    auto p = r["prompt"].asString();
    if (!p) return false;
    outPrompt = p.unwrap();
    auto d = r["difficulty"].asString();
    if (d) Mod::get()->setSettingValue<std::string>("difficulty", d.unwrap());
    auto s = r["style"].asString();
    if (s) Mod::get()->setSettingValue<std::string>("style", s.unwrap());
    auto l = r["length"].asString();
    if (l) Mod::get()->setSettingValue<std::string>("length", l.unwrap());
    auto ids = r["example_ids"].asString();
    if (ids) Mod::get()->setSettingValue<std::string>("example-level-ids", ids.unwrap());
    return true;
}

// Built-in prompt templates with {a|b|c} choice groups — each tap re-rolls
// the slots, so the same card yields endless concrete prompts.
static const char* PROMPT_TEMPLATES[] = {
    "A {easy|medium|hard} {cube|ship|wave|mixed-gamemode} level with {neon|retro|dark cave|sunset|underwater} visuals and {spike gauntlets|saw corridors|orb chains|tight ship tunnels}",
    "A {short|medium-length} {flow|bouncy|rhythmic} level synced to a {120|140|160} bpm feel with {pulsing colors|camera zooms on drops|building intensity}",
    "A {beginner-friendly|relaxing} auto-scrolling showcase with {lush decoration|minimalist shapes|glowing outlines} and almost no difficulty",
    "A {hard|insane} {wave spam|ship straight-fly|timing-heavy cube} challenge section, {300|600} units long, fair but punishing",
    "A {memory|maze}-style level with {teleport pairs|hidden paths revealed by toggle triggers|fake spikes}",
    "A boss-rush feel: {dramatic camera work|screen shake on hits|dark palette with red accents}, alternating {cube and ship|ball and wave} sections",
};

static std::string expandPromptTemplate(const char* tmpl) {
    static std::mt19937 rng{std::random_device{}()};
    std::string in(tmpl), out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '{') { out += in[i++]; continue; }
        auto close = in.find('}', i);
        if (close == std::string::npos) { out += in.substr(i); break; }
        std::string group = in.substr(i + 1, close - i - 1);
        std::vector<std::string> opts;
        size_t s = 0;
        while (s <= group.size()) {
            auto bar = group.find('|', s);
            if (bar == std::string::npos) { opts.push_back(group.substr(s)); break; }
            opts.push_back(group.substr(s, bar - s));
            s = bar + 1;
        }
        if (!opts.empty())
            out += opts[std::uniform_int_distribution<size_t>(0, opts.size() - 1)(rng)];
        i = close + 1;
    }
    return out;
}

// ─── Request Inspector popup ─────────────────────────────────────────────────
class DebugInspectorPopup : public Popup {
protected:
    static constexpr float W = 410.f, H = 274.f;
    // Snapshot at init - the live deque shifts if a generation fires while
    // the inspector is open.
    std::vector<RequestLogEntry> m_log;

    bool init() override {
        if (!Popup::init(W, H)) return false;
        this->setID("debug-inspector-popup"_spr);
        this->setTitle("Request Inspector");
        ui::styleTitle(m_title, 0.8f);
        ui::addGroove(m_mainLayer, 360.f, W / 2.f, H - 36.f);

        m_log.assign(s_requestLog.begin(), s_requestLog.end());
        if (m_log.empty()) {
            auto l = CCLabelBMFont::create("No API requests yet this session.",
                                           "chatFont.fnt");
            l->setScale(0.6f);
            l->setColor(ui::TEXT_SECONDARY);
            l->setPosition({W / 2.f, H / 2.f});
            m_mainLayer->addChild(l);
        } else {
            constexpr float LIST_W = 374.f, ROW = 24.f;
            float top = H - 56.f;
            auto menu = CCMenu::create();
            menu->setContentSize({LIST_W, top - 40.f});
            menu->setPosition({(W - LIST_W) / 2.f, 40.f});
            int row = 0;
            // Newest first.
            for (auto it = m_log.rbegin(); it != m_log.rend(); ++it, ++row) {
                float y = top - 40.f - (row + 0.5f) * ROW;
                auto cell = ui::makeCell(LIST_W, ROW - 2.f, row % 2 == 1);
                cell->setPosition({W / 2.f, 40.f + y});
                m_mainLayer->addChild(cell, -1);

                bool ok = it->httpCode >= 200 && it->httpCode < 300;
                auto dot = ui::makeStatusDot(ok ? ui::SUCCESS_COL : ui::ERROR_COL, 0.5f);
                dot->setPosition({(W - LIST_W) / 2.f + 12.f, 40.f + y});
                m_mainLayer->addChild(dot);

                auto lbl = CCLabelBMFont::create(
                    fmt::format("{} {} - HTTP {} - {:.0f}ms - {:.1f}KB out",
                        it->provider, it->model, it->httpCode, it->latencyMs,
                        it->requestBody.size() / 1024.0).c_str(),
                    "chatFont.fnt");
                lbl->limitLabelWidth(LIST_W - 40.f, 0.5f, 0.35f);
                lbl->setAnchorPoint({0.f, 0.5f});
                lbl->setColor(ui::TEXT_PRIMARY);
                auto btn = CCMenuItemSpriteExtra::create(lbl, this,
                    menu_selector(DebugInspectorPopup::onEntry));
                btn->setTag(row);
                btn->setAnchorPoint({0.f, 0.5f});
                btn->setPosition({26.f, y});
                menu->addChild(btn);
            }
            m_mainLayer->addChild(menu);
        }

        // Bottom actions
        auto barMenu = CCMenu::create();
        barMenu->setContentSize({320.f, 24.f});
        barMenu->ignoreAnchorPointForPosition(false);
        barMenu->setAnchorPoint({0.5f, 0.5f});
        barMenu->setPosition({W / 2.f, 20.f});
        auto mk = [&](const char* text, SEL_MenuHandler sel, float x) {
            auto l = CCLabelBMFont::create(text, "goldFont.fnt");
            l->setScale(0.4f);
            auto b = CCMenuItemSpriteExtra::create(l, this, sel);
            b->setPosition({x, 12.f});
            barMenu->addChild(b);
        };
        mk("Copy Last Exchange", menu_selector(DebugInspectorPopup::onCopyLast), 80.f);
        mk("Validate Clipboard EAS", menu_selector(DebugInspectorPopup::onValidate), 240.f);
        m_mainLayer->addChild(barMenu);
        return true;
    }

    void onEntry(CCObject* sender) {
        int row = static_cast<CCNode*>(sender)->getTag();
        if (row < 0 || row >= (int)m_log.size()) return;
        const auto& e = *(m_log.rbegin() + row);
        std::string body = fmt::format(
            "<cy>{} {}</c>\nURL: {}\nHTTP {} in {:.0f}ms\n\n"
            "<cg>Request ({} chars):</c>\n{}\n\n<cg>Response ({} chars):</c>\n{}",
            e.provider, e.model, e.url, e.httpCode, e.latencyMs,
            e.requestBody.size(),
            e.requestBody.substr(0, 700),
            e.responseBody.size(),
            e.responseBody.substr(0, 700));
        FLAlertLayer::create(nullptr, "Exchange", body, "OK", nullptr, 400.f)->show();
    }

    void onCopyLast(CCObject*) {
        if (m_log.empty()) {
            Notification::create("Nothing logged yet", NotificationIcon::Warning)->show();
            return;
        }
        const auto& e = m_log.back();
        utils::clipboard::write(fmt::format(
            "=== EditorAI exchange ===\n{} {} -> HTTP {} in {:.0f}ms\nURL: {}\n\n"
            "--- REQUEST ---\n{}\n\n--- RESPONSE ---\n{}",
            e.provider, e.model, e.httpCode, e.latencyMs, e.url,
            e.requestBody, e.responseBody));
        Notification::create("Exchange copied", NotificationIcon::Success)->show();
    }

    void onValidate(CCObject*) {
        std::string clip = utils::clipboard::read();
        if (clip.empty()) {
            Notification::create("Clipboard is empty", NotificationIcon::Warning)->show();
            return;
        }
        FLAlertLayer::create(nullptr, "EAS Validation",
                             validateEASReport(clip), "OK", nullptr, 380.f)->show();
    }

public:
    static DebugInspectorPopup* create() {
        auto ret = new DebugInspectorPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

static void openDebugInspector() { DebugInspectorPopup::create()->show(); }

// ─── Palette Designer popup ──────────────────────────────────────────────────
// 12 curated palettes rendered as tappable swatch cards. Picking one locks
// COLOR lines into the next generation's prompt so the AI designs around the
// user's palette instead of inventing its own.
struct PaletteDef {
    const char* name;
    const char* bg;      // ch 1000
    const char* ground;  // ch 1001
    const char* line;    // ch 1002
    const char* accent;  // ch 10
    bool accentBlend;
};
static constexpr PaletteDef PALETTES[] = {
    {"Dark Cave",  "0a0a1a", "1a1a2a", "8090a0", "00ccff", true},
    {"Neon Night", "050020", "120038", "ff00ff", "00ffff", true},
    {"Sunset",     "ff7733", "cc4422", "ffeebb", "ffd700", false},
    {"Underwater", "0a2a4c", "1a3a8c", "66aadd", "4488cc", true},
    {"Industrial", "2a2a2a", "3a3a3a", "777777", "ff6633", false},
    {"Candy",      "ffd6ec", "ff9ad5", "ffffff", "7a3df0", false},
    {"Monochrome", "101010", "303030", "ffffff", "cccccc", false},
    {"Inferno",    "1a0000", "4d0a00", "ff5500", "ffaa00", true},
    {"Glacier",    "dff3ff", "a8d8f0", "5599cc", "2266aa", false},
    {"Forest",     "0c2210", "1d4422", "7abf6a", "c4ff7a", false},
    {"Deep Space", "030308", "0d0d22", "5050a0", "b070ff", true},
    {"Retro Game", "101840", "283090", "f0f0d8", "f05050", false},
};

class PalettePopup : public Popup {
protected:
    std::function<void(const std::string&, const std::string&)> m_onPick;
    static constexpr float W = 410.f, H = 250.f;

    bool init() override {
        if (!Popup::init(W, H)) return false;
        this->setID("palette-popup"_spr);
        this->setTitle("Palette Designer");
        ui::styleTitle(m_title, 0.8f);
        ui::addGroove(m_mainLayer, 360.f, W / 2.f, H - 36.f);

        // 4×3 grid of swatch cards. Each card: BG-colored plate, accent
        // stripe, name label.
        constexpr float CARD_W = 88.f, CARD_H = 50.f, GAP = 6.f;
        constexpr int COLS = 4;
        float gridW = COLS * CARD_W + (COLS - 1) * GAP;
        float left = (W - gridW) / 2.f;
        auto menu = CCMenu::create();
        menu->setContentSize({W, H});
        menu->setPosition({0.f, 0.f});

        auto hexToColor = [](const char* hex) -> ccColor3B {
            GLubyte r = 255, g = 255, b = 255;
            parseHexColor(hex, r, g, b);
            return {r, g, b};
        };

        for (int i = 0; i < (int)(sizeof(PALETTES) / sizeof(*PALETTES)); ++i) {
            int col = i % COLS, row = i / COLS;
            float cx = left + col * (CARD_W + GAP) + CARD_W / 2.f;
            float cy = H - 66.f - row * (CARD_H + GAP);

            // Card visual is a container node the button wraps whole.
            auto card = CCNode::create();
            card->setContentSize({CARD_W, CARD_H});
            card->setAnchorPoint({0.5f, 0.5f});

            auto plate = CCScale9Sprite::create("square02b_001.png");
            plate->setContentSize({CARD_W, CARD_H});
            plate->setColor(hexToColor(PALETTES[i].bg));
            plate->setOpacity(235);
            plate->setPosition({CARD_W / 2.f, CARD_H / 2.f});
            card->addChild(plate);

            auto stripe = CCScale9Sprite::create("square02b_001.png");
            stripe->setContentSize({CARD_W - 12.f, 6.f});
            stripe->setColor(hexToColor(PALETTES[i].accent));
            stripe->setPosition({CARD_W / 2.f, 14.f});
            card->addChild(stripe);

            auto name = CCLabelBMFont::create(PALETTES[i].name, "bigFont.fnt");
            name->limitLabelWidth(CARD_W - 10.f, 0.32f, 0.18f);
            name->setColor(ui::TEXT_PRIMARY);
            name->setPosition({CARD_W / 2.f, CARD_H - 16.f});
            card->addChild(name);

            auto btn = CCMenuItemSpriteExtra::create(card, this,
                menu_selector(PalettePopup::onPick));
            btn->setTag(i);
            btn->setPosition({cx, cy});
            menu->addChild(btn);
        }
        m_mainLayer->addChild(menu);

        auto hint = CCLabelBMFont::create(
            "Locks these colors into the next generation", "chatFont.fnt");
        hint->setScale(0.5f);
        hint->setColor(ui::TEXT_SECONDARY);
        hint->setPosition({W / 2.f, 18.f});
        m_mainLayer->addChild(hint);
        return true;
    }

    void onPick(CCObject* sender) {
        int i = static_cast<CCNode*>(sender)->getTag();
        if (i < 0 || i >= (int)(sizeof(PALETTES) / sizeof(*PALETTES))) return;
        const auto& p = PALETTES[i];
        std::string colorLines = fmt::format(
            "COLOR ch=1000 hex={}\nCOLOR ch=1001 hex={}\nCOLOR ch=1002 hex={}\n"
            "COLOR ch=10 hex={}{}",
            p.bg, p.ground, p.line, p.accent, p.accentBlend ? " blend" : "");
        if (m_onPick) m_onPick(p.name, colorLines);
        this->onClose(nullptr);
    }

public:
    static PalettePopup* create(
        std::function<void(const std::string&, const std::string&)> onPick) {
        auto ret = new PalettePopup();
        ret->m_onPick = std::move(onPick);
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ─── Prompt Library popup: presets (left) + templates (right) ────────────────
class PromptLibraryPopup : public Popup {
protected:
    std::string m_currentPrompt;
    std::function<void(const PromptPreset&)> m_onApply;
    std::vector<PromptPreset> m_presets;
    CCNode* m_presetList = nullptr;

    static constexpr float W = 410.f, H = 264.f;
    static constexpr float COL_W = 184.f, ROW_H = 26.f;
    static constexpr float LIST_TOP = H - 70.f;

    bool init() override {
        if (!Popup::init(W, H)) return false;
        this->setID("prompt-library-popup"_spr);
        this->setTitle("Prompt Library");
        ui::styleTitle(m_title, 0.8f);
        ui::addGroove(m_mainLayer, 360.f, W / 2.f, H - 36.f);

        m_presets = loadPromptPresets();

        auto mkHeader = [this](const char* text, float cx) {
            auto h = CCLabelBMFont::create(text, "bigFont.fnt");
            h->setScale(0.33f);
            h->setColor(ui::TEXT_SECONDARY);
            h->setPosition({cx, H - 52.f});
            m_mainLayer->addChild(h);
        };
        mkHeader("MY PRESETS", 14.f + COL_W / 2.f);
        mkHeader("TEMPLATES", W - 14.f - COL_W / 2.f);

        rebuildPresetList();

        // Templates column — static, one row per card. Cells live on a
        // plain background node (CCMenu children must be CCMenuItems only).
        {
            float colX = W - 14.f - COL_W / 2.f;
            auto bg = CCNode::create();
            bg->setContentSize({COL_W, H - 80.f});
            bg->setAnchorPoint({0.5f, 1.f});
            bg->ignoreAnchorPointForPosition(false);
            bg->setPosition({colX, LIST_TOP + 8.f});
            auto menu = CCMenu::create();
            menu->setContentSize({COL_W, H - 80.f});
            menu->ignoreAnchorPointForPosition(false);
            menu->setAnchorPoint({0.5f, 1.f});
            menu->setPosition({colX, LIST_TOP + 8.f});
            int idx = 0;
            for (auto* tmpl : PROMPT_TEMPLATES) {
                float y = (H - 80.f) - (idx + 0.5f) * ROW_H;
                auto cell = ui::makeCell(COL_W, ROW_H - 3.f, idx % 2 == 1);
                cell->setPosition({COL_W / 2.f, y});
                bg->addChild(cell);
                // First few words as the card label, ellipsised.
                std::string label(tmpl);
                if (auto brace = label.find('{'); brace != std::string::npos && brace > 2)
                    label = label.substr(0, brace - 1) + "...";
                if (label.size() > 34) { label.resize(33); label += "..."; }
                auto l = CCLabelBMFont::create(label.c_str(), "chatFont.fnt");
                l->limitLabelWidth(COL_W - 16.f, 0.55f, 0.45f);
                l->setColor(ui::TEXT_PRIMARY);
                auto btn = CCMenuItemSpriteExtra::create(l, this,
                    menu_selector(PromptLibraryPopup::onTemplateTap));
                btn->setTag(idx);
                btn->setPosition({COL_W / 2.f, y});
                menu->addChild(btn);
                ++idx;
            }
            m_mainLayer->addChild(bg);
            m_mainLayer->addChild(menu);
        }

        // Bottom bar: copy the current prompt+settings as a shareable recipe
        // code (importable via the generator's Paste button).
        {
            auto barMenu = CCMenu::create();
            barMenu->setContentSize({160.f, 22.f});
            barMenu->ignoreAnchorPointForPosition(false);
            barMenu->setAnchorPoint({0.5f, 0.5f});
            barMenu->setPosition({W / 2.f, 18.f});
            auto lbl = CCLabelBMFont::create("Copy as Recipe Code", "goldFont.fnt");
            lbl->setScale(0.45f);
            auto btn = CCMenuItemSpriteExtra::create(lbl, this,
                menu_selector(PromptLibraryPopup::onCopyRecipe));
            btn->setPosition({80.f, 11.f});
            barMenu->addChild(btn);
            m_mainLayer->addChild(barMenu);
        }
        return true;
    }

    void onCopyRecipe(CCObject*) {
        if (m_currentPrompt.empty()) {
            Notification::create("Type a prompt first", NotificationIcon::Warning)->show();
            return;
        }
        utils::clipboard::write(buildRecipeCode(m_currentPrompt));
        Notification::create("Recipe code copied - share it anywhere",
                             NotificationIcon::Success)->show();
    }

    // (Re)builds the preset rows — called on init and after delete/save.
    // Cells and inert labels live on a background node; only CCMenuItems go
    // in the menu (house rule). Both hang off one root so rebuild is one
    // removeFromParent.
    void rebuildPresetList() {
        if (m_presetList) m_presetList->removeFromParent();
        float colX = 14.f + COL_W / 2.f;
        auto root = CCNode::create();
        root->setContentSize({COL_W, H - 80.f});

        auto bg = CCNode::create();
        bg->setContentSize({COL_W, H - 80.f});
        bg->setAnchorPoint({0.5f, 1.f});
        bg->ignoreAnchorPointForPosition(false);
        bg->setPosition({colX, LIST_TOP + 8.f});
        auto menu = CCMenu::create();
        menu->setContentSize({COL_W, H - 80.f});
        menu->ignoreAnchorPointForPosition(false);
        menu->setAnchorPoint({0.5f, 1.f});
        menu->setPosition({colX, LIST_TOP + 8.f});

        int idx = 0;
        for (auto& p : m_presets) {
            float y = (H - 80.f) - (idx + 0.5f) * ROW_H;
            auto cell = ui::makeCell(COL_W, ROW_H - 3.f, idx % 2 == 1);
            cell->setPosition({COL_W / 2.f, y});
            bg->addChild(cell);

            auto l = CCLabelBMFont::create(p.name.c_str(), "chatFont.fnt");
            l->limitLabelWidth(COL_W - 40.f, 0.55f, 0.45f);
            l->setColor(ui::TEXT_PRIMARY);
            auto btn = CCMenuItemSpriteExtra::create(l, this,
                menu_selector(PromptLibraryPopup::onPresetTap));
            btn->setTag(idx);
            btn->setPosition({(COL_W - 24.f) / 2.f, y});
            menu->addChild(btn);

            auto xSpr = CCSprite::createWithSpriteFrameName("GJ_deleteIcon_001.png");
            xSpr->setScale(0.45f);
            auto xBtn = CCMenuItemSpriteExtra::create(xSpr, this,
                menu_selector(PromptLibraryPopup::onPresetDelete));
            xBtn->setTag(idx);
            xBtn->setPosition({COL_W - 14.f, y});
            menu->addChild(xBtn);
            ++idx;
        }

        // "+ Save current" appears while there's room and a prompt to save.
        if (m_presets.size() < 6 && !m_currentPrompt.empty()) {
            float y = (H - 80.f) - (idx + 0.5f) * ROW_H;
            auto l = CCLabelBMFont::create("+ Save current prompt", "chatFont.fnt");
            l->limitLabelWidth(COL_W - 16.f, 0.55f, 0.45f);
            l->setColor(ui::SUCCESS_COL);
            auto btn = CCMenuItemSpriteExtra::create(l, this,
                menu_selector(PromptLibraryPopup::onSaveCurrent));
            btn->setPosition({COL_W / 2.f, y});
            menu->addChild(btn);
        } else if (m_presets.empty()) {
            auto l = CCLabelBMFont::create("(no presets yet)", "chatFont.fnt");
            l->setScale(0.5f);
            l->setColor(ui::TEXT_SECONDARY);
            l->setPosition({COL_W / 2.f, (H - 80.f) - 1.5f * ROW_H});
            bg->addChild(l);
        }

        root->addChild(bg);
        root->addChild(menu);
        m_mainLayer->addChild(root);
        m_presetList = root;
    }

    void onPresetTap(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_presets.size()) return;
        if (m_onApply) m_onApply(m_presets[idx]);
        this->onClose(nullptr);
    }

    void onPresetDelete(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_presets.size()) return;
        m_presets.erase(m_presets.begin() + idx);
        savePromptPresets(m_presets);
        rebuildPresetList();
    }

    void onSaveCurrent(CCObject*) {
        PromptPreset p;
        p.prompt = m_currentPrompt;
        p.name = m_currentPrompt.size() > 22 ? m_currentPrompt.substr(0, 21) + "..."
                                             : m_currentPrompt;
        p.difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        p.style      = Mod::get()->getSettingValue<std::string>("style");
        p.length     = Mod::get()->getSettingValue<std::string>("length");
        m_presets.push_back(std::move(p));
        savePromptPresets(m_presets);
        rebuildPresetList();
        Notification::create("Preset saved", NotificationIcon::Success)->show();
    }

    void onTemplateTap(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)(sizeof(PROMPT_TEMPLATES) / sizeof(*PROMPT_TEMPLATES)))
            return;
        PromptPreset p;
        p.prompt = expandPromptTemplate(PROMPT_TEMPLATES[idx]);
        if (m_onApply) m_onApply(p);
        this->onClose(nullptr);
    }

public:
    static PromptLibraryPopup* create(std::string currentPrompt,
                                      std::function<void(const PromptPreset&)> onApply) {
        auto ret = new PromptLibraryPopup();
        ret->m_currentPrompt = std::move(currentPrompt);
        ret->m_onApply       = std::move(onApply);
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ─── Generation History popup: browse + re-apply past generations ────────────
class HistoryPopup : public Popup {
protected:
    std::function<void(const std::string&)> m_onReapply;
    // Snapshot of the feedback store taken at init (newest first) — the
    // live vector can shift under us if a rating lands while we're open.
    std::vector<FeedbackEntry> m_entries;

    static constexpr float W = 410.f, H = 274.f;

    bool init() override {
        if (!Popup::init(W, H)) return false;
        this->setID("history-popup"_spr);
        this->setTitle("Generation History");
        ui::styleTitle(m_title, 0.8f);
        ui::addGroove(m_mainLayer, 360.f, W / 2.f, H - 36.f);

        {
            const auto& live = loadFeedback();
            m_entries.assign(live.rbegin(), live.rend());  // newest first
        }
        if (m_entries.empty()) {
            auto l = CCLabelBMFont::create(
                "No rated generations yet.\nRate a few and they'll show up here.",
                "chatFont.fnt");
            l->setScale(0.6f);
            l->setAlignment(kCCTextAlignmentCenter);
            l->setColor(ui::TEXT_SECONDARY);
            l->setPosition({W / 2.f, H / 2.f - 10.f});
            m_mainLayer->addChild(l);
            return true;
        }

        constexpr float LIST_W = 374.f, LIST_H = 196.f, ROW = 28.f;
        auto well = ui::makeWell(LIST_W + 6.f, LIST_H + 6.f);
        well->setPosition({W / 2.f, 36.f + LIST_H / 2.f});
        m_mainLayer->addChild(well);

        auto scroll = ScrollLayer::create({LIST_W, LIST_H});
        scroll->setPosition({(W - LIST_W) / 2.f, 36.f});

        int total = (int)m_entries.size();
        float contentH = std::max(LIST_H, total * ROW);
        scroll->m_contentLayer->setContentSize({LIST_W, contentH});

        auto menu = CCMenu::create();
        menu->setContentSize({LIST_W, contentH});
        menu->setPosition({0.f, 0.f});
        scroll->m_contentLayer->addChild(menu);

        for (int row = 0; row < total; ++row) {
            const auto& e = m_entries[row];   // snapshot is already newest-first
            float y = contentH - (row + 0.5f) * ROW;

            auto cell = ui::makeCell(LIST_W, ROW - 2.f, row % 2 == 1);
            cell->setPosition({LIST_W / 2.f, y});
            scroll->m_contentLayer->addChild(cell, -1);

            auto dot = ui::makeStatusDot(e.accepted ? ui::SUCCESS_COL : ui::ERROR_COL, 0.5f);
            dot->setPosition({14.f, y});
            scroll->m_contentLayer->addChild(dot);

            auto rating = CCLabelBMFont::create(
                fmt::format("{}/10", e.rating).c_str(), "bigFont.fnt");
            rating->setScale(0.3f);
            rating->setColor(e.rating >= 6 ? ui::TEXT_PRIMARY : ui::TEXT_SECONDARY);
            rating->setPosition({40.f, y});
            scroll->m_contentLayer->addChild(rating);

            auto snippet = CCLabelBMFont::create(e.prompt.c_str(), "chatFont.fnt");
            snippet->limitLabelWidth(238.f, 0.55f, 0.4f);
            snippet->setAnchorPoint({0.f, 0.5f});
            snippet->setColor(ui::TEXT_PRIMARY);
            auto rowBtn = CCMenuItemSpriteExtra::create(snippet, this,
                menu_selector(HistoryPopup::onRowDetail));
            rowBtn->setTag(row);
            rowBtn->setAnchorPoint({0.f, 0.5f});
            rowBtn->setPosition({62.f, y});
            menu->addChild(rowBtn);

            if (!e.objectsJson.empty()) {
                auto reSpr = CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png");
                reSpr->setScale(0.42f);
                auto reBtn = CCMenuItemSpriteExtra::create(reSpr, this,
                    menu_selector(HistoryPopup::onRowReapply));
                reBtn->setTag(row);
                reBtn->setPosition({LIST_W - 18.f, y});
                menu->addChild(reBtn);
            }
        }

        m_mainLayer->addChild(scroll);
        // Start at top (cocos scroll content anchors bottom).
        scroll->moveToTop();
        return true;
    }

    void onRowDetail(CCObject* sender) {
        int row = static_cast<CCNode*>(sender)->getTag();
        if (row < 0 || row >= (int)m_entries.size()) return;
        const auto& e = m_entries[row];
        std::string body = fmt::format(
            "<cy>Prompt:</c> {}\n<cy>Settings:</c> {} / {} / {}\n<cy>Rating:</c> {}/10 ({})",
            e.prompt, e.difficulty, e.style, e.length,
            e.rating, e.accepted ? "accepted" : "denied");
        if (!e.feedback.empty())    body += fmt::format("\n<cy>Feedback:</c> {}", e.feedback);
        if (!e.editSummary.empty()) body += fmt::format("\n<cy>Your edits:</c> {}", e.editSummary);
        FLAlertLayer::create(nullptr, "Generation", body, "OK", nullptr, 380.f)->show();
    }

    void onRowReapply(CCObject* sender) {
        int row = static_cast<CCNode*>(sender)->getTag();
        if (row < 0 || row >= (int)m_entries.size()) return;
        if (m_onReapply) m_onReapply(m_entries[row].objectsJson);
        this->onClose(nullptr);
    }

public:
    static HistoryPopup* create(std::function<void(const std::string&)> onReapply) {
        auto ret = new HistoryPopup();
        ret->m_onReapply = std::move(onReapply);
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ─── Generation sessions ─────────────────────────────────────────────────────
// Struct lives in sessions.hpp (shared with overlay.cpp); the registry and
// engine seam functions are defined here.

static bool s_sessionsDirty  = false;
static int  s_sessionsNextId = 1;
void editoraiMarkSessionsDirty() { s_sessionsDirty = true; }

// Load persisted sessions once, on first registry access. Restored sessions
// are read-only history: the engine (in-flight network state, tool history)
// can't be serialized, so anything that was live becomes Done with a note.
static void loadPersistedSessions(std::vector<std::shared_ptr<GenSession>>& out) {
    auto path = Mod::get()->getSaveDir() / "sessions.json";
    auto read = geode::utils::file::readString(path);
    if (!read) return;
    auto parsed = matjson::parse(read.unwrap());
    if (!parsed) return;
    const auto arr = parsed.unwrap();
    if (!arr.isArray()) return;
    // Keep the NEWEST 12 (sessions are stored oldest-first) — capping from
    // the front would discard the most recent conversations, the ones a user
    // is most likely to resume.
    size_t startI = arr.size() > 12 ? arr.size() - 12 : 0;
    for (size_t i = startI; i < arr.size(); ++i) {
        const auto& o = arr[i];
        if (!o.isObject()) continue;
        auto s = std::make_shared<GenSession>();
        s->id          = (int)o["id"].asInt().unwrapOr(0);
        s->title       = o["title"].asString().unwrapOr("");
        s->startedAt   = o["startedAt"].asInt().unwrapOr(0);
        s->needsRating = o["needsRating"].asBool().unwrapOr(false);
        s->fbPrompt            = o["fbPrompt"].asString().unwrapOr("");
        s->fbDifficulty        = o["fbDifficulty"].asString().unwrapOr("");
        s->fbStyle             = o["fbStyle"].asString().unwrapOr("");
        s->fbLength            = o["fbLength"].asString().unwrapOr("");
        s->fbObjectsJson       = o["fbObjectsJson"].asString().unwrapOr("");
        s->fbEditedObjectsJson = o["fbEditedObjectsJson"].asString().unwrapOr("");
        s->fbEditSummary       = o["fbEditSummary"].asString().unwrapOr("");
        s->fbAccepted = o["fbAccepted"].asBool().unwrapOr(false);
        s->fbRating   = (int)o["fbRating"].asInt().unwrapOr(0);
        s->fbShared   = o["fbShared"].asBool().unwrapOr(false);
        s->restored   = true;
        s->chatSummary     = o["chatSummary"].asString().unwrapOr("");
        s->targetLevelName = o["targetLevelName"].asString().unwrapOr("");
        s->pendingEdit     = o["pendingEdit"].asString().unwrapOr("");
        s->pendingEditMode = (int)o["pendingEditMode"].asInt().unwrapOr(0);
        int st = (int)o["state"].asInt().unwrapOr((int)GenSession::State::Done);
        s->state = st == (int)GenSession::State::Failed
            ? GenSession::State::Failed : GenSession::State::Done;
        // A queued edit survives the restart — keep the session visibly
        // waiting (go-to-level button, adoption on editor open) instead of
        // burying it as Done.
        if (!s->pendingEdit.empty())
            s->state = GenSession::State::AwaitingEditor;
        const auto& tr = o["transcript"];
        if (tr.isArray()) {
            for (size_t j = 0; j < tr.size() && j < 400; ++j) {
                const auto& e = tr[j];
                if (!e.isObject()) continue;
                int k = (int)e["k"].asInt().unwrapOr(0);
                s->transcript.push_back({
                    (GenSession::Entry::Kind)std::clamp(k, 0, 6),
                    e["t"].asString().unwrapOr("")});
            }
        }
        // Durable chat memory — what the AI context is rebuilt from when the
        // user keeps talking to this session.
        const auto& ch = o["chat"];
        if (ch.isArray()) {
            // Cap from the TAIL: a resumed conversation needs its newest
            // turns; the oldest are exactly what chatPush folds away anyway.
            size_t startJ = ch.size() > 200 ? ch.size() - 200 : 0;
            for (size_t j = startJ; j < ch.size(); ++j) {
                const auto& m = ch[j];
                if (!m.isObject()) continue;
                s->chat.push_back({
                    (int)std::clamp<int64_t>(m["r"].asInt().unwrapOr(0), 0, 1),
                    m["t"].asString().unwrapOr("")});
            }
        }
        // Re-resolve the target level by name so go-to-level and edit
        // resumes work across restarts (CCObject pointers don't persist).
        if (!s->targetLevelName.empty()) {
            if (auto* llm = LocalLevelManager::get(); llm && llm->m_localLevels) {
                for (auto* raw : CCArrayExt<CCObject*>(llm->m_localLevels)) {
                    auto* cand = typeinfo_cast<GJGameLevel*>(raw);
                    if (cand && std::string(cand->m_levelName) == s->targetLevelName) {
                        s->targetLevel = cand;
                        break;
                    }
                }
            }
        }
        bool wasLive = st == (int)GenSession::State::Running ||
                       st == (int)GenSession::State::AwaitingEditor ||
                       st == (int)GenSession::State::Staged;
        if (wasLive)
            s->push(GenSession::Entry::Kind::Status,   // push() keeps the cap
                "(restored after restart - anything unstaged was lost, but "
                "the conversation lives: just send another message)");
        if (s->id >= s_sessionsNextId) s_sessionsNextId = s->id + 1;
        out.push_back(std::move(s));
    }
    s_sessionsDirty = false;  // loading isn't a change
    log::info("EditorAI: restored {} session(s) from disk", out.size());
}

std::vector<std::shared_ptr<GenSession>>& genSessions() {
    static std::vector<std::shared_ptr<GenSession>> s = [] {
        std::vector<std::shared_ptr<GenSession>> v;
        loadPersistedSessions(v);
        return v;
    }();
    return s;
}

// Serialize on the caller, write on a detached thread (persistFeedback
// pattern). Throttled by the dirty flag — the overlay ticks this every
// few seconds and $on_mod(DataSaved) flushes on exit.
void editoraiPersistSessionsIfDirty() {
    if (!s_sessionsDirty) return;
    s_sessionsDirty = false;
    auto arr = matjson::Value::array();
    for (auto& s : genSessions()) {
        if (!s) continue;
        auto o = matjson::Value::object();
        o["id"]          = s->id;
        o["title"]       = s->title;
        o["state"]       = (int)s->state;
        o["startedAt"]   = s->startedAt;
        o["needsRating"] = s->needsRating;
        o["fbPrompt"]            = s->fbPrompt;
        o["fbDifficulty"]        = s->fbDifficulty;
        o["fbStyle"]             = s->fbStyle;
        o["fbLength"]            = s->fbLength;
        o["fbObjectsJson"]       = s->fbObjectsJson;
        o["fbEditedObjectsJson"] = s->fbEditedObjectsJson;
        o["fbEditSummary"]       = s->fbEditSummary;
        o["fbAccepted"] = s->fbAccepted;
        o["fbRating"]   = s->fbRating;
        o["fbShared"]   = s->fbShared;
        auto tr = matjson::Value::array();
        for (auto& e : s->transcript) {
            auto eo = matjson::Value::object();
            eo["k"] = (int)e.kind;
            eo["t"] = e.text;
            tr.push(std::move(eo));
        }
        o["transcript"] = tr;
        auto ch = matjson::Value::array();
        for (auto& m : s->chat) {
            auto mo = matjson::Value::object();
            mo["r"] = m.role;
            mo["t"] = m.text;
            ch.push(std::move(mo));
        }
        o["chat"]            = ch;
        o["chatSummary"]     = s->chatSummary;
        o["targetLevelName"] = s->targetLevelName;
        o["pendingEdit"]     = s->pendingEdit;
        o["pendingEditMode"] = s->pendingEditMode;
        arr.push(std::move(o));
    }
    // Same delivery scheme as persistFeedback: snapshots carry a sequence
    // number (last-SNAPSHOT-wins, not last-thread-wins) and land via temp
    // file + rename so a crash or overlapping writer can't tear the file.
    static std::atomic<uint64_t> s_writeGen{0};
    uint64_t gen = ++s_writeGen;
    std::string out = arr.dump(matjson::NO_INDENTATION);
    std::thread([out = std::move(out), gen,
                 path = Mod::get()->getSaveDir() / "sessions.json"] {
        static std::mutex s_writeMutex;
        static uint64_t   s_lastWritten = 0;
        std::lock_guard<std::mutex> lock(s_writeMutex);
        if (gen <= s_lastWritten) return;  // a newer snapshot already landed
        auto tmp = path;
        tmp += ".tmp";
        auto res = geode::utils::file::writeString(tmp, out);
        if (!res) {
            log::error("Failed to save sessions.json: {}", res.unwrapErr());
            return;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) log::error("Failed to commit sessions.json: {}", ec.message());
        else    s_lastWritten = gen;
    }).detach();
}

$on_mod(DataSaved) {
    s_sessionsDirty = true;          // force a flush even if throttle just ran
    editoraiPersistSessionsIfDirty();
}

static std::shared_ptr<GenSession> newGenSession() {
    auto s = std::make_shared<GenSession>();
    s->id = s_sessionsNextId++;
    s->startedAt = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto& all = genSessions();
    all.push_back(s);
    // Bound memory: drop the oldest finished sessions past 12. Staged is
    // live too (ghosts in the editor, rating linkage pending) — never evict.
    while (all.size() > 12) {
        auto it = std::find_if(all.begin(), all.end(), [](auto& e) {
            return e->state != GenSession::State::Running &&
                   e->state != GenSession::State::AwaitingEditor &&
                   e->state != GenSession::State::Staged;
        });
        if (it == all.end()) break;
        all.erase(it);
    }
    // Second bound: AwaitingEditor sessions pin whole engines (deferred
    // objects, tool history, TaskHolders) via engineRef. Queuing many
    // never-opened targets must not grow without limit — keep the 3 newest.
    int awaiting = 0;
    for (auto& e : all)
        if (e && e->state == GenSession::State::AwaitingEditor) ++awaiting;
    while (awaiting > 3) {
        auto it = std::find_if(all.begin(), all.end(), [](auto& e) {
            return e && e->state == GenSession::State::AwaitingEditor;
        });
        if (it == all.end()) break;
        (*it)->state = GenSession::State::Failed;
        (*it)->push(GenSession::Entry::Kind::Status,
                    "(dropped - too many generations waiting for editors)");
        (*it)->engineRef = nullptr;   // releases the pinned engine
        (*it)->enginePtr = nullptr;
        (*it)->pendingEdit.clear();   // a dropped session must not fire later
        --awaiting;
    }
    editoraiMarkSessionsDirty();
    return s;
}

class AIGeneratorPopup : public Popup {
protected:
    TextInput*               m_promptInput    = nullptr;
    CCLabelBMFont*           m_statusLabel    = nullptr;
    CCMenuItemSpriteExtra*   m_generateBtn    = nullptr;
    CCMenuItemSpriteExtra*   m_cancelBtn      = nullptr;
    std::string              m_manualPromptBlob;   // manual provider: last prompt copied to clipboard
    CCMenuItemToggler*       m_editModeToggle = nullptr;

    // ── Tool-use plumbing (AI-only — not user-facing) ────────────────────
    // These TaskHolders fire the network requests behind the AI's tool calls
    // (web_search, download_level, search_newgrounds, get_newgrounds_song,
    // analyze_level, get_level_length, ...). The AI decides when to call
    // each tool via the tool-use loop in runToolLoop; the user never sees an
    // input field for them. Three holders so concurrent calls don't step on
    // each other (though the executor runs them serially anyway).
    async::TaskHolder<web::WebResponse> m_toolListenerLevel;
    async::TaskHolder<web::WebResponse> m_toolListenerNG;
    async::TaskHolder<web::WebResponse> m_toolListenerWeb;

    // Invariant: m_shouldClearLevel == !m_editMode, maintained by
    // onToggleEditMode. The defaults must satisfy it too — a fresh popup is
    // in fresh-generation mode (toggle off ⇒ clear level, with confirmation).
    bool                     m_shouldClearLevel = true;
    bool                     m_isGenerating     = false;
    int                      m_transientRetries = 0;   // reset per generation
    std::string              m_lastCallPrompt;          // for transient retry
    std::string              m_lastCallKey;
    CCLabelBMFont*           m_costLabel        = nullptr;
    std::string              m_styleRefId;              // "style: <ID>" directive
    std::string              m_styleBrief;              // fetched summary (single-shot)
    std::string              m_lockedPalette;           // COLOR lines from PalettePopup
    std::shared_ptr<GenSession> m_session;               // background-survivable identity
    std::shared_ptr<matjson::Value> m_pendingApplyObjects; // staged while no editor
    std::shared_ptr<matjson::Value> m_pendingMetadata;
    std::string              m_platinumStatus;          // queue widget text
    CCLabelBMFont*           m_chipLabel = nullptr;     // provider chip live refresh
    CCSprite*                m_chipDot   = nullptr;
    std::string              m_chipProviderShown;
    async::TaskHolder<web::WebResponse> m_platinumPoll;
    async::TaskHolder<web::WebResponse> m_subagentTask;
    bool                     m_mutationMode = false;    // Mutate button flow
    bool                     m_coopMode     = false;    // AI-continues-your-build flow
    CCMenuItemToggler*       m_coopToggle   = nullptr;
    std::string              m_lastCostPrompt;          // dirty-check for the cost tick
    size_t                   m_sysPromptLenEst  = 0;    // measured once at popup open
    // When true the popup is in "small edits" mode — a different system
    // prompt is built (additive only, no clearing, conservative changes)
    // and the popup retitles to "Editor AI - Edit Mode".
    bool                     m_editMode         = false;

    LevelEditorLayer*        m_editorLayer    = nullptr;

    async::TaskHolder<web::WebResponse> m_listener;

    std::vector<DeferredObject> m_deferredObjects;
    size_t m_currentObjectIndex = 0;
    bool   m_isCreatingObjects  = false;
    // Settings snapshotted when spawning starts — the spawn tick runs at 20 Hz
    // and applyObjectProperties runs per object, so don't hit the settings
    // store from those paths.
    int    m_spawnBatchSize     = 8;
    bool   m_advFeatures        = false;
    std::chrono::steady_clock::time_point m_generationStartTime;

    // ── init ──────────────────────────────────────────────────────────────────

    bool init(LevelEditorLayer* editorLayer) {
        // GD-native layout — 380 × 200 on the stock GJ_square01 popup chrome
        // (untinted; tinting the brown base yields mud). goldFont title,
        // embossed groove, black square02b wells, chunky GD buttons. Every
        // member keeps its old name so the rest of the popup wiring is
        // unchanged. Width lint: <= 410 (4:3 design width is ~426.7).
        constexpr float W = 380.f, H = 200.f;
        if (!Popup::init(W, H)) return false;

        m_editorLayer = editorLayer;
        this->setID("generator-popup"_spr);
        this->setTitle("Editor AI");
        ui::styleTitle(m_title, 0.8f);

        // Embossed groove under the title.
        ui::addGroove(m_mainLayer, 330.f, W / 2.f, H - 36.f);

        // ── Provider chip — recessed pill, clickable, jumps to settings ──
        {
            std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
            std::string model    = getProviderModel(provider);
            bool local     = (provider == "ollama" || provider == "lm-studio" ||
                              provider == "llama-cpp");
            bool hasToken  = !oauth::savedToken(provider).empty();
            // The custom provider's key id is "custom-provider-api-key" —
            // "custom-api-key" is undeclared (getProviderApiKey agrees).
            bool hasManual = !Mod::get()->getSettingValue<std::string>(
                              provider == "custom" ? std::string("custom-provider-api-key")
                                                   : provider + "-api-key").empty();
            bool hasAny    = hasToken || hasManual || provider == "custom";
            // Dot carries the auth state: amber = local (no key needed),
            // green = configured, red = not configured.
            ccColor3B dot = local  ? ui::WARN_COL
                          : hasAny ? ui::SUCCESS_COL
                                   : ui::ERROR_COL;

            constexpr float CHIP_W = 216.f, CHIP_H = 20.f;
            auto container = CCNode::create();
            container->setContentSize({CHIP_W, CHIP_H});

            auto well = ui::makeWell(CHIP_W, CHIP_H, 70);
            well->setPosition({CHIP_W / 2.f, CHIP_H / 2.f});
            container->addChild(well);

            auto dotSpr = ui::makeStatusDot(dot);
            dotSpr->setPosition({12.f, CHIP_H / 2.f});
            container->addChild(dotSpr);

            // " - " separator: bigFont has no '·' glyph. The dot color above
            // signals auth, so no trailing checkmark text either. The custom
            // provider shows its user-given display name.
            std::string shownName = provider;
            if (provider == "custom") {
                std::string nm = Mod::get()->getSettingValue<std::string>("custom-provider-name");
                if (!nm.empty()) shownName = nm;
            }
            auto txtLbl = CCLabelBMFont::create(
                fmt::format("{} - {}", shownName, model).c_str(), "bigFont.fnt");
            txtLbl->limitLabelWidth(168.f, 0.3f, 0.1f);
            txtLbl->setColor(ui::TEXT_PRIMARY);
            txtLbl->setAnchorPoint({0.f, 0.5f});
            txtLbl->setPosition({24.f, CHIP_H / 2.f});
            container->addChild(txtLbl);
            // Kept as members so refreshes propagate live (the chip used to
            // go stale until the popup was reopened — user-reported).
            m_chipLabel = txtLbl;
            m_chipDot   = dotSpr;
            m_chipProviderShown = provider;

            auto arrow = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
            arrow->setScale(0.3f);
            arrow->setAnchorPoint({1.f, 0.5f});
            arrow->setPosition({CHIP_W - 8.f, CHIP_H / 2.f});
            container->addChild(arrow);

            auto chipMenu = CCMenu::create();
            chipMenu->setID("provider-chip"_spr);
            chipMenu->setContentSize({CHIP_W, CHIP_H});
            chipMenu->ignoreAnchorPointForPosition(false);
            chipMenu->setAnchorPoint({0.5f, 0.5f});
            chipMenu->setPosition({W / 2.f, H - 54.f});
            auto chipBtn = CCMenuItemSpriteExtra::create(container, this,
                menu_selector(AIGeneratorPopup::onAccount));
            chipBtn->setPosition({CHIP_W / 2.f, CHIP_H / 2.f});
            chipMenu->addChild(chipBtn);
            m_mainLayer->addChild(chipMenu);
        }

        // ── Prompt well + input ──────────────────────────────────────────
        {
            auto well = ui::makeWell(320.f, 30.f);
            well->setPosition({W / 2.f, 114.f});
            m_mainLayer->addChild(well);

            // Width is PRE-scale: 540 × 0.55 ≈ 297pt — the touch target
            // fills the visible well instead of half of it.
            m_promptInput = TextInput::create(540.f, "describe your level",
                                              "bigFont.fnt");
            m_promptInput->setPosition({W / 2.f - 10.f, 114.f});
            m_promptInput->setScale(0.55f);
            // Our well IS the input background — hide the built-in one.
            if (auto bg = m_promptInput->getBGSprite()) bg->setVisible(false);
            m_mainLayer->addChild(m_promptInput);
        }

        // Prompt-history arrows, fully clear of the well's right edge.
        // Smaller scale: texture packs often ship chunkier nav arrows, and
        // 0.3 collided with the well in practice (user screenshot).
        {
            auto historyMenu = CCMenu::create();
            historyMenu->setContentSize({20.f, 40.f});
            historyMenu->ignoreAnchorPointForPosition(false);
            historyMenu->setAnchorPoint({0.5f, 0.5f});
            historyMenu->setPosition({W / 2.f + 172.f, 114.f});
            auto mkArrow = [this](float rot, SEL_MenuHandler sel, float yy) {
                auto s = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
                s->setScale(0.22f);
                s->setRotation(rot);
                auto btn = CCMenuItemSpriteExtra::create(s, this, sel);
                btn->setPosition({10.f, yy});
                return btn;
            };
            historyMenu->addChild(mkArrow(-90, menu_selector(AIGeneratorPopup::onHistoryUp),   29.f));
            historyMenu->addChild(mkArrow( 90, menu_selector(AIGeneratorPopup::onHistoryDown), 11.f));
            m_mainLayer->addChild(historyMenu);
        }

        // ── Edit-mode row (toggler + tappable label) ─────────────────────
        {
            auto editMenu = CCMenu::create();
            editMenu->setContentSize({170.f, 24.f});
            editMenu->ignoreAnchorPointForPosition(false);
            editMenu->setAnchorPoint({0.f, 0.5f});
            editMenu->setPosition({30.f, 86.f});

            auto onSpr  = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
            auto offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            onSpr->setScale(0.6f);
            offSpr->setScale(0.6f);
            m_editModeToggle = CCMenuItemToggler::create(offSpr, onSpr, this,
                menu_selector(AIGeneratorPopup::onToggleEditMode));
            m_editModeToggle->toggle(m_editMode);
            m_editModeToggle->setPosition({12.f, 12.f});
            editMenu->addChild(m_editModeToggle);

            // Tapping the text flips the toggler too. activate() both flips
            // the sprite AND fires onToggleEditMode — one source of truth.
            auto editLabel = CCLabelBMFont::create("Edit existing level", "bigFont.fnt");
            editLabel->setScale(0.35f);
            editLabel->setColor(ui::TEXT_PRIMARY);
            auto labelItem = CCMenuItemSpriteExtra::create(editLabel, this,
                menu_selector(AIGeneratorPopup::onEditModeLabel));
            labelItem->setAnchorPoint({0.f, 0.5f});
            labelItem->setPosition({28.f, 12.f});
            editMenu->addChild(labelItem);
            m_mainLayer->addChild(editMenu);

            // Co-op row mirrors the edit row on the right: the AI continues
            // whatever the user built, one ~900u chunk per generation.
            auto coopMenu = CCMenu::create();
            coopMenu->setContentSize({160.f, 24.f});
            coopMenu->ignoreAnchorPointForPosition(false);
            coopMenu->setAnchorPoint({0.f, 0.5f});
            coopMenu->setPosition({208.f, 86.f});
            auto cOn  = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
            auto cOff = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            cOn->setScale(0.6f);
            cOff->setScale(0.6f);
            m_coopToggle = CCMenuItemToggler::create(cOff, cOn, this,
                menu_selector(AIGeneratorPopup::onToggleCoop));
            m_coopToggle->toggle(false);
            m_coopToggle->setPosition({12.f, 12.f});
            coopMenu->addChild(m_coopToggle);
            auto coopLabel = CCLabelBMFont::create("Co-op: continue my build", "bigFont.fnt");
            coopLabel->limitLabelWidth(118.f, 0.35f, 0.2f);
            coopLabel->setColor(ui::TEXT_PRIMARY);
            auto coopItem = CCMenuItemSpriteExtra::create(coopLabel, this,
                menu_selector(AIGeneratorPopup::onCoopLabel));
            coopItem->setAnchorPoint({0.f, 0.5f});
            coopItem->setPosition({28.f, 12.f});
            coopMenu->addChild(coopItem);
            m_mainLayer->addChild(coopMenu);
        }

        m_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_statusLabel->setScale(STATUS_SCALE);
        m_statusLabel->setPosition({W / 2.f, 64.f});
        m_statusLabel->setVisible(false);
        m_mainLayer->addChild(m_statusLabel);

        // ── Action row (Generate / Cancel swap in one slot) ──────────────
        {
            auto btnMenu = CCMenu::create();
            btnMenu->setContentSize({180.f, 40.f});
            btnMenu->ignoreAnchorPointForPosition(false);
            btnMenu->setAnchorPoint({0.5f, 0.5f});
            btnMenu->setPosition({W / 2.f, 32.f});

            m_generateBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Generate", "goldFont.fnt", "GJ_button_01.png", 0.8f),
                this, menu_selector(AIGeneratorPopup::onGenerate));
            m_generateBtn->setPosition({90.f, 20.f});
            btnMenu->addChild(m_generateBtn);

            m_cancelBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Cancel", "goldFont.fnt", "GJ_button_06.png", 0.7f),
                this, menu_selector(AIGeneratorPopup::onCancel));
            m_cancelBtn->setPosition({90.f, 20.f});
            m_cancelBtn->setVisible(false);
            btnMenu->addChild(m_cancelBtn);
            m_mainLayer->addChild(btnMenu);
        }

        // ── Corner icons: info top-right, gear bottom-left (4.5) ─────────
        {
            auto infoMenu = CCMenu::create();
            infoMenu->setContentSize({30.f, 30.f});
            infoMenu->ignoreAnchorPointForPosition(false);
            infoMenu->setAnchorPoint({0.5f, 0.5f});
            auto infoSpr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
            infoSpr->setScale(0.7f);
            auto infoBtn = CCMenuItemSpriteExtra::create(infoSpr, this,
                menu_selector(AIGeneratorPopup::onInfo));
            infoBtn->setPosition({15.f, 15.f});
            infoMenu->addChild(infoBtn);
            m_mainLayer->addChildAtPosition(infoMenu, Anchor::TopRight, {-22.f, -22.f});

            auto gearMenu = CCMenu::create();
            gearMenu->setContentSize({30.f, 30.f});
            gearMenu->ignoreAnchorPointForPosition(false);
            gearMenu->setAnchorPoint({0.5f, 0.5f});
            auto gearSpr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
            gearSpr->setScale(0.4f);
            auto gearBtn = CCMenuItemSpriteExtra::create(gearSpr, this,
                menu_selector(AIGeneratorPopup::onSettings));
            gearBtn->setPosition({15.f, 15.f});
            gearMenu->addChild(gearBtn);
            m_mainLayer->addChildAtPosition(gearMenu, Anchor::BottomLeft, {22.f, 22.f});

            // Bottom-right mini-toolbar: Prompt Library + History. Mirrors
            // the gear's corner placement so the chrome stays symmetric.
            auto toolMenu = CCMenu::create();
            toolMenu->setContentSize({58.f, 30.f});
            toolMenu->ignoreAnchorPointForPosition(false);
            toolMenu->setAnchorPoint({0.5f, 0.5f});
            auto libSpr = CCSprite::createWithSpriteFrameName("GJ_select_001.png");
            libSpr->setScale(0.55f);
            auto libBtn = CCMenuItemSpriteExtra::create(libSpr, this,
                menu_selector(AIGeneratorPopup::onOpenLibrary));
            libBtn->setID("library-btn"_spr);
            libBtn->setPosition({15.f, 15.f});
            toolMenu->addChild(libBtn);
            auto histSpr = CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png");
            histSpr->setScale(0.5f);
            auto histBtn = CCMenuItemSpriteExtra::create(histSpr, this,
                menu_selector(AIGeneratorPopup::onOpenHistory));
            histBtn->setID("history-btn"_spr);
            histBtn->setPosition({43.f, 15.f});
            toolMenu->addChild(histBtn);
            // Paste: imports a recipe code OR raw EAS from the clipboard.
            auto pasteSpr = CCSprite::createWithSpriteFrameName("GJ_duplicateBtn_001.png");
            if (!pasteSpr) pasteSpr = CCSprite::createWithSpriteFrameName("GJ_select_001.png");
            pasteSpr->setScale(0.45f);
            auto pasteBtn = CCMenuItemSpriteExtra::create(pasteSpr, this,
                menu_selector(AIGeneratorPopup::onPasteImport));
            pasteBtn->setID("paste-btn"_spr);
            pasteBtn->setPosition({71.f, 15.f});
            toolMenu->addChild(pasteBtn);
            // Palette: lock a curated color set into the next generation.
            auto palSpr = CCSprite::createWithSpriteFrameName("GJ_paintBtn_001.png");
            if (!palSpr) palSpr = CCSprite::createWithSpriteFrameName("GJ_select_001.png");
            palSpr->setScale(0.45f);
            auto palBtn = CCMenuItemSpriteExtra::create(palSpr, this,
                menu_selector(AIGeneratorPopup::onOpenPalette));
            palBtn->setID("palette-btn"_spr);
            palBtn->setPosition({99.f, 15.f});
            toolMenu->addChild(palBtn);
            toolMenu->setContentSize({114.f, 30.f});
            m_mainLayer->addChildAtPosition(toolMenu, Anchor::BottomRight, {-66.f, 22.f});
        }

        // Idle cost-estimate label shares the status slot — visible only
        // while the status label isn't.
        m_costLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_costLabel->setScale(0.45f);
        m_costLabel->setColor(ui::TEXT_SECONDARY);
        m_costLabel->setPosition({W / 2.f, 64.f});
        m_mainLayer->addChild(m_costLabel);

        // Mutate-button entry: same popup, mutation framing. Must precede
        // the prompt-length measurement so the estimate includes the
        // injected level context.
        if (s_openInMutationMode) {
            s_openInMutationMode = false;
            m_mutationMode = true;
            this->setTitle("Mutate Level");
            ui::styleTitle(m_title, 0.8f);
            showStatus("Describe the change - or 'region 300-900: redo as wave'");
        }

        // System prompt length measured ONCE per popup open — never in the
        // tick (the full-mode prompt is ~60 KB of formatting; mutation adds
        // up to 24 KB of level context via appendModeContext).
        {
            std::string sysEst = buildSystemPrompt();
            appendModeContext(sysEst);
            m_sysPromptLenEst = sysEst.size();
        }
        this->schedule(schedule_selector(AIGeneratorPopup::updateCostEstimate), 1.0f);
        this->schedule(schedule_selector(AIGeneratorPopup::pollPlatinumStatus), 5.0f);

        this->schedule(schedule_selector(AIGeneratorPopup::updateObjectCreation), 0.05f);
        return true;
    }

    // ── Prompt Library / History wiring ────────────────────────────────────
    void onOpenLibrary(CCObject*) {
        PromptLibraryPopup::create(
            m_promptInput ? m_promptInput->getString() : "",
            [this](const PromptPreset& p) {
                if (m_promptInput) m_promptInput->setString(p.prompt);
                // Presets carry their saved difficulty/style/length; template
                // expansions leave them empty (keep current settings).
                if (!p.difficulty.empty())
                    Mod::get()->setSettingValue<std::string>("difficulty", p.difficulty);
                if (!p.style.empty())
                    Mod::get()->setSettingValue<std::string>("style", p.style);
                if (!p.length.empty())
                    Mod::get()->setSettingValue<std::string>("length", p.length);
            })->show();
    }

    void onOpenHistory(CCObject*) {
        HistoryPopup::create([this](const std::string& objectsJson) {
            this->reapplyFromHistory(objectsJson);
        })->show();
    }

    void onOpenPalette(CCObject*) {
        PalettePopup::create([this](const std::string& name, const std::string& colorLines) {
            m_lockedPalette = colorLines;
            showStatus(fmt::format("Palette locked: {}", name));
        })->show();
    }

    // Clipboard import: recipe code ("EAIR1:...") fills the prompt+settings;
    // raw EAS text stages straight into a blueprint preview.
    void onPasteImport(CCObject*) {
        std::string clip = utils::clipboard::read();
        // Trim outer whitespace.
        auto first = clip.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            Notification::create("Clipboard is empty", NotificationIcon::Warning)->show();
            return;
        }
        clip.erase(0, first);
        while (!clip.empty() && (clip.back() == ' ' || clip.back() == '\n' ||
                                 clip.back() == '\r' || clip.back() == '\t'))
            clip.pop_back();

        // Sanity caps: recipes are tiny; EAS pastes can be larger but a
        // multi-MB clipboard would stall the main thread in the decoder.
        if (clip.size() > 1024 * 1024) {
            Notification::create("Clipboard too large to import",
                                 NotificationIcon::Warning)->show();
            return;
        }
        std::string recipePrompt;
        if (clip.size() <= 65536 && applyRecipeCode(clip, recipePrompt)) {
            if (m_promptInput) m_promptInput->setString(recipePrompt);
            Notification::create("Recipe loaded", NotificationIcon::Success)->show();
            return;
        }
        if (eas::looksLikeEAS(clip) || clip.find("## Level Script") != std::string::npos) {
            if (m_isGenerating || m_isCreatingObjects || s_inPreviewMode || s_inEditMode) {
                Notification::create("Finish the current generation/preview first",
                                     NotificationIcon::Warning)->show();
                return;
            }
            auto er = eas::parse(eas::extractScript(clip));
            if (!er.ok) {
                Notification::create("Clipboard EAS didn't parse",
                                     NotificationIcon::Error)->show();
                return;
            }
            auto objectsArray = er.root.contains("objects") && er.root["objects"].isArray()
                ? er.root["objects"] : matjson::Value::array();
            if (er.root.contains("macros") && er.root["macros"].isArray()) {
                std::vector<matjson::Value> expanded;
                macros::expandAll(er.root["macros"], expanded);
                for (auto& obj : expanded) objectsArray.push(std::move(obj));
            }
            if (objectsArray.size() == 0) {
                Notification::create("EAS parsed but had no objects",
                                     NotificationIcon::Warning)->show();
                return;
            }
            log::info("Clipboard import: staging {} objects", objectsArray.size());
            // Same invariant note as reapplyFromHistory: no clear-flag override.
            prepareObjects(objectsArray);
            return;
        }
        Notification::create("Clipboard has neither a recipe nor EAS",
                             NotificationIcon::Warning)->show();
    }

    // Re-stage a past generation as a fresh blueprint — no API call.
    void reapplyFromHistory(const std::string& objectsJson) {
        if (m_isGenerating || m_isCreatingObjects || s_inPreviewMode || s_inEditMode) {
            Notification::create("Finish the current generation/preview first",
                                 NotificationIcon::Warning)->show();
            return;
        }
        auto parsed = editorai::json_lenient::parse(objectsJson);
        if (!parsed.ok || !parsed.value.isArray() || parsed.value.size() == 0) {
            Notification::create("Stored generation couldn't be parsed",
                                 NotificationIcon::Error)->show();
            return;
        }
        log::info("History re-apply: staging {} objects", parsed.value.size());
        // Strip edit ops defensively (old files may carry them): replaying a
        // MOVE/DELETE against today's level would hit unrelated objects.
        {
            auto cleaned = matjson::Value::array();
            for (size_t i = 0; i < parsed.value.size(); ++i) {
                const auto& e = parsed.value[i];
                if (e.isObject() && e.contains("op")) continue;
                cleaned.push(e);
            }
            parsed.value = std::move(cleaned);
        }
        if (parsed.value.size() == 0) {
            Notification::create("That entry only contained level edits - "
                                 "nothing to re-stage", NotificationIcon::Warning)->show();
            return;
        }
        // NOTE: do NOT touch m_shouldClearLevel here — staging never clears,
        // and overriding it would silently break the next normal generation's
        // clear-level dialog (invariant: m_shouldClearLevel == !m_editMode).
        prepareObjects(parsed.value);
    }

    // ── Platinum queue status ──────────────────────────────────────────────
    // 5 s poll while the popup is open and Platinum is selected. Result is a
    // short string the cost-label tick appends. Endpoint absent → silent.
    void pollPlatinumStatus(float) {
        if (m_isGenerating) return;
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        if (provider != "ollama" || !Mod::get()->getSettingValue<bool>("use-platinum")) {
            m_platinumStatus.clear();
            return;
        }
        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(4));
        // Live coordinator (VLT GG-hosted distributed Ollama network).
        m_platinumPoll.spawn(
            request.get("http://sn-1.vltgg.net:21800/api/status"),
            [this](web::WebResponse resp) {
                if (!resp.ok()) { m_platinumStatus.clear(); return; }
                auto json = resp.json();
                if (!json) { m_platinumStatus.clear(); return; }
                const auto j = json.unwrap();
                if (!j.contains("coordinator")) { m_platinumStatus.clear(); return; }
                const auto& coord = j["coordinator"];
                auto queued  = coord["queued_requests"].asInt();
                auto active  = coord["active_workers"].asInt();
                auto workers = coord["workers"].asInt();
                int q = queued  ? (int)queued.unwrap()  : 0;
                int a = active  ? (int)active.unwrap()  : 0;
                int w = workers ? (int)workers.unwrap() : 0;
                if (w <= 0)
                    m_platinumStatus = "Platinum: no workers online";
                else if (q <= 0)
                    m_platinumStatus = fmt::format("Platinum: idle ({} worker{})",
                                                   w, w == 1 ? "" : "s");
                else
                    m_platinumStatus = fmt::format("Platinum: {} queued, {}/{} busy",
                                                   q, a, w);
                m_lastCostPrompt.clear();  // force the cost label to refresh
            });
    }

    // ── Idle cost estimate ─────────────────────────────────────────────────
    // chars/4 heuristic + per-provider price table. Rough by design — the
    // label says "est". Local providers show as free.
    void updateCostEstimate(float) {
        // Provider chip live-refresh: settings changed behind this popup
        // (the settings popup stacks on top) used to leave stale text here.
        if (m_chipLabel) {
            std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
            if (provider != m_chipProviderShown) {
                m_chipProviderShown = provider;
                std::string model = getProviderModel(provider);
                std::string shownName = provider;
                if (provider == "custom") {
                    std::string nm = Mod::get()->getSettingValue<std::string>("custom-provider-name");
                    if (!nm.empty()) shownName = nm;
                }
                m_chipLabel->setString(fmt::format("{} - {}", shownName, model).c_str());
                m_chipLabel->limitLabelWidth(168.f, 0.3f, 0.1f);
                if (m_chipDot) {
                    bool local = provider == "ollama" || provider == "lm-studio" ||
                                 provider == "llama-cpp";
                    bool hasAny = !oauth::savedToken(provider).empty() ||
                                  !Mod::get()->getSettingValue<std::string>(
                                      provider == "custom"
                                          ? std::string("custom-provider-api-key")
                                          : provider + "-api-key").empty() ||
                                  provider == "custom";
                    m_chipDot->setColor(local ? ui::WARN_COL
                                      : hasAny ? ui::SUCCESS_COL : ui::ERROR_COL);
                }
                m_sysPromptLenEst = buildSystemPrompt().size();  // provider changed → re-measure
                m_lastCostPrompt.clear();                        // force cost refresh
            }
        }
        if (!m_costLabel) return;
        // Status label owns the slot while generating.
        if (m_isGenerating || (m_statusLabel && m_statusLabel->isVisible())) {
            m_costLabel->setVisible(false);
            return;
        }
        std::string prompt = m_promptInput ? m_promptInput->getString() : "";
        if (prompt == m_lastCostPrompt && m_costLabel->isVisible()) return;
        m_lastCostPrompt = prompt;

        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        bool local = provider == "ollama" || provider == "lm-studio" ||
                     provider == "llama-cpp";
        size_t inTokens = m_sysPromptLenEst / 4 + prompt.size() / 4 + 900 /*tools+few-shot*/;
        if (local) {
            bool platinum = provider == "ollama" &&
                            Mod::get()->getSettingValue<bool>("use-platinum");
            std::string tail = platinum
                ? (m_platinumStatus.empty() ? std::string("Platinum (free)") : m_platinumStatus)
                : std::string("local, free");
            m_costLabel->setString(fmt::format("~{:.1f}k tok in - {}",
                inTokens / 1000.0, tail).c_str());
        } else {
            // $/1M input, $/1M output — coarse mid-2026 list prices.
            struct Price { const char* p; double in, out; };
            static constexpr Price PRICES[] = {
                {"gemini", 0.35, 1.50}, {"claude", 3.0, 15.0}, {"openai", 2.5, 10.0},
                {"openrouter", 1.0, 3.0}, {"ministral", 0.10, 0.30},
                {"huggingface", 0.30, 0.60}, {"deepseek", 0.27, 1.10},
                {"custom", 1.0, 3.0},
            };
            double inP = 1.0, outP = 3.0;
            for (auto& pr : PRICES)
                if (provider == pr.p) { inP = pr.in; outP = pr.out; break; }
            constexpr double EST_OUT_TOKENS = 4000.0;
            double perCall = inTokens / 1e6 * inP + EST_OUT_TOKENS / 1e6 * outP;
            // Quality rounds multiply the spend — say so. One generation =
            // initial + refinement rounds + two-pass + self-review.
            int rounds = 1
                + (int)Mod::get()->getSettingValue<int64_t>("refinement-rounds")
                + (Mod::get()->getSettingValue<bool>("two-pass-generation") ? 1 : 0)
                + (Mod::get()->getSettingValue<bool>("enable-self-critique") ? 1 : 0);
            rounds = std::max(rounds, 1);
            m_costLabel->setString(fmt::format(
                "~{:.1f}k tok in - ~${:.3f} est (x{} rounds)",
                inTokens / 1000.0, perCall * rounds, rounds).c_str());
        }
        m_costLabel->limitLabelWidth(220.f, 0.45f, 0.3f);
        m_costLabel->setVisible(true);
    }

    void onToggleCoop(CCObject*) {
        m_coopMode = !m_coopMode;
        // Edit and co-op both inject a MODE section — mutually exclusive.
        if (m_coopMode && m_editMode) {
            m_editMode = false;
            m_shouldClearLevel = true;
            if (m_editModeToggle) m_editModeToggle->toggle(false);
        }
        log::info("Co-op mode: {}", m_coopMode ? "ON" : "OFF");
    }
    void onCoopLabel(CCObject*) {
        if (m_coopToggle) m_coopToggle->activate();
    }

    // Tapping the edit-mode label flips the checkbox (and fires its callback).
    void onEditModeLabel(CCObject*) {
        if (m_editModeToggle) m_editModeToggle->activate();
    }

    // ── UI callbacks ──────────────────────────────────────────────────────────

    // Edit-mode toggle replaces the legacy Clear-Level toggle. Inverted
    // semantics: m_editMode = true ⇒ keep level + add edits; false ⇒ start fresh.
    // m_shouldClearLevel is now a derived view (== !m_editMode) so the rest of
    // the popup keeps working unchanged.
    void onToggleEditMode(CCObject*) {
        m_editMode = !m_editMode;
        m_shouldClearLevel = !m_editMode;
        // Mutual exclusion with co-op (see onToggleCoop).
        if (m_editMode && m_coopMode) {
            m_coopMode = false;
            if (m_coopToggle) m_coopToggle->toggle(false);
        }
        log::info("Edit-mode toggle: {} (clear-level derived: {})",
                  m_editMode ? "ON (additive)" : "OFF (fresh)",
                  m_shouldClearLevel ? "ON" : "OFF");
        // Retitle so the user sees the active mode reflected.
        // Hyphen, not em-dash: goldFont has no '—' glyph (it silently drops).
        // styleTitle re-applies scale — setTitle rebuilds the label.
        this->setTitle(m_editMode ? "Editor AI - Edit Mode" : "Editor AI");
        ui::styleTitle(m_title, 0.8f);
    }

    // Closing the popup mid-generation BACKGROUNDS it instead of killing it:
    // the session's Ref keeps this engine node (and its TaskHolders) alive,
    // network callbacks keep arriving via queueInMainThread, and the result
    // stages into whatever editor exists when it lands.
    void onClose(CCObject* sender) override {
        if (m_isGenerating) {
            if (m_session)
                m_session->push(GenSession::Entry::Kind::Status,
                                "Popup closed — generation continues in background");
            Notification::create(
                "Generation continues in background", NotificationIcon::Info)->show();
        }
        Popup::onClose(sender);
    }

    void onCancel(CCObject*) {
        if (!m_isGenerating) return;
        m_listener = {};  // destroy the task holder, cancelling the request
        // Also cancel any in-flight TOOL requests — with parallel tool
        // execution these run on their own holders, and a surviving callback
        // would push results into m_toolHistory and silently restart the
        // loop after the user cancelled. (Holder reset flips the shared
        // cancelled flag; the wrapper then drops the callback.)
        m_toolListenerLevel = {};
        m_toolListenerNG    = {};
        m_toolListenerWeb   = {};
        m_isGenerating = false;
        // Turn-scoped flags die with the turn — leaking them poisons the
        // next generation's gates (see startGeneration's reset block).
        m_followUpTurn    = false;
        m_critiquePending = false;
        if (m_session) {
            m_session->state = GenSession::State::Done;
            m_session->push(GenSession::Entry::Kind::Status, "Cancelled by user");
        }
        m_generateBtn->setVisible(true);
        m_generateBtn->setEnabled(true);
        m_cancelBtn->setVisible(false);
        showStatus("Cancelled.", true);
        log::info("EditorAI: generation cancelled by user");
    }

    void onHistoryUp(CCObject*) {
        if (s_promptHistory.empty()) return;
        if (s_promptHistoryIndex <= 0) return;
        --s_promptHistoryIndex;
        m_promptInput->setString(s_promptHistory[s_promptHistoryIndex]);
    }

    void onHistoryDown(CCObject*) {
        if (s_promptHistory.empty()) return;
        if (s_promptHistoryIndex >= (int)s_promptHistory.size() - 1) {
            s_promptHistoryIndex = (int)s_promptHistory.size();
            m_promptInput->setString("");
            return;
        }
        ++s_promptHistoryIndex;
        m_promptInput->setString(s_promptHistory[s_promptHistoryIndex]);
    }

    void onSettings(CCObject*) {
        // In-mod settings popup — no detour through Geode's settings page.
        // Defaults to General tab; the chip click route hands in Provider.
        if (auto* p = AISettingsPopup::create(AISettingsPopup::Tab::General))
            p->show();
    }

    // Clicking the provider chip opens the AI Settings popup on the Provider
    // tab — auth / model / API key all live there now.
    void onAccount(CCObject*) {
        if (auto* p = AISettingsPopup::create(AISettingsPopup::Tab::Provider))
            p->show();
    }

    void onInfo(CCObject*) {
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model    = getProviderModel(provider);
        std::string apiKey   = getProviderApiKey(provider);

        std::string keyStatus;
        if (provider == "ollama") {
            bool usePlatinum = Mod::get()->getSettingValue<bool>("use-platinum");
            keyStatus = usePlatinum ? "<cg>Platinum cloud</c>" : "<cg>Local — no key needed</c>";
        } else if (provider == "lm-studio") {
            keyStatus = "<cg>Local (LM Studio) — no key needed</c>";
        } else if (provider == "llama-cpp") {
            keyStatus = "<cg>Local (llama.cpp) — no key needed</c>";
        } else if (provider == "custom") {
            std::string customUrl = Mod::get()->getSettingValue<std::string>("custom-provider-url");
            if (customUrl.empty()) keyStatus = "<cr>URL not set</c>";
            else if (apiKey.empty()) keyStatus = "<cy>No key (OK for unauth servers)</c>";
            else keyStatus = "<cg>URL + key set</c>";
        } else {
            keyStatus = apiKey.empty()
                ? "<cr>Not set — go to mod settings</c>"
                : "<cg>Set</c>";
        }

        bool advFeatures = Mod::get()->getSettingValue<bool>("enable-advanced-features");
        int currentObjects = (m_editorLayer && m_editorLayer->m_objects)
            ? m_editorLayer->m_objects->count() : 0;

        FLAlertLayer::create(
            "Editor AI",
            gd::string(fmt::format(
                "<cy>Provider:</c> {}\n"
                "<cy>Model:</c> {}\n"
                "<cy>API Key:</c> {}\n"
                "<cy>Advanced Features:</c> {}\n"
                "<cy>Objects in library:</c> {}\n"
                "<cy>Objects in level:</c> {}",
                provider, model, keyStatus,
                advFeatures ? "<cg>ON</c>" : "<cr>OFF</c>",
                OBJECT_IDS.size(), currentObjects
            )),
            "OK"
        )->show();
    }

    // Base scale of m_statusLabel; the error micro-pop returns to this.
    static constexpr float STATUS_SCALE = 0.3f;

    void showStatus(const std::string& msg, bool error = false) {
        if (m_session && !msg.empty()) {
            // Skip exact-duplicate consecutive status lines (progress spam).
            auto& t = m_session->transcript;
            if (t.empty() || t.back().kind != GenSession::Entry::Kind::Status ||
                t.back().text != msg)
                m_session->push(GenSession::Entry::Kind::Status, msg);
        }
        m_statusLabel->setString(msg.c_str());
        m_statusLabel->limitLabelWidth(340.f, STATUS_SCALE, 0.1f);
        m_statusLabel->setColor(error ? ui::ERROR_COL : ui::SUCCESS_COL);
        m_statusLabel->setVisible(true);
        if (error) {
            // Subtle attention pop: 6% overshoot and settle. Stop any prior
            // pop so rapid errors can't compound the scale.
            float base = m_statusLabel->getScale();
            m_statusLabel->stopAllActions();
            m_statusLabel->runAction(CCSequence::create(
                CCScaleTo::create(0.05f, base * 1.06f),
                CCScaleTo::create(0.08f, base),
                nullptr));
        }
    }

    void updateGenerationTimer(float) {
        if (!m_isGenerating) {
            this->unschedule(schedule_selector(AIGeneratorPopup::updateGenerationTimer));
            return;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_generationStartTime).count();
        showStatus(fmt::format("AI is generating... ({}s)", elapsed));
    }

    // ── Level manipulation ────────────────────────────────────────────────────

    void clearLevel() {
        if (!m_editorLayer) return;
        auto objects = m_editorLayer->m_objects;
        if (!objects) return;

        int count = objects->count();
        // One batch call instead of per-object removeObject() — the per-object
        // path does full editor bookkeeping each time and visibly hangs the
        // frame on multi-thousand-object levels.
        m_editorLayer->removeAllObjects();

        log::info("Cleared {} objects from editor", count);
    }

    // Serialize the current editor objects to compact JSON so the AI can see what
    // is already in the level. Capped at 300 objects to keep the prompt manageable.
    // NOTE: Only called when m_shouldClearLevel is false (building on top of existing).
    // get_level_region tool: current editor objects with X in [x0, x1],
    // capped at 80. Same shape as buildLevelDataJson but range-filtered, so
    // the model can inspect one section without paying for the whole level.
    std::string buildLevelRegionJson(float x0, float x1) {
        if (!revalidateEditor() || !m_editorLayer->m_objects)
            return "{\"region_object_count\":0,\"objects\":[]}";
        auto objects = m_editorLayer->m_objects;

        static const std::unordered_map<int, std::string>& idToName = objectIdToName();

        constexpr int MAX_REPORT = 80;
        int total = 0, reported = 0;
        std::string items;
        for (auto* raw : CCArrayExt<CCObject*>(objects)) {
            auto* gameObj = typeinfo_cast<GameObject*>(raw);
            if (!gameObj) continue;
            float x = gameObj->getPositionX();
            if (x < x0 || x > x1) continue;
            ++total;
            if (reported >= MAX_REPORT) continue;
            std::string typeName = "unknown";
            auto it = idToName.find(gameObj->m_objectID);
            if (it != idToName.end()) typeName = it->second;
            if (reported) items += ",";
            items += fmt::format("{{\"type\":\"{}\",\"x\":{:.0f},\"y\":{:.0f}}}",
                                 typeName, x, gameObj->getPositionY());
            ++reported;
        }
        return fmt::format(
            "{{\"x_range\":[{:.0f},{:.0f}],\"region_object_count\":{},"
            "\"shown\":{},\"objects\":[{}]}}",
            x0, x1, total, reported, items);
    }

    // Current editor objects as a mod-format matjson array (capped) — the
    // source for mutation/co-op EAS context via eas::objectsToEAS.
    matjson::Value buildLevelDataArray(int cap = 500) {
        auto arr = matjson::Value::array();
        // revalidate first: the editor scene may have been freed mid-loop
        // (the loop is unbounded and survives backgrounding) — a stale
        // m_editorLayer here is a use-after-free.
        if (!revalidateEditor() || !m_editorLayer->m_objects) return arr;
        static const std::unordered_map<int, std::string>& idToName = objectIdToName();
        int added = 0;
        for (auto* raw : CCArrayExt<CCObject*>(m_editorLayer->m_objects)) {
            if (added >= cap) break;
            auto* gameObj = typeinfo_cast<GameObject*>(raw);
            if (!gameObj) continue;
            auto it = idToName.find(gameObj->m_objectID);
            if (it == idToName.end()) continue;
            auto o = matjson::Value::object();
            o["type"] = it->second;
            auto pos = gameObj->getPosition();
            o["x"] = (double)pos.x;
            o["y"] = (double)pos.y;
            float rot = gameObj->getRotation();
            if (std::abs(rot) > 0.01f) o["rotation"] = (double)rot;
            float scl = gameObj->getScale();
            if (std::abs(scl - 1.f) > 0.01f) o["scale"] = (double)scl;
            arr.push(std::move(o));
            ++added;
        }
        return arr;
    }

    std::string buildLevelDataJson() {
        if (!revalidateEditor() || !m_editorLayer->m_objects)
            return "{\"object_count\":0,\"objects\":[]}";

        auto objects = m_editorLayer->m_objects;
        if (objects->count() == 0)
            return "{\"object_count\":0,\"objects\":[]}";

        static const std::unordered_map<int, std::string>& idToName = objectIdToName();

        int totalCount = objects->count();
        int maxReport  = std::min(totalCount, 300);

        std::string result;
        result.reserve(maxReport * 60);
        result += fmt::format("{{\"object_count\":{},\"objects\":[", totalCount);

        bool first   = true;
        int reported = 0;
        for (auto* raw : CCArrayExt<CCObject*>(objects)) {
            if (reported >= maxReport) break;
            auto* gameObj = typeinfo_cast<GameObject*>(raw);
            if (!gameObj) continue;

            int   id  = gameObj->m_objectID;
            auto  pos = gameObj->getPosition();
            float rot = gameObj->getRotation();
            float scl = gameObj->getScale();

            std::string typeName;
            if (id == 899) typeName = "color_trigger";
            else if (id == 901) typeName = "move_trigger";
            else if (id == 34)  typeName = "end_trigger";
            else {
                typeName = "unknown";
                auto it = idToName.find(id);
                if (it != idToName.end()) typeName = it->second;
            }

            if (!first) result += ",";
            result += fmt::format(
                "{{\"type\":\"{}\",\"x\":{:.0f},\"y\":{:.0f}",
                typeName, pos.x, pos.y
            );
            if (rot != 0.0f) result += fmt::format(",\"rotation\":{:.1f}", rot);
            if (scl != 1.0f) result += fmt::format(",\"scale\":{:.2f}", scl);
            result += "}";

            first = false;
            ++reported;
        }

        if (totalCount > maxReport)
            result += fmt::format(",{{\"note\":\"...{} more objects not shown\"}}", totalCount - maxReport);

        result += "]}";
        return result;
    }

    // ── Edit-op machinery (MOVE / DELETE / EDIT on existing objects) ─────────

    // Numbered inventory of the CURRENT editor objects, sorted by X then Y.
    // The listing string goes to the model; m_editInventory keeps the same
    // ordering as Refs so `#index` selectors in its reply resolve to the
    // exact objects it saw. Beyond the cap, a density histogram tells the
    // model what's out there so rect:/id: selectors can still reach it.
    std::vector<Ref<GameObject>> m_editInventory;

    std::string buildLevelInventoryListing(int cap) {
        m_editInventory.clear();
        // Self-revalidate (stale-pointer safe) like the other build* helpers —
        // a freed-but-non-null m_editorLayer here would be a use-after-free
        // the bare null check can't catch. revalidateEditor() is idempotent.
        if (!revalidateEditor() || !m_editorLayer->m_objects ||
            m_editorLayer->m_objects->count() == 0)
            return "(the level is empty)";
        static const std::unordered_map<int, std::string>& idToName = objectIdToName();
        std::vector<GameObject*> objs;
        objs.reserve(m_editorLayer->m_objects->count());
        for (auto* raw : CCArrayExt<CCObject*>(m_editorLayer->m_objects)) {
            auto* go = typeinfo_cast<GameObject*>(raw);
            if (!go || s_editOpDeleted.count(go)) continue;
            objs.push_back(go);
        }
        std::sort(objs.begin(), objs.end(), [](GameObject* a, GameObject* b) {
            float ax = a->getPositionX(), bx = b->getPositionX();
            if (ax != bx) return ax < bx;
            return a->getPositionY() < b->getPositionY();
        });

        std::string out;
        int listed = (int)std::min<size_t>(objs.size(), (size_t)cap);
        out.reserve((size_t)listed * 40);
        out += fmt::format("OBJECT INVENTORY ({} objects total, first {} listed; "
                           "#index is stable for THIS turn's MOVE/DELETE/EDIT):\n",
                           objs.size(), listed);
        for (int i = 0; i < listed; ++i) {
            auto* go = objs[i];
            auto it = idToName.find(go->m_objectID);
            std::string name = it != idToName.end()
                ? it->second : fmt::format("obj{}", go->m_objectID);
            out += fmt::format("#{} {} x={:.0f} y={:.0f}",
                i, name, go->getPositionX(), go->getPositionY());
            float rot = go->getRotation();
            if (std::abs(rot) > 0.01f) out += fmt::format(" r={:.0f}", rot);
            float scl = go->getScale();
            if (std::abs(scl - 1.f) > 0.01f) out += fmt::format(" s={:.2f}", scl);
            out += "\n";
        }
        if ((int)objs.size() > listed) {
            out += "DENSITY BEYOND THE LISTING (use rect:/id: selectors there):\n";
            std::map<int, int> buckets;
            for (size_t i = listed; i < objs.size(); ++i)
                ++buckets[(int)(objs[i]->getPositionX() / 500.f)];
            for (auto& [b, n] : buckets)
                out += fmt::format("  X {}-{}: {} objects\n", b * 500, b * 500 + 500, n);
        }
        m_editInventory.assign(objs.begin(), objs.begin() + listed);
        return out;
    }

    // Resolve one op's selector to live objects. Read-only — shared by the
    // applier and by the enforcement gate's planned-edit counter.
    std::vector<GameObject*> resolveOpSelector(const matjson::Value& op) {
        std::vector<GameObject*> out;
        if (!revalidateEditor() || !m_editorLayer->m_objects) return out;
        std::string sel = levelcheck::getStr(op, "sel", "");
        if (sel.empty()) return out;
        constexpr size_t CAP = 3000;

        // Optional filters that combine with rect selectors.
        int filterId = (int)levelcheck::getFloat(op, "filter_id", 0.f);
        std::string filterType = levelcheck::getStr(op, "filter_type", "");
        if (!filterType.empty() && filterId == 0) {
            auto it = OBJECT_IDS.find(filterType);
            if (it != OBJECT_IDS.end()) filterId = it->second;
        }

        auto passesFilter = [&](GameObject* go) {
            return filterId == 0 || go->m_objectID == filterId;
        };

        if (sel[0] == '#') {
            // #a or #a-b over the inventory snapshot.
            std::string body = sel.substr(1);
            auto dash = body.find('-');
            int a = -1, b = -1;
            if (dash == std::string::npos) {
                a = b = geode::utils::numFromString<int>(body).unwrapOr(-1);
            } else {
                a = geode::utils::numFromString<int>(body.substr(0, dash)).unwrapOr(-1);
                b = geode::utils::numFromString<int>(body.substr(dash + 1)).unwrapOr(-1);
            }
            if (a < 0 || b < a) return out;
            // The inventory may be stale on an adopted editor — rebuild it so
            // indices resolve against the same sorted ordering the model saw.
            if (m_editInventory.empty()) buildLevelInventoryListing(2500);
            for (int i = a; i <= b && i < (int)m_editInventory.size(); ++i) {
                GameObject* go = m_editInventory[i];
                if (go && go->getParent() && !s_editOpDeleted.count(go)
                    && passesFilter(go))
                    out.push_back(go);
                if (out.size() >= CAP) break;
            }
            return out;
        }

        float rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
        bool useRect = false;
        if (sel.rfind("rect:", 0) == 0) {
            std::array<float, 4> v{};
            size_t pos = 5;
            int got = 0;
            while (got < 4 && pos <= sel.size()) {
                size_t comma = sel.find(',', pos);
                std::string tok = sel.substr(pos,
                    comma == std::string::npos ? std::string::npos : comma - pos);
                v[got++] = eas::tryFloat(tok, 0.f);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (got < 4) return out;
            rx0 = std::min(v[0], v[2]); rx1 = std::max(v[0], v[2]);
            ry0 = std::min(v[1], v[3]); ry1 = std::max(v[1], v[3]);
            useRect = true;
        } else if (sel.rfind("id:", 0) == 0) {
            std::string body = sel.substr(3);
            int id = geode::utils::numFromString<int>(body).unwrapOr(0);
            if (id == 0) {
                auto it = OBJECT_IDS.find(body);
                if (it != OBJECT_IDS.end()) id = it->second;
            }
            if (id == 0) return out;
            filterId = id;
        } else {
            log::warn("Edit op: unknown selector '{}'", sel);
            return out;
        }

        for (auto* raw : CCArrayExt<CCObject*>(m_editorLayer->m_objects)) {
            auto* go = typeinfo_cast<GameObject*>(raw);
            if (!go || s_editOpDeleted.count(go)) continue;
            if (useRect) {
                float x = go->getPositionX(), y = go->getPositionY();
                if (x < rx0 || x > rx1 || y < ry0 || y > ry1) continue;
            }
            if (!passesFilter(go)) continue;
            out.push_back(go);
            if (out.size() >= CAP) break;
        }
        return out;
    }

    // Journal an object once (first touch wins — that's the state Deny
    // must restore).
    void journalEditOp(GameObject* go, bool asDelete) {
        for (auto& rec : s_editOpJournal)
            if (rec.obj == go) {
                if (asDelete) rec.deleted = true;
                return;
            }
        EditOpRecord rec;
        rec.obj        = go;
        rec.pos        = go->getPosition();
        rec.rot        = go->getRotation();
        rec.scaleX     = go->getScaleX();
        rec.scaleY     = go->getScaleY();
        if (go->m_baseColor) {
            rec.hadBaseColor = true;
            rec.baseColorID  = go->m_baseColor->m_colorID;
        }
        if (go->m_detailColor) {
            rec.hadDetailColor = true;
            rec.detailColorID  = go->m_detailColor->m_colorID;
        }
        rec.wasVisible = go->isVisible();
        rec.deleted    = asDelete;
        s_editOpJournal.push_back(std::move(rec));
    }

    // Execute every op against the live editor. Returns objects affected.
    int applyEditOps(const std::vector<matjson::Value>& ops) {
        if (!revalidateEditor()) return 0;
        auto* ui = m_editorLayer->m_editorUI;
        int affected = 0;
        for (const auto& op : ops) {
            std::string kind = levelcheck::getStr(op, "op", "");
            auto targets = resolveOpSelector(op);
            if (targets.empty()) {
                log::warn("Edit op '{}' sel='{}' matched nothing",
                          kind, levelcheck::getStr(op, "sel", ""));
                continue;
            }
            if (kind == "move") {
                float dx = levelcheck::getFloat(op, "dx", 0.f);
                float dy = levelcheck::getFloat(op, "dy", 0.f);
                if (dx == 0.f && dy == 0.f) continue;
                for (auto* go : targets) {
                    journalEditOp(go, false);
                    if (ui) ui->moveObject(go, {dx, dy});
                    else {
                        go->setPosition(go->getPosition() + CCPoint{dx, dy});
                        m_editorLayer->updateObjectSection(go);
                    }
                    ++affected;
                }
            } else if (kind == "delete") {
                for (auto* go : targets) {
                    journalEditOp(go, true);
                    go->setVisible(false);          // soft until Accept
                    s_editOpDeleted.insert(go);
                    ++affected;
                }
            } else if (kind == "edit") {
                bool hasRot   = op.contains("rotation");
                bool hasScale = op.contains("scale");
                bool flipX    = levelcheck::getBool(op, "flip_x", false);
                bool flipY    = levelcheck::getBool(op, "flip_y", false);
                int  colorCh  = (int)levelcheck::getFloat(op, "color_channel", 0.f);
                int  detailCh = (int)levelcheck::getFloat(op, "detail_color_channel", 0.f);
                for (auto* go : targets) {
                    journalEditOp(go, false);
                    if (hasRot)
                        go->setRotation(levelcheck::getFloat(op, "rotation", 0.f));
                    if (hasScale)
                        go->setScale(levelcheck::getFloat(op, "scale", 1.f));
                    if (flipX) go->setScaleX(-go->getScaleX());
                    if (flipY) go->setScaleY(-go->getScaleY());
                    if (colorCh > 0 && go->m_baseColor)
                        go->m_baseColor->m_colorID = colorCh;
                    if (detailCh > 0 && go->m_detailColor)
                        go->m_detailColor->m_colorID = detailCh;
                    ++affected;
                }
            }
        }
        if (affected > 0)
            log::info("Applied {} edit op(s) touching {} objects",
                      ops.size(), affected);
        return affected;
    }

    // Planned-edit counter for the enforcement gate: ops in the accumulator
    // resolved (read-only) + plain objects each counting 1.
    int countPlannedEdits() {
        int total = 0;
        for (size_t i = 0; i < m_accumulatedObjects.size(); ++i) {
            const auto& e = m_accumulatedObjects[i];
            if (!e.isObject()) continue;
            if (e.contains("op") && e["op"].isString())
                total += (int)resolveOpSelector(e).size();
            else
                ++total;
        }
        return total;
    }

    // ── Progressive object spawner ────────────────────────────────────────────

    // Simulated cube run drawn over the staged blueprint: green where the
    // bot survives, red rings where it dies. Advisory only.
    // ── Vision: level snapshot for image-capable models ─────────────────────
    // Renders the editor's object layer (framed around the generated region)
    // to a small PNG and returns it base64-encoded; empty when capture isn't
    // possible. Attached to review/refinement/follow-up turns so the model
    // can SEE what it built instead of inferring from coordinates.
    std::string m_snapCacheB64;       // last snapshot, keyed by fingerprint
    size_t      m_snapCacheFp = 0;    // (the encode is main-thread disk IO —
                                      //  never pay it twice for the same view)

    std::string captureLevelSnapshotB64() {
        if (!revalidateEditor() || !m_editorLayer->m_objectLayer) return "";
        size_t editorCount = m_editorLayer->m_objects
            ? (size_t)m_editorLayer->m_objects->count() : 0;
        size_t fp = m_accumulatedObjects.size() * 1315423911u
                  ^ s_previewObjects.size()     * 2654435761u
                  ^ editorCount                 * 97u;
        if (fp == m_snapCacheFp && !m_snapCacheB64.empty())
            return m_snapCacheB64;

        // Frame from the accumulated draft; fall back to the whole level.
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        auto widen = [&](float x, float y) {
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
        };
        for (size_t i = 0; i < m_accumulatedObjects.size(); ++i) {
            const auto& o = m_accumulatedObjects[i];
            if (!o.isObject()) continue;
            widen(levelcheck::getFloat(o, "x", 0.f),
                  levelcheck::getFloat(o, "y", 0.f));
        }
        if (maxX <= minX && m_editorLayer->m_objects) {
            for (auto* raw : CCArrayExt<CCObject*>(m_editorLayer->m_objects)) {
                if (auto* go = typeinfo_cast<GameObject*>(raw))
                    widen(go->getPositionX(), go->getPositionY());
            }
        }
        if (maxX <= minX) return "";
        minX -= 60.f; maxX += 60.f;
        minY = std::max(minY - 90.f, 0.f); maxY += 90.f;
        // Very long levels: cap the framed span so objects stay legible.
        if (maxX - minX > 9000.f) maxX = minX + 9000.f;
        float dx = maxX - minX;
        float dy = std::max(maxY - minY, 240.f);

        auto* objLayer = m_editorLayer->m_objectLayer;
        auto renderAt = [&](int W) -> std::vector<std::uint8_t> {
            int H = (int)std::clamp(W * dy / dx, 160.f, 640.f);
            auto* rt = CCRenderTexture::create(W, H);
            if (!rt) return {};
            // Reframe, render, restore — main thread, between frames.
            float oldScale = objLayer->getScale();
            CCPoint oldPos = objLayer->getPosition();
            float scale = std::min((float)W / dx, (float)H / dy);
            objLayer->setScale(scale);
            objLayer->setPosition({-minX * scale, -minY * scale});
            rt->beginWithClear(0.10f, 0.11f, 0.14f, 1.f);
            objLayer->visit();
            rt->end();
            objLayer->setScale(oldScale);
            objLayer->setPosition(oldPos);
            CCImage* img = rt->newCCImage(true);
            if (!img) return {};
            auto path = Mod::get()->getSaveDir() / "level-snapshot.png";
            std::string pathStr = utils::string::pathToString(path);
            bool saved = img->saveToFile(pathStr.c_str(), false);
            img->release();
            if (!saved) return {};
            auto bytes = utils::file::readBinary(path);
            if (!bytes) return {};
            auto data = bytes.unwrap();
            log::info("Vision snapshot: {}x{}, {} KB", W, H, data.size() / 1024);
            return data;
        };
        // 800px is plenty for the model; drop to 512 if a dense level
        // compresses badly, and bail rather than ship a megabyte.
        auto vec = renderAt(800);
        if (vec.size() > 700'000) vec = renderAt(512);
        if (vec.empty() || vec.size() > 900'000) return "";
        m_snapCacheB64 = utils::base64::encode(
            std::span<const std::uint8_t>(vec.data(), vec.size()),
            utils::base64::Base64Variant::Normal);
        m_snapCacheFp = fp;
        return m_snapCacheB64;
    }

    // Gate + capture in one call. Empty when: vision off, model can't see,
    // or the editor view would MISLEAD (fresh-mode generation still shows
    // the old level until apply).
    std::string visionSnapshotIfSupported() {
        if (!Mod::get()->getSettingValue<bool>("enable-vision")) return "";
        if (m_shouldClearLevel && !s_inPreviewMode) return "";
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        if (!toolUse::supportsVision(provider, getProviderModel(provider)))
            return "";
        return captureLevelSnapshotB64();
    }

    void drawPlaytestGhost() {
        removePlaytestGhost();
        if (!m_editorLayer || !m_editorLayer->m_objectLayer) return;
        if (m_deferredObjects.empty()) return;

        // Rebuild a lightweight matjson array from the deferred slots (the
        // accumulator was moved out at final apply).
        auto arr = matjson::Value::array();
        for (auto& d : m_deferredObjects) {
            auto o = matjson::Value::object();
            const matjson::Value& dataConst = d.data;
            auto typeRes = dataConst["type"].asString();
            if (!typeRes) continue;
            o["type"] = typeRes.unwrap();
            o["x"] = (double)d.position.x;
            o["y"] = (double)d.position.y;
            auto sc = dataConst["scale"].asDouble();
            if (sc) o["scale"] = sc.unwrap();
            arr.push(std::move(o));
        }
        auto sim = levelcheck::simulateCube(arr,
            (float)Mod::get()->getSettingValue<int64_t>("ai-ground-y"));
        if (sim.path.size() < 2) return;

        auto draw = CCDrawNode::create();
        constexpr ccColor4F PATH_COL  {0.25f, 0.9f, 0.35f, 0.55f};
        constexpr ccColor4F DEATH_COL {1.f, 0.25f, 0.25f, 0.9f};
        for (size_t i = 1; i < sim.path.size(); ++i)
            draw->drawSegment(sim.path[i - 1], sim.path[i], 1.2f, PATH_COL);
        for (auto& d : sim.deaths) {
            draw->drawDot({d.x, d.y}, 6.f, DEATH_COL);
            draw->drawCircle({d.x, d.y}, 14.f, DEATH_COL, 1.5f, ccColor4F{0,0,0,0}, 24);
        }
        draw->setZOrder(900);
        m_editorLayer->m_objectLayer->addChild(draw);
        s_playtestGhost = draw;
        if (!sim.deaths.empty())
            log::info("Playtest ghost: {} death(s), first at X={:.0f}",
                      sim.deaths.size(), sim.deaths[0].x);
    }

    // Spawns one deferred object as a ghost. Returns false when the editor
    // is gone (revalidated against the live scene).
    bool spawnDeferredOne() {
        if (!revalidateEditor()) {
            log::error("Editor layer destroyed during object creation!");
            m_isCreatingObjects = false;
            return false;
        }
        auto& deferred = m_deferredObjects[m_currentObjectIndex];
        GameObject* gameObj =
            m_editorLayer->createObject(deferred.objectID, deferred.position, false);
        if (gameObj && gameObj->m_objectID) {
            applyObjectProperties(gameObj, deferred.data);
            // Layer-based preview: remember the layer(s) the AI assigned
            // (Accept restores them), then move the object onto the preview
            // layer. The editor is already switched there, so the new build
            // renders full-color over the faded level.
            s_previewIntendedLayers.emplace_back(
                gameObj->m_editorLayer, gameObj->m_editorLayer2);
            gameObj->m_editorLayer  = s_previewLayer;
            gameObj->m_editorLayer2 = s_previewLayer;
            s_previewObjects.emplace_back(gameObj);
        }
        ++m_currentObjectIndex;
        return true;
    }

    // Completion transition shared by the 20 Hz tick and immediate staging.
    void finishSpawning() {
        m_isCreatingObjects = false;

        // Playtest ghost: run the cube bot over what was just staged and
        // draw its trajectory on the editor (sub-millisecond — swept
        // geometry, one pass per generation).
        this->drawPlaytestGhost();

        // Enter blueprint preview mode — ghost objects are placed,
        // user must accept or deny via buttons on the editor UI.
        s_inPreviewMode = true;

        if (m_editorLayer && m_editorLayer->m_editorUI) {
            m_editorLayer->m_editorUI->updateButtons();
            showPreviewButtonsOnEditorUI(m_editorLayer->m_editorUI);
        }

        if (m_generateBtn) m_generateBtn->setEnabled(true);
        // Edit ops (moves/deletes/restyles of existing objects) are part of
        // the staged changeset too — surface them next to the spawn count.
        std::string editNote = s_editOpJournal.empty()
            ? std::string()
            : fmt::format(" + {} edited", s_editOpJournal.size());
        showStatus(fmt::format("Preview: {} objects{}",
                               s_previewObjects.size(), editNote), false);
        if (m_session) {
            m_session->state = GenSession::State::Staged;
            m_session->push(GenSession::Entry::Kind::Status,
                fmt::format("Staged {} objects{} on preview layer {}",
                            s_previewObjects.size(), editNote, s_previewLayer));
        }
        Notification::create(
            fmt::format("Preview on layer {}: {} objects{} — accept or deny",
                s_previewLayer, s_previewObjects.size(), editNote),
            NotificationIcon::Info
        )->show();

        // Auto-close only applies while the popup is actually on screen.
        if (this->getParent()) {
            this->runAction(CCSequence::create(
                CCDelayTime::create(1.5f),
                CCCallFunc::create(this, callfunc_selector(AIGeneratorPopup::closePopup)),
                nullptr
            ));
        }

        m_deferredObjects.clear();
        m_currentObjectIndex = 0;
    }

    // One-pass staging for editor adoption (the spawn scheduler is dead on
    // an off-scene node). 500 objects in one frame is ~tens of ms — fine
    // for the rare adopt path.
    void stageAllDeferredImmediate() {
        if (m_deferredObjects.empty()) return;
        m_isCreatingObjects = true;
        while (m_currentObjectIndex < m_deferredObjects.size())
            if (!spawnDeferredOne()) return;
        finishSpawning();
    }

    void updateObjectCreation(float /*dt*/) {
        if (!m_isCreatingObjects || m_deferredObjects.empty()) return;

        if (m_currentObjectIndex >= m_deferredObjects.size()) {
            finishSpawning();
            return;
        }

        for (int b = 0; b < m_spawnBatchSize && m_currentObjectIndex < m_deferredObjects.size(); ++b)
            if (!spawnDeferredOne()) return;

        if (m_currentObjectIndex % 10 == 0) {
            float pct = (float)m_currentObjectIndex / (float)m_deferredObjects.size() * 100.0f;
            showStatus(fmt::format("Creating objects... {:.0f}%", pct), false);
        }
    }

    void applyObjectProperties(GameObject* gameObj, matjson::Value& objData) {
        if (!gameObj) return;
        bool advFeatures = m_advFeatures;

        // Merge in any block_template properties for this object's type FIRST,
        // so they act as defaults the per-object fields can override. Mutates
        // objData in place; safe because we only fill in missing keys.
        int templateMerged = applyBlockTemplateToObject(objData);
        if (templateMerged > 0) {
            log::debug("Applied block template ({} props) to object id {}",
                       templateMerged, gameObj->m_objectID);
        }

        {
            // Probe through a CONST ref: non-const matjson operator[] inserts a
            // null member on every missing key — per object, per spawn tick.
            const matjson::Value& objConst = objData;
            // ── Basic transform ───────────────────────────────────────────────
            auto rotResult = objConst["rotation"].asDouble();
            if (rotResult) {
                float r = static_cast<float>(rotResult.unwrap());
                if (r >= -360.0f && r <= 360.0f) gameObj->setRotation(r);
            }
            auto scaleResult = objConst["scale"].asDouble();
            if (scaleResult) {
                float s = static_cast<float>(scaleResult.unwrap());
                if (s >= 0.1f && s <= 10.0f) gameObj->setScale(s);
            }
            auto flipXResult = objConst["flip_x"].asBool();
            if (flipXResult && flipXResult.unwrap())
                gameObj->setScaleX(-gameObj->getScaleX());

            auto flipYResult = objConst["flip_y"].asBool();
            if (flipYResult && flipYResult.unwrap())
                gameObj->setScaleY(-gameObj->getScaleY());

            // ── Z layering & editor layers ────────────────────────────────────
            // The EAS parser and macro passthroughs have carried these fields
            // since day one; this is where they finally land on the GameObject.
            // ZLayer uses GD's odd-number scheme: B5=-5, B4=-3, B3=-1, B2=1,
            // B1=3, Default=0, T1=5..T4=11 — accept those values verbatim.
            auto zLayerResult = objConst["z_layer"].asInt();
            if (zLayerResult) {
                // Only odd values + 0 are real ZLayer members; snap anything
                // else (the AI loves z_layer=2) to the nearest valid one.
                static constexpr int kValidZLayers[] = {-5, -3, -1, 0, 1, 3, 5, 7, 9, 11};
                int zl = std::clamp((int)zLayerResult.unwrap(), -5, 11);
                int best = 0, bestDist = 99;
                for (int v : kValidZLayers) {
                    int d = std::abs(zl - v);
                    if (d < bestDist) { bestDist = d; best = v; }
                }
                // setCustomZLayer also re-parents the sprite to the right
                // layer batch node — assigning m_zLayer alone leaves the
                // ghost rendering in its original layer until reload.
                gameObj->setCustomZLayer(best);
            }
            auto zOrderResult = objConst["z_order"].asInt();
            if (zOrderResult) {
                gameObj->m_zOrder = std::clamp((int)zOrderResult.unwrap(), -999, 999);
            }
            auto edLayerResult = objConst["editor_layer"].asInt();
            if (edLayerResult) {
                gameObj->m_editorLayer =
                    (short)std::clamp((int)edLayerResult.unwrap(), 0, 999);
            }
            auto edLayer2Result = objConst["editor_layer_2"].asInt();
            if (edLayer2Result) {
                gameObj->m_editorLayer2 =
                    (short)std::clamp((int)edLayer2Result.unwrap(), 0, 999);
            }

            // ── Group IDs (advanced features) ─────────────────────────────────
            if (advFeatures && objData.contains("groups") && objConst["groups"].isArray()) {
                auto& groupsArr = objConst["groups"];
                int assigned = 0;
                for (size_t gi = 0; gi < groupsArr.size() && assigned < 10; ++gi) {
                    auto gid = groupsArr[gi].asInt();
                    if (gid) {
                        int groupID = gid.unwrap();
                        if (groupID >= 1 && groupID <= 9999) {
                            if (gameObj->addToGroup(groupID) == 1 && m_editorLayer) {
                                m_editorLayer->addToGroup(gameObj, groupID, false);
                                ++assigned;
                            }
                        }
                    }
                }
            }

            // ── Color channel assignment ──────────────────────────────────────
            // Assigns the object's base color to a GD color channel (1-999).
            // Objects on the same channel change together when a color trigger fires.
            auto baseColorResult = objConst["color_channel"].asInt();
            if (baseColorResult) {
                int ch = std::clamp((int)baseColorResult.unwrap(), 1, 1010);
                if (gameObj->m_baseColor) {
                    gameObj->m_baseColor->m_colorID = ch;
                }
            }

            auto detailColorResult = objConst["detail_color_channel"].asInt();
            if (detailColorResult) {
                int ch = std::clamp((int)detailColorResult.unwrap(), 1, 1010);
                if (gameObj->m_detailColor) {
                    gameObj->m_detailColor->m_colorID = ch;
                }
            }

            // ── 2.2 editor flags (any object) ─────────────────────────────────
            // Set-only (true): all members verified against the 2.2074
            // bindings. passable = player phases through a solid block;
            // no_touch = hazard loses its hitbox (decorative spikes).
            {
                auto setFlag = [&objConst](const char* key, bool& member) {
                    auto r = objConst[key].asBool();
                    if (r && r.unwrap()) member = true;
                };
                setFlag("passable",    gameObj->m_isPassable);
                setFlag("no_touch",    gameObj->m_isNoTouch);
                setFlag("hide",        gameObj->m_isHide);
                setFlag("no_glow",     gameObj->m_hasNoGlow);
                setFlag("dont_fade",   gameObj->m_isDontFade);
                setFlag("dont_enter",  gameObj->m_isDontEnter);
                setFlag("high_detail", gameObj->m_isHighDetail);
                setFlag("no_effects",  gameObj->m_hasNoEffects);
            }

            // One RTTI check for every trigger block below — the old code
            // repeated typeinfo_cast<EffectGameObject*> in each of the 9
            // per-trigger branches.
            auto* effectObj = advFeatures
                ? typeinfo_cast<EffectGameObject*>(gameObj) : nullptr;

            // ── Multi-activate (orbs, pads, triggers, portals) ─────────────
            if (advFeatures && effectObj) {
                auto multiResult = objConst["multi_activate"].asBool();
                if (multiResult && multiResult.unwrap()) {
                    effectObj->m_isMultiTriggered = true;
                }
            }

            // ── Color Trigger properties (advanced features, ID 899) ───────────
            if (advFeatures && gameObj->m_objectID == 899) {
                if (!effectObj) return;

                auto channelResult = objConst["color_channel"].asInt();
                if (channelResult) {
                    effectObj->m_targetColor = std::clamp((int)channelResult.unwrap(), 1, 1010);
                }

                auto colorHexResult = objConst["color"].asString();
                if (!colorHexResult && objConst.contains("color") &&
                    objConst["color"].isArray() && objConst["color"].size() >= 3) {
                    // EAS-parser origin: color stored as [r,g,b].
                    const auto& carr = objConst["color"];
                    auto rr = carr[0].asInt(); auto gg = carr[1].asInt(); auto bb = carr[2].asInt();
                    effectObj->m_triggerTargetColor = {
                        (GLubyte)std::clamp(rr ? (int)rr.unwrap() : 255, 0, 255),
                        (GLubyte)std::clamp(gg ? (int)gg.unwrap() : 255, 0, 255),
                        (GLubyte)std::clamp(bb ? (int)bb.unwrap() : 255, 0, 255)};
                }
                if (colorHexResult) {
                    GLubyte r = 255, g = 255, b = 255;
                    if (parseHexColor(colorHexResult.unwrap(), r, g, b)) {
                        effectObj->m_triggerTargetColor = {r, g, b};
                    }
                }

                auto durResult = objConst["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                auto blendResult = objConst["blending"].asBool();
                if (blendResult) {
                    effectObj->m_usesBlending = blendResult.unwrap();
                }

                auto opacityResult = objConst["opacity"].asDouble();
                if (opacityResult) {
                    effectObj->m_opacity = std::clamp(
                        static_cast<float>(opacityResult.unwrap()), 0.0f, 1.0f);
                }

            }

            // ── Move Trigger properties (advanced features, ID 901) ────────────
            if (advFeatures && gameObj->m_objectID == 901) {
                if (!effectObj) return;

                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto moveXResult = objConst["move_x"].asDouble();
                auto moveYResult = objConst["move_y"].asDouble();
                float offsetX = moveXResult ? static_cast<float>(moveXResult.unwrap()) : 0.0f;
                float offsetY = moveYResult ? static_cast<float>(moveYResult.unwrap()) : 0.0f;
                offsetX = std::clamp(offsetX, -32767.0f, 32767.0f);
                offsetY = std::clamp(offsetY, -32767.0f, 32767.0f);
                effectObj->m_moveOffset = CCPoint(offsetX, offsetY);

                auto durResult = objConst["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                auto easingResult = objConst["easing"].asInt();
                if (easingResult) {
                    int easingVal = std::clamp((int)easingResult.unwrap(), 0, 18);
                    effectObj->m_easingType = (EasingType)easingVal;
                } else {
                    effectObj->m_easingType = EasingType::None;
                }

                auto easingRateResult = objConst["easing_rate"].asDouble();
                if (easingRateResult) {
                    effectObj->m_easingRate = std::clamp(
                        static_cast<float>(easingRateResult.unwrap()), 0.01f, 100.0f);
                }

                auto lockXResult = objConst["lock_to_player_x"].asBool();
                if (lockXResult && lockXResult.unwrap()) {
                    effectObj->m_lockToPlayerX = true;
                }

                auto lockYResult = objConst["lock_to_player_y"].asBool();
                if (lockYResult && lockYResult.unwrap()) {
                    effectObj->m_lockToPlayerY = true;
                }

            }

            // ── Alpha Trigger (ID 1007) — fades a group's opacity ──────────────
            if (advFeatures && gameObj->m_objectID == 1007) {
                if (!effectObj) return;

                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto opacityResult = objConst["opacity"].asDouble();
                if (opacityResult) {
                    effectObj->m_opacity = std::clamp(
                        static_cast<float>(opacityResult.unwrap()), 0.0f, 1.0f);
                }

                auto durResult = objConst["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                auto easingResult = objConst["easing"].asInt();
                if (easingResult) {
                    effectObj->m_easingType = (EasingType)std::clamp((int)easingResult.unwrap(), 0, 18);
                }

                auto easingRateResult = objConst["easing_rate"].asDouble();
                if (easingRateResult) {
                    effectObj->m_easingRate = std::clamp(
                        static_cast<float>(easingRateResult.unwrap()), 0.01f, 100.0f);
                }

            }

            // ── Rotate Trigger (ID 1346) — rotates a group around a center ─────
            if (advFeatures && gameObj->m_objectID == 1346) {
                if (!effectObj) return;

                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto centerGroupResult = objConst["center_group"].asInt();
                if (centerGroupResult) {
                    effectObj->m_centerGroupID = std::clamp((int)centerGroupResult.unwrap(), 1, 9999);
                }

                auto degreesResult = objConst["degrees"].asDouble();
                if (degreesResult) {
                    effectObj->m_rotationDegrees = static_cast<float>(degreesResult.unwrap());
                }

                auto durResult = objConst["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                auto easingResult = objConst["easing"].asInt();
                if (easingResult) {
                    effectObj->m_easingType = (EasingType)std::clamp((int)easingResult.unwrap(), 0, 18);
                }

                auto easingRateResult = objConst["easing_rate"].asDouble();
                if (easingRateResult) {
                    effectObj->m_easingRate = std::clamp(
                        static_cast<float>(easingRateResult.unwrap()), 0.01f, 100.0f);
                }

                auto lockRotResult = objConst["lock_object_rotation"].asBool();
                if (lockRotResult && lockRotResult.unwrap()) {
                    effectObj->m_lockObjectRotation = true;
                }

            }

            // ── Toggle Trigger (ID 1049) — shows or hides a group ──────────────
            if (advFeatures && gameObj->m_objectID == 1049) {
                if (!effectObj) return;

                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto activateResult = objConst["activate_group"].asBool();
                if (activateResult) {
                    effectObj->m_activateGroup = activateResult.unwrap();
                }

            }

            // ── Pulse Trigger (ID 1006) — pulses color on a group or channel ───
            if (advFeatures && gameObj->m_objectID == 1006) {
                if (!effectObj) return;

                // Can target either a group or a color channel
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                    effectObj->m_pulseTargetType = 1;  // group
                }

                auto targetColorResult = objConst["target_color_channel"].asInt();
                if (targetColorResult) {
                    effectObj->m_targetColor = std::clamp((int)targetColorResult.unwrap(), 1, 1010);
                    effectObj->m_pulseTargetType = 0;  // color channel
                }

                auto colorHexResult = objConst["color"].asString();
                if (!colorHexResult && objConst.contains("color") &&
                    objConst["color"].isArray() && objConst["color"].size() >= 3) {
                    // EAS-parser origin: color stored as [r,g,b].
                    const auto& carr = objConst["color"];
                    auto rr = carr[0].asInt(); auto gg = carr[1].asInt(); auto bb = carr[2].asInt();
                    effectObj->m_triggerTargetColor = {
                        (GLubyte)std::clamp(rr ? (int)rr.unwrap() : 255, 0, 255),
                        (GLubyte)std::clamp(gg ? (int)gg.unwrap() : 255, 0, 255),
                        (GLubyte)std::clamp(bb ? (int)bb.unwrap() : 255, 0, 255)};
                }
                if (colorHexResult) {
                    GLubyte r = 255, g = 255, b = 255;
                    if (parseHexColor(colorHexResult.unwrap(), r, g, b)) {
                        effectObj->m_triggerTargetColor = {r, g, b};
                    }
                }

                auto fadeInResult = objConst["fade_in"].asDouble();
                if (fadeInResult) {
                    effectObj->m_fadeInDuration = std::clamp(
                        static_cast<float>(fadeInResult.unwrap()), 0.0f, 10.0f);
                }

                auto holdResult = objConst["hold"].asDouble();
                if (holdResult) {
                    effectObj->m_holdDuration = std::clamp(
                        static_cast<float>(holdResult.unwrap()), 0.0f, 10.0f);
                }

                auto fadeOutResult = objConst["fade_out"].asDouble();
                if (fadeOutResult) {
                    effectObj->m_fadeOutDuration = std::clamp(
                        static_cast<float>(fadeOutResult.unwrap()), 0.0f, 10.0f);
                }

                auto exclusiveResult = objConst["exclusive"].asBool();
                if (exclusiveResult) {
                    effectObj->m_pulseExclusive = exclusiveResult.unwrap();
                }

            }

            // ── Spawn Trigger (ID 1268) — spawns/activates another trigger group
            if (advFeatures && gameObj->m_objectID == 1268) {
                if (!effectObj) return;

                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto delayResult = objConst["delay"].asDouble();
                if (delayResult) {
                    effectObj->m_spawnTriggerDelay = std::clamp(
                        static_cast<float>(delayResult.unwrap()), 0.0f, 30.0f);
                }

                auto editorDisableResult = objConst["editor_disable"].asBool();
                if (editorDisableResult) {
                    effectObj->m_previewDisable = editorDisableResult.unwrap();
                }

            }

            // ── Stop Trigger (ID 1616) — stops a trigger group ─────────────────
            if (advFeatures && gameObj->m_objectID == 1616) {
                if (!effectObj) return;

                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

            }

            // ── Scale trigger (2.2, ID 2067) ────────────────────────────────────
            // EAS has emitted these since the verb landed; the fields finally
            // get applied here. Uniform X/Y scale from the "scale" key.
            if (advFeatures && gameObj->m_objectID == 2067) {
                if (!effectObj) return;
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                auto durResult = objConst["duration"].asDouble();
                if (durResult)
                    effectObj->m_duration = std::clamp((float)durResult.unwrap(), 0.f, 30.f);
                if (auto* xform = typeinfo_cast<TransformTriggerGameObject*>(gameObj)) {
                    auto scaleToResult = objConst["scale"].asDouble();
                    if (scaleToResult) {
                        float s = std::clamp((float)scaleToResult.unwrap(), 0.05f, 10.f);
                        xform->m_objectScaleX = s;
                        xform->m_objectScaleY = s;
                    }
                }
            }

            // ── Shake trigger (ID 1520) ────────────────────────────────────────
            if (advFeatures && gameObj->m_objectID == 1520) {
                if (!effectObj) return;
                auto durResult = objConst["duration"].asDouble();
                if (durResult)
                    effectObj->m_duration = std::clamp((float)durResult.unwrap(), 0.f, 30.f);
                auto strengthResult = objConst["strength"].asDouble();
                if (strengthResult)
                    effectObj->m_shakeStrength = std::clamp((float)strengthResult.unwrap(), 0.f, 20.f);
                auto intervalResult = objConst["interval"].asDouble();
                if (intervalResult)
                    effectObj->m_shakeInterval = std::clamp((float)intervalResult.unwrap(), 0.f, 5.f);
            }

            // ── Camera triggers (2.2): zoom 1913 / static 1914 / offset 1916 ───
            if (advFeatures && gameObj->m_objectID == 1913) {
                if (!effectObj) return;
                auto zoomResult = objConst["zoom"].asDouble();
                if (zoomResult)
                    effectObj->m_zoomValue = std::clamp((float)zoomResult.unwrap(), 0.25f, 4.f);
                auto durResult = objConst["duration"].asDouble();
                if (durResult)
                    effectObj->m_duration = std::clamp((float)durResult.unwrap(), 0.f, 30.f);
            }
            if (advFeatures && gameObj->m_objectID == 1914) {
                if (!effectObj) return;
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                auto durResult = objConst["duration"].asDouble();
                if (durResult)
                    effectObj->m_duration = std::clamp((float)durResult.unwrap(), 0.f, 30.f);
                if (auto* cam = typeinfo_cast<CameraTriggerGameObject*>(gameObj)) {
                    auto exitResult = objConst["exit"].asBool();
                    if (exitResult && exitResult.unwrap()) cam->m_exitStatic = true;
                }
            }
            if (advFeatures && gameObj->m_objectID == 1916) {
                if (!effectObj) return;
                // Camera offset reuses the move-trigger offset pair.
                auto offXResult = objConst["move_x"].asDouble();
                auto offYResult = objConst["move_y"].asDouble();
                float ox = offXResult ? std::clamp((float)offXResult.unwrap(), -2000.f, 2000.f) : 0.f;
                float oy = offYResult ? std::clamp((float)offYResult.unwrap(), -2000.f, 2000.f) : 0.f;
                effectObj->m_moveOffset = CCPoint(ox, oy);
                auto durResult = objConst["duration"].asDouble();
                if (durResult)
                    effectObj->m_duration = std::clamp((float)durResult.unwrap(), 0.f, 30.f);
            }

            // ── Timewarp trigger (2.2, ID 1935) ─────────────────────────────────
            if (advFeatures && gameObj->m_objectID == 1935) {
                if (!effectObj) return;
                auto modResult = objConst["mod"].asDouble();
                if (modResult)
                    effectObj->m_timeWarpTimeMod =
                        std::clamp((float)modResult.unwrap(), 0.1f, 3.f);
            }

            // ── Song (1934) / SFX (3602) triggers ───────────────────────────────
            if (advFeatures && (gameObj->m_objectID == 1934 || gameObj->m_objectID == 3602)) {
                if (!effectObj) return;
                if (auto* sfx = typeinfo_cast<SFXTriggerGameObject*>(gameObj)) {
                    auto idResult = objConst["sound_id"].asInt();
                    if (idResult)
                        sfx->m_soundID = std::max(0, (int)idResult.unwrap());
                    auto volResult = objConst["volume"].asDouble();
                    if (volResult)
                        sfx->m_volume = std::clamp((float)volResult.unwrap(), 0.f, 2.f);
                    auto pitchResult = objConst["pitch"].asDouble();
                    if (pitchResult)
                        sfx->m_pitch = std::clamp((float)pitchResult.unwrap(), -12.f, 12.f);
                    if (gameObj->m_objectID == 1934) {
                        if (auto* song = typeinfo_cast<SongTriggerGameObject*>(gameObj)) {
                            auto chResult = objConst["channel"].asInt();
                            if (chResult)
                                song->m_songChannel = std::clamp((int)chResult.unwrap(), 0, 4);
                        }
                    }
                }
            }

            // ── Follow trigger (ID 1347) ────────────────────────────────────────
            if (advFeatures && gameObj->m_objectID == 1347) {
                if (!effectObj) return;
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                auto followResult = objConst["follow_group"].asInt();
                if (followResult)
                    effectObj->m_centerGroupID = std::clamp((int)followResult.unwrap(), 1, 9999);
                auto xModResult = objConst["x_mod"].asDouble();
                if (xModResult)
                    effectObj->m_followXMod = std::clamp((float)xModResult.unwrap(), -10.f, 10.f);
                auto yModResult = objConst["y_mod"].asDouble();
                if (yModResult)
                    effectObj->m_followYMod = std::clamp((float)yModResult.unwrap(), -10.f, 10.f);
                auto durResult = objConst["duration"].asDouble();
                if (durResult)
                    effectObj->m_duration = std::clamp((float)durResult.unwrap(), 0.f, 600.f);
            }

            // ── Follow Player Y trigger (ID 1814) ───────────────────────────────
            if (advFeatures && gameObj->m_objectID == 1814) {
                if (!effectObj) return;
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                auto speedResult = objConst["speed"].asDouble();
                if (speedResult)
                    effectObj->m_followYSpeed = std::clamp((float)speedResult.unwrap(), 0.f, 100.f);
                auto delayResult = objConst["delay"].asDouble();
                if (delayResult)
                    effectObj->m_followYDelay = std::clamp((float)delayResult.unwrap(), 0.f, 10.f);
                auto offsetResult = objConst["offset"].asInt();
                if (offsetResult)
                    effectObj->m_followYOffset = std::clamp((int)offsetResult.unwrap(), -500, 500);
                auto maxSpeedResult = objConst["max_speed"].asDouble();
                if (maxSpeedResult)
                    effectObj->m_followYMaxSpeed = std::clamp((float)maxSpeedResult.unwrap(), 0.f, 100.f);
                auto durResult = objConst["duration"].asDouble();
                if (durResult)
                    effectObj->m_duration = std::clamp((float)durResult.unwrap(), 0.f, 600.f);
            }

            // ── Touch trigger (ID 1595) ─────────────────────────────────────────
            if (advFeatures && gameObj->m_objectID == 1595) {
                if (!effectObj) return;
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                auto activateResult = objConst["activate"].asBool();
                if (activateResult)
                    effectObj->m_activateGroup = activateResult.unwrap();
                auto holdResult = objConst["hold"].asBool();
                if (holdResult)
                    effectObj->m_touchHoldMode = holdResult.unwrap();
            }

            // ── Count (1611) / Instant Count (1811) / Pickup (1817) triggers ────
            if (advFeatures && (gameObj->m_objectID == 1611 ||
                                gameObj->m_objectID == 1811 ||
                                gameObj->m_objectID == 1817)) {
                if (!effectObj) return;
                auto itemResult = objConst["item_id"].asInt();
                if (itemResult)
                    effectObj->m_itemID = std::clamp((int)itemResult.unwrap(), 1, 9999);
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                auto activateResult = objConst["activate"].asBool();
                if (activateResult)
                    effectObj->m_activateGroup = activateResult.unwrap();
                if (auto* count = typeinfo_cast<CountTriggerGameObject*>(gameObj)) {
                    auto countResult = objConst["count"].asInt();
                    if (countResult)
                        count->m_pickupCount =
                            std::clamp((int)countResult.unwrap(), -9999, 9999);
                }
            }

            // ── On-Death trigger (ID 1812) ──────────────────────────────────────
            if (advFeatures && gameObj->m_objectID == 1812) {
                if (!effectObj) return;
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                auto activateResult = objConst["activate"].asBool();
                if (activateResult)
                    effectObj->m_activateGroup = activateResult.unwrap();
            }

            // ── Linked teleport portal (ID 747) — vertical jump distance ───────
            // Not advFeatures-gated: teleports are plain gameplay objects like
            // every other portal.
            if (gameObj->m_objectID == 747) {
                if (auto* tp = typeinfo_cast<TeleportPortalObject*>(gameObj)) {
                    auto offResult = objConst["teleport_y_offset"].asDouble();
                    if (offResult)
                        tp->m_teleportYOffset =
                            std::clamp((float)offResult.unwrap(), -450.f, 450.f);
                }
            }

            // ── Animate trigger (ID 1585) ───────────────────────────────────────
            if (advFeatures && gameObj->m_objectID == 1585) {
                if (!effectObj) return;
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                auto animResult = objConst["animation_id"].asInt();
                if (animResult)
                    effectObj->m_animationID = std::clamp((int)animResult.unwrap(), 0, 50);
            }

            // ── Gravity trigger (ID 2066) ───────────────────────────────────────
            if (advFeatures && gameObj->m_objectID == 2066) {
                if (!effectObj) return;
                auto gravResult = objConst["gravity"].asDouble();
                if (gravResult)
                    effectObj->m_gravityValue =
                        std::clamp((float)gravResult.unwrap(), 0.f, 10.f);
                auto durResult = objConst["duration"].asDouble();
                if (durResult)
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
            }

            // ── Teleport trigger (ID 3022) — sends the player to a group ───────
            if (advFeatures && gameObj->m_objectID == 3022) {
                if (!effectObj) return;
                auto targetGroupResult = objConst["target_group"].asInt();
                if (targetGroupResult)
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
            }

            // ── Placement-only effect triggers — the ID alone is the effect ────
            //   1612/1613 hide/show player, 32/33 hide/show trail,
            //   1917 reverse, 1818/1819 BG-effect on/off, 1915 no-enter-effect,
            //   200-203/1334 speed portals. No fields needed.

            // ── Trigger activation mode (any trigger) ───────────────────────────
            // Runs AFTER the per-trigger branches. Default is position-
            // triggered: the trigger fires when the screen reaches its X —
            // matching the EAS emitter and the "triggers at y=0" convention
            // (a touch-triggered trigger at y=0 can never fire; the old
            // hardcoded touch default is why AI triggers silently did nothing).
            //   touch_triggered=true → fires on player contact instead
            //   spawn_triggered=true → fires only via a Spawn-trigger chain
            if (advFeatures && effectObj) {
                auto ttResult = objConst["touch_triggered"].asBool();
                if (ttResult)
                    effectObj->m_isTouchTriggered = ttResult.unwrap();
                auto stResult = objConst["spawn_triggered"].asBool();
                if (stResult && stResult.unwrap()) {
                    effectObj->m_isSpawnTriggered = true;
                    effectObj->m_isTouchTriggered = false;
                }
            }

        }
    }

    // ── Level metadata (name, description, song, background, ground, colors) ────
    // The AI may emit an optional top-level "level_metadata" object that mutates
    // level-wide settings independently of the objects array. All keys are
    // optional; unknown keys are ignored.
    //
    // Accessed fields (all verified against 2.2081 bindings):
    //   GJGameLevel:        m_levelName, m_levelDesc, m_songID, m_audioTrack
    //   LevelSettingsObject:m_platformerMode, m_backgroundIndex, m_groundIndex,
    //                       m_middleGroundIndex, m_groundLineIndex, m_fontIndex
    //   GJEffectManager:    getColorAction(int), colorActionChanged(ColorAction*)
    //   ColorAction:        m_color, m_fromColor, m_toColor, m_blending
    void applyLevelMetadata(matjson::Value& metadata) {
        if (!m_editorLayer) return;
        if (!metadata.isObject()) return;

        auto* level    = m_editorLayer->m_level;
        auto* settings = m_editorLayer->m_levelSettings;
        auto* fxMgr    = m_editorLayer->m_effectManager;

        bool changed       = false;
        bool needsLevelRefresh = false;  // background/ground/platformer need a visual refresh
        std::string summary;

        // ── Level name ────────────────────────────────────────────────────────
        if (level) {
            auto nameRes = metadata["name"].asString();
            if (nameRes) {
                std::string name = nameRes.unwrap();
                if (name.size() > 20) name.resize(20);    // GD enforces ~20 char limit
                // Strip control chars but keep printable ASCII + spaces
                std::string cleaned;
                cleaned.reserve(name.size());
                for (char c : name)
                    if (c >= 0x20 && c < 0x7f) cleaned += c;
                if (!cleaned.empty()) {
                    level->m_levelName = cleaned;
                    summary += fmt::format(" name=\"{}\"", cleaned);
                    changed = true;
                }
            }

            // ── Description ───────────────────────────────────────────────────
            auto descRes = metadata["description"].asString();
            if (descRes) {
                std::string desc = descRes.unwrap();
                if (desc.size() > 140) desc.resize(140); // GD enforces ~140 char limit
                level->m_levelDesc = desc;
                summary += fmt::format(" desc={}chars", desc.size());
                changed = true;
            }

            // ── Newgrounds custom song ID ─────────────────────────────────────
            // song_id = 0 falls back to built-in track (m_audioTrack).
            auto songRes = metadata["song_id"].asInt();
            if (songRes) {
                int sid = (int)songRes.unwrap();
                sid = std::clamp(sid, 0, 999'999'999);
                level->m_songID = sid;
                summary += fmt::format(" song={}", sid);
                changed = true;
            }

            // ── Built-in track (optional fallback when song_id is 0) ──────────
            auto trackRes = metadata["audio_track"].asInt();
            if (trackRes) {
                int track = std::clamp((int)trackRes.unwrap(), 0, 21);
                level->m_audioTrack = track;
                summary += fmt::format(" track={}", track);
                changed = true;
            }
        }

        // ── LevelSettings: platformer mode, background, ground, line ──────────
        if (settings) {
            auto platRes = metadata["platformer_mode"].asBool();
            if (platRes) {
                bool plat = platRes.unwrap();
                settings->m_platformerMode = plat;
                summary += fmt::format(" platformer={}", plat ? "on" : "off");
                changed = true;
                needsLevelRefresh = true;
            }

            auto bgRes = metadata["background_id"].asInt();
            if (bgRes) {
                int bg = std::clamp((int)bgRes.unwrap(), 1, 50);
                settings->m_backgroundIndex = bg;
                summary += fmt::format(" bg={}", bg);
                changed = true;
                needsLevelRefresh = true;
            }

            auto grRes = metadata["ground_id"].asInt();
            if (grRes) {
                int gr = std::clamp((int)grRes.unwrap(), 1, 50);
                settings->m_groundIndex = gr;
                summary += fmt::format(" ground={}", gr);
                changed = true;
                needsLevelRefresh = true;
            }

            auto mgRes = metadata["middle_ground_id"].asInt();
            if (mgRes) {
                int mg = std::clamp((int)mgRes.unwrap(), 1, 50);
                settings->m_middleGroundIndex = mg;
                summary += fmt::format(" midground={}", mg);
                changed = true;
                needsLevelRefresh = true;
            }

            auto glRes = metadata["ground_line_id"].asInt();
            if (glRes) {
                int gl = std::clamp((int)glRes.unwrap(), 1, 10);
                settings->m_groundLineIndex = gl;
                summary += fmt::format(" line={}", gl);
                changed = true;
                needsLevelRefresh = true;
            }

            auto fontRes = metadata["font_id"].asInt();
            if (fontRes) {
                int f = std::clamp((int)fontRes.unwrap(), 1, 20);
                settings->m_fontIndex = f;
                summary += fmt::format(" font={}", f);
                changed = true;
            }
        }

        // ── Default color channels ───────────────────────────────────────────
        // Common channels: 1000=BG, 1001=Ground, 1002=Line, 1003=3DL/Object Line,
        // 1004=Obj, 1005=Line2, 1009=Ground2.
        if (fxMgr && metadata.contains("default_colors") && metadata["default_colors"].isArray()) {
            auto& colorsArr = metadata["default_colors"];
            int applied = 0;
            for (size_t i = 0; i < colorsArr.size() && applied < 30; ++i) {
                auto& entry = colorsArr[i];
                if (!entry.isObject()) continue;
                auto chRes  = entry["channel"].asInt();
                auto hexRes = entry["color"].asString();
                if (!chRes || !hexRes) continue;
                int channel = (int)chRes.unwrap();
                if (channel < 1 || channel > 1015) continue;

                GLubyte r = 255, g = 255, b = 255;
                if (!parseHexColor(hexRes.unwrap(), r, g, b)) continue;

                auto* action = fxMgr->getColorAction(channel);
                if (!action) continue;

                ccColor3B col{r, g, b};
                action->m_color     = col;
                action->m_fromColor = col;
                action->m_toColor   = col;

                auto blendRes = entry["blending"].asBool();
                if (blendRes) action->m_blending = blendRes.unwrap();

                fxMgr->colorActionChanged(action);
                ++applied;
            }
            if (applied > 0) {
                summary += fmt::format(" colors={}ch", applied);
                changed = true;
                needsLevelRefresh = true;
            }
        }

        if (!changed) return;

        log::info("EditorAI: applied level metadata:{}", summary);

        // Mark the level dirty so GD's save flow persists the changes.
        if (level) level->levelWasAltered();

        // Refresh the editor view so the user sees the new background/ground/colors.
        if (needsLevelRefresh) {
            m_editorLayer->loadLevelSettings();
        }

        if (m_editorLayer->m_editorUI)
            m_editorLayer->m_editorUI->updateButtons();

        Notification::create(
            fmt::format("Level metadata updated:{}", summary),
            NotificationIcon::Info
        )->show();
    }

    void prepareObjects(matjson::Value& objectsArray) {
        if (!m_editorLayer || !objectsArray.isArray()) return;

        // Split out MOVE/DELETE/EDIT ops first — they act on EXISTING
        // objects immediately (journaled, so Deny restores everything);
        // whatever remains spawns as preview objects like always.
        int opsAffected = 0;
        {
            bool anyOp = false;
            for (size_t i = 0; i < objectsArray.size() && !anyOp; ++i)
                anyOp = objectsArray[i].isObject()
                     && objectsArray[i].contains("op")
                     && objectsArray[i]["op"].isString();
            if (anyOp) {
                std::vector<matjson::Value> ops;
                auto rest = matjson::Value::array();
                for (size_t i = 0; i < objectsArray.size(); ++i) {
                    auto& e = objectsArray[i];
                    if (e.isObject() && e.contains("op") && e["op"].isString())
                        ops.push_back(std::move(e));
                    else
                        rest.push(std::move(e));
                }
                objectsArray = std::move(rest);
                opsAffected  = applyEditOps(ops);
            }
        }

        // Nothing spawned AND nothing edited (e.g. every op selector missed):
        // error out BEFORE touching the editor's layer state — switching to
        // a fresh empty preview layer with no Accept/Deny to leave it would
        // strand the user staring at a blank, dimmed level.
        if (objectsArray.size() == 0 && opsAffected == 0) {
            onError("No Valid Objects",
                fmt::format("The AI's reply contained no placeable objects, and its "
                            "edit operations didn't match anything in the level. "
                            "Try re-phrasing, or re-generate. ({})",
                            autoErrorCode(70, 3)));
            return;
        }

        // Ops-only turn: real changes happened but nothing new spawns.
        // Enter the staged state directly so Accept/Deny appear; keep the
        // editor on its current layer (there is no preview layer to show).
        if (objectsArray.size() == 0 && opsAffected > 0 && !s_inPreviewMode) {
            s_previewLayer             = m_editorLayer->m_currentLayer;
            s_editorLayerBeforePreview = m_editorLayer->m_currentLayer;
            m_deferredObjects.clear();
            m_currentObjectIndex = 0;
            m_isCreatingObjects  = false;
            finishSpawning();
            return;
        }

        // Fresh preview: pick an unused editor layer (max used + 1) and
        // switch the editor to it. Follow-up turns while a preview is live
        // APPEND to the existing preview layer instead — that's how long
        // conversations make many small edits before one Accept.
        if (!s_inPreviewMode) {
            s_previewObjects.clear();
            s_previewIntendedLayers.clear();
            short maxLayer = 0;
            if (m_editorLayer->m_objects) {
                for (auto* raw : CCArrayExt<CCObject*>(m_editorLayer->m_objects)) {
                    auto* go = typeinfo_cast<GameObject*>(raw);
                    if (!go) continue;
                    maxLayer = std::max({maxLayer, go->m_editorLayer, go->m_editorLayer2});
                }
            }
            s_previewLayer = (short)std::min<int>(maxLayer + 1, 999);
            s_editorLayerBeforePreview = m_editorLayer->m_currentLayer;
            setEditorCurrentLayer(m_editorLayer, s_previewLayer);
            log::info("Preview layer {} (editor was on {})",
                      s_previewLayer, s_editorLayerBeforePreview);
        }
        // NOTE: block templates are NOT reset here; they were already reset
        // before macros ran (see onAPISuccess) so any template the AI emitted
        // via a block_template macro is still active when objects spawn.

        m_deferredObjects.clear();
        m_currentObjectIndex = 0;

        int    maxObjects  = (int)Mod::get()->getSettingValue<int64_t>("max-objects");
        size_t objectCount = std::min(objectsArray.size(), static_cast<size_t>(maxObjects));
        log::info("Preparing {} objects for progressive creation...", objectCount);

        for (size_t i = 0; i < objectCount; ++i) {
            // FIX: resolve type -> ID by writing directly into objectsArray[i],
            // then read id/x/y back from objectsArray[i] — NOT from a local copy
            // captured before the write (that was the original bug causing
            // "Prepared 0 valid objects" since objData never had the id field).
            auto typeResult = objectsArray[i]["type"].asString();
            if (typeResult) {
                const std::string& typeName = typeResult.unwrap();
                // Triggers with hardcoded IDs not in object_ids.json
                static const std::unordered_map<std::string, int> TRIGGER_IDS = {
                    {"color_trigger", 899},
                    {"move_trigger", 901},
                    {"end_trigger", 34},
                    {"show_trail_trigger", 32},
                    {"hide_trail_trigger", 33},
                };
                auto trigIt = TRIGGER_IDS.find(typeName);
                if (trigIt != TRIGGER_IDS.end()) {
                    objectsArray[i]["id"] = trigIt->second;
                } else {
                    auto it = OBJECT_IDS.find(typeName);
                    objectsArray[i]["id"] = (it != OBJECT_IDS.end()) ? it->second : 1;
                }
            }

            // Read from the authoritative array slot (not a stale local copy)
            auto idResult = objectsArray[i]["id"].asInt();
            auto xResult  = objectsArray[i]["x"].asDouble();
            auto yResult  = objectsArray[i]["y"].asDouble();

            if (!idResult || !xResult || !yResult) continue;

            int   objectID = idResult.unwrap();
            float x        = static_cast<float>(xResult.unwrap());
            float y        = static_cast<float>(yResult.unwrap());

            if (objectID < 1 || objectID > 10000) {
                log::warn("Invalid object ID {} at index {} — skipping", objectID, i);
                continue;
            }

            // Capture the full slot (with id now set) for applyObjectProperties.
            // Move — objectsArray is a local copy that is never read again,
            // and matjson copies are deep.
            m_deferredObjects.push_back({objectID, CCPoint{x, y}, std::move(objectsArray[i])});
        }

        // If any object sits below the configured ground Y, shift the entire
        // set upward so the lowest object lands exactly on the ground row.
        // Preserves relative layout instead of clamping each object
        // individually (which would crush structures).
        //
        // The ground Y is configurable via the "ai-ground-y" setting (default
        // 105) because GD's visible ground sprite is taller than the level-
        // data "Y=0 ground line" — blocks placed at level-Y=15 render INSIDE
        // the ground sprite and look underground. Triggers placed at Y=0 work
        // correctly when shifted up because triggers fire by X, not Y.
        if (!m_deferredObjects.empty()) {
            float minY = m_deferredObjects[0].position.y;
            for (auto& obj : m_deferredObjects)
                minY = std::min(minY, obj.position.y);

            const float groundY = getGroundY();
            if (minY < groundY) {
                float shift = groundY - minY;
                log::info("Shifting all objects up by {:.1f} units (lowest was at Y={:.1f}, ground-y setting is {:.1f})",
                    shift, minY, groundY);
                for (auto& obj : m_deferredObjects)
                    obj.position.y += shift;
            }
        }

        // LOD-first spawn order: gameplay skeleton (blocks, hazards, portals,
        // orbs, triggers) materializes before decoration, so the user can
        // read the level's shape seconds earlier on big generations. Stable
        // partition preserves the AI's relative order within each class.
        std::stable_partition(m_deferredObjects.begin(), m_deferredObjects.end(),
            [](const DeferredObject& d) {
                auto typeRes = d.data["type"].asString();
                if (!typeRes) return true;  // no type info — treat as gameplay
                const std::string& t = typeRes.unwrap();
                return !(t.rfind("decor_", 0) == 0 ||
                         t.rfind("cloud_", 0) == 0 ||
                         t.rfind("smoke_", 0) == 0 ||
                         t.rfind("effect_pulsing_", 0) == 0 ||
                         t.find("deco") != std::string::npos);
            });

        log::info("Prepared {} valid objects", m_deferredObjects.size());

        if (m_deferredObjects.empty()) {
            if (opsAffected > 0) {
                // Everything this turn was edit ops (adds all invalid or
                // absent) — stage what was applied instead of erroring.
                m_isCreatingObjects = false;
                finishSpawning();
                return;
            }
            // Every entry failed validation — undo the fresh layer switch
            // above before erroring, or the editor is stranded on an empty
            // preview layer with no buttons to come back from.
            if (!s_inPreviewMode && s_previewLayer >= 0) {
                setEditorCurrentLayer(m_editorLayer, s_editorLayerBeforePreview);
                s_previewLayer = -1;
            }
            onError("No Valid Objects",
                fmt::format("The AI returned an objects array, but none of the entries had recognizable "
                            "object types or valid x/y fields. Try rephrasing the prompt to ask for "
                            "common shapes (blocks, spikes, orbs), or switch to a more capable model in "
                            "settings. ({})",
                            autoErrorCode(70, 2)));
            return;
        }

        m_spawnBatchSize = (int)Mod::get()->getSettingValue<int64_t>("spawn-batch-size");
        m_advFeatures    = Mod::get()->getSettingValue<bool>("enable-advanced-features");
        m_isCreatingObjects = true;
        showStatus("Starting object creation...", false);
    }

    // ── System prompt ─────────────────────────────────────────────────────────

    std::string buildSystemPrompt() {
        bool advFeatures   = Mod::get()->getSettingValue<bool>("enable-advanced-features");

        // ── Mode preface (Creation vs Edit) ───────────────────────────────
        // The whole rest of the prompt assumes a from-scratch level. When the
        // user has the popup in Edit Mode, we lead with a stricter contract:
        //   - do NOT regenerate sections already present
        //   - emit only the NEW objects being added
        //   - keep the analysis short (one sentence)
        //   - respect the existing X cursor (analyze_level / get_level_length
        //     surface it)
        // In Creation mode, we lead with a "build from scratch" framing that
        // tells the model the level is empty.
        std::string modePrefix;
        if (m_editMode) {
            int existingCount = (m_editorLayer && m_editorLayer->m_objects)
                ? m_editorLayer->m_objects->count() : 0;
            modePrefix = fmt::format(
                "MODE: EDIT. You are a full co-editor of the user's ~{}-object "
                "level. You can MOVE existing objects, DELETE them, EDIT their "
                "rotation/scale/colors, and ADD new ones — restructure as "
                "boldly as the request demands. A serious rework touches "
                "hundreds to thousands of objects via bulk selectors; never "
                "answer a rework request with a handful of additions. "
                "Call analyze_level first if tools are available.\n\n",
                existingCount);
        } else if (m_coopMode || m_mutationMode) {
            // appendModeContext emits the full MODE: CO-OP / MODE: MUTATION
            // framing — a "blank canvas" preface here would contradict it.
            modePrefix = "You are generating Geometry Dash objects in EAS format.\n\n";
        } else {
            modePrefix =
                "MODE: CREATION. Blank canvas. Build a complete playable level matching "
                "the requested length/difficulty. Use macros liberally.\n\n";
        }

        // Cache the configured ground Y for the prompt.
        // {1}=ground, {2}=ground+30, {3}=ground+60, {4}=ground+90.
        const int gY  = (int)getGroundY();
        const int gY1 = gY + 30;
        const int gY2 = gY + 60;
        const int gY3 = gY + 90;

        // Build the object catalog. Dumping the full 3,986-entry object_ids.json
        // would be ~120 KB / ~28K tokens — fine on 128K-context frontier models,
        // fatal on the 8K-context small ones we ship to, and a cost multiplier
        // everywhere. So we ALWAYS send a single curated allowlist (~140 entries,
        // ~3 KB / ~800 tokens) covering every gameplay-essential shape plus core
        // decoration; the AI reaches the long tail via the search_objects tool
        // or `OBJ <name>`.
        //
        // The allowlist is hand-picked against the actual catalog so every entry
        // resolves. If you add new names to object_ids.json, you can safely
        // append them here without touching the prompt logic.
        static const std::vector<std::string> COMPACT_OBJECT_ALLOWLIST = {
            // ── Blocks (gameplay foundation) ───────────────────────────────
            "block_black_gradient_square",
            "block_black_gradient_small_square",
            "block_black_gradient_slab_middle",
            "block_black_gradient_slab_side",
            "block_black_gradient_single_slab",
            "block_black_inner_square",
            "block_black_inner_corner_square",
            "block_grid_patterned_top_square",
            "block_grid_patterned_inner_square",
            "block_grid_patterned_inner_corner_square",
            "block_grid_patterned_outer_corner_square",
            "block_grid_patterned_top_pillar_square",
            "block_grid_patterned_pillar_square",
            // ── Slopes ─────────────────────────────────────────────────────
            "block_beveled_black_slope",
            "block_beveled_black_wide_slope",
            "block_brick_slope",
            "block_brick_wide_slope",
            // ── Spikes (hazards on ground row) ─────────────────────────────
            "spike_black_gradient_spike",
            "spike_black_gradient_tiny_spike",
            "spike_colored_spike",
            "spike_colored_small_spike",
            "spike_colored_half_spike",
            "spike_colored_tiny_spike",
            "spike_fake_black_spike",
            "spike_fake_black_half_spike",
            "spike_black_pit_hazard",
            "spike_black_slope_hazard",
            "spike_black_wide_slope_hazard",
            // ── Sawblades (deadly rotating obstacles) ──────────────────────
            "spike_small_black_sawblade",
            "spike_medium_black_sawblade",
            "spike_large_black_sawblade",
            // ── Generic hazards (rounded / squared / wavy / animated) ──────
            "hazard_non_colorable_round_black_hazard",
            "hazard_non_colorable_square_black_hazard",
            "hazard_non_colorable_wavy_black_hazard",
            "hazard_non_colorable_wavy_black_pit_hazard",
            "hazard_animated_black_pit_hazard",
            // ── Gamemode portals ───────────────────────────────────────────
            "portal_cube_portal",
            "portal_ship_portal",
            "portal_ball_portal",
            "portal_ufo_portal",
            "portal_wave_portal",
            "portal_robot_portal",
            "portal_spider_portal",
            "portal_swing_portal",
            // ── Gravity portals ────────────────────────────────────────────
            "portal_normal_gravity_portal",
            "portal_flipped_gravity_portal",
            "portal_reverse_gravity_portal",
            // ── Speed portals (pacing) ─────────────────────────────────────
            "portal_yellow_slow_speed_portal",
            "portal_blue_normal_speed_portal",
            "portal_green_fast_speed_portal",
            "portal_red_fast_speed_portal",
            "portal_pink_fast_speed_portal",
            // ── Size & mirror portals (modifiers) ──────────────────────────
            "portal_green_size_portal",
            "portal_pink_size_portal",
            "portal_blue_mirror_portal",
            "portal_orange_mirror_portal",
            // ── Dual & teleport portals ────────────────────────────────────
            "portal_dual_portal",
            "portal_exit_dual_portal",
            "portal_linked_teleport_portals",
            "portal_unlinked_blue_teleport_portal",
            "portal_unlinked_orange_teleport_portal",
            // ── Jump pads ──────────────────────────────────────────────────
            "jump_pad_yellow_jump_pad",
            "jump_pad_pink_jump_pad",
            "jump_pad_red_jump_pad",
            // ── Jump / utility orbs ────────────────────────────────────────
            "jump_orb_yellow_jump_orb",
            "jump_orb_pink_jump_orb",
            "jump_orb_red_jump_orb",
            "obj_blue_gravity_orb",
            "obj_blue_gravity_pad",
            "obj_green_dash_orb",
            "obj_pink_gravity_dash_orb",
            "obj_green_gravity_orb",
            "obj_black_drop_orb",
            "obj_teleport_orb",
            "obj_toggle_orb",
            "obj_spider_orb",
            "obj_spider_pad",
            // ── Decorations (visuals) ──────────────────────────────────────
            "decor_small_decorative_gear",
            "decor_medium_decorative_gear",
            "decor_large_decorative_gear",
            "decor_small_cartwheel",
            "decor_medium_cartwheel",
            "decor_large_cartwheel",
            "decor_small_blade",
            "decor_medium_blade",
            "decor_large_blade",
            "decor_small_saw_blade",
            "decor_medium_saw_blade",
            "decor_large_saw_blade",
            // ── Common effect triggers (when advanced mode is OFF, AI still
            //    benefits from knowing color/move/spawn exist) ──────────────
            "effect_color_trigger",
            "effect_alpha_trigger",
            "effect_move_trigger",
            "effect_pulse_trigger",
            "effect_rotate_trigger",
            "effect_scale_trigger",
            "effect_spawn_trigger",
            "effect_toggle_trigger",
            "effect_stop_trigger",
            "effect_10_level_end_trigger",
            // ── 2.2 trigger surface (camera / time / audio / interactivity) ─
            "effect_shake_trigger",
            "effect_zoom_camera_trigger",
            "effect_static_camera_trigger",
            "effect_offset_camera_trigger",
            "effect_timewarp_trigger",
            "effect_song_trigger",
            "effect_sfx_trigger",
            "effect_follow_trigger",
            "effect_follow_player_y_trigger",
            "effect_touch_trigger",
            "effect_count_trigger",
            "effect_instant_count_trigger",
            "effect_pickup_trigger",
            "effect_on_death_trigger",
            // ── Animated decoration ──────────────────────────────────────────
            "effect_pulsing_circle",
            "effect_pulsing_heart",
            "effect_pulsing_star",
            "effect_pulsing_music_note",
            // ── Teleport ─────────────────────────────────────────────────────
            "portal_linked_teleport_portals",
            // ── Extra decoration (depth & detail without the full catalog;
            //    any name missing from object_ids.json is silently skipped) ────
            "decor_tall_rod", "decor_medium_rod", "decor_short_rod",
            "decor_tall_chain",
            "spike_large_decorative_spikes", "spike_medium_decorative_spikes",
            "spike_small_decorative_spikes",
            "effect_pulsing_filled_circle", "effect_pulsing_diamond",
            "effect_large_pulsing_arrow_1", "effect_large_pulsing_arrow_2",
            "obj_cartoon_arrow", "obj_tiny_arrow",
            "large_cloud", "small_cloud", "medium_cloud", "large_round_cloud",
            "large_fading_cloud", "small_fading_cloud"
        };
        // Curated essentials catalog — ALWAYS. The full 3,986-entry dump was
        // ~120 KB / ~28K tokens: fatal on small-context local models and a
        // needless cost multiplier on hosted APIs. This list covers every
        // gameplay-essential shape plus core decoration; the AI reaches the
        // long tail via the search_objects tool or `OBJ <name>`. Built once
        // (deterministic after startup); names missing from OBJECT_IDS are
        // silently skipped (defensive against object_ids.json drift).
        static const std::pair<std::string, size_t> s_catalog = [] {
            std::string list;
            size_t included = 0;
            list.reserve(COMPACT_OBJECT_ALLOWLIST.size() * 32);
            bool first = true;
            for (const auto& name : COMPACT_OBJECT_ALLOWLIST) {
                if (OBJECT_IDS.find(name) == OBJECT_IDS.end()) continue;
                if (!first) list += ", ";
                list += name;
                first = false;
                ++included;
            }
            return std::make_pair(std::move(list), included);
        }();
        const std::string& objectList = s_catalog.first;
        size_t catalogIncluded = s_catalog.second;
        (void)catalogIncluded;

        // ── EAS-teaching prompt — ONE optimized prompt for every model ─────
        // EditorAI Script (EAS) is the PREFERRED output format: a line-based
        // DSL far more compact than JSON (~6 tokens/obj vs ~25), that forces
        // structural thinking via macros and bakes metadata into a mandatory
        // first verb. There is no longer a compact/full split — this single
        // prompt carries the COMPLETE grammar + trigger docs, but pairs it with
        // the CURATED catalog built above (not the 3,986-entry dump), so the
        // whole system prompt lands around ~3K tokens: dense enough for good
        // levels, small enough for 8K-context local models, and cheap on hosted
        // APIs. The long tail of objects is reachable via the search_objects
        // tool or `OBJ <name>`.

        // Prompt body — kept terse on purpose. Prior revisions exploded
        // to ~5K tokens of prose; the model already knows GD design after
        // training, so we ship grammar + rules + one worked example, not a
        // textbook. The 3,986-entry catalog dump remains the dominant share.
        std::string base = modePrefix + fmt::format(
            "GD level designer. Output ONE format end-to-end:\n"
            " A) EAS (preferred). Markdown plan, then `## Level Script`, then EAS lines.\n"
            "    Parser keeps everything after `## Level Script`.\n"
            " B) JSON: {{\"analysis\"?,\"objects\"?:[{{type,x,y,...}}],\"macros\"?:[{{name,...}}],"
            "\"level_metadata\"?:{{name,description,song_id,background_id,ground_id,"
            "default_colors:[{{channel,color:\"#RGB\"}}]}}}}.\n"
            "Analysis and objects are optional — metadata-only updates are valid.\n\n"
            "GRID: 30u cells. X = left→right. Y = object CENTER. Ground={1}; rows {2}, {3}, {4}, ... = {1}+30*n. Never Y<{1}. Triggers conventionally at y=0.\n\n"
            "EAS GRAMMAR — one per line, `[opt]`:\n"
            " META name=\"\" desc=\"\" song_id=N audio_track=N bg=N ground=N middle=N "
            "ground_line=N font=N platformer=true|false\n"
            " COLOR ch=N hex=RRGGBB [blend] [player_color]            (default colors)\n"
            " COLOR ch=N hex=RRGGBB at=X duration=T [blend]           (runtime color trigger)\n"
            " SECTION x0..x1 difficulty=easy|medium|hard|insane|demon mode=cube|ship|ball|ufo|wave|robot|spider|swing\n"
            " FLOOR x0..x1 [y={1}]                CORRIDOR x0..x1 ceiling=Y floor=Y\n"
            " PLATFORM-RUN x0..x1 y=Y [gap=G gap-every=E]\n"
            " SPIKE x [variant=basic|tiny|small|half|colored|pit|slope]\n"
            " BLOCK x y [variant=basic|small|slab|slab-middle|slab-side|grid|grid-top|grid-corner]\n"
            " SAW   x y [size=small|medium|large]   or   SAW <size> x y\n"
            " PYRAMID x=X [base=N y=Y block=<variant>]      (centered block pyramid)\n"
            " CEILING-SPIKES x0..x1 [y=375 spacing=30]      (downward spikes from ceiling)\n"
            " SAW-GAUNTLET x0..x1 [y=Y spacing=120 size=small|medium|large weave=U]\n"
            " ORB   <yellow|pink|red|blue|green|black|dash|teleport|toggle> x y\n"
            " PAD   <yellow|pink|red|blue|spider> x y\n"
            " PORTAL <cube|ship|ball|ufo|wave|robot|spider|swing|mini|normal|"
            "mirror-on|mirror-off|speed-half|speed-normal|speed-double|speed-triple|speed-quad|"
            "gravity-up|gravity-down|gravity-reverse|teleport-blue|teleport-orange|teleport-linked|dual> x [y]\n"
            " SPIKE-TRAIN x count=N [spacing=30 spike-type=NAME]\n"
            " STAIR-UP|STAIR-DOWN x steps=N [step-w=30 step-h=30] [y_top=Y]\n"
            " PILLAR / BLOCK-WALL x y_bot=Y y_top=Y\n"
            " BLOCK-STACK x y count=N [variant=...]\n"
            " ARC-ORBS x y count=N [spacing=60 orb=yellow]\n"
            " COPY from=x0..x1 offset=DX            MIRROR axis=X [from=x0..x1]\n"
            " ROW <catalog_name> x0..x1 [y={1} step=30]  (run-length: a row of ANY object in one line)\n"
            " DUAL x0..x1 [floor=Y gap=240 block=<variant>]  (entry portal + mirrored corridor + exit portal)\n"
            " TELEPORT x [y=Y y_offset=DY]          (linked teleport pair; DY = vertical jump)\n"
            " OBJ <catalog_name> x y                (escape hatch — any catalog entry)\n\n"
            "TOKEN ECONOMY: y may be omitted when it equals ground ({1}). Prefer "
            "ROW / FLOOR / SPIKE-TRAIN / COPY / MIRROR over runs of OBJ lines.\n\n"
            "PER-LINE FIELDS (apply to any verb): scale=F rot=N color=N detail=N "
            "groups=1,2,3 flip_x flip_y multi_activate z_layer=N z_order=N editor_layer=N\n"
            " `FLOOR 0..1500 color=10 groups=1` → every block in the floor gets color_channel=10 and group 1.\n"
            "OBJECT FLAGS (any object): passable (player phases through a solid "
            "block — fake walls/secret routes), notouch (hazard loses its hitbox "
            "— decorative spikes), hide (invisible but functional), noglow, "
            "nofade, highdetail, dontenter (skip enter animation), noeffects.\n"
            " `SPIKE 600 notouch scale=1.4` → big spike that can't kill; "
            "`BLOCK 900 105 passable` → fake wall.\n\n"
            "TRIGGER VERBS — fire by X position. Place at y=0. Add `multi_activate` for repeating sections:\n"
            " TRIGGER color ch=N hex=RRGGBB at=X [duration=T opacity=A blend]\n"
            " TRIGGER alpha groups=g at=X to=A [duration=T easing=N]\n"
            " TRIGGER move  groups=g at=X dx=DX dy=DY [duration=T easing=N lock_to_player_x lock_to_player_y]\n"
            " TRIGGER toggle groups=g at=X on=true|false\n"
            " TRIGGER pulse ch=N|groups=g hex=RRGGBB at=X [fade_in=T hold=T fade_out=T exclusive]\n"
            " TRIGGER rotate groups=g at=X degrees=D [duration=T center=g lock_rotation]\n"
            " TRIGGER scale groups=g at=X to=F [duration=T]\n"
            " TRIGGER shake at=X [duration=T strength=F interval=F]\n"
            " TRIGGER spawn target=g at=X [delay=T editor_disable]\n"
            " TRIGGER stop  groups=g at=X            TRIGGER end at=X\n"
            " TRIGGER show-player|hide-player|show-trail|hide-trail at=X\n"
            " TRIGGER zoom at=X zoom=F [duration=T]      ← 2.2 camera zoom (1=normal)\n"
            " TRIGGER static-cam at=X target=g [duration=T exit]   ← lock camera to group\n"
            " TRIGGER offset-cam at=X x=DX y=DY [duration=T]       ← pan camera\n"
            " TRIGGER timewarp at=X mod=F        ← 0.5=slow-mo, 2=double speed\n"
            " TRIGGER song at=X [sound_id=N channel=0-4 volume=F]\n"
            " TRIGGER sfx at=X sound_id=N [volume=F pitch=F]\n"
            " TRIGGER follow at=X target=g follow=g2 [x_mod=F y_mod=F duration=T]\n"
            " TRIGGER follow-y at=X target=g [speed=F delay=T offset=N max_speed=F]\n"
            " TRIGGER touch at=X target=g [activate hold]   ← fires on player tap\n"
            " TRIGGER count at=X item_id=N count=C target=g [activate]\n"
            " TRIGGER instant-count / pickup / on-death — same fields\n"
            " TRIGGER animate at=X groups=g anim=N      ← play animation N on animated objects\n"
            " TRIGGER gravity at=X g=F [duration=T]     ← 1=normal, 0.5=floaty, 2=heavy\n"
            " TRIGGER teleport at=X groups=g            ← teleport player to group g's position\n"
            " TRIGGER reverse|bg-on|bg-off|no-enter-fx at=X   ← placement-only effects\n"
            "ACTIVATION: triggers fire when the screen reaches their X (default). "
            "Add `touch=true` for player-contact firing, or `spawn_triggered` + "
            "`TRIGGER spawn target=g` chains for logic (give the chained trigger a group).\n"
            "EASING: 0 none, 1 inout, 2 in, 3 out, 4-6 elastic, 7-9 bounce, 10-12 exp, 13-15 sine, 16-18 back.\n\n"
            "COLOR SLOTS: 1000=BG 1001=G1 1002=Line 1003=3DL 1004=Object 1005=Line2 1009=G2. 1-999 = user.\n\n"
            "RULES: section must start with FLOOR or CORRIDOR. Obstacle every "
            "90-210u (easy 150-210, medium 90-150, hard 60-90, insane 30-60). "
            "Mix obstacle types, don't spam SPIKE-TRAIN. End each gamemode at a portal. "
            "Use macros for 30-60% of objects (FLOOR/CORRIDOR/PLATFORM-RUN do the bones).\n"
            "DECORATION (what separates rated levels from object soup): set "
            "META bg/ground + 2-3 custom COLOR channels and shift them with "
            "TRIGGER color at section boundaries; ground strips/slabs every "
            "150-300u; large dim background decor (z_layer=-3) every "
            "400-600u for depth; TRIGGER pulse on the beat; never leave a "
            "300u stretch bare. Decoration is NOT optional - budget ~30% of "
            "your objects for it.\n"
            "LENGTH (X span): short 1200-2400, medium 2400-9000, long 9000-18000, xl 18000+. "
            "Aim 100-800 objects.\n"
            "OUTPUT BUDGET: analysis ≤ 2 sentences, no prose between EAS lines, "
            "prefer macros over individual blocks, don't restate already-given facts.\n\n"
            "EXAMPLE — 30s medium cube→ship drop:\n"
            "## Plan\n"
            "Cube intro 0-3000 easy, ship corridor 3000-7000 hard with color shift.\n"
            "## Level Script\n"
            "META name=\"Drop\" desc=\"30s\" song_id=467339 bg=1 ground=1\n"
            "COLOR ch=1000 hex=1a2030 blend   COLOR ch=1001 hex=2a3040\n"
            "SECTION 0..3000 difficulty=medium mode=cube\n"
            "FLOOR 0..3000 color=1001\n"
            "SPIKE-TRAIN 300 count=2   ORB yellow 600 135   STAIR-UP 900 steps=3\n"
            "SPIKE 1200   ARC-ORBS 1500 165 count=3 spacing=120\n"
            "SPIKE-TRAIN 2000 count=4 spacing=45   PAD yellow 2500 105   PORTAL ship 2900 165\n"
            "SECTION 3000..7000 difficulty=hard mode=ship\n"
            "TRIGGER color ch=1000 hex=2a1030 at=3000 duration=0.4 blend\n"
            "CORRIDOR 3000..7000 ceiling=270 floor=60 color=1001\n"
            "SAW small 3300 180   PORTAL speed-double 3600 0\n"
            "SAW medium 4200 165   SAW large 4800 195\n"
            "TRIGGER move groups=5 at=5000 dx=-300 dy=0 duration=2.0 easing=1\n"
            "BLOCK 5400 180 groups=5   TRIGGER end at=7000\n\n"
            "CATALOG — essentials only (curated core). For ANY object beyond "
            "this list, call the search_objects tool (when tools are on) or use "
            "OBJ <name>; the full library is 3,986 objects:\n {0}",
            objectList, gY, gY1, gY2, gY3
        );

        if (advFeatures) {
            base +=
                "\nADV: copy_color_channel=N (live), easing_rate=F (0.5-10), "
                "duration_per (color_pulse macro), spawn-trigger chains via delay=T, "
                "multi-group `groups=1,2,3` up to 10, hide-player/show-player for cutscenes.\n";
        }

        // META slots already documented in grammar above; just a quick mood
        // palette cheatsheet to anchor "give me a dark/neon/sunset level".
        base +=
            "\nMOOD palettes (COLOR lines after META):\n"
            " dark cave  ch=1000 #0a0a1a / 1001 #1a1a2a / 1002 #8090a0 blend\n"
            " neon night ch=1000 #050020 / 1002 #ff00ff blend / 10 #00ffff\n"
            " sunset     ch=1000 #ff7733 / 1001 #cc4422 / 1002 #ffeebb\n"
            " underwater ch=1000 #0a2a4c / 1001 #1a3a8c / 10  #4488cc blend\n"
            " industrial ch=1000 #2a2a2a / 1001 #3a3a3a / 10  #ff6633\n";

        // Few-shot examples — real .gmd slices matched to the requested
        // style. Capped at 1 to keep the prompt small; each example is the raw
        // objects array (X normalized to 0).
        if (!EXAMPLE_SECTIONS.empty()) {
            int wantPicks = 1;
            auto picks = pickExampleIndices(s_lastDifficulty, s_lastStyle, wantPicks);
            if (!picks.empty()) {
                base += "\n\nFEW-SHOT (real GD slices, X normalized to 0 — adapt anywhere):\n";
                for (size_t k = 0; k < picks.size(); ++k) {
                    const auto& ex = EXAMPLE_SECTIONS[picks[k]];
                    // Cap the example at ~2400 chars, cut at a complete object
                    // boundary so the model never sees a half-written object to
                    // imitate. Style transfer works off the first dozens of
                    // objects; the tail is repetition.
                    std::string_view objs = ex.objectsJson;
                    std::string truncated;
                    if (objs.size() > 2400) {
                        size_t cut = objs.rfind("},", 2400);
                        if (cut != std::string_view::npos) {
                            truncated.assign(objs.substr(0, cut + 1));
                            truncated += "] (truncated — full slice is longer)";
                            objs = truncated;
                        }
                    }
                    base += fmt::format(" [{}] \"{}\" by {} ({}, {}; {} obj): {}\n",
                        k + 1, ex.levelName, ex.author, ex.theme, ex.difficulty,
                        ex.objectCount, objs);
                }
            }
        }

        // Inject user feedback as few-shot learning context.
        // Examples are prioritized by similarity to the current request
        // (matching difficulty/style/length), then by rating.
        // Guard: cap total feedback injection at ~8000 chars (~2000 tokens)
        // to avoid blowing up context windows on smaller models.
        if (Mod::get()->getSettingValue<bool>("enable-rating")) {
            int maxExamples = (int)Mod::get()->getSettingValue<int64_t>("max-feedback-examples");
            auto curDiff  = s_lastDifficulty;
            auto curStyle = s_lastStyle;
            auto curLen   = s_lastLength;

            // Halved from 8000 → 4000 chars (~1000 tokens). Past ratings stay
            // useful for style/difficulty cues; we don't need 5 verbose ones.
            constexpr size_t FEEDBACK_CHAR_BUDGET = 4000;
            std::string feedbackSection;

            // Truncate a level dump at a clean object boundary so a few
            // representative objects inject instead of the whole level.
            // (Real-world bug: full dumps NEVER fit, so feedback injection
            // was silently dead — user logs showed "exhausted (121 chars)"
            // on every single generation.)
            auto sampleJson = [](const std::string& json, size_t cap) -> std::string {
                if (json.size() <= cap) return json;
                auto cut = json.rfind("},", cap);
                if (cut == std::string::npos) return json.substr(0, cap) + "...";
                return json.substr(0, cut + 1) + "] (sample of full level)";
            };

            auto appendFeedback = [&](const FeedbackEntry& fb, bool isPositive) {
                std::string entry;
                if (isPositive) {
                    entry += fmt::format(
                        "  + (rated {}/10) prompt=\"{}\" difficulty={} style={} length={}",
                        fb.rating, fb.prompt, fb.difficulty, fb.style, fb.length);
                } else {
                    entry += fmt::format(
                        "  - (rated {}/10) prompt=\"{}\"", fb.rating, fb.prompt);
                }
                if (!fb.feedback.empty())
                    entry += fmt::format(" — user {}: \"{}\"",
                        isPositive ? "said" : "complaint", fb.feedback);
                entry += "\n";
                if (!fb.objectsJson.empty())
                    entry += fmt::format("    AI generated: {}\n",
                                         sampleJson(fb.objectsJson, 700));
                if (!fb.editedObjectsJson.empty())
                    entry += fmt::format("    user corrected to: {}\n",
                                         sampleJson(fb.editedObjectsJson, 700));
                else if (!fb.editSummary.empty())
                    entry += fmt::format("    user edits: {}\n", fb.editSummary);

                // Exact budget check (formatting adds a little overhead text)
                if (feedbackSection.size() + entry.size() > FEEDBACK_CHAR_BUDGET) {
                    log::info("Feedback budget exhausted ({} chars), skipping remaining examples",
                        feedbackSection.size());
                    return false;
                }
                feedbackSection += entry;
                return true;
            };

            // Positive examples
            auto topFeedback = getTopFeedback(maxExamples, curDiff, curStyle, curLen);
            if (!topFeedback.empty()) {
                feedbackSection += "\n\nUSER LIKES — The user rated these past generations highly. "
                    "Study the actual output and match this style and quality:\n";
                for (const FeedbackEntry* fb : topFeedback)
                    if (!appendFeedback(*fb, true)) break;
            }

            // Negative examples
            auto bottomFeedback = getBottomFeedback(maxExamples, curDiff, curStyle, curLen);
            if (!bottomFeedback.empty()) {
                feedbackSection += "\n\nUSER DISLIKES — The user rated these poorly. "
                    "AVOID reproducing these patterns:\n";
                for (const FeedbackEntry* fb : bottomFeedback)
                    if (!appendFeedback(*fb, false)) break;
            }

            base += feedbackSection;
        }

        // Edit-op grammar — always available (follow-up edit turns can occur
        // on any session), with the heavy-edit doctrine appended last in
        // edit mode so the AI sees it freshest.
        base += "\nEDIT OPS (only valid when editing an existing level - the "
                "mod sends you an OBJECT INVENTORY listing when they apply):\n"
                "MOVE <sel> dx=F dy=F     shift existing objects by a delta\n"
                "DELETE <sel>             remove existing objects\n"
                "EDIT <sel> rot=F scale=F color=N detail=N flip_x flip_y   restyle\n"
                "<sel> forms: #12 (inventory index) | #5-40 (index range) | "
                "rect:x1,y1,x2,y2 (everything in the box) | id:spike (every "
                "object of that type). rect/id may add type=NAME to filter. "
                "One bulk line can touch hundreds of objects. Ops run in "
                "order at apply time and are previewed/reversible.\n";
        if (m_editMode) {
            base += "\nEDIT MODE DOCTRINE: think like a level designer doing a "
                    "serious revision pass, not a patcher. Combine MOVE/DELETE/"
                    "EDIT/additions across the WHOLE level: re-space bad pacing "
                    "(MOVE rect), cut clutter (DELETE), restyle sections (EDIT "
                    "+ COLOR/TRIGGER), then build new content where the level "
                    "is weak. Macros are allowed and encouraged for new "
                    "geometry. Stay inside the user's requested scope when "
                    "they give one.\n";
        }

        // Append the GD Creator School design tips digest. This is a tiny
        // appendix (~700 bytes) that gives small fine-tunes a stronger prior on
        // pacing, difficulty curves, decoration, and common mistakes — things
        // the model can't easily infer from the object catalog alone.
        return base + std::string(editorai::GDCS_DESIGN_TIPS);
    }

    // ── API call ──────────────────────────────────────────────────────────────

    // Strips leading/trailing ASCII whitespace (spaces, tabs, newlines, carriage
    // returns). API keys are frequently copy-pasted with invisible trailing
    // characters that cause 401/403 responses even when the key is valid.
    static std::string trimKey(std::string s) {
        const std::string ws = " \t\r\n";
        size_t start = s.find_first_not_of(ws);
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(ws);
        return s.substr(start, end - start + 1);
    }

    // ── Tool 1: download a reference level from GD's servers ───────────────
    // Builds a compact summary the AI can use as design inspiration. Skips
    // entirely if input is empty.
    // ── Reference-level cache ───────────────────────────────────────────────
    // Summaries are small (≤ ~4 KB) and immutable for a given level ID, so
    // they cache hard: in-memory for the session, mirrored to
    // level_cache.json so pinned example-level-ids stop re-downloading from
    // boomlings.com on every single generation.
    static std::unordered_map<std::string, std::string>& levelSummaryCache() {
        static std::unordered_map<std::string, std::string> cache = [] {
            std::unordered_map<std::string, std::string> c;
            auto path = Mod::get()->getSaveDir() / "level_cache.json";
            auto res = utils::file::readString(path);
            if (res) {
                auto parsed = matjson::parse(res.unwrap());
                if (parsed && parsed.unwrap().isObject()) {
                    for (auto& [key, value] : parsed.unwrap()) {
                        auto s = value.asString();
                        if (s) c[key] = s.unwrap();
                    }
                }
            }
            return c;
        }();
        return cache;
    }

    static void persistLevelCache() {
        auto& cache = levelSummaryCache();
        auto obj = matjson::Value::object();
        for (auto& [id, summary] : cache) obj[id] = summary;
        std::string out = obj.dump();
        auto path = Mod::get()->getSaveDir() / "level_cache.json";
        // Same pattern as persistFeedback: serialize on the caller, write on
        // a detached thread, mutex serializes concurrent writers.
        std::thread([path, out = std::move(out)] {
            static std::mutex s_cacheWriteMutex;
            std::lock_guard lock(s_cacheWriteMutex);
            auto res = utils::file::writeString(path, out);
            if (!res) log::warn("level_cache.json write failed: {}", res.unwrapErr());
        }).detach();
    }

    void fireFetchLevelByID(const std::string& idInput,
                            std::function<void(const std::string&)> onDone)
    {
        if (idInput.empty()) { onDone(""); return; }
        std::string trimmed = trimKey(idInput);
        bool isNumeric = !trimmed.empty()
            && std::all_of(trimmed.begin(), trimmed.end(),
                           [](char c){ return (unsigned char)c >= '0' && (unsigned char)c <= '9'; });
        if (!isNumeric) {
            Notification::create("Reference Level: enter a numeric GD level ID",
                                 NotificationIcon::Warning)->show();
            onDone("");
            return;
        }

        // Cache first — pinned example IDs hit this on every generation.
        {
            auto& cache = levelSummaryCache();
            auto it = cache.find(trimmed);
            if (it != cache.end() && !it->second.empty()) {
                log::info("Reference level {}: served from cache ({} chars)",
                          trimmed, it->second.size());
                onDone(it->second);
                return;
            }
        }

        showStatus(fmt::format("Fetching reference level {}...", trimmed));
        log::info("Reference level: fetching ID {}", trimmed);

        auto request = web::WebRequest();
        // GD servers behave best with their own UA convention (a real one is
        // also fine). Match what existing GD-API mods send.
        request.userAgent("");
        request.header("Content-Type", "application/x-www-form-urlencoded");
        std::string body = fmt::format(
            "levelID={}&secret=Wmfd2893gb0&gameVersion=22&binaryVersion=42",
            trimmed
        );
        request.bodyString(body);
        request.timeout(std::chrono::seconds(20));

        m_toolListenerLevel.spawn(
            request.post("https://www.boomlings.com/database/downloadGJLevel22.php"),
            [this, trimmed, onDone = std::move(onDone)](web::WebResponse resp) {
                if (!resp.ok()) {
                    log::warn("Level fetch HTTP {}", resp.code());
                    Notification::create(
                        fmt::format("Level {} fetch failed (HTTP {})", trimmed, resp.code()),
                        NotificationIcon::Warning)->show();
                    onDone("");
                    return;
                }
                std::string respBody = resp.string().unwrapOr("");
                if (respBody.empty() || respBody == "-1") {
                    Notification::create(
                        fmt::format("Level {} not found on GD servers", trimmed),
                        NotificationIcon::Warning)->show();
                    onDone("");
                    return;
                }

                // Strip everything after the first '#' (hash + creators sections).
                auto hashPos = respBody.find('#');
                if (hashPos != std::string::npos) respBody.resize(hashPos);

                auto kv = parseColonKV(respBody);
                std::string name = kv.count("2") ? kv["2"] : "?";
                std::string desc;
                if (kv.count("3")) desc = base64UrlDecodeText(kv["3"]);
                std::string k4 = kv.count("4") ? kv["4"] : "";
                if (k4.empty()) {
                    log::error("Level fetch: response had no key 4 (got: {})", respBody.substr(0, 200));
                    onDone("");
                    return;
                }

                auto bytes = urlSafeBase64Decode(k4);
                if (bytes.empty()) {
                    log::error("Level fetch: base64 decode of k4 failed ({} chars)", k4.size());
                    onDone("");
                    return;
                }
                std::string lvlStr = zlibInflateBytes(bytes.data(), bytes.size());
                if (lvlStr.empty()) {
                    log::error("Level fetch: zlib inflate of k4 failed");
                    onDone("");
                    return;
                }
                std::string formatted = AIGeneratorPopup::summarizeReferenceLevel(
                    name, desc, trimmed, lvlStr);
                log::info("Reference level {}: '{}' summarized ({} chars, level string was {} chars)",
                          trimmed, name, formatted.size(), lvlStr.size());
                if (!formatted.empty()) {
                    auto& cache = levelSummaryCache();
                    // Bound the disk file: drop a random entry past 30. (Levels
                    // are immutable; WHICH ones stay cached barely matters.)
                    if (cache.size() >= 30) cache.erase(cache.begin());
                    cache[trimmed] = formatted;
                    persistLevelCache();
                }
                onDone(formatted);
            }
        );
    }

    // Parses a decoded GD level string and produces a compact EAI-formatted
    // summary the AI can crib from. Maps object IDs to names via OBJECT_IDS
    // where possible; skips unknown ones rather than emitting opaque "obj_N"
    // tokens that the AI couldn't reproduce.
    static std::string summarizeReferenceLevel(const std::string& name,
                                               const std::string& desc,
                                               const std::string& idStr,
                                               const std::string& lvlStr)
    {
        const auto& idToName = objectIdToName();

        // Parse objects: skip header (first ';'-section), then each object is
        // "1,id,2,x,3,y,..." comma-pair list.
        size_t headerEnd = lvlStr.find(';');
        if (headerEnd == std::string::npos) return "";

        struct Obj { int id; float x, y; std::string name; };
        std::vector<Obj> known;
        known.reserve(2000);
        std::unordered_map<std::string, int> typeCounts;
        float maxX = 0.f;

        size_t pos = headerEnd + 1;
        while (pos < lvlStr.size()) {
            size_t end = lvlStr.find(';', pos);
            if (end == std::string::npos) end = lvlStr.size();
            std::string_view obj(lvlStr.data() + pos, end - pos);
            pos = end + 1;
            if (obj.empty()) continue;
            int id = -1; float x = 0, y = 0;
            // Parse key/value pairs separated by commas. We only need 1/2/3.
            size_t i = 0;
            while (i < obj.size()) {
                size_t comma1 = obj.find(',', i);
                if (comma1 == std::string_view::npos) break;
                size_t comma2 = obj.find(',', comma1 + 1);
                std::string_view key = obj.substr(i, comma1 - i);
                std::string_view val = (comma2 == std::string_view::npos)
                    ? obj.substr(comma1 + 1)
                    : obj.substr(comma1 + 1, comma2 - comma1 - 1);
                if (key == "1") {
                    if (auto n = geode::utils::numFromString<int>(val)) id = n.unwrap();
                } else if (key == "2") {
                    if (auto n = geode::utils::numFromString<float>(val)) x = n.unwrap();
                } else if (key == "3") {
                    if (auto n = geode::utils::numFromString<float>(val)) y = n.unwrap();
                }
                if (comma2 == std::string_view::npos) break;
                i = comma2 + 1;
            }
            if (id < 0) continue;
            if (x > maxX) maxX = x;
            auto it = idToName.find(id);
            if (it == idToName.end()) continue;  // unknown objects skipped
            known.push_back({id, x, y, it->second});
            typeCounts[it->second] += 1;
        }

        // Build the summary
        std::string out;
        out += fmt::format("Reference: \"{}\" (GD level ID {})\n", name, idStr);
        if (!desc.empty()) {
            std::string trimmedDesc = desc;
            if (trimmedDesc.size() > 200) trimmedDesc.resize(200);
            out += fmt::format("Description: {}\n", trimmedDesc);
        }
        out += fmt::format("Scale: {} recognized objects, X range 0-{:.0f} ({:.0f} cells)\n",
                           (int)known.size(), maxX, maxX / 30.0f);

        // Top object types by frequency
        std::vector<std::pair<std::string, int>> sortedCounts(typeCounts.begin(), typeCounts.end());
        std::sort(sortedCounts.begin(), sortedCounts.end(),
                  [](const auto& a, const auto& b){ return a.second > b.second; });
        out += "Top types:";
        for (int i = 0; i < std::min((int)sortedCounts.size(), 8); ++i) {
            out += fmt::format(" {}×{}", sortedCounts[i].first, sortedCounts[i].second);
        }
        out += "\n";

        // First ~25 objects as a concrete sample so the AI can see real placement.
        if (!known.empty()) {
            out += "Sample (first 25 objects): [";
            int taken = 0;
            for (auto& o : known) {
                if (taken > 0) out += ",";
                out += fmt::format("{{\"type\":\"{}\",\"x\":{:.0f},\"y\":{:.0f}}}",
                                   o.name, o.x, o.y);
                if (++taken >= 25) break;
            }
            out += "]\n";
        }
        return out;
    }

    // ── Tool 2: search Newgrounds for a song (or fetch by ID directly) ─────
    // Accepts either a numeric ID or a free-text query. For queries we hit
    // the public search URL and pull the first song result.
    // Newgrounds via DuckDuckGo: when NG itself is unreachable (Cloudflare
    // 403, cert failure under proton), search the public index for
    // site:newgrounds.com/audio/listen links instead. Returns "id - title"
    // pairs the model can pick from; reuses the proven lite-endpoint shape.
    void fireFetchNGViaDDG(const std::string& query,
                           std::function<void(const std::string&)> onDone)
    {
        auto request = web::WebRequest();
        request.userAgent("Mozilla/5.0 (X11; Linux x86_64; rv:135.0) Gecko/20100101 Firefox/135.0");
        request.timeout(std::chrono::seconds(15));
        std::string url = fmt::format(
            "https://lite.duckduckgo.com/lite/?q={}",
            urlFormEncode("site:newgrounds.com/audio/listen " + query));
        m_toolListenerNG.spawn(
            request.get(url),
            [query, onDone = std::move(onDone)](web::WebResponse resp) mutable {
                if (!resp.ok()) {
                    onDone(fmt::format(
                        "(Newgrounds AND the search fallback are unreachable "
                        "(HTTP {}). Do NOT retry song tools this generation; "
                        "only set song_id if the user gave one explicitly.)",
                        resp.code()));
                    return;
                }
                std::string body = resp.string().unwrapOr("");
                // Result anchors: href=".../audio/listen/<id>" ... >TITLE<
                static const std::regex rowRx(
                    R"(audio/listen/(\d+)[^>]*>([^<]{2,80})<)",
                    std::regex::ECMAScript | std::regex::optimize);
                std::string out;
                int found = 0;
                std::unordered_set<std::string> seen;
                for (auto it = std::sregex_iterator(body.begin(), body.end(), rowRx);
                     it != std::sregex_iterator() && found < 5; ++it) {
                    std::string id = (*it)[1].str();
                    if (!seen.insert(id).second) continue;
                    if (found) out += "\n";
                    out += fmt::format("  song_id={} - {}", id,
                                       stripHtmlBasic((*it)[2].str()));
                    ++found;
                }
                if (!found) {
                    onDone(fmt::format(
                        "(no Newgrounds songs found for \"{}\" via the search "
                        "index - try ONE different phrasing, or continue "
                        "without a song.)", query));
                    return;
                }
                log::info("NG-via-DDG: {} result(s) for '{}'", found, query);
                onDone(fmt::format(
                    "Newgrounds songs matching \"{}\" (via search index):\n{}\n"
                    "Set level_metadata.song_id to your pick.", query, out));
            }
        );
    }

    void fireFetchNGSong(const std::string& input,
                        std::function<void(const std::string&)> onDone)
    {
        if (input.empty()) { onDone(""); return; }
        std::string trimmed = trimKey(input);
        bool isNumeric = !trimmed.empty()
            && std::all_of(trimmed.begin(), trimmed.end(),
                           [](char c){ return (unsigned char)c >= '0' && (unsigned char)c <= '9'; });

        if (isNumeric) {
            // numFromString instead of stoi: an absurdly long digit string from
            // the AI must not throw out_of_range.
            auto idRes = geode::utils::numFromString<int>(trimmed);
            if (!idRes) { onDone(""); return; }
            fetchNGSongByID(idRes.unwrap(), std::move(onDone));
        } else {
            // Search NG, pull first hit, then fetch its ID.
            showStatus(fmt::format("Searching Newgrounds: \"{}\"...", trimmed));
            log::info("NG search: '{}'", trimmed);
            // NG sits behind Cloudflare and 403s naive UAs. Use a realistic
            // Chrome/Edge UA + sec-fetch headers so the request looks like a
            // browser. Without these the request returns 403 reliably.
            auto request = web::WebRequest();
            request.userAgent(
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/126.0.0.0 Safari/537.36");
            request.header("Accept",
                "text/html,application/xhtml+xml,application/xml;q=0.9,"
                "image/avif,image/webp,*/*;q=0.8");
            request.header("Accept-Language", "en-US,en;q=0.9");
            request.header("Sec-Fetch-Dest", "document");
            request.header("Sec-Fetch-Mode", "navigate");
            request.header("Sec-Fetch-Site", "none");
            request.header("Upgrade-Insecure-Requests", "1");
            request.timeout(std::chrono::seconds(20));
            std::string url = fmt::format(
                "https://www.newgrounds.com/search/conduct/audio?terms={}",
                urlFormEncode(trimmed)
            );
            m_toolListenerNG.spawn(
                request.get(url),
                [this, trimmed, onDone = std::move(onDone)](web::WebResponse resp) mutable {
                    if (!resp.ok()) {
                        log::warn("NG search HTTP {} - falling back to DDG site-search",
                                  resp.code());
                        // NG 403s (Cloudflare) and cert-fails (-60, proton)
                        // routinely. The DDG lite route finds the same
                        // /audio/listen/<id> links from the search index.
                        this->fireFetchNGViaDDG(trimmed, std::move(onDone));
                        return;
                    }
                    std::string body = resp.string().unwrapOr("");
                    // Look for the first audio-listen link in the HTML.
                    // NG result links look like /audio/listen/<id> or /audio_search/embed/<id>
                    static const std::regex idRx(
                        R"(/audio/listen/(\d+))", std::regex::optimize | std::regex::ECMAScript);
                    std::smatch m;
                    if (!std::regex_search(body, m, idRx)) {
                        Notification::create(
                            fmt::format("No NG results for \"{}\"", trimmed),
                            NotificationIcon::Warning)->show();
                        onDone(fmt::format(
                            "(no Newgrounds results for \"{}\" — try a different "
                            "query once, or continue without a song.)", trimmed));
                        return;
                    }
                    auto songIdRes = geode::utils::numFromString<int>(m[1].str());
                    if (!songIdRes) { onDone(""); return; }
                    int songId = songIdRes.unwrap();
                    log::info("NG search: first result ID = {}", songId);
                    this->fetchNGSongByID(songId, std::move(onDone));
                }
            );
        }
    }

    // Fetch a single NG song page and extract title/artist. Returns a
    // formatted string the AI can use. The level_metadata.song_id field
    // applied via the AI's normal output then ties the song to the level.
    void fetchNGSongByID(int songId, std::function<void(const std::string&)> onDone) {
        showStatus(fmt::format("Fetching NG song #{}...", songId));
        log::info("NG: fetching song {}", songId);
        // Same browser-like headers as the search request — without them NG
        // returns 403 from its Cloudflare edge.
        auto request = web::WebRequest();
        request.userAgent(
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/126.0.0.0 Safari/537.36");
        request.header("Accept",
            "text/html,application/xhtml+xml,application/xml;q=0.9,"
            "image/avif,image/webp,*/*;q=0.8");
        request.header("Accept-Language", "en-US,en;q=0.9");
        request.header("Sec-Fetch-Dest", "document");
        request.header("Sec-Fetch-Mode", "navigate");
        request.header("Sec-Fetch-Site", "none");
        request.header("Upgrade-Insecure-Requests", "1");
        request.timeout(std::chrono::seconds(20));
        m_toolListenerNG.spawn(
            request.get(fmt::format("https://www.newgrounds.com/audio/listen/{}", songId)),
            [this, songId, onDone = std::move(onDone)](web::WebResponse resp) {
                if (!resp.ok()) {
                    log::warn("NG song fetch HTTP {} - falling back to DDG index",
                              resp.code());
                    this->fireFetchNGViaDDG(fmt::format("{}", songId),
                                            std::move(onDone));
                    return;
                }
                std::string body = resp.string().unwrapOr("");
                std::string title  = "Unknown Title";
                std::string artist = "Unknown Artist";

                // Title from <title>...</title> (NG format: "Song Name - Artist | Free Listening on Newgrounds")
                {
                    static const std::regex titleRx(
                        R"(<title>([^<]*)</title>)", std::regex::ECMAScript);
                    std::smatch m;
                    if (std::regex_search(body, m, titleRx)) {
                        std::string full = stripHtmlBasic(m[1].str());
                        auto sep = full.find(" - ");
                        if (sep != std::string::npos) {
                            title = full.substr(0, sep);
                            auto rest = full.substr(sep + 3);
                            auto bar = rest.find(" |");
                            artist = (bar == std::string::npos) ? rest : rest.substr(0, bar);
                        } else {
                            title = full;
                        }
                    }
                }
                // Artist sometimes also appears as `<meta name="author" content="...">`.
                {
                    static const std::regex metaAuthorRx(
                        R"(<meta[^>]+name=\"author\"[^>]+content=\"([^\"]+)\")",
                        std::regex::ECMAScript);
                    std::smatch m;
                    if (artist == "Unknown Artist" && std::regex_search(body, m, metaAuthorRx)) {
                        artist = m[1].str();
                    }
                }

                std::string out = fmt::format(
                    "Newgrounds song #{}: \"{}\" by {}\n"
                    "  → Apply via level_metadata.song_id={}\n"
                    "  → BPM is not in the page metadata; if the user mentioned one\n"
                    "    use it for a beat_sync macro, otherwise estimate from genre.\n",
                    songId, title, artist, songId);
                log::info("NG song {}: '{}' by {}", songId, title, artist);
                Notification::create(
                    fmt::format("Found: {} by {}", title, artist),
                    NotificationIcon::Success)->show();
                onDone(out);
            }
        );
    }

    // ── Tool 3: DuckDuckGo HTML search ─────────────────────────────────────
    // POSTs to the html.duckduckgo.com endpoint (no JS required) and scrapes
    // the top 3 results' title + snippet for prompt enrichment.
    // Format parsed titles/snippets into the model-facing result block.
    static std::string formatSearchResults(const std::string& query,
                                           const std::vector<std::string>& titles,
                                           const std::vector<std::string>& snippets) {
        std::string out = fmt::format("DuckDuckGo results for \"{}\":\n", query);
        int n = std::min((int)titles.size(), 3);
        for (int i = 0; i < n; ++i) {
            out += fmt::format("  {}. {}\n", i + 1, titles[i]);
            if (i < (int)snippets.size()) {
                std::string snip = snippets[i];
                if (snip.size() > 220) snip.resize(220);
                if (!snip.empty()) out += fmt::format("     {}\n", snip);
            }
        }
        return out;
    }

    // Returned to the model when every search attempt fails — actionable,
    // unlike the old empty string (the model would just retry or stall).
    static constexpr const char* WEB_SEARCH_UNAVAILABLE =
        "(web search unavailable: the search service is blocked or "
        "rate-limiting on this network. Do NOT retry web_search this "
        "generation — continue designing from your own knowledge.)";

    // Fallback: DDG's /lite endpoint (simpler markup, separate anomaly
    // budget from /html). GET with a plain browser UA.
    void fireFetchWebSearchLite(const std::string& query,
                                std::function<void(const std::string&)> onDone)
    {
        auto request = web::WebRequest();
        request.userAgent("Mozilla/5.0 (X11; Linux x86_64; rv:135.0) Gecko/20100101 Firefox/135.0");
        request.timeout(std::chrono::seconds(15));
        m_toolListenerWeb.spawn(
            request.get(fmt::format("https://lite.duckduckgo.com/lite/?q={}",
                                    urlFormEncode(query))),
            [query, onDone = std::move(onDone)](web::WebResponse resp) {
                if (!resp.ok()) {
                    log::warn("Web search (lite) HTTP {}", resp.code());
                    onDone(WEB_SEARCH_UNAVAILABLE);
                    return;
                }
                std::string body = resp.string().unwrapOr("");
                // Lite markup: <a rel="nofollow" href="..." class='result-link'>TITLE</a>
                //              <td class='result-snippet'>SNIPPET</td>
                static const std::regex titleRx(
                    R"(class='result-link'>([^<]+)<)",
                    std::regex::ECMAScript | std::regex::optimize);
                // [\s\S] instead of `.` — ECMAScript `.` excludes newlines and
                // the snippet <td> spans multiple lines in lite markup.
                static const std::regex snippetRx(
                    R"(class='result-snippet'>([\s\S]*?)</td>)",
                    std::regex::ECMAScript | std::regex::optimize);
                std::vector<std::string> titles, snippets;
                for (auto it = std::sregex_iterator(body.begin(), body.end(), titleRx);
                     it != std::sregex_iterator() && titles.size() < 5; ++it)
                    titles.push_back(stripHtmlBasic(it->str(1)));
                for (auto it = std::sregex_iterator(body.begin(), body.end(), snippetRx);
                     it != std::sregex_iterator() && snippets.size() < 5; ++it)
                    snippets.push_back(stripHtmlBasic(it->str(1)));

                if (titles.empty()) {
                    log::warn("Web search: lite endpoint had no results either ({})",
                              body.find("anomaly") != std::string::npos
                                  ? "bot challenge" : "no matches");
                    onDone(WEB_SEARCH_UNAVAILABLE);
                    return;
                }
                log::info("Web search (lite fallback) returned {} results",
                          std::min((int)titles.size(), 3));
                onDone(formatSearchResults(query, titles, snippets));
            }
        );
    }

    void fireFetchWebSearch(const std::string& query,
                           std::function<void(const std::string&)> onDone)
    {
        if (query.empty()) { onDone(""); return; }
        std::string trimmed = trimKey(query);
        if (trimmed.empty()) { onDone(""); return; }

        // Session cache — the model often repeats a query across extension /
        // refinement rounds. Memory-only (results go stale; no disk).
        static std::unordered_map<std::string, std::string> s_searchCache;
        {
            auto it = s_searchCache.find(trimmed);
            if (it != s_searchCache.end()) {
                log::info("Web search: '{}' served from session cache", trimmed);
                onDone(it->second);
                return;
            }
        }

        showStatus(fmt::format("Web search: \"{}\"...", trimmed));
        log::info("Web search: '{}'", trimmed);
        auto request = web::WebRequest();
        // Full modern UA — DDG's anomaly detection flags bare/truncated UAs
        // far more often (observed: 200 OK with a bot-challenge page and
        // zero results, which used to read as "web search broken").
        request.userAgent(
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
        request.header("Content-Type", "application/x-www-form-urlencoded");
        request.timeout(std::chrono::seconds(15));
        request.bodyString("q=" + urlFormEncode(trimmed));

        m_toolListenerWeb.spawn(
            request.post("https://html.duckduckgo.com/html/"),
            [this, trimmed, onDone = std::move(onDone)](web::WebResponse resp) mutable {
                std::string body = resp.ok() ? resp.string().unwrapOr("") : "";
                // DDG HTML results are inside `<a class="result__a" href="...">TITLE</a>`
                // and `<a class="result__snippet">SNIPPET</a>` blocks.
                static const std::regex titleRx(
                    R"(class=\"result__a\"[^>]*>([^<]+)<)",
                    std::regex::ECMAScript | std::regex::optimize);
                static const std::regex snippetRx(
                    R"(class=\"result__snippet\"[^>]*>([\s\S]*?)</a>)",
                    std::regex::ECMAScript | std::regex::optimize);

                std::vector<std::string> titles, snippets;
                for (auto it = std::sregex_iterator(body.begin(), body.end(), titleRx);
                     it != std::sregex_iterator() && titles.size() < 5; ++it)
                    titles.push_back(stripHtmlBasic(it->str(1)));
                for (auto it = std::sregex_iterator(body.begin(), body.end(), snippetRx);
                     it != std::sregex_iterator() && snippets.size() < 5; ++it)
                    snippets.push_back(stripHtmlBasic(it->str(1)));

                if (titles.empty()) {
                    // HTTP error, network block, or DDG's anomaly page
                    // (200 OK, no results) — try the lite endpoint once.
                    log::warn("Web search: primary endpoint failed (HTTP {}, {}) — "
                              "falling back to lite",
                              resp.code(),
                              body.find("anomaly") != std::string::npos
                                  ? "bot challenge" : "no results parsed");
                    this->fireFetchWebSearchLite(trimmed, std::move(onDone));
                    return;
                }

                log::info("Web search returned {} results",
                          std::min((int)titles.size(), 3));
                std::string formatted = formatSearchResults(trimmed, titles, snippets);
                if (s_searchCache.size() < 50) s_searchCache[trimmed] = formatted;
                onDone(std::move(formatted));
            }
        );
    }

    // ─── Multi-turn tool-use loop ──────────────────────────────────────────
    // Wires up the abstraction in tool_use.hpp to AIGeneratorPopup's state.
    // The loop runs entirely client-side: each round POSTs to the user's
    // configured provider, parses the response into tool calls vs final
    // text, executes any tool calls (via the same fireFetchX helpers used
    // by the pre-generation Tools UI), appends the results to the in-memory
    // conversation history, and loops.

    // State that lives across rounds. Not used outside the loop, so it gets
    // reset every time runToolLoop starts.
    std::vector<toolUse::Message> m_toolHistory;
    int                           m_toolIterations  = 0;
    // Tool use is UNBOUNDED by design — the AI runs as many rounds, and as
    // many calls to any one tool, as it wants. Runaway loops are prevented
    // structurally, NOT by a per-tool count:
    //   - an exact-duplicate-call guard (same name+args) → reuse the prior result
    //   - a very high anti-runaway backstop that finalizes GRACEFULLY (never errors)
    bool                          m_forceFinalize     = false;  // backstop tripped: next round must finalize
    int                           m_forceFinalizeTries = 0;     // bounds the forced-finalize retries
    std::unordered_map<std::string, int> m_toolCallSigCounts;   // (name|args) → count, duplicate guard
    std::string                   m_toolProvider;
    std::string                   m_toolModel;
    std::string                   m_toolApiKey;

    // Length enforcement: accumulate objects across "extension" rounds so the
    // mod can refuse to apply until the level is long enough. Each time
    // processFinalResponse parses a response with insufficient length, it
    // appends the parsed objects to m_accumulatedObjects and injects an
    // "extend further" user message into m_toolHistory, then runs another
    // tool round. After m_maxExtensionRounds we give up and apply whatever
    // we've got (so the AI can't pin the user forever).
    matjson::Value m_accumulatedObjects = matjson::Value::array();
    LengthTarget   m_lengthTarget       = {"Medium", 30.f, 60.f};
    int            m_extensionRounds    = 0;
    int            m_maxExtensionRounds = 4;
    int            m_passabilityFixRounds = 0;     // bounded by MAX_PASSABILITY_FIXES in processFinalResponse
    int            m_refinementRounds     = 0;     // bounded by setting "refinement-rounds" (0 disables)
    bool           m_critiqueDone         = false; // self-critique fired this generation
    bool           m_critiquePending      = false; // next response is the critique reply
    bool           m_decorationPassDone   = false; // two-pass decoration fired
    bool           m_followUpTurn         = false; // conversational turn: skip length/refine gates
    int            m_followUpMode         = 0;     // 0 edit, 1 plan, 2 chat (this turn)
    int            m_editEnforceRounds    = 0;     // edit-workload continuation rounds used
    int            m_targetObjRounds      = 0;     // target-object continuation rounds used
    // True only when THIS turn actually runs through doToolRound. The
    // loop-back gates key off this — a non-empty m_toolHistory alone proves
    // nothing (single-shot paths seed it for follow-up chat), and looping a
    // single-shot turn into doToolRound breaks Platinum (no /api/chat),
    // ignores enable-ai-tools=false, and hijacks mutation/copilot turns.
    bool           m_usingToolLoop        = false;

    // Entry point. Called instead of callAPI's single-shot when tool use is
    // enabled and the selected provider supports it.
    void runToolLoop(const std::string& userPrompt, const std::string& rawApiKey) {
        m_toolApiKey   = trimKey(rawApiKey);
        m_toolProvider = Mod::get()->getSettingValue<std::string>("ai-provider");
        m_toolModel    = getProviderModel(m_toolProvider);
        // Tool use is unbounded — no round budget. The model runs until it
        // emits a final answer; the per-tool caps + duplicate guard +
        // backstop in doToolRound prevent runaway loops.
        m_toolIterations  = 0;
        m_forceFinalize   = false;
        m_forceFinalizeTries = 0;
        m_usingToolLoop   = true;
        m_toolHistory.clear();
        m_accumulatedObjects = matjson::Value::array();
        m_extensionRounds = 0;
        m_passabilityFixRounds = 0;
        m_refinementRounds = 0;
        m_critiqueDone    = false;
        m_critiquePending = false;
        m_decorationPassDone = false;
        m_toolCallSigCounts.clear();
        m_followUpTurn = false;
        m_editEnforceRounds = 0;
        m_targetObjRounds   = 0;
        m_lengthTarget = lengthTargetForSetting(
            Mod::get()->getSettingValue<std::string>("length"));

        // Prompt context locals. (callAPI — our only caller — already wrote
        // the s_last* rating-context statics from the same settings.)
        std::string difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        std::string style      = Mod::get()->getSettingValue<std::string>("style");
        std::string length     = Mod::get()->getSettingValue<std::string>("length");
        // "levelID" is the overlay's reference-level mode, not a style word —
        // the actual reference arrives via the "style: <id>" prompt directive.
        if (style == "levelID") style = "match the reference level";

        std::string levelDataSection;
        if (m_editMode) {
            // Edit runs get the numbered inventory — MOVE/DELETE/EDIT
            // selectors resolve against exactly this listing.
            std::string inv = buildLevelInventoryListing(1500);
            log::info("=== Tool-loop: edit inventory context ({} chars) ===", inv.size());
            levelDataSection = "\n\n" + inv;
        } else if (!m_shouldClearLevel) {
            std::string levelJson = buildLevelDataJson();
            log::info("=== Tool-loop: current level data context ===");
            logLong("LevelData", levelJson);
            levelDataSection = "\n\nCurrent level data (build upon or extend):\n" + levelJson;
        }

        // Compute the concrete length target in BOTH seconds AND units so the
        // model has the math pre-done.
        float targetMinX = m_lengthTarget.minSeconds * GD_PLAYER_SPEED_1X;
        float targetMaxX = m_lengthTarget.maxSeconds * GD_PLAYER_SPEED_1X;

        // Edit mode: full co-editor framing instead of from-scratch creation.
        std::string fullUserPrompt;
        if (m_editMode) {
            int editTarget = (int)Mod::get()->getSettingValue<int64_t>("edit-target-ops");
            fullUserPrompt = fmt::format(
                "Rework the user's EXISTING Geometry Dash level.\n\n"
                "Request: {}\nDifficulty: {} | Style: {}\n\n"
                "You are a full co-editor, not a decorator. MOVE objects to fix "
                "pacing and spacing, DELETE clutter/broken/ugly sections, EDIT "
                "rotation/scale/colors to restyle, and ADD new geometry, hazards "
                "and decoration. Restructure boldly — small timid patches are a "
                "failure mode.{}\n"
                "Use BULK selectors so single lines touch dozens or hundreds of "
                "objects: MOVE rect:x1,y1,x2,y2 dx=.. dy=.., DELETE id:spike, "
                "EDIT #a-b scale=... Ops execute in order when the result stages; "
                "everything is previewed and reversible, so do not hold back.\n"
                "Use tools (analyze_level, check_passability, get_level_region) "
                "to understand the level before and after big changes.{}",
                userPrompt, difficulty, style,
                editTarget > 0 ? fmt::format(
                    "\nWORKLOAD REQUIREMENT: this rework must total at least {} "
                    "edits (every moved, deleted, restyled, or added object "
                    "counts as one). The mod tallies your ops and will keep "
                    "asking you to continue until the target is met.", editTarget)
                               : std::string(),
                levelDataSection);
            // (reference levels / style ref / beat grid append below for
            // both branches)
        } else
        fullUserPrompt = fmt::format(
            "Generate a Geometry Dash level:\n\n"
            "Request: {}\nDifficulty: {}\nStyle: {}\nLength: {} "
            "(target {:.0f}-{:.0f} seconds = max X around {:.0f}-{:.0f}){}\n\n"
            "GD length tiers and the X distance they correspond to at 1× speed\n"
            "(player moves {:.1f} units/sec):\n"
            "  Tiny    : 0-10 s    ≈ 0-3115 units of X\n"
            "  Short   : 10-30 s   ≈ 3115-9347 units\n"
            "  Medium  : 30-60 s   ≈ 9347-18695 units\n"
            "  Long    : 60-120 s  ≈ 18695-37389 units\n"
            "  XL      : 120 s+    ≈ 37389+ units\n"
            "Your level's playable extent is determined by the largest X of any\n"
            "non-trigger object. You MUST reach at least the user's target min\n"
            "seconds (max X ≥ {:.0f}). If your first JSON answer is shorter, the\n"
            "mod will ask you to EXTEND, and you will keep appending until the\n"
            "target is met (capped at {} extension rounds).\n\n"
            "You have tools available (declared in the request's tools field — each\n"
            "carries its own usage docs). The user does NOT see tool calls or their\n"
            "results; only you do. Call them whenever they'd help: verify drafts with\n"
            "get_level_length / check_passability between rounds, discover object\n"
            "names with search_objects, pull references with download_level.\n"
            "There is NO limit on tool calls — use as many as the task needs. When\n"
            "you have enough context, STOP calling tools and return your final answer.\n\n"
            "Final answer JSON: \"analysis\" string, \"objects\" array, optional \"macros\"\n"
            "array (use aggressively), optional \"level_metadata\". DO NOT wrap the\n"
            "final JSON in another tool call — emit it as your normal assistant reply.",
            userPrompt, difficulty, style, length,
            m_lengthTarget.minSeconds, m_lengthTarget.maxSeconds,
            targetMinX, targetMaxX,
            levelDataSection,
            GD_PLAYER_SPEED_1X,
            targetMinX, m_maxExtensionRounds
        );

        // User-pinned reference levels ("example-level-ids" setting, saved
        // across generations): instruct the model to pull each one down as a
        // style reference before designing.
        {
            auto refIds = parseExampleLevelIds();
            if (!refIds.empty()) {
                std::string idList;
                for (size_t i = 0; i < refIds.size(); ++i) {
                    if (i) idList += ", ";
                    idList += fmt::format("{}", refIds[i]);
                }
                fullUserPrompt += fmt::format(
                    "\n\nREFERENCE LEVELS (user-pinned): before designing, call "
                    "download_level for each of these IDs and absorb their pacing, "
                    "density, and structure choices: {}. Imitate their feel, do "
                    "not copy them verbatim.", idList);
                log::info("Injecting {} pinned reference level(s): {}",
                          refIds.size(), idList);
            }
        }

        // "style: <ID>" reference — in tool mode the model fetches it itself.
        if (!m_styleRefId.empty()) {
            fullUserPrompt += fmt::format(
                "\n\nSTYLE REFERENCE: FIRST call download_level({}) and match "
                "that level's palette, object families, and density feel - "
                "NOT its layout.", m_styleRefId);
        }
        // Music sync: prompt named a BPM -> hand the model the beat grid.
        fullUserPrompt += buildBeatGridNote(userPrompt);

        // Build the conversation history.
        toolUse::Message sys;
        sys.role = toolUse::MessageRole::System;
        sys.text = buildSystemPrompt();
        appendModeContext(sys.text);  // co-op rides the tool loop too
        m_toolHistory.push_back(std::move(sys));

        // ── In-context few-shot for small EditorAI Ollama fine-tunes ─────
        // Our 1.5B fine-tune (entity12208/editorai:v2 and similar) learned
        // the analysis+objects schema but under the mod's verbose runtime
        // system prompt it tends to hallucinate object shapes ({"type":"line",
        // "x1":...}) or collapse into degenerate repetition. We anchor BOTH
        // behaviors in-context with two pairs:
        //   (1) plain level-gen (anchors object schema + valid type strings)
        //   (2) length-targeted with a get_level_length tool call (preserves
        //       the proactive tool-use behavior — without this, the pure
        //       level-gen example crowds out tool calling).
        // Only applied when provider is ollama AND model name contains
        // "editorai", so Claude/GPT/etc. are untouched.
        {
            std::string lowerModel = m_toolModel;
            for (auto& c : lowerModel) c = (char)std::tolower((unsigned char)c);
            if (m_toolProvider == "ollama" &&
                lowerModel.find("editorai") != std::string::npos) {
                // (1) Pure level-gen
                toolUse::Message u1;
                u1.role = toolUse::MessageRole::User;
                u1.text =
                    "Generate a Geometry Dash level:\n\n"
                    "Request: spikes and a small jump\n"
                    "Difficulty: medium\nStyle: classic\nLength: tiny";
                m_toolHistory.push_back(std::move(u1));

                toolUse::Message a1;
                a1.role = toolUse::MessageRole::Assistant;
                a1.text =
                    "## Plan\n"
                    "Tiny medium classic: two spikes then a block to jump onto.\n\n"
                    "## Level Script\n"
                    "META name=\"Jump\" desc=\"Tiny classic\" bg=1 ground=1\n"
                    "SECTION 0..400 difficulty=medium mode=cube\n"
                    "FLOOR 0..400\n"
                    "SPIKE 195\n"
                    "SPIKE 225\n"
                    "BLOCK 285 105\n"
                    "BLOCK 315 135\n"
                    "TRIGGER end at=400";
                m_toolHistory.push_back(std::move(a1));

                // (2) Length-targeted with tool call → result → batch
                toolUse::Message u2;
                u2.role = toolUse::MessageRole::User;
                u2.text = "Make a 10-second easy level. Use get_level_length to "
                          "check progress.";
                m_toolHistory.push_back(std::move(u2));

                toolUse::Message a2_tool;
                a2_tool.role = toolUse::MessageRole::Assistant;
                {
                    toolUse::ToolCall tc;
                    tc.id   = "call_fs_len_1";
                    tc.name = "get_level_length";
                    tc.args = matjson::Value::object();
                    a2_tool.toolCalls.push_back(std::move(tc));
                }
                m_toolHistory.push_back(std::move(a2_tool));

                toolUse::Message tres;
                tres.role = toolUse::MessageRole::ToolResults;
                {
                    toolUse::ToolResult tr;
                    tr.toolCallId = "call_fs_len_1";
                    tr.content    = R"({"current_length_seconds":0.0,)"
                                    R"("target_seconds":10,"x_cursor":0})";
                    tr.isError    = false;
                    tres.toolResults.push_back(std::move(tr));
                }
                m_toolHistory.push_back(std::move(tres));

                toolUse::Message a2_final;
                a2_final.role = toolUse::MessageRole::Assistant;
                a2_final.text =
                    "## Plan\n"
                    "10s easy intro: flat floor with spaced spikes and a jump orb.\n\n"
                    "## Level Script\n"
                    "META name=\"Easy 10s\" desc=\"Short intro\" bg=1 ground=1\n"
                    "SECTION 0..3115 difficulty=easy mode=cube\n"
                    "FLOOR 0..3115\n"
                    "SPIKE 400\n"
                    "SPIKE-TRAIN 800 count=2 spacing=45\n"
                    "ORB yellow 1300 135\n"
                    "STAIR-UP 1700 steps=3\n"
                    "SPIKE 2300\n"
                    "PAD yellow 2700 105\n"
                    "TRIGGER end at=3115";
                m_toolHistory.push_back(std::move(a2_final));

                log::info("EditorAI fine-tune detected — injected balanced few-shot "
                          "(1 level-gen + 1 tool-use exchange)");
            }
        }

        toolUse::Message usr;
        usr.role = toolUse::MessageRole::User;
        usr.text = std::move(fullUserPrompt);
        m_toolHistory.push_back(std::move(usr));

        log::info("=== Tool-use loop start ({}, model {}, unbounded rounds) ===",
                  m_toolProvider, m_toolModel);
        showStatus("Tool loop: round 1...");
        this->doToolRound();
    }

    // Append the "stop and finalize" instruction to the conversation WITHOUT
    // ever creating two consecutive user-role turns — Claude and Gemini both
    // reject that with a 400 (a ToolResults turn maps to role=user too). So:
    //   tail is Assistant / empty → push a fresh User turn (clean alternation)
    //   tail is ToolResults       → ride on its last result's content (same user turn)
    //   tail is User              → append to that user turn's text
    void injectStopNudge() {
        static const std::string STOP =
            "STOP calling tools now. Output your COMPLETE level as EAS "
            "('## Level Script' then the script) using everything you already "
            "have. Do NOT call any tool.";
        if (m_toolHistory.empty() ||
            m_toolHistory.back().role == toolUse::MessageRole::Assistant) {
            toolUse::Message stop;
            stop.role = toolUse::MessageRole::User;
            stop.text = STOP;
            m_toolHistory.push_back(std::move(stop));
            return;
        }
        auto& back = m_toolHistory.back();
        if (back.role == toolUse::MessageRole::ToolResults) {
            if (!back.toolResults.empty()) {
                // Ride on the last result's content (the builders emit that;
                // they do NOT emit a ToolResults message's .text field).
                back.toolResults.back().content += "\n\n" + STOP;
            } else {
                // Degenerate empty ToolResults — its .text would be dropped by
                // every builder, so push a fresh User turn instead (the tail
                // is a user-role turn, but an empty ToolResults emits nothing,
                // so this does not create a visible consecutive-user pair).
                toolUse::Message stop;
                stop.role = toolUse::MessageRole::User;
                stop.text = STOP;
                m_toolHistory.push_back(std::move(stop));
            }
        } else {  // User turn — append to its text.
            if (!back.text.empty()) back.text += "\n\n";
            back.text += STOP;
        }
    }

    void doToolRound() {
        // Loop-back gates in processFinalResponse call this AFTER
        // resetGenerationUI() dropped m_isGenerating — without re-arming it,
        // tool-call results get discarded (executeToolCalls bails on
        // !m_isGenerating), transient retries are dropped, and Cancel
        // becomes a no-op mid-loop.
        m_isGenerating = true;
        if (m_cancelBtn)   m_cancelBtn->setVisible(true);
        if (m_generateBtn) m_generateBtn->setVisible(false);
        ++m_toolIterations;

        // Anti-runaway backstop. Tool use is unbounded — a real generation
        // (even a heavy multi-phase edit with extension/refinement/critique
        // continuations) tops out well under this. Reaching it means the
        // model is stuck spinning; flip to forced finalize, which GRACEFULLY
        // salvages a final answer (never errors out, never an arbitrary
        // user-facing "limit"). Set high enough to be irrelevant to honest work.
        static constexpr int TOOL_ROUND_BACKSTOP = 200;
        if (!m_forceFinalize && m_toolIterations >= TOOL_ROUND_BACKSTOP) {
            log::warn("Tool loop reached the {}-round anti-runaway backstop — "
                      "forcing a final answer", TOOL_ROUND_BACKSTOP);
            m_forceFinalize = true;
            // Once finalizing, the loop-back gates must NOT re-loop the
            // salvaged answer (they'd push more rounds past the backstop and
            // stack another user turn after the STOP). Disabling the shared
            // gate flag short-circuits all of them; it's re-armed for the
            // next turn by runToolLoop / sendFollowUp.
            m_usingToolLoop = false;
        }
        if (m_forceFinalize)
            injectStopNudge();   // role-aware (never creates back-to-back user turns)

        // History pruning: every round re-sends the whole conversation, and
        // old tool results (level dumps, search results) are its bulk. Keep
        // the last 2 ToolResults messages verbatim; truncate older ones to a
        // stub. Structure (roles, tool_call ids, Gemini thoughtSignatures)
        // stays intact — only the payload text shrinks. Idempotent: already-
        // pruned results are below the cap and untouched.
        {
            constexpr size_t PRUNE_KEEP_LAST = 2;
            constexpr size_t PRUNE_CAP       = 300;
            size_t resultMsgsSeen = 0;
            for (auto it = m_toolHistory.rbegin(); it != m_toolHistory.rend(); ++it) {
                if (it->role != toolUse::MessageRole::ToolResults) continue;
                if (++resultMsgsSeen <= PRUNE_KEEP_LAST) continue;
                static const std::string PRUNE_MARK =
                    "... (older tool result pruned to save context; "
                    "call the tool again if you need it)";
                for (auto& tr : it->toolResults) {
                    // Marker check keeps this idempotent — content sits at
                    // ~CAP + mark length after the first prune.
                    if (tr.content.size() <= PRUNE_CAP + PRUNE_MARK.size()) continue;
                    // UTF-8-safe trim — a raw resize() can split a codepoint,
                    // and strict providers 400 on invalid UTF-8 in the body.
                    GenSession::utf8Trim(tr.content, PRUNE_CAP);
                    tr.content += PRUNE_MARK;
                }
            }
        }

        // Vision dedup: only the NEWEST snapshot travels with the request.
        // Historical images replay on every round otherwise — thousands of
        // vision tokens re-billed per round for stale views of the level.
        {
            bool newestKept = false;
            for (auto it = m_toolHistory.rbegin(); it != m_toolHistory.rend(); ++it) {
                if (it->imageB64.empty()) continue;
                if (!newestKept) { newestKept = true; continue; }
                it->imageB64.clear();
            }
        }

        auto body = toolUse::buildRequest(m_toolProvider, m_toolHistory, m_toolModel);
        std::string bodyStr = body.dump();
        std::string url     = toolUse::urlFor(m_toolProvider, m_toolModel);
        log::info("Tool round {}: POST {} ({} bytes)",
                  m_toolIterations, url, bodyStr.size());
        logApiRequest(m_toolProvider, m_toolModel, url, bodyStr);

        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");
        applyProviderAuth(request, m_toolProvider, m_toolApiKey);
        // Tool rounds tend to be smaller than the final answer, but the
        // first one carries the full system prompt + few-shot. Generous
        // timeout matches the single-shot path.
        request.timeout(providerTimeout(m_toolProvider));
        request.bodyString(bodyStr);
        m_listener.spawn(
            request.post(url),
            [this](web::WebResponse resp) { this->onToolRoundResponse(std::move(resp)); }
        );
    }

    // One automatic retry on transient provider failures (rate limit /
    // server error / overload). Returns true if a retry was scheduled.
    // Ref<> keeps the popup alive across the backoff; m_isGenerating gates
    // the re-fire so a cancelled generation doesn't resurrect itself.
    bool retryToolRoundIfTransient(int httpCode) {
        bool transient = httpCode == 429 || httpCode == 500 || httpCode == 502 ||
                         httpCode == 503 || httpCode == 529;
        if (!transient || m_transientRetries >= 1) return false;
        ++m_transientRetries;
        log::warn("Transient HTTP {} — retrying round once in 2s", httpCode);
        showStatus(fmt::format("Provider hiccup (HTTP {}) — retrying...", httpCode));
        // Move the Ref all the way into the main-thread closure: the worker
        // thread must never destroy a Ref copy (CCObject's ref-count is not
        // atomic, so release() off the main thread is a data race on ARM).
        Ref<AIGeneratorPopup> self = this;
        std::thread([self = std::move(self)]() mutable {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            Loader::get()->queueInMainThread([self = std::move(self)] {
                if (!self->m_isGenerating) return;
                --self->m_toolIterations;  // the retry isn't a new round
                self->doToolRound();
            });
            // thread-local self is moved-from (null) — destructor is a no-op
        }).detach();
        return true;
    }

    void onToolRoundResponse(web::WebResponse resp) {
        logApiResponse(resp.code(), resp.string().unwrapOr(""));
        if (!resp.ok()) {
            if (this->retryToolRoundIfTransient(resp.code())) return;
            auto [title, msg] = parseAPIError(
                resp.string().unwrapOr("No body"), resp.code());
            onError(title, msg);
            return;
        }
        // A round succeeded — refresh the transient-retry budget so a later
        // hiccup in this (now unbounded) loop still gets its one retry. The
        // old single budget for the WHOLE loop meant a second 429/503 on a
        // long generation hard-errored.
        m_transientRetries = 0;
        auto jsonRes = resp.json();
        if (!jsonRes) {
            onError("Tool Response",
                fmt::format("The AI provider returned a response that isn't valid JSON. This usually "
                            "means the provider is down, behind a captive portal, or your API key has "
                            "been revoked. Check the provider's status page and re-test your key. ({})",
                            autoErrorCode(60, 50)));
            return;
        }
        auto json   = jsonRes.unwrap();
        auto parsed = toolUse::parseResponse(m_toolProvider, json);

        if (!parsed.ok) {
            log::error("Tool round {} parse error: {}", m_toolIterations, parsed.errorMessage);
            onError("Tool Response",
                fmt::format("{} — try lowering the temperature in mod settings (closer to 0.3), or "
                            "switch providers. ({})",
                            parsed.errorMessage.empty()
                                ? std::string("The AI returned a tool-call format the parser didn't recognize")
                                : parsed.errorMessage,
                            autoErrorCode(60, 51)));
            return;
        }

        if (parsed.toolCalls.empty()) {
            // Final answer — same downstream pipeline as the single-shot path.
            if (!parsed.reasoningText.empty())
                pushSession(GenSession::Entry::Kind::Thinking, parsed.reasoningText);
            log::info("Tool loop finished after {} round(s); final response {} chars",
                      m_toolIterations, parsed.finalText.size());
            this->processFinalResponse(std::move(parsed.finalText), m_toolProvider);
            return;
        }

        // Forced finalize (anti-runaway backstop tripped in doToolRound): the
        // model was told to stop but is here with more tool calls. Salvage
        // any text it produced and finish; if it returned only tool calls and
        // no text, give it a couple more forced rounds (the stop nudge is
        // re-injected each time) before giving up gracefully. This never burns
        // unlimited quota, yet never imposes an arbitrary limit on honest work.
        if (m_forceFinalize) {
            std::string finalText = !parsed.finalText.empty()
                ? parsed.finalText
                : parsed.assistantTextWithCalls;
            if (!finalText.empty()) {
                log::info("Backstop finalize: salvaging {} chars of model text",
                          finalText.size());
                this->processFinalResponse(std::move(finalText), m_toolProvider);
                return;
            }
            if (++m_forceFinalizeTries >= 3) {
                onError("AI Couldn't Finish",
                    fmt::format("The AI kept calling tools and never produced a level, "
                                "even after being told to stop. Try a more capable "
                                "model or a simpler request. ({})",
                                autoErrorCode(60, 52)));
                return;
            }
            log::warn("Backstop finalize: no usable text yet — forcing another "
                      "round ({}/3)", m_forceFinalizeTries);
            // Record a minimal assistant turn (the model's ignored tool calls
            // are deliberately dropped) so the next forced round's STOP nudge
            // follows an assistant turn — strict alternation stays intact.
            toolUse::Message ack;
            ack.role = toolUse::MessageRole::Assistant;
            ack.text = parsed.assistantTextWithCalls.empty()
                ? "(Understood — emitting the level now.)"
                : parsed.assistantTextWithCalls;
            m_toolHistory.push_back(std::move(ack));
            this->doToolRound();
            return;
        }

        // Live thinking display: provider-reported reasoning first, then any
        // plain text the model wrote alongside its tool calls — this is the
        // "brainstorming as it goes" the overlay renders as collapsible
        // thinking entries.
        if (!parsed.reasoningText.empty())
            pushSession(GenSession::Entry::Kind::Thinking, parsed.reasoningText);
        if (!parsed.assistantTextWithCalls.empty())
            pushSession(GenSession::Entry::Kind::Thinking,
                        parsed.assistantTextWithCalls);

        // Record the assistant turn (with its tool calls) before executing.
        toolUse::Message asstMsg;
        asstMsg.role      = toolUse::MessageRole::Assistant;
        asstMsg.text      = parsed.assistantTextWithCalls;
        asstMsg.toolCalls = parsed.toolCalls;
        m_toolHistory.push_back(std::move(asstMsg));

        // Show user what's happening
        std::string status;
        for (size_t i = 0; i < parsed.toolCalls.size(); ++i) {
            if (i > 0) status += ", ";
            status += parsed.toolCalls[i].name;
        }
        showStatus(fmt::format("Round {}: tool calls → {}",
                               m_toolIterations, status));
        log::info("Round {} requested {} tool call(s): {}",
                  m_toolIterations, parsed.toolCalls.size(), status);

        // Calls sharing a TaskHolder (web/level/NG) stay serial within their
        // chain so they don't cancel each other; chains for different holders
        // run concurrently — a mixed round takes max(t) instead of sum(t).
        // See executeToolCalls / runToolChain below.
        auto calls = parsed.toolCalls;
        executeToolCalls(std::move(calls), {},
            [this](std::vector<toolUse::ToolResult> results) {
                // Guard against a cancelled generation: holder resets in
                // onCancel drop pending callbacks, but the synchronous-tool
                // chain can still complete this batch.
                if (!m_isGenerating) return;
                // (No round-countdown nudge: tool use is unbounded. The
                // per-tool caps + duplicate guard handle finalize pressure.)
                toolUse::Message resMsg;
                resMsg.role        = toolUse::MessageRole::ToolResults;
                resMsg.toolResults = std::move(results);
                m_toolHistory.push_back(std::move(resMsg));
                this->doToolRound();
            }
        );
    }

    // Executor internals: pops the first call of each chain, runs it,
    // recurses on the remainder with the result slotted in. When all slots
    // are filled, calls onAllDone with
    // the accumulated results vector.
    // Parallel tool execution. Calls are partitioned by which TaskHolder
    // their network request runs on (a holder cancels its in-flight task when
    // re-spawned, so two web_searches must stay serial) — but DIFFERENT
    // holders run concurrently. The common round of web_search +
    // download_level + search_newgrounds now takes max(t) instead of sum(t).
    struct ToolBatchState {
        std::vector<toolUse::ToolResult> results;   // slot-indexed (original order)
        size_t remaining = 0;
        std::function<void(std::vector<toolUse::ToolResult>)> onAllDone;
    };

    static int toolHolderCategory(const std::string& name) {
        if (name == "web_search")     return 1;  // m_toolListenerWeb
        if (name == "download_level") return 2;  // m_toolListenerLevel
        if (name == "search_newgrounds" || name == "get_newgrounds_song")
            return 3;                             // m_toolListenerNG
        return 0;  // synchronous mod-side tools — no shared holder
    }

    void runToolChain(std::vector<std::pair<size_t, toolUse::ToolCall>> chain,
                      std::shared_ptr<ToolBatchState> st, size_t idx)
    {
        if (idx >= chain.size()) return;
        toolUse::ToolCall call = std::move(chain[idx].second);
        size_t slot = chain[idx].first;
        executeOneToolCall(std::move(call),
            [this, chain = std::move(chain), st, idx, slot](toolUse::ToolResult r) mutable {
                st->results[slot] = std::move(r);
                if (--st->remaining == 0) {
                    st->onAllDone(std::move(st->results));
                    return;
                }
                this->runToolChain(std::move(chain), st, idx + 1);
            }
        );
    }

    void executeToolCalls(std::vector<toolUse::ToolCall> pending,
                          std::vector<toolUse::ToolResult> accumulated,
                          std::function<void(std::vector<toolUse::ToolResult>)> onAllDone)
    {
        if (pending.empty()) {
            onAllDone(std::move(accumulated));
            return;
        }
        auto st = std::make_shared<ToolBatchState>();
        st->results.resize(pending.size());
        st->remaining = pending.size();
        st->onAllDone =
            [accumulated = std::move(accumulated),
             onAllDone = std::move(onAllDone)](std::vector<toolUse::ToolResult> results) mutable {
                for (auto& r : results) accumulated.push_back(std::move(r));
                onAllDone(std::move(accumulated));
            };

        std::array<std::vector<std::pair<size_t, toolUse::ToolCall>>, 4> chains;
        for (size_t i = 0; i < pending.size(); ++i)
            chains[toolHolderCategory(pending[i].name)].emplace_back(i, std::move(pending[i]));
        for (auto& chain : chains)
            if (!chain.empty()) this->runToolChain(std::move(chain), st, 0);
    }

    // One-shot completion against the user's configured SECOND provider —
    // powers ask_subagent. Minimal request bodies (no tools, no history):
    // OpenAI-compat family + ollama generate + claude + gemini.
    void fireSubagentCompletion(const std::string& provider,
                                const std::string& question,
                                std::function<void(std::string)> onDone)
    {
        std::string model = Mod::get()->getSettingValue<std::string>("subagent-model");
        if (model.empty()) model = getProviderModel(provider);
        std::string apiKey = trimKey(getProviderApiKey(provider));
        static const char* SUB_SYS =
            "You are a concise expert consultant for a Geometry Dash level-design "
            "AI. Answer the question directly in under 250 words. No preamble.";

        matjson::Value body = matjson::Value::object();
        std::string url;
        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");
        request.timeout(std::chrono::seconds(90));

        if (provider == "claude") {
            auto msg = matjson::Value::object();
            msg["role"] = "user";
            msg["content"] = question;
            body["model"] = model;
            body["max_tokens"] = 1024;
            body["system"] = SUB_SYS;
            body["messages"] = std::vector<matjson::Value>{msg};
            url = "https://api.anthropic.com/v1/messages";
        } else if (provider == "gemini") {
            auto part = matjson::Value::object();
            part["text"] = question;
            auto content = matjson::Value::object();
            auto parts = matjson::Value::array();
            parts.push(part);
            content["parts"] = parts;
            auto contents = matjson::Value::array();
            contents.push(content);
            body["contents"] = contents;
            auto sysPart = matjson::Value::object();
            sysPart["text"] = SUB_SYS;
            auto sysParts = matjson::Value::array();
            sysParts.push(sysPart);
            auto sysInst = matjson::Value::object();
            sysInst["parts"] = sysParts;
            body["systemInstruction"] = sysInst;
            url = fmt::format(
                "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent",
                model);
        } else if (provider == "ollama") {
            body["model"] = model;
            body["prompt"] = fmt::format("{}\n\n{}", SUB_SYS, question);
            body["stream"] = false;
            url = getOllamaUrl() + "/api/generate";
        } else {
            // OpenAI-compatible family (openai/openrouter/ministral/hf/
            // deepseek/lm-studio/llama-cpp).
            auto sys = matjson::Value::object();
            sys["role"] = "system";  sys["content"] = SUB_SYS;
            auto usr = matjson::Value::object();
            usr["role"] = "user";    usr["content"] = question;
            auto msgs = matjson::Value::array();
            msgs.push(sys); msgs.push(usr);
            body["model"] = model;
            body["messages"] = msgs;
            body["max_tokens"] = 1024;
            url = toolUse::urlFor(provider, model);
        }
        applyProviderAuth(request, provider, apiKey);
        request.bodyString(body.dump());
        log::info("ask_subagent -> {} ({})", provider, model);
        m_subagentTask.spawn(
            request.post(url),
            [provider, onDone = std::move(onDone)](web::WebResponse resp) mutable {
                if (!resp.ok()) {
                    onDone(fmt::format("(subagent HTTP {})", resp.code()));
                    return;
                }
                auto json = resp.json();
                if (!json) { onDone("(subagent returned non-JSON)"); return; }
                const auto j = json.unwrap();
                std::string text;
                if (provider == "claude") {
                    if (j.contains("content") && j["content"].isArray() && j["content"].size() > 0) {
                        auto t = j["content"][0]["text"].asString();
                        if (t) text = t.unwrap();
                    }
                } else if (provider == "gemini") {
                    if (j.contains("candidates") && j["candidates"].isArray() &&
                        j["candidates"].size() > 0) {
                        const auto& cand = j["candidates"][0];
                        if (cand.contains("content") && cand["content"].contains("parts") &&
                            cand["content"]["parts"].isArray()) {
                            // All text parts — a leading thought part would
                            // otherwise make parts[0]["text"] miss.
                            const auto& parts = cand["content"]["parts"];
                            for (size_t pi = 0; pi < parts.size(); ++pi)
                                if (parts[pi].contains("text"))
                                    if (auto t = parts[pi]["text"].asString())
                                        text += t.unwrap();
                        }
                    }
                } else if (provider == "ollama") {
                    auto t = j["response"].asString();
                    if (t) text = t.unwrap();
                } else {
                    if (j.contains("choices") && j["choices"].isArray() && j["choices"].size() > 0) {
                        auto t = j["choices"][0]["message"]["content"].asString();
                        if (t) text = t.unwrap();
                    }
                }
                onDone(std::move(text));
            });
    }

    // Run one tool call. Each known tool either does a synchronous mod-side
    // lookup (e.g. analyze_level) or hands off to a fireFetchX async path
    // and forwards the result to the callback.
    void executeOneToolCall(toolUse::ToolCall call,
                            std::function<void(toolUse::ToolResult)> onDone)
    {
        toolUse::ToolResult r;
        r.toolCallId = call.id;
        log::info("→ Tool call: {} args={}", call.name, call.args.dump());
        pushSession(GenSession::Entry::Kind::ToolCall,
                    fmt::format("{} {}", call.name, call.args.dump()));
        // Wrap onDone so every result also lands in the transcript.
        onDone = [this, inner = std::move(onDone)](toolUse::ToolResult tr) mutable {
            pushSession(GenSession::Entry::Kind::ToolResult, tr.content);
            inner(std::move(tr));
        };

        // ── Redundant-call guard ───────────────────────────────────────────
        // Tool use is UNBOUNDED — the AI may call any tool as many times as it
        // likes. There are NO per-tool "you've called this N times, stop" caps
        // (those wrongly told the model to stop mid-task). The ONLY guard is
        // an exact-duplicate check on the deterministic DISCOVERY tools: a
        // repeat of the SAME query/ID returns the prior result note instead of
        // re-running the network fetch — pure efficiency, never a stop signal,
        // and it does not limit varied tool use. State-reading tools
        // (analyze_level / get_level_length / check_passability /
        // get_level_region) are NOT guarded at all — their output changes as
        // the level evolves, so honest repeats are expected. The 200-round
        // backstop in doToolRound is the sole anti-runaway catch.
        {
            static const std::unordered_set<std::string> DISCOVERY = {
                "search_objects", "web_search", "download_level",
                "search_newgrounds", "get_newgrounds_song",
            };
            if (DISCOVERY.count(call.name)) {
                std::string sig = call.name + "|" + call.args.dump();
                if (++m_toolCallSigCounts[sig] > 1) {
                    log::info("Repeat tool call '{}' (same args) — returning the "
                              "cached-result note instead of re-fetching", call.name);
                    r.content = fmt::format(
                        "(You already called {} with these exact arguments earlier "
                        "this generation — its result is in the conversation above. "
                        "Reuse it; no need to re-run the same query. Call it again "
                        "only with DIFFERENT arguments.)", call.name);
                    onDone(std::move(r));
                    return;
                }
            }
        }

        if (call.name == "analyze_level") {
            // Build a richer-than-buildLevelDataJson summary by also pulling
            // basic level metadata (name, song ID) when available.
            std::string body = this->buildLevelDataJson();
            if (m_editorLayer && m_editorLayer->m_level) {
                auto* lvl = m_editorLayer->m_level;
                body = fmt::format(
                    "{{\"level_name\":\"{}\",\"song_id\":{},"
                    "\"audio_track\":{},\"objects_json\":{}}}",
                    std::string(lvl->m_levelName),
                    (int)lvl->m_songID, (int)lvl->m_audioTrack,
                    body);
            }
            r.content = body;
            onDone(std::move(r));
            return;
        }
        if (call.name == "search_objects") {
            auto q = call.args["query"].asString();
            std::string query = q ? q.unwrap() : "";
            for (auto& c : query) c = (char)std::tolower((unsigned char)c);
            if (query.empty()) {
                r.content = "(search_objects needs a non-empty query substring)";
                r.isError = true;
                onDone(std::move(r));
                return;
            }
            // The catalog holds several aliases per ID (obj_-stripped names,
            // legacy spellings). Dedupe by ID, keeping the shortest name, so
            // 30 result slots mean 30 distinct objects instead of ~15.
            std::unordered_map<int, std::string> byId;
            for (auto& [name, id] : OBJECT_IDS) {
                if (name.find(query) == std::string::npos) continue;
                // prepareObjects drops particle_ objects — don't return names
                // the model can never place.
                if (name.rfind("particle_", 0) == 0) continue;
                auto it = byId.find(id);
                if (it == byId.end() || name.size() < it->second.size()
                    || (name.size() == it->second.size() && name < it->second))
                    byId[id] = name;
            }
            std::vector<std::string> hits;
            hits.reserve(byId.size());
            for (auto& [id, name] : byId) hits.push_back(name);
            std::sort(hits.begin(), hits.end(),
                [](const std::string& a, const std::string& b) {
                    return a.size() != b.size() ? a.size() < b.size() : a < b;
                });
            if (hits.size() > 30) hits.resize(30);
            if (hits.empty()) {
                r.content = fmt::format(
                    "(no object names contain \"{}\" — try a shorter substring)", query);
            } else {
                std::string out = fmt::format("{} object types matching \"{}\": ",
                                              hits.size(), query);
                for (size_t i = 0; i < hits.size(); ++i) {
                    if (i) out += ", ";
                    out += hits[i];
                }
                r.content = std::move(out);
            }
            onDone(std::move(r));
            return;
        }
        if (call.name == "ask_subagent") {
            std::string subProvider =
                Mod::get()->getSettingValue<std::string>("subagent-provider");
            auto q = call.args["question"].asString();
            std::string question = q ? q.unwrap() : "";
            if (question.size() > 1500) question.resize(1500);
            if (subProvider.empty() || question.empty()) {
                r.content = subProvider.empty()
                    ? "(no subagent configured - the user must set Subagent "
                      "Provider in settings. Continue without it.)"
                    : "(ask_subagent needs a non-empty question)";
                r.isError = question.empty();
                onDone(std::move(r));
                return;
            }
            this->fireSubagentCompletion(subProvider, question,
                [r, onDone = std::move(onDone)](std::string answer) mutable {
                    if (answer.size() > 4000) { answer.resize(4000); answer += "..."; }
                    r.content = answer.empty()
                        ? "(subagent did not answer - continue without it)"
                        : fmt::format("Subagent answer:\n{}", answer);
                    onDone(std::move(r));
                });
            return;
        }
        if (call.name == "simulate_physics") {
            if (m_accumulatedObjects.size() == 0) {
                r.content = "(no accepted draft yet - emit your first draft, then "
                            "call this during EXTEND rounds.)";
            } else {
                auto sim = levelcheck::simulateCube(m_accumulatedObjects,
                    (float)Mod::get()->getSettingValue<int64_t>("ai-ground-y"));
                if (sim.deaths.empty()) {
                    r.content = fmt::format(
                        "Physics bot CLEARED the draft (reached X={:.0f}{}). "
                        "Cube sections look jumpable.",
                        sim.reachedX, sim.finished ? ", end of level" : "");
                } else {
                    std::string body = fmt::format(
                        "Physics bot died {} time(s):\n", sim.deaths.size());
                    for (auto& d : sim.deaths)
                        body += fmt::format("  X={:.0f} Y={:.0f}: {}\n", d.x, d.y, d.reason);
                    body += "Fix these spots (wider spacing, lower obstacles, or an "
                            "orb/pad assist) before finalizing.";
                    r.content = std::move(body);
                }
            }
            onDone(std::move(r));
            return;
        }
        if (call.name == "analyze_difficulty_curve") {
            if (m_accumulatedObjects.size() == 0) {
                r.content = "(no accepted draft yet — emit your first draft, then "
                            "call this during EXTEND rounds.)";
            } else {
                auto hist = levelcheck::difficultyHistogram(m_accumulatedObjects);
                if (hist.empty()) {
                    r.content = "(draft has no measurable span yet)";
                } else {
                    float mean = 0.f;
                    for (auto& w : hist) mean += w.density;
                    mean /= (float)hist.size();
                    std::string body = fmt::format(
                        "Hazard density per 1000u across {} windows (mean {:.1f}):\n",
                        hist.size(), mean);
                    for (auto& w : hist) {
                        body += fmt::format("  X {:>6.0f}-{:<6.0f}: {:.1f}", w.x0, w.x1, w.density);
                        if (mean > 0.f && w.density > 2.f * mean)
                            body += "  <- SPIKE (much harder than neighbors)";
                        else if (mean > 0.f && w.density < 0.25f * mean)
                            body += "  <- FLAT (consider an obstacle or two)";
                        body += "\n";
                    }
                    r.content = std::move(body);
                }
            }
            onDone(std::move(r));
            return;
        }
        if (call.name == "check_passability") {
            if (m_accumulatedObjects.size() == 0) {
                r.content = "(no accepted draft yet — this tool only sees objects from "
                            "your previous JSON answers. Emit your first draft, then "
                            "call this during EXTEND rounds.)";
            } else {
                auto pass = levelcheck::check(m_accumulatedObjects);
                std::string zones;
                for (size_t k = 0; k < pass.deaths.size() && k < 8; ++k) {
                    if (k) zones += ", ";
                    zones += fmt::format("X={:.0f}-{:.0f}",
                                         pass.deaths[k].x_start, pass.deaths[k].x_end);
                }
                r.content = fmt::format(
                    "Passability: {:.1f}% across {} columns. {}{}",
                    pass.pass_rate * 100.f, pass.total_columns,
                    pass.deaths.empty()
                        ? "No death zones — clear to finalize."
                        : fmt::format("{} death zone(s) that MUST be fixed before the "
                                      "final answer: ", pass.deaths.size()),
                    zones);
            }
            onDone(std::move(r));
            return;
        }
        if (call.name == "get_level_region") {
            auto x0r = call.args["x_start"].asInt();
            auto x1r = call.args["x_end"].asInt();
            if (!x0r || !x1r) {
                r.content = "(get_level_region needs integer x_start and x_end)";
                r.isError = true;
                onDone(std::move(r));
                return;
            }
            float x0 = (float)std::min(x0r.unwrap(), x1r.unwrap());
            float x1 = (float)std::max(x0r.unwrap(), x1r.unwrap());
            r.content = this->buildLevelRegionJson(x0, x1);
            onDone(std::move(r));
            return;
        }
        if (call.name == "web_search") {
            auto q = call.args["query"].asString();
            std::string query = q ? q.unwrap() : "";
            this->fireFetchWebSearch(query,
                [r, onDone = std::move(onDone)](const std::string& result) mutable {
                    r.content = result.empty() ? "(no results)" : result;
                    onDone(std::move(r));
                });
            return;
        }
        if (call.name == "download_level") {
            std::string idStr;
            auto idI = call.args["level_id"].asInt();
            if (idI) idStr = fmt::format("{}", idI.unwrap());
            else {
                auto idS = call.args["level_id"].asString();
                if (idS) idStr = idS.unwrap();
            }
            if (idStr.empty()) {
                r.content = "Missing or invalid level_id";
                r.isError = true;
                onDone(std::move(r));
                return;
            }
            this->fireFetchLevelByID(idStr,
                [r, onDone = std::move(onDone)](const std::string& result) mutable {
                    r.content = result.empty() ? "(level not found)" : result;
                    onDone(std::move(r));
                });
            return;
        }
        if (call.name == "search_newgrounds") {
            auto q = call.args["query"].asString();
            std::string query = q ? q.unwrap() : "";
            this->fireFetchNGSong(query,
                [r, onDone = std::move(onDone)](const std::string& result) mutable {
                    r.content = result.empty() ? "(no song found)" : result;
                    onDone(std::move(r));
                });
            return;
        }
        if (call.name == "get_newgrounds_song") {
            auto idI = call.args["song_id"].asInt();
            if (!idI) {
                r.content = "Missing or invalid song_id";
                r.isError = true;
                onDone(std::move(r));
                return;
            }
            this->fetchNGSongByID((int)idI.unwrap(),
                [r, onDone = std::move(onDone)](const std::string& result) mutable {
                    r.content = result.empty() ? "(song not found)" : result;
                    onDone(std::move(r));
                });
            return;
        }
        if (call.name == "get_level_length") {
            // Sum two sources of "what's already in the level":
            //   1. The editor's existing objects (m_editorLayer->m_objects)
            //   2. Anything we've already accumulated this generation
            //      (m_accumulatedObjects) — relevant during extension rounds
            //      so the AI sees the running total, not just the editor.
            float editorMaxX = 0.f;
            int editorObjectCount = 0;
            // revalidate: the editor may have been freed during this unbounded
            // loop — iterating a dead m_objects is a use-after-free.
            if (revalidateEditor() && m_editorLayer->m_objects) {
                for (auto* raw : CCArrayExt<CCObject*>(m_editorLayer->m_objects)) {
                    auto* obj = typeinfo_cast<GameObject*>(raw);
                    if (!obj) continue;
                    float x = obj->getPositionX();
                    if (x > editorMaxX) editorMaxX = x;
                    ++editorObjectCount;
                }
            }
            float accumMaxX = computeMaxXFromObjects(m_accumulatedObjects);
            float maxX = std::max(editorMaxX, accumMaxX);
            auto  [secs, cat] = describeLengthByX(maxX);
            float targetMinX  = m_lengthTarget.minSeconds * GD_PLAYER_SPEED_1X;
            float targetMaxX  = m_lengthTarget.maxSeconds * GD_PLAYER_SPEED_1X;

            r.content = fmt::format(
                "{{\"current_seconds\":{:.2f},\"current_max_x\":{:.0f},"
                "\"category_now\":\"{}\",\"target_label\":\"{}\","
                "\"target_min_seconds\":{:.0f},\"target_max_seconds\":{:.0f},"
                "\"target_min_x\":{:.0f},\"target_max_x\":{:.0f},"
                "\"editor_object_count\":{},\"accumulated_object_count\":{},"
                "\"keep_building_from_x\":{:.0f},"
                "\"is_satisfied\":{},"
                "\"player_speed_1x_units_per_sec\":{:.2f}}}",
                secs, maxX, cat, m_lengthTarget.label,
                m_lengthTarget.minSeconds, m_lengthTarget.maxSeconds,
                targetMinX, targetMaxX,
                editorObjectCount, (int)m_accumulatedObjects.size(),
                maxX,  // "keep building from" = current furthest object
                (secs >= m_lengthTarget.minSeconds) ? "true" : "false",
                GD_PLAYER_SPEED_1X
            );
            log::info("get_level_length: {:.2f}s ({} cat); target {}-{}s",
                      secs, cat, m_lengthTarget.minSeconds, m_lengthTarget.maxSeconds);
            onDone(std::move(r));
            return;
        }

        // Unknown tool — explicit error so the model knows to stop.
        r.content = fmt::format("Unknown tool '{}'. Use only: web_search, "
                                "download_level, search_newgrounds, "
                                "get_newgrounds_song, analyze_level, "
                                "get_level_length, search_objects, "
                                "check_passability, analyze_difficulty_curve, "
                                "simulate_physics, get_level_region. To reason, "
                                "write plain text before your tool calls.",
                                call.name);
        r.isError = true;
        onDone(std::move(r));
    }

    // Shared "generation finished (one way or another)" UI reset: hide the
    // cancel button, restore Generate.
    void resetGenerationUI() {
        m_isGenerating = false;
        m_cancelBtn->setVisible(false);
        m_generateBtn->setVisible(true);
        if (!m_isCreatingObjects) m_generateBtn->setEnabled(true);
    }

    // ── processFinalResponse ──────────────────────────────────────────────
    // The portion of onAPISuccess after aiResponse is in hand. Factored out so
    // the tool-use loop can call it once its loop completes.
    void processFinalResponse(std::string aiResponse, const std::string& provider) {
        resetGenerationUI();
        // Self-critique replies may legitimately contain no level content
        // ("ALL GOOD") — that must fall through to apply, not error out.
        bool wasCritiqueReply = m_critiquePending;
        m_critiquePending = false;
        if (wasCritiqueReply) {
            // Surface the AI's self-rating ("RATING: n/10") in the session.
            // Line-start matches only — "# RATING: ..." inside the fix
            // script must not masquerade as the verdict.
            auto rp = aiResponse.find("RATING:");
            if (rp != std::string::npos &&
                !(rp == 0 || aiResponse[rp - 1] == '\n'))
                rp = std::string::npos;
            if (rp != std::string::npos) {
                size_t p = rp + 7;
                while (p < aiResponse.size() && aiResponse[p] == ' ') ++p;
                int val = 0; bool any = false;
                while (p < aiResponse.size() &&
                       aiResponse[p] >= '0' && aiResponse[p] <= '9') {
                    val = val * 10 + (aiResponse[p] - '0');
                    ++p; any = true;
                }
                if (any) {
                    val = std::clamp(val, 1, 10);
                    s_lastSelfRating = val;   // rides along with telemetry
                    pushSession(GenSession::Entry::Kind::Status,
                        fmt::format("AI self-rated this level {}/10{}", val,
                                    val >= 8 ? "" : " - applying its own fixes"));
                    log::info("Self-review rating: {}/10", val);
                }
            }
        }

        // Log + sanitize the response.
        log::info("=== Full AI Response from {} ===", provider);
        logLong("AIResponse", aiResponse);
        log::info("=== End AI Response ===");

        // Local reasoning models (Ollama DeepSeek distills, QwQ, ...) inline
        // their chain of thought as <think>...</think>. Surface it in the
        // session transcript like any other thinking, then strip it so the
        // EAS/JSON extractors never see it.
        {
            size_t tp;
            while ((tp = aiResponse.find("<think>")) != std::string::npos) {
                // Find the close tag MATCHING this open — models sometimes
                // nest <think> blocks, and grabbing the first </think> would
                // leave an orphaned "C</think>" tail in the live content.
                size_t scan = tp + 7;
                int depth = 1;
                size_t te = std::string::npos;
                while (depth > 0) {
                    size_t no = aiResponse.find("<think>", scan);
                    size_t nc = aiResponse.find("</think>", scan);
                    if (nc == std::string::npos) break;       // unterminated
                    if (no != std::string::npos && no < nc) {
                        ++depth;
                        scan = no + 7;
                    } else {
                        --depth;
                        if (depth == 0) te = nc;
                        scan = nc + 8;
                    }
                }
                size_t bodyStart = tp + 7;
                std::string thought = te == std::string::npos
                    ? aiResponse.substr(bodyStart)
                    : aiResponse.substr(bodyStart, te - bodyStart);
                if (!thought.empty())
                    pushSession(GenSession::Entry::Kind::Thinking, thought);
                aiResponse.erase(tp, te == std::string::npos
                    ? std::string::npos : te + 8 - tp);
            }
        }

        {
            size_t pos = 0;
            while ((pos = aiResponse.find("```", pos)) != std::string::npos) {
                size_t end = pos + 3;
                while (end < aiResponse.size() && aiResponse[end] != '\n' && aiResponse[end] != '\r')
                    ++end;
                aiResponse.erase(pos, end - pos);
            }
        }

        // Record the assistant's reply in the conversation log — follow-up
        // turns need the model to see what it previously produced (the tool
        // loop never appends its final answer; single-shot never appended
        // anything). Capped so a giant level script can't bloat every later
        // request; the history pruner only shrinks ToolResults.
        {
            toolUse::Message asst;
            asst.role = toolUse::MessageRole::Assistant;
            asst.text = aiResponse.size() > 6000
                ? aiResponse.substr(0, 6000) + "\n[...response truncated...]"
                : aiResponse;
            // (Durable chat memory gets only the TURN's final answer — see
            // the terminal paths below. Mirroring every continuation round
            // here drowned real conversation turns in intermediate drafts.)
            m_toolHistory.push_back(std::move(asst));
        }

        // Capture the AI's planning narration (the prose before the script /
        // JSON block) — the "Why?" button on the preview bar surfaces it.
        // EAS path: text before "## Level Script". JSON path: text before
        // the first '{'. Trimmed and capped; empty when the model went
        // straight to output.
        {
            std::string narration;
            auto markPos = aiResponse.rfind("## Level Script");
            if (markPos != std::string::npos) {
                narration = aiResponse.substr(0, markPos);
            } else {
                auto brace = aiResponse.find('{');
                if (brace != std::string::npos && brace > 40)
                    narration = aiResponse.substr(0, brace);
            }
            // Trim whitespace; cap at ~900 chars on a sentence boundary.
            auto first = narration.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) narration.clear();
            else {
                narration.erase(0, first);
                while (!narration.empty() &&
                       (narration.back() == ' ' || narration.back() == '\n' ||
                        narration.back() == '\r' || narration.back() == '\t'))
                    narration.pop_back();
                if (narration.size() > 900) {
                    auto cut = narration.rfind(". ", 900);
                    narration.resize(cut != std::string::npos ? cut + 1 : 900);
                    // Never cut mid-codepoint: an orphaned UTF-8 continuation
                    // byte garbles the cocos label in the "Why?" popup.
                    while (!narration.empty() &&
                           ((unsigned char)narration.back() & 0xC0) == 0x80)
                        narration.pop_back();
                    if (!narration.empty() &&
                        (unsigned char)narration.back() >= 0xC0)
                        narration.pop_back();
                    narration += " [...]";
                }
            }
            s_lastAINarration = std::move(narration);
            if (!s_lastAINarration.empty())
                pushSession(GenSession::Entry::Kind::Assistant, s_lastAINarration);
        }

        // ── EAS auto-detect ─────────────────────────────────────────────
        // First, try the EditorAI Script (EAS) line-based format. EAS is the
        // PREFERRED format: more compact, harder for the AI to malform, and
        // forces metadata to be emitted via the META verb. If the response
        // contains "## Level Script" we use whatever follows; otherwise we
        // sniff the first non-comment line for an EAS verb.
        matjson::Value levelData;
        bool parsedAsEAS = false;
        {
            std::string scriptBody = eas::extractScript(aiResponse);
            if (eas::looksLikeEAS(scriptBody)) {
                auto er = eas::parse(scriptBody);
                if (er.ok) {
                    levelData = std::move(er.root);
                    parsedAsEAS = true;
                    log::info("Parsed response as EAS ({} chars script body)", scriptBody.size());
                }
            }
        }

        if (!parsedAsEAS) {
            // ── JSON fallback ───────────────────────────────────────────
            // Some models — especially smaller ones via Ollama — emit narration
            // around the JSON, or a draft JSON followed by a final one, or even
            // a stale tool-call payload. Scan brace-aware for the LAST balanced
            // JSON object that's shaped like an EAI response, prefer that.
            // Plan/Chat-mode (and plain conversational) replies contain no
            // script — on follow-up turns that's success, not an error.
            // The narration capture above only fires when a script marker
            // or late brace exists, so prose-only answers land here with
            // nothing in the transcript yet.
            auto finishProseTurn = [&]() -> bool {
                if (!m_followUpTurn) return false;
                if (s_lastAINarration.empty())
                    pushSession(GenSession::Entry::Kind::Assistant, aiResponse);
                if (m_session) m_session->chatPush(1, aiResponse);  // durable memory
                log::info("Follow-up turn answered with prose only");
                if (m_session) m_session->state = GenSession::State::Done;
                m_followUpTurn = false;
                showStatus("Answered", false);
                return true;
            };
            std::string jsonBlock = extractLastEAIJsonBlock(aiResponse);
            if (jsonBlock.empty()) {
                size_t s = aiResponse.find('{');
                size_t e = aiResponse.rfind('}');
                if (s == std::string::npos || e == std::string::npos || e < s) {
                    if (wasCritiqueReply) {
                        // "ALL GOOD" (or any prose) — accept the level as-is.
                        log::info("Self-critique: no changes requested by the model");
                        levelData = matjson::Value::object();
                        goto critiquePassThrough;
                    }
                    if (finishProseTurn()) return;
                    onError("Invalid Response",
                        fmt::format("The AI's reply didn't contain a valid level (no EAS verbs and "
                                    "no JSON object found). It probably refused or replied "
                                    "conversationally. Re-generate, or rephrase the prompt to be more "
                                    "concrete (\"make a 30s easy level with spikes\"). ({})",
                                    autoErrorCode(60, 5)));
                    return;
                }
                jsonBlock = aiResponse.substr(s, e - s + 1);
            }
            auto levelLenient = editorai::json_lenient::parse(jsonBlock);
            if (!levelLenient.ok) {
                if (wasCritiqueReply) {
                    log::info("Self-critique: reply unparseable — accepting the level as-is");
                    levelData = matjson::Value::object();
                    goto critiquePassThrough;
                }
                if (finishProseTurn()) return;  // prose with stray braces
                onError("Parse Error",
                    fmt::format("The AI's response wasn't valid EAS or JSON even after the parser tried "
                                "to fix trailing commas, missing brackets, and comments. Re-generate "
                                "(output is non-deterministic), or pick a smaller \"length\" setting "
                                "to keep the response from being truncated. ({})",
                                autoErrorCode(60, 6)));
                return;
            }
            levelData = std::move(levelLenient.value);
            if (levelLenient.fixesApplied > 0) {
                log::info("Level JSON: auto-fixed {} category(ies) of model mistakes", levelLenient.fixesApplied);
            }
        }

        critiquePassThrough:;

        // Tolerate responses with no objects + no macros — the AI may have
        // emitted only level_metadata (renaming the level, changing colors,
        // toggling platformer mode) without adding any geometry. That's a
        // valid metadata-only update. Same applies if macros expand to nothing.
        bool hasObjects = levelData.contains("objects")
                       && levelData["objects"].isArray()
                       && levelData["objects"].size() > 0;
        bool hasMacros  = levelData.contains("macros")
                       && levelData["macros"].isArray()
                       && levelData["macros"].size() > 0;
        bool hasMetadata = levelData.contains("level_metadata")
                       && levelData["level_metadata"].isObject();

        matjson::Value objectsArray = hasObjects
            ? levelData["objects"]
            : matjson::Value::array();

        resetBlockTemplates();

        if (hasMacros) {
            std::vector<matjson::Value> expanded;
            macros::expandAll(levelData["macros"], expanded);
            for (auto& obj : expanded) objectsArray.push(std::move(obj));
        }

        // Empty after expansion is allowed when metadata is present (the AI
        // can make level-wide changes without adding geometry). If neither
        // metadata nor objects/macros exist, log a warning and continue with
        // an empty array — applyResult below will short-circuit cleanly and
        // the user just sees "nothing applied" instead of a hard error.
        if (objectsArray.size() == 0 && !hasMetadata) {
            if (m_followUpTurn) {
                // Conversational answer with no level changes — perfectly
                // valid for a follow-up turn. The transcript already carries
                // the assistant text; nothing to stage.
                log::info("Follow-up turn answered without level changes");
                if (m_session) {
                    m_session->chatPush(1, aiResponse);  // durable memory
                    m_session->state = GenSession::State::Done;
                }
                m_followUpTurn = false;
                m_isGenerating = false;
                showStatus("Answered (no level changes)", false);
                return;
            }
            if (m_accumulatedObjects.size() > 0) {
                // A zero-add round atop earlier rounds (critique "ALL GOOD",
                // a refinement that found nothing) is success, not a failure
                // worth a scary notification.
                log::info("Round added nothing; applying the {} accumulated "
                          "objects", (int)m_accumulatedObjects.size());
            } else {
                log::warn("AI response had no objects, macros, OR level_metadata — "
                          "applying nothing");
                Notification::create(
                    "AI returned no objects and no metadata changes. Try re-generating.",
                    NotificationIcon::Warning, 3.f
                )->show();
            }
            // Fall through — the apply path handles empty arrays gracefully
        } else if (objectsArray.size() == 0) {
            log::info("AI response had no objects but level_metadata present — "
                      "applying metadata only");
        }

        // Append this round's objects to the running accumulator. For the
        // single-shot path m_accumulatedObjects starts empty, so the result
        // is just this response. For the tool-loop path, this stacks each
        // extension's contribution on top of earlier rounds' so the FINAL
        // apply uses every object the AI produced across the whole loop.
        // Move — matjson copies are deep and objectsArray is local; only its
        // size is read after this loop.
        size_t roundAdded = objectsArray.size();
        for (size_t i = 0; i < roundAdded; ++i) {
            m_accumulatedObjects.push(std::move(objectsArray[i]));
        }
        log::info("Accumulator now has {} objects (this round added {})",
                  (int)m_accumulatedObjects.size(), (int)roundAdded);

        auto analysisResult = levelData["analysis"].asString();
        if (analysisResult) log::info("AI Analysis: {}", analysisResult.unwrap());

        // ── Length enforcement (tool-loop only) ─────────────────────────
        // If the user's "length" setting demands more seconds than the
        // running accumulator currently provides AND we have a tool-loop
        // history (i.e. we're not on the single-shot/custom path AND not in
        // edit mode), inject an "extend further" user message and run
        // another tool round. Capped by m_maxExtensionRounds so a stubborn
        // model can't hang forever.
        // Loop-back eligibility: only a turn that genuinely ran through
        // doToolRound may loop back into it (m_usingToolLoop). Mutation and
        // co-op turns never loop — copilot fixes and "change this" mutations
        // are deliberately bounded single answers, and length/density gates
        // firing on their few-object deltas would bulldoze the user's level
        // with unsolicited extensions.
        bool inToolLoop = m_usingToolLoop && !m_editMode
                       && !m_mutationMode && !m_coopMode;
        if (inToolLoop) {
            float currentMaxX = computeMaxXFromObjects(m_accumulatedObjects);
            auto [curSecs, curCat] = describeLengthByX(currentMaxX);
            float targetSecs       = m_lengthTarget.minSeconds;
            float targetMinX       = targetSecs * GD_PLAYER_SPEED_1X;

            if (!m_followUpTurn && curSecs < targetSecs && m_extensionRounds < m_maxExtensionRounds) {
                ++m_extensionRounds;
                log::info("Length {}/{}s short of {}-{}s target. Extension {} / {}.",
                          curSecs, (int)currentMaxX,
                          m_lengthTarget.minSeconds, m_lengthTarget.maxSeconds,
                          m_extensionRounds, m_maxExtensionRounds);

                // (assistant turn already recorded once, unconditionally,
                // near the top of processFinalResponse)

                toolUse::Message more;
                more.role = toolUse::MessageRole::User;
                more.text = fmt::format(
                    "Only {:.1f}s long (maxX={:.0f}). Target {}: {:.0f}-{:.0f}s, "
                    "maxX≈{:.0f}-{:.0f}. EXTEND from X={:.0f} rightward — same format "
                    "as your last reply (EAS or JSON), additional objects/macros only, "
                    "do not restart or re-emit prior objects. COPY from=x0..x1 "
                    "offset=DX and MIRROR can duplicate earlier sections in one "
                    "line — use them for repeats/variations instead of re-writing "
                    "objects. Round {}/{}.",
                    curSecs, currentMaxX,
                    m_lengthTarget.label,
                    m_lengthTarget.minSeconds, m_lengthTarget.maxSeconds,
                    targetMinX, m_lengthTarget.maxSeconds * GD_PLAYER_SPEED_1X,
                    currentMaxX,
                    m_extensionRounds, m_maxExtensionRounds);
                more.imageB64 = visionSnapshotIfSupported();
                m_toolHistory.push_back(std::move(more));

                showStatus(fmt::format("Length {}/{}s — asking for more (round {})",
                                       (int)curSecs, (int)targetSecs,
                                       m_extensionRounds));
                this->doToolRound();
                return;
            }

            if (curSecs < targetSecs) {
                log::warn("Length still short ({:.1f}s < {:.0f}s target) after {} "
                          "extension rounds — applying what we have.",
                          curSecs, targetSecs, m_extensionRounds);
                Notification::create(
                    fmt::format("Level is {:.0f}s (target {:.0f}s) — extensions exhausted.",
                                curSecs, targetSecs),
                    NotificationIcon::Warning)->show();
            } else {
                log::info("Length OK: {:.1f}s ≥ {:.0f}s target.",
                          curSecs, targetSecs);
            }
        }

        // ── Target-object-count enforcement ─────────────────────────────
        // The "target-object-count" generation option: the level must reach
        // at least N total objects; too few and the model is asked to keep
        // building (densify + decorate, not just lengthen). Generation only
        // (not edits/follow-ups), capped at 12 continuation rounds.
        {
            int targetObjs = (int)Mod::get()->getSettingValue<int64_t>("target-object-count");
            int maxObjs    = (int)Mod::get()->getSettingValue<int64_t>("max-objects");
            targetObjs = std::min(targetObjs, maxObjs);
            if (targetObjs > 0 && inToolLoop && !m_followUpTurn
                && m_targetObjRounds < 12) {
                int placed = 0;
                for (size_t i = 0; i < m_accumulatedObjects.size(); ++i) {
                    const auto& e = m_accumulatedObjects[i];
                    if (e.isObject() && !e.contains("op")) ++placed;
                }
                if (placed < targetObjs) {
                    ++m_targetObjRounds;
                    log::info("Object count {}/{} below target - continuation "
                              "round {}/12", placed, targetObjs, m_targetObjRounds);
                    toolUse::Message more;
                    more.role = toolUse::MessageRole::User;
                    more.text = fmt::format(
                        "The level has {} objects; the user requires at least "
                        "{}. CONTINUE building: densify weak sections, layer "
                        "decoration (background silhouettes, ground detail, "
                        "structure dressing), and extend gameplay where it "
                        "helps - do not re-emit existing objects. Use macros "
                        "and COPY/MIRROR for bulk. Add at least {} objects "
                        "this round. Round {}/12.",
                        placed, targetObjs,
                        std::min(targetObjs - placed, 800),
                        m_targetObjRounds);
                    more.imageB64 = visionSnapshotIfSupported();
                    m_toolHistory.push_back(std::move(more));
                    showStatus(fmt::format("Objects {}/{} — asking for more "
                                           "(round {})", placed, targetObjs,
                                           m_targetObjRounds));
                    this->doToolRound();
                    return;
                }
                log::info("Object count OK: {} ≥ {} target", placed, targetObjs);
            }
        }

        // ── Passability check ──────────────────────────────────────────
        // Run the fly-anywhere pathfinder on the about-to-apply objects.
        // If < 95 % passable, in the tool loop: inject a "fix these death
        // zones" message and re-prompt the model. In single-shot: surface
        // a warning popup with a "Re-generate" button.
        //
        // The check skips itself when:
        //   - extension rounds aren't available (single-shot/custom path
        //     where we can't loop back) — falls through to popup
        //   - the user already exceeded the max correction rounds (avoid
        //     hanging if the model can't fix it)
        //   - the level is in EDIT mode (player is iterating on an existing
        //     level; bad-collision warnings would be noisy and incorrect
        //     because we don't have the existing level's geometry mapped in)
        constexpr float PASS_THRESHOLD = 0.95f;
        constexpr int MAX_PASSABILITY_FIXES = 2;
        // Check the accumulator directly — the apply snapshot is only taken
        // after every early-return round below, so no copies are wasted on
        // intermediate passability/refinement rounds.
        auto passResult = levelcheck::check(m_accumulatedObjects);
        log::info("Passability: {}", passResult.summary);

        // Follow-up turns are skipped outright: the accumulator holds only
        // this turn's delta, so "passability" against it is meaningless and
        // its warnings would be noise.
        if (passResult.pass_rate < PASS_THRESHOLD && !m_editMode && !m_mutationMode
            && !m_followUpTurn) {
            bool canLoopBack = m_usingToolLoop && !m_followUpTurn
                            && m_passabilityFixRounds < MAX_PASSABILITY_FIXES;
            if (canLoopBack) {
                ++m_passabilityFixRounds;
                log::warn("Level only {:.1f}% passable. Asking model to fix "
                          "(round {}/{}).",
                          passResult.pass_rate * 100.f,
                          m_passabilityFixRounds, MAX_PASSABILITY_FIXES);

                // (assistant turn already recorded once, unconditionally,
                // near the top of processFinalResponse)

                std::string deathList;
                for (size_t k = 0; k < passResult.deaths.size() && k < 8; ++k) {
                    if (k) deathList += ", ";
                    deathList += fmt::format("X={:.0f}-{:.0f}",
                        passResult.deaths[k].x_start,
                        passResult.deaths[k].x_end);
                }
                if (passResult.deaths.size() > 8)
                    deathList += fmt::format(" (+{} more zones)",
                        passResult.deaths.size() - 8);

                toolUse::Message fix;
                fix.role = toolUse::MessageRole::User;
                fix.text = fmt::format(
                    "The level you produced has unpassable sections — only "
                    "{:.1f}% of columns have an open vertical path (target: "
                    "≥95%). A column is \"dead\" if it has a solid block "
                    "or hazard at EVERY Y from 15 to 540 with no airspace to "
                    "fly through. Death zones: {}. "
                    "FIX THE LEVEL: remove or reposition objects in those X "
                    "ranges so the player can fly through. Either re-emit the "
                    "WHOLE level with those zones unblocked, OR emit just the "
                    "ADDITIONAL objects/macros needed to clear the path (the "
                    "mod accumulates). DO NOT add more obstacles in the dead "
                    "zones — open them up. Fix round {} of {}.",
                    passResult.pass_rate * 100.f, deathList,
                    m_passabilityFixRounds, MAX_PASSABILITY_FIXES);
                fix.imageB64 = visionSnapshotIfSupported();
                m_toolHistory.push_back(std::move(fix));
                showStatus(fmt::format("Fixing impassable level (round {})...",
                    m_passabilityFixRounds));
                this->doToolRound();
                return;
            }
            // Out of fix rounds or no loop available — warn the user but
            // still apply (they may be able to repair manually).
            Notification::create(
                fmt::format("Level is only {:.1f}% passable — applying anyway. "
                            "{} death zone(s) flagged in the log.",
                            passResult.pass_rate * 100.f, passResult.deaths.size()),
                NotificationIcon::Warning, 5.f
            )->show();
        }

        // ── Edit-workload enforcement ───────────────────────────────────
        // Edits must be REAL reworks: the planned edit count (resolved ops +
        // new objects) has to reach the "edit-target-ops" setting before the
        // result stages. Below target → the model is told to continue. A
        // prose-only or zero-content turn never reaches this gate, so plain
        // questions in edit mode still get plain answers.
        {
            int editTarget = (int)Mod::get()->getSettingValue<int64_t>("edit-target-ops");
            // m_editMode can linger from a previous resumed edit turn — on
            // follow-ups the MODE of this turn decides, not the stale flag.
            bool editTurn  = m_followUpTurn ? m_followUpMode == 0 : m_editMode;
            bool canLoop   = m_usingToolLoop && !m_mutationMode && !m_coopMode;
            if (editTarget > 0 && editTurn && canLoop && m_editEnforceRounds < 8) {
                int planned = countPlannedEdits();
                // Surgical follow-ups stay surgical: a small targeted tweak
                // ("delete the spike at x=300", < 30 edits) stages as-is —
                // the workload target is for REWORKS, where the model is
                // already attempting scale and must not stop early. Edit-
                // mode generations (the "rework this level" path) always
                // enforce.
                bool surgical = m_followUpTurn && planned < 30;
                if (planned > 0 && planned < editTarget && !surgical) {
                    ++m_editEnforceRounds;
                    log::info("Edit workload {}/{} below target - continuation "
                              "round {}/8", planned, editTarget, m_editEnforceRounds);
                    toolUse::Message more;
                    more.role = toolUse::MessageRole::User;
                    more.text = fmt::format(
                        "Your rework currently totals {} edits; the target is "
                        "at least {}. CONTINUE the rework now - this is a "
                        "demand for breadth, not busywork: MOVE whole regions "
                        "to fix pacing (rect selectors), DELETE weak or "
                        "cluttered stretches, EDIT colors/scale across "
                        "sections, and ADD new geometry and decoration where "
                        "the level is thin. Bulk selectors count every object "
                        "they touch. Do not undo or repeat earlier ops. "
                        "Round {}/8.",
                        planned, editTarget, m_editEnforceRounds);
                    more.imageB64 = visionSnapshotIfSupported();
                    m_toolHistory.push_back(std::move(more));
                    showStatus(fmt::format("Edit workload {}/{} — asking for "
                                           "more (round {})", planned,
                                           editTarget, m_editEnforceRounds));
                    this->doToolRound();
                    return;
                }
                if (planned >= editTarget)
                    log::info("Edit workload OK: {} ≥ {} target", planned, editTarget);
            }
        }

        // ── Refinement loop ────────────────────────────────────────────
        // After the level passes the length + passability checks, optionally
        // bounce the AI N more times asking it to look at its own work and
        // polish it. The setting "refinement-rounds" controls how many
        // (default 3, range 0–10, 0 disables entirely).
        //
        // Each pass asks the AI for a SMALL incremental polish — accent
        // objects, better pacing, color cohesion, difficulty curve. Not a
        // rebuild. The accumulator stacks each pass on top so all earlier
        // work is preserved.
        //
        // Skipped when: not in a tool-capable loop, in edit mode, or rounds
        // are exhausted. Also skipped if the user set the count to 0.
        {
            int maxRefine = (int)Mod::get()->getSettingValue<int64_t>("refinement-rounds");
            bool canRefine = !m_followUpTurn
                          && m_usingToolLoop
                          && !m_mutationMode && !m_coopMode
                          && !m_editMode
                          && maxRefine > 0
                          && m_refinementRounds < maxRefine;
            if (canRefine) {
                ++m_refinementRounds;
                log::info("Refinement pass {}/{}", m_refinementRounds, maxRefine);

                // Rotating focus per round so the AI doesn't repeat the same
                // type of polish three times. We cycle through 5 angles and
                // the model gets a different one each call.
                static const std::vector<const char*> REFINEMENT_FOCUSES = {
                    "PACING + OBSTACLE VARIETY: scan for runs of identical objects "
                    "(e.g. 5 spike-trains in a row) and replace some with orbs, "
                    "pads, stairs, or platform sections. Fix any spacing < 60 "
                    "units between obstacles (too cramped) or > 300 units (boring "
                    "gap).",

                    "VISUAL COHESION: add decoration objects (gears, blades, "
                    "decorative blocks) sparsely to make the level feel built, "
                    "not random. Ensure color channels are used consistently — "
                    "if the level has a color palette, route blocks/spikes to "
                    "those channels with `color=N`.",

                    "DIFFICULTY CURVE: the level should ramp up, not be flat. "
                    "Easy intro (first 25%), build-up (next 50%), climax (last "
                    "25% — densest, hardest, with optional speed-up portal). If "
                    "the level is uniformly easy or uniformly chaotic, add or "
                    "remove obstacles to create a curve.",

                    "GAMEPLAY FLOW: verify every gamemode-change portal has a "
                    "FLOOR or CORRIDOR on the OTHER side leading away from it. "
                    "Verify orbs are reachable from the ground row (Y=105) with "
                    "a single jump (Y=135-165). Add small details that make "
                    "movement feel rhythmic — paired spikes, alternating orbs.",

                    "POLISH PASS: scan for anything that feels random — an "
                    "orphaned block in the air, a spike too close to a portal, "
                    "a color trigger that fires too late. Fix specific issues. "
                    "Add 1-2 TRIGGER pulse/color lines to mark transitions you "
                    "care about. End on a strong note (climax + clean transition "
                    "to TRIGGER end).",
                };
                const char* focus = REFINEMENT_FOCUSES[
                    (m_refinementRounds - 1) % REFINEMENT_FOCUSES.size()];

                // (assistant turn already recorded once, unconditionally,
                // near the top of processFinalResponse)

                toolUse::Message refine;
                refine.role = toolUse::MessageRole::User;
                refine.text = fmt::format(
                    "Refinement pass {} of {}. Look back at the level you just "
                    "built. It's {:.1f}% passable across {} columns. Don't "
                    "rebuild it — emit a small polish update that adds, "
                    "removes, or replaces 10-30 objects/macros to improve it.\n\n"
                    "FOCUS for this pass: {}\n\n"
                    "Emit additional EAS lines (or JSON objects/macros) that "
                    "the mod will ACCUMULATE on top of what you've already "
                    "produced. To remove or replace an earlier object, you "
                    "can't 'delete' — but you can override its position with "
                    "a `TRIGGER move` or hide it with `TRIGGER alpha to=0` "
                    "targeting its group, OR you can just add new objects that "
                    "improve the flow around the bad spot. After this pass the "
                    "level will be applied — make it count.",
                    m_refinementRounds, maxRefine,
                    passResult.pass_rate * 100.f, passResult.total_columns,
                    focus);
                refine.imageB64 = visionSnapshotIfSupported();
                m_toolHistory.push_back(std::move(refine));
                showStatus(fmt::format("Refinement pass {}/{}...",
                    m_refinementRounds, maxRefine));
                this->doToolRound();
                return;
            }
        }

        // ── Two-pass decoration gate (one shot, after refinements) ───────
        // Pass 1 built gameplay; this pass adds ONLY visuals on top. Rides
        // the same accumulate-and-rerun machinery as extensions, so the user
        // still gets a single blueprint to accept.
        {
            bool twoPass = Mod::get()->getSettingValue<bool>("two-pass-generation");
            if (twoPass && !m_followUpTurn && !m_decorationPassDone
                && m_usingToolLoop
                && !m_editMode && !m_mutationMode && !m_coopMode) {
                m_decorationPassDone = true;

                // (assistant turn already recorded once, unconditionally,
                // near the top of processFinalResponse)

                toolUse::Message decor;
                decor.role = toolUse::MessageRole::User;
                decor.text =
                    "DECORATION PASS. The gameplay skeleton is done — now make "
                    "it look BUILT, not generated. Work the checklist, every "
                    "item, whole level:\n"
                    "1. PALETTE: META bg/ground + COLOR lines for 2-3 custom "
                    "channels; TRIGGER color at each section boundary so the "
                    "mood shifts as the level progresses.\n"
                    "2. GROUND DETAIL: decorative strips/slabs along the floor "
                    "every 150-300u (vary them); small gears/plants/crystals "
                    "in dead corners.\n"
                    "3. BACKGROUND DEPTH: large slow decor (z_layer=-3, dim "
                    "color) behind the play path every 400-600u — silhouettes "
                    "make a level feel deep.\n"
                    "4. STRUCTURE DRESSING: outline existing block clusters "
                    "with slopes/connectors; glow edges (noglow off) on "
                    "platform lips the player lands on.\n"
                    "5. MOTION: TRIGGER pulse on the beat for 1-2 channels; "
                    "slow TRIGGER rotate on big background gears; one camera "
                    "zoom or shake at the biggest drop.\n"
                    "6. NEGATIVE SPACE: any 300u stretch with nothing but "
                    "floor gets at least one decor element.\n"
                    "DO NOT add, move, or block anything in the play path — "
                    "no new spikes, blocks at player height, portals, or "
                    "orbs. Decor that overlaps the path goes passable + "
                    "z_layer'd behind. Emit additional EAS lines only.";
                decor.imageB64 = visionSnapshotIfSupported();
                m_toolHistory.push_back(std::move(decor));
                showStatus("Decoration pass...");
                this->doToolRound();
                return;
            }
        }

        // ── Self-critique gate (one shot, after refinements) ─────────────
        // Cheaper than another refinement round: the model scores itself and
        // either replies "ALL GOOD" (tolerated above as a no-op) or emits a
        // targeted patch that the accumulator absorbs like any other round.
        {
            bool wantCritique = Mod::get()->getSettingValue<bool>("enable-self-critique");
            if (wantCritique && !m_followUpTurn && !m_critiqueDone
                && m_usingToolLoop
                && !m_editMode && !m_mutationMode && !m_coopMode) {
                m_critiqueDone    = true;
                m_critiquePending = true;

                // (assistant turn already recorded once, unconditionally,
                // near the top of processFinalResponse)

                toolUse::Message crit;
                crit.role = toolUse::MessageRole::User;
                crit.text =
                    "FINAL SELF-REVIEW before the level is applied. First "
                    "line of your reply MUST be \"RATING: n/10\" - your "
                    "honest overall score. Judge like a harsh playtester: "
                    "(a) playability - every jump makeable, no blind traps; "
                    "(b) pacing - density ramps with the difficulty curve; "
                    "(c) variety - no copy-pasted obstacle spam; "
                    "(d) decoration - no bare stretches, cohesive palette, "
                    "background/ground colors set; (e) faithfulness to the "
                    "user's request. If you rate 8+, follow the rating line "
                    "with exactly: ALL GOOD. Otherwise follow it with ONLY "
                    "the fix - 10-40 additional EAS lines/macros repairing "
                    "EVERY issue you found (fill empty stretches, fix "
                    "impossible jumps, decorate bare sections, add missing "
                    "color triggers). No rebuild, no commentary.";
                crit.imageB64 = visionSnapshotIfSupported();
                m_toolHistory.push_back(std::move(crit));
                showStatus("Self-check...");
                this->doToolRound();
                return;
            }
        }

        m_followUpTurn = false;  // staged: next generation's gates fire normally
        // (The assistant reply was already recorded once, unconditionally,
        // near the top of processFinalResponse — no second push here.)
        // Durable chat memory: the turn is complete, so THIS response is the
        // one a resumed conversation needs.
        if (m_session) m_session->chatPush(1, aiResponse);

        // Final apply reached — every extension/passability/refinement path
        // above returned early, so the dump and the apply snapshot really do
        // run once per generation here.
        // The dump excludes {"op":...} entries: they only mean something
        // against THIS turn's inventory — replaying them via the history
        // popup, feeding them to telemetry/feedback, or exporting them as a
        // blueprint would execute or teach stale edits.
        {
            auto cleaned = matjson::Value::array();
            for (size_t i = 0; i < m_accumulatedObjects.size(); ++i) {
                const auto& e = m_accumulatedObjects[i];
                if (e.isObject() && e.contains("op")) continue;
                cleaned.push(e);
            }
            s_lastGeneratedJson = cleaned.dump();
        }

        // Telemetry ON = every output ships to the community collector the
        // moment it's done (rating 0 = not yet rated; the AI's self-review
        // score rides along; a second send follows if the user rates).
        autoContributeGeneration(0);

        // MOVE the accumulator into the apply snapshot — this is the final
        // apply (every loop path above returned early), nothing reads the
        // accumulator again before the next generation re-initializes it.
        // Saves a full deep copy of a potentially multi-hundred-KB tree.
        // prepareObjects mutates/moves out of the snapshot, which is now fine
        // by construction. The dump for the rating popup was taken above.
        auto applyObjects = std::make_shared<matjson::Value>(std::move(m_accumulatedObjects));
        m_accumulatedObjects = matjson::Value::array();  // defensive re-init

        // Capture only the (small) metadata object — levelData still holds
        // the original full objects array, which the lambda never needs.
        auto metadata = std::make_shared<matjson::Value>(
            hasMetadata ? levelData["level_metadata"] : matjson::Value());
        auto applyResult = [this, metadata, applyObjects]() {
            // No live editor (user left the level mid-generation): hold the
            // result; the next editor session adopts and stages it.
            if (!revalidateEditor()) {
                m_pendingApplyObjects = applyObjects;
                m_pendingMetadata     = metadata;
                if (m_session) {
                    m_session->state = GenSession::State::AwaitingEditor;
                    m_session->push(GenSession::Entry::Kind::Status,
                        "Generation finished — waiting for a level editor to stage into");
                }
                Notification::create(
                    "AI level ready — open a level editor to stage it",
                    NotificationIcon::Success)->show();
                return;
            }
            if (m_shouldClearLevel) clearLevel();
            if (metadata->isObject()) applyLevelMetadata(*metadata);
            prepareObjects(*applyObjects);
            // Off-scene engines (backgrounded popup, copilot) have no spawn
            // scheduler — place everything in one pass.
            if (!this->isRunning()) this->stageAllDeferredImmediate();
        };

        // "Show AI output": write the FULL raw response to a file in the mod
        // save folder (background thread — never block the frame on disk).
        // Replaces the old 1800-char review popup, which truncated long
        // responses and gated the apply on a click.
        if (Mod::get()->getSettingValue<bool>("show-ai-output")) {
            auto path = Mod::get()->getSaveDir() / "last-ai-response.txt";
            log::info("EditorAI: raw response ({} chars) -> {}",
                      aiResponse.size(), utils::string::pathToString(path));
            std::thread([path, text = aiResponse] {
                // Serialize writers: back-to-back generations would otherwise
                // race two detached threads on the same file.
                static std::mutex s_dumpMutex;
                bool okWrite;
                {
                    std::lock_guard lock(s_dumpMutex);
                    auto res = utils::file::writeString(path, text);
                    okWrite = res.isOk();
                    if (!okWrite)
                        log::error("Failed to write last-ai-response.txt: {}",
                                   res.unwrapErr());
                }
                // Notify only after the write actually finished (and only
                // claim success when it succeeded). UI must run on the main
                // thread; queueInMainThread is safe to call from any thread.
                Loader::get()->queueInMainThread([okWrite] {
                    Notification::create(
                        okWrite ? "AI output saved to last-ai-response.txt"
                                : "Failed to save AI output (see logs)",
                        okWrite ? NotificationIcon::Info
                                : NotificationIcon::Error)->show();
                });
            }).detach();
        }
        applyResult();
    }

    // Appends the mutation / co-op framing (with the current level as EAS)
    // to a system prompt. Shared by the single-shot path and the tool loop.
    void appendModeContext(std::string& systemPrompt) {
        // Locked palette applies to every mode, including plain generation.
        if (!m_lockedPalette.empty()) {
            systemPrompt += "\n\nLOCKED PALETTE (user-chosen - start your script "
                            "with EXACTLY these lines and design around them):\n";
            systemPrompt += m_lockedPalette;
        }
        if (!m_mutationMode && !m_coopMode) return;
        auto current = buildLevelDataArray();
        std::string currentEAS = eas::objectsToEAS(current,
            (float)Mod::get()->getSettingValue<int64_t>("ai-ground-y"));
        // Hard cap the context: huge levels would blow small-model windows.
        if (currentEAS.size() > 24000) {
            currentEAS.resize(24000);
            auto nl = currentEAS.rfind('\n');
            if (nl != std::string::npos) currentEAS.resize(nl);
            currentEAS += "\n# (level truncated for context)";
        }
        if (m_mutationMode) {
            std::string regionNote;
            if (s_pendingRegionDelete.active)
                regionNote = fmt::format(
                    " REGION REBUILD: the user marked X=[{:.0f},{:.0f}] for "
                    "replacement — the mod DELETES the old objects in that "
                    "range when your preview is accepted. Emit a complete "
                    "replacement section for exactly that range (and nothing "
                    "outside it).",
                    s_pendingRegionDelete.x0, s_pendingRegionDelete.x1);
            systemPrompt += fmt::format(
                "\n\nMODE: MUTATION. The user wants a CHANGE to their existing "
                "level, described in their message. The current level is below "
                "in EAS. You can only ADD objects/triggers (additions stage as "
                "a preview the user approves) — to alter the feel, add new "
                "geometry, recolor via COLOR/TRIGGER color lines, add decor, "
                "or overlay harder/easier obstacle variants near existing ones. "
                "Do NOT re-emit the existing level.{}\n\n"
                "## Current Level\n{}", regionNote, currentEAS);
        } else {
            float curMaxX = computeMaxXFromObjects(current);
            systemPrompt += fmt::format(
                "\n\nMODE: CO-OP TURN. The human built the level up to "
                "X={:.0f}. Continue it SEAMLESSLY for roughly 900 units "
                "(X={:.0f} to {:.0f}): match their object families, density, "
                "and palette. Do not modify anything before X={:.0f}. End "
                "your chunk at a clean transition point.\n\n"
                "## Current Level\n{}",
                curMaxX, curMaxX + 30.f, curMaxX + 930.f, curMaxX, currentEAS);
        }
    }

    void callAPI(const std::string& prompt, const std::string& rawApiKey) {
        // Stashed for the transient-failure retry in onAPISuccess.
        m_lastCallPrompt = prompt;
        m_lastCallKey    = rawApiKey;
        std::string apiKey     = trimKey(rawApiKey);
        std::string provider   = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model      = getProviderModel(provider);
        // Keep the tool-loop identity fields current on EVERY generation —
        // processFinalResponse's loop-back gates read them, and a stale
        // provider from a previous tool-loop run would route fix rounds to
        // the wrong provider/model/key.
        m_toolProvider = provider;
        m_toolModel    = model;
        m_toolApiKey   = apiKey;
        std::string difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        std::string style      = Mod::get()->getSettingValue<std::string>("style");
        std::string length     = Mod::get()->getSettingValue<std::string>("length");
        // "levelID" is the overlay's reference-level mode, not a style word —
        // the actual reference arrives via the "style: <id>" prompt directive.
        if (style == "levelID") style = "match the reference level";

        // Capture generation context for the rating popup
        s_lastUserPrompt = prompt;
        if (!m_followUpTurn) s_lastSelfRating = 0;  // fresh generation, fresh score
        s_lastDifficulty = difficulty;
        s_lastStyle      = style;
        s_lastLength     = length;

        // ── Multi-turn tool-use path ──────────────────────────────────────
        // When the user has tool use on (default) and the selected provider
        // supports it, route through the tool-use loop — including edit
        // mode, which needs the analysis tools and enforcement rounds for
        // heavyweight reworks. The "custom" provider deliberately
        // never goes through this path: we don't know its tool-use dialect.
        // Mutation and co-op are ALWAYS additive — the user's level must
        // never be cleared by these flows.
        if (m_mutationMode || m_coopMode) m_shouldClearLevel = false;

        bool toolsEnabled = Mod::get()->getSettingValue<bool>("enable-ai-tools");
        // Platinum's coordinator only serves /api/generate — the tool loop
        // needs /api/chat, which 404s there. Force single-shot on Platinum.
        bool platinum = provider == "ollama" &&
            Mod::get()->getSettingValue<bool>("use-platinum");
        // Mutation skips the tool loop: it's a targeted single-shot change
        // with the whole level already in context — web/NG tools just burn
        // rounds. Co-op keeps tools (the model may want references).
        // Edit mode RIDES the tool loop (heavy reworks need analyze/verify
        // tools, the object inventory, and the workload-enforcement rounds).
        if (toolsEnabled && !m_mutationMode && !platinum
            && toolUse::supportsToolUse(provider)) {
            log::info("Routing to tool-use loop (provider={})", provider);
            this->runToolLoop(prompt, apiKey);
            return;
        }
        m_usingToolLoop = false;   // single-shot turn: gates must not loop back

        // Single-shot + style reference: fetch the reference's style brief
        // first (one hop through the disk-backed level cache), then re-enter.
        // The "(unavailable)" sentinel breaks the loop when the fetch fails.
        if (!m_styleRefId.empty() && m_styleBrief.empty()) {
            showStatus("Fetching style reference...");
            this->fireFetchLevelByID(m_styleRefId,
                [this, prompt, apiKey](const std::string& summary) {
                    m_styleBrief = summary.empty() ? "(unavailable)" : summary;
                    this->callAPI(prompt, apiKey);
                });
            return;
        }
        log::info("Tool-use loop skipped (toolsEnabled={}, editMode={}, provider={}); using single-shot",
                  toolsEnabled, m_editMode, provider);

        log::info("Calling {} API with model: {}", provider, model);

        std::string systemPrompt = buildSystemPrompt();
        appendModeContext(systemPrompt);
        if (!m_styleRefId.empty() && !m_styleBrief.empty() && m_styleBrief != "(unavailable)") {
            systemPrompt += "\n\nMATCH THIS VISUAL STYLE (reference level the user picked):\n";
            systemPrompt += m_styleBrief;
            systemPrompt += "\nImitate its palette, object families, and density feel - NOT its layout.";
        }

        // ── Level data context ────────────────────────────────────────────────
        // When the user is NOT clearing the level, we pass the existing objects
        // to the AI so it can build on top of them and avoid collisions.
        // When clear level is ON, there is no useful context to send.
        // The full level JSON is also logged here for debugging.
        std::string levelDataSection;
        if (m_editMode) {
            // Edit runs get the numbered inventory so MOVE/DELETE/EDIT
            // selectors resolve against exactly what the model saw — even
            // on single-shot providers (tools off, Platinum, custom).
            std::string inv = buildLevelInventoryListing(1500);
            log::info("=== Single-shot: edit inventory context ({} chars) ===", inv.size());
            levelDataSection = "\n\n" + inv;
        } else if (!m_shouldClearLevel) {
            std::string levelJson = buildLevelDataJson();
            log::info("=== Current Level Data (sent to AI as context) ===");
            logLong("LevelData", levelJson);
            log::info("=== End Level Data ===");
            levelDataSection = "\n\nCurrent level data (build upon or extend these existing objects):\n" + levelJson;
        } else {
            log::info("Clear level is ON — not sending existing level data to AI");
        }

        // Per-call user prompt — kept tight. The system prompt already covers
        // format details and rules; here we just relay the request + plan
        // cues. Both EAS and JSON are accepted (see system prompt).
        std::string fullPrompt = fmt::format(
            "Generate a GD level.\n"
            "Request: {}\n"
            "Difficulty: {} | Style: {} | Length: {}{}\n\n"
            "Plan first (theme → 2-5 sections with X ranges → palette → "
            "macro choices), then emit EAS (preferred) or JSON.",
            prompt, difficulty, style, length, levelDataSection
        );
        fullPrompt += buildBeatGridNote(prompt);

        // Seed the conversation log so follow-up chat works for single-shot
        // generations too (edit mode, tools off, Platinum, custom provider —
        // every path that skips the tool loop used to leave m_toolHistory
        // empty, which made sendFollowUp refuse with "No conversation yet").
        // Follow-up turns append to the existing log instead of reseeding.
        if (!m_followUpTurn) {
            m_toolHistory.clear();
            toolUse::Message sys;
            sys.role = toolUse::MessageRole::System;
            sys.text = systemPrompt;
            m_toolHistory.push_back(std::move(sys));
            toolUse::Message usr;
            usr.role = toolUse::MessageRole::User;
            usr.text = fullPrompt;
            m_toolHistory.push_back(std::move(usr));
        }

        // Log the full system prompt and user prompt before sending
        log::info("=== System Prompt ===");
        logLong("SysPrompt", systemPrompt);
        log::info("=== End System Prompt ===");
        log::info("=== User Prompt ===");
        logLong("UserPrompt", fullPrompt);
        log::info("=== End User Prompt ===");

        matjson::Value requestBody;
        std::string    url;

        // ── Gemini ─────────────────────────────────────────────────────────────
        if (provider == "gemini") {
            // system_instruction: dedicated top-level body field, parts must be an ARRAY.
            auto sysInstructPart = matjson::Value::object();
            sysInstructPart["text"] = systemPrompt;
            auto sysInstruct = matjson::Value::object();
            sysInstruct["parts"] = std::vector<matjson::Value>{sysInstructPart};

            auto userPart = matjson::Value::object();
            userPart["text"] = fullPrompt;
            auto message = matjson::Value::object();
            message["role"]  = "user";
            message["parts"] = std::vector<matjson::Value>{userPart};

            auto genConfig = matjson::Value::object();
            genConfig["temperature"]     = 0.7;
            genConfig["maxOutputTokens"] = 65536;
            // Disable the thinking budget for latency — but ONLY on Flash
            // models. Pro models cannot disable thinking and reject
            // thinkingBudget: 0 with HTTP 400 INVALID_ARGUMENT.
            if (model.find("flash") != std::string::npos) {
                auto thinkingConfig = matjson::Value::object();
                thinkingConfig["thinkingBudget"] = 0;
                genConfig["thinkingConfig"] = thinkingConfig;
            }

            requestBody                      = matjson::Value::object();
            requestBody["systemInstruction"] = sysInstruct;
            requestBody["contents"]          = std::vector<matjson::Value>{message};
            requestBody["generationConfig"]  = genConfig;

            url = fmt::format(
                "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent",
                model
            );

        // ── Claude (Anthropic) ─────────────────────────────────────────────────
        } else if (provider == "claude") {
            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                = matjson::Value::object();
            requestBody["model"]       = model;
            requestBody["max_tokens"]  = 8192;
            requestBody["temperature"] = 0.7;
            // cache_control on the system block: repeat generations with the
            // same settings re-read the cached prefix at ~10% input price.
            {
                auto sysBlock = matjson::Value::object();
                sysBlock["type"] = "text";
                sysBlock["text"] = systemPrompt;
                auto cache = matjson::Value::object();
                cache["type"] = "ephemeral";
                sysBlock["cache_control"] = cache;
                auto sysArr = matjson::Value::array();
                sysArr.push(sysBlock);
                requestBody["system"] = sysArr;
            }
            requestBody["messages"]    = std::vector<matjson::Value>{userMsg};

            url = "https://api.anthropic.com/v1/messages";

        // ── Ollama ─────────────────────────────────────────────────────────────
        // stream=true is REQUIRED — without it, curl times out on large responses
        // because Ollama doesn't send the final HTTP chunk until generation is done.
        // With stream=true we get newline-delimited JSON (NDJSON); onAPISuccess
        // handles the line-by-line parsing and accumulation of the response field.
        } else if (provider == "ollama") {
            std::string ollamaUrl = getOllamaUrl();
            log::info("Using Ollama at: {}", ollamaUrl + "/api/generate");

            auto options = matjson::Value::object();
            options["temperature"] = 0.7;

            requestBody            = matjson::Value::object();
            requestBody["model"]   = model;
            requestBody["prompt"]  = systemPrompt + "\n\n" + fullPrompt;
            requestBody["stream"]  = true;   // REQUIRED: prevents curl timeout
            // NO format:"json" — the system prompt prefers EAS (a line-based
            // DSL) and grammar-constraining the output to one JSON value
            // fought that instruction. The response pipeline handles EAS,
            // fenced JSON, and lenient JSON equally well.
            requestBody["options"] = options;

            url = ollamaUrl + "/api/generate";

        // ── OpenAI-compatible chat completions ─────────────────────────────
        // OpenAI, Mistral, HuggingFace, OpenRouter, DeepSeek, LM Studio,
        // llama.cpp, and BYOPAK ("custom") all take the same body; they only
        // differ in URL, which token-limit field they honour, and how many
        // tokens to ask for. The custom path covers ~95% of hosted and
        // self-hosted providers (Groq, xAI, Cerebras, Together, Fireworks,
        // Perplexity, DeepInfra, Azure-style, vLLM, Tabby, you name it).
        } else {
            if (!toolUse::isOpenAICompat(provider) && provider != "custom") {
                onError("Unknown Provider",
                    fmt::format("Provider '{}' is not supported. Pick a valid provider in settings.",
                                provider));
                return;
            }
            // Token-limit field + value comes from the same table the
            // tool-use loop uses (exactly one field — two at once is an
            // HTTP 400 on strict servers).
            const auto spec = toolUse::tokenLimitSpec(provider);

            auto sysMsg = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                  = matjson::Value::object();
            requestBody["model"]         = model;
            requestBody["messages"]      = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody[spec.field] = spec.limit;
            // OpenAI o-series reasoning models reject a temperature param.
            if (!(provider == "openai" && isOSeriesModel(model)))
                requestBody["temperature"] = 0.7;

            if (provider == "custom") {
                url = Mod::get()->getSettingValue<std::string>("custom-provider-url");
                // The user may paste the base URL (https://api.x.com) or the
                // full chat-completions URL. If it looks like a base URL (no
                // /chat or /completions in the path), append the OpenAI-
                // compatible path.
                if (url.find("/chat/completions") == std::string::npos
                    && url.find("/v1/messages") == std::string::npos
                    && url.find("/completions") == std::string::npos) {
                    if (!url.empty() && url.back() == '/') url.pop_back();
                    url += "/v1/chat/completions";
                }
                log::info("Custom provider URL resolved to: {}", url);
            } else {
                // Same endpoints the tool-use loop hits.
                url = toolUse::urlFor(provider, model);
            }
        }

        std::string jsonBody = requestBody.dump();
        log::info("Sending request to {} ({} bytes)", provider, jsonBody.length());

        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");
        // Same auth headers and timeouts the tool-use loop applies.
        applyProviderAuth(request, provider, apiKey);
        request.timeout(providerTimeout(provider));

        request.bodyString(jsonBody);
        logApiRequest(provider, model, url, jsonBody);
        m_listener.spawn(
            request.post(url),
            [this, provider](web::WebResponse response) {
                this->onAPISuccess(std::move(response), provider);
            }
        );
    }

    // ── Generate button handler ───────────────────────────────────────────────

    void startGeneration(std::string prompt, const std::string& apiKey) {
        // Manual provider has no network: copy the prompt + show the cocos Build
        // dialog instead of calling an API. Defensive single chokepoint — covers
        // any path (cocos onGenerate, headless, resumed) that reaches here with
        // manual selected. (The overlay uses the editoraiManual* bridge directly.)
        if (Mod::get()->getSettingValue<std::string>("ai-provider") == "manual") {
            startManualCopy(prompt);
            return;
        }
        // Rate limiting — prevent excessive API calls
        static std::chrono::steady_clock::time_point lastRequestTime{};
        if (Mod::get()->getSettingValue<bool>("enable-rate-limiting")) {
            auto now = std::chrono::steady_clock::now();
            int64_t minSeconds = Mod::get()->getSettingValue<int64_t>("rate-limit-seconds");
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastRequestTime).count();
            if (elapsed < minSeconds) {
                FLAlertLayer::create("Rate Limited",
                    gd::string(fmt::format("Please wait {} more second(s).", minSeconds - elapsed)),
                    "OK")->show();
                return;
            }
            lastRequestTime = now;
        }

        m_isGenerating = true;
        m_transientRetries = 0;
        m_generateBtn->setVisible(false);
        m_cancelBtn->setVisible(true);

        // No loading circle: the overlay's Sessions tab is the live progress
        // surface now, and the spinner only obscured the editor.
        m_generationStartTime = std::chrono::steady_clock::now();
        showStatus("AI is generating...");
        this->schedule(schedule_selector(AIGeneratorPopup::updateGenerationTimer), 1.0f);
        log::info("=== Generation Request === Prompt: {}", prompt);

        if (m_session) {
            if (m_session->title.empty()) {
                std::string t = prompt;
                if (t.size() > 40) { GenSession::utf8Trim(t, 39); t += "…"; }
                m_session->title = t;
            }
            m_session->state = GenSession::State::Running;
            m_session->push(GenSession::Entry::Kind::User, prompt);
            m_session->chatPush(0, prompt);  // durable chat memory (restarts)
            // Full prompt stored up-front: the overlay's "retry with same
            // prompt" button needs it even when the rating snapshot (which
            // also fills fbPrompt) never runs (e.g. Failed sessions).
            m_session->fbPrompt = prompt;
        }
        addToPromptHistory(prompt);

        // "style: <levelID>" directive — pulled out of the prompt here; the
        // reference's summarized style brief is injected at request time
        // (tool path tells the model to download it; single-shot pre-fetches
        // through the level cache).
        m_styleRefId = extractStyleId(prompt);  // removes the directive in place
        m_styleBrief.clear();
        if (!m_styleRefId.empty())
            log::info("Style reference: level {}", m_styleRefId);

        // Region rebuild only applies in mutation mode; any stale marker from
        // a previous generation is cleared either way.
        s_pendingRegionDelete = {};
        if (m_mutationMode) {
            float rx0 = 0, rx1 = 0;
            if (extractRegionRange(prompt, rx0, rx1)) {
                s_pendingRegionDelete = {rx0, rx1, true};
                log::info("Region rebuild armed: X=[{:.0f},{:.0f}] (old objects "
                          "delete on accept)", rx0, rx1);
            }
        }

        // Reset per-generation state. runToolLoop also resets these, but
        // doing it here too keeps the single-shot / custom-provider /
        // edit-mode paths consistent — a cancelled or errored follow-up
        // must never leak m_followUpTurn/m_critiquePending into the next
        // generation (stale flags made fresh single-shot runs report
        // garbage replies as "Answered" and mis-parse first responses).
        m_accumulatedObjects = matjson::Value::array();
        m_extensionRounds = 0;
        m_passabilityFixRounds = 0;
        m_refinementRounds = 0;
        m_followUpTurn       = false;
        m_followUpMode       = 0;
        m_critiquePending    = false;
        m_critiqueDone       = false;
        m_decorationPassDone = false;
        m_targetObjRounds    = 0;
        m_editEnforceRounds  = 0;
        m_lengthTarget = lengthTargetForSetting(
            Mod::get()->getSettingValue<std::string>("length"));

        // Hand off directly to the API. Tool fetching is done by the AI
        // itself via the multi-turn tool-use loop (see runToolLoop) — there
        // are no user-facing pre-generation tool inputs.
        callAPI(prompt, apiKey);
    }

    void onGenerate(CCObject*) {
        std::string prompt = m_promptInput->getString();
        if (prompt.empty() || prompt == "e.g. Medium difficulty platforming") {
            FLAlertLayer::create("Empty Prompt", gd::string("Please enter a description!"), "OK")->show();
            return;
        }

        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        if (provider == "manual") { startManualCopy(prompt); return; }
        std::string apiKey   = getProviderApiKey(provider);

        // Custom provider needs a URL (always) but the API key is optional
        // (the user might be hitting a local self-hosted server with no auth).
        if (provider == "custom") {
            std::string customUrl = Mod::get()->getSettingValue<std::string>("custom-provider-url");
            if (customUrl.empty()) {
                FLAlertLayer::create("Custom Provider URL Required",
                    gd::string("Open mod settings → Provider tab and enter the base URL of your custom OpenAI-compatible endpoint."),
                    "OK")->show();
                return;
            }
            std::string customModel = Mod::get()->getSettingValue<std::string>("custom-provider-model");
            if (customModel.empty()) {
                FLAlertLayer::create("Custom Provider Model Required",
                    gd::string("Open mod settings → Provider tab and enter the model name your endpoint expects."),
                    "OK")->show();
                return;
            }
        } else if (apiKey.empty() && provider != "ollama" && provider != "lm-studio" && provider != "llama-cpp") {
            FLAlertLayer::create("API Key Required",
                gd::string(fmt::format(
                    "Please open mod settings and enter your API key under the {} section.",
                    provider == "gemini"      ? "Gemini"         :
                    provider == "claude"      ? "Claude"         :
                    provider == "openai"      ? "OpenAI"         :
                    provider == "ministral"   ? "Ministral"      :
                    provider == "huggingface" ? "HuggingFace"    : provider
                )),
                "OK")->show();
            return;
        }

        if (m_shouldClearLevel) {
            geode::createQuickPopup(
                "Clear Level?",
                "This will permanently delete ALL objects in your current level before generating.\n\nThis cannot be undone. Proceed?",
                "Cancel", "Proceed",
                [this, prompt, apiKey](FLAlertLayer*, bool btn2) {
                    if (btn2) this->startGeneration(prompt, apiKey);
                }
            );
        } else {
            startGeneration(prompt, apiKey);
        }
    }

    public:
    // ── Manual provider: copy-paste with any chatbot ────────────────────────
    // No API, no key, no network. Copy builds the full prompt to the clipboard;
    // the user pastes it into any AI (ChatGPT/Claude/Gemini/…), copies the
    // reply, and Build runs the pasted text through the exact same parser +
    // staging pipeline as a real API response. These are public so the overlay
    // bridge (editoraiManual*) and the cocos AI-button path can both drive them.
    std::string buildManualUserBlock(const std::string& prompt) {
        std::string difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        std::string style      = Mod::get()->getSettingValue<std::string>("style");
        std::string length     = Mod::get()->getSettingValue<std::string>("length");
        if (style == "levelID") style = "match the reference level";
        auto lt = lengthTargetForSetting(length);
        float minX = lt.minSeconds * GD_PLAYER_SPEED_1X;
        float maxX = lt.maxSeconds * GD_PLAYER_SPEED_1X;
        return fmt::format(
            "## YOUR LEVEL REQUEST\n"
            "Difficulty: {}   Style: {}   Length: {} (~{:.0f}-{:.0f}s, X span ~{:.0f}-{:.0f})\n"
            "Request: {}\n\n"
            "Reply with ONLY the level: the short markdown plan, then `## Level Script`, "
            "then the EAS lines (or a single JSON object). No other commentary.",
            difficulty, style, lt.label, lt.minSeconds, lt.maxSeconds, minX, maxX, prompt);
    }

    std::string buildManualBlob(const std::string& prompt) {
        return buildSystemPrompt() + "\n\n" + buildManualUserBlock(prompt);
    }

    // Shared core: build the prompt, copy it to the clipboard, record the turn.
    // UI-agnostic — both the cocos popup and the overlay call this.
    void doManualCopy(const std::string& prompt) {
        s_lastUserPrompt = prompt;
        s_lastSelfRating = 0;
        s_lastDifficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        s_lastStyle      = Mod::get()->getSettingValue<std::string>("style");
        s_lastLength     = Mod::get()->getSettingValue<std::string>("length");

        std::string blob = buildManualBlob(prompt);
        m_manualPromptBlob = blob;
        utils::clipboard::write(blob);
        addToPromptHistory(prompt);

        if (m_session) {
            if (m_session->title.empty()) {
                std::string t = prompt;
                if (t.size() > 40) { GenSession::utf8Trim(t, 39); t += "…"; }
                m_session->title = t;
            }
            m_session->fbPrompt = prompt;
            m_session->push(GenSession::Entry::Kind::User, prompt);
            m_session->chatPush(0, prompt);
        }
        showStatus(fmt::format("Prompt copied (~{} tokens) - paste into your AI, "
                               "copy its reply, then Build.", blob.size() / 4));
    }

    // Cocos popup path (editor AI button): copy, then a Build dialog.
    void startManualCopy(const std::string& prompt) {
        doManualCopy(prompt);
        geode::createQuickPopup(
            "Prompt copied",
            gd::string(
                "The full prompt is on your clipboard.\n\n"
                "1. Paste it into any AI - ChatGPT, Claude, Gemini, anything.\n"
                "2. Copy the AI's ENTIRE reply.\n"
                "3. Come back and press Build.\n\n"
                "No key, no account - your own chatbot makes the level."),
            "Close", "Build",
            [this](FLAlertLayer*, bool build) { if (build) this->onManualBuild(); });
    }

    // Overlay path: set the generation mode, then copy (no cocos dialog — the
    // overlay surfaces its own Build button while a manual copy is pending).
    void startManualHeadless(const std::string& prompt, bool replaceContents) {
        m_shouldClearLevel = replaceContents;
        m_editMode = !replaceContents;
        doManualCopy(prompt);
    }

    void onManualBuild() {
        std::string pasted = utils::clipboard::read();
        if (pasted.empty()) {
            FLAlertLayer::create("Clipboard Empty",
                gd::string("Copy your AI's full reply to the clipboard first, then press Build."),
                "OK")->show();
            return;
        }
        // Guard: Build pressed before the AI's reply was copied — the clipboard
        // still holds the prompt we wrote.
        if (pasted == m_manualPromptBlob ||
            pasted.find("## YOUR LEVEL REQUEST") != std::string::npos) {
            FLAlertLayer::create("That's the prompt",
                gd::string("Your clipboard still has the prompt. Paste it into your AI, "
                           "copy the AI's reply, then press Build."),
                "OK")->show();
            return;
        }

        // Per-generation reset — single-shot, no tool loop, no API follow-up
        // rounds (m_usingToolLoop = false gates every extension/refine/critique
        // path in processFinalResponse).
        m_accumulatedObjects = matjson::Value::array();
        m_extensionRounds = 0; m_passabilityFixRounds = 0; m_refinementRounds = 0;
        m_targetObjRounds = 0;  m_editEnforceRounds = 0;
        m_followUpTurn = false; m_followUpMode = 0;
        m_critiquePending = false; m_critiqueDone = false; m_decorationPassDone = false;
        m_usingToolLoop = false;
        m_toolProvider = "manual"; m_toolModel = "manual"; m_toolApiKey.clear();
        m_lengthTarget = lengthTargetForSetting(
            Mod::get()->getSettingValue<std::string>("length"));
        m_isGenerating = true;
        if (m_session) m_session->state = GenSession::State::Running;

        showStatus("Building from pasted output...");
        processFinalResponse(std::move(pasted), "manual");
    }
    protected:

    // ── API response handler ──────────────────────────────────────────────────

    void onAPISuccess(web::WebResponse response, const std::string& provider) {
        logApiResponse(response.code(), response.string().unwrapOr(""));
        // Transient-failure retry runs BEFORE the UI reset so the loading
        // state survives the backoff.
        if (!response.ok()) {
            int code = response.code();
            bool transient = code == 429 || code == 500 || code == 502 ||
                             code == 503 || code == 529;
            if (transient && m_transientRetries < 1) {
                ++m_transientRetries;
                log::warn("Transient HTTP {} on single-shot — retrying once in 2s", code);
                showStatus(fmt::format("Provider hiccup (HTTP {}) — retrying...", code));
                // Move-through capture — see retryToolRoundIfTransient for
                // why the worker thread must never destroy a Ref copy.
                Ref<AIGeneratorPopup> self = this;
                std::thread([self = std::move(self)]() mutable {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    Loader::get()->queueInMainThread([self = std::move(self)] {
                        if (!self->m_isGenerating) return;
                        self->callAPI(self->m_lastCallPrompt, self->m_lastCallKey);
                    });
                }).detach();
                return;
            }
        }

        resetGenerationUI();

        if (!response.ok()) {
            auto [title, message] = parseAPIError(
                response.string().unwrapOr("No error details available"),
                response.code()
            );
            showStatus("Failed!", true);
            FLAlertLayer::create(title.c_str(), gd::string(message), "OK")->show();
            return;
        }

        {
            std::string aiResponse;

            // ── Ollama: NDJSON streaming response ─────────────────────────────
            // With stream=true, Ollama sends one JSON object per line:
            //   {"model":"...","response":"chunk","done":false}
            //   {"model":"...","response":"chunk","done":false}
            //   {"model":"...","response":"","done":true,"context":[...]}
            //
            // response.json() tries to parse the whole body as one JSON object
            // and always fails on streaming output. We must parse line by line,
            // accumulate all "response" fields, and verify "done":true on the
            // final line.
            if (provider == "ollama") {
                auto rawResult = response.string();
                if (!rawResult) {
                    onError("Invalid Response",
                        fmt::format("The provider returned data that didn't look like JSON at all. "
                                    "This is almost always a temporary upstream outage — try again "
                                    "in a minute, or switch providers in settings. ({})",
                                    autoErrorCode(60, 1)));
                    return;
                }

                std::string rawBody = rawResult.unwrap();
                std::string accumulated;
                bool isDone = false;
                int  lineCount = 0;

                // Walk the buffer in place — istringstream would copy the
                // whole (potentially multi-MB) stream body just to split it
                // into lines. matjson::parse accepts the string_view directly.
                size_t scanPos = 0;
                while (scanPos < rawBody.size()) {
                    size_t eol = rawBody.find('\n', scanPos);
                    if (eol == std::string::npos) eol = rawBody.size();
                    std::string_view line(rawBody.data() + scanPos, eol - scanPos);
                    scanPos = eol + 1;

                    // Trim carriage return from Windows line endings
                    if (!line.empty() && line.back() == '\r')
                        line.remove_suffix(1);
                    if (line.empty()) continue;

                    ++lineCount;
                    auto lineJson = matjson::parse(line);
                    if (!lineJson) {
                        // Non-JSON line in the stream — skip silently
                        log::warn("Ollama: skipping non-JSON stream line {}: {}", lineCount, line.substr(0, 80));
                        continue;
                    }

                    // Const: non-const matjson operator[] would insert a null
                    // member per missing key, thousands of lines per response.
                    const auto lineObj = lineJson.unwrap();

                    // Accumulate text chunks
                    auto chunk = lineObj["response"].asString();
                    if (chunk) accumulated += chunk.unwrap();

                    // Check done flag — Ollama sets this true on the final line
                    auto doneResult = lineObj["done"].asBool();
                    if (doneResult && doneResult.unwrap()) {
                        isDone = true;
                    }

                    // Also surface any Ollama-level error messages
                    auto errorMsg = lineObj["error"].asString();
                    if (errorMsg) {
                        onError("Ollama Error",
                            fmt::format("Ollama reported: {}. Check that the model is installed "
                                        "(`ollama list`), the server is running (`ollama serve`), and "
                                        "your selected model name in mod settings matches exactly. ({})",
                                        errorMsg.unwrap(), autoErrorCode(80, 1)));
                        return;
                    }
                }

                log::info("Ollama stream: {} lines parsed, done={}, accumulated {} chars",
                    lineCount, isDone, accumulated.size());

                if (!isDone) {
                    onError("Incomplete Response",
                        fmt::format("Ollama cut the stream off before saying it was done. The model "
                                    "probably hit its context window. Pick a shorter \"length\" setting, "
                                    "lower \"max objects\", or switch to a model with a bigger context "
                                    "(num_ctx 16k+). ({})",
                                    autoErrorCode(80, 3)));
                    return;
                }

                if (accumulated.empty()) {
                    onError("Invalid Response",
                        fmt::format("Ollama finished cleanly but the response text was empty. The "
                                    "model may have refused, or the system prompt may have overflowed "
                                    "the context. Lower max objects or simplify the prompt. ({})",
                                    autoErrorCode(80, 2)));
                    return;
                }

                aiResponse = accumulated;

            // ── All other providers: standard single-JSON response ─────────────
            } else {
                auto jsonRes = response.json();
                if (!jsonRes) {
                    onError("Invalid Response",
                        fmt::format("The provider returned data that didn't look like JSON at all. "
                                    "This is almost always a temporary upstream outage — try again "
                                    "in a minute, or switch providers in settings. ({})",
                                    autoErrorCode(60, 1)));
                    return;
                }

                const auto json = jsonRes.unwrap();  // const: reads must not insert

                if (provider == "gemini") {
                    // Check if the entire request was blocked before generation started.
                    if (json.contains("promptFeedback")) {
                        auto blockReasonResult = json["promptFeedback"]["blockReason"].asString();
                        if (blockReasonResult) {
                            const std::string& reason = blockReasonResult.unwrap();
                            if (!reason.empty() && reason != "BLOCK_REASON_UNSPECIFIED") {
                                onError("Prompt Blocked",
                                    fmt::format("[{}] Gemini blocked the request.\n\nReason: {}\n\n"
                                        "Try rephrasing your prompt.", autoErrorCode(70, 10), reason));
                                return;
                            }
                        }
                    }

                    auto candidates = json["candidates"];
                    if (!candidates.isArray() || candidates.size() == 0) {
                        onError("No Response", fmt::format("[{}] The AI returned no content.", autoErrorCode(60, 4))); return;
                    }

                    auto finishReasonResult = candidates[0]["finishReason"].asString();
                    if (finishReasonResult) {
                        const std::string& finishReason = finishReasonResult.unwrap();
                        if (finishReason == "SAFETY") {
                            onError("Response Blocked",
                                fmt::format("[{}] Gemini's safety filter blocked the response.\n\n"
                                "Try rephrasing your prompt or changing difficulty/style.", autoErrorCode(70, 11)));
                            return;
                        }
                        if (finishReason == "RECITATION") {
                            onError("Response Blocked",
                                fmt::format("[{}] Gemini blocked the response (recitation policy).\n\n"
                                "Try rephrasing your prompt.", autoErrorCode(70, 12)));
                            return;
                        }
                        if (finishReason == "MAX_TOKENS") {
                            log::warn("Gemini hit max token limit — response may be truncated");
                        }
                    }

                    // Accumulate ALL text parts: Pro models (which can't
                    // disable thinking) may put a thought part first, so
                    // parts[0] isn't guaranteed to carry the text.
                    {
                        const auto& parts = candidates[0]["content"]["parts"];
                        std::string acc;
                        if (parts.isArray())
                            for (size_t pi = 0; pi < parts.size(); ++pi) {
                                const auto& p = parts[pi];
                                if (!p.contains("text")) continue;
                                auto t = p["text"].asString();
                                if (t) acc += t.unwrap();
                            }
                        if (acc.empty()) { onError("Invalid Response", fmt::format("[{}] Failed to extract text from AI response.", autoErrorCode(60, 3))); return; }
                        aiResponse = std::move(acc);
                    }

                } else if (provider == "claude") {
                    const auto& content = json["content"];
                    if (!content.isArray() || content.size() == 0) {
                        onError("No Response", fmt::format("[{}] The AI returned no content.", autoErrorCode(60, 4))); return;
                    }
                    // Skip non-text blocks (thinking etc.) instead of trusting
                    // content[0] to be the text block.
                    std::string acc;
                    for (size_t ci = 0; ci < content.size(); ++ci) {
                        const auto& blk = content[ci];
                        auto bt = blk["type"].asString();
                        if (bt && bt.unwrap() == "thinking") {
                            auto th = blk["thinking"].asString();
                            if (th && !th.unwrap().empty())
                                pushSession(GenSession::Entry::Kind::Thinking, th.unwrap());
                            continue;
                        }
                        auto t = blk["text"].asString();
                        if (t) acc += t.unwrap();
                    }
                    if (acc.empty()) { onError("Invalid Response", fmt::format("[{}] Failed to extract text from AI response.", autoErrorCode(60, 3))); return; }
                    aiResponse = std::move(acc);

                // OpenAI, Mistral AI, HuggingFace, OpenRouter, LM Studio,
                // llama.cpp, DeepSeek, and BYOPAK ("custom") all share the
                // OpenAI /v1/chat/completions response envelope.
                } else if (provider == "openai" || provider == "ministral" || provider == "huggingface"
                        || provider == "openrouter" || provider == "lm-studio" || provider == "llama-cpp"
                        || provider == "deepseek"  || provider == "custom") {
                    auto choices = json["choices"];
                    if (!choices.isArray() || choices.size() == 0) {
                        onError("No Response", fmt::format("[{}] The AI returned no content.", autoErrorCode(60, 4))); return;
                    }
                    auto textResult = choices[0]["message"]["content"].asString();
                    if (!textResult) { onError("Invalid Response", fmt::format("[{}] Failed to extract text from AI response.", autoErrorCode(60, 3))); return; }
                    aiResponse = textResult.unwrap();
                }
            }

            // Final text in hand — hand off to the shared post-processing
            // path (same logic the tool-use loop calls when its final answer
            // is the model's plain-text JSON).
            this->processFinalResponse(std::move(aiResponse), provider);
        }
    }

    void onError(const std::string& title, const std::string& message) {
        resetGenerationUI();
        m_followUpTurn    = false;   // turn-scoped flags die with the turn
        m_critiquePending = false;
        showStatus("Failed!", true);
        log::error("Generation failed: {}", message);
        if (m_session) {
            m_session->state = GenSession::State::Failed;
            m_session->push(GenSession::Entry::Kind::Error,
                            fmt::format("{}: {}", title, message));
        }
        // Off-scene (backgrounded): an FLAlert would pop over an unrelated
        // scene — a notification is the right weight there.
        if (this->getParent())
            FLAlertLayer::create(title.c_str(), gd::string(message), "OK")->show();
        else
            Notification::create(fmt::format("AI generation failed: {}", title),
                                 NotificationIcon::Error)->show();
    }

    void closePopup() { this->onClose(nullptr); }

public:
    static AIGeneratorPopup* create(LevelEditorLayer* layer) {
        auto ret = new AIGeneratorPopup();
        if (ret->init(layer)) {
            ret->autorelease();
            ret->m_session = newGenSession();
            ret->m_session->engineRef = ret;   // keeps the engine alive off-scene
            ret->m_session->enginePtr = ret;
            // Remember which level this generation belongs to — adoption and
            // the go-to-level button must never apply a blueprint to a level
            // the user didn't pick. The NAME is persisted so a restored
            // session can re-resolve its level after a restart.
            if (layer && layer->m_level) {
                ret->m_session->targetLevel     = layer->m_level;
                ret->m_session->targetLevelName = layer->m_level->m_levelName;
            }
            return ret;
        }
        delete ret;
        return nullptr;
    }

    // Re-attach an engine to an EXISTING session (restored from disk, or one
    // whose engine died). `layer` may be null — plan/chat turns never need
    // an editor, and edit turns without one park as AwaitingEditor like any
    // other off-editor result.
    static AIGeneratorPopup* createForSession(LevelEditorLayer* layer,
                                              const std::shared_ptr<GenSession>& sess) {
        if (!sess) return nullptr;
        auto ret = new AIGeneratorPopup();
        if (ret->init(layer)) {
            ret->autorelease();
            ret->m_session = sess;
            sess->engineRef = ret;
            sess->enginePtr = ret;
            sess->restored  = false;           // live again
            if (layer && layer->m_level && !sess->targetLevel) {
                sess->targetLevel     = layer->m_level;
                sess->targetLevelName = layer->m_level->m_levelName;
            }
            editoraiMarkSessionsDirty();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    // Rebuild the AI context from the session's durable chat memory (the
    // mod's own history system): system prompt + rolling summary + the
    // newest turns, merged so roles strictly alternate (Claude rejects
    // consecutive same-role messages).
    void seedHistoryFromSession() {
        if (!m_session || !m_toolHistory.empty()) return;
        toolUse::Message sys;
        sys.role = toolUse::MessageRole::System;
        sys.text = buildSystemPrompt();
        appendModeContext(sys.text);
        m_toolHistory.push_back(std::move(sys));

        std::vector<std::pair<int, std::string>> merged;
        if (!m_session->chatSummary.empty())
            merged.push_back({0, "(Summary of this conversation's earlier "
                                 "turns - context, not a new request:)\n"
                                 + m_session->chatSummary});
        for (auto& m : m_session->chat) {
            if (m.text.empty()) continue;
            if (!merged.empty() && merged.back().first == m.role)
                merged.back().second += "\n\n" + m.text;
            else
                merged.push_back({m.role, m.text});
        }
        // The caller is about to append a fresh User turn — a seeded history
        // that ends on a user turn would break strict role alternation, so
        // close it with a minimal assistant turn instead of dropping content.
        if (!merged.empty() && merged.back().first == 0)
            merged.push_back({1, "(Understood.)"});
        for (auto& [role, txt] : merged) {
            toolUse::Message msg;
            msg.role = role == 0 ? toolUse::MessageRole::User
                                 : toolUse::MessageRole::Assistant;
            msg.text = std::move(txt);
            m_toolHistory.push_back(std::move(msg));
        }
        log::info("Session {}: rebuilt AI context from chat memory "
                  "({} messages{})", m_session->id, m_toolHistory.size(),
                  m_session->chatSummary.empty() ? "" : " + summary");
    }

    // Re-point a surviving engine at a freshly (re)opened editor for its
    // level. Stale inventory dies with the old scene; revalidateEditor's
    // null-out is undone by the fresh pointer.
    void reattachEditor(LevelEditorLayer* layer) {
        if (!layer || m_editorLayer == layer) return;
        m_editorLayer = layer;
        m_editInventory.clear();
    }

    // Continue a conversation on a freshly re-attached engine. echoUser
    // mirrors sendFollowUp's (queued edits already echoed when typed).
    void resumeFollowUp(const std::string& text, int mode, bool echoUser = true) {
        m_shouldClearLevel = false;
        if (mode == 0) m_editMode = true;   // edit doctrine in the seed prompt
        seedHistoryFromSession();
        this->sendFollowUp(text, mode, echoUser);
    }

    // Continue the conversation after a generation finished. The session's
    // tool history is intact, so the model has full context; the follow-up
    // gates below keep length-enforcement / refinement / two-pass machinery
    // from re-firing on what is now a conversational turn.
    // Public seam wrapper — onCancel itself is a protected UI callback.
    void requestCancel() { this->onCancel(nullptr); }

    // Public seam: EditorPauseLayer::onResume re-draws the playtest ghost
    // (it is removed on pause and was never restored).
    void redrawPlaytestGhost() { this->drawPlaytestGhost(); }

    // Headless overlay entry: plain generation with explicit overwrite
    // semantics; the engine is never shown (off-scene staging path).
    void startHeadless(const std::string& prompt, bool replaceContents) {
        m_shouldClearLevel = replaceContents;
        m_editMode = !replaceContents;  // additive runs use edit-mode framing
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        startGeneration(prompt, getProviderApiKey(provider));
    }

    // Headless copilot entry: mutation-framed generation started by the
    // editor's copilot tick (the popup is never shown; results stage via
    // the off-scene immediate path).
    void startCopilotFix(const std::string& problems) {
        m_mutationMode = true;
        if (m_session) m_session->title = "Copilot fix";
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        startGeneration(
            fmt::format("COPILOT FIX: automatic check found these problems in "
                        "the user's level: {}. Repair ONLY these spots with "
                        "minimal additive changes (widen gaps, lower obstacles, "
                        "or add an orb/pad assist). Do not touch anything else.",
                        problems),
            getProviderApiKey(provider));
    }

    // mode: 0 = edit (default — may change the level), 1 = plan (plan only,
    // never emits objects), 2 = chat (conversational answer, never a script).
    // echoUser=false when the message was already echoed to the transcript
    // (queued edits show the moment they're typed, not when they run).
    void sendFollowUp(const std::string& text, int mode = 0, bool echoUser = true) {
        if (text.empty()) return;
        if (m_isGenerating) {
            Notification::create("Still generating — wait for this turn",
                                 NotificationIcon::Warning)->show();
            return;
        }
        if (m_toolHistory.empty()) {
            Notification::create("No conversation yet — generate first",
                                 NotificationIcon::Warning)->show();
            return;
        }
        log::info("Session {}: follow-up turn (mode {}): {}",
                  m_session ? m_session->id : 0, mode, text);
        if (m_session) {
            m_session->state = GenSession::State::Running;
            if (echoUser) m_session->push(GenSession::Entry::Kind::User, text);
            m_session->chatPush(0, text);   // durable chat memory (restarts)
        }
        m_followUpTurn       = true;
        m_followUpMode       = mode;
        m_isGenerating       = true;
        m_transientRetries   = 0;
        m_toolIterations     = 0;        // fresh, unbounded round count this turn
        m_forceFinalize      = false;
        m_forceFinalizeTries = 0;
        m_editEnforceRounds  = 0;
        m_toolCallSigCounts.clear();
        m_shouldClearLevel = false;      // follow-ups always modify additively
        m_accumulatedObjects = matjson::Value::array();

        std::string modeNote;
        if (mode == 1) {
            modeNote = "\n\n(PLAN MODE: respond with a structured build plan "
                       "only — sections, pacing, object choices, trigger ideas. "
                       "Do NOT emit any EAS script or JSON objects; nothing "
                       "will be placed this turn.)";
        } else if (mode == 2) {
            modeNote = "\n\n(CHAT MODE: answer conversationally. Do NOT emit "
                       "any EAS script or JSON — no level changes this turn.)";
        } else {
            int editTarget = (int)Mod::get()->getSettingValue<int64_t>("edit-target-ops");
            modeNote = fmt::format(
                "\n\n(EDIT MODE: follow-up turn on the same level. If the user "
                "wants changes, act as a full co-editor: MOVE/DELETE/EDIT "
                "existing objects (bulk selectors: #a-b, rect:x1,y1,x2,y2, "
                "id:type) plus new additions — a substantive request deserves "
                "a substantive rework{}. If this is just a question, answer in "
                "plain text with no script.)",
                editTarget > 0
                    ? fmt::format(", and the mod enforces a minimum of {} "
                                  "total edits on rework turns", editTarget)
                    : std::string());
            // The numbered inventory the selectors resolve against — rebuilt
            // every edit turn so indices always match what the model sees.
            if (revalidateEditor()) {
                modeNote += "\n\n" + buildLevelInventoryListing(1500);
            }
        }
        // Unbounded conversations: past ~40 messages, fold the oldest turns
        // away (keep the system prompt + the newest 30). The level itself is
        // re-sent as context each turn, so old turns lose value fast — but
        // the request would otherwise grow without limit.
        {
            size_t nonSystem = 0;
            for (auto& m : m_toolHistory)
                if (m.role != toolUse::MessageRole::System) ++nonSystem;
            if (nonSystem > 40) {
                size_t drop = nonSystem - 30;
                for (auto it = m_toolHistory.begin();
                     it != m_toolHistory.end() && drop > 0;) {
                    if (it->role != toolUse::MessageRole::System &&
                        it->toolCalls.empty() &&
                        it->toolResults.empty()) {
                        it = m_toolHistory.erase(it);
                        --drop;
                    } else if (it->role != toolUse::MessageRole::System) {
                        // tool-call/result pairs must go together or
                        // provider validation breaks — drop them as a unit
                        auto next = std::next(it);
                        if (next != m_toolHistory.end() &&
                            next->role == toolUse::MessageRole::ToolResults) {
                            it = m_toolHistory.erase(it, std::next(next));
                            drop = drop > 2 ? drop - 2 : 0;
                        } else {
                            it = m_toolHistory.erase(it);
                            --drop;
                            // Never leave an orphaned ToolResults behind a
                            // dropped assistant — providers 400 on it.
                            if (it != m_toolHistory.end() &&
                                it->role == toolUse::MessageRole::ToolResults) {
                                it = m_toolHistory.erase(it);
                                if (drop > 0) --drop;
                            }
                        }
                    } else {
                        ++it;
                    }
                }
                log::info("Conversation pruned to {} messages", m_toolHistory.size());
            }
        }

        // Role hygiene after pruning: Claude (and Gemini) reject a request
        // whose first conversational message isn't a User turn — an odd
        // prune count leaves an Assistant message at the front and every
        // later follow-up 400s. Drop leading non-User messages.
        while (true) {
            auto it = m_toolHistory.begin();
            while (it != m_toolHistory.end() &&
                   it->role == toolUse::MessageRole::System) ++it;
            if (it == m_toolHistory.end() ||
                it->role == toolUse::MessageRole::User) break;
            m_toolHistory.erase(it);
        }

        // Pruned turns aren't lost: the session's rolling summary (chatPush
        // folds old turns into it) is pinned inside the system message, so
        // however long the conversation runs, the request stays bounded at
        // roughly system + summary + the newest turns. Idempotent — the
        // marker section is replaced, never stacked.
        if (m_session && !m_session->chatSummary.empty() && !m_toolHistory.empty()
            && m_toolHistory.front().role == toolUse::MessageRole::System) {
            static const char* MARK =
                "\n\n## Conversation summary (earlier turns, condensed)\n";
            auto& sysText = m_toolHistory.front().text;
            auto pos = sysText.find(MARK);
            if (pos != std::string::npos) sysText.resize(pos);
            sysText += MARK + m_session->chatSummary;
        }

        // Strict alternation: a prior turn that ended in an error or the
        // force-finalize give-up can leave the history tail on a user-equivalent
        // role (User, or ToolResults which maps to role=user on Claude/Gemini).
        // Pushing this follow-up's User turn after that would be back-to-back
        // user turns → 400. Insert a minimal assistant turn to keep roles
        // alternating (the front-of-history hygiene below handles the head).
        if (!m_toolHistory.empty()) {
            auto tailRole = m_toolHistory.back().role;
            if (tailRole == toolUse::MessageRole::User ||
                tailRole == toolUse::MessageRole::ToolResults) {
                toolUse::Message gap;
                gap.role = toolUse::MessageRole::Assistant;
                gap.text = "(ok)";
                m_toolHistory.push_back(std::move(gap));
            }
        }

        toolUse::Message user;
        user.role = toolUse::MessageRole::User;
        user.text = text + modeNote;
        // Vision models see the CURRENT state of the level with every
        // follow-up — "make the drop section harder" works off the actual
        // picture, not coordinate guesswork.
        user.imageB64 = visionSnapshotIfSupported();
        m_toolHistory.push_back(std::move(user));

        // Tool-capable providers continue the native tool conversation.
        // Everyone else (custom endpoint, Platinum — whose coordinator has
        // no /api/chat) gets a single-shot follow-up: the recent exchange is
        // serialized into one prompt and sent through the normal API path.
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        bool platinum = provider == "ollama" &&
            Mod::get()->getSettingValue<bool>("use-platinum");
        bool toolsEnabled = Mod::get()->getSettingValue<bool>("enable-ai-tools");
        m_usingToolLoop = toolsEnabled && toolUse::supportsToolUse(provider) && !platinum;
        if (m_usingToolLoop) {
            this->doToolRound();
            return;
        }
        std::string convo;
        int taken = 0;
        for (auto it = m_toolHistory.rbegin();
             it != m_toolHistory.rend() && taken < 6; ++it) {
            if (it->role == toolUse::MessageRole::System ||
                it->role == toolUse::MessageRole::ToolResults) continue;
            std::string t = it->text;
            if (t.empty()) continue;
            if (t.size() > 1500) { t.resize(1500); t += " [...]"; }
            convo = fmt::format("{}: {}\n\n",
                it->role == toolUse::MessageRole::Assistant
                    ? "You previously said" : "User said", t) + convo;
            ++taken;
        }
        std::string followPrompt = fmt::format(
            "(Conversation so far - newest last)\n{}"
            "Reply to the user's newest message above.", convo);
        this->callAPI(followPrompt, getProviderApiKey(provider));
    }

    // Transcript helper — every UI surface reads the session transcript.
    void pushSession(GenSession::Entry::Kind k, std::string text) {
        if (m_session) m_session->push(k, std::move(text));
    }

    // The editor pointer is only trusted after re-validation against the
    // LIVE editor — the user can exit the level mid-generation, and a stale
    // pointer would be a use-after-free.
    bool revalidateEditor() {
        if (m_editorLayer && m_editorLayer != LevelEditorLayer::get())
            m_editorLayer = nullptr;
        return m_editorLayer != nullptr;
    }

    // Called from a NEW editor session to take over a generation that
    // finished while no editor existed (or continue one mid-flight).
    void adoptEditor(LevelEditorLayer* layer) {
        m_editorLayer = layer;
        // The inventory still holds Refs from the editor that was destroyed
        // — non-empty, so resolveOpSelector's rebuild-if-empty path would
        // never fire and every #index op would silently match nothing
        // (while pinning the dead scene's objects in memory). Clear it; the
        // resolver rebuilds against the fresh scene with identical ordering.
        m_editInventory.clear();
        // Mirror the live-editor apply path: a fresh-mode generation clears
        // before staging — skipping this piled ghosts onto the new level.
        if (m_pendingApplyObjects && m_shouldClearLevel) {
            clearLevel();
            m_shouldClearLevel = false;  // never double-clear on re-adopt
        }
        if (m_pendingApplyObjects) {
            log::info("Session {}: adopting new editor, staging pending blueprint",
                      m_session ? m_session->id : 0);
            if (m_pendingMetadata && m_pendingMetadata->isObject())
                applyLevelMetadata(*m_pendingMetadata);
            auto objs = std::move(*m_pendingApplyObjects);
            m_pendingApplyObjects.reset();
            m_pendingMetadata.reset();
            prepareObjects(objs);
            // Off-scene node: the 20 Hz spawn scheduler is dead, so place
            // everything in one pass and run the completion transition.
            this->stageAllDeferredImmediate();
        }
    }

};

// ─── EditorUI hook — mounts AI button onto "editor-buttons-menu" ─────────────

static CCNode* getAIButton(EditorUI* ui) {
    if (!ui) return nullptr;
    auto menu = ui->getChildByID("editor-buttons-menu");
    if (!menu) return nullptr;
    return menu->getChildByID("ai-button"_spr);
}

// Move every preview object from the preview layer back to its intended
// editor layer(s), and return the editor to the layer it was on before the
// preview opened. Shared by Accept and Edit (Deny removes the objects).
static void restorePreviewLayers(LevelEditorLayer* lel) {
    for (size_t i = 0; i < s_previewObjects.size(); ++i) {
        if (GameObject* obj = s_previewObjects[i]) {
            auto intended = i < s_previewIntendedLayers.size()
                ? s_previewIntendedLayers[i]
                : std::pair<short, short>{0, 0};
            obj->m_editorLayer  = intended.first;
            obj->m_editorLayer2 = intended.second;
        }
    }
    setEditorCurrentLayer(lel, s_editorLayerBeforePreview);
    s_previewLayer = -1;
}

// Snapshot id+position of every preview object as the baseline for implicit
// edit-tracking feedback. Shared by Accept and Edit.
static void snapshotAcceptedObjects() {
    s_acceptedSnapshot.clear();
    for (auto& objRef : s_previewObjects) {
        if (GameObject* obj = objRef) {
            s_acceptedSnapshot.push_back({
                obj->m_objectID,
                obj->getPositionX(),
                obj->getPositionY()
            });
        }
    }
    s_snapshotPrompt = s_lastUserPrompt;
}

// Overlay-initiated generation queued until its target editor is ready
// (consumed by AIEditorUI::addAIButton, filled by editoraiStartGeneration).
// `level` pins the queue entry to ONE level: if the user wanders into a
// different editor first, the request stays queued and fires only when the
// right level finally opens.
struct PendingOverlayGen {
    bool                  active = false;
    std::string           prompt;
    bool                  replaceContents = true;
    geode::Ref<GJGameLevel> level;
};
static PendingOverlayGen s_pendingOverlayGen;

// True from a go-to-level click until the destination editor signals ready;
// blocks double-clicks during the fade (both layers read null mid-fade, so
// the layer checks alone can't catch a second click).
static bool s_gotoTransitionPending = false;

class $modify(AIEditorUI, EditorUI) {
    struct Fields {
        bool m_buttonAdded = false;
        // Parent CCNode of the preview tray (bg + labels + button menu).
        // The pause/playtest hooks only getChildByID + setVisible, so the
        // CCMenu→CCNode change is transparent to them.
        CCNode* m_previewButtonMenu = nullptr;
        // Copilot mode state (see copilotTick).
        int m_copilotLastCount   = -1;
        int m_copilotStableTicks = 0;
        std::chrono::steady_clock::time_point m_copilotLastFix{};
        std::string m_copilotLastFingerprint;
    };

    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;

        // A fresh EditorUI means any previous editor scene is gone. If the
        // user exited mid-preview, the shared preview state still points at
        // objects from the dead scene and would permanently block the AI
        // button ("Preview Active") with no buttons left to clear it.
        if (s_inPreviewMode || s_inEditMode || !s_previewObjects.empty()
            || !s_editOpJournal.empty()) {
            log::info("EditorAI: clearing stale preview state from a previous editor session");
            s_inPreviewMode = false;
            s_inEditMode = false;
            s_previewObjects.clear();
            s_previewIntendedLayers.clear();
            s_previewLayer = -1;
            s_editorLayerBeforePreview = -1;   // never carry across editors
            // Pending edit ops from the dead scene: the objects are gone with
            // their editor — drop the journal (Refs would pin dead nodes).
            s_editOpJournal.clear();
            s_editOpDeleted.clear();
            removePlaytestGhost();  // Ref would otherwise leak a dead-scene node
        }

        // Ensure NodeIDs has assigned IDs before we look anything up.
        NodeIDs::provideFor(this);

        this->runAction(CCSequence::create(
            CCDelayTime::create(0.1f),
            CCCallFunc::create(this, callfunc_selector(AIEditorUI::addAIButton)),
            nullptr
        ));
        return true;
    }

    void addAIButton() {
        // The cocos AI/Mutate buttons are gone — the ImGui overlay (E key /
        // floating bubble) is the UI now. This hook remains the "editor is
        // ready" signal: it adopts waiting sessions, consumes queued
        // overlay-initiated generations, and starts the copilot watcher.
        if (m_fields->m_buttonAdded) return;
        m_fields->m_buttonAdded = true;
        s_gotoTransitionPending = false;   // destination editor arrived

#ifndef EDITORAI_HAS_IMGUI
        // Platforms built without the ImGui overlay keep the classic cocos
        // buttons as their entry point (the popups still exist for them).
        if (auto menu = this->getChildByID("editor-buttons-menu")) {
            auto aiSpr = ButtonSprite::create("AI", 30, true, "bigFont.fnt",
                                              "GJ_button_04.png", 30.f, 0.6f);
            auto aiBtn = CCMenuItemSpriteExtra::create(
                aiSpr, this, menu_selector(AIEditorUI::onAIButton));
            aiBtn->setID("ai-button"_spr);
            menu->addChild(aiBtn);
            auto mutSpr = ButtonSprite::create("Mut", 30, true, "bigFont.fnt",
                                               "GJ_button_04.png", 30.f, 0.5f);
            auto mutBtn = CCMenuItemSpriteExtra::create(
                mutSpr, this, menu_selector(AIEditorUI::onMutateButton));
            mutBtn->setID("ai-mutate-button"_spr);
            menu->addChild(mutBtn);
            menu->updateLayout();
        }
#endif

        // A generation that finished while no editor existed is waiting:
        // adopt the most recent one whose TARGET LEVEL matches this editor.
        // Any other level's editor changes nothing — the session keeps
        // waiting for its own level.
        GJGameLevel* hereLevel = this->m_editorLayer
            ? this->m_editorLayer->m_level : nullptr;
        for (auto it = genSessions().rbegin(); it != genSessions().rend(); ++it) {
            auto& s = *it;
            if (s->state != GenSession::State::AwaitingEditor || !s->enginePtr) continue;
            if (s->targetLevel && s->targetLevel != hereLevel) continue;
            auto* engine = static_cast<AIGeneratorPopup*>(s->enginePtr);
            log::info("EditorAI: adopting waiting session {} into new editor", s->id);
            engine->adoptEditor(this->m_editorLayer);
            break;  // one blueprint at a time — preview is a singleton
        }

        // Overlay queued a generation — start it only if THIS editor is the
        // level it was queued for; otherwise leave it queued. (level is
        // always set by the producer; warn if that invariant ever breaks.)
        if (s_pendingOverlayGen.active && !s_pendingOverlayGen.level) {
            log::warn("EditorAI: pending overlay gen has no target level - dropping it");
            s_pendingOverlayGen = {};
        }
        if (s_pendingOverlayGen.active && s_pendingOverlayGen.level == hereLevel) {
            auto req = s_pendingOverlayGen;
            s_pendingOverlayGen = {};
            log::info("EditorAI: starting queued overlay generation");
            if (auto* engine = AIGeneratorPopup::create(this->m_editorLayer))
                engine->startHeadless(req.prompt, req.replaceContents);
        }

        // An engineless session (restored, or its engine died) queued an
        // edit follow-up for its level — if THIS is that level, rebuild the
        // engine from the session's chat memory and run the edit now.
        for (auto it = genSessions().rbegin(); it != genSessions().rend(); ++it) {
            auto& s = *it;
            if (!s || s->pendingEdit.empty() || s->enginePtr) continue;
            if (s->targetLevel && s->targetLevel != hereLevel) continue;
            if (!s->targetLevel && hereLevel && !s->targetLevelName.empty()
                && s->targetLevelName != std::string(hereLevel->m_levelName))
                continue;
            std::string text = std::move(s->pendingEdit);
            s->pendingEdit.clear();
            int mode = s->pendingEditMode;
            log::info("EditorAI: resuming session {} edit in its editor", s->id);
            if (auto* engine = AIGeneratorPopup::createForSession(this->m_editorLayer, s))
                engine->resumeFollowUp(text, mode, /*echoUser=*/false);
            break;  // one preview at a time
        }

        // Copilot: watch the level while the user edits; propose fixes when
        // problems appear and the editor has been idle a moment.
        this->schedule(schedule_selector(AIEditorUI::copilotTick), 4.0f);
    }

    // 4 s cadence; all checks are local math until a fix actually fires.
    void copilotTick(float) {
        if (!Mod::get()->getSettingValue<bool>("copilot-mode")) return;
        if (s_inPreviewMode || s_inEditMode) return;
        // Never fire while any generation is in flight — a copilot engine
        // would race the running one for the singleton preview state.
        for (auto& s : genSessions())
            if (s && s->state == GenSession::State::Running) return;
        auto* editor = this->m_editorLayer;
        if (!editor || !editor->m_objects) return;

        auto& f = m_fields;
        int count = editor->m_objects->count();
        if (count < 10) return;                       // nothing to analyze yet
        if (count != f->m_copilotLastCount) {         // user is actively editing
            f->m_copilotLastCount  = count;
            f->m_copilotStableTicks = 0;
            return;
        }
        if (++f->m_copilotStableTicks < 2) return;    // wait ~8 s of idle

        auto now = std::chrono::steady_clock::now();
        if (f->m_copilotLastFix.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(
                now - f->m_copilotLastFix).count() < 120)
            return;                                   // 2-minute fix cooldown

        // Collect a capped object snapshot (same shape as the mutation flow).
        static const std::unordered_map<int, std::string>& idToName = objectIdToName();
        auto arr = matjson::Value::array();
        int added = 0;
        for (auto* raw : CCArrayExt<CCObject*>(editor->m_objects)) {
            if (added >= 600) break;
            auto* gameObj = typeinfo_cast<GameObject*>(raw);
            if (!gameObj) continue;
            auto it = idToName.find(gameObj->m_objectID);
            if (it == idToName.end()) continue;
            auto o = matjson::Value::object();
            o["type"] = it->second;
            o["x"] = (double)gameObj->getPositionX();
            o["y"] = (double)gameObj->getPositionY();
            arr.push(std::move(o));
            ++added;
        }
        if (added < 10) return;

        auto pass = levelcheck::check(arr);
        auto sim  = levelcheck::simulateCube(arr,
            (float)Mod::get()->getSettingValue<int64_t>("ai-ground-y"));
        if (pass.deaths.empty() && sim.deaths.empty()) {
            f->m_copilotLastFingerprint.clear();
            return;                                   // level is healthy
        }

        // Fingerprint the problem set so the same issue never double-fires.
        std::string fp;
        for (auto& d : pass.deaths) fp += fmt::format("z{:.0f};", d.x_start);
        for (auto& d : sim.deaths)  fp += fmt::format("s{:.0f};", d.x);
        if (fp == f->m_copilotLastFingerprint) return;
        f->m_copilotLastFingerprint = fp;
        f->m_copilotLastFix = now;

        std::string problems;
        for (size_t i = 0; i < pass.deaths.size() && i < 4; ++i)
            problems += fmt::format("fully blocked X={:.0f}-{:.0f}; ",
                pass.deaths[i].x_start, pass.deaths[i].x_end);
        for (size_t i = 0; i < sim.deaths.size() && i < 4; ++i)
            problems += fmt::format("cube bot dies at X={:.0f} ({}); ",
                sim.deaths[i].x, sim.deaths[i].reason);

        log::info("Copilot: firing auto-fix for: {}", problems);
        Notification::create("Copilot: proposing a fix for detected problems...",
                             NotificationIcon::Info)->show();

        // Headless engine: mutation-mode generation whose result stages as a
        // normal accept/deny blueprint (stageAllDeferredImmediate path).
        auto* engine = AIGeneratorPopup::create(editor);
        if (!engine) return;
        engine->startCopilotFix(problems);
    }

#ifndef EDITORAI_HAS_IMGUI
    // Classic cocos entry points — only wired (and only compiled) on
    // platforms built without the ImGui overlay.
    void onMutateButton(CCObject*) {
        if (!this->m_editorLayer) return;
        cancelActiveRatingPopup();
        if (s_inPreviewMode || s_inEditMode) {
            FLAlertLayer::create("Preview Active",
                gd::string("Finish the current AI preview first."), "OK")->show();
            return;
        }
        if (!this->m_editorLayer->m_objects || this->m_editorLayer->m_objects->count() == 0) {
            FLAlertLayer::create("Empty Level",
                gd::string("Mutation changes an existing level - build or generate "
                           "something first (or use the AI button)."), "OK")->show();
            return;
        }
        s_openInMutationMode = true;
        AIGeneratorPopup::create(this->m_editorLayer)->show();
    }

    void onAIButton(CCObject*) {
        if (!this->m_editorLayer) {
            FLAlertLayer::create("Error", gd::string("No editor layer found!"), "OK")->show();
            return;
        }

        // If a rating prompt is currently showing, dismiss it so the user can
        // start a fresh generation without first having to rate the previous
        // one. Rating data is simply discarded.
        cancelActiveRatingPopup();

        if (s_inPreviewMode || s_inEditMode) {
            FLAlertLayer::create("Preview Active",
                gd::string(s_inEditMode
                    ? "Please press Done to finish editing first."
                    : "Please accept, edit, or deny the current AI preview first."),
                "OK")->show();
            return;
        }
        AIGeneratorPopup::create(this->m_editorLayer)->show();
    }
#endif  // !EDITORAI_HAS_IMGUI

    // ── Blueprint preview accept/deny UI ─────────────────────────────────────

    // Shared tray scaffolding: GJ_square01 panel + "AI Preview" header +
    // groove. The CCMenu holds ONLY CCMenuItems (CCMenu touch dispatch
    // crashes on non-item children — documented at the settings cycler), so
    // bg/labels live on the parent CCNode, which also carries the
    // "ai-preview-menu"_spr ID the pause/playtest hooks look up.
    CCNode* buildTrayFrame(float trayH, float menuH, CCMenu** outMenu) {
        constexpr float TRAY_W = 96.f;
        auto container = CCNode::create();
        container->setContentSize({TRAY_W, trayH});
        container->setAnchorPoint({0.5f, 0.5f});
        container->ignoreAnchorPointForPosition(false);
        container->setID("ai-preview-menu"_spr);

        auto bg = CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({TRAY_W, trayH});
        bg->setPosition({TRAY_W / 2.f, trayH / 2.f});
        container->addChild(bg);

        auto header = CCLabelBMFont::create("AI Preview", "goldFont.fnt");
        header->setScale(0.35f);
        header->setPosition({TRAY_W / 2.f, trayH - 14.f});
        container->addChild(header);

        auto menu = CCMenu::create();
        menu->setContentSize({TRAY_W, menuH});
        menu->ignoreAnchorPointForPosition(false);
        menu->setAnchorPoint({0.5f, 0.5f});
        menu->setPosition({TRAY_W / 2.f, menuH / 2.f + 8.f});
        container->addChild(menu);
        *outMenu = menu;

        // Entrance: quick back-out settle from 85%.
        container->setScale(0.85f);
        container->runAction(CCEaseBackOut::create(CCScaleTo::create(0.18f, 1.f)));
        return container;
    }

    // Fixed-width tray button so the stack reads as one unit.
    CCMenuItemSpriteExtra* trayButton(const char* lbl, const char* tex,
                                      SEL_MenuHandler sel, std::string const& id) {
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(lbl, 64, true, "goldFont.fnt", tex, 30.f, 0.6f),
            this, sel);
        btn->setID(id);
        return btn;
    }

    void showPreviewButtons() {
        if (m_fields->m_previewButtonMenu) return;

        CCMenu* menu = nullptr;
        // menuH must cover the Why button at y=135 (menu touch rect =
        // contentSize) — 140 gives it margin.
        auto container = buildTrayFrame(162.f, 140.f, &menu);

        // Object-count line under the header, with a "why?" info dot that
        // opens the AI's own plan narration for this generation.
        auto count = CCLabelBMFont::create(
            fmt::format("{} objects", s_previewObjects.size()).c_str(), "chatFont.fnt");
        count->limitLabelWidth(70.f, 0.45f, 0.45f);
        count->setColor(ui::TEXT_SECONDARY);
        count->setPosition({42.f, 135.f});
        container->addChild(count);
        if (!s_lastAINarration.empty()) {
            auto whySpr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
            whySpr->setScale(0.42f);
            auto whyBtn = CCMenuItemSpriteExtra::create(whySpr, this,
                menu_selector(AIEditorUI::onWhyPreview));
            whyBtn->setID("why-btn"_spr);
            whyBtn->setPosition({86.f, 135.f});
            menu->addChild(whyBtn);
        }
        ui::addGroove(container, 76.f, 48.f, 126.f);

        auto acceptBtn = trayButton("Accept", "GJ_button_01.png",
            menu_selector(AIEditorUI::onAcceptPreview), "accept-btn"_spr);
        acceptBtn->setPosition({48.f, 98.f});
        menu->addChild(acceptBtn);

        auto editBtn = trayButton("Edit", "GJ_button_02.png",
            menu_selector(AIEditorUI::onEditPreview), "edit-btn"_spr);
        editBtn->setPosition({48.f, 70.f});
        menu->addChild(editBtn);

        auto denyBtn = trayButton("Deny", "GJ_button_06.png",
            menu_selector(AIEditorUI::onDenyPreview), "deny-btn"_spr);
        denyBtn->setPosition({48.f, 42.f});
        menu->addChild(denyBtn);

        auto exportBtn = trayButton("Export", "GJ_button_04.png",
            menu_selector(AIEditorUI::onExportPreview), "export-btn"_spr);
        exportBtn->setPosition({48.f, 14.f});
        menu->addChild(exportBtn);

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        container->setPosition({70.f, winSize.height - 92.f});
        this->addChild(container, 1000);

        m_fields->m_previewButtonMenu = container;
        log::info("EditorAI: preview accept/deny/edit buttons shown");
    }

    void showDoneButton() {
        removePreviewButtons();

        CCMenu* menu = nullptr;
        auto container = buildTrayFrame(76.f, 40.f, &menu);
        ui::addGroove(container, 76.f, 48.f, 53.f);

        auto doneBtn = trayButton("Done", "GJ_button_01.png",
            menu_selector(AIEditorUI::onDoneEditing), "done-btn"_spr);
        doneBtn->setPosition({48.f, 20.f});
        menu->addChild(doneBtn);

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        container->setPosition({70.f, winSize.height - 50.f});
        this->addChild(container, 1000);

        m_fields->m_previewButtonMenu = container;
        log::info("EditorAI: Done button shown for edit mode");
    }

    void removePreviewButtons() {
        if (m_fields->m_previewButtonMenu) {
            m_fields->m_previewButtonMenu->removeFromParentAndCleanup(true);
            m_fields->m_previewButtonMenu = nullptr;
        }
    }

    void onAcceptPreview(CCObject*) {
        removePlaytestGhost();
        log::info("EditorAI: accepting {} preview objects", s_previewObjects.size());

        // Before accepting new objects, check if the user edited the PREVIOUS
        // accepted generation — if so, update that feedback entry with edit info.
        if (!s_acceptedSnapshot.empty() && m_editorLayer) {
            auto editSummary = computeEditSummary(m_editorLayer);
            if (!editSummary.empty()) {
                log::info("EditorAI: detected user edits on previous generation: {}", editSummary);
                // Update the most recent accepted feedback entry with the edit summary
                auto& entries = loadFeedback();
                for (int i = (int)entries.size() - 1; i >= 0; --i) {
                    if (entries[i].accepted && entries[i].prompt == s_snapshotPrompt) {
                        entries[i].editSummary = editSummary;
                        persistFeedback();
                        log::info("Updated feedback entry with edit summary");
                        break;
                    }
                }
            }
        }

        // Region rebuild: the replacement was accepted — remove the ORIGINAL
        // objects inside the marked range. Preview objects are exempt (the
        // pointer set), so the freshly accepted replacement survives. The
        // deletion itself is not in the undo batch (v1 limitation, logged).
        if (s_pendingRegionDelete.active && m_editorLayer && m_editorLayer->m_objects) {
            std::unordered_set<GameObject*> previewSet;
            previewSet.reserve(s_previewObjects.size());
            for (auto& objRef : s_previewObjects)
                if (GameObject* obj = objRef) previewSet.insert(obj);

            std::vector<GameObject*> toDelete;
            for (auto* raw : CCArrayExt<CCObject*>(m_editorLayer->m_objects)) {
                auto* gameObj = typeinfo_cast<GameObject*>(raw);
                if (!gameObj || previewSet.count(gameObj)) continue;
                float ox = gameObj->getPositionX();
                if (ox >= s_pendingRegionDelete.x0 && ox <= s_pendingRegionDelete.x1)
                    toDelete.push_back(gameObj);
            }
            for (auto* obj : toDelete)
                m_editorLayer->removeObject(obj, true);
            log::info("Region rebuild: deleted {} original objects in X=[{:.0f},{:.0f}] "
                      "(not undoable - the added batch is)",
                      toDelete.size(), s_pendingRegionDelete.x0, s_pendingRegionDelete.x1);
            s_pendingRegionDelete = {};
        }

        // AI edit ops (moves/deletes/restyles of existing objects) become
        // permanent: soft deletes are removed for real, the journal drops.
        finalizeEditOps(m_editorLayer);

        // Restore objects from ghost to solid, then snapshot them as the
        // baseline for future edit tracking
        restorePreviewLayers(m_editorLayer);
        snapshotAcceptedObjects();

        // Register the whole accepted batch as ONE undo step (same command a
        // paste uses). Without this, Ctrl+Z after accepting did nothing —
        // the generation was irreversible, which is scary in edit mode.
        if (m_editorLayer && m_editorLayer->m_undoObjects && !s_previewObjects.empty()) {
            auto batch = CCArray::create();
            for (auto& objRef : s_previewObjects)
                if (GameObject* obj = objRef)
                    if (obj->getParent()) batch->addObject(obj);
            if (batch->count() > 0) {
                if (auto* undo = UndoObject::createWithArray(batch, UndoCommand::Paste)) {
                    m_editorLayer->m_undoObjects->addObject(undo);
                    // GD's own mutation path (handleAction) clears the redo
                    // stack on every new action — mirror that, or a stale
                    // Ctrl+Y could replay an old action on top of the
                    // freshly accepted generation.
                    if (m_editorLayer->m_redoObjects)
                        m_editorLayer->m_redoObjects->removeAllObjects();
                }
            }
        }

        s_previewObjects.clear();
        s_previewIntendedLayers.clear();
        s_inPreviewMode = false;
        removePreviewButtons();

        if (m_editorLayer && m_editorLayer->m_editorUI)
            m_editorLayer->m_editorUI->updateButtons();

        Notification::create("Objects accepted!", NotificationIcon::Success)->show();

        s_lastWasAccepted = true;
        showRatingIfEnabled();
    }

    // Export the staged blueprint as a shareable .eas text file (+clipboard).
    // Source is the accumulated matjson (s_lastGeneratedJson) — richer than
    // reverse-engineering live GameObjects.
    void onExportPreview(CCObject*) {
        if (s_lastGeneratedJson.empty()) {
            Notification::create("Nothing to export yet", NotificationIcon::Warning)->show();
            return;
        }
        auto parsed = matjson::parse(s_lastGeneratedJson);
        if (!parsed || !parsed.unwrap().isArray()) {
            Notification::create("Export failed (unparseable blueprint)",
                                 NotificationIcon::Error)->show();
            return;
        }
        std::string easText = "# EditorAI blueprint export\n";
        easText += eas::objectsToEAS(parsed.unwrap(),
            (float)Mod::get()->getSettingValue<int64_t>("ai-ground-y"));

        auto dir = Mod::get()->getSaveDir() / "exports";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto path = dir / fmt::format("blueprint-{}.eas",
            (long long)std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        // Clipboard immediately (cheap); file write off-thread.
        utils::clipboard::write(easText);
        std::thread([path, easText] {
            auto res = utils::file::writeString(path, easText);
            std::string name = utils::string::pathToString(path.filename());
            Loader::get()->queueInMainThread([ok = res.isOk(), name] {
                Notification::create(
                    ok ? fmt::format("Exported {} (also on clipboard)", name)
                       : "Export write failed (EAS still on clipboard)",
                    ok ? NotificationIcon::Success : NotificationIcon::Warning)->show();
            });
        }).detach();
    }

    // "Why?" — the AI's own plan prose for this generation, captured in
    // processFinalResponse before the script/JSON block was stripped.
    void onWhyPreview(CCObject*) {
        std::string body = s_lastAINarration.empty()
            ? "The AI went straight to output without narrating a plan."
            : s_lastAINarration;
        FLAlertLayer::create(nullptr, "AI's Plan", body, "OK", nullptr, 380.f)->show();
    }

    void onDenyPreview(CCObject*) {
        removePlaytestGhost();
        s_pendingRegionDelete = {};  // replacement denied - originals stay
        // Undo every AI edit op: moved objects return, soft-deleted ones
        // reappear, restyles revert.
        rollbackEditOps(m_editorLayer);
        log::info("EditorAI: denying {} preview objects", s_previewObjects.size());

        if (m_editorLayer) {
            for (auto& objRef : s_previewObjects) {
                if (GameObject* obj = objRef) {
                    // A live parent pointer means the object is still in the
                    // editor (we created it via createObject; anything the
                    // user deleted got detached). This replaces a
                    // containsObject() linear scan of the whole level per
                    // preview object — O(preview × level) on big levels.
                    if (obj->getParent()) {
                        m_editorLayer->removeObject(obj, true);
                    } else {
                        // Object not in editor — just detach from scene graph
                        obj->removeFromParentAndCleanup(true);
                    }
                }
            }
        }

        s_previewObjects.clear();
        s_previewIntendedLayers.clear();
        s_inPreviewMode = false;
        // Objects are gone — just hop the editor back off the preview layer.
        setEditorCurrentLayer(m_editorLayer, s_editorLayerBeforePreview);
        s_previewLayer = -1;
        removePreviewButtons();

        if (m_editorLayer && m_editorLayer->m_editorUI)
            m_editorLayer->m_editorUI->updateButtons();

        Notification::create("Objects denied and removed.", NotificationIcon::Warning)->show();

        s_lastWasAccepted = false;
        showRatingIfEnabled();
    }

    void onEditPreview(CCObject*) {
        removePlaytestGhost();
        log::info("EditorAI: entering edit mode for {} preview objects", s_previewObjects.size());

        // The user is taking over from here — keep the AI's edit ops (they
        // can adjust manually) and make the soft deletes real.
        finalizeEditOps(m_editorLayer);

        // Make objects solid and interactable so the user can edit them, and
        // snapshot positions BEFORE the user edits — this is the baseline
        restorePreviewLayers(m_editorLayer);
        snapshotAcceptedObjects();

        s_previewObjects.clear();
        s_previewIntendedLayers.clear();
        s_inPreviewMode = false;
        s_inEditMode    = true;

        // Replace Accept/Edit/Deny with Done
        showDoneButton();

        if (m_editorLayer && m_editorLayer->m_editorUI)
            m_editorLayer->m_editorUI->updateButtons();

        Notification::create("Edit the objects, then press Done.", NotificationIcon::Info)->show();
    }

    void onDoneEditing(CCObject*) {
        log::info("EditorAI: done editing, computing edit summary");

        s_inEditMode = false;
        removePreviewButtons();

        // Capture the edited objects and compute what changed
        if (!s_acceptedSnapshot.empty() && m_editorLayer) {
            s_lastEditedObjectsJson = captureEditedObjects(m_editorLayer);
            s_lastEditSummary = computeEditSummary(m_editorLayer);
            if (!s_lastEditSummary.empty())
                log::info("EditorAI: user edits: {}", s_lastEditSummary);
            if (!s_lastEditedObjectsJson.empty())
                log::info("EditorAI: captured {} chars of edited objects", s_lastEditedObjectsJson.size());
        } else {
            s_lastEditedObjectsJson.clear();
            s_lastEditSummary.clear();
        }

        if (m_editorLayer && m_editorLayer->m_editorUI)
            m_editorLayer->m_editorUI->updateButtons();

        Notification::create("Edits saved!", NotificationIcon::Success)->show();

        s_lastWasAccepted = true;
        showRatingIfEnabled();
    }
};

// Bridge function: lets AIGeneratorPopup call showPreviewButtons() on EditorUI
// without needing AIEditorUI's definition (which comes after the popup class).
static void showPreviewButtonsOnEditorUI(EditorUI* ui) {
    static_cast<AIEditorUI*>(ui)->showPreviewButtons();
}

// Shows the rating popup if the setting is enabled.
static void showRatingIfEnabled() {
    if (!Mod::get()->getSettingValue<bool>("enable-rating")) return;
    // Rating now lives inline in the overlay's session view (the old
    // RatingPopup is retired). Flag the latest session and nudge.
    for (auto it = genSessions().rbegin(); it != genSessions().rend(); ++it) {
        if ((*it)->state == GenSession::State::Staged ||
            (*it)->state == GenSession::State::Done) {
            auto& s = *it;
            s->needsRating = true;
            s->state = GenSession::State::Done;
            // Snapshot the feedback data NOW, while the s_last* globals
            // still describe this generation — by rating time a newer
            // generation may have overwritten them.
            s->fbPrompt            = s_lastUserPrompt;
            s->fbDifficulty        = s_lastDifficulty;
            s->fbStyle             = s_lastStyle;
            s->fbLength            = s_lastLength;
            s->fbObjectsJson       = s_lastGeneratedJson;
            s->fbEditedObjectsJson = s_lastEditedObjectsJson;
            s->fbEditSummary       = s_lastEditSummary;
            s->fbAccepted          = s_lastWasAccepted;
            break;
        }
    }
    Notification::create(
#ifdef GEODE_IS_MOBILE
        "Rate this generation in the AI panel (AI bubble)",
#else
        "Rate this generation in the AI panel (E)",
#endif
                         NotificationIcon::Info)->show();
}

// ─── EditorPauseLayer hook — restore button visibility on resume ──────────────

class $modify(EditorPauseLayer) {
    bool init(LevelEditorLayer* layer) {
        if (!EditorPauseLayer::init(layer)) return false;
        // Hide the preview accept/deny menu while paused so it doesn't float
        // over the pause UI; onResume restores it.
        if (layer) {
            if (auto editorUI = layer->m_editorUI) {
                if (auto previewMenu = editorUI->getChildByID("ai-preview-menu"_spr))
                    previewMenu->setVisible(false);
            }
        }
        return true;
    }

    // Saving with staged edit ops pending would otherwise HALF-commit them:
    // moves/restyles are already applied to the live objects (and would
    // serialize), but soft deletes are only setVisible(false) — which GD
    // does NOT serialize, so they'd resurrect on the next load. Saving is
    // the user keeping the editor's current contents (staged preview
    // objects persist the same way), so commit the deletes for real.
    void saveLevel() {
        if (!s_editOpJournal.empty() && m_editorLayer) {
            log::info("EditorAI: save with {} pending edit ops - committing "
                      "them (soft deletes would not survive the save)",
                      s_editOpJournal.size());
            finalizeEditOps(m_editorLayer);
        }
        EditorPauseLayer::saveLevel();
    }

    void onResume(CCObject* sender) {
        EditorPauseLayer::onResume(sender);
        if (m_editorLayer) {
            if (auto editorUI = m_editorLayer->m_editorUI) {
                if (auto btn = getAIButton(editorUI))
                    btn->setVisible(true);
                if (auto previewMenu = editorUI->getChildByID("ai-preview-menu"_spr))
                    previewMenu->setVisible(true);
            }
            // The playtest ghost is removed on pause but was never put back.
            if (s_inPreviewMode) {
                for (auto it = genSessions().rbegin(); it != genSessions().rend(); ++it) {
                    auto& s = *it;
                    if (!s || s->state != GenSession::State::Staged || !s->enginePtr)
                        continue;
                    static_cast<AIGeneratorPopup*>(s->enginePtr)->redrawPlaytestGhost();
                    break;
                }
            }
        }
    }
};

// ─── LevelEditorLayer hooks — hide during playtest, show on exit ──────────────

class $modify(AILevelEditorLayer, LevelEditorLayer) {
    void onPlaytest() {
        // Block playtest while ghost objects are awaiting accept/deny/edit
        if (s_inPreviewMode || s_inEditMode) {
            Notification::create(
                s_inEditMode
                    ? "Press Done to finish editing first!"
                    : "Accept, edit, or deny the AI preview first!",
                NotificationIcon::Warning
            )->show();
            return;
        }
        LevelEditorLayer::onPlaytest();
        if (auto editorUI = this->m_editorUI) {
            if (auto btn = getAIButton(editorUI))
                btn->setVisible(false);
        }
    }

    void onStopPlaytest() {
        LevelEditorLayer::onStopPlaytest();
        if (auto editorUI = this->m_editorUI) {
            if (auto btn = getAIButton(editorUI))
                btn->setVisible(true);
            if (auto previewMenu = editorUI->getChildByID("ai-preview-menu"_spr))
                previewMenu->setVisible(true);
        }
    }
};

// ─── Text input bypasses (Eclipse Menu parity) ───────────────────────────────
//
// Two opt-in toggles mirroring Eclipse Menu's Bypass hacks, implemented the
// same way: hook CCTextInputNode::updateLabel — the single place GD applies
// both its allowed-character filter and its max-length truncation — and relax
// those limits just before the original runs.
//
// With both toggles OFF the hook is a pure pass-through, so every text box in
// the game (and this mod's own inputs) behaves exactly as vanilla. With a
// toggle ON the relaxation applies to every CCTextInputNode, which is also
// exactly what Eclipse does. Like Eclipse, turning a toggle back off does not
// re-tighten inputs that were already touched — they reset when recreated.

static bool s_bypassCharFilter = false;
static bool s_bypassCharLimit  = false;

// Tracks whether ANY GD text box currently has keyboard focus — the overlay
// hotkey reads this so 'E' types normally while editing text.
static int s_gdTextInputFocusCount = 0;
bool editoraiIsGDTextInputActive() { return s_gdTextInputFocusCount > 0; }

class $modify(BypassCCTextInputNode, CCTextInputNode) {
    bool onTextFieldAttachWithIME(cocos2d::CCTextFieldTTF* tField) {
        ++s_gdTextInputFocusCount;
        return CCTextInputNode::onTextFieldAttachWithIME(tField);
    }
    bool onTextFieldDetachWithIME(cocos2d::CCTextFieldTTF* tField) {
        if (s_gdTextInputFocusCount > 0) --s_gdTextInputFocusCount;
        return CCTextInputNode::onTextFieldDetachWithIME(tField);
    }

    void updateLabel(gd::string str) {
        if (s_bypassCharFilter) {
            // Same charset Eclipse allows: every printable ASCII character.
            this->setAllowedChars(
                "abcdefghijklmnopqrstuvwxyz"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "0123456789!@#$%^&*()-=_+"
                "`~[]{}/?.>,<\\|;:'\""
                " "
            );
        }
        if (s_bypassCharLimit) {
            this->setMaxLabelLength(99999);
        }
        CCTextInputNode::updateLabel(str);
    }
};

$on_mod(Loaded) {
    // Warm the feedback cache off the main thread — feedback.json can carry
    // multi-hundred-KB level dumps, and the magic-static guard inside
    // loadFeedback() makes a concurrent first call from the main thread
    // simply wait instead of double-loading.
    std::thread([] { loadFeedback(); }).detach();

    s_bypassCharFilter = Mod::get()->getSettingValue<bool>("bypass-char-filter");
    s_bypassCharLimit  = Mod::get()->getSettingValue<bool>("bypass-char-limit");
    listenForSettingChanges<bool>("bypass-char-filter", [](bool value) {
        s_bypassCharFilter = value;
    });
    listenForSettingChanges<bool>("bypass-char-limit", [](bool value) {
        s_bypassCharLimit = value;
    });
}

// ─── Mod startup ─────────────────────────────────────────────────────────────

// ─── Overlay seam implementations (declared in sessions.hpp) ────────────────
void editoraiSendFollowUp(const std::shared_ptr<GenSession>& session,
                          const std::string& text, int mode) {
    if (!session || text.empty()) return;
    if (session->enginePtr) {
        auto* engine = static_cast<AIGeneratorPopup*>(session->enginePtr);
        // A live engine can outlive its editor (user exited and came back).
        // If the session's own level is open RIGHT NOW, re-attach before the
        // turn: otherwise edit follow-ups run blind (no inventory) and the
        // finished result parks "waiting for an editor" while the user is
        // sitting inside the correct one.
        if (auto* lel = LevelEditorLayer::get()) {
            GJGameLevel* here = lel->m_level;
            if (!session->targetLevel || session->targetLevel == here)
                engine->reattachEditor(lel);
        }
        engine->sendFollowUp(text, mode);
        return;
    }

    // No engine (restored session, or its engine died): rebuild one from the
    // session's chat memory — sessions never go read-only.
    auto* lel = LevelEditorLayer::get();
    GJGameLevel* here = lel ? lel->m_level : nullptr;
    bool editorMatches = lel && (
        (session->targetLevel && session->targetLevel == here) ||
        (!session->targetLevel &&
         (session->targetLevelName.empty() ||
          (here && session->targetLevelName == std::string(here->m_levelName)))));

    if (mode == 0 && !editorMatches) {
        // Edit turns need the session's own level open. Park the request —
        // the editor-ready hook resumes it the moment that level opens.
        // Multiple sends stack into ONE combined edit (overwriting would
        // silently drop the earlier message the transcript already shows).
        if (!session->pendingEdit.empty()) session->pendingEdit += "\n\n";
        session->pendingEdit    += text;
        session->pendingEditMode = mode;
        session->state = GenSession::State::AwaitingEditor;
        session->push(GenSession::Entry::Kind::User, text);
        session->push(GenSession::Entry::Kind::Status,
            session->targetLevel || !session->targetLevelName.empty()
                ? "(edit queued - open this session's level to run it; the "
                  "go-to-level button takes you there)"
                : "(edit queued - open the level you want edited)");
        editoraiMarkSessionsDirty();
        return;
    }

    // Plan/chat turns run anywhere (no editor needed); matched edit turns
    // attach to the live editor.
    auto* engine = AIGeneratorPopup::createForSession(mode == 0 ? lel : nullptr,
                                                      session);
    if (!engine) {
        Notification::create("Couldn't restart this session's engine.",
                             NotificationIcon::Error)->show();
        return;
    }
    engine->resumeFollowUp(text, mode);
}

void editoraiCancelSession(const std::shared_ptr<GenSession>& session) {
    if (!session || !session->enginePtr) return;
    static_cast<AIGeneratorPopup*>(session->enginePtr)->requestCancel();
}

std::vector<LocalLevelInfo> editoraiListLocalLevels() {
    std::vector<LocalLevelInfo> out;
    auto* llm = LocalLevelManager::get();
    if (!llm || !llm->m_localLevels) return out;
    for (auto* raw : CCArrayExt<CCObject*>(llm->m_localLevels)) {
        auto* level = typeinfo_cast<GJGameLevel*>(raw);
        if (!level) continue;
        LocalLevelInfo info;
        info.name = level->m_levelName;
        info.objectCount = level->m_objectCount.value();
        out.push_back(std::move(info));
    }
    return out;
}

// ── Manual provider bridge (overlay copy-paste flow) ───────────────────────
// The engine that copied a prompt and is waiting for the user to paste a reply.
// A Ref keeps it alive between Copy and Build even though it's never on-scene.
static Ref<AIGeneratorPopup> s_pendingManual = nullptr;

// Copy step: build the full prompt for the CURRENT editor and put it on the
// clipboard. Manual builds into the open editor, so one must be open.
bool editoraiManualCopy(const std::string& prompt, bool replaceContents,
                        std::string& err) {
    if (prompt.empty()) { err = "Type a prompt first."; return false; }
    if (s_inPreviewMode || s_inEditMode) {
        err = "Finish the current blueprint preview first."; return false;
    }
    auto* editor = LevelEditorLayer::get();
    if (!editor) {
        err = "Open a level first - Manual builds into the current editor.";
        return false;
    }
    auto* engine = AIGeneratorPopup::create(editor);
    if (!engine) { err = "Engine creation failed."; return false; }
    engine->startManualHeadless(prompt, replaceContents);
    s_pendingManual = engine;   // kept alive until Build (also via its session)
    return true;
}

// Is a copied prompt waiting to be built? (overlay shows its Build button)
bool editoraiManualPending() { return s_pendingManual != nullptr; }

// Build step: read the AI's reply from the clipboard and stage it.
bool editoraiManualBuild(std::string& err) {
    if (!s_pendingManual) {
        err = "Nothing to build - press Copy prompt first."; return false;
    }
    AIGeneratorPopup* engine = s_pendingManual;   // Ref -> raw ptr (implicit)
    s_pendingManual = nullptr;   // consume (engine stays alive via its session)
    engine->onManualBuild();
    return true;
}

bool editoraiStartGeneration(int target, const std::string& prompt,
                             bool replaceContents, std::string& err,
                             const std::string& expectedName) {
    if (prompt.empty()) { err = "Type a prompt first."; return false; }
    if (s_inPreviewMode || s_inEditMode) {
        err = "Finish the current blueprint preview first.";
        return false;
    }

    // Current editor: start immediately, headless.
    if (target == -1) {
        auto* editor = LevelEditorLayer::get();
        if (!editor) { err = "No editor open - pick a level or 'New level'."; return false; }
        auto* engine = AIGeneratorPopup::create(editor);
        if (!engine) { err = "Engine creation failed."; return false; }
        engine->startHeadless(prompt, replaceContents);
        return true;
    }

    // New or existing level: resolve the GJGameLevel, queue the request,
    // and enter its editor — generation starts when the editor signals ready.
    // Guards first: an unconditional queue assignment would silently discard
    // a previously queued request (and -2 would orphan a freshly created
    // level if a later guard failed).
    if (s_pendingOverlayGen.active) {
        err = "A generation is already queued - wait for its editor to open.";
        return false;
    }
    if (LevelEditorLayer::get()) {
        err = "Close the current editor first (save & exit), then retry.";
        return false;
    }
    GJGameLevel* level = nullptr;
    if (target == -2) {
        auto* glm = GameLevelManager::sharedState();
        if (!glm) { err = "Level manager unavailable."; return false; }
        level = glm->createNewLevel();
        if (level) level->m_levelName = "AI Level";
    } else {
        auto* llm = LocalLevelManager::get();
        if (!llm || !llm->m_localLevels ||
            target >= (int)llm->m_localLevels->count()) {
            err = "That level no longer exists - refresh the list.";
            return false;
        }
        level = typeinfo_cast<GJGameLevel*>(llm->m_localLevels->objectAtIndex(target));
        // The list can reorder between listing and use (level created or
        // deleted elsewhere) with the count unchanged — re-validate by name
        // and fall back to a name search before trusting the index.
        if (level && !expectedName.empty() &&
            std::string(level->m_levelName) != expectedName) {
            level = nullptr;
            for (auto* raw : CCArrayExt<CCObject*>(llm->m_localLevels)) {
                auto* cand = typeinfo_cast<GJGameLevel*>(raw);
                if (cand && std::string(cand->m_levelName) == expectedName) {
                    level = cand;
                    break;
                }
            }
            if (!level) {
                err = "The level list changed - refresh and pick again.";
                return false;
            }
        }
    }
    if (!level) { err = "Couldn't resolve the target level."; return false; }

    s_pendingOverlayGen = {true, prompt, replaceContents, level};
    // This runs inside the ImGui draw callback (mid-frame). Building the
    // whole editor scene there is heavyweight and fragile — defer to the
    // next main-loop tick instead.
    Ref<GJGameLevel> levelRef = level;
    Loader::get()->queueInMainThread([levelRef] {
        auto* scene = LevelEditorLayer::scene(levelRef, false);
        CCDirector::sharedDirector()->replaceScene(
            CCTransitionFade::create(0.5f, scene));
    });
    log::info("Overlay generation queued; entering editor for '{}'",
              std::string(level->m_levelName));
    return true;
}

bool editoraiGoToSessionLevel(const std::shared_ptr<GenSession>& session) {
    if (!session || !session->targetLevel) return false;
    if (s_gotoTransitionPending) return false;
    // Inert while playing or while a different editor is open — the button
    // renders, but travel mid-level would lose the user's progress.
    if (PlayLayer::get() || LevelEditorLayer::get()) return false;
    s_gotoTransitionPending = true;
    Ref<GJGameLevel> levelRef = session->targetLevel;
    Loader::get()->queueInMainThread([levelRef] {
        if (PlayLayer::get() || LevelEditorLayer::get()) {
            s_gotoTransitionPending = false;
            return;  // something opened in the meantime
        }
        auto* scene = LevelEditorLayer::scene(levelRef, false);
        CCDirector::sharedDirector()->replaceScene(
            CCTransitionFade::create(0.5f, scene));
    });
    return true;
}

// ── Telemetry (opt-in) ──────────────────────────────────────────────────────
// With allow-telemetry ON, EVERY completed generation uploads to the
// Platinum collector automatically: once at completion (user rating 0, AI
// self-rating attached) and once more when the user rates it. Payload is
// prompt + settings + ratings + the generated level — no keys, no identity.
static async::TaskHolder<web::WebResponse> s_shareTask;
static async::TaskHolder<web::WebResponse> s_autoContribTask;
static bool        s_autoContribInFlight = false;
static std::string s_autoContribPending;   // one-slot queue: newest wins

static void autoContribSend(std::string body);  // fwd (self-chaining)

static void autoContribSend(std::string body) {
    s_autoContribInFlight = true;
    auto request = web::WebRequest();
    request.header("Content-Type", "application/json");
    request.timeout(std::chrono::seconds(10));
    request.bodyString(body);
    s_autoContribTask.spawn(
        request.post("http://sn-1.vltgg.net:21800/api/contribute"),
        [](web::WebResponse resp) {
            s_autoContribInFlight = false;
            if (!resp.ok())
                log::info("Telemetry upload skipped (collector said {})", resp.code());
            if (!s_autoContribPending.empty()) {
                std::string next = std::move(s_autoContribPending);
                s_autoContribPending.clear();
                autoContribSend(std::move(next));
            }
        });
}

static void autoContributeGeneration(int userRating) {
    if (!Mod::get()->getSettingValue<bool>("allow-telemetry")) return;
    if (s_lastGeneratedJson.empty() || s_lastUserPrompt.empty()) return;
    auto body = matjson::Value::object();
    body["v"]          = 2;
    body["rating"]     = userRating;          // 0 = not (yet) user-rated
    body["ai_rating"]  = s_lastSelfRating;    // self-review score, 0 if none
    body["prompt"]     = s_lastUserPrompt;
    body["difficulty"] = s_lastDifficulty;
    body["style"]      = s_lastStyle;
    body["length"]     = s_lastLength;
    body["objects"]    = s_lastGeneratedJson;
    std::string dump = body.dump();
    if (s_autoContribInFlight) {
        s_autoContribPending = std::move(dump);  // newest wins; older drop
        return;
    }
    autoContribSend(std::move(dump));
}

void editoraiRateSession(const std::shared_ptr<GenSession>& session, int rating) {
    if (!session || rating < 1 || rating > 10) return;
    session->needsRating = false;
    session->fbRating = rating;      // arms the Share button (8+, opt-in)
    editoraiMarkSessionsDirty();
    FeedbackEntry entry;
    entry.prompt            = session->fbPrompt;
    entry.difficulty        = session->fbDifficulty;
    entry.style             = session->fbStyle;
    entry.length            = session->fbLength;
    entry.objectsJson       = session->fbObjectsJson;
    entry.editedObjectsJson = session->fbEditedObjectsJson;
    entry.editSummary       = session->fbEditSummary;
    entry.rating            = rating;
    entry.accepted          = session->fbAccepted;
    entry.timestamp         = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    saveFeedbackEntry(entry);
    s_lastEditSummary.clear();
    s_lastEditedObjectsJson.clear();
    session->push(GenSession::Entry::Kind::Status,
                  fmt::format("Rated {}/10 - thanks!", rating));

    // Telemetry ON: re-upload with the human verdict attached — built from
    // THIS session's snapshot (the s_last* globals may already describe a
    // newer generation).
    if (Mod::get()->getSettingValue<bool>("allow-telemetry") &&
        !session->fbObjectsJson.empty() && !session->fbPrompt.empty()) {
        auto body = matjson::Value::object();
        body["v"]          = 2;
        body["rating"]     = rating;
        body["ai_rating"]  = 0;
        body["prompt"]     = session->fbPrompt;
        body["difficulty"] = session->fbDifficulty;
        body["style"]      = session->fbStyle;
        body["length"]     = session->fbLength;
        body["objects"]    = session->fbObjectsJson;
        std::string dump = body.dump();
        if (s_autoContribInFlight) s_autoContribPending = std::move(dump);
        else                       autoContribSend(std::move(dump));
        session->fbShared = true;   // collector has it — retire any Share UI
    }
}

std::string editoraiGetStr(const char* id) {
    return Mod::get()->getSettingValue<std::string>(id);
}
void editoraiSetStr(const char* id, const std::string& v) {
    Mod::get()->setSettingValue<std::string>(id, v);
}
bool editoraiGetBool(const char* id) {
    return Mod::get()->getSettingValue<bool>(id);
}
void editoraiSetBool(const char* id, bool v) {
    Mod::get()->setSettingValue<bool>(id, v);
}
int64_t editoraiGetInt(const char* id) {
    return Mod::get()->getSettingValue<int64_t>(id);
}
void editoraiSetInt(const char* id, int64_t v) {
    Mod::get()->setSettingValue<int64_t>(id, v);
}

// ── Saved-value bridge (persists overlay inputs / theme across restarts) ───
std::string editoraiGetSavedStr(const char* key, const std::string& def) {
    return Mod::get()->getSavedValue<std::string>(key, def);
}
void editoraiSetSavedStr(const char* key, const std::string& v) {
    Mod::get()->setSavedValue<std::string>(key, v);
}
int64_t editoraiGetSavedInt(const char* key, int64_t def) {
    return Mod::get()->getSavedValue<int64_t>(key, def);
}
void editoraiSetSavedInt(const char* key, int64_t v) {
    Mod::get()->setSavedValue<int64_t>(key, v);
}

bool editoraiShareSession(const std::shared_ptr<GenSession>& session,
                          std::string& err) {
    if (!session) { err = "No session."; return false; }
    if (session->fbShared) { err = "Already shared."; return false; }
    if (session->fbRating < 8) { err = "Sharing needs a rating of 8+."; return false; }
    if (!Mod::get()->getSettingValue<bool>("allow-telemetry")) {
        err = "Enable the share/telemetry setting first."; return false;
    }
    if (session->fbObjectsJson.empty() || session->fbPrompt.empty()) {
        err = "Nothing to share for this generation."; return false;
    }
    // One share at a time: the holder is shared, and a re-spawn would
    // silently cancel an in-flight upload.
    static bool s_shareInFlight = false;
    if (s_shareInFlight) { err = "A share is already uploading - wait a moment."; return false; }
    s_shareInFlight = true;

    auto body = matjson::Value::object();
    body["v"]          = 1;
    body["rating"]     = session->fbRating;
    body["prompt"]     = session->fbPrompt;
    body["difficulty"] = session->fbDifficulty;
    body["style"]      = session->fbStyle;
    body["length"]     = session->fbLength;
    body["objects"]    = session->fbObjectsJson;

    auto request = web::WebRequest();
    request.header("Content-Type", "application/json");
    request.timeout(std::chrono::seconds(10));
    request.bodyString(body.dump());
    s_shareTask.spawn(
        request.post("http://sn-1.vltgg.net:21800/api/contribute"),
        [session](web::WebResponse resp) {
            s_shareInFlight = false;
            if (resp.ok()) {
                // Only a confirmed upload retires the Share button — a
                // failed share must stay retryable.
                session->fbShared = true;
                editoraiMarkSessionsDirty();
            }
            Notification::create(
                resp.ok() ? "Shared - thanks for improving the model!"
                          : "Share failed (collector unreachable) - try again later",
                resp.ok() ? NotificationIcon::Success
                          : NotificationIcon::Warning)->show();
        });
    return true;
}

// ── Saved (online) levels — example/style pickers ───────────────────────────
std::vector<SavedLevelInfo> editoraiListSavedLevels() {
    std::vector<SavedLevelInfo> out;
    auto* glm = GameLevelManager::sharedState();
    if (!glm) return out;
    auto* arr = glm->getSavedLevels(false, 0);
    if (!arr) return out;
    for (auto* raw : CCArrayExt<CCObject*>(arr)) {
        auto* level = typeinfo_cast<GJGameLevel*>(raw);
        if (!level || level->m_levelID.value() <= 0) continue;
        out.push_back({std::string(level->m_levelName),
                       (int)level->m_levelID.value()});
        if (out.size() >= 200) break;   // plenty for a picker
    }
    return out;
}

// ── Headless OAuth driver (ticked from the overlay's ImGui frame) ──────────
// Reuses the oauth namespace plumbing the old settings popup drove with a
// cocos scheduler; the overlay redraws every frame, so it ticks us instead.
namespace {
struct OAuthDriver {
    std::unique_ptr<oauth::LocalCallback> listener;
    async::TaskHolder<web::WebResponse>   net;
    std::string provider, verifier, state, status;
    int   port = 0;
    float elapsed = 0.f;
    bool  active = false;
    bool  exchanging = false;   // code received, token POST in flight —
                                // the 300 s browser timeout no longer applies
                                // (the request carries its own 30 s timeout)
    void abort(const std::string& why) {
        if (listener) { listener->stop(); listener.reset(); }
        // Holder reset cancels any in-flight token exchange — its callback
        // must never fire after an abort and clobber this status.
        net = {};
        active = false;
        exchanging = false;
        status = why;
    }
};
OAuthDriver& oauthDriver() { static OAuthDriver d; return d; }
constexpr int OPENROUTER_REDIRECT_PORT = 53948;
}

bool editoraiOAuthAvailable(const std::string& provider) {
    return provider == "huggingface" || provider == "openrouter";
}
std::string editoraiOAuthStatus() { return oauthDriver().status; }
bool editoraiOAuthActive()        { return oauthDriver().active; }

bool editoraiOAuthStart(const std::string& provider) {
    auto& d = oauthDriver();
    if (d.active) return false;
    if (!editoraiOAuthAvailable(provider)) {
        d.status = "This provider has no sign-in flow - paste an API key.";
        return false;
    }
    if (provider == "huggingface" && std::string(oauth::HF_CLIENT_ID).empty()) {
        d.status = "No HuggingFace OAuth app registered in this build.";
        return false;
    }
    d.listener = std::make_unique<oauth::LocalCallback>();
    int wantPort = provider == "huggingface" ? oauth::HF_REDIRECT_PORT
                                             : OPENROUTER_REDIRECT_PORT;
    int port = 0;
    if (!d.listener->start(wantPort, port)) {
        d.status = fmt::format(
            "Port {} is busy - close the app using it and retry.", wantPort);
        d.listener.reset();
        return false;
    }
    d.provider = provider;
    d.port     = port;
    d.verifier = oauth::randomVerifier();
    d.state    = oauth::randomVerifier().substr(0, 16);
    std::string redirect  = fmt::format("http://127.0.0.1:{}/cb", port);
    // The redirect goes into a query-string VALUE — RFC 3986 wants its
    // reserved characters percent-encoded.
    std::string redirectEnc;
    for (char ch : redirect) {
        if (ch == ':')      redirectEnc += "%3A";
        else if (ch == '/') redirectEnc += "%2F";
        else                redirectEnc += ch;
    }
    std::string challenge = oauth::s256Challenge(d.verifier);
    std::string url;
    if (provider == "huggingface") {
        url = fmt::format(
            "https://huggingface.co/oauth/authorize?client_id={}"
            "&redirect_uri={}&response_type=code&scope={}"
            "&state={}&code_challenge={}&code_challenge_method=S256",
            oauth::HF_CLIENT_ID, redirectEnc, oauth::HF_SCOPES,
            d.state, challenge);
    } else {
        // OpenRouter's public PKCE endpoint needs no app registration.
        // (No state param: OpenRouter doesn't echo one back. The PKCE
        // challenge binds the code; the residual local-forgery window is
        // a same-machine-only concern.)
        url = fmt::format(
            "https://openrouter.ai/auth?callback_url={}"
            "&code_challenge={}&code_challenge_method=S256",
            redirectEnc, challenge);
    }
    d.elapsed = 0.f;
    d.active  = true;
    d.status  = "Authorise in your browser...";
    web::openLinkInBrowser(url);
    return true;
}

void editoraiOAuthTick(float dt) {
    auto& d = oauthDriver();
    if (!d.active || d.exchanging) return;
    d.elapsed += dt;
    if (d.elapsed > 300.f) {
        d.abort("Sign-in timed out - try again.");
        return;
    }
    if (!d.listener || !d.listener->done()) return;
    std::string q = d.listener->query();
    d.listener->stop();
    d.listener.reset();
    auto getQ = [&](const char* k) -> std::string {
        std::string key = std::string(k) + "=";
        auto pos = q.find(key);
        if (pos == std::string::npos) return "";
        auto end = q.find('&', pos);
        return q.substr(pos + key.size(),
            end == std::string::npos ? std::string::npos
                                     : end - pos - key.size());
    };
    std::string code = getQ("code");
    if (code.empty()) { d.abort("No code returned - try again."); return; }
    if (d.provider == "huggingface" && getQ("state") != d.state) {
        d.abort("State mismatch - try again.");
        return;
    }
    d.status = "Exchanging code...";
    d.exchanging = true;
    if (d.provider == "huggingface") {
        std::string redirect = fmt::format("http://127.0.0.1:{}/cb", d.port);
        std::string body = fmt::format(
            "client_id={}&grant_type=authorization_code&code={}"
            "&redirect_uri={}&code_verifier={}",
            oauth::HF_CLIENT_ID, code, redirect, d.verifier);
        auto req = web::WebRequest();
        req.header("Content-Type", "application/x-www-form-urlencoded");
        req.timeout(std::chrono::seconds(30));
        req.bodyString(body);
        d.net.spawn(req.post("https://huggingface.co/oauth/token"),
            [](web::WebResponse resp) {
                auto& d2 = oauthDriver();
                d2.active = false;
                d2.exchanging = false;
                auto jr = resp.json();
                if (!resp.ok() || !jr) {
                    d2.status = fmt::format("Token exchange failed (HTTP {}).",
                                            resp.code());
                    return;
                }
                const auto json = jr.unwrap();
                std::string t = json["access_token"].asString().unwrapOr("");
                if (t.empty()) { d2.status = "Empty token."; return; }
                oauth::setSavedToken("huggingface", t);
                oauth::setSavedExpiry("huggingface",
                    json["expires_in"].asInt().unwrapOr(0));
                d2.status = "Signed in to HuggingFace!";
                Notification::create("Signed in to HuggingFace!",
                                     NotificationIcon::Success)->show();
            });
    } else {
        auto body = matjson::Value::object();
        body["code"]                  = code;
        body["code_verifier"]         = d.verifier;
        body["code_challenge_method"] = "S256";
        auto req = web::WebRequest();
        req.header("Content-Type", "application/json");
        req.timeout(std::chrono::seconds(30));
        req.bodyString(body.dump());
        d.net.spawn(req.post("https://openrouter.ai/api/v1/auth/keys"),
            [](web::WebResponse resp) {
                auto& d2 = oauthDriver();
                d2.active = false;
                d2.exchanging = false;
                auto jr = resp.json();
                if (!resp.ok() || !jr) {
                    d2.status = fmt::format("Key exchange failed (HTTP {}).",
                                            resp.code());
                    return;
                }
                const auto json = jr.unwrap();
                std::string key = json["key"].asString().unwrapOr("");
                if (key.empty()) { d2.status = "No key in the response."; return; }
                Mod::get()->setSettingValue<std::string>("openrouter-api-key", key);
                d2.status = "Signed in to OpenRouter!";
                Notification::create("Signed in to OpenRouter - key saved!",
                                     NotificationIcon::Success)->show();
            });
    }
}

bool editoraiOpenGenerator() {
    auto* editor = LevelEditorLayer::get();
    if (!editor) return false;
    if (s_inPreviewMode || s_inEditMode) return false;
    AIGeneratorPopup::create(editor)->show();
    return true;
}

$execute {
    log::info("========================================");
    log::info("         Editor AI {}", Mod::get()->getVersion().toVString());
    log::info("========================================");
    log::info("Object library: {} entries ({})", OBJECT_IDS.size(),
        OBJECT_IDS.size() > 10 ? "bundled in .geode" : "defaults only — resource missing!");

    std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
    std::string model    = getProviderModel(provider);
    log::info("Provider: {} | Model: {}", provider, model);

    if (provider == "ollama") {
        bool usePlatinum = Mod::get()->getSettingValue<bool>("use-platinum");
        log::info("Ollama URL: {}", usePlatinum ? "Platinum cloud" : "localhost:11434");
    }

    log::info("Advanced features: {}",
        Mod::get()->getSettingValue<bool>("enable-advanced-features") ? "ON" : "OFF");

    // Warm the reverse ID→name map now (~4k entries) so the first generation
    // doesn't pay the build cost mid-flight.
    (void)objectIdToName();
    log::info("========================================");
}
