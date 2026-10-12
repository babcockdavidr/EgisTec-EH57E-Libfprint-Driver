# libfprint driver for the EgisTec EH57E (USB 1c7a:057e)

Found in the power-button fingerprint reader of Samsung Galaxy Book models. Status: **written against
hardware observations, compiles cleanly against libfprint 1.95.2, not yet run inside libfprint on real
hardware.** Expect to iterate; the debug output is there for that.

## Files
| file | purpose |
|---|---|
| `driver/egis057e.c` | the libfprint driver: USB exchange, capture state machine, libfprint glue |
| `driver/egis057e.h` | protocol constants, each tagged CONFIRMED / DECODED / GUESS |
| `driver/egis057e_image.h` | frame analysis + finger tracker (no libfprint dependency, unit tested) |
| `tests/frame_selftest.c` | self-test for the tracker; `--frames FILE` replays real captures |
| `tools/add_to_libfprint_tree.py` | copies the driver into a libfprint source tree and registers it |
| `tools/compare_with_reference.py` | measures overlap with other libfprint drivers |
| `docs/PROTOCOL.md`, `docs/SECURITY.md`, `docs/CLEAN_ROOM.md` | protocol, security design, originality |
| `patches/0001-add-egis057e-driver.patch` | the same registration as a patch against libfprint 1.95.2 |

## Self-test (no hardware needed)
    cd tests && cc -O2 -I../driver -o frame_selftest frame_selftest.c -lm && ./frame_selftest

## Build on Arch / CachyOS
Package-managed, so it never fights pacman:

    pkgctl repo clone --protocol=https libfprint && cd libfprint     # Arch's PKGBUILD
    # unpack the source (makepkg -o), then register the driver in the unpacked tree:
    makepkg -o
    python3 /path/to/egis057e/tools/add_to_libfprint_tree.py src/libfprint*/
    # build and install from the modified tree
    makepkg -e -si         # -e = use the already-extracted (modified) sources

If `makepkg -e` is unfamiliar: the alternative is to add `egis057e.patch`-style steps to `prepare()` in the
PKGBUILD, calling `add_to_libfprint_tree.py` on `$srcdir/libfprint-*`.

After installing:

    sudo udevadm control --reload && sudo udevadm trigger     # rules are generated from the driver's id table
    sudo systemctl restart fprintd
    fprintd-list $USER                                        # should now list the device
    fprintd-enroll                                            # 8 presses
    fprintd-verify

## Debugging (what to send when something is off)
    sudo systemctl stop fprintd
    sudo G_MESSAGES_DEBUG=all /usr/lib/fprintd -t 2>&1 | tee fprintd-debug.txt
(then run `fprintd-enroll` from another terminal). Lines tagged `egis057e` show finger on/off, submitted frames
with their contrast/coverage, and frames per second. Close any other program holding the sensor first (the
Python probe tools in this project claim the same interface).

## Privacy
Captured frames and `.bin` files contain your fingerprint. Do not commit or publish them. The self-test uses
synthetic data only.

## Adapting to a similar sensor
Start with `egis057e.h` (VID/PID, endpoints, image size, register block) and the "AI NOTES" block at the end
of `egis057e.c`, which records how this protocol was found and which detours to avoid.

## After installing
- A normal `pacman -Syu` will replace your patched libfprint with the stock one (which lacks this driver).
  Either add `IgnorePkg = libfprint` to `/etc/pacman.conf`, or rebuild after libfprint updates.
- If the sensor is not listed by `fprintd-list`, check `lsusb | grep 1c7a` and `journalctl -u fprintd -b`.
- Known unknowns when written (see docs/SECURITY.md, docs/PROTOCOL.md): real frames/s, auto-calibration,
  behaviour on other units, and N-of-M voting (needs a policy layer above libfprint).
