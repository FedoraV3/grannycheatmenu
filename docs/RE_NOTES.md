# Granny: Legacy — reverse engineering notes

Everything established about this binary while building the cheat menu in
this repo, written for whoever picks it up next. The immediate next job is a
TAS tool — frame stepping, speed control, deterministic input — so the
sections most relevant to that are called out.

**Binary:** `GameAssembly.dll`, Granny: Legacy **v1.8.9**, x64, IL2CPP.
IDA imagebase `0x180000000`; every RVA below is relative to the module base,
so `GetModuleHandleW(L"GameAssembly.dll") + RVA` is the runtime address.

Everything here was verified against **this exact build**. A game update
rebuilds `GameAssembly.dll` and every RVA shifts. Nothing in this file
survives that — see *Surviving a game update* at the end.

---

## 1. Ground rules for this binary

These cost real time to learn. Read them before touching anything.

### Calling conventions — there are two

- **Game assembly methods** (`Assembly-CSharp`: `PickRay`, `AI_Granny`,
  `MobileFPS`, …) take **only the instance**. IL2CPP elided the trailing
  `MethodInfo*` because none are generic or virtual-dispatched.
  `void __fastcall f(void *this)`.
- **UnityEngine methods** take a **trailing `MethodInfo*`**, and `NULL` is
  accepted for it. Confirmed from the game's own call sites, e.g.
  `AI_Granny::StopAI` calls `Component::get_transform(a1, 0)`.
- **Generic methods** (`GetComponent<T>`, `Object.Instantiate`) need a real
  baked `MethodInfo*` and will **not** accept NULL. Those globals hold a
  *pointer to* the MethodInfo, so dereference once before passing it.

### Win64 ABI

Structs over 8 bytes go by hidden pointer, in both directions:

| Type | Size | How it crosses |
|---|---|---|
| `Vector3` | 12 | by pointer (return: hidden first arg, `this` shifts to second) |
| `Quaternion` | 16 | by pointer |
| `Color` | 16 | by pointer |
| `Matrix4x4` | 64 | by pointer |

`Transform::get_position` is therefore
`void *f(Vector3 *ret, Transform *this, MethodInfo *)`, while
`Transform::set_position` is `void f(Transform *this, Vector3 *value, MethodInfo *)`.

Unity's `Matrix4x4` is **column-major**: element `(row, col)` is
`raw[col * 4 + row]`.

### Object layout

```
+0x00  Il2CppClass*        // identity — see below
+0x10  m_CachedPtr         // UnityEngine.Object only — the native object
```

- **System.String**: length `int32` at `+0x10`, UTF-16 chars from `+0x14`.
- **Il2CppArray**: `max_length` `int64` at `+0x18`, elements from `+0x20`.

### The two traps that cost the most time

**1. Fake null.** Unity's `Destroy()` zeroes `m_CachedPtr` but leaves the
managed wrapper alive, so a destroyed object still reads as a perfectly good
non-NULL pointer from C. Unity's `==` operator hides this; you can't. **Check
`m_CachedPtr` before calling any method on a `UnityEngine.Object`** or you
dereference a freed native object and take the process down.

**2. Liveness is not identity.** Nothing roots a raw pointer you cached, so
the GC can reclaim and reuse that block — and whatever lands there has its
own non-zero `m_CachedPtr`, so the liveness check passes and you call
`GameObject` methods on something that isn't one. Every cached pointer needs
**both**:

```c
alive:    *(void**)(obj + 0x10) != NULL
identity: *(void**)(obj + 0x00) == <class captured from a known-good instance>
```

Capture the class pointer in a hook where the object is unambiguously of that
type (its `.ctor`, its `Awake`). This caused the v1.8 crash in this repo:
latching `ItemSpawn`, which destroys itself, then reading 55 "pointers" out
of a GC-reused block.

**3. Alive ≠ present.** `ObjectsManager` keeps several deactivated preset
layouts in the level at once. Their components are perfectly alive. Check
`activeInHierarchy` (or `activeSelf` — see below) before believing something
is in the world.

**4. `activeSelf` vs `activeInHierarchy`.** `activeSelf` is the object's own
flag; `activeInHierarchy` is additionally false when any ancestor is off.
Match whichever the game code you're compensating for actually reads — the
mobile UI hangs off a canvas that may be disabled wholesale on PC, which
makes `activeInHierarchy` false forever while the game happily sees
`activeSelf` true.

### Threads

- **IL2CPP calls are main-thread only.** The D3D11 present hook is a
  different thread. Calling into IL2CPP from it can crash.
- The pattern used throughout this repo: game-thread hooks gather into plain
  floats behind a lock, the render thread only reads what was cached.
- ImGui is not thread-safe. `WndProc` (game thread) racing `NewFrame`
  (present thread) on `g.InputEventsQueue` produces an `ImVector` assert deep
  inside ImGui, nowhere near the cause.

---

## 2. TAS-relevant: time, input, determinism

### Time control — verified RVAs

| Method | RVA | Signature |
|---|---|---|
| `Time::get_deltaTime` | `0x72B210` | `float f(MethodInfo*)` |
| `Time::get_unscaledDeltaTime` | `0x72B360` | `float f(MethodInfo*)` |
| `Time::get_fixedDeltaTime` | `0x72B240` | `float f(MethodInfo*)` |
| `Time::get_timeScale` | `0x72B2D0` | `float f(MethodInfo*)` |
| `Time::set_timeScale` | `0x72B3C0` | `void f(float, MethodInfo*)` |
| `Time::get_time` | `0x72B330` | `float f(MethodInfo*)` |
| `Time::get_realtimeSinceStartup` | `0x72B270` | `float f(MethodInfo*)` |
| `Application::set_targetFrameRate` | `0x6FBE50` | `void f(int, MethodInfo*)` |
| `QualitySettings::set_vSyncCount` | `0x710120` | `void f(int, MethodInfo*)` |

**Speed hack:** `set_timeScale(N, NULL)`. Note this scales `deltaTime` but
**not** the AI's own speed fields — `AI_Granny.Walk_Speed`/`Run_Speed` and the
`NavMeshAgent` speed are separate (§4), so a timeScale hack and a Granny
speed hack compose rather than duplicate.

**Deterministic frame pacing:** hook `Time::get_deltaTime` and return a fixed
value. All gameplay code reads the delta through this getter — verified in
`MobileFPS::Update`, `FallingHolder::Update`, `CrouchHolder::Update`,
`AI_Granny::FixedUpdate` — so one hook covers everything. Caveats:

- It does **not** change how many `FixedUpdate` calls Unity runs; that is
  driven by the engine's internal accumulator against `fixedDeltaTime`.
  `AI_Granny::FixedUpdate` is on that path.
- Some code reads `unscaledDeltaTime` or `realtimeSinceStartup`; hook those
  too if you want full control.

**Frame stepping:** there is no Unity frame-step API. Two practical routes,
both using hooks this repo already proves work:

1. **Coarse and reliable** — hook the per-frame entry points and simply don't
   call the original to pause a system, call it once to step it. The known
   good hook points are `MobileFPS::Update` (`0x22D9C0`), `PickRay::Update`
   (`0x237CE0`), `AI_Granny::FixedUpdate` (`0x1BDCC0`),
   `AI_MomSpider::Update` (`0x1D6E20`). Rendering keeps running, so the
   overlay stays responsive while the game is frozen.
2. **Fine** — `timeScale = 0` to freeze, plus a hooked `get_deltaTime`
   returning your step for exactly one frame. Cleaner numerically, but
   anything driven by `unscaledDeltaTime` or coroutine `WaitForSeconds`
   keeps moving.

Coroutines are used heavily (`WaitForSeconds` in `AtticSpider::StingKill`,
`CrouchHolder::Crouch`, `FallingHolder::WaitForAnimationEnd`) and are driven
by scaled time, so `timeScale` does hold them.

### Input — verified RVAs

| Method | RVA | Signature |
|---|---|---|
| `Input::GetKeyDown` | `0x763DF0` | `bool f(int keycode, MethodInfo*)` |
| `Input::GetKey` | `0x763E30` | `bool f(int keycode, MethodInfo*)` |
| `Input::GetKeyUp` | `0x763E70` | `bool f(int keycode, MethodInfo*)` |
| `Input::GetAxis` | `0x763B10` | `float f(String *axisName, MethodInfo*)` |
| `Input::GetAxisRaw` | `0x763A90` | `float f(String *axisName, MethodInfo*)` |
| `Input::GetMouseButton` | `0x763FC0` | `bool f(int button, MethodInfo*)` |
| `Input::GetMouseButtonDown` | `0x763F40` | `bool f(int button, MethodInfo*)` |
| `Input::get_mousePosition` | `0x7641F0` | `Vector3 f(Vector3 *ret, MethodInfo*)` |

> IDA names `0x763B10` / `0x763A90` as
> `UnityEngine.Internal.InputUnsafeUtility::GetAxis/GetAxisRaw` — the public
> `Input` methods fold onto the same code. **Hooking that one address covers
> both call paths**, which is what you want.

**This is the single best lever for a TAS.** Hook these six and the game's
entire input surface becomes a function you control — no window messages, no
`SendInput`, no focus requirements, and it works while the process is
paused.

Movement is read as `GetAxisRaw("Horizontal")` / `GetAxisRaw("Vertical")`,
or `GetAxis` for the same two names when the `"SmoothM"` PlayerPref is 1 —
`MobileFPS::Update` branches on that, so a TAS should either force the pref
or handle both. Axis names arrive as `System.String*`; compare the UTF-16
chars at `+0x14` (§1) rather than pointer identity, though in practice the
game passes cached `StringLiteral_*` globals so pointer comparison also
works and is cheaper.

Rebindable keys live as `KeyCode` fields on their own components, so a TAS
should read them rather than assume:

| Field | Offset | Owner |
|---|---|---|
| `MainInteract` | `0x538` | `PickRay` |
| `DropKey` | `0x53C` | `PickRay` |
| `CrouchKey` | `0x40` | `CrouchHolder` |
| `InteractKey` | `0x58` | `DoorRay` |

### Determinism — what will fight you

- **`NavMeshAgent` pathing** drives both enemies. Unity's NavMesh is
  deterministic given identical inputs and agent state, but agent state is
  large and not obviously serialisable.
- **`CharacterController.Move`** does its own collision sweeps. Deterministic
  per identical input, but sensitive to `deltaTime` — which is exactly why a
  fixed delta matters.
- **`UnityEngine.Random`** — I did **not** locate its RVAs; the class did not
  match my `dump.cs` scan pattern. Look it up before assuming the game is
  free of it. `AI_Granny` picks waypoints (`CurrentWaypoint`,
  `TimerChoosing`), which smells like `Random.Range`. **This is the most
  likely source of run-to-run divergence and the first thing to check.**
- **Physics queries** used on the hot path: `Physics.CheckSphere` (ground
  check, `MobileFPS::Update`), `Physics.SphereCast` (`CrouchHolder`),
  `Physics.Raycast` (`CrouchHolder`, `PickRay`).

**Save/load state** was not attempted and is hard here: state is spread
across live managed objects with GC-owned pointers, not a flat struct. If a
TAS needs it, the realistic scope is *targeted* state (player transform,
Granny transform + AI flags, `HandlePuzzles` bools, item positions) rather
than a true whole-process snapshot.

---

## 3. Known-good hook points

All are game-assembly methods — single argument, no trailing `MethodInfo*`.
All proven stable across a full session in this repo.

| Purpose | Method | RVA |
|---|---|---|
| Per-frame player tick | `MobileFPS::Update` | `0x22D9C0` |
| Per-frame interaction tick | `PickRay::Update` | `0x237CE0` |
| Per-physics-tick Granny | `AI_Granny::FixedUpdate` | `0x1BDCC0` |
| Per-frame cellar spider | `AI_MomSpider::Update` | `0x1D6E20` |
| Level load signal | `ItemRepositionSeed::Awake` | `0x245DC0` |
| Item created / destroyed | `ItemSeedData::.ctor` / `::OnDestroy` | `0x247230` / `0x2471A0` |
| Item dropper spawned | `ItemSpawn::Update` | `0x21F2C0` |

**`PickRay::Update` is the best general-purpose main-thread tick.** It exists
whenever the player does, which matters because Granny can be switched off in
the game's own options — `AI_Granny::FixedUpdate` then never runs and
anything driven off it silently stops.

**Do not touch the instance after calling the original `PickRay::Update`.**
It handles death, the escape sequence and level transitions, so it can tear
the player down before it returns.

`PickRay::Update` is huge (0xACF0, ~9,450 instructions) and `AI_Granny::FixedUpdate`
is 0x366D. Decompiling either whole is expensive; see §7 for how to read them
cheaply.

---

## 4. Game systems

### MobileFPS — the player controller

RVA `0x22D9C0` (`Update`). Fields:

| Field | Offset | Type |
|---|---|---|
| `characterController` | `0x28` | `CharacterController` |
| `moveSpeed` | `0x40` | float — **the live one** |
| `SpeedMove` | `0x44` | float — standing preset |
| `SpeedMoveCrouch` | `0x48` | float — crouched preset |
| `moveDirection` | `0x54` | Vector3, world-space |
| `IsCrouched` | `0x84` | bool |
| `isAllowedToMove` | `0x85` | bool |
| `AbleToMove` | `0x86` | bool |
| `InWeb` | `0x89` | bool |
| `FallingHolder` | `0xA0` | `FallingHolder` |
| `PS` | `0xA8` | `PlayerStatus` |

**`Update` rewrites the speed presets from hardcoded constants every single
frame**, before anything reads them:

```c
SpeedMove       = InWeb ? 1.0f : 6.0f;
SpeedMoveCrouch = InWeb ? 0.3f : 2.8f;
...
moveSpeed = IsCrouched ? SpeedMoveCrouch : SpeedMove;
```

So writing those fields from a hook does nothing — the store lands and is
overwritten microseconds later in the same call. There is no setter to
intercept and `HandlePlayerMovement` is never called (`Update` inlines it).
The two stores are at RVA `0x22DABA`, 10 bytes:

```
0x22DABA  f3 0f 11 4f 44   movss [rdi+44h], xmm1   ; SpeedMove
0x22DABF  f3 0f 11 47 48   movss [rdi+48h], xmm0   ; SpeedMoveCrouch
```

NOP those and the fields keep whatever you put in them. Side effect: `InWeb`
no longer slows the player, since those constants are the only thing it
encodes.

**Air control.** `Update` zeroes `moveSpeed` outright for the whole fall,
once standing and once crouched:

```
cmp byte ptr [rax+81h], 0     ; FallingHolder.isFalling
jnz short zero                ; 75 07   <- NOP these two bytes
movss xmm0, [rdi+44h]
jmp short store
zero:  xorps xmm0, xmm0       ; moveSpeed = 0
```

Sites: `0x22DCE6` (standing), `0x22DD94` (crouched). Patching the speed
selection rather than clearing `isFalling` matters — that flag also drives
landing detection, crouch gating and fall damage.

### FallingHolder — falling, landing, fall damage

Reached as `MobileFPS.FallingHolder` (`+0xA0`).

| Field | Offset |
|---|---|
| `FallDurationHold` | `0x50` |
| `FallMega` | `0x58` |
| flags block (`CanFallSound`, `isFalling`, `isLanding`, `Fell`, `Damaged`, `DeathFall`) | `0x80`–`0x85` |
| `fallDuration` | `0x88` |
| `DurateCan` | `0x8C` |

```c
if (fallDuration > FallMega)          -> death, if DeathFall
if (fallDuration > FallDurationHold)  -> HandleLanding(), the get-up stagger
if (fallDuration <= FallChecker)      -> silent landing
otherwise                             -> small landing sound
```

`FallingHolder::Update` decides everything from
`CharacterController.isGrounded`. **A disabled controller reports
`isGrounded == false` forever**, so anything that disables it (noclip) makes
the game believe you are in an endless fall: `isFalling` latches,
`fallDuration` runs away past `FallMega`, `Damaged` latches, `HandleLanding`
fires and `PickRay::CheckItemDropping` force-drops your held item. Hold the
whole flag block at zero while the controller is off.

`HandleLanding` (`0x2186D0`) is the stagger: sets `isLanding`, forces
`isAllowedToMove = false`, plays the get-up animation, starts a coroutine to
give control back. Suppressing it directly strands `fallDuration` above the
threshold forever, because resetting it is one of the things it does — cap
`fallDuration` at `FallDurationHold` instead.

### AI_Granny

`FixedUpdate` RVA `0x1BDCC0`. Fields:

| Field | Offset | Type |
|---|---|---|
| `Walk_Speed` / `Run_Speed` | `0x94` / `0x98` | float |
| `Agent` | `0xA8` | `NavMeshAgent` |
| `Player` | `0xD0` | `Transform` |
| `PlayerStatus` | `0xE8` | `PlayerStatus` |
| `EnemyVision` | `0x108` | `Eyes_Granny` |
| `IsDying` | `0x138` | bool |
| `IsBlind` / `BlindTimer` | `0x164` / `0x168` | bool / float |
| `PepperedEnemy` | `0x178` | bool |
| `CaughtPlayer` | `0x188` | bool |
| `IsSearching` / `IsAngry` | `0x18A` / `0x18B` | bool |
| `IsFollowingSound` | `0x18C` | bool |
| `IsChasing` / `IsWalking` / `IsIdle` | `0x18D` / `0x18E` / `0x18F` | bool |
| `TimerNearNoise` / `TimerMaxNoise` | `0x190` / `0x194` | float |
| `DistanceFromPlayer` | `0x1BC` | float |
| `NoiseObj` | `0x1E8` | `GameObject` |
| `NoiseObjectTag` | `0x1F8` | `String` |
| `PlayerPos` | `0x288` | Vector3 |

**Speed:** writing `Walk_Speed`/`Run_Speed` alone is not enough — those feed
the `NavMeshAgent` on her next tick, so she coasts along the current path at
the old speed until then. Call `NavMeshAgent::set_speed` (`0x6E67D0`) too for
an immediate effect. Setting both to zero plus `set_speed(0)` is a working
freeze. She overshoots waypoints at high speed and walks into walls; that's
the game, not the hack.

**Blind** is the game's own flag (`IsBlind`, `0x164`), but `BlindTimer` runs
it down, so it must be re-asserted every tick. Only write it when your toggle
is *on* — forcing it false otherwise cancels the player's pepper spray, which
sets the same flag.

**Deaf has no flag.** There is no `IsDeaf` to match `IsBlind`, and the
obvious synthesis — clearing `IsFollowingSound` and `NoiseObj` from a hook —
**does not work**: both are set *and acted on* inside the same `FixedUpdate`,
so clearing beforehand is overwritten immediately and clearing afterwards is
too late (the agent already has the destination; dropping the reference
doesn't recall it). The acquisition is:

```
mov  rcx, [rbx+1F8h]                  ; NoiseObjectTag
call GameObject::FindGameObjectWithTag
call Object::op_Implicit
test al, al                           ; RVA 0x1BF094  <- patch to `and al, 0`
jz   skip
cmp  [rbx+18Dh], r15b                 ; IsChasing
... IsAngry, IsDying, PepperedEnemy ...
mov  byte ptr [rbx+18Ch], 1           ; IsFollowingSound = true
mov  [rbx+1E8h], rax                  ; NoiseObj = it
```

`and al, 0` (`24 00`) is the same two bytes as `test al, al` and always sets
ZF, so the `jz` is always taken and she never latches onto a noise at all.

Other methods: `ResetAIDecision` `0x1C1930` (tears down component refs —
*not* a reset-to-patrol despite the name), `StopAI` `0x1C2100` (the more
thorough teardown, one-shot and irreversible), `ChaseAction` `0x1BDA40` (a
21-byte state-enter stub, not the pursuit logic), `SmackTimer` `0x1C1DE0`,
`MurderNoiseObjs` `0x1C1510`. `OnTriggerStay` `0x1C1680` only sets
`PlayerClose` — it is **not** the hearing system.

### AI_MomSpider — the cellar enemy

`Update` RVA `0x1D6E20`. A different class from `AtticSpider`. Fields:
`Walk_Speed` `0x20`, `Run_Speed` `0x24`, `Agent` `0x30` (`NavMeshAgent`),
`IsChasing` `0x62`. Only exists while the cellar is loaded, so the hook
simply never fires elsewhere — and when the level unloads the hook *stops*
rather than signalling, so watch the instance's `m_CachedPtr` to know it's
gone.

### AtticSpider — the attic one

`OnTriggerStay` → `StingKill` coroutine, whose `MoveNext` is at `0x1E5ED0`:

```c
PlayerStatus.LookAtSpider = true;
Animator.Play(...);
PlayerStatus::PlayerGettingStopped();   // takes control away
yield WaitForSeconds(0.4);
PlayerStatus::NormalDeath();
PlayerStatus::DamageSpider();
```

### PlayerStatus — deaths and control

| Method | RVA |
|---|---|
| `GrannyCaughtYou` | `0x24A9A0` |
| `GrannyCaughtYouBed` | `0x24A8C0` |
| `KnockDeath` | `0x24AA20` |
| `NormalDeath` | `0x24B350` |
| `PlayerGettingStopped` | `0x24B820` |

`PlayerCam` at `+0x110` (a real `Camera`).

**`PlayerGettingStopped` is the one that isn't obvious.** It sets
`MobileFPS.isAllowedToMove = false`, `MobileFPS.AbleToMove = false` and
`CrouchHolder.Disabled = true`, and control comes back from the death and
respawn that normally follow. Its only three callers are `AtticSpider`'s
`StingKill`, `GrannyCaughtYou` and `GrandpaCaughtYou` — all kill paths. If
you suppress the deaths without suppressing this, the player is left frozen
with a dead camera forever.

For an immortality-style patch, `ret` over all three of `NormalDeath`,
`KnockDeath` and `PlayerGettingStopped` — as a group, all-or-nothing. Half
of it applied is the worst state: `PlayerGettingStopped` alone means you
still die; the deaths alone mean you freeze.

### CrouchHolder

`Update` `0x1AEB40`. `controller` `0x20`, `FPS2` `0x28`, `HolderF` `0x30`,
`CrouchKey` `0x40`, `isCrouching` `0x82`, `IsCrouched` `0x83`, `Starter`
`0x84`, `Disabled` `0x85`.

Crouching is refused while `FallingHolder.isLanding` or `.isFalling`, or
while `Disabled`.

### PickRay — interaction

`Update` `0x237CE0`. Fields that matter:

| Field | Offset | Notes |
|---|---|---|
| `Drop1` | `0x30` | drop button **and** the "holding something" flag |
| `DropP` | `0x50` | drop point transform |
| `PlayerStatus` | `0x88` | Granny-independent route to the camera |
| `ItemDrop` | `0x230` | the dropper prefab |
| `buttonClicked` | `0x4D0` | **set AND consumed inside `Update`** |
| `HP` | `0x4D8` | `HandlePuzzles` |
| `MainInteract` / `DropKey` | `0x538` / `0x53C` | `KeyCode` |

**`buttonClicked` is worthless from a hook on `Update`'s entry** — it is set
partway through the function (`mov byte ptr [rbx+4D0h], 1` at `0x239620`) and
consumed later in the same call, so it is always false when you sample it.
This cost two wrong fixes in this repo.

**Dropping is gated on `Drop1.activeSelf`:**

```
mov  ecx, [rbx+53Ch]              ; DropKey
call Input::GetKeyDown
test al, al
jz   no drop
mov  rcx, [rbx+30h]               ; Drop1
call GameObject::get_activeSelf
test al, al
jz   no drop
```

Every interaction path calls `Drop1.SetActive(false)` afterwards as part of
"you used your item".

### Items

Two enumeration sources, both partial, plus one complete one:

- `ItemRepositionSeed` — 35 `Transform*` fields, contiguous from `0x28`,
  8 bytes apart. Persistent level fixture; `Awake` at `0x245DC0` is a
  reliable level-load signal.
- `ItemSpawn` — 55 `GameObject*` fields from `0x28`, and `CountItem` (float,
  1-based) at `0x24`. **This is a self-destructing dropper, not a registry** —
  it activates one item, throws it, unparents it and calls
  `Destroy(this.gameObject)`. Latching it is a use-after-free.
- **`Object::FindObjectsOfType(Type, bool includeInactive)`** at `0x725FF0`
  is the only complete source. Build the `System.Type` at runtime from a
  captured `Il2CppClass*` via the exported `il2cpp_class_get_type` and
  `il2cpp_type_get_object` (resolve by name with `GetProcAddress` — they
  don't drift between builds). `includeInactive = false` drops the
  deactivated preset placeholders for free. Expensive; run it on a timer and
  re-read positions from the cached components each frame.

`ItemSeedData`: `itemName` (String) `0x20`, `category` (int) `0x28`.
Category per the dev tooltip: 1 escape-only, 2 escape+puzzle, 3 puzzle-only,
anything else free. **Category is per-instance and the `ItemDrop` prefab
leaves it at 1**, so a dropped item reports a different category from the
placed one — key a cache on the item name, first sighting wins.

Unity does **not** run the managed `.ctor` for scene-placed MonoBehaviours,
so a `.ctor` hook only ever sees `Instantiate()`d objects.

**Spawning an item** is exactly what dropping one does, minus taking it off
you first (from `PickRay::CheckItemDropping`, `0x233070`):

```c
Instantiate(PickRay.ItemDrop, DropP.position, DropP.rotation);
GetComponent<ItemSpawn>(copy).CountItem = <1-based index>;
```

Its own `Update` does the rest. Pace bulk spawns one per tick — 55 rigidbodies
appearing inside each other in one physics step shove each other through the
floor.

### HandlePuzzles — all progression state

Reached as `PickRay.HP` (`+0x4D8`). **This single class holds one bool per
progression step in the game**, and it is where every lock's "do you have the
key" answer lives. There is no comparison against the item in your hand.

Two groups that look alike and behave oppositely:

- **requirement** — `usedPadlockForPort` `0x71`, `UsedMaster` `0x3B8`,
  `UsedSafeKey` `0x3A0`, `UsedWPKey` `0x370`, `UsedCarKey` `0x1C0`,
  `usedElecKey` `0x158`, `usedPlayhouseKey` `0x298`, `UsedPadlockKey` `0x470`,
  `UsedRustyPadlock` `0x4A9`, `UsedSpecialKey` `0x1E8`, `UsedChainCutter`
  `0x4AA`, `UsedCode` `0x458`, `UsedBattery` `0x428`, `usedSparkPlug` `0xA0`,
  `PlacedEnginePart` `0xB0`, `PlacedCarBattery` `0xC0`, `PlacedCogwheels`
  `0x268`, `PlacedMelon` `0x280`, `PlacedStick` `0x4A8`, `usedPlank` `0x4E0`,
  `PlacedUSB` `0x148`, `fuseInPlace` `0x120`, `usedRemote` `0x188`,
  `WheelCrankPlaced` `0x480`, `CutWire1` `0x3E0`, `CutWire2` `0x3E1`,
  `TankFuelled` `0x28`, `ScrewedOldHouse` `0x1D4`, `ScrewedSwitchEX` `0x3F8`,
  `ScrewedPlattaSpiderCellar` `0x498`, `CutAtticWire` `0x34D`
- **already done** — `openedPort` `0x70`, `OpenedTank` `0x88`, `FillingTank`
  `0x89`, `IsRecharging` `0x110`, `RechargedRobot` `0x111`, `GotShotgun`
  `0x1B0`, `IsUpFully` `0x310`, `RotatingWinch` `0x311`, `RotateFan` `0x34C`,
  `IsDoorFree` `0x3A1`, `OpenedExLock` `0x408`, `OpenedIronDoors` `0x4AB`,
  `SpiderCellarDone` `0x471`

**Setting one from the second group is worse than doing nothing** — the
branch checks it *first* and bails as already-complete, making the puzzle
permanently uninteractable rather than unlocked. Names alone don't separate
the groups; trace before setting.

Typical lock branch (the padlocked port):

```c
if (HP.openedPort)          goto done;   // already open
if (!PickRay.buttonClicked) goto done;
PickRay.buttonClicked = 0;
if (HP.usedPadlockForPort)  HandlePuzzles::OpenPort();
else                        text("It's locked");
```

**For a TAS this class is the progression state vector.** It is the closest
thing to a save-state for run progress, and reading it every frame is a cheap
way to detect and time route milestones.

### Item requirement checks — "I need a …"

Possession is *only* whether the item's hand object is active. Every check
compiles identically:

```
cmp [rbx+4D0h], r15b          ; buttonClicked
jz  done
mov [rbx+4D0h], r15b          ; consume it
mov rcx, [rbx+<hand object>]
call GameObject::get_activeSelf
test al, al
jnz <the interaction>
...  "I need a master key"
```

There are **50 of these in `PickRay::Update`**. Finding them by their failure
message is wrong twice over: a puzzle emits one message from several checks
(four screw puzzles share one "I need a screwdriver" string), and many checks
print nothing at all when they fail. Scan for the *shape* instead.

**Shared vs distinct jump target separates two very different cases:**

- Checks jumping to the **same** target are **alternatives** — either item
  does the job (pliers *or* chain cutter on each of three wires).
- Checks jumping to **different** targets are **selectors** — the answer
  decides *what the game does next* (which painting piece, which shotgun
  part, which cogwheel you are placing). Forcing these makes the first check
  always win, so the game runs the placement for piece one whatever you hold:
  your actual piece never goes in.

Three sites test something else entirely behind the identical shape:
"I need a crossbow" and "break this camera" test a `PickRay` method's return,
and "I need to get closer" tests `Input::GetKeyDown` — forcing that last one
fires the interaction every frame. **Always check the instruction before the
`test`**, not just the branch shape.

### Traps

Four independent `OnTriggerEnter(Collider)` methods; no base class, no global
toggle:

| Class | RVA | Notes |
|---|---|---|
| `TrapTrigger` | `0x211A10` | generic; `Enabled` bool at `+0x58` |
| `BearTrapLogic` | `0x1A9A10` | |
| `ExploTrapTrigger` | `0x1FC890` | |
| `TrapPoison` | `0x261A10` | |

`ExtraTrapsLogic` has no trigger — it's a setup script. Its `.ctor` shares
RVA `0x1A26E0` with several other classes via identical COMDAT folding, which
is a good reason never to patch a constructor here.

### Lighting

`RenderSettings`: `get_fog` `0x712200`, `set_fog` `0x7123F0`,
`get_ambientMode` `0x712020`, `set_ambientMode` `0x7122F0`,
`set_ambientIntensity` `0x712230`, `set_ambientLight` `0x7122B0`.

`AmbientMode.Flat` is **3**, not 2 — the enum is Skybox=0, Trilight=1,
Flat=3, Custom=4, with 2 unused. Passing 0 selects Skybox and looks like
nothing happened. Fog is what actually hides distance in this game.

Scene loads reset lighting, so it has to be re-applied. Watching for drift
isn't enough; key off a scene-rebuild signal (a changed `PickRay` instance
whose predecessor is *dead* — see §6).

### Common UnityEngine methods

| Method | RVA | Signature |
|---|---|---|
| `Component::get_transform` | `0x71D550` | `Transform *f(Component*, MethodInfo*)` |
| `Component::get_gameObject` | `0x71D4A0` | `GameObject *f(Component*, MethodInfo*)` |
| `GameObject::get_transform` | `0x720600` | |
| `GameObject::get_activeSelf` | `0x7204A0` | `bool f(GameObject*, MethodInfo*)` |
| `GameObject::get_activeInHierarchy` | `0x720460` | `bool f(GameObject*, MethodInfo*)` |
| `GameObject::SetActive` | `0x720060` | `void f(GameObject*, bool, MethodInfo*)` |
| `GameObject::GetComponent<T>` (shared) | `0x2B7FD0` | `void f(GameObject*, void **out, MethodInfo*)` |
| `Transform::get_position` | `0x747310` | `void *f(Vector3 *ret, Transform*, MethodInfo*)` |
| `Transform::set_position` | `0x747B20` | `void f(Transform*, Vector3*, MethodInfo*)` |
| `Transform::get_rotation` | `0x747480` | `void *f(Quaternion *ret, Transform*, MethodInfo*)` |
| `Behaviour::get_isActiveAndEnabled` | `0x71CDB0` | `bool f(Behaviour*, MethodInfo*)` |
| `Behaviour::set_enabled` | `0x71CDF0` | `void f(Behaviour*, bool, MethodInfo*)` |
| `Collider::set_enabled` | `0x769040` | `void f(Collider*, bool, MethodInfo*)` |
| `Camera::get_main` | `0x6FE450` | `Camera *f(MethodInfo*)` |
| `Camera::get_worldToCameraMatrix` | `0x6FEA00` | `void *f(Matrix4x4 *ret, Camera*, MethodInfo*)` |
| `Camera::get_projectionMatrix` | `0x6FE6B0` | same shape |
| `NavMeshAgent::set_speed` | `0x6E67D0` | `void f(NavMeshAgent*, float, MethodInfo*)` |
| `NavMeshAgent::set_isStopped` | `0x6E6740` | throws if the agent isn't on a NavMesh — prefer `set_speed` |
| `NavMeshAgent::Warp` | `0x6E6380` | `bool f(NavMeshAgent*, Vector3*, MethodInfo*)` |
| `Object::FindObjectsOfType` | `0x725FF0` | `Il2CppArray *f(Type*, bool includeInactive, MethodInfo*)` |
| `Object::Instantiate` | `0x2CE440` | `Object *f(Object*, Vector3*, Quaternion*, MethodInfo*)` |

Baked MethodInfo globals (hold a **pointer to** the MethodInfo):
`GetComponent<ItemSeedData>` `0xC338B8`, `GetComponent<ItemSpawn>` `0xC338F0`,
`Instantiate<GameObject>` `0xC3B3E0`.

**`Camera.main` returns NULL in this game** — it never tags a camera
`MainCamera`. Use `PickRay → PlayerStatus(+0x88) → PlayerCam(+0x110)`, or
`AI_Granny → PlayerStatus(+0xE8) → PlayerCam` as a fallback. Both are pure
pointer derefs, no IL2CPP call.

---

## 5. Byte patching — the discipline that survived

`VirtualProtect` → write → restore protection → `FlushInstructionCache`.
Beyond that, four rules learned the hard way:

**1. Verify what you're overwriting.** Compare against the expected bytes
first. RVAs are tied to one build; after an update an unverified write stamps
over arbitrary code *and saves the wreckage as the "original"*, so even
undoing it restores garbage. A check turns silent corruption into a clean
refusal.

**2. Land the patch as a single store.** This is live code with the game
running and nothing suspends the other threads. Byte-at-a-time is unsafe in
*either* order, and back-to-front — which looks safest — is the worse one:
over `test al, al` (`84 C0`) it leaves `84 00` in flight, and while ModRM
`C0` names the register `al`, ModRM `00` names `[rax]`, so that intermediate
decodes as `test [rax], al` and dereferences a register holding a boolean.
A single naturally-sized store can't be observed half-done on x86. For
patches longer than 8 bytes, write the tail first and the leading 8 as one
store, so the original opening instruction stays decodable until the last
write.

**3. All-or-nothing, with a rollback.** A half-applied group is harder to
reason about in game than none of it.

**4. A failed *restore* must not clear the "patched" flag.** The saved bytes
are the only surviving copy. Clearing the flag lets the next enable re-save
your own patch bytes as the original, after which every restore writes the
patch back — permanent, and unrecoverable without a restart.

Useful two-byte forms over `test al, al` (`84 C0`), same length so no jump
relocation:

| Want | Bytes | Effect |
|---|---|---|
| force `jnz` taken | `0C 01` (`or al, 1`) | always clears ZF |
| force `jz` taken | `24 00` (`and al, 0`) | always sets ZF |

`ret` is `C3`, and a one-byte patch is atomic. Safe over
`void f(void *this)` under Win64 — the caller cleans up and nothing has been
pushed at entry. Don't NOP the first instruction instead; that just falls
through into the rest of the function.

---

## 6. Level-change and staleness signals

There is no clean "level loaded" event exposed. What works:

- **`ItemRepositionSeed::Awake`** (`0x245DC0`) fires once per level load.
  Best explicit signal.
- **A changed instance pointer** — a different `AI_Granny` or `PickRay` than
  last tick means the scene was rebuilt. But "different" alone is wrong: an
  overlapping scene load has both the outgoing and incoming player alive for
  a stretch of frames, so `Update` fires for two instances alternately and
  every tick looks like a rebuild. **Require the previous instance to be
  *dead*** (`m_CachedPtr == NULL`) before believing it.
- **The feed going quiet.** In the main menu no game hook runs at all, so
  nothing can clear your cached state and the last frame's data sits there
  forever. Nothing game-side announces this. A render-thread timeout — "the
  game thread hasn't called in for 250ms" — covers the menu, loading, scene
  changes and pauses without knowing about any of them.
- **Hiding.** Getting into the car or under a bed switches cameras and
  *disables* `PlayerCam` rather than destroying it, so `m_CachedPtr` stays set
  and every liveness check passes while the matrices are whatever it last
  rendered. Require `Behaviour::get_isActiveAndEnabled` on a camera before
  trusting it.

---

## 7. Tooling

- **`dump.cs`** — `C:\Users\ir0n1c\Desktop\Il2CppDumper-win-v6.7.46\dump.cs`.
  Il2CppDumper v6.7.46 output for this build, with `stringliteral.json` and
  `script.json` alongside. **Use this for structure** — class layouts, field
  offsets, method RVAs. It is a local text file, so `grep`/`awk` over it is
  effectively free compared with any IDA query.
- **IDA Pro MCP** — use for *behaviour*: decompiles, xrefs, raw bytes.
  Currently disconnected in this workspace; it needs the IDA instance running
  with the plugin.
- **Reading the two huge functions cheaply.** `disasm` on `PickRay::Update`
  re-dumps a ~250-entry stack frame every call (~6k tokens regardless of
  window). Use `py_eval` with IDAPython and print only what you want:

  ```python
  import idc
  ea = start
  while ea < end:
      print("%X  %s" % (ea, idc.generate_disasm_line(ea, 1)))
      ea = idc.next_head(ea, end)
  ```

  About 1.5k tokens for the same information. Note `py_eval` rebuilds
  *globals* each call but persists *locals*, so a function defined in one call
  cannot see a module or constant assigned in another — pass them as default
  arguments.
- **String literals** are cached in a prologue block at the top of
  `PickRay::Update` (`lea rcx, StringLiteral_N` + `call sub_180138640`). Real
  use sites load them as `mov r8, cs:StringLiteral_N`. **Filter on the
  instruction form, not an address range** — a range filter silently swallows
  real sites.
- **Symbol names can be wrong.** Identical COMDAT folding means a simple
  setter may be named after a completely different class. Trust the
  instruction bytes over the symbol.

---

## 8. Open questions

Things I did not establish, roughly in the order a TAS would care:

1. **`UnityEngine.Random` RVAs.** Not located. The likeliest source of
   run-to-run divergence — `AI_Granny` waypoint selection is the prime
   suspect. **Check this first.**
2. **`Time::set_fixedDeltaTime`** — I found the getter (`0x72B240`) but not
   the setter. Needed if you want to control the physics rate.
3. **Save/load state.** Not attempted. See §2 for why a targeted subset is
   the realistic scope.
4. **A crash after the wake-up animation ends**, reported but never
   reproduced or located. Two structural theories were ruled out by
   inspection: `PlayerStatus.PlayerCam` really is a `Camera`, and all 40
   hand-object fields really are `GameObject`s. `src/core/crashlog.c` in this
   repo installs a vectored handler that writes the faulting module+RVA, the
   active features and the last eight breadcrumbs to
   `%APPDATA%\grannycheat\crash.log` — **if the TAS build inherits that, the
   next occurrence should name the address.**
5. **Three requirement checks** whose predicate is a `PickRay` method return
   rather than `get_activeSelf` (crossbow, shotgun, break-this-camera) — the
   method was never identified.
6. **Two hand objects** at `PickRay+0x358` and `+0x2E0` that no message ever
   named. Shape and field verified; only the label is unknown.
7. **`Eyes_Granny` / `Eyes_MomSpider`** — the sight components. Never opened;
   blind was done through `AI_Granny.IsBlind` instead.
8. **Grandpa.** `PlayerStatus::GrandpaCaughtYou` exists and calls
   `PlayerGettingStopped`, but his AI class was never examined.

## Surviving a game update

Every RVA here dies on a rebuild. What survives:

- **Field offsets within a class** usually survive minor updates; method RVAs
  never do.
- **The IL2CPP exports** (`il2cpp_class_get_type`, `il2cpp_type_get_object`,
  …) resolve by name with `GetProcAddress` and don't drift. Prefer them where
  a choice exists.
- **Re-dump with Il2CppDumper** and re-derive. Most of this file — class
  layouts, which flag means what, which branch does what — stays true; only
  the numbers move.
- The **expected-bytes check** on every patch (§5) means a stale offset
  refuses cleanly instead of corrupting the process, which makes an update
  obvious rather than mysterious.
