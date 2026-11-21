# rdmp scripting API

Resources are JavaScript (V8) run by the MafiaHub Framework. Each resource is a folder under `resources/` with a
`package.json` whose `mafiahub` block names the entry scripts:

```json
{ "name": "example", "version": "1.0.0", "mafiahub": { "server": "server.js", "client": "client.js" } }
```

The Framework's own globals (`Events`, `Chat`, `console`, `setTimeout`/`setInterval`, ...) are available on both
sides. Elements (players, blips) are opaque handles; compare them with `String(element)` when used as map keys.

**Resource ownership:** every element a script creates belongs to the resource that created it and is destroyed when
that resource stops or restarts.

## Server

### Events (`Events.on(name, fn)`)
| Event | Arguments | When |
|---|---|---|
| `playerConnected` | player | a player joined (they wait in limbo, black screen, until a script spawns them) |
| `playerDisconnected` | player | a player left |
| `playerSpawned` | player | the player's game confirmed a spawn |
| `playerWasted` | player | the player died |

### Players
| Function | Notes |
|---|---|
| `spawnPlayer(player, x, y, z[, heading])` | (re)spawns: full health, teleported, control on |
| `fadeCamera(player, fadeIn, ms)` | `fadeIn` true = fade from black |
| `setCameraTarget(player)` | gameplay camera back on the player |
| `setElementControl(player, enabled)` | toggles player control |
| `setElementHealth(player, hp)` / `getElementHealth(player)` | |
| `getPlayerName(player)` | |

### Elements
| Function | Notes |
|---|---|
| `getElementPosition(element)` → `{x, y, z}` | |
| `setElementPosition(element, x, y, z)` | |
| `getElementType(element)` → `"player"` / `"blip"` / `false` | |
| `isElement(value)` | |
| `getElementsByType("player" \| "blip")` → array | |
| `destroyElement(element)` | blips |
| `attachElements(blip, element)` / `detachElements(blip)` / `getElementAttachedTo(blip)` | |
| `setElementVisibleTo(blip, player \| null, visible)` | MTA semantics: a player = show/hide for that player; `null` = everyone/nobody |

### Blips (radar + pause map)
| Function | Notes |
|---|---|
| `createBlip(x, y, z[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo])` → blip | |
| `createBlipAttachedTo(element[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo])` → blip | follows the element |
| `get/setBlipIcon`, `get/setBlipSize`, `get/setBlipColor`, `get/setBlipOrdering`, `get/setBlipVisibleDistance` | |
| `setBlipVisible(blip, bool)`, `setBlipDisplay(blip, "both" \| "radar" \| "map")` | |

Differences from MTA: `icon` is an RDR blip icon, a number 299..572 or a name (`BlipIcon.BLIP_OBJECTIVE`,
`"objective"`); the global `BlipIcon` lists them all. `size` 2 (the default) is the engine's normal scale. The colour
is a tint applied only when given (most RDR icons have their own colours; `BLIP_MP_COLOR_*` take tints well).
`visibleDistance >= 16383` means unlimited. `visibleTo` (a player) makes the blip exist for that player only.

### Console
`players`, `tp <name> x y z`, `help` on the server console, plus the Framework's own `start` / `stop` / `restart` /
`refresh <resource>` and `status`. Stopping or restarting a resource destroys the elements it created.

## Client

Client scripts run on the game's main thread every frame.

### Events
| Event | Arguments |
|---|---|
| `clientKey` | key name (`"f8"`, `"e"`, `"space"`, ...), down (bool); only while the game window has focus |
| `clientRender` | ms since the last frame |

### Functions
| Function | Notes |
|---|---|
| `getLocalPlayer()` → element \| null | |
| `getLocalActor()` → number | the local player's script Actor handle (`GET_PLAYER_ACTOR`, for `invokeNative`), 0 = none |
| `getElementPosition(element)` | the local player's live game position |
| `setElementPosition(localPlayer, x, y, z)` | local player only |
| `getElementHealth(element)`, `setElementHealth(localPlayer, hp)` | |
| `getPlayerName`, `getElementType`, `isElement`, `getElementsByType` | as on the server |
| `fadeCamera(fadeIn, ms)`, `setCameraTarget()`, `toggleAllControls(enabled)` | the local player |
| `outputChatBox(text[, ms])` | a line at the bottom centre of this player's screen (one at a time: a new line replaces the current one after a short fade) |
| `isKeyDown(key)`, `getTickCount()` | |
| `invokeNative(nameOrHash, types, ...args)` | calls any game native |
| `BlipIcon.*` | |

`invokeNative` type string: one letter per argument, `i` int, `u` unsigned, `f` float, `b` bool, `s` string,
`v` Vector3 in (`{x,y,z}` or `[x,y,z]`), `V` Vector3 out; end with `>x` for the return type (`i`, `u`, `f`, `b`,
`n` = none; default `i`). With `V` arguments the result is `{result, vectors: [{x,y,z}, ...]}`. Names are hashed
with joaat (lower-case); `re/natives_0x673783F3.csv` lists the natives this build registers.

```js
invokeNative("KILL_ACTOR", "i>n", getLocalActor());   // die (with the death animation)
```

## Bundled resources
| Resource | Side | What |
|---|---|---|
| `spawner` | server | on join: fade out, spawn at Armadillo station, camera, fade in |
| `wasted` | server | on death: announce, fade out, respawn after 4 s, fade in |
| `playerblips` | server | a random-colour blip on every player, visible to everyone but that player (`SHOW_OWN_BLIP = true` shows it to them too, for testing alone) |
| `devtools` | client | F7 position, F8 die, F9 players (testing only) |
