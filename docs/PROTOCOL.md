# EgisTec EH57E (USB 1c7a:057e) - protocol notes

Evidence levels: **CONFIRMED** = observed on real hardware from Linux; **DECODED** = read from the
vendor's Windows driver, not independently tested; **GUESS** = informed guess.

## Device
- USB 2.0 high speed, vendor-specific class, one interface, `bcdDevice` 0x1201 on the test unit. CONFIRMED
- Endpoints: bulk OUT `0x01` (commands), bulk IN `0x82` (replies, image data), interrupt IN `0x83`/`0x84`
  (16 bytes; never seen to carry anything, unused). CONFIRMED
- It is an image sensor with host-side matching (Windows uses a separate matching engine DLL). DECODED
- Image: 70 x 57 px, 8 bit, 3990 bytes. CONFIRMED. Ridges are bright in the raw data. CONFIRMED
- Resolution is not documented anywhere; median ridge period 6.8 px implies roughly 12-16 px/mm. GUESS

## Framing  (CONFIRMED)
Request (host to sensor, bulk OUT): `45 47 49 53` ("EGIS"), opcode, arg1, arg2 [, payload].
Reply (bulk IN, 7 bytes): `53 49 47 45` ("SIGE"), b4, b5, status. Status `01` = OK.

| opcode | arg1 / arg2 | meaning | reply |
|---|---|---|---|
| 0x60 | reg / 0 | read register | b4 = reg, b5 = value |
| 0x61 | reg / value | write register | echoes reg and value |
| 0x63 | first reg / count, then `count` payload bytes | write register block | none seen |
| 0x64 | length hi / length lo | read image | raw bytes only, no "SIGE" header |

Other opcodes seen in the Windows driver but NOT used here (DECODED, untested unless stated):
0x03/0x05 (read a byte/flag; both answer on hardware, meaning unknown), 0x04, 0x62 (read N regs),
0x67/0x69 (on/off style pair), 0x73 (set 16-bit value), 0x80/0x81 (block writes), 0x90 (CONFIRMED to return
a copy of the frame but leaves the ready flag stuck - do not use for capture), 0x98, 0xA3 (timer/keep-alive
style), 0x99 (blob writes of unknown purpose: **avoid**, could touch calibration/firmware).

## Registers (CONFIRMED unless noted)
- `0x00`: reads `0xAA` idle; bit 0 set (`0xAB`) = frame ready. Cleared by reading the frame with 0x64 and then
  writing `0x2D = 0x20`.
- `0x2D`: `0x13` triggers one frame, `0x20` = idle.
- `0x09..0x13`: one configuration block. Cold power-on contents `83 FC 03 00 03 0C 14 20 01 0A 72`;
  capture mode `83 24 00 44 0F 08 20 20 01 05 12`. `0x0A` reads back `0x24` in capture mode.
- `0x0C` reads back **twice** the value written (0x44 reads 0x88) and cannot be cleared by writing 0.
  Only a full power-off resets it. Harmless so far.
- A USB reset does NOT reset the register file. A full power-off does.

## Capture sequence (CONFIRMED, ~20 frames at ~1 frame/s with a deliberately slow tool)
1. once: write block `0x09` (count 11) = capture block; read reg `0x0A`, expect `0x24`.
2. per frame: write `0x2D=0x13`; poll reg `0x00` until bit 0; send `45 47 49 53 64 0F 96`; read the data;
   write `0x2D=0x20`.
3. data = seven 512-byte packets then one 406-byte packet (7*512+406 = 3990).

## Transport behaviour that surprised us (all CONFIRMED)
- The sensor will not accept an OUT write while it has data waiting and nobody is reading bulk IN (the write
  times out). Always keep an IN read pending. Both transfers are submitted together.
- After power-up, and until it sees a command, the sensor replays a stale ~55 KB buffer on bulk IN.
  Recognise replies by content ("SIGE" + expected bytes) and skip everything else.
- While the host keeps polling IN, the sensor repeats its last reply (or last frame) indefinitely. Stop reading
  once you have what you need.
- Empty sensor: frame mean ~97, pixel std ~3. Finger: std 43-51. A cleaned sensor really is empty; an earlier
  "empty" capture contained ridges because a finger had pressed the sensor (it is also the power button) just
  before.

## Open questions
- What 0x83/0x84 interrupts report and whether a low-power "detect mode" with interrupts exists (the Windows
  driver uses one; this driver polls frames instead).
- Whether more than the single cold-power-on capture-mode block is needed on other units or firmware versions.
- Real scan resolution; lifetime/power cost of continuous capturing.
