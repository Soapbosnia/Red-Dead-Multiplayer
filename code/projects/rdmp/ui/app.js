// Red Dead Multiplayer main menu. Talks to the client (src/client/frontend.cpp):
//   page -> client: callEvent("rdmp:<name>", json)      client -> page: rdmp.receive({type, data})
// Events out: ready, refresh {list}, connect {host, port, password?}, favorite {host, port, name, add},
//             settings {nickname, nametags, nametagDistance, chat}, gameSettings {values}, quit.
// Messages in: settings, gameSettings {available, values, modes}, version, lists, server, listError, status.
"use strict";

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
const icon = (name) => { const s = document.createElementNS("http://www.w3.org/2000/svg", "svg"); s.setAttribute("class", "i"); const u = document.createElementNS("http://www.w3.org/2000/svg", "use"); u.setAttribute("href", "#i-" + name); s.appendChild(u); return s; };

function send(name, data) {
  if (typeof callEvent === "function") callEvent("rdmp:" + name, JSON.stringify(data || {}));
  else console.log("[rdmp] would send", name, data); // opened in a normal browser
}

// ---------- toast ----------
let toastTimer = 0;
function toast(message, kind) {
  const t = $("#toast");
  t.textContent = message;
  t.className = "toast show" + (kind ? " " + kind : "");
  clearTimeout(toastTimer);
  if (kind !== "busy") toastTimer = setTimeout(() => t.classList.remove("show"), 4000);
}

// ---------- pages ----------
function showPage(page) {
  $$(".nav-item[data-page]").forEach((b) => b.classList.toggle("active", b.dataset.page === page));
  $$(".page").forEach((p) => p.classList.toggle("active", p.id === "page-" + page));
}
$$(".nav-item[data-page]").forEach((b) => b.addEventListener("click", () => showPage(b.dataset.page)));
$("#quit").addEventListener("click", () => send("quit"));

// =====================================================================================================
// Server browser
// =====================================================================================================
const lists = { internet: new Map(), local: new Map(), favorites: new Map(), recent: new Map() };
const listErrors = {};
let favorites = [], recentEntries = [];
let currentList = "internet";
let selected = null;           // "host:port"
let sortKey = "players", sortAsc = false;
let pendingPassword = null;

const keyOf = (host, port) => host + ":" + port;
const isFavorite = (key) => favorites.some((f) => keyOf(f.host, f.port) === key);

function parseAddress(text) {
  const m = /^\s*([^\s:]+)(?::(\d{1,5}))?\s*$/.exec(text || "");
  if (!m) return null;
  const port = m[2] ? Number(m[2]) : 27015;
  return port > 0 && port < 65536 ? { host: m[1], port } : null;
}
function findServer(key) {
  for (const list of Object.values(lists)) if (list.has(key)) return list.get(key);
  return null;
}

$$(".tab").forEach((t) => t.addEventListener("click", () => selectList(t.dataset.list)));
function selectList(list) {
  currentList = list;
  $$(".tab").forEach((t) => t.classList.toggle("active", t.dataset.list === list));
  refresh();
}
function refresh() {
  lists[currentList].clear();
  delete listErrors[currentList];
  if (currentList === "favorites" || currentList === "recent") {
    for (const e of (currentList === "favorites" ? favorites : recentEntries))
      lists[currentList].set(keyOf(e.host, e.port), { host: e.host, port: e.port, name: e.name || "", online: null });
  }
  render();
  send("refresh", { list: currentList });
}
$("#refresh").addEventListener("click", refresh);
$("#search").addEventListener("input", render);
$("#hide-empty").addEventListener("change", render);
$("#hide-locked").addEventListener("change", render);
$$(".servers th.sortable").forEach((th) => th.addEventListener("click", () => {
  if (sortKey === th.dataset.sort) sortAsc = !sortAsc; else { sortKey = th.dataset.sort; sortAsc = sortKey !== "players"; }
  render();
}));

const pingClass = (ping) => ping < 0 ? "" : ping < 80 ? "good" : ping < 180 ? "mid" : "bad";

function visibleRows() {
  const filter = $("#search").value.trim().toLowerCase();
  const hideEmpty = $("#hide-empty").checked, hideLocked = $("#hide-locked").checked;
  const keep = currentList === "favorites" || currentList === "recent";
  const rows = [...lists[currentList].values()]
    .filter((s) => s.online !== false || keep)
    .filter((s) => !filter || (s.name || "").toLowerCase().includes(filter) || keyOf(s.host, s.port).includes(filter))
    .filter((s) => !hideEmpty || (s.players || 0) > 0)
    .filter((s) => !hideLocked || !s.password);
  const dir = sortAsc ? 1 : -1;
  rows.sort((a, b) => {
    if ((a.online === true) !== (b.online === true)) return (b.online === true) - (a.online === true);
    if (sortKey === "name") return dir * (a.name || keyOf(a.host, a.port)).localeCompare(b.name || keyOf(b.host, b.port));
    if (sortKey === "ping") return dir * ((a.ping >= 0 ? a.ping : 9999) - (b.ping >= 0 ? b.ping : 9999));
    return dir * ((a.players || 0) - (b.players || 0));
  });
  return rows;
}

function render() {
  $$(".servers th.sortable").forEach((th) => { th.classList.toggle("sorted", th.dataset.sort === sortKey); th.classList.toggle("asc", sortAsc); });
  const rows = visibleRows();
  const body = $("#rows");
  body.textContent = "";
  for (const s of rows) {
    const key = keyOf(s.host, s.port);
    const tr = el("tr", (key === selected ? "selected " : "") + (s.online === false ? "offline" : ""));
    const fav = el("td", "c-fav");
    if (isFavorite(key)) fav.appendChild(icon("star-fill"));
    const name = el("td");
    const nc = el("div", "name-cell");
    if (s.password) nc.appendChild(icon("lock"));
    const nm = el("div");
    nm.appendChild(el("span", "nm", s.name || (s.online === false ? "Offline" : s.online === null ? "Asking…" : key)));
    nm.appendChild(el("span", "sub", key + (s.version ? "  ·  v" + s.version : "")));
    nc.appendChild(nm);
    name.appendChild(nc);
    const players = el("td", "c-players");
    if (s.online) {
      const pb = el("div", "pbar");
      const track = el("div", "track"), fill = el("div", "fill");
      fill.style.width = Math.min(100, (s.players / Math.max(1, s.maxPlayers)) * 100) + "%";
      track.appendChild(fill);
      pb.appendChild(track);
      pb.appendChild(el("span", "num", s.players + " / " + s.maxPlayers));
      players.appendChild(pb);
    }
    const ping = el("td", "c-ping");
    if (s.online && s.ping >= 0) {
      const p = el("div", "ping");
      p.appendChild(el("span", "dot " + pingClass(s.ping)));
      p.appendChild(document.createTextNode(s.ping + " ms"));
      ping.appendChild(p);
    }
    tr.append(fav, name, players, ping);
    tr.addEventListener("click", () => select(key));
    tr.addEventListener("dblclick", () => { select(key); join(); });
    body.appendChild(tr);
  }
  const empty = $("#empty");
  empty.style.display = rows.length ? "none" : "block";
  empty.textContent = "";
  const [title, text] = listErrors[currentList] ? ["Could not load the list", listErrors[currentList]]
    : currentList === "favorites" ? ["No favorites yet", "Select a server and press Favorite."]
    : currentList === "recent" ? ["Nothing here yet", "Servers you join show up here."]
    : currentList === "local" ? ["No servers on this PC", "Start RDMPServer.exe, then refresh."]
    : ["Looking for servers…", "Asking the server list."];
  empty.appendChild(el("b", null, title));
  empty.appendChild(document.createTextNode(text));
  const online = rows.filter((s) => s.online).length;
  const players = rows.reduce((n, s) => n + (s.online ? s.players : 0), 0);
  $("#count").textContent = rows.length ? `${online} server${online === 1 ? "" : "s"} online · ${players} player${players === 1 ? "" : "s"}` : "";
  renderDetail();
}

function select(key) {
  selected = key;
  $("#address").value = key;
  render();
}

function renderDetail() {
  const address = parseAddress($("#address").value);
  const key = address ? keyOf(address.host, address.port) : null;
  const s = key ? findServer(key) : null;
  $("#d-name").textContent = s && s.name ? s.name : key ? key : "No server selected";
  $("#d-addr").textContent = key ? key : "Pick one from the list, or connect directly below.";
  const facts = $("#d-facts");
  facts.hidden = !(s && s.online);
  if (s && s.online) {
    $("#d-players").textContent = s.players + " / " + s.maxPlayers;
    $("#d-ping").textContent = s.ping >= 0 ? s.ping + " ms" : "—";
    $("#d-version").textContent = s.version ? "v" + s.version : "—";
    $("#d-access").textContent = s.password ? "Password" : "Open";
  }
  const fav = $("#favorite-toggle");
  fav.disabled = !key;
  fav.querySelector("span").textContent = key && isFavorite(key) ? "Unfavorite" : "Favorite";
  fav.querySelector("use").setAttribute("href", key && isFavorite(key) ? "#i-star-fill" : "#i-star");
  $("#connect").disabled = !key;
}

function join() {
  const address = parseAddress($("#address").value);
  if (!address) return toast("Enter a server address (host:port).", "error");
  const s = findServer(keyOf(address.host, address.port));
  if (s && s.password) {
    pendingPassword = address;
    $("#password-for").textContent = (s.name || keyOf(address.host, address.port)) + " asks for a password.";
    $("#password").value = "";
    $("#password-modal").hidden = false;
    $("#password").focus();
    return;
  }
  send("connect", { host: address.host, port: address.port });
}
$("#connect").addEventListener("click", join);
$("#direct-connect").addEventListener("click", join);
$("#address").addEventListener("input", () => { selected = null; render(); });
$("#address").addEventListener("keydown", (e) => { if (e.key === "Enter") join(); });
$("#favorite-toggle").addEventListener("click", () => {
  const address = parseAddress($("#address").value);
  if (!address) return;
  const key = keyOf(address.host, address.port);
  const s = findServer(key);
  send("favorite", { host: address.host, port: address.port, name: s ? s.name : "", add: !isFavorite(key) });
});

function submitPassword() {
  if (!pendingPassword) return;
  send("connect", { host: pendingPassword.host, port: pendingPassword.port, password: $("#password").value });
  $("#password-modal").hidden = true;
  pendingPassword = null;
}
$("#password-ok").addEventListener("click", submitPassword);
$("#password-cancel").addEventListener("click", () => { $("#password-modal").hidden = true; pendingPassword = null; });
$("#password").addEventListener("keydown", (e) => { if (e.key === "Enter") submitPassword(); if (e.key === "Escape") $("#password-cancel").click(); });

// =====================================================================================================
// Settings
// =====================================================================================================
// Item types: toggle, slider {min, max, step, fmt}, seg {options: [[value, label]]}, select (resolution / refresh),
// text. Multiplayer items live in rdmp.ini (the client); everything else is the game's own setting (key = its XML
// name, applied through the game, which saves its own files).
const pct = (v) => Math.round(v * 100) + "%";
const meters = (v) => Math.round(v) + " m";
const SECTIONS = [
  { id: "multiplayer", icon: "multiplayer", label: "Multiplayer", groups: [
    { title: "You", items: [
      { key: "mp.nickname", label: "Nickname", hint: "Shown to other players", type: "text", max: 22 },
    ] },
    { title: "Other players", items: [
      { key: "mp.nametags", label: "Nametags", hint: "Names and health above other players", type: "toggle" },
      { key: "mp.nametagDistance", label: "Nametag distance", type: "slider", min: 10, max: 200, step: 5, fmt: meters },
      { key: "mp.chat", label: "Chat box", hint: "T opens it in game", type: "toggle" },
    ] },
  ] },
  { id: "display", icon: "display", label: "Display", groups: [
    { title: "Screen", items: [
      { key: "Fullscreen", label: "Fullscreen", type: "toggle" },
      { key: "@resolution", label: "Resolution", type: "resolution" },
      { key: "RefreshRate", label: "Refresh rate", type: "refresh" },
      { key: "Vsync", label: "V-Sync", type: "seg", options: [[0, "Off"], [1, "On"]] },
      { key: "TripleBuffer", label: "Triple buffering", type: "toggle" },
      { key: "FrameRateLimit", label: "Frame rate limit", type: "seg", options: [[0, "30"], [1, "60"], [2, "Unlimited"]] },
      { key: "bFocusPaused", label: "Pause when unfocused", type: "toggle" },
      { key: "bConstrainMouse", label: "Keep the mouse in the window", type: "toggle" },
    ] },
    { title: "Picture", items: [
      { key: "display.fBrightness", label: "Brightness", type: "slider", min: 0, max: 1, step: 0.01, fmt: pct },
      { key: "display.fContrast", label: "Contrast", type: "slider", min: 0, max: 1, step: 0.01, fmt: pct },
      { key: "display.fSaturation", label: "Saturation", type: "slider", min: 0, max: 1, step: 0.01, fmt: pct },
    ] },
    { title: "HDR", items: [
      { key: "HDR", label: "HDR", hint: "Needs an HDR display", type: "toggle" },
      { key: "PeakBrightness", label: "Peak brightness", type: "slider", min: 200, max: 2000, step: 50, fmt: (v) => Math.round(v) + " nits" },
      { key: "PaperWhite", label: "Paper white", type: "slider", min: 80, max: 500, step: 10, fmt: (v) => Math.round(v) + " nits" },
    ] },
  ] },
  { id: "graphics", icon: "graphics", label: "Graphics", groups: [
    { title: "Quality", items: [
      { key: "ShadowQuality", label: "Shadow quality", type: "seg", options: [[0, "Low"], [1, "Medium"], [2, "High"], [3, "Ultra"]] },
      { key: "ShadowSoftness", label: "Shadow softness", type: "seg", options: [[0, "Low"], [1, "Medium"], [2, "High"], [3, "Ultra"]] },
      { key: "ShadowBlend", label: "Shadow blend", type: "seg", options: [[0, "Low"], [1, "Medium"], [2, "High"]] },
      { key: "MinAnisotropicFiltering", label: "Anisotropic filtering", type: "seg", options: [[1, "1x"], [2, "2x"], [4, "4x"], [8, "8x"], [16, "16x"]] },
      { key: "aaAntiAliasing", label: "Anti-aliasing", type: "seg", options: [[0, "Off"], [1, "Low"], [2, "High"]] },
      { key: "MotionBlurStyle", label: "Motion blur", type: "seg", options: [[0, "Off"], [1, "Camera"], [2, "Full"]] },
      { key: "MotionBlurStrength", label: "Motion blur strength", type: "slider", min: 0, max: 1, step: 0.05, fmt: pct },
    ] },
    { title: "World detail", items: [
      { key: "WorldStreamingRadius", label: "World streaming distance", type: "slider", min: 80, max: 320, step: 10, fmt: meters },
      { key: "TerrainStreamingFactor", label: "Terrain detail", type: "slider", min: 1, max: 10, step: 0.5, fmt: (v) => v.toFixed(1) },
      { key: "TreeLevelOfDetail", label: "Tree detail", type: "slider", min: 0, max: 4, step: 0.5, fmt: (v) => v.toFixed(1) },
      { key: "TreeImposterHighLodStreamingDistance", label: "Distant trees", type: "slider", min: 256, max: 2048, step: 64, fmt: meters },
      { key: "GrassStreamingDistance", label: "Grass distance", type: "slider", min: 50, max: 600, step: 10, fmt: meters },
    ] },
    { title: "Upscaling and latency", items: [
      { key: "DynamicResolution", label: "Dynamic resolution", type: "toggle" },
      { key: "FSR3UpscalingQuality", label: "AMD FSR 3", type: "seg", options: [[0, "Off"], [1, "Quality"], [2, "Balanced"], [3, "Performance"], [4, "Ultra perf."]] },
      { key: "FSR3AdditionalSharpness", label: "FSR sharpening", type: "slider", min: 0, max: 1, step: 0.05, fmt: pct },
      { key: "DLSSUpscalingQuality", label: "NVIDIA DLSS", type: "seg", options: [[0, "Off"], [1, "Quality"], [2, "Balanced"], [3, "Performance"], [4, "Ultra perf."]] },
      { key: "DLSSFrameGeneration", label: "DLSS frame generation", type: "seg", options: [[0, "Off"], [1, "On"]] },
      { key: "NVIDIAReflex", label: "NVIDIA Reflex", type: "seg", options: [[0, "Off"], [1, "On"], [2, "On + Boost"]] },
    ] },
  ] },
  { id: "audio", icon: "audio", label: "Audio", groups: [
    { title: "Volume", items: [
      { key: "audio.fSFXVolume", label: "Sound effects", type: "slider", min: 0, max: 1, step: 0.01, fmt: pct },
      { key: "audio.fMusicVolume", label: "Music", type: "slider", min: 0, max: 1, step: 0.01, fmt: pct },
      { key: "audio.bHeadphones", label: "Headphones mix", type: "toggle" },
    ] },
  ] },
  { id: "kbm", icon: "keyboard", label: "Keyboard & mouse", groups: [
    { title: "Mouse", items: [
      { key: "kbMouse.fMouseScalarSensitivity", label: "Mouse sensitivity", type: "slider", min: 0, max: 1, step: 0.01, fmt: pct },
      { key: "kbMouse.fADSScalarSensitivity", label: "Aiming sensitivity", type: "slider", min: 0, max: 1, step: 0.01, fmt: pct },
      { key: "kbMouse.bInvertedY", label: "Invert vertical look", type: "toggle" },
      { key: "kbMouse.bSwapCamX", label: "Invert horizontal look", type: "toggle" },
    ] },
    { title: "Movement", items: [
      { key: "kbMouse.bHoldSprint", label: "Hold to sprint", type: "toggle" },
      { key: "kbMouse.bHoldSprintVehicle", label: "Hold to spur the horse", type: "toggle" },
      { key: "kbMouse.iHorseCameraRelativity", label: "Horse steering", type: "seg", options: [[0, "Camera"], [1, "Horse"]] },
    ] },
  ] },
  { id: "gamepad", icon: "gamepad", label: "Gamepad", groups: [
    { title: "Sticks", items: [
      { key: "controller.fScalarSensitivity", label: "Look sensitivity", type: "slider", min: 0, max: 1, step: 0.01, fmt: pct },
      { key: "controller.fLeftStickDeadzone", label: "Left stick dead zone", type: "slider", min: 0, max: 0.5, step: 0.01, fmt: pct },
      { key: "controller.fRightStickDeadzone", label: "Right stick dead zone", type: "slider", min: 0, max: 0.5, step: 0.01, fmt: pct },
      { key: "controller.bInvertedY", label: "Invert vertical look", type: "toggle" },
      { key: "controller.bSwapCamX", label: "Invert horizontal look", type: "toggle" },
      { key: "controller.bAutoCenter", label: "Camera auto-center", type: "toggle" },
      { key: "controller.bSouthPaw", label: "Southpaw", type: "toggle" },
    ] },
    { title: "Controller", items: [
      { key: "controller.bRumble", label: "Vibration", type: "toggle" },
      { key: "controller.bSwapButtons", label: "Swap buttons", type: "toggle" },
      { key: "controller.iCombatMode", label: "Targeting", type: "seg", options: [[0, "Casual"], [1, "Normal"], [2, "Expert"]] },
      { key: "controller.iHorseCameraRelativity", label: "Horse steering", type: "seg", options: [[0, "Camera"], [1, "Horse"]] },
    ] },
  ] },
  { id: "interface", icon: "gameplay", label: "Interface", groups: [
    { title: "HUD", items: [
      { key: "display.bRadarOn", label: "Radar", type: "toggle" },
      { key: "display.bRouteOn", label: "GPS route", type: "toggle" },
      { key: "display.Subtitles", label: "Subtitles", type: "seg", options: [[0, "Off"], [1, "On"]] },
      { key: "display.fSubtitleScale", label: "Subtitle size", type: "slider", min: 0.5, max: 1.5, step: 0.05, fmt: pct },
      { key: "display.fObjectiveScale", label: "Objective text size", type: "slider", min: 0.5, max: 1.5, step: 0.05, fmt: pct },
      { key: "display.fHelpScale", label: "Help text size", type: "slider", min: 0.5, max: 1.5, step: 0.05, fmt: pct },
    ] },
  ] },
];

const GRAPHICS_KEYS = new Set(["Fullscreen", "Monitor", "ResolutionX", "ResolutionY", "RefreshRate", "FrameRateLimit", "Vsync", "TripleBuffer",
  "DynamicResolution", "DynamicResolutionTargetFramerate", "DynamicResolutionMinScaleIndex", "MinAnisotropicFiltering", "MotionBlurStyle",
  "MotionBlurStrength", "ShadowQuality", "ShadowSoftness", "ShadowBlend", "WorldStreamingRadius", "TerrainStreamingFactor", "TreeLevelOfDetail",
  "TreeImposterHighLodStreamingDistance", "GrassStreamingDistance", "bFocusPaused", "bConstrainMouse", "HDR", "PeakBrightness", "PaperWhite",
  "aaAntiAliasing", "FSR3UpscalingQuality", "FSR3AdditionalSharpness", "NVIDIAReflex", "DLSSUpscalingQuality", "DLSSFrameGeneration"]);

let saved = {};        // the values as they are (game + multiplayer)
let draft = {};        // what the page shows
let gameAvailable = false;
let modes = { resolutions: [], refreshRates: [] };
let currentSection = "multiplayer";
let keepTimer = 0, keepPrevious = null;

const close = (a, b) => (typeof a === "number" && typeof b === "number") ? Math.abs(a - b) < 1e-4 : a === b;
const changedKeys = () => Object.keys(draft).filter((k) => !close(draft[k], saved[k]));
const sectionOfKey = (key) => SECTIONS.find((s) => s.groups.some((g) => g.items.some((i) => i.key === key || (i.key === "@resolution" && (key === "ResolutionX" || key === "ResolutionY")))));

function renderSubnav() {
  const nav = $("#subnav");
  nav.textContent = "";
  const dirty = changedKeys();
  for (const s of SECTIONS) {
    const b = el("button", s.id === currentSection ? "active" : "");
    b.appendChild(icon(s.icon));
    b.appendChild(el("span", null, s.label));
    const n = dirty.filter((k) => sectionOfKey(k) === s).length;
    if (n) b.appendChild(el("span", "badge", String(n)));
    b.addEventListener("click", () => { currentSection = s.id; renderSettings(); });
    nav.appendChild(b);
  }
}

function setDraft(key, value) {
  draft[key] = value;
  renderSubnav();
  updateApplyBar();
  const row = document.querySelector(`[data-key="${CSS.escape(key)}"]`);
  if (row) row.classList.toggle("changed", !close(draft[key], saved[key]));
}

function control(item) {
  const v = draft[item.key];
  if (item.type === "toggle") {
    const c = el("input", "toggle");
    c.type = "checkbox";
    c.checked = !!v;
    c.addEventListener("change", () => setDraft(item.key, c.checked));
    return c;
  }
  if (item.type === "slider") {
    const wrap = el("div", "slider");
    const r = el("input");
    r.type = "range"; r.min = item.min; r.max = item.max; r.step = item.step;
    r.value = typeof v === "number" ? v : item.min;
    const out = el("output", null, item.fmt(Number(r.value)));
    r.addEventListener("input", () => { out.textContent = item.fmt(Number(r.value)); setDraft(item.key, Number(r.value)); });
    wrap.append(r, out);
    return wrap;
  }
  if (item.type === "seg") {
    const seg = el("div", "seg");
    const known = item.options.some(([val]) => val === v);
    const options = known || typeof v !== "number" ? item.options : [...item.options, [v, "Custom (" + v + ")"]];
    for (const [val, label] of options) {
      const b = el("button", val === v ? "on" : "", label);
      b.addEventListener("click", () => { setDraft(item.key, val); $$("button", seg).forEach((x) => x.classList.toggle("on", x === b)); });
      seg.appendChild(b);
    }
    return seg;
  }
  if (item.type === "text") {
    const t = el("input");
    t.type = "text"; t.maxLength = item.max || 64; t.spellcheck = false; t.value = v || "";
    t.addEventListener("input", () => setDraft(item.key, t.value));
    return t;
  }
  if (item.type === "resolution") {
    const s = el("select");
    const cur = draft.ResolutionX + "x" + draft.ResolutionY;
    const list = modes.resolutions.map(([w, h]) => w + "x" + h);
    if (!list.includes(cur)) list.unshift(cur);
    for (const r of list) { const o = el("option", null, r.replace("x", " × ")); o.value = r; o.selected = r === cur; s.appendChild(o); }
    s.addEventListener("change", () => { const [w, h] = s.value.split("x").map(Number); setDraft("ResolutionX", w); setDraft("ResolutionY", h); });
    return s;
  }
  if (item.type === "refresh") {
    const s = el("select");
    const cur = Math.round(draft.RefreshRate || 60);
    const list = modes.refreshRates.length ? [...modes.refreshRates] : [cur];
    if (!list.includes(cur)) list.unshift(cur);
    for (const r of list) { const o = el("option", null, r + " Hz"); o.value = r; o.selected = r === cur; s.appendChild(o); }
    s.addEventListener("change", () => setDraft("RefreshRate", Number(s.value)));
    return s;
  }
  return el("span");
}

function renderSettings() {
  renderSubnav();
  const panel = $("#panel");
  panel.textContent = "";
  const section = SECTIONS.find((s) => s.id === currentSection);
  if (section.id !== "multiplayer" && !gameAvailable) {
    panel.appendChild(el("div", "unavailable", "The game's settings are not available yet. They appear once the game has finished loading."));
    return;
  }
  for (const g of section.groups) {
    const group = el("div", "group");
    group.appendChild(el("h3", "group-title", g.title));
    if (g.note) group.appendChild(el("p", "group-note", g.note));
    const rows = el("div", "rows");
    for (const item of g.items) {
      const row = el(item.type === "toggle" ? "label" : "div", "row"); // a label clicks its first control
      row.dataset.key = item.key === "@resolution" ? "ResolutionX" : item.key;
      if (item.key !== "@resolution" && !close(draft[item.key], saved[item.key])) row.classList.add("changed");
      const lab = el("span", "row-label", item.label);
      if (item.hint) lab.appendChild(el("small", null, item.hint));
      const ctl = el("span", "row-ctl");
      ctl.appendChild(control(item));
      row.append(lab, ctl);
      rows.appendChild(row);
    }
    group.appendChild(rows);
    panel.appendChild(group);
  }
}

function updateApplyBar() {
  const n = changedKeys().length;
  $("#applybar").classList.toggle("dirty", n > 0);
  $("#dirty-text").textContent = n ? `${n} unsaved change${n === 1 ? "" : "s"}` : "No changes";
  $("#apply").disabled = !n;
  $("#revert").disabled = !n;
}

function applySettings() {
  const dirty = changedKeys();
  if (!dirty.length) return;
  if (dirty.some((k) => k.startsWith("mp."))) {
    send("settings", { nickname: (draft["mp.nickname"] || "").trim(), nametags: !!draft["mp.nametags"],
      nametagDistance: Number(draft["mp.nametagDistance"]), chat: !!draft["mp.chat"] });
  }
  const game = {};
  for (const k of dirty) if (!k.startsWith("mp.")) game[k] = draft[k];
  if (Object.keys(game).length) {
    // display / graphics changes: offer to go back, like the game's own menu
    if (Object.keys(game).some((k) => GRAPHICS_KEYS.has(k))) {
      keepPrevious = {};
      for (const k of Object.keys(game)) if (GRAPHICS_KEYS.has(k)) keepPrevious[k] = saved[k];
      startKeepCountdown();
    }
    send("gameSettings", { values: game });
  }
}
$("#apply").addEventListener("click", applySettings);
$("#revert").addEventListener("click", () => { draft = { ...saved }; renderSettings(); updateApplyBar(); });

function startKeepCountdown() {
  let left = 15;
  $("#keep-seconds").textContent = left;
  $("#keep-modal").hidden = false;
  clearInterval(keepTimer);
  keepTimer = setInterval(() => {
    left -= 1;
    $("#keep-seconds").textContent = left;
    if (left <= 0) revertDisplay();
  }, 1000);
}
function endKeep() { clearInterval(keepTimer); $("#keep-modal").hidden = true; }
function revertDisplay() {
  endKeep();
  if (keepPrevious) send("gameSettings", { values: keepPrevious });
  keepPrevious = null;
}
$("#keep-ok").addEventListener("click", () => { endKeep(); keepPrevious = null; });
$("#keep-revert").addEventListener("click", revertDisplay);

// =====================================================================================================
// Messages from the client
// =====================================================================================================
window.rdmp = {
  receive({ type, data }) {
    if (type === "settings") {
      const mp = { "mp.nickname": data.nickname || "", "mp.nametags": !!data.nametags, "mp.nametagDistance": data.nametagDistance || 60, "mp.chat": !!data.chat };
      Object.assign(saved, mp);
      Object.assign(draft, mp);
      $("#who-name").textContent = data.nickname || "player";
      renderSettings();
      updateApplyBar();
    } else if (type === "gameSettings") {
      gameAvailable = !!data.available;
      if (data.modes) modes = data.modes;
      if (data.values) {
        for (const [k, v] of Object.entries(data.values)) { saved[k] = v; draft[k] = v; }
      }
      renderSettings();
      updateApplyBar();
    } else if (type === "version") {
      $("#version").textContent = data.version ? "Version " + data.version : "";
    } else if (type === "lists") {
      favorites = data.favorites || [];
      recentEntries = data.recent || [];
      render();
    } else if (type === "server") {
      const list = lists[data.list];
      if (!list) return;
      const key = keyOf(data.host, data.port);
      list.set(key, { ...data, name: data.name || (list.get(key) || {}).name || "" });
      if (data.list === currentList) render();
    } else if (type === "listError") {
      listErrors[data.list] = data.message;
      if (data.list === currentList) render();
    } else if (type === "status") {
      const kind = data.state === "error" ? "error" : data.state === "connecting" ? "busy" : data.state === "connected" || data.state === "ok" ? "ok" : "";
      toast(data.message, kind);
      if (data.state === "error" || data.state === "connected") send("refresh", { list: "recent" });
    }
  },
};

document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !$("#password-modal").hidden) $("#password-cancel").click();
});

renderSettings();
send("ready");
selectList("internet");
