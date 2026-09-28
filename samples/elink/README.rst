.. SPDX-License-Identifier: Apache-2.0

elink — Pervasive Displays E2271CS091 e-paper (full refresh only)
=================================================================

Drives the **E2271CS091** (2.7", 264×176, Pervasive Displays "G2"/iTC COG
panel) from the AgRV2K. The bus runs on the **SPI1 engine**, so the sample
works on the stock 200 MHz bitstream (`samples/bitstreams`) with
no custom pin map and no bit-banging: SCL/SDA/CSB are SPI1's SCK/MOSI/CSN,
while D/C#, RST_N and the panel's BUSY_N output are GPIOs.

.. note::

   This panel family loads its waveform from the driver's OTP during init and
   **only supports full updates** — there is no partial or fast refresh.

Wiring (pins as the stock bitstream routes them)
-----------------------------------------------

================  =============  =============  =============
Panel signal      Pin            SoC signal     Notes
================  =============  =============  =============
SCL (clock)       PIN_62         SPI1_SCK       engine
SDA (data in)     PIN_63         SPI1_SI_IO0     engine MOSI
CSB (chip select) PIN_65         SPI1_CSN       engine; pulsed per byte
DC                PIN_24         GPIO6_4        board Button2 pin
RST_N             PIN_23         GPIO6_2        board Button1 pin
BUSY_N            PIN_55         GPIO6_5        board Key pin
console           PIN_69 / 68    UART0          RX / TX
================  =============  =============  =============

`dts/bindings/display/agm,epd-g2.yaml` describes the front end (SPI child node
plus the three GPIOs and the geometry); `boards/agrv2k_407.overlay` carries the
pin list, which is exactly the SPI1 + GPIO6 set the stock bitstream already
routes. Swapping a pin is one line in that file (pin list *and* the `*-gpios`
property), and `west build -t logic` then reproduces the same routing in a new
bitstream if one is ever needed.

Why SPI1 and not bit-banging: the engine shifts the bytes (8 MHz) and asserts
CSN for exactly one transfer, which is what the panel wants -- see below.
The bit-banged first revision of this sample spent ~100 µs on a byte the
engine sends in ~1 µs plus the driver's per-transfer overhead.

Where the sequence comes from
-----------------------------

The flow is taken from Pervasive's *Application Note for small size Monochrome
EPD with iTC (OTP LUT)*, rev 02
(`$HOME/ApplicationNote_Small_Size_Mono_v02_220606.pdf`) — sections 2..5
cover power-on, temperature/PSR, the image transfer and the update command — and
the pin list plus the ``BUSY_N`` semantics from the panel datasheet `1P186-00_02`
(`$HOME/1P186-00_02_E2271CS091_20180817.pdf`).

It is then cross-checked against `$HOME/esp-elink-main/` (Marcelo Barros
de Almeida, MIT), an ESP-IDF driver that runs **this same panel**. Its value
here is mostly negative information: it never writes the DC/DC setting
(``0x01``), booster soft start (``0x06``) or PLL control (``0x30``) registers,
so the COG is happy on its OTP defaults and this sample does not write them
either.

Status (dev board-verified 2026-09-17, agrv2k_407, stock 200 MHz bitstream)
----------------------------------------------------------------------

Implemented and measured:

* the SPI format the note specifies, which is **not** the usual one: every byte
  — the register index and each data byte — gets its **own CS# pulse** ("if
  register data is more than one byte, the CS# pulse is necessary between each
  data byte"). The engine asserts CSN per transfer, so the sample issues one
  ``spi_write()`` per byte and gets exactly that framing. esp-elink instead
  runs a whole frame under one CS# and lets the ESP32 hardware drive it;
* the power-on sequence: RES# low → high, then the soft reset ``0x00 <- 0x0E``
  and a BUSY wait;
* environment temperature and panel settings (note section 3): TSSET (``0xE5``)
  with the note's encoding (25 °C = ``0x19``; the top bit is the sign, negatives
  in two's complement — this sample has no sensor and pins 25 °C), the active
  temperature ``0xE0 <- 0x02``, then PSR (``0x00``) = ``0xCF, 0x8D`` (the note's
  value for every size except 2.9" HR and the 3.7"/4.2"/4.37" group);
* the **two-frame** image transfer (note section 4): ``0x10`` + the image
  (1 = black), then ``0x13`` + the same number of ``0x00`` bytes — 5 808 bytes
  each for the 2.7" (176 pixels per line × 264 lines: the COG's frame memory is
  transposed relative to the 264×176 glass, and the sample renders into it that
  way);
* the update command (note section 5): ``0x04`` DC/DC on → BUSY → ``0x12``
  display refresh → BUSY → ``0x02`` DC/DC off, after which the bus is parked
  (DC and RES# low) exactly as the reference leaves it;
* a two-phase BUSY wait: a bounded window for ``BUSY_N`` to go low, then a
  bounded wait for it to come back high. Waiting for the assertion first is what
  makes an unattached panel visible instead of a silent no-op (the reference
  only waits for the release);
* a test image — border plus every 16th line — and PASS/FAIL lines with the
  elapsed time.

Measured on the dev board: the two 5 808-byte frames go out in **180-215 ms**
(the engine's per-transfer overhead dominates an 8 MHz byte), the DC/DC-on
step returns in tens of milliseconds, and the sample reaches
``RESULT: PASS`` after the refresh — a full capture with
``tools/test_uart_capture.sh -t 20`` reads::

   elink: E2271CS091 264x176, 176x264 frame of 5808 bytes, spi@40013000 at 8000000 Hz
   elink: DC=gpio@4001a000.4 RST_N=gpio@4001a000.2 BUSY_N=gpio@4001a000.5
   elink: init sent -- soft reset 0x0e, TSSET 0x19 (25 C), active temp 0x02, PSR 0xcf,0x8d
   elink: two 5808-byte frames out in 180 ms, DC/DC on; refreshing
   elink: RESULT: PASS -- two 5808-byte frames pushed in 180 ms and the DC/DC cycled; ...

One earlier capture stopped after the "refreshing" line while the panel's boost
current was drawing (the board also showed a fresh boot banner in a 90 s
window), which is why the progress line is printed *before* the refresh: a log
that ends there still says the bus and both frames were fine.

Not implemented: partial/fast updates (this panel has none) and a real
temperature sensor for TSSET (the note reads one; this pins 25 °C).

Build / run
-----------

.. code-block:: sh

   source <your-venv>/bin/activate
   cd $HOME/zephyrproject
   west build -d /tmp/b_elink -b agrv2k_407 --pristine=auto $HOME/zephyr-hal-ag32/samples/elink
   # Plan A (recommended): single west flash writes both firmware + bitstream.
   # The bitstream defaults to ${CMAKE_BINARY_DIR}/zephyr/board.bin -- generate
   # it with `west build -t bitstream` after Quartus synthesises logic/.
   west flash -d /tmp/b_elink
   # Or, point straight at the SDK reference bitstream without copy:
   #   AGM_BITSTREAM_BIN=<your canonical bitstream> \
   #       west flash -d /tmp/b_elink
   bash $HOME/zephyr-hal-ag32/tools/test_uart_capture.sh -t 20
