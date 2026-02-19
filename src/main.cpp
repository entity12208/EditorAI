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

static std::string maskApiKey(const std::string& key) {
    if (key.length() <= 8) return "***";
    return key.substr(0, 4) + "..." + key.substr(key.length() - 4);
}

static std::pair<std::string, std::string> parseAPIError(const std::string& errorBody, int statusCode) {
    std::string title = "API Error";
    std::string message = "An unknown error occurred. Please try again.";
    try {
        auto json = matjson::parse(errorBody);
        if (!json) return {title, message};
        auto error = json.unwrap();
        std::string errorMsg;
        if (error.contains("error")) {
            auto errorObj = error["error"];
            if (errorObj.contains("message")) {
                auto msgResult = errorObj["message"].asString();
                if (msgResult) errorMsg = msgResult.unwrap();
            }
        }
        if (statusCode == 401 || statusCode == 403) {
            title = "Invalid API Key";
            message = "Your API key is invalid or expired.\n\nPlease check your API key and try again.";
        } else if (statusCode == 429) {
            title = "Rate Limit Exceeded";
            if (errorMsg.find("quota") != std::string::npos || errorMsg.find("Quota") != std::string::npos) {
                message = "You've exceeded your API quota.\n\nPlease wait or upgrade your plan.";
            } else {
                message = "Too many requests.\n\nPlease wait a moment and try again.";
            }
        } else if (statusCode == 400) {
            title = "Invalid Request";
            message = errorMsg.find("model") != std::string::npos
                ? "The selected model is invalid.\n\nPlease check your model settings."
                : "The request was invalid.\n\nPlease check your settings and try again.";
        } else if (statusCode >= 500) {
            title = "Service Error";
            message = "The AI service is currently unavailable.\n\nPlease try again later.";
        } else if (!errorMsg.empty()) {
            message = errorMsg.substr(0, 200);
            if (errorMsg.length() > 200) message += "...";
        }
    } catch (...) {}
    return {title, message};
}

// ─── API Key Popup ────────────────────────────────────────────────────────────

class APIKeyPopup : public Popup {
protected:
    TextInput* m_keyInput;
    std::function<void(std::string)> m_callback;

    bool init(std::function<void(std::string)> callback) {
        if (!Popup::init(400.f, 200.f))
            return false;

        m_callback = std::move(callback);
        auto winSize = this->m_size;
        this->setTitle("Enter API Key");

        auto descLabel = CCLabelBMFont::create("Paste your API key below:", "bigFont.fnt");
        descLabel->setScale(0.4f);
        descLabel->setPosition({winSize.width / 2, winSize.height / 2 + 50});
        m_mainLayer->addChild(descLabel);

        auto inputBG = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        inputBG->setContentSize({360, 40});
        inputBG->setColor({0, 0, 0});
        inputBG->setOpacity(100);
        inputBG->setPosition({winSize.width / 2, winSize.height / 2});
        m_mainLayer->addChild(inputBG);

        m_keyInput = TextInput::create(350, "Paste key here...", "bigFont.fnt");
        m_keyInput->setPosition({winSize.width / 2, winSize.height / 2});
        m_keyInput->setScale(0.6f);
        m_keyInput->setMaxCharCount(500);
        m_keyInput->setPasswordMode(true);

        // Fix: set allowed chars directly on the underlying CCTextInputNode so that
        // _, -, and all other valid API key characters can actually be typed.
        // This replaces the old global CCTextInputNode hook from filter.hpp.
        m_keyInput->getInputNode()->setAllowedChars(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789-_=+/\\.,;:!@#$%^&*()[]{}|<>?`~ \"\'"
        );

        m_mainLayer->addChild(m_keyInput);

        auto saveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Save", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(APIKeyPopup::onSave)
        );
        auto cancelBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Cancel", "bigFont.fnt", "GJ_button_06.png", 0.8f),
            this, menu_selector(APIKeyPopup::onClose)
        );

        auto btnMenu = CCMenu::create();
        btnMenu->setPosition({winSize.width / 2, winSize.height / 2 - 65});
        saveBtn->setPosition({-50, 0});
        cancelBtn->setPosition({50, 0});
        btnMenu->addChild(saveBtn);
        btnMenu->addChild(cancelBtn);
        m_mainLayer->addChild(btnMenu);

        return true;
    }

    void onSave(CCObject*) {
        std::string key = m_keyInput->getString();
        if (!key.empty() && key != "Paste key here...") {
            m_callback(key);
            this->onClose(nullptr);
        } else {
            FLAlertLayer::create("Error", gd::string("Please enter a valid API key"), "OK")->show();
        }
    }

public:
    static APIKeyPopup* create(std::function<void(std::string)> callback) {
        auto ret = new APIKeyPopup();
        if (ret->init(std::move(callback))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ─── Deferred object struct ───────────────────────────────────────────────────

struct DeferredObject {
    int objectID;
    CCPoint position;
    matjson::Value data;
};

// ─── Main generation popup ────────────────────────────────────────────────────

class AIGeneratorPopup : public Popup {
protected:
    TextInput*               m_promptInput     = nullptr;
    CCLabelBMFont*           m_statusLabel     = nullptr;
    LoadingCircle*           m_loadingCircle   = nullptr;
    CCMenuItemSpriteExtra*   m_generateBtn     = nullptr;
    CCMenuItemToggler*       m_clearToggle     = nullptr;
    bool                     m_shouldClearLevel = true;
    LevelEditorLayer*        m_editorLayer     = nullptr;

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

        // Description label
        auto descLabel = CCLabelBMFont::create("Describe the level you want to generate:", "bigFont.fnt");
        descLabel->setScale(0.45f);
        descLabel->setPosition({winSize.width / 2, winSize.height / 2 + 70});
        m_mainLayer->addChild(descLabel);

        // Prompt input background
        auto inputBG = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        inputBG->setContentSize({360, 100});
        inputBG->setColor({0, 0, 0});
        inputBG->setOpacity(100);
        inputBG->setPosition({winSize.width / 2, winSize.height / 2 + 15});
        m_mainLayer->addChild(inputBG);

        // Prompt text input
        m_promptInput = TextInput::create(350, "e.g. Medium difficulty platforming", "bigFont.fnt");
        m_promptInput->setPosition({winSize.width / 2, winSize.height / 2 + 15});
        m_promptInput->setScale(0.65f);
        m_promptInput->setMaxCharCount(200);

        // Allow all printable characters in the prompt box (same fix as API key input)
        m_promptInput->getInputNode()->setAllowedChars(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789-_=+/\\.,;:!@#$%^&*()[]{}|<>?`~'\" "
        );

        m_mainLayer->addChild(m_promptInput);

        // Clear-level toggle
        auto clearLabel = CCLabelBMFont::create("Clear level before generating", "bigFont.fnt");
        clearLabel->setScale(0.4f);

        auto onSpr  = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        onSpr->setScale(0.7f);
        offSpr->setScale(0.7f);

        m_clearToggle = CCMenuItemToggler::create(offSpr, onSpr, this,
            menu_selector(AIGeneratorPopup::onToggleClear));
        m_clearToggle->toggle(true);

        auto toggleMenu = CCMenu::create();
        toggleMenu->setPosition({winSize.width / 2, winSize.height / 2 - 35});
        m_clearToggle->setPosition({-80, 0});
        clearLabel->setPosition({20, 0});
        toggleMenu->addChild(m_clearToggle);
        toggleMenu->addChild(clearLabel);
        m_mainLayer->addChild(toggleMenu);

        // Status label
        m_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_statusLabel->setScale(0.4f);
        m_statusLabel->setPosition({winSize.width / 2, winSize.height / 2 - 60});
        m_statusLabel->setVisible(false);
        m_mainLayer->addChild(m_statusLabel);

        // Generate button
        m_generateBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Generate", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this, menu_selector(AIGeneratorPopup::onGenerate)
        );
        auto btnMenu = CCMenu::create();
        btnMenu->setPosition({winSize.width / 2, winSize.height / 2 - 95});
        m_generateBtn->setPosition({0, 0});
        btnMenu->addChild(m_generateBtn);
        m_mainLayer->addChild(btnMenu);

        // Info + key corner buttons
        auto infoBtn = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png"); s->setScale(0.7f); return s; }(),
            this, menu_selector(AIGeneratorPopup::onInfo)
        );
        auto keyBtn = CCMenuItemSpriteExtra::create(
            []{ auto s = CCSprite::createWithSpriteFrameName("GJ_lock_001.png"); s->setScale(0.7f); return s; }(),
            this, menu_selector(AIGeneratorPopup::onKeyButton)
        );

        auto cornerMenu = CCMenu::create();
        cornerMenu->setPosition({winSize.width - 25, winSize.height - 25});
        infoBtn->setPosition({0, 0});
        keyBtn->setPosition({-35, 0});
        cornerMenu->addChild(infoBtn);
        cornerMenu->addChild(keyBtn);
        m_mainLayer->addChild(cornerMenu);

        // Tick-based object spawner (every 0.05 s)
        this->schedule(schedule_selector(AIGeneratorPopup::updateObjectCreation), 0.05f);

        return true;
    }

    // ── UI callbacks ──────────────────────────────────────────────────────────

    void onToggleClear(CCObject*) {
        m_shouldClearLevel = !m_shouldClearLevel;
    }

    void onInfo(CCObject*) {
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");

        // Show the effective model name for both Ollama and cloud providers
        std::string model = (provider == "ollama")
            ? Mod::get()->getSettingValue<std::string>("ollama-model")
            : Mod::get()->getSettingValue<std::string>("model");

        // Key status (Ollama doesn't need one)
        std::string apiKey    = Mod::get()->getSavedValue<std::string>("api-key", "");
        std::string keyStatus = (provider == "ollama")
            ? "<cg>Not required</c>"
            : (apiKey.empty() ? "<cr>Not set — click the lock icon</c>" : "<cg>Set</c>");

        FLAlertLayer::create(
            "Editor AI",
            gd::string(fmt::format(
                "<cy>Provider:</c> {}\n"
                "<cy>Model:</c> {}\n"
                "<cy>API Key:</c> {}\n"
                "<cy>Objects in library:</c> {}",
                provider, model, keyStatus, OBJECT_IDS.size()
            )),
            "OK"
        )->show();
    }

    void onKeyButton(CCObject*) {
        std::string currentKey = Mod::get()->getSavedValue<std::string>("api-key", "");
        if (!currentKey.empty()) {
            geode::createQuickPopup(
                "API Key",
                fmt::format("Saved key: {}\n\nDelete it?", maskApiKey(currentKey)),
                "Cancel", "Delete",
                [](FLAlertLayer*, bool btn2) {
                    if (btn2) {
                        Mod::get()->setSavedValue<std::string>("api-key", std::string());
                        Notification::create("API Key Deleted", NotificationIcon::Success)->show();
                    }
                }
            );
        } else {
            APIKeyPopup::create([](std::string key) {
                Mod::get()->setSavedValue("api-key", key);
                Notification::create("API Key Saved!", NotificationIcon::Success)->show();
                log::info("API key saved");
            })->show();
        }
    }

    void showStatus(const std::string& msg, bool error = false) {
        m_statusLabel->setString(msg.c_str());
        m_statusLabel->setColor(error ? ccColor3B{255, 100, 100} : ccColor3B{100, 255, 100});
        m_statusLabel->setVisible(true);
    }

    // ── Level manipulation ────────────────────────────────────────────────────

    void clearLevel() {
        if (!m_editorLayer) return;
        auto objects  = m_editorLayer->m_objects;
        if (!objects)  return;

        auto toRemove = CCArray::create();
        for (auto* obj : CCArrayExt<CCObject*>(objects))
            toRemove->addObject(obj);

        for (auto* gameObj : CCArrayExt<GameObject*>(toRemove))
            m_editorLayer->removeObject(gameObj, true);

        log::info("Cleared {} objects from editor", toRemove->count());
    }

    // ── Progressive object spawner ────────────────────────────────────────────

    void updateObjectCreation(float /*dt*/) {
        if (!m_isCreatingObjects || m_deferredObjects.empty()) return;

        if (m_currentObjectIndex >= m_deferredObjects.size()) {
            // All done
            m_isCreatingObjects = false;
            if (m_editorLayer && m_editorLayer->m_editorUI)
                m_editorLayer->m_editorUI->updateButtons();

            showStatus(fmt::format("Created {} objects!", m_deferredObjects.size()), false);
            Notification::create(
                fmt::format("Generated {} objects!", m_deferredObjects.size()),
                NotificationIcon::Success
            )->show();

            // Auto-close after 2 seconds
            this->runAction(CCSequence::create(
                CCDelayTime::create(2.0f),
                CCCallFunc::create(this, callfunc_selector(AIGeneratorPopup::closePopup)),
                nullptr
            ));

            m_deferredObjects.clear();
            m_currentObjectIndex = 0;
            return;
        }

        // Spawn up to batchSize objects this tick
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

        // Update progress label every 10 objects
        if (m_currentObjectIndex % 10 == 0) {
            float pct = (float)m_currentObjectIndex / (float)m_deferredObjects.size() * 100.0f;
            showStatus(fmt::format("Creating objects... {:.0f}%", pct), false);
        }
    }

    void applyObjectProperties(GameObject* gameObj, matjson::Value& objData) {
        if (!gameObj) return;
        try {
            auto rotResult = objData["rotation"].asDouble();
            if (rotResult) {
                float r = static_cast<float>(rotResult.unwrap());
                if (r >= -360.0f && r <= 360.0f)
                    gameObj->setRotation(r);
            }
            auto scaleResult = objData["scale"].asDouble();
            if (scaleResult) {
                float s = static_cast<float>(scaleResult.unwrap());
                if (s >= 0.1f && s <= 10.0f)
                    gameObj->setScale(s);
            }
            auto flipXResult = objData["flip_x"].asBool();
            if (flipXResult && flipXResult.unwrap())
                gameObj->setScaleX(-gameObj->getScaleX());

            auto flipYResult = objData["flip_y"].asBool();
            if (flipYResult && flipYResult.unwrap())
                gameObj->setScaleY(-gameObj->getScaleY());
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

                auto idResult = objData["id"].asInt();
                auto xResult  = objData["x"].asDouble();
                auto yResult  = objData["y"].asDouble();

                if (!idResult || !xResult || !yResult) continue;

                int   objectID = idResult.unwrap();
                float x        = static_cast<float>(xResult.unwrap());
                float y        = static_cast<float>(yResult.unwrap());

                // Underground-block fix: clamp Y so objects are never placed below ground
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
        // Send the FULL object list — no arbitrary cap.
        // This is intentionally verbose; cloud models have sufficient context windows.
        std::string objectList;
        objectList.reserve(OBJECT_IDS.size() * 30);
        bool first = true;
        for (auto& [name, id] : OBJECT_IDS) {
            if (!first) objectList += ", ";
            objectList += name;
            first = false;
        }

        return fmt::format(
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
    }

    // ── API call ──────────────────────────────────────────────────────────────

    void callAPI(const std::string& prompt, const std::string& apiKey) {
        std::string provider   = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model      = Mod::get()->getSettingValue<std::string>("model");
        std::string difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        std::string style      = Mod::get()->getSettingValue<std::string>("style");
        std::string length     = Mod::get()->getSettingValue<std::string>("length");

        log::info("Calling {} API with model: {}", provider, model);

        std::string systemPrompt = buildSystemPrompt();
        std::string fullPrompt   = fmt::format(
            "Generate a Geometry Dash level:\n\n"
            "Request: {}\nDifficulty: {}\nStyle: {}\nLength: {}\n\n"
            "Return JSON with analysis and objects array.",
            prompt, difficulty, style, length
        );

        matjson::Value requestBody;
        std::string    url;

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

            requestBody                    = matjson::Value::object();
            requestBody["contents"]        = std::vector<matjson::Value>{message};
            requestBody["generationConfig"] = genConfig;

            url = fmt::format(
                "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent?key={}",
                model, apiKey
            );

        } else if (provider == "claude") {
            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                = matjson::Value::object();
            requestBody["model"]       = model;
            requestBody["max_tokens"]  = 4096;
            requestBody["temperature"] = 0.7;
            requestBody["system"]      = systemPrompt;
            requestBody["messages"]    = std::vector<matjson::Value>{userMsg};

            url = "https://api.anthropic.com/v1/messages";

        } else if (provider == "openai") {
            auto sysMsg  = matjson::Value::object();
            sysMsg["role"]    = "system";
            sysMsg["content"] = systemPrompt;

            auto userMsg = matjson::Value::object();
            userMsg["role"]    = "user";
            userMsg["content"] = fullPrompt;

            requestBody                = matjson::Value::object();
            requestBody["model"]       = model;
            requestBody["messages"]    = std::vector<matjson::Value>{sysMsg, userMsg};
            requestBody["temperature"] = 0.7;
            requestBody["max_tokens"]  = 4096;

            url = "https://api.openai.com/v1/chat/completions";

        } else if (provider == "ollama") {
            std::string ollamaUrl   = Mod::get()->getSettingValue<std::string>("ollama-url");
            std::string ollamaModel = Mod::get()->getSettingValue<std::string>("ollama-model");

            auto options = matjson::Value::object();
            options["temperature"] = 0.7;

            requestBody           = matjson::Value::object();
            requestBody["model"]  = ollamaModel;
            requestBody["prompt"] = systemPrompt + "\n\n" + fullPrompt;
            requestBody["stream"] = false;
            requestBody["format"] = "json";
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
        } else if (provider == "openai") {
            request.header("Authorization", fmt::format("Bearer {}", apiKey));
        } else if (provider == "ollama") {
            // Fail fast if the local Ollama server is unreachable
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

    void onGenerate(CCObject*) {
        std::string prompt = m_promptInput->getString();
        if (prompt.empty() || prompt == "e.g. Medium difficulty platforming") {
            FLAlertLayer::create("Empty Prompt", gd::string("Please enter a description!"), "OK")->show();
            return;
        }

        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string apiKey   = Mod::get()->getSavedValue<std::string>("api-key", "");

        if (apiKey.empty() && provider != "ollama") {
            FLAlertLayer::create("API Key Required",
                gd::string("Please click the lock icon to enter your API key!"), "OK")->show();
            return;
        }

        m_generateBtn->setEnabled(false);

        // Loading circle fix: add it as a direct child of m_mainLayer, then call show(),
        // then reposition to the popup centre so it doesn't appear at a screen corner.
        m_loadingCircle = LoadingCircle::create();
        m_loadingCircle->setParentLayer(m_mainLayer);
        m_loadingCircle->show();
        m_loadingCircle->setPosition(m_mainLayer->getContentSize() / 2);

        showStatus("AI is thinking...");
        log::info("=== Generation Request === Prompt: {}", prompt);

        callAPI(prompt, apiKey);
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

            } else if (provider == "openai") {
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

            // Resolve "type" name → numeric GD object ID
            for (size_t i = 0; i < objectsArray.size(); ++i) {
                auto obj = objectsArray[i];
                auto typeResult = obj["type"].asString();
                if (typeResult) {
                    auto& typeName = typeResult.unwrap();
                    auto it = OBJECT_IDS.find(typeName);
                    objectsArray[i]["id"] = (it != OBJECT_IDS.end()) ? it->second : 1;
                }
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
        CCMenuItemSpriteExtra* m_aiButton  = nullptr;
        CCMenu*                m_aiMenu    = nullptr;
        bool                   m_buttonAdded = false;
    };

    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;

        // Delay by one frame so other mods finish their init first
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

        // Moved left relative to original (was -45) to avoid edge clipping
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

    // Fix: restore the AI button when the player exits playtest mode.
    // The original code hid it on onPlaytest but never showed it again on exit.
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
    log::info("========================================");

    Loader::get()->queueInMainThread([] {
        updateObjectIDsFromGitHub();
    });
}