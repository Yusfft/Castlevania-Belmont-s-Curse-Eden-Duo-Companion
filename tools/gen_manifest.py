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
MARKER = {"bc.mk.z0": 30, "bc.mk.z1": 35, "bc.mk.z2": 70}
TITLE_W, TITLE_H = 640, 344  # must match kTitleW / kTitleH in 0100BA4026E94000.cpp
ZOOM_W, ZOOM_H = 640, 130     # bc_ui::ZoomBarLayout
ZOOM_X, ZOOM_Y = (W - ZOOM_W) // 2, H - ZOOM_H - 30
ZOOM_TICKS = [130, 320, 510]  # ZoomBarLayout::Tick(0..2)
ZOOM_BAR_Y = 44
BTN_W, BTN_H = 170, 56        # kButtonW / kButtonH
LABEL_W, LABEL_H = 700, 70    # kLabelW / kLabelH


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
        # The map screen's gold-edged frame (game art, drawn by the module).
        {"type": "image", "rect": [0, 0, W, H], "src": "module:bc:ui:frame", "need_bind": "bc.ui.ok"},
        {"type": "image", "rect": [(W - LABEL_W) // 2, 505, LABEL_W, LABEL_H], "src": "module:bc:ui:wait",
         "need_bind": "bc.ui.wait"},
        # Placed by hand (20 glyphs of 16 px): a centred label opens shifted on some hosts.
        label([W // 2 - 160, 520, 360, 40], text="WAITING FOR THE GAME", scale=4, color=DIM,
              need_bind="bc.ui.waitplain"),
        # Level banner with the area and district names (a plain label until the art is read).
        {"type": "image", "rect": [14, 10, TITLE_W, TITLE_H], "src_bind": "bc.title.key",
         "need_bind": "bc.title.img"},
        label([36, 30, 900, 40], bind_text="bc.title", scale=5, color=GOLD,
              need_bind="!bc.title.img"),
        # Zoom scale: - and + buttons, or tap a level's name.
        {"type": "image", "rect": [ZOOM_X, ZOOM_Y, ZOOM_W, ZOOM_H], "src_bind": "bc.ui.zoom",
         "need_bind": "bc.ui.ok"},
        tap([ZOOM_X - 10, ZOOM_Y + ZOOM_BAR_Y - 52, 104, 104], "act_zoom_out"),
        tap([ZOOM_X + ZOOM_W - 94, ZOOM_Y + ZOOM_BAR_Y - 52, 104, 104], "act_zoom_in"),
        *[tap([ZOOM_X + t - 60, ZOOM_Y + ZOOM_BAR_Y + 20, 120, 64], "act_zoom%d" % z)
          for z, t in enumerate(ZOOM_TICKS)],
        # Debug page.
        {"type": "image", "rect": [W - BTN_W - 40, H - BTN_H - 34, BTN_W, BTN_H], "src": "module:bc:ui:btn:debug",
         "need_bind": "bc.ui.ok"},
        tap([W - BTN_W - 40, H - BTN_H - 34, BTN_W, BTN_H], "act_debug"),
        # Without the game's art: plain buttons.
        button(40, "-", "act_zoom_out", y=H - 90, w=120, h=70) | {"need_bind": "bc.ui.plain"},
        button(180, "+", "act_zoom_in", y=H - 90, w=120, h=70) | {"need_bind": "bc.ui.plain"},
        button(W - 260, "DEBUG", "act_debug", y=H - 90, w=220, h=70) | {"need_bind": "bc.ui.plain"},
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
    ("LAST SAVE", "t", "bc.save"),
    ("MARKERS  DEATH SPOT", "t", "bc.marks"),
    ("WORLD BIOMES ROOMS", "iii", "bc.world.ready", "bc.world.biomes", "bc.world.rooms"),
    ("WORLD LOG", "t2", "bc.world.log"),
    ("LEVEL MAP  LOAD MS  DRAW MS", "iii", "bc.lm.state", "bc.lm.ms", "bc.render.ms"),
    ("LEVEL MAP LOG", "t3", "bc.lm.log"),
    ("ZOOM  POS MODE  SAMPLE US", "iii", "bc.zoom", "bc.pos.mode", "bc.us"),
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
debug_widgets += [
    rect([0, 984, W, 96], BAR),
    {"type": "image", "rect": [24, 1004, BTN_W, BTN_H], "src": "module:bc:ui:btn:map", "need_bind": "bc.ui.ok"},
    tap([24, 1004, BTN_W, BTN_H], "act_map") | {"need_bind": "bc.ui.ok"},
    button(14, "MAP", "act_map", w=300) | {"need_bind": "bc.ui.plain"},
]

manifest = {
    "format": 1,
    "title_id": "0100BA4026E94000",
    "name": "Belmonts Curse map",
    "min_runtime": 16,
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
        "act_zoom0": {"kind": "module", "action": "zoom_set", "argument": 0},
        "act_zoom1": {"kind": "module", "action": "zoom_set", "argument": 1},
        "act_zoom2": {"kind": "module", "action": "zoom_set", "argument": 2},
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
