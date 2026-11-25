# How MTA:SA syncs players, and what we do differently (2026-11-24)

Notes from reading MTA:SA's source (`multitheftauto/mtasa-blue`: `Client/mods/deathmatch/logic/CNetAPI.cpp`,
`CClientPed.cpp`, `CClientPlayer.cpp`, `CClientGame.cpp`). Only how it works is described here; no MTA code is used
(it is GPL). Measurements of ours: `tools/syncdiff.py`, re/NOTES.md "Measuring the sync".

## What goes over the wire, and when

| | MTA | Ours |
|---|---|---|
| Full state ("puresync") | fixed interval, usually 100 ms, unreliable sequenced | every server tick (16.7 ms), unreliable |
| Buttons ("keysync") | an extra small packet **the moment a button changes**, the stick moves > 32, or camera / aim turns > 0.05 rad while moving / aiming | inside the transform only; taps held 120 ms so they survive |
| Contents of the full state | position (relative to what the player stands on: "contact entity"), rotation, **velocity**, health, armour, camera rotation, weapon slot + clip, arm angle, aim origin + target, controller state, flags: ducked, choking, on fire, stealth aiming, in water, hanging / climbing | position, heading, speed (no velocity vector), move stick, all 30 inputs, aim point, camera position, weapon state (aim state, drawn, reloading, ready time, shots, clip) |
| Shots | **a reliable packet per shot, sent when it is fired**: weapon, start, end, and the hit the shooter's game saw (damage, zone, victim) | a shot counter in the transform; the receiver fires one per increment |
| Death | the victim's game decides and sends it with the **death animation** it chose | the victim's game decides; the death animation is not sent |

## What the receiving game does with it

**Position.** MTA applies the newest state at once, with no interpolation buffer. Each full state sets a target: the
error (target minus where the ped is now) is spread linearly over the next sync interval, on top of what the ped's
own controls and physics do, so the ped always converges. It warps only past a threshold that grows with speed. An
**under-floor fix** forces the height when the remote is not moving vertically but the local ped is more than 10 cm
off for two syncs in a row. Velocity is set straight on the ped.
*Ours:* 30 ms behind the newest sample plus a lead prediction, and a pull only once the gap passes 0.6-1.0 m (height
only past 0.5 m). Measured: moving puppets 200-700 ms behind, 1.15 m median error, 0.47 m too low; standing ones float
0.49 m high.

**Rotation.** Set directly from each full state; aim (arm direction) and camera rotation are interpolated over the
aim interval. *Ours:* heading set from the interpolated stream; aim point interpolated.

**Controls.** The remote's controller state drives the remote ped through GTA's own player code (context switch), and
the remote peds are real `CPlayerPed`s, created like the local player. *Ours:* the same context switch, but our puppets
are NPC actors given the player components; their character controller's ground probe never hits (the game thinks
they are always in the air), see re/NOTES.md.

**States the keys cannot be trusted with.** MTA does not leave toggles to the buttons: the crouch button is zeroed and
`Duck(state)` is set from the flag; choking, on fire and stealth aiming are set from flags; if the remote hangs / climbs
the climbing task is started, if they are in water the swim task. *Ours:* everything from the buttons, plus forced
weapon state (aim state, ready time).

**Weapons.** Weapon slot changes are queued and applied with the aim interval. Remote peds fire from their synced fire
button (animation, effects), but for bullet-sync weapons that fire makes **no bullet**: bullets come only from the
shot packets, fired along the shooter's start-end line (`FireBullet`). *Ours:* the same split since bullet sync, but
shots ride the 16.7 ms transform as a counter instead of a per-shot event.

**Damage.** The victim's game decides. A remote player's health is **locked**: a local hit on them is undone. MTA
lets the hit animation play on a remote player, **except when they are aiming a gun** (it would break their aim) and
never the death animation. *Ours:* the same: lock (clamp + restore) and the hit reaction, none while they aim.

## Redesign, in order

1. **Position like MTA** (done: 0.05 m median moving, on the ground 100%, no delay beyond the network): correct toward the newest state continuously (error spread over ~100 ms), velocity on the
   puppet, height forced when the remote is not moving vertically (under-floor fix), warp past a speed-scaled
   threshold. Drop the dead zones.
2. **Puppets on the ground**: find why their ground probe never hits (first suspect: the player bit, actor +0x118, as
   for the aim reset), so their own player code stops thinking they are in the air.
3. **Shots as events**: done (shared/shot.h): one reliable message per shot, sent when fired, with start and end,
   relayed by the server; 17-48 ms on one PC. (The shooter's own hit report, MTA's backup damage, is not sent yet.)
4. **Explicit states** instead of buttons for toggles: crouch, cover, climbing / hanging, swimming, on fire; the death
   with the victim's death animation. *In progress:* sprint (the remote's gait onto the puppet's control object) and
   the cover state are synced; RDR's crouch is the low cover stance. The puppet leaves cover from the state, but does
   not take it yet (re/NOTES.md "Cover, crouch and sprint as states"). Climbing, swimming, fire, death animation: to do.
5. **No hit reaction while the remote aims**: done.
6. Then re-measure every part with `tools/syncdiff.py` and the duel test.
