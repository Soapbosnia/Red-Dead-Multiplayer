# rdmp scripting API

Resources are JavaScript (V8) run by the MafiaHub Framework. A resource is a folder under `resources/` with a
`package.json`; its `mafiahub` block names the entry scripts. A "gamemode" is just a resource.

```json
{ "name": "example", "version": "1.0.0", "mafiahub": { "server": "server.js", "client": "client.js" } }
```

Client scripts are sent to players by the server automatically. The Framework's own globals (`Events`, `Chat`,
`console`, `setTimeout` / `setInterval`, ...) are available on both sides. Elements (players, blips) are opaque
handles; compare them with `String(element)` when used as map keys.

**Resource ownership:** every element a script creates belongs to the resource that created it and is destroyed when
that resource stops or restarts (server console: `start` / `stop` / `restart` / `refresh <resource>`).

Names follow MTA where an MTA function does the same thing. Below, each function is listed on the side(s) it exists on.

## Shared (server and client)

| Function | Notes |
|---|---|
| `tocolor(r, g, b[, a = 255])` → number | `0xAARRGGBB`; colours for `dx*` drawing |
| `BlipIcon.*` | RDR blip icon ids by name (`BlipIcon.BLIP_OBJECTIVE`, ...) |
| `getElementPosition(element)` → `{x, y, z}` | client: the local player's live game position |
| `getElementType(element)` → `"player"` \| `"blip"` \| `false` | |
| `isElement(value)` | `false` once the element is gone (a player who left) |
| `getElementsByType("player" \| "blip")` → array | client: what is streamed to this player |
| `getPlayerName(player)` | |
| `getElementHealth(player)` | the game's real health |
| `setElementPosition(element, x, y, z)` | client: the local player only |
| `setElementHealth(player, hp)` | client: the local player only; server: `0` is a scripted kill without animation |
| `killPed(ped)` | the game's own death (animation), then `playerWasted`; client: the local player only |
| `fadeCamera(...)` | server `(player, fadeIn[, ms])`, client `(fadeIn[, ms])` |
| `setCameraTarget(...)` | server `(player)`, client `()`: put the camera back behind the player (`spawnPlayer` does this already) |
| `toggleAllControls(...)` | server `(player, enabled)`, client `(enabled)` |
| `outputChatBox(...)` | server `(text[, visibleTo, r, g, b])` (visibleTo: a player, or null = everyone); client `(text[, r, g, b])` = this player's chat box |
| `showGameText(...)` | the game's HUD text. server `(player \| null, text[, ms, style])`, client `(text[, ms, style])`; style `"subtitle"` (bottom line, one at a time) or `"big"` (big centred title) |
| `setPlayerNametagShowing(player, ...)` | server `(player, bool)` for everyone; client `(player, bool \| null)` = this viewer's override (null follows the server) |

## Server

### Events (`Events.on(name, fn)`)
| Event | Arguments | When |
|---|---|---|
| `playerConnected` | player | joined; they wait in limbo (black screen) until a script spawns them |
| `playerDisconnected` | player | left |
| `playerSpawned` | player | `spawnPlayer` was called |
| `playerWasted` | player, killer \| null, weapon, bodyPart | died (any cause); killer, weapon and body part of the last hit if it came within 5 s (else `null`, -1, -1) |
| `playerDamage` | player, attacker \| null, weapon, bodyPart, loss | lost health; `attacker` is the player who hit them (`null`: a fall, fire, the world). The victim's own game decided the hit, as in MTA, and their client could have cancelled it (`clientPlayerDamage`); weapon ids as in Weapons, -1 = none |
| `playerChat` | player, message | a chat line; return `false` from a handler to keep it from being broadcast |
| `playerCommand` | player, command, ...args | a chat line starting with `/` (`/tp 1 2 3` → `"tp", "1", "2", "3"`) |
| `playerVehicleEnter` | player, vehicle | got on a vehicle (the game's own mount, E held, or `warpPedIntoVehicle`) |
| `playerVehicleExit` | player, vehicle | got off (own dismount, `removePedFromVehicle`, the vehicle destroyed, the rider left) |
| `playerWeaponSwitch` | player, previous, current | the weapon in hand changed (ids, -1 = none; the game holsters after a few seconds without aiming) |

### Players
| Function | Notes |
|---|---|
| `spawnPlayer(player, x, y, z[, heading])` | (re)spawn: full health, teleported, control on, camera behind |
| `setPlayerNametagText(player, text)` / `getPlayerNametagText(player)` | `""` = the player's name |
| `setPlayerNametagColor(player, r, g, b)` / `getPlayerNametagColor(player)` → `[r, g, b]` | `(player, false)` = default |
| `isPlayerNametagShowing(player)` | |

### Elements
| Function | Notes |
|---|---|
| `destroyElement(element)` | blips, vehicles |
| `attachElements(blip, element)` / `detachElements(blip)` / `getElementAttachedTo(blip)` | |
| `setElementVisibleTo(blip, player \| null, visible)` | a player: show/hide for them; `null`: everyone / nobody |

### Blips (radar + pause map)
| Function | Notes |
|---|---|
| `createBlip(x, y, z[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo])` → blip | |
| `createBlipAttachedTo(element[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo])` → blip | follows the element |
| `get/setBlipIcon`, `get/setBlipSize`, `get/setBlipColor`, `get/setBlipOrdering`, `get/setBlipVisibleDistance` | |
| `setBlipVisible(blip, bool)`, `setBlipDisplay(blip, "both" \| "radar" \| "map")` | |

`icon` is an RDR blip icon: a number 299..572 or a name (`BlipIcon.BLIP_OBJECTIVE`, `"objective"`). `size` 2 (the
default) is the engine's normal scale. The colour is a tint applied only when given (`BLIP_MP_COLOR_*` take tints
well). `visibleDistance >= 16383` means unlimited. `visibleTo` (a player) makes the blip exist for that player only.

### Vehicles (horses)
| Function | Notes |
|---|---|
| `createVehicle(model, x, y, z[, rz])` → vehicle | `model`: a rideable animal, actor definition 976..1005 (995 = saddle horse) |
| `getVehicleModel(vehicle)` | |
| `warpPedIntoVehicle(player, vehicle)` → bool | false when someone else rides it |
| `removePedFromVehicle(player)` → bool | false when they were on foot |
| `getPedOccupiedVehicle(player)` → vehicle \| false | |
| `getVehicleOccupant(vehicle)` → player \| false | |

A vehicle is an element: `getElementPosition`, `setElementPosition`, `getElementType` (`"vehicle"`),
`getElementsByType("vehicle")`, `destroyElement`. Players also get on and off with the game's own controls (hold E
near a horse); the server accepts it when the horse is free. While someone rides it, the rider's game syncs it; a
horse nobody rides stands still for everyone (no animal AI).

### Weapons
| Function | Notes |
|---|---|
| `giveWeapon(player, weapon[, ammo = 30[, inHand = false]])` → bool | adds it to their weapon wheel with that much ammo (the reserve of its ammo type) |
| `takeWeapon(player, weapon)` / `takeAllWeapons(player)` → bool | |
| `setWeaponAmmo(player, weapon, ammo)` → bool | |
| `setPedWeapon(player, weapon)` → bool | put it in hand; `-1` / `null` holsters |
| `getPedWeapon(player)` | the weapon in hand, `-1` = none |
| `getPedTotalAmmo(player)` | ammo of the weapon in hand |
| `getPedWeapons(player)` → `[ids]` | what the server gave and did not take |
| `getWeaponNameFromID(id)` / `getWeaponIDFromName(name)` | |

`weapon` is an id 0..31 or a name: the game's (`"REVOLVER_Cattleman"`) or just the model (`"cattleman"`). 0 Volcanic
pistol, 1 semi-auto pistol, 2 high power pistol, 3 Mauser, 4 Cattleman revolver, 5 Schofield, 6 double-action
revolver, 7 LeMat, 8 carbine repeater, 9 Winchester repeater, 10 Henry repeater, 11 Evans repeater, 12 Springfield
rifle, 13 bolt-action rifle, 14 Buffalo rifle, 15 sawed-off shotgun, 16 double-barrel shotgun, 17 pump-action
shotgun, 18 semi-auto shotgun, 19 rolling block rifle, 20 Carcano rifle, 21 lasso, 22 knife, 23 fire bottle, 24
dynamite, 25 throwing knife, 26 Gatling gun, 27 Browning gun, 28 cannon, 29 tomahawk, 30 bow, 31 antitank rifle.
Other players see the weapon in your hand and you using it (aim and fire come with your controls). Other players see
you aim where your crosshair is. In progress: damage events.

### Console
`players`, `tp <name> x y z`, `help`, plus the Framework's `start` / `stop` / `restart` / `refresh <resource>`, `status`.

## Client

Client scripts run on the game's main thread every frame.

### Events
| Event | Arguments |
|---|---|
| `clientRender` | ms since the last frame; draw with `dx*` here (drawing lasts one frame) |
| `clientKey` | key name, down (bool); only while the game has focus and the chat input is closed |
| `clientClick` | button (`"left"` \| `"right"` \| `"middle"`), state (`"down"` \| `"up"`), x, y; while the cursor is shown |
| `clientPlayerDamage` | attacker \| null, weapon, bodyPart, loss: the local player was hurt. Return `false` to cancel: the health goes back and the server hears nothing (a hit that kills cannot be cancelled) |

### Local player
| Function | Notes |
|---|---|
| `getLocalPlayer()` → element \| null | |
| `getLocalActor()` → number | the player's script Actor handle (for `invokeNative`), 0 = none |

### Drawing (overlay, pixels, one frame)
| Function | Notes |
|---|---|
| `dxDrawText(text, x, y[, color, scale, alignX, shadow])` | alignX `"left"` \| `"center"` \| `"right"` |
| `dxDrawRectangle(x, y, width, height[, color, rounding])` | |
| `dxDrawLine(x1, y1, x2, y2[, color, width])` | |
| `dxDrawCircle(x, y, radius[, color, filled = true, width])` | |
| `dxGetTextWidth(text[, scale])`, `dxGetFontHeight([scale])` | pixels |
| `guiGetScreenSize()` → `{x, y}` | |
| `getScreenFromWorldPosition(x, y, z)` → `{x, y}` \| `false` | false when behind the camera |
| `getCameraPosition()` → `{x, y, z}` \| `false` | |

### Cursor, chat, nametags
| Function | Notes |
|---|---|
| `showCursor(bool)`, `isCursorShowing()`, `getCursorPosition()` → `{x, y}` \| `false` | while shown the game gets no mouse input |
| `showChat(bool)`, `isChatVisible()`, `isChatInputActive()` | the chat box opens with **T**; typing never reaches the game |
| `showPlayerNametags(bool)`, `setNametagDrawDistance(meters)` | this viewer's nametags (default on, 60 m; your own tag is hidden) |

### Input and game natives
| Function | Notes |
|---|---|
| `isKeyDown(key)`, `getTickCount()` | keys: `"a".."z"`, `"0".."9"`, `"f1".."f12"`, `"num_0"..`, `"enter"`, `"space"`, `"tab"`, `"lshift"`, `"mouse1"`, ... |
| `getPedWeapon(player)`, `getPedTotalAmmo(player)`, `getWeaponNameFromID(id)`, `getWeaponIDFromName(name)` | as on the server |
| `bindKey(key, "down" \| "up" \| "both", handler(key, state) [, description])` | MTA-style key bind (same key names); with a description it is listed in the player's Settings > Binds under the resource's name, and the player can move it to another key (kept in their rdmp.ini). Removed when the resource stops. |
| `unbindKey(key [, state [, handler]])` -> bool | this resource's binds on that key |
| `invokeNative(nameOrHash, types, ...args)` | any game native |

`invokeNative` type string: one letter per argument, `i` int, `u` unsigned, `f` float, `b` bool, `s` string,
`v` Vector3 in (`{x,y,z}` or `[x,y,z]`), `V` Vector3 out; end with `>x` for the return type (`i`, `u`, `f`, `b`,
`n` = none; default `i`). With `V` arguments the result is `{result, vectors: [{x,y,z}, ...]}`. Names are hashed with
joaat (lower-case); `re/natives_0x673783F3.csv` lists the natives this build has. Some natives crash with bad handles
(`GET_CAMERA_POSITION` does): prefer the functions above.

## Bundled resources
| Resource | Side | What |
|---|---|---|
| `spawner` | server | on join: spawn at Armadillo station (side by side, 4 m apart, when several join), fade in, join / leave lines in chat |
| `wasted` | server | on death: announce (and who killed them, with what), fade out, respawn after 4 s, fade in |
| `playerblips` | server | a random-colour blip on every player (`SHOW_OWN_BLIP = true` while testing alone) |
| `scoreboard` | client | hold Tab: the players (a `dx*` drawing example) |
| `devtools` | both | F4 god mode (cancels your damage), F7 position, F8 die, F9 players; logs `playerDamage` on the server console; `/pos`, `/tp x y z`, `/kill`, `/heal`, `/big text`, `/horse [model]`, `/mount`, `/dismount` (testing only) |
