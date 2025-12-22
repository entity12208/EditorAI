#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/ui/TextInput.hpp>
#include <chrono>

using namespace geode::prelude;

// Global object IDs storage
static std::unordered_map<std::string, int> OBJECT_IDS;

// Rate limiting tracker
static std::chrono::steady_clock::time_point lastRequestTime;

// Known trigger IDs - verified from GD resources
static const int TRIGGER_ALPHA = 1007;
static const int TRIGGER_MOVE = 901;
static const int TRIGGER_TOGGLE = 1049;

// Load object IDs from cached file or defaults
static void loadObjectIDs() {
    auto cachePath = Mod::get()->getSaveDir() / "object_ids.json";
    
    if (std::filesystem::exists(cachePath)) {
        try {
            std::ifstream file(cachePath);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            
            auto json = matjson::parse(content);
            if (json) {
                auto obj = json.unwrap();
                if (obj.isObject()) {
                    for (auto& [key, value] : obj) {
                        auto intVal = value.asInt();
                        if (intVal) {
                            OBJECT_IDS[key] = intVal.unwrap();
                        }
                    }
                    log::info("Loaded {} object IDs from cache", OBJECT_IDS.size());
                    return;
                }
            }
        } catch (...) {}
    }
    
    // Use defaults if no cache
    log::info("Using default object IDs, will download full list...");
    OBJECT_IDS["block_black_gradient_square"] = 1;
    OBJECT_IDS["spike_black_gradient_spike"] = 8;
    OBJECT_IDS["platform"] = 1731;
    OBJECT_IDS["jump_orb_yellow_jump_orb"] = 36;
    OBJECT_IDS["jump_pad_yellow_jump_pad"] = 35;
    OBJECT_IDS["alpha_trigger"] = TRIGGER_ALPHA;
    OBJECT_IDS["move_trigger"] = TRIGGER_MOVE;
    OBJECT_IDS["toggle_trigger"] = TRIGGER_TOGGLE;
}

// Download object IDs from GitHub
static void downloadObjectIDs() {
    static EventListener<web::WebTask> listener;
    
    listener.bind([](web::WebTask::Event* e) {
        if (auto res = e->getValue()) {
            if (!res->ok()) {
                log::warn("Failed to download object_ids.json: HTTP {}", res->code());
                return;
            }
            
            try {
                auto response = res->string();
                if (!response) {
                    log::warn("Failed to get response string");
                    return;
                }
                
                auto json = matjson::parse(response.unwrap());
                if (!json) {
                    log::error("Failed to parse downloaded JSON");
                    return;
                }
                
                auto obj = json.unwrap();
                if (!obj.isObject()) {
                    log::error("Downloaded JSON is not an object");
                    return;
                }
                
                // Update global map
                OBJECT_IDS.clear();
                for (auto& [key, value] : obj) {
                    auto intVal = value.asInt();
                    if (intVal) {
                        OBJECT_IDS[key] = intVal.unwrap();
                    }
                }
                
                // Save to cache
                auto cachePath = Mod::get()->getSaveDir() / "object_ids.json";
                std::ofstream file(cachePath);
                file << response.unwrap();
                file.close();
                
                log::info("Downloaded and cached {} object IDs", OBJECT_IDS.size());
            } catch (std::exception& e) {
                log::error("Failed to process downloaded object_ids.json: {}", e.what());
            }
        }
    });
    
    auto req = web::WebRequest();
    listener.setFilter(req.get("https://raw.githubusercontent.com/entity12208/EditorAI/refs/heads/main/resources/object_ids.json"));
}

// Helper to mask API keys in logs
static std::string maskApiKey(const std::string& key) {
    if (key.length() <= 8) return "***";
    return key.substr(0, 4) + "..." + key.substr(key.length() - 4);
}

// Parse HEX color string to ccColor3B
static ccColor3B parseHexColor(const std::string& hex) {
    if (hex.length() != 6 && hex.length() != 7) {
        return {255, 255, 255}; // Default to white
    }
    
    std::string cleanHex = hex;
    if (cleanHex[0] == '#') {
        cleanHex = cleanHex.substr(1);
    }
    
    try {
        unsigned int hexValue = std::stoul(cleanHex, nullptr, 16);
        return {
            static_cast<GLubyte>((hexValue >> 16) & 0xFF),
            static_cast<GLubyte>((hexValue >> 8) & 0xFF),
            static_cast<GLubyte>(hexValue & 0xFF)
        };
    } catch (...) {
        return {255, 255, 255};
    }
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

// Helper to configure trigger properties
// NOTE: Limited by Geode bindings - logs configuration for future implementation
static void configureTrigger(GameObject* trigger, matjson::Value& triggerData) {
    if (!trigger) return;
    
    int objectID = trigger->m_objectID;
    bool isTrigger = (objectID == TRIGGER_ALPHA || objectID == TRIGGER_MOVE || objectID == TRIGGER_TOGGLE);
    
    if (!isTrigger) return;
    
    log::info("Creating trigger {} at position ({}, {})", objectID, trigger->getPositionX(), trigger->getPositionY());
    
    // Log target group configuration (for future when bindings expose trigger internals)
    auto targetGroupResult = triggerData["target_group"].asInt();
    if (targetGroupResult) {
        int targetGroup = targetGroupResult.unwrap();
        log::info("  Target Group: {}", targetGroup);
    }
    
    // Log touch-triggered mode
    auto touchTriggeredResult = triggerData["touch_triggered"].asBool();
    if (touchTriggeredResult && touchTriggeredResult.unwrap()) {
        log::info("  Touch Triggered: YES");
    }
    
    // Log trigger-specific configurations
    switch (objectID) {
        case TRIGGER_ALPHA: {
            auto opacityResult = triggerData["opacity"].asDouble();
            if (opacityResult) {
                float opacity = static_cast<float>(opacityResult.unwrap());
                log::info("  Opacity: {}", opacity);
            }
            
            auto durationResult = triggerData["duration"].asDouble();
            if (durationResult) {
                float duration = static_cast<float>(durationResult.unwrap());
                log::info("  Duration: {} seconds", duration);
            }
            break;
        }
        
        case TRIGGER_MOVE: {
            auto moveXResult = triggerData["move_x"].asDouble();
            auto moveYResult = triggerData["move_y"].asDouble();
            
            if (moveXResult || moveYResult) {
                float moveX = moveXResult ? static_cast<float>(moveXResult.unwrap()) : 0.0f;
                float moveY = moveYResult ? static_cast<float>(moveYResult.unwrap()) : 0.0f;
                log::info("  Move Offset: ({}, {})", moveX, moveY);
            }
            
            auto durationResult = triggerData["duration"].asDouble();
            if (durationResult) {
                float duration = static_cast<float>(durationResult.unwrap());
                log::info("  Duration: {} seconds", duration);
            }
            
            auto easingResult = triggerData["easing"].asInt();
            if (easingResult) {
                int easing = easingResult.unwrap();
                log::info("  Easing: {}", easing);
            }
            break;
        }
        
        case TRIGGER_TOGGLE: {
            auto activateResult = triggerData["activate_group"].asBool();
            if (activateResult) {
                bool activate = activateResult.unwrap();
                log::info("  Activate: {}", activate ? "ON" : "OFF");
            }
            break;
        }
    }
    
    // Assign groups to the trigger object itself (for trigger activation control)
    auto groupsData = triggerData["groups"];
    if (groupsData.isArray() && trigger->m_groups) {
        for (size_t g = 0; g < groupsData.size() && trigger->m_groupCount < 10; g++) {
            auto groupResult = groupsData[g].asInt();
            if (groupResult) {
                int groupID = groupResult.unwrap();
                (*trigger->m_groups)[trigger->m_groupCount] = static_cast<short>(groupID);
                trigger->m_groupCount++;
                log::info("  Assigned to Group: {}", groupID);
            }
        }
    }
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
        m_keyInput->setAllowedChars("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_!@#$%^&*()+=[]{}|;:',.<>?/~`");
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

        return true;
    }

    void onToggleClear(CCObject*) {
        m_shouldClearLevel = !m_shouldClearLevel;
    }

    void onInfo(CCObject*) {
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model = Mod::get()->getSettingValue<std::string>("model");
        bool rateLimiting = Mod::get()->getSettingValue<bool>("enable-rate-limiting");
        bool colors = Mod::get()->getSettingValue<bool>("enable-colors");
        bool groups = Mod::get()->getSettingValue<bool>("enable-group-ids");
        bool triggers = Mod::get()->getSettingValue<bool>("enable-triggers");

        std::string features = "<cy>Features:</c>\n";
        if (colors) features += "• <cg>Color Control</c>\n";
        if (groups) features += "• <cg>Group IDs</c>\n";
        if (triggers) features += "• <cg>Trigger Support</c>\n";

        FLAlertLayer::create(
            "Editor AI",
            gd::string(fmt::format(
                "<cy>Direct API Integration</c>\n\n"
                "<cg>Provider:</c> <co>{}</co>\n"
                "<cg>Model:</c> <co>{}</co>\n"
                "<cy>Rate Limiting:</c> {}\n\n"
                "{}\n"
                "<cy>Setup:</c>\n"
                "• Click lock icon to enter API key\n"
                "• Select provider & model in settings\n"
                "• Configure features in settings\n"
                "• Start generating!\n\n"
                "<co>Loaded {} object types</co>\n\n"
                "<cl>Trigger Support:</cl>\n"
                "• Alpha Trigger (1007)\n"
                "• Move Trigger (901)\n"
                "• Toggle Trigger (1049)\n\n"
                "<cp>Available on all platforms!</cp>",
                provider,
                model,
                rateLimiting ? "<cg>On</c>" : "<cr>Off</c>",
                features,
                OBJECT_IDS.size()
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

    void createObjects(matjson::Value& objectsArray) {
        if (!m_editorLayer) {
            log::error("No editor layer!");
            return;
        }

        if (!objectsArray.isArray()) {
            log::error("Objects value is not an array");
            return;
        }

        bool enableColors = Mod::get()->getSettingValue<bool>("enable-colors");
        bool enableGroups = Mod::get()->getSettingValue<bool>("enable-group-ids");
        bool enableTriggers = Mod::get()->getSettingValue<bool>("enable-triggers");

        int created = 0;
        int triggers = 0;
        int maxObjects = Mod::get()->getSettingValue<int64_t>("max-objects");

        size_t objectCount = std::min(objectsArray.size(), static_cast<size_t>(maxObjects));

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

                auto gameObj = m_editorLayer->createObject(objectID, {x, y}, false);

                if (!gameObj) {
                    continue;
                }

                // Standard properties
                auto rotResult = objData["rotation"].asDouble();
                if (rotResult) {
                    gameObj->setRotation(static_cast<float>(rotResult.unwrap()));
                }

                auto scaleResult = objData["scale"].asDouble();
                if (scaleResult) {
                    gameObj->setScale(static_cast<float>(scaleResult.unwrap()));
                }

                auto flipXResult = objData["flip_x"].asBool();
                if (flipXResult && flipXResult.unwrap()) {
                    gameObj->setScaleX(-gameObj->getScaleX());
                }

                auto flipYResult = objData["flip_y"].asBool();
                if (flipYResult && flipYResult.unwrap()) {
                    gameObj->setScaleY(-gameObj->getScaleY());
                }

                // Initial Colors
                if (enableColors && objData.contains("color")) {
                    auto colorResult = objData["color"].asString();
                    if (colorResult) {
                        auto color = parseHexColor(colorResult.unwrap());
                        gameObj->setObjectColor(color);
                        log::debug("Set color {} on object {}", colorResult.unwrap(), objectID);
                    }
                }

                // Group IDs
                if (enableGroups && objData.contains("groups")) {
                    auto groupsData = objData["groups"];
                    if (groupsData.isArray()) {
                        for (size_t g = 0; g < groupsData.size(); g++) {
                            auto groupResult = groupsData[g].asInt();
                            if (groupResult) {
                                int groupID = groupResult.unwrap();
                                gameObj->addToGroup(groupID);
                                log::debug("Added object {} to group {}", objectID, groupID);
                            }
                        }
                    }
                }

                // Trigger Configuration
                bool isTrigger = (objectID == TRIGGER_ALPHA || objectID == TRIGGER_MOVE || objectID == TRIGGER_TOGGLE);
                if (enableTriggers && isTrigger) {
                    configureTrigger(gameObj, objData);
                    triggers++;
                    log::info("Configured trigger {} at ({}, {})", objectID, x, y);
                }

                created++;

            } catch (std::exception& e) {
                log::warn("Failed to create object at index {}: {}", i, e.what());
            }
        }

        log::info("Successfully created {} objects ({} triggers)", created, triggers);

        if (auto editorUI = m_editorLayer->m_editorUI) {
            editorUI->updateButtons();
        }
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

        bool enableColors = Mod::get()->getSettingValue<bool>("enable-colors");
        bool enableGroups = Mod::get()->getSettingValue<bool>("enable-group-ids");
        bool enableTriggers = Mod::get()->getSettingValue<bool>("enable-triggers");

        std::string features;
        if (enableColors || enableGroups || enableTriggers) {
            features = "\n\n<cy>ENABLED FEATURES:</c>\n";
            if (enableColors) {
                features += "- You can set \"color\": \"#RRGGBB\" (HEX) on objects\n";
            }
            if (enableGroups) {
                features += "- You can set \"groups\": [1, 2, 3] (array of group IDs) on objects\n";
            }
            if (enableTriggers) {
                features += "- You can use TRIGGERS with full configuration:\n"
                                       "  \n"
                                       "  Alpha Trigger (alpha_trigger):\n"
                                       "  {{\"type\": \"alpha_trigger\", \"x\": 100, \"y\": 0,\n"
                                       "   \"target_group\": 1, \"opacity\": 0.5, \"duration\": 1.0,\n"
                                       "   \"touch_triggered\": true}}\n"
                                       "  \n"
                                       "  Move Trigger (move_trigger):\n"
                                       "  {{\"type\": \"move_trigger\", \"x\": 200, \"y\": 0,\n"
                                       "   \"target_group\": 1, \"move_x\": 100, \"move_y\": 50,\n"
                                       "   \"duration\": 2.0, \"easing\": 1, \"touch_triggered\": true}}\n"
                                       "  \n"
                                       "  Toggle Trigger (toggle_trigger):\n"
                                       "  {{\"type\": \"toggle_trigger\", \"x\": 300, \"y\": 0,\n"
                                       "   \"target_group\": 2, \"activate_group\": true,\n"
                                       "   \"touch_triggered\": true}}\n"
                                       "  \n"
                                       "  - Use groups to organize objects and triggers\n"
                                       "  - Touch-triggered triggers activate on player contact\n"
                                       "  - Combine triggers for complex mechanics\n";
            }
        }

        return fmt::format(
            "You are a Geometry Dash level designer AI.\n\n"
            "Return ONLY valid JSON - no markdown, no explanations.\n\n"
            "Available objects: {}{}\n\n"
            "JSON Format:\n"
            "{{\n"
            "  \"analysis\": \"Brief reasoning\",\n"
            "  \"objects\": [\n"
            "    {{\"type\": \"block_black_gradient_square\", \"x\": 0, \"y\": 30}},\n"
            "    {{\"type\": \"spike_black_gradient_spike\", \"x\": 150, \"y\": 0}}\n"
            "  ]\n"
            "}}\n\n"
            "Coordinates: X=horizontal (10 units=1 grid), Y=vertical (0=ground, 30=1 block up)\n"
            "Spacing: EASY=150-200, MEDIUM=90-150, HARD=60-90, EXTREME=30-60 units\n"
            "Length: SHORT=500-1000, MEDIUM=1000-2000, LONG=2000-4000, XL=4000-8000, XXL=8000+ X units",
            objectList,
            features
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

        request.bodyString(jsonBody);
        m_listener.setFilter(request.post(url));
    }

    void onGenerate(CCObject*) {
        std::string prompt = m_promptInput->getString();

        if (prompt.empty() || prompt == "e.g. Medium difficulty platforming") {
            FLAlertLayer::create("Empty Prompt", gd::string("Please enter a description!"), "OK")->show();
            return;
        }

        std::string apiKey = Mod::get()->getSavedValue<std::string>("api-key", "");

        if (apiKey.empty()) {
            FLAlertLayer::create(
                "API Key Required",
                gd::string("Please click the lock icon to enter your API key!"),
                "OK"
            )->show();
            return;
        }

        // Rate limiting check
        if (Mod::get()->getSettingValue<bool>("enable-rate-limiting")) {
            int rateLimitSeconds = Mod::get()->getSettingValue<int64_t>("rate-limit-seconds");
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastRequestTime).count();

            if (elapsed < rateLimitSeconds) {
                int remaining = rateLimitSeconds - elapsed;
                FLAlertLayer::create(
                    "Rate Limit",
                    gd::string(fmt::format(
                        "Please wait {} more second{} before generating again.\n\n"
                        "This prevents excessive API usage and saves tokens.",
                        remaining,
                        remaining == 1 ? "" : "s"
                    )),
                    "OK"
                )->show();
                return;
            }
        }

        m_generateBtn->setEnabled(false);

        m_loadingCircle = LoadingCircle::create();
        m_loadingCircle->setParentLayer(m_mainLayer);
        m_loadingCircle->setPosition(m_mainLayer->getContentSize() / 2);
        m_loadingCircle->show();

        showStatus("AI is thinking...");

        log::info("=== Generation Request ===");
        log::info("Prompt: {}", prompt);

        // Update rate limit tracker
        lastRequestTime = std::chrono::steady_clock::now();

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

            showStatus("Creating objects...");

            if (m_shouldClearLevel) {
                clearLevel();
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

            createObjects(objectsArray);

            showStatus(fmt::format("✓ Created {} objects!", objectsArray.size()), false);

            Notification::create(
                fmt::format("Generated {} objects!", objectsArray.size()),
                NotificationIcon::Success
            )->show();

            auto closeAction = CCSequence::create(
                CCDelayTime::create(2.0f),
                CCCallFunc::create(this, callfunc_selector(AIGeneratorPopup::closePopup)),
                nullptr
            );
            this->runAction(closeAction);

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
    log::info("       Editor AI v2.4.0 Loaded");
    log::info("   Cross-Platform Support Enabled!");
    log::info("========================================");
    
    // Initialize rate limit tracker
    lastRequestTime = std::chrono::steady_clock::now() - std::chrono::hours(1);
    
    // Load object IDs from cache or defaults
    loadObjectIDs();
    log::info("Loaded {} object types", OBJECT_IDS.size());
    
    // Download latest object IDs from GitHub
    downloadObjectIDs();
    
    // Log feature status
    bool colors = Mod::get()->getSettingValue<bool>("enable-colors");
    bool groups = Mod::get()->getSettingValue<bool>("enable-group-ids");
    bool triggers = Mod::get()->getSettingValue<bool>("enable-triggers");
    
    log::info("Features: Colors={}, Groups={}, Triggers={}", colors, groups, triggers);
    
    // Log rate limiting status
    bool rateLimiting = Mod::get()->getSettingValue<bool>("enable-rate-limiting");
    if (rateLimiting) {
        int seconds = Mod::get()->getSettingValue<int64_t>("rate-limit-seconds");
        log::info("Rate limiting: ENABLED ({} seconds)", seconds);
    } else {
        log::info("Rate limiting: DISABLED");
    }
    
    log::info("Trigger support: Alpha ({}), Move ({}), Toggle ({})", TRIGGER_ALPHA, TRIGGER_MOVE, TRIGGER_TOGGLE);
    log::info("Platform support: Windows, Android32, Android64, MacOS, iOS");
    log::info("========================================");
}