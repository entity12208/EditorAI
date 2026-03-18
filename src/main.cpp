#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <Geode/Geode.hpp>
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

static std::unordered_map<std::string, int> loadObjectIDs() {
    auto path = Mod::get()->getResourcesDir() / "object_ids.json";
    if (std::filesystem::exists(path)) {
        try {
            std::ifstream file(path);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto ids = parseObjectIDs(content, "local file");
            if (!ids.empty()) return ids;
        } catch (std::exception& e) {
            log::error("Error reading local object_ids.json: {}", e.what());
        }
    } else {
        log::warn("Local object_ids.json not found");
    }

    log::warn("Using default object IDs (5 objects only)");
    return {
        {"block_black_gradient_square", 1},
        {"spike_black_gradient_spike", 8},
        {"platform", 1731},
        {"orb_yellow", 36},
        {"pad_yellow", 35}
    };
}

static std::unordered_map<std::string, int> OBJECT_IDS = loadObjectIDs();

static void updateObjectIDsFromGitHub() {
    log::info("Scheduling GitHub object_ids.json update...");
    static async::TaskHolder<web::WebResponse> listener;
    auto request = web::WebRequest();
    listener.spawn(
        request.get("https://raw.githubusercontent.com/entity12208/EditorAI/refs/heads/main/resources/object_ids.json"),
        [](web::WebResponse res) {
            if (res.ok()) {
                auto content = res.string();
                if (content) {
                    auto newIds = parseObjectIDs(content.unwrap(), "GitHub");
                    if (newIds.size() > OBJECT_IDS.size()) {
                        OBJECT_IDS = newIds;
                        log::info("Updated to {} object IDs from GitHub!", OBJECT_IDS.size());
                        Notification::create(
                            fmt::format("Object library updated! ({} objects)", OBJECT_IDS.size()),
                            NotificationIcon::Success
                        )->show();
                    }
                }
            } else {
                log::warn("GitHub fetch failed with HTTP {}", res.code());
            }
        }
    );
}

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

static std::pair<std::string, std::string> parseAPIError(const std::string& errorBody, int statusCode) {
    std::string title   = "API Error";
    std::string message = "An unknown error occurred. Please try again.";
    try {
        auto json = matjson::parse(errorBody);
        if (!json) return {title, message};
        auto error = json.unwrap();
        std::string errorMsg;
        std::string errorStatus; // Google APIs include a "status" string e.g. "PERMISSION_DENIED"

        // Standard format: {"error": {"message": "...", "status": "..."}}  (OpenAI, Claude, Mistral, Gemini)
        if (error.contains("error")) {
            auto errorObj = error["error"];
            if (errorObj.isObject()) {
                auto msgResult = errorObj["message"].asString();
                if (msgResult) errorMsg = msgResult.unwrap();
                // Google-specific: machine-readable status code in error.status
                auto statusResult = errorObj["status"].asString();
                if (statusResult) errorStatus = statusResult.unwrap();
            } else {
                // HuggingFace format: {"error": "message string"}
                auto directMsg = errorObj.asString();
                if (directMsg) errorMsg = directMsg.unwrap();
            }
        }

        // Returns true if the error message/status indicates a bad API key specifically
        // (as opposed to a quota, billing, or permission issue unrelated to key validity).
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
            // 401 = Unauthorized — definitively a bad/missing API key
            title   = "Invalid API Key";
            message = "Your API key was rejected (HTTP 401).\n\nPlease check your API key in mod settings and try again.";
            if (!errorMsg.empty()) message += "\n\nDetail: " + errorMsg.substr(0, 150);

        } else if (statusCode == 403) {
            // 403 = Forbidden — could be a bad key, BUT more commonly means the key
            // is valid but doesn't have access to this resource. Common causes:
            //   - Model requires a paid tier / billing not enabled
            //   - Free-tier quota exhausted for this model
            //   - API not enabled in Google Cloud Console
            //   - IP or referrer restriction on the key
            if (isKeyError()) {
                title   = "Invalid API Key";
                message = "Your API key does not have permission to access this API.\n\nPlease check your API key in mod settings.";
            } else {
                title   = "Access Denied (HTTP 403)";
                message = "The service denied the request. This is usually NOT a bad key.\n\n"
                          "Common causes:\n"
                          "- This model requires a paid plan or billing enabled\n"
                          "- Your free-tier quota for this model is exhausted\n"
                          "- The API is not enabled in your provider dashboard\n\n"
                          "Check your account limits at the provider's dashboard.";
            }
            if (!errorMsg.empty()) message += "\n\nDetail: " + errorMsg.substr(0, 150);

        } else if (statusCode == 404) {
            // 404 = Not Found — usually an invalid model name
            title   = "Model Not Found";
            message = "The specified model was not found (HTTP 404).\n\nPlease check your model setting.";
            if (!errorMsg.empty()) message += "\n\nDetail: " + errorMsg.substr(0, 150);

        } else if (statusCode == 429) {
            title   = "Rate Limit Exceeded";
            message = (errorMsg.find("quota") != std::string::npos || errorMsg.find("Quota") != std::string::npos)
                ? "You've exceeded your API quota.\n\nPlease wait or upgrade your plan."
                : "Too many requests.\n\nPlease wait a moment and try again.";

        } else if (statusCode == 400) {
            // 400 = Bad Request — Gemini returns this for invalid API keys
            // (INVALID_ARGUMENT), so we need to distinguish key errors from
            // other malformed-request errors.
            if (isKeyError()) {
                title   = "Invalid API Key";
                message = "Your API key was rejected by the service.\n\nPlease check your API key in mod settings.";
                if (!errorMsg.empty()) message += "\n\nDetail: " + errorMsg.substr(0, 150);
            } else if (errorMsg.find("model") != std::string::npos
                    || errorStatus == "NOT_FOUND") {
                title   = "Invalid Model";
                message = "The selected model is invalid or not supported.\n\nPlease check your model setting.";
                if (!errorMsg.empty()) message += "\n\nDetail: " + errorMsg.substr(0, 150);
            } else {
                title   = "Invalid Request";
                message = "The request was rejected by the service.";
                if (!errorMsg.empty()) message += "\n\nDetail: " + errorMsg.substr(0, 150);
            }

        } else if (statusCode >= 500) {
            title   = fmt::format("Service Error (HTTP {})", statusCode);
            message = "The AI service is currently unavailable.\n\nPlease try again later.";
            if (!errorMsg.empty()) message += "\n\nDetail: " + errorMsg.substr(0, 150);
        } else if (statusCode == 0) {
            title   = "Connection Failed";
            message = "Could not reach the AI service.\n\n"
                      "Check your internet connection. If using Ollama, make sure it's running "
                      "(run 'ollama serve' in a terminal).";
        } else if (!errorMsg.empty()) {
            title   = fmt::format("API Error (HTTP {})", statusCode);
            message = errorMsg.substr(0, 200);
            if (errorMsg.length() > 200) message += "...";
        } else {
            title   = fmt::format("API Error (HTTP {})", statusCode);
            // Show truncated raw body for debugging
            std::string body = errorBody.substr(0, 200);
            if (errorBody.length() > 200) body += "...";
            message = fmt::format("Unexpected error.\n\nHTTP {}\n\n{}", statusCode, body);
        }
    } catch (...) {
        title   = fmt::format("API Error (HTTP {})", statusCode);
        message = fmt::format("HTTP {} — {}", statusCode, errorBody.substr(0, 200));
    }
    return {title, message};
}

// ─── Per-provider API key / model helpers ─────────────────────────────────────

static std::string getProviderApiKey(const std::string& provider) {
    if (provider == "gemini")       return Mod::get()->getSettingValue<std::string>("gemini-api-key");
    if (provider == "claude")       return Mod::get()->getSettingValue<std::string>("claude-api-key");
    if (provider == "openai")       return Mod::get()->getSettingValue<std::string>("openai-api-key");
    if (provider == "ministral")    return Mod::get()->getSettingValue<std::string>("ministral-api-key");
    if (provider == "huggingface")  return Mod::get()->getSettingValue<std::string>("huggingface-api-key");
    return ""; // ollama / local — no key needed
}

static std::string getProviderModel(const std::string& provider) {
    if (provider == "gemini")       return Mod::get()->getSettingValue<std::string>("gemini-model");
    if (provider == "claude")       return Mod::get()->getSettingValue<std::string>("claude-model");
    if (provider == "openai")       return Mod::get()->getSettingValue<std::string>("openai-model");
    if (provider == "ministral")    return Mod::get()->getSettingValue<std::string>("ministral-model");
    if (provider == "huggingface")  return Mod::get()->getSettingValue<std::string>("huggingface-model");
    if (provider == "ollama")       return Mod::get()->getSettingValue<std::string>("ollama-model");
    return "unknown";
}

static std::string getOllamaUrl() {
    bool usePlatinum = Mod::get()->getSettingValue<bool>("use-platinum");
    return usePlatinum
        ? "https://ollama-proxy-sh88.onrender.com"
        : "http://localhost:11434";
}

// ─── Deferred object struct ───────────────────────────────────────────────────

struct DeferredObject {
    int objectID;
    CCPoint position;
    matjson::Value data;
};

// ─── Blueprint preview shared state ─────────────────────────────────────────
// These statics are shared between AIGeneratorPopup (which creates ghost objects)
// and AIEditorUI (which shows accept/deny buttons). Using statics avoids a
// circular dependency between the two classes.

static bool s_inPreviewMode = false;
static bool s_inEditMode   = false;  // user is editing accepted objects before clicking Done
static std::vector<Ref<GameObject>> s_previewObjects;
static std::vector<ccColor3B> s_previewOriginalColors;

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
        m_feedbackInput->setMaxCharCount(200);
        m_feedbackInput->getInputNode()->setAllowedChars(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789-_=+/\\.,;:!@#$%^&*()[]{}|<>?`~'\" "
        );
        m_mainLayer->addChild(m_feedbackInput);

        // Submit button
        auto submitBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Submit", "goldFont.fnt", "GJ_button_01.png", 0.7f),
            this, menu_selector(RatingPopup::onSubmit)
        );
        auto submitMenu = CCMenu::create();
        submitMenu->setPosition({winSize.width / 2, winSize.height / 2 - 90});
        submitBtn->setPosition({0, 0});
        submitMenu->addChild(submitBtn);
        m_mainLayer->addChild(submitMenu);

        return true;
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

public:
    static RatingPopup* create() {
        auto ret = new RatingPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// Forward declaration — defined after AIEditorUI
static void showRatingIfEnabled();

// ─── Settings popup ──────────────────────────────────────────────────────────

enum class SettingsTab { General, Provider, Advanced };

struct CyclerInfo {
    std::string settingId;
    std::vector<std::string> options;
};
static std::vector<CyclerInfo> s_cyclerInfos;

struct IntInputMeta {
    std::string settingId;
    TextInput* input;
    int64_t min, max, defaultVal;
};

class AISettingsPopup : public Popup {
protected:
    SettingsTab m_currentTab = SettingsTab::General;
    CCNode*     m_contentLayer = nullptr;
    std::vector<std::pair<std::string, TextInput*>> m_textInputs;
    std::vector<IntInputMeta> m_intInputs;
    std::vector<CCMenuItemSpriteExtra*> m_tabBtns;
    async::TaskHolder<web::WebResponse> m_ollamaListener;
    std::vector<std::string> m_ollamaModels;  // auto-detected

    float m_rowY = 0;
    float m_labelX = 0;
    float m_valX = 0;

    bool init() override {
        if (!Popup::init(360.f, 260.f))
            return false;

        auto ws = this->m_size;
        this->setTitle("AI Settings");

        // Tab buttons across the top
        auto tabMenu = CCMenu::create();
        tabMenu->setPosition({ws.width / 2, ws.height / 2 + 85});

        const char* tabNames[] = {"General", "Provider", "Advanced"};
        for (int i = 0; i < 3; ++i) {
            bool active = (i == (int)m_currentTab);
            auto btn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create(tabNames[i], "bigFont.fnt",
                    active ? "GJ_button_01.png" : "GJ_button_04.png", 0.4f),
                this, menu_selector(AISettingsPopup::onTabSwitch)
            );
            btn->setTag(i);
            btn->setPosition({(float)(i - 1) * 95.f, 0});
            tabMenu->addChild(btn);
            m_tabBtns.push_back(btn);
        }
        m_mainLayer->addChild(tabMenu);

        // Content container
        m_contentLayer = CCNode::create();
        m_contentLayer->setPosition({0, 0});
        m_mainLayer->addChild(m_contentLayer);

        buildTab(m_currentTab);
        return true;
    }

    void onClose(CCObject* obj) override {
        saveTextInputs();
        Popup::onClose(obj);
    }

    void saveTextInputs() {
        for (auto& [sid, input] : m_textInputs) {
            Mod::get()->setSettingValue<std::string>(sid, std::string(input->getString()));
        }
        for (auto& meta : m_intInputs) {
            std::string val = meta.input->getString();
            int64_t num = val.empty() ? meta.defaultVal : 0;
            if (!val.empty()) {
                try { num = std::stoll(val); }
                catch (...) { num = meta.defaultVal; }
            }
            num = std::clamp(num, meta.min, meta.max);
            Mod::get()->setSettingValue<int64_t>(meta.settingId, num);
        }
    }

    // ── Tab management ──────────────────────────────────────────────────

    void onTabSwitch(CCObject* sender) {
        saveTextInputs();  // save current tab's inputs before switching
        int tag = static_cast<CCNode*>(sender)->getTag();
        m_currentTab = (SettingsTab)tag;

        // Update tab button appearance
        const char* tabNames[] = {"General", "Provider", "Advanced"};
        for (int i = 0; i < (int)m_tabBtns.size(); ++i) {
            bool active = (i == tag);
            auto parent = m_tabBtns[i]->getParent();
            auto pos = m_tabBtns[i]->getPosition();
            parent->removeChild(m_tabBtns[i], true);
            auto newBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create(tabNames[i], "bigFont.fnt",
                    active ? "GJ_button_01.png" : "GJ_button_04.png", 0.4f),
                this, menu_selector(AISettingsPopup::onTabSwitch)
            );
            newBtn->setTag(i);
            newBtn->setPosition(pos);
            parent->addChild(newBtn);
            m_tabBtns[i] = newBtn;
        }

        buildTab(m_currentTab);
    }

    void buildTab(SettingsTab tab) {
        m_contentLayer->removeAllChildren();
        s_cyclerInfos.clear();
        m_textInputs.clear();
        m_intInputs.clear();

        auto ws = this->m_size;
        m_rowY = ws.height / 2 + 60;
        m_labelX = ws.width / 2 - 60;
        m_valX = ws.width / 2 + 60;

        switch (tab) {
            case SettingsTab::General:  buildGeneralTab(); break;
            case SettingsTab::Provider: buildProviderTab(); break;
            case SettingsTab::Advanced: buildAdvancedTab(); break;
        }
    }

    // ── Row helpers ─────────────────────────────────────────────────────

    void addRowLabel(const char* text) {
        auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
        lbl->setScale(0.3f);
        lbl->setAnchorPoint({1, 0.5f});
        lbl->setPosition({m_labelX, m_rowY});
        m_contentLayer->addChild(lbl);
    }

    void addCycler(const char* label, const char* settingId,
                    std::vector<std::string> options) {
        addRowLabel(label);
        std::string current = Mod::get()->getSettingValue<std::string>(settingId);
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(current.c_str(), "bigFont.fnt", "GJ_button_04.png", 0.4f),
            this, menu_selector(AISettingsPopup::onCycleSetting)
        );
        int idx = (int)s_cyclerInfos.size();
        btn->setTag(idx);
        s_cyclerInfos.push_back({settingId, std::move(options)});

        auto menu = CCMenu::create();
        menu->setPosition({m_valX, m_rowY});
        btn->setPosition({0, 0});
        menu->addChild(btn);
        m_contentLayer->addChild(menu);
        m_rowY -= 30;
    }

    void addToggle(const char* label, const char* settingId) {
        addRowLabel(label);
        bool val = Mod::get()->getSettingValue<bool>(settingId);
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(val ? "ON" : "OFF", "bigFont.fnt",
                val ? "GJ_button_01.png" : "GJ_button_06.png", 0.4f),
            this, menu_selector(AISettingsPopup::onCycleToggle)
        );
        int idx = (int)s_cyclerInfos.size();
        btn->setTag(idx);
        s_cyclerInfos.push_back({settingId, {"false", "true"}});

        auto menu = CCMenu::create();
        menu->setPosition({m_valX, m_rowY});
        btn->setPosition({0, 0});
        menu->addChild(btn);
        m_contentLayer->addChild(menu);
        m_rowY -= 30;
    }

    void addTextRow(const char* label, const char* settingId, const char* placeholder, int maxChars = 100) {
        addRowLabel(label);

        auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        bg->setContentSize({155, 22});
        bg->setColor({0, 0, 0});
        bg->setOpacity(100);
        bg->setPosition({m_valX, m_rowY});
        m_contentLayer->addChild(bg);

        std::string current = Mod::get()->getSettingValue<std::string>(settingId);
        auto input = TextInput::create(145, placeholder, "bigFont.fnt");
        input->setPosition({m_valX, m_rowY});
        input->setScale(0.45f);
        input->setMaxCharCount(maxChars);
        if (!current.empty()) input->setString(current);
        m_contentLayer->addChild(input);
        m_textInputs.push_back({settingId, input});
        m_rowY -= 30;
    }

    void addIntRow(const char* label, const char* settingId, int64_t min, int64_t max, int64_t def) {
        addRowLabel(label);

        auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        bg->setContentSize({80, 22});
        bg->setColor({0, 0, 0});
        bg->setOpacity(100);
        bg->setPosition({m_valX - 20, m_rowY});
        m_contentLayer->addChild(bg);

        int64_t current = Mod::get()->getSettingValue<int64_t>(settingId);
        auto input = TextInput::create(70, fmt::format("{}", def).c_str(), "bigFont.fnt");
        input->setPosition({m_valX - 20, m_rowY});
        input->setScale(0.45f);
        input->setMaxCharCount(6);
        input->getInputNode()->setAllowedChars("0123456789");
        input->setString(fmt::format("{}", current));
        m_contentLayer->addChild(input);
        m_intInputs.push_back({settingId, input, min, max, def});

        // Range hint
        auto hint = CCLabelBMFont::create(
            fmt::format("{}-{}", min, max).c_str(), "bigFont.fnt");
        hint->setScale(0.18f);
        hint->setColor({150, 150, 150});
        hint->setPosition({m_valX + 45, m_rowY});
        m_contentLayer->addChild(hint);

        m_rowY -= 30;
    }

    void addInfoRow(const char* label, const std::string& value, ccColor3B color) {
        addRowLabel(label);
        auto lbl = CCLabelBMFont::create(value.c_str(), "bigFont.fnt");
        lbl->setScale(0.25f);
        lbl->setColor(color);
        lbl->setPosition({m_valX, m_rowY});
        m_contentLayer->addChild(lbl);
        m_rowY -= 30;
    }

    // ── Tab builders ────────────────────────────────────────────────────

    void buildGeneralTab() {
        addCycler("Provider:", "ai-provider",
            {"gemini", "claude", "openai", "ministral", "huggingface", "ollama"});
        addCycler("Difficulty:", "difficulty",
            {"easy", "medium", "hard", "extreme"});
        addCycler("Style:", "style",
            {"modern", "retro", "minimalist", "decorated"});
        addCycler("Length:", "length",
            {"short", "medium", "long", "xl", "xxl"});
        addIntRow("Max Objects:", "max-objects", 10, 10000, 500);
        addIntRow("Spawn Speed:", "spawn-batch-size", 1, 100, 8);
    }

    void buildProviderTab() {
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");

        addInfoRow("Provider:", provider, {100, 255, 100});

        if (provider == "gemini") {
            addCycler("Model:", "gemini-model",
                {"gemini-2.5-flash", "gemini-2.5-pro"});
            addTextRow("API Key:", "gemini-api-key", "Your Google AI Studio key", 200);
        } else if (provider == "claude") {
            addCycler("Model:", "claude-model",
                {"claude-sonnet-4-6", "claude-opus-4-6"});
            addTextRow("API Key:", "claude-api-key", "Your Anthropic key", 200);
        } else if (provider == "openai") {
            addCycler("Model:", "openai-model",
                {"gpt-4o", "gpt-4.1-mini"});
            addTextRow("API Key:", "openai-api-key", "Your OpenAI key", 200);
        } else if (provider == "ministral") {
            addCycler("Model:", "ministral-model",
                {"ministral-3b-latest", "ministral-8b-latest", "mistral-small-latest",
                 "mistral-medium-latest", "mistral-large-latest"});
            addTextRow("API Key:", "ministral-api-key", "Your Mistral AI key", 200);
        } else if (provider == "huggingface") {
            addTextRow("Model:", "huggingface-model", "e.g. meta-llama/Llama-3.1-8B", 200);
            addTextRow("API Key:", "huggingface-api-key", "Your HuggingFace token", 200);
        } else if (provider == "ollama") {
            addToggle("Platinum:", "use-platinum");

            // Both Platinum and local use auto-detect from their respective URLs
            if (m_ollamaModels.empty()) {
                addInfoRow("Models:", "Detecting...", {200, 200, 100});
                fetchOllamaModels();
            } else if (m_ollamaModels.size() == 1 && m_ollamaModels[0].rfind("(", 0) == 0) {
                // Error state — show red message
                addInfoRow("Models:", m_ollamaModels[0], {255, 100, 100});
            } else {
                addCycler("Model:", "ollama-model", m_ollamaModels);
            }

            addIntRow("Timeout (s):", "ollama-timeout", 60, 1800, 600);
        }

        // Hint about key security
        if (provider != "ollama") {
            auto hint = CCLabelBMFont::create("Keys stored locally in Geode save data.", "bigFont.fnt");
            hint->setScale(0.2f);
            hint->setColor({150, 150, 150});
            hint->setPosition({this->m_size.width / 2, m_rowY - 8});
            m_contentLayer->addChild(hint);
        }
    }

    void buildAdvancedTab() {
        addToggle("Triggers/Colors:", "enable-advanced-features");
        addToggle("Rate Limiting:", "enable-rate-limiting");
        addIntRow("Rate Limit (s):", "rate-limit-seconds", 1, 60, 3);
        addToggle("Rate Generations:", "enable-rating");
        addIntRow("Feedback Examples:", "max-feedback-examples", 1, 10, 3);
    }

    // ── Ollama auto-detect ──────────────────────────────────────────────

    void fetchOllamaModels() {
        std::string ollamaUrl = getOllamaUrl();
        bool isPlatinum = Mod::get()->getSettingValue<bool>("use-platinum");
        log::info("Fetching models from {}/api/tags (platinum={})", ollamaUrl, isPlatinum);

        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(isPlatinum ? 10 : 5));

        m_ollamaListener.spawn(
            request.get(ollamaUrl + "/api/tags"),
            [this, isPlatinum](web::WebResponse response) {
                if (!response.ok()) {
                    int code = response.code();
                    log::warn("Failed to fetch models: HTTP {}", code);
                    if (isPlatinum) {
                        m_ollamaModels = {"(Platinum unavailable)"};
                        Notification::create(
                            "Platinum service is not available right now.",
                            NotificationIcon::Error
                        )->show();
                    } else {
                        m_ollamaModels = code == 0
                            ? std::vector<std::string>{"(Ollama not running)"}
                            : std::vector<std::string>{fmt::format("(error: HTTP {})", code)};
                    }
                    buildTab(SettingsTab::Provider);
                    return;
                }

                auto jsonRes = response.json();
                if (!jsonRes) {
                    m_ollamaModels = {"(invalid response)"};
                    buildTab(SettingsTab::Provider);
                    return;
                }

                auto json = jsonRes.unwrap();
                m_ollamaModels.clear();

                if (json.contains("models") && json["models"].isArray()) {
                    for (size_t i = 0; i < json["models"].size(); ++i) {
                        auto nameResult = json["models"][i]["name"].asString();
                        if (nameResult)
                            m_ollamaModels.push_back(nameResult.unwrap());
                    }
                }

                if (m_ollamaModels.empty()) {
                    if (isPlatinum) {
                        m_ollamaModels = {"(no Platinum models available)"};
                        Notification::create(
                            "No models on Platinum. Service may be down.",
                            NotificationIcon::Error
                        )->show();
                    } else {
                        m_ollamaModels = {"(no models installed)"};
                    }
                }

                log::info("Detected {} models from {}", m_ollamaModels.size(),
                    isPlatinum ? "Platinum" : "local Ollama");
                buildTab(SettingsTab::Provider);
            }
        );
    }



    // ── Cycler callbacks ────────────────────────────────────────────────

    void onCycleSetting(CCObject* sender) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        int idx = btn->getTag();
        if (idx < 0 || idx >= (int)s_cyclerInfos.size()) return;

        auto& info = s_cyclerInfos[idx];
        std::string current = Mod::get()->getSettingValue<std::string>(info.settingId);

        int curIdx = 0;
        for (int i = 0; i < (int)info.options.size(); ++i) {
            if (info.options[i] == current) { curIdx = i; break; }
        }
        int nextIdx = (curIdx + 1) % (int)info.options.size();
        std::string next = info.options[nextIdx];

        Mod::get()->setSettingValue(info.settingId, next);

        auto parent = btn->getParent();
        auto pos = btn->getPosition();
        parent->removeChild(btn, true);

        auto newBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(next.c_str(), "bigFont.fnt", "GJ_button_04.png", 0.4f),
            this, menu_selector(AISettingsPopup::onCycleSetting)
        );
        newBtn->setTag(idx);
        newBtn->setPosition(pos);
        parent->addChild(newBtn);

        // If provider changed, rebuild Provider tab data
        if (info.settingId == "ai-provider" && m_currentTab == SettingsTab::Provider) {
            m_ollamaModels.clear();
            buildTab(SettingsTab::Provider);
        }

        log::info("Setting {} -> {}", info.settingId, next);
    }

    void onCycleToggle(CCObject* sender) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        int idx = btn->getTag();
        if (idx < 0 || idx >= (int)s_cyclerInfos.size()) return;

        auto& info = s_cyclerInfos[idx];
        bool current = Mod::get()->getSettingValue<bool>(info.settingId);
        bool next = !current;

        Mod::get()->setSettingValue(info.settingId, next);

        // If use-platinum changed, rebuild provider tab to show different model list
        if (info.settingId == "use-platinum") {
            m_ollamaModels.clear();  // force re-detect if switching off platinum
            buildTab(SettingsTab::Provider);
            return;
        }

        auto parent = btn->getParent();
        auto pos = btn->getPosition();
        parent->removeChild(btn, true);

        auto newBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(next ? "ON" : "OFF", "bigFont.fnt",
                next ? "GJ_button_01.png" : "GJ_button_06.png", 0.4f),
            this, menu_selector(AISettingsPopup::onCycleToggle)
        );
        newBtn->setTag(idx);
        newBtn->setPosition(pos);
        parent->addChild(newBtn);

        log::info("Setting {} -> {}", info.settingId, next);
    }

public:
    static AISettingsPopup* create() {
        auto ret = new AISettingsPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
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
    CCMenuItemToggler*       m_clearToggle    = nullptr;

    bool                     m_shouldClearLevel = false;
    bool                     m_isGenerating     = false;

    LevelEditorLayer*        m_editorLayer    = nullptr;

    async::TaskHolder<web::WebResponse> m_listener;

    std::vector<DeferredObject> m_deferredObjects;
    size_t m_currentObjectIndex = 0;
    bool   m_isCreatingObjects  = false;
    std::chrono::steady_clock::time_point m_generationStartTime;

    // ── init ──────────────────────────────────────────────────────────────────

    bool init(LevelEditorLayer* editorLayer) {
        if (!Popup::init(420.f, 300.f))
            return false;

        m_editorLayer = editorLayer;
        auto winSize  = this->m_size;
        this->setTitle("Editor AI");

        auto descLabel = CCLabelBMFont::create("Describe the level you want to generate:", "bigFont.fnt");
        descLabel->setScale(0.45f);
        descLabel->setPosition({winSize.width / 2, winSize.height / 2 + 70});
        m_mainLayer->addChild(descLabel);

        auto inputBG = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        inputBG->setContentSize({330, 100});
        inputBG->setColor({0, 0, 0});
        inputBG->setOpacity(100);
        inputBG->setPosition({winSize.width / 2, winSize.height / 2 + 15});
        m_mainLayer->addChild(inputBG);

        m_promptInput = TextInput::create(320, "e.g. Medium difficulty platforming", "bigFont.fnt");
        m_promptInput->setPosition({winSize.width / 2, winSize.height / 2 + 15});
        m_promptInput->setScale(0.65f);
        m_promptInput->setMaxCharCount(200);
        m_promptInput->getInputNode()->setAllowedChars(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789-_=+/\\.,;:!@#$%^&*()[]{}|<>?`~'\" "
        );
        m_mainLayer->addChild(m_promptInput);

        // Prompt history arrows (left of input)
        auto historyMenu = CCMenu::create();
        historyMenu->setPosition({winSize.width / 2 + 180, winSize.height / 2 + 15});

        auto upArrow = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
                s->setScale(0.35f); s->setRotation(-90); return s; }(),
            this, menu_selector(AIGeneratorPopup::onHistoryUp)
        );
        upArrow->setPosition({0, 15});
        historyMenu->addChild(upArrow);

        auto downArrow = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
                s->setScale(0.35f); s->setRotation(90); return s; }(),
            this, menu_selector(AIGeneratorPopup::onHistoryDown)
        );
        downArrow->setPosition({0, -15});
        historyMenu->addChild(downArrow);
        m_mainLayer->addChild(historyMenu);

        // Clear-level toggle — default OFF
        auto clearLabel = CCLabelBMFont::create("Clear level before generating", "bigFont.fnt");
        clearLabel->setScale(0.4f);

        auto onSpr  = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        onSpr->setScale(0.7f);
        offSpr->setScale(0.7f);

        m_clearToggle = CCMenuItemToggler::create(offSpr, onSpr, this,
            menu_selector(AIGeneratorPopup::onToggleClear));
        m_clearToggle->toggle(false);

        auto toggleMenu = CCMenu::create();
        toggleMenu->setPosition({winSize.width / 2, winSize.height / 2 - 35});
        m_clearToggle->setPosition({-80, 0});
        clearLabel->setPosition({20, 0});
        toggleMenu->addChild(m_clearToggle);
        toggleMenu->addChild(clearLabel);
        m_mainLayer->addChild(toggleMenu);

        m_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_statusLabel->setScale(0.35f);
        m_statusLabel->setPosition({winSize.width / 2, winSize.height / 2 - 55});
        m_statusLabel->setVisible(false);
        m_mainLayer->addChild(m_statusLabel);

        // Generate + Cancel buttons
        auto btnMenu = CCMenu::create();
        btnMenu->setPosition({winSize.width / 2, winSize.height / 2 - 90});

        m_generateBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Generate", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(AIGeneratorPopup::onGenerate)
        );
        m_generateBtn->setPosition({0, 0});
        btnMenu->addChild(m_generateBtn);

        m_cancelBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Cancel", "goldFont.fnt", "GJ_button_06.png", 0.7f),
            this, menu_selector(AIGeneratorPopup::onCancel)
        );
        m_cancelBtn->setPosition({0, 0});
        m_cancelBtn->setVisible(false);
        btnMenu->addChild(m_cancelBtn);
        m_mainLayer->addChild(btnMenu);

        // Top-right: info button
        auto cornerMenu = CCMenu::create();
        cornerMenu->setPosition({winSize.width - 25, winSize.height - 25});
        auto infoBtn = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png"); s->setScale(0.7f); return s; }(),
            this, menu_selector(AIGeneratorPopup::onInfo)
        );
        infoBtn->setPosition({0, 0});
        cornerMenu->addChild(infoBtn);
        m_mainLayer->addChild(cornerMenu);

        // Top-left: settings button
        auto settingsMenu = CCMenu::create();
        settingsMenu->setPosition({25, winSize.height - 25});
        auto settingsBtn = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png"); s->setScale(0.4f); return s; }(),
            this, menu_selector(AIGeneratorPopup::onSettings)
        );
        settingsBtn->setPosition({0, 0});
        settingsMenu->addChild(settingsBtn);
        m_mainLayer->addChild(settingsMenu);

        this->schedule(schedule_selector(AIGeneratorPopup::updateObjectCreation), 0.05f);

        return true;
    }

    // ── UI callbacks ──────────────────────────────────────────────────────────

    void onToggleClear(CCObject*) {
        m_shouldClearLevel = !m_shouldClearLevel;
        log::info("Clear level toggle: {}", m_shouldClearLevel ? "ON" : "OFF");
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

    void onSettings(CCObject*);  // defined after AISettingsPopup class

    void onInfo(CCObject*) {
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model    = getProviderModel(provider);
        std::string apiKey   = getProviderApiKey(provider);

        std::string keyStatus;
        if (provider == "ollama") {
            bool usePlatinum = Mod::get()->getSettingValue<bool>("use-platinum");
            keyStatus = usePlatinum ? "<cg>Platinum cloud</c>" : "<cg>Local — no key needed</c>";
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

    void prepareObjects(matjson::Value& objectsArray) {
        if (!m_editorLayer || !objectsArray.isArray()) return;

        // Clear any leftover preview state from a previous generation
        s_previewObjects.clear();
        s_previewOriginalColors.clear();

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

        // If any object sits below ground, shift the entire set upward so the
        // lowest object lands exactly at Y=0. Preserves relative layout instead
        // of clamping each object individually (which would crush structures).
        if (!m_deferredObjects.empty()) {
            float minY = m_deferredObjects[0].position.y;
            for (auto& obj : m_deferredObjects)
                minY = std::min(minY, obj.position.y);

            if (minY < 0.0f) {
                float shift = -minY;
                log::info("Shifting all objects up by {:.1f} units (lowest was at Y={:.1f})", shift, minY);
                for (auto& obj : m_deferredObjects)
                    obj.position.y += shift;
            }
        }

        log::info("Prepared {} valid objects", m_deferredObjects.size());

        if (m_deferredObjects.empty()) {
            onError("No Valid Objects",
                "The AI response contained no usable objects.\n\n"
                "This can happen when the model uses unknown type names or omits "
                "required fields (x, y). Try rephrasing your prompt or switching "
                "to a more capable model.");
            return;
        }

        m_isCreatingObjects = true;
        showStatus("Starting object creation...", false);
    }

    // ── System prompt ─────────────────────────────────────────────────────────

    std::string buildSystemPrompt() {
        bool advFeatures = Mod::get()->getSettingValue<bool>("enable-advanced-features");

        std::string objectList;
        objectList.reserve(OBJECT_IDS.size() * 30);
        bool first = true;
        for (auto& [name, id] : OBJECT_IDS) {
            if (!first) objectList += ", ";
            objectList += name;
            first = false;
        }

        std::string base = fmt::format(
            "You are a Geometry Dash level designer AI.\n\n"
            "Return ONLY valid JSON — no markdown, no explanations, no code fences.\n\n"
            "Available objects: {}\n\n"
            "JSON Format:\n"
            "{{\n"
            "  \"analysis\": \"Brief reasoning about layout and design choices\",\n"
            "  \"objects\": [\n"
            "    {{\"type\": \"block_black_gradient_square\", \"x\": 0, \"y\": 10}},\n"
            "    {{\"type\": \"spike_black_gradient_spike\", \"x\": 50, \"y\": 0}}\n"
            "  ]\n"
            "}}\n\n"
            "COORDINATE SYSTEM — read carefully:\n"
            "  X = horizontal position. 10 units = 1 grid cell. Level runs left to right.\n"
            "  Y = vertical position. 0 = ground. 10 = one block above ground. Y must always be >= 0.\n"
            "  One grid cell is 10 units. A block sitting ON the ground has Y=0.\n"
            "  A block one cell above ground has Y=10. Two cells above = Y=20.\n\n"
            "OBSTACLE SPACING (X distance between obstacles):\n"
            "  EASY=50-70  MEDIUM=30-50  HARD=20-30  EXTREME=10-20\n\n"
            "LEVEL LENGTH (total X span):\n"
            "  SHORT=200-400  MEDIUM=400-800  LONG=800-1600  XL=1600-3200  XXL=3200+\n\n"
            "RULES:\n"
            "  - Only use type names from the available objects list above. Never invent new names.\n"
            "  - Y must be >= 0. Never place objects with negative Y.\n"
            "  - Vary object types to create interesting, playable layouts.\n"
            "  - A spike or hazard sitting on the ground has Y=0.\n"
            "  - Platforms and blocks used as stepping stones should be at Y=10 or Y=20.",
            objectList
        );

        if (advFeatures) {
            base +=
                "\n\n"
                "ADVANCED FEATURES (enabled):\n\n"

                "JSON FORMAT RULES — follow exactly or the response will fail to parse:\n"
                "  * Return ONLY the JSON object — no markdown, no comments, no trailing commas\n"
                "  * All string values must use double quotes\n"
                "  * Numbers must not be quoted: \"x\": 50 not \"x\": \"50\"\n"
                "  * Arrays use square brackets: \"groups\": [1, 5]\n"
                "  * Do not include fields with null values — omit them entirely\n\n"

                "1. GROUP IDs — Assign up to 10 group IDs per object with the optional \"groups\" array.\n"
                "   Objects must be in a group for a move/toggle/rotate/alpha trigger to target them.\n"
                "   Example: {\"type\": \"block_black_gradient_square\", \"x\": 100, \"y\": 10, \"groups\": [1]}\n\n"

                "2. COLOR CHANNELS — Assign any object to a color channel so it responds to color triggers.\n"
                "   \"color_channel\": integer 1-999 — the base (primary) color channel\n"
                "   \"detail_color_channel\": integer 1-999 — the detail (secondary) color channel\n"
                "   Standard channels: 1=BG, 2=Ground, 3=Line, 4=3DL, 5=Object, 6=Player1, 7=Player2\n"
                "   Use custom channels (e.g. 10, 11, 12) and pair with color triggers to create themed sections.\n"
                "   All objects on the same channel change color together when a color trigger targeting that channel fires.\n"
                "   Example: {\"type\":\"block_black_gradient_square\",\"x\":100,\"y\":10,\"color_channel\":10}\n\n"

                "3. MULTI-ACTIVATE — Add \"multi_activate\": true to any orb, pad, portal, or trigger\n"
                "   to let it fire every time the player touches it, not just the first time.\n"
                "   Useful for orbs in repeating sections or triggers that should fire on every pass.\n\n"

                "4. COLOR TRIGGERS (type \"color_trigger\", ID 899)\n"
                "   Changes a color channel over time. Fires when the player reaches its X position.\n"
                "   Required fields:\n"
                "     \"color_channel\": integer 1-999 — which GD channel to change\n"
                "       (1=Background, 2=Ground1, 3=Line, 4=Object, 1000=Player1, 1001=Player2)\n"
                "     \"color\": \"#RRGGBB\" — target hex color (6 hex digits after #)\n"
                "   Optional fields:\n"
                "     \"duration\": float seconds (default 0.5)\n"
                "     \"blending\": true/false — additive blend (default false)\n"
                "     \"opacity\": 0.0-1.0 — channel opacity (default 1.0)\n"
                "   Example: {\"type\":\"color_trigger\",\"x\":170,\"y\":0,\"color_channel\":1,\"color\":\"#FF4400\",\"duration\":1.0}\n\n"

                "5. MOVE TRIGGERS (type \"move_trigger\", ID 901)\n"
                "   Moves a group of objects by an offset over time. Objects must have a matching group ID.\n"
                "   Place at y=0 so the player activates it on the ground.\n"
                "   Required fields:\n"
                "     \"target_group\": integer 1-9999 — group ID to move\n"
                "     \"move_x\": float — horizontal offset in GD units (10 = 1 grid cell, negative = left)\n"
                "     \"move_y\": float — vertical offset (positive = up)\n"
                "   Optional fields:\n"
                "     \"duration\": float seconds (default 0.5)\n"
                "     \"easing\": integer 0-18\n"
                "       0=None, 1=EaseInOut, 2=EaseIn, 3=EaseOut,\n"
                "       4=ElasticInOut, 5=ElasticIn, 6=ElasticOut,\n"
                "       7=BounceInOut, 8=BounceIn, 9=BounceOut,\n"
                "       10=ExponentialInOut, 11=ExponentialIn, 12=ExponentialOut,\n"
                "       13=SineInOut, 14=SineIn, 15=SineOut,\n"
                "       16=BackInOut, 17=BackIn, 18=BackOut\n"
                "     \"easing_rate\": float (0.01-100, controls how extreme the easing curve is, default ~2.0)\n"
                "     \"lock_to_player_x\": true — object follows the player's X position\n"
                "     \"lock_to_player_y\": true — object follows the player's Y position\n"
                "   IMPORTANT: The trigger and the objects it moves are SEPARATE. Place the trigger\n"
                "   where the player will reach it; place the objects wherever they should start.\n"
                "   Example:\n"
                "     Objects:  {\"type\":\"block_black_gradient_square\",\"x\":270,\"y\":30,\"groups\":[2]}\n"
                "     Trigger:  {\"type\":\"move_trigger\",\"x\":170,\"y\":0,\"target_group\":2,\"move_x\":0,\"move_y\":30,\"duration\":0.5,\"easing\":1}\n\n"

                "6. ALPHA TRIGGER (type \"alpha_trigger\", ID 1007)\n"
                "   Fades a group's opacity in or out over time. Great for making objects appear/disappear.\n"
                "   Required fields:\n"
                "     \"target_group\": integer 1-9999 — group ID to fade\n"
                "     \"opacity\": 0.0-1.0 — target opacity (0 = invisible, 1 = fully visible)\n"
                "   Optional fields:\n"
                "     \"duration\": float seconds (default 0.5)\n"
                "     \"easing\": integer 0-18 (same values as move trigger)\n"
                "     \"easing_rate\": float (0.01-100)\n"
                "   Example: {\"type\":\"alpha_trigger\",\"x\":200,\"y\":0,\"target_group\":3,\"opacity\":0.0,\"duration\":1.0}\n\n"

                "7. ROTATE TRIGGER (type \"rotate_trigger\", ID 1346)\n"
                "   Rotates a group of objects around a center point over time.\n"
                "   Required fields:\n"
                "     \"target_group\": integer 1-9999 — group to rotate\n"
                "     \"degrees\": float — rotation amount (positive = clockwise, negative = counter-clockwise)\n"
                "   Optional fields:\n"
                "     \"center_group\": integer 1-9999 — group ID to use as the rotation center\n"
                "       (if omitted, rotates around each object's own center)\n"
                "     \"duration\": float seconds (default 0.5)\n"
                "     \"easing\": integer 0-18 (same values as move trigger)\n"
                "     \"easing_rate\": float (0.01-100)\n"
                "     \"lock_object_rotation\": true — objects keep facing the same direction while orbiting\n"
                "   Example: {\"type\":\"rotate_trigger\",\"x\":300,\"y\":0,\"target_group\":4,\"center_group\":5,\"degrees\":360,\"duration\":2.0,\"easing\":1}\n\n"

                "8. TOGGLE TRIGGER (type \"toggle_trigger\", ID 1049)\n"
                "   Shows or hides a group of objects instantly. Use to reveal hidden paths or remove obstacles.\n"
                "   Required fields:\n"
                "     \"target_group\": integer 1-9999 — group to toggle\n"
                "     \"activate_group\": true/false — true = show the group, false = hide it\n"
                "   Example: {\"type\":\"toggle_trigger\",\"x\":500,\"y\":0,\"target_group\":6,\"activate_group\":false}\n\n"

                "9. PULSE TRIGGER (type \"pulse_trigger\", ID 1006)\n"
                "   Pulses a group or color channel with a temporary color flash. Great for visual emphasis.\n"
                "   Target ONE of:\n"
                "     \"target_group\": integer — pulse objects in this group\n"
                "     \"target_color_channel\": integer — pulse this color channel instead\n"
                "   Required fields:\n"
                "     \"color\": \"#RRGGBB\" — the color to pulse\n"
                "   Optional fields:\n"
                "     \"fade_in\": float seconds (default 0.0)\n"
                "     \"hold\": float seconds (default 0.0)\n"
                "     \"fade_out\": float seconds (default 0.0)\n"
                "     \"exclusive\": true/false — only this pulse affects the target (default false)\n"
                "   Example: {\"type\":\"pulse_trigger\",\"x\":400,\"y\":0,\"target_group\":1,\"color\":\"#FF0000\",\"fade_in\":0.1,\"hold\":0.3,\"fade_out\":0.5}\n\n"

                "10. SPAWN TRIGGER (type \"spawn_trigger\", ID 1268)\n"
                "   Activates another trigger group after a delay. Lets you chain trigger sequences\n"
                "   or fire triggers that aren't at ground level.\n"
                "   Required fields:\n"
                "     \"target_group\": integer 1-9999 — group containing the trigger(s) to activate\n"
                "   Optional fields:\n"
                "     \"delay\": float seconds (default 0.0)\n"
                "     \"editor_disable\": true — prevent this trigger from firing in the editor\n"
                "   Example: {\"type\":\"spawn_trigger\",\"x\":100,\"y\":0,\"target_group\":7,\"delay\":0.5}\n\n"

                "11. STOP TRIGGER (type \"stop_trigger\", ID 1616)\n"
                "    Stops all active triggers targeting a specific group. Use to cancel ongoing\n"
                "    move/rotate/alpha animations.\n"
                "    Required fields:\n"
                "      \"target_group\": integer 1-9999 — group whose triggers to stop\n"
                "    Example: {\"type\":\"stop_trigger\",\"x\":600,\"y\":0,\"target_group\":2}\n\n"

                "12. SPEED PORTALS — Change the game speed. Place at y=0. No extra fields needed.\n"
                "    Types: \"speed_portal_half\" (0.5x slow), \"speed_portal_normal\" (1x),\n"
                "    \"speed_portal_double\" (2x), \"speed_portal_triple\" (3x), \"speed_portal_quadruple\" (4x)\n"
                "    Example: {\"type\":\"speed_portal_double\",\"x\":300,\"y\":0}\n\n"

                "13. PLAYER VISIBILITY TRIGGERS — No extra fields. Place at y=0.\n"
                "    \"show_player_trigger\" (ID 1613) — makes the player icon visible\n"
                "    \"hide_player_trigger\" (ID 1612) — makes the player icon invisible\n"
                "    Use hide before a cutscene/effect, show after to restore visibility.\n\n"

                "14. TRAIL TRIGGERS — No extra fields. Place at y=0.\n"
                "    \"show_trail_trigger\" (or \"effect_enable_ghost_trail\", ID 32) — enables the player's ghost trail\n"
                "    \"hide_trail_trigger\" (or \"effect_disable_ghost_trail\", ID 33) — disables the ghost trail\n\n"

                "15. END TRIGGER — Mark the end of the level. No extra fields.\n"
                "    \"end_trigger\" (or \"effect_10_level_end_trigger\", ID 34)\n"
                "    Place at the X position where the level should end.\n"
                "    Example: {\"type\":\"end_trigger\",\"x\":2000,\"y\":0}\n\n"

                "TRIGGER DESIGN TIPS:\n"
                "- All triggers fire when the player reaches their X position. Place at y=0.\n"
                "- Keep group IDs consistent — if a trigger targets group 2, the affected objects\n"
                "  must have 2 in their \"groups\" array.\n"
                "- Color triggers set mood at section changes. Alpha triggers create fade-in reveals.\n"
                "- Move triggers add dynamic platforms. Rotate triggers spin decorations or obstacles.\n"
                "- Toggle triggers hide/show secret paths. Pulse triggers add visual emphasis at drops.\n"
                "- Spawn triggers chain sequences. Stop triggers cancel ongoing animations.\n"
                "- Speed portals control pacing — slow down for tricky sections, speed up for straights.\n"
                "- Use multi_activate on orbs/triggers in looping sections.\n";
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

            constexpr size_t FEEDBACK_CHAR_BUDGET = 8000;
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

        return base;
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

        std::string fullPrompt = fmt::format(
            "Generate a Geometry Dash level:\n\n"
            "Request: {}\nDifficulty: {}\nStyle: {}\nLength: {}{}\n\n"
            "Return JSON with analysis and objects array.",
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
        }

        std::string jsonBody = requestBody.dump();
        log::info("Sending request to {} ({} bytes)", provider, jsonBody.length());

        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");

        if (provider == "gemini") {
            // x-goog-api-key header is required for Gemini 2.5+ models.
            request.header("x-goog-api-key", apiKey);
        } else if (provider == "claude") {
            request.header("x-api-key", apiKey);
            request.header("anthropic-version", "2023-06-01");
        } else if (provider == "openai" || provider == "ministral" || provider == "huggingface") {
            request.header("Authorization", fmt::format("Bearer {}", apiKey));
        } else if (provider == "ollama") {
            // Ollama can be very slow on large models with partial GPU offload.
            int timeoutSec = (int)Mod::get()->getSettingValue<int64_t>("ollama-timeout");
            request.timeout(std::chrono::seconds(timeoutSec));
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

        m_loadingCircle = LoadingCircle::create();
        m_loadingCircle->setParentLayer(m_mainLayer);
        m_loadingCircle->show();
        m_loadingCircle->setPosition(m_mainLayer->getContentSize() / 2);

        m_generationStartTime = std::chrono::steady_clock::now();
        showStatus("AI is generating...");
        this->schedule(schedule_selector(AIGeneratorPopup::updateGenerationTimer), 1.0f);
        log::info("=== Generation Request === Prompt: {}", prompt);

        addToPromptHistory(prompt);
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

        if (apiKey.empty() && provider != "ollama") {
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
                    onError("Invalid Response", "The API returned invalid data.");
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
                        onError("Ollama Error", errorMsg.unwrap());
                        return;
                    }
                }

                log::info("Ollama stream: {} lines parsed, done={}, accumulated {} chars",
                    lineCount, isDone, accumulated.size());

                if (!isDone) {
                    onError("Incomplete Response",
                        "Ollama stopped generating before the level was complete.\n\n"
                        "Try requesting a shorter level, reducing the max-objects setting, "
                        "or using a model with a larger context window (e.g. llama3.1:8b or mistral:7b).");
                    return;
                }

                if (accumulated.empty()) {
                    onError("Invalid Response", "Ollama returned an empty response.");
                    return;
                }

                aiResponse = accumulated;

            // ── All other providers: standard single-JSON response ─────────────
            } else {
                auto jsonRes = response.json();
                if (!jsonRes) {
                    onError("Invalid Response", "The API returned invalid data.");
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
                                    fmt::format("Gemini blocked the request before generating.\n\nReason: {}\n\n"
                                        "Try rephrasing your prompt.", reason));
                                return;
                            }
                        }
                    }

                    auto candidates = json["candidates"];
                    if (!candidates.isArray() || candidates.size() == 0) {
                        onError("No Response", "The AI didn't generate any content."); return;
                    }

                    auto finishReasonResult = candidates[0]["finishReason"].asString();
                    if (finishReasonResult) {
                        const std::string& finishReason = finishReasonResult.unwrap();
                        if (finishReason == "SAFETY") {
                            onError("Response Blocked",
                                "Gemini's safety filter blocked the generated level.\n\n"
                                "Try rephrasing your prompt or using different difficulty/style settings.");
                            return;
                        }
                        if (finishReason == "RECITATION") {
                            onError("Response Blocked",
                                "Gemini blocked the response due to recitation policy.\n\n"
                                "Try rephrasing your prompt.");
                            return;
                        }
                        if (finishReason == "MAX_TOKENS") {
                            log::warn("Gemini hit max token limit — response may be truncated");
                        }
                    }

                    auto textResult = candidates[0]["content"]["parts"][0]["text"].asString();
                    if (!textResult) { onError("Invalid Response", "Failed to extract AI response."); return; }
                    aiResponse = textResult.unwrap();

                } else if (provider == "claude") {
                    auto content = json["content"];
                    if (!content.isArray() || content.size() == 0) {
                        onError("No Response", "The AI didn't generate any content."); return;
                    }
                    auto textResult = content[0]["text"].asString();
                    if (!textResult) { onError("Invalid Response", "Failed to extract AI response."); return; }
                    aiResponse = textResult.unwrap();

                // OpenAI, Mistral AI, and HuggingFace all use the same response format
                } else if (provider == "openai" || provider == "ministral" || provider == "huggingface") {
                    auto choices = json["choices"];
                    if (!choices.isArray() || choices.size() == 0) {
                        onError("No Response", "The AI didn't generate any content."); return;
                    }
                    auto textResult = choices[0]["message"]["content"].asString();
                    if (!textResult) { onError("Invalid Response", "Failed to extract AI response."); return; }
                    aiResponse = textResult.unwrap();
                }
            }

            // Log the full raw AI response (chunked to prevent truncation)
            log::info("=== Full AI Response from {} ===", provider);
            logLong("AIResponse", aiResponse);
            log::info("=== End AI Response ===");

            // Strip markdown code fence markers (``` and optional language tag)
            // while preserving the content between them.
            {
                size_t pos = 0;
                while ((pos = aiResponse.find("```", pos)) != std::string::npos) {
                    size_t end = pos + 3;
                    // Skip optional language tag on same line (e.g. "json")
                    while (end < aiResponse.size() && aiResponse[end] != '\n' && aiResponse[end] != '\r')
                        ++end;
                    aiResponse.erase(pos, end - pos);
                }
            }

            // Extract JSON object
            size_t jsonStart = aiResponse.find('{');
            size_t jsonEnd   = aiResponse.rfind('}');
            if (jsonStart == std::string::npos || jsonEnd == std::string::npos) {
                onError("Invalid Response", "No valid level data found in response."); return;
            }
            aiResponse = aiResponse.substr(jsonStart, jsonEnd - jsonStart + 1);

            auto levelJson = matjson::parse(aiResponse);
            if (!levelJson) { onError("Parse Error", "Failed to parse level data."); return; }

            auto levelData = levelJson.unwrap();
            if (!levelData.contains("objects")) {
                onError("Invalid Data", "Response doesn't contain level objects."); return;
            }

            auto objectsArray = levelData["objects"];
            if (!objectsArray.isArray() || objectsArray.size() == 0) {
                onError("No Objects", "The AI didn't generate any objects."); return;
            }

            // Capture the generated objects JSON for feedback storage
            s_lastGeneratedJson = objectsArray.dump();

            // Log analysis field if present
            auto analysisResult = levelData["analysis"].asString();
            if (analysisResult) {
                log::info("AI Analysis: {}", analysisResult.unwrap());
            }

            log::info("Parsed {} objects from AI response", objectsArray.size());

            if (m_shouldClearLevel) clearLevel();
            prepareObjects(objectsArray);

        } catch (std::exception& e) {
            log::error("Exception during response processing: {}", e.what());
            onError("Processing Error", "Failed to process AI response.");
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

// Out-of-line definition — AISettingsPopup is defined before AIGeneratorPopup
void AIGeneratorPopup::onSettings(CCObject*) {
    AISettingsPopup::create()->show();
}

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
    log::info("========================================");

    Loader::get()->queueInMainThread([] {
        updateObjectIDsFromGitHub();
    });
}
