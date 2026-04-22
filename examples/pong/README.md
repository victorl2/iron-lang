# Iron Pong

Small two-player Pong example for Iron + raylib.

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

## What It Shows

This game exercises:
- A complete game loop with update and render stages
- Keyboard input, collision checks, and score tracking
- The same source building for native and web targets
- Optional bounce audio when the sound asset is available

## Notes

- Run from the repo root so the example can find `tests/assets/bounce.wav`.
