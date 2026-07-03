#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/StartPosObject.hpp>
#include <Geode/binding/LevelDownloadDelegate.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include "links.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

using namespace geode::prelude;

namespace {
uint64_t g_playGeneration = 0;
uint64_t g_switchToken = 0;
uint64_t g_switchPlayGeneration = 0;
int g_switchLevelID = 0;
bool g_switching = false;
void cancelplay(uint64_t generation);
void cancelotherdownload(uint64_t generation);
}

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
    int relayLevelID();
};

class $modify(RelayPlayLayer, PlayLayer) {
    struct Fields {
        std::optional<float> m_lastDeathX;
        std::optional<float> m_relayDeathX;
        uint64_t m_relayGeneration = 0;
        StartPosObject* m_sp = nullptr;
        float m_spX = 0.f;
        bool m_seenSP = false;
        int m_spCount = 0;
        bool m_spSet = false;

        ~Fields() {
            cancelplay(m_relayGeneration);
        }
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
    uint64_t relayGeneration();
};

namespace relay {
    void reqswitch();
}

namespace {
std::optional<float> g_nextRelayDeathX;

bool sameplay(uint64_t generation, int levelID) {
    auto pl = PlayLayer::get();
    return pl && pl->m_level &&
        geode::cast::modify_cast<RelayPlayLayer*>(pl)->relayGeneration() == generation &&
        relay::levelKey(pl->m_level) == levelID;
}

void finishswitch(uint64_t token) {
    if (token != g_switchToken) return;

    g_switching = false;
    g_switchPlayGeneration = 0;
    g_switchLevelID = 0;
}

bool currentswitch(uint64_t token, uint64_t generation, int levelID) {
    return g_switching && token == g_switchToken &&
        g_switchPlayGeneration == generation && g_switchLevelID == levelID &&
        sameplay(generation, levelID);
}

RelayPauseLayer* currentpause(uint64_t generation, int levelID) {
    if (!sameplay(generation, levelID)) return nullptr;

    auto director = CCDirector::get();
    auto scene = director ? director->getRunningScene() : nullptr;
    auto node = scene ? scene->getChildByIDRecursive("relay-menu"_spr) : nullptr;
    auto menu = typeinfo_cast<CCMenu*>(node);
    auto pause = menu ? typeinfo_cast<PauseLayer*>(menu->getParent()) : nullptr;
    if (!pause || pause->getChildByID("relay-menu"_spr) != menu) return nullptr;

    auto relayPause = geode::cast::modify_cast<RelayPauseLayer*>(pause);
    if (!relayPause || relayPause->relayLevelID() != levelID) return nullptr;
    return relayPause;
}

void refreshpause(uint64_t generation, int levelID) {
    if (auto pause = currentpause(generation, levelID)) pause->refreshBtns();
}

void switchtolvl(
    GJGameLevel* level,
    std::optional<float> deathX,
    uint64_t token,
    uint64_t generation,
    int levelID
) {
    if (!level) {
        log::warn("can't switch relay levels; target level is null");
        finishswitch(token);
        return;
    }
    if (!currentswitch(token, generation, levelID)) {
        log::warn("cancelled stale relay switch to '{}'", level->m_levelName);
        finishswitch(token);
        return;
    }

    auto director = CCDirector::get();
    if (!director || !director->getRunningScene()) {
        log::warn("can't switch relay levels; no running scene");
        finishswitch(token);
        return;
    }

    auto scene = CCScene::create();
    if (!scene) {
        log::error("couldn't build relay switch trampoline");
        finishswitch(token);
        return;
    }

    auto sceneRef = geode::Ref<CCScene>(scene);
    auto delay = CCDelayTime::create(0.05f);
    auto call = CallFuncExt::create([
        level = geode::Ref<GJGameLevel>(level), deathX, token, sceneRef
    ] {
        if (!g_switching || token != g_switchToken) return;

        auto director = CCDirector::get();
        if (!director || !director->getRunningScene() ||
            director->getRunningScene() != sceneRef.data()) {
            log::warn("cancelled stale relay switch trampoline");
            finishswitch(token);
            return;
        }
        if (!level.data()) {
            log::warn("can't switch relay levels; target level expired");
            finishswitch(token);
            return;
        }

        g_nextRelayDeathX = deathX;
        auto ps = PlayLayer::scene(level.data(), false, false);
        g_nextRelayDeathX.reset();

        if (!ps) {
            log::error("couldn't build PlayLayer for '{}'", level->m_levelName);
            finishswitch(token);
            return;
        }

        director = CCDirector::get();
        if (!g_switching || token != g_switchToken || !director ||
            director->getRunningScene() != sceneRef.data()) {
            log::warn("relay switch scene changed while building PlayLayer");
            finishswitch(token);
            return;
        }

        auto transition = CCTransitionFade::create(0.5f, ps);
        if (!transition) {
            log::warn("couldn't build relay switch fade; using PlayLayer directly");
        }

        finishswitch(token);
        director->replaceScene(transition ? transition : ps);
    });
    auto sequence = delay && call ? CCSequence::create(delay, call, nullptr) : nullptr;
    if (!sequence) {
        log::error("couldn't schedule relay switch trampoline");
        finishswitch(token);
        return;
    }

    // PlayLayer::scene can crash while the old layer is still leaving
    scene->runAction(sequence);

    if (!currentswitch(token, generation, levelID)) {
        finishswitch(token);
        return;
    }
    if (auto pause = currentpause(generation, levelID)) pause->onResume(nullptr);

    director = CCDirector::get();
    if (!director || !director->getRunningScene() ||
        !currentswitch(token, generation, levelID)) {
        log::warn("relay switch source changed before the trampoline");
        finishswitch(token);
        return;
    }
    director->replaceScene(scene);
}

std::string cutname(std::string const& name, size_t maxLen = 18) {
    if (name.size() <= maxLen) return name;
    if (maxLen <= 3) return "...";
    return name.substr(0, maxLen - 3) + "...";
}

class relaydl;
relaydl* g_download = nullptr;
relaydl* dropdl(relaydl* expected);

class relaydl : public CCNode, public LevelDownloadDelegate {
    LinkedLevel m_target;
    std::optional<float> m_deathX;
    uint64_t m_token = 0;
    uint64_t m_playGeneration = 0;
    int m_levelID = 0;
    LevelDownloadDelegate* m_old = nullptr;
    bool m_ownsDelegate = false;
public:
    static relaydl* create(
        LinkedLevel const& target,
        std::optional<float> deathX,
        uint64_t token,
        uint64_t playGeneration,
        int levelID
    ) {
        auto ret = new relaydl();
        if (ret->init()) {
            ret->m_target = target;
            ret->m_deathX = deathX;
            ret->m_token = token;
            ret->m_playGeneration = playGeneration;
            ret->m_levelID = levelID;
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
        if (!currentswitch(m_token, m_playGeneration, m_levelID)) {
            log::warn("cancelled stale relay download for {}", m_target.id);
            cancel();
            return;
        }

        auto gm = GameLevelManager::sharedState();
        if (!gm) {
            log::warn("can't download relay target {}; no GameLevelManager", m_target.id);
            cancel();
            return;
        }
        if (gm->m_levelDownloadDelegate) {
            log::warn("can't download relay target {}; another delegate is active", m_target.id);
            Notification::create(
                "Another level download is already active.", NotificationIcon::Error, 2.f
            )->show();
            cancel();
            return;
        }

        Notification::create(
            fmt::format("Loading {}...", cutname(m_target.name, 24)),
            NotificationIcon::Info, 2.f
        )->show();

        // only borrow gd's delegate slot while its empty
        m_old = gm->m_levelDownloadDelegate;
        gm->m_levelDownloadDelegate = this;
        m_ownsDelegate = true;

        // gd can finish a cached download before downloadLevel returns
        [[maybe_unused]] auto self = geode::Ref<relaydl>(this);
        gm->downloadLevel(m_target.id, false, 0);

        if (g_download == this && m_ownsDelegate &&
            gm->m_levelDownloadDelegate != this) {
            log::warn("relay download delegate changed while starting {}", m_target.id);
            cancel();
        }
    }

    void restore() {
        if (!m_ownsDelegate) return;
        m_ownsDelegate = false;

        auto old = m_old;
        m_old = nullptr;
        auto gm = GameLevelManager::sharedState();
        if (gm && gm->m_levelDownloadDelegate == this) {
            gm->m_levelDownloadDelegate = old;
        } else if (gm) {
            // another mod took the delegate, dont restore over it
            log::warn("relay download delegate changed before restore");
        }
    }

    void cancel() {
        [[maybe_unused]] auto self = geode::Ref<relaydl>(this);
        restore();
        auto hold = dropdl(this);
        finishswitch(m_token);
        if (hold) hold->release();
    }

    bool belongsTo(uint64_t generation) const {
        return m_playGeneration == generation;
    }

    bool ownsDelegate() const {
        auto gm = GameLevelManager::sharedState();
        return m_ownsDelegate && gm && gm->m_levelDownloadDelegate == this;
    }

    void levelDownloadFinished(GJGameLevel* lvl) override {
        restore();

        if (!lvl) {
            auto hold = dropdl(this);
            log::warn("relay download came back null");
            finishswitch(m_token);
            if (hold) hold->release();
            return;
        }

        auto hold = dropdl(this);
        if (!currentswitch(m_token, m_playGeneration, m_levelID)) {
            log::warn("ignored stale relay download for {}", m_target.id);
            finishswitch(m_token);
            if (hold) hold->release();
            return;
        }
        if (relay::levelKey(lvl) != m_target.id) {
            log::warn("ignored wrong relay download for {}", m_target.id);
            finishswitch(m_token);
            if (hold) hold->release();
            return;
        }

        relay::seen(lvl);
        log::info("downloaded relay target '{}' ({})", lvl->m_levelName, relay::levelKey(lvl));
        switchtolvl(lvl, m_deathX, m_token, m_playGeneration, m_levelID);
        if (hold) hold->release();
    }

    void levelDownloadFailed(int res) override {
        restore();
        log::warn("couldn't download relay target {} ({})", m_target.id, res);

        auto hold = dropdl(this);
        if (!currentswitch(m_token, m_playGeneration, m_levelID)) {
            log::warn("ignored stale relay download failure for {}", m_target.id);
            finishswitch(m_token);
            if (hold) hold->release();
            return;
        }

        finishswitch(m_token);
        auto msg = fmt::format("Couldn't download {}.", m_target.name);
        if (auto alert = FLAlertLayer::create("Can't Load Level", msg.c_str(), "OK")) {
            alert->show();
        } else {
            log::error("couldn't show relay download failure popup");
        }
        if (hold) hold->release();
    }
};

relaydl* dropdl(relaydl* expected) {
    auto ret = g_download;
    if (!ret || ret != expected) return nullptr;

    // keep it alive until the download callback is completely done
    ret->retain();
    g_download = nullptr;
    ret->release();
    return ret;
}

void cancelplay(uint64_t generation) {
    if (!g_download || !g_download->belongsTo(generation)) return;

    log::debug("cancelled relay download for old PlayLayer");
    g_download->cancel();
}

void cancelotherdownload(uint64_t generation) {
    if (!g_download || g_download->belongsTo(generation)) return;

    log::debug("cancelled relay download after PlayLayer changed");
    g_download->cancel();
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
    auto playGeneration = geode::cast::modify_cast<RelayPlayLayer*>(pl)->relayGeneration();

    if (auto pair = relay::linkFor(id)) {
        auto const& other = *pair->other(id);
        geode::createQuickPopup(
            "Unlink Level",
            fmt::format("Unlink {} and {}?", this->m_fields->m_levelName, other.name),
            "Cancel", "Unlink",
            [id, playGeneration](auto*, bool yes) {
                if (!yes || !sameplay(playGeneration, id)) return;
                relay::unlinkLevel(id);
                Notification::create("Link removed.", NotificationIcon::None, 2.f)->show();
                refreshpause(playGeneration, id);
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
    relay::reqswitch();
}

void relay::reqswitch() {
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_level) {
        log::warn("can't switch relay levels; no active PlayLayer");
        return;
    }

    int id = relay::levelKey(pl->m_level);
    auto playGeneration = geode::cast::modify_cast<RelayPlayLayer*>(pl)->relayGeneration();
    if (g_download && (!g_download->belongsTo(playGeneration) ||
        !g_download->ownsDelegate())) {
        log::debug("cleared stale relay download before switching");
        g_download->cancel();
    }

    auto link = relay::linkFor(id);
    if (!link) {
        Notification::create("This level isn't linked.", NotificationIcon::Error, 2.f)->show();
        refreshpause(playGeneration, id);
        return;
    }

    auto otherLevel = link->other(id);
    if (!otherLevel) {
        log::error("relay link for {} has no other level", id);
        return;
    }
    LinkedLevel other = *otherLevel;

    auto doswitch = [other, id, playGeneration] {
        if (!sameplay(playGeneration, id)) {
            log::warn("cancelled relay switch; the active PlayLayer changed");
            return;
        }

        if (g_switching) {
            if (g_switchPlayGeneration == playGeneration && g_switchLevelID == id) {
                Notification::create(
                    "Already switching linked levels.", NotificationIcon::Info, 2.f
                )->show();
                return;
            }

            // a switch from an old PlayLayer must not finish in this one
            ++g_switchToken;
            g_switching = false;
        }

        auto token = ++g_switchToken;
        g_switching = true;
        g_switchPlayGeneration = playGeneration;
        g_switchLevelID = id;

        log::debug("relay switch -> '{}' ({})", other.name, other.id);

        std::optional<float> deathX;
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level || !sameplay(playGeneration, id)) {
            log::warn("cancelled relay switch; the active PlayLayer changed");
            finishswitch(token);
            return;
        }
        if (Mod::get()->getSettingValue<bool>("autoSPSelect")) {
            deathX = geode::cast::modify_cast<RelayPlayLayer*>(pl)->lastDeathX();
        }

        bool dupe = false;
        auto lvl = relay::trysaved(other, dupe);

        if (!lvl) {
            // local levels have to exist already, online ones can use the download path
            if (other.id > 0) {
                log::info("relay target {} is online and not cached yet", other.id);

                if (g_download) {
                    Notification::create("Already loading a linked level.", NotificationIcon::Info, 2.f)->show();
                    finishswitch(token);
                    return;
                }

                auto dl = relaydl::create(other, deathX, token, playGeneration, id);
                if (!dl) {
                    log::warn("couldn't start relay download for {}", other.id);
                    finishswitch(token);
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

            finishswitch(token);
            geode::createQuickPopup(
                "Can't Find Level", msg + "\n\nRemove link?", "Keep Link", "Remove Link",
                [id = other.id, sourceID = id, playGeneration, token](auto*, bool remove) {
                    if (!remove || token != g_switchToken ||
                        !sameplay(playGeneration, sourceID)) return;
                    relay::unlinkLevel(id);
                    refreshpause(playGeneration, sourceID);
                }
            );
            return;
        }

        relay::seen(lvl);
        switchtolvl(lvl, deathX, token, playGeneration, id);
    };

    if (!Mod::get()->getSettingValue<bool>("confirmSwitch")) {
        doswitch();
        return;
    }

    auto director = CCDirector::get();
    auto scene = director ? director->getRunningScene() : nullptr;
    if (!scene) {
        log::warn("can't show relay switch confirmation; no running scene");
        return;
    }
    if (scene->getChildByIDRecursive("relay-switch-confirm"_spr)) return;

    auto popup = geode::createQuickPopup(
        "Switch Level",
        fmt::format("Switch to {}?\nYour current run will end.", other.name),
        "Cancel", "Switch",
        [doswitch](auto*, bool yes) mutable {
            if (yes) doswitch();
        }
    );
    if (!popup) {
        log::error("couldn't build relay switch confirmation");
        return;
    }
    popup->setID("relay-switch-confirm"_spr);
}

int RelayPauseLayer::relayLevelID() {
    return this->m_fields->m_levelID;
}

bool RelayPlayLayer::init(GJGameLevel* lvl, bool replay, bool noObj) {
    this->m_fields->m_relayGeneration = ++g_playGeneration;
    this->m_fields->m_relayDeathX = g_nextRelayDeathX;
    this->m_fields->m_sp = nullptr;
    this->m_fields->m_spX = -std::numeric_limits<float>::infinity();
    this->m_fields->m_seenSP = false;
    this->m_fields->m_spCount = 0;
    this->m_fields->m_spSet = false;

    if (!PlayLayer::init(lvl, replay, noObj)) return false;

    cancelotherdownload(this->m_fields->m_relayGeneration);

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

uint64_t RelayPlayLayer::relayGeneration() {
    return this->m_fields->m_relayGeneration;
}
