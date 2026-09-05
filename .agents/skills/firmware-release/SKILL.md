---
name: firmware-release
description: Release a new OpenSprinkler firmware version (ESP32-C5 zigbee/matter + ESP8266). Use when asked to "release the firmware", "cut a firmware release", "bump firmware version", create/update the firmware CHANGELOG for a release, publish firmware OTA binaries to IONOS, tag a firmware version on GitHub, or update the documented version on opensprinklershop.github.io including regenerating multilingual screenshots via ScreenShotMachine and updating per-language manual pages. Covers the full flow: writing a thorough CHANGELOG entry from git history, running `./fw.sh release`, refreshing screenshots + multilingual docs, and publishing via mkdocs gh-deploy. DO NOT USE FOR the mobile/web app release (use app-release) or for the app changelog page at opensprinklershop.de/app.
---

# Firmware Release

Release workflow for the OpenSprinkler-Firmware repo (`/data/Workspace/OpenSprinkler-Firmware`).
The heavy lifting is done by `./fw.sh release`; your job is a **thorough, accurate CHANGELOG**
and the **docs version bump**.

## Version scheme
- `defines.h`: `OS_FW_VERSION` (e.g. `240` = `2.4.0`) and `OS_FW_MINOR` (build/patch, e.g. `225`).
- CHANGELOG section header format: `## [2.4.0(225)] — unveröffentlicht` (German).
- Release tag: `v${OS_FW_VERSION}_${OS_FW_MINOR}` (e.g. `v240_225`).
- The **current** `OS_FW_MINOR` in `defines.h` is the build you are releasing. After release,
  `fw.sh` auto-bumps it to the next dev build.

## Step 1 — Write the CHANGELOG (be thorough)
1. Read the top of `CHANGELOG.md` to find the last released build (e.g. `224`).
2. Find the commit that wrote that release's changelog:
   `git log --oneline -S "Update CHANGELOG.md for v..." ` or locate the `## [2.4.0(224)]` add commit.
3. List all commits since then: `git log <that-commit>..HEAD --format="%h %s"`.
   Commit bodies are usually terse — inspect `git show --stat <c>` per commit and, for detail,
   cross-reference `/memories/repo/*.md` (most fixes have a matching memory note with root cause).
4. Insert a **new** section at the very top of `CHANGELOG.md` (right after the `---` intro divider),
   marked `## [2.4.0(NNN)] — unveröffentlicht`, grouped `### Added` / `### Changed` / `### Fixed`
   (German prose). Keep the existing older sections below.
   - `extract_changelog_section()` in `fw.sh` reads the FIRST `## [` section, strips blank lines,
     and keeps `head -50` non-empty lines for the manifest/versions changelog — so put the release
     content in that first section and keep it reasonably tight.
   - The literal marker `unveröffentlicht` is REQUIRED: `update_changelog()` replaces it with
     `veröffentlicht <date>` during the release.

## Step 2 — Run the release
Preconditions: working tree clean except your `CHANGELOG.md` edit (fw.sh commits `defines.h` +
`CHANGELOG.md` itself), on `master`.

```bash
cd /data/Workspace/OpenSprinkler-Firmware
./fw.sh release 2>&1 | tee /tmp/fw_release_NNN.log
```

`./fw.sh release` does everything automatically:
1. Runs pre-release tests (`/data/Workspace/OpenSprinkler-Test/test_api.py`, `test_monitors.py`);
   aborts on failure (needs device .151 reachable).
2. Builds all 3 variants without debug flags: `esp8266`, `esp32-c5-zigbee`, `esp32-c5-matter`
   (stashes binaries around per-env cleans).
3. Archives the previous version, copies binaries to `/data/upgrade/`.
4. Updates `/data/upgrade/manifest.json` + `/data/upgrade/versions.json` (with SHA-256) using the
   extracted changelog section.
5. Promotes the current UI dev build into a versioned `ui-live/www/<ver>.<minor>` folder.
6. Marks CHANGELOG released (date), commits + tags (`v240_NNN`) + pushes, creates the GitHub release.
7. Online-deploys `/data/upgrade/` to IONOS (`/home/www/public/upgrade`).
8. Bumps `OS_FW_MINOR` to the next dev build and pushes.

Verify from the log tail: `Release complete!`, tag line, and grep for milestones:
`grep -nE "verification checks passed|manifest.json updated|versions.json|GitHub release|Promoting|Online deploy complete" /tmp/fw_release_NNN.log`

## Step 3 — Documentation update (opensprinklershop.github.io)
After a successful release, update the documented version, refresh screenshots if the UI changed,
update multilingual content for new/changed features, and publish.

Docs live at `/data/Workspace/OpenSprinkler-Firmware/docs` (MkDocs, theme `readthedocs`) and are
published to `opensprinklershop.github.io` via `gh-deploy`.
**Git nuance**: the `docs` directory is in `.gitignore` (so `git add docs/...` on a NEW file is
blocked — use `-f` only if you truly intend to track it), BUT a subset of pages is **already
tracked** (`docs/docs/index*.md` and some `opensprinklerpro_*.md`). Their version-bump edits show as
modified and **must be committed** — see Step 3e.

### 3a. Version string bump (all language variants)
The version string `2.4.0(NNN)` appears in ~22 files. Find and replace all:
```bash
grep -rlE "2\.4\.0\(22[0-9]\)" docs/docs   # then set them all to the new 2.4.0(NNN)
```

### 3b. Multilingual content (when a feature/API changed)
Doc pages are **per-language files**, English is the unsuffixed default. Coverage varies per page.
- Suffix convention (mind the mix): most pages use a **hyphen** (`index-de.md`, `zigbee-fr.md`,
  `pro-api-endpoints-it.md`, `analog-sensor-config-pt.md`, `troubleshooting-de.md`, `faq-fr.md`),
  but `opensprinklerpro` uses an **underscore** (`opensprinklerpro_de.md`). English = no suffix
  (`index.md`, `opensprinklerpro.md`).
- Languages in use: `de, en, fr, it, hu, pl, pt` (not every page has all 7 — e.g. `pro-api-endpoints`,
  `analog-sensor-config`, `troubleshooting`, `faq` currently have de/fr/it/pt + en; `index`, `zigbee`
  have de/fr/hu/it/pl/pt + en). List a page's variants with `ls <base>*.md`.
- When you document a new/changed feature, edit the **English** page first, then mirror the change
  into every existing language variant of that page. Do not add a language variant that didn't exist.
- Docs source location conventions (from copilot-instructions): API docs → `docs/as_api_docs`,
  reference/manual → `docs/docs`.

### 3c. Screenshots via ScreenShotMachine (when the UI changed visibly)
Playwright automation that captures mobile-size UI screenshots into the manual, per language.
- Tool: `/data/Workspace/ScreenShotMachine` (Playwright). Output:
  `docs/docs/assets/screenshots/pro/<lang>/<name>.png`. Docs embed them per language, e.g.
  `![...](assets/screenshots/pro/de/online-update.png){ .mobile-screenshot }`.
- Run (regenerate all 7 languages against a device UI):
  ```bash
  cd /data/Workspace/ScreenShotMachine
  npm install    # first run only
  LANGUAGES=de,en,fr,it,hu,pl,pt OS_BASE_URL=http://192.168.0.151 \
    OS_PASSWORD='<admin-pw>' npm run capture
  ```
  - **Credentials are secret**: pass `OS_PASSWORD` (or `OS_PASSWORD_HASH`) at runtime; the user types
    it in the terminal — never hardcode, print, or commit it. Do NOT request it via a prompt tool.
  - Capture a subset with `SHOTS=analog-sensor-editor,monitors` and a subset of langs via `LANGUAGES=`.
  - The automation is **read-only** (opens pages/dialogs only; never saves, reboots, changes ESP32
    mode, resets RainMaker, or starts a firmware update).
  - Logic-monitor shots (`monitor-and/or/xor/not/set-sensor12`) need a **test** controller pre-seeded
    with one monitor of every leaf type: `OS_BASE_URL=http://<test-ip> OS_PASSWORD='...' bash
    create_monitors.sh` (run only against a dedicated test device, never production).
- If you added a NEW screenshot: add a shot definition in `capture.js`, regenerate, then reference the
  new `assets/screenshots/pro/<lang>/<name>.png` in the English page and each language variant.

### 3d. Publish
```bash
cd docs && ./.venv/bin/mkdocs gh-deploy \
  --remote-name pages_origin --remote-branch main --force --ignore-version
```
`--ignore-version` is needed because a prior deploy recorded a different mkdocs version marker.
`pages_origin` = `https://github.com/opensprinklershop/opensprinklershop.github.io.git` (serves from `main`).
Verify the live version afterwards (GitHub Pages may take a minute).

### 3e. Commit the tracked doc changes (don't leave them dangling)
The TRACKED doc files (index*/some opensprinklerpro_*) modified in 3a–3c must be committed to the
firmware repo (mirrors prior releases, e.g. `Update firmware version to 2.4.0 (build NNN) in
documentation`). Use `git add -u` so gitignored/new files under `docs` are NOT force-added:
```bash
git add -u docs/docs
git commit -m "Update firmware version to 2.4.0 (build NNN) in documentation"
git push origin HEAD
```
Untracked doc variants (gitignored) stay local; only `gh-deploy` publishes them. Finish with a clean
`git status` (except intentional untracked items like `.agents/`).

## Release-doc invariants (from copilot-instructions)
Whenever `OS_FW_MINOR`/`OS_FW_VERSION` changes, these must be consistent (fw.sh handles the upgrade
files automatically): `CHANGELOG.md`, `/data/upgrade/manifest.json`, `/data/upgrade/versions.json`.
Archive URL pattern: `https://opensprinklershop.de/upgrade/archive/v{fw_version}_{fw_minor}/firmware_{zigbee|matter|esp8266}.bin`.

## Gotchas
- `docs/` is gitignored for NEW files, but a subset (`index*.md`, some `opensprinklerpro_*.md`) is
  already TRACKED — commit their version bump with `git add -u docs/docs` (Step 3e) so nothing is
  left dangling. The live github.io site is published via gh-deploy regardless.
- `fw.sh online-deploy` re-derives metadata from `defines.h`; after the post-release minor bump it
  would write next-dev-build metadata. To push manual metadata for the released build, sync via
  `cd /srv/www/htdocs/ui && ./fw.sh` instead.
- Pre-release tests require the test device (192.168.0.151) online; a failure aborts the release.
- Commit messages: English. CHANGELOG prose: German.
