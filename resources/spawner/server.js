// spawner: a joining player is parked in limbo (black screen, no control) by the game until a script spawns them.
// This fades the camera, spawns the player at a fixed point, attaches the camera and fades back in.

const SPAWN = { x: -2208.91, y: 16.51, z: 2605.41, heading: -100.3 }; // Armadillo Station

// Per player, so one player joining never cancels another's spawn sequence.
const tokens = new Map();
const key = (player) => String(player);

Events.on("playerConnected", function (player) {
    const token = (tokens.get(key(player)) || 0) + 1;
    tokens.set(key(player), token);
    const current = () => tokens.get(key(player)) === token && isElement(player);

    console.log("[spawner] " + getPlayerName(player) + " joined");
    fadeCamera(player, false, 0);
    spawnPlayer(player, SPAWN.x, SPAWN.y, SPAWN.z, SPAWN.heading);
    setCameraTarget(player);

    setTimeout(function () {
        if (!current()) return;
        fadeCamera(player, true, 1000);
        Chat.sendToAll(getPlayerName(player) + " joined the server");
    }, 500);
});

Events.on("playerDisconnected", function (player) {
    tokens.delete(key(player));
});
