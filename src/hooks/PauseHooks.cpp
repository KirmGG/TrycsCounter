// Hooks of the pause menu (TC button) and the end screen.

#include "../App.hpp"
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>

using namespace tc;

class $modify(TCPauseLayer, PauseLayer) {
    // Size of the icon when there is no other button to match.
    static constexpr float kButtonSize = 28.f;

    void customSetup() {
        PauseLayer::customSetup();
        overlay::setCountingScreen(PlayLayer::get(), false);

        auto* sprite = CCSprite::create("tc-button.png"_spr);
        if (!sprite) return;
        auto* menu = typeinfo_cast<CCMenu*>(this->getChildByID("right-button-menu"));
        // As wide as the other buttons of the column.
        float width = kButtonSize;
        if (menu) {
            for (auto* item : CCArrayExt<CCNode*>(menu->getChildren())) {
                if (typeinfo_cast<CCMenuItemSpriteExtra*>(item) && item->getContentWidth() > 0.f) {
                    width = item->getContentWidth();
                    break;
                }
            }
        }
        sprite->setScale(width / std::max(1.f, sprite->getContentWidth()));
        // Reddish while the frame windows are counted.
        if (Session::get().isAnalyzing()) sprite->setColor({255, 140, 140});
        auto* button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(TCPauseLayer::onMenu));
        button->setID("menu-button"_spr);

        // The right-hand button column of the pause menu (node IDs), or our own.
        if (menu) {
            menu->addChild(button);
            menu->updateLayout();
            return;
        }
        auto const size = CCDirector::sharedDirector()->getWinSize();
        menu = CCMenu::create();
        menu->setID("pause-menu"_spr);
        menu->addChild(button);
        menu->setPosition({size.width - 36.f, size.height / 2.f});
        this->addChild(menu, 20);
    }

    void onMenu(CCObject*) {
        openMenu();
    }
};

class $modify(TCEndLayer, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();

        auto* menu = CCMenu::create();
        menu->setID("end-menu"_spr);
        auto* button = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Count Frame Windows", "goldFont.fnt", "GJ_button_01.png", 0.5f),
            this, menu_selector(TCEndLayer::onCountFrameWindows));
        menu->addChild(button);

        auto const size = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({size.width / 2.f, 24.f});
        m_mainLayer->addChild(menu, 20);
    }

    void onCountFrameWindows(CCObject*) {
        auto& s = Session::get();
        auto* play = m_playLayer;
        if (!play) return;

        if (s.previousCompletion.empty()) {
            FLAlertLayer::create("TrycsCounter",
                "No recorded completion. Pause, open TC and press Start Recording, then beat the level in normal mode.", "OK")->show();
            return;
        }

        // Same as the Retry button.
        s.pendingAnalysis = s.previousCompletion;
        this->onReplay(nullptr);
    }
};
