// SPDX-License-Identifier: GPL-3.0-or-later
// The game's Equipment menu with live data. See bc_menupage.h.

#include "bc_menupage.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>

namespace bc_menupage {
namespace {

using bc_menu::Comp;
using bc_menu::Node;
using bc_menu::Scene;
using bc_unity::Value;

constexpr int kClassMonoBehaviour = 114;

std::int64_t Int(const Value* v, std::int64_t fb = 0) {
    return v ? v->AsInt(fb) : fb;
}
const std::string& StrV(const Value* v) {
    static const std::string empty;
    return v && v->kind == Value::Kind::String ? v->s : empty;
}
std::int64_t KeyOf(const Value* localized) {
    const Value* e = localized ? localized->Get("m_TableEntryReference") : nullptr;
    return Int(e ? e->Get("m_KeyId") : nullptr);
}
bool HasPPtr(const Value* v) {
    return v && Int(v->Get("m_PathID")) != 0;
}

std::uint64_t Fnv(std::uint64_t h, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}
std::uint64_t Fnv(std::uint64_t h, const std::string& s) {
    h = Fnv(h, s.data(), s.size());
    return Fnv(h, "\0", 1);
}

const std::vector<std::string> kIconPrefixes = {"ui-items", "ui-"};

void SetText(Scene& sc, int node, const std::string& text) {
    if (node < 0)
        return;
    for (auto& t : sc.nodes[static_cast<std::size_t>(node)].texts)
        t.text = text;
}

/// The class of a [SerializeReference] field of `obj` ("" when unset).
std::string RefClass(const Value& obj, const Value* field) {
    const std::int64_t rid = Int(field ? field->Get("rid") : nullptr, -2);
    const Value* refs = obj.Get("references");
    const Value* ids = refs ? refs->Get("RefIds") : nullptr;
    for (const auto& e : ids ? ids->list : std::vector<Value>{}) {
        if (Int(e.Get("rid")) != rid)
            continue;
        const Value* type = e.Get("type");
        return type ? StrV(type->Get("class")) : std::string{};
    }
    return {};
}

/// A LocalizedString's local variables (Unity Localization persistent variables), as text.
void LocalVars(const Value& obj, const Value* localized, std::map<std::string, std::string>& out) {
    const Value* lv = localized ? localized->Get("m_LocalVariables") : nullptr;
    const Value* refs = obj.Get("references");
    const Value* ids = refs ? refs->Get("RefIds") : nullptr;
    for (const auto& e : lv ? lv->list : std::vector<Value>{}) {
        const std::string name = StrV(e.Get("name"));
        const Value* var = e.Get("variable");
        const std::int64_t rid = Int(var ? var->Get("rid") : nullptr, -2);
        for (const auto& r : ids ? ids->list : std::vector<Value>{}) {
            if (Int(r.Get("rid")) != rid)
                continue;
            const Value* data = r.Get("data");
            const Value* v = data ? data->Get("m_Value") : nullptr;
            if (!v)
                break;
            char b[64];
            if (v->kind == Value::Kind::Float) {
                std::snprintf(b, sizeof b, "%g", v->f);
                out[name] = b;
            } else if (v->kind == Value::Kind::String) {
                out[name] = v->s;
            } else {
                std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v->AsInt()));
                out[name] = b;
            }
            break;
        }
    }
}

/// Unity Localization smart strings, the simple form the item texts use: "{name}" and
/// "{name:format}" become the variable's value.
std::string Smart(const std::string& s, const std::map<std::string, std::string>& vars) {
    if (vars.empty() || s.find('{') == std::string::npos)
        return s;
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '{') {
            const std::size_t end = s.find('}', i);
            if (end != std::string::npos) {
                std::string name = s.substr(i + 1, end - i - 1);
                if (const auto colon = name.find(':'); colon != std::string::npos)
                    name.resize(colon);
                if (const auto it = vars.find(name); it != vars.end()) {
                    out += it->second;
                    i = end + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

std::string Num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%d", static_cast<int>(std::lround(v)));
    return b;
}

} // namespace

std::uint64_t Snapshot::Hash() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const int ints[] = {view, slot, page, ok ? 1 : 0, static_cast<int>(std::lround(level)),
                        static_cast<int>(std::lround(xp)), static_cast<int>(std::lround(xp_max))};
    h = Fnv(h, ints, sizeof ints);
    h = Fnv(h, inspect);
    h = Fnv(h, weapon);
    h = Fnv(h, spell);
    for (const auto& c : charms)
        h = Fnv(h, c);
    h = Fnv(h, "|");
    for (const auto& o : owned)
        h = Fnv(h, o);
    for (const auto& [k, v] : stats) {
        h = Fnv(h, k);
        const int iv = static_cast<int>(std::lround(v));
        h = Fnv(h, &iv, sizeof iv);
    }
    return h;
}

/// The game draws an absorbed arcana's art with its gold-leaf material: the texture's alpha is
/// not transparency there but where the gold leaf lies. My approximation of what that material
/// gives on a still menu (no pointer or selection glow): the painting a little darker, warmed
/// towards gold where the mask is set, and opaque.
bc_menu::SpriteP Menu::Goldleaf(const bc_menu::SpriteP& in) {
    auto out = std::make_shared<bc_menu::MenuSprite>(*in);
    static const auto lin = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<std::size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    const auto enc = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        c = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return static_cast<std::uint8_t>(std::lround(c * 255.0f));
    };
    constexpr float kBase = 0.8f;
    constexpr float kGold[3] = {1.6f, 1.45f, 1.07f};
    auto& px = out->s.tex.rgba;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        const float a = static_cast<float>(px[i + 3]) / 255.0f;
        float g = std::clamp((a - 0.5f) / 0.45f, 0.0f, 1.0f);
        g = g * g * (3 - 2 * g);
        for (int c = 0; c < 3; ++c) {
            const float k = kBase * (1.0f + g * (kGold[c] - 1.0f));
            px[i + static_cast<std::size_t>(c)] = enc(lin[px[i + static_cast<std::size_t>(c)]] * k);
        }
        px[i + 3] = px[i + 3] == 0 ? 0 : 255;
    }
    return out;
}

Menu::Menu(bc_levelmap::Assets& assets, const bc_levelmap::Codecs& codecs)
    : assets_(assets), bank_(assets, codecs) {}

std::string Menu::Str(std::int64_t key) const {
    return key && names_ ? names_->strings.Get(key) : std::string{};
}

std::string Menu::Str(std::int64_t key, const ItemInfo* it) const {
    return it ? Smart(Str(key), it->vars) : Str(key);
}

std::string Menu::Name(const std::string& id) const {
    const ItemInfo* it = Info(id);
    return it ? Str(it->name) : std::string{};
}

const ItemInfo* Menu::Info(const std::string& id) const {
    const auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

void Menu::Index(std::string* log) {
    for (const char* prefix : {"weapon-", "charms-", "arcana-", "item-"}) {
        for (const auto& b : assets_.FindAll(prefix)) {
            const auto f = assets_.Open(b);
            if (!f || !f->sf)
                continue;
            for (const auto& o : f->sf->Objects()) {
                if (o.class_id != kClassMonoBehaviour)
                    continue;
                const auto* t = f->sf->TypeOf(o);
                if (!t || !bc_unity::SerializedFile::HasField(*t, "itemID") ||
                    !bc_unity::SerializedFile::HasField(*t, "itemType"))
                    continue;
                const auto v = f->sf->ReadObject(o);
                if (!v)
                    continue;
                ItemInfo it;
                it.id = StrV(v->Get("itemID"));
                if (it.id.empty())
                    continue;
                it.type = static_cast<int>(Int(v->Get("itemType")));
                it.name = KeyOf(v->Get("itemDisplayName"));
                it.desc = KeyOf(v->Get("itemMainDescription"));
                it.desc2 = KeyOf(v->Get("itemSecondaryDescription"));
                it.special = KeyOf(v->Get("weaponSpecialDescription"));
                it.whip = KeyOf(v->Get("weaponWhipAttackDescription"));
                for (const char* fld : {"itemDisplayIconRef", "itemDisplayIconAtlased"}) {
                    const Value* r = v->Get(fld);
                    if (it.icon.empty() && r)
                        it.icon = StrV(r->Get("m_SubObjectName"));
                }
                it.bundle = f->bundle_name;
                if (HasPPtr(v->Get("weaponIllustration")))
                    it.illustration = std::make_shared<Value>(*v->Get("weaponIllustration"));
                it.dps = static_cast<int>(Int(v->Get("fixedDpsDisplay")));
                for (const char* fld : {"itemMainDescription", "itemSecondaryDescription", "weaponSpecialDescription",
                                        "weaponWhipAttackDescription"})
                    LocalVars(*v, v->Get(fld), it.vars);
                it.mana_cost = static_cast<int>(Int(v->Get("spellManaCost")));
                it.mana_cost_type = static_cast<int>(Int(v->Get("manaCostType")));
                if (const Value* ns = v->Get("itemNotions")) {
                    for (const auto& n : ns->list) {
                        const auto ref = assets_.Resolve(f, &n, {"arcana-", "notion", "item-"});
                        if (!ref)
                            continue;
                        if (const auto nv = ref.file->sf->ReadObject(*ref.obj)) {
                            ItemInfo::Notion no;
                            no.desc = KeyOf(nv->Get("description"));
                            if (const Value* ic = nv->Get("iconRef"))
                                no.icon = StrV(ic->Get("m_SubObjectName"));
                            if (no.desc)
                                it.notions.push_back(std::move(no));
                        }
                    }
                }
                if (HasPPtr(v->Get("weaponTier"))) {
                    const auto ref = assets_.Resolve(f, v->Get("weaponTier"), {"weapontier", "weapon-"});
                    if (ref)
                        if (const auto tv = ref.file->sf->ReadObject(*ref.obj))
                            it.tier_name = KeyOf(tv->Get("tierName"));
                }
                if (HasPPtr(v->Get("arcanaCharacterID"))) {
                    const auto ref = assets_.Resolve(f, v->Get("arcanaCharacterID"), {"savablescriptableobject", "arcana"});
                    if (ref) {
                        std::string n = ref.file->sf->PeekName(*ref.obj);
                        if (n.rfind("CharacterID_", 0) == 0)
                            it.character = n.substr(12);
                    }
                }
                auto& slot = items_[it.id];
                if (slot.id.empty() || (slot.icon.empty() && !it.icon.empty()))
                    slot = std::move(it);
            }
        }
    }
    if (const auto f = assets_.Open(assets_.Find("characteruidisplaydata")); f && f->sf) {
        for (const auto& o : f->sf->Objects()) {
            if (o.class_id != kClassMonoBehaviour)
                continue;
            const std::string n = f->sf->PeekName(o);
            if (n.rfind("CharacterUIDisplay_", 0) == 0) {
                characters_[n.substr(19)] = f;
                character_objs_[n.substr(19)] = &o;
            }
        }
    }
    if (log)
        *log += std::to_string(items_.size()) + " items, " + std::to_string(characters_.size()) + " characters\n";
}

bool Menu::Load(const bc_levelmap::Names& names, const bc_levelmap::Font& font, std::string* log) {
    names_ = &names;
    font_ = &font;
    const std::vector<std::string> skip = {"TabContent_Map", "TabContent_TarotDeck", "TabContent_Progress",
                                           "TabContent_GameMenu", "GameStateRequester", "GameMenu_MenuStackLayer",
                                           "OnboardingManager", "ArcanaCompleteCutscene", "TalentAcquisition_MenuStackLayer",
                                           "FirstTalentAttribution_MenuStackLayer"};
    // The page is drawn at about a third of the game's 4K reference: half the texels do.
    bank_.SetReduce(1);
    auto scene = bc_menu::LoadScene(assets_, bank_, names.strings, "GameMenuView", skip, log);
    if (!scene)
        return false;
    Index(log);
    bank_.DropTextures();
    assets_.Trim();
    scene_ = scene;
    return true;
}

bc_menu::SpriteP Menu::Icon(const ItemInfo* it) {
    if (!it || it->icon.empty())
        return nullptr;
    auto s = bank_.ByName(it->icon, kIconPrefixes, 256);
    bank_.DropTextures();
    return s;
}

bc_menu::SpriteP Menu::Illustration(const ItemInfo* it) {
    if (!it || !it->illustration)
        return nullptr;
    auto s = bank_.Resolve(assets_.Open(it->bundle), it->illustration.get(), 512);
    bank_.DropTextures();
    return s;
}

bc_menu::SpriteP Menu::ArcanaArt(const ItemInfo* it, bool absorbed) {
    if (!it || it->character.empty())
        return nullptr;
    const auto f = characters_.find(it->character);
    const auto o = character_objs_.find(it->character);
    if (f == characters_.end() || o == character_objs_.end())
        return nullptr;
    const auto v = f->second->sf->ReadObject(*o->second);
    if (!v)
        return nullptr;
    const Value* p = v->Get(absorbed ? "absorbedArcanaIllustration" : "unabsorbedArcanaIllustration");
    if (!HasPPtr(p))
        p = v->Get("unabsorbedArcanaIllustration");
    auto s = HasPPtr(p) ? bank_.Resolve(f->second, p, 512) : nullptr;
    bank_.DropTextures();
    if (s && absorbed && Int(v->Get("readyForGoldShader")))
        return Goldleaf(s);
    return s;
}

void Menu::Display(Scene& sc, const Comp* d, const ItemInfo* it, const Snapshot& s) {
    (void)s;
    if (!d || !d->v)
        return;
    const Value& v = *d->v;
    const auto image = [&](const char* fld, bc_menu::SpriteP sp) {
        if (bc_menu::Image* im = sc.ImageOf(v.Get(fld))) {
            im->sprite = std::move(sp);
            im->runtime = !im->sprite;
        }
    };
    const auto text = [&](const char* fld, const std::string& t) {
        bc_menu::Text* tx = sc.TextOf(v.Get(fld));
        if (std::getenv("BC_MENU_DEBUG"))
            std::fprintf(stderr, "%s.%s -> %s '%s' node %d\n", d->script.c_str(), fld, tx ? "ok" : "MISSING", t.c_str(), sc.NodeOf(v.Get(fld)));
        if (tx)
            tx->text = t;
    };
    if (d->script == "InventoryItemDisplayBase" || d->script == "InventorySpellDisplay") {
        image("itemIconImage", it ? Icon(it) : nullptr);
        text("itemNameText", it ? Str(it->name) : "");
        text("itemDescriptionText", it ? Str(it->desc, it) : "");
        text("itemSecondaryDescritpionText", it ? Str(it->desc2, it) : "");
        if (d->script == "InventorySpellDisplay") {
            const bool all = it && it->mana_cost_type == 1;
            text("spellCostText", it ? Num(static_cast<float>(it->mana_cost)) : "");
            if (const int n = sc.NodeOf(v.Get("spellCostText")); n >= 0)
                sc.nodes[static_cast<std::size_t>(n)].active = !all;
            if (const int n = sc.NodeOf(v.Get("allManaCostText")); n >= 0)
                sc.nodes[static_cast<std::size_t>(n)].active = all;
        }
        sc.RunEvent(v.Get(it ? "onSetDisplayItemValid" : "onSetDisplayItemNull"));
        if (it)
            sc.RunEvent(v.Get(it->desc2 ? "onDisplayedItemWithLore" : "onDisplayedItemWithoutLore"));
        return;
    }
    if (d->script == "InventoryItemDisplayWeaponCard") {
        image("weaponIllustrationImage", it ? Illustration(it) : nullptr);
        text("weaponNameText", it ? Str(it->name) : "");
        text("weaponGameplayDescriptionText", it ? Str(it->desc, it) : "");
        text("weaponSpecialDescriptionText", it ? Str(it->special, it) : "");
        text("weaponWhipAttackDescriptionText", it ? Str(it->whip, it) : "");
        text("weaponLoreDescriptionText", it ? Str(it->desc2, it) : "");
        text("weaponTierText", it ? Str(it->tier_name) : "");
        text("powerText", it && it->dps ? Num(static_cast<float>(it->dps)) : "");
        sc.RunEvent(v.Get(it ? "onSetDisplayItemValid" : "onSetDisplayItemNull"));
        return;
    }
    if (d->script == "InventoryItemDisplayCollection") {
        if (const Value* ds = v.Get("itemDisplays"))
            for (const auto& x : ds->list)
                if (const Comp* c = sc.CompOf(&x); c && c != d)
                    Display(sc, c, it, s);
        return;
    }
    if (d->script == "InventoryItemDisplayNotions") {
        // One copy of the notion box per notion of the item, as the game instantiates them.
        const int box = sc.NodeOf(v.Get("notionsContainer"));
        const int tmpl = box >= 0 ? sc.Find("P_UI_NotionDisplay", box) : -1;
        if (tmpl < 0)
            return;
        for (const auto& no : it ? it->notions : std::vector<ItemInfo::Notion>{}) {
            const int c = sc.Clone(tmpl, box);
            if (c < 0)
                break;
            sc.nodes[static_cast<std::size_t>(c)].active = true;
            SetText(sc, sc.Find("NotionDescription", c), Str(no.desc));
            const int icon = sc.Find("Image", c);
            if (icon >= 0 && !no.icon.empty())
                if (auto sp = bank_.ByName(no.icon, kIconPrefixes, 128))
                    for (auto& im : sc.nodes[static_cast<std::size_t>(icon)].images)
                        im.sprite = sp, im.runtime = false;
        }
        bank_.DropTextures();
        return;
    }
    if (d->script == "ArcanaDisplay") {
        image("illustrationImage", it ? ArcanaArt(it, true) : nullptr);
        image("spellIconImage", it ? Icon(it) : nullptr);
        if (bc_menu::Text* tx = sc.TextOf(v.Get("nameDisplay")))
            tx->text = it ? Str(it->name) : "";
        sc.RunEvent(v.Get(it ? "onDisplayRevealed" : "onDisplayUnknown"));
        return;
    }
}

void Menu::Bind(Scene& sc, const Snapshot& s) {
    const auto info = [&](const std::string& id) -> const ItemInfo* { return id.empty() ? nullptr : Info(id); };
    const auto owned = [&](const std::string& id) {
        return std::find(s.owned.begin(), s.owned.end(), id) != s.owned.end();
    };
    const auto charm = [&](int i) -> std::string {
        return i >= 0 && i < static_cast<int>(s.charms.size()) ? s.charms[static_cast<std::size_t>(i)] : std::string{};
    };
    const auto stat = [&](const char* name, float fb = 0) {
        const auto it = s.stats.find(name);
        return it == s.stats.end() ? fb : it->second;
    };
    // The tab bar: the Equipment tab is on (Doozy toggle animators: On raises it and lights it).
    for (const auto& c : sc.comps) {
        if ((c.script != "UISelectableColorAnimator" && c.script != "UISelectableUIAnimator") || !c.v)
            continue;
        const int ctl = sc.NodeOf(c.v->Get("Controller"));
        if (ctl < 0 || sc.nodes[static_cast<std::size_t>(ctl)].name.rfind("TabButton_", 0) != 0)
            continue;
        const bool on = sc.nodes[static_cast<std::size_t>(ctl)].name == "TabButton_Equipment";
        if ((Int(c.v->Get("ToggleCommand")) == 0) != on)
            continue;
        const Value* normal = c.v->Get("NormalAnimation");
        if (c.script == "UISelectableColorAnimator") {
            const Value* anim = normal ? normal->Get("Animation") : nullptr;
            const Value* to = anim ? anim->Get("ToValue") : nullptr;
            const int target = sc.NodeOf(c.v->Get("ColorTarget"));
            if (!to || target < 0)
                continue;
            const bc_menu::Rgba col{static_cast<float>(to->Get("r") ? to->Get("r")->AsFloat() : 1),
                                    static_cast<float>(to->Get("g") ? to->Get("g")->AsFloat() : 1),
                                    static_cast<float>(to->Get("b") ? to->Get("b")->AsFloat() : 1),
                                    static_cast<float>(to->Get("a") ? to->Get("a")->AsFloat() : 1)};
            Node& n = sc.nodes[static_cast<std::size_t>(target)];
            for (auto& im : n.images)
                im.color = col;
            for (auto& t : n.texts)
                t.color = col;
        } else if (normal && Int(normal->Get("AnimationType")) == 5) {
            const Value* move = normal->Get("Move");
            const Value* to = move ? move->Get("ToValue") : nullptr;
            if (to && Int(move->Get("Enabled"), 1)) {
                Node& n = sc.nodes[static_cast<std::size_t>(c.node)];
                n.pos[0] += static_cast<float>(to->Get("x") ? to->Get("x")->AsFloat() : 0);
                n.pos[1] += static_cast<float>(to->Get("y") ? to->Get("y")->AsFloat() : 0);
            }
        }
    }
    // Equipped items, in the equipment view and the relics inventory's slots.
    for (const auto& c : sc.comps) {
        if (c.script != "EquipmentViewEquippedItemController" || !c.v)
            continue;
        const int type = static_cast<int>(Int(c.v->Get("itemType")));
        const int index = static_cast<int>(Int(c.v->Get("charmSlotIndex")));
        const std::string id = type == 1 ? s.weapon : type == 2 ? s.spell : charm(index);
        if (const Value* ds = c.v->Get("displays"))
            for (const auto& d : ds->list)
                Display(sc, sc.CompOf(&d), info(id), s);
        Display(sc, sc.CompOf(c.v->Get("arcanaDisplay")), info(id), s);
        if (type == 3) {
            const int to_equip = sc.NodeOf(c.v->Get("toEquipSlotDisplay"));
            if (to_equip >= 0)
                sc.nodes[static_cast<std::size_t>(to_equip)].active = s.view == 2 && index == s.slot;
            // The picked slot lights up.
            if (index == s.slot)
                for (auto& im : sc.nodes[static_cast<std::size_t>(c.node)].images)
                    im.color = {1.0f, 0.95f, 0.75f, 1.0f};
            // Slots the player has not unlocked yet stay dim.
            if (s.ok && index >= static_cast<int>(s.charms.size()))
                sc.nodes[static_cast<std::size_t>(c.node)].alpha = 0.35f;
        }
    }
    // The relic picked in the equipment view: its name and description.
    if (const int box = sc.Find("Charms_PreviewBox"); box >= 0) {
        const ItemInfo* it = s.view == 0 ? info(charm(s.slot)) : nullptr;
        sc.nodes[static_cast<std::size_t>(box)].active = it != nullptr;
        if (it) {
            SetText(sc, sc.Find("ItemName", box), Str(it->name));
            SetText(sc, sc.Find("ItemDescription", box), Str(it->desc));
        }
    }
    // Stats, as the game's stat displays read them (each names its stat classes).
    if (s.ok) {
        for (const auto& c : sc.comps) {
            if (!c.v)
                continue;
            const Value& v = *c.v;
            if (c.script == "IntStatValueDisplay") {
                const auto read = [&](const char* fld) { return stat(RefClass(v, v.Get(fld)).c_str()); };
                float value = read("statToRead");
                if (Int(v.Get("hasAdditionalValue")))
                    value += read("additionalStatToRead");
                std::string t = Num(value);
                if (Int(v.Get("hasMaxValue")))
                    t += "/" + Num(read("maxStatToRead"));
                if (bc_menu::Text* tx = sc.TextOf(v.Get("statValueText")))
                    tx->text = t;
            } else if (c.script == "ExperienceBarDisplay") {
                const float cur = stat(RefClass(v, v.Get("currentXpStat")).c_str());
                const float mx = stat(RefClass(v, v.Get("maxXpStat")).c_str());
                const float xp = std::clamp(mx > 0 ? cur / mx : 0.0f, 0.0f, 1.0f);
                if (bc_menu::Image* im = sc.ImageOf(v.Get("fillImage"))) {
                    if (im->type == 3) {
                        im->fill_amount = xp;
                    } else if (const int fill = sc.NodeOf(v.Get("fillImage")); fill >= 0) {
                        Node& n = sc.nodes[static_cast<std::size_t>(fill)];
                        if (n.amax[0] > n.amin[0]) {
                            n.amax[0] = n.amin[0] + (n.amax[0] - n.amin[0]) * xp;
                        } else {
                            // A fixed-width bar: shortened, its left end kept.
                            const float w = n.size[0];
                            n.size[0] = w * xp;
                            n.pos[0] -= (1.0f - xp) * w * n.pivot[0]; // keeps the left end
                        }
                    }
                }
            } else if (c.script == "FragmentedCollectibleCountDisplay") {
                const int done = static_cast<int>(stat(RefClass(v, v.Get("completedCollectibleStat")).c_str()));
                const int frag = static_cast<int>(stat(RefClass(v, v.Get("uncompletedCollectibleFragmentStat")).c_str()));
                const Value* list = v.Get("collectibleDisplays");
                for (std::size_t i = 0; list && i < list->list.size(); ++i) {
                    const Comp* d = sc.CompOf(&list->list[i]);
                    if (!d || !d->v)
                        continue;
                    const int ii = static_cast<int>(i);
                    if (ii < done) {
                        sc.RunEvent(d->v->Get("onDisplayCompleted"));
                    } else if (const Value* un = d->v->Get("onDisplayUncompleted"); un && !un->list.empty()) {
                        const std::size_t k = std::min<std::size_t>(ii == done ? static_cast<std::size_t>(std::max(frag, 0)) : 0,
                                                                     un->list.size() - 1);
                        sc.RunEvent(&un->list[k]);
                    }
                }
            }
        }
    }
    // Inventories.
    const auto highlight = [&](int node, bool on) {
        const int bg = node >= 0 ? sc.Find("Background", node) : -1;
        if (bg >= 0 && on)
            for (auto& im : sc.nodes[static_cast<std::size_t>(bg)].images)
                im.color = {1.0f, 0.9f, 0.6f, 1.0f};
    };
    const auto boolean = [&](const Value* pptr, bool on) {
        const Comp* b = sc.CompOf(pptr);
        if (b && b->v)
            sc.RunEvent(b->v->Get(on ? "onSetTrue" : "onSetFalse"));
    };
    const std::string equipped_weapon = s.weapon, equipped_spell = s.spell;
    if (s.view == 1) {
        for (const auto& c : sc.comps) {
            if (c.script != "InventoryWeaponChecker" || !c.v)
                continue;
            const Comp* ctl = sc.CompOf(c.v->Get("weaponSlotController"));
            if (!ctl || !ctl->v)
                continue;
            const bool have = owned(c.item_id) || c.item_id == equipped_weapon;
            Display(sc, sc.CompOf(ctl->v->Get("itemDisplay")), have ? info(c.item_id) : nullptr, s);
            boolean(ctl->v->Get("equippedDisplay"), c.item_id == equipped_weapon);
            highlight(c.node, have && c.item_id == s.inspect);
        }
        for (const auto& c : sc.comps) {
            if (c.script != "InventoryWeaponArchetypeList" || !c.v)
                continue;
            bool any = false, eq = false;
            if (const Value* ws = c.v->Get("orderedWeapons")) {
                for (const auto& w : ws->list) {
                    const int n = sc.NodeOf(&w);
                    const Comp* ch = n >= 0 ? sc.CompOn(n, "InventoryWeaponChecker") : nullptr;
                    if (ch) {
                        any |= owned(ch->item_id) || ch->item_id == equipped_weapon;
                        eq |= ch->item_id == equipped_weapon;
                    }
                }
            }
            boolean(c.v->Get("booleanUIView"), any);
            boolean(c.v->Get("containsEquippedWeaponView"), eq);
        }
        const int card = sc.Find("Selected_WeaponCard");
        if (card >= 0)
            Display(sc, sc.CompOn(card, "InventoryItemDisplayWeaponCard"), info(s.inspect.empty() ? equipped_weapon : s.inspect), s);
    }
    if (s.view == 2) {
        for (const auto& c : sc.comps) {
            if (c.script != "InventoryCharmChecker" || !c.v)
                continue;
            const Comp* ctl = sc.CompOf(c.v->Get("charmSlotController"));
            if (!ctl || !ctl->v)
                continue;
            bool worn = false;
            for (const auto& w : s.charms)
                worn |= !w.empty() && w == c.item_id;
            const bool have = owned(c.item_id) || worn;
            Display(sc, sc.CompOf(ctl->v->Get("itemDisplay")), have ? info(c.item_id) : nullptr, s);
            boolean(ctl->v->Get("equippedDisplay"), worn);
            highlight(c.node, have && c.item_id == s.inspect);
        }
        const int p1 = sc.Find("CharmsItemSlots_GridPage"), p2 = sc.Find("CharmsItemSlots_GridPage2");
        if (p1 >= 0 && p2 >= 0) {
            Node& a = sc.nodes[static_cast<std::size_t>(p1)];
            Node& b = sc.nodes[static_cast<std::size_t>(p2)];
            a.active = s.page == 0, a.alpha = 1;
            b.active = s.page == 1, b.alpha = 1;
            b.pos[0] = a.pos[0], b.pos[1] = a.pos[1];
        }
        const int sel = sc.Find("Selected_PreviewCard"), eqc = sc.Find("Equipped_PreviewCard");
        if (sel >= 0)
            Display(sc, sc.CompOn(sel, "InventoryItemDisplayBase"), info(s.inspect), s);
        if (eqc >= 0)
            Display(sc, sc.CompOn(eqc, "InventoryItemDisplayBase"), info(charm(s.slot)), s);
    }
    if (s.view == 3) {
        for (const auto& c : sc.comps) {
            if (c.script != "InventorySpellChecker" || !c.v)
                continue;
            const Comp* ctl = sc.CompOf(c.v->Get("spellSlotController"));
            if (!ctl || !ctl->v)
                continue;
            const bool have = owned(c.item_id) || c.item_id == equipped_spell;
            Display(sc, sc.CompOf(ctl->v->Get("itemDisplay")), have ? info(c.item_id) : nullptr, s);
            boolean(ctl->v->Get("equippedDisplay"), c.item_id == equipped_spell);
            if (have && c.item_id == s.inspect) {
                const int slot = sc.Find("CommonUI_SpellSlot", c.node);
                if (slot >= 0)
                    for (auto& im : sc.nodes[static_cast<std::size_t>(slot)].images)
                        im.color = {1.0f, 0.85f, 0.45f, 1.0f};
            }
        }
        const std::string shown = s.inspect.empty() ? equipped_spell : s.inspect;
        const int card = sc.Find("ArcanaCard_Display_Anim");
        if (card >= 0)
            Display(sc, sc.CompOn(card, "ArcanaDisplay"), info(shown), s);
        const int col = sc.Find("SpellAndTalents_Display");
        if (const Comp* c = col >= 0 ? sc.CompOn(col, "InventoryItemDisplayCollection") : nullptr; c && c->v)
            if (const Value* ds = c->v->Get("itemDisplays"))
                for (const auto& d : ds->list)
                    Display(sc, sc.CompOf(&d), info(shown), s);
    }
}

std::vector<std::uint8_t> Menu::Render(const Snapshot& s, int w, int h, std::vector<Target>* targets) {
    if (!scene_ || !font_)
        return {};
    const auto t0 = std::chrono::steady_clock::now();
    const auto lap = [&](const char* what) {
        if (std::getenv("BC_MENU_TIME"))
            std::fprintf(stderr, "  %s %.0f ms\n", what,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    };
    Scene sc = *scene_;
    lap("copy");
    Bind(sc, s);
    lap("bind");
    float cw, ch, scale;
    bc_menu::CanvasSize(w, h, kMatch, cw, ch, scale);
    bc_menu::Layout(sc, *font_, cw, ch);
    // The inventories' dark backdrops cover the game's 16:9 screen; on this taller page they
    // cover all of it, as they do in the game.
    for (auto& n : sc.nodes)
        if (n.name == "CommonUI_DarkOverlay")
            n.rect = bc_menu::Rect{0, 0, cw, ch};
    lap("layout");
    // Hidden: other views, input prompts and footnotes (no buttons on a touch screen).
    std::vector<bool> hide(sc.nodes.size(), false);
    static const char* const kAlways[] = {
        "Footnote_Equipment", "Footnote_Inventory", "Footnote_CharmsInventory", "Footnote_InventorySpell",
        "OpenInventoryButton_Weapon", "OpenInventoryButton_Arcana", "OpenInventoryButton_Charms",
        "CommonUI_PreviousTab", "CommonUI_NextTab", "CommonUI_NextTab_Anim", "CommonUI_PreviousTab_Anim",
        "MenuTutorialLauncher_WeaponInventory", "MenuTutorialLauncher_CharmsInventory",
        "MenuTutorialLauncher_SpellInventory", "MenuTutorialLauncher_WorksOfMercy", "Masks", "TalentAcquisition_Overlay",
        "TaskList_Display_Anim", "TalentGem_VerticalLayout", "TalentAttribution_UnlockTalent", "SortingBar_Inputs",
        "P_UI_Commons_InputButton", "P_UI_Commons_InputButton (1)", "VFX_P_Menu_Mist Variant Variant"};
    const auto hide_name = [&](const char* name) {
        for (std::size_t i = 0; i < sc.nodes.size(); ++i)
            if (sc.nodes[i].name == name)
                hide[i] = true;
    };
    for (const char* n : kAlways)
        hide_name(n);
    // Input prompts (gamepad buttons): not on a touch screen.
    static const char* const kPrefixes[] = {"P_UI_Commons_InputButton", "CommonUI_Input", "OpenInventoryButton_",
                                            "Footnote_", "MenuTutorialLauncher_"};
    for (std::size_t i = 0; i < sc.nodes.size(); ++i)
        for (const char* p : kPrefixes)
            if (sc.nodes[i].name.rfind(p, 0) == 0)
                hide[i] = true;
    if (const char* at = std::getenv("BC_MENU_AT")) {
        // The drawn images under a pixel, in drawing order.
        float px = 0, py = 0;
        std::sscanf(at, "%f,%f", &px, &py);
        const float ux = px / scale, uy = ch - py / scale;
        const std::function<void(int, bool)> walk = [&](int i, bool vis) {
            const Node& n = sc.nodes[static_cast<std::size_t>(i)];
            vis = vis && n.active && !hide[static_cast<std::size_t>(i)];
            if (!vis)
                return;
            if (n.rect.Contains(ux, uy))
                for (const auto& im : n.images)
                    std::fprintf(stderr, "AT %s img %s color %.2f,%.2f,%.2f,%.2f alpha %.2f rect %.0f,%.0f %.0fx%.0f\n",
                                 n.name.c_str(), im.sprite ? im.sprite->name.c_str() : "(none)", im.color.r, im.color.g,
                                 im.color.b, im.color.a, n.alpha, n.rect.x, n.rect.y, n.rect.w, n.rect.h);
            for (int c : n.children)
                walk(c, vis);
        };
        walk(sc.root, true);
    }
    if (const char* dump = std::getenv("BC_MENU_DUMP")) {
        const std::function<void(int, int)> walk = [&](int i, int d) {
            const Node& n = sc.nodes[static_cast<std::size_t>(i)];
            std::fprintf(stderr, "%*s%s%s rect %.0f,%.0f %.0fx%.0f%s\n", d * 2, "", n.active ? "" : "#", n.name.c_str(),
                         n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.texts.empty() ? "" : (" '" + n.texts[0].text.substr(0, 20) + "'").c_str());
            if (d < 6)
                for (int c : n.children)
                    walk(c, d + 1);
        };
        const int at = sc.Find(dump);
        if (at >= 0)
            walk(at, 0);
    }
    if (s.view != 1)
        hide_name("Overlay_WeaponInventory");
    if (s.view != 2)
        hide_name("Overlay_CharmsInventory");
    if (s.view != 3)
        hide_name("Overlay_SpellInventory");
    lap("hide");
    auto px = bc_menu::Render(sc, *font_, scale, w, h, &hide);
    lap("render");
    if (targets) {
        targets->clear();
        const auto add = [&](Target::Kind k, int node, int index = 0, const std::string& item = {}) {
            if (node < 0)
                return;
            const Node& n = sc.nodes[static_cast<std::size_t>(node)];
            if (!n.active)
                return;
            Target t{k, index, item, 0, 0, 0, 0};
            bc_menu::PixelRect(n.rect, scale, ch, t.x, t.y, t.w, t.h);
            if (t.w > 1 && t.h > 1)
                targets->push_back(std::move(t));
        };
        const char* const tabs[] = {"TabButton_Map", "TabButton_Equipment", "TabButton_TarotDeck", "TabButton_Progress",
                                    "TabButton_GameMenu"};
        for (int i = 0; i < 5; ++i)
            add(Target::Tab, sc.Find(tabs[i]), i);
        if (s.view == 0) {
            add(Target::OpenWeapons, sc.Find("Equippement_Weapon_ItemSlot"));
            add(Target::OpenArcanas, sc.Find("Equippement_Spell_ItemSlot"));
            add(Target::OpenWeapons, sc.Find("WeaponInventoryTitle"));
            add(Target::OpenArcanas, sc.Find("ArcanaInventoryTitle"));
            add(Target::OpenRelics, sc.Find("CharmsInventoryTitle"));
            const int charms = sc.Find("Charms");
            const char* const slots[] = {"EquippedCharmSlot", "EquippedCharmSlot (1)", "EquippedCharmSlot (2)"};
            for (int i = 0; i < 3; ++i)
                add(Target::RelicSlot, charms >= 0 ? sc.Find(slots[i], charms) : -1, i);
        } else {
            const auto items = [&](const char* checker) {
                for (const auto& c : sc.comps)
                    if (c.script == checker && !c.item_id.empty()) {
                        // Only the slots on screen (the relics page shown).
                        bool shown = true;
                        for (int p = c.node; p >= 0; p = sc.nodes[static_cast<std::size_t>(p)].parent)
                            shown &= sc.nodes[static_cast<std::size_t>(p)].active;
                        if (shown)
                            add(Target::Item, c.node, 0, c.item_id);
                    }
            };
            if (s.view == 1) {
                items("InventoryWeaponChecker");
                add(Target::Close, -1);
            } else if (s.view == 2) {
                items("InventoryCharmChecker");
                const int group = sc.Find("EquipedCharmsGroup");
                const char* const slots[] = {"EquippedCharmSlot_Inventory", "EquippedCharmSlot_Inventory (1)",
                                             "EquippedCharmSlot_Inventory (2)"};
                for (int i = 0; i < 3; ++i)
                    add(Target::RelicSlot, group >= 0 ? sc.Find(slots[i], group) : -1, i);
                add(Target::PrevPage, sc.Find("PreviousPageView"));
                add(Target::NextPage, sc.Find("NextPageView"));
            } else if (s.view == 3) {
                items("InventorySpellChecker");
            }
            // A tap on the panel does nothing; outside it closes the inventory.
            const char* const panels[] = {"ParchmentSupport", "Selected_WeaponCard", "InventoryGroup",
                                          "Selected_PreviewCard", "Equipped_PreviewCard", "SelectedSpell_TalentView",
                                          "SpellSelection_ArcanaListView_Icons", "Banner_Anim", "Header"};
            for (const char* p : panels) {
                const int ov = sc.Find(s.view == 1 ? "Overlay_WeaponInventory" : s.view == 2 ? "Overlay_CharmsInventory"
                                                                                            : "Overlay_SpellInventory");
                add(Target::Close, ov >= 0 ? sc.Find(p, ov) : -1, -1);
            }
            Target all{Target::Close, 0, {}, 0, 0, static_cast<float>(w), static_cast<float>(h)};
            targets->push_back(all);
        }
    }
    return px;
}

} // namespace bc_menupage
