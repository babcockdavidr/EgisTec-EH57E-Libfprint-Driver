#!/usr/bin/env bash
# One-command build + install of libfprint with the egis057e driver on Arch / CachyOS.
#
#   bash tools/install-arch.sh
#
# It: finds (or creates) your libfprint packaging folder, registers the driver in the source tree,
# VERIFIES the registration, forces a clean rebuild, installs the package (tests skipped), then checks that
# the installed library really contains the driver and asks fprintd to list the sensor.
# Safe to run repeatedly. Overridable: LIBFPRINT_BUILD_DIR (default ~/build/libfprint), LIB_PATH.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
BUILD_DIR="${LIBFPRINT_BUILD_DIR:-$HOME/build/libfprint}"
LIB_PATH="${LIB_PATH:-/usr/lib/libfprint-2.so.2}"

say()  { printf '\n== %s\n' "$*"; }
die()  { printf '\n!! %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "Run this as your normal user, not root (makepkg refuses root)."
[ -f "$PROJECT/driver/egis057e.c" ] || die "Cannot find $PROJECT/driver/egis057e.c - run this script from inside the project's tools/ folder."
for c in python3 makepkg sudo; do command -v "$c" >/dev/null || die "Missing command: $c"; done

say "1/6 libfprint packaging folder: $BUILD_DIR"
if [ ! -d "$BUILD_DIR" ]; then
    command -v pkgctl >/dev/null || die "Folder $BUILD_DIR not found and 'pkgctl' (package devtools) is not installed. Run: sudo pacman -S --needed devtools base-devel"
    mkdir -p "$(dirname "$BUILD_DIR")" || die "cannot create $(dirname "$BUILD_DIR")"
    (cd "$(dirname "$BUILD_DIR")" && pkgctl repo clone --protocol=https libfprint) || die "pkgctl clone failed"
fi
cd "$BUILD_DIR" || die "cannot enter $BUILD_DIR"

say "2/6 unpacked source"
if ! ls -d src/libfprint >/dev/null 2>&1; then
    makepkg -o || die "makepkg -o failed (if it asks for a PGP key, import it with the gpg --recv-keys command it prints, then rerun)"
fi
SRC="$BUILD_DIR/src/libfprint"
[ -d "$SRC/libfprint/drivers" ] || die "Unexpected source layout: $SRC/libfprint/drivers not found"

say "3/6 registering the driver in the source tree"
python3 "$HERE/add_to_libfprint_tree.py" "$SRC" || die "Driver registration FAILED (see the message above). Nothing was installed."

say "4/6 clean rebuild"
# Remove build leftovers AND any previously built package: otherwise makepkg says "a package has already
# been built" and silently reinstalls the old, driver-less one. -f forces a rebuild as a second safeguard.
rm -rf "$BUILD_DIR/src/build" "$BUILD_DIR/pkg"
rm -f "$BUILD_DIR"/*.pkg.tar.*
makepkg -e -f -si --nocheck || die "Build or install failed - copy the last 30 lines above and send them to me."

say "5/6 checking the installed library"
COUNT="$(grep -a -c egis057e "$LIB_PATH" 2>/dev/null | head -n1)"; COUNT="${COUNT:-0}"
if [ "${COUNT:-0}" -gt 0 ]; then
    echo "OK: the installed library contains the egis057e driver."
else
    die "The installed library does NOT contain the driver. Send me: pacman -Qi libfprint | head -5   and   ls -l $LIB_PATH*"
fi

say "6/6 restarting fprintd and listing devices"
sudo udevadm control --reload && sudo udevadm trigger
sudo systemctl restart fprintd
sleep 1
fprintd-list "$USER"
echo
echo "If a device is listed above, the next step is:  fprintd-enroll"
echo "If it says 'No devices available', run:  sudo systemctl stop fprintd; sudo env G_MESSAGES_DEBUG=all /usr/lib/fprintd -t"
echo "(then run fprintd-list in a second terminal) and send me the output."
