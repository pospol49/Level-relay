#pragma once

#include <Geode/Geode.hpp>

// local editor lvls are negative, online ones keep the real gd id
struct LinkedLevel {
    int id = 0;
    std::string name;
    std::string fingerprint;
};

struct LevelLink {
    LinkedLevel a;
    LinkedLevel b;

    bool has(int id) const { return a.id == id || b.id == id; }
    LinkedLevel const* other(int id) const;
};

namespace relay {
    void loadSaved();
    int levelKey(GJGameLevel* lvl);
    void seen(GJGameLevel* lvl);
    GJGameLevel* trysaved(LinkedLevel const& saved, bool& bbSUCKS);

    LevelLink* linkFor(int levelID);
    LinkedLevel const* firstpick();
    void pairlvl(GJGameLevel* lvl);
    void clearpick();
    void unlinkLevel(int id);
}
