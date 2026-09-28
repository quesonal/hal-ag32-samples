// SPDX-License-Identifier: Apache-2.0
//
// dual_ip — two fabric IPs behind the fabric's single IP slot.
//
// The AgRV2K fabric attaches exactly *one* IP macro to the MCU: alta_rv32_top
// has one mem_ahb_* slave port (MCU -> IP), one slave_ahb_* master port
// (IP -> MCU bus), one ext_dma_* and one local_int (see the generated
// alta_rv32_top.v, and the pin-routing notes 12.14/3.27.20). The SDK picks
// that macro with `gen_vlog -m <ip>.v`, whose -m option is single-valued, so
// two IPs cannot both be attached: they go inside one wrapper. This is that
// wrapper -- the vendor's own shape for it is examples/custom_ip (an IP that
// instantiates a sub-IP), see docs/CUSTOM-IP.md 1.5.
//
// Its port list is the one gen_vlog writes into <ip>_tmpl.v from the pin map:
// the standard macro interface plus the SPI IP's pins (csn/sck/si_io0/so_io1
// and their *_out_data/_out_en). The IP's own pins are NOT brought out: the
// RAM block needs none of them for the test (its trigger is tied low, so its
// master port stays idle).
//
// Window split (the MCU reaches both through 0x60000000+, cpld0):
//
//   haddr[15:11] == 0  -> custom_ip's RAM (MMIO_BASE + 0x000..0x7FF below)
//   everything else    -> full_duplex_spi, i.e. byte-for-byte the wiring it
//                         has when it is the only IP
//
// The unselected slave is handed an IDLE transfer instead of the real one, so
// it cannot latch a write; only the selected slave drives hreadyout/hrdata.
//
// Why the RAM is instantiated with RAM_SIZE = 2048 and not the vendor's 4096:
// the user-logic LogicLock region has room for 4 M9K blocks and the vendor's
// own qsf enforces it (`set_global_assignment -name MAX_RAM_BLOCKS_M4K 4`,
// present in the reference project too). This design needs the SPI IP's RX
// FIFO (lpm_width 32 x lpm_numwords 256 = 8 Kbit -> 1 M9K) *and* the RAM, and
// a 4 KiB RAM is 32 Kbit -> 4 M9K, i.e. one block too many:
//
//     Quartus: "You have limited the RAM location(s) of type M9K to 4.
//               However, the current design needs more than 4 to fit"
//
// 2 KiB is 16 Kbit -> 2 M9K, so the design sits at 3 of 4 and both IPs stay.
// RAM_SIZE is a parameter of the vendor module and ADDR_BITS follows it, so
// this is the supported way to trim it; the vendor example's 4 KiB does fit,
// but only because it is the design's single IP.

`timescale 1ns/1ps

module dual_ip (
  output tri0        csn,
  output tri0        sck,
  output tri0        si_io0,
  input              so_io1,
  input              csn_out_data,
  input              csn_out_en,
  input              sck_out_data,
  input              sck_out_en,
  input              si_io0_out_data,
  input              si_io0_out_en,
  input              sys_clock,
  input              bus_clock,
  input              resetn,
  input              stop,
  input       [1:0]  mem_ahb_htrans,
  input              mem_ahb_hready,
  input              mem_ahb_hwrite,
  input       [31:0] mem_ahb_haddr,
  input       [2:0]  mem_ahb_hsize,
  input       [2:0]  mem_ahb_hburst,
  input       [31:0] mem_ahb_hwdata,
  output tri1        mem_ahb_hreadyout,
  output tri0        mem_ahb_hresp,
  output tri0 [31:0] mem_ahb_hrdata,
  output tri0        slave_ahb_hsel,
  output tri1        slave_ahb_hready,
  input              slave_ahb_hreadyout,
  output tri0 [1:0]  slave_ahb_htrans,
  output tri0 [2:0]  slave_ahb_hsize,
  output tri0 [2:0]  slave_ahb_hburst,
  output tri0        slave_ahb_hwrite,
  output tri0 [31:0] slave_ahb_haddr,
  output tri0 [31:0] slave_ahb_hwdata,
  input              slave_ahb_hresp,
  input       [31:0] slave_ahb_hrdata,
  output tri0 [3:0]  ext_dma_DMACBREQ,
  output tri0 [3:0]  ext_dma_DMACLBREQ,
  output tri0 [3:0]  ext_dma_DMACSREQ,
  output tri0 [3:0]  ext_dma_DMACLSREQ,
  input       [3:0]  ext_dma_DMACCLR,
  input       [3:0]  ext_dma_DMACTC,
  output tri0 [3:0]  local_int
);

  // ---- window decode -------------------------------------------------
  wire ram_sel = (mem_ahb_haddr[15:11] == 5'd0);

  wire [1:0] spi_htrans = ram_sel ? 2'b00 : mem_ahb_htrans;
  wire       spi_hwrite = ram_sel ? 1'b0  : mem_ahb_hwrite;
  wire [1:0] ram_htrans = ram_sel ? mem_ahb_htrans : 2'b00;
  wire       ram_hwrite = ram_sel ? mem_ahb_hwrite : 1'b0;

  wire        spi_hreadyout;
  wire        spi_hresp;
  wire [31:0] spi_hrdata;
  wire        ram_hreadyout;
  wire        ram_hresp;
  wire [31:0] ram_hrdata;

  assign mem_ahb_hreadyout = ram_sel ? ram_hreadyout : spi_hreadyout;
  assign mem_ahb_hresp     = ram_sel ? ram_hresp     : spi_hresp;
  assign mem_ahb_hrdata    = ram_sel ? ram_hrdata    : spi_hrdata;

  // ---- IP 1: the full-duplex SPI wrapper (unchanged wiring) ----------
  full_duplex_spi u_spi (
    .csn                (csn                ),
    .sck                (sck                ),
    .si_io0             (si_io0             ),
    .so_io1             (so_io1             ),
    .csn_out_data       (csn_out_data       ),
    .csn_out_en         (csn_out_en         ),
    .sck_out_data       (sck_out_data       ),
    .sck_out_en         (sck_out_en         ),
    .si_io0_out_data    (si_io0_out_data    ),
    .si_io0_out_en      (si_io0_out_en      ),
    .sys_clock          (sys_clock          ),
    .bus_clock          (bus_clock          ),
    .resetn             (resetn             ),
    .stop               (stop               ),
    .mem_ahb_htrans     (spi_htrans         ),
    .mem_ahb_hready     (mem_ahb_hready     ),
    .mem_ahb_hwrite     (spi_hwrite         ),
    .mem_ahb_haddr      (mem_ahb_haddr      ),
    .mem_ahb_hsize      (mem_ahb_hsize      ),
    .mem_ahb_hburst     (mem_ahb_hburst     ),
    .mem_ahb_hwdata     (mem_ahb_hwdata     ),
    .mem_ahb_hreadyout  (spi_hreadyout      ),
    .mem_ahb_hresp      (spi_hresp          ),
    .mem_ahb_hrdata     (spi_hrdata         ),
    .slave_ahb_hsel     (slave_ahb_hsel     ),
    .slave_ahb_hready   (slave_ahb_hready   ),
    .slave_ahb_hreadyout(slave_ahb_hreadyout),
    .slave_ahb_htrans   (slave_ahb_htrans   ),
    .slave_ahb_hsize    (slave_ahb_hsize    ),
    .slave_ahb_hburst   (slave_ahb_hburst   ),
    .slave_ahb_hwrite   (slave_ahb_hwrite   ),
    .slave_ahb_haddr    (slave_ahb_haddr    ),
    .slave_ahb_hwdata   (slave_ahb_hwdata   ),
    .slave_ahb_hresp    (slave_ahb_hresp    ),
    .slave_ahb_hrdata   (slave_ahb_hrdata   ),
    .ext_dma_DMACBREQ   (ext_dma_DMACBREQ   ),
    .ext_dma_DMACLBREQ  (ext_dma_DMACLBREQ  ),
    .ext_dma_DMACSREQ   (ext_dma_DMACSREQ   ),
    .ext_dma_DMACLSREQ  (ext_dma_DMACLSREQ  ),
    .ext_dma_DMACCLR    (ext_dma_DMACCLR    ),
    .ext_dma_DMACTC     (ext_dma_DMACTC     ),
    .local_int          (local_int          )
  );

  // ---- IP 2: the AHB RAM block (vendor examples/custom_ip) -----------
  //
  // MCU side answers the decoded window. Its pin-driven master port is parked:
  // ip_pin_out_en/_data tied low means ram_trigger never rises, so nothing is
  // issued on slave_ahb_*; those outputs are therefore left unconnected and
  // the inputs it would read back are tied to the idle response.
  custom_ip #(.RAM_SIZE(2048)) u_ram (
    .top_in             (1'b0              ),
    .top_inout          (                  ),
    .top_out            (                  ),
    .ip_inout_in        (                  ),
    .ip_inout_out_data  (1'b0              ),
    .ip_inout_out_en    (1'b0              ),
    .ip_pin_in          (                  ),
    .ip_pin_out_data    (1'b0              ),
    .ip_pin_out_en      (1'b0              ),
    .sys_clock          (sys_clock         ),
    .bus_clock          (bus_clock         ),
    .resetn             (resetn            ),
    .stop               (stop              ),
    .mem_ahb_htrans     (ram_htrans        ),
    .mem_ahb_hready     (mem_ahb_hready    ),
    .mem_ahb_hwrite     (ram_hwrite        ),
    .mem_ahb_haddr      (mem_ahb_haddr     ),
    .mem_ahb_hsize      (mem_ahb_hsize     ),
    .mem_ahb_hburst     (mem_ahb_hburst    ),
    .mem_ahb_hwdata     (mem_ahb_hwdata    ),
    .mem_ahb_hreadyout  (ram_hreadyout     ),
    .mem_ahb_hresp      (ram_hresp         ),
    .mem_ahb_hrdata     (ram_hrdata        ),
    .slave_ahb_hsel     (                  ),
    .slave_ahb_hready   (                  ),
    .slave_ahb_hreadyout(1'b1              ),
    .slave_ahb_htrans   (                  ),
    .slave_ahb_hsize    (                  ),
    .slave_ahb_hburst   (                  ),
    .slave_ahb_hwrite   (                  ),
    .slave_ahb_haddr    (                  ),
    .slave_ahb_hwdata   (                  ),
    .slave_ahb_hresp    (1'b0              ),
    .slave_ahb_hrdata   (32'b0             ),
    .ext_dma_DMACBREQ   (                  ),
    .ext_dma_DMACLBREQ  (                  ),
    .ext_dma_DMACSREQ   (                  ),
    .ext_dma_DMACLSREQ  (                  ),
    .ext_dma_DMACCLR    (4'b0              ),
    .ext_dma_DMACTC     (4'b0              ),
    .local_int          (                  )
  );

endmodule
