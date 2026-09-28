# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(spi_boot_loader)

target_sources(app PRIVATE src/main.c)

target_include_directories(app PRIVATE src)

# Optional: the public key the driver verifies signed images with. Required when
# the build selects a BOOT_AGM_SIG_* profile, and written by
# `tools/sign_image.py --pubkey-out` in the form that profile keeps: raw 64-byte
# X||Y for ECDSA P-256, 270-byte PKCS#1 DER for RSA-2048 (AGM_BOOT_PUBKEY_LEN in
# include/zephyr/drivers/misc/boot_agm.h). The driver's weak default is all
# zeroes, which accepts nothing.
# STRING, not FILEPATH: CMake turns a relative FILEPATH cache value into an
# absolute one relative to the *current working directory* (the west
# workspace, for `west build`) before this file ever runs, which would silently
# point at the wrong place. Keeping it a string lets the resolution below use
# this directory -- so scenarios and hand-written commands can say
# `-DSPI_BOOT_PUBKEY=fixtures/ci_production.pub` and mean the sample's own
# fixtures/.
set(SPI_BOOT_PUBKEY "" CACHE STRING "trusted public key for signed builds (absolute, or relative to this directory)")
if(SPI_BOOT_PUBKEY AND NOT IS_ABSOLUTE "${SPI_BOOT_PUBKEY}")
	set(SPI_BOOT_PUBKEY "${CMAKE_CURRENT_SOURCE_DIR}/${SPI_BOOT_PUBKEY}")
endif()
if(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
	set(_pub_expect 270)
	set(_pub_what "270-byte PKCS#1 RSAPublicKey DER")
else()
	set(_pub_expect 64)
	set(_pub_what "raw 64-byte X||Y key")
endif()
if(SPI_BOOT_PUBKEY)
	if(NOT EXISTS "${SPI_BOOT_PUBKEY}")
		message(FATAL_ERROR "SPI_BOOT_PUBKEY=${SPI_BOOT_PUBKEY} does not exist")
	endif()
	file(READ "${SPI_BOOT_PUBKEY}" _pub_hex HEX)
	string(LENGTH "${_pub_hex}" _pub_hex_len)
	math(EXPR _pub_len "${_pub_hex_len} / 2")
	if(NOT _pub_len EQUAL ${_pub_expect})
		message(FATAL_ERROR
			"SPI_BOOT_PUBKEY must be the ${_pub_what}, got ${_pub_len} B")
	endif()
	string(REGEX REPLACE "(..)" "0x\\1," _pub_bytes "${_pub_hex}")
	set(_pub_c "${CMAKE_CURRENT_BINARY_DIR}/spi_boot_pubkey.c")
	file(WRITE "${_pub_c}"
"/* Generated from ${SPI_BOOT_PUBKEY} -- do not edit. */
#include <stdint.h>

const uint8_t agm_boot_pubkey[${_pub_expect}] = {
${_pub_bytes}
};
")
	# In the app target, not libzephyr: the driver ships a weak all-zeroes
	# default, and a strong definition inside the same archive would
	# never be picked over it.
	target_sources(app PRIVATE "${_pub_c}")
	message(STATUS "spi_boot_loader: trusted public key from ${SPI_BOOT_PUBKEY}")
endif()

# The production profile (CONFIG_BOOT_AGM_PRODUCTION_PROFILE) gates every
# state change behind a command signed with that key. Building it without one
# produces the exact board the production-lock design 6.4 warns about: it
# boots, it answers, and nothing can ever update it again in-band. The driver
# prints that at boot for applications that wire it up themselves; here, where
# the key is a build input, the build stops instead.
if(CONFIG_BOOT_AGM_PRODUCTION_PROFILE AND NOT SPI_BOOT_PUBKEY)
	message(FATAL_ERROR
		"CONFIG_BOOT_AGM_PRODUCTION_PROFILE=y needs a trusted public key: "
		"pass -DSPI_BOOT_PUBKEY=<file> (raw 64-byte X||Y from "
		"`tools/sign_image.py --pubkey-out`, or the 270-byte PKCS#1 DER for "
		"an RSA profile). A production image without one would refuse every "
		"state change -- see the production-lock design 6.4.")
endif()

# Optional: the trusted key for signed *fabric* (bitstream) images
# (CONFIG_BOOT_AGM_BITSTREAM_SIGNED). Always the raw 64-byte X||Y P-256 key,
# whatever the application profile is -- the fabric is verified in the boot
# path, before the kernel's heap exists, and only the tinycrypt P-256 verifier
# is allocation-free (include/zephyr/drivers/misc/agm_bitstream.h).
#
# Leaving this empty is fine in a P-256 build: the driver then falls back to
# `agm_boot_pubkey`, so one keypair signs both. An RSA build has nothing to
# fall back to and needs this to be set (otherwise the all-zeroes weak default
# refuses every fabric update, loudly).
set(SPI_BOOT_BITSTREAM_PUBKEY "" CACHE STRING
    "trusted public key for signed fabric builds (64 B raw X||Y; absolute, or relative to this directory)")
if(SPI_BOOT_BITSTREAM_PUBKEY AND NOT IS_ABSOLUTE "${SPI_BOOT_BITSTREAM_PUBKEY}")
	set(SPI_BOOT_BITSTREAM_PUBKEY "${CMAKE_CURRENT_SOURCE_DIR}/${SPI_BOOT_BITSTREAM_PUBKEY}")
endif()
if(SPI_BOOT_BITSTREAM_PUBKEY)
	if(NOT EXISTS "${SPI_BOOT_BITSTREAM_PUBKEY}")
		message(FATAL_ERROR
			"SPI_BOOT_BITSTREAM_PUBKEY=${SPI_BOOT_BITSTREAM_PUBKEY} does not exist")
	endif()
	file(READ "${SPI_BOOT_BITSTREAM_PUBKEY}" _bspub_hex HEX)
	string(LENGTH "${_bspub_hex}" _bspub_hex_len)
	math(EXPR _bspub_len "${_bspub_hex_len} / 2")
	if(NOT _bspub_len EQUAL 64)
		message(FATAL_ERROR
			"SPI_BOOT_BITSTREAM_PUBKEY must be the raw 64-byte X||Y P-256 key, got ${_bspub_len} B")
	endif()
	string(REGEX REPLACE "(..)" "0x\\1," _bspub_bytes "${_bspub_hex}")
	set(_bspub_c "${CMAKE_CURRENT_BINARY_DIR}/spi_boot_bitstream_pubkey.c")
	file(WRITE "${_bspub_c}"
"/* Generated from ${SPI_BOOT_BITSTREAM_PUBKEY} -- do not edit. */
#include <stdint.h>

const uint8_t agm_boot_bitstream_pubkey[64] = {
${_bspub_bytes}
};
")
	target_sources(app PRIVATE "${_bspub_c}")
	message(STATUS "spi_boot_loader: fabric public key from ${SPI_BOOT_BITSTREAM_PUBKEY}")
endif()
