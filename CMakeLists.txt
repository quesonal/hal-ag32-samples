# Placeholder top-level CMakeLists.txt for the hal_ag32_samples module.
# When west places this repo at <ws>/modules/hal_ag32_samples, Zephyr's
# module-extension machinery finds each samples/<sample>/CMakeLists.txt
# via west build's -S argument; this file exists so the module has a
# CMake target root.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hal_ag32_samples)
