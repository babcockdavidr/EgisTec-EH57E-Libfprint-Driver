/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * egis057e_image.h - frame analysis and finger tracking for the EH57E sensor.
 *
 * This file has NO dependency on libfprint or GLib so that the exact logic the
 * driver runs can be unit-tested and audited on its own (see tests/).
 *
 * Why this exists: libfprint matches ONE image per verify operation, so the
 * driver itself decides when a touch is good enough to hand over.  Instead of
 * submitting the first frame that looks like skin, it requires a short run of
 * consecutive, mutually consistent frames (a settled press) and submits the
 * best of them.  This filters out brushes, sliding fingers, half-touches and
 * stuck/replayed sensor buffers.  It is a quality gate, not a matcher: the
 * accept/reject decision about WHOSE finger it is stays in libfprint.
 *
 * All thresholds were chosen from real frames captured from one unit:
 *   empty sensor : std 2.9-3.4
 *   finger       : std 43-51, mean gradient 24-36
 *   settled finger, consecutive frames: correlation 0.98-0.998
 *   moving finger / re-press            : correlation about 0
 * They are macros so they can be overridden at build time.
 */
#ifndef EGIS057E_IMAGE_H
#define EGIS057E_IMAGE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EGIS057E_FRAME_W 70
#define EGIS057E_FRAME_H 57
#define EGIS057E_FRAME_N (EGIS057E_FRAME_W * EGIS057E_FRAME_H)

/* Pixel standard deviation separating "skin" from "empty sensor". */
#ifndef EGIS057E_STD_FINGER_MIN
#define EGIS057E_STD_FINGER_MIN 20.0
#endif
#ifndef EGIS057E_STD_EMPTY_MAX
#define EGIS057E_STD_EMPTY_MAX 12.0
#endif

/* An 8x8 block "has ridges" when its own standard deviation is at least this. */
#ifndef EGIS057E_BLOCK_STD_MIN
#define EGIS057E_BLOCK_STD_MIN 14.0
#endif
/* Fraction of blocks that must have ridges for a frame to be submittable. */
#ifndef EGIS057E_COVERAGE_MIN
#define EGIS057E_COVERAGE_MIN 0.70
#endif

/* Consecutive frames must correlate at least this well to count as "settled". */
#ifndef EGIS057E_NCC_STABLE_MIN
#define EGIS057E_NCC_STABLE_MIN 0.80
#endif
/* Number of consecutive settled frames needed before a frame is submitted. */
#ifndef EGIS057E_STABLE_FRAMES
#define EGIS057E_STABLE_FRAMES 3
#endif
/* Frames of agreement needed to declare the finger on / off. */
#ifndef EGIS057E_DEBOUNCE_FRAMES
#define EGIS057E_DEBOUNCE_FRAMES 2
#endif
/* Byte-identical consecutive frames are physically implausible (sensor noise
 * differs every capture); this many in a row means a stuck/replayed buffer. */
#ifndef EGIS057E_STALE_LIMIT
#define EGIS057E_STALE_LIMIT 4
#endif

typedef struct
{
  double mean;
  double stddev;    /* over all pixels */
  double coverage;  /* fraction of 8x8 blocks whose own stddev >= BLOCK_STD_MIN */
  double sharpness; /* mean absolute horizontal+vertical gradient */
} Egis057eFrameStats;

static inline void
egis057e_frame_stats (const uint8_t *px, Egis057eFrameStats *s)
{
  double sum = 0, sum2 = 0, grad = 0;
  int i, x, y, bx, by, blocks = 0, good = 0;

  for (i = 0; i < EGIS057E_FRAME_N; i++)
    {
      sum += px[i];
      sum2 += (double) px[i] * px[i];
    }
  s->mean = sum / EGIS057E_FRAME_N;
  s->stddev = sqrt (fmax (0.0, sum2 / EGIS057E_FRAME_N - s->mean * s->mean));

  for (y = 0; y < EGIS057E_FRAME_H - 1; y++)
    for (x = 0; x < EGIS057E_FRAME_W - 1; x++)
      {
        int p = px[y * EGIS057E_FRAME_W + x];
        grad += abs (px[y * EGIS057E_FRAME_W + x + 1] - p) + abs (px[(y + 1) * EGIS057E_FRAME_W + x] - p);
      }
  s->sharpness = grad / ((EGIS057E_FRAME_W - 1) * (EGIS057E_FRAME_H - 1));

  for (by = 0; by + 8 <= EGIS057E_FRAME_H; by += 8)
    for (bx = 0; bx + 8 <= EGIS057E_FRAME_W; bx += 8)
      {
        double bs = 0, bs2 = 0, bm;
        for (y = 0; y < 8; y++)
          for (x = 0; x < 8; x++)
            {
              double v = px[(by + y) * EGIS057E_FRAME_W + bx + x];
              bs += v;
              bs2 += v * v;
            }
        bm = bs / 64.0;
        blocks++;
        if (sqrt (fmax (0.0, bs2 / 64.0 - bm * bm)) >= EGIS057E_BLOCK_STD_MIN)
          good++;
      }
  s->coverage = blocks ? (double) good / blocks : 0.0;
}

/* Normalised cross-correlation of two frames: 1.0 identical shape, ~0 unrelated. */
static inline double
egis057e_frame_ncc (const uint8_t *a, const uint8_t *b)
{
  double ma = 0, mb = 0, saa = 0, sbb = 0, sab = 0;
  int i;

  for (i = 0; i < EGIS057E_FRAME_N; i++)
    {
      ma += a[i];
      mb += b[i];
    }
  ma /= EGIS057E_FRAME_N;
  mb /= EGIS057E_FRAME_N;
  for (i = 0; i < EGIS057E_FRAME_N; i++)
    {
      double da = a[i] - ma, db = b[i] - mb;
      saa += da * da;
      sbb += db * db;
      sab += da * db;
    }
  return sab / sqrt (saa * sbb + 1e-9);
}


/* ---- preprocessing for libfprint's matcher ---------------------------------
 * libfprint extracts minutiae with NBIS, which was designed for much larger
 * images.  On the raw 70x57 frames it finds 0-3 minutiae (zero in 12 of 17
 * real frames) and can never reach a usable match score.  Mirroring the frame
 * outward by EGIS057E_PAD_PX pixels on every side gives NBIS the surrounding
 * area it expects; it then finds 9-30 minutiae per frame and scores two
 * presses of the same spot around 80.  The padding is synthetic (mirrored)
 * data: it adds no new information, so treat the matcher's output on this
 * sensor as less trustworthy than on a full-size reader (see docs/SECURITY.md).
 * Set to 0 to submit the raw frame. */
#ifndef EGIS057E_PAD_PX
#define EGIS057E_PAD_PX 32
#endif

static inline void
egis057e_pad_reflect (const uint8_t *src, uint8_t *dst, int pad)
{
  int w2 = EGIS057E_FRAME_W + 2 * pad, h2 = EGIS057E_FRAME_H + 2 * pad;
  int x, y;

  for (y = 0; y < h2; y++)
    for (x = 0; x < w2; x++)
      {
        int sx = x - pad, sy = y - pad;

        if (sx < 0)
          sx = -sx;
        if (sx >= EGIS057E_FRAME_W)
          sx = 2 * EGIS057E_FRAME_W - 2 - sx;
        if (sx < 0)
          sx = 0;
        if (sy < 0)
          sy = -sy;
        if (sy >= EGIS057E_FRAME_H)
          sy = 2 * EGIS057E_FRAME_H - 2 - sy;
        if (sy < 0)
          sy = 0;
        dst[y * w2 + x] = src[sy * EGIS057E_FRAME_W + sx];
      }
}

/* ---- enrollment coverage ---------------------------------------------------
 * The Windows software makes you press the centre a few times and then other
 * parts of the same finger.  libfprint cannot ask for that, so the driver does
 * it: the first EGIS057E_ENROLL_REPEATS presses may be anywhere; after that a
 * press that looks like an area already captured is bounced with "lift and
 * try again", so the stored prints cover more of the finger.
 * "Looks like" = best normalised correlation over +-EGIS057E_ENROLL_SHIFT px.
 * Measured on real frames: same spot 0.80-1.00, different spots mean 0.22. */
#ifndef EGIS057E_ENROLL_REPEATS
#define EGIS057E_ENROLL_REPEATS 3
#endif
#ifndef EGIS057E_ENROLL_SHIFT
#define EGIS057E_ENROLL_SHIFT 8
#endif
#ifndef EGIS057E_ENROLL_SIMILAR_NCC
#define EGIS057E_ENROLL_SIMILAR_NCC 0.75
#endif

/* Best normalised cross-correlation of a and b over all shifts up to max_shift
 * pixels in each direction, using only overlaps of at least half a frame. */
static inline double
egis057e_shifted_ncc (const uint8_t *a, const uint8_t *b, int max_shift)
{
  double best = -1.0;
  int dx, dy;

  for (dy = -max_shift; dy <= max_shift; dy++)
    for (dx = -max_shift; dx <= max_shift; dx++)
      {
        int x0 = dx > 0 ? dx : 0, x1 = dx < 0 ? EGIS057E_FRAME_W + dx : EGIS057E_FRAME_W;
        int y0 = dy > 0 ? dy : 0, y1 = dy < 0 ? EGIS057E_FRAME_H + dy : EGIS057E_FRAME_H;
        int x, y, n = (x1 - x0) * (y1 - y0);
        double ma = 0, mb = 0, saa = 0, sbb = 0, sab = 0;

        if (2 * n < EGIS057E_FRAME_W * EGIS057E_FRAME_H)
          continue;
        for (y = y0; y < y1; y++)
          for (x = x0; x < x1; x++)
            {
              ma += a[y * EGIS057E_FRAME_W + x];
              mb += b[(y - dy) * EGIS057E_FRAME_W + (x - dx)];
            }
        ma /= n;
        mb /= n;
        for (y = y0; y < y1; y++)
          for (x = x0; x < x1; x++)
            {
              double da = a[y * EGIS057E_FRAME_W + x] - ma;
              double db = b[(y - dy) * EGIS057E_FRAME_W + (x - dx)] - mb;
              saa += da * da;
              sbb += db * db;
              sab += da * db;
            }
        if (saa > 0 && sbb > 0)
          {
            double c = sab / sqrt (saa * sbb);
            if (c > best)
              best = c;
          }
      }
  return best;
}

/* ------------------------------------------------------------------------ */

typedef enum
{
  EGIS057E_EVENT_NONE = 0,
  EGIS057E_EVENT_FINGER_ON,     /* debounced: a finger has arrived */
  EGIS057E_EVENT_FINGER_OFF,    /* debounced: the finger has left */
  EGIS057E_EVENT_STABLE_IMAGE,  /* a settled press: tracker->best is ready */
  EGIS057E_EVENT_STALE_FRAME,   /* frame identical to the previous one */
  EGIS057E_EVENT_STUCK,         /* too many stale frames: sensor is not capturing */
} Egis057eEvent;

typedef struct
{
  bool    finger_on;
  bool    submitted;       /* the best frame of this touch was already handed over */
  int     present_run;
  int     absent_run;
  int     stable_run;
  int     stale_run;
  bool    have_prev;
  bool    have_last;
  double  best_score;
  uint8_t last[EGIS057E_FRAME_N];  /* previous raw frame (stale detection) */
  uint8_t prev[EGIS057E_FRAME_N];  /* previous good frame (stability reference) */
  uint8_t best[EGIS057E_FRAME_N];  /* best frame of the current settled run */
  Egis057eFrameStats best_stats;
} Egis057eTracker;

static inline void
egis057e_tracker_reset (Egis057eTracker *t)
{
  memset (t, 0, sizeof (*t));
}

/* Call after handing t->best to libfprint. */
static inline void
egis057e_tracker_mark_submitted (Egis057eTracker *t)
{
  t->submitted = true;
}

/* Feed one freshly captured frame.  stats_out may be NULL. */
static inline Egis057eEvent
egis057e_tracker_push (Egis057eTracker *t, const uint8_t *frame, Egis057eFrameStats *stats_out)
{
  Egis057eFrameStats st;
  bool finger_like, empty_like, good, became_on = false;

  egis057e_frame_stats (frame, &st);
  if (stats_out)
    *stats_out = st;

  /* 1. Freshness: the sensor must deliver a new capture every time. */
  if (t->have_last && memcmp (t->last, frame, EGIS057E_FRAME_N) == 0)
    {
      t->stale_run++;
      return t->stale_run >= EGIS057E_STALE_LIMIT ? EGIS057E_EVENT_STUCK : EGIS057E_EVENT_STALE_FRAME;
    }
  t->stale_run = 0;
  memcpy (t->last, frame, EGIS057E_FRAME_N);
  t->have_last = true;

  /* 2. Presence with debounce. */
  finger_like = st.stddev >= EGIS057E_STD_FINGER_MIN;
  empty_like = st.stddev <= EGIS057E_STD_EMPTY_MAX;
  if (finger_like)
    {
      t->present_run++;
      t->absent_run = 0;
    }
  else if (empty_like)
    {
      t->absent_run++;
      t->present_run = 0;
    }
  else
    {
      t->present_run = 0;   /* in-between: neither skin nor empty, count as nothing */
      t->absent_run = 0;
    }

  if (t->finger_on && t->absent_run >= EGIS057E_DEBOUNCE_FRAMES)
    {
      t->finger_on = false;
      t->submitted = false;
      t->have_prev = false;
      t->stable_run = 0;
      t->best_score = 0;
      return EGIS057E_EVENT_FINGER_OFF;
    }
  if (!t->finger_on && t->present_run >= EGIS057E_DEBOUNCE_FRAMES)
    {
      t->finger_on = true;
      t->submitted = false;
      t->have_prev = false;
      t->stable_run = 0;
      t->best_score = 0;
      became_on = true;    /* this frame may also count towards stability */
    }
  if (!t->finger_on)
    return EGIS057E_EVENT_NONE;

  /* 3. Stability of a present finger. */
  good = finger_like && st.coverage >= EGIS057E_COVERAGE_MIN;
  if (!good)
    {
      t->stable_run = 0;
      t->have_prev = false;
      return became_on ? EGIS057E_EVENT_FINGER_ON : EGIS057E_EVENT_NONE;
    }

  if (t->have_prev && egis057e_frame_ncc (t->prev, frame) >= EGIS057E_NCC_STABLE_MIN)
    {
      t->stable_run++;
    }
  else
    {
      t->stable_run = 1;     /* new reference: finger moved or just arrived */
      t->best_score = 0;
    }
  memcpy (t->prev, frame, EGIS057E_FRAME_N);
  t->have_prev = true;

  {
    double score = st.coverage * st.sharpness;
    if (score >= t->best_score)
      {
        t->best_score = score;
        memcpy (t->best, frame, EGIS057E_FRAME_N);
        t->best_stats = st;
      }
  }

  if (t->stable_run >= EGIS057E_STABLE_FRAMES && !t->submitted)
    return EGIS057E_EVENT_STABLE_IMAGE;

  return became_on ? EGIS057E_EVENT_FINGER_ON : EGIS057E_EVENT_NONE;
}

#endif /* EGIS057E_IMAGE_H */
