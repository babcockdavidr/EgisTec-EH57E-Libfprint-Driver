# Originality check

The goal was an independent driver with no copied logic. This file records what was done and measured.

## Honest statement
A strict clean-room process separates the people who read the existing code from the people who write the new
code. That was not possible here: the author of this driver (an AI assistant) did read libfprint's existing
`egis0570.c` to learn the library's conventions. What this file claims is narrower: the *protocol and logic*
are different (different command set, different state machine, a finger tracker that no other libfprint driver
has) and the remaining overlap was measured and is limited to API idioms.

## Method
`tools/compare_with_reference.py` strips comments and whitespace, then reports (a) identical non-trivial
lines and (b) the share of this driver's 12-token windows that occur in each reference file, against every
driver in libfprint 1.95.2.

## Result (this driver: 330 distinct non-trivial lines, 4815 windows)
closest references (window overlap = share of OUR code found in that file):
  egis0570.c                             windows   1.3%   shared lines 8
  aes2501.c                              windows   1.0%   shared lines 8
  vcom5s.c                               windows   0.9%   shared lines 11
  aes2550.c                              windows   0.8%   shared lines 8
  vfs7552.c                              windows   0.5%   shared lines 9
  uru4000.c                              windows   0.5%   shared lines 9
  upektc.c                               windows   0.5%   shared lines 7
  aes1610.c                              windows   0.4%   shared lines 8
Lines shared with the Egis 0570 driver (8, all of them libfprint API calls that cannot be written another way):
```
#include "drivers_api.h"
fpi_image_device_close_complete (dev, error);
fpi_image_device_deactivate_complete (dev, NULL);
fpi_image_device_open_complete (dev, error);
fpi_ssm_next_state (ssm);
g_assert_not_reached ();
static const FpIdEntry id_table[] = {
switch (fpi_ssm_get_cur_state (ssm))
```
Every other shared line is also shared with many unrelated drivers (include of `drivers_api.h`, the
`FpIdEntry` table, completing open/close, SSM calls). The 0570 driver is a swipe-sensor driver with a
different protocol; nothing of its image assembly or command handling appears here.

## Licence
The files are LGPL-2.1-or-later (SPDX tag in each) so they can be merged into libfprint. Add your own
copyright line before publishing. The Egis protocol description in docs/PROTOCOL.md was derived for
interoperability with hardware the author owns; do not redistribute vendor binaries or firmware.
