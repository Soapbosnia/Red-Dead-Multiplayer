// spawner: a joining player waits in limbo (black screen, no control) until a script spawns them.
// spawnPlayer also puts the camera behind the player; the fade brings the screen back.

const SPAWN = { x: -2208.91, y: 16.51, z: 2605.41, heading: -100.3 }; // Armadillo Station

const SPACING = 4; // metres between players spawning together, so they don't stand inside each other
// Testing: > 0 spawns the second player this many metres in front of the first, the two facing each other (each one's
// camera then looks at the other: real aiming, shots and melee between two game copies). 0 = side by side.
const DUEL_DISTANCE = 0;
const duel = [null, null]; // the two players in the duel slots

Events.on("playerConnected", (player) => {
    const n = getElementsByType("player").filter((p) => p !== player).length;
    const slot = DUEL_DISTANCE > 0 ? duel.findIndex((p) => !p || !isElement(p)) : -1;
    if (slot >= 0) {
        // along the platform (+z): the first faces +z (heading 180), the second stands further on, facing back (0)
        duel[slot] = player;
        // (the second 1.5 m to the side, so each one's camera sees the other past its own player), each facing the other
        const dx = slot === 0 ? 1.5 : -1.5, dz = slot === 0 ? DUEL_DISTANCE : -DUEL_DISTANCE;
        const heading = (Math.atan2(-dx, -dz) * 180) / Math.PI; // the game's heading faces (-sin h, -cos h)
        spawnPlayer(player, SPAWN.x + slot * 1.5, SPAWN.y, SPAWN.z + slot * DUEL_DISTANCE, heading);
    } else {
        // the n-th player here: 0, +4 m, +8 m, ... along the platform (-z of the spawn is a stack of crates)
        const side = n * SPACING;
        spawnPlayer(player, SPAWN.x, SPAWN.y, SPAWN.z + side, SPAWN.heading);
    }
    fadeCamera(player, true, 1000);
    outputChatBox(getPlayerName(player) + " joined the server", null, 200, 200, 200);
});

Events.on("playerDisconnected", (player) => {
    outputChatBox(getPlayerName(player) + " left the server", null, 200, 200, 200);
});
