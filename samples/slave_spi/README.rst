.. SPDX-License-Identifier: Apache-2.0

slave_spi — FPGA-side slave SPI smoke test
===========================================

Zephyr port of ``AgRV_pio/platforms/AgRV/examples/spi/slave_spi``. The
FPGA bitstream instantiates two user-logic slave SPIs
(``slave_spi_dma.v`` + ``slave_spi_ahb.v``) on the same pins as the
on-die SPI0/SPI1 master controllers. The MCU drives the master pair
and verifies that data round-trips through the slave logic.

Topology
--------

::

   ┌─────────────────────────────── FPGA bitstream ───────────────────────────────┐
   │                                                                              │
   │  ┌─────────────┐           ┌─────────────┐                                    │
   │  │ SPI0 master ├─PIN_1/15/─┤ sspi0 slave ├─AHB slave @ MMIO_BASE+0x00 ──┐    │
   │  │ (HW ctrl)   │  17/25    │ (dma_agm    │ CTRL/DATA + DMA reqs 0,1  │    │
   │  │             │           │   driven)   │                            │    │
   │  └─────────────┘           └─────────────┘                            │    │
   │                                                                        │    │
   │  ┌─────────────┐           ┌─────────────┐                            ▼    │
   │  │ SPI1 master ├─PIN_29/46/┤ sspi1 slave ├─AHB master @ MMIO_BASE+0x100 ┐  │
   │  │ (HW ctrl)   │  48/52    │ (logic      │ RX_ADDR/TX_ADDR (SRAM ptrs)  │  │
   │  │             │           │   writes    │                            ▼  │
   │  │             │           │   SRAM)     │                       SRAM (Zephyr)│
   │  └─────────────┘           └─────────────┘                               │  │
   │                                                                       └──┘  │
   └──────────────────────────────────────────────────────────────────────────────┘

Pin assignment
--------------

The slave_spi sample ships a standard Zephyr devicetree overlay
(``boards/agrv2k_407.overlay``) that extends the shared
``agm,agrv2k-pins`` node from ``dts/riscv/agm/agrv2k-pins.dtsi``:

* **SYSCLK 100 MHz** (override; default board dts is 200 MHz).
* **mcu-functions / mcu-pins** — adds SPI0 (on new pins) + SPI1
  (new entry; shared pins dtsi has no SPI1).
* **cpld-signals / cpld-pins / cpld-directions** — adds sspi0_*/sspi1_*
  as user-logic signals on the same pins as the master SPI pair.

``generate_board_ve.py`` reads the merged dts and emits
``<board_dir>/board.ve`` with case A (master SPI) + case B (slave SPI)
sections. See ``docs/BOARD-VE-FROM-DTS.md`` for the full architecture.

This bitstream replaces the default pin list
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A dts overlay *replaces* a whole array property, it does not append: this
overlay's ``mcu-functions`` / ``mcu-pins`` become the entire case A list,
so the shared dtsi's UART0/1, CAN0, LED and GPTIMER rows are **not** in
this sample's ``board.ve`` (measured 2026-09-12, see
``docs/BOARD-VE-FROM-DTS.md`` §4). ``GPTIMER4_CH0 PIN_15`` therefore
cannot collide with this overlay's ``SPI0_SCK PIN_15``: no dtsi edit and
no workaround are needed.

The pins listed in both ``mcu-functions`` (master SPI) and
``cpld-signals`` (slave SPI) are intentional — the bitstream joins the
two in its internal cross-bar. ``generate_board_ve.py`` prints those as
``note:`` lines and only errors when one pin is claimed twice *within*
the same array.

Build & run
-----------

::

   # 1. Generate Quartus-ready logic/ from the merged dts
   west build -b agrv2k_407 -t logic samples/slave_spi
   # → <build>/logic/board.qsf plus board.vx, board.hx, board.vex

   # 2. On a Quartus workstation:
   #      cd <build>/logic
   #      quartus_sh -t af_quartus.tcl
   #    copy simulation/modelsim/board.vo back here, then run:
   #      west build -d <build> -t bitstream      # Supra -> <build>/zephyr/board.bin

   # 3. Build + flash firmware
   west build -b agrv2k_407 samples/slave_spi
   source <your-venv>/bin/activate
   west flash -d /tmp/b_slave_spi              # both
   west flash -d /tmp/b_slave_spi --skip-bitstream # firmware only
   west flash -d /tmp/b_slave_spi --bitstream-only # bitstream only

Expected UART output
--------------------

::

   slave_spi: start testing sspi0 RX (reg mode)
   slave_spi: start testing sspi0 TX (reg mode)
   slave_spi: start testing sspi0 RX with DMA
   slave_spi: start testing sspi0 TX with DMA
   slave_spi: start testing sspi1 RX (AHB mode)
   slave_spi: start testing sspi1 TX (AHB mode)
   slave_spi: tests passed

What it tests
-------------

1. **sspi0 RX in reg mode** (``run_spi_reg``): master SPI0 pushes 16
   words via DMA; sspi0 captures them into MMIO at
   ``MMIO_BASE + 0x04`` and asserts ``SSPI_RX_VALID_BIT`` after each
   word. MCU verifies the 16 captured words match the master's TX
   buffer.

2. **sspi0 TX in reg mode**: master SPI0 reads 16 words back; sspi0
   sources the data from MCU writes to ``MMIO_BASE + 0x04``, gated by
   ``SSPI_TX_READY_BIT``. Verifies the 16 master RX words match the
   MCU's TX buffer.

3. **sspi0 RX with DMA**: ``dma_agm`` drains ``MMIO_BASE + 0x04``
   straight into SRAM on ``EXT_DMA0_REQ`` (sspi0_rx_dma_req;
   slave_spi.v:94 wires it to ``ext_dma_DMACBREQ[0]``). Master SPI0
   DMA-feeds TX on channel 7 (MSPI_DMAC_TX_CHANNEL). Verifies the
   SRAM buffer matches master's TX.

4. **sspi0 TX with DMA**: ``dma_agm`` refills ``MMIO_BASE + 0x04``
   from SRAM on ``EXT_DMA1_REQ`` (sspi0_tx_dma_req; slave_spi.v:95
   wires it to ``ext_dma_DMACBREQ[1]``). Master reads back the
   bytes on channel 6 (MSPI_DMAC_RX_CHANNEL). Verifies master RX
   matches SRAM.

5. **sspi1 RX/TX in AHB mode**: no DMA — sspi1 logic is itself the
   AHB master and reads/writes SRAM at the address programmed into
   ``MMIO_BASE + 0x104 / 0x108``. MCU programs the addresses,
   master SPI1 clocks data, and the slave logic streams bytes into
   SRAM. Verifies the SRAM buffer matches master TX / MCU-staged TX.

Why no Zephyr SPI driver?
-------------------------

The AgRV on-die SPI controllers are wired only to the FLASH boot
peripheral (``drivers/flash`` uses SPI0 directly); there is no
upstream Zephyr ``spi_agm`` driver and no need for one for FLASH
operations. For everything else, user logic exposes custom MMIO
windows (this sample's ``MMIO_BASE + 0x00..0x10C``) and the
firmware pokes those registers directly. A future ``spi_agm`` driver
would only cover the existing register window — it'd be a thin
``sys_write32`` wrapper and isn't worth the binding boilerplate
until a second user shows up.

Files in this sample
--------------------

* ``src/main.c`` — test firmware (4 functions / 5 numbered sub-cases:
  ``run_spi_reg`` covers sspi0 RX + TX, ``run_sspi0_dma_rx`` /
  ``run_sspi0_dma_tx``, ``run_sspi1_ahb`` covers sspi1 RX + TX)
* ``boards/agrv2k_407.overlay`` — sample-level pin overrides
  (extends ``agm,agrv2k-pins``; standard Zephyr dts overlay)
* ``prj.conf`` — ``CONFIG\_DMA=y`` (master SPI + SSPI DMA test,
  same pattern as ``samples/dma\_memcpy``) + standard console /
  XIP boilerplate (SERIAL / UART\_CONSOLE / PRINTK /
  STDOUT\_CONSOLE / XIP) shared with every AgRV2K sample
* ``CMakeLists.txt`` / ``sample.yaml`` — standard Zephyr sample
  boilerplate
