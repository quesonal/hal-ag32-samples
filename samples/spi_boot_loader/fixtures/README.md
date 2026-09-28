# Sample fixtures

`ci_production.pub` — the trusted public key the production-profile scenario
(`sample.spi_boot_loader.production`) builds with. It is the **public** half of
a disposable test keypair (the same 64-byte raw X||Y blob as
`tests/drivers/misc/boot_agm_ecdsa/fixtures/pubkey.bin`); the private half is
not in the tree, so nothing can actually sign an image for it.

That is all the scenario needs: `CONFIG_BOOT_AGM_PRODUCTION_PROFILE=y` makes the
sample refuse to *build* without a key (a locked board whose key is all zeroes
can never be updated again), and this file is what lets the CI build the shape
without shipping a real secret. A real production image passes
`-DSPI_BOOT_PUBKEY=<the key tools/sign_image.py --pubkey-out emits>`.
