# Isolated X11: Approach Comparison

All approaches replace host X11 passthrough with an isolated per-app
Xwayland, giving each app its own X server instead of sharing the host's
X11 socket.

## Option A: Host xwayland-satellite only

Flatpak finds xwayland-satellite on the host, spawns it in a sandbox,
binds the X socket into the app sandbox.

- **Pro**: Simple — no extension, no wrappers
- **Pro**: Works with every runtime
- **Pro**: Always matches host Wayland/GPU stack
- **Pro**: Host distro handles updates and security patches
- **Con**: New host dependency — hosts without xwayland-satellite fall back to host X11 passthrough
- **Con**: No rootful Xwayland path (some apps may need it)

## Option B: Runtime extension only

An extension (org.freedesktop.Platform.XWayland) provides
xwayland-satellite, Xwayland, and optionally xwayland-run. Runs in a
sandbox built from the app's runtime.

- **Pro**: Fully self-contained — no host dependencies
- **Pro**: xwayland-satellite runs sandboxed
- **Pro**: Can support rootful Xwayland via xwayland-run
- **Pro**: Versioned per runtime — reproducible
- **Con**: Extension must exist for each runtime version
- **Con**: Old runtimes without the extension get no isolated X11
- **Con**: More moving parts (extension build, wrappers, environment setup)
- **Con**: xwayland-run brings additional complexity (Python, xauth, window manager)

## Option C: Host xwayland-satellite with extension fallback

Try host xwayland-satellite first. If not available, try the extension.
Last resort: host X11 passthrough.

- **Pro**: Works everywhere something provides xwayland-satellite
- **Pro**: Graceful degradation across all host/runtime combinations
- **Con**: Two code paths to maintain
- **Con**: Extension still carries all its complexity for the fallback case
- **Con**: Behavior varies by host — harder to reproduce bugs
