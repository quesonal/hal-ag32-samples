.. SPDX-License-Identifier: Apache-2.0

lcd_40pin — 40-pin LCD adapter on a bit-banged 8080 bus
======================================================

Drives the 40-pin adapter board (``转接板``) that carries the
``FPC4301MS-L190-40C-9A`` LCM — **HX8369A**, 480×800, 4.3" — from the
AgRV2K. The adapter brings the panel out as the generic 40-pin pinout:
a 16-bit 8080 (MIPI DBI type A) bus, a backlight input, and a capacitive
touch section (GT911 on I2C).

The AgRV2K has no LCD controller, so every adapter pin is a **GPIO**: the
FPGA fabric only routes pins to GPIO bits (``GPIOb_n`` has no AFSEL bit, so
these rows carry no ``agm,pins`` cell). That means this sample owns its pin
list — the bitstream has to be rebuilt from it (``west build -t logic``).

Adapter pin → AgRV2K pin (proposal, see below)
----------------------------------------------

==============  =====================  ===========  =============
Adapter (40pin) Signal                 GPIO         AgRV2K pin
==============  =====================  ===========  =============
1               CS                     gpio3.0      PIN_15
2               RS / DC                gpio3.1      PIN_16
3               WR                     gpio3.2      PIN_17
4               RD                     gpio3.3      PIN_18
5               RST                    gpio6.2      PIN_23
6..13           DB0..DB7               gpio1.0-7    PIN_25,26,29..34
14..21          DB8..DB15              gpio2.0-7    PIN_35,36,38..41,91,1
22              GND                    —            —
23              BL_CTR                 gpio6.4      PIN_89
24,25           VCC3.3                 —            —
26,27           GND                    —            —
28              VCC5                   —            —
29              CTP_SDA (GT911)        i2c0 SDA     PIN_3
30              CTP_SCL (GT911)        i2c0 SCL     PIN_2
31              CTP_INT                gpio5.0      PIN_4
32              CTP_RST                gpio5.1      PIN_5
==============  =====================  ===========  =============

(UART0 stays where the board has it: ``PIN_69`` RX / ``PIN_68`` TX — the
sample prints over it.)

**The pins are a proposal.** The adapter's pin *names* come from the module
documentation; which AgRV2K pin each one lands on is a wiring decision, and
this sample is where it is written down (``boards/agrv2k_407.overlay``, the
``mcu-functions``/``mcu-pins`` arrays and the ``lcd0`` node — change both
together). The build already checks both ends: ``generate_board_ve.py``
refuses pins the package does not have (it caught five in the first draft),
and ``../../tools/check_pin_routing.sh`` keeps the pin list and any committed
fragment in step.

Build
-----

.. code-block:: sh

   source <your-venv>/bin/activate
   cd $HOME/zephyrproject
   west build -d /tmp/b_lcd -b agrv2k_407 $HOME/zephyr-hal-ag32/samples/lcd_40pin
   # Plan A: one command instead of separate flash_fw.sh + flash_logic.sh
   west flash -d /tmp/b_lcd
   bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh -t 30

``sample.yaml`` is ``build_only``: the firmware needs a bitstream that
routes the adapter's pins, so it cannot run on the board's current
(canonical) bitstream as it is.

What this revision does
-----------------------

* configures the bus GPIOs, pulses ``RST``, and writes the head of the
  vendor's init sequence: ``SWRESET`` (0x01), ``EXTC`` unlock (0xB9 =
  FF 83 69), ``COLMOD`` = 0x55 (16-bit RGB565), ``SLPOUT`` (0x11),
  ``DISPON`` (0x29);
* paints four full-screen colours (black / red / green / blue) through
  ``CASET``/``PASET``/``RAMWR`` so the panel shows something to look at;
* prints what it did and asks for eyes on the panel:
  ``RESULT: INCONCLUSIVE```.

Not done yet (tracked in the the development notes (not published here)):

* the vendor's **full** register sequence (power/gamma tables) from
  ```LCM/C51调试例程/8369A_LG4.3 MCU 16bit IM2-0设置的110，选DB0-15.c``;
* panel **ID read-back** (0x04 / 0xDA-0xDC — needs the RD path and
  tri-stating DB), which is what would turn the result into PASS/FAIL
  without a human;
* touch reporting: the GT911 is described in DT and its driver is enabled,
  but nothing reads points yet;
* the RGB path: this sample is the MCU-8080 one; the fabric RGB route
  (vendor ``HyperRAM/rgb`` reference + Zephyr ``panel-timing``) needs a
  framebuffer and is a separate decision.

Bring-up status (2026-09-16) -- work in progress
-----------------------------------------------

This sample is a bring-up vehicle, not a finished demo. What is *verified on
the dev board* so far:

* the fabric really maps the adapter pins to the GPIO bits this sample drives
  (the generated wrapper matches the vendor's working board line for line), the
  bank gates are open (``APB_CLKENABLE=0c201fe1``), DIR/AFSEL are right, and a
  written byte reads back (``DB self-check: gpio1 DATA=f8``); DB0..DB7 toggle
  as an ~2 Hz square wave on a multimeter;
* the panel is an **8-bit MCU (8080) part** (adapter straps ``IM[2:0]=100``) and
  the init sequence is the panel vendor's own 16-command table (the newest of
  the three vendor tables; ``0x3A=0x55``` = RGB565);
* CS/RS/WR/RD/RST must be driven with *logical* levels -- writing raw 0/1 kept
  CS high and held the panel in reset for the first several attempts;
* measured with the CPU writing the bus directly (mask-addressed register
  stores, i.e. one store per byte, no GPIO API): **376 ms per 480x800 frame,
  2.6 fps**. 60 fps would need 46 MB/s and is out of reach for a bit-banged
  8080 -- it needs a fabric scan-out engine (see the the development notes (not published here));
* the panel's column order is the mirror of the frame-memory order (```0x36``
  does not change it), and only ~400 of the 480 driver columns appear on the
  glass; the frame memory is 480x864, so unwritten rows/columns keep the
  previous run's content.

Runtime diagnostics (the "hram_test" method, applied to a 8080 panel):
``RDDID``/``RDDST`` read-back, a 64x64 GRAM write-then-``0x2E`` read-back
compare, a full-GRAM wipe with its bandwidth, and a leftover probe outside the
visible area. The test image is the ruler: white frame, red verticals and green
horizontals 20 pixels inside the edges.

Open items (work paused here, 2026-09-16): the dev board panel only showed part of
the written area and its column order did not follow the frame memory (mirror-
like, not monotonic), so the geometry knobs in ``src/main.c``
(``PANEL_WIDTH`` / ``PANEL_X_OFF`` / ``PANEL_MIRROR``) are left at their plain
defaults until a known-good panel can re-measure them. Touch (GT911) is
described in devicetree and its driver probes, but nothing reads points yet.

