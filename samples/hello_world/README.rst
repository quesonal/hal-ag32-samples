.. _hello_world-agm:

AGM AgRV2K hello_world
######################

This sample is the AgRV2K counterpart of the standard Zephyr
``hello_world`` smoke test. It validates the end-to-end integration
of Zephyr on the ``agrv2k_407`` board:

* the **UART0** console prints a boot banner (native PL011-register-compatible
  driver, pinctrl stub, ``pinctrl_configure_pins`` no-op);
* the **CLINT MTIME** ``k_msleep`` advances (proves the riscv-machine-timer
  driver is bound and the timer ISR fires);
* a **direct-register LED1** (GPIO4_1 / PIN_51) toggles every 500 ms
  (2026-09-10 改绑:原 PIN_34 → PIN_51,比特流已就位);
* a **polled IO_Button1** (GPIO6_2 / PIN_23) press prints "button
  pressed" on transition.

It is the smallest sample that exercises every peripheral currently
exposed to Zephyr by ``hal_ag32`` — see ``boards/agm/agrv2k_407/doc/index.rst``
for the full list.

Design notes
************

The sample is deliberately kept short and free of AgRV-proprietary
SDK dependencies. All peripherals used are bound to upstream Zephyr
drivers (``drivers/serial/uart_agm.c``(本仓库 native,PL011 寄存器布局) ,
``drivers/timer/riscv_machine_timer.c``).

GPIO access is *direct register access*, not through
``drivers/gpio/gpio_stellaris.c``. The reason is the PLIC vs MTI
collision documented in the header of ``src/main.c``: GPIO bank 0's
PLIC IRQ is 7, which is the same value as the CPU-local MTI exception
(also 7), so enabling ``ti,stellaris-gpio`` on this SoC trips
``gen_isr_tables.py`` with "multiple registrations at table_index 7
for irq 7". Wiring up a proper PLIC-aware ``__soc_handle_irq`` is
non-trivial (it has to translate MEIP into a per-source PLIC slice
on the way into ``_sw_isr_table``) and was deferred from MVP. Until
that lands, the sample drives the same register layout directly at
the SoC base addresses:

* ``SYS_BASE + 0x60`` — APB clock enable (``APB_MASK_GPIO0``,
  ``APB_MASK_GPIO6``)
* ``GPIOx_BASE + 0x51C`` — digital enable (``GPIO_DEN``)
* ``GPIOx_BASE + 0x400`` — direction (``GPIO_DIR``)
* ``GPIOx_BASE + 0x510`` — pull-up (``GPIO_PUR``)
* ``GPIOx_BASE + 0x000`` — data (``GPIO_DATA``)

The button is polled rather than interrupt-driven for the same
reason: routing a button interrupt through PLIC source 13 would
require the same PLIC-aware trap handler that we just deferred.

Pin mappings
************

The pin numbers (``PIN_51`` for LED1, ``PIN_23`` for the button) are
baked into the FPGA bitstream (``board.ve``); they are not visible to
the firmware at runtime. The 407 ``board.dts`` declares the standard
Zephyr aliases (``led0``..``led3``, ``sw0``) directly; the
``&gpio4`` bank is enabled to drive LED1..4, ``&gpio6`` for
IO_Button1.

.. list-table::
   :header-rows: 1
   :widths: 20 20 20 40

   * - Alias
     - Bank
     - Bit
     - SoC pin (from .ve)
   * - ``led0``
     - gpio0 (GPIO4)
     - 1
     - ``GPIO4_1 PIN_51`` (LED1)
   * - ``sw0``
     - gpio6
     - 2
     - ``GPIO6_2 PIN_23`` (IO_Button1)

Building and running
********************

Prerequisites — see ``boards/agm/agrv2k_407/doc/index.rst`` for the
SDK + Supra + AgRV-patched-openocd requirements.

::

   west build -b agrv2k_407 modules/hal_ag32/samples/hello_world
   west flash --runner openocd
   minicom -D /dev/ttyUSB0 -b 115200

Expected console output::

   *** Booting Zephyr OS build v4.4.99 …

   === AgRV2K hello_world ===
   Board : agrv2k_407
   SoC   : AgRV2K (RV32IMAFC @ 240 MHz)
   UART  : PL011 @ 0x40025000 (console)
   LED   : GPIO4_1 @ 0x40018000 (PIN_51)
   BTN   : GPIO6_2 @ 0x4001a000 (PIN_23)
   =========================

   tick 1
   tick 2
   button pressed (tick=4)
   tick 3
   …

LED1 (next to the USB-C connector on agrv2k_407) toggles at 1 Hz;
each press of IO_Button1 prints a "button pressed" line on the next
main-loop tick.

Expected artifacts::

   build/zephyr/zephyr.elf      ~340 KB   (with debug sections)
   build/zephyr/zephyr.bin       ~20 KB   (raw firmware image)
   boards/agm/agrv2k_407/board.bin ~100 KB   (FPGA bitstream)

Memory region usage at this sample::

   text    20468 B
   data       84 B
   bss      7284 B   (~21 % of 128 KB SRAM)

This sample is the MVP acceptance test for ``hal_ag32``: if the banner
appears, the LED blinks, and the button triggers a printed message,
the SoC + board wiring is correct and the FPGA bitstream was loaded
with the right pin mux.