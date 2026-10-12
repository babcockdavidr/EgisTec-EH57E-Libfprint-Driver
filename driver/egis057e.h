/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * egis057e.h - protocol constants for the Egis Technology (LighTuning)
 *              1c7a:057e "EgisTec EH57E" fingerprint sensor.
 *
 * Everything in this file was established by (a) reading the Windows driver's
 * behaviour and (b) talking to a real sensor from Linux.  Each item is tagged
 * with how well it is known:
 *
 *   [CONFIRMED]  observed working on real hardware
 *   [DECODED]    read from the Windows driver, not independently verified
 *   [GUESS]      informed guess; treat as tunable
 *
 * The prose version of this lives in docs/PROTOCOL.md.
 */
#ifndef EGIS057E_H
#define EGIS057E_H

#include <glib.h>

/* ---- USB identity and layout ------------------------------------------- */

#define EGIS057E_VID 0x1c7a
#define EGIS057E_PID 0x057e

/* [CONFIRMED] One vendor-specific interface.  Bulk OUT 0x01 carries commands,
 * bulk IN 0x82 carries replies and image data.  Two 16-byte interrupt IN
 * endpoints (0x83, 0x84) exist; this driver never uses them. */
#define EGIS057E_INTERFACE   0
#define EGIS057E_EP_CMD_OUT  0x01
#define EGIS057E_EP_DATA_IN  0x82

/* ---- Command framing ---------------------------------------------------- */

/* [CONFIRMED] A request is 7 bytes:   'E' 'G' 'I' 'S'  <opcode> <arg1> <arg2>
 * A register/status reply is 7 bytes: 'S' 'I' 'G' 'E'  <b4> <b5> <status>
 * where status 0x01 means success.  Write-block requests carry extra payload
 * bytes after the 7-byte header. */
#define EGIS057E_REQ_HDR_LEN     7
#define EGIS057E_REPLY_LEN       7
#define EGIS057E_REPLY_STATUS_OK 0x01

/* Opcodes ([CONFIRMED] unless noted). */
#define EGIS057E_OP_READ_REG    0x60 /* arg1 = reg; reply b4 = reg, b5 = value   */
#define EGIS057E_OP_WRITE_REG   0x61 /* arg1 = reg, arg2 = value; reply echoes   */
#define EGIS057E_OP_WRITE_BLOCK 0x63 /* arg1 = first reg, arg2 = count + payload;
                                      * no 7-byte reply was ever observed        */
#define EGIS057E_OP_READ_IMAGE  0x64 /* arg1:arg2 = length (big endian); the
                                      * sensor answers with raw image bytes only */

/* ---- Registers ---------------------------------------------------------- */

/* [CONFIRMED] Reads 0xAA when idle.  Bit 0 is set (0xAB) once a triggered
 * frame is ready.  Reading the frame with OP_READ_IMAGE and then writing
 * REG_CAPTURE = CAPTURE_IDLE returns it to 0xAA. */
#define EGIS057E_REG_STATUS      0x00
#define EGIS057E_STATUS_ID_MASK  0xfe
#define EGIS057E_STATUS_ID       0xaa
#define EGIS057E_STATUS_READY    0x01

/* [CONFIRMED] Frame trigger / idle register. */
#define EGIS057E_REG_CAPTURE     0x2d
#define EGIS057E_CAPTURE_TRIGGER 0x13
#define EGIS057E_CAPTURE_IDLE    0x20

/* [CONFIRMED] Registers 0x09..0x13 form one configuration block.  The Windows
 * driver switches the sensor between "detect" and "capture" modes by writing
 * this whole block.  REG_MODE_PROBE is read back to prove the write took. */
#define EGIS057E_REG_BLOCK_FIRST    0x09
#define EGIS057E_BLOCK_LEN          11
#define EGIS057E_REG_MODE_PROBE     0x0a  /* 0x24 in capture mode, 0xfc when cold */
#define EGIS057E_MODE_PROBE_CAPTURE 0x24

/* Capture-mode block.  Bytes 0-3 come from the Windows driver's "sensor mode"
 * routine [DECODED]; bytes 4-6, 9 and 10 are the constants the Windows driver
 * had left in the sensor after calibrating on the test unit [CONFIRMED to give
 * good images on that unit; no auto-calibration is performed here]. */
#define EGIS057E_CAPTURE_BLOCK \
  { 0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12 }

/* Block the sensor holds after a cold power-on.  Written back on deactivation
 * so the sensor is left in the state in which it was found [CONFIRMED safe]. */
#define EGIS057E_COLD_BLOCK \
  { 0x83, 0xfc, 0x03, 0x00, 0x03, 0x0c, 0x14, 0x20, 0x01, 0x0a, 0x72 }

/* ---- Image geometry ----------------------------------------------------- */

/* [CONFIRMED] 70 x 57 pixels, 8 bits per pixel, row-major.  Ridges are
 * BRIGHT in the raw data (libfprint wants FPI_IMAGE_COLORS_INVERTED). */
#define EGIS057E_IMG_WIDTH   70
#define EGIS057E_IMG_HEIGHT  57
#define EGIS057E_IMG_BYTES   (EGIS057E_IMG_WIDTH * EGIS057E_IMG_HEIGHT) /* 3990 */

/* [CONFIRMED] The image arrives as seven 512-byte USB packets followed by one
 * 406-byte packet (7*512 + 406 = 3990).  The short packet is how a frame end
 * is recognised. */
#define EGIS057E_PKT_FULL        512
#define EGIS057E_PKT_FULL_COUNT  7
#define EGIS057E_PKT_LAST        406

/* [GUESS] Scan resolution in pixels per millimetre.  Measured median ridge
 * period is about 6.8 px; typical adult ridge spacing is 0.45-0.55 mm, giving
 * 12-16 px/mm.  libfprint's NBIS assumes ~19.7 px/mm for "normal" sensors. */
#define EGIS057E_PPMM 14.0

#endif /* EGIS057E_H */
