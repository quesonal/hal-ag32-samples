# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

# Two fabric IPs in one bitstream (docs/CUSTOM-IP.md 1.5: the fabric has a
# single IP slot, so the IP that gen_vlog instantiates is a *wrapper*).
#
# AGM_LOGIC_IP names that wrapper: it is what build_bitstream.sh passes to
# gen_vlog as `-m <name>.v` (one macro per run) and what the IP-prepare step
# in <build>/logic_ip/ is called after. The other IPs are ordinary source
# files: AGM_USER_RTL is a list, every entry is staged into <build>/logic and
# registered in board.qsf, and Quartus resolves the sub-modules from there.
set(AGM_LOGIC_IP "dual_ip")
set(AGM_USER_RTL
    "${CMAKE_CURRENT_SOURCE_DIR}/ip/dual_ip.v"
    "${CMAKE_CURRENT_SOURCE_DIR}/ip/full_duplex_spi.v"
    "${CMAKE_CURRENT_SOURCE_DIR}/ip/custom_ip.v"
    "${CMAKE_CURRENT_SOURCE_DIR}/ip/ram2ahb.v"
    "${CMAKE_CURRENT_SOURCE_DIR}/ip/ahb2ram.v")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dual_ip LANGUAGES C)

target_sources(app PRIVATE src/main.c src/ram_window.c)
