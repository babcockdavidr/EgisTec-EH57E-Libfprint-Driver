/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Driver for the Egis Technology (LighTuning) 1c7a:057e "EgisTec EH57E"
 * fingerprint sensor, as found in the power button of Samsung Galaxy Book
 * laptops.
 *
 * Overview
 * --------
 * The sensor is a small area ("press") sensor that returns a 70x57, 8-bit
 * grayscale image of whatever touches it.  It does not match anything itself.
 * This driver therefore does three things:
 *
 *   1. talks to the sensor (7-byte "EGIS" commands, see egis057e.h);
 *   2. decides when a settled, good-quality press is present (see
 *      egis057e_image.h) and hands exactly that image to libfprint;
 *   3. reports finger on/off so libfprint can run its normal enroll/verify.
 *
 * Matching and the accept/reject decision happen in libfprint, not here.
 *
 * Structure of this file (read top to bottom):
 *   - tunables
 *   - device structure
 *   - "exchange": one USB command and its reply, the only code that touches USB
 *   - the capture state machine (activation, frame loop, deactivation)
 *   - libfprint glue (open/close/activate/deactivate, id table, class setup)
 *
 * Every protocol statement is documented in docs/PROTOCOL.md together with how
 * well it is known.  Nothing here is copied from another driver.
 */

#define FP_COMPONENT "egis057e"

#include "drivers_api.h"

#include "egis057e.h"
#include "egis057e_image.h"

/* ---------------------------------------------------------------------------
 * Tunables.  Everything that trades speed, power or strictness is here.
 * ------------------------------------------------------------------------ */

/* Minimum bozorth3 score to call two prints a match.  libfprint's default is
 * 40 and it is deliberately NOT lowered: a tiny 70x57 window yields few
 * minutiae, and a low threshold would trade away security.  See SECURITY.md. */
#define EGIS057E_BZ3_THRESHOLD 40

/* Presses needed to enroll.  More stages give the matcher more of the finger
 * to compare against, which matters for a sensor this small. */
#define EGIS057E_ENROLL_STAGES 8

/* Pause between frames while NO finger is present (power/CPU saving).  Once a
 * finger is on the sensor frames are taken back to back.  40 ms = 25 scans per
 * second when idle; set to 0 to scan as fast as the sensor allows. */
#define EGIS057E_IDLE_INTERVAL_MS 40

/* Waiting for the "frame ready" bit: poll spacing and give-up count. */
#define EGIS057E_POLL_DELAY_MS 1
#define EGIS057E_POLL_MAX      300

/* USB timeouts. */
#define EGIS057E_USB_TIMEOUT_MS        1000
#define EGIS057E_TOLERANT_TIMEOUT_MS   150

/* Packets to discard while searching for a reply or a frame.  A sensor that
 * has just powered up replays a stale buffer until it sees its first command,
 * so some skipping is normal (about 60 packets were seen). */
#define EGIS057E_SKIP_LIMIT 1024

/* Consecutive failed frames before giving up on the sensor. */
#define EGIS057E_MAX_FAILURES 5

/* A finger that never settles (keeps sliding) for this long gets a "try again". */
#define EGIS057E_TOUCH_TIMEOUT_MS 3000

/* ---------------------------------------------------------------------------
 * Device structure
 * ------------------------------------------------------------------------ */

typedef enum {
  XCHG_REPLY,   /* expect one 7-byte "SIGE" reply */
  XCHG_IMAGE,   /* expect a 3990-byte frame as 7 x 512 + 406 bytes */
  XCHG_NONE,    /* the sensor sends no reply (write-block); just drain once */
} XchgKind;

/* State of the single command/reply exchange that may be in flight. */
typedef struct
{
  FpiSsm   *ssm;
  XchgKind  kind;
  guint8    req[EGIS057E_REQ_HDR_LEN + EGIS057E_BLOCK_LEN];
  gsize     req_len;
  gint      want_b4;            /* required reply byte 4, or -1 */
  gint      want_b5;            /* required reply byte 5, or -1 */
  gboolean  tolerate_timeout;   /* an IN timeout counts as success */
  gboolean  out_done;           /* the request has been written */
  gboolean  in_done;            /* the reply/frame/drain has been obtained */
  gboolean  failed;
  gboolean  finished;
  gboolean  warned_status;
  guint     skipped;
  guint8    reply[EGIS057E_REPLY_LEN];
  guint8    ring[EGIS057E_PKT_FULL_COUNT][EGIS057E_PKT_FULL];
  guint     ring_fill;          /* full packets currently held in ring */
  guint     ring_next;          /* ring slot the next packet goes to (= oldest when full) */
  guint8   *image_out;          /* destination for XCHG_IMAGE, or NULL to discard */
} Exchange;

struct _FpDeviceEgis057e
{
  FpImageDevice parent;

  /* session */
  FpiSsm             *ssm;
  gboolean            stop_requested;
  gboolean            activated;
  FpiImageDeviceState core_state;     /* libfprint's view, from change_state() */

  /* the one USB exchange in flight */
  Exchange            x;

  /* frame loop */
  guint8              frame[EGIS057E_IMG_BYTES];
  gboolean            frame_valid;
  guint               poll_tries;
  guint               failures;
  gint64              touch_started_us;
  Egis057eTracker     tracker;

  /* debug statistics */
  gint64              stats_t0_us;
  guint               stats_frames;
};

G_DECLARE_FINAL_TYPE (FpDeviceEgis057e, fpi_device_egis057e, FPI, DEVICE_EGIS057E, FpImageDevice)
G_DEFINE_TYPE (FpDeviceEgis057e, fpi_device_egis057e, FP_TYPE_IMAGE_DEVICE)

/* ---------------------------------------------------------------------------
 * Exchange: send one request, collect the answer.
 *
 * Two facts about this sensor shape the design (both observed on hardware):
 *   - It will not accept a write unless an IN read is pending, because it may
 *     be busy trying to push data out.  So the OUT write and the first IN read
 *     are always submitted together.
 *   - Before its first command after power-up it streams stale 512-byte
 *     packets.  So replies are recognised by content ("SIGE" + expected bytes)
 *     and everything else is skipped, never assumed.
 * ------------------------------------------------------------------------ */

static void xchg_in_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error);

static void
xchg_fail (FpDeviceEgis057e *self, GError *error)
{
  Exchange *x = &self->x;

  x->failed = TRUE;
  fpi_ssm_mark_failed (x->ssm, error);
}

static void
xchg_check_finish (FpDeviceEgis057e *self)
{
  Exchange *x = &self->x;

  if (x->failed || x->finished || !x->out_done || !x->in_done)
    return;
  x->finished = TRUE;
  fpi_ssm_next_state (x->ssm);
}

static void
xchg_out_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  Exchange *x = &self->x;

  if (x->failed || x->finished)
    {
      g_clear_error (&error);
      return;
    }
  if (error)
    {
      xchg_fail (self, error);
      return;
    }
  x->out_done = TRUE;
  xchg_check_finish (self);
}

static void
xchg_submit_in (FpDeviceEgis057e *self)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (FP_DEVICE (self));
  guint timeout = self->x.tolerate_timeout ? EGIS057E_TOLERANT_TIMEOUT_MS : EGIS057E_USB_TIMEOUT_MS;

  fpi_usb_transfer_fill_bulk (t, EGIS057E_EP_DATA_IN, EGIS057E_PKT_FULL);
  fpi_usb_transfer_submit (t, timeout, NULL, xchg_in_cb, NULL);
}

static void
xchg_submit_out (FpDeviceEgis057e *self)
{
  Exchange *x = &self->x;
  FpiUsbTransfer *t = fpi_usb_transfer_new (FP_DEVICE (self));

  fpi_usb_transfer_fill_bulk_full (t, EGIS057E_EP_CMD_OUT,
                                   g_memdup2 (x->req, x->req_len), x->req_len, g_free);
  fpi_usb_transfer_submit (t, EGIS057E_USB_TIMEOUT_MS, NULL, xchg_out_cb, NULL);
}

/* Is this packet the reply we are waiting for? */
static gboolean
reply_matches (Exchange *x, const guint8 *p, gsize len)
{
  if (len != EGIS057E_REPLY_LEN || memcmp (p, "SIGE", 4) != 0)
    return FALSE;
  if (p[6] != EGIS057E_REPLY_STATUS_OK)
    {
      if (!x->warned_status)
        fp_dbg ("ignoring reply with status 0x%02x (bytes: %02x %02x)", p[6], p[4], p[5]);
      x->warned_status = TRUE;
      return FALSE;
    }
  if (x->want_b4 >= 0 && p[4] != x->want_b4)
    return FALSE;
  if (x->want_b5 >= 0 && p[5] != x->want_b5)
    return FALSE;
  return TRUE;
}

/* Feed one packet of an image transfer.  Returns TRUE when the frame is complete.
 * The last seven full-size packets before the 406-byte terminator are the image;
 * anything else (stale data, a stray reply) breaks the run and is dropped. */
static gboolean
image_feed (Exchange *x, const guint8 *p, gsize len)
{
  guint i;

  if (len == EGIS057E_PKT_FULL)
    {
      memcpy (x->ring[x->ring_next], p, EGIS057E_PKT_FULL);
      x->ring_next = (x->ring_next + 1) % EGIS057E_PKT_FULL_COUNT;
      if (x->ring_fill < EGIS057E_PKT_FULL_COUNT)
        x->ring_fill++;
      return FALSE;
    }

  if (len == EGIS057E_PKT_LAST && x->ring_fill == EGIS057E_PKT_FULL_COUNT)
    {
      if (x->image_out)
        {
          for (i = 0; i < EGIS057E_PKT_FULL_COUNT; i++)
            memcpy (x->image_out + i * EGIS057E_PKT_FULL,
                    x->ring[(x->ring_next + i) % EGIS057E_PKT_FULL_COUNT], EGIS057E_PKT_FULL);
          memcpy (x->image_out + EGIS057E_PKT_FULL_COUNT * EGIS057E_PKT_FULL, p, EGIS057E_PKT_LAST);
        }
      return TRUE;
    }

  x->ring_fill = 0;
  x->ring_next = 0;
  return FALSE;
}

static void
xchg_in_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  Exchange *x = &self->x;
  gboolean done = FALSE;

  if (x->failed || x->finished)
    {
      g_clear_error (&error);
      return;
    }

  if (error)
    {
      if (x->tolerate_timeout && g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_clear_error (&error);
          x->in_done = TRUE;
          xchg_check_finish (self);
          return;
        }
      xchg_fail (self, error);
      return;
    }

  switch (x->kind)
    {
    case XCHG_REPLY:
      if (reply_matches (x, transfer->buffer, transfer->actual_length))
        {
          memcpy (x->reply, transfer->buffer, EGIS057E_REPLY_LEN);
          done = TRUE;
        }
      break;

    case XCHG_IMAGE:
      done = image_feed (x, transfer->buffer, transfer->actual_length);
      break;

    case XCHG_NONE:
      done = TRUE;
      break;
    }

  if (done)
    {
      x->in_done = TRUE;
      xchg_check_finish (self);
      return;
    }

  if (++x->skipped > EGIS057E_SKIP_LIMIT)
    {
      xchg_fail (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                 "No valid answer from the sensor after %u packets",
                                                 EGIS057E_SKIP_LIMIT));
      return;
    }
  xchg_submit_in (self);
}

static void
xchg_start (FpDeviceEgis057e *self, FpiSsm *ssm, XchgKind kind,
            const guint8 *req, gsize req_len, gint want_b4, gint want_b5,
            gboolean tolerate_timeout, guint8 *image_out)
{
  Exchange *x = &self->x;

  g_assert (req_len <= sizeof (x->req));
  x->ssm = ssm;
  x->kind = kind;
  memcpy (x->req, req, req_len);
  x->req_len = req_len;
  x->want_b4 = want_b4;
  x->want_b5 = want_b5;
  x->tolerate_timeout = tolerate_timeout;
  x->out_done = x->in_done = x->failed = x->finished = x->warned_status = FALSE;
  x->skipped = 0;
  x->ring_fill = x->ring_next = 0;
  x->image_out = image_out;

  xchg_submit_out (self);
  xchg_submit_in (self);
}

/* --- request builders: the only places that spell out the wire format ---- */

static void
send_read_reg (FpDeviceEgis057e *self, FpiSsm *ssm, guint8 reg)
{
  const guint8 req[] = { 'E', 'G', 'I', 'S', EGIS057E_OP_READ_REG, reg, 0x00 };

  xchg_start (self, ssm, XCHG_REPLY, req, sizeof (req), reg, -1, FALSE, NULL);
}

static void
send_write_reg (FpDeviceEgis057e *self, FpiSsm *ssm, guint8 reg, guint8 value)
{
  const guint8 req[] = { 'E', 'G', 'I', 'S', EGIS057E_OP_WRITE_REG, reg, value };

  xchg_start (self, ssm, XCHG_REPLY, req, sizeof (req), reg, value, FALSE, NULL);
}

static void
send_write_block (FpDeviceEgis057e *self, FpiSsm *ssm, const guint8 *block)
{
  guint8 req[EGIS057E_REQ_HDR_LEN + EGIS057E_BLOCK_LEN] =
  { 'E', 'G', 'I', 'S', EGIS057E_OP_WRITE_BLOCK, EGIS057E_REG_BLOCK_FIRST, EGIS057E_BLOCK_LEN };

  memcpy (req + EGIS057E_REQ_HDR_LEN, block, EGIS057E_BLOCK_LEN);
  /* The sensor never answers this command, so only a drain is expected. */
  xchg_start (self, ssm, XCHG_NONE, req, sizeof (req), -1, -1, TRUE, NULL);
}

static void
send_read_image (FpDeviceEgis057e *self, FpiSsm *ssm, guint8 *dest, gboolean tolerate_timeout)
{
  const guint8 req[] = { 'E', 'G', 'I', 'S', EGIS057E_OP_READ_IMAGE,
                         (EGIS057E_IMG_BYTES >> 8) & 0xff, EGIS057E_IMG_BYTES & 0xff };

  xchg_start (self, ssm, XCHG_IMAGE, req, sizeof (req), -1, -1, tolerate_timeout, dest);
}

/* ---------------------------------------------------------------------------
 * Capture state machine
 *
 *   activation : read status -> (clear a stale frame) -> write capture mode
 *                -> read back to prove it -> tell libfprint we are active
 *   frame loop : trigger -> wait for ready -> read image -> idle -> analyse
 *   shutdown   : write the original block back
 * ------------------------------------------------------------------------ */

enum {
  ST_ALIVE_READ,
  ST_ALIVE_CHECK,
  ST_RECOVER_READ_IMAGE,
  ST_RECOVER_IDLE,
  ST_RECOVER_REREAD,
  ST_RECOVER_RECHECK,
  ST_MODE_WRITE,
  ST_MODE_VERIFY,
  ST_MODE_CHECK,
  ST_ACTIVATED,
  ST_ABORT_WRITE,      /* reached only by jump: a frame never became ready */
  ST_TRIGGER,
  ST_POLL,
  ST_POLL_CHECK,
  ST_READ_IMAGE,
  ST_IDLE_WRITE,
  ST_PROCESS,
  ST_STOP_RESTORE,
  ST_NUM
};

static void
fail_proto (FpiSsm *ssm, const char *what)
{
  fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "%s", what));
}

static void
submit_best_frame (FpDeviceEgis057e *self)
{
  FpImage *img = fp_image_new (EGIS057E_IMG_WIDTH, EGIS057E_IMG_HEIGHT);

  memcpy (img->data, self->tracker.best, EGIS057E_IMG_BYTES);
  /* Ridges are bright in the raw data; the window is far smaller than a
   * fingertip, so the scan is partial (perimeter minutiae are discarded). */
  img->flags |= FPI_IMAGE_COLORS_INVERTED | FPI_IMAGE_PARTIAL;
  img->ppmm = EGIS057E_PPMM;

  fp_dbg ("submitting frame: std %.1f coverage %.2f sharpness %.1f",
          self->tracker.best_stats.stddev, self->tracker.best_stats.coverage,
          self->tracker.best_stats.sharpness);
  egis057e_tracker_mark_submitted (&self->tracker);
  fpi_image_device_image_captured (FP_IMAGE_DEVICE (self), img);
}

/* Analyse the frame just read and act on what the tracker concludes. */
static void
process_frame (FpDeviceEgis057e *self, FpiSsm *ssm)
{
  FpImageDevice *imgdev = FP_IMAGE_DEVICE (self);
  Egis057eFrameStats st;
  Egis057eEvent ev = egis057e_tracker_push (&self->tracker, self->frame, &st);
  gint64 now = g_get_monotonic_time ();
  guint delay;

  self->failures = 0;

  switch (ev)
    {
    case EGIS057E_EVENT_FINGER_ON:
      fp_dbg ("finger on (std %.1f)", st.stddev);
      self->touch_started_us = now;
      fpi_image_device_report_finger_status (imgdev, TRUE);
      break;

    case EGIS057E_EVENT_FINGER_OFF:
      fp_dbg ("finger off");
      fpi_image_device_report_finger_status (imgdev, FALSE);
      break;

    case EGIS057E_EVENT_STABLE_IMAGE:
      /* Only hand over an image when libfprint is actually waiting for one;
       * otherwise leave it pending (e.g. enrolling, finger still down). */
      if (self->core_state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
        submit_best_frame (self);
      break;

    case EGIS057E_EVENT_STALE_FRAME:
      fp_dbg ("sensor returned an identical frame");
      break;

    case EGIS057E_EVENT_STUCK:
      fail_proto (ssm, "The sensor keeps returning the same frame (stuck buffer)");
      return;

    case EGIS057E_EVENT_NONE:
      break;
    }

  /* A finger that is present but never settles: ask the user to retry. */
  if (self->tracker.finger_on && !self->tracker.submitted &&
      self->core_state == FPI_IMAGE_DEVICE_STATE_CAPTURE &&
      (now - self->touch_started_us) / 1000 > EGIS057E_TOUCH_TIMEOUT_MS)
    {
      fp_dbg ("finger never settled, asking for a retry");
      self->touch_started_us = now;
      fpi_image_device_retry_scan (imgdev, FP_DEVICE_RETRY_REMOVE_FINGER);
    }

  /* Frame-rate statistics (debug only). */
  self->stats_frames++;
  if (now - self->stats_t0_us >= 2 * G_USEC_PER_SEC)
    {
      fp_dbg ("%.1f frames/s", self->stats_frames * (double) G_USEC_PER_SEC / (double) (now - self->stats_t0_us));
      self->stats_t0_us = now;
      self->stats_frames = 0;
    }

  if (self->stop_requested)
    {
      fpi_ssm_jump_to_state (ssm, ST_STOP_RESTORE);
      return;
    }

  /* Scan back to back while a finger is down; throttle only when idle. */
  delay = self->tracker.finger_on ? 0 : EGIS057E_IDLE_INTERVAL_MS;
  if (delay)
    fpi_ssm_jump_to_state_delayed (ssm, ST_TRIGGER, delay);
  else
    fpi_ssm_jump_to_state (ssm, ST_TRIGGER);
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  static const guint8 capture_block[EGIS057E_BLOCK_LEN] = EGIS057E_CAPTURE_BLOCK;
  static const guint8 cold_block[EGIS057E_BLOCK_LEN] = EGIS057E_COLD_BLOCK;
  guint8 status;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* ---- activation ---------------------------------------------------- */

    case ST_ALIVE_READ:
      send_read_reg (self, ssm, EGIS057E_REG_STATUS);
      break;

    case ST_ALIVE_CHECK:
      status = self->x.reply[5];
      if ((status & EGIS057E_STATUS_ID_MASK) != EGIS057E_STATUS_ID)
        {
          fail_proto (ssm, "Unexpected sensor status value; is this really an EH57E?");
          break;
        }
      if (status & EGIS057E_STATUS_READY)
        fpi_ssm_next_state (ssm);                  /* a stale frame is pending: clear it */
      else
        fpi_ssm_jump_to_state (ssm, ST_MODE_WRITE);
      break;

    case ST_RECOVER_READ_IMAGE:
      fp_dbg ("sensor holds an unread frame, consuming it");
      send_read_image (self, ssm, NULL, TRUE);
      break;

    case ST_RECOVER_IDLE:
      send_write_reg (self, ssm, EGIS057E_REG_CAPTURE, EGIS057E_CAPTURE_IDLE);
      break;

    case ST_RECOVER_REREAD:
      send_read_reg (self, ssm, EGIS057E_REG_STATUS);
      break;

    case ST_RECOVER_RECHECK:
      if (self->x.reply[5] & EGIS057E_STATUS_READY)
        fail_proto (ssm, "The sensor still reports a pending frame; a full power-off is needed");
      else
        fpi_ssm_next_state (ssm);
      break;

    case ST_MODE_WRITE:
      send_write_block (self, ssm, capture_block);
      break;

    case ST_MODE_VERIFY:
      send_read_reg (self, ssm, EGIS057E_REG_MODE_PROBE);
      break;

    case ST_MODE_CHECK:
      if (self->x.reply[5] != EGIS057E_MODE_PROBE_CAPTURE)
        fail_proto (ssm, "The sensor did not enter capture mode");
      else
        fpi_ssm_next_state (ssm);
      break;

    case ST_ACTIVATED:
      self->activated = TRUE;
      self->stats_t0_us = g_get_monotonic_time ();
      self->stats_frames = 0;
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), NULL);
      fpi_ssm_jump_to_state (ssm, ST_TRIGGER);
      break;

    /* ---- frame loop ---------------------------------------------------- */

    case ST_ABORT_WRITE:
      /* A frame never became ready: put the capture register back to idle. */
      send_write_reg (self, ssm, EGIS057E_REG_CAPTURE, EGIS057E_CAPTURE_IDLE);
      break;

    case ST_TRIGGER:
      if (self->stop_requested)
        {
          fpi_ssm_jump_to_state (ssm, ST_STOP_RESTORE);
          break;
        }
      self->frame_valid = FALSE;
      self->poll_tries = 0;
      send_write_reg (self, ssm, EGIS057E_REG_CAPTURE, EGIS057E_CAPTURE_TRIGGER);
      break;

    case ST_POLL:
      send_read_reg (self, ssm, EGIS057E_REG_STATUS);
      break;

    case ST_POLL_CHECK:
      if (self->x.reply[5] & EGIS057E_STATUS_READY)
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      if (++self->poll_tries > EGIS057E_POLL_MAX)
        {
          fp_warn ("frame did not become ready (%u failures so far)", self->failures + 1);
          if (++self->failures > EGIS057E_MAX_FAILURES)
            fail_proto (ssm, "The sensor stopped producing frames");
          else
            fpi_ssm_jump_to_state (ssm, ST_ABORT_WRITE);
          break;
        }
      fpi_ssm_jump_to_state_delayed (ssm, ST_POLL, EGIS057E_POLL_DELAY_MS);
      break;

    case ST_READ_IMAGE:
      send_read_image (self, ssm, self->frame, FALSE);
      break;

    case ST_IDLE_WRITE:
      self->frame_valid = TRUE;
      send_write_reg (self, ssm, EGIS057E_REG_CAPTURE, EGIS057E_CAPTURE_IDLE);
      break;

    case ST_PROCESS:
      process_frame (self, ssm);
      break;

    /* ---- shutdown ------------------------------------------------------ */

    case ST_STOP_RESTORE:
      send_write_block (self, ssm, cold_block);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
capture_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  FpImageDevice *imgdev = FP_IMAGE_DEVICE (dev);

  self->ssm = NULL;

  if (self->stop_requested)
    {
      /* A failure while restoring the original block is not worth failing for. */
      if (error)
        fp_warn ("could not restore the sensor state: %s", error->message);
      g_clear_error (&error);
      fpi_image_device_deactivate_complete (imgdev, NULL);
      return;
    }

  if (!error)
    return;     /* the loop only ends by error or by a stop request */

  fp_warn ("capture loop failed: %s", error->message);
  if (!self->activated)
    fpi_image_device_activate_complete (imgdev, error);
  else
    fpi_image_device_session_error (imgdev, error);
}

/* ---------------------------------------------------------------------------
 * libfprint glue
 * ------------------------------------------------------------------------ */

static void
egis_track_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FPI_DEVICE_EGIS057E (dev)->core_state = state;
}

static void
egis_open (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                EGIS057E_INTERFACE, 0, &error);
  fpi_image_device_open_complete (dev, error);
}

static void
egis_close (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  EGIS057E_INTERFACE, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static void
egis_activate (FpImageDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  self->stop_requested = FALSE;
  self->activated = FALSE;
  self->frame_valid = FALSE;
  self->failures = 0;
  egis057e_tracker_reset (&self->tracker);

  self->ssm = fpi_ssm_new (FP_DEVICE (dev), capture_run_state, ST_NUM);
  fpi_ssm_start (self->ssm, capture_complete);
}

static void
egis_deactivate (FpImageDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  if (self->ssm)
    self->stop_requested = TRUE;      /* the loop finishes its current step, then restores */
  else
    fpi_image_device_deactivate_complete (dev, NULL);
}

static const FpIdEntry id_table[] = {
  { .vid = EGIS057E_VID, .pid = EGIS057E_PID },
  { .vid = 0, .pid = 0 },
};

static void
fpi_device_egis057e_init (FpDeviceEgis057e *self)
{
}

static void
fpi_device_egis057e_class_init (FpDeviceEgis057eClass *klass)
{
  FpDeviceClass *fp_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *image_class = FP_IMAGE_DEVICE_CLASS (klass);

  fp_class->id = "egis057e";
  fp_class->full_name = "Egis Technology Inc. (aka. LighTuning) EH57E";
  fp_class->type = FP_DEVICE_TYPE_USB;
  fp_class->id_table = id_table;
  fp_class->scan_type = FP_SCAN_TYPE_PRESS;
  fp_class->nr_enroll_stages = EGIS057E_ENROLL_STAGES;

  image_class->img_open = egis_open;
  image_class->img_close = egis_close;
  image_class->activate = egis_activate;
  image_class->deactivate = egis_deactivate;
  image_class->change_state = egis_track_state;

  image_class->img_width = EGIS057E_IMG_WIDTH;
  image_class->img_height = EGIS057E_IMG_HEIGHT;
  image_class->bz3_threshold = EGIS057E_BZ3_THRESHOLD;
}

/* ===========================================================================
 * AI NOTES - machine-oriented summary. Safe to ignore: it is a comment on
 * purpose. Terse on purpose. Written so another model can reproduce this
 * work for sibling sensors quickly. No personal data in here.
 * ===========================================================================
 *
 * TASK    : libfprint driver for a USB fingerprint sensor that has only a
 *           Windows driver. Target 1c7a:057e (EgisTec EH57E, Samsung power
 *           button). Libfprint matches host-side; sensor = image source only.
 * RESULT  : wire protocol fully working from Linux (pyusb, real frames
 *           obtained). This driver compiles clean vs libfprint 1.94.9 and
 *           1.95.2 (strict warnings) and tracker is unit tested. NOT yet run
 *           inside libfprint on hardware when written: expect to iterate.
 *
 * WINNING WORKFLOW (do this, in this order)
 *  W1 lsusb -v -d VID:PID -> endpoints/bcdDevice. Compare to this layout:
 *     1 vendor iface, bulk OUT 0x01, bulk IN 0x82, 2x int IN 16B.
 *  W2 get the Windows driver package (pnputil /enum-drivers, then
 *     pnputil /export-driver oemNN.inf DIR). Use it ONLY AS READ-ONLY INPUT.
 *  W3 static analysis: INF (UMDF? sensor mode "Basic" = host matching), strings,
 *     PE imports/resources (pefile; a custom resource FWxxx = embedded blob;
 *     BCrypt imports existed but were NOT on the sensor wire), Ghidra headless
 *     (needs FULL JDK 21+, javac included; release zip from GitHub releases,
 *     API is rate limited). Java GhidraScripts used: decompile functions that
 *     reference strings matching a regex; callers-of; dump pointer tables;
 *     scalar-constant search.
 *  W4 find magic: 'EGIS'=0x53494745 stored LE written into request buffers,
 *     'SIGE'=0x45474953 compared on replies. Handlers share one shape:
 *     7-byte request EGIS,opcode,arg1,arg2 ; 7-byte reply SIGE,b4,b5,status.
 *     Image size appears as constants in the get_image state handler
 *     (0x46 x 0x39 = 70x57, malloc 0xF96 = 3990).
 *  W5 PROBE ON LINUX IMMEDIATELY. Device is reachable with no kernel driver:
 *     pyusb, claim iface 0, udev rule MODE=0660 TAG+=uaccess. MUST run a thread
 *     that drains bulk IN continuously or OUT writes time out. Start read-only.
 *  W6 reproduce the driver's own call sequence exactly, verify every claim
 *     by READ-BACK (write replies always echo; echo proves nothing).
 *  W7 reshape 3990 B to WxH, view it, derive thresholds from real frames.
 *  W8 write libfprint driver, build inside a libfprint tree with meson.
 *
 * MISSTEPS (each cost real time - avoid)
 *  M1 BIGGEST: spent hours trying to capture Windows-side USB traffic. USBPcap
 *     on bare metal loses the device after the first disable/enable and showed
 *     only enumeration; Win10/Win11 KVM guests with USB passthrough gave Code 10
 *     (STATUS_POWER_STATE_INVALID) and the driver never sent one command.
 *     Should have gone straight to W5. Static analysis + Linux probing was
 *     enough; a Windows capture was never needed.
 *  M2 believed the constant bulk stream seen at power-up was an image; it is a
 *     stale-buffer replay. Sensor repeats its last reply/frame while the host
 *     keeps polling IN.
 *  M3 assumed registers are plain storage. reg 0x0C reads 2x the written value
 *     and stays; USB reset does not clear registers; only full power-off does.
 *     Always check a known cold state before writing; keep a restore path.
 *  M4 used opcode 0x90 (reply with size header + data) to read images: gives a
 *     picture but leaves reg0 bit0 (frame ready) stuck, later frames return
 *     "no data". Real image read = opcode 0x64, raw bytes, no header. Lesson:
 *     trace what the driver's get_image state machine really calls.
 *  M5 added retry/escalation loops before reproducing the proven single-shot
 *     script. Change ONE thing at a time; log WHY on every failure.
 *  M6 wrong theory: "driver refuses VMs via SMBIOS". It only hashes
 *     UUID+serial into a machine id. Also mapped decompiled wrappers to
 *     vtable slots with the wrong base once - verify base with two known slots.
 *  M7 the sensor is also the power button: a boot-time press leaves a stale
 *     buffer / latent print, so an "empty" capture contained ridges.
 *  M8 reply sync: replies repeat; match by content (b4 echo + status) never by
 *     position; ~1 in 9 early reads returned the previous register's reply.
 *
 * PROTOCOL (all CONFIRMED on hw unless marked)
 *  req   : 45 47 49 53 OP A1 A2 [payload]      reply: 53 49 47 45 B4 B5 ST  (ST=01 ok)
 *  60 rd reg A1 -> B4=reg B5=val | 61 wr reg A1=A2 -> echo | 63 wr block
 *  A1=first A2=count + payload (no reply) | 64 read image A1:A2=len BE ->
 *  raw 3990 B as 7x512+406 | [DECODED only: 62 rdN, 67/69, 73, 80/81, 98, A3
 *  timer keepalive, 99 blob write = AVOID]
 *  reg00 AA idle, AB frame ready | reg2D 13 trigger, 20 idle
 *  block@09 len11 cold 83 FC 03 00 03 0C 14 20 01 0A 72
 *           capture 83 24 00 44 0F 08 20 20 01 05 12 (reg0A reads 24)
 *  per frame: W2D=13 ; poll reg00 bit0 ; 64 0F 96 ; read ; W2D=20
 *  empty frame mean~97 std~3 ; finger std 43-51 ; settled-finger NCC 0.98-0.998
 *  image: ridges bright in raw => FPI_IMAGE_COLORS_INVERTED|PARTIAL; ppmm ~14 GUESS
 *  no fw upload needed, no wire encryption seen.
 *  transport: write+read submitted together; skip stale 512B packets before
 *  first reply; sync on SIGE; frame = last 7x512 before a 406 B packet.
 *
 * REUSE RECIPE for sibling sensors (EH5xx/ET5xx; Egis framing family)
 *  - same magic? yes -> reuse this file: edit egis057e.h (VID/PID, W/H, block
 *    bytes, probe reg/value), egis057e_image.h thresholds, rename symbols.
 *  - different magic / ETU905-style match-on-chip: see libfprint egismoc.c
 *    (different framing); etes603.c documents an older EGIS/SIGE variant.
 *  - Windows driver branches on bcdDevice: version<3 counts 0x64 length in
 *    512 B units; check every `version` compare in the decompile.
 *  - capture block: take the sensor-mode routine's block; unknown calibration
 *    bytes: read the sensor registers after a Windows session or use cold
 *    values; verify with real frames.
 *  - port check list: compile in libfprint 1.94.x AND 1.95.x trees (meson
 *    layouts differ; tools/add_to_libfprint_tree.py handles three).
 *
 * EXPERIMENT SAFETY RULES USED
 *  dry-run default, hard whitelist (assert) of opcodes/registers/values,
 *  restore after every write test, refuse to write when start state differs,
 *  never send 0x99/0x80/0x81 blobs, never publish captured frames (biometric).
 *
 * OPEN / NOT DONE
 *  interrupt EPs 0x83/0x84 meaning; low-power detect mode; auto-calibration;
 *  real scan resolution; measured fps; N-of-M voting (libfprint verifies ONE
 *  image per operation: needs a policy layer above libfprint, see SECURITY.md).
 *
 * ENV GOTCHAS
 *  Ghidra needs javac (apt install openjdk-21-jdk-headless). GitHub API rate
 *  limits: take release assets via the expanded_assets page. Ubuntu libfprint
 *  orig tarballs miss tod/libfprint-tod.ver: full link fails, object compile
 *  is still a valid check. fish shell has no heredocs: ship scripts as files.
 *  USB tools need sudo or the udev rule. Close probe scripts before fprintd.
 * ======================================================================== */
