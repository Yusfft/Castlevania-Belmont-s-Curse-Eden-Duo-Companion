// SPDX-License-Identifier: GPL-3.0-or-later
//
// Castlevania facts for the loading page. The texts are the mod's own, written from public
// sources (listed in README.md, "Loading facts"); nothing here is taken from the game. A fact
// may name one of the game's items whose art shows beside it (drawn at runtime from the
// player's romfs, framed as the game frames it): an arcana (its tarot card), a weapon (its
// card) or a relic (its slot).
#pragma once

namespace bc_facts {

enum Art { kNone, kArcana, kWeapon, kRelic };

struct Fact {
    const char* title;
    const char* text;
    Art art;
    const char* item; // the item's id (kNone: "")
};

inline constexpr Fact kFacts[] = {
    {"Demon Castle Dracula",
     "The first Castlevania came out in Japan on 26 September 1986 for the Famicom Disk System. Its Japanese "
     "title, Akumajo Dracula, means \"Demon Castle Dracula\".",
     kWeapon, "WeaponWhipTier1"},
    {"Almost Peter Dante",
     "Early in development Simon Belmont was called Peter Dante, a nod to Peter Cushing, Joe Dante and "
     "Christopher Lee.",
     kWeapon, "WeaponWhipTier3"},
    {"Lost sub-weapons",
     "Garlic, wooden stakes and an item that turned the hero into a werewolf were planned for the first "
     "Castlevania and cut before release.",
     kNone, ""},
    {"A cross that comes back",
     "The first Castlevania's sub-weapons were throwing knives, axes, holy water, a stopwatch and a cross "
     "that flies back like a boomerang.",
     kArcana, "BoomerangCross"},
    {"Twin castles",
     "Vampire Killer, a different take on the same story for the MSX2, was made alongside the first "
     "Castlevania and released a month after it.",
     kNone, ""},
    {"A million hunters",
     "The first Castlevania sold over a million copies and was the best-rated game Famitsu reviewed in 1986.",
     kNone, ""},
    {"The first Belmont",
     "Lament of Innocence (2003) goes furthest back: in 1094 Leon Belmont storms a vampire's castle to save "
     "his betrothed, Sara.",
     kArcana, "BoomerangCross"},
    {"Birth of the Vampire Killer",
     "The whip was made by the alchemist Rinaldo Gandolfi. In Lament of Innocence it only becomes the "
     "Vampire Killer when Leon completes a ritual that costs Sara her life.",
     kWeapon, "WeaponWhipTier4"},
    {"Before Dracula",
     "Lament of Innocence ends with a revelation: Leon's friend Mathias Cronqvist is the man who becomes "
     "Dracula.",
     kNone, ""},
    {"Dracula's Curse",
     "Castlevania III: Dracula's Curse (1989) is set in 1476. Trevor Belmont can team up with the sorceress "
     "Sypha, the pirate Grant or Alucard.",
     kNone, ""},
    {"Two roads to the castle",
     "Castlevania III splits into two routes, over the lake or through the caverns. Its ending changes with "
     "the companion at Trevor's side.",
     kNone, ""},
    {"Extra channels",
     "In Japan, Castlevania III shipped with Konami's VRC6 chip, which added three sound channels. The "
     "Western cartridge could not use it, so its music was rearranged.",
     kNone, ""},
    {"Trevor's daughter",
     "Belmont's Curse is set in Paris in 1499, 23 years after Castlevania III. Its hero Rose, Trevor and "
     "Sypha's daughter, carries Dracula's curse in her right arm.",
     kArcana, "CursedFireBall"},
    {"Forty years",
     "Belmont's Curse marks the series' 40th anniversary. Evil Empire made it with Konami, advised by Motion "
     "Twin, the studio behind Dead Cells.",
     kNone, ""},
    {"Dracula, reversed",
     "Alucard is Dracula spelled backwards. Dracula's half-human son first appeared in Castlevania III, as a "
     "boss who could join Trevor.",
     kWeapon, "WeaponAndShieldTier4"},
    {"Adrian",
     "Alucard's full name is Adrian Fahrenheit Tepes. His mother was Lisa, a human.",
     kWeapon, "WeaponAndShieldTier4"},
    {"Symphony of the Night",
     "Symphony of the Night came out in Japan on 20 March 1997. It shaped the metroidvania, a genre named "
     "after Metroid and Castlevania.",
     kNone, ""},
    {"A pile of secrets",
     "\"What is a man? A miserable little pile of secrets.\" Translator Jeremy Blaustein added the line to "
     "Symphony of the Night, after the writer Andre Malraux.",
     kNone, ""},
    {"Upside down",
     "Symphony of the Night hides a second castle: the whole castle again, upside down.",
     kNone, ""},
    {"A first portrait",
     "Symphony of the Night was Ayami Kojima's first work in games: she designed its characters, Alucard "
     "among them.",
     kNone, ""},
    {"A horrible night",
     "In Simon's Quest (1987) day and night change every five minutes. Nights bring stronger monsters and "
     "the series' best-known line: \"What a horrible night to have a curse.\"",
     kNone, ""},
    {"Bloody Tears",
     "In Simon's Quest, Simon gathers Dracula's scattered remains to lift a curse. Its soundtrack includes "
     "the much-loved \"Bloody Tears\".",
     kNone, ""},
    {"Rondo of Blood",
     "Rondo of Blood first came out only in Japan, on 29 October 1993. Its 19-year-old hero Richter Belmont can "
     "rescue Maria Renard, who then becomes playable.",
     kNone, ""},
    {"Devil forgemasters",
     "Curse of Darkness (2005) is set in 1479. Hector hunts Isaac, a rival devil forgemaster, with demons he "
     "forged himself.",
     kArcana, "RiseAndFall"},
    {"Carmilla",
     "Carmilla, Dracula's fanatical follower, is most likely based on Sheridan Le Fanu's 1872 novella "
     "Carmilla.",
     kArcana, "BloodSpikes"},
    {"Medusa",
     "In the first Castlevania, Medusa waits at the end of the second stage as a giant floating head. Her "
     "flying heads became a series staple.",
     kArcana, "PetrifyingRay"},
    {"Dracula's reaper",
     "Death guards the fifth stage of the first Castlevania. In Symphony of the Night he strips Alucard of "
     "his gear at the castle gate.",
     kArcana, "LaserScytheSlash"},
    {"Two faces of Death",
     "In Castlevania III, Death was the first to come back as a giant laughing skull after his first defeat.",
     kArcana, "LaserScytheSlash"},
    {"The painter",
     "In Portrait of Ruin (2006), Brauner draws on the suffering of the Second World War through his "
     "paintings, worlds the heroes must enter to break his spells.",
     kArcana, "InvocationStone"},
    {"The sword of Fierbois",
     "Joan of Arc's sword was found buried behind the altar of the chapel of Sainte-Catherine-de-Fierbois. "
     "She carried it when she retook Orleans.",
     kWeapon, "WeaponTwoHandedTier1"},
    {"Joan of Arc",
     "Belmont's Curse takes place in 1499, 68 years after Joan of Arc was burned at Rouen. She is among the "
     "game's characters.",
     kArcana, "FireSeismicWave"},
    {"A chapel of glass",
     "Louis IX built the Sainte-Chapelle in Paris to keep the Crown of Thorns. It was consecrated on 26 "
     "April 1248.",
     kArcana, "Detainment"},
    {"The bronze guardian",
     "In Greek myth Talos, a man of bronze, guarded Crete, circling it three times a day. Medea brought him "
     "down through the one vein in his ankle.",
     kArcana, "GiantFist"},
    {"Durandal",
     "Roland's sword Durandal kept relics in its golden hilt. Dying at Roncevaux, Roland tried to break it "
     "on a rock, and the blade held.",
     kWeapon, "WeaponTwoHandedTier3"},
    {"Caliburnus",
     "Around 1136 Geoffrey of Monmouth called Arthur's sword Caliburnus. Later writers turned the name into "
     "Excalibur.",
     kWeapon, "WeaponTwoHandedTier4"},
    {"Back to the lake",
     "As Arthur lay dying, a knight threw Excalibur into a lake. In the English tellings it is Bedivere, "
     "and a hand rises from the water to take it.",
     kArcana, "GlassArmor"},
    {"Gungnir",
     "Odin's spear Gungnir was forged by the dwarven sons of Ivaldi and never misses its mark.",
     kWeapon, "WeaponSpearTier3"},
    {"On the small screen",
     "Netflix's Castlevania premiered on 7 July 2017. Written by Warren Ellis, its first two seasons adapt "
     "Castlevania III. It ran for four seasons, until May 2021.",
     kNone, ""},
    {"Nocturne",
     "Castlevania: Nocturne (2023) moves to France in 1792, during the Revolution, with Richter Belmont and "
     "Maria Renard.",
     kNone, ""},
    {"In the arcade",
     "In April 1987 the first Castlevania reached arcades as VS. Castlevania, on Nintendo's VS. System.",
     kNone, ""},
};

inline constexpr int kCount = static_cast<int>(sizeof(kFacts) / sizeof(kFacts[0]));

} // namespace bc_facts
