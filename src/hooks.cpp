#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/StartPosObject.hpp>
#include <Geode/binding/LevelDownloadDelegate.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include "links.hpp"

#include <cmath>
#include <limits>
#include <optional>

using namespace geode::prelude;

class $modify(RelayPauseLayer, PauseLayer) {
    struct Fields {
        int m_levelID = 0;
        std::string m_levelName;
    };

    void customSetup();

    void onPair(cocos2d::CCObject*);
    void onSwitch(cocos2d::CCObject*);

public:
    void refreshBtns();
};

class $modify(RelayPlayLayer, PlayLayer) {
    struct Fields {
        std::optional<float> m_lastDeathX;
        std::optional<float> m_relayDeathX;
        StartPosObject* m_sp = nullptr;
        float m_spX = 0.f;
        bool m_seenSP = false;
        int m_spCount = 0;
        bool m_spSet = false;
    };

    void checkSP(GameObject* obj);
    void scanSPs();
    void pickSP();

public:
    static void onModify(auto& self) {
        // mh touches sp late, so relay goes after it
        if (geode::Loader::get()->getInstalledMod("absolllute.megahack")) {
            if (!self.setHookPriorityAfterPost(
                "PlayLayer::setupHasCompleted", "absolllute.megahack"
            )) {
                geode::log::warn("couldn't put relay auto sp after Mega Hack");
            }
        }
    }

    bool init(GJGameLevel* lvl, bool replay, bool noObj);
    void addObject(GameObject* obj);
    void createObjectsFromSetupFinished();
    void setupHasCompleted();
    void destroyPlayer(PlayerObject* p, GameObject* obj);

    std::optional<float> lastDeathX();
};

namespace relay {
    void reqswitch();
    void reqswitch(RelayPauseLayer* pause);
}

namespace {
std::optional<float> g_nextRelayDeathX;

void switchtolvl(GJGameLevel* level, std::optional<float> deathX, RelayPauseLayer* pause) {
    auto scene = CCScene::create();

    // PlayLayer::scene can crash while the old layer is still leaving
    scene->runAction(CCSequence::create(
        CCDelayTime::create(0.05f),
        CallFuncExt::create([level = geode::Ref<GJGameLevel>(level), deathX] {
            g_nextRelayDeathX = deathX;
            auto ps = PlayLayer::scene(level.data(), false, false);
            g_nextRelayDeathX.reset();

            if (!ps) {
                log::error("couldn't build PlayLayer for '{}'", level->m_levelName);
                return;
            }

            CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, ps));
        }),
        nullptr
    ));

    if (pause) pause->onResume(nullptr);
    CCDirector::sharedDirector()->replaceScene(scene);
}

std::string cutname(std::string const& name, size_t maxLen = 18) {
    if (name.size() <= maxLen) return name;
    if (maxLen <= 3) return "...";
    return name.substr(0, maxLen - 3) + "...";
}

class relaydl;
relaydl* g_download = nullptr;
relaydl* dropdl();

class relaydl : public CCNode, public LevelDownloadDelegate {
    LinkedLevel m_target;
    RelayPauseLayer* m_pause = nullptr;
    std::optional<float> m_deathX;
    LevelDownloadDelegate* m_old = nullptr;
public:
    static relaydl* create(
        LinkedLevel const& target,
        std::optional<float> deathX,
        RelayPauseLayer* pause
    ) {
        auto ret = new relaydl();
        if (ret->init()) {
            ret->m_target = target;
            ret->m_deathX = deathX;
            ret->m_pause = pause;
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    ~relaydl() override {
        restore();
    }

    void start() {
        auto gm = GameLevelManager::sharedState();
        if (!gm) {
            auto hold = dropdl();
            log::warn("can't download relay target {}; no GameLevelManager", m_target.id);
            if (hold) hold->release();
            return;
        }

        Notification::create(
            fmt::format("Loading {}...", cutname(m_target.name, 24)),
            NotificationIcon::Info, 2.f
        )->show();

        // gd only has one level download delegate, put the old one back when we're done
        m_old = gm->m_levelDownloadDelegate;
        gm->m_levelDownloadDelegate = this;
        gm->downloadLevel(m_target.id, false, 0);
    }

    void restore() {
        auto gm = GameLevelManager::sharedState();
        if (gm && gm->m_levelDownloadDelegate == this) {
            gm->m_levelDownloadDelegate = m_old;
        }
        m_old = nullptr;
    }

    void levelDownloadFinished(GJGameLevel* lvl) override {
        restore();

        if (!lvl) {
            auto hold = dropdl();
            log::warn("relay download came back null");
            if (hold) hold->release();
            return;
        }

        relay::seen(lvl);

        auto hold = dropdl();
        log::info("downloaded relay target '{}' ({})", lvl->m_levelName, relay::levelKey(lvl));
        switchtolvl(lvl, m_deathX, m_pause);
        if (hold) hold->release();
    }

    void levelDownloadFailed(int res) override {
        restore();
        log::warn("couldn't download relay target {} ({})", m_target.id, res);

        auto hold = dropdl();
        auto msg = fmt::format("Couldn't download {}.", m_target.name);
        FLAlertLayer::create("Can't Load Level", msg.c_str(), "OK")->show();
        if (hold) hold->release();
    }
};

relaydl* dropdl() {
    auto ret = g_download;
    if (!ret) return nullptr;

    // keep it alive until the download callback is completely done
    ret->retain();
    g_download = nullptr;
    ret->release();
    return ret;
}

}

void RelayPauseLayer::customSetup() {
    PauseLayer::customSetup();

    auto pl = PlayLayer::get();
    if (!pl || !pl->m_level) return;

    this->m_fields->m_levelID = relay::levelKey(pl->m_level);
    this->m_fields->m_levelName = pl->m_level->m_levelName;

    if (!this->m_fields->m_levelID) return;

    relay::seen(pl->m_level);
    refreshBtns();
}

// pause gets rebuilt, so rebuild relay buttons too
void RelayPauseLayer::refreshBtns() {
    if (auto old = this->getChildByID("relay-menu"_spr))
        old->removeFromParent();

    int id = this->m_fields->m_levelID;
    auto link = relay::linkFor(id);
    auto first = relay::firstpick();
    bool cancelling = first && first->id == id;
    bool legacy = Mod::get()->getSettingValue<bool>("legacyButtons");

    auto mkbtn = [this](
        std::string const& text,
        char const* bg,
        cocos2d::SEL_MenuHandler cb
    ) {
        auto spr = ButtonSprite::create(
            text.c_str(), 0, 85, 0.42f, false,
            "bigFont.fnt", bg, 28.f
        );
        spr->setScale(0.85f);
        return CCMenuItemSpriteExtra::create(spr, this, cb);
    };

    auto mkicon = [this](
        char const* icon,
        CircleBaseColor color,
        cocos2d::SEL_MenuHandler cb,
        float scale = 1.2f
    ) {
        auto spr = CircleButtonSprite::createWithSprite(
            icon, scale, color, CircleBaseSize::Small
        );
        return CCMenuItemSpriteExtra::create(spr, this, cb);
    };

    std::string txt = "Link Level";
    if (link) {
        auto const& other = *link->other(id);
        txt = "Unlink (" + cutname(other.name) + ")";
    }
    else if (cancelling) {
        txt = "Cancel Link";
    }
    else if (first) {
        txt = "Link -> " + cutname(first->name);
    }

    auto menu = CCMenu::create();
    menu->setID("relay-menu"_spr);
    menu->setPosition(CCPointZero);
    addChild(menu, 10);

    CCMenuItemSpriteExtra* btn;
    if (!legacy) {
        auto icon = link || cancelling
            ? "relay-unlink.png"_spr
            : "relay-link.png"_spr;
        auto color = link || cancelling
            ? CircleBaseColor::Red
            : CircleBaseColor::Green;
        btn = mkicon(
            icon, color, menu_selector(RelayPauseLayer::onPair),
            link || cancelling ? 1.5f : 1.2f
        );
    } else {
        btn = mkbtn(txt, "GJ_button_05.png", menu_selector(RelayPauseLayer::onPair));
    }
    btn->setID("pairBtn"_spr);
    float w = btn->getScaledContentSize().width / 2.f;
    btn->setPosition({ 15.f + w, legacy ? 20.f : 25.f });
    menu->addChild(btn);

    if (!link) return;

    auto const& other = *link->other(id);
    auto name = "Switch -> " + cutname(other.name);

    auto switchbtn = legacy
        ? mkbtn(
            name,
            "GJ_button_02.png",
            menu_selector(RelayPauseLayer::onSwitch)
        )
        : mkicon(
            "relay-switch.png"_spr,
            CircleBaseColor::Blue,
            menu_selector(RelayPauseLayer::onSwitch)
        );
    switchbtn->setID("switchBtn"_spr);
    auto win = CCDirector::sharedDirector()->getWinSize();
    float ww = switchbtn->getScaledContentSize().width / 2.f;
    // 16:9 spot rn, smaller screens might need another spot
    switchbtn->setPosition({ win.width - 15.f - ww, legacy ? 20.f : 25.f });
    menu->addChild(switchbtn);

    if (!legacy) return;

    auto label = CCLabelBMFont::create(
        fmt::format("Linked to: {}", cutname(other.name, 32)).c_str(),
        "chatFont.fnt"
    );
    label->setID("linkedLabel"_spr);
    label->setScale(0.42f);
    label->setColor({ 200, 255, 200 });
    label->setAnchorPoint({ 0.f, 0.5f });
    label->setPosition({ 18.f, 45.f });
    menu->addChild(label);
}

void RelayPauseLayer::onPair(CCObject*) {
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_level) return;

    int id = this->m_fields->m_levelID;

    if (auto pair = relay::linkFor(id)) {
        auto const& other = *pair->other(id);
        geode::createQuickPopup(
            "Unlink Level",
            fmt::format("Unlink {} and {}?", this->m_fields->m_levelName, other.name),
            "Cancel", "Unlink",
            [this, id](auto*, bool yes) {
                if (!yes) return;
                relay::unlinkLevel(id);
                Notification::create("Link removed.", NotificationIcon::None, 2.f)->show();
                this->refreshBtns();
            }
        );
        return;
    }

    auto first = relay::firstpick();
    if (first && first->id == id) {
        relay::clearpick();
        Notification::create("Cancelled link.", NotificationIcon::None, 2.f)->show();
    }
    else {
        relay::pairlvl(pl->m_level);
    }
    refreshBtns();
}

void RelayPauseLayer::onSwitch(CCObject*) {
    relay::reqswitch(this);
}

void relay::reqswitch() {
    reqswitch(nullptr);
}

void relay::reqswitch(RelayPauseLayer* pause) {
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_level) return;

    int id = relay::levelKey(pl->m_level);
    auto link = relay::linkFor(id);
    if (!link) {
        Notification::create("This level isn't linked.", NotificationIcon::Error, 2.f)->show();
        if (pause) pause->refreshBtns();
        return;
    }

    LinkedLevel other = *link->other(id);

    auto doswitch = [pause, other] {
        log::debug("relay switch -> '{}' ({})", other.name, other.id);

        std::optional<float> deathX;
        auto pl = PlayLayer::get();
        if (pl && Mod::get()->getSettingValue<bool>("autoSPSelect")) {
            deathX = static_cast<RelayPlayLayer*>(pl)->lastDeathX();
        }

        bool dupe = false;
        auto lvl = relay::trysaved(other, dupe);

        if (!lvl) {
            // local levels have to exist already, online ones can use the download path
            if (other.id > 0) {
                log::info("relay target {} is online and not cached yet", other.id);

                if (g_download) {
                    Notification::create("Already loading a linked level.", NotificationIcon::Info, 2.f)->show();
                    return;
                }

                auto dl = relaydl::create(other, deathX, pause);
                if (!dl) {
                    log::warn("couldn't start relay download for {}", other.id);
                    return;
                }

                g_download = dl;
                dl->retain();
                dl->start();
                return;
            }

            auto msg = dupe
                ? fmt::format("There are multiple local levels named {}.", other.name)
                : fmt::format("Can't find {}.", other.name);

            geode::createQuickPopup(
                "Can't Find Level", msg + "\n\nRemove link?", "Keep Link", "Remove Link",
                [pause, id = other.id](auto*, bool remove) {
                    if (!remove) return;
                    relay::unlinkLevel(id);
                    if (pause) pause->refreshBtns();
                }
            );
            return;
        }

        relay::seen(lvl);
        switchtolvl(lvl, deathX, pause);
    };

    if (!Mod::get()->getSettingValue<bool>("confirmSwitch")) {
        doswitch();
        return;
    }

    auto scene = CCDirector::get()->getRunningScene();
    if (scene && scene->getChildByIDRecursive("relay-switch-confirm"_spr)) return;

    auto popup = geode::createQuickPopup(
        "Switch Level",
        fmt::format("Switch to {}?\nYour current run will end.", other.name),
        "Cancel", "Switch",
        [doswitch](auto*, bool yes) mutable {
            if (yes) doswitch();
        }
    );
    popup->setID("relay-switch-confirm"_spr);
}

bool RelayPlayLayer::init(GJGameLevel* lvl, bool replay, bool noObj) {
    this->m_fields->m_relayDeathX = g_nextRelayDeathX;
    this->m_fields->m_sp = nullptr;
    this->m_fields->m_spX = -std::numeric_limits<float>::infinity();
    this->m_fields->m_seenSP = false;
    this->m_fields->m_spCount = 0;
    this->m_fields->m_spSet = false;

    if (!PlayLayer::init(lvl, replay, noObj)) return false;

    // local sp copies can already have objs when init returns
    if (this->m_fields->m_relayDeathX && this->m_objects) {
        for (auto obj : CCArrayExt<GameObject*>(this->m_objects)) {
            checkSP(obj);
        }
    }

    relay::seen(lvl);
    return true;
}

void RelayPlayLayer::addObject(GameObject* obj) {
    checkSP(obj);
    PlayLayer::addObject(obj);
}

void RelayPlayLayer::createObjectsFromSetupFinished() {
    PlayLayer::createObjectsFromSetupFinished();
    scanSPs();
    pickSP();
}

void RelayPlayLayer::checkSP(GameObject* obj) {
    auto deathX = this->m_fields->m_relayDeathX;
    if (!deathX || !obj) return;

    // 31 = startpos, disabled ones still matter in some lvls
    if (obj->m_objectID != 31) return;

    auto sp = static_cast<StartPosObject*>(obj);

    this->m_fields->m_seenSP = true;
    ++this->m_fields->m_spCount;

    float x = sp->getPositionX();
    // only use the closest sp that is actually before where we died
    if (x <= *deathX && x > this->m_fields->m_spX) {
        this->m_fields->m_sp = sp;
        this->m_fields->m_spX = x;
    }
}

void RelayPlayLayer::scanSPs() {
    if (!this->m_fields->m_relayDeathX || !this->m_objects) return;

    this->m_fields->m_sp = nullptr;
    this->m_fields->m_spX = -std::numeric_limits<float>::infinity();
    this->m_fields->m_seenSP = false;
    this->m_fields->m_spCount = 0;

    for (auto obj : CCArrayExt<GameObject*>(this->m_objects)) {
        checkSP(obj);
    }
}


void RelayPlayLayer::pickSP() {
    auto deathX = this->m_fields->m_relayDeathX;
    if (!deathX || this->m_fields->m_spSet) return;

    this->m_fields->m_spSet = true;

    auto sp = this->m_fields->m_sp;
    this->setStartPosObject(sp);
    this->m_currentCheckpoint = nullptr;
    this->m_isTestMode = sp != nullptr;
    this->updateTestModeLabel();

    if (sp) {
        log::info(
            "auto sp: death x {:.1f}, picked x {:.1f} ({} startpos)",
            *deathX, sp->getPositionX(), this->m_fields->m_spCount
        );
    } else if (this->m_fields->m_seenSP) {
        log::info(
            "auto sp: no startpos before x {:.1f}, using 0 ({} startpos)",
            *deathX, this->m_fields->m_spCount
        );
    } else {
        log::info("auto sp: no startpos objects, using 0");
    }
}

void RelayPlayLayer::setupHasCompleted() {
    PlayLayer::setupHasCompleted();

    if (!this->m_fields->m_relayDeathX) return;

    scanSPs();
    pickSP();

    auto force = [this] {
        auto sp = this->m_fields->m_sp;
        this->setStartPosObject(sp);
        this->m_currentCheckpoint = nullptr;
        this->m_isTestMode = sp != nullptr;
        this->updateTestModeLabel();

        if (this->m_isPracticeMode) this->resetLevelFromStart();
        this->resetLevel();
        this->startMusic();
    };

    // gd can overwrite the sp during setup, so force it again
    this->runAction(CCSequence::create(
        CCDelayTime::create(0.f),
        CallFuncExt::create(force),
        CCDelayTime::create(0.05f),
        CallFuncExt::create(force),
        nullptr
    ));

    this->m_fields->m_relayDeathX.reset();
}

void RelayPlayLayer::destroyPlayer(PlayerObject* p, GameObject* obj) {
    bool wasDead = this->m_playerDied || (p && p->m_isDead);
    std::optional<float> deathX;

    if (p) {
        float x = p->getPositionX();
        if (std::isfinite(x)) deathX = x;
    }

    PlayLayer::destroyPlayer(p, obj);

    // noclip can call this while ur alive, so only keep real deaths
    bool died = this->m_playerDied || (p && p->m_isDead);
    if (!wasDead && died && deathX) {
        this->m_fields->m_lastDeathX = deathX;
        log::debug("auto sp: saved death x {:.1f}", *deathX);
    }
}

std::optional<float> RelayPlayLayer::lastDeathX() {
    return this->m_fields->m_lastDeathX;
}
