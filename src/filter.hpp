#pragma once
#include <limits>
#include <Geode/Geode.hpp>
#include <Geode/modify/CCTextInputNode.hpp>

class $modify(CharacterFilterCCTINHook, CCTextInputNode) {
public:
    void updateLabel(gd::string str) {
        this->setAllowedChars(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789!@#$%^&*()-=_+"
            "`~[]{}/?.>,<\\|;:'\""
            " "
        );
        this->setMaxLabelLength(std::numeric_limits<int>::max());
        CCTextInputNode::updateLabel(std::move(str));
    }
};
