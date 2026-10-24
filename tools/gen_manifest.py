#!/usr/bin/env python3
"""Writes package/dualscreen/manifest.json for the Belmont's Curse map companion.

The built-in UI font is a 3x5 bitmap with digits, letters (drawn upper-case), space and
: - . / % + , only, so every literal text here sticks to those characters.
"""
import json
from pathlib import Path

W, H = 1240, 1080
BAR = "#FF16121E"
GOLD = "#FFE9C46A"
DIM = "#FF9A8FB0"
TEXT = "#FFE6E1F0"
BG = "#FF0C0A10"

# Player marker sizes per zoom level (the game draws it 5 world units tall; 7 and 14 px per
# unit in the medium and close views, a fixed size in the whole-area view).
# Player marker sizes per zoom level: the game draws it 5 world units tall (5, 8, 13 and 20 px
# per unit in the four follow views, never under 30 px), a fixed size in the whole-area view.
MARKER = {"bc.mk.z0": 30, "bc.mk.z1": 30, "bc.mk.z2": 40, "bc.mk.z3": 65, "bc.mk.z4": 100}
TITLE_W, TITLE_H = 640, 344  # must match kTitleW / kTitleH in 0100BA4026E94000.cpp
ZOOM_W, ZOOM_H = 900, 130     # bc_ui::ZoomBarLayout
ZOOM_LEVELS = 5
ZOOM_X, ZOOM_Y = (W - ZOOM_W) // 2, H - ZOOM_H - 120
ZOOM_TICKS = [150 + (750 - 150) * z // (ZOOM_LEVELS - 1) for z in range(ZOOM_LEVELS)]  # ZoomBarLayout::Tick
ZOOM_BAR_Y = 44
ZBTN_W, ZBTN_H = 230, 64      # kZoomButtonW / kZoomButtonH
ZBTN_X, ZBTN_Y = 40, H - ZBTN_H - 30
PIN_CELL = 84                 # kPinCell
PIN_TYPES = 3                 # kPinTypes
BTN_W, BTN_H = 170, 56        # kButtonW / kButtonH
LABEL_W, LABEL_H = 700, 70    # kLabelW / kLabelH
PIN_X, PIN_Y = W - BTN_W - 40, 30
GO_W, GO_H = 760, 420         # kGameOverW / kGameOverH
PAUSED_W, PAUSED_H = 600, 200 # kPausedW / kPausedH
PROMPT_W, PROMPT_H = 700, 70  # kPromptW / kPromptH
VER_W, VER_H = 460, 50        # kVersionW / kVersionH


def fade(group, ms):
    return {"anim": {"bind": group, "from": "fade", "ms": ms, "group": group}}


def label(rect, text=None, bind_text=None, scale=4, color=TEXT, **kw):
    w = {"type": "label", "rect": rect, "text_scale": scale, "color": color}
    if text is not None:
        w["text"] = text
    if bind_text is not None:
        w["bind_text"] = bind_text
    w.update(kw)
    return w


def value(rect, bind, scale=4, color=TEXT, **kw):
    w = {"type": "value", "rect": rect, "bind": bind, "text_scale": scale, "color": color}
    w.update(kw)
    return w


def button(x, text, action, y=996, w=192, h=72):
    return {"type": "button", "rect": [x, y, w, h], "text": text, "on_tap": action,
            "text_scale": 5, "color": TEXT, "bg": "#FF2A2238", "border": 3}


def rect(r, bg, **kw):
    w = {"type": "rect", "rect": r, "bg": bg, "color": "#00000000", "frame": 0}
    w.update(kw)
    return w


def marker(bind, size):
    return {"type": "image", "rect": [W // 2 - size // 2, H // 2 - size // 2, size, size],
            "src": "module:bc:player:%d" % size, "x_bind": "bc.mk.dx", "y_bind": "bc.mk.dy",
            "need_bind": bind}


def tap(r, action):
    return rect(r, "#00000000", on_tap=action)


map_page = {
    "id": "map",
    "title": "Map",
    "widgets": [
        rect([0, 0, W, H], BG),
        # Follow view: the previous window stays underneath while the next one loads.
        {"type": "image", "rect": [0, 0, 2048, 1536], "src_bind": "bc.map.pkey",
         "x_bind": "bc.map.pox", "y_bind": "bc.map.poy", "need_bind": "bc.map.pshow"},
        {"type": "image", "rect": [0, 0, 2048, 1536], "src_bind": "bc.map.key",
         "x_bind": "bc.map.ox", "y_bind": "bc.map.oy", "need_bind": "bc.map.follow"},
        # Whole-area view.
        {"type": "image", "rect": [0, 0, W, H], "src_bind": "bc.map.akey",
         "need_bind": "bc.map.area"},
        # The game's player marker, centred; it only moves in the whole-area view.
        *[marker(b, sz) for b, sz in MARKER.items()],
        # Pause (a menu or the pause screen open in the game): the map blurred under "Paused".
        {"type": "image", "rect": [0, 0, 2048, 1536], "src_bind": "bc.map.bkey",
         "x_bind": "bc.map.ox", "y_bind": "bc.map.oy", **fade("bc.paused.f", 260)},
        {"type": "image", "rect": [0, 0, W, H], "src_bind": "bc.map.bakey", **fade("bc.paused.a", 260)},
        rect([0, 0, W, H], "#59000000", **fade("bc.paused", 260)),
        {"type": "image", "rect": [(W - PAUSED_W) // 2, (H - PAUSED_H) // 2, PAUSED_W, PAUSED_H],
         "src": "module:bc:ui:paused", **fade("bc.paused", 260)},
        # Pin mode: a tap anywhere on the map (under the buttons) goes to the module.
        rect([0, 0, W, H], "#00000000", on_tap="act_pin_tap", payload="$tap", need_bind="bc.pin.mode"),
        # Before a save is loaded: one of the game's painted biome cards.
        {"type": "image", "rect": [0, 0, W, H], "src": "module:bc:ui:menuart", "need_bind": "bc.ui.wait"},
        # The map screen's gold-edged frame (game art, drawn by the module).
        {"type": "image", "rect": [0, 0, W, H], "src": "module:bc:ui:frame", "need_bind": "bc.ui.ok"},
        # Before a save is loaded (the game's own menus): the prompt and the mod's version.
        {"type": "image", "rect": [(W - PROMPT_W) // 2, H - PROMPT_H - 40, PROMPT_W, PROMPT_H],
         "src": "module:bc:ui:prompt", "need_bind": "bc.ui.wait"},
        {"type": "image", "rect": [W - VER_W - 30, H - VER_H - 24, VER_W, VER_H],
         "src": "module:bc:ui:version", "need_bind": "bc.ui.wait"},
        # Level banner with the area and district names (a plain label until the art is read).
        {"type": "image", "rect": [14, 10, TITLE_W, TITLE_H], "src_bind": "bc.title.key",
         "need_bind": "bc.title.img"},
        label([36, 30, 900, 40], bind_text="bc.title", scale=5, color=GOLD,
              need_bind="!bc.title.img"),
        # Zoom: a small button (tap: next level out, wrapping to the closest; hold: the scale).
        {"type": "image", "rect": [ZBTN_X, ZBTN_Y, ZBTN_W, ZBTN_H], "src_bind": "bc.ui.zbtn",
         "need_bind": "bc.ui.ok"},
        tap([ZBTN_X, ZBTN_Y, ZBTN_W, ZBTN_H], "act_zoom") | {"on_hold": "act_zbar_open", "hold_ms": 450,
                                                           "need_bind": "bc.ui.ok"},
        # The scale, over a layer that closes it: - and + buttons, or tap a level's name.
        rect([0, 0, W, H], "#00000000", on_tap="act_zbar_close", need_bind="bc.zbar"),
        {"type": "image", "rect": [ZOOM_X, ZOOM_Y, ZOOM_W, ZOOM_H], "src_bind": "bc.ui.zoom",
         "need_bind": "bc.zbar"},
        tap([ZOOM_X - 10, ZOOM_Y + ZOOM_BAR_Y - 52, 120, 110], "act_zoom_out") | {"need_bind": "bc.zbar"},
        tap([ZOOM_X + ZOOM_W - 110, ZOOM_Y + ZOOM_BAR_Y - 52, 120, 110], "act_zoom_in") | {"need_bind": "bc.zbar"},
        *[tap([ZOOM_X + t - 70, ZOOM_Y + ZOOM_BAR_Y - 30, 140, 116], "act_zoom%d" % z) | {"need_bind": "bc.zbar"}
          for z, t in enumerate(ZOOM_TICKS)],
        # Pins: the button opens the symbols under it; then a tap on the map puts a pin there
        # (on a pin: another symbol replaces it, the same symbol takes it away).
        {"type": "image", "rect": [PIN_X, PIN_Y, BTN_W, BTN_H], "src_bind": "bc.pin.btn", "need_bind": "bc.ui.ok"},
        tap([PIN_X, PIN_Y, BTN_W, BTN_H], "act_pin_mode") | {"need_bind": "bc.ui.ok"},
        *[{"type": "image", "rect": [PIN_X + BTN_W // 2 - PIN_CELL // 2, PIN_Y + BTN_H + 12 + k * (PIN_CELL + 8),
                                     PIN_CELL, PIN_CELL],
           "src_bind": "bc.pin.k%d" % k, "need_bind": "bc.pin.mode"} for k in range(PIN_TYPES)],
        *[tap([PIN_X + BTN_W // 2 - PIN_CELL // 2, PIN_Y + BTN_H + 12 + k * (PIN_CELL + 8), PIN_CELL, PIN_CELL],
              "act_pin_type%d" % k) | {"need_bind": "bc.pin.mode"} for k in range(PIN_TYPES)],
        {"type": "image", "rect": [(W - LABEL_W) // 2, H - LABEL_H - 26, LABEL_W, LABEL_H],
         "src": "module:bc:ui:pinhint", "need_bind": "bc.pin.mode"},
        # Debug page.
        {"type": "image", "rect": [W - BTN_W - 40, H - BTN_H - 34, BTN_W, BTN_H], "src": "module:bc:ui:btn:debug",
         "need_bind": "bc.ui.ok"},
        tap([W - BTN_W - 40, H - BTN_H - 34, BTN_W, BTN_H], "act_debug"),
        # Without the game's art: plain buttons.
        button(40, "-", "act_zoom_out", y=H - 90, w=120, h=70) | {"need_bind": "bc.ui.plain"},
        button(180, "+", "act_zoom_in", y=H - 90, w=120, h=70) | {"need_bind": "bc.ui.plain"},
        button(W - 260, "DEBUG", "act_debug", y=H - 90, w=220, h=70) | {"need_bind": "bc.ui.plain"},
        # Game over: the screen darkens and the card glows up and fades.
        rect([0, 0, W, H], "#B3000000", **fade("bc.go.show", 380)),
        {"type": "image", "rect": [(W - GO_W) // 2, (H - GO_H) // 2, GO_W, GO_H], "src_bind": "bc.go.key",
         **fade("bc.go.show", 380)},
        # Holding the title also opens the debug page.
        label([24, 14, 560, 110], text=" ", scale=5, color=GOLD, on_hold="act_debug", hold_ms=900),
    ],
}

# Debug page rows: (caption, kind, bind[, extra]) with kind "t" text, "i" int.
rows = [
    ("VERSION", "t", "bc.ver"),
    ("MODULE READY", "i", "module_ready"),
    ("MODULE ERROR", "t", "module_error_message"),
    ("BUILD MATCH", "i", "build_match"),
    ("STAGE", "i", "bc.stage"),
    ("STATUS", "t", "bc.status"),
    ("DATA SHIFT", "t", "bc.delta"),
    ("STATIC OFFSET", "i", "bc.sf"),
    ("SYSTEMS", "i", "bc.nsys"),
    ("SYSTEM NAMES", "t2", "bc.systems"),
    ("CHUNK X Y", "ii", "bc.chunk.x", "bc.chunk.y"),
    ("ROOM UID  EX UID", "ii", "bc.room.uid", "bc.ex.uid"),
    ("ROOM POS", "ii", "bc.room.x", "bc.room.y"),
    ("WORLD", "t", "bc.room.world"),
    ("ROOM NAME", "t", "bc.room.inst"),
    ("ROOM ID", "t", "bc.room.id"),
    ("MAP POS X100", "iii", "bc.mm.x", "bc.mm.y", "bc.mm.z"),
    ("FOG OK N MATCH ZONES", "iiii", "bc.fog.ok", "bc.fog.n", "bc.fog.matched", "bc.fog.zones"),
    ("FOG HERE", "t", "bc.fog.cur"),
    ("FOG KEYS", "t", "bc.fog.keys"),
    ("LAST SAVE  PINS", "t", "bc.savepins"),
    ("MARKERS  DEATH SPOT", "t", "bc.marks"),
    ("WORLD BIOMES ROOMS", "iii", "bc.world.ready", "bc.world.biomes", "bc.world.rooms"),
    ("WORLD LOG", "t2", "bc.world.log"),
    ("LEVEL MAP  LOAD MS  DRAW MS", "iii", "bc.lm.state", "bc.lm.ms", "bc.render.ms"),
    ("LEVEL MAP LOG", "t3", "bc.lm.log"),
    ("ZOOM POS GLITCH US", "iiii", "bc.zoom", "bc.pos.mode", "bc.pos.glitch", "bc.us"),
]

debug_widgets = [
    rect([0, 0, W, 72], BAR),
    {"type": "image", "rect": [(W - LABEL_W) // 2, 2, LABEL_W, LABEL_H], "src": "module:bc:ui:debug", "need_bind": "bc.ui.ok"},
    label([24, 20, 800, 40], text="DEBUG", scale=6, color=GOLD, need_bind="bc.ui.plain"),
]
y = 84
ROW = 29
for row in rows:
    caption, kind, binds = row[0], row[1], row[2:]
    debug_widgets.append(label([24, y, 380, ROW], text=caption, scale=3, color=DIM))
    if kind.startswith("t"):
        lines = int(kind[1:]) if len(kind) > 1 else 1
        debug_widgets.append(label([420, y, 800, ROW * lines], bind_text=binds[0], scale=3,
                                   wrap_width=800, max_lines=lines))
        y += ROW * lines
    else:
        for i, b in enumerate(binds):
            debug_widgets.append(value([420 + i * 200, y, 190, ROW], b, scale=3))
        y += ROW
assert y < 984, y
# Inventory debug page (stage 1 of the game menus: what the module reads).
inv_rows = [
    ("LEVEL", "t", "bc.inv.level"),
    ("EQUIPPED", "t2", "bc.inv.eq"),
    ("RELICS WORN", "t2", "bc.inv.relics"),
    ("WEAPONS", "t3", "bc.inv.t1"),
    ("ARCANAS", "t3", "bc.inv.t2"),
    ("RELICS", "t3", "bc.inv.t3"),
    ("ABILITIES", "t3", "bc.inv.t4"),
    ("CLUES", "t2", "bc.inv.t5"),
    ("QUEST  SKINS", "t2", "bc.inv.t6"),
    ("STATS", "t5", "bc.inv.stats"),
]
inv_widgets = [
    rect([0, 0, W, 72], BAR),
    label([24, 20, 800, 40], text="INVENTORY", scale=6, color=GOLD),
]
iy = 84
for caption, kind, bind in inv_rows:
    lines = int(kind[1:]) if len(kind) > 1 else 1
    inv_widgets.append(label([24, iy, 260, ROW], text=caption, scale=3, color=DIM))
    inv_widgets.append(label([290, iy, 930, ROW * lines], bind_text=bind, scale=3, wrap_width=930, max_lines=lines))
    iy += ROW * lines + 4
assert iy < 984, iy
inv_widgets += [
    rect([0, 984, W, 96], BAR),
    button(14, "DEBUG", "act_debug", w=300),
    button(W - 314, "MAP", "act_map", w=300),
]

debug_widgets += [
    rect([0, 984, W, 96], BAR),
    {"type": "image", "rect": [24, 1004, BTN_W, BTN_H], "src": "module:bc:ui:btn:map", "need_bind": "bc.ui.ok"},
    tap([24, 1004, BTN_W, BTN_H], "act_map") | {"need_bind": "bc.ui.ok"},
    button(14, "MAP", "act_map", w=300) | {"need_bind": "bc.ui.plain"},
    button(W - 314, "INVENTORY", "act_inv", w=300),
]

manifest = {
    "format": 1,
    "title_id": "0100BA4026E94000",
    "name": "Belmonts Curse map",
    "min_runtime": 22,
    "requires_module": True,
    "_note": "Castlevania: Belmont's Curse 1.1.0 (build 0F1169AA7D0FA452). The map, its icons "
             "and the title font are read by the module from the player's own romfs; nothing "
             "from the game is shipped.",
    "canvas_w": W,
    "canvas_h": H,
    "background": BG,
    "nav": False,
    "haptics": {"enabled": True, "respect_system": True, "tap": "light", "refused": "off"},
    "actions": {
        "act_zoom": {"kind": "module", "action": "zoom", "argument": 0},
        "act_zoom_in": {"kind": "module", "action": "zoom_in", "argument": 0},
        "act_zoom_out": {"kind": "module", "action": "zoom_out", "argument": 0},
        **{"act_zoom%d" % z: {"kind": "module", "action": "zoom_set", "argument": z} for z in range(ZOOM_LEVELS)},
        "act_zbar_open": {"kind": "module", "action": "zbar_open", "argument": 0},
        "act_zbar_close": {"kind": "module", "action": "zbar_close", "argument": 0},
        "act_pin_mode": {"kind": "module", "action": "pin_mode", "argument": 0},
        **{"act_pin_type%d" % k: {"kind": "module", "action": "pin_type", "argument": k} for k in range(PIN_TYPES)},
        "act_pin_tap": {"kind": "module", "action": "pin_tap", "argument": "$payload"},
        "act_debug": {"kind": "page", "page": "debug", "transition": "fade", "duration_ms": 150},
        "act_map": {"kind": "page", "page": "map", "transition": "fade", "duration_ms": 150},
        "act_inv": {"kind": "page", "page": "inventory", "transition": "fade", "duration_ms": 150},
    },
    "module_outputs": ["bc.*"],
    "pages": [map_page, {"id": "debug", "title": "Debug", "widgets": debug_widgets},
              {"id": "inventory", "title": "Inventory", "widgets": inv_widgets}],
}

out = Path(__file__).resolve().parent.parent / "package" / "dualscreen" / "manifest.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
print("wrote", out, "debug rows end at y =", y)
