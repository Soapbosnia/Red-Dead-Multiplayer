// wasted: when a player dies, fade to black, wait, respawn them at the spawn point and fade back in.

const SPAWN = { x: -2208.91, y: 16.51, z: 2605.41, heading: -100.3 }; // Armadillo Station
const RESPAWN_DELAY_MS = 4000;

const tokens = new Map();
const key = (player) => String(player);

Events.on("playerWasted", function (player) {
    const token = (tokens.get(key(player)) || 0) + 1;
    tokens.set(key(player), token);
    const current = () => tokens.get(key(player)) === token && isElement(player);

    console.log("[wasted] " + getPlayerName(player) + " died");
    Chat.sendToAll(getPlayerName(player) + " was wasted");

    setTimeout(function () {
        if (!current()) return;
        fadeCamera(player, false, 800);
    }, RESPAWN_DELAY_MS - 1000);

    setTimeout(function () {
        if (!current()) return;
        spawnPlayer(player, SPAWN.x, SPAWN.y, SPAWN.z, SPAWN.heading);
        setCameraTarget(player);
        fadeCamera(player, true, 1000);
    }, RESPAWN_DELAY_MS);
});

Events.on("playerDisconnected", function (player) {
    tokens.delete(key(player));
});
