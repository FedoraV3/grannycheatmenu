"""
Annotate GameAssembly.dll's IDB with everything grannycheat relies on.

Run inside IDA: File -> Script file... (Alt+F7), pick this file. Safe to
re-run; it just overwrites the same names, prototypes and comments.

Run Il2CppDumper's own ida_with_struct_py3.py FIRST if you haven't. That
mass-renames all ~50k functions and applies the IL2CPP structs from
script.json. This script only covers the handful of functions the cheat
actually calls or hooks, and records what was learned about them by
decompiling -- the parts a bulk dump import can't tell you: calling
conventions, argument shapes, and what the field writes actually mean.

Addresses here are RVAs and get rebased onto whatever imagebase the IDB is
loaded at, so this keeps working if the module gets rebased.

Two calling conventions show up, and mixing them up crashes the game:

  * Game assembly (Assembly-CSharp: AI_Granny, PlayerStatus, ItemSpawn)
    takes ONLY the instance pointer. IL2CPP elided the trailing MethodInfo*
    because none of these are generic or virtual-dispatch calls.

  * UnityEngine (Camera, Transform, Component, GameObject) DOES take a
    trailing MethodInfo*, and NULL is fine for it -- AI_Granny::StopAI
    itself calls get_transform/get_position that way.

Win64 ABI: a struct bigger than 8 bytes comes back through a hidden first
pointer argument, so for Vector3 (12 bytes) and Matrix4x4 (64 bytes) the
return buffer is arg 1 and `this` shifts to arg 2.
"""

import ida_auto
import ida_funcs
import ida_kernwin
import ida_name
import ida_typeinf
import idaapi

# (rva, name, prototype or None, comment or None)
ANNOTATIONS = [
    # ---- Assembly-CSharp: single argument, no trailing MethodInfo* -------
    (
        0x1BDCC0,
        "AI_Granny__FixedUpdate",
        "void __fastcall f(void *__this);",
        "AI_Granny::FixedUpdate -- the real per-tick AI driver (2917 insns, 81\n"
        "callees). Everything else on this class is a small helper next to it;\n"
        "the actual chase/pursuit logic lives in here, NOT in ChaseAction.\n"
        "\n"
        "grannycheat hooks this rather than patching it, purely to capture the\n"
        "live instance pointer every tick. IL2CPP reallocates AI_Granny on the\n"
        "GC heap on every level reload, so a cached pointer goes stale but a\n"
        "hook self-heals within one tick.\n"
        "\n"
        "Zero static callers is expected -- Unity dispatches FixedUpdate through\n"
        "its own message system, not a direct call site.",
    ),
    (
        0x1BDA40,
        "AI_Granny__ChaseAction",
        "void __fastcall f(void *__this);",
        "AI_Granny::ChaseAction -- only 0x21 bytes. Despite the name this is a\n"
        "state-ENTER stub, not the chase loop. Decoded against dump.cs:\n"
        "\n"
        "  *(DWORD*)(this+396) = 256   -> IsFollowingSound(0x18C) = 0\n"
        "                                 IsChasing      (0x18D) = 1\n"
        "                                 IsWalking      (0x18E) = 0\n"
        "                                 IsIdle         (0x18F) = 0\n"
        "  *(QWORD*)(this+412) = 0     -> TimerChoosing  (0x19C) = 0\n"
        "                                 SearchTimer    (0x1A0) = 0\n"
        "  *(DWORD*)(this+640) = 0     -> TimerBedCaught (0x280) = 0\n"
        "  *(BYTE *)(this+394) = 0     -> IsSearching    (0x18A) = 0\n"
        "\n"
        "So: 'switch her into chase state and reset the timers'.",
    ),
    (
        0x1C2100,
        "AI_Granny__StopAI",
        "void __fastcall f(void *__this);",
        "AI_Granny::StopAI -- tears down her components (SetActive(false)/Stop()\n"
        "style calls on the refs at +216, +512/+520, +168, +48/+40, +264) and\n"
        "sets byte +392 = 1. Confirmed working in game: calling this freezes her\n"
        "in place.\n"
        "\n"
        "One-way as far as we know -- no function has been found that undoes it.\n"
        "\n"
        "Also the proof that engine getters take a NULL MethodInfo*: it calls\n"
        "Component::get_transform(this, 0) then Transform::get_position(buf, tf, 0)\n"
        "and stores the result into CurrentTarget (+0xB0).",
    ),
    (
        0x1C1510,
        "AI_Granny__MurderNoiseObjs",
        "void __fastcall f(void *__this);",
        "AI_Granny::MurderNoiseObjs -- the helper both StopAI and\n"
        "ResetAIDecision call first (shows up as sub_1801C1510 before naming).\n"
        "Uses GameObject::FindGameObjectsWithTag, so noise objects ARE tagged\n"
        "-- but items are not, which rules out tag-based item enumeration.",
    ),
    (
        0x1C1930,
        "AI_Granny__ResetAIDecision",
        "void __fastcall f(void *__this);",
        "AI_Granny::ResetAIDecision -- same shape as StopAI but touches fewer\n"
        "fields and clears +392/+312 instead of setting them. NOT a 'reset to\n"
        "patrol' despite the name; it reads as component teardown.\n"
        "\n"
        "grannycheat calls this on the live AI_Granny when a catch fires. Note\n"
        "it must be given an AI_Granny instance -- passing the PlayerStatus*\n"
        "from a PlayerStatus hook corrupts memory, since every field offset\n"
        "then lands on a different object's layout.",
    ),
    (
        0x1C1DE0,
        "AI_Granny__SmackTimer",
        "void __fastcall f(void *__this);",
        "AI_Granny::SmackTimer -- lazily inits a static (byte_180CE4221 guard),\n"
        "fetches a singleton, stashes `this` at +32 of the result and returns it.\n"
        "Reads as creating/returning a timer or coroutine handle rather than\n"
        "ticking one.",
    ),
    (
        0x24A9A0,
        "PlayerStatus__GrannyCaughtYou",
        "void __fastcall f(void *__this);",
        "PlayerStatus::GrannyCaughtYou -- runs when she catches you upright.\n"
        "Hooked by grannycheat to prevent the catch resolving. Skipping the\n"
        "original also skips the camera lock/cutscene, so she just re-enters her\n"
        "attack animation on a loop instead of killing you.\n"
        "\n"
        "NOTE: __this is a PlayerStatus*, not an AI_Granny*.",
    ),
    (
        0x24A8C0,
        "PlayerStatus__GrannyCaughtYouBed",
        "void __fastcall f(void *__this);",
        "PlayerStatus::GrannyCaughtYouBed -- catch while hiding under a bed.",
    ),
    (
        0x24AA20,
        "PlayerStatus__KnockDeath",
        "void __fastcall f(void *__this);",
        "PlayerStatus::KnockDeath -- knockout/trap death path.",
    ),
    (
        0x24B350,
        "PlayerStatus__NormalDeath",
        "void __fastcall f(void *__this);",
        "PlayerStatus::NormalDeath -- generic death path.",
    ),
    (
        0x21F2C0,
        "ItemSpawn__Update",
        "void __fastcall f(void *__this);",
        "ItemSpawn::Update -- a ONE-SHOT SELF-DESTRUCTING DROPPER, not a\n"
        "per-frame tick and not an item registry. Decompiled:\n"
        "\n"
        "  if (!Spawned) {\n"
        "      item = <one field picked by CountItem: 1.0=crossbow ...\n"
        "              20.0=melon ... 55.0=fuse>;\n"
        "      SetActive(item, true);\n"
        "      AddForce(item.rigidbody, forward * DropForceItem);\n"
        "      set_parent(item.transform, null);\n"
        "      Spawned = true;\n"
        "      Destroy(this.gameObject);   // <-- destroys ITSELF\n"
        "  }\n"
        "\n"
        "PickRay::CheckItemDropping is what creates these: on a player drop it\n"
        "Instantiates the ItemDrop prefab, GetComponent<ItemSpawn>()s it, and\n"
        "writes CountItem to choose which item. So an instance exists only for\n"
        "the single frame of one drop.\n"
        "\n"
        "Consequences, learned the hard way:\n"
        "  * Hooking this yields a tick only per pickup/drop, never per frame.\n"
        "  * Latching `this` is a use-after-free -- the object is destroyed at\n"
        "    the end of the call. Once the GC reuses the block, a m_CachedPtr\n"
        "    liveness check passes on unrelated data and the 55 item fields\n"
        "    read as garbage pointers. That crashed the game on game version\n"
        "    1.8, where many drops happen at level start.\n"
        "  * The 55 GameObject* fields (crossbow +0x28 .. fuse +0x1D8, stride\n"
        "    8) are this dropper's own references, NOT the level's items.\n"
        "\n"
        "For enumerating world items, this class is a dead end -- so are\n"
        "Inventory/ItemDefs, which describe held items (name, hand model,\n"
        "pickup sound), not world positions.",
    ),
    # ---- UnityEngine: trailing MethodInfo*, NULL is accepted -------------
    (
        0x71D550,
        "Component__get_transform",
        "void *__fastcall f(void *__this, void *method);",
        "Component::get_transform. MethodInfo* may be NULL -- AI_Granny::StopAI\n"
        "calls it that way itself.",
    ),
    (
        0x747310,
        "Transform__get_position",
        "void *__fastcall f(void *ret_vector3, void *__this, void *method);",
        "Transform::get_position. Vector3 is 12 bytes, so under the Win64 ABI it\n"
        "returns through a hidden first pointer and `this` becomes arg 2.",
    ),
    (
        0x6FE450,
        "Camera__get_main",
        "void *__fastcall f(void *method);",
        "Camera::get_main (static, so MethodInfo* is the only argument).\n"
        "\n"
        "WARNING: returns NULL in this game. Camera.main only resolves a camera\n"
        "tagged 'MainCamera' and Granny doesn't tag its camera. grannycheat\n"
        "falls back to AI_Granny(+0xE8) -> PlayerStatus(+0x110 PlayerCam), which\n"
        "is two pointer derefs and needs no tag or IL2CPP call at all.",
    ),
    (
        0x6FEA00,
        "Camera__get_worldToCameraMatrix",
        "void *__fastcall f(void *ret_matrix4x4, void *__this, void *method);",
        "Camera::get_worldToCameraMatrix. Matrix4x4 is 64 bytes -> hidden return\n"
        "pointer as arg 1. Unity stores it column-major: element (row, col) is\n"
        "raw[col * 4 + row].",
    ),
    (
        0x6FE6B0,
        "Camera__get_projectionMatrix",
        "void *__fastcall f(void *ret_matrix4x4, void *__this, void *method);",
        "Camera::get_projectionMatrix. Same shape as get_worldToCameraMatrix.\n"
        "view-projection = projection * worldToCamera.",
    ),
    (
        0x720600,
        "GameObject__get_transform",
        "void *__fastcall f(void *__this, void *method);",
        "GameObject::get_transform. GameObject is not a Component, so it has its\n"
        "own getter -- don't reuse Component::get_transform for it.",
    ),
    (
        0x720460,
        "GameObject__get_activeInHierarchy",
        "bool __fastcall f(void *__this, void *method);",
        "GameObject::get_activeInHierarchy. Used to skip items already picked up,\n"
        "since the game deactivates their GameObject.",
    ),
]

# AI_Granny instance field offsets worth having to hand while reversing.
# Attached as a comment on the class's FixedUpdate rather than built into a
# struct -- Il2CppDumper's il2cpp.h already has the full type if you want it.
FIELD_NOTE_RVA = 0x1BDCC0
FIELD_NOTE = """
AI_Granny fields used by grannycheat (offsets into the instance):
  +0x94  float  Walk_Speed          +0x164 bool   IsBlind (game's own flag)
  +0x98  float  Run_Speed           +0x168 float  BlindTimer
  +0xA8  void*  Agent (NavMeshAgent) +0x188 bool   CaughtPlayer
  +0xB0  Vec3   CurrentTarget       +0x18A bool   IsSearching
  +0xD0  void*  Player (Transform)  +0x18B bool   IsAngry
  +0xE8  void*  PlayerStatus        +0x18C bool   IsFollowingSound
  +0x138 bool   IsDying             +0x18D bool   IsChasing
  +0x1BC float  DistanceFromPlayer  +0x18E bool   IsWalking
  +0x288 Vec3   PlayerPos           +0x18F bool   IsIdle

PlayerStatus:
  +0x110 void*  PlayerCam (Camera)  <- the reliable camera source
"""


def apply_prototype(ea, prototype):
    """Apply a C declaration to `ea`. Returns True if it took."""
    tif = ida_typeinf.tinfo_t()
    if not ida_typeinf.parse_decl(tif, None, prototype, ida_typeinf.PT_SIL):
        return False
    return ida_typeinf.apply_tinfo(ea, tif, ida_typeinf.TINFO_DEFINITE)


def main():
    ida_auto.auto_wait()

    base = idaapi.get_imagebase()
    print("[grannycheat] imagebase 0x%X" % base)

    renamed = typed = commented = missing = 0

    for rva, name, prototype, comment in ANNOTATIONS:
        ea = base + rva

        func = ida_funcs.get_func(ea)
        if not func or func.start_ea != ea:
            print("[grannycheat] !! no function at 0x%X (rva 0x%X) -- wrong "
                  "binary version?" % (ea, rva))
            missing += 1
            continue

        if ida_name.set_name(ea, name, ida_name.SN_NOCHECK):
            renamed += 1
        else:
            print("[grannycheat] !! rename failed at 0x%X -> %s" % (ea, name))

        if prototype and apply_prototype(ea, prototype):
            typed += 1

        if comment:
            body = comment
            if rva == FIELD_NOTE_RVA:
                body = body + "\n" + FIELD_NOTE
            ida_funcs.set_func_cmt(func, body, True)
            commented += 1

    print("[grannycheat] renamed %d, typed %d, commented %d, missing %d"
          % (renamed, typed, commented, missing))
    if missing:
        print("[grannycheat] missing entries mean the offsets no longer match "
              "this GameAssembly.dll -- re-dump with Il2CppDumper and update "
              "include/offsets.h too.")

    ida_kernwin.refresh_idaview_anyway()


if __name__ == "__main__":
    main()
