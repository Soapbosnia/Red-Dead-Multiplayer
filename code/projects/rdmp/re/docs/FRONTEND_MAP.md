# RDO frontend map

Pseudo-code map of the Red Dead Redemption frontend as it actually runs on
`RDR.exe 1.0.42.49788435`, from process start to the point where the game would start.
Every entry below was either read out of the retail binary (Ghidra) or confirmed by a live
hook; the evidence column says which. Addresses are image-relative (image base
`0x140000000`).

The client implements exactly these calls — nothing is undiagnosed guesswork.

---

## 1. What runs from launch to the title

```
mainFrame()                                  FUN_1401F3140    game frame: intro, UI render, state
├─ introWrapper()                            FUN_1405F4390    legal + logo phases, per frame
│  └─ introController()                      FUN_1405F5070    state DAT_142ac5130
│     ├─ publish "LegalsCompleted"           movie variable   gate for the legal screen
│     ├─ phase timer +0x08 = 1001            intro phase machine
│     └─ logo reel: Bink frames (bink2w64)
├─ uiRender()                                FUN_1405C8720    gated by controller+0x3C0
│  └─ movieRender(manager, snapshot)         FUN_14011A8F0
└─ uiState = DAT_142b0f070                   current UI state object (null on this build)
```

`introController` never tears itself down after an early skip, so the client publishes
`LegalsCompleted` **once** and retires the reel phases itself
(`docs/INTRO_NOTES.md`, `intro.cpp`).

## 2. Frontend object model

```
uiController = *(DAT_142aac8b8)                      singleton, 0x2AAC8B8
├─ +0x0A0 : movie                                    the "UserInterface" GFx movie
├─ +0x175 : hide-ui flag
└─ vtable +0x390 : eventDispatch(controller, name, param, a, b, c, d)
                                                    FUN_1401EC230  (frontend event handler)

movie
├─ +0x008 / +0x010 : component registry              FUN_1400ef100  name -> widget
├─ +0x720 / +0x728 : localization table               FUN_1400fac70  hash -> string
└─ views: manager DAT_142C45610 -> +0x30 + index*8    index 28 = "startscreen"
              view: +0x14 playhead, +0xE8 advance
```

Pseudo-code of the two lookups the client uses:

```c
// FUN_1400ef100: find a UI widget by clip name (case-insensitive, custom hash)
widget *FindWidget(movie *m, const char *name) {
    uint32 h = 0;
    for (p = name; *p; ++p) {
        uint32 c = *p;
        if ((uint8)(c - 0x20) > 0x19) { /* keep */ } else c -= 0x20;   // ASCII fold
        h = c + h * 0x10;
        uint32 top = h & 0xF0000000;
        if (top) h ^= (top >> 0x18) ^ top;
    }
    for (node *n = m->buckets[h % m->count]; n; n = n->next)
        if (_stricmp(n->name, name) == 0) return n->value;
    return NULL;
}

// FUN_1400fac70 / FUN_1400fb020: resolve a localization key (mixed hash) to text
const char *LocalizedText(uint32 mixed) {
    const char *s = tableLookup(DAT_142c45608, mixed);   // critical-section guarded map
    if (s) return s;
    return tableLookup(DAT_142c45608, MixedHash("Common_Null"));
}

// FUN_1400c7510 + the frontend's mix: the key hash stored on every text widget
uint32 MixedHash(const char *name) {
    uint32 h = NameHash(name, 0);       // engine string hash
    uint32 v = h * 9;
    return ((v >> 11) ^ v) * 0x8001;
}
```

## 3. The text widget class (`vftable 0x141C61950`)

Every menu label, button and list row is an instance of this class. The client drives it
through these virtuals only:

```c
// +0x420  FUN_14010dc10   SetKeyByName(w, name)
void SetKeyByName(widget *w, const char *name) {
    if (engineStringsLoaded) LocalizedText(MixedHash(name));   // preload into the map
    w->dirty = 1;                       // +0x80
    w->key   = MixedHash(name);         // +0x88
    w->Invalidate();                    // +0x3D0
}

// +0x428  FUN_1400eb430   SetKeyHash(w, hash)
void SetKeyHash(widget *w, uint32 hash) {
    if (hash == w->key) return;
    w->dirty = 1; w->key = hash; w->Invalidate();
}

// +0x400  FUN_14010e220   GetText(w)          (resolves through LocalizedText)
const char *GetText(widget *w) {
    return w->dirty ? LocalizedText(w->key) : w->cachedText;
}

// +0x438  FUN_1400eb1f0   SetAlpha(w, a)      a is 0..100, 0 = never rendered
void SetAlpha(widget *w, float a) { w->alpha = a; w->fading = (a >= 0); }

// +0x3D0  FUN_140103ba0   Invalidate(w)       re-layout subtree
void Invalidate(widget *w) {
    w->needsLayout = 1;                              // +0x2A
    for (child *c = w->children; c; c = c->sibling)  // +0x30, sibling +0x18
        if (!(c->flags & 0x800)) Invalidate(c);      // flags at +0x28
}

// +0x1A0  FUN_140103b20   Update(w)           fade + draw
void Update(widget *w) {
    if (w->fading && (w->alpha -= dt) <= 0) w->Hide();   // +0x71, +0x74, +0x158
    w->Draw();                                            // +0x450
    w->PostDraw();                                        // +0x458
}

// +0x450  FUN_140103380   Draw(w)             no-op when w->renderCtx == NULL
void Draw(widget *w) {
    if (!w->renderCtx) return;                 // +0x68
    ... emit text via renderCtx->vtable(+0x130)->+0x58
}

// +0x158 / +0x160  FUN_1400fd1a0 / FUN_1400fd030   Hide / Show
```

Field summary: `+0x18` sibling · `+0x28` flags · `+0x2A` needs-layout · `+0x30` children ·
`+0x50` text style · `+0x68` render context · `+0x71` fading · `+0x74` alpha ·
`+0x80` text dirty · `+0x88` key hash / cached text.

### Creating a widget

```c
// FUN_14007fd70 -> allocator singleton, vtable +0x10 = alloc(bytes, align)
// FUN_14010d980 -> constructor(name = <vftable>), FUN_1400f7890 -> factory for the
// plain label class (0xA8 bytes); FUN_1400f78f0 -> a variant (0xB0 bytes).
widget *NewLabel(void) {
    alloc = GetAllocator();                  // FUN_14007fd70
    w = alloc->Alloc(0xA8, 0x10);
    ConstructLabel(w);                       // FUN_14010d980: vtable, alpha 0, key 0
    w->vtable = &label_vtable;               // derived class override
    return w;
}
```

## 4. Title strip → `startScreenExit` → start screen

```c
// widgets created at boot, each labelled through SetKeyByName:
//   Common_Play     x2  -> strip entry "Online"  (the Play prompt)
//   Common_Settings     -> strip entry "Settings" (Y)
//   Common_Quit         -> strip entry "Quit"
//   StartScreen_Normal / _MPNormal / _MPExpert / _Zombie -> the singleplayer list rows

// pressing ACCEPT (Enter) on the title raises, in this order:
eventDispatch(ui, "startScreenExit", 0)          // FUN_1401EC230 case -> tears down title
eventDispatch(ui, "fileCheckSignedIn", 0)
eventDispatch(ui, "fileSignedInPlayer", 0)
singlePlayerScreen()                             // FUN_140335700
```

```c
// FUN_140335700  singlePlayerScreen()  - pseudo-code from the decompilation
void singlePlayerScreen(void) {
    movie *m = ui->movie;
    widget *list = FindWidget(m, "StartScreen_SinglePlayer");
    widget *uiw  = FindWidget(m, "UserInterface");
    widget *back = FindWidget(m, "SS_SP_BackPrompt");
    (void)list; (void)uiw; (void)back;

    widget *rows = FindWidget(m, "StartScreen_Zombie");
    if (!rows && !FindWidget(m, "SinglePlayerList")) { /* keep waiting */ }

    select_fn select = *(select_fn *)((uint8_t *)ui + 0xC0);   // <-- indirect slot
    select(ui, "StartScreen_Zombie", 0);                       // select a row

    raise("@UI.ACCEPT*RELEASED");
}
```

On this build the `ui + 0xC0` slot is not initialised, so the indirect call jumps to a
garbage address: `RDR.exe+0x103B3` (unaligned, hence the reported offset) → `0xC0000005`.
That is the "Play freezes and closes" report, reproducible with the client's reroute
disabled.

## 5. The native message box

```
Escape on the title -> eventDispatch(ui, "UI_MessageBoxOnShow", 0)
box text is laid out by C++ (not by AS), so it renders correctly:
    title  "ALERT"
    body   "Are you sure you want to quit to desktop? All unsaved progress will be lost."
```

This is the only other screen that both renders and carries text on this build, which makes
it the fallback host for a list if a dedicated movie is not used.

## 6. What the client does, mapped to the above

| client action | engine call it uses | file |
|---|---|---|
| skip legal screen | publish `LegalsCompleted` once on the intro controller | `intro.cpp` |
| retire logo reel | IAT redirect of `bink2w64!BinkDoFrame`, skip draws 6 s | `intro.cpp` |
| own labels | insert `RDO_*` keys into the localization map | `rdr.cpp` |
| rename Play prompt | `SetKeyByName(playWidget, "RDO_Online")` at creation | `menu.cpp` |
| keep the title alive | swallow `startScreenExit` in the event proxy | `frontend.cpp` |
| Online menu | `SetKeyByName(settings,"RDO_Browse")`, `(quit,"RDO_Host")` | `menu.cpp` |
| wordmark | `SetKeyByName(logoWidget, "RDO_Title")` (`"Logo"` widget) | `menu.cpp` |

## 7. Known gaps (what the map says is missing)

1. **Wordmark**: the `"Logo"` text widget keeps the constructor's alpha `0`, and the baked
   `RED DEAD REDEMPTION` art is drawn by other widgets. Both are addressable by name; the
   art widget still has to be identified (see `docs/STATUS.md` round 44+).
2. **Browser list**: the `StartScreen_*` rows belong to a movie that is only loaded inside
   `singlePlayerScreen()`; the startscreen movie does not contain them at any playhead
   position (swept 0..240). A list needs either the message box host or a new movie.
3. **Settings page**: options pages are singleton objects bound to clips by name
   (`DisplayOption_*`, `AudioOption_*`, ...); the registry is built in `FUN_1401EE680`.
