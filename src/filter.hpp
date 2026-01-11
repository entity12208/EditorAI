#pragma once
#include <climits>
#include <Geode/binding/CCTextInputNode.hpp>

class $modify(CCTextInputNode) {
public:
    bool init(float width, float height, char const* placeholder, char const* textFont, int fontSize, char const* labelFont) {
        if (!CCTextInputNode::init(width, height, placeholder, textFont, fontSize, labelFont))
            return false;
        this->setMaxLabelLength(INT_MAX);
        this->setAllowedChars(gd::string());
        return true;
    }

    void setMaxLabelLength(int v) {
        CCTextInputNode::setMaxLabelLength(INT_MAX);
    }

    bool onTextFieldInsertText(cocos2d::CCTextFieldTTF* pSender, char const* text, int nLen, cocos2d::enumKeyCodes keyCodes) {
        if (text && nLen > 0) {
            try {
                auto cur = this->getString();
                cur.append(text, static_cast<size_t>(nLen));
                this->setString(cur);
                return true;
            } catch (...) {}
        }
        return CCTextInputNode::onTextFieldInsertText(pSender, text, nLen, keyCodes);
    }
};
