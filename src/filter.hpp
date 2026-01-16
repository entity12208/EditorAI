#pragma once
#include <limits>
#include <string>
#include <Geode/Geode.hpp>
#include <Geode/modify/CCTextInputNode.hpp>

class $modify(CCTextInputNode) {
public:
    bool init(float width, float height, const char* placeholder, const char* textFont, int fontSize, const char* labelFont) {
        if (!CCTextInputNode::init(width, height, placeholder, textFont, fontSize, labelFont))
            return false;
        CCTextInputNode::setMaxLabelLength(std::numeric_limits<int>::max());
        return true;
    }

    bool onTextFieldInsertText(cocos2d::CCTextFieldTTF* pSender, const char* text, int nLen, cocos2d::enumKeyCodes keyCodes) {
        if (text && nLen > 0) {
            try {
                auto cur = this->getString();
                std::string s = cur.c_str();
                s.append(text, static_cast<size_t>(nLen));
                this->setString(s.c_str());
                return true;
            } catch (...) {}
        }
        return CCTextInputNode::onTextFieldInsertText(pSender, text, nLen, keyCodes);
    }
};
