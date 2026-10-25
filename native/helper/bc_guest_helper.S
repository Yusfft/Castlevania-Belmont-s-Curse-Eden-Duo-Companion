// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guest helper for Castlevania: Belmont's Curse 1.1.0 (build 0F1169AA7D0FA452), installed by the
// package's load plan (format 2, tools/gen_load_plan.py). The load plan replaces the first
// instruction of MotherBase.ApplicationFlowManager.ApplicationFlowManager::Update with a branch
// here; the helper runs that instruction, serves at most one request from the mailbox and
// returns to Update+4. So the game's own functions run on the game's own thread, once a frame,
// the way its menus call them.
//
// Mailbox (written by the module through the host's mailbox accessors):
//   0x00 u32 magic 'BCH1'      0x04 u32 frames seen (helper)   0x08 u32 request (module)
//   0x0C u32 command           0x10 u32 token = host epoch     0x14 s32 integer argument
//   0x18 f32 x, 0x1C f32 y     0x20 target (Inventory / MapMarkersComponent)
//   0x28 object 1              0x30 object 2                   0x38 object 3
//   0x40..0x58 the classes the module saw for target and objects 1..3
//   0x60 u32 done (helper)     0x64 u32 result                 0x68 s64 value
//   0xFFC host epoch (bumped by Eden on pause/resume: a request made before it is refused)
//
// Commands (result 0 = done):
//   1 equip weapon  (Inventory, WeaponItemData in its inventory)
//   2 equip arcana  (Inventory, SpellItemData in its inventory)
//   3 equip relic   (Inventory, CharmItemData in its inventory, slot) -> value = slot used
//   4 remove relic  (Inventory, slot)
//   5 add marker    (MapMarkersComponent, LevelID, type guid string, x, y, zone) -> value = id
//   6 remove marker (MapMarkersComponent, LevelID, marker id)
//   7 change marker (MapMarkersComponent, LevelID, MapMarkerData, MapMarkerTypeID)
// Results: 1 bad object, 2 bad command, 3 stale request, 4 game busy (loading), 5 item not
// owned, 6 bad slot, 7 allocation failed.
//
// Every object is checked against the class the module saw (and, for items, against the item
// class through the parent chain) before any call, so nothing is called with a freed object or
// one of the wrong type. Calls that could throw are avoided (a relic slot is always given and
// checked against the slots array).
//
// `MB` is the mailbox and `M` the main module's base: the load plan turns their relocations into
// the "mailbox" and "main" targets. Main offsets are build 1.1.0's.

    .set MAGIC, 0x31484342                  // 'BCH1'
    .set AFM_TYPEINFO, 0x6D3F780            // ApplicationFlowManager TypeInfo slot
    .set MARKERDATA_TYPEINFO, 0x6D444D8     // MapMarkerData TypeInfo slot
    .set FN_INIT_METADATA, 0x5BE410         // il2cpp: resolve a metadata usage slot
    .set FN_OBJECT_NEW, 0x5BE780            // il2cpp: new object of a class
    .set FN_MARKERDATA_CTOR, 0x1C311A0      // MapMarkerData(Vector2 position, int zone, string type)
    .set FN_ADD_MARKER, 0x1C2E7A0           // MapMarkersComponent.AddMarker(LevelID, MapMarkerData)
    .set FN_REMOVE_MARKER, 0x1C2EF60        // MapMarkersComponent.RemoveMarker(LevelID, int)
    .set FN_CHANGE_MARKER, 0x1C2FDB0        // MapMarkersComponent.ChangeMarkerType(LevelID, data, type)
    .set FN_EQUIP_WEAPON, 0x1F546E0         // Inventory.EquipWeaponFromInventory(WeaponItemData)
    .set FN_EQUIP_SPELL, 0x1F547B0          // Inventory.EquipSpellFromInventory(SpellItemData)
    .set FN_EQUIP_CHARM, 0x1F54880          // Inventory.EquipCharmFromInventory(CharmItemData, int)
    .set FN_UNEQUIP_CHARM, 0x1F53520        // Inventory.UnequipCharm(int)
    .set UPDATE_RESUME, 0x40D4F04           // ApplicationFlowManager::Update + 4

    .text
    .globl bc_helper
bc_helper:
    stp     x29, x30, [sp, #-0x30]!         // Update's first instruction, displaced
    sub     sp, sp, #0x40
    stp     x0, x1, [sp]
    stp     x19, x20, [sp, #0x10]
    stp     x21, x22, [sp, #0x20]
    str     x30, [sp, #0x30]

    adrp    x19, MB
    add     x19, x19, :lo12:MB
    ldr     w9, [x19, #4]                   // frames seen: tells the module the helper runs
    add     w9, w9, #1
    str     w9, [x19, #4]
    ldr     w9, [x19]
    movz    w10, #0x4342
    movk    w10, #0x3148, lsl #16
    cmp     w9, w10
    b.ne    .Lexit
    add     x9, x19, #8
    ldar    w20, [x9]                       // request
    ldr     w9, [x19, #0x60]
    cmp     w9, w20
    b.eq    .Lexit                          // nothing new

    // A request made before the last pause/resume is stale.
    mov     w0, #3
    ldr     w9, [x19, #0x10]
    add     x10, x19, #0xFFC
    ldar    w10, [x10]                      // (0 until Eden's first Run: then it matches too)
    cmp     w9, w10
    b.ne    .Lfinish

    // Not while the game loads a scene (ApplicationFlowManager._isLoading, first static).
    mov     w0, #4
    adrp    x9, M + AFM_TYPEINFO
    ldr     x9, [x9, :lo12:M + AFM_TYPEINFO]
    cbz     x9, .Lfinish
    tbnz    x9, #0, .Lfinish                // TypeInfo not resolved yet
    ldr     x10, [x9, #0xB8]                // static fields
    cbz     x10, .Lfinish
    ldrb    w11, [x10]
    cbnz    w11, .Lfinish

    // The target is the object the module saw (same class).
    mov     w0, #1
    ldr     x21, [x19, #0x20]
    cbz     x21, .Lfinish
    ldr     x9, [x21]
    ldr     x10, [x19, #0x40]
    cbz     x10, .Lfinish
    cmp     x9, x10
    b.ne    .Lfinish

    ldr     w9, [x19, #0xC]
    cmp     w9, #1
    b.eq    .Lequip_weapon
    cmp     w9, #2
    b.eq    .Lequip_spell
    cmp     w9, #3
    b.eq    .Lequip_charm
    cmp     w9, #4
    b.eq    .Lunequip_charm
    cmp     w9, #5
    b.eq    .Ladd_marker
    cmp     w9, #6
    b.eq    .Lremove_marker
    cmp     w9, #7
    b.eq    .Lchange_marker
    mov     w0, #2
    b       .Lfinish

// ---- equipment

.Lequip_weapon:
    bl      .Lcheck_item
    cbnz    w0, .Lfinish
    mov     x0, x21
    mov     x1, x22
    mov     x2, xzr
    bl      M + FN_EQUIP_WEAPON
    b       .Ldone_zero

.Lequip_spell:
    bl      .Lcheck_item
    cbnz    w0, .Lfinish
    mov     x0, x21
    mov     x1, x22
    mov     x2, xzr
    bl      M + FN_EQUIP_SPELL
    b       .Ldone_zero

.Lequip_charm:
    bl      .Lcheck_item
    cbnz    w0, .Lfinish
    bl      .Lcheck_slot
    cbnz    w0, .Lfinish
    mov     x0, x21
    mov     x1, x22
    ldr     w2, [x19, #0x14]
    mov     x3, xzr
    bl      M + FN_EQUIP_CHARM
    sxtw    x9, w0
    str     x9, [x19, #0x68]
    b       .Ldone

.Lunequip_charm:
    bl      .Lcheck_slot
    cbnz    w0, .Lfinish
    mov     x0, x21
    ldr     w1, [x19, #0x14]
    mov     x2, xzr
    bl      M + FN_UNEQUIP_CHARM
    b       .Ldone_zero

// ---- map markers (object 1 = the LevelID of the map)

.Ladd_marker:
    mov     w0, #1
    ldr     x2, [x19, #0x28]
    ldr     x3, [x19, #0x48]
    bl      .Lexact
    cbz     w4, .Lfinish
    ldr     x2, [x19, #0x30]                // the type's guid string
    ldr     x3, [x19, #0x50]
    bl      .Lexact
    cbz     w4, .Lfinish
    adrp    x0, M + MARKERDATA_TYPEINFO
    add     x0, x0, :lo12:M + MARKERDATA_TYPEINFO
    bl      M + FN_INIT_METADATA
    mov     w0, #7
    adrp    x9, M + MARKERDATA_TYPEINFO
    ldr     x9, [x9, :lo12:M + MARKERDATA_TYPEINFO]
    cbz     x9, .Lfinish
    tbnz    x9, #0, .Lfinish
    mov     x0, x9
    bl      M + FN_OBJECT_NEW
    cbz     x0, .Lalloc_failed
    mov     x22, x0
    ldr     s0, [x19, #0x18]
    ldr     s1, [x19, #0x1C]
    ldr     w1, [x19, #0x14]
    ldr     x2, [x19, #0x30]
    mov     x3, xzr
    bl      M + FN_MARKERDATA_CTOR
    mov     x0, x21
    ldr     x1, [x19, #0x28]
    mov     x2, x22
    mov     x3, xzr
    bl      M + FN_ADD_MARKER
    ldrsw   x9, [x22, #0x10]                // the id AddMarker gave it
    str     x9, [x19, #0x68]
    b       .Ldone
.Lalloc_failed:
    mov     w0, #7
    b       .Lfinish

.Lremove_marker:
    mov     w0, #1
    ldr     x2, [x19, #0x28]
    ldr     x3, [x19, #0x48]
    bl      .Lexact
    cbz     w4, .Lfinish
    mov     x0, x21
    ldr     x1, [x19, #0x28]
    ldr     w2, [x19, #0x14]
    mov     x3, xzr
    bl      M + FN_REMOVE_MARKER
    b       .Ldone_zero

.Lchange_marker:
    mov     w0, #1
    ldr     x2, [x19, #0x28]
    ldr     x3, [x19, #0x48]
    bl      .Lexact
    cbz     w4, .Lfinish
    ldr     x2, [x19, #0x30]
    ldr     x3, [x19, #0x50]
    bl      .Lexact
    cbz     w4, .Lfinish
    ldr     x2, [x19, #0x38]
    ldr     x3, [x19, #0x58]
    bl      .Lexact
    cbz     w4, .Lfinish
    mov     x0, x21
    ldr     x1, [x19, #0x28]
    ldr     x2, [x19, #0x30]
    ldr     x3, [x19, #0x38]
    mov     x4, xzr
    bl      M + FN_CHANGE_MARKER
    b       .Ldone_zero

// ---- end of a request

.Ldone_zero:
    str     xzr, [x19, #0x68]
.Ldone:
    mov     w0, #0
    b       .Lpublish
.Lfinish:                                   // w0 = result, no value
    str     xzr, [x19, #0x68]
.Lpublish:
    str     w0, [x19, #0x64]
    add     x9, x19, #0x60
    stlr    w20, [x9]                       // done = request: the module may read the result
.Lexit:
    ldr     x30, [sp, #0x30]
    ldp     x21, x22, [sp, #0x20]
    ldp     x19, x20, [sp, #0x10]
    ldp     x0, x1, [sp]
    add     sp, sp, #0x40
    b       M + UPDATE_RESUME

// ---- checks (leaf routines; x19 = mailbox, x21 = Inventory)

// Object 1 is an item owned by the Inventory, of the item class the module gave (parent chain).
// Out: w0 = 0 ok, else the result code; x22 = the item.
.Lcheck_item:
    mov     w0, #1
    ldr     x22, [x19, #0x28]
    cbz     x22, 9f
    ldr     x3, [x19, #0x48]
    cbz     x3, 9f
    ldr     x5, [x22]
    mov     x6, #8
1:  cmp     x5, x3
    b.eq    2f
    ldr     x5, [x5, #0x58]                 // Il2CppClass.parent
    cbz     x5, 9f
    subs    x6, x6, #1
    b.ne    1b
    b       9f
2:  mov     w0, #5                          // Inventory._itemsInInventory (List<ItemData>)
    ldr     x9, [x21, #0x20]
    cbz     x9, 9f
    ldrsw   x10, [x9, #0x18]                // size
    ldr     x11, [x9, #0x10]                // items
    cbz     x11, 9f
    ldr     x12, [x11, #0x18]               // array length
    cmp     x10, x12
    csel    x10, x10, x12, lt
    mov     x13, #0
3:  cmp     x13, x10
    b.ge    9f
    cmp     x13, #2048
    b.ge    9f
    add     x14, x11, #0x20
    ldr     x15, [x14, x13, lsl #3]
    cmp     x15, x22
    b.eq    4f
    add     x13, x13, #1
    b       3b
4:  mov     w0, #0
9:  ret

// The integer argument is a relic slot of the Inventory (Inventory._equippedCharms).
// Out: w0 = 0 ok, else 6.
.Lcheck_slot:
    mov     w0, #6
    ldr     x9, [x21, #0x40]
    cbz     x9, 9f
    ldr     x10, [x9, #0x18]
    ldrsw   x11, [x19, #0x14]
    tbnz    x11, #63, 9f
    cmp     x11, x10
    b.ge    9f
    mov     w0, #0
9:  ret

// x2 is an object of exactly class x3. Out: w4 = 1 yes, 0 no.
.Lexact:
    mov     w4, #0
    cbz     x2, 9f
    cbz     x3, 9f
    ldr     x5, [x2]
    cmp     x5, x3
    b.ne    9f
    mov     w4, #1
9:  ret
