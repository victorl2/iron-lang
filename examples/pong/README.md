# Iron Pong

Phase 12 reference game for the Iron WASM worker.

## Build

```sh
# Native
iron build examples/pong/pong.iron
./pong

# Web
iron build --target=web examples/pong/pong.iron
# -> dist/web/index.html
iron run --target=web examples/pong/pong.iron
# -> opens in browser via emrun
```

## Controls

- **Left paddle:** W (up) / S (down)
- **Right paddle:** Up Arrow / Down Arrow
- **Start / Restart:** Space

First player to 5 wins.

## Phase 12 coverage

This game exercises:
- LIR main-loop split pass (Phase 5) with ≥4 captured locals
- `emit_web.c` frame-callback emission (Phase 6)
- `--target=web` pipeline (Phase 7)
- Raylib web amalgamation link (Phase 8)
- Default shell template with COOP/COEP guard (Phase 9)
- `[web].assets` preload directive (Phase 10)
- `dist/web/` output layout (Phase 11)

## Known limitations in this phase

- **No audio** — raylib.iron does not yet expose `InitAudioDevice`/`LoadSound`/`PlaySound` bindings. The `assets/paddle.wav` file is present per WEB-VALIDATE-03 but not played. Audio bindings deferred to a stdlib-extension follow-up.
- **Web interactivity** — WEB-VALIDATE-02 (in-browser play verification), WEB-VALIDATE-07 (first-hit audio), and WEB-VALIDATE-08 (page-reload memory stability) require a real browser session and are marked as manual/deferred.
