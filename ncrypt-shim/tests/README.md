# Display patch regression tests

These tests execute the compiled 32-bit shim's installer, generated thunks,
and coordinate helpers in Unicorn against the installed game's original PE
image. Only Win32 imports are mocked. They do not require a running game,
account, Wine, GPU, or redistributing either proprietary game binary.

```sh
cargo build --release
uv run --with unicorn==2.1.4 --with pefile==2024.8.26 \
  python ncrypt-shim/tests/test_display_thunks.py \
  --dll target/release/build/<current-build>/out/ncrypt32.dll \
  --game /path/to/BinaryTrU.exe -v
```

The seven tests cover the wheel continuation address, the mouse helper's
custom register and stack contract, signed coordinates, 1,000 alternating
resolution/mouse/wheel cycles, window size fallbacks, and refusing to overwrite
input code that no longer matches the supported game build. The v0.1.7 DLL
fails the wheel continuation and mouse register tests.

The game shim writes first-chance access violation/illegal instruction details
to `Z:\tmp\goley_display.log` (`/tmp/goley_display.log` with the usual Wine Z:
mapping). A logged exception is not necessarily an unhandled crash. Set
`GOLEY_DISPLAY_TRACE=1` on the launcher process before starting the game to
add D3D9 Reset parameters and HRESULTs. Normal launches do not install the
additional Reset tracing hook. Tracing does not change Reset's result or
recovery branches.

The emulated tests do not establish DXVK, compositor, or server behavior; those
require live testing. See [the investigation](../DISPLAY_CRASH.md) for the
reproduced crash and live validation.
