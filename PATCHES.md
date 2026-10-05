# Patches on `patched`

This branch tracks [hyprwm/hyprlock](https://github.com/hyprwm/hyprlock) `main` with a small set of personal fixes on top.

| Commit | Summary |
|--------|---------|
| `fingerprint: restart verify on empty Enter` | Empty Enter re-claims/restarts fprintd verify so a wedged reader can be woken without a PAM failure flash. |
| `fingerprint: call restart via CAuth without removed getImpl` | Route empty-Enter through `CAuth::restartFingerprint` (upstream removed `getImpl`). |
| `fingerprint: USB-reset Validity via fingerprint-wake on empty Enter` | Empty Enter used to always USB-reset and wait 3s. A live reader stays up (second Enter within 1.2s soft-rearms). USB reset only if Claim fails, then hyprlock polls until the device is back. |
| `fingerprint: auth:fingerprint:max_attempts` | Configurable fail limit (default 4). Set `0` for unlimited retries. |
| `lock: do not GPU-screencopy for fade-in` | Fade used to force `zwlr_screencopy` even with a static background. That wedges the first lock frame on NVIDIA-render / Intel-scanout. |
| `lock: skip outputs via HYPRLOCK_SKIP_OUTPUTS` | Comma-separated `wl_output` names. Aura no longer sets this. HDMI stays on so the lock UI and keyboard exist on the HP too. |
| `lock: GBM EGL + wl_shm present` | `HYPRLOCK_GBM_DEVICE` (or Wayland-EGL fallback) renders to an FBO and publishes `wl_shm`. NVIDIA `wl_egl_window` swap is incomplete on Intel eDP. |
| `lock: scissor FBO Y origin` | Password/fail text uses `glScissor` in widget (top-left) space. Offscreen FBOs are bottom-left, so the field was clipping its own placeholder and `$FAIL`. |
| `fingerprint: show reject on retry` | `verify-retry-*` and unknown fprintd statuses set `$FPRINTFAIL` / enqueue PAM-style fail so a rejected scan is visible. |

## Updating from upstream

**Automated:** GitHub Actions rebases `patched` onto [hyprwm/hyprlock](https://github.com/hyprwm/hyprlock) `main` every Monday. If your patches conflict, the workflow fails and GitHub emails you (with default notification settings).

**Manual:**

```bash
git fetch upstream
git checkout patched
git rebase upstream/main
# fix conflicts if any, then:
git push --force-with-lease origin patched
```

Or from the PKGBUILD directory:

```bash
~/.local/share/pkgbuilds/hyprlock-patched/rebase-fork.sh
```

**Local rebuild** (by hand):

```bash
~/.local/share/pkgbuilds/hyprlock-patched/update.sh
```

The local `hyprlock-patched` PKGBUILD pulls this branch directly — no `.patch` files.
