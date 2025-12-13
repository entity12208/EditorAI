#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/ui/TextInput.hpp>

using namespace geode::prelude;

// Load object IDs from JSON file
static std::unordered_map<std::string, int> loadObjectIDs() {
    std::unordered_map<std::string, int> ids;
    
    auto path = Mod::get()->getResourcesDir() / "object_ids.json";
    
    if (!std::filesystem::exists(path)) {
        log::warn("object_ids.json not found, using defaults");
        ids["block_black_gradient"] = 1;
        ids["spike"] = 8;
        ids["platform"] = 1731;
        ids["orb_yellow"] = 36;
        ids["pad_yellow"] = 35;
        return ids;
    }
    
    try {
        std::ifstream file(path);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        
        auto json = matjson::parse(content);
        if (!json) {
            log::error("Failed to parse object_ids.json: {}", json.unwrapErr());
            return ids;
        }
        
        auto obj = json.unwrap();
        if (!obj.isObject()) {
            log::error("object_ids.json root is not an object");
            return ids;
        }
        
        for (auto& [key, value] : obj) {
            auto intVal = value.asInt();
            if (intVal) {
                ids[key] = intVal.unwrap();
            }
        }
        
        log::info("Loaded {} object IDs", ids.size());
    } catch (std::exception& e) {
        log::error("Error loading object_ids.json: {}", e.what());
    }
    
    return ids;
}

static std::unordered_map<std::string, int> OBJECT_IDS = loadObjectIDs();

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
        saveBtn->setPosition({-60, -60});
        cancelBtn->setPosition({60, -60});
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
            FLAlertLayer::create("Error", "Please enter a valid API key", "OK")->show();
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
        toggleMenu->addChild(m_clearToggle);
        m_clearToggle->setPosition({-80, 0});
        clearLabel->setPosition({20, 0});
        toggleMenu->addChild(clearLabel);
        toggleMenu->setPosition({winSize.width / 2, winSize.height / 2 - 35});
        m_mainLayer->addChild(toggleMenu);
        
        m_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_statusLabel->setScale(0.4f);
        m_statusLabel->setPosition({winSize.width / 2, winSize.height / 2 - 60});
        m_statusLabel->setVisible(false);
        m_mainLayer->addChild(m_statusLabel);
        
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
        
        auto btnMenu = CCMenu::create();
        m_generateBtn->setPosition({0, -85});
        infoBtn->setPosition({165, 95});
        keyBtn->setPosition({-165, 95});
        btnMenu->addChild(m_generateBtn);
        btnMenu->addChild(infoBtn);
        btnMenu->addChild(keyBtn);
        m_mainLayer->addChild(btnMenu);
        
        return true;
    }
    
    void onToggleClear(CCObject*) {
        m_shouldClearLevel = !m_shouldClearLevel;
    }
    
    void onInfo(CCObject*) {
        std::string provider = Mod::get()->getSettingValue<std::string>("ai-provider");
        std::string model = Mod::get()->getSettingValue<std::string>("model");
        
        FLAlertLayer::create(
            "Editor AI",
            fmt::format(
                "<cy>Direct API Integration</c>\n\n"
                "<cg>Provider:</c> <co>{}</co>\n"
                "<cg>Model:</c> <co>{}</co>\n\n"
                "<cy>Setup:</c>\n"
                "• Click lock icon to enter API key\n"
                "• Select provider & model in settings\n"
                "• Start generating!\n\n"
                "<co>Loaded {} object types</co>",
                provider,
                model,
                OBJECT_IDS.size()
            ),
            "OK"
        )->show();
    }
    
    void onKeyButton(CCObject*) {
        std::string currentKey = Mod::get()->getSavedValue<std::string>("api-key", "");
        
        if (!currentKey.empty()) {
            // Show options menu - only 2 buttons (Cancel and Delete)
            geode::createQuickPopup(
                "API Key",
                "API key is saved. Delete it?",
                "Cancel", "Delete",
                [this](FLAlertLayer*, bool btn2) {
                    if (btn2) {
                        // Delete button
                        Mod::get()->setSavedValue<std::string>("api-key", std::string());
                        Notification::create("API Key Deleted", NotificationIcon::Success)->show();
                    }
                }
            );
        } else {
            // No key saved, show popup to enter
            showKeyPopup();
        }
    }
    
    void showKeyPopup() {
        APIKeyPopup::create([](std::string key) {
            Mod::get()->setSavedValue("api-key", key);
            Notification::create("API Key Saved!", NotificationIcon::Success)->show();
            log::info("API key saved (length: {})", key.length());
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
        
        log::info("[Level Clear] Cleared {} objects from editor", toRemove->count());
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
        
        int created = 0;
        int maxObjects = Mod::get()->getSettingValue<int64_t>("max-objects");
        
        size_t objectCount = std::min(objectsArray.size(), static_cast<size_t>(maxObjects));
        
        for (size_t i = 0; i < objectCount; i++) {
            try {
                auto objData = objectsArray[i];
                
                auto idResult = objData["id"].asInt();
                auto xResult = objData["x"].asDouble();
                auto yResult = objData["y"].asDouble();
                
                if (!idResult || !xResult || !yResult) {
                    log::warn("Missing required fields for object at index {}", i);
                    continue;
                }
                
                int objectID = idResult.unwrap();
                float x = static_cast<float>(xResult.unwrap());
                float y = static_cast<float>(yResult.unwrap());
                
                auto gameObj = m_editorLayer->createObject(objectID, {x, y}, false);
                
                if (!gameObj) {
                    log::warn("Failed to create object {}", objectID);
                    continue;
                }
                
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
                
                created++;
                
            } catch (std::exception& e) {
                log::warn("Failed to create object at index {}: {}", i, e.what());
            }
        }
        
        log::info("[Object Creation] Successfully created {} objects in editor", created);
        
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
        
        log::info("[API Call] Provider: {}, Model: {}", provider, model);
        log::info("[API Call] Settings: {} / {} / {}", difficulty, style, length);
        
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
            // Build contents array using std::vector
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
            // Build messages array using std::vector
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
            // Build messages array using std::vector
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
        
        auto req = web::WebRequest();
        req.header("Content-Type", "application/json");
        
        if (provider == "gemini") {
            // Key in URL for Gemini
        } else if (provider == "claude") {
            req.header("x-api-key", apiKey);
            req.header("anthropic-version", "2023-06-01");
        } else if (provider == "openai") {
            req.header("Authorization", fmt::format("Bearer {}", apiKey));
        }
        
        req.bodyString(jsonBody);
        
        m_listener.bind([this, provider](web::WebTask::Event* e) {
            if (auto res = e->getValue()) {
                this->onAPISuccess(res, provider);
            } else if (e->isCancelled()) {
                this->onCancelled();
            }
        });
        
        log::info("[API Call] Sending request to {} API...", provider);
        log::info("[API Call] Endpoint: {}", url.substr(0, 50));
        m_listener.setFilter(req.post(url));
    }
    
    void onGenerate(CCObject*) {
        std::string prompt = m_promptInput->getString();
        
        if (prompt.empty() || prompt == "e.g. Medium difficulty platforming") {
            FLAlertLayer::create("Empty Prompt", "Please enter a description!", "OK")->show();
            return;
        }
        
        std::string apiKey = Mod::get()->getSavedValue<std::string>("api-key", "");
        
        if (apiKey.empty()) {
            FLAlertLayer::create(
                "API Key Required",
                "Please enter your API key in mod settings!\n\n"
                "Settings → Mods → Editor AI → Enter API Key",
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
            onError(fmt::format("API error: HTTP {}", response->code()));
            return;
        }
        
        try {
            auto jsonRes = response->json();
            if (!jsonRes) {
                onError("Invalid JSON response");
                return;
            }
            
            auto json = jsonRes.unwrap();
            std::string aiResponse;
            
            log::info("[API Response] Received from {}, parsing...", provider);
            
            if (provider == "gemini") {
                auto candidates = json["candidates"];
                if (!candidates.isArray() || candidates.size() == 0) {
                    onError("No response from Gemini");
                    return;
                }
                auto textResult = candidates[0]["content"]["parts"][0]["text"].asString();
                if (!textResult) {
                    onError("Failed to extract text");
                    return;
                }
                aiResponse = textResult.unwrap();
            } else if (provider == "claude") {
                auto content = json["content"];
                if (!content.isArray() || content.size() == 0) {
                    onError("No response from Claude");
                    return;
                }
                auto textResult = content[0]["text"].asString();
                if (!textResult) {
                    onError("Failed to extract text");
                    return;
                }
                aiResponse = textResult.unwrap();
            } else if (provider == "openai") {
                auto choices = json["choices"];
                if (!choices.isArray() || choices.size() == 0) {
                    onError("No response from OpenAI");
                    return;
                }
                auto textResult = choices[0]["message"]["content"].asString();
                if (!textResult) {
                    onError("Failed to extract text");
                    return;
                }
                aiResponse = textResult.unwrap();
            }
            
            log::info("[AI Response] Got {} characters", aiResponse.length());
            
            // Clean markdown
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
                onError("No JSON found in response");
                return;
            }
            
            aiResponse = aiResponse.substr(jsonStart, jsonEnd - jsonStart + 1);
            
            auto levelJson = matjson::parse(aiResponse);
            if (!levelJson) {
                onError("Failed to parse JSON");
                return;
            }
            
            auto levelData = levelJson.unwrap();
            
            if (!levelData.contains("objects")) {
                onError("Missing objects array");
                return;
            }
            
            auto objectsArray = levelData["objects"];
            if (!objectsArray.isArray() || objectsArray.size() == 0) {
                onError("No objects generated");
                return;
            }
            
            showStatus("Processing objects...");
            
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
            
            log::info("[Level Generation] Creating {} objects...", objectsArray.size());
            createObjects(objectsArray);
            log::info("[Level Generation] Successfully created objects!");
            
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
            onError(fmt::format("Error: {}", e.what()));
        }
    }
    
    void onError(const std::string& error) {
        if (m_loadingCircle) {
            m_loadingCircle->fadeAndRemove();
            m_loadingCircle = nullptr;
        }
        m_generateBtn->setEnabled(true);
        
        showStatus("Failed!", true);
        log::error("[ERROR] Generation failed: {}", error);
        log::error("[ERROR] Check your API key and internet connection");
        
        FLAlertLayer::create(
            "Generation Failed",
            fmt::format("<cr>Error:</c>\n\n{}", error),
            "OK"
        )->show();
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
        
        log::info("Adding AI button to editor");
        
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
        m_fields->m_aiButton->setPosition({0, 0});
        
        auto winSize = CCDirector::get()->getWinSize();
        m_fields->m_aiMenu->setPosition({60, winSize.height - 30});
        m_fields->m_aiMenu->setZOrder(100);
        m_fields->m_aiMenu->setID("ai-generator-menu"_spr);
        
        this->addChild(m_fields->m_aiMenu);
        
        log::info("AI button added successfully");
    }
    
    void onAIButton(CCObject*) {
        log::info("AI button clicked");
        
        auto layer = this->m_editorLayer;
        if (!layer) {
            FLAlertLayer::create("Error", "No editor layer found!", "OK")->show();
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
    log::info("       Editor AI v2.1.0 Loaded");
    log::info("========================================");
    log::info("Direct API Integration");
    log::info("Supported AI Providers:");
    log::info("  • Google Gemini");
    log::info("  • Anthropic Claude");
    log::info("  • OpenAI ChatGPT");
    log::info("----------------------------------------");
    log::info("Loaded {} object types", OBJECT_IDS.size());
    log::info("========================================");
}
