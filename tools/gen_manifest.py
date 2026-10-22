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

MAP_TOP, MAP_H = 96, 888  # must match kMapTop / kMapH in 0100BA4026E94000.cpp


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


map_page = {
    "id": "map",
    "title": "Map",
    "widgets": [
        rect([0, 0, W, 92], BAR),
        label([24, 26, 860, 40], bind_text="bc.biome.label", scale=6, color=GOLD),
        label([900, 34, 120, 30], text="ROOM", scale=4, color=DIM),
        value([1040, 34, 180, 30], "bc.room.uid", scale=4, need_bind="bc.room.ok"),
        {"type": "image", "rect": [0, MAP_TOP, W, MAP_H], "src_bind": "bc.map.key",
         "need_bind": "bc.map.ready"},
        label([40, 480, 1160, 40], text="READING MAP DATA...", scale=5, color=DIM,
              need_bind="!bc.map.ready"),
        label([40, 540, 1160, 40], text="IF THIS STAYS, OPEN DEBUG", scale=4, color=DIM,
              need_bind="!bc.map.ready"),
        # Player marker: a white square with a red core, moved by the module.
        rect([0, MAP_TOP, 32, 32], "#FFFFFFFF", x_bind="bc.mk.x", y_bind="bc.mk.y",
             need_bind="bc.mk.show"),
        rect([6, MAP_TOP + 6, 20, 20], "#FFE0402F", x_bind="bc.mk.x", y_bind="bc.mk.y",
             need_bind="bc.mk.show"),
        rect([0, 984, W, 96], BAR),
        button(14, "ZOOM", "act_zoom"),
        button(218, "FOG", "act_unexp"),
        button(422, "- AREA", "act_prev"),
        button(626, "AREA +", "act_next"),
        button(830, "FOLLOW", "act_follow"),
        button(1034, "DEBUG", "act_debug"),
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
    ("SYSTEM NAMES", "t3", "bc.systems"),
    ("CHUNK X Y", "ii", "bc.chunk.x", "bc.chunk.y"),
    ("PREV CHUNK", "ii", "bc.prev.x", "bc.prev.y"),
    ("CHUNK MODE", "i", "bc.chunk.mode"),
    ("ROOM UID  EX UID", "ii", "bc.room.uid", "bc.ex.uid"),
    ("ROOM POS", "ii", "bc.room.x", "bc.room.y"),
    ("WORLD", "t", "bc.room.world"),
    ("ROOM NAME", "t", "bc.room.inst"),
    ("ROOM ID", "t", "bc.room.id"),
    ("MAP POS X100", "iii", "bc.mm.x", "bc.mm.y", "bc.mm.z"),
    ("FOG OK N MATCH", "iii", "bc.fog.ok", "bc.fog.n", "bc.fog.matched"),
    ("FOG HERE", "t", "bc.fog.cur"),
    ("FOG KEYS", "t2", "bc.fog.keys"),
    ("WORLD BIOMES ROOMS", "iii", "bc.world.ready", "bc.world.biomes", "bc.world.rooms"),
    ("WORLD LOG", "t2", "bc.world.log"),
    ("ZOOM FOG FOLLOW", "iii", "bc.zoom", "bc.unexp", "bc.follow"),
    ("SAMPLE US", "i", "bc.us"),
]

debug_widgets = [
    rect([0, 0, W, 72], BAR),
    label([24, 20, 800, 40], text="DEBUG", scale=6, color=GOLD),
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
            debug_widgets.append(value([420 + i * 220, y, 200, ROW], b, scale=3))
        y += ROW
assert y < 984, y
debug_widgets += [rect([0, 984, W, 96], BAR), button(14, "MAP", "act_map", w=300)]

manifest = {
    "format": 1,
    "title_id": "0100BA4026E94000",
    "name": "Belmonts Curse map",
    "min_runtime": 16,
    "requires_module": True,
    "_note": "Castlevania: Belmont's Curse 1.1.0 (build 0F1169AA7D0FA452). The map is drawn by "
             "the module from the player's own romfs; nothing from the game is shipped.",
    "canvas_w": W,
    "canvas_h": H,
    "background": BG,
    "nav": False,
    "haptics": {"enabled": True, "respect_system": True, "tap": "light", "refused": "off"},
    "actions": {
        "act_zoom": {"kind": "module", "action": "zoom", "argument": 0},
        "act_unexp": {"kind": "module", "action": "unexp", "argument": 0},
        "act_prev": {"kind": "module", "action": "biome_prev", "argument": 0},
        "act_next": {"kind": "module", "action": "biome_next", "argument": 0},
        "act_follow": {"kind": "module", "action": "follow", "argument": 0},
        "act_debug": {"kind": "page", "page": "debug", "transition": "fade", "duration_ms": 150},
        "act_map": {"kind": "page", "page": "map", "transition": "fade", "duration_ms": 150},
    },
    "module_outputs": ["bc.*"],
    "pages": [map_page, {"id": "debug", "title": "Debug", "widgets": debug_widgets}],
}

out = Path(__file__).resolve().parent.parent / "package" / "dualscreen" / "manifest.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
print("wrote", out, "debug rows end at y =", y)
