#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/ui/TextInput.hpp>
#include "filter.hpp"

using namespace geode::prelude;

// Parse object IDs from JSON string
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
            if (intVal) {
                ids[key] = intVal.unwrap();
            }
        }
        
        log::info("Loaded {} object IDs from {}", ids.size(), source);
    } catch (std::exception& e) {
        log::error("Error parsing {} object_ids.json: {}", source, e.what());
    }
    
    return ids;
}

// Load object IDs with priority: Local file > Defaults
// GitHub updates happen asynchronously after startup
static std::unordered_map<std::string, int> loadObjectIDs() {
    std::unordered_map<std::string, int> ids;
    
    // Try local file first (synchronous, fast)
    auto path = Mod::get()->getResourcesDir() / "object_ids.json";
    
    if (std::filesystem::exists(path)) {
        try {
            log::info("Loading object_ids.json from local file...");
            std::ifstream file(path);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ids = parseObjectIDs(content, "local file");
            if (ids.size() > 0) {
                return ids;
            }
        } catch (std::exception& e) {
            log::error("Error reading local object_ids.json: {}", e.what());
        }
    } else {
        log::warn("Local object_ids.json not found");
    }
    
    // Fallback to defaults
    log::warn("Using default object IDs (5 objects only)");
    ids["block_black_gradient"] = 1;
    ids["spike"] = 8;
    ids["platform"] = 1731;
    ids["orb_yellow"] = 36;
    ids["pad_yellow"] = 35;
    
    return ids;
}

static std::unordered_map<std::string, int> OBJECT_IDS = loadObjectIDs();

// Async GitHub update function (called after startup)
static void updateObjectIDsFromGitHub() {
    log::info("Scheduling GitHub object_ids.json update...");
    
    auto request = web::WebRequest();
    auto task = request.get("https://raw.githubusercontent.com/entity12208/EditorAI/refs/heads/main/resources/object_ids.json");
    
    // This listener will be static and persist
    static EventListener<web::WebTask> listener;
    listener.bind([](web::WebTask::Event* e) {
        if (auto res = e->getValue()) {
            if (res->ok()) {
                auto content = res->string();
                if (content) {
                    auto newIds = parseObjectIDs(content.unwrap(), "GitHub");
                    if (newIds.size() > OBJECT_IDS.size()) {
                        OBJECT_IDS = newIds;
                        log::info("Successfully updated to {} object IDs from GitHub!", OBJECT_IDS.size());
                        Notification::create(
                            fmt::format("Object library updated! ({} objects)", OBJECT_IDS.size()),
                            NotificationIcon::Success
                        )->show();
                    } else {
                        log::info("GitHub version has same or fewer objects, keeping current");
                    }
                }
            } else {
                log::warn("GitHub fetch failed with HTTP {}", res->code());
            }
        }
    });
    listener.setFilter(task);
}

// Helper to mask API keys in logs
static std::string maskApiKey(const std::string& key) {
    if (key.length() <= 8) return "***";
    return key.substr(0, 4) + "..." + key.substr(key.length() - 4);
}

// Parse API error and return user-friendly message
static std::pair<std::string, std::string> parseAPIError(const std::string& errorBody, int statusCode) {
    std::string title = "API Error";
    std::string message = "An unknown error occurred. Please try again.";
    
    try {
        auto json = matjson::parse(errorBody);
        if (!json) return {title, message};
        
        auto error = json.unwrap();
        
        // Try to extract error message
        std::string errorMsg;
        if (error.contains("error")) {
            auto errorObj = error["error"];
            if (errorObj.contains("message")) {
                auto msgResult = errorObj["message"].asString();
                if (msgResult) errorMsg = msgResult.unwrap();
            }
        }
        
        // Handle specific error codes
        if (statusCode == 401 || statusCode == 403) {
            title = "Invalid API Key";
            message = "Your API key is invalid or expired.\n\nPlease check your API key and try again.";
        } else if (statusCode == 429) {
            title = "Rate Limit Exceeded";
            if (errorMsg.find("quota") != std::string::npos || errorMsg.find("Quota") != std::string::npos) {
                message = "You've exceeded your API quota.\n\n"
                         "Please wait a few minutes or upgrade your plan.";
            } else {
                message = "Too many requests.\n\nPlease wait a moment and try again.";
            }
        } else if (statusCode == 400) {
            title = "Invalid Request";
            if (errorMsg.find("model") != std::string::npos) {
                message = "The selected model is invalid.\n\nPlease check your model settings.";
            } else {
                message = "The request was invalid.\n\nPlease check your settings and try again.";
            }
        } else if (statusCode >= 500) {
            title = "Service Error";
            message = "The AI service is currently unavailable.\n\nPlease try again later.";
        } else if (!errorMsg.empty()) {
            // Use extracted message for other errors
            message = errorMsg.substr(0, 200); // Truncate if too long
            if (errorMsg.length() > 200) message += "...";
        }
        
    } catch (...) {
        // If parsing fails, use generic message
    }
    
    return {title, message};
}

// API Key Entry Popup
class APIKeyPopup : public Popup<std::function<void(std::string)>> {
protected:
    TextInput* m_keyInput;
    std::function<void(std::string)> m_callback;
    
    bool setup(std::function<void(std::string)> callback) {
        m_callback = callback;
        
        auto winSize = this->m_size;
        this->setTitle("Enter API Key");
        
        auto descLabel = CCLabelBMFont::create(
            "Paste your API key below:",
            "bigFont.fnt"
        );
        descLabel->setScale(0.4f);
        descLabel->setPosition({winSize.width / 2, winSize.height / 2 + 50});
        m_mainLayer->addChild(descLabel);
        
        auto inputBG = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        inputBG->setContentSize({360, 40});
        inputBG->setColor({0, 0, 0});
        inputBG->setOpacity(100);
        inputBG->setPosition({winSize.width / 2, winSize.height / 2});
        m_mainLayer->addChild(inputBG);
        
        m_keyInput = TextInput::create(350, "sk-...", "bigFont.fnt");
        m_keyInput->setPosition({winSize.width / 2, winSize.height / 2});
        m_keyInput->setScale(0.6f);
        m_keyInput->setMaxCharCount(500);
        m_keyInput->setPasswordMode(true);
        m_mainLayer->addChild(m_keyInput);
        
        auto saveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Save", "goldFont.fnt", "GJ_button_01.png", 0.8f),
            this,
            menu_selector(APIKeyPopup::onSave)
        );
        
        auto cancelBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Cancel", "bigFont.fnt", "GJ_button_06.png", 0.8f),
            this,
            menu_selector(APIKeyPopup::onClose)
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
        if (!key.empty() && key != "sk-...") {
            m_callback(key);
            this->onClose(nullptr);
        } else {
            FLAlertLayer::create("Error", gd::string("Please enter a valid API key"), "OK")->show();
        }
    }

public:
    static APIKeyPopup* create(std::function<void(std::string)> callback) {
        auto ret = new APIKeyPopup();
        if (ret->initAnchored(400.f, 200.f, callback)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Structure to hold object data for deferred creation
struct DeferredObject {
    int objectID;
    CCPoint position;
    matjson::Value data;
};

// AI Generator Popup
class AIGeneratorPopup : public Popup<LevelEditorLayer*> {
protected:
    TextInput* m_promptInput;
    CCLabelBMFont* m_statusLabel;
    LoadingCircle* m_loadingCircle;
    CCMenuItemSpriteExtra* m_generateBtn;
    CCMenuItemToggler* m_clearToggle;
    bool m_shouldClearLevel = true;
    LevelEditorLayer* m_editorLayer = nullptr;
    EventListener<web::WebTask> m_listener;
    
    // CRASH FIX: Deferred object creation
    std::vector<DeferredObject> m_deferredObjects;
    size_t m_currentObjectIndex = 0;
    bool m_isCreatingObjects = false;

    bool setup(LevelEditorLayer* editorLayer) {
        m_editorLayer = editorLayer;

        auto winSize = this->m_size;
        this->setTitle("Editor AI");

        auto descLabel = CCLabelBMFont::create(
            "Describe the level you want to generate:",
            "bigFont.fnt"
        );
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
        m_mainLayer->addChild(m_promptInput);

        auto clearLabel = CCLabelBMFont::create("Clear level before generating", "bigFont.fnt");
        clearLabel->setScale(0.4f);

        auto onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        onSpr->setScale(0.7f);
        offSpr->setScale(0.7f);

        m_clearToggle = CCMenuItemToggler::create(
            offSpr,
            onSpr,
            this,
            menu_selector(AIGeneratorPopup::onToggleClear)
        );
        m_clearToggle->toggle(true);

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

        // Bottom button menu with proper layout
        auto generateSprite = ButtonSprite::create(
            "Generate",
            "goldFont.fnt",
            "GJ_button_01.png",
            0.8f
        );
        m_generateBtn = CCMenuItemSpriteExtra::create(
            generateSprite,
            this,
            menu_selector(AIGeneratorPopup::onGenerate)
        );

        auto btnMenu = CCMenu::create();
        btnMenu->setPosition({winSize.width / 2, winSize.height / 2 - 95});
        m_generateBtn->setPosition({0, 0});
        btnMenu->addChild(m_generateBtn);
        m_mainLayer->addChild(btnMenu);

        // Top-right corner menu for info and key buttons
        auto infoSprite = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
        infoSprite->setScale(0.7f);
        auto infoBtn = CCMenuItemSpriteExtra::create(
            infoSprite,
            this,
            menu_selector(AIGeneratorPopup::onInfo)
        );

        auto keySprite = CCSprite::createWithSpriteFrameName("GJ_lock_001.png");
        keySprite->setScale(0.7f);
        auto keyBtn = CCMenuItemSpriteExtra::create(
            keySprite,
            this,
            menu_selector(AIGeneratorPopup::onKeyButton)
        );

        auto cornerMenu = CCMenu::create();
        cornerMenu->setPosition({winSize.width - 25, winSize.height - 25});
        infoBtn->setPosition({0, 0});
        keyBtn->setPosition({-35, 0});
        cornerMenu->addChild(infoBtn);
        cornerMenu->addChild(keyBtn);
        m_mainLayer->addChild(cornerMenu);

        // CRASH FIX: Schedule progressive object creation
        this->schedule(schedule_selector(AIGeneratorPopup::updateObjectCreation), 0.05f);

        return true;
    }

    void onToggleClear(CCObject*) {
        m_shouldClearLevel = !m_shouldClearLevel;
    }

    void onInfo(CCObject*) {
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model = Mod::get()->getSettingValue<std::string>("model");

        std::string objectSource = "defaults";
        if (OBJECT_IDS.size() > 10) {
            objectSource = "local file (auto-updates from GitHub)";
        } else if (OBJECT_IDS.size() > 5) {
            objectSource = "local file";
        }
        
        FLAlertLayer::create(
            "Editor AI",
            gd::string(fmt::format(
                "<cy>Direct API Integration</c>\n\n"
                "<cg>Provider:</c> <co>{}</co>\n"
                "<cg>Model:</c> <co>{}</co>\n\n"
                "<cy>Setup:</c>\n"
                "• For Cloud AI: Click lock icon for API key\n"
                "• For Ollama: Just select in settings!\n"
                "• Select provider & model in settings\n"
                "• Start generating!\n\n"
                "<co>Loaded {} object types from {}</co>\n\n"
                "<cl>v2.1.2 - Ollama Integration</cl>\n"
                "• Added full Ollama support (local AI)\n"
                "• Auto-updates object library from GitHub\n"
                "• Fixed crashes with editor-collab mod\n"
                "• Progressive object spawning",
                provider,
                model,
                OBJECT_IDS.size(),
                objectSource
            )),
            "OK"
        )->show();
    }

    void onKeyButton(CCObject*) {
        std::string currentKey = Mod::get()->getSavedValue<std::string>("api-key", "");

        if (!currentKey.empty()) {
            geode::createQuickPopup(
                "API Key",
                fmt::format("API key saved: {}\n\nDelete it?", maskApiKey(currentKey)),
                "Cancel", "Delete",
                [this](FLAlertLayer*, bool btn2) {
                    if (btn2) {
                        Mod::get()->setSavedValue<std::string>("api-key", std::string());
                        Notification::create("API Key Deleted", NotificationIcon::Success)->show();
                    }
                }
            );
        } else {
            showKeyPopup();
        }
    }

    void showKeyPopup() {
        APIKeyPopup::create([](std::string key) {
            Mod::get()->setSavedValue("api-key", key);
            Notification::create("API Key Saved!", NotificationIcon::Success)->show();
            log::info("API key saved successfully");
        })->show();
    }

    void showStatus(const std::string& msg, bool error = false) {
        m_statusLabel->setString(msg.c_str());
        m_statusLabel->setColor(error ? ccColor3B{255, 100, 100} : ccColor3B{100, 255, 100});
        m_statusLabel->setVisible(true);
    }

    void clearLevel() {
        if (!m_editorLayer) return;

        auto objects = m_editorLayer->m_objects;
        if (!objects) return;

        auto toRemove = CCArray::create();
        CCObject* obj;
        CCARRAY_FOREACH(objects, obj) {
            toRemove->addObject(obj);
        }

        CCARRAY_FOREACH(toRemove, obj) {
            auto gameObj = static_cast<GameObject*>(obj);
            if (gameObj) {
                m_editorLayer->removeObject(gameObj, true);
            }
        }

        log::info("Cleared {} objects from editor", toRemove->count());
    }

    // CRASH FIX: Progressive object creation - one object per frame
    void updateObjectCreation(float dt) {
        if (!m_isCreatingObjects || m_deferredObjects.empty()) return;
        
        // Check if all objects have been created
        if (m_currentObjectIndex >= m_deferredObjects.size()) {
            m_isCreatingObjects = false;
            
            // Update editor UI after all objects created
            if (m_editorLayer && m_editorLayer->m_editorUI) {
                m_editorLayer->m_editorUI->updateButtons();
            }
            
            showStatus(fmt::format("✓ Created {} objects!", m_deferredObjects.size()), false);
            
            Notification::create(
                fmt::format("Generated {} objects!", m_deferredObjects.size()),
                NotificationIcon::Success
            )->show();
            
            // Auto-close after success
            auto closeAction = CCSequence::create(
                CCDelayTime::create(2.0f),
                CCCallFunc::create(this, callfunc_selector(AIGeneratorPopup::closePopup)),
                nullptr
            );
            this->runAction(closeAction);
            
            m_deferredObjects.clear();
            m_currentObjectIndex = 0;
            return;
        }
        
        // CRITICAL: Create ONE object per frame to avoid overwhelming the editor
        try {
            auto& deferred = m_deferredObjects[m_currentObjectIndex];
            
            // SAFETY CHECK: Verify editor layer still exists
            if (!m_editorLayer) {
                log::error("Editor layer destroyed during object creation!");
                m_isCreatingObjects = false;
                return;
            }
            
            // CRITICAL: Create object with comprehensive error handling
            GameObject* gameObj = nullptr;
            try {
                gameObj = m_editorLayer->createObject(deferred.objectID, deferred.position, false);
            } catch (std::exception& e) {
                log::warn("Exception creating object {}: {}", m_currentObjectIndex, e.what());
                m_currentObjectIndex++;
                return;
            } catch (...) {
                log::warn("Unknown exception creating object {}", m_currentObjectIndex);
                m_currentObjectIndex++;
                return;
            }
            
            // CRITICAL: Verify object was actually created
            if (!gameObj) {
                log::warn("Failed to create object {} (ID: {}) - skipping", 
                         m_currentObjectIndex, deferred.objectID);
                m_currentObjectIndex++;
                return;
            }
            
            // CRITICAL: Verify object state is valid before modification
            if (!gameObj->m_objectID) {
                log::warn("Object {} has invalid state - skipping properties", m_currentObjectIndex);
                m_currentObjectIndex++;
                return;
            }
            
            // Apply properties safely
            applyObjectProperties(gameObj, deferred.data);
            
            // Update progress indicator every 10 objects
            if (m_currentObjectIndex % 10 == 0) {
                float progress = (float)m_currentObjectIndex / m_deferredObjects.size() * 100.0f;
                showStatus(fmt::format("Creating objects... {:.0f}%", progress), false);
            }
            
            m_currentObjectIndex++;
            
        } catch (std::exception& e) {
            log::error("Exception in updateObjectCreation: {}", e.what());
            m_currentObjectIndex++;
        } catch (...) {
            log::error("Unknown exception in updateObjectCreation");
            m_currentObjectIndex++;
        }
    }

    // CRASH FIX: Safe property application with validation
    void applyObjectProperties(GameObject* gameObj, matjson::Value& objData) {
        if (!gameObj) return;
        
        try {
            // Rotation
            auto rotResult = objData["rotation"].asDouble();
            if (rotResult) {
                float rotation = static_cast<float>(rotResult.unwrap());
                if (rotation >= -360.0f && rotation <= 360.0f) {
                    gameObj->setRotation(rotation);
                }
            }

            // Scale
            auto scaleResult = objData["scale"].asDouble();
            if (scaleResult) {
                float scale = static_cast<float>(scaleResult.unwrap());
                if (scale >= 0.1f && scale <= 10.0f) {
                    gameObj->setScale(scale);
                }
            }

            // Flip X
            auto flipXResult = objData["flip_x"].asBool();
            if (flipXResult && flipXResult.unwrap()) {
                gameObj->setScaleX(-gameObj->getScaleX());
            }

            // Flip Y
            auto flipYResult = objData["flip_y"].asBool();
            if (flipYResult && flipYResult.unwrap()) {
                gameObj->setScaleY(-gameObj->getScaleY());
            }
            
        } catch (std::exception& e) {
            log::warn("Failed to apply object properties: {}", e.what());
        } catch (...) {
            log::warn("Unknown exception applying object properties");
        }
    }

    // CRASH FIX: Prepare objects for deferred creation instead of creating immediately
    void prepareObjects(matjson::Value& objectsArray) {
        if (!m_editorLayer) {
            log::error("No editor layer!");
            return;
        }

        if (!objectsArray.isArray()) {
            log::error("Objects value is not an array");
            return;
        }

        m_deferredObjects.clear();
        m_currentObjectIndex = 0;

        int maxObjects = Mod::get()->getSettingValue<int64_t>("max-objects");
        size_t objectCount = std::min(objectsArray.size(), static_cast<size_t>(maxObjects));

        log::info("Preparing {} objects for progressive creation...", objectCount);

        for (size_t i = 0; i < objectCount; i++) {
            try {
                auto objData = objectsArray[i];

                auto idResult = objData["id"].asInt();
                auto xResult = objData["x"].asDouble();
                auto yResult = objData["y"].asDouble();

                if (!idResult || !xResult || !yResult) {
                    continue;
                }

                int objectID = idResult.unwrap();
                float x = static_cast<float>(xResult.unwrap());
                float y = static_cast<float>(yResult.unwrap());

                // Validate object ID
                if (objectID < 1 || objectID > 10000) {
                    log::warn("Invalid object ID {} at index {}", objectID, i);
                    continue;
                }

                // Store for deferred creation
                DeferredObject deferred;
                deferred.objectID = objectID;
                deferred.position = CCPoint{x, y};
                deferred.data = objData;
                m_deferredObjects.push_back(deferred);

            } catch (std::exception& e) {
                log::warn("Failed to prepare object at index {}: {}", i, e.what());
            }
        }

        log::info("Prepared {} valid objects for creation", m_deferredObjects.size());

        // Start progressive creation
        m_isCreatingObjects = true;
        showStatus("Starting object creation...", false);
    }

    std::string buildSystemPrompt() {
        std::string objectList;
        int count = 0;
        for (auto& [name, id] : OBJECT_IDS) {
            if (count > 0) objectList += ", ";
            objectList += name;
            count++;
            if (count >= 80) {
                objectList += fmt::format("... ({} more)", OBJECT_IDS.size() - 80);
                break;
            }
        }

        return fmt::format(
            "You are a Geometry Dash level designer AI.\n\n"
            "Return ONLY valid JSON - no markdown, no explanations.\n\n"
            "Available objects: {}\n\n"
            "JSON Format:\n"
            "{{\n"
            "  \"analysis\": \"Brief reasoning\",\n"
            "  \"objects\": [\n"
            "    {{\"type\": \"block_black_gradient\", \"x\": 0, \"y\": 30}},\n"
            "    {{\"type\": \"spike\", \"x\": 150, \"y\": 0}}\n"
            "  ]\n"
            "}}\n\n"
            "Coordinates: X=horizontal (10 units=1 grid), Y=vertical (0=ground, 30=1 block up)\n"
            "Spacing: EASY=150-200, MEDIUM=90-150, HARD=60-90, EXTREME=30-60 units\n"
            "Length: SHORT=500-1000, MEDIUM=1000-2000, LONG=2000-4000, XL=4000-8000, XXL=8000+ X units",
            objectList
        );
    }

    void callAPI(const std::string& prompt, const std::string& apiKey) {
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model = Mod::get()->getSettingValue<std::string>("model");
        std::string difficulty = Mod::get()->getSettingValue<std::string>("difficulty");
        std::string style = Mod::get()->getSettingValue<std::string>("style");
        std::string length = Mod::get()->getSettingValue<std::string>("length");

        log::info("Calling {} API with model: {}", provider, model);

        std::string systemPrompt = buildSystemPrompt();
        std::string fullPrompt = fmt::format(
            "Generate a Geometry Dash level:\n\n"
            "Request: {}\n"
            "Difficulty: {}\n"
            "Style: {}\n"
            "Length: {}\n\n"
            "Return JSON with analysis and objects array.",
            prompt, difficulty, style, length
        );

        matjson::Value requestBody;
        std::string url;

        if (provider == "gemini") {
            std::vector<matjson::Value> contents;
            auto message = matjson::Value::object();
            message["role"] = "user";

            std::vector<matjson::Value> parts;
            auto textPart = matjson::Value::object();
            textPart["text"] = systemPrompt + "\n\n" + fullPrompt;
            parts.push_back(textPart);
            message["parts"] = parts;

            contents.push_back(message);

            auto genConfig = matjson::Value::object();
            genConfig["temperature"] = 0.7;
            genConfig["maxOutputTokens"] = 65536;

            requestBody = matjson::Value::object();
            requestBody["contents"] = contents;
            requestBody["generationConfig"] = genConfig;

            url = fmt::format("https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent?key={}", model, apiKey);

        } else if (provider == "claude") {
            std::vector<matjson::Value> messages;
            auto message = matjson::Value::object();
            message["role"] = "user";
            message["content"] = fullPrompt;
            messages.push_back(message);

            requestBody = matjson::Value::object();
            requestBody["model"] = model;
            requestBody["max_tokens"] = 4096;
            requestBody["temperature"] = 0.7;
            requestBody["system"] = systemPrompt;
            requestBody["messages"] = messages;

            url = "https://api.anthropic.com/v1/messages";

        } else if (provider == "openai") {
            std::vector<matjson::Value> messages;
            auto sysMsg = matjson::Value::object();
            sysMsg["role"] = "system";
            sysMsg["content"] = systemPrompt;
            messages.push_back(sysMsg);

            auto userMsg = matjson::Value::object();
            userMsg["role"] = "user";
            userMsg["content"] = fullPrompt;
            messages.push_back(userMsg);

            requestBody = matjson::Value::object();
            requestBody["model"] = model;
            requestBody["messages"] = messages;
            requestBody["temperature"] = 0.7;
            requestBody["max_tokens"] = 4096;

            url = "https://api.openai.com/v1/chat/completions";

        } else if (provider == "ollama") {
            std::string ollamaUrl = Mod::get()->getSettingValue<std::string>("ollama-url");
            std::string ollamaModel = Mod::get()->getSettingValue<std::string>("ollama-model");

            // Combine system and user prompts for Ollama
            std::string combinedPrompt = systemPrompt + "\n\n" + fullPrompt;

            requestBody = matjson::Value::object();
            requestBody["model"] = ollamaModel;
            requestBody["prompt"] = combinedPrompt;
            requestBody["stream"] = false;  // Disable streaming for simpler handling
            requestBody["format"] = "json";  // Request JSON output
            
            auto options = matjson::Value::object();
            options["temperature"] = 0.7;
            requestBody["options"] = options;

            url = ollamaUrl + "/api/generate";
            log::info("Using Ollama at: {}", url);
        }

        std::string jsonBody = requestBody.dump();

        m_listener.bind([this, provider](web::WebTask::Event* e) {
            if (auto res = e->getValue()) {
                this->onAPISuccess(res, provider);
            } else if (e->isCancelled()) {
                this->onCancelled();
            }
        });

        log::info("Sending request to {} (body: {} bytes)", provider, jsonBody.length());

        auto request = web::WebRequest();
        request.header("Content-Type", "application/json");

        if (provider == "claude") {
            request.header("x-api-key", apiKey);
            request.header("anthropic-version", "2023-06-01");
        } else if (provider == "openai") {
            request.header("Authorization", fmt::format("Bearer {}", apiKey));
        }
        // Ollama doesn't need API key headers (local server)

        request.bodyString(jsonBody);
        m_listener.setFilter(request.post(url));
    }

    void onGenerate(CCObject*) {
        std::string prompt = m_promptInput->getString();

        if (prompt.empty() || prompt == "e.g. Medium difficulty platforming") {
            FLAlertLayer::create("Empty Prompt", gd::string("Please enter a description!"), "OK")->show();
            return;
        }

        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string apiKey = Mod::get()->getSavedValue<std::string>("api-key", "");

        // Ollama doesn't need an API key (local server)
        if (apiKey.empty() && provider != "ollama") {
            FLAlertLayer::create(
                "API Key Required",
                gd::string("Please click the lock icon to enter your API key!"),
                "OK"
            )->show();
            return;
        }

        m_generateBtn->setEnabled(false);

        m_loadingCircle = LoadingCircle::create();
        m_loadingCircle->setParentLayer(m_mainLayer);
        m_loadingCircle->setPosition(m_mainLayer->getContentSize() / 2);
        m_loadingCircle->show();

        showStatus("AI is thinking...");

        log::info("=== Generation Request ===");
        log::info("Prompt: {}", prompt);

        callAPI(prompt, apiKey);
    }

    void onAPISuccess(web::WebResponse* response, const std::string& provider) {
        if (m_loadingCircle) {
            m_loadingCircle->fadeAndRemove();
            m_loadingCircle = nullptr;
        }
        m_generateBtn->setEnabled(true);

        if (!response->ok()) {
            std::string errorBody = response->string().unwrapOr("No error details available");
            log::error("API request failed: HTTP {}", response->code());

            auto [title, message] = parseAPIError(errorBody, response->code());

            showStatus("Failed!", true);
            FLAlertLayer::create(title.c_str(), gd::string(message), "OK")->show();
            return;
        }

        try {
            auto jsonRes = response->json();
            if (!jsonRes) {
                onError("Invalid Response", "The API returned invalid data.");
                return;
            }

            auto json = jsonRes.unwrap();
            std::string aiResponse;

            log::info("Received response from {}", provider);

            if (provider == "gemini") {
                auto candidates = json["candidates"];
                if (!candidates.isArray() || candidates.size() == 0) {
                    onError("No Response", "The AI didn't generate any content.");
                    return;
                }
                auto textResult = candidates[0]["content"]["parts"][0]["text"].asString();
                if (!textResult) {
                    onError("Invalid Response", "Failed to extract AI response.");
                    return;
                }
                aiResponse = textResult.unwrap();
            } else if (provider == "claude") {
                auto content = json["content"];
                if (!content.isArray() || content.size() == 0) {
                    onError("No Response", "The AI didn't generate any content.");
                    return;
                }
                auto textResult = content[0]["text"].asString();
                if (!textResult) {
                    onError("Invalid Response", "Failed to extract AI response.");
                    return;
                }
                aiResponse = textResult.unwrap();
            } else if (provider == "openai") {
                auto choices = json["choices"];
                if (!choices.isArray() || choices.size() == 0) {
                    onError("No Response", "The AI didn't generate any content.");
                    return;
                }
                auto textResult = choices[0]["message"]["content"].asString();
                if (!textResult) {
                    onError("Invalid Response", "Failed to extract AI response.");
                    return;
                }
                aiResponse = textResult.unwrap();
            } else if (provider == "ollama") {
                // Ollama has a simpler response format
                auto responseResult = json["response"].asString();
                if (!responseResult) {
                    onError("Invalid Response", "Failed to extract Ollama response.");
                    return;
                }
                aiResponse = responseResult.unwrap();
                
                // Check if generation completed
                auto doneResult = json["done"].asBool();
                if (doneResult && !doneResult.unwrap()) {
                    log::warn("Ollama response marked as incomplete");
                }
                
                log::info("Received Ollama response ({} chars)", aiResponse.length());
            }

            // Clean markdown code blocks
            size_t codeBlockStart = aiResponse.find("```");
            while (codeBlockStart != std::string::npos) {
                size_t codeBlockEnd = aiResponse.find("```", codeBlockStart + 3);
                if (codeBlockEnd != std::string::npos) {
                    aiResponse.erase(codeBlockStart, codeBlockEnd - codeBlockStart + 3);
                    codeBlockStart = aiResponse.find("```");
                } else {
                    break;
                }
            }

            // Extract JSON
            size_t jsonStart = aiResponse.find('{');
            size_t jsonEnd = aiResponse.rfind('}');

            if (jsonStart == std::string::npos || jsonEnd == std::string::npos) {
                onError("Invalid Response", "No valid level data found in response.");
                return;
            }

            aiResponse = aiResponse.substr(jsonStart, jsonEnd - jsonStart + 1);

            auto levelJson = matjson::parse(aiResponse);
            if (!levelJson) {
                onError("Parse Error", "Failed to parse level data.");
                return;
            }

            auto levelData = levelJson.unwrap();

            if (!levelData.contains("objects")) {
                onError("Invalid Data", "Response doesn't contain level objects.");
                return;
            }

            auto objectsArray = levelData["objects"];
            if (!objectsArray.isArray() || objectsArray.size() == 0) {
                onError("No Objects", "The AI didn't generate any objects.");
                return;
            }

            // Convert types to IDs
            for (size_t i = 0; i < objectsArray.size(); i++) {
                auto obj = objectsArray[i];
                auto typeResult = obj["type"].asString();
                if (typeResult) {
                    std::string type = typeResult.unwrap();
                    if (OBJECT_IDS.find(type) != OBJECT_IDS.end()) {
                        objectsArray[i]["id"] = OBJECT_IDS[type];
                    } else {
                        objectsArray[i]["id"] = 1;
                    }
                }
            }

            // Clear level if requested
            if (m_shouldClearLevel) {
                clearLevel();
            }

            // CRASH FIX: Prepare objects for progressive creation
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

    void onCancelled() {
        if (m_loadingCircle) {
            m_loadingCircle->fadeAndRemove();
            m_loadingCircle = nullptr;
        }
        m_generateBtn->setEnabled(true);
        showStatus("Cancelled", true);
    }

    void closePopup() {
        this->onClose(nullptr);
    }

public:
    static AIGeneratorPopup* create(LevelEditorLayer* layer) {
        auto ret = new AIGeneratorPopup();
        if (ret->initAnchored(420.f, 300.f, layer)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Hook EditorUI to add AI button
class $modify(AIEditorUI, EditorUI) {
    struct Fields {
        CCMenuItemSpriteExtra* m_aiButton;
        CCMenu* m_aiMenu;
        bool m_buttonAdded = false;
    };

    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;

        auto addButtonAction = CCSequence::create(
            CCDelayTime::create(0.1f),
            CCCallFunc::create(this, callfunc_selector(AIEditorUI::addAIButton)),
            nullptr
        );
        this->runAction(addButtonAction);

        return true;
    }

    void addAIButton() {
        if (m_fields->m_buttonAdded) return;
        m_fields->m_buttonAdded = true;

        auto bg = ButtonSprite::create(
            "AI",
            "goldFont.fnt",
            "GJ_button_04.png",
            0.8f
        );

        m_fields->m_aiButton = CCMenuItemSpriteExtra::create(
            bg,
            this,
            menu_selector(AIEditorUI::onAIButton)
        );

        m_fields->m_aiMenu = CCMenu::create();
        m_fields->m_aiMenu->addChild(m_fields->m_aiButton);

        // Position relative to editor UI elements
        auto winSize = CCDirector::get()->getWinSize();
        m_fields->m_aiButton->setPosition({0, 0});
        m_fields->m_aiMenu->setPosition({winSize.width - 45, winSize.height - 30});
        m_fields->m_aiMenu->setZOrder(100);
        m_fields->m_aiMenu->setID("ai-generator-menu"_spr);

        this->addChild(m_fields->m_aiMenu);

        log::info("AI button added to editor");
    }

    void onAIButton(CCObject*) {
        auto layer = this->m_editorLayer;
        if (!layer) {
            FLAlertLayer::create("Error", gd::string("No editor layer found!"), "OK")->show();
            return;
        }
        
        AIGeneratorPopup::create(layer)->show();
    }
};

// Hide AI button during playtest
class $modify(EditorPauseLayer) {
    void onResume(CCObject* sender) {
        EditorPauseLayer::onResume(sender);
        
        if (auto editorUI = m_editorLayer->m_editorUI) {
            if (auto aiMenu = typeinfo_cast<CCMenu*>(editorUI->getChildByID("ai-generator-menu"_spr))) {
                aiMenu->setVisible(true);
            }
        }
    }
};

// Hide button when entering playtest
class $modify(LevelEditorLayer) {
    void onPlaytest() {
        LevelEditorLayer::onPlaytest();
        
        if (auto editorUI = this->m_editorUI) {
            if (auto aiMenu = typeinfo_cast<CCMenu*>(editorUI->getChildByID("ai-generator-menu"_spr))) {
                aiMenu->setVisible(false);
            }
        }
    }
};

$execute {
    log::info("========================================");
    log::info("  Editor AI v2.1.2 - Ollama Integration");
    log::info("========================================");
    log::info("Loaded {} object types", OBJECT_IDS.size());
    if (OBJECT_IDS.size() > 10) {
        log::info("Object library: Loaded from local file");
    } else {
        log::info("Object library: Using defaults (5 objects)");
    }
    log::info("Progressive object creation enabled");
    log::info("Ollama (local AI) support enabled");
    log::info("========================================");
    
    // Schedule async GitHub update after a short delay
    Loader::get()->queueInMainThread([] {
        updateObjectIDsFromGitHub();
    });
}
