# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(usb_msc_dfu)

# Reuse the upstream USB sample common code (sample_usbd.h +
# sample_usbd_init.c) instead of forking it into hal_ag32.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
