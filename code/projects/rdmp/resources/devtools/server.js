// devtools (server): chat commands for testing. Remove this resource from servers people play on.
//   /pos   /tp x y z   /kill   /heal   /big text   /horse [model]   /mount   /dismount
//   /weapon name|id [ammo]   /weapons   /disarm

const commands = {
    pos(player) {
        const p = getElementPosition(player);
        outputChatBox("pos " + p.x.toFixed(2) + " " + p.y.toFixed(2) + " " + p.z.toFixed(2), player);
    },
    tp(player, x, y, z) {
        if (z === undefined) return outputChatBox("usage: /tp x y z", player);
        setElementPosition(player, Number(x), Number(y), Number(z));
    },
    kill(player) { killPed(player); },
    heal(player) { setElementHealth(player, 100); },
    big(player, ...words) { showGameText(null, words.join(" ") || "Red Dead Multiplayer", 3000, "big"); },
    // a horse next to you (model: a rideable animal 976..1005, default 995 = saddle horse)
    horse(player, model) {
        const p = getElementPosition(player);
        const horse = createVehicle(Number(model) || 995, p.x + 3, p.y, p.z);
        outputChatBox(horse ? "horse created" : "could not create a horse", player);
    },
    // onto the nearest horse within 15 m
    mount(player) {
        const p = getElementPosition(player);
        let best = null, bestDist = 15;
        for (const v of getElementsByType("vehicle")) {
            const q = getElementPosition(v);
            const d = Math.hypot(q.x - p.x, q.z - p.z);
            if (d < bestDist && !getVehicleOccupant(v)) { best = v; bestDist = d; }
        }
        if (!best) return outputChatBox("no free horse nearby", player);
        warpPedIntoVehicle(player, best);
    },
    dismount(player) { removePedFromVehicle(player); },
    // a weapon in hand: /weapon cattleman 60   (names: getWeaponNameFromID 0..31)
    weapon(player, name, ammo) {
        if (name === undefined) return outputChatBox("usage: /weapon name|id [ammo]", player);
        const id = isNaN(Number(name)) ? getWeaponIDFromName(name) : Number(name);
        if (getWeaponNameFromID(id) === null) return outputChatBox("unknown weapon " + name, player);
        giveWeapon(player, id, Number(ammo) || 60, true);
        outputChatBox("given " + getWeaponNameFromID(id), player);
    },
    weapons(player) {
        const names = [];
        for (let i = 0; i < 32; i++) names.push(i + " " + getWeaponNameFromID(i));
        outputChatBox(names.join(", "), player);
    },
    disarm(player) { takeAllWeapons(player); },
};

Events.on("playerWeaponSwitch", (player, previous, current) =>
    console.log(getPlayerName(player) + " weapon " + previous + " -> " + current));

Events.on("playerVehicleEnter", (player, vehicle) => outputChatBox(getPlayerName(player) + " mounted a horse"));
Events.on("playerVehicleExit", (player, vehicle) => outputChatBox(getPlayerName(player) + " got off a horse"));

Events.on("playerCommand", (player, command, ...args) => {
    const fn = commands[command];
    if (fn) fn(player, ...args);
    else outputChatBox("commands: /" + Object.keys(commands).join(" /"), player);
});
