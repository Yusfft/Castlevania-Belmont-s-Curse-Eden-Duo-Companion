// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's Equipment menu on the second screen: the menu scene (bc_menu) with what the game
// shows in it filled in - the equipped weapon's illustration, the arcana card, the relics, the
// stats - and its three inventories (weapons, relics, arcanas), driven by taps.
//
// The game's own display scripts are followed: each slot's display component names the image
// and texts it fills, and its UnityEvents (which objects it shows or hides) are run as the game
// runs them. Item data (names, descriptions, icons, illustrations) comes from the item assets
// in romfs, found by their itemID.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bc_levelmap.h"
#include "bc_menu.h"

namespace bc_menupage {

/// What one menu image shows. Everything the picture depends on is in here.
struct Snapshot {
    int view = 0;             // 0 equipment, 1 weapons, 2 relics, 3 arcanas
    int slot = -1;            // the relic slot picked (equipment view: its info box; relics: where to equip)
    std::string inspect;      // the item shown in an inventory's card
    int page = 0;             // relics page
    bool ok = false;          // live inventory read
    std::string weapon, spell;
    std::vector<std::string> charms; // per unlocked relic slot ("" = empty)
    std::vector<std::string> owned;  // item ids in the inventory
    std::map<std::string, float> stats;
    float level = 0, xp = 0, xp_max = 0;

    std::uint64_t Hash() const;
};

/// A tappable part of the picture (pixels, top-left origin).
struct Target {
    enum Kind {
        Tab,           // index: 0 map, 1 equipment, 2 tarot, 3 journey, 4 system
        OpenWeapons,
        OpenArcanas,
        OpenRelics,
        RelicSlot,     // index: slot
        Item,          // item: id (weapons, relics, arcanas lists)
        PrevPage,
        NextPage,
        Close,         // outside an inventory's panel
    } kind;
    int index = 0;
    std::string item;
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct ItemInfo {
    std::string id;
    int type = 0;
    std::int64_t name = 0, desc = 0, desc2 = 0, special = 0, whip = 0, tier_name = 0;
    std::string icon;                // sprite name (ui-items)
    std::string bundle;                          // the item data's bundle
    std::shared_ptr<bc_unity::Value> illustration; // weapon: PPtr in that bundle
    std::string character;          // arcana: "Carmilla" (CharacterID_<x>)
    int dps = 0;
    std::map<std::string, std::string> vars; // smart-string variables of its descriptions
    int mana_cost = 0, mana_cost_type = 0;    // arcana: spellManaCost, manaCostType (1 = all mana)
    struct Notion {
        std::int64_t desc = 0;
        std::string icon;
    };
    std::vector<Notion> notions;              // arcana: what its description's keywords mean
};

class Menu {
public:
    Menu(bc_levelmap::Assets& assets, const bc_levelmap::Codecs& codecs);

    /// Reads the menu scene and the item index (slow: seconds). Call off the sample thread,
    /// with the assets to yourself.
    bool Load(const bc_levelmap::Names& names, const bc_levelmap::Font& font, std::string* log);
    bool Ready() const { return scene_ != nullptr; }
    std::size_t SpriteBytes() const { return bank_.Bytes(); }

    /// Draws `s` at w x h. `targets` receives the tappable parts.
    std::vector<std::uint8_t> Render(const Snapshot& s, int w, int h, std::vector<Target>* targets);

    /// An item's display name ("" when unknown).
    std::string Name(const std::string& id) const;
    const ItemInfo* Info(const std::string& id) const;
    /// Every indexed item id, sorted.
    std::vector<std::string> Ids() const {
        std::vector<std::string> out;
        for (const auto& [id, it] : items_)
            out.push_back(id);
        return out;
    }

    /// The canvas the menu is laid out in: width matched (the game's horizontal layout).
    static constexpr float kMatch = 0.0f;

private:
    void Index(std::string* log);
    bc_menu::SpriteP Icon(const ItemInfo* it);
    bc_menu::SpriteP Illustration(const ItemInfo* it);
    bc_menu::SpriteP ArcanaArt(const ItemInfo* it, bool absorbed);
    static bc_menu::SpriteP Goldleaf(const bc_menu::SpriteP& in);
    void Display(bc_menu::Scene& sc, const bc_menu::Comp* display, const ItemInfo* it, const Snapshot& s);
    void Bind(bc_menu::Scene& sc, const Snapshot& s);
    std::string Str(std::int64_t key) const;
    std::string Str(std::int64_t key, const ItemInfo* it) const;

    bc_levelmap::Assets& assets_;
    bc_menu::SpriteBank bank_;
    std::shared_ptr<const bc_menu::Scene> scene_;
    const bc_levelmap::Names* names_ = nullptr;
    const bc_levelmap::Font* font_ = nullptr;
    std::map<std::string, ItemInfo> items_;
    std::map<std::string, std::shared_ptr<bc_levelmap::File>> characters_; // "Carmilla" -> display data file
    std::map<std::string, const bc_unity::ObjectInfo*> character_objs_;
};

} // namespace bc_menupage
