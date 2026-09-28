# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(usb_hid_mouse)

# Reuse the upstream USB samples' shared usbd-init helper (sample_usbd.h
# + sample_usbd_init.c) — no need to fork those into hal_ag32.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
