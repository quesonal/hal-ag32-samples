.. SPDX-License-Identifier: Apache-2.0

elink_monitor — Pervasive Displays E2271CS091 e-paper monitoring console
=========================================================================

Same panel, same wiring, same init/update flow as ``samples/elink``, but where
elink runs once and exits, this one is a *loop*: every 15 s it re-renders a
"monitoring console" page (CPU / MEM / TEMP / LOAD / NET TX / NET RX tiles, a
CPU sparkline, a status footer) and pushes a fresh full refresh to the panel.

The numbers are synthesised from a small xorshift32 PRNG seeded by the cycle
index. The same cycle number always draws the same numbers, which is handy
for dev board comparisons across runs; deltas look plausible on the dashboard.

Why 15 s: a full refresh on this family is ~5 s of Tr on top of the ~200 ms
bus transfers, so 15 s leaves ~10 s of "fresh frame" between paints. The panel
only supports full refresh (no partial / fast), so the page that gets written
is the page that stays on the glass until the next 15 s tick. No countdown is
rendered -- anything drawn would just lie while the next refresh is brewing.

Layout (176x264 frame, mapped to 264x176 glass by the COG)
-----------------------------------------------------------

::

   y=0       ZEPHYR MON                 (2x scale, ~110 px wide)
   y=14      #0014 14:23:47             (1x scale, uptime from k_uptime_get)
   y=26      ───────────────────────     (divider)
   y=29..68  CPU 42% [████░] | MEM 18KB [██░░░]
   y=71..110 TEMP 35C [████░] | LOAD 0.86 [███░]
   y=114     CPU 60s
   y=124..171 ▁▂▃▅▆▇█▇▆▅▃▂ sparkline (170 samples)
   y=175..214 TX 1234 [██░░] | RX 567 [█░░░]
   y=217     ───────────────────────     (divider)
   y=220     err 0  can ACT
   y=232     tx 892KB rx 421KB
   y=244     ───────────────────────
   y=250     PASS                        (centre, 2x)

The sparkline is a 170-sample circular buffer of CPU%, 1 px per sample, oldest
on the left. With a 15 s tick the visible strip is the last ~42 minutes.

Wiring, init and the two-frame + 0x04 + 0x12 + 0x02 update are bit-for-bit the
same as ``samples/elink``. The overlay in ``boards/agrv2k_407.overlay`` is also
identical to elink's, so the stock 200 MHz bitstream serves both.

Why no partial / fast refresh
------------------------------

The E2271CS091 belongs to Pervasive's G2/iTC COG family, which loads its
waveform from the driver's OTP during init and only supports full updates. So
the "monitoring tick" here is exactly the elink sample's whole-frame refresh,
every 15 s -- the user's perception of "15 s refresh" matches reality.

Build / run
-----------

.. code-block:: sh

   source <your-venv>/bin/activate
   cd $HOME/zephyrproject
   west build -d /tmp/b_elink_monitor -b agrv2k_407 --pristine=auto \
       $HOME/zephyr-hal-ag32/samples/elink_monitor
   # Plan A (recommended): single west flash writes both firmware + bitstream.
   # The bitstream defaults to ${CMAKE_BINARY_DIR}/zephyr/board.bin -- generate
   # it with `west build -t bitstream` after Quartus synthesises logic/.
   west flash -d /tmp/b_elink_monitor
   bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh -t 60
