# RDR1 PC MP mod: RE notes

## Mod code: `src/` (build with `build.bat`, output in `build\bin`)
- `RDMP.exe` launcher: starts RDR.exe suspended, injects `rdmp.dll` (CreateRemoteThread + LoadLibraryW), then resumes.
  Options: `--dll <path>`, `--game <RDR.exe>`, `--wait`, `-- <game args>`. Tested working 2026-11-12.
- `rdmp.dll`: logs to `build\bin\rdmp.log`, checks the build fingerprint (timestamp 0x673783F3 + bytes of FUN_1401ee380).
  RDR.exe loads at its preferred base 0x140000000 (no ASLR shift), so Ghidra addresses map 1:1.
- SP sandbox lead: root-script selector `FUN_1401ee380` falls back to **`$/content/sp_idle`** when `game+0x12e & 0x10` is set.
  The script override params live at 0x142b0d838/870/8a8 (0x38-byte param objects, list head 0x142ab31c8, names look hashed).

## SP sandbox boot findings (live tests)
- `sp_idle` as root script: the menu still appears (the front-end is engine UI). After Play, the save loads, then
  loadStart -> root script requested by FUN_1401ee430 -> sp_idle -> **stuck on the loading screen** (loadComplete never comes).
- UI events (logged via the UI manager *(0x142aac8b8) vtable slot 0x390 = post-event, original 0x1400EEED0):
  boot: postLoad, fileDoFreeSpaceCheck, EndLegals, MissionLoadingBrush1-3Begin, InTransitionComplete, loadComplete, setupComplete, startPadSelected
  Play: startScreenExit, mainMenuExit, enterSinglePlayer, fileStartPlayerChecks, file* save checks, fileLoadSuccess x3, fileAllFilesLoaded, loadStart, fileStartupChecksComplete
- Loading finish: `FUN_14063b0a0` FinishLoading(loader, a, b, flag) hides the spinner, fades, posts "loadComplete".
  The frame update `FUN_1401f3ad0` calls it when `FUN_1405f73c0(*(0x142ac5140), 6)` and bit 0x10 of loader+0x257e5 is set
  (loader = *(0x142c456c8); args at +0x25814/+0x25818). Also called from `TransitionEventHudFadeIn__vf9` (scripted fade-in).
- **F5 test worked:** setting loader+0x257e5 |= 0x10 on the sp_idle loading screen -> loadComplete -> the world renders
  with the HUD minimap, but there's **no player** (actor handle 0). sp_idle never creates the player.
- Player info ctor/reset = `FUN_140344f30` (sets +0x5ec = 0; called from game init FUN_1401ee680). No static MOV/LEA writer
  of a real handle exists at +0x5ec, so the next step is a runtime HW write-watch during a normal "main" boot (rdmp.ini [debug]).
- Local player: info *(0x1422daf30), +0x5ec actor handle; handle table *(0x142c457f8) 16-byte {obj*, u16 gen}.
- Game object *(0x142c456d0): +0x12e flags (|1 new game, |8 MP load, 0x10 sp_idle mode).

## SANDBOX BOOT: WORKING (2026-11-13) — world + Marston + gameplay camera, no story/NPCs, no input needed
Sequence implemented in src/game/sandbox.cpp (rdmp.ini [boot] root_script=$/content/sp_idle, [sandbox] enabled=1):
1. Root script forced to sp_idle (FUN_1401ee380 replaced).
2. Menus: DLL presses Enter (SendInput scan code) on the title until "enterSinglePlayer" (title -> mode select "Normal").
   Posting the UI events does nothing (they're notifications; the Play flow is ActionScript -> ValidateNormalMode cb).
3. World load starts when FUN_1401ee430 requests the root script (caller 0x1401EE442). After finish_delay_ms (8s),
   the DLL sets loader+0x257e5 |= 0x10 on each Loading* event -> FinishLoading -> "loadComplete".
4. On loadComplete: CreateActor(owner=0x328 (goh[808], static obj 0x1422D9F50 — the story's player owner), "player",
   0, pos, rot, 12) via FUN_1402211a0 -> local player linked (playerInfo+0x5ec = actor handle).
   Owner = an ordinary Layout creates the actor but NOT as the local player.
5. Kill all script threads (scrThread::Kill FUN_1401e6710 on *(0x142b0d1e0)[i] with program != 0) so sp_idle stops
   driving scenic camera shots.
6. Attach the gameplay camera exactly like world-load setup FUN_1401f9830: cam = *(camMgr+0x28);
   FUN_140376e50(cam, playerHandle, 0); FUN_140376f40(cam, 0, 0); cam+0x860 = actor+0x188.
   camMgr = *(0x142c45738); +0x20 = "GameCamera" gohCamera, +0x28 its camera instance (= active camera).
   NEVER deactivate camMgr+0x20 (gohCamera vf2) -> active camera null -> crash reading cam+0x860.
7. Enable player control: controller component = *(actor+0x70); byte +0x48e is a "controls disabled" bitfield
   (spawns as 0xff). Clear like the hogtie-pickup handler: &= 0x82. (putdown sets |= 0x7d.)
Result (verified 00:27): Marston on screen with third-person camera + HUD, WALKS with W, Esc opens the pause menu.
Launch -> playable in ~45-60 s with no input. Open items: legal screen still ~14 s; live player position field
(the actor+0x50->+0x70 etc. values are static spawn copies); spawn location is the story's (736.49, 89.17, 1199.33).
Live player root position: *(actor+0x40)+0x20 (vec3, Y up); also translation in *(actor+0x88)+0x70 / *(actor+0xc8)+0x70
(transform matrices -> heading from the rows before). *(actor+0x50)+0x70 is a static spawn copy. (found with F8 diff)

### Remote-player puppets (verified 00:50): F9 spawns NPCs in front of the player
- Main-thread work: detour on the script VM runner FUN_1402066d0 (runs every frame on the main thread even with no
  scripts; 12-byte position-independent prologue) -> src/game/mainthread.cpp RunOnMainThread().
- Create via the factory FUN_1402209e0(out, owner, name, def, variation, &pos, &rot, 1) DIRECTLY: the wrapper
  FUN_1402211a0 refuses everything while playerInfo+0x5e9 != 0 (it is 1 once the player exists).
- owner = a base Layout (vt 0x141C821C0) from the goh table; NOT goh[808] (that's the player's owner -> "second player").
- def must be a NON-player template: defs = *(0x142b12070), count *(u16*)0x142b12078, def+0x10 = name
  (1344 defs; 0 template_player, 1 template_player_jack, 2 template_player_cs, 3.. template_base_human).
  Player templates crash the player-controller update (FUN_14039fbc0 reads c->+0x10 = null, via FUN_14034a6d0).
- Unique name per actor (we use "rdrmp_remote_N"); variation 12 with def 3 gives a saloon-girl model.

- **playerInfo+0x5ec is NOT stable**: every actor creation overwrites it with the newest actor's handle
  (FUN_140ae3ff0 writes the handle into the controller at actor+0xb8, which is shared). Cache the player's actor
  handle right after the player spawn and use that afterwards (done: g_playerActorHandle).
- *(actor+0x40)+0x20 is NOT per-actor either (showed the last puppet's spawn point). Per-actor translation:
  *(actor+0xc8)+0x70 (and *(actor+0x88)+0x70).

### Puppets: position/heading sync SOLVED via engine teleport (2026-11-13 ~02:15)
- **Actor "is player" flag**: the create-actor wrapper FUN_1402211a0 always calls the factory with last arg = 1
  (= create as THE player; FUN_140264b80 passes goh byte +0xca bit0 as FUN_140ae3ff0's param_6, which writes
  playerInfo+0x5ec / +0x5e9 and grabs the player controller). Puppets must use factory(..., 0):
  CreateActorDirect(..., asPlayer=0). With 0, Marston's own template (def 0 template_player) works as an NPC puppet,
  and the local player keeps control. (asPlayer=1 puppets stole input; the earlier def 0..2 crash was this too.)
- Engine teleport: FUN_140ad4ca0(actorHandle, const Vec3* pos, float headingDeg, 1, 0, 0) (found via
  TransitionEventTeleportWithHeading::vf9 @ 0x1404025a0; actorHandle = *(actor+8) = goh+0xb0). Builds a Y-rotation
  matrix + translation and applies it via FUN_140ad4680 (physics/spatial update). Per-frame calls are fine.
- Heading convention: heading 0 faces -Z; forward = (-sin h, 0, -cos h); read back from transform *(actor+0xc8):
  row0 at +0x40 = (cos h, 0, -sin h) -> h = atan2(-row0.z, row0.x).
- F10 = mirror test (puppets teleported beside the player each frame) works: follow position + heading exactly.
- Player handle is cached right after spawn (g_playerActorHandle); camera/control/position use it.

### Puppet locomotion (walk/run animation): NOT solved (state 02:35)
- AI task route WITHOUT scripts: build the task in a buffer {vt_TASK_GO_TO_COORD 0x141D2C7B8, +8 i32 style,
  +0x10 u8 0, +0x20 Vec3 target}, call FUN_140af93c0(builder{task*,0,0,0}, actorHandle). builder[2] (open
  sequence) must be null. It creates an AI brain on demand (template_player puppets have none) and calls
  FUN_140b385e0(brain+0x1440, task, -1, 0) which memcpy's 0xb0 bytes into the brain's script-task slot.
- Verified live: the brain update FUN_140b111a0 (rcx = core = brain+0x30) runs ~2x/frame per puppet; the slot holds
  our task (vt ok, active=1, pending=1); every start gate in FUN_140b111a0 passes (core+0x155a bit0 && !bit1,
  core+0x155d=1, state byte 0). The go-to task's update (vf13 FUN_140bd4ac0) maps style 1->1, 2/7->2, 3->4, 4->3,
  5->0, then either FUN_140b0ce00 or starts navigation FUN_140b732c0(ctx+0xeb0, req) and polls its status
  (vf 0x18: 1 running, 2 done). Still NO movement for def 0 or def 3 puppets -> suspect navmesh/path request
  failing (rooftop spawn?) or the locomotion layer. ctx (task param_2+0x18) is not core; not resolved yet.
- Debug helpers: F11 follow mode (go-to every 100 ms), LogAiState dumps slot/gate state, ArmExecWatch (DR0 execute
  breakpoint, watch.cpp) to check whether a function runs; WatchExecRcx(i) returns the distinct rcx values seen.
- Runtime checks (02:45-02:56), all via execute breakpoints during F11 follow:
  go-to task update FUN_140bd4ac0 runs every frame (rcx = our task in the slot); it starts a path request
  FUN_140b732c0(nav, req) for each new target (nav object vt 0x141D24DB0, size 0x3b0); the nav object update
  FUN_140b736b0 runs every frame on the main thread (called from 0x140AF8BB5). BUT nav state (+0x350) stays 0
  forever (request counter +0x354 climbs) -> no path is ever computed -> no movement.
  Global nav gate = *(u8*)(*(0x142ad3238) + 0x70) & 1 (disasm TEST [RAX+0x70],1; Ghidra shows "+0xe" because the
  global is typed longlong*) -> it's 1 in the sandbox, so that's NOT the blocker.
  State 0 -> 1 needs the navmesh tile for the actor's position loaded: tile = DAT_142ae0848 tables, state field
  +0x418 must be 3 or 5. Suspect the rooftop spawn has no navmesh / navmesh streaming isn't requested without
  scripts. Next: log that tile state, or test on open ground; or skip pathing (TASK_GO_TO_COORD_* variants,
  FUN_140b0ce00 direct steer at nav+0x120).

### Hands / outfit pieces: FIXED (2026-11-13 06:46)
- Marston and puppets had no hands: right after creating the player the story script calls native FUN_1402826c0(goh,
  bit, 1) for bits 0x1a,0x17,0x19,0x1b,0x1c,0x1d,0x1e (rdrmp/bin/rdrmp_story_trace.log). The native sets that bit in the
  u32 at +0x114 of *(*(*(actor+0x50)+0x88)+0x110) = component visibility mask. sandbox.cpp ApplyStoryComponentMask() sets
  mask |= 0x7e800000 on the player and every puppet -> gloved hands, bandolier, rifle on the back.
- (The old native tracer printed 8-byte VM arg slots as 4-byte values: a[1] was the high dword of slot 0.)
- Model variation (create-actor a7 / factory param_5) = outfit; template_player has 21. ini player_variation /
  puppet_variation (default 12 = the story's; -1 = cycle per F9 for comparing).

### Puppet WALK/RUN ANIMATION: SOLVED (2026-11-13 07:20) via locomotion-desire override
- AI path route is a dead end here: nav requests (FUN_140b732c0 -> nav obj vt 0x141D24DB0) run 0->1->2->0 but return
  result code 6 with 0 points (+0x1fa / +0x2f8) -> no path (no navmesh on the story's rooftop spawn). Off the roof the
  player even freezes (no collision streamed outside the start area: spawn at (696,100,1199) floats, walking plays the
  anim in place). Streaming focus (*(0x142ac50d0)+0xa0, count +0xa8) DOES follow the camera; FUN_1405b3ff0 says loaded.
  -> open issue: collision/navmesh beyond the start rooftop (MP roaming needs it).
- Found with a 3-phase F8 diff (idle / W held / idle) + DR0 write watch on *(actor+0x38)+0xab0: per-actor brain update
  (FUN_140b0ba10, worker threads) -> FUN_140b61910(self, state) -> locomotion FUN_140ba6a80(loco, desire = state+0x240,
  ...), actor = *(loco+0x80). desire = the per-frame movement request (player input or AI), 0x110 bytes, plain data:
  [0x14] local right, [0x15] -0, [0x16] -forward (walk = -1), [0x34] remaining turn (desired - current heading, rad,
  + = left), [0x35] -1, [0x38] desired world heading + pi (rad), [0x3a] -1, [0x3f] 0xc2280000 | moving bit, [0x40] 0x1000.
  It produces *(actor+0x38)+0xab0 = -[0x16], +0xab4 = [0x14].
- core/locomotion.cpp detours FUN_140ba6a80 (prologue 15 bytes PIC, trampoline): captures the local player's desire; for
  registered puppets copies an override desire in before the original runs. Puppets need an AI brain (template_player
  has none): created on demand via FUN_140af93c0 (GiveGoToCoord once).
- IMPORTANT: don't teleport puppets every frame (resets locomotion -> idle slide). Teleport only to correct drift.
- Netcode v2: PlayerState carries the desire sparse (non-zero u32 words, max 20); snapshots split in chunks of 8.
  Receiver: decodes it, re-aims it at a point 0.5 m ahead of the interpolated network position (local move vector from
  the puppet's facing + [0x34] turn + [0x38] heading), applies it; teleports only if error > 2 m (moving) / 0.3 m (idle).
  Verified with rdrmp_bot walking a 4 m circle: puppet walks + turns with real animation, error steady ~0.45 m, no
  corrections. Running/sprint/jump desires not verified yet (needs a real second player).
### Collision / map streaming for the local player: WORKS (2026-11-13 08:00) — earlier "no collision" was wrong
- The off-roof freeze was a bad test: spawning ~20 m above ground freezes a newly created actor (probably no ground
  within the spawn ground-probe). Teleported 3-25 m up he falls and lands normally.
- Probe (rdmp.ini [debug] collision_probe=1: teleport 16 m every 2 s = gallop speed, 6 m above last ground):
  collision followed the player for 1 km of terrain (y 85 -> 118). At x~-271 he landed in a RIVER -> drowned
  (RDR John can't swim) -> death camera moved the streaming focus to garbage (-288,-670,-1023) -> everything
  unloaded. collision_probe=2 walks him into that river (x -257, y 105) to confirm.
- World stream grid: mgr *(0x142ac5098): cell sizes *(mgr+0x20) (64 m), width [0xe] (129 x 101), cells *(mgr+8),
  origin (-6144,-256) .. (2048,6144) (0x1422e42e0/e8/d0/d8), cell loaded = *(u8*)(cell+0xc); ~32 cells (~3-cell
  radius) loaded around the streaming focus. Focus points *(*(0x142ac50d0)+0xa0) (count +0xa8) = the camera.
  FUN_1405b3ff0(*(0x142ac50a0), pos, -, r) = "world loaded around pos"; FUN_140619440 = the grid check.
- TODO: death / respawn handling (drowning, falls) — the sandbox has no story respawn logic.
- Actor::vf31 (0x1402666f0, vtable slot 0xf8) = snap actor to ground (raycast FUN_140898020 up/down by param).

### Remote players stream in/out by distance: WORKS (2026-11-13 08:05)
- Remote puppet object = goh class Actor (vt 0x141C82B90). Actor::vf0 (0x140264ed0) activates (model resources via
  +0xc0, then FUN_140264b80 -> actor instance); Actor::vf2 (0x140265040) deactivates (releases the model/visuals; in
  practice the actor instance pointer survives and its blob shadow stayed -> we also park it 500 m below first).
- netclient: a remote gets an actor within [net] stream_in m (150), released beyond stream_out m (200) or on timeout
  (instead of parking). Re-entering range re-activates the same goh (StreamInPuppet: vf0, teleport to the network
  position, component mask re-applied — vf2 resets it to 0). Verified with stream_in=6/out=8 and the bot circling:
  out/in every lap, model disappears/reappears walking with hands, no crash.
- Not found yet: a real "destroy actor" (FUN_140221770 = change outfit/variation; FUN_140230620 = native that calls
  goh vf 0xf8 + FUN_1402eaa00 deactivate; FUN_1402eaa00 = generic goh deactivate incl. children).
### Direct start: no title / menus / save system (2026-11-13 09:37) — rdmp.ini skip_menus=2
- At UI event "startPadSelected" (boot data load finished; direct_start_event in the ini) the DLL (StartGameDirect):
  posts startScreenExit + mainMenuExit (only trigger the sign-in check), sets DAT_142ac2956 = 1 and calls
  FUN_140637520(loader) (= what the game-flow handler does on "fileStartupChecksComplete": HUD/map init incl. map shader
  "rdr2_map" — without it the minimap's first draw crashes at RDR.exe+0x167315), calls FUN_14063a900(loader) (loading
  screen + load state machines; the ValidateNormalMode callback's core), and sets game(*(0x142c456d0))+0x12e |= 1
  ("new game"). The per-frame level-load trigger FUN_1401f38d0(game) then posts "loadStart" and runs the level load
  FUN_1401f85b0 (-> FUN_1401f8a00 world load -> FUN_1401f9830 world setup -> root script sp_idle).
- Skips the title and mode-select menus and the whole save chain (fileStartPlayerChecks, fileSaveStart/Success,
  fileNewSaveCreated, fileLoad*). Verified: RDR2SAVE0.SAV / RDR2EXTRAS.SAV (%APPDATA%\.1911\Red Dead Redemption\profile)
  untouched across runs; player ready ~37 s after launch (was ~50 s).
- Calling it earlier (fileDoFreeSpaceCheck / setupComplete) or with only FUN_14063a900 doesn't start the load.
- Boot splash (legal pages, Rockstar logo, Double Eleven logo) still plays during the ~20 s boot data load. NOT the
  game's Flash UI: during it only the "Setup"/"Startup" scenes are active and hiding "Setup" changes nothing;
  FUN_1401f9c80 / FUN_1401f9e10 (legal renderer at game+0xd8) patched to ret change nothing either. "EndLegals" comes from
  ActionScript. Likely the Double Eleven layer. Removing it would only show black during the same load.
- UI scene helpers: uiMgr *(0x142aac8b8), root +0xa0, FUN_1400ef100(root, name), vf 0xd8(1) active, vf 0x158 hide.
### Local player death + respawn: WORKS (2026-11-13 09:03)
- Nothing in the engine respawns a dead player in the sandbox (the story script does it normally); no UI event either.
- Health component = *(actor+0x60): +0x20 float health (100 alive -> 0 dead), +0x18 u32 state (2 also while alive on
  slopes / in water -> NOT a death flag), +0x80 damage-type bits (0x2000 drowning?), +0x84/+0x158 time. Player has no
  AI brain (actor+0x28 = 0). Found by an alive/alive/dead/dead state diff of actor + pointees (TakeStateSnap).
- sandbox.cpp DeathMonitor (frame callback): health <= 0 -> wait respawn_delay_ms (ini, 5000) -> release the body
  (StreamOutPuppet: park + goh Actor vf2; this also clears playerInfo+0x5e9/+0x5ec) -> 1.5 s later create a new player
  -> SetupLocalPlayer (cache playerInfo+0x5ec, hands mask, locomotion capture, camera attach + reset, controls).
- The player owner goh[808] never takes another actor after its first player: the factory's object allocation
  FUN_140235cd0 (find-or-create goh by name in the owner; new ones via the type registry *(DAT_142ac2ad0+0x910) key
  0xf) returns null there even after the body is released (traced with 4 execute breakpoints, ArmExecBreakpoints).
  Respawned players are created into an ordinary base Layout with asPlayer=1 -> fully working local player.
- Creating in the same frame as the release "worked" once but the player was invisible; don't.
- A hidden "type anchor" puppet (def 0, parked 500 m under the spawn) is created at boot (EnsureTypeAnchor) - turned out
  not to be the fix, harmless; keep in mind if template unloading ever matters.
- Verified: drown -> respawn on the roof with hands, controllable; twice in a row; no crash.
- TODO: death animation/ragdoll + respawn sync for remote players; respawn points other than the spawn.
### Netcode MVP: WORKING (2026-11-13 02:40)
- shared/net_proto.h: UDP, HELLO/WELCOME/STATE(20 Hz)/SNAPSHOT(20 Hz, all others)/BYE, max 24 players/datagram.
- server/server.cpp -> bin/rdrmp_server.exe [port] [bind ip]; server/bot.cpp -> bin/rdrmp_bot.exe (walks a circle).
- core/netclient.cpp (rdmp.ini [net] enabled/server/port/name/interp_ms): net thread starts once the player exists
  (boot-time Winsock activity killed an early socket); recreates the socket on errors. Main thread per frame
  (mainthread.cpp AddFrameCallback: frame boundary = game clock 0x142314d3c changes; the VM runner is called
  several times per frame) publishes local pos/heading/speed and teleports one puppet per remote player,
  interpolated 100 ms behind. Stale (3 s) remotes are parked 500 m below (no actor-destroy routine found yet).
- Test: rdrmp/tools/run_net.ps1 (server on 127.0.0.1 + bot + game, screenshots, closes all). Verified: bot appears as
  a Marston puppet walking its circle in front of the player.
- TODO: walk/run animation for puppets, actor destroy, nameplates, weapons/aim/fire, horses, 2-PC test.
Gotchas: never __try-probe memory (RDR's VEH crash reporter fires on first-chance AVs -> RDRMessage dialog);
"near" is an empty macro in windows.h; a missing root script is fatal.
Script VM native call site 0x1401E4353 is hooked (natives.cpp): logs/traces native calls (ctx: [0] result*, [8] argc, [0x10] args).

## DIRECTION (2026-11-12): MTA-style sandbox, NOT reviving Rockstar MP
Goal: only the map and players. World events and story/ambient scripts are scrapped. Remote players are fully synced and spawned
in the world by our own code. Game mechanics (run, ride, shoot) come from the engine. Our own netcode (server/client),
like Multi Theft Auto. The native session/join layer below is reference only: it is broken in several places
(joiner handshake stripped, peer-info import `FUN_140457a60` always returns false, socket never opened).

MTA-style RE targets:
1. Injection + per-frame hook: RDR.exe imports DINPUT8.dll, so a dinput8 proxy works (same approach as TDMP). Check whether MTLX.dll anti-tamper reacts.
2. Boot control: root script `$/content/main` (+ `main_z`), loaded by `FUN_1401ee380` (also `FUN_1401ec230` startup).
   Skip it, or run a minimal script, to get "map + player only". Script VM: `scrThread` (`FUN_1401e6710`).
3. Local player: player actor pointer, position/heading/velocity, movement state, current weapon, aim/fire, health.
4. Remote players: create an actor with a player model at a position, then drive it (position/heading interpolation, movement anims,
   aim/fire via the task system: `TASK_SHOOT_*`, `TASK_DRAW_HOLSTER_WEAPON`, ...), plus damage/death sync.
5. Netcode in our DLL (ENet/UDP), server-relayed, with interpolation, MTA-style.

## Build analyzed
- `RDR.exe` (Razor1911-patched copy; build timestamp 2024-11-15). The original `RDR.exe.rzr` has an encrypted `.text` (Rockstar `.rkstr` stub), so only the decrypted copy is readable statically.
- Addresses are for **this build only**. Hooks for a mod must use byte-pattern signatures, not hard-coded addresses.
- Ghidra 11.1.2 + JDK 17. Projects are in `ghidra_proj/` (`rdr_main` = RDR.exe, `rdr_misc` = small binaries).

## Analysis stats
- 50,117 functions after the vtable pass (VtApply created 8,957), 41,132 after the .pdata fix-up (`FixPdata.py` created 3,554 missed functions; 4,043 uncovered entries were chained-unwind fragments, which is expected). 32,402 strings, image base 0x140000000.
- RTTI is stripped (only std/d11 helper classes), so game class names come from strings, not vftables.

## Other binaries (not relevant to MP)
- `d11steamutilities.dll`: Steam achievements only.
- `MTLX.dll`: anti-tamper/integrity module. Leave alone.
- `RDRMessage.exe`: error message box.

## Key finding: the console MP code is still compiled in
Engine layer (RAGE net):
- `netConnectionManager` (FUN_14044c7a0), `[RAGE] netConnectionManager Worker` (FUN_14044c200)
- `netThreadPool` (FUN_14042d590), `snuSocket` / net shutdown (FUN_14042f720)
- Session: `snHostSessionTask`, `snConnectToPeerTask`, `snMigrateSessionTask`, `snAddRemoteGamerTask`, `snEventSession*`, `snMsg*`, `MigrateHostMsg`, `BootPeerMsg`
- rline / presence / QoS: `rlPresenceEvent*`, `rlMsgQosProbeRequest/Response`, `rlDbMsg*`
- `netEvent*` (ConnectionRequested/Established/Closed, FrameReceived, AckReceived, BandwidthExceeded)

Game layer (RDR net objects):
- `NetObjectManager::*Ownership*` events, `NetObjectBase::DataSyncMsg`, `NetObjAckMsg`, `NetObjectCreate/Remove/LocallyDestroyed Event`
- Net object types: `NetGOHActor`, `NetGOHProp`, `NetGOHPickup`, `NetGOHMovable`, `gohNetSessionObject`, `NetArrayHandler`
- Events: `NetActorDeath`, `NetActorEvent`, `NetGameWeather`, `NetPickup::TakePickupEvent`, `SyncData` (FUN_140497f40)
- Game modes/UI: `Posse`, `FreeRoam`, `GAME_LOBBY`, `FREE_ROAM_POSSE_*`, `mp_HUD.swf`, `rdrLobbyLayer`, `NetPlayersList`
- Entry points: `net.StartOnline` (FUN_1401ec230), `net.EnterMultiplayerStartMenu`, `net.inGameSession` / `net.readyForInvite` (FUN_140470ef0), `IsNetGame`

## Transport layer: CONFIRMED stubbed (socket send/recv compiled out)
No winsock imports, strings, or dynamic loads. Tracing the code confirms it:

Init chain (who owns what):
- `FUN_14042eeb0` = main net Init (pair of shutdown `FUN_14042f720`). netMgr+0x40 = socket object, +0xd08 = connection-manager owner.
- `FUN_140444fd0(owner, socket)` allocates cxnMgr (0x860 bytes, ctor `FUN_14044bd10`), calls Init.
- `FUN_14044c200` = `netConnectionManager::Init(this, allocator, ?, socket, ?)`.
  cxnMgr+0x8 = socket*, +0x10 = queue-transport*, +0x18 = allocator (vtbl +0x10 alloc, +0x20 free), +0x20 = pending-send list when no transport.
  Starts thread `FUN_14044bb70` "[RAGE] netConnectionManager Worker".
- Socket object: +0x4 = int handle (<0 = invalid), +0x20 / +0x68 = critical sections, +0x50 = listener list, +0x98 = packet-recorder list, +0xe4 = u16 port.

Worker loop: `FUN_14044bb70` -> every 10 ms `FUN_14044e5d0(cxnMgr, timeGetTime())`, which runs:
- `FUN_14044f2a0` = **receive stage**. Socket branch: the netSocket::Receive call was compiled to `len = *(int*)(sock+4) >> 31` (always 0 or -1), so **nothing is ever received**. Listener/recorder callbacks around it are still there.
  Queue branch (sock==NULL, +0x10 set): `FUN_14046c430` pops `{u64 addr, u8* data, int size, next}` nodes (max 0x400) from a locked list at q+0x18.
  Each received packet goes to `FUN_14044df40(cxnMgr, &addr, buf, len)` = **process incoming packet** (decrypts, dispatches to connections).
- `FUN_14044efe0` = send stage: `FUN_14044da60` packs outgoing frames, then `FUN_140450470(cxnMgr, &addr, buf, len)` = **send**.
  `FUN_140450470` only checks size <= 0x400 and handle >= 0, then returns 0 (success) **without sending anything**. Return codes: 3 = too big, 7 = bad socket, 2 = alloc fail.
- `FUN_14044f660`, `FUN_14044f960`, `FUN_14044bbf0` x16 = connection/channel bookkeeping.

Packet facts:
- Address is 8 bytes (looks like u32 IPv4 + u16 port + pad). Max packet 0x400 (1024) bytes.
- Packets are RC4-encrypted above the transport: `FUN_14046c1d0` (key schedule, 32-byte key) and `FUN_14046c270` (crypt), used by the packer and `FUN_14044df40`. Both peers run the same code, so a transport only moves bytes.

## Socket object: the handle is never set
- Startup: `FUN_140470330` (game net startup) allocates netMgr (0x52248, ctor `FUN_14042e200`), stores it in global **`DAT_142c45620`**, calls RAGE init `FUN_14042eeb0`, then registers channels:
  0 "game session", 1 "party session", 2 "complaint", 5 "voice", 7 "object", 8 "script".
- `FUN_14042eeb0` calls `FUN_140451ef0(netMgr+0x38 socketMgr, netMgr+0x40 socket, 0x67bc)` = **netSocketManager::AddSocket, port 26556**.
  AddSocket writes **handle(+4) = -1** and port(+0xe4) = 26556.
- `FUN_140452170` = netSocket ctor (handle -1), `FUN_140452200` = dtor, `FUN_140451fd0` = RemoveSocket (resets to -1).
- No code in the socket manager writes a valid handle (no socket()/bind() equivalent exists). The handle stays -1, so:
  receive length = `-1 >> 31` = -1 (nothing), and send `FUN_140450470` returns **7 (bad socket)** before reaching the stub.
  -> The send hook must **replace FUN_140450470 entirely**, and the receive hook must replace `FUN_14044f2a0`.
- netSocket layout: +0x4 handle, +0x8 owner mgr, +0x20/+0x68/+0xb0 critical sections, +0x50 listeners, +0x98 recorders, +0xe4 u16 port, +0xe6 flags.

## Session layer (names recovered from GetName getters)
- `VtApply.py` named 671 classes through their `lea rax,[name]; ret` getters (`GetName_*`, `vt_*`, `*__vfN`). Task vtable: vf0 dtor, vf1 GetName, vf3 Start, vf4 Update(dt), vf7 Finish(result).
- Task classes: `snHostSessionTask`, `snHostStartTask`, `snQueryHostConfigTask`, `snQueryConfigTask`, `snGuestRegisterForMatchTask`, `snGuestStartTask`, `snHandleJoinRequestTask`, `snAddRemoteGamerTask`, `snConnectToPeerTask`, `snJoinGamersToRlineTask`, `snLeaveGamersFromRlineTask`, `snDropGamersTask`, `snMigrateSessionTask`, `snChangeAttributesTask`, `snSetInvitableTask`, `snRegisterRemoteGamersForMatchTask`, `snEndTask`, `snDestroyTask`.
- `FUN_1404418f0` = **snSession incoming-message dispatcher** `(session, ?, ?, envelope*)`. The envelope starts with the 16-byte sender address; +0x18 = payload ptr, +0x20 = payload size. Each branch deserializes one snMsg (`FUN_14043a290`, `FUN_14043aa70`, ...).
- Adding a remote player: dispatcher -> alloc 0x450 -> `FUN_140460120` (ctor; **snConnectToPeerTask is embedded at +0x330**) -> `FUN_140460430` configure -> `FUN_140442690` (session AddRemoteGamer) + `snJoinGamersToRlineTask`.
- `FUN_14044cce0(cxnMgr, ..., request*, status*)` = **peer-address resolve request**, queued in the list at cxnMgr+0x7a8. Connect-task fields: +0xc0 request, +0xc4 resolved addr (u32 ip, u16 port), +0x10c status.
- `snConnectToPeerTask__vf4` (Update) branches on status:
  3 = use the resolved address; 1 = pending, wait; else = **make up `127.0.0.16+n : 0xFFFF`** (`0x7f000010 | counter`).
  Then `FUN_14044ce50` = **cxnMgr::OpenConnection(addr, channel<16)** -> connection id.
- Nothing found that completes the resolve requests on PC (the console used XNet/NP signaling), so peers most likely end up on fake 127.x addresses.
- Joiner side: `snQueryHostConfigTask__vf4` sends the config request (up to 10 tries) via `FUN_1404350b0(DAT_1422de544, msg)`. **Still open:** where the host's address comes from in the first place (rline session info / matchmaking / invite).

## Join flow: the host half survives, the joiner half was stripped
Message descriptors live in .data around 0x1422de380-0x1422de840: `+0x0` base, `+0xc` **u32 message id**, `+0x18` name ptr.
Writing a header = `FUN_1404350b0(id, bitbuf)`; reading = `FUN_140431870` then compare with the id.
Code references per message id (cmp = receive/deserialize, load = send/serialize):

| message | id addr | receive | send |
|---|---|---|---|
| rlMsgSearchRequest | 1422de384 | - | `FUN_1404564d0` (serializer, called by rline pump `FUN_140456bb0`) |
| rlMsgSearchResponse | 1422de3bc | `FUN_140456bb0` | - |
| snMsgConfigRequest | 1422de544 | dispatcher `FUN_1404418f0` | `snQueryHostConfigTask__vf4` (**builds it, never sends, then Finish(failed)**) |
| snMsgConfigResponse | 1422de57c | `FUN_14045f130` | (not found by id) |
| snMsgJoinRequest | 1422de5b4 | `FUN_140439740` deserialize, `FUN_140441460` host handler | **none: joiner send path removed** |
| snMsgJoinResponse | 1422de5ec | **none: joiner receive path removed** | `FUN_140439b30` serialize, `FUN_14043ef90` host send |
| snMsgAddGamerToSessionCmd | 1422de624 | `FUN_14043a290` -> dispatcher -> snAddRemoteGamerTask | `FUN_14043a070` |
| snMsgStartMatchCmd | 1422de6cc | dispatcher (-> snGuestStartTask) | `FUN_14045e3f0` |
| snMsgRegisterForMatchRequest/Response | 1422de694 / 1422de50c | yes | yes |
| snMsgRequestGamerInfo / SessionMemberIds | 1422de81c / 1422de7e4 | yes | yes |

- Session entry points: `FUN_14043d090` -> `FUN_140442e80` = **snSession::Host** (creates snHostSessionTask; called from `FUN_14044a160`, `FUN_1404f86b0`, `FUN_1404cda80`).
  `FUN_140443010` = HostStart. `FUN_14043eaf0` = host creates snHandleJoinRequestTask.
  `FUN_14043d940` = snSession::QueryConfig on the global session **`DAT_142b54a50`** (driven by netMgr update `FUN_140430f00`).
- No `snJoin*Task` class exists. The joiner's join handshake (send JoinRequest, handle JoinResponse) was dead-stripped,
  and the host-config query is gutted (always fails). Everything **after** joining exists on both sides
  (AddGamerToSession, StartMatch, RegisterForMatch, member ids, gamer info, object sync).
- So the mod must **re-implement the joiner's handshake**: build snMsgJoinRequest in the same bit layout the host's
  deserializer `FUN_140439740` reads, send it to the host's virtual address, parse the host's JoinResponse
  (layout = serializer `FUN_140439b30`), and put the guest session into the joined state the original join task would have.
  Next RE step: get the exact field order from `FUN_140439740` / `FUN_140439b30` and find the session state fields
  (session+0xca8 state, session+0x195 role) that the guest path in the dispatcher checks.
- **Host side is broken too:** JoinRequest field 2 is read by `FUN_140457a60` (peer-info import), which peeks up to 88 bytes,
  stores the first u64 at +0x10, and **always returns false** (verified in the disassembly: every path runs `XOR AL,AL` before the only RET).
  It's also called by the AddGamerToSessionCmd reader `FUN_14043a290`, so both deserializers always fail.
  JoinRequest layout: u64 token(!=0) | peerInfo blob(<=88B) | 2b | 10b len(<=0x200) | len bytes | 1b | 1b.
  JoinResponse layout: u64 | 2b result | 3b | 10b lenA | 10b lenB | A bytes | B bytes.
- rline search: `FUN_140456bb0` (pump; called from `FUN_140453d70`, `FUN_1404546e0`) still sends search requests and handles responses (magic `0x8ae45` in the request).
  It could serve as LAN-style discovery, but whether it actually reaches the network is unverified.

## Plan for our transport (proxy DLL / ASI)
1. Hook `FUN_140450470` (send) -> forward (addr, buf, len) over our own UDP/ENet/Steam socket; return 0.
2. Hook `FUN_14044f2a0` (receive stage), or let it run, and feed packets by calling `FUN_14044df40(cxnMgr, &addr, buf, len)` from the worker thread.
3. Map RAGE 8-byte addresses <-> real peers (a virtual-IP table in our layer).
   Best place: hook `FUN_14044cce0` (resolve) and complete it right away with status 3 and **our** virtual address for that peer,
   so the game never falls back to the anonymous 127.0.0.16+n addresses (which carry no peer identity).
4. Joining: there is no Join to feed a host address into. The joiner handshake must be re-implemented (see "Join flow").
   Our launcher/server supplies the host's virtual address; our DLL sends JoinRequest, handles JoinResponse,
   and moves the guest session to "joined". After that the game's own AddGamer/StartMatch messages take over.

## Tools
- `ghidra_scripts/ExportInfo.py`: function/string/import/RTTI exports -> `ghidra_out/<bin>/`
- `ghidra_scripts/DecompileList.py <out.c> <addr...>`: decompile chosen functions, plus a .pdata coverage report
- `tools/RipScan.ps1`: find RIP-relative code refs to addresses (finds tiny leaf getters Ghidra never disassembled)
- `tools/VtScan.ps1` -> `ghidra_out/RDR.exe/vtables.tsv`: all 6,869 MSVC vtables (COL + code pointers)
- `ghidra_scripts/VtApply.py`: creates vtable slot functions and names classes from getters
- `ghidra_scripts/DecompByName.py <out.c> @<regex-file> [max]`: decompile by name regex (pass the regex in a file; cmd mangles | and ())
- `ghidra_scripts/RefsTo.py`, `FindPtrs.py`: refs to addresses / raw pointer search
- `ghidra_scripts/FixPdata.py`: creates functions from .pdata entries Ghidra missed
- `ghidra_scripts/Callers.py <out.c> <0|1> <addr...>`: callers of each target (+ decompile them)
- `ghidra_scripts/CalleeTree.py <out.c> <depth> <max> <addr...>`: decompile a call tree
- Run on the saved project: `analyzeHeadless ghidra_proj rdr_main -process RDR.exe -noanalysis -readOnly -scriptPath ghidra_scripts -postScript <script> <args>`. Only one process can open the project at a time.
- `decomp/net_core.c`: first decompile batch of the net functions above
- `decomp/cxnmgr_worker_tree.c`, `decomp/socket_io_tree.c`, `decomp/send_tree.c`, `decomp/*_callers.c`: transport trace

## Boot splash / loading screen / engine fade (2026-11-13 ~11:45) — core/bootvideo.cpp + sandbox.cpp
- Boot render FUN_1405f4390 clears black, then (legals controller *(0x142AC5130), countdown +8 > 1000) calls
  FUN_1405f4cc0 = draw legal pages (Flash) + Rockstar logo Bink. **skip_legals=1 NOPs that call (0x1405F44F2)**; the
  legals LOGIC still runs (EndLegals posted, platform sign-in finishes behind it). Skipping the logic instead
  (countdown=4, ctrl+0x11 done, uiMgr+0x78) outruns the sign-in -> "You have been logged out of Rockstar Games" box.
  Game's own skip: FUN_1406558e0/FUN_1403303d0. Main update FUN_1401f5820 continues when (ctrl==0||ctrl+0x11) && uiMgr+0x78.
- Logo video: one in-memory Bink (logo_hires/lowres, loaded in ctor FUN_1405f4690 unless *(u8*)0x142B67D80) opened in
  FUN_1405f5070 after "LegalsCompleted". BinkOpen returning null STALLS the boot (player flag +0x79 never clears).
  **skip_videos=1: BinkOpen IAT hook sets FrameNum(+0xc)=Frames(+8)** -> FUN_140752740 ends it -> normal path.
- Loader UI movie wrappers (Scaleform, draw = vtable slot +0x28 FUN_1401157d0, advance +0x18; name at +0x68):
  "loading" vt 0x141CE4118 (LoadingScreen art + revolver), "startscreen" vt 0x141CE5318 (title logo), "fader"
  vt 0x141CE3C18 (loader+0xc418), "popup" vt 0x141CE3B18. **hide_loading_screen / hide_title_screen stub slot +0x28**
  (logic keeps running: Loading* events, loadComplete). Hiding the LoadingScreen/LoadingSpinner SCENES kills the
  Loading* events -> finish never happens.
- Engine fade: FadeOut FUN_14063b1b0(loader, sec, alpha, flag) (pushes "FadeToBlack" scene), FadeIn FUN_14063b0a0
  (pops it, posts loadComplete; frame update calls it on finish with loader+0x25814 sec / +0x25818 alpha).
  Fader state: +0x257e5 bit8 active, +0x257ec bit1 to-black, +0x25800 value, +0x25804 alpha, +0x25808 speed (1/sec),
  colour via FUN_14063b3c0(loader, float rgba[4]). The fader is NOT drawn while the loading scenes are up (not in the
  loading render list) -> sandbox does FadeOut(0) at player spawn, waits fade_hold_ms (camera settles; otherwise a
  scripted-camera brick wall / desert flashes), then FadeIn(fade_in_ms). Verified smooth 1.5 s fade.
- The "black" while loading measures #1C1819 in screenshots, identical to the engine fader's full black -> it is the
  game's black after its brightness/gamma, not a leftover layer. Debug aids: [debug] ui_draw_log (detour on
  FUN_1401157d0 via DetourPic), fader_log, fade_color_test (red fader), hide_scene.

## Own game loop / boot (rdmp.ini [game] own_loop=1) — core/gameflow.cpp (2026-11-13 ~13:15, WORKING)
Engine structure: main FUN_14007f860 -> app FUN_1401fd270 (vt 0x141C7B008; +8 = create game + Game::Init, +0x18 = tail
jump to Game::Run). Game object vt 0x141C7AFD0: +8 Init FUN_1401ee680, +0x10 Run FUN_1401f3140 (frame loop),
+0x28 Update FUN_1401f5820. Render thread FUN_1405d9780: rm(*0x142AC50D0)+0x34 = loading mode -> FUN_1405f4390 loop
(boot/loading frame: legals controller, Bink loading movies rm+0x6e8, loading UI), else FUN_1405c8720 normal render.
Run starts the front-end script "$/content/PressStart" (thread id at game+0x144) before its loop.
What own_loop installs (all verified to boot, player ready ~16 s, no crash):
- OurRun (game vt +0x10) = transliteration of FUN_1401f3140 minus PressStart, plus BootStep. NOTE FUN_1400B70D0 takes a
  persistent frame-time slot (Run passes the same stack slot each frame) - keep it outside the loop.
- Init: JMP 0x1401EF9D7 -> 0x1401EFA21 skips the legals/logo controller (keep FUN_14011a220 UI renderer init before it!).
- OurLoadingFrame replaces FUN_1405f4390: frame begin (*0x142AC50C8 vf20), streamer bookkeeping (ac4c98 CS, FUN_14053ab10,
  FUN_140517a50), FUN_140ebf550, black clear, UI ADVANCE ONLY (ui+0x1e0=1; FUN_140119f10(ui) - movies (HUD, fader) load
  inside their Advance), present (vf28).
- Loading ("loading" vt 0x141CE4118) + title ("startscreen" vt 0x141CE5318) wrappers: Advance/Draw stubbed.
- BootStep: wait game signed-in check FUN_140ef0e70(*0x142AAC8D8) (what the legals' 7 s hid; else "logged out" box,
  shown by FUN_140330a60) -> uiMgr+0x78=1 (boot gate) -> BeginWorldLoad: 0x142AC2956=1, close "Startup" scene,
  post startScreenExit/mainMenuExit, FUN_140637520 (HUD map init + UI movie load requests), game+0x12e|=1 -> hold
  loader fader black (+0x25800=1, ec|=1, e5&=~0x48) until FUN_1401f38d0 runs the level load -> drop top UI phase
  "effects" (the loading screen cleanup normally pops it; it covers all UI) -> stream_ms -> RequestFinishLoading ->
  loader+0x257c0=1 (HUD draw switch, cleared by the level load; front end sets it via native 0x140655680).
- WORLD STREAMING START (the key fix): FUN_1401f3ad0 does a one-time step when the title screen ("StartScreen1") shows:
  if (*(0x142AC5140)+0x124) { +0x124=0; DAT_1422A3E2C=1; grid(*0x142AC5098)+0x28=0; sc+0x70=(100.0f,0);
  sc+0x78=0; sc+0x7c=10.0f; sc+0x11f=0 }. Without it the world grid never streams (0 cells loaded) -> no collision,
  the player stays a dormant low-LOD shell (no hat, can't walk, turning only), no minimap. BeginWorldLoad does it.
- Result (verified 13:12): launch -> player in world ~16 s, engine black throughout, engine fade-in, HUD + minimap,
  full-LOD Marston with hat, walks; netcode puppets work. Remaining cosmetic: one ~0.1 s flat dark-brown frame at the
  loading->normal render hand-off. Debug aids: [debug] scene_report (state dump at spawn+2/6 s: game state, input
  pads, desire, stream grid, UI list), input_trace, ui_draw_log, signin_log; [game] replace_loading_frame=0.
- Bisect lesson: compare live state dumps between own_loop=0/1 (scene_report) instead of guessing; the grid line
  ("N loaded") found it.

## Spawn point = story start (2026-11-13 ~14:05)
- Story boot (tools/run_story.ps1: root script main, sandbox spawn off, [debug] pos_log=1): main creates the player at the
  placeholder rooftop (736.49, 89.17, 1199.33), plays the intro (Morningstar ferry at Blackwater, cameras around
  (858, 80, 1245)), then places John at (-2208.91, 16.51, 2605.41) heading -100.3 = Armadillo Station (train arrival).
  After that the story loops loadStart/loadComplete (also with the engine flow) - not needed for the sandbox.
- rdmp.ini spawn_x/y/z/heading now = Armadillo Station; respawn uses the same point.
- gameflow: after our boot, later "loadStart"s are finished by us after stream_ms (no loading screen to end them).

## Radar map + pause map: WORKING (2026-11-17 ~21:40) — core/gameflow.cpp BootStep HUD-on step
Root causes were script state our boot never sets (no story script runs), not missing textures or render bugs.
- **Radar zoom.** The radar draws its world-map tiles (`streaming_worldMap_N`, 12x12 grid of 500 map units, one DXT1
  596x596 texture each) through an ortho camera in FUN_140692340, with ortho half-size = radar+0xc (0.8371) *
  max(zoom / 0.75, 1) * 0.5, centred on (player + radar+0x18/+0x1c) * 0.8371.
  - zoom = entry+0x2a0 of the HUD entry list loader+0x25760 (+8 entries, stride 0x350, +0x10 u16 count, 5 entries).
    It is the radar diameter in metres (the blip code compares distances against zoom/2).
  - Constructor default 1.2: the radar shows ~1 texel, which looks blank/transparent.
  - Scripts set entry+0x2a8 via native 0x140654430; the HUD reset FUN_1406399e0 copies +0x2a8 -> +0x2a0.
  - Fix: SetRadarZoom writes +0x2a8 and +0x2a0 on all entries; rdmp.ini [sandbox] radar_zoom=150.
- **Radar flags.** These are still needed: DAT_14227D32E (tile streaming) and DAT_14227D32F (map draw).
  - The earlier projection globals (bee918..) and the mapRenderProxy show did not matter; both were removed.
- **Pause map.** It opens fine: open flag 0x142AC50BB, `worldMap_LOD_NN` txd loads.
  - The draw FUN_140600f20 also needs 0x142AC5257 <- 52AA. FUN_140603410 sets that only when the map tiles around the
    centre are in AND the UI texture dictionary "blips" is loaded.
  - "blips" is requested by scripts through FUN_1400fba10(*0x142C45608, name): request UI txd, list +0xf8 entries
    stride 0x60, +0x6f8 count.
  - Fix: request "blips" at HUD-on. The full map then shows the whole world with the player marker.
- **How it was found.**
  - [debug] radar_trace=1: detour on the tile draw FUN_140690ef0 logs draws/tile range/live zoom.
  - [debug] map_trace=1: pause-map gates + "blips" state.
  - The radar's blend is state 2 = mode 4 (src * destAlpha + dest, mask written with color-write 8). It is not broken:
    the map is a translucent overlay by design.
  - The game renders through D3D12 (Double Eleven "d11" layer). The state setter is FUN_140147110(id, value), which
    stores into (&0x142AC0B60)[id].
- Script notes: the radar effect `rdr2_map` (techniques drawMask/drawMap/drawRadar/drawPath, vars DiffuseTex, MaskTex,
  NormalTex, light vars, g_fAspect, g_fMaskAnim) is set up in FUN_14068aba0.

## Pause menu without freezing the world (2026-11-17 ~22:40) — rdmp.ini [sandbox] no_pause_freeze=1 (default)
- **How pausing works.**
  - The pause menu calls FUN_140331580(uiObj *0x142AC4748), which bumps the pause counter +0x3df and requests game
    state 4 (next 0x1422D9C0C, change flag 0x1422D9C08).
  - Closing it calls FUN_1403315b0, which decrements the counter and requests state 3 when it reaches 0 and
    PauseMenuScene is gone.
  - Game::Run applies the state to 0x1422D9BD0: 3 = play, 4 = paused, 5 = loading/fade, 6 = exit.
- **The freeze is an MP rule.** Every "paused?" test is
  `state == 4 && (!*(net 0x142C456B8 + 0x1634) || DAT_142B0EBC8)`. In a net session the world keeps simulating in
  state 4.
  - These tests are in Game::Update (world sim, 12 sites), FUN_1405c5220 (game clock 0x142314D3C) and the renderer.
  - net+0x1634 is read in about 170 places (actor/prop/pickup vf40, UI), so it must not be set globally.
- **Fix (gameflow.cpp).**
  - OurRun presents state 3 only around the Game::Update and FUN_1405c5220 calls when state is 4, then restores 4.
    The UI and menu logic and the unpause path keep seeing "paused".
  - Three render-thread tests are patched JNZ -> JMP so they take "not paused": 0x1405C8A46, 0x1405CBD8E, 0x1405CC44F.
  - Verified: the game clock advances in real time behind the menu, the menu opens and closes with Esc, the world is
    drawn live behind the menu (watch, camera), and remote players keep moving (user-confirmed).
  - Debug: [debug] pause_trace=1. Test with tools/run_pause.ps1 -NoFreeze 0|1.

## Server layer on the MafiaHub Framework (2026-11-19)
- Framework: C:\Users\ELITE\MafiaFW\Framework-develop (MafiaHub Framework v10.5.1, zip of develop). Verified unmodified:
  all 5.7k framework files keep the archive timestamp 2026-07-21 23:30:24; the 6 newer files are the MafiaHub Services
  package + its cache, which the framework's own CMake downloads. Not used: Desktop\Framework (Soapbosnia fork, modified).
- Our project: code/projects/RDRMP (auto-discovered; the framework is not edited). See its README.txt.
  RDRMPServer (FrameworkServer) + RDRMPClient (headless FrameworkClient: \bot / \bridge). Build: code\projects\RDRMP\build.bat.
- Entity RDRMP::Player = net_proto v2 PlayerState + health + nickname. Y-up -> interest grid XZ (framework default).
- Bridge: rdmp.dll unchanged; it talks the old UDP protocol to 127.0.0.1:27020, the bridge is its framework client
  (WELCOME id 0xFFFF; remote ids = framework net ids). Test: rdrmp\tools\run_fw.ps1 -> bot1 puppet walks in front of the player.
- Gotchas: GitHub is blocked on this PC (TLS handshake fails; jsDelivr works). A configure with a changed FetchContent
  state deletes build/_deps/v8pp-src before re-cloning -> v8pp now from C:\Users\ELITE\MafiaFW\deps\v8pp-2bada50
  (-DFETCHCONTENT_SOURCE_DIR_V8PP). Server bound to 0.0.0.0 raises a Windows Firewall prompt; tests use --host 127.0.0.1.
- 2026-11-19 ~17:50: NATIVE in-game framework client works. rdrmp_fw.dll (code/projects/RDRMP/client_dll, FrameworkClient headless,
  /MD) is loaded by rdmp.dll's net thread (LoadLibraryEx + LOAD_WITH_ALTERED_SEARCH_PATH from [net] fw_dir so libnode/libcef
  resolve), C table rdrmp/shared/fw_api.h. rdmp.ini [net] backend=framework|udp. Health (*(actor+0x60)+0x20) is sent; server
  'tp' -> ForceState -> fw poll_forced -> TeleportLocalPlayer on the main thread (verified). Framework logs -> bin\fwlogs + rdrmp.log.
  Quirks: server drops the first stdin line; framework ignores --apihost/--apiport (our main.cpp applies them).
- 2026-11-19 ~17:58: two game copies on one PC FAIL at the platform layer: the second RDR.exe (RDMP.exe --instance 2) exits ~1 s
  after start (our DLL had just loaded; the game's own boot never ran), and the first copy shows "You have been logged out of Rockstar
  Games". Rockstar sign-in = the crack's platform layer -> not analysed/bypassed (project rule). Two players need a second PC or a VM.
  Kept: RDMP.exe --instance N / --name X (rdrmp_N.log, name+N, spawn 5 m ahead facing player 1), shot.ps1 -ProcessId/-Front,
  tools/run_two.ps1. RDRMPServer now binds 127.0.0.1 by default (--host/--apihost 0.0.0.0 for LAN).
- 2026-11-19 ~18:06: the user saw "logged out of Rockstar Games" + black screen with ONE copy. Cause: started via PlayRDR.exe/RDR.exe
  (no rdmp.dll; rdrmp.log untouched). RDMP.exe launches fine (verified twice). Added: RDMP.exe starts the local RDRMPServer.exe
  (own console) when [net] start_local_server=1, backend=framework, server=127.0.0.1 and none runs; desktop shortcut
  "RDR Multiplayer.lnk" -> rdrmp\bin\RDMP.exe. Verified: shortcut -> server + sandbox + connected in 26 s.
- 2026-11-19 ~18:13: TWO COPIES ON ONE PC WORK (SDMP-style). RE: the game's main FUN_14007f800 calls FUN_140eb8e40 =
  CreateMutexA("esdksfkfhkhdfj2255984x") and returns 1 when GetLastError()==ERROR_ALREADY_EXISTS (game code, .text). core/instancediag.cpp
  IAT-hooks CreateMutexA and clears ALREADY_EXISTS for that name when InstanceId()>1 (RDMP.exe --instance N). The earlier
  "logged out of Rockstar Games" on copy 1 was a side effect of copy 2 exiting through that guard; with the guard passed both
  copies sign in and play (verified: both in world, both connected, each sees the other as a puppet, no warning).
  Shortcuts: Desktop "RDR Multiplayer" (copy 1 + local server), "RDR Multiplayer (2nd copy)" (--instance 2). Test: tools/run_two.ps1.
- 2026-11-19 ~18:40: the random "logged out of Rockstar Games" box with ONE copy (via RDMP.exe) = boot race. RE: UI alert update
  FUN_140330a60 (per frame in Game::Update) tail: if !FUN_140ef0e70(*0x142AAC8D8) && (legals ctrl *0x142AC5130 == 0 || ctrl+0xd)
  && StartScreen1/2 not active (vf+0xd8) && uiMgr(*0x142AAC8B8)+0x7c == 0 -> show "SocialClubReturnToMenu" once (+0x3e9 latch);
  OK -> AwaitRebootTransition -> legals (black screen). Real boot: legal pages cover the first ~7 s. Ours has no legals ctrl, so
  the check ran from frame 1 and sometimes beat the sign-in (which only progresses inside the game loop - a pre-loop wait just
  times out) by 4-12 ms. Fix (gameflow.cpp UiAlertUpdateHook, DetourPic 14-byte prologue): skip FUN_140330a60 while boot phase 0
  && !SignedIn() (max 20 s); after sign-in it runs every frame unchanged. Verified: race run (controllerDisconnected 5 ms before
  sign-in) no box; 3 more launches clean.
- Gotcha: the game's NVIDIA DLSS module spawns ~15 nvngx_update.exe per launch; they outlive the game and inherited our log handle
  (rdrmp.log locked -> rdrmp_pid*.log fallback). log.cpp now opens with "wN" (non-inheritable). Also: relaunching while the old
  RDR.exe is still exiting hits the game's own single-instance mutex (new copy exits with 1) - wait for the old process to exit.
- 2026-11-19 ~19:20: copy 2 crashed on its first puppet spawn: FindLayoutHandle (goh scan) read an object's vtable right after
  IsReadable() said OK (race with the streaming threads freeing objects right after world load). Fix: TryRead64 via
  ReadProcessMemory(self) - NOT SEH: the game's crash reporting reacts to first-chance AVs and starts RDRMessage.exe even when the
  fault is handled. Verified 2 rounds x 2 copies via the desktop shortcuts: both in world, connected, see each other, no box, no crash.

## Exact position/movement sync (2026-11-20)
- Measurement: [debug] net_trace=1 -> rdrmp_trace[_N].csv (QPC clock shared by game copies on one PC): L rows = own player
  (+ raw desire words), R rows = each remote's network target, puppet pose, error. tools/run_sync.ps1 (2 copies, scripted
  route on copy 1 via tools/key.ps1 -ProcessId/-Scans with Alt-tap focus; copy 2 topmost for screenshot bursts) +
  tools/sync_report.ps1 (error vs truth at the same instant / minus best-fit latency, heading, teleports).
- Desire word 0x3f low byte = gait: 0 stand, 1 walk, 4 sprint (old code read only bit 0 -> sprint looked like standing ->
  snapped every frame). "Moving" now = interpolated velocity > 0.3 m/s (gait bits stay set while walking into a wall).
- Teleport RE: FUN_140ad4ca0 -> FUN_140ad4680(entity, mtx, 1, p4, p5, p6). Both p4 branches reset the motion component
  *(actor+0x38) (p4=1: FUN_140ce4190 resets 23 sub-objects; p4=0: FUN_140cf01f0 + FUN_140cfb6e0 reinit) = why teleports
  stop the walk anim. The move itself = message 7 to FUN_140ae0100(entity, msg): +0x10 old rows, +0x50 new matrix*,
  +0x58 flags (0x6b; bit0 = reset motion fields + FUN_140cf8310, bit2 = skip cloth re-init, bit6 = physics move
  FUN_14089dc60, bit7 = camera snap), +0x59 bit0 = update AI position. sandbox.cpp NudgePuppet sends it directly with
  flags 0x6e -> position + facing move while the locomotion keeps animating (verified by screenshot bursts: full stride).
- Controller (netclient.cpp): remote desire drives the animation (gait kept; steered by pure pursuit 0.35 s ahead on the
  remote's path; stops instead of turning back); every frame NudgePuppet pulls the puppet (tau 0.1 s) to the target
  predicted 0.1 s + lead_ms ahead along its velocity, facing eased to the remote heading; teleport only > 1.5 m moving /
  > 1 m standing; standing: walk in / tiny snap after 0.4 s / turn in place. QPC sample timestamps, interp_ms 50.
- Result (same instant, copy 1 vs copy 2): moving mean 0.60 m -> 0.06 m (p95 1.12 -> 0.11), lag 200 -> 25 ms, heading
  13 -> 4.4 deg mean, standing p95 4-5 cm, teleports 41 -> 1 (spawn). Network target alone: 125 ms behind, 1-2 cm accurate.

## Network delay cut: 125 ms -> 40 ms (2026-11-20)
Measured with send timestamps (one PC = one QPC clock, so arrival - send = the real delay; trace R cols 15-17 =
transit, clock offset, lead; sync_report prints TRANSIT/OFFSET/LEAD). Where the old ~125 ms went and the fixes:
- interp buffer 50 ms timed by ARRIVAL (jittery) -> PlayerState.timeMs (protocol v3, fw API v2, Player entity field) =
  sender's game-frame capture time; receiver maps sender clock -> local with min(arrival - send) over 3 s (eased), so
  the timeline is jitter-free and the buffer is now interp_ms=30. Past the newest sample: extrapolate <= 100 ms.
- rdmp.dll net thread slept 16 ms (and 15.6 ms timer resolution) -> woken by an event from every game frame + 4 ms
  wait with timeBeginPeriod(1).
- rdrmp_fw.dll wrote the local state AFTER Instance::Update -> every state waited one more call. Now written before.
- ReplicaManager3 default priority High waits for MafiaNet's update thread (WaitOnEvent(10)) -> SetDefaultPacketPriority
  (Immediate) on client (OnConnectionFinalized) and server (PostInit); ReplicationManager is a public RM3 subclass,
  so this needs no framework change.
- Server sleep_for(1 ms) between ticks oversleeps at the default timer resolution -> timeBeginPeriod(1) in server main.
- Server forwards an update only on the tick AFTER it arrives (RM3 serializes before applying that tick's packets):
  60 Hz -> mean transit 33 ms; server default now 120 Hz (main.cpp; --tickrate N / rdmp.ini [net] server_tick_hz,
  the launcher passes it) -> mean 19 ms, fastest 11 ms. Clients serialize at the server tick (handshake tickRate).
- lead_ms=auto (default): interp_ms + (max(myPing-10,0) + max(theirPing-10,0))/2 + tick + 2. MafiaNet's ping reads
  ~10 ms on localhost (thread wakeups), hence -10. Each player's ping rides in the Player entity (pingMs).
- Stuck snap: gap > 0.3 m not shrinking for 150 ms while the remote moves -> teleport. Puppets (NPCs) hit an NPC-only
  blocker at x ~ -2202.4, z 2610-2611 near the Armadillo spawn that the player walks through.
Result (run_sync defaults): network target 125 -> 40 ms old; puppet 0 ms behind (was 25), moving mean 0.057 -> 0.045 m,
median 0.049 -> 0.018 m (walking 1-2 cm, sprint/curves 7-10 cm), p95 0.11 -> 0.19 m (blocker moment + first steps).

## Native registry + blips (2026-11-21)

- Natives register at static init: `lea r8,[handler]; mov edx,<joaat(lowercase name)>; call 0x1401E7140`, 3439 of
  them. The registrar inserts into an open-addressed table: entries `*(0x142B0D210)` (16 bytes: u32 hash, u64
  handler), capacity `*(u32*)0x142B0D218`, count `*(u32*)0x142B0D220`, slot `hash % cap`, step `(hash>>1)+1`.
  Table dump: `natives_0x673783F3.csv` (`tools/extract_natives.py`). Names: RDR-PC-Natives-DB (K3rhos, MIT).
- Native context: `+0 result*, +8 argc, +0x10 args* (8-byte slots), +0x18 vector count, +0x20 vector refs,
  +0x40 16-byte vector copies`. The old probes used 4-byte slots (results about specific natives invalid).
- Blips: `ADD_BLIP_FOR_COORD 0xC6F43D0E -> 0x140653B90` (Vector3*, icon, min distance, bool, int); pool at
  `loader+0x2270`, 199 slots; needs the HUD entry list up. Full table and engine notes: docs/SCRIPTING_RE.md
  "Native registry" / "Blips". Implementation: core/scriptnatives.cpp, core/blips.cpp.


## Player input path + MTA-style context switch (2026-11-21)
MTA:SA (studied from mtasa-blue, GPLv3 - only the method is used): remote players are real `CPlayerPed`s; a hook on
`CPlayerPed::ProcessControl` copies the remote's synced `CControllerState` into the single global pad (plus camera
rotation, gravity, stats), runs the game's own player code, then restores the local pad (`SwitchContext` /
`ReturnContextToLocalPlayer`, multiplayer_sa/multiplayer_keysync.cpp). Puresync packet (CNetAPI::ReadPlayerPuresync):
keys, position, rotation, velocity, camera rotation, flags. Receiver: SetMoveSpeed(velocity), SetControllerState,
SetTargetPosition = the error (target - current) is applied linearly over the next sync interval
(CClientPed::UpdateTargetPosition), warp only beyond 5 + 5 * speed.

RDR (build 0x673783F3), found with write watches (jump_probe=3/5) + Ghidra:
- `playerInfo` = `*(0x1422DAF30)`, one global for the local player: +0x5ec actor handle, +0x5e9 / +0x1c8 "is set up",
  +0x5d4 control state (2, 3, 5), +0x118 control object (vtable), +0x138 movement object (vtable, +0x30 update),
  +0x410 / +0x418 / +0x420 sub-objects.
- Input -> control object, main thread, player-info update: game loop 0x1401F69CD -> 0x14034C986 -> 0x14034FB2C ->
  FUN_1403461c0 -> FUN_140370cd0 -> FUN_1403726f0 (writes the control object). The control object reads every
  input through **vfunc +0x58(this, inputId)** -> input record (byte [0] value, [2] new, [3] previous, [4] index,
  +8 table; the game writes [2] itself to force sprint). Ids: 0x11 / 0x12 left stick x / y (analog,
  FUN_1403704b0), 0x18 sprint, 0x0c another button. Control object fields: +0xd0 / +0xd4 stick, +0x134 stick
  magnitude, +0x100 stick + camera heading, +0x104 desired heading (camera-relative: atan2 of playerInfo+0x10's
  camera forward (+0x30, +0x38) + stick angle), +0x10c gait 0-4, +0x184 sprint state machine.
- Control object -> desire, main thread, per-actor update: 0x140AF8BB5 -> FUN_140bb4150 (player-control component;
  **returns unless playerInfo+0x5ec == its actor**) -> clears the desire (FUN_140b25880(state+0x240)), then
  state 2: FUN_14034e9b0 / 3: FUN_14034e470 / 5: playerInfo+0x138 vfunc +0x30, FUN_14034dbe0, FUN_14034df00 ->
  0x14034EA09 -> 0x140374028 -> 0x140373E23 writes the stick into the desire.
- Only the local player has that component (exec breakpoint on FUN_140bb4150 with a puppet present: one rcx).
  FUN_140ae3ff0(out, def, ..., asPlayer, noPlayerInfo): asPlayer && !noPlayerInfo writes playerInfo (+0x5e9, +0x4f8,
  +0x5ec, +0x1c8, +0x1c0) and creates the actor's +0xb8 and +0xd8 components from the player template
  (*(*(*(0x142AE0468)+0x18)+0x10) +0xb8 / +0xe8 descriptors, FUN_140597660 factory, vfunc +8 init, +0x18 activate).
- Jump: IS_ACTOR_JUMPING (0x140281BF0) = motion(*(actor+0x38))+0xc6f != 0, or (actor+0x88)+0x85d < 0 (~1.1 s per
  jump). IS_ACTOR_USING_LEDGE = motion+0xaa8 -> +0x48 -> +0xf8 != 0. A jump is not in the desire.
- playerInfo is created once in FUN_1401ee680: alloc 0x5f0 (FUN_14007d6c0), ctor FUN_140344f30, then
  FUN_140345a20(info, *(0x142AC4CA0)) and FUN_1403458f0(info). FUN_140345a20 allocates +0x410 / +0x418 / +0x420 /
  +0x408 and loads one **input mapper per context** from XML (vfunc +0x40): +0x118 input_foot.xml (the control object
  above), +0x130 input_cam, +0x120 input_death, +0x70 input_boat, +0x78 input_car, +0x80 input_wagon, +0x128
  input_inv, +0x138 input_horse, +0x60 input_pushcart, +0x68 input_train, +0x140 input_minigame. A mapper's vfunc
  +0x58(id) returns that input's record = RDR's CControllerState equivalent (horses use input_horse).
Plan (RDR's SwitchContext): per puppet a player-info + control object of its own, the control object's input fetch
(+0x58) answering from the remote's synced input values; around the puppet's player-info update and its
FUN_140bb4150, the global playerInfo pointer points at the puppet's, so the game's own player code drives the puppet
from the remote's buttons, stick and camera heading. Open: playerInfo construction / size, the +0xb8 / +0xd8
components on a puppet, which other threads read playerInfo during the swap.

## Remote players on the player code: WORKING (2026-11-21)
- Input records: the foot control's vfunc +0x58 = FUN_1403d9dd0: record = *(*(ctrl+0x20)) + id*16, 30 ids (0x00..0x1d):
  [0] invert mask, [1] per-input timer, [2] value, [3] previous; analog = ([2] ^ [0]) - 127.5. PC keys: space 0x18,
  shift 0x06, ctrl 0x10/0x1a, E 0x0f/0x1b, F 0x05/0x07, W/S stick 0x12, A/D stick 0x11 (0x7f centre).
- Control object: input factory FUN_1403d9520(type, owner), type 6 = on foot, 0x1b90 bytes (ctor FUN_1403705d0).
  Per frame (FUN_14034c830 -> FUN_14034f700): resets (+0x160, +0x18c, +0x104, +0x144, +0x100, +0x124, +0x120,
  +0x1a0), input step FUN_1403461c0 -> FUN_140370cd0 -> FUN_1403726f0, then **vfunc +0x28 (FUN_140372e50)** which
  computes the move vector +0x120/+0x124/+0x128 (gated by FUN_1403dc1a0(info+0x410)). The apply (vfunc +0x30 ->
  FUN_140373f70 -> FUN_140373d40) writes the desire from it: words 0x14..0x16 = +0x120.., 0x38 = +0x104, 0x34 =
  +0x108, gait byte = +0x10c.
- Behaviour: every actor's behaviour holder (actor+0x40, vt 0x141D20BD0, set up by FUN_140b09bd0) runs its active
  behaviour holder+0x388 each frame (FUN_140b0c920 -> FUN_140af8bd0 -> FUN_140af8b50). Its behaviour set holder+0x370
  (0x2050 bytes, FUN_140b6b9b0) embeds **MarstonBehave** (vt 0x141D26BB8, update = FUN_140bb4150) at +0x1950. The
  player's active behaviour is that MarstonBehave; NPC puppets had none. The player actor has no AI brain
  (actor+0x28 = 0).
- puppet_player.cpp: per puppet a copy of playerInfo (+0x118 own control copy, +0x5ec puppet handle, +0x5d4 2, own
  +0x408/+0x410/+0x418/+0x420), the control copy's +0x20 -> own records, the +0xb8/+0xd8 player components
  (FUN_140597660), holder+0x388 = its MarstonBehave; hooks FUN_140bb4150 and FUN_1403726f0 (15-byte PIC prologues).
  Each frame: records from the network, resets + FUN_1403726f0 + remote heading into +0x104 + vfunc +0x28, all with
  *(0x1422DAF30) pointing at the puppet's player-info; FUN_140bb4150 for the puppet runs in the same context.
- Verified: the puppet walks / sprints / stops / climbs onto the station porch roof with the remote; two copies with
  replayed key input: moving median 0.31 m, p95 0.89 m, 4.1% pulled, vertical mean 0.03 m.

## Game settings (2026-11-22)
- Files (the game's own, never written by us): %APPDATA%\.1911\Red Dead Redemption\title\options.xml
  (rage::LoadedOptions), graphicsOptions.xml (rage::GraphicsOptions), gabindings.xml (key bindings).
- Parser descriptors: LoadedOptions FUN_14061e630 (0x70 bytes: gameplay @0, display @4, controller @0x24, kbMouse
  @0x44, audio @0x54, MP fields @0x60..0x6d), sub-structures FUN_14061e8b0 / ea30 / ec00 / ede0 / efa0, GraphicsOptions
  FUN_140158580 (0x5c bytes). Each member descriptor holds its offset; the case-sensitive joaat of its name sits 8
  bytes before it, so every field is named for certain (store order in the registration is NOT declaration order).
  Full table: src/shared/game_options.h.
- Options menu object (rdrOptionsLayer, UI "OptionsLayer") keeps a LoadedOptions at +0x98 (+0x110 / +0x114 = -1
  "no override"). SyncOptionsFromGame FUN_140625f80 (live -> +0x98), SyncOptionsToGame FUN_140627370 (+0x98 -> live,
  sets +0x10b when something changed), menu close FUN_140624f70 = UI read FUN_140626c10, ToGame, save
  FUN_14061f410(layer+0x98) (options.xml via the parser, FUN_1400c1810). Startup gather + load: FUN_14061f170.
- Volumes are not in ToGame: audio category table *(0x142AE3380), stride 0x14, float at +0 = volume, index
  *(u16*)(category+0x28); SFX category *(0x142AE2E10), music *(0x142AE2E18) (FromGame reads them there).
- Graphics: live instance at 0x1422CA340. Graphics menu callbacks (FUN_1405d3ba0 registers them):
  ApplyCurrentGraphicsOptions FUN_1405d7260 = every handler in the std::map at *(0x142B67370) (node+0x28 = handler,
  vfunc +0x10(handler, pending GraphicsOptions = layer+0x2d8)), then save FUN_140158be0 (graphicsOptions.xml);
  ConfirmApplyChanges FUN_1405d7480 copies live -> pending.
- UI option ids (Scaleform): DisplayOption_*, ControlsOption_*, KBM_*, AudioOption_* (list in this file's history).
  The meaning of some enum values (FrameRateLimit, aaAntiAliasing, MotionBlurStyle, upscaler qualities) is not
  confirmed yet: the menu labels them, keeps unknown values as "Custom (n)", and offers a 15 s revert for graphics.

## Native menus (2026-11-22, RETIRED 2026-11-23 for the ImGui menu; still runs with [game] title_screen=1)
- Title mode boots the game's own front end inside our loop: Game::Init keeps the legals/logo controller, the original
  loading frame FUN_1405f4390 runs, the "loading"/"startscreen" movies are not stubbed, OurRun starts PressStart
  (t = FUN_1401e9c30(); FUN_140205900(t, "$/content/PressStart", 0x600, 0, 0); FUN_1402060b0(t, 0, 0, 0, 0);
  game+0x144 = t+0x374) and BootStep does not open the boot gate nor start the world load until
  GameflowRequestWorldLoad. With skip_videos=1 the title is up ~10 s after our loop starts (8 s legal pages).
  Enter = Play -> the mode list (Normal / Hardcore / Undead Nightmare); the Settings key is layout-dependent (on a
  QWERTZ keyboard the key labelled Y = scan code 0x2C) and opens the game's options (OptionsLayer) natively.
- Widget registry (FUN_1400ef100): root = *(uiMgr+0xa0); +8 buckets, +0x10 u16 count; node {+0 name, +8 u16 len,
  +0x10 widget, +0x18 next}. Widget +0x28 u16 flags (0x800 hidden; FUN_1400fe810(w, hide) toggles it and relayouts
  the parent: vt+0x218 parent, parent vt+0x2b8). Text widgets vt 0x141C61950 (key hash +0x88, vt+0x420 SetKeyByName).
- Class factories (name -> C++ ctor, FUN_1400f6e90 into *(0x142AAC8C0)+0x40): FUN_14063b5f0 (rdrOptionsLayer,
  rdrLobbyLayer, rdrPauseMenuScene, ...), FUN_140483cd0 (rdr2NetUI, rdrNetworkingMenu, NetMenu, NetPlaylistItem,
  UIFriendList, NetInvitesList, ...).
- Screen stack: FUN_1400f2d50(*(uiMgr+0x90), widget, 1) pushes (array +0x5a0, count +0x6a0) and calls widget
  vt+0x188 (activate); used e.g. by FUN_1404d97e0 for OL_NetworkingMenu.
- The ORIGINAL RDR1 MULTIPLAYER MENU is loaded (2660 widgets on the title; dump: build\bin\ui_widgets.txt):
  NetworkingLayerOffline (in PauseMenuScene, next to OptionsLayer): NetTabs {NetTab_Online, _Private, _Party, _LAN,
  _Invite, _Exit}, NetOfflineTabs, Net_ContentLayer.s1 {NetContent_Public, _Private, _LAN, _Party (list class
  vt 0x141C65420), OnlineInbox}, MPProfile (rank panel), NetLayer_Prompts. OL_NetworkingMenu (lobby, under
  UserInterface): LobbyTabs {NetTab_Games, NetTab_Players}, NetLobby_Prompts. Pause menu MP tabs exist hidden
  (PM_Network, PM_Leaderboards, PM_SocialClub, GameTab_ExitMp2Main; FUN_14061f8f0 hides them in single player).
- Pushing NetworkingLayerOffline with FUN_1400f2d50 on the title opened the pause scene shell (red tint, clock,
  "OPTIONS" header, prompts) but no networking content. Next: how PauseMenuScene selects its layer (the title's
  Settings path: "AUDIO_StartMenuItemSelected" / "PauseBrush3Begin"), then fill NetContent_* rows.
- (07:40, superseded) Filling the title Options' CHEATS rows from C++ worked, but taking over existing tabs was
  dropped for real custom menus (below). Row layout for reference: row = extras+0x98+i*0x3f0, +0x178 label,
  +0x218 value widget. Literal text on a text widget: +0x80 = 0, +0x88 = char*, vt+0x3d0.
- THE MENUS ARE DATA: SCXML files in content.rpf (extract: re/tools/rpf6.py, needs pycryptodome + zstandard from
  D:\Development\Tools\pylibs). Root "UserInterface.sc" <include>s every screen (Boot.sc = legals/title/mode list,
  pausemenu/PauseMenuScene.sc, Options.sc.xml, Net/OfflineMenu.sc, ...). Elements: UILayer, UIPanel, UIScrollableList,
  UIList, UILabel, UIOption, UIProgressBar, UIPromptStrip, UIButton (icon="{@UI.ACCEPT}"), class layers (rdrOptionsLayer
  ...); <transition event=...> with actions Enter/Exit/Focus/StackPush/SendEvent/SendDelayedEvent/PlaySound/
  SetFlashInt/StreamMovie and C++ calls ("HDRLayer.OnHDRStyleChanged()"); <ifdef expr="ULTIMATE"> build flags.
- Loader: FUN_1400ec440(name, ...) opens every .sc (cache table *0x142ABC0C0 {hash, data, size}, served as
  "memory:$%p,%d,%d:%s"; a name containing ':' is an absolute path). src/game/uifiles.cpp detours it (custom
  trampoline: the prologue's 2nd instruction is mov r15,[rip]) and serves: UserInterface.sc + <include
  src="rdmp/Main.sc">, Boot.sc with SinglePlayerList replaced by ui/native/MainMenuList.sc, rdmp/* from
  build/bin/ui/native. Reading a game stream: device vt+0x70 size, FUN_1400c7c20 read, FUN_1400c7e90 close.
- WORKS (08:10): title -> Play -> OUR main menu in the game's own style (Server Browser, Direct Connect, Multiplayer
  Settings, Settings, Quit). Settings opens the game's Options; our items raise UI events (rdmp.browser, rdmp.direct,
  rdmp.mpsettings) that reach our event hook. A plain UIPanel with movie="startscreen" draws no list: title-style
  lists must live inside RDRUIStartScreen2 (its C++ class drives the movie). Texts not in the game's string table
  are set at runtime by widget id (uifiles.cpp kTexts). Arrow keys = extended scan codes (E0 48/50).
- (08:30) The boot goes straight to our main menu: Boot.sc benchmarkComplete -> StartScreen2 (was StartScreen1, the
  "press Play" title), StartScreen2's Back = Enter(QuitGame) labelled Quit. Flash variables from C++ (uifiles.cpp
  FlashSet*; [debug] ui_exec=1 runs build\bin\ui_exec.txt lines live): RDRLOGO_target_alpha 0 hides the logo.
- Title logo = brush-letter DXT1 masks (white on black, tinted by the movie) embedded in the startscreen movie
  resource (flash.rpf payload 0x4D8DB2A4, 10.9 MB; texture records {+0 name ptr 0x50.., +8 u16 w, +0xa u16 h, +0xc
  fourcc, +0x34 data ptr 0x60..}, physical data = file end - 0xA00000): _brs_startscreen_z_0111 976x552 "RED",
  _0110 1248x548 "DEAD", _0117 1476x556 "REDEM", _0116 1368x552 (presumably "PTION"). startscreen_z_0104/_0107 =
  the Undead Nightmare logo. "RED DEAD MULTIPLAYER" = rebuild _0117/_0116 as MULTI / PLAYER from the game's own
  letters at runtime (U, L, Y composed) and patch them into the resource as it loads (load hook not found yet).
- Automation note: never send Esc while Claude controls the PC (any Esc stops the session). Keys: scan codes via
  SendInput only when RDR.exe is in front (DirectInput ignores virtual-key-only input).
- (2026-11-22, one menu, views) Pause scene menu column (pause_main movie "menu"): the scene builds it when it opens
  from the main list's entries shown at that moment (an entry hidden then gets no row; un-hiding later does not add
  one, hiding does remove one). menu.elements[i] = row i; a category list with flashObject="menu.elements[i]" draws as
  the submenu of row i (the game's Options tabs were menu.elements[5], its OPTIONS row). Our views (Options, server
  browser) hide every other entry so theirs is row 0 and use menu.elements[0]; the column then shows "OPTIONS" with its
  tabs under it. Disconnect closes the scene and re-opens it (rdmp.menu, posted ~0.4 s later: a push during the closing
  transition is lost) so the main menu's entries get rows.
- UIOption value: vt+0x410(w, 0) = index, vt+0x468(w, index, 0) sets it (FUN_140626c10 reads, FUN_1406262b0 writes);
  setting it raises the option's onchanged. Options_Header literal text works like other labels (+0x80 = 0, +0x88).
- Multiplayer targeting mode (ControlsOption_MPCombat_Mode): used only when *(0x142C456D0)+0x12E bit 8 (the game's own
  online session) is set (FUN_1406262b0 / FUN_140624740); ours never sets it, the row is hidden by the game anyway.
- Text entry = the cheat entry's mechanism (ExtrasLayer.OnKeyboardActivate FUN_140623270, frame FUN_140622a00, end
  FUN_1406234a0): keyboard K = *(0x142AC4748)+0x68 (+0x160 initial, +0x1c0 result, +0x220/+0x224 title/description
  hashes, +0x228 state 0 open / 1 accepted / 2 cancelled, +0x22c open, +0x230 platform keyboard vt+0 show(K+0x270) /
  vt+8 close, +0x280/+0x2b8/+0x2f0 strings, +0x328 max length); typed text = *(*(0x142893518)+0x68): +0 std::wstring,
  +0x20 allowed characters (the game's list includes . and :), +0x60 max length; FUN_1401fc820(wstr, wchar*, len)
  assigns. The box text is the game's dynamic string Enter_Cheat_Dialog: FUN_1400fb250(*(0x142C45608), key, fmt, ...)
  formats an existing dynamic entry (hash map at mgr+0x720), it cannot add keys. Typing and Backspace work on PC.
- "Return to main menu" in the game (AwaitRebootTransition, also the graphics RestartGame) restarts the process: game
  state 6 ends Game::Run and the game starts "PlayRDR.exe -restart" (not through RDR.exe's CreateProcessA import; the
  Rockstar layer), which starts a fresh RDR.exe without the mod. Our Disconnect therefore stays in-process (world kept,
  player in limbo). Leaving without an engine fade: EngineFadeToBlack brings the UI's FadeToBlack scene, which ends the
  pause (pauseEnd) and holds the UI stack (no HUD / pause push works) until a fade-in.
- Framework client shutdown: left to the static destructor at DLL unload (process exit) it crashed in the Framework's
  scripting module (scripting_catalog.h:359 via instance.cpp:217); NetShutdown now runs it when our game loop ends.
- (open) No mouse cursor in the in-game pause menu. The window's message pump (vt+0 of the window object, vtable
  0x14105A448; cursor part FUN_140ec4bb0) shows the OS cursor when the input manager *(0x142AAC8D8) has +0x290 set and
  the 1911.dll interface at +0x60 says so (vt+0x10, "cursor mode"; 1911.dll is the port's obfuscated layer, unreadable);
  else it hides it (ShowCursor loop, flag window+0xa8). Measured with GetCursorInfo: title = shown; in game = hidden,
  also in the pause menu with [sandbox] no_pause_freeze=1; with no_pause_freeze=0 the pause menu shows it. So
  something in Game::Update that runs when it sees state 3 (our pause hold, gameflow OurRun) keeps the mouse captured.
  Next: bisect what in Game::Update / the masked game-clock call (FUN_1405c5220) re-captures it (e.g. take the clock
  call out of the hold first), then give that part the real state 4.
- (open, 2026-11-22) The same pause hold also leaves the player's input live behind the in-game pause menu (mouse-look
  turns the camera, movement keys move the player). Tried: (a) DisablePlayerControl (actor comp+0x48e |= 0x7d) while
  the menu is open: logged, but the player still sprints and the camera turns (something restores it in the pause);
  (b) the game's own MP rule instead of state 3 - keep state 4 and set net+0x1634 (*(0x142C456B8)) while paused, so
  only the ~90 inlined "paused" tests (state 4 && (!net+0x1634 || DAT_142b0ebc8)) let the world run: crashes in
  Game::Update -> FUN_140472a02 -> null read at 0x1404bd02f (online-session code with no session objects), in game
  and on the title. Both reverted. Next: patch the "paused" tests that freeze the world (not input/camera) one by one,
  as done for the three render tests (gameflow.cpp InstallGameFlow step 5), then drop the state-3 hold; that should also
  give the cursor back.

## Disconnect: unloading the world (2026-11-22, [game] disconnect_unload=1, open)
- The game never unloads a level in-process: its "return to main menu" (AwaitRebootTransition, also the graphics
  RestartGame) restarts the process ("PlayRDR.exe -restart", started by the Rockstar layer, not RDR.exe's
  CreateProcessA import; the new copy has no mod); quitting returns from Game::Run and calls ExitProcess (0x14007FD48)
  with the level loaded. "net.DevSoftReset" (FUN_1401f38d0, game+0x12d bit 0x20 + 0x12e bit 8) is commented out in the
  UI files ("ResetNotSupported").
- But its level load restarts the session when one exists: game+0x12e |= 1 in game -> FUN_1401f38d0 -> FUN_1401f85b0
  (drains streaming 0x142AC50F8, waits on *(0x142C45780), renderer to loading mode: rm+0x34 = 1 with the frame mutex
  rm+0x48 / event rm+0x10 handshake at 0x1401F887F) -> FUN_1401f8a00: with a session (0x142AC2925 set) the block from
  0x1401F8BE3 releases it (session objects 0x142AD1CC8 / 0x142AD1F50 / 0x142AC5118, actors - the player too, scripts)
  before the common init at 0x1401F8FEE. Tested: in game, a new-game request reloads in-process (loadStart ->
  loadComplete), no crash, the player gone (our sandbox creates it only at the boot spawn).
- gameflow GameflowUnloadWorld uses it: request the restart (fader held black until the trigger ran), finish its load
  at once (an empty session), no scripts run meanwhile (mainthread.cpp skips the VM runner while parked: running them
  on the restarted, unfinished session crashed in the VM at 0x1401E4EB5 via FUN_1402066d0), the sandbox spawns nothing
  and the net client forgets puppets / horses / blips; a join requests another restart and runs the boot's phases.
- Open: (1) the empty session's terrain stays drawn behind the menu. Switching the renderer to loading mode ourselves
  (the handshake above) was undone within ~4 ms and the game died, also when done once ~1 s after the load's hand-back
  to the normal render; the loader fader held black (loader+0x25800 = 1) does not cover the normal render. The fader
  layer FadeToBlack (RDRUIFader, movie "fader": fadeVal / fadeRefresh / fadeColor; Enter-able, not a stack item; its
  onactivate also calls UIPlayer.Disable()) goes black with fadeVal 100 + fadeRefresh, but covers our menu column too
  (only the prompt strip stays above); moved below pause_main in the UI movie list (*0x142C45610, ui_exec "o") it is
  not drawn at all - the list is not plain draw order. (2) SOLVED: after a restart the player owner goh[808] (the
  static object 0x1422D9F50, vt 0x141C8B5E0) has generation 1 (handle 0x10328); the sandbox now reads the slot's
  generation at spawn (CurrentOwnerHandle). Join after an unload: the player spawns, the server's spawnPlayer works.
  (3) Re-entering the boot's Startup / StartScreen2 for the title background drew a broken menu column and the logo.
- Debug: ui_exec "w <addr> <off> <hex>" writes a byte at *(addr)+off; lines already in ui_exec.txt at start are no
  longer run (a stale "w" request re-ran a level load at every boot).
- Covering the empty session (2026-11-22, open). UI movie list *(0x142C45610): wrappers at +0x30 in draw order (0
  mapRenderProxy, 1 pause_main, ... 9 list, ... 28 startscreen, 30 fader, 31 loading, 34 popup, 35 effects), name -> slot
  through a hash (+0x170 buckets, +0x178 count, node {+0 name, +8 u8 slot, +0x10 next}) read by FUN_14011a000;
  advance FUN_140119f10 walks all slots (vt+0x18). Tried: (a) the title screen's background (startscreen BG): a
  letterbox frame (black bars, transparent middle) drawn above our menu; (b) FadeToBlack fader black (fadeVal 100,
  fadeRefresh) + pause_main and list moved above fader (ui_exec "u", hash renumbered): our menu over solid black,
  BUT a moved movie stops updating on screen (selection no longer moves; also after moving pause_main alone) - some
  per-slot state outside the array and the hash (no slot field in the wrappers: ui_exec "d"); (c) pause_main's
  pausetransition types are partial brush strokes. Next: find what keeps the per-slot draw/update state (the render
  side of the UI list) or draw our menu in a movie that is already above the fader (popup/loading are).
- Owner's idea (2026-11-22): run the game like MTA - load the world once at boot, hidden under our menu, so joining
  only connects and Disconnect only resets the session; needs the same cover.

## Keeping Esc and input from the game under our menu (2026-11-23, src/client/overlay.cpp)
* The game reads keys through DirectInput 8; its pause menu opens on the keyboard's DIK_ESCAPE. To keep it shut while
  the ImGui menu owns Esc: zero byte DIK_ESCAPE (1) of the 256-byte GetDeviceState buffer and drop GetDeviceData events
  with dwOfs == 1 (no mouse / joystick offset is 1), swallow WM_KEYDOWN / WM_KEYUP VK_ESCAPE in the subclassed window
  procedure and hand raw-input Esc to DefWindowProc. Only the window-message path is verified so far (a real Esc press
  is the owner's check).
* While the menu is up the whole keyboard and mouse are blocked the same way (state zeroed, buffered events dropped)
  and the client's set_input_lock holds the player; verified in game: W held behind the menu does not move him.

## Game fonts and title art (2026-11-23, src/client/rdr_assets.cpp, read at runtime, never shipped)
* Archives: RPF6, TOC AES-256-ECB applied 16 times (key in re/tools/rpf6.py), 20-byte big-endian entries; payloads
  are RSC5 (12-byte header) + zstd or plain zstd. RDR.exe's zlib 1.2.3 (inflateInit_ 0x140F021D0, inflate 0x140F01B80,
  inflateEnd 0x140F01FC0; Win64 z_stream, 0x58 bytes) decodes zstd frames while the global 0x14225B228 is set (1 from
  the start): fw::Host::inflate (fw_api v13) calls it the way the resource loader does (Z_SYNC_FLUSH).
* fonts.rpf = rage bitmap fonts: a glyph table (float 1.02, +4 size, +0x2C glyph count; 10-byte glyphs {u16 code,
  u16 0, u8 x, u8 y, u8 w, u8 h, s8 top from the baseline, s8 advance adjust}; then u32 page count and the index of
  each page's end) plus one 128 x 128 DXT5 DDS per page (coverage in alpha). Text fonts (glyph table: pages):
  - 0x0411E993 slab serif, size 31 (the game's menu face): 4EE9E283 08AF780F C3220EEC 501362D8 EA6D86F7 EF5113A7
    8CB5145E 2EEC2FDC
  - 0xBA922545 condensed, size 37: C47FB29E 1EBCC74D F7B13ADD 7093E1D6 84EDA8FA
  - 0xEAE379F1 small serif, size 20: D94CD6B0 DD3CC5A2 12513FF0 31CAD755
  The 512 x 512 pages and the 0xA35A5753 / 0x8DEC952B family are controller-button and icon fonts; 05BBE1DB /
  7F66C050 / DEDC033D look like shadow variants. Line metrics: take ascent / descent from letters and digits (a few
  symbols reach much further). Served to ImGui through a custom ImFontLoader (glyphs area-resampled to each size).
* The menus' Scaleform fonts (Redemption, RDR2Lucid, RDR2Narrow: *.charset_N.dds) sit inside the flash.rpf movies'
  serialized GFx graphs; their glyph metrics were not located (not needed: fonts.rpf has the faces).
* Title art: flash.rpf 0x4D8DB2A4 (startscreen): brush masks _brs_startscreen_z_0111 RED 976x552, _0110 DEAD
  1248x548, _0117 REDEM, _0116 PTION, brush strokes _0020 / _0023 / _0026, splatter _0069 (DXT1, white on black);
  _0127 / startscreen_z_0104 / _0107 Undead Nightmare. Texture records as above (name ptr 0x5.., data ptr 0x6..,
  physical = the last 0xA00000 bytes).

