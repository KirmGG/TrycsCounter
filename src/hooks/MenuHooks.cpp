// The TC button in the main menu: opens the library of counted runs.

#include "../App.hpp"
#include <Geode/modify/MenuLayer.hpp>

using namespace tc;

class $modify(TCMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        auto* menu = this->getChildByID("bottom-menu");
        auto* sprite = CCSprite::create("tc-menu-button.png"_spr);
        if (!menu || !sprite) return true;
        // As high as the other buttons of the row (the sprite has their
        // layout: the same circle size and drop shadow).
        float size = 46.f;
        for (auto* item : CCArrayExt<CCNode*>(menu->getChildren())) {
            if (item->getContentHeight() > 0.f) {
                size = item->getContentHeight();
                break;
            }
        }
        sprite->setScale(size / std::max(1.f, sprite->getContentHeight()));
        auto* button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(TCMenuLayer::onLibrary));
        button->setID("library-button"_spr);
        menu->addChild(button);
        menu->updateLayout();
        return true;
    }

    void onLibrary(CCObject*) {
        openLibrary();
    }
};
