#include "links.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

using namespace geode::prelude;

template <>
struct matjson::Serialize<LevelLink> {
    static geode::Result<LevelLink> fromJson(Value const& data) {
        GEODE_UNWRAP_INTO(auto bob, data["aID"].as<int>());
        GEODE_UNWRAP_INTO(auto wpopoff, data["aName"].asString());
        GEODE_UNWRAP_INTO(auto netermind, data["bID"].as<int>());
        GEODE_UNWRAP_INTO(auto sushi, data["bName"].asString());
        auto aFingerprint = data["aFingerprint"].asString().unwrapOrDefault();
        auto bFingerprint = data["bFingerprint"].asString().unwrapOrDefault();

        return geode::Ok(LevelLink {
            LinkedLevel { bob, std::move(wpopoff), std::move(aFingerprint) },
            LinkedLevel { netermind, std::move(sushi), std::move(bFingerprint) },
        });
    }

    static Value toJson(LevelLink const& link) {
        auto data = Value::object();
        data["aID"] = link.a.id;
        data["aName"] = link.a.name;
        data["bID"] = link.b.id;
        data["bName"] = link.b.name;
        if (!link.a.fingerprint.empty()) data["aFingerprint"] = link.a.fingerprint;
        if (!link.b.fingerprint.empty()) data["bFingerprint"] = link.b.fingerprint;
        return data;
    }
};

namespace {
constexpr char linkfile[] = "links.json";

std::vector<LevelLink> spaceuk;
std::optional<LinkedLevel> Zonk;
std::map<int, geode::Ref<GJGameLevel>> g_seen;

std::filesystem::path savepath() {
    return Mod::get()->getSaveDir() / linkfile;
}

bool save() {
    auto data = matjson::Value::object();
    data["links"] = spaceuk;

    auto res = file::writeStringSafe(savepath(), data.dump(4));
    if (!res) {
        log::error("couldn't save Level Relay links: {}", res.unwrapErr());
        return false;
    }
    return true;
}


std::string localfingerprint(GJGameLevel* lvl) {
    if (!lvl || lvl->m_levelType != GJLevelType::Editor) return {};

    std::string_view leveldata = lvl->m_levelString;
    if (leveldata.empty()) return {};

    // fnv is stable between restarts unlike std::hash
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : leveldata) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return fmt::format("{:016x}", hash);
}

std::string levelname(GJGameLevel* lvl) {
    return lvl ? static_cast<std::string>(lvl->m_levelName) : std::string();
}

LinkedLevel makelinked(GJGameLevel* lvl) {
    return LinkedLevel {
        relay::levelKey(lvl),
        levelname(lvl),
        localfingerprint(lvl),
    };
}

bool samelocal(LinkedLevel const& saved, GJGameLevel* lvl) {
    if (!lvl || relay::levelKey(lvl) >= 0) return false;

    auto fingerprint = localfingerprint(lvl);
    if (!saved.fingerprint.empty() && !fingerprint.empty() &&
        saved.fingerprint == fingerprint) {
        return true;
    }

    return saved.name == levelname(lvl);
}

LinkedLevel* endpointbyid(int id, LinkedLevel* except = nullptr) {
    for (auto& link : spaceuk) {
        if (&link.a != except && link.a.id == id) return &link.a;
        if (&link.b != except && link.b.id == id) return &link.b;
    }
    return nullptr;
}

LinkedLevel* localendpoint(GJGameLevel* lvl, bool& ambiguous) {
    ambiguous = false;
    auto id = relay::levelKey(lvl);
    auto name = levelname(lvl);
    auto fingerprint = localfingerprint(lvl);

    std::vector<LinkedLevel*> fingerprints;
    std::vector<LinkedLevel*> names;
    for (auto& link : spaceuk) {
        for (auto saved : { &link.a, &link.b }) {
            if (saved->id >= 0) continue;
            if (!fingerprint.empty() && saved->fingerprint == fingerprint)
                fingerprints.push_back(saved);
            if (saved->name == name)
                names.push_back(saved);
        }
    }

    auto choose = [id](std::vector<LinkedLevel*> const& matches) -> LinkedLevel* {
        if (matches.size() == 1) return matches.front();
        for (auto saved : matches) {
            if (saved->id == id) return saved;
        }
        return nullptr;
    };

    if (auto saved = choose(fingerprints)) return saved;
    if (fingerprints.size() > 1) ambiguous = true;

    if (auto saved = choose(names)) return saved;
    if (names.size() > 1) ambiguous = true;
    return nullptr;
}

bool reconcilelocal(GJGameLevel* lvl) {
    int id = relay::levelKey(lvl);
    if (id >= 0) return false;

    bool ambiguous = false;
    auto saved = localendpoint(lvl, ambiguous);
    if (!saved) {
        if (ambiguous) {
            log::warn("multiple saved local levels match '{}'; not guessing", lvl->m_levelName);
        }
        return false;
    }

    bool changed = false;
    if (saved->id != id) {
        // local level ids can swap after restarting, so swap the saved ids too
        if (auto occupied = endpointbyid(id, saved)) {
            occupied->id = saved->id;
        }
        log::info("fixed local relay id {} -> {} for '{}'", saved->id, id, lvl->m_levelName);
        saved->id = id;
        changed = true;
    }

    auto name = levelname(lvl);
    auto fingerprint = localfingerprint(lvl);
    if (saved->name != name) {
        saved->name = std::move(name);
        changed = true;
    }
    if (!fingerprint.empty() && saved->fingerprint != fingerprint) {
        saved->fingerprint = std::move(fingerprint);
        changed = true;
    }
    return changed;
}

bool reconcilelocals() {
    auto mgr = LocalLevelManager::sharedState();
    auto local = mgr ? mgr->m_localLevels : nullptr;
    if (!local) return false;

    bool changed = false;
    for (auto lvl : CCArrayExt<GJGameLevel*>(local)) {
        if (lvl) changed |= reconcilelocal(lvl);
    }
    return changed;
}

GJGameLevel* localbyid(LinkedLevel const& saved) {
    auto mgr = LocalLevelManager::sharedState();
    auto local = mgr ? mgr->m_localLevels : nullptr;
    if (!local) return nullptr;

    for (auto lvl : CCArrayExt<GJGameLevel*>(local)) {
        if (lvl && relay::levelKey(lvl) == saved.id && samelocal(saved, lvl)) return lvl;
    }
    return nullptr;
}

// copied editor lvls can come back with a diff local id
GJGameLevel* localbyidentity(LinkedLevel const& saved, bool& bbSUCKS) {
    bbSUCKS = false;

    auto mgr = LocalLevelManager::sharedState();
    auto local = mgr ? mgr->m_localLevels : nullptr;
    if (!local) return nullptr;

    std::vector<GJGameLevel*> fingerprints;
    std::vector<GJGameLevel*> names;
    for (auto lvl : CCArrayExt<GJGameLevel*>(local)) {
        if (!lvl) continue;
        if (!saved.fingerprint.empty() && localfingerprint(lvl) == saved.fingerprint)
            fingerprints.push_back(lvl);
        if (lvl->m_levelName == saved.name)
            names.push_back(lvl);
    }

    if (fingerprints.size() == 1) return fingerprints.front();
    if (names.size() == 1) return names.front();

    if (fingerprints.size() > 1 || names.size() > 1) {
        bbSUCKS = true;
        log::warn("multiple local levels match '{}'; not guessing", saved.name);
    }
    return nullptr;
}

GJGameLevel* cachedonline(int id) {
    auto gm = GameLevelManager::sharedState();
    if (!gm) return nullptr;

    if (auto saved = gm->getSavedLevel(id); saved && !saved->m_levelString.empty())
        return saved;

    // getSavedLevel misses some cached downloads, so check gd's maps too
    auto key = std::to_string(id);
    if (gm->m_downloadedLevels) {
        if (auto obj = gm->m_downloadedLevels->objectForKey(key)) {
            auto lvl = static_cast<GJGameLevel*>(obj);
            if (!lvl->m_levelString.empty()) return lvl;
        }
    }
    if (gm->m_onlineLevels) {
        if (auto obj = gm->m_onlineLevels->objectForKey(key)) {
            auto lvl = static_cast<GJGameLevel*>(obj);
            if (!lvl->m_levelString.empty()) return lvl;
        }
    }
    return nullptr;
}
}

LinkedLevel const* LevelLink::other(int id) const {
    if (a.id == id) return &b;
    if (b.id == id) return &a;
    return nullptr;
}

int relay::levelKey(GJGameLevel* lvl) {
    if (!lvl) return 0;

    if (lvl->m_levelType == GJLevelType::Editor && lvl->m_M_ID > 0)
        return -lvl->m_M_ID;

    if (lvl->m_levelID > 0) return lvl->m_levelID;
    return 0;
}

void relay::seen(GJGameLevel* lvl) {
    int id = levelKey(lvl);
    if (!id) return;

    if (id < 0 && reconcilelocal(lvl)) save();
    g_seen[id] = geode::Ref<GJGameLevel>(lvl);
    log::debug("relay saw '{}' as {}", lvl->m_levelName, id);
}

GJGameLevel* relay::trysaved(LinkedLevel const& saved, bool& bbSUCKS) {
    bbSUCKS = false;

    // use anything gd already has before starting another download

    if (auto it = g_seen.find(saved.id); it != g_seen.end()) {
        auto lvl = it->second.data();
        if (saved.id > 0 || samelocal(saved, lvl)) return lvl;
    }

    if (auto lvl = localbyid(saved)) {
        return lvl;
    }

    if (saved.id < 0) {
        if (auto lvl = localbyidentity(saved, bbSUCKS)) {
            if (reconcilelocal(lvl)) save();
            return lvl;
        }
        return nullptr;
    }

    return cachedonline(saved.id);
}

LevelLink* relay::linkFor(int id) {
    GJGameLevel* seen = nullptr;
    if (id < 0) {
        if (auto it = g_seen.find(id); it != g_seen.end()) seen = it->second.data();
    }

    for (auto& link : spaceuk) {
        if (link.a.id == id && (!seen || samelocal(link.a, seen))) return &link;
        if (link.b.id == id && (!seen || samelocal(link.b, seen))) return &link;
    }
    return nullptr;
}

LinkedLevel const* relay::firstpick() {
    return Zonk ? &*Zonk : nullptr;
}

void relay::unlinkLevel(int id) {
    auto before = spaceuk.size();
    std::erase_if(spaceuk, [id](LevelLink const& link) {
        return link.has(id);
    });

    if (spaceuk.size() != before) save();
}

void relay::clearpick() {
    Zonk.reset();
}

void relay::pairlvl(GJGameLevel* lvl) {
    int v69 = levelKey(lvl);
    if (!v69) {
        Notification::create("Couldn't get a level ID.", NotificationIcon::Error, 3.f)->show();
        return;
    }

    seen(lvl);
    LinkedLevel tmp = makelinked(lvl);

    if (!Zonk) {
        Zonk = tmp;
        Notification::create("Saved first level. Open another one to finish linking.", NotificationIcon::Info, 3.f)->show();
        return;
    }

    if (Zonk->id == v69) {
        FLAlertLayer::create("Link Error", "Pick a different level.", "OK")->show();
        clearpick();
        return;
    }

    auto first = *Zonk;

    // one relay per lvl cause chains get annoying fast
    auto before = spaceuk.size();
    std::erase_if(spaceuk, [&](LevelLink const& link) {
        return link.has(first.id) || link.has(v69);
    });
    bool changed = spaceuk.size() != before;

    spaceuk.push_back({ first, tmp });
    clearpick();
    save();

    auto& added = spaceuk.back();
    auto msg = changed
        ? fmt::format("Replaced old link with '{}' <-> '{}'.", added.a.name, added.b.name)
        : fmt::format("Linked '{}' <-> '{}'.", added.a.name, added.b.name);
    Notification::create(msg, NotificationIcon::Success, 3.5f)->show();
}

void relay::loadSaved() {
    spaceuk.clear();

    if (!std::filesystem::exists(savepath())) return;

    auto data = file::readJson(savepath());
    if (!data) {
        // dont wipe links just because the json got messed with
        log::error("couldn't read Level Relay links: {}", data.unwrapErr());
        return;
    }

    auto json = std::move(data).unwrap();
    auto res = json["links"].as<std::vector<LevelLink>>();
    if (!res) {
        // same deal if the links array is busted
        log::error("couldn't read Level Relay links: {}", res.unwrapErr());
        return;
    }

    auto old = std::move(res).unwrap();
    size_t bad = 0;

    for (auto const& link : old) {
        if (!link.a.id || !link.b.id || link.a.id == link.b.id) {
            ++bad;
            continue;
        }

        if (linkFor(link.a.id) || linkFor(link.b.id)) {
            ++bad;
            continue;
        }

        spaceuk.push_back(link);
    }

    if (reconcilelocals()) save();

    if (bad) {
        log::warn("skipped {} saved relay link(s)", bad);
        save();
    }
}
