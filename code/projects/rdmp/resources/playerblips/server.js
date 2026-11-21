// playerblips: every player gets a blip on the radar and pause map in a random colour. Everybody else sees it;
// the player themselves does not (their own arrow already marks them). The blips belong to this resource, so
// stopping it removes them all.

// true: the player also sees their own blip (handy when testing alone).
const SHOW_OWN_BLIP = false;
// Blip size (MTA scale: 2 = the game's own icon size, 1 = half of it).
const BLIP_SIZE = 1;

const blips = new Map(); // player -> blip
const key = (player) => String(player);

function randomColor() {
    // Bright, saturated colours read well on the radar: one channel high, one low, one random.
    const c = [255, 40 + Math.floor(Math.random() * 60), Math.floor(Math.random() * 256)];
    for (let i = c.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [c[i], c[j]] = [c[j], c[i]];
    }
    return c;
}

function addBlip(player) {
    if (blips.has(key(player))) return;
    const [r, g, b] = randomColor();
    // createBlipAttachedTo(element, icon, size, r, g, b, a, ordering, visibleDistance)
    const blip = createBlipAttachedTo(player, BlipIcon.BLIP_MP_COLOR_1, BLIP_SIZE, r, g, b, 255, 1, 16383);
    if (!blip) return;
    if (!SHOW_OWN_BLIP) setElementVisibleTo(blip, player, false);
    blips.set(key(player), blip);
    console.log("[playerblips] " + getPlayerName(player) + " -> rgb(" + r + ", " + g + ", " + b + ")");
}

function removeBlip(player) {
    const blip = blips.get(key(player));
    if (blip) destroyElement(blip);
    blips.delete(key(player));
}

Events.on("playerConnected", addBlip);
Events.on("playerDisconnected", removeBlip);

// Players already on the server when the resource (re)starts.
for (const player of getElementsByType("player")) addBlip(player);
