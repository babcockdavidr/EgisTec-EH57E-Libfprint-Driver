# Security notes and recommendations

## What this driver does and does not decide
libfprint matches **one image per verify operation** and the match decision lives in libfprint (NBIS
minutiae + bozorth3 score against a threshold). A sensor driver cannot make libfprint vote over several
images inside one verify. So the security design is split:

| layer | what it does here |
|---|---|
| driver (`egis057e_image.h`) | submits a frame only after a *settled press*: finger present for several
  consecutive frames, >= 70% of the window showing ridges, consecutive frames correlating >= 0.8, and every
  frame different from the last (a replayed/stuck buffer is rejected and ends the session). The best frame of
  the settled run is submitted. Brushes, sliding fingers and half touches never reach the matcher. |
| libfprint | extracts minutiae, compares to the enrolled prints, applies `bz3_threshold` (kept at 40). |
| *your policy layer (not written yet)* | N-of-M voting, lockouts, which services may use fingerprints. |

## Your "scan a few times, require X of Y within half a second" idea
Good idea, and it belongs one layer up. Design sketch:
1. A small PAM module/helper (or a patch to `pam_fprintd`) calls fprintd `Verify` up to Y times back to back.
2. The finger stays down; each new verify re-activates the sensor, the tracker re-detects the finger and
   submits a fresh settled frame (about 2 frames to detect + 3 to settle + sensor re-init).
3. Unlock only if at least X of the Y verifies match inside the time window; otherwise fall back to password.
Not measured yet: each verify's real latency. If the sensor delivers 20+ frames/s, two or three verifies fit in
about a second; fitting them in 0.5 s may need `EGIS057E_STABLE_FRAMES` lowered to 2 for that mode.

## Frame rate
The Python tools were slow on purpose (~1 frame/s). The driver scans back to back while a finger is on the
sensor, and every 40 ms (25 Hz) while idle (`EGIS057E_IDLE_INTERVAL_MS`, set 0 for the maximum). The real
ceiling is the sensor's exposure time, which has not been measured; `G_MESSAGES_DEBUG=all` prints frames/s.
Continuous maximum-rate scanning keeps the sensor and CPU busy for as long as the lock screen is shown, which is
why the idle rate is throttled by default.

## Recommendations, most important first
1. **Treat it as convenience, not strong authentication.** There is no liveness detection. Latent prints on
   a power-button sensor and moulded fakes are realistic attacks. Wipe the sensor sometimes.
2. **Keep `bz3_threshold` at 40 or higher.** A 70x57 window holds few minutiae. Lowering the threshold to make
   enrollment "work" trades directly away security. Measure before changing it: enroll one finger, then try
   many verifies with *other* fingers (family, with their consent) and count false accepts.
3. **Enroll more than once / more stages.** 8 stages are configured. More coverage of the fingertip improves
   both false rejects and false accepts.
4. **Limit what a fingerprint can unlock.** Lock screen: reasonable. `sudo`, polkit and anything that can
   change the system: use the password or require the fingerprint *plus* something else.
5. **Never use it for first login after boot or disk unlock.** The password is needed to unlock the key ring and
   encrypted disk anyway.
6. **Cap attempts.** Use `pam_fprintd` options (`max-tries=3`) so repeated failures fall back to the password.
7. **Fail closed.** Any driver or USB error produces "no match", never a match.
8. **Protect the templates.** fprintd stores minutiae data root-only under `/var/lib/fprint`; keep the disk
   encrypted. Never publish captured frames, `.bin` capture files or screenshots of ridges: they are your
   fingerprint.
9. **Keep logs free of biometrics.** The driver logs only numbers (contrast, coverage); keep it that way.
10. **Review changes to `egis057e_image.h` carefully.** It is the quality/replay gate; weakening a threshold
   weakens security. The self-test (`tests/frame_selftest.c`) should keep passing.

## Known limitations
- No auto-calibration: gain/threshold bytes in the capture block are fixed to values that worked on one unit.
  Very dry/wet fingers may need tuning (see `EGIS057E_CAPTURE_BLOCK`).
- The scan resolution (`EGIS057E_PPMM`) is an estimate; matching quality depends on it.
- Not tested on other units, firmware versions or `bcdDevice` values (the Windows driver branches on version:
  e.g. for versions < 3 the 0x64 length is counted in 512-byte units).
