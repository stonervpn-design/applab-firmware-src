# QR Code Generator App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A standalone "QR Code" main-menu app that generates + displays QR codes from Text/URL, WiFi, From File, and vCard. Generation-only. Reuses upstream's proven MIT encoder + render.

**Architecture:** Vendor `qrcode.{c,h}` (Richard Moore / bmatcuk MIT encoder) verbatim; adapt the scaled-module render; build a scene-based app (menu → per-source input → shared QR display view). Compiled-in `MENUEXTERNAL` app like `clock_app`.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper app framework (view_dispatcher, scene_manager, submenu, text_input, variable_item_list, dialogs, canvas). Firmware build only.

## Global Constraints

- **Build** (synchronous, never backgrounded, 600000 ms):
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = `Project build complete` + `=== BUILD OK ===`, EXIT=0. Don't delete `build_cardputer_adv/`. `build_*.log` gitignored.
- **Upstream reference (read-only):** `<scratchpad>/upstream-116/applications_user/flipperzero-qrcode/` — `qrcode.c`/`qrcode.h` (vendor VERBATIM, MIT — keep the license header) and `qrcode_app.c` (adapt the `render_callback` scaling/draw + the too-long handling). Do NOT copy `qrcode_app.c` wholesale (its file-load architecture differs from our menu design).
- **Encoder API:** `qrcode_getBufferSize(version)` → buffer size; `int8_t qrcode_initBytes(QRCode* qr, uint8_t* modules, int8_t mode, uint8_t version, uint8_t ecc, uint8_t* data, uint16_t len)` (mode `MODE_BYTE`, ecc `ECC_MEDIUM`); `bool qrcode_getModule(QRCode* qr, uint8_t x, uint8_t y)`; `qr->size` = module count. Auto-version: try increasing `version` (1..~10) until `qrcode_initBytes` succeeds AND the scaled render fits ≥1px/module in 128×64 with quiet zone; else "too long".
- **App type:** `MENUEXTERNAL` (main-menu), mirror `applications/main/clock_app/application.fam` (icon/order/fap_icon/fap_category). Sources auto-globbed (no explicit `sources=`).
- **Canvas:** 128×64 (`canvas_width`/`canvas_height`). Render black-on-white, integer pixel scale, centered, quiet-zone margin.
- **Templates to mirror:** `text_input` → `applications/main/archive/scenes/archive_scene_new_dir.c`; scene/app skeleton → `clock_app` or any `main/*` app with scenes; `dialog_file_browser` → grep a user of `dialog_file_browser_show`.
- Repo is git on `master`; commit per task. Leave untracked helper scripts alone.

---

### Task 1: App skeleton + encoder + QR display view + Text/URL flow

**Files:**
- Create: `applications/main/qrcode/application.fam`, `qrcode_app.c`, `qrcode_app_i.h`
- Create: `applications/main/qrcode/lib/qrcode.c`, `lib/qrcode.h` (vendored)
- Create: `applications/main/qrcode/views/qrcode_display.c`, `.h` (the QR render view)
- Create: `applications/main/qrcode/scenes/qrcode_scene_config.h`, `qrcode_scene_start.c`, `qrcode_scene_text.c`, `qrcode_scene_display.c` + `qrcode_scene.c`/`.h` (scene manager glue — mirror a sibling app's scene setup)
- Create: `applications/main/qrcode/icon.png` (copy upstream `icons/qrcode_10px.png` or a placeholder; icon asset)

- [ ] **Step 1: Vendor the encoder.** Copy `<scratchpad>/upstream-116/applications_user/flipperzero-qrcode/qrcode.c` and `qrcode.h` into `applications/main/qrcode/lib/` VERBATIM (keep the MIT license header). Confirm they compile standalone (pure C, no Flipper deps).

- [ ] **Step 2: application.fam** mirroring `clock_app`: `appid="qrcode"`, `name="QR Code"`, `apptype=FlipperAppType.MENUEXTERNAL`, `entry_point="qrcode_app"`, `stack_size=4*1024`, an `icon`/`fap_icon`, `order` (pick an unused value near other Tools), `fap_category="Tools"`, `requires=["gui","dialogs"]`.

- [ ] **Step 3: App skeleton** (`qrcode_app.c` + `qrcode_app_i.h`): a struct with `Gui*`, `ViewDispatcher*`, `SceneManager*`, `Submenu*`, `TextInput*`, `QRCodeDisplay*` (the view), and a `char payload[256]` + `char input_buf[256]` buffers. Alloc/free all; register views under an enum (`QRCodeViewSubmenu`, `QRCodeViewTextInput`, `QRCodeViewDisplay`). `entry_point` `qrcode_app(void*)` → alloc, run scene manager at `QRCodeSceneStart`, free. Mirror a sibling scened app's alloc/run/free.

- [ ] **Step 4: QR display view** (`views/qrcode_display.c`): a custom `View` holding the payload string. Its draw callback: `qrcode_initBytes` with auto-version (loop versions until it fits), compute `pixel_size = min((W - 2*margin)/size, (H - 2*margin)/size)`; if `pixel_size < 1` OR encode fails at max version → draw centered "Too long for screen QR"; else center the `size*pixel_size` block and for each module `qrcode_getModule(qr,x,y)` draw a filled box `pixel_size×pixel_size` (adapt from upstream `render_callback` lines ~155-235; drop the ECC/version on-screen selectors — just render). A setter `qrcode_display_set_payload(view, const char*)`. Free the modules buffer appropriately (alloc in draw or on set; keep it simple — alloc a max-version buffer once in the view).

- [ ] **Step 5: Scenes.** `qrcode_scene_config.h`: `ADD_SCENE(qrcode, start, Start)`, `ADD_SCENE(qrcode, text, Text)`, `ADD_SCENE(qrcode, display, Display)`. `qrcode_scene_start.c`: submenu with `Text / URL`, `WiFi`, `From File`, `Contact (vCard)` (only Text wired this task — the others added in Tasks 2-3; add them as items that are inert/return for now OR omit until their task — prefer adding all four items but routing only Text, with the others showing a "coming soon" placeholder scene state; simplest: add only the Text item this task and the rest in their tasks). `qrcode_scene_text.c`: `text_input_set_header_text("Enter text/URL")`, `text_input_set_result_callback(..., app->input_buf, sizeof, clear_default=true)` → on result copy to `app->payload`, `scene_manager_next_scene(QRCodeSceneDisplay)` (mirror `archive_scene_new_dir.c`). `qrcode_scene_display.c`: `qrcode_display_set_payload(app->qrcode_display, app->payload)`, switch to `QRCodeViewDisplay`; Back → previous scene.

- [ ] **Step 6: Build** → `build_qr1.log`, expect `=== BUILD OK ===`, EXIT=0. Fix scene/view glue + encoder-integration errors by reading sibling apps + `qrcode.h`. Verify the QR app appears as a `main` app (globbed).

- [ ] **Step 7: Commit**
```bash
git add applications/main/qrcode/
git commit -m "feat(qrcode): QR generator app — encoder + display view + Text/URL flow"
```

*After Task 1: the app opens, Text/URL → QR renders. WiFi/File/vCard added next.*

---

### Task 2: WiFi + vCard flows

**Files:**
- Create: `applications/main/qrcode/scenes/qrcode_scene_wifi.c`, `qrcode_scene_vcard.c`
- Modify: `qrcode_scene_config.h` (register), `qrcode_scene_start.c` (route the WiFi + vCard menu items)
- Modify: `qrcode_app_i.h` if extra buffers needed (ssid/pass/auth/name/phone/email)

- [ ] **Step 1: Register + route** the `WiFi` and `Contact (vCard)` menu items in start → their scenes.
- [ ] **Step 2: WiFi scene(s).** Collect SSID (`text_input`), Password (`text_input`), Auth (a `variable_item_list` or a 3-option submenu WPA/WEP/Open). Simplest: a `variable_item_list` with SSID/Password (opening text_input on select) + an Auth selector + a "Generate" row. On Generate: build `WIFI:S:<ssid>;T:<WPA|WEP|nopass>;P:<pass>;;` into `app->payload` with escaping of `\ ; , : "` (prefix with `\`), then `scene_manager_next_scene(QRCodeSceneDisplay)`. (If a multi-field VIL is complex, do sequential text_input scenes SSID→Password with a fixed WPA default + a note; keep it working.)
- [ ] **Step 3: vCard scene(s).** Collect Name, Phone, Email (text_input, sequentially or via a VIL). Build `BEGIN:VCARD\nVERSION:3.0\nN:<name>\nFN:<name>\nTEL:<phone>\nEMAIL:<email>\nEND:VCARD` into `app->payload` → display scene.
- [ ] **Step 4: Build** → `build_qr2.log`, `=== BUILD OK ===`.
- [ ] **Step 5: Commit** `git commit -m "feat(qrcode): WiFi-join + vCard QR flows"`

---

### Task 3: From File flow

**Files:**
- Create: `applications/main/qrcode/scenes/qrcode_scene_file.c`
- Modify: `qrcode_scene_config.h`, `qrcode_scene_start.c`

- [ ] **Step 1: Register + route** the `From File` item.
- [ ] **Step 2: File scene.** Use `DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS)` + `dialog_file_browser_show` (read a user of it for the exact `DialogsFileBrowserOptions` setup — base path `/ext`, no extension filter or `.txt`/`.qrcode`). On a chosen path, open the file via storage, read up to `sizeof(app->payload)-1` bytes (or the first line if the file is large) into `app->payload`; if the first line is `Message:<...>` (upstream .qrcode format) strip the prefix. Then `scene_manager_next_scene(QRCodeSceneDisplay)`. On cancel → back to start. `furi_record_close(RECORD_DIALOGS)`.
- [ ] **Step 3: Build** → `build_qr3.log`, `=== BUILD OK ===`.
- [ ] **Step 4: Commit** `git commit -m "feat(qrcode): generate QR from an SD file"`

---

### Task 4: Docs — test checklist + README note

**Files:**
- Create: `docs/QRCODE_TEST.md`
- Modify: `README.md`

- [ ] **Step 1: Checklist** `docs/QRCODE_TEST.md`: open QR Code app; Text/URL → type a URL → scannable QR (phone opens it); WiFi → SSID/pass/WPA → phone scan offers join; From File → pick a .txt → renders content; vCard → name/phone/email → phone scan adds contact; over-long text → "Too long for screen QR"; Back navigation clean. Mark "not yet hardware-validated — requires physical Cardputer ADV + a phone to scan."
- [ ] **Step 2: README note** under Tools/Apps: QR Code — generate QR from Text/URL, WiFi credentials, an SD file, or a contact vCard (display only; MIT encoder from flipperzero-qrcode).
- [ ] **Step 3: Commit** `git commit -m "docs: QR code app test checklist + README note"`

---

## Self-Review

**Spec coverage:** encoder+render+Text flow → Task 1; WiFi+vCard → Task 2; From File → Task 3; docs → Task 4. Generation-only, cap+too-long handled in the display view. ✅
**Placeholder scan:** encoder vendored verbatim (named files); render adapted from a named function with line refs; scenes modeled on named sibling templates. The WiFi-input "VIL vs sequential text_input" note gives a concrete working fallback, not a vague TODO.
**Type consistency:** `qrcode_initBytes`/`qrcode_getModule`/`QRCode.size`, `qrcode_display_set_payload`, scene names from ADD_SCENE, `app->payload` used across scenes.
**Ordering:** Task 1 (core) → 2 → 3 → 4. Task 1 is the shippable core (Text→QR works).
**Attribution:** qrcode.{c,h} MIT header kept; app/menu/flows new.
