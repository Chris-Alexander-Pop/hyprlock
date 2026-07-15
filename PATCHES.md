# Patches on `patched`

This branch tracks [hyprwm/hyprlock](https://github.com/hyprwm/hyprlock) `main` with a small set of personal fixes on top.

| Commit | Summary |
|--------|---------|
| `fingerprint: restart verify on empty Enter` | Empty Enter re-claims/restarts fprintd verify so a wedged reader can be woken without a PAM failure flash. |
| `fingerprint: call restart via CAuth without removed getImpl` | Route empty-Enter through `CAuth::restartFingerprint` (upstream removed `getImpl`). |
| `fingerprint: USB-reset Validity via fingerprint-wake on empty Enter` | Empty Enter starts `fingerprint-wake.service` (usbreset 06cb:009a + restart python3-validity/open-fprintd), then re-claims after 3s. |

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
