# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

# The fabric IP this sample's pin map uses, *declared by name* (the same shape
# as the vendor's platformio.ini `ip_name = full_duplex_spi`). The sources come
# from the convention next to it: ip/<name>.v (+ ip/<name>_core.v if the IP is
# split), which board_common.cmake resolves and hands to build_bitstream.sh as
# AGM_USER_RTL -- staged into <build>/logic and registered in board.qsf (see
# docs/CUSTOM-IP.md). Pass -DAGM_USER_RTL=<path> to use a file from elsewhere,
# or -DAGM_LOGIC_IP= to build without an IP.
set(AGM_LOGIC_IP "full_duplex_spi")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(spi_quad_read LANGUAGES C)

target_sources(app PRIVATE src/main.c)
