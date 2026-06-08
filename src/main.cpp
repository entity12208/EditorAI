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
// decompress gzip-encoded level data fetched from the GD servers. base64.h
// gives us base64Decode for the URL-safe-base64 wrapper around k4.
#include <Geode/cocos/support/zip_support/ZipUtils.h>
#include <Geode/cocos/support/base64.h>
// Auto-generated header containing the bundled few-shot example sections
// as a raw string literal. Embedded directly into the binary so the data
// ships with the mod with zero external dependencies.
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

// True if the character at index i is INSIDE a string literal — i.e. an odd
// number of unescaped double-quotes precede it. O(i) per call so use sparingly.
// Prefer the single-pass state machines below for actual transforms.
inline bool isInsideStringAt(std::string_view s, size_t i) {
    bool inStr = false, esc = false;
    for (size_t k = 0; k < i && k < s.size(); ++k) {
        char c = s[k];
        if (esc) { esc = false; continue; }
        if (inStr) {
            if (c == '\\')      esc = true;
            else if (c == '"')  inStr = false;
        } else if (c == '"') {
            inStr = true;
        }
    }
    return inStr;
}

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
    // 0. strict
    {
        auto p = matjson::parse(std::string(input));
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = input; return r; }
        r.error = "strict parse failed";
    }
    // 1. strip comments
    auto s1 = stripComments(input);
    {
        auto p = matjson::parse(s1);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = s1; r.fixesApplied = 1; return r; }
    }
    // 2. + strip trailing commas
    auto s2 = stripTrailingCommas(s1);
    {
        auto p = matjson::parse(s2);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = s2; r.fixesApplied = 2; return r; }
    }
    // 3. + single→double quotes
    auto s3 = singleToDoubleQuotes(s2);
    {
        auto p = matjson::parse(s3);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = s3; r.fixesApplied = 3; return r; }
    }
    // 4. + quote bare keys
    auto s4 = quoteBareKeys(s3);
    {
        auto p = matjson::parse(s4);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = s4; r.fixesApplied = 4; return r; }
    }
    // 5. + auto-close brackets (last because it's the most destructive)
    auto s5 = autoCloseBrackets(s4);
    {
        auto p = matjson::parse(s5);
        if (p) { r.ok = true; r.value = p.unwrap(); r.transformed = s5; r.fixesApplied = 5; return r; }
    }
    // give up
    r.transformed = s5;
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

// ─── OAuth / device-flow plumbing ────────────────────────────────────────────
//
// One-click sign-in for the providers that actually support OAuth:
//   • Gemini      → device flow (RFC 8628) — no local HTTP server needed.
//   • HuggingFace → OAuth 2.0 + PKCE with loopback redirect.
//   • OpenRouter  → OAuth 2.0 + PKCE with loopback redirect.
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
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
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
// Google (Gemini) OAuth intentionally NOT supported. The only valid Gemini
// scope for generateContent (`cloud-platform`) forces every call to also
// carry an X-Goog-User-Project header naming the user's GCP project — i.e.
// the user has to set up a GCP project AND paste its ID after sign-in. That
// breaks the "click a button, done" promise, so Gemini stays API-key-only.
// (Constants left here as dead anchors in case Google adds a scope without
// the project requirement later.)
inline constexpr const char* GOOGLE_CLIENT_ID     = "";
inline constexpr const char* GOOGLE_CLIENT_SECRET = "";
inline constexpr const char* GOOGLE_SCOPES        = "";

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
        m_port = portOut;
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
    void runOnce() {
        sockaddr_in clientAddr{};
        socklen_compat clen = sizeof clientAddr;
        int client = (int)::accept(m_sock, (sockaddr*)&clientAddr, &clen);
        if (client < 0) { m_done = true; return; }

        // Read the request line. HTTP GET; we only need "GET /cb?<query> HTTP/1.1"
        char buf[4096];
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
    int  m_port = 0;
    std::string m_query;
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;
};

// ── Result types ───────────────────────────────────────────────────────────
struct Result {
    bool ok = false;
    std::string token;          // access token / API key
    std::string refreshToken;   // refresh token (Google only)
    int         expiresIn = 0;  // seconds (Google only; 0 = never)
    std::string error;
};
struct DeviceCode {
    std::string userCode;
    std::string verifyUrl;
    std::string deviceCode;
    int         interval = 5;
    int         expiresIn = 0;
};

// Stored credential helpers — saved value, not setting. Settings stay
// user-editable for the manual key path; OAuth tokens are mod-managed.
// We fully qualify `geode::Mod` because this namespace is defined BEFORE
// the file's `using namespace geode::prelude;` at line ~1600.
inline std::string savedToken(const std::string& provider) {
    return geode::Mod::get()->getSavedValue<std::string>(provider + "-oauth-token");
}
inline void setSavedToken(const std::string& provider, const std::string& token) {
    geode::Mod::get()->setSavedValue<std::string>(provider + "-oauth-token", token);
}
inline void clearSavedToken(const std::string& provider) {
    setSavedToken(provider, "");
    geode::Mod::get()->setSavedValue<std::string>(provider + "-oauth-refresh", "");
    geode::Mod::get()->setSavedValue<int64_t>(provider + "-oauth-expires-at", 0);
}
inline std::string savedRefresh(const std::string& provider) {
    return geode::Mod::get()->getSavedValue<std::string>(provider + "-oauth-refresh");
}
inline int64_t savedExpiresAt(const std::string& provider) {
    return geode::Mod::get()->getSavedValue<int64_t>(provider + "-oauth-expires-at");
}

} // namespace oauth

// Multi-turn tool-use support for every supported AI provider EXCEPT custom.
//
// Architecture:
//   1. We define a small, fixed catalog of tools the AI can call (web_search,
//      download_level, search_newgrounds, get_newgrounds_song, analyze_level,
//      think). Each tool has a JSON schema the providers understand.
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
// The loop is capped at MAX_TOOL_ITERATIONS so a runaway model can't drain
// the user's API quota.
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

namespace toolUse {

// One iteration = one round-trip to the provider. The AI can do up to this
// many tool calls before we force a final answer.
inline constexpr int MAX_TOOL_ITERATIONS = 6;

// Tools the AI can call. Anything outside this list is rejected during dispatch.
inline constexpr std::array<std::string_view, 7> KNOWN_TOOL_NAMES = {
    "web_search",
    "download_level",
    "search_newgrounds",
    "get_newgrounds_song",
    "analyze_level",
    "get_level_length",
    "think",
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
        "think",
        "Record internal reasoning. The mod does nothing with this except log "
        "it; use it to keep your chain of thought clean when you don't need a "
        "specific tool result yet.",
        {{"thought", {"string", "What you're reasoning about right now."}}},
        {"thought"}
    ));
    return arr;
}

// ── Conversation types ─────────────────────────────────────────────────────
struct ToolCall {
    std::string id;    // tool_call_id (used by OpenAI / Claude; we synthesize for Gemini)
    std::string name;  // one of KNOWN_TOOL_NAMES
    matjson::Value args = matjson::Value::object();
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
};

// ── Parsed-response abstraction ────────────────────────────────────────────
struct ParsedResponse {
    bool ok = false;
    std::string finalText;                     // present iff no tool calls
    std::vector<ToolCall> toolCalls;
    std::string assistantTextWithCalls;        // text the assistant emitted alongside the tool calls (rare but possible)
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
                msg["role"]    = "user";
                msg["content"] = m.text;
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
    body["tool_choice"]            = "auto";
    body["max_tokens"]             = 8192;
    body["max_completion_tokens"]  = 16384;
    body["temperature"]            = 0.7;
    // CRITICAL for Ollama: without stream:false, /api/chat returns
    // newline-delimited JSON (one object per chunk), which fails as a single
    // JSON parse. Setting it explicitly is harmless for every other
    // OpenAI-compat provider (they all treat unset and false the same).
    body["stream"] = false;
    return body;
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

    // Tool calls? Both OpenAI and Ollama use `tool_calls` at the message level.
    if (msg.contains("tool_calls") && msg["tool_calls"].isArray()
        && msg["tool_calls"].size() > 0) {
        auto& tcs = msg["tool_calls"];
        for (size_t i = 0; i < tcs.size(); ++i) {
            auto& tc = tcs[i];
            ToolCall call;
            // Ollama sometimes omits the id. Synthesize a stable one so the
            // tool_call → tool_result pairing still works.
            auto idRes = tc["id"].asString();
            call.id = idRes ? idRes.unwrap() : fmt::format("call_{}", i);
            auto fn  = tc["function"];
            auto nameRes = fn["name"].asString();
            call.name = nameRes ? nameRes.unwrap() : "";
            // Arguments can be either a JSON string (OpenAI) or a JSON object
            // (Ollama). Handle both: probe for string first, fall back to
            // copying the object verbatim.
            auto argsField = fn["arguments"];
            if (auto argsStrRes = argsField.asString(); argsStrRes) {
                auto parsed = editorai::json_lenient::parse(argsStrRes.unwrap());
                if (parsed.ok) call.args = std::move(parsed.value);
                // else: keep default empty object
            } else if (argsField.isObject()) {
                call.args = argsField;
            }
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
                    auto argsField = obj["arguments"];
                    if (auto argsStrRes = argsField.asString(); argsStrRes) {
                        auto p = editorai::json_lenient::parse(argsStrRes.unwrap());
                        if (p.ok) call.args = std::move(p.value);
                    } else if (argsField.isObject()) {
                        call.args = argsField;
                    }
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
                    if (obj.contains("arguments")) {
                        auto argsField = obj["arguments"];
                        if (auto argsStrRes = argsField.asString(); argsStrRes) {
                            auto p = editorai::json_lenient::parse(argsStrRes.unwrap());
                            if (p.ok) call.args = std::move(p.value);
                        } else if (argsField.isObject()) {
                            call.args = argsField;
                        }
                    }
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
            msg["role"]    = "user";
            msg["content"] = m.text;   // simple string content
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
        auto fn = catalog[i]["function"];
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
    if (!systemText.empty()) body["system"] = systemText;
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

    // Function declarations
    auto fns = matjson::Value::array();
    auto catalog = buildToolCatalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        fns.push(catalog[i]["function"]);
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

    // Disable thinking budget so 0.7 temperature works
    auto thinkConfig = matjson::Value::object();
    thinkConfig["thinkingBudget"] = 0;
    auto genConfig = matjson::Value::object();
    genConfig["temperature"]     = 0.7;
    genConfig["maxOutputTokens"] = 32768;
    genConfig["thinkingConfig"]  = thinkConfig;
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


// Multi-turn tool-use support (web search / level download / NG search /
// analyze_level / think). One header that knows every provider's tool dialect.

#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/ui/TextInput.hpp>
// NodeIDs utilities: NodeIDs::provideFor, switchToMenu, etc.
#include <Geode/utils/NodeIDs.hpp>

using namespace geode::prelude;

// ─── Logging helper — prevents Geode's spdlog from silently truncating long
//     strings. Splits anything over 1800 characters into numbered chunks.
// ─────────────────────────────────────────────────────────────────────────────
static void logLong(const std::string& label, const std::string& content) {
    constexpr size_t CHUNK = 1800;
    if (content.size() <= CHUNK) {
        log::info("{}: {}", label, content);
        return;
    }
    size_t total = (content.size() + CHUNK - 1) / CHUNK;
    log::info("{} ({} chars, {} parts):", label, content.size(), total);
    for (size_t i = 0; i < content.size(); i += CHUNK) {
        size_t part = i / CHUNK + 1;
        log::info("[{}/{}] {}", part, total, content.substr(i, CHUNK));
    }
}

// ─── Object ID registry ──────────────────────────────────────────────────────

static std::unordered_map<std::string, int> parseObjectIDs(const std::string& jsonContent, const std::string& source) {
    std::unordered_map<std::string, int> ids;
    try {
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
    } catch (std::exception& e) {
        log::error("Error parsing {} object_ids.json: {}", source, e.what());
    }
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
        // ── Player visibility / trail (legacy aliases that the prompt
        //    documentation already promises) ───────────────────────────────
        // These map by ID, not by canonical name — they're game-specific
        // numeric IDs the catalog might not include under any string name.
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
    if (std::filesystem::exists(path)) {
        try {
            std::ifstream file(path);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ids = parseObjectIDs(content, "local file");
        } catch (std::exception& e) {
            log::error("Error reading local object_ids.json: {}", e.what());
        }
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
    return ids;
}

static std::unordered_map<std::string, int> OBJECT_IDS = loadObjectIDs();

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
    // Data is embedded as a raw string literal in example_sections_data.hpp,
    // so there's no disk read and the examples ship with the binary itself.
    // No fallback path, no GitHub fetch, no missing-resource warning — the
    // JSON is part of the compiled mod.
    try {
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
    } catch (std::exception& e) {
        log::error("Exception loading embedded EXAMPLE_SECTIONS_JSON: {}", e.what());
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
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.length() < 6) return false;
    try {
        r = (GLubyte)std::stoi(h.substr(0, 2), nullptr, 16);
        g = (GLubyte)std::stoi(h.substr(2, 2), nullptr, 16);
        b = (GLubyte)std::stoi(h.substr(4, 2), nullptr, 16);
        return true;
    } catch (...) { return false; }
}

// ─── Codec / network helpers (used by the AI's optional tools) ──────────────
// Decode URL-safe base64 (used by GD's level-string encoding). Returns empty
// vector on failure. The input may use - and _ in place of + and /; pad chars
// can be missing.
static std::vector<unsigned char> urlSafeBase64Decode(std::string input) {
    if (input.empty()) return {};
    for (auto& c : input) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (input.size() % 4 != 0) input += '=';
    unsigned char* out = nullptr;
    int len = cocos2d::base64Decode(
        reinterpret_cast<unsigned char*>(const_cast<char*>(input.data())),
        (unsigned int)input.size(), &out);
    if (len <= 0 || !out) return {};
    std::vector<unsigned char> v(out, out + len);
    // cocos2d's base64Decode mallocs the buffer — free with std::free.
    std::free(out);
    return v;
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
    try {
        auto json = matjson::parse(errorBody);
        std::string errorMsg, errorStatus;
        if (json) {
            auto error = json.unwrap();
            // Standard format: {"error": {"message": "...", "status": "..."}}
            if (error.contains("error")) {
                auto errorObj = error["error"];
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
    } catch (...) {
        title = fmt::format("API Error (HTTP {})", statusCode);
        code  = autoErrorCode(10, 98);
        msg = fmtUserError(
            "Something unexpected went wrong while interpreting the provider's error response.",
            "Open Geode log for details and try again.",
            code, errorBody.substr(0, 140));
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

// True iff the active key for `provider` came from an OAuth flow rather than
// a manually-entered API key. Used by the Gemini call path to pick between
// `x-goog-api-key:` (API key) and `Authorization: Bearer` (OAuth).
static bool providerUsesOAuth(const std::string& provider) {
    return !oauth::savedToken(provider).empty();
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

static std::string getOllamaUrl() {
    bool usePlatinum = Mod::get()->getSettingValue<bool>("use-platinum");
    return usePlatinum
        ? "http://sn-1.vltgg.net:21800"
        : "http://localhost:11434";
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
static constexpr float GD_GRID_SIZE      = 30.0f;
static constexpr int   DEFAULT_GROUND_Y  = 105;

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
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(0, 1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back()  == '\r')) s.pop_back();
    return s;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Safe std::stof — returns `dflt` instead of throwing on bad input. Used in
// every positional/value parse so a single garbage token doesn't kill the
// whole EAS line. (Before this helper, std::stof("basic") would throw and the
// outer handle() wrapper would silently drop the entire line. That's why
// `SPIKE basic 105 180` and friends used to vanish on some models.)
inline float tryFloat(const std::string& s, float dflt) {
    if (s.empty()) return dflt;
    try { return std::stof(s); } catch (...) { return dflt; }
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
        if (it == kv.end()) return dflt;
        try { return std::stof(it->second); } catch (...) { return dflt; }
    }
    int inum(const std::string& k, int dflt) const {
        auto it = kv.find(k);
        if (it == kv.end()) return dflt;
        try { return std::stoi(it->second); } catch (...) { return dflt; }
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
            // Bare positional
            out.pos.push_back(tok);
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
    try {
        return { std::stof(s.substr(0, dd)), std::stof(s.substr(dd + 2)), true };
    } catch (...) { return {0, 0, false}; }
}

// Hex string → RGB triplet matjson array. Supports "RRGGBB" with optional "#".
inline matjson::Value hexToRGBArray(const std::string& hex) {
    auto h = hex;
    if (!h.empty() && h.front() == '#') h.erase(0, 1);
    auto arr = matjson::Value::array();
    if (h.size() != 6) { arr.push(255); arr.push(255); arr.push(255); return arr; }
    try {
        int r = std::stoi(h.substr(0, 2), nullptr, 16);
        int g = std::stoi(h.substr(2, 2), nullptr, 16);
        int b = std::stoi(h.substr(4, 2), nullptr, 16);
        arr.push(r); arr.push(g); arr.push(b);
    } catch (...) { arr.push(255); arr.push(255); arr.push(255); }
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
            try { arr.push((double)std::stoi(tok)); } catch (...) {}
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
    if (ln.flag("multi_activate") || ln.flag("multi"))
                                    t["multi_activate"] = true;
    if (ln.kv.count("easing"))      t["easing"]      = (double)ln.inum("easing", 0);
    if (ln.kv.count("easing_rate")) t["easing_rate"] = (double)ln.fnum("easing_rate", 2.f);
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
        if (ln.verb == "saw") {
            // Two accepted shapes (consistent w/ ORB/PAD/PORTAL):
            //   SAW <size> x y       — size as positional, e.g. "SAW small 2550 180"
            //   SAW x y size=<size>  — size as keyword
            std::string size = ln.str("size", "small");
            float x = 0, y = 105;
            if (!ln.pos.empty()) {
                // Probe: is pos[0] numeric (treat as x), or a size word?
                try {
                    x = std::stof(ln.pos[0]);
                    if (ln.pos.size() > 1) {
                        try { y = std::stof(ln.pos[1]); } catch (...) {}
                    } else if (ln.kv.count("y")) {
                        y = ln.fnum("y", 105);
                    }
                } catch (...) {
                    // Non-numeric → it's the size descriptor
                    size = ln.pos[0];
                    if (ln.pos.size() > 1) {
                        try { x = std::stof(ln.pos[1]); } catch (...) {}
                    }
                    if (ln.pos.size() > 2) {
                        try { y = std::stof(ln.pos[2]); } catch (...) {}
                    } else if (ln.kv.count("y")) {
                        y = ln.fnum("y", 105);
                    }
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
            int count = ln.inum("count", 2);
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
                objects.push(t);
            } else if (kind == "end") {
                auto t = triggerObj("effect_10_level_end_trigger", at, 0, ln);
                objects.push(t);
            } else if (kind == "show-player" || kind == "show_player") {
                objects.push(triggerObj("show_player_trigger", at, 0, ln));
            } else if (kind == "hide-player" || kind == "hide_player") {
                objects.push(triggerObj("hide_player_trigger", at, 0, ln));
            } else if (kind == "show-trail" || kind == "show_trail") {
                objects.push(triggerObj("show_trail_trigger", at, 0, ln));
            } else if (kind == "hide-trail" || kind == "hide_trail") {
                objects.push(triggerObj("hide_trail_trigger", at, 0, ln));
            }
            return;
        }
    };
    // Wrap handle to swallow per-line exceptions — one malformed line should
    // not kill the whole parse. The bad line is silently skipped.
    auto handle = [&](const std::string& raw) {
        try { handle_inner(raw); }
        catch (std::exception& e) {
            geode::log::warn("EAS: skipping malformed line '{}': {}", raw, e.what());
        }
        catch (...) {
            geode::log::warn("EAS: skipping malformed line '{}': unknown", raw);
        }
    };
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
        "mirror","copy","trigger",
    };
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        auto t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '#' || (t.size() >= 2 && t[0] == '/' && t[1] == '/')) continue;
        // First real line — check its first word
        size_t sp = t.find_first_of(" \t");
        std::string word = lower(t.substr(0, sp));
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

} // namespace eas

// Scan a free-form text response and extract the LAST valid JSON object
// that looks like our EAI level shape (has "objects", "macros", or
// "level_metadata"). Used after a tool-loop final answer because some models
// emit narration alongside the JSON, or include earlier draft JSON blocks
// before the real one. Returns empty string if nothing usable is found.
static std::string extractLastEAIJsonBlock(const std::string& text) {
    std::string best;
    std::string bestFallback;  // any valid JSON object, if no EAI-shaped one found
    size_t i = 0;
    while (i < text.size()) {
        size_t openPos = text.find('{', i);
        if (openPos == std::string::npos) break;

        // Walk forward with brace counting (string-aware) to find the
        // matching close. Skips quoted strings so they don't disturb depth.
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
            // Unbalanced from this opener; nothing further can be found from before it.
            break;
        }

        std::string candidate = text.substr(openPos, closePos - openPos + 1);
        // Use the lenient parser so the extractor finds JSON blocks even when
        // the model emitted a trailing comma, comment, or single-quoted string.
        auto lenient = editorai::json_lenient::parse(candidate);
        if (lenient.ok) {
            auto& val = lenient.value;
            if (val.isObject()) {
                bool eaiShaped = val.contains("objects")
                              || val.contains("macros")
                              || val.contains("level_metadata");
                if (eaiShaped) best         = candidate;
                else           bestFallback = candidate;
            }
        }
        // Continue past this candidate's interior so we find later ones too.
        i = closePos + 1;
    }
    return !best.empty() ? best : bestFallback;
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

inline float getFloat(const matjson::Value& v, const std::string& key, float dflt) {
    if (!v.contains(key)) return dflt;
    auto d = v[key].asDouble();
    if (d) return (float)d.unwrap();
    auto i = v[key].asInt();
    if (i) return (float)i.unwrap();
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

    // First pass: collect triggers (so we know which groups are "moved away")
    for (size_t i = 0; i < objectsArray.size(); ++i) {
        const auto& o = objectsArray[i];
        if (!o.isObject()) continue;
        auto typeRes = o["type"].asString();
        if (!typeRes) continue;
        std::string type = typeRes.unwrap();
        if (!isMovingTrigger(type)) continue;
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

        float x = getFloat(o, "x", 0.f);
        float y = getFloat(o, "y", 0.f);
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

    // For each column, collect the Y rows that are blocked.
    std::vector<std::set<int>> blockedRows(totalCols);
    for (const auto& b : blockers) {
        float effHalfW = b.half_w + PLAYER_DETECTION_HALF;
        float effHalfH = b.half_h + PLAYER_DETECTION_HALF;
        int colStart = std::max(0, (int)((b.x - effHalfW) / COL_STEP));
        int colEnd   = std::min(totalCols - 1, (int)((b.x + effHalfW) / COL_STEP));
        int rowStart = std::max(0, (int)((b.y - effHalfH - Y_MIN) / ROW_STEP));
        int rowEnd   = std::min(totalRows - 1, (int)((b.y + effHalfH - Y_MIN) / ROW_STEP));
        for (int c = colStart; c <= colEnd; ++c) {
            for (int rr = rowStart; rr <= rowEnd; ++rr) {
                blockedRows[c].insert(rr);
            }
        }
    }

    // Walk columns; a column is "dead" if EVERY row is blocked.
    std::vector<bool> dead(totalCols, false);
    int deadCount = 0;
    for (int c = 0; c < totalCols; ++c) {
        if ((int)blockedRows[c].size() >= totalRows) {
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
    return r;
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
    // Small helpers — matjson's typed accessors return Result<T>, so we wrap
    // them with .unwrapOr-style fallbacks for compact macro code.
    static int   getInt   (const matjson::Value& p, const char* k, int def) {
        auto r = p[k].asInt();    return r ? (int)r.unwrap() : def;
    }
    static float getFloat (const matjson::Value& p, const char* k, float def) {
        auto r = p[k].asDouble(); return r ? (float)r.unwrap() : def;
    }
    static std::string getStr(const matjson::Value& p, const char* k, const char* def) {
        auto r = p[k].asString(); return r ? r.unwrap() : std::string(def);
    }

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
        auto colorsArr = p["colors"];
        if (colorsArr.isArray()) {
            for (size_t i = 0; i < colorsArr.size() && i < 12; ++i) {
                auto cr = colorsArr[i].asString();
                if (cr) colors.push_back(cr.unwrap());
            }
        }
        if (colors.empty()) {
            colors = {"#ff0040", "#ff8000", "#ffff00", "#00ff40", "#00aaff", "#a040ff"};
        }

        for (size_t i = 0; i < colors.size(); ++i) {
            auto o = matjson::Value::object();
            o["type"] = "color_trigger";
            o["x"] = (double)(x + i * spacing);
            o["y"] = (double)getGroundY();
            o["color_channel"] = channel;
            o["color"] = colors[i];
            o["duration"] = (double)duration;
            out.push_back(o);
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
        float unitsPerSec  = SPEEDS[speedTier];
        float secondsPerBeat = 60.f / bpm;
        float unitsPerBeat   = unitsPerSec * secondsPerBeat;
        if (unitsPerBeat < 15.f) unitsPerBeat = 15.f;  // safety floor

        float fadeIn  = std::clamp(getFloat(p, "fade_in",  0.05f), 0.f, 5.f);
        float hold    = std::clamp(getFloat(p, "hold",     0.05f), 0.f, 5.f);
        float fadeOut = std::clamp(getFloat(p, "fade_out", 0.20f), 0.f, 5.f);

        const int maxBeats = 200;
        int produced = 0;
        for (float x = x0; x <= x1 && produced < maxBeats; x += unitsPerBeat, ++produced) {
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
        log::info("beat_sync: BPM={} speed={}x → {:.1f} units/beat → {} pulses over X=[{:.0f},{:.0f}]",
                  bpm, speedTier, unitsPerBeat, produced, x0, x1);
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
        if (name != "copy_paste" && name != "mirror_horizontal") {
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
static std::vector<ccColor3B> s_previewOriginalColors;

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

static std::string s_lastUserPrompt;    // the raw prompt the user typed
static std::string s_lastDifficulty;
static std::string s_lastStyle;
static std::string s_lastLength;
static bool        s_lastWasAccepted = false;
static std::string s_lastGeneratedJson;  // raw JSON objects array from the AI
static std::string s_lastEditSummary;       // edit diff from Edit mode (set by onDoneEditing)
static std::string s_lastEditedObjectsJson; // objects after user edits (set by onDoneEditing)

// Edit tracking: snapshot of accepted objects for implicit feedback
struct AcceptedObjectSnapshot {
    int objectID;
    float x, y;
};
static std::vector<AcceptedObjectSnapshot> s_acceptedSnapshot;
static std::string s_snapshotPrompt;
static std::string s_snapshotDifficulty;
static std::string s_snapshotStyle;
static std::string s_snapshotLength;

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
};

static std::filesystem::path getFeedbackPath() {
    return Mod::get()->getSaveDir() / "feedback.json";
}

static std::vector<FeedbackEntry> loadFeedback() {
    std::vector<FeedbackEntry> entries;
    auto path = getFeedbackPath();
    if (!std::filesystem::exists(path)) return entries;
    try {
        std::ifstream file(path);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto json = matjson::parse(content);
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
            if (!e.prompt.empty() && e.rating >= 1 && e.rating <= 10)
                entries.push_back(std::move(e));
        }
    } catch (std::exception& ex) {
        log::error("Failed to load feedback.json: {}", ex.what());
    }
    return entries;
}

static void saveFeedbackEntry(const FeedbackEntry& entry) {
    auto entries = loadFeedback();
    entries.push_back(entry);

    // Keep at most 50 entries (oldest dropped)
    while (entries.size() > 50)
        entries.erase(entries.begin());

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
        arr.push(obj);
    }

    try {
        std::ofstream file(getFeedbackPath());
        file << arr.dump();
        log::info("Saved feedback entry (rating={}, accepted={})", entry.rating, entry.accepted);
    } catch (std::exception& ex) {
        log::error("Failed to save feedback.json: {}", ex.what());
    }
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
static std::vector<FeedbackEntry> getTopFeedback(int maxCount,
                                                  const std::string& difficulty = "",
                                                  const std::string& style = "",
                                                  const std::string& length = "") {
    auto entries = loadFeedback();

    std::vector<FeedbackEntry> accepted;
    for (auto& e : entries) {
        if (e.accepted && e.rating >= 6)
            accepted.push_back(e);
    }

    // Sort by similarity first (most relevant), then by rating
    bool hasCurrent = !difficulty.empty() || !style.empty() || !length.empty();
    std::sort(accepted.begin(), accepted.end(),
        [&](const FeedbackEntry& a, const FeedbackEntry& b) {
            if (hasCurrent) {
                float simA = feedbackSimilarity(a, difficulty, style, length);
                float simB = feedbackSimilarity(b, difficulty, style, length);
                if (simA != simB) return simA > simB;
            }
            return a.rating > b.rating;
        });

    if ((int)accepted.size() > maxCount)
        accepted.resize(maxCount);
    return accepted;
}

// Returns the N lowest-rated entries with feedback text, prioritized by
// similarity to the current request, then worst-rated first.
static std::vector<FeedbackEntry> getBottomFeedback(int maxCount,
                                                     const std::string& difficulty = "",
                                                     const std::string& style = "",
                                                     const std::string& length = "") {
    auto entries = loadFeedback();

    std::vector<FeedbackEntry> negative;
    for (auto& e : entries) {
        if (e.rating <= 5 && (!e.feedback.empty() || !e.editSummary.empty()))
            negative.push_back(e);
    }

    bool hasCurrent = !difficulty.empty() || !style.empty() || !length.empty();
    std::sort(negative.begin(), negative.end(),
        [&](const FeedbackEntry& a, const FeedbackEntry& b) {
            if (hasCurrent) {
                float simA = feedbackSimilarity(a, difficulty, style, length);
                float simB = feedbackSimilarity(b, difficulty, style, length);
                if (simA != simB) return simA > simB;
            }
            return a.rating < b.rating;
        });

    if ((int)negative.size() > maxCount)
        negative.resize(maxCount);
    return negative;
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
    static std::unordered_map<int, std::string> idToName;
    if (idToName.empty()) {
        for (auto& [name, id] : OBJECT_IDS)
            idToName[id] = name;
    }

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
    CCLabelBMFont* m_ratingLabel     = nullptr;
    CCLabelBMFont* m_feedbackHint    = nullptr;
    TextInput*     m_feedbackInput   = nullptr;

    bool init() {
        if (!Popup::init(380.f, 280.f))
            return false;

        auto winSize = this->m_size;
        this->setTitle("Rate This Generation");

        auto descLabel = CCLabelBMFont::create(
            s_lastWasAccepted ? "How good was this generation?" : "How was this generation?",
            "bigFont.fnt");
        descLabel->setScale(0.4f);
        descLabel->setPosition({winSize.width / 2, winSize.height / 2 + 90});
        m_mainLayer->addChild(descLabel);

        auto hintLabel = CCLabelBMFont::create("Your ratings help the AI learn your style", "bigFont.fnt");
        hintLabel->setScale(0.3f);
        hintLabel->setColor({180, 180, 180});
        hintLabel->setPosition({winSize.width / 2, winSize.height / 2 + 72});
        m_mainLayer->addChild(hintLabel);

        // 1-10 rating buttons in two rows of 5
        auto ratingMenu = CCMenu::create();
        ratingMenu->setPosition({winSize.width / 2, winSize.height / 2 + 38});

        for (int i = 1; i <= 10; ++i) {
            auto label = fmt::format("{}", i);
            auto btn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create(label.c_str(), "bigFont.fnt", "GJ_button_04.png", 0.6f),
                this, menu_selector(RatingPopup::onRatingButton)
            );
            btn->setTag(i);
            float col = (float)((i - 1) % 5) - 2.0f;
            float row = (i <= 5) ? 1.0f : -1.0f;
            btn->setPosition({col * 50.f, row * 22.f});
            ratingMenu->addChild(btn);
            m_ratingButtons.push_back(btn);
        }
        m_mainLayer->addChild(ratingMenu);

        m_ratingLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_ratingLabel->setScale(0.35f);
        m_ratingLabel->setPosition({winSize.width / 2, winSize.height / 2 - 10});
        m_ratingLabel->setVisible(false);
        m_mainLayer->addChild(m_ratingLabel);

        // Feedback hint — changes text based on selected rating
        m_feedbackHint = CCLabelBMFont::create("(optional)", "bigFont.fnt");
        m_feedbackHint->setScale(0.3f);
        m_feedbackHint->setColor({180, 180, 180});
        m_feedbackHint->setPosition({winSize.width / 2, winSize.height / 2 - 28});
        m_mainLayer->addChild(m_feedbackHint);

        // Feedback text input
        auto inputBG = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        inputBG->setContentSize({320, 40});
        inputBG->setColor({0, 0, 0});
        inputBG->setOpacity(100);
        inputBG->setPosition({winSize.width / 2, winSize.height / 2 - 52});
        m_mainLayer->addChild(inputBG);

        m_feedbackInput = TextInput::create(310, "What could be improved?", "bigFont.fnt");
        m_feedbackInput->setPosition({winSize.width / 2, winSize.height / 2 - 52});
        m_feedbackInput->setScale(0.6f);
        // No setMaxCharCount / setAllowedChars — the global CCTextInputNode
        // bypass hook below makes both dead code. Free-text feedback can be
        // any length and any character set.
        m_mainLayer->addChild(m_feedbackInput);

        // Bottom row: [No Feedback] [Submit]
        // - Submit (right, gold/green): saves the chosen rating to feedback.json
        // - No Feedback (left, red): dismisses without saving. The previous
        //   generation's accept/deny still counts; only the rating is skipped.
        auto submitMenu = CCMenu::create();
        submitMenu->setPosition({winSize.width / 2, winSize.height / 2 - 90});

        auto skipBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("No Feedback", "goldFont.fnt", "GJ_button_06.png", 0.55f),
            this, menu_selector(RatingPopup::onSkipFeedback)
        );
        skipBtn->setPosition({-70, 0});
        submitMenu->addChild(skipBtn);

        auto submitBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Submit", "goldFont.fnt", "GJ_button_01.png", 0.7f),
            this, menu_selector(RatingPopup::onSubmit)
        );
        submitBtn->setPosition({60, 0});
        submitMenu->addChild(submitBtn);

        m_mainLayer->addChild(submitMenu);

        return true;
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

        // Highlight selected button, dim others
        for (auto* b : m_ratingButtons) {
            bool selected = (b->getTag() == m_selectedRating);
            b->setColor(selected ? ccColor3B{255, 255, 255} : ccColor3B{150, 150, 150});
            b->setScale(selected ? 1.15f : 1.0f);
        }

        m_ratingLabel->setString(fmt::format("Rating: {}/10", m_selectedRating).c_str());
        m_ratingLabel->setVisible(true);

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
        if (s_activeRatingPopup.data() == static_cast<CCNode*>(this)) {
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
// (device flow for Gemini, PKCE+loopback for HF/OR), test, sign-out.
// One popup, one mental model, no more "open Geode settings".
//
// Visual target: tight rows, small fonts, slim buttons. No chunky goldFont/
// GJ_button_01 spam — only the primary Generate button on the main popup
// wears that.

class AISettingsPopup : public Popup {
public:
    enum class Tab { General, Provider, Advanced };

protected:
    Tab m_tab = Tab::General;
    CCNode* m_content = nullptr;
    std::array<CCMenuItemSpriteExtra*, 3> m_tabBtns{nullptr, nullptr, nullptr};
    // Cyan underline rectangle that slides under the active tab. Replaces
    // the old "swap the sprite" tab indicator — feels more like Eclipse.
    CCLayerColor* m_tabUnderline = nullptr;

    struct TextRow  { std::string sid; TextInput* in; bool password; };
    struct IntRow   { std::string sid; TextInput* in; int64_t min, max, def; };
    // CycleRow now stores a pointer to the value-display label so the
    // arrow handlers can update its text in place — no full tab rebuild
    // on every cycle.
    struct CycleRow { std::string sid; std::vector<std::string> opts; CCLabelBMFont* lbl; };
    std::vector<TextRow>  m_texts;
    std::vector<IntRow>   m_ints;
    std::vector<CycleRow> m_cycles;

    // Provider-tab OAuth flow state (PKCE only — HF / OpenRouter).
    std::unique_ptr<oauth::LocalCallback> m_pkceListener;
    std::string m_pkceVerifier, m_pkceState;
    int         m_pkcePort = 0;
    async::TaskHolder<web::WebResponse> m_authNet;
    CCLabelBMFont* m_authStatus = nullptr;

    // Wider than the old 400 × 280 so the Advanced two-column layout
    // actually has room for labels + values + a comfortable gutter, and
    // every tab gets more breathing room generally.
    static constexpr float W = 480.f;
    static constexpr float H = 290.f;
    float m_rowY = 0.f, m_labelX = 0.f, m_valX = 0.f, m_fieldW = 150.f;
    // Per-tab override of the per-row Y decrement so Advanced can spread
    // its rows out further. buildTab resets to the default each time.
    float m_rowGap = 20.f;

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
        this->setTitle("AI Settings");

        // Inset content surface — GJ_square03.png is one of GD's stock
        // ScaleNine panels (same sprite family used in the comment list and
        // the editor's edit-object panels), so it reads as native chrome
        // rather than improvised. Slightly translucent so the popup's body
        // bleeds through subtly.
        auto surface = CCScale9Sprite::create("GJ_square03.png");
        surface->setContentSize({W - 30.f, H - 80.f});
        surface->setOpacity(220);
        surface->setPosition({W / 2.f, (H - 60.f) / 2.f + 6.f});
        m_mainLayer->addChild(surface);

        // Thin decorative top edge — GD's "table_top" sprite, repeated
        // across the surface width. Gives the inset a finished edge like
        // the GJGarageLayer / MoreOptionsLayer tabs.
        auto topEdge = CCScale9Sprite::create("GJ_table_top_001.png", {0, 0, 18, 12});
        topEdge->setContentSize({W - 50.f, 4.f});
        topEdge->setOpacity(180);
        topEdge->setPosition({W / 2.f, H - 70.f});
        m_mainLayer->addChild(topEdge);

        // Tab strip — flat labels, no per-tab button sprite. The active tab
        // is indicated by a 2-px cyan rule underneath, not a sprite swap.
        auto tabMenu = CCMenu::create();
        tabMenu->setPosition({W / 2.f, H - 50.f});
        for (int i = 0; i < 3; ++i) {
            Tab t = (Tab)i;
            bool active = (t == m_tab);
            // Custom hit target — a tiny invisible sprite + label so the tap
            // area is a clean rectangle without the chunky GJ_button frame.
            auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
            bg->setContentSize({84.f, 22.f});
            bg->setOpacity(0);  // invisible — clickable only
            auto lbl = CCLabelBMFont::create(tabName(t), "bigFont.fnt");
            lbl->setScale(0.28f);  // matches the settings-icon visual weight
            lbl->setColor(active ? ccColor3B{130, 210, 240}
                                 : ccColor3B{160, 175, 200});
            lbl->setPosition({42.f, 11.f});
            bg->addChild(lbl);
            auto btn = CCMenuItemSpriteExtra::create(bg, this,
                menu_selector(AISettingsPopup::onTabSwitch));
            btn->setTag(i);
            btn->setPosition({(float)(i - 1) * 90.f, 0});
            tabMenu->addChild(btn);
            m_tabBtns[i] = btn;
        }
        m_mainLayer->addChild(tabMenu);

        // Cyan underline beneath the active tab — slides on switch.
        m_tabUnderline = CCLayerColor::create({110, 210, 240, 230});
        m_tabUnderline->setContentSize({60.f, 2.f});
        m_tabUnderline->ignoreAnchorPointForPosition(false);
        m_tabUnderline->setAnchorPoint({0.5f, 0.5f});
        m_tabUnderline->setPosition({
            W / 2.f + ((float)(int)m_tab - 1) * 90.f,
            H - 62.f});
        m_mainLayer->addChild(m_tabUnderline);

        m_content = CCNode::create();
        m_content->setPosition({0, 0});
        m_mainLayer->addChild(m_content);

        buildTab();
        return true;
    }

    void onClose(CCObject* o) override { flushInputs(); Popup::onClose(o); }

    void flushInputs() {
        for (auto& r : m_texts)
            geode::Mod::get()->setSettingValue<std::string>(r.sid,
                std::string(r.in->getString()));
        for (auto& r : m_ints) {
            std::string s = r.in->getString();
            int64_t v = r.def;
            if (!s.empty()) { try { v = std::stoll(s); } catch (...) {} }
            v = std::clamp(v, r.min, r.max);
            geode::Mod::get()->setSettingValue<int64_t>(r.sid, v);
        }
    }

    void onTabSwitch(CCObject* sender) {
        flushInputs();
        Tab t = (Tab)static_cast<CCNode*>(sender)->getTag();
        if (t == m_tab) return;
        m_tab = t;
        // Recolor tab labels — active gets cyan, inactive muted.
        for (int i = 0; i < 3; ++i) {
            bool act = ((int)m_tab == i);
            // Find the label child inside the hit-area sprite and recolor it.
            auto bg = m_tabBtns[i]->getNormalImage();
            if (bg) {
                auto children = bg->getChildren();
                if (children && children->count() > 0) {
                    auto lbl = static_cast<CCLabelBMFont*>(children->objectAtIndex(0));
                    if (lbl) lbl->setColor(act ? ccColor3B{130, 210, 240}
                                                : ccColor3B{160, 175, 200});
                }
            }
        }
        // Slide the cyan underline.
        if (m_tabUnderline)
            m_tabUnderline->setPosition({
                W / 2.f + ((float)(int)m_tab - 1) * 90.f,
                H - 62.f});
        buildTab();
    }

    void buildTab() {
        m_content->removeAllChildren();
        m_texts.clear(); m_ints.clear(); m_cycles.clear();
        m_authStatus = nullptr;
        m_rowY   = H - 80.f;
        m_labelX = W / 2.f - 80.f;
        m_valX   = W / 2.f + 50.f;
        m_fieldW = 150.f;
        m_rowGap = 22.f;  // default row pitch — Advanced overrides higher
        switch (m_tab) {
            case Tab::General:  buildGeneral();  break;
            case Tab::Provider: buildProvider(); break;
            case Tab::Advanced: buildAdvanced(); break;
        }
    }

    // Row labels stay right-anchored so they read as "left of the value".
    // Everything else floats centered (status, notes, button text, cycler
    // values). User asked: center-aligned by default, left only when needed.
    //
    // No more compact-row shrinking — the smaller compact dimensions made
    // Geode's TextInput crash when constructed (sub-50 px widths + low
    // scales), and the visual saving wasn't worth the bug. Two-column
    // layouts now achieve density through column positioning, not by
    // shrinking individual widgets.
    void label(const char* t) {
        auto l = CCLabelBMFont::create(t, "bigFont.fnt");
        l->setScale(0.27f);
        l->setColor({195, 210, 230});
        l->setAnchorPoint({1, 0.5f});
        l->setPosition({m_labelX, m_rowY});
        m_content->addChild(l);
    }
    // Eclipse-style toggle pill — flat, slate when off, mint when on.
    static ccColor3B togglePillOn()  { return {110, 220, 180}; }
    static ccColor3B togglePillOff() { return {110, 120, 135}; }

    // Settings-icon-sized buttons everywhere. The settings gear is at 0.25;
    // we match that across cyclers, toggles, action buttons, and tab labels
    // for visual consistency.
    static constexpr float CYCLER_SCALE = 0.25f;
    static constexpr float TOGGLE_SCALE = 0.22f;
    static constexpr float ACTION_SCALE = 0.25f;

    // ── Cycler — ◀ value ▶ (label flanked by two arrow buttons) ──────────
    // Structure:
    //   container (CCNode)         ← parent, sits on m_content
    //     ├── valLbl (CCLabelBMFont)   ← the current value text, centered
    //     └── arrowMenu (CCMenu)       ← ONLY CCMenuItem children (the arrows)
    //          ├── leftBtn (CCMenuItemSpriteExtra)  → onCyclePrev
    //          └── rightBtn (CCMenuItemSpriteExtra) → onCycleNext
    // The earlier attempt put valLbl INSIDE the CCMenu — that crashed
    // because CCMenu's touch dispatch iterates children and assumes they
    // are CCMenuItem. Keeping the label as a sibling of the menu (both
    // inside a plain CCNode) sidesteps that completely.
    void addCycler(const char* lbl, const char* sid,
                   const std::vector<std::string>& opts) {
        label(lbl);
        std::string cur = geode::Mod::get()->getSettingValue<std::string>(sid);
        if (cur.empty() && !opts.empty()) cur = opts[0];
        if (cur.empty()) cur = "—";

        int tag = (int)m_cycles.size();
        m_cycles.push_back({sid, opts, nullptr});

        // 100 × 22 widget area centered at the row's value position.
        constexpr float CW = 100.f, CH = 22.f;
        auto container = CCNode::create();
        container->setContentSize({CW, CH});
        container->setAnchorPoint({0.5f, 0.5f});
        container->ignoreAnchorPointForPosition(false);
        container->setPosition({m_valX, m_rowY});

        // Value label centered within the container.
        auto valLbl = CCLabelBMFont::create(cur.c_str(), "bigFont.fnt");
        valLbl->setScale(0.40f);
        valLbl->setColor({215, 230, 245});
        valLbl->setPosition({CW / 2.f, CH / 2.f});
        container->addChild(valLbl);
        m_cycles.back().lbl = valLbl;

        // Arrow menu — CCMenuItem children only. Same size as the
        // container so touches register on the visible arrows.
        auto arrowMenu = CCMenu::create();
        arrowMenu->setContentSize({CW, CH});
        arrowMenu->ignoreAnchorPointForPosition(false);
        arrowMenu->setAnchorPoint({0.5f, 0.5f});
        arrowMenu->setPosition({CW / 2.f, CH / 2.f});

        auto mkArrow = [this, tag](float rotation, SEL_MenuHandler sel, float xx) {
            auto sprite = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
            if (!sprite) return (CCMenuItemSpriteExtra*)nullptr;
            sprite->setScale(0.30f);
            sprite->setRotation(rotation);
            sprite->setColor({180, 205, 230});
            auto btn = CCMenuItemSpriteExtra::create(sprite, this, sel);
            btn->setTag(tag);
            btn->setPosition({xx, 0.f});
            return btn;
        };
        if (auto* a = mkArrow(180.f, menu_selector(AISettingsPopup::onCyclePrev), -CW / 2.f + 8.f))
            arrowMenu->addChild(a);
        if (auto* a = mkArrow(  0.f, menu_selector(AISettingsPopup::onCycleNext),  CW / 2.f - 8.f))
            arrowMenu->addChild(a);
        container->addChild(arrowMenu);

        m_content->addChild(container);
        m_rowY -= m_rowGap;
    }

    void addToggle(const char* lbl, const char* sid) {
        label(lbl);
        bool v = geode::Mod::get()->getSettingValue<bool>(sid);
        auto spr = ButtonSprite::create(v ? "ON" : "OFF", "bigFont.fnt",
                                        "GJ_button_04.png", TOGGLE_SCALE);
        spr->setColor(v ? togglePillOn() : togglePillOff());
        auto btn = CCMenuItemSpriteExtra::create(spr, this,
            menu_selector(AISettingsPopup::onToggle));
        btn->setUserObject(CCString::create(sid));
        btn->setPosition({0, 0});

        auto menu = CCMenu::create();
        menu->setPosition({m_valX, m_rowY});
        menu->addChild(btn);
        m_content->addChild(menu);
        m_rowY -= m_rowGap;
    }

    // ── Text input — no custom bg, just TextInput's own chrome ─────────────
    void addText(const char* lbl, const char* sid, const char* ph,
                 int maxLen = 200, bool password = false) {
        label(lbl);
        auto in = TextInput::create(105.f, ph, "bigFont.fnt");
        in->setPosition({m_valX, m_rowY});
        in->setScale(0.55f);
        in->setMaxCharCount(maxLen);
        if (password) in->setPasswordMode(true);
        std::string cur = geode::Mod::get()->getSettingValue<std::string>(sid);
        if (!cur.empty()) in->setString(cur);
        m_content->addChild(in);
        m_texts.push_back({sid, in, password});
        m_rowY -= m_rowGap;
    }

    void addInt(const char* lbl, const char* sid,
                int64_t mn, int64_t mx, int64_t df) {
        label(lbl);
        // Materialise the format strings into named locals so the c_str
        // pointer outlives any synchronous internal copy TextInput may do.
        std::string phStr = fmt::format("{}", df);
        auto in = TextInput::create(50.f, phStr.c_str(), "bigFont.fnt");
        in->setPosition({m_valX - 12.f, m_rowY});
        in->setScale(0.55f);
        in->setCommonFilter(geode::CommonFilter::Uint);
        in->setMaxCharCount(7);
        int64_t cur = geode::Mod::get()->getSettingValue<int64_t>(sid);
        in->setString(fmt::format("{}", cur));
        m_content->addChild(in);
        m_ints.push_back({sid, in, mn, mx, df});

        std::string hintStr = fmt::format("{}-{}", mn, mx);
        auto hint = CCLabelBMFont::create(hintStr.c_str(), "bigFont.fnt");
        hint->setScale(0.13f);
        hint->setColor({130, 145, 165});
        hint->setAnchorPoint({0, 0.5f});
        hint->setPosition({m_valX + 18.f, m_rowY});
        m_content->addChild(hint);
        m_rowY -= m_rowGap;
    }
    void addNote(const std::string& txt, ccColor3B col = {130, 145, 170},
                 float sc = 0.18f) {
        auto n = CCLabelBMFont::create(txt.c_str(), "bigFont.fnt");
        n->setScale(sc);
        n->setColor(col);
        n->setPosition({W / 2.f, m_rowY});  // centered
        m_content->addChild(n);
        m_rowY -= 11.f;
    }
    void addInlineAction(const char* lbl, SEL_MenuHandler sel,
                         ccColor3B tint = {130, 155, 180},
                         float scale = ACTION_SCALE) {
        auto menu = CCMenu::create();
        menu->setPosition({W / 2.f, m_rowY});  // centered
        auto spr = ButtonSprite::create(lbl, "bigFont.fnt",
                                        "GJ_button_04.png", scale);
        spr->setColor(tint);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, sel);
        btn->setPosition({0, 0});
        menu->addChild(btn);
        m_content->addChild(menu);
        m_rowY -= 22.f;
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

        if (info.lbl) info.lbl->setString(v.c_str());
        if (info.sid == "ai-provider" && m_tab == Tab::Provider) buildTab();
    }

    void onToggle(CCObject* sender) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto obj = static_cast<CCString*>(btn->getUserObject());
        if (!obj) return;
        std::string sid = obj->getCString();
        bool v = !geode::Mod::get()->getSettingValue<bool>(sid);
        geode::Mod::get()->setSettingValue(sid, v);
        auto parent = btn->getParent();
        auto pos    = btn->getPosition();
        parent->removeChild(btn, true);
        auto spr = ButtonSprite::create(v ? "ON" : "OFF", "bigFont.fnt",
                                        "GJ_button_04.png", TOGGLE_SCALE);
        spr->setColor(v ? togglePillOn() : togglePillOff());
        auto fresh = CCMenuItemSpriteExtra::create(spr, this,
            menu_selector(AISettingsPopup::onToggle));
        fresh->setUserObject(CCString::create(sid.c_str()));
        fresh->setPosition(pos);
        parent->addChild(fresh);
    }

    void buildGeneral() {
        addCycler("Provider", "ai-provider",
            {"gemini","claude","openai","openrouter","ministral","huggingface",
             "deepseek","ollama","lm-studio","llama-cpp","custom"});
        addText  ("Difficulty", "difficulty", "easy/medium/hard/extreme", 30);
        addText  ("Style",      "style",      "modern/retro/flow/memory", 30);
        addText  ("Length",     "length",     "short/medium/long/xl/xxl", 30);
        addInt   ("Max objects","max-objects", 10, 1000000, 500);
        addInt   ("Spawn speed","spawn-batch-size", 1, 100, 8);
        addInt   ("Ground Y",   "ai-ground-y", 15, 300, 105);
        addNote  ("Free-form text is OK for difficulty/style/length.");
    }

    // ── Advanced tab — declarative layout via geode::{Row,Column}Layout ──
    // Each row is a CCMenu (so touch dispatch reaches its button child) sized
    // 200×24 with a Between row layout — label sticks left, value sticks
    // right. Rows are stacked in a ColumnLayout (one per column), and the
    // two columns are arranged side-by-side in a top-level RowLayout. No
    // manual m_labelX/m_valX/m_rowY bookkeeping.

    CCNode* makeAdvColumn() {
        auto col = CCNode::create();
        col->setContentSize({200.f, 200.f});
        col->setAnchorPoint({0.5f, 0.5f});
        col->setLayout(geode::ColumnLayout::create()
            ->setGap(6.f)
            ->setAxisAlignment(geode::AxisAlignment::Center)
            ->setCrossAxisAlignment(geode::AxisAlignment::Center)
            ->setAutoScale(false));
        return col;
    }

    void addAdvToggleRow(CCNode* col, const char* lblText, const char* sid) {
        auto row = CCMenu::create();
        row->setContentSize({190.f, 24.f});
        row->setAnchorPoint({0.5f, 0.5f});
        row->ignoreAnchorPointForPosition(false);
        row->setLayout(geode::RowLayout::create()
            ->setGap(8.f)
            ->setAxisAlignment(geode::AxisAlignment::Between)
            ->setCrossAxisAlignment(geode::AxisAlignment::Center)
            ->setAutoScale(false));

        auto labelNode = CCLabelBMFont::create(lblText, "bigFont.fnt");
        labelNode->setScale(0.32f);
        labelNode->setColor({200, 215, 235});
        row->addChild(labelNode);

        bool v = geode::Mod::get()->getSettingValue<bool>(sid);
        auto spr = ButtonSprite::create(v ? "ON" : "OFF", "bigFont.fnt",
                                         "GJ_button_04.png", 0.45f);
        spr->setColor(v ? togglePillOn() : togglePillOff());
        auto btn = CCMenuItemSpriteExtra::create(spr, this,
            menu_selector(AISettingsPopup::onToggle));
        btn->setUserObject(CCString::create(sid));
        row->addChild(btn);

        row->updateLayout();
        col->addChild(row);
    }

    void addAdvIntRow(CCNode* col, const char* lblText, const char* sid,
                       int64_t mn, int64_t mx, int64_t df) {
        auto row = CCNode::create();
        row->setContentSize({190.f, 24.f});
        row->setAnchorPoint({0.5f, 0.5f});
        row->setLayout(geode::RowLayout::create()
            ->setGap(8.f)
            ->setAxisAlignment(geode::AxisAlignment::Between)
            ->setCrossAxisAlignment(geode::AxisAlignment::Center)
            ->setAutoScale(false));

        auto labelNode = CCLabelBMFont::create(lblText, "bigFont.fnt");
        labelNode->setScale(0.32f);
        labelNode->setColor({200, 215, 235});
        row->addChild(labelNode);

        // input + hint grouped on the right so they stay together
        auto rightGroup = CCNode::create();
        rightGroup->setContentSize({72.f, 22.f});
        rightGroup->setAnchorPoint({0.5f, 0.5f});
        rightGroup->setLayout(geode::RowLayout::create()
            ->setGap(4.f)
            ->setAxisAlignment(geode::AxisAlignment::End)
            ->setCrossAxisAlignment(geode::AxisAlignment::Center)
            ->setAutoScale(false));

        std::string phStr = fmt::format("{}", df);
        auto in = TextInput::create(48.f, phStr.c_str(), "bigFont.fnt");
        in->setScale(0.55f);
        in->setCommonFilter(geode::CommonFilter::Uint);
        in->setMaxCharCount(7);
        int64_t cur = geode::Mod::get()->getSettingValue<int64_t>(sid);
        in->setString(fmt::format("{}", cur));
        rightGroup->addChild(in);
        m_ints.push_back({sid, in, mn, mx, df});

        std::string hintStr = fmt::format("{}-{}", mn, mx);
        auto hint = CCLabelBMFont::create(hintStr.c_str(), "bigFont.fnt");
        hint->setScale(0.16f);
        hint->setColor({130, 145, 165});
        rightGroup->addChild(hint);
        rightGroup->updateLayout();

        row->addChild(rightGroup);
        row->updateLayout();
        col->addChild(row);
    }

    void buildAdvanced() {
        auto twoCol = CCNode::create();
        twoCol->setContentSize({W - 60.f, H - 100.f});
        twoCol->setAnchorPoint({0.5f, 0.5f});
        twoCol->setLayout(geode::RowLayout::create()
            ->setGap(20.f)
            ->setAxisAlignment(geode::AxisAlignment::Center)
            ->setCrossAxisAlignment(geode::AxisAlignment::Center)
            ->setAutoScale(false));
        twoCol->setPosition({W / 2.f, (H - 60.f) / 2.f - 6.f});

        auto leftCol = makeAdvColumn();
        addAdvToggleRow(leftCol, "Compact prompts",  "compact-prompts");
        addAdvToggleRow(leftCol, "Enable AI tools",  "enable-ai-tools");
        addAdvIntRow   (leftCol, "Tool iterations",   "ai-tools-max-iterations", 1, 12, 6);
        addAdvToggleRow(leftCol, "Triggers & colors","enable-advanced-features");
        addAdvToggleRow(leftCol, "Rate limiting",    "enable-rate-limiting");
        leftCol->updateLayout();
        // Wrap the bare column in a subtle bordered frame so the two-
        // column visual reads as two distinct cards rather than two loose
        // groups of widgets. Border is Geode's premade chrome — give it a
        // very dim near-black bg.
        auto leftFrame = geode::Border::create(leftCol, {0, 0, 0, 90},
                                                {210.f, 175.f}, {6.f, 4.f});
        twoCol->addChild(leftFrame);

        // Vertical divider between the two cards. BreakLine is also Geode-
        // provided — a 1-px-thick coloured rule with a clean look.
        auto divider = geode::BreakLine::create(1.f, 150.f,
                                                 {1.f, 1.f, 1.f, 0.15f});
        twoCol->addChild(divider);

        auto rightCol = makeAdvColumn();
        addAdvIntRow   (rightCol, "Rate limit (s)",    "rate-limit-seconds", 1, 60, 3);
        addAdvToggleRow(rightCol, "Show AI output",   "show-ai-output");
        addAdvToggleRow(rightCol, "Rate generations", "enable-rating");
        addAdvIntRow   (rightCol, "Feedback examples","max-feedback-examples", 1, 10, 3);
        addAdvIntRow   (rightCol, "Refinement rounds","refinement-rounds", 0, 10, 3);
        rightCol->updateLayout();
        auto rightFrame = geode::Border::create(rightCol, {0, 0, 0, 90},
                                                 {210.f, 175.f}, {6.f, 4.f});
        twoCol->addChild(rightFrame);

        twoCol->updateLayout();
        m_content->addChild(twoCol);
    }

    void buildProvider() {
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        // Only HuggingFace ships a button-only OAuth that works from a
        // loopback HTTP listener. OpenRouter's PKCE flow rejects non-https
        // callbacks (per their docs — only https on ports 443/3000); Gemini's
        // OAuth needs a GCP project step. Both stay on the paste-key path.
        bool isOAuth  = (p == "huggingface");
        bool isLocal  = (p == "ollama" || p == "lm-studio"  || p == "llama-cpp");
        bool isCustom = (p == "custom");

        addCycler("Provider", "ai-provider",
            {"gemini","claude","openai","openrouter","ministral","huggingface",
             "deepseek","ollama","lm-studio","llama-cpp","custom"});

        if (p == "gemini")
            addCycler("Model", "gemini-model", {"gemini-2.5-flash","gemini-2.5-pro"});
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
        }

        if (!isLocal) {
            std::string keySid = isCustom ? std::string("custom-provider-api-key")
                                          : (p + "-api-key");
            bool hasOAuth = !oauth::savedToken(p).empty();
            const char* ph = hasOAuth ? "(signed in — paste to override)"
                                      : "paste API key";
            addText("API key", keySid.c_str(), ph, 220, true);
        }

        m_authStatus = CCLabelBMFont::create("", "bigFont.fnt");
        m_authStatus->setScale(0.28f);
        m_authStatus->setColor({200, 215, 235});
        m_authStatus->setPosition({W / 2.f, m_rowY - 4.f});
        m_content->addChild(m_authStatus);
        m_rowY -= 18.f;

        // Primary actions tint mint; sign-out tints muted red; everything
        // else stays in the default subtle blue-gray.
        constexpr ccColor3B PRIMARY = {110, 215, 175};
        constexpr ccColor3B DANGER  = {220, 130, 140};
        if (isOAuth)
            addInlineAction("Sign In",
                menu_selector(AISettingsPopup::onSignIn), PRIMARY, 0.34f);
        if (!isLocal && !isCustom)
            addInlineAction("Open key page",
                menu_selector(AISettingsPopup::onOpenKeyPage));
        if (isLocal)
            addInlineAction("Test connection",
                menu_selector(AISettingsPopup::onTestKey), PRIMARY, 0.34f);
        if (!isLocal)
            addInlineAction("Save & test",
                menu_selector(AISettingsPopup::onSaveAndTest));
        if (!oauth::savedToken(p).empty())
            addInlineAction("Sign out",
                menu_selector(AISettingsPopup::onSignOut), DANGER, 0.30f);

        bool hasOAuth  = !oauth::savedToken(p).empty();
        bool hasManual = !isLocal && !isCustom &&
            !geode::Mod::get()->getSettingValue<std::string>(p + "-api-key").empty();
        if      (hasOAuth)  setAuthStatus("✓ Signed in via OAuth.",          {180, 230, 180});
        else if (hasManual) setAuthStatus("Manual API key set.",             {200, 215, 235});
        else if (isLocal)   setAuthStatus("No auth needed — Test to verify.",{200, 215, 235});
        else                setAuthStatus("Not configured.",                 {230, 180, 180});
    }

    void setAuthStatus(const std::string& msg, ccColor3B col) {
        if (!m_authStatus) return;
        m_authStatus->setString(msg.c_str());
        m_authStatus->setColor(col);
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
                          {220, 230, 245});
        }
    }
    void onSaveAndTest(CCObject*) {
        flushInputs();
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string key = geode::Mod::get()->getSettingValue<std::string>(
            p == "custom" ? "custom-provider-api-key" : (p + "-api-key"));
        if (key.empty()) {
            setAuthStatus("No key to test.", {255, 180, 180}); return;
        }
        oauth::clearSavedToken(p);
        setAuthStatus("Saved. Testing...", {220, 230, 245});
        runValidate(p, key);
    }
    void onTestKey(CCObject*) {
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string key = getProviderApiKey(p);
        setAuthStatus("Testing...", {220, 230, 245});
        runValidate(p, key);
    }
    void onSignOut(CCObject*) {
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        oauth::clearSavedToken(p);
        setAuthStatus("Signed out.", {220, 230, 245});
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
        else { setAuthStatus("✓ Saved.", {180, 230, 180}); return; }

        auto req = web::WebRequest();
        req.timeout(std::chrono::seconds(10));
        if (provider == "gemini") {
            req.header("x-goog-api-key", key);
        } else if (provider == "claude") {
            req.header("x-api-key", key);
            req.header("anthropic-version", "2023-06-01");
        } else if (!key.empty()) {
            req.header("Authorization", fmt::format("Bearer {}", key));
        }
        m_authNet.spawn(req.get(url),
            [this](web::WebResponse resp) {
                if (resp.ok()) setAuthStatus("✓ Connected.", {180, 230, 180});
                else setAuthStatus(fmt::format("✗ HTTP {}.", resp.code()),
                                   {255, 180, 180});
            });
    }

    void onSignIn(CCObject*) {
        std::string p = geode::Mod::get()->getSettingValue<std::string>("ai-provider");
        if (p == "huggingface") startPKCE(p);
        else setAuthStatus("This provider has no OAuth flow.", {255, 180, 180});
    }

    void startPKCE(const std::string& provider) {
        if (provider == "huggingface" && std::string(oauth::HF_CLIENT_ID).empty()) {
            setAuthStatus("Add an HF client_id in oauth namespace first.",
                          {255, 180, 180}); return;
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
                oauth::HF_REDIRECT_PORT), {255, 180, 180});
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
        setAuthStatus("Authorise in browser...", {220, 230, 245});
        geode::utils::web::openLinkInBrowser(authUrl);
        this->schedule(schedule_selector(AISettingsPopup::tickPkce), 0.5f);
    }
    void tickPkce(float) {
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
        if (code.empty()) { setAuthStatus("No code returned.", {255, 180, 180}); return; }
        if (state != m_pkceState) {
            setAuthStatus("State mismatch.", {255, 180, 180}); return;
        }
        setAuthStatus("Exchanging code...", {220, 230, 245});
        std::string redirect = fmt::format("http://127.0.0.1:{}/cb", m_pkcePort);
        std::string body = fmt::format(
            "client_id={}&grant_type=authorization_code&code={}"
            "&redirect_uri={}&code_verifier={}",
            oauth::HF_CLIENT_ID, code, redirect, m_pkceVerifier);
        auto req = web::WebRequest();
        req.header("Content-Type", "application/x-www-form-urlencoded");
        req.bodyString(body);
        m_authNet.spawn(req.post("https://huggingface.co/oauth/token"),
            [this](web::WebResponse resp) {
                auto jr = resp.json();
                if (!resp.ok() || !jr) {
                    setAuthStatus(fmt::format("HTTP {}.", resp.code()),
                                  {255, 180, 180}); return;
                }
                std::string t = jr.unwrap()["access_token"].asString().unwrapOr("");
                if (t.empty()) { setAuthStatus("Empty token.", {255, 180, 180}); return; }
                oauth::setSavedToken("huggingface", t);
                setAuthStatus("✓ Signed in.", {180, 230, 180});
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

class AIGeneratorPopup : public Popup {
protected:
    TextInput*               m_promptInput    = nullptr;
    CCLabelBMFont*           m_statusLabel    = nullptr;
    LoadingCircle*           m_loadingCircle  = nullptr;
    CCMenuItemSpriteExtra*   m_generateBtn    = nullptr;
    CCMenuItemSpriteExtra*   m_cancelBtn      = nullptr;
    CCMenuItemToggler*       m_editModeToggle = nullptr;

    // ── Tool-use plumbing (AI-only — not user-facing) ────────────────────
    // These TaskHolders fire the network requests behind the AI's tool calls
    // (web_search, download_level, search_newgrounds, get_newgrounds_song,
    // analyze_level, get_level_length, think). The AI decides when to call
    // each tool via the tool-use loop in runToolLoop; the user never sees an
    // input field for them. Three holders so concurrent calls don't step on
    // each other (though the executor runs them serially anyway).
    async::TaskHolder<web::WebResponse> m_toolListenerLevel;
    async::TaskHolder<web::WebResponse> m_toolListenerNG;
    async::TaskHolder<web::WebResponse> m_toolListenerWeb;

    bool                     m_shouldClearLevel = false;
    bool                     m_isGenerating     = false;
    // When true the popup is in "small edits" mode — a different system
    // prompt is built (additive only, no clearing, conservative changes)
    // and the Generate button reads "Apply Edit".
    bool                     m_editMode         = false;

    LevelEditorLayer*        m_editorLayer    = nullptr;

    async::TaskHolder<web::WebResponse> m_listener;

    std::vector<DeferredObject> m_deferredObjects;
    size_t m_currentObjectIndex = 0;
    bool   m_isCreatingObjects  = false;
    std::chrono::steady_clock::time_point m_generationStartTime;

    // ── init ──────────────────────────────────────────────────────────────────

    bool init(LevelEditorLayer* editorLayer) {
        // Eclipse-style slim layout — 340 × 175. Dark inset surface for the
        // content area, hairline cyan accent under the title, flat buttons
        // throughout. No goldFont anywhere; the Generate button is the
        // brightest thing but still slim. Every member keeps its old name
        // so the rest of the popup wiring is unchanged.
        constexpr float W = 340.f, H = 175.f;
        if (!Popup::init(W, H)) return false;

        m_editorLayer = editorLayer;
        this->setTitle("Editor AI");

        // Thin cyan rule under the title bar.
        auto accent = CCLayerColor::create({110, 200, 230, 100});
        accent->setContentSize({W * 0.76f, 1.f});
        accent->ignoreAnchorPointForPosition(false);
        accent->setAnchorPoint({0.5f, 0.5f});
        accent->setPosition({W / 2.f, H - 24.f});
        m_mainLayer->addChild(accent);

        // Inset content surface — same GJ_square03 stock GD panel used by
        // the settings popup so both popups read as one visual family.
        auto surface = CCScale9Sprite::create("GJ_square03.png");
        surface->setContentSize({W - 24.f, H - 80.f});
        surface->setOpacity(220);
        surface->setPosition({W / 2.f, H / 2.f - 16.f});
        m_mainLayer->addChild(surface);

        // Decorative top-edge strip — GD's table_top sprite.
        auto topEdge = CCScale9Sprite::create("GJ_table_top_001.png", {0, 0, 18, 12});
        topEdge->setContentSize({W - 40.f, 4.f});
        topEdge->setOpacity(180);
        topEdge->setPosition({W / 2.f, H - 36.f});
        m_mainLayer->addChild(topEdge);

        // ── Provider chip — flat row, clickable, slides to settings ──────
        {
            std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
            std::string model    = getProviderModel(provider);
            bool local     = (provider == "ollama" || provider == "lm-studio" ||
                              provider == "llama-cpp");
            bool hasToken  = !oauth::savedToken(provider).empty();
            bool hasManual = !Mod::get()->getSettingValue<std::string>(
                              provider + "-api-key").empty();
            bool hasAny    = hasToken || hasManual || provider == "custom";
            ccColor3B dot  = local  ? ccColor3B{255, 195, 90}
                           : hasAny ? ccColor3B{110, 220, 160}
                                    : ccColor3B{220, 120, 130};
            std::string text = fmt::format("{} · {}{}",
                provider, model, hasToken ? " ✓" : "");

            auto chipMenu = CCMenu::create();
            chipMenu->setPosition({W / 2.f, H - 38.f});

            auto container = CCNode::create();
            container->setContentSize({160.f, 12.f});
            auto dotLbl = CCLabelBMFont::create("●", "bigFont.fnt");
            dotLbl->setScale(0.32f);
            dotLbl->setColor(dot);
            dotLbl->setAnchorPoint({0.f, 0.5f});
            dotLbl->setPosition({0.f, 6.f});
            container->addChild(dotLbl);
            auto txtLbl = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
            txtLbl->setScale(0.27f);
            txtLbl->setColor({185, 200, 225});
            txtLbl->setAnchorPoint({0.f, 0.5f});
            txtLbl->setPosition({10.f, 6.f});
            container->addChild(txtLbl);

            auto chipBtn = CCMenuItemSpriteExtra::create(container, this,
                menu_selector(AIGeneratorPopup::onAccount));
            chipBtn->setPosition({0, 0});
            chipMenu->addChild(chipBtn);
            m_mainLayer->addChild(chipMenu);
        }

        // ── Prompt input row (slimmer than before) ───────────────────────
        auto inputBG = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        inputBG->setContentSize({258.f, 24.f});
        inputBG->setColor({4, 8, 14});
        inputBG->setOpacity(200);
        inputBG->setPosition({W / 2.f - 14.f, H / 2.f + 6.f});
        m_mainLayer->addChild(inputBG);

        m_promptInput = TextInput::create(245, "describe your level",
                                          "bigFont.fnt");
        m_promptInput->setPosition({W / 2.f - 14.f, H / 2.f + 6.f});
        m_promptInput->setScale(0.45f);
        m_mainLayer->addChild(m_promptInput);

        // History arrows inline on the right.
        auto historyMenu = CCMenu::create();
        historyMenu->setPosition({W / 2.f + 124.f, H / 2.f + 6.f});
        auto mkArrow = [this](float rot, SEL_MenuHandler sel, float yy) {
            auto btn = CCMenuItemSpriteExtra::create(
                [rot]{ auto s = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
                       s->setScale(0.22f); s->setRotation(rot);
                       s->setColor({170, 190, 215}); return s; }(),
                this, sel);
            btn->setPosition({0, yy});
            return btn;
        };
        historyMenu->addChild(mkArrow(-90, menu_selector(AIGeneratorPopup::onHistoryUp),    7));
        historyMenu->addChild(mkArrow( 90, menu_selector(AIGeneratorPopup::onHistoryDown), -7));
        m_mainLayer->addChild(historyMenu);

        // ── Edit-mode + status row ───────────────────────────────────────
        auto editLabel = CCLabelBMFont::create("Edit existing", "bigFont.fnt");
        editLabel->setScale(0.30f);
        editLabel->setColor({200, 215, 235});

        auto onSpr  = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        onSpr->setScale(0.40f);
        offSpr->setScale(0.40f);
        m_editModeToggle = CCMenuItemToggler::create(offSpr, onSpr, this,
            menu_selector(AIGeneratorPopup::onToggleEditMode));
        m_editModeToggle->toggle(m_editMode);

        auto toggleMenu = CCMenu::create();
        toggleMenu->setPosition({W / 2.f - 80.f, H / 2.f - 18.f});
        m_editModeToggle->setPosition({0.f, 0.f});
        editLabel->setAnchorPoint({0.f, 0.5f});
        editLabel->setPosition({10.f, 0.f});
        toggleMenu->addChild(m_editModeToggle);
        toggleMenu->addChild(editLabel);
        m_mainLayer->addChild(toggleMenu);

        m_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_statusLabel->setScale(0.24f);
        m_statusLabel->setColor({170, 190, 215});
        m_statusLabel->setPosition({W / 2.f, H / 2.f - 32.f});
        m_statusLabel->setVisible(false);
        m_mainLayer->addChild(m_statusLabel);

        // ── Action row (Generate / Cancel) ───────────────────────────────
        auto btnMenu = CCMenu::create();
        btnMenu->setPosition({W / 2.f, 20.f});

        // Settings-icon scale class — Generate gets a tiny bump as the
        // primary action (0.30 vs 0.25) but reads at roughly the same
        // visual weight as the gear and info icons in the corners.
        auto genSpr = ButtonSprite::create("Generate", "bigFont.fnt",
                                           "GJ_button_01.png", 0.30f);
        genSpr->setColor({120, 230, 180});
        m_generateBtn = CCMenuItemSpriteExtra::create(genSpr, this,
            menu_selector(AIGeneratorPopup::onGenerate));
        m_generateBtn->setPosition({0, 0});
        btnMenu->addChild(m_generateBtn);

        auto cancelSpr = ButtonSprite::create("Cancel", "bigFont.fnt",
                                              "GJ_button_04.png", 0.26f);
        cancelSpr->setColor({220, 130, 140});
        m_cancelBtn = CCMenuItemSpriteExtra::create(cancelSpr, this,
            menu_selector(AIGeneratorPopup::onCancel));
        m_cancelBtn->setPosition({0, 0});
        m_cancelBtn->setVisible(false);
        btnMenu->addChild(m_cancelBtn);
        m_mainLayer->addChild(btnMenu);

        // ── Corner buttons (gear left, info right) ───────────────────────
        auto cornerMenu = CCMenu::create();
        cornerMenu->setPosition({W - 14.f, H - 14.f});
        auto infoBtn = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
                s->setScale(0.40f); s->setColor({180, 200, 220}); return s; }(),
            this, menu_selector(AIGeneratorPopup::onInfo));
        infoBtn->setPosition({0, 0});
        cornerMenu->addChild(infoBtn);
        m_mainLayer->addChild(cornerMenu);

        // Settings gear sits in the bottom-left corner — out of the way of
        // the Generate button but still one click from anywhere on the popup.
        auto settingsMenu = CCMenu::create();
        settingsMenu->setPosition({14.f, 14.f});
        auto settingsBtn = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
                s->setScale(0.25f); s->setColor({180, 200, 220}); return s; }(),
            this, menu_selector(AIGeneratorPopup::onSettings));
        settingsBtn->setPosition({0, 0});
        settingsMenu->addChild(settingsBtn);
        m_mainLayer->addChild(settingsMenu);

        this->schedule(schedule_selector(AIGeneratorPopup::updateObjectCreation), 0.05f);
        return true;
    }

    // ── UI callbacks ──────────────────────────────────────────────────────────

    // Edit-mode toggle replaces the legacy Clear-Level toggle. Inverted
    // semantics: m_editMode = true ⇒ keep level + add edits; false ⇒ start fresh.
    // m_shouldClearLevel is now a derived view (== !m_editMode) so the rest of
    // the popup keeps working unchanged.
    void onToggleEditMode(CCObject*) {
        m_editMode = !m_editMode;
        m_shouldClearLevel = !m_editMode;
        log::info("Edit-mode toggle: {} (clear-level derived: {})",
                  m_editMode ? "ON (additive)" : "OFF (fresh)",
                  m_shouldClearLevel ? "ON" : "OFF");
        // Retitle so the user sees the active mode reflected.
        this->setTitle(m_editMode ? "Editor AI — Edit Mode" : "Editor AI");
    }

    void onCancel(CCObject*) {
        if (!m_isGenerating) return;
        m_listener = {};  // destroy the task holder, cancelling the request
        m_isGenerating = false;
        if (m_loadingCircle) {
            m_loadingCircle->fadeAndRemove();
            m_loadingCircle = nullptr;
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

    void showStatus(const std::string& msg, bool error = false) {
        m_statusLabel->setString(msg.c_str());
        m_statusLabel->setColor(error ? ccColor3B{255, 100, 100} : ccColor3B{100, 255, 100});
        m_statusLabel->setVisible(true);
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

        auto toRemove = CCArray::create();
        for (auto* obj : CCArrayExt<CCObject*>(objects))
            toRemove->addObject(obj);

        for (auto* gameObj : CCArrayExt<GameObject*>(toRemove))
            m_editorLayer->removeObject(gameObj, true);

        log::info("Cleared {} objects from editor", toRemove->count());
    }

    // Serialize the current editor objects to compact JSON so the AI can see what
    // is already in the level. Capped at 300 objects to keep the prompt manageable.
    // NOTE: Only called when m_shouldClearLevel is false (building on top of existing).
    std::string buildLevelDataJson() {
        if (!m_editorLayer || !m_editorLayer->m_objects)
            return "{\"object_count\":0,\"objects\":[]}";

        auto objects = m_editorLayer->m_objects;
        if (objects->count() == 0)
            return "{\"object_count\":0,\"objects\":[]}";

        std::unordered_map<int, std::string> idToName;
        idToName.reserve(OBJECT_IDS.size());
        for (auto& [name, id] : OBJECT_IDS) {
            if (idToName.find(id) == idToName.end())
                idToName[id] = name;
        }

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

    // ── Progressive object spawner ────────────────────────────────────────────

    void updateObjectCreation(float /*dt*/) {
        if (!m_isCreatingObjects || m_deferredObjects.empty()) return;

        if (m_currentObjectIndex >= m_deferredObjects.size()) {
            m_isCreatingObjects = false;

            // Enter blueprint preview mode — ghost objects are placed,
            // user must accept or deny via buttons on the editor UI.
            s_inPreviewMode = true;

            if (m_editorLayer && m_editorLayer->m_editorUI) {
                m_editorLayer->m_editorUI->updateButtons();
                showPreviewButtonsOnEditorUI(m_editorLayer->m_editorUI);
            }

            m_generateBtn->setEnabled(true);
            showStatus(fmt::format("Preview: {} objects", s_previewObjects.size()), false);
            Notification::create(
                fmt::format("Preview: {} ghost objects — accept or deny in editor",
                    s_previewObjects.size()),
                NotificationIcon::Info
            )->show();

            this->runAction(CCSequence::create(
                CCDelayTime::create(1.5f),
                CCCallFunc::create(this, callfunc_selector(AIGeneratorPopup::closePopup)),
                nullptr
            ));

            m_deferredObjects.clear();
            m_currentObjectIndex = 0;
            return;
        }

        int batchSize = (int)Mod::get()->getSettingValue<int64_t>("spawn-batch-size");
        for (int b = 0; b < batchSize && m_currentObjectIndex < m_deferredObjects.size(); ++b) {
            try {
                auto& deferred = m_deferredObjects[m_currentObjectIndex];

                if (!m_editorLayer) {
                    log::error("Editor layer destroyed during object creation!");
                    m_isCreatingObjects = false;
                    return;
                }

                GameObject* gameObj = nullptr;
                try {
                    gameObj = m_editorLayer->createObject(deferred.objectID, deferred.position, false);
                } catch (...) {
                    log::warn("Exception creating object at index {}", m_currentObjectIndex);
                    ++m_currentObjectIndex;
                    continue;
                }

                if (!gameObj || !gameObj->m_objectID) {
                    ++m_currentObjectIndex;
                    continue;
                }

                applyObjectProperties(gameObj, deferred.data);

                // Blueprint preview: save original color, apply ghost style
                s_previewOriginalColors.push_back(gameObj->getColor());
                gameObj->setOpacity(102);              // ~40% opacity
                gameObj->setColor({100, 180, 255});    // blue tint
                s_previewObjects.emplace_back(gameObj);

            } catch (...) {
                log::error("Unknown exception in updateObjectCreation at index {}", m_currentObjectIndex);
            }
            ++m_currentObjectIndex;
        }

        if (m_currentObjectIndex % 10 == 0) {
            float pct = (float)m_currentObjectIndex / (float)m_deferredObjects.size() * 100.0f;
            showStatus(fmt::format("Creating objects... {:.0f}%", pct), false);
        }
    }

    void applyObjectProperties(GameObject* gameObj, matjson::Value& objData) {
        if (!gameObj) return;
        bool advFeatures = Mod::get()->getSettingValue<bool>("enable-advanced-features");

        // Merge in any block_template properties for this object's type FIRST,
        // so they act as defaults the per-object fields can override. Mutates
        // objData in place; safe because we only fill in missing keys.
        int templateMerged = applyBlockTemplateToObject(objData);
        if (templateMerged > 0) {
            log::info("Applied block template ({} props) to object id {}",
                      templateMerged, gameObj->m_objectID);
        }

        try {
            // ── Basic transform ───────────────────────────────────────────────
            auto rotResult = objData["rotation"].asDouble();
            if (rotResult) {
                float r = static_cast<float>(rotResult.unwrap());
                if (r >= -360.0f && r <= 360.0f) gameObj->setRotation(r);
            }
            auto scaleResult = objData["scale"].asDouble();
            if (scaleResult) {
                float s = static_cast<float>(scaleResult.unwrap());
                if (s >= 0.1f && s <= 10.0f) gameObj->setScale(s);
            }
            auto flipXResult = objData["flip_x"].asBool();
            if (flipXResult && flipXResult.unwrap())
                gameObj->setScaleX(-gameObj->getScaleX());

            auto flipYResult = objData["flip_y"].asBool();
            if (flipYResult && flipYResult.unwrap())
                gameObj->setScaleY(-gameObj->getScaleY());

            // ── Group IDs (advanced features) ─────────────────────────────────
            if (advFeatures && objData.contains("groups") && objData["groups"].isArray()) {
                auto& groupsArr = objData["groups"];
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
            auto baseColorResult = objData["color_channel"].asInt();
            if (baseColorResult) {
                int ch = std::clamp((int)baseColorResult.unwrap(), 1, 1010);
                if (gameObj->m_baseColor) {
                    gameObj->m_baseColor->m_colorID = ch;
                }
            }

            auto detailColorResult = objData["detail_color_channel"].asInt();
            if (detailColorResult) {
                int ch = std::clamp((int)detailColorResult.unwrap(), 1, 1010);
                if (gameObj->m_detailColor) {
                    gameObj->m_detailColor->m_colorID = ch;
                }
            }

            // ── Multi-activate (orbs, pads, triggers, portals) ─────────────
            if (advFeatures) {
                auto multiResult = objData["multi_activate"].asBool();
                if (multiResult && multiResult.unwrap()) {
                    auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                    if (effectObj) {
                        effectObj->m_isMultiTriggered = true;
                    }
                }
            }

            // ── Color Trigger properties (advanced features, ID 899) ───────────
            if (advFeatures && gameObj->m_objectID == 899) {
                auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                if (!effectObj) return;

                auto channelResult = objData["color_channel"].asInt();
                if (channelResult) {
                    effectObj->m_targetColor = std::clamp((int)channelResult.unwrap(), 1, 1010);
                }

                auto colorHexResult = objData["color"].asString();
                if (colorHexResult) {
                    GLubyte r = 255, g = 255, b = 255;
                    if (parseHexColor(colorHexResult.unwrap(), r, g, b)) {
                        effectObj->m_triggerTargetColor = {r, g, b};
                    }
                }

                auto durResult = objData["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                auto blendResult = objData["blending"].asBool();
                if (blendResult) {
                    effectObj->m_usesBlending = blendResult.unwrap();
                }

                auto opacityResult = objData["opacity"].asDouble();
                if (opacityResult) {
                    effectObj->m_opacity = std::clamp(
                        static_cast<float>(opacityResult.unwrap()), 0.0f, 1.0f);
                }

                effectObj->m_isTouchTriggered = true;
            }

            // ── Move Trigger properties (advanced features, ID 901) ────────────
            if (advFeatures && gameObj->m_objectID == 901) {
                auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                if (!effectObj) return;

                auto targetGroupResult = objData["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto moveXResult = objData["move_x"].asDouble();
                auto moveYResult = objData["move_y"].asDouble();
                float offsetX = moveXResult ? static_cast<float>(moveXResult.unwrap()) : 0.0f;
                float offsetY = moveYResult ? static_cast<float>(moveYResult.unwrap()) : 0.0f;
                offsetX = std::clamp(offsetX, -32767.0f, 32767.0f);
                offsetY = std::clamp(offsetY, -32767.0f, 32767.0f);
                effectObj->m_moveOffset = CCPoint(offsetX, offsetY);

                auto durResult = objData["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                auto easingResult = objData["easing"].asInt();
                if (easingResult) {
                    int easingVal = std::clamp((int)easingResult.unwrap(), 0, 18);
                    effectObj->m_easingType = (EasingType)easingVal;
                } else {
                    effectObj->m_easingType = EasingType::None;
                }

                auto easingRateResult = objData["easing_rate"].asDouble();
                if (easingRateResult) {
                    effectObj->m_easingRate = std::clamp(
                        static_cast<float>(easingRateResult.unwrap()), 0.01f, 100.0f);
                }

                auto lockXResult = objData["lock_to_player_x"].asBool();
                if (lockXResult && lockXResult.unwrap()) {
                    effectObj->m_lockToPlayerX = true;
                }

                auto lockYResult = objData["lock_to_player_y"].asBool();
                if (lockYResult && lockYResult.unwrap()) {
                    effectObj->m_lockToPlayerY = true;
                }

                effectObj->m_isTouchTriggered = true;
            }

            // ── Alpha Trigger (ID 1007) — fades a group's opacity ──────────────
            if (advFeatures && gameObj->m_objectID == 1007) {
                auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                if (!effectObj) return;

                auto targetGroupResult = objData["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto opacityResult = objData["opacity"].asDouble();
                if (opacityResult) {
                    effectObj->m_opacity = std::clamp(
                        static_cast<float>(opacityResult.unwrap()), 0.0f, 1.0f);
                }

                auto durResult = objData["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                auto easingResult = objData["easing"].asInt();
                if (easingResult) {
                    effectObj->m_easingType = (EasingType)std::clamp((int)easingResult.unwrap(), 0, 18);
                }

                auto easingRateResult = objData["easing_rate"].asDouble();
                if (easingRateResult) {
                    effectObj->m_easingRate = std::clamp(
                        static_cast<float>(easingRateResult.unwrap()), 0.01f, 100.0f);
                }

                effectObj->m_isTouchTriggered = true;
            }

            // ── Rotate Trigger (ID 1346) — rotates a group around a center ─────
            if (advFeatures && gameObj->m_objectID == 1346) {
                auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                if (!effectObj) return;

                auto targetGroupResult = objData["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto centerGroupResult = objData["center_group"].asInt();
                if (centerGroupResult) {
                    effectObj->m_centerGroupID = std::clamp((int)centerGroupResult.unwrap(), 1, 9999);
                }

                auto degreesResult = objData["degrees"].asDouble();
                if (degreesResult) {
                    effectObj->m_rotationDegrees = static_cast<float>(degreesResult.unwrap());
                }

                auto durResult = objData["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                auto easingResult = objData["easing"].asInt();
                if (easingResult) {
                    effectObj->m_easingType = (EasingType)std::clamp((int)easingResult.unwrap(), 0, 18);
                }

                auto easingRateResult = objData["easing_rate"].asDouble();
                if (easingRateResult) {
                    effectObj->m_easingRate = std::clamp(
                        static_cast<float>(easingRateResult.unwrap()), 0.01f, 100.0f);
                }

                auto lockRotResult = objData["lock_object_rotation"].asBool();
                if (lockRotResult && lockRotResult.unwrap()) {
                    effectObj->m_lockObjectRotation = true;
                }

                effectObj->m_isTouchTriggered = true;
            }

            // ── Toggle Trigger (ID 1049) — shows or hides a group ──────────────
            if (advFeatures && gameObj->m_objectID == 1049) {
                auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                if (!effectObj) return;

                auto targetGroupResult = objData["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto activateResult = objData["activate_group"].asBool();
                if (activateResult) {
                    effectObj->m_activateGroup = activateResult.unwrap();
                }

                effectObj->m_isTouchTriggered = true;
            }

            // ── Pulse Trigger (ID 1006) — pulses color on a group or channel ───
            if (advFeatures && gameObj->m_objectID == 1006) {
                auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                if (!effectObj) return;

                // Can target either a group or a color channel
                auto targetGroupResult = objData["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                    effectObj->m_pulseTargetType = 1;  // group
                }

                auto targetColorResult = objData["target_color_channel"].asInt();
                if (targetColorResult) {
                    effectObj->m_targetColor = std::clamp((int)targetColorResult.unwrap(), 1, 1010);
                    effectObj->m_pulseTargetType = 0;  // color channel
                }

                auto colorHexResult = objData["color"].asString();
                if (colorHexResult) {
                    GLubyte r = 255, g = 255, b = 255;
                    if (parseHexColor(colorHexResult.unwrap(), r, g, b)) {
                        effectObj->m_triggerTargetColor = {r, g, b};
                    }
                }

                auto fadeInResult = objData["fade_in"].asDouble();
                if (fadeInResult) {
                    effectObj->m_fadeInDuration = std::clamp(
                        static_cast<float>(fadeInResult.unwrap()), 0.0f, 10.0f);
                }

                auto holdResult = objData["hold"].asDouble();
                if (holdResult) {
                    effectObj->m_holdDuration = std::clamp(
                        static_cast<float>(holdResult.unwrap()), 0.0f, 10.0f);
                }

                auto fadeOutResult = objData["fade_out"].asDouble();
                if (fadeOutResult) {
                    effectObj->m_fadeOutDuration = std::clamp(
                        static_cast<float>(fadeOutResult.unwrap()), 0.0f, 10.0f);
                }

                auto exclusiveResult = objData["exclusive"].asBool();
                if (exclusiveResult) {
                    effectObj->m_pulseExclusive = exclusiveResult.unwrap();
                }

                effectObj->m_isTouchTriggered = true;
            }

            // ── Spawn Trigger (ID 1268) — spawns/activates another trigger group
            if (advFeatures && gameObj->m_objectID == 1268) {
                auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                if (!effectObj) return;

                auto targetGroupResult = objData["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                auto delayResult = objData["delay"].asDouble();
                if (delayResult) {
                    effectObj->m_spawnTriggerDelay = std::clamp(
                        static_cast<float>(delayResult.unwrap()), 0.0f, 30.0f);
                }

                auto editorDisableResult = objData["editor_disable"].asBool();
                if (editorDisableResult) {
                    effectObj->m_previewDisable = editorDisableResult.unwrap();
                }

                effectObj->m_isTouchTriggered = true;
            }

            // ── Stop Trigger (ID 1616) — stops a trigger group ─────────────────
            if (advFeatures && gameObj->m_objectID == 1616) {
                auto* effectObj = typeinfo_cast<EffectGameObject*>(gameObj);
                if (!effectObj) return;

                auto targetGroupResult = objData["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                effectObj->m_isTouchTriggered = true;
            }

            // ── Show/Hide Player triggers (1613/1612) — no extra properties ────
            // ── Show/Hide Trail triggers (32/33) — no extra properties ──────────
            // ── Speed portals (200-203, 1334) — no extra properties ─────────────
            // These work by their object ID alone, no fields needed.

        } catch (...) {
            log::warn("Failed to apply object properties");
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
            try {
                m_editorLayer->loadLevelSettings();
            } catch (...) {
                log::warn("EditorAI: loadLevelSettings() threw — visuals may be stale");
            }
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

        // Clear any leftover preview state from a previous generation
        s_previewObjects.clear();
        s_previewOriginalColors.clear();
        // NOTE: block templates are NOT reset here; they were already reset
        // before macros ran (see onAPISuccess) so any template the AI emitted
        // via a block_template macro is still active when objects spawn.

        m_deferredObjects.clear();
        m_currentObjectIndex = 0;

        int    maxObjects  = (int)Mod::get()->getSettingValue<int64_t>("max-objects");
        size_t objectCount = std::min(objectsArray.size(), static_cast<size_t>(maxObjects));
        log::info("Preparing {} objects for progressive creation...", objectCount);

        for (size_t i = 0; i < objectCount; ++i) {
            try {
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

                // Capture the full slot (with id now set) for applyObjectProperties
                m_deferredObjects.push_back({objectID, CCPoint{x, y}, objectsArray[i]});
            } catch (...) {
                log::warn("Failed to prepare object at index {}", i);
            }
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

        log::info("Prepared {} valid objects", m_deferredObjects.size());

        if (m_deferredObjects.empty()) {
            onError("No Valid Objects",
                fmt::format("The AI returned an objects array, but none of the entries had recognizable "
                            "object types or valid x/y fields. Try rephrasing the prompt to ask for "
                            "common shapes (blocks, spikes, orbs), or switch to a more capable model in "
                            "settings. ({})",
                            autoErrorCode(70, 2)));
            return;
        }

        m_isCreatingObjects = true;
        showStatus("Starting object creation...", false);
    }

    // ── System prompt ─────────────────────────────────────────────────────────

    std::string buildSystemPrompt() {
        bool advFeatures   = Mod::get()->getSettingValue<bool>("enable-advanced-features");
        bool compactPrompt = Mod::get()->getSettingValue<bool>("compact-prompts");

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
                "MODE: EDIT. ~{} existing objects stay. Emit ONLY new additions "
                "(3-30 objects). Place AFTER the current X cursor unless asked otherwise. "
                "Call analyze_level first if tools are available. 1-sentence analysis.\n\n",
                existingCount);
        } else {
            modePrefix =
                "MODE: CREATION. Blank canvas. Build a complete playable level matching "
                "the requested length/difficulty. Use macros liberally.\n\n";
        }

        // Cache the configured ground Y. Used by both the full and compact
        // prompts. {1}=ground, {2}=ground+30, {3}=ground+60, {4}=ground+90.
        const int gY  = (int)getGroundY();
        const int gY1 = gY + 30;
        const int gY2 = gY + 60;
        const int gY3 = gY + 90;

        // Build the object catalog. The expanded 3,986-entry object_ids.json
        // dumped wholesale would be ~120 KB / ~28K tokens — fine on 128K-context
        // frontier models, fatal on the 8K-context small ones we ship to. Two
        // tiers:
        //
        //   compact-prompts = true   →  curated ~90-entry allowlist below
        //                                (covers every gameplay-essential
        //                                shape; ~3 KB / ~750 tokens, fits 8K)
        //
        //   compact-prompts = false  →  full catalog dump
        //                                (~28 K tokens; targets ≥64K context)
        //
        // The allowlist is hand-picked against the actual catalog so every
        // entry resolves. If you add new names to object_ids.json, you can
        // safely append them here without touching the prompt logic.
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
            "effect_10_level_end_trigger"
        };
        std::string objectList;
        size_t catalogIncluded = 0;
        if (compactPrompt) {
            // Filter the allowlist against what we actually have IDs for.
            // Anything missing from OBJECT_IDS is silently skipped (defensive
            // — every entry above was verified, but the file could drift).
            objectList.reserve(COMPACT_OBJECT_ALLOWLIST.size() * 32);
            bool first = true;
            for (const auto& name : COMPACT_OBJECT_ALLOWLIST) {
                if (OBJECT_IDS.find(name) == OBJECT_IDS.end()) continue;
                if (!first) objectList += ", ";
                objectList += name;
                first = false;
                ++catalogIncluded;
            }
            log::info("Compact prompt: catalog {} of {} curated entries valid (catalog has {} total)",
                catalogIncluded, COMPACT_OBJECT_ALLOWLIST.size(), OBJECT_IDS.size());
        } else {
            // Full mode: dump every name. Stable iteration order via a sorted
            // temporary so the prompt is reproducible across runs (helps cache
            // hits on prompt-cache-capable providers).
            std::vector<const std::string*> sorted;
            sorted.reserve(OBJECT_IDS.size());
            for (auto& [name, id] : OBJECT_IDS) sorted.push_back(&name);
            std::sort(sorted.begin(), sorted.end(),
                [](const std::string* a, const std::string* b){ return *a < *b; });
            objectList.reserve(OBJECT_IDS.size() * 28);
            bool first = true;
            for (auto* p : sorted) {
                if (!first) objectList += ", ";
                objectList += *p;
                first = false;
                ++catalogIncluded;
            }
            log::info("Full prompt: catalog dumped, {} entries (~{} chars)",
                catalogIncluded, objectList.size());
        }

        // ── EAS-teaching prompt (used for both compact and full modes) ─────
        // EditorAI Script (EAS) is the PREFERRED output format. It's a
        // line-based DSL that's far more compact than JSON (~6 tokens/obj
        // vs ~25 for JSON), forces structural thinking via macros, and
        // bakes metadata into a mandatory first verb.
        //
        // Compact mode: short catalog + brief verbs only. Full mode adds
        // the long catalog + the trigger docs.
        if (compactPrompt) {
            // Compact prompt — ~1.4 KB before catalog. Drops every long
            // explanation; keeps verb grammar + the must-know coordinate rule
            // + a tiny worked example. Optimised for cost (hosted APIs charge
            // per token) and small-context models (≤8K).
            return modePrefix + fmt::format(
                "GD level designer. Output ONE format end-to-end:\n"
                " A) EAS (preferred). Markdown plan, then `## Level Script`, then\n"
                "    EAS lines. Parser keeps everything after `## Level Script`.\n"
                " B) JSON: {{\"analysis\"?, \"objects\"?:[{{type,x,y,...}}], "
                "\"macros\"?:[{{name,...}}], \"level_metadata\"?:{{...}}}}.\n"
                "Analysis/objects are optional — metadata-only updates are valid.\n\n"
                "GRID: 30 unit cells. Y = object CENTER. Ground={1}; up rows {2}, {3}, {4}. Never Y<{1}.\n\n"
                "EAS GRAMMAR (one per line; `[opt]`):\n"
                " META name=\"\" desc=\"\" song_id=N bg=N ground=N [platformer=true]\n"
                " COLOR ch=N hex=RRGGBB [blend] [at=X duration=T  ← runtime]\n"
                " SECTION x0..x1 difficulty=easy|medium|hard|insane mode=cube|ship|ball|ufo|wave|robot|spider|swing\n"
                " FLOOR x0..x1 [y={1}]            CORRIDOR x0..x1 ceiling=Y floor=Y\n"
                " PLATFORM-RUN x0..x1 y=Y [gap=G gap-every=E]\n"
                " SPIKE x [variant=basic|tiny|small|half|pit|slope]\n"
                " BLOCK x y [variant=basic|small|slab|grid]   SAW x y [size=small|medium|large]\n"
                " SPIKE-TRAIN x count=N [spacing=30]   STAIR-UP|STAIR-DOWN x steps=N\n"
                " PILLAR x y_bot=Y y_top=Y   BLOCK-STACK x y count=N\n"
                " ORB <yellow|pink|red|blue|green|black|dash> x y\n"
                " PAD <yellow|pink|red|blue|spider> x y\n"
                " PORTAL <cube|ship|ball|ufo|wave|robot|spider|swing|mini|normal|mirror-on|mirror-off"
                "|speed-half|speed-normal|speed-double|speed-triple|speed-quad|gravity-up|gravity-down|gravity-reverse|dual> x [y]\n"
                " ARC-ORBS x y count=N [spacing=60]   COPY from=x0..x1 offset=DX\n"
                " MIRROR axis=X [from=x0..x1]\n"
                " TRIGGER color|alpha|move|toggle|pulse|rotate|scale|shake|spawn|stop|end ...\n"
                " OBJ <catalog_name> x y          ← escape hatch\n"
                "PER-LINE FIELDS (work on any verb): scale=F rot=N color=N detail=N "
                "groups=1,2 z_layer=N z_order=N flip_x flip_y multi_activate\n\n"
                "RULES: section must START with FLOOR or CORRIDOR. Obstacle every "
                "90-210u (easy 150-210, medium 90-150, hard 60-90, insane 30-60). "
                "End each gamemode at a portal. Triggers are X-positional.\n"
                "LENGTH (X span): short 1200-2400, medium 2400-9000, long 9000-18000, xl 18000+. "
                "Aim 100-500 objects — let macros do the heavy lifting.\n"
                "OUTPUT BUDGET: keep analysis to 1-2 sentences. No prose between lines. "
                "Prefer macros over emitting individual blocks. Don't repeat already-known facts.\n\n"
                "CATALOG (use these names with OBJ / variant=): {0}\n\n"
                "EXAMPLE:\n"
                "## Plan\nMedium cube → ship drop, 30s, modern.\n"
                "## Level Script\n"
                "META name=\"Drop\" desc=\"30s\" song_id=467339 bg=1 ground=1\n"
                "COLOR ch=1000 hex=1a2030 blend\n"
                "SECTION 0..3000 difficulty=medium mode=cube\n"
                "FLOOR 0..3000\n"
                "SPIKE-TRAIN 300 count=2   ORB yellow 600 135   STAIR-UP 900 steps=3\n"
                "SPIKE 1200   ARC-ORBS 1500 165 count=3 spacing=120\n"
                "SPIKE-TRAIN 2000 count=4 spacing=45   PAD yellow 2500 105\n"
                "PORTAL ship 2900 165\n"
                "SECTION 3000..6000 difficulty=hard mode=ship\n"
                "TRIGGER color ch=1000 hex=2a1030 at=3000 duration=0.4 blend\n"
                "CORRIDOR 3000..6000 ceiling=270 floor=60\n"
                "SAW small 3300 180   PORTAL speed-double 3600 0\n"
                "SAW medium 4200 165   SAW large 4800 195\n"
                "TRIGGER pulse ch=1001 hex=ff3366 at=5000 duration=0.5\n"
                "TRIGGER end at=6000",
                objectList, gY, gY1, gY2, gY3
            ) + std::string(editorai::GDCS_DESIGN_TIPS);
        }

        // Full-mode prompt — kept terse on purpose. Prior revisions exploded
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
            " OBJ <catalog_name> x y                (escape hatch — any catalog entry)\n\n"
            "PER-LINE FIELDS (apply to any verb): scale=F rot=N color=N detail=N "
            "groups=1,2,3 flip_x flip_y multi_activate z_layer=N z_order=N editor_layer=N\n"
            " `FLOOR 0..1500 color=10 groups=1` → every block in the floor gets color_channel=10 and group 1.\n\n"
            "TRIGGER VERBS — fire by X position. Place at y=0. Add `multi_activate` for repeating sections:\n"
            " TRIGGER color ch=N hex=RRGGBB at=X [duration=T opacity=A blend]\n"
            " TRIGGER alpha groups=g at=X to=A [duration=T easing=N]\n"
            " TRIGGER move  groups=g at=X dx=DX dy=DY [duration=T easing=N lock_to_player_x lock_to_player_y]\n"
            " TRIGGER toggle groups=g at=X on=true|false\n"
            " TRIGGER pulse ch=N|groups=g hex=RRGGBB at=X [fade_in=T hold=T fade_out=T exclusive]\n"
            " TRIGGER rotate groups=g at=X degrees=D [duration=T center=g lock_rotation]\n"
            " TRIGGER scale groups=g at=X to=F [duration=T]\n"
            " TRIGGER shake at=X [duration=T strength=F]\n"
            " TRIGGER spawn target=g at=X [delay=T editor_disable]\n"
            " TRIGGER stop  groups=g at=X            TRIGGER end at=X\n"
            " TRIGGER show-player|hide-player|show-trail|hide-trail at=X\n"
            "EASING: 0 none, 1 inout, 2 in, 3 out, 4-6 elastic, 7-9 bounce, 10-12 exp, 13-15 sine, 16-18 back.\n\n"
            "COLOR SLOTS: 1000=BG 1001=G1 1002=Line 1003=3DL 1004=Object 1005=Line2 1009=G2. 1-999 = user.\n\n"
            "RULES: section must start with FLOOR or CORRIDOR. Obstacle every "
            "90-210u (easy 150-210, medium 90-150, hard 60-90, insane 30-60). "
            "Mix obstacle types, don't spam SPIKE-TRAIN. End each gamemode at a portal. "
            "Use macros for 30-60% of objects (FLOOR/CORRIDOR/PLATFORM-RUN do the bones).\n"
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
            "CATALOG (names for OBJ / variant fields):\n {0}",
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
        // style. Compact mode caps at 1 (the catalog already eats budget);
        // full mode keeps 3 for stylistic variety. Each example is the raw
        // objects array (X normalized to 0).
        if (!EXAMPLE_SECTIONS.empty()) {
            int wantPicks = compactPrompt ? 1 : 3;
            auto picks = pickExampleIndices(s_lastDifficulty, s_lastStyle, wantPicks);
            if (!picks.empty()) {
                base += "\n\nFEW-SHOT (real GD slices, X normalized to 0 — adapt anywhere):\n";
                for (size_t k = 0; k < picks.size(); ++k) {
                    const auto& ex = EXAMPLE_SECTIONS[picks[k]];
                    base += fmt::format(" [{}] \"{}\" by {} ({}, {}; {} obj): {}\n",
                        k + 1, ex.levelName, ex.author, ex.theme, ex.difficulty,
                        ex.objectCount, ex.objectsJson);
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
                    entry += fmt::format("    AI generated: {}\n", fb.objectsJson);
                if (!fb.editedObjectsJson.empty())
                    entry += fmt::format("    user corrected to: {}\n", fb.editedObjectsJson);
                else if (!fb.editSummary.empty())
                    entry += fmt::format("    user edits: {}\n", fb.editSummary);

                // Budget check: skip this entry if it would exceed the limit
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
                for (auto& fb : topFeedback)
                    if (!appendFeedback(fb, true)) break;
            }

            // Negative examples
            auto bottomFeedback = getBottomFeedback(maxExamples, curDiff, curStyle, curLen);
            if (!bottomFeedback.empty()) {
                feedbackSection += "\n\nUSER DISLIKES — The user rated these poorly. "
                    "AVOID reproducing these patterns:\n";
                for (auto& fb : bottomFeedback)
                    if (!appendFeedback(fb, false)) break;
            }

            base += feedbackSection;
        }

        // Edit-mode override — last so the AI sees it freshest.
        if (m_editMode) {
            base += "\nEDIT MODE OVERRIDE: emit 1-50 objects total (not hundreds). "
                    "Build on top of existing; no large macros (block_floor with long ranges, "
                    "mirror_horizontal level-wide). Stay in the user's requested X scope.\n";
        }

        // Append the GD Creator School design tips digest. This is a tiny
        // appendix (~1.5KB) that gives small fine-tunes a stronger prior on
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
        // Build a reverse map once per call. The full OBJECT_IDS is small
        // enough that this cost is negligible.
        std::unordered_map<int, std::string> idToName;
        idToName.reserve(OBJECT_IDS.size());
        for (auto& [n, id] : OBJECT_IDS) {
            if (idToName.find(id) == idToName.end()) idToName[id] = n;
        }

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
                try {
                    if (key == "1") id = std::stoi(std::string(val));
                    else if (key == "2") x = std::stof(std::string(val));
                    else if (key == "3") y = std::stof(std::string(val));
                } catch (...) {}
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
    void fireFetchNGSong(const std::string& input,
                        std::function<void(const std::string&)> onDone)
    {
        if (input.empty()) { onDone(""); return; }
        std::string trimmed = trimKey(input);
        bool isNumeric = !trimmed.empty()
            && std::all_of(trimmed.begin(), trimmed.end(),
                           [](char c){ return (unsigned char)c >= '0' && (unsigned char)c <= '9'; });

        if (isNumeric) {
            fetchNGSongByID(std::stoi(trimmed), std::move(onDone));
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
                        log::warn("NG search HTTP {}", resp.code());
                        onDone("");
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
                        onDone("");
                        return;
                    }
                    int songId = std::stoi(m[1].str());
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
                    log::warn("NG song fetch HTTP {}", resp.code());
                    onDone("");
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
    void fireFetchWebSearch(const std::string& query,
                           std::function<void(const std::string&)> onDone)
    {
        if (query.empty()) { onDone(""); return; }
        std::string trimmed = trimKey(query);
        if (trimmed.empty()) { onDone(""); return; }

        showStatus(fmt::format("Web search: \"{}\"...", trimmed));
        log::info("Web search: '{}'", trimmed);
        auto request = web::WebRequest();
        request.userAgent("Mozilla/5.0 (Windows NT 10.0; Win64) Gecko/20100101");
        request.header("Content-Type", "application/x-www-form-urlencoded");
        request.timeout(std::chrono::seconds(15));
        request.bodyString("q=" + urlFormEncode(trimmed));

        m_toolListenerWeb.spawn(
            request.post("https://html.duckduckgo.com/html/"),
            [this, trimmed, onDone = std::move(onDone)](web::WebResponse resp) {
                if (!resp.ok()) {
                    log::warn("Web search HTTP {}", resp.code());
                    onDone("");
                    return;
                }
                std::string body = resp.string().unwrapOr("");
                // DDG HTML results are inside `<a class="result__a" href="...">TITLE</a>`
                // and `<a class="result__snippet">SNIPPET</a>` blocks.
                static const std::regex titleRx(
                    R"(class=\"result__a\"[^>]*>([^<]+)<)",
                    std::regex::ECMAScript | std::regex::optimize);
                static const std::regex snippetRx(
                    R"(class=\"result__snippet\"[^>]*>(.*?)</a>)",
                    std::regex::ECMAScript | std::regex::optimize);

                std::vector<std::string> titles, snippets;
                {
                    auto begin = std::sregex_iterator(body.begin(), body.end(), titleRx);
                    auto end   = std::sregex_iterator();
                    for (auto it = begin; it != end && titles.size() < 5; ++it) {
                        titles.push_back(stripHtmlBasic(it->str(1)));
                    }
                }
                {
                    auto begin = std::sregex_iterator(body.begin(), body.end(), snippetRx);
                    auto end   = std::sregex_iterator();
                    for (auto it = begin; it != end && snippets.size() < 5; ++it) {
                        snippets.push_back(stripHtmlBasic(it->str(1)));
                    }
                }

                if (titles.empty()) {
                    log::warn("Web search: no results parsed from DDG HTML");
                    onDone("");
                    return;
                }

                std::string out = fmt::format("DuckDuckGo results for \"{}\":\n", trimmed);
                int n = std::min({(int)titles.size(), (int)snippets.size(), 3});
                if (n == 0) n = std::min((int)titles.size(), 3);
                for (int i = 0; i < n; ++i) {
                    out += fmt::format("  {}. {}\n", i + 1, titles[i]);
                    if (i < (int)snippets.size()) {
                        std::string snip = snippets[i];
                        if (snip.size() > 220) snip.resize(220);
                        if (!snip.empty()) out += fmt::format("     {}\n", snip);
                    }
                }
                log::info("Web search returned {} results", n);
                onDone(out);
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
    int                           m_toolMaxIters    = 6;
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

    // Apply the per-provider authentication header(s) to a WebRequest. Mirrors
    // the same logic in callAPI's single-shot path so behavior is identical
    // for both code paths.
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
        }
        // ollama / lm-studio / llama-cpp: no auth header
    }

    // Entry point. Called instead of callAPI's single-shot when tool use is
    // enabled and the selected provider supports it.
    void runToolLoop(const std::string& userPrompt, const std::string& rawApiKey) {
        m_toolApiKey   = trimKey(rawApiKey);
        m_toolProvider = Mod::get()->getSettingValue<std::string>("ai-provider");
        m_toolModel    = getProviderModel(m_toolProvider);
        m_toolMaxIters = std::clamp(
            (int)Mod::get()->getSettingValue<int64_t>("ai-tools-max-iterations"),
            1, 12);
        m_toolIterations  = 0;
        m_toolHistory.clear();
        m_accumulatedObjects = matjson::Value::array();
        m_extensionRounds = 0;
        m_passabilityFixRounds = 0;
        m_refinementRounds = 0;
        m_lengthTarget = lengthTargetForSetting(
            Mod::get()->getSettingValue<std::string>("length"));

        // Snapshot user-prompt context so the AI sees difficulty/style/length
        // and any current-level JSON.
        std::string difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        std::string style      = Mod::get()->getSettingValue<std::string>("style");
        std::string length     = Mod::get()->getSettingValue<std::string>("length");
        s_lastUserPrompt = userPrompt;
        s_lastDifficulty = difficulty;
        s_lastStyle      = style;
        s_lastLength     = length;

        std::string levelDataSection;
        if (!m_shouldClearLevel) {
            std::string levelJson = buildLevelDataJson();
            log::info("=== Tool-loop: current level data context ===");
            logLong("LevelData", levelJson);
            levelDataSection = "\n\nCurrent level data (build upon or extend):\n" + levelJson;
        }

        // Compute the concrete length target in BOTH seconds AND units so the
        // model has the math pre-done.
        float targetMinX = m_lengthTarget.minSeconds * GD_PLAYER_SPEED_1X;
        float targetMaxX = m_lengthTarget.maxSeconds * GD_PLAYER_SPEED_1X;

        std::string fullUserPrompt = fmt::format(
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
            "You have tools available — call them whenever they'd help. The user\n"
            "does NOT see these tools or their results; only you do. Tools:\n"
            "  • web_search(query)         — general web search\n"
            "  • download_level(level_id)  — download a real GD level as inspiration\n"
            "  • search_newgrounds(query)  — find a song by name/keyword\n"
            "  • get_newgrounds_song(id)   — get song metadata by NG ID\n"
            "  • analyze_level()           — inspect the current editor level\n"
            "  • get_level_length()        — see how long your level is RIGHT NOW\n"
            "                                 (call between drafts to verify progress)\n"
            "  • think(thought)            — log private reasoning\n"
            "Call tools as many times as needed (up to {} rounds). When you have\n"
            "enough context, STOP calling tools and return your final JSON answer.\n\n"
            "Final answer JSON: \"analysis\" string, \"objects\" array, optional \"macros\"\n"
            "array (use aggressively), optional \"level_metadata\". DO NOT wrap the\n"
            "final JSON in another tool call — emit it as your normal assistant reply.",
            userPrompt, difficulty, style, length,
            m_lengthTarget.minSeconds, m_lengthTarget.maxSeconds,
            targetMinX, targetMaxX,
            levelDataSection,
            GD_PLAYER_SPEED_1X,
            targetMinX, m_maxExtensionRounds,
            m_toolMaxIters
        );

        // Build the conversation history.
        toolUse::Message sys;
        sys.role = toolUse::MessageRole::System;
        sys.text = buildSystemPrompt();
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

        log::info("=== Tool-use loop start ({}, model {}, max {} rounds) ===",
                  m_toolProvider, m_toolModel, m_toolMaxIters);
        showStatus("Tool loop: round 1...");
        this->doToolRound();
    }

    void doToolRound() {
        ++m_toolIterations;
        if (m_toolIterations > m_toolMaxIters) {
            log::warn("Tool loop hit max iterations ({}); forcing final answer", m_toolMaxIters);
            // Inject a system nudge to make the model stop calling tools and
            // emit its final answer NOW.
            toolUse::Message stop;
            stop.role = toolUse::MessageRole::User;
            stop.text = "You've reached the tool-use round limit. STOP calling tools and "
                        "produce the final JSON answer now.";
            m_toolHistory.push_back(std::move(stop));
        }

        auto body = toolUse::buildRequest(m_toolProvider, m_toolHistory, m_toolModel);
        std::string bodyStr = body.dump();
        std::string url     = toolUse::urlFor(m_toolProvider, m_toolModel);
        log::info("Tool round {}: POST {} ({} bytes)",
                  m_toolIterations, url, bodyStr.size());

        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");
        applyProviderAuth(request, m_toolProvider, m_toolApiKey);
        // Tool rounds tend to be smaller than the final answer, but the
        // first one carries the full system prompt + few-shot. Generous
        // timeout matches the single-shot path.
        if (m_toolProvider == "ollama") {
            request.timeout(std::chrono::seconds(
                (int)Mod::get()->getSettingValue<int64_t>("ollama-timeout")));
        } else if (m_toolProvider == "lm-studio" || m_toolProvider == "llama-cpp") {
            request.timeout(std::chrono::seconds(300));
        } else {
            request.timeout(std::chrono::seconds(180));
        }
        request.bodyString(bodyStr);
        m_listener.spawn(
            request.post(url),
            [this](web::WebResponse resp) { this->onToolRoundResponse(std::move(resp)); }
        );
    }

    void onToolRoundResponse(web::WebResponse resp) {
        if (!resp.ok()) {
            auto [title, msg] = parseAPIError(
                resp.string().unwrapOr("No body"), resp.code());
            onError(title, msg);
            return;
        }
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
            log::info("Tool loop finished after {} round(s); final response {} chars",
                      m_toolIterations, parsed.finalText.size());
            this->processFinalResponse(std::move(parsed.finalText), m_toolProvider);
            return;
        }

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

        // Execute serially (the popup's network helpers all use the same
        // listener members internally; serial avoids stepping on each other).
        auto calls = parsed.toolCalls;
        executeToolCalls(std::move(calls), {},
            [this](std::vector<toolUse::ToolResult> results) {
                toolUse::Message resMsg;
                resMsg.role        = toolUse::MessageRole::ToolResults;
                resMsg.toolResults = std::move(results);
                m_toolHistory.push_back(std::move(resMsg));
                this->doToolRound();
            }
        );
    }

    // Serial executor: pops the first call, runs it, recurses on the remainder
    // with the result appended. When `pending` is empty, calls onAllDone with
    // the accumulated results vector.
    void executeToolCalls(std::vector<toolUse::ToolCall> pending,
                          std::vector<toolUse::ToolResult> accumulated,
                          std::function<void(std::vector<toolUse::ToolResult>)> onAllDone)
    {
        if (pending.empty()) {
            onAllDone(std::move(accumulated));
            return;
        }
        toolUse::ToolCall first = std::move(pending.front());
        pending.erase(pending.begin());

        executeOneToolCall(first,
            [this, pending = std::move(pending), accumulated = std::move(accumulated),
             onAllDone = std::move(onAllDone)](toolUse::ToolResult r) mutable {
                accumulated.push_back(std::move(r));
                this->executeToolCalls(std::move(pending), std::move(accumulated), std::move(onAllDone));
            }
        );
    }

    // Run one tool call. Each known tool either does a synchronous mod-side
    // lookup (think / analyze_level) or hands off to a fireFetchX async path
    // and forwards the result to the callback.
    void executeOneToolCall(toolUse::ToolCall call,
                            std::function<void(toolUse::ToolResult)> onDone)
    {
        toolUse::ToolResult r;
        r.toolCallId = call.id;
        log::info("→ Tool call: {} args={}", call.name, call.args.dump());

        if (call.name == "think") {
            auto t = call.args["thought"].asString();
            std::string thought = t ? t.unwrap() : "(no thought)";
            log::info("AI thought: {}", thought);
            r.content = "Thought logged. Continue.";
            onDone(std::move(r));
            return;
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
            if (m_editorLayer && m_editorLayer->m_objects) {
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
                                "get_level_length, think.",
                                call.name);
        r.isError = true;
        onDone(std::move(r));
    }

    // ── processFinalResponse ──────────────────────────────────────────────
    // The portion of onAPISuccess after aiResponse is in hand. Factored out so
    // the tool-use loop can call it once its loop completes.
    void processFinalResponse(std::string aiResponse, const std::string& provider) {
        m_isGenerating = false;
        m_cancelBtn->setVisible(false);
        m_generateBtn->setVisible(true);

        if (m_loadingCircle) {
            m_loadingCircle->fadeAndRemove();
            m_loadingCircle = nullptr;
        }
        if (!m_isCreatingObjects) m_generateBtn->setEnabled(true);

        // Log + sanitize the response.
        log::info("=== Full AI Response from {} ===", provider);
        logLong("AIResponse", aiResponse);
        log::info("=== End AI Response ===");

        {
            size_t pos = 0;
            while ((pos = aiResponse.find("```", pos)) != std::string::npos) {
                size_t end = pos + 3;
                while (end < aiResponse.size() && aiResponse[end] != '\n' && aiResponse[end] != '\r')
                    ++end;
                aiResponse.erase(pos, end - pos);
            }
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
            std::string jsonBlock = extractLastEAIJsonBlock(aiResponse);
            if (jsonBlock.empty()) {
                size_t s = aiResponse.find('{');
                size_t e = aiResponse.rfind('}');
                if (s == std::string::npos || e == std::string::npos || e < s) {
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
            log::warn("AI response had no objects, macros, OR level_metadata — "
                      "applying nothing");
            Notification::create(
                "AI returned no objects and no metadata changes. Try re-generating.",
                NotificationIcon::Warning, 3.f
            )->show();
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
        for (size_t i = 0; i < objectsArray.size(); ++i) {
            m_accumulatedObjects.push(objectsArray[i]);
        }
        log::info("Accumulator now has {} objects (this round added {})",
                  (int)m_accumulatedObjects.size(), (int)objectsArray.size());

        s_lastGeneratedJson = m_accumulatedObjects.dump();

        auto analysisResult = levelData["analysis"].asString();
        if (analysisResult) log::info("AI Analysis: {}", analysisResult.unwrap());

        log::info("Parsed {} objects from AI response (total {} after merge)",
                  objectsArray.size(), (int)m_accumulatedObjects.size());

        // ── Length enforcement (tool-loop only) ─────────────────────────
        // If the user's "length" setting demands more seconds than the
        // running accumulator currently provides AND we have a tool-loop
        // history (i.e. we're not on the single-shot/custom path AND not in
        // edit mode), inject an "extend further" user message and run
        // another tool round. Capped by m_maxExtensionRounds so a stubborn
        // model can't hang forever.
        bool inToolLoop = !m_toolHistory.empty() && !m_editMode
                       && toolUse::supportsToolUse(m_toolProvider);
        if (inToolLoop) {
            float currentMaxX = computeMaxXFromObjects(m_accumulatedObjects);
            auto [curSecs, curCat] = describeLengthByX(currentMaxX);
            float targetSecs       = m_lengthTarget.minSeconds;
            float targetMinX       = targetSecs * GD_PLAYER_SPEED_1X;

            if (curSecs < targetSecs && m_extensionRounds < m_maxExtensionRounds) {
                ++m_extensionRounds;
                log::info("Length {}/{}s short of {}-{}s target. Extension {} / {}.",
                          curSecs, curMaxXForLog(currentMaxX),
                          m_lengthTarget.minSeconds, m_lengthTarget.maxSeconds,
                          m_extensionRounds, m_maxExtensionRounds);

                // Record the assistant's last JSON so the model has the
                // shared context for "what you just emitted is too short".
                toolUse::Message asst;
                asst.role = toolUse::MessageRole::Assistant;
                asst.text = aiResponse;
                m_toolHistory.push_back(std::move(asst));

                toolUse::Message more;
                more.role = toolUse::MessageRole::User;
                more.text = fmt::format(
                    "Only {:.1f}s long (maxX={:.0f}). Target {}: {:.0f}-{:.0f}s, "
                    "maxX≈{:.0f}-{:.0f}. EXTEND from X={:.0f} rightward — same format "
                    "as your last reply (EAS or JSON), additional objects/macros only, "
                    "do not restart or re-emit prior objects. Round {}/{}.",
                    curSecs, currentMaxX,
                    m_lengthTarget.label,
                    m_lengthTarget.minSeconds, m_lengthTarget.maxSeconds,
                    targetMinX, m_lengthTarget.maxSeconds * GD_PLAYER_SPEED_1X,
                    currentMaxX,
                    m_extensionRounds, m_maxExtensionRounds);
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

        // Snapshot the FULL accumulator (everything across all rounds) as
        // the objects to apply. We use the accumulator instead of the
        // per-round objectsArray so previous extension rounds aren't lost.
        matjson::Value applyObjects = m_accumulatedObjects;

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
        auto passResult = levelcheck::check(applyObjects);
        log::info("Passability: {}", passResult.summary);

        if (passResult.pass_rate < PASS_THRESHOLD && !m_editMode) {
            bool canLoopBack = !m_toolHistory.empty()
                            && toolUse::supportsToolUse(m_toolProvider)
                            && m_passabilityFixRounds < MAX_PASSABILITY_FIXES;
            if (canLoopBack) {
                ++m_passabilityFixRounds;
                log::warn("Level only {:.1f}% passable. Asking model to fix "
                          "(round {}/{}).",
                          passResult.pass_rate * 100.f,
                          m_passabilityFixRounds, MAX_PASSABILITY_FIXES);

                // Record the assistant's last response so the conversation is coherent
                toolUse::Message asst;
                asst.role = toolUse::MessageRole::Assistant;
                asst.text = aiResponse;
                m_toolHistory.push_back(std::move(asst));

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
            bool canRefine = !m_toolHistory.empty()
                          && toolUse::supportsToolUse(m_toolProvider)
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

                // Record the assistant's last response so the conversation flows
                toolUse::Message asst;
                asst.role = toolUse::MessageRole::Assistant;
                asst.text = aiResponse;
                m_toolHistory.push_back(std::move(asst));

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
                m_toolHistory.push_back(std::move(refine));
                showStatus(fmt::format("Refinement pass {}/{}...",
                    m_refinementRounds, maxRefine));
                this->doToolRound();
                return;
            }
        }

        auto applyResult = [this, levelData, applyObjects]() mutable {
            if (m_shouldClearLevel) clearLevel();
            if (levelData.contains("level_metadata")
                && levelData["level_metadata"].isObject())
            {
                applyLevelMetadata(levelData["level_metadata"]);
            }
            prepareObjects(applyObjects);
        };

        if (Mod::get()->getSettingValue<bool>("show-ai-output")) {
            std::string preview = aiResponse;
            constexpr size_t MAX_PREVIEW = 1800;
            if (preview.size() > MAX_PREVIEW) {
                preview = preview.substr(0, MAX_PREVIEW)
                        + fmt::format("\n\n[... {} more chars truncated — see geode.log for full text]",
                                      aiResponse.size() - MAX_PREVIEW);
            }
            geode::createQuickPopup(
                "AI Response", preview, "Cancel", "Continue",
                [this, applyResult](FLAlertLayer*, bool btnContinue) mutable {
                    if (btnContinue) {
                        applyResult();
                    } else {
                        showStatus("Cancelled by user.", true);
                        log::info("EditorAI: user cancelled after reviewing AI response");
                    }
                }
            );
        } else {
            applyResult();
        }
    }

    // tiny helper used only by the length-enforcement log line
    static int curMaxXForLog(float x) { return (int)x; }

    void callAPI(const std::string& prompt, const std::string& rawApiKey) {
        std::string apiKey     = trimKey(rawApiKey);
        std::string provider   = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model      = getProviderModel(provider);
        std::string difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        std::string style      = Mod::get()->getSettingValue<std::string>("style");
        std::string length     = Mod::get()->getSettingValue<std::string>("length");

        // Capture generation context for the rating popup
        s_lastUserPrompt = prompt;
        s_lastDifficulty = difficulty;
        s_lastStyle      = style;
        s_lastLength     = length;

        // ── Multi-turn tool-use path ──────────────────────────────────────
        // When the user has tool use on (default), the selected provider
        // supports it, AND we're not in edit mode (edit mode is supposed to
        // be a small targeted change — no point pulling in web search etc.),
        // route through the tool-use loop. The "custom" provider deliberately
        // never goes through this path: we don't know its tool-use dialect.
        bool toolsEnabled = Mod::get()->getSettingValue<bool>("enable-ai-tools");
        if (toolsEnabled && !m_editMode && toolUse::supportsToolUse(provider)) {
            log::info("Routing to tool-use loop (provider={})", provider);
            this->runToolLoop(prompt, apiKey);
            return;
        }
        log::info("Tool-use loop skipped (toolsEnabled={}, editMode={}, provider={}); using single-shot",
                  toolsEnabled, m_editMode, provider);

        log::info("Calling {} API with model: {}", provider, model);

        std::string systemPrompt = buildSystemPrompt();

        // ── Level data context ────────────────────────────────────────────────
        // When the user is NOT clearing the level, we pass the existing objects
        // to the AI so it can build on top of them and avoid collisions.
        // When clear level is ON, there is no useful context to send.
        // The full level JSON is also logged here for debugging.
        std::string levelDataSection;
        if (!m_shouldClearLevel) {
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

            // Disable thinking budget to allow temperature < 1.0 and reduce latency.
            auto thinkingConfig = matjson::Value::object();
            thinkingConfig["thinkingBudget"] = 0;

            auto genConfig = matjson::Value::object();
            genConfig["temperature"]     = 0.7;
            genConfig["maxOutputTokens"] = 65536;
            genConfig["thinkingConfig"]  = thinkingConfig;

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
            requestBody["system"]      = systemPrompt;
            requestBody["messages"]    = std::vector<matjson::Value>{userMsg};

            url = "https://api.anthropic.com/v1/messages";

        // ── OpenAI ─────────────────────────────────────────────────────────────
        } else if (provider == "openai") {
            auto sysMsg  = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                          = matjson::Value::object();
            requestBody["model"]                 = model;
            requestBody["messages"]              = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["max_completion_tokens"] = 16384;

            if (!isOSeriesModel(model)) {
                requestBody["temperature"] = 0.7;
            }

            url = "https://api.openai.com/v1/chat/completions";

        // ── Mistral AI (Ministral) ─────────────────────────────────────────────
        } else if (provider == "ministral") {
            auto sysMsg  = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                = matjson::Value::object();
            requestBody["model"]       = model;
            requestBody["messages"]    = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["max_tokens"]  = 16384;
            requestBody["temperature"] = 0.7;

            url = "https://api.mistral.ai/v1/chat/completions";

        // ── HuggingFace Inference API ──────────────────────────────────────────
        } else if (provider == "huggingface") {
            auto sysMsg  = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                = matjson::Value::object();
            requestBody["model"]       = model;
            requestBody["messages"]    = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["max_tokens"]  = 8192;
            requestBody["temperature"] = 0.7;

            url = "https://router.huggingface.co/v1/chat/completions";

        // ── OpenRouter (OpenAI-compatible, 300+ models) ──────────────────────
        } else if (provider == "openrouter") {
            auto sysMsg  = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                          = matjson::Value::object();
            requestBody["model"]                 = model;
            requestBody["messages"]              = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["max_tokens"]            = 16384;
            requestBody["temperature"]           = 0.7;

            url = "https://openrouter.ai/api/v1/chat/completions";

        // ── DeepSeek (OpenAI-compatible API) ─────────────────────────────────
        } else if (provider == "deepseek") {
            auto sysMsg  = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                          = matjson::Value::object();
            requestBody["model"]                 = model;
            requestBody["messages"]              = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["max_tokens"]            = 8192;
            requestBody["temperature"]           = 0.7;

            url = "https://api.deepseek.com/v1/chat/completions";

        // ── LM Studio (OpenAI-compatible local server) ─────────────────────────
        } else if (provider == "lm-studio") {
            auto sysMsg  = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                          = matjson::Value::object();
            requestBody["model"]                 = model;
            requestBody["messages"]              = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["max_completion_tokens"] = 16384;
            requestBody["temperature"]           = 0.7;

            std::string lmUrl = Mod::get()->getSettingValue<std::string>("lm-studio-url");
            url = lmUrl + "/v1/chat/completions";

        // ── llama.cpp server (OpenAI-compatible local server) ─────────────────
        } else if (provider == "llama-cpp") {
            auto sysMsg  = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                          = matjson::Value::object();
            requestBody["model"]                 = model;
            requestBody["messages"]              = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["max_completion_tokens"] = 16384;
            requestBody["temperature"]           = 0.7;

            std::string lcUrl = Mod::get()->getSettingValue<std::string>("llama-cpp-url");
            url = lcUrl + "/v1/chat/completions";

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
            requestBody["format"]  = "json"; // tell Ollama to output valid JSON
            requestBody["options"] = options;

            url = ollamaUrl + "/api/generate";

        // ── BYOPAK (custom OpenAI-compatible endpoint) ───────────────────────
        // The user supplies the URL, model name, and (optional) API key + auth
        // header template via mod settings. Body is the standard OpenAI /v1/
        // chat/completions JSON, which covers ~95% of hosted and self-hosted
        // providers (Groq, xAI, Cerebras, Together, Fireworks, Perplexity,
        // DeepInfra, Azure-style, vLLM, Tabby, oogabooga, you name it).
        } else if (provider == "custom") {
            auto sysMsg = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                = matjson::Value::object();
            requestBody["model"]       = model;
            requestBody["messages"]    = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["max_tokens"]  = 8192;
            requestBody["temperature"] = 0.7;

            url = Mod::get()->getSettingValue<std::string>("custom-provider-url");
            // The user may paste the base URL (https://api.x.com) or the full
            // chat-completions URL. If it looks like a base URL (no /chat or
            // /completions in the path), append the OpenAI-compatible path.
            if (url.find("/chat/completions") == std::string::npos
                && url.find("/v1/messages") == std::string::npos
                && url.find("/completions") == std::string::npos) {
                if (!url.empty() && url.back() == '/') url.pop_back();
                url += "/v1/chat/completions";
            }
            log::info("Custom provider URL resolved to: {}", url);
        }

        std::string jsonBody = requestBody.dump();
        log::info("Sending request to {} ({} bytes)", provider, jsonBody.length());

        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");

        if (provider == "gemini") {
            request.header("x-goog-api-key", apiKey);
        } else if (provider == "claude") {
            request.header("x-api-key", apiKey);
            request.header("anthropic-version", "2023-06-01");
        } else if (provider == "openai" || provider == "ministral" || provider == "huggingface" || provider == "deepseek") {
            request.header("Authorization", fmt::format("Bearer {}", apiKey));
        } else if (provider == "openrouter") {
            log::info("OpenRouter key length: {}, starts with: {}", apiKey.length(),
                apiKey.length() > 8 ? apiKey.substr(0, 8) + "..." : "(empty)");
            request.header("Authorization", fmt::format("Bearer {}", apiKey));
            request.header("authorization", fmt::format("Bearer {}", apiKey));
            request.header("Referer", "https://editorai.pages.dev");
            request.header("X-Title", "EditorAI");
        } else if (provider == "lm-studio" || provider == "llama-cpp") {
            // Local servers — no auth needed, generous timeout
            request.timeout(std::chrono::seconds(300));
        } else if (provider == "ollama") {
            // Ollama can be very slow on large models with partial GPU offload.
            int timeoutSec = (int)Mod::get()->getSettingValue<int64_t>("ollama-timeout");
            request.timeout(std::chrono::seconds(timeoutSec));
        } else if (provider == "custom") {
            // BYOPAK: apply the user's auth header template (with ${KEY} subst).
            // Skip entirely if the template is empty or malformed (local servers
            // may need no auth header at all).
            if (auto header = parseCustomAuthHeader(apiKey)) {
                request.header(header->first.c_str(), header->second);
                log::info("Custom provider: auth header '{}' set ({} chars)",
                    header->first, header->second.size());
            } else {
                log::info("Custom provider: no auth header (template empty or no colon)");
            }
            // Generous timeout — could be a local server, a slow paid API, etc.
            request.timeout(std::chrono::seconds(180));
        }

        request.bodyString(jsonBody);
        m_listener.spawn(
            request.post(url),
            [this, provider](web::WebResponse response) {
                this->onAPISuccess(std::move(response), provider);
            }
        );
    }

    // ── Generate button handler ───────────────────────────────────────────────

    void startGeneration(const std::string& prompt, const std::string& apiKey) {
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
        m_generateBtn->setVisible(false);
        m_cancelBtn->setVisible(true);

        // LoadingCircle::show() adds the circle to the running scene and
        // positions it at the screen's true center. Setting a parent layer
        // (m_mainLayer) made the circle position relative to the popup's
        // origin, which is offset from the screen center on widescreen
        // resolutions — that's why it landed in the top-right corner.
        // Don't set a parent layer; show()'s default placement is correct.
        m_loadingCircle = LoadingCircle::create();
        m_loadingCircle->show();
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        m_loadingCircle->setPosition({winSize.width / 2, winSize.height / 2});

        m_generationStartTime = std::chrono::steady_clock::now();
        showStatus("AI is generating...");
        this->schedule(schedule_selector(AIGeneratorPopup::updateGenerationTimer), 1.0f);
        log::info("=== Generation Request === Prompt: {}", prompt);

        addToPromptHistory(prompt);

        // Reset length-enforcement state for this generation. runToolLoop
        // also resets these, but doing it here too keeps the single-shot /
        // custom-provider / edit-mode paths consistent.
        m_accumulatedObjects = matjson::Value::array();
        m_extensionRounds = 0;
        m_passabilityFixRounds = 0;
        m_refinementRounds = 0;
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

    // ── API response handler ──────────────────────────────────────────────────

    void onAPISuccess(web::WebResponse response, const std::string& provider) {
        m_isGenerating = false;
        m_cancelBtn->setVisible(false);
        m_generateBtn->setVisible(true);

        if (m_loadingCircle) {
            m_loadingCircle->fadeAndRemove();
            m_loadingCircle = nullptr;
        }
        if (!m_isCreatingObjects)
            m_generateBtn->setEnabled(true);

        if (!response.ok()) {
            auto [title, message] = parseAPIError(
                response.string().unwrapOr("No error details available"),
                response.code()
            );
            showStatus("Failed!", true);
            FLAlertLayer::create(title.c_str(), gd::string(message), "OK")->show();
            return;
        }

        try {
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

                std::istringstream bodyStream(rawBody);
                std::string line;
                while (std::getline(bodyStream, line)) {
                    // Trim carriage return from Windows line endings
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.empty()) continue;

                    ++lineCount;
                    auto lineJson = matjson::parse(line);
                    if (!lineJson) {
                        // Non-JSON line in the stream — skip silently
                        log::warn("Ollama: skipping non-JSON stream line {}: {}", lineCount, line.substr(0, 80));
                        continue;
                    }

                    auto lineObj = lineJson.unwrap();

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

                auto json = jsonRes.unwrap();

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

                    auto textResult = candidates[0]["content"]["parts"][0]["text"].asString();
                    if (!textResult) { onError("Invalid Response", fmt::format("[{}] Failed to extract text from AI response.", autoErrorCode(60, 3))); return; }
                    aiResponse = textResult.unwrap();

                } else if (provider == "claude") {
                    auto content = json["content"];
                    if (!content.isArray() || content.size() == 0) {
                        onError("No Response", fmt::format("[{}] The AI returned no content.", autoErrorCode(60, 4))); return;
                    }
                    auto textResult = content[0]["text"].asString();
                    if (!textResult) { onError("Invalid Response", fmt::format("[{}] Failed to extract text from AI response.", autoErrorCode(60, 3))); return; }
                    aiResponse = textResult.unwrap();

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
        } catch (std::exception& e) {
            log::error("Exception during response processing: {}", e.what());
            onError("Processing Error", fmt::format("[{}] Exception while processing AI response.", autoErrorCode(60, 99)));
        }
    }

    void onError(const std::string& title, const std::string& message) {
        m_isGenerating = false;
        m_cancelBtn->setVisible(false);
        m_generateBtn->setVisible(true);
        if (m_loadingCircle) {
            m_loadingCircle->fadeAndRemove();
            m_loadingCircle = nullptr;
        }
        m_generateBtn->setEnabled(true);
        showStatus("Failed!", true);
        log::error("Generation failed: {}", message);
        FLAlertLayer::create(title.c_str(), gd::string(message), "OK")->show();
    }

    void closePopup() { this->onClose(nullptr); }

public:
    static AIGeneratorPopup* create(LevelEditorLayer* layer) {
        auto ret = new AIGeneratorPopup();
        if (ret->init(layer)) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }

};

// ─── EditorUI hook — mounts AI button onto "editor-buttons-menu" ─────────────

static CCNode* getAIButton(EditorUI* ui) {
    if (!ui) return nullptr;
    auto menu = ui->getChildByID("editor-buttons-menu");
    if (!menu) return nullptr;
    return menu->getChildByID("ai-button"_spr);
}

class $modify(AIEditorUI, EditorUI) {
    struct Fields {
        bool m_buttonAdded = false;
        CCMenu* m_previewButtonMenu = nullptr;
    };

    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;

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
        if (m_fields->m_buttonAdded) return;

        auto menu = this->getChildByID("editor-buttons-menu");
        if (!menu) {
            log::error("EditorAI: 'editor-buttons-menu' not found — geode.node-ids may not have run");
            return;
        }

        m_fields->m_buttonAdded = true;

        // Single "AI" button. Edit mode is selected inside the popup via a
        // toggle (used to be a separate button + a separate "Clear Level"
        // toggle; the popup's Edit-Mode toggle is the inverse of that).
        auto aiButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("AI", "goldFont.fnt", "GJ_button_04.png", 0.8f),
            this, menu_selector(AIEditorUI::onAIButton)
        );
        aiButton->setID("ai-button"_spr);
        menu->addChild(aiButton);

        menu->updateLayout();

        log::info("EditorAI: AI button mounted on editor-buttons-menu");
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

    // ── Blueprint preview accept/deny UI ─────────────────────────────────────

    void showPreviewButtons() {
        if (m_fields->m_previewButtonMenu) return;

        auto menu = CCMenu::create();
        menu->setID("ai-preview-menu"_spr);

        auto acceptBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Accept", "goldFont.fnt", "GJ_button_01.png", 0.7f),
            this, menu_selector(AIEditorUI::onAcceptPreview)
        );
        acceptBtn->setID("accept-btn"_spr);

        auto editBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Edit", "goldFont.fnt", "GJ_button_02.png", 0.7f),
            this, menu_selector(AIEditorUI::onEditPreview)
        );
        editBtn->setID("edit-btn"_spr);

        auto denyBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Deny", "goldFont.fnt", "GJ_button_06.png", 0.7f),
            this, menu_selector(AIEditorUI::onDenyPreview)
        );
        denyBtn->setID("deny-btn"_spr);

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({70.f, winSize.height - 50.f});
        acceptBtn->setPosition({0.f, 30.f});
        editBtn->setPosition({0.f, 0.f});
        denyBtn->setPosition({0.f, -30.f});

        menu->addChild(acceptBtn);
        menu->addChild(editBtn);
        menu->addChild(denyBtn);
        this->addChild(menu, 1000);

        m_fields->m_previewButtonMenu = menu;
        log::info("EditorAI: preview accept/deny/edit buttons shown");
    }

    void showDoneButton() {
        removePreviewButtons();

        auto menu = CCMenu::create();
        menu->setID("ai-preview-menu"_spr);

        auto doneBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Done", "goldFont.fnt", "GJ_button_01.png", 0.7f),
            this, menu_selector(AIEditorUI::onDoneEditing)
        );
        doneBtn->setID("done-btn"_spr);

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({70.f, winSize.height - 50.f});
        doneBtn->setPosition({0.f, 0.f});

        menu->addChild(doneBtn);
        this->addChild(menu, 1000);

        m_fields->m_previewButtonMenu = menu;
        log::info("EditorAI: Done button shown for edit mode");
    }

    void removePreviewButtons() {
        if (m_fields->m_previewButtonMenu) {
            m_fields->m_previewButtonMenu->removeFromParentAndCleanup(true);
            m_fields->m_previewButtonMenu = nullptr;
        }
    }

    void onAcceptPreview(CCObject*) {
        log::info("EditorAI: accepting {} preview objects", s_previewObjects.size());

        // Before accepting new objects, check if the user edited the PREVIOUS
        // accepted generation — if so, update that feedback entry with edit info.
        if (!s_acceptedSnapshot.empty() && m_editorLayer) {
            auto editSummary = computeEditSummary(m_editorLayer);
            if (!editSummary.empty()) {
                log::info("EditorAI: detected user edits on previous generation: {}", editSummary);
                // Update the most recent accepted feedback entry with the edit summary
                auto entries = loadFeedback();
                for (int i = (int)entries.size() - 1; i >= 0; --i) {
                    if (entries[i].accepted && entries[i].prompt == s_snapshotPrompt) {
                        entries[i].editSummary = editSummary;
                        // Re-save the updated entries
                        auto arr = matjson::Value::array();
                        for (auto& e : entries) {
                            auto obj = matjson::Value::object();
                            obj["prompt"]     = e.prompt;
                            obj["difficulty"] = e.difficulty;
                            obj["style"]      = e.style;
                            obj["length"]     = e.length;
                            if (!e.feedback.empty())          obj["feedback"]          = e.feedback;
                            if (!e.objectsJson.empty())       obj["objectsJson"]       = e.objectsJson;
                            if (!e.editedObjectsJson.empty()) obj["editedObjectsJson"] = e.editedObjectsJson;
                            if (!e.editSummary.empty())       obj["editSummary"]       = e.editSummary;
                            obj["rating"]     = e.rating;
                            obj["accepted"]   = e.accepted;
                            arr.push(obj);
                        }
                        try {
                            std::ofstream file(getFeedbackPath());
                            file << arr.dump();
                            log::info("Updated feedback entry with edit summary");
                        } catch (...) {}
                        break;
                    }
                }
            }
        }

        // Restore objects from ghost to solid
        for (size_t i = 0; i < s_previewObjects.size(); ++i) {
            if (GameObject* obj = s_previewObjects[i]) {
                obj->setOpacity(255);
                obj->setColor(
                    i < s_previewOriginalColors.size()
                        ? s_previewOriginalColors[i]
                        : ccColor3B{255, 255, 255}
                );
            }
        }

        // Snapshot the accepted objects for future edit tracking
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
        s_snapshotPrompt     = s_lastUserPrompt;
        s_snapshotDifficulty = s_lastDifficulty;
        s_snapshotStyle      = s_lastStyle;
        s_snapshotLength     = s_lastLength;

        s_previewObjects.clear();
        s_previewOriginalColors.clear();
        s_inPreviewMode = false;
        removePreviewButtons();

        if (m_editorLayer && m_editorLayer->m_editorUI)
            m_editorLayer->m_editorUI->updateButtons();

        Notification::create("Objects accepted!", NotificationIcon::Success)->show();

        s_lastWasAccepted = true;
        showRatingIfEnabled();
    }

    void onDenyPreview(CCObject*) {
        log::info("EditorAI: denying {} preview objects", s_previewObjects.size());

        if (m_editorLayer) {
            for (auto& objRef : s_previewObjects) {
                if (GameObject* obj = objRef) {
                    try {
                        // Check the object is still in the editor's object array
                        // before attempting removal (prevents crash if already gone)
                        if (obj->getParent() && m_editorLayer->m_objects &&
                            m_editorLayer->m_objects->containsObject(obj)) {
                            m_editorLayer->removeObject(obj, true);
                        } else {
                            // Object not in editor — just detach from scene graph
                            obj->removeFromParentAndCleanup(true);
                        }
                    } catch (...) {
                        log::warn("Failed to remove preview object, skipping");
                    }
                }
            }
        }

        s_previewObjects.clear();
        s_previewOriginalColors.clear();
        s_inPreviewMode = false;
        removePreviewButtons();

        if (m_editorLayer && m_editorLayer->m_editorUI)
            m_editorLayer->m_editorUI->updateButtons();

        Notification::create("Objects denied and removed.", NotificationIcon::Warning)->show();

        s_lastWasAccepted = false;
        showRatingIfEnabled();
    }

    void onEditPreview(CCObject*) {
        log::info("EditorAI: entering edit mode for {} preview objects", s_previewObjects.size());

        // Make objects solid and interactable so the user can edit them
        for (size_t i = 0; i < s_previewObjects.size(); ++i) {
            if (GameObject* obj = s_previewObjects[i]) {
                obj->setOpacity(255);
                obj->setColor(
                    i < s_previewOriginalColors.size()
                        ? s_previewOriginalColors[i]
                        : ccColor3B{255, 255, 255}
                );
            }
        }

        // Snapshot positions BEFORE the user edits — this is the baseline
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
        s_snapshotPrompt     = s_lastUserPrompt;
        s_snapshotDifficulty = s_lastDifficulty;
        s_snapshotStyle      = s_lastStyle;
        s_snapshotLength     = s_lastLength;

        s_previewObjects.clear();
        s_previewOriginalColors.clear();
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
    if (Mod::get()->getSettingValue<bool>("enable-rating")) {
        RatingPopup::create()->show();
    }
}

// ─── EditorPauseLayer hook — restore button visibility on resume ──────────────

class $modify(EditorPauseLayer) {
    void onResume(CCObject* sender) {
        EditorPauseLayer::onResume(sender);
        if (m_editorLayer) {
            if (auto editorUI = m_editorLayer->m_editorUI) {
                if (auto btn = getAIButton(editorUI))
                    btn->setVisible(true);
                if (auto previewMenu = editorUI->getChildByID("ai-preview-menu"_spr))
                    previewMenu->setVisible(true);
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

// ─── Mod startup ─────────────────────────────────────────────────────────────

$execute {
    log::info("========================================");
    log::info("         Editor AI v2.1.6");
    log::info("========================================");
    log::info("Loaded {} object types", OBJECT_IDS.size());
    log::info("Object library: {}", OBJECT_IDS.size() > 10 ? "local file" : "defaults (5 objects)");

    std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
    std::string model    = getProviderModel(provider);
    log::info("Provider: {} | Model: {}", provider, model);

    if (provider == "ollama") {
        bool usePlatinum = Mod::get()->getSettingValue<bool>("use-platinum");
        log::info("Ollama URL: {}", usePlatinum ? "Platinum cloud" : "localhost:11434");
    }

    log::info("Advanced features: {}",
        Mod::get()->getSettingValue<bool>("enable-advanced-features") ? "ON" : "OFF");
    log::info("Object library: {} entries (bundled in .geode)", OBJECT_IDS.size());
    log::info("========================================");
}
