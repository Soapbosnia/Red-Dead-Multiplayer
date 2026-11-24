// spawner: a joining player waits in limbo (black screen, no control) until a script spawns them.
// spawnPlayer also puts the camera behind the player; the fade brings the screen back.

const SPAWN = { x: -2208.91, y: 16.51, z: 2605.41, heading: -100.3 }; // Armadillo Station

const SPACING = 4; // metres between players spawning together, so they don't stand inside each other

Events.on("playerConnected", (player) => {
    // the n-th player here: 0, +4 m, -4 m, +8 m, ... sideways along the platform
    const n = getElementsByType("player").filter((p) => p !== player).length;
    const side = (n % 2 ? 1 : -1) * Math.ceil(n / 2) * SPACING;
    spawnPlayer(player, SPAWN.x, SPAWN.y, SPAWN.z + side, SPAWN.heading);
    fadeCamera(player, true, 1000);
    outputChatBox(getPlayerName(player) + " joined the server", null, 200, 200, 200);
});

Events.on("playerDisconnected", (player) => {
    outputChatBox(getPlayerName(player) + " left the server", null, 200, 200, 200);
});
