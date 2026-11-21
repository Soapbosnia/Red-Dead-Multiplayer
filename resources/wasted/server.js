// wasted: when a player dies, announce it, fade to black, respawn them at the spawn point and fade back in.

const SPAWN = { x: -2208.91, y: 16.51, z: 2605.41, heading: -100.3 }; // Armadillo Station
const RESPAWN_DELAY_MS = 4000;

Events.on("playerWasted", (player) => {
    outputChatBox(getPlayerName(player) + " was wasted", null, 220, 60, 60);

    setTimeout(() => {
        if (isElement(player)) fadeCamera(player, false, 800);
    }, RESPAWN_DELAY_MS - 1000);

    setTimeout(() => {
        if (!isElement(player)) return; // left meanwhile
        spawnPlayer(player, SPAWN.x, SPAWN.y, SPAWN.z, SPAWN.heading);
        fadeCamera(player, true, 1000);
    }, RESPAWN_DELAY_MS);
});
