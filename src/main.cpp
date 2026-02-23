#include <algorithm>
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/ui/TextInput.hpp>

using namespace geode::prelude;

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

        // Standard format: {"error": {"message": "..."}}  (OpenAI, Claude, Mistral)
        if (error.contains("error")) {
            auto errorObj = error["error"];
            if (errorObj.isObject() && errorObj.contains("message")) {
                auto msgResult = errorObj["message"].asString();
                if (msgResult) errorMsg = msgResult.unwrap();
            } else {
                // HuggingFace format: {"error": "message string"}
                auto directMsg = errorObj.asString();
                if (directMsg) errorMsg = directMsg.unwrap();
            }
        }

        if (statusCode == 401 || statusCode == 403) {
            title   = "Invalid API Key";
            message = "Your API key is invalid or expired.\n\nPlease check your API key in mod settings and try again.";
        } else if (statusCode == 429) {
            title   = "Rate Limit Exceeded";
            message = (errorMsg.find("quota") != std::string::npos || errorMsg.find("Quota") != std::string::npos)
                ? "You've exceeded your API quota.\n\nPlease wait or upgrade your plan."
                : "Too many requests.\n\nPlease wait a moment and try again.";
        } else if (statusCode == 400) {
            title   = "Invalid Request";
            message = errorMsg.find("model") != std::string::npos
                ? "The selected model is invalid.\n\nPlease check your model setting."
                : "The request was invalid.\n\nPlease check your settings and try again.";
            if (!errorMsg.empty())
                message += "\n\nDetail: " + errorMsg.substr(0, 150);
        } else if (statusCode >= 500) {
            title   = "Service Error";
            message = "The AI service is currently unavailable.\n\nPlease try again later.";
        } else if (!errorMsg.empty()) {
            message = errorMsg.substr(0, 200);
            if (errorMsg.length() > 200) message += "...";
        }
    } catch (...) {}
    return {title, message};
}

// ─── Per-provider API key / model helpers ─────────────────────────────────────

static std::string getProviderApiKey(const std::string& provider) {
    if (provider == "gemini")       return Mod::get()->getSettingValue<std::string>("gemini-api-key");
    if (provider == "claude")       return Mod::get()->getSettingValue<std::string>("claude-api-key");
    if (provider == "openai")       return Mod::get()->getSettingValue<std::string>("openai-api-key");
    if (provider == "ministral")    return Mod::get()->getSettingValue<std::string>("ministral-api-key");
    if (provider == "huggingface")  return Mod::get()->getSettingValue<std::string>("huggingface-api-key");
    return ""; // ollama — no key needed
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

// ─── Main generation popup ────────────────────────────────────────────────────

class AIGeneratorPopup : public Popup {
protected:
    TextInput*               m_promptInput    = nullptr;
    CCLabelBMFont*           m_statusLabel    = nullptr;
    LoadingCircle*           m_loadingCircle  = nullptr;
    CCMenuItemSpriteExtra*   m_generateBtn    = nullptr;
    CCMenuItemToggler*       m_clearToggle    = nullptr;

    // Default to false — clearing is destructive and must be explicitly opted into.
    bool                     m_shouldClearLevel = false;

    LevelEditorLayer*        m_editorLayer    = nullptr;

    async::TaskHolder<web::WebResponse> m_listener;

    std::vector<DeferredObject> m_deferredObjects;
    size_t m_currentObjectIndex = 0;
    bool   m_isCreatingObjects  = false;

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
        inputBG->setContentSize({360, 100});
        inputBG->setColor({0, 0, 0});
        inputBG->setOpacity(100);
        inputBG->setPosition({winSize.width / 2, winSize.height / 2 + 15});
        m_mainLayer->addChild(inputBG);

        m_promptInput = TextInput::create(350, "e.g. Medium difficulty platforming", "bigFont.fnt");
        m_promptInput->setPosition({winSize.width / 2, winSize.height / 2 + 15});
        m_promptInput->setScale(0.65f);
        m_promptInput->setMaxCharCount(200);
        m_promptInput->getInputNode()->setAllowedChars(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789-_=+/\\.,;:!@#$%^&*()[]{}|<>?`~'\" "
        );
        m_mainLayer->addChild(m_promptInput);

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
        m_statusLabel->setScale(0.4f);
        m_statusLabel->setPosition({winSize.width / 2, winSize.height / 2 - 60});
        m_statusLabel->setVisible(false);
        m_mainLayer->addChild(m_statusLabel);

        m_generateBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Generate", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(AIGeneratorPopup::onGenerate)
        );
        auto btnMenu = CCMenu::create();
        btnMenu->setPosition({winSize.width / 2, winSize.height / 2 - 95});
        m_generateBtn->setPosition({0, 0});
        btnMenu->addChild(m_generateBtn);
        m_mainLayer->addChild(btnMenu);

        // Info button only (lock icon removed — keys are now in settings)
        auto infoBtn = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png"); s->setScale(0.7f); return s; }(),
            this, menu_selector(AIGeneratorPopup::onInfo)
        );

        auto cornerMenu = CCMenu::create();
        cornerMenu->setPosition({winSize.width - 25, winSize.height - 25});
        infoBtn->setPosition({0, 0});
        cornerMenu->addChild(infoBtn);
        m_mainLayer->addChild(cornerMenu);

        this->schedule(schedule_selector(AIGeneratorPopup::updateObjectCreation), 0.05f);

        return true;
    }

    // ── UI callbacks ──────────────────────────────────────────────────────────

    void onToggleClear(CCObject*) {
        m_shouldClearLevel = !m_shouldClearLevel;
        log::info("Clear level toggle: {}", m_shouldClearLevel ? "ON" : "OFF");
    }

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
            if (m_editorLayer && m_editorLayer->m_editorUI)
                m_editorLayer->m_editorUI->updateButtons();

            showStatus(fmt::format("Created {} objects!", m_deferredObjects.size()), false);
            Notification::create(
                fmt::format("Generated {} objects!", m_deferredObjects.size()),
                NotificationIcon::Success
            )->show();

            this->runAction(CCSequence::create(
                CCDelayTime::create(2.0f),
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
            // AI can assign up to 10 group IDs per object using:
            //   "groups": [1, 5, 12]
            // Groups enable triggers to target specific objects.
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

            // ── Color Trigger properties (advanced features, ID 899) ───────────
            // AI specifies color triggers as:
            // {
            //   "type": "color_trigger", "x": ..., "y": 0,
            //   "color_channel": 1,          -- int 1-999, which channel to change
            //   "color": "#FF8800",           -- hex color to set it to
            //   "duration": 0.5,              -- transition time in seconds
            //   "blending": false,            -- (optional) additive blending
            //   "opacity": 1.0               -- (optional) 0.0 – 1.0
            // }
            if (advFeatures && gameObj->m_objectID == 899) {
                auto* effectObj = static_cast<EffectGameObject*>(gameObj);

                // Color channel (which GD color channel to modify, 1-999)
                auto channelResult = objData["color_channel"].asInt();
                if (channelResult) {
                    effectObj->m_targetColor = std::clamp((int)channelResult.unwrap(), 1, 999);
                }

                // Hex color "#RRGGBB"
                auto colorHexResult = objData["color"].asString();
                if (colorHexResult) {
                    GLubyte r = 255, g = 255, b = 255;
                    if (parseHexColor(colorHexResult.unwrap(), r, g, b)) {
                        effectObj->m_triggerTargetColor = {r, g, b};
                    }
                }

                // Duration (seconds, 0 = instant)
                auto durResult = objData["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                // Blending (additive color blend)
                auto blendResult = objData["blending"].asBool();
                if (blendResult) {
                    effectObj->m_usesBlending = blendResult.unwrap();
                }

                // Opacity (0.0 – 1.0)
                auto opacityResult = objData["opacity"].asDouble();
                if (opacityResult) {
                    effectObj->m_opacity = std::clamp(
                        static_cast<float>(opacityResult.unwrap()), 0.0f, 1.0f);
                }

                // Touch-triggered by default so it fires when the player reaches it
                effectObj->m_isTouchTriggered = true;
            }

            // ── Move Trigger properties (advanced features, ID 901) ────────────
            // AI specifies move triggers as:
            // {
            //   "type": "move_trigger", "x": ..., "y": 0,
            //   "target_group": 1,       -- int 1-9999, group of objects to move
            //   "move_x": 150,           -- horizontal offset in units (30 = 1 cell)
            //   "move_y": 0,             -- vertical offset in units
            //   "duration": 0.5,         -- transition time in seconds
            //   "easing": 0             -- (optional) 0=None,1=EaseInOut,2=EaseIn,3=EaseOut,
            //                           --   4=ElasticInOut,5=ElasticIn,6=ElasticOut,
            //                           --   7=BounceInOut,8=BounceIn,9=BounceOut
            // }
            if (advFeatures && gameObj->m_objectID == 901) {
                auto* effectObj = static_cast<EffectGameObject*>(gameObj);

                // Which group to move
                auto targetGroupResult = objData["target_group"].asInt();
                if (targetGroupResult) {
                    effectObj->m_targetGroupID = std::clamp((int)targetGroupResult.unwrap(), 1, 9999);
                }

                // X and Y movement offset (in GD units; 30 units = 1 grid cell)
                auto moveXResult = objData["move_x"].asDouble();
                auto moveYResult = objData["move_y"].asDouble();
                float offsetX = moveXResult ? static_cast<float>(moveXResult.unwrap()) : 0.0f;
                float offsetY = moveYResult ? static_cast<float>(moveYResult.unwrap()) : 0.0f;
                // Clamp to a sane range (±32,767 units)
                offsetX = std::clamp(offsetX, -32767.0f, 32767.0f);
                offsetY = std::clamp(offsetY, -32767.0f, 32767.0f);
                effectObj->m_moveOffset = CCPoint(offsetX, offsetY);

                // Duration
                auto durResult = objData["duration"].asDouble();
                if (durResult) {
                    effectObj->m_duration = std::clamp(
                        static_cast<float>(durResult.unwrap()), 0.0f, 30.0f);
                }

                // Easing type (integer 0–18, cast directly as the game does)
                auto easingResult = objData["easing"].asInt();
                if (easingResult) {
                    int easingVal = std::clamp((int)easingResult.unwrap(), 0, 18);
                    effectObj->m_easingType = (EasingType)easingVal;
                } else {
                    effectObj->m_easingType = EasingType::None;
                }

                // Touch-triggered so it fires when the player passes
                effectObj->m_isTouchTriggered = true;
            }

        } catch (...) {
            log::warn("Failed to apply object properties");
        }
    }

    void prepareObjects(matjson::Value& objectsArray) {
        if (!m_editorLayer || !objectsArray.isArray()) return;

        m_deferredObjects.clear();
        m_currentObjectIndex = 0;

        int    maxObjects  = (int)Mod::get()->getSettingValue<int64_t>("max-objects");
        size_t objectCount = std::min(objectsArray.size(), static_cast<size_t>(maxObjects));
        log::info("Preparing {} objects for progressive creation...", objectCount);

        for (size_t i = 0; i < objectCount; ++i) {
            try {
                auto objData = objectsArray[i];

                // Resolve type name → numeric object ID
                // "color_trigger" maps to ID 899 (GD color trigger)
                // All other types look up OBJECT_IDS map
                auto typeResult = objData["type"].asString();
                if (typeResult) {
                    const std::string& typeName = typeResult.unwrap();
                    if (typeName == "color_trigger") {
                        objectsArray[i]["id"] = 899;
                    } else if (typeName == "move_trigger") {
                        objectsArray[i]["id"] = 901;
                    } else {
                        auto it = OBJECT_IDS.find(typeName);
                        objectsArray[i]["id"] = (it != OBJECT_IDS.end()) ? it->second : 1;
                    }
                }

                auto idResult = objData["id"].asInt();
                auto xResult  = objData["x"].asDouble();
                auto yResult  = objData["y"].asDouble();

                if (!idResult || !xResult || !yResult) continue;

                int   objectID = idResult.unwrap();
                float x        = static_cast<float>(xResult.unwrap());
                float y        = static_cast<float>(yResult.unwrap());

                // Clamp Y so objects are never placed underground
                if (y < 0.0f) y = 0.0f;

                if (objectID < 1 || objectID > 10000) {
                    log::warn("Invalid object ID {} at index {} — skipping", objectID, i);
                    continue;
                }

                m_deferredObjects.push_back({objectID, CCPoint{x, y}, objData});
            } catch (...) {
                log::warn("Failed to prepare object at index {}", i);
            }
        }

        log::info("Prepared {} valid objects", m_deferredObjects.size());
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
            "  \"analysis\": \"Brief reasoning\",\n"
            "  \"objects\": [\n"
            "    {{\"type\": \"block_black_gradient_square\", \"x\": 0, \"y\": 30}},\n"
            "    {{\"type\": \"spike_black_gradient_spike\", \"x\": 150, \"y\": 0}}\n"
            "  ]\n"
            "}}\n\n"
            "Coordinates: X=horizontal (30 units=1 grid cell), Y=vertical (0=ground, 30=1 block above ground).\n"
            "Y must be >= 0. Never place objects below Y=0.\n"
            "Spacing: EASY=150-200, MEDIUM=90-150, HARD=60-90, EXTREME=30-60 X units between obstacles.\n"
            "Length: SHORT=500-1000, MEDIUM=1000-2000, LONG=2000-4000, XL=4000-8000, XXL=8000+ X units.",
            objectList
        );

        if (advFeatures) {
            base +=
                "\n\n"
                "ADVANCED FEATURES (enabled):\n\n"

                "JSON FORMAT RULES — follow exactly or the response will fail to parse:\n"
                "  • Return ONLY the JSON object — no markdown, no comments, no trailing commas\n"
                "  • All string values must use double quotes\n"
                "  • Numbers must not be quoted: \"x\": 150 not \"x\": \"150\"\n"
                "  • Arrays use square brackets: \"groups\": [1, 5]\n"
                "  • Do not include fields with null values — omit them entirely\n\n"

                "1. GROUP IDs — Assign up to 10 group IDs per object with the optional \"groups\" array.\n"
                "   Objects must be in a group for a move/toggle trigger to target them.\n"
                "   Example: {\"type\": \"platform\", \"x\": 100, \"y\": 30, \"groups\": [1]}\n\n"

                "2. COLOR TRIGGERS (type \"color_trigger\", ID 899)\n"
                "   Place at x positions where the color should change; set y to 0 (ground-level).\n"
                "   They fire automatically when the player reaches them.\n"
                "   Required fields:\n"
                "     \"color_channel\": integer 1–999 — which GD channel to change\n"
                "       (1=Background, 2=Ground1, 3=Line, 4=Object, 1000=Player1, 1001=Player2)\n"
                "     \"color\": \"#RRGGBB\" — target hex color (6 hex digits after #)\n"
                "   Optional fields:\n"
                "     \"duration\": float seconds (default 0.5)\n"
                "     \"blending\": true/false — additive blend (default false)\n"
                "     \"opacity\": 0.0–1.0 (default 1.0)\n"
                "   Example: {\"type\":\"color_trigger\",\"x\":500,\"y\":0,\"color_channel\":1,\"color\":\"#FF4400\",\"duration\":1.0}\n\n"

                "3. MOVE TRIGGERS (type \"move_trigger\", ID 901)\n"
                "   Move a group of objects by an offset over time. Objects must already have a\n"
                "   matching group ID assigned via the \"groups\" field.\n"
                "   Place the trigger at y=0 so the player activates it on the ground.\n"
                "   Required fields:\n"
                "     \"target_group\": integer 1–9999 — group ID to move (must match object groups)\n"
                "     \"move_x\": float — horizontal distance in GD units (30 = 1 grid cell, negative = left)\n"
                "     \"move_y\": float — vertical distance in GD units (positive = up)\n"
                "   Optional fields:\n"
                "     \"duration\": float seconds (default 0.5)\n"
                "     \"easing\": integer 0–9\n"
                "       0=None, 1=EaseInOut, 2=EaseIn, 3=EaseOut,\n"
                "       4=ElasticInOut, 5=ElasticIn, 6=ElasticOut,\n"
                "       7=BounceInOut, 8=BounceIn, 9=BounceOut\n"
                "   IMPORTANT: The trigger and the objects it moves are SEPARATE. Place the trigger\n"
                "   where the player will reach it, and place the objects being moved wherever they\n"
                "   should start (they will shift by move_x/move_y when the trigger fires).\n"
                "   Example:\n"
                "     Objects to move:    {\"type\":\"block_black_gradient_square\",\"x\":800,\"y\":90,\"groups\":[2]}\n"
                "     Trigger to fire it: {\"type\":\"move_trigger\",\"x\":500,\"y\":0,\"target_group\":2,\"move_x\":0,\"move_y\":90,\"duration\":0.5,\"easing\":1}\n\n"

                "Use advanced features purposefully to enhance the level. Color triggers set the mood\n"
                "at natural section changes (drops, transitions). Move triggers add dynamic platforming\n"
                "elements like rising platforms or sliding walls. Keep group IDs consistent — if a\n"
                "move trigger targets group 2, the objects it should move must have 2 in their groups array.\n";
        }

        return base;
    }

    // ── API call ──────────────────────────────────────────────────────────────

    void callAPI(const std::string& prompt, const std::string& apiKey) {
        std::string provider   = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model      = getProviderModel(provider);
        std::string difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        std::string style      = Mod::get()->getSettingValue<std::string>("style");
        std::string length     = Mod::get()->getSettingValue<std::string>("length");

        log::info("Calling {} API with model: {}", provider, model);

        std::string systemPrompt = buildSystemPrompt();
        std::string levelData    = buildLevelDataJson();

        std::string fullPrompt = fmt::format(
            "Generate a Geometry Dash level:\n\n"
            "Request: {}\nDifficulty: {}\nStyle: {}\nLength: {}\n\n"
            "Current level data (you may build upon or extend these existing objects): {}\n\n"
            "Return JSON with analysis and objects array.",
            prompt, difficulty, style, length, levelData
        );

        matjson::Value requestBody;
        std::string    url;

        // ── Gemini ─────────────────────────────────────────────────────────────
        if (provider == "gemini") {
            auto textPart = matjson::Value::object();
            textPart["text"] = systemPrompt + "\n\n" + fullPrompt;

            std::vector<matjson::Value> parts = {textPart};
            auto message = matjson::Value::object();
            message["role"]  = "user";
            message["parts"] = parts;

            auto genConfig = matjson::Value::object();
            genConfig["temperature"]     = 0.7;
            genConfig["maxOutputTokens"] = 65536;

            requestBody                     = matjson::Value::object();
            requestBody["contents"]         = std::vector<matjson::Value>{message};
            requestBody["generationConfig"] = genConfig;

            url = fmt::format(
                "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent?key={}",
                model, apiKey
            );

        // ── Claude (Anthropic) ─────────────────────────────────────────────────
        // Required headers: x-api-key, anthropic-version: 2023-06-01
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
        // o-series models (o1-mini etc.) reject the "temperature" field.
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
            requestBody["max_tokens"]            = 16384;

            if (!isOSeriesModel(model)) {
                requestBody["temperature"] = 0.7;
            }

            url = "https://api.openai.com/v1/chat/completions";

        // ── Mistral AI (Ministral) ─────────────────────────────────────────────
        // OpenAI-compatible endpoint. Auth: Bearer token.
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
        // Uses the OpenAI-compatible /v1/chat/completions endpoint.
        // Auth: Bearer token. Model is specified in the request body.
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

            url = "https://api-inference.huggingface.co/v1/chat/completions";

        // ── Ollama ─────────────────────────────────────────────────────────────
        } else if (provider == "ollama") {
            std::string ollamaUrl   = getOllamaUrl();
            std::string ollamaModel = model; // already fetched via getProviderModel

            auto options = matjson::Value::object();
            options["temperature"] = 0.7;

            requestBody            = matjson::Value::object();
            requestBody["model"]   = ollamaModel;
            requestBody["prompt"]  = systemPrompt + "\n\n" + fullPrompt;
            requestBody["stream"]  = false;
            requestBody["format"]  = "json";
            requestBody["options"] = options;

            url = ollamaUrl + "/api/generate";
            log::info("Using Ollama at: {}", url);
        }

        std::string jsonBody = requestBody.dump();
        log::info("Sending request to {} ({} bytes)", provider, jsonBody.length());

        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");

        if (provider == "claude") {
            request.header("x-api-key", apiKey);
            request.header("anthropic-version", "2023-06-01");
        } else if (provider == "openai" || provider == "ministral" || provider == "huggingface") {
            request.header("Authorization", fmt::format("Bearer {}", apiKey));
        } else if (provider == "ollama") {
            request.timeout(std::chrono::seconds(120));
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
        m_generateBtn->setEnabled(false);

        m_loadingCircle = LoadingCircle::create();
        m_loadingCircle->setParentLayer(m_mainLayer);
        m_loadingCircle->show();
        m_loadingCircle->setPosition(m_mainLayer->getContentSize() / 2);

        showStatus("AI is thinking...");
        log::info("=== Generation Request === Prompt: {}", prompt);

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
        if (m_loadingCircle) {
            m_loadingCircle->fadeAndRemove();
            m_loadingCircle = nullptr;
        }
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
            auto jsonRes = response.json();
            if (!jsonRes) {
                onError("Invalid Response", "The API returned invalid data.");
                return;
            }

            auto json = jsonRes.unwrap();
            std::string aiResponse;

            if (provider == "gemini") {
                auto candidates = json["candidates"];
                if (!candidates.isArray() || candidates.size() == 0) {
                    onError("No Response", "The AI didn't generate any content."); return;
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

            } else if (provider == "ollama") {
                auto responseResult = json["response"].asString();
                if (!responseResult) { onError("Invalid Response", "Failed to extract Ollama response."); return; }
                aiResponse = responseResult.unwrap();

                auto doneResult = json["done"].asBool();
                if (doneResult && !doneResult.unwrap())
                    log::warn("Ollama response marked as incomplete");
            }

            // Strip markdown code fences if present
            size_t codeBlockStart = aiResponse.find("```");
            while (codeBlockStart != std::string::npos) {
                size_t codeBlockEnd = aiResponse.find("```", codeBlockStart + 3);
                if (codeBlockEnd != std::string::npos) {
                    aiResponse.erase(codeBlockStart, codeBlockEnd - codeBlockStart + 3);
                    codeBlockStart = aiResponse.find("```");
                } else break;
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

            if (m_shouldClearLevel) clearLevel();
            prepareObjects(objectsArray);

        } catch (std::exception& e) {
            log::error("Exception during response processing: {}", e.what());
            onError("Processing Error", "Failed to process AI response.");
        }
    }

    void onError(const std::string& title, const std::string& message) {
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

// ─── EditorUI hook — adds the AI button ──────────────────────────────────────

class $modify(AIEditorUI, EditorUI) {
    struct Fields {
        CCMenuItemSpriteExtra* m_aiButton    = nullptr;
        CCMenu*                m_aiMenu      = nullptr;
        bool                   m_buttonAdded = false;
    };

    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;

        this->runAction(CCSequence::create(
            CCDelayTime::create(0.1f),
            CCCallFunc::create(this, callfunc_selector(AIEditorUI::addAIButton)),
            nullptr
        ));
        return true;
    }

    void addAIButton() {
        if (m_fields->m_buttonAdded) return;
        m_fields->m_buttonAdded = true;

        m_fields->m_aiButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("AI", "goldFont.fnt", "GJ_button_04.png", 0.8f),
            this, menu_selector(AIEditorUI::onAIButton)
        );

        m_fields->m_aiMenu = CCMenu::create();
        m_fields->m_aiMenu->addChild(m_fields->m_aiButton);

        auto winSize = CCDirector::get()->getWinSize();
        m_fields->m_aiButton->setPosition({0, 0});
        m_fields->m_aiMenu->setPosition({winSize.width - 70, winSize.height - 30});
        m_fields->m_aiMenu->setZOrder(100);
        m_fields->m_aiMenu->setID("ai-generator-menu"_spr);

        this->addChild(m_fields->m_aiMenu);
        log::info("AI button added to editor");
    }

    void onAIButton(CCObject*) {
        if (!this->m_editorLayer) {
            FLAlertLayer::create("Error", gd::string("No editor layer found!"), "OK")->show();
            return;
        }
        AIGeneratorPopup::create(this->m_editorLayer)->show();
    }
};

// ─── EditorPauseLayer hook — restore button on resume ────────────────────────

class $modify(EditorPauseLayer) {
    void onResume(CCObject* sender) {
        EditorPauseLayer::onResume(sender);
        if (auto editorUI = m_editorLayer->m_editorUI) {
            if (auto menu = typeinfo_cast<CCMenu*>(editorUI->getChildByID("ai-generator-menu"_spr)))
                menu->setVisible(true);
        }
    }
};

// ─── LevelEditorLayer hooks — hide during playtest, show on exit ──────────────

class $modify(AILevelEditorLayer, LevelEditorLayer) {
    void onPlaytest() {
        LevelEditorLayer::onPlaytest();
        if (auto editorUI = this->m_editorUI) {
            if (auto menu = typeinfo_cast<CCMenu*>(editorUI->getChildByID("ai-generator-menu"_spr)))
                menu->setVisible(false);
        }
    }

    void onStopPlaytest() {
        LevelEditorLayer::onStopPlaytest();
        if (auto editorUI = this->m_editorUI) {
            if (auto menu = typeinfo_cast<CCMenu*>(editorUI->getChildByID("ai-generator-menu"_spr)))
                menu->setVisible(true);
        }
    }
};

// ─── Mod startup ─────────────────────────────────────────────────────────────

$execute {
    log::info("========================================");
    log::info("         Editor AI v2.1.5");
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
