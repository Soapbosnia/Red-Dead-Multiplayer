# Player sync review (2026-11-24)

An honest look at how remote players are synced today, part by part: what we send, how the other game plays it, how
MTA:SA does the same job, what has really been tested, and what should change. The short version is at the end.

## How it works today

**One stream per player.** The owner's client writes its state every frame into the replicated `Player` entity
(`src/shared/player.h`); the transform part goes out unreliably at the network rate, the rest as reliable deltas.

| Sent (transform, unreliable) | Sent (fields, reliable) |
|---|---|
| position, heading, speed | weapon id, ammo |
| move stick, move heading, gait | health / alive, vehicle |
| all 30 on-foot inputs (8 bit each) | last hit (dmgSeq, attacker, weapon, body part, loss) |
| crosshair world point (aim point) | |
| weapon state: aim state, drawn / reloading, ready time, shot counter, clip | |

**The remote side** keeps a timeline of samples placed by the sender's clock and plays it 30 ms behind the newest
(`remote_timeline.h`), with a velocity lead that cancels the network delay. Each remote player is a **puppet**: a
real game actor driven by the game's own player code. Around the puppet's player update, the global `playerInfo`
pointer is switched to a private copy for that puppet, whose input records answer with the remote's buttons (MTA's
context switch, `puppet_player.cpp`). A position correction pulls the puppet softly toward the network position and
teleports it when more than 4 m off (`puppet_control.cpp`).

## Part by part

### Walk, run, sprint, stop, climb
* **Ours:** the remote's stick, buttons and camera heading go into the puppet's own player code: the animations are
  the game's, not ours. Correction pull / teleport on top.
* **MTA:** the same idea (controller state in the ped's ProcessControl context), plus velocity in every puresync
  and a position interpolation in the ped itself.
* **Tested:** two copies with replayed key input: moving error median 0.31 m, p95 0.89 m, 4 % of the time pulled.
  On one PC only (about 11 ms delay), never with real latency or packet loss.
* **Verdict:** the right approach. Gaps: no velocity sent (only speed), the correction can fight the animation, and
  nothing tested over a real connection.

### Jump
* **Ours:** the jump button is one of the 30 inputs; the puppet jumps when its player code sees the press. Taps are
  now held at least 120 ms so they survive the network rate.
* **MTA:** the same (keys), and jumping is a task GTA starts from them.
* **Tested:** not really. The notes still say "jump not verified, needs a real second player".
* **Verdict:** right approach, unverified.

### Looking around (head)
* **Ours:** not synced at all. A remote player's head never turns to where they look.
* **MTA:** syncs the camera rotation and the head "look at" direction.
* **Verdict:** missing.

### Weapon in hand, switching
* **Ours:** the weapon id goes as a reliable field; the puppet is given that weapon and it is put in its hand
  (`SyncPuppetWeapon`). Draw / holster then follow the remote's weapon state.
* **MTA:** the same: weapon slot and ammo in sync, the ped is given the weapon and switched to it.
* **Tested:** with logs and screenshots: long guns and sidearms appear, draw and holster.
* **Verdict:** fine. The weapon wheel's own switch animation is not reproduced (MTA doesn't either).

### Weapon animations (ready, aim, fire, reload, holster)
* **Ours:** this is where we drift from MTA. The puppet runs part of the local player's frame (actions, weapon state
  machine, reticle, aim update), but some of the state those steps need is normally written by code that only runs
  for the local player (the camera's aim code). So we **write the remote's aim state and weapon-ready time into the
  puppet every frame**, press fire once per shot the remote reports, and press reload when they start reloading.
* **MTA:** nothing forced: GTA's ped tasks run the same way for every player ped, so the keys alone are enough.
* **Tested:** draw / ready / aim / fire / reload / holster seen in logs and a few screenshots; 30-200 ms behind on
  one PC.
* **Verdict:** works, but it is a patchwork of overrides. Every new action (melee, special moves) hits the same
  wall: something only the local player runs. The proper fix is to find the whole local-player-only part of the frame
  and run all of it for puppets, instead of adding overrides one by one.

### Aim (where the gun points)
* **Ours:** the crosshair world point is synced; around the puppet's update the camera is swapped to the remote's
  view, and two detours give the puppet's aim task the remote's point (arm pitch).
* **MTA:** arm direction and camera in sync, applied in the context switch.
* **Tested:** yaw and pitch checked with test points; looks right.
* **Verdict:** the right approach.

### Bullets
* **Ours (new):** every shot the remote's game reports is fired from the puppet's weapon through the game's own shot
  function; the puppet animation's own shots are dropped, so it is one bullet per shot (`shots.cpp`).
* **MTA:** bullet sync sends each shot's **start and end point**; the receiver fires exactly that line.
* **Tested:** two copies: 12 of 20 and 26 of 38 shots hit (misses mostly while the target was dead).
* **Verdict:** close to MTA, but without start / end points the bullet leaves the puppet's gun toward its synced aim
  instead of along the shooter's own bullet line. Sending start / end per shot is the next step.

### Damage
* **Ours:** the victim's game decides (as in MTA). Each frame the local health is compared with the last; a drop
  reads the game's last attacker / weapon / hit zone, raises `clientPlayerDamage` (cancel = health put back) and
  goes to the server (`playerDamage`, killer on `playerWasted`).
* **MTA:** also the victim's game, but `onClientPlayerDamage` runs **before** the damage is applied, so even a lethal
  hit can be cancelled, and attacker / weapon / body part come straight from the hit.
* **Verdict:** right model, wrong place. **Fixed:** hits are now decided in the game's damage function (FUN_140ae5a40)
  before they land, lethal ones included, with the hit's own attacker / weapon / zone; polling only covers falls and
  fire.

### Hit reactions
* **Ours (new):** a puppet takes our hits through the damage function so the game plays its reaction; the damage is
  clamped below lethal and its health put back.
* **Tested:** seen once on screen (clutching the wound), lethal-damage hit clamped and survived.
* **Verdict:** right idea; special kills (head shots, knockouts) not checked.

### Melee
* **Broken.** The puppet gets the attack and aim presses but never takes the fist stance or punches; local punches
  rarely go through the damage function. Same wall as the weapon animations (local-player-only code).

## How we test, and why results kept misleading us

Everything has been tested with **two game copies on one PC**, driven by **debug automation**: a fake held aim,
an aim point sent without the camera turning, scripted fire presses. That produced several false trails: the test
players stood 1 m apart (no shot can go off), a copy "aimed" at the other while really looking elsewhere, the build
overwrote the test spawner. Most checks were log counters, few were visual, none ran over a real network.

## What should change

1. **Testing first.** Before more features: record a real input session (keyboard, mouse, camera) on one copy and
   replay it, capture both copies' screens side by side, and compare states with natives (jumping, weapon state, aim
   angles, position) to put numbers on every part above. Add latency / loss to the test link. And one real session
   with two people on two PCs to see what it looks like.
2. **Run the whole local-player frame for puppets**, not pieces. Map every step the game runs only for the local
   player (the game loop's local section: FUN_14034a6d0, FUN_140392f00, FUN_140348600, the camera's aim code) and run
   them in the puppet's context with its swapped camera. Then drop the weapon-state overrides. This is the change
   that should fix melee, special moves and the weapon animations at once, and it is how MTA gets away with forcing
   nothing.
3. **Damage in the damage function**: decide before it is applied (cancel lethal hits too), exact attacker / weapon /
   body part, no health polling.
4. **Bullet sync with start / end points**, MTA's packet: the shooter's game captures each bullet's line, the others
   fire that line.
5. **Head look sync** and velocity in the transform.

## Short version

The foundation is the right one: it is MTA's (the remote's real controller state into the game's own player code,
the victim decides damage, bullet sync). Movement, aim and weapon switching follow it well. Where it falls short:
weapon actions are forced into the puppet instead of coming from its own player code, damage is read after the fact
instead of decided in the damage function, bullets lack MTA's start / end points, head look and melee are missing,
and the testing has been too artificial to trust. Fix the testing first, then items 2 to 5.

## Progress (2026-11-24, after the review)
* Damage now decided in the damage function (item 3): done, verified.
* Item 2 mapped: the on-foot frame (FUN_14034f700) is already run for puppets almost whole (input step, actions,
  weapon state machine, after-weapon; only the inventory mapper's update is skipped, on purpose: weapon-wheel
  crosstalk). What only the local player gets is the camera / targeting side of the game loop (FUN_14037d120,
  FUN_14034a6d0, FUN_140392f00, FUN_140336810, FUN_140348600). There is one camera, so that part cannot simply run per
  puppet: its results have to be synced instead, like the aim point already is. For melee that result is the lock-on
  target. Where it is stored was not found: the automated test copy never really locks on (its camera is not on the
  target), so this needs a session with real input.
* Bullet start / end points (item 4): done after all, it mattered: a player's bullet starts at the camera, and a
  replay from the puppet's gun hit players the shooter missed. Synced shots now fly the shooter's camera -> crosshair
  line; in the duel test the shooter's hits and the victim's damage line up (about 70 ms apart).
