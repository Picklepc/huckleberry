# Huckleberry Release / Milestone Checklist

Reviewed and updated for **every major release / milestone** (a batch of related
features). Copy this list into the release PR/notes and check each item. The goal
is that every major step is validated before it's considered done.

## Last release validation
`v0.2.0` was built, USB flashed, OTA flashed, and verified on trailer Wi-Fi on
2026-07-25. See `docs/RELEASE_0.2.0.md`.

## 1. Code validation
- [ ] Builds clean: `pio run -e huckleberry` with no errors (drive warnings toward zero).
- [ ] Static sanity: no obvious UB, no blocking calls on the UI loop, BLE/web work off the UI task.
- [ ] Flash usage within budget (app fits the 3 MB slot with headroom for OTA).
- [ ] Secrets: `include/secrets.local.h` NOT staged; no keys/passwords/MACs in tracked source.

## 2. Compaction
- [ ] Consolidate duplicated logic into shared helpers/modules.
- [ ] Keep modules focused (net / ble / web / settings / ui / thermostat).
- [ ] Trim oversized functions; extract embedded HTML/assets sensibly.

## 3. Remove abandoned code / features
- [ ] Delete dead code, unused settings keys, and commented-out blocks.
- [ ] Remove half-built features that didn't make the milestone (or gate them clearly).
- [ ] Prune stale comments and TODOs that are done.

## 4. Testing (validate each feature in the batch, on hardware)
- [ ] Boot: clean serial boot, no panic/reset loop.
- [ ] Display: clock renders; day/night switches correctly; night is home-page only; 1-min timeout returns home.
- [ ] Touch/nav: tap→thermostat; swipe through Climate/Power/Status.
- [ ] Wi-Fi: joins a known STA; `Huckleberry` AP reachable; captive portal works off-grid.
- [ ] Web: dashboard loads; live values update; settings persist across reboot.
- [ ] BLE: battery SOC/V/A live; Victron watts/state live; reconnects after dropout.
- [ ] Time: NTP on STA; browser-push off-grid; survives Wi-Fi loss.
- [ ] Feature-of-the-milestone: exercised end-to-end and observed working.

## 5. Docs & housekeeping
- [ ] `CHANGELOG.md` updated; version string bumped in firmware + web footer.
- [ ] `docs/ROADMAP.md` milestone marked done; next milestone refined.
- [ ] Task list updated (completed/added).

## 6. Ship
- [ ] Commit with clear history; tag `vX.Y.Z`.
- [ ] Push to GitHub; write release notes.
- [ ] (If OTA enabled) publish the merged/OTA binary.
