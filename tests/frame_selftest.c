/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * frame_selftest.c - exercises the finger tracker in driver/egis057e_image.h.
 *
 *   cc -O2 -I../driver -o frame_selftest frame_selftest.c -lm
 *   ./frame_selftest                      # synthetic scenarios (no biometric data)
 *   ./frame_selftest --frames FILE        # replay frames captured from a real sensor
 *
 * FILE may hold raw 3990-byte frames back to back, or the 8+3990-byte records
 * written by the capture tools (8-byte timestamp followed by the frame).
 * NEVER commit or publish such files: they contain your fingerprint.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "egis057e_image.h"

static uint32_t rng_state = 12345u;
static double
rnd (void)   /* xorshift32, uniform in [0,1) */
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return (rng_state & 0xffffff) / (double) 0x1000000;
}

static double
gauss (void)
{
  return sqrt (-2.0 * log (rnd () + 1e-12)) * cos (6.283185307 * rnd ());
}

static uint8_t
clamp (double v)
{
  return v < 0 ? 0 : v > 255 ? 255 : (uint8_t) v;
}

static void
make_empty (uint8_t *f)
{
  for (int i = 0; i < EGIS057E_FRAME_N; i++)
    f[i] = clamp (97 + 3.0 * gauss ());
}

/* Synthetic "ridges": a sinusoid with ~6.8 px period plus per-capture noise. */
static void
make_ridges (uint8_t *f, double phase, double covered_fraction)
{
  for (int y = 0; y < EGIS057E_FRAME_H; y++)
    for (int x = 0; x < EGIS057E_FRAME_W; x++)
      {
        double u = (x * 0.9 + y * 0.43) / 6.8 * 6.283185307 + phase;
        double v = 140 + 85 * sin (u) + 3.0 * gauss ();
        if (x > covered_fraction * EGIS057E_FRAME_W)
          v = 97 + 3.0 * gauss ();    /* part of the window not touched */
        f[y * EGIS057E_FRAME_W + x] = clamp (v);
      }
}

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf ("  FAIL: %s\n", msg); failures++; } else printf ("  ok:   %s\n", msg); } while (0)

static int
count_events (Egis057eTracker *t, uint8_t frames[][EGIS057E_FRAME_N], int n, Egis057eEvent want, int *first_idx)
{
  int c = 0;
  *first_idx = -1;
  for (int i = 0; i < n; i++)
    if (egis057e_tracker_push (t, frames[i], NULL) == want)
      {
        if (*first_idx < 0)
          *first_idx = i;
        c++;
      }
  return c;
}

static void
synthetic_tests (void)
{
  static uint8_t fr[40][EGIS057E_FRAME_N];
  Egis057eTracker t;
  int first, n;

  printf ("scenario 1: empty sensor produces no events\n");
  egis057e_tracker_reset (&t);
  for (int i = 0; i < 20; i++)
    make_empty (fr[i]);
  n = 0;
  for (int i = 0; i < 20; i++)
    n += egis057e_tracker_push (&t, fr[i], NULL) != EGIS057E_EVENT_NONE;
  CHECK (n == 0 && !t.finger_on, "20 empty frames: no events, finger not on");

  printf ("scenario 2: a settled finger is submitted after EGIS057E_STABLE_FRAMES\n");
  egis057e_tracker_reset (&t);
  for (int i = 0; i < 6; i++)
    make_ridges (fr[i], 0.0, 1.0);
  int on = 0, stable = 0, stable_at = -1;
  for (int i = 0; i < 6; i++)
    {
      Egis057eEvent e = egis057e_tracker_push (&t, fr[i], NULL);
      on += e == EGIS057E_EVENT_FINGER_ON;
      if (e == EGIS057E_EVENT_STABLE_IMAGE && stable++ == 0)
        stable_at = i;
    }
  CHECK (on == 1, "exactly one FINGER_ON event");
  CHECK (stable_at >= EGIS057E_STABLE_FRAMES - 1, "STABLE_IMAGE not before the required number of frames");
  CHECK (stable_at != -1 && stable_at <= EGIS057E_STABLE_FRAMES, "STABLE_IMAGE arrives promptly");
  egis057e_tracker_mark_submitted (&t);
  CHECK (egis057e_tracker_push (&t, fr[5], NULL) != EGIS057E_EVENT_STABLE_IMAGE, "no second submission for the same touch");

  printf ("scenario 3: lifting the finger is reported once, and re-arms\n");
  for (int i = 0; i < 4; i++)
    make_empty (fr[i]);
  int off = 0;
  for (int i = 0; i < 4; i++)
    off += egis057e_tracker_push (&t, fr[i], NULL) == EGIS057E_EVENT_FINGER_OFF;
  CHECK (off == 1 && !t.finger_on && !t.submitted, "one FINGER_OFF, tracker re-armed");

  printf ("scenario 4: a moving finger is never declared settled\n");
  egis057e_tracker_reset (&t);
  for (int i = 0; i < 20; i++)
    make_ridges (fr[i], i * 1.7, 1.0);     /* pattern shifts every frame */
  n = count_events (&t, fr, 20, EGIS057E_EVENT_STABLE_IMAGE, &first);
  CHECK (n == 0, "20 frames of a sliding pattern: no STABLE_IMAGE");

  printf ("scenario 5: a half-covered sensor is never submittable\n");
  egis057e_tracker_reset (&t);
  for (int i = 0; i < 12; i++)
    make_ridges (fr[i], 0.0, 0.45);
  n = count_events (&t, fr, 12, EGIS057E_EVENT_STABLE_IMAGE, &first);
  CHECK (n == 0, "45% coverage: no STABLE_IMAGE");

  printf ("scenario 6: a replayed (byte-identical) buffer is detected\n");
  egis057e_tracker_reset (&t);
  make_ridges (fr[0], 0.0, 1.0);
  for (int i = 1; i < 10; i++)
    memcpy (fr[i], fr[0], EGIS057E_FRAME_N);
  int stale = 0, stuck = 0;
  for (int i = 0; i < 10; i++)
    {
      Egis057eEvent e = egis057e_tracker_push (&t, fr[i], NULL);
      stale += e == EGIS057E_EVENT_STALE_FRAME;
      stuck += e == EGIS057E_EVENT_STUCK;
    }
  CHECK (stale >= 1 && stuck >= 1, "identical frames flagged STALE then STUCK");
  CHECK (!t.submitted, "a replayed buffer is never submitted");
}

static int
replay (const char *path)
{
  FILE *fp = fopen (path, "rb");
  long size;
  size_t rec;
  uint8_t buf[8 + EGIS057E_FRAME_N];
  Egis057eTracker t;
  static const char *names[] = { "-", "FINGER_ON", "FINGER_OFF", "STABLE_IMAGE", "STALE", "STUCK" };

  if (!fp)
    {
      perror (path);
      return 2;
    }
  fseek (fp, 0, SEEK_END);
  size = ftell (fp);
  rewind (fp);
  rec = (size % (8 + EGIS057E_FRAME_N) == 0) ? 8 + EGIS057E_FRAME_N : EGIS057E_FRAME_N;
  printf ("replaying %ld bytes as %zu-byte records\n", size, rec);
  printf ("idx   std   cover  sharp  event\n");
  egis057e_tracker_reset (&t);
  for (int i = 0; fread (buf, 1, rec, fp) == rec; i++)
    {
      Egis057eFrameStats st;
      Egis057eEvent e = egis057e_tracker_push (&t, buf + (rec - EGIS057E_FRAME_N), &st);
      printf ("%3d %5.1f  %5.2f  %5.1f  %s\n", i, st.stddev, st.coverage, st.sharpness, names[e]);
    }
  fclose (fp);
  return 0;
}

int
main (int argc, char **argv)
{
  if (argc == 3 && strcmp (argv[1], "--frames") == 0)
    return replay (argv[2]);
  synthetic_tests ();
  printf ("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures != 0;
}
