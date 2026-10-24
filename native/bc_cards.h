// SPDX-License-Identifier: GPL-3.0-or-later
//
// The save screen's painted biome cards (sprite atlas T_UI_Save_BiomeCards in the player's own
// ui-commonsalltime bundle): the art of the title and loading pages. Read on demand, one card at
// a time; nothing is kept or shipped.
#pragma once

#include <optional>
#include <string>

#include "bc_levelmap.h"

namespace bc_cards {

/// Cards in the atlas, in the order of its packed sprite names.
constexpr int kCount = 9;

/// The card of a world ("World_Biome_06_MainHall" -> the Main Hall card), -1 when it has none.
int ForWorld(const std::string& world_name);

/// Card `index` (0 .. kCount-1), cropped from its atlas page; rows bottom-up like the game's
/// textures. Not thread-safe with other users of `assets` (the caller serialises).
std::optional<bc_levelmap::Sprite> Load(bc_levelmap::Assets& assets, const bc_levelmap::Codecs& codecs, int index,
                                        std::string* err);

} // namespace bc_cards
