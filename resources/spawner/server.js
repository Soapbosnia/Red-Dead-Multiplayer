// spawner: a joining player waits in limbo (black screen, no control) until a script spawns them.
// spawnPlayer also puts the camera behind the player; the fade brings the screen back.

const SPAWN = { x: -2208.91, y: 16.51, z: 2605.41, heading: -100.3 }; // Armadillo Station

Events.on("playerConnected", (player) => {
    spawnPlayer(player, SPAWN.x, SPAWN.y, SPAWN.z, SPAWN.heading);
    fadeCamera(player, true, 1000);
    outputChatBox(getPlayerName(player) + " joined the server", null, 200, 200, 200);
});

Events.on("playerDisconnected", (player) => {
    outputChatBox(getPlayerName(player) + " left the server", null, 200, 200, 200);
});
