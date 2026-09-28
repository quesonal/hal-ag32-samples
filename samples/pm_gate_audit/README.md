# pm_gate_audit

Print `SYS.APB_CLKENABLE` at three points so the bitstream-declared
gate set can be cross-checked against what `soc.c` actually opens:

| when                        | what happens                                              |
|-----------------------------|-----------------------------------------------------------|
| `boot`                      | `main()` first print; everything inited, no sleep        |
| `idle (5 s)`                | after `k_sleep(K_SECONDS(5))` — every PM-aware device put |
| `all-PM-aware-put`          | after `pm_device_runtime_put()` from `main()` on every PM-aware device |

`soc.c::agrv2k_apb_gates()` derives the same bit set from devicetree, so
the boot value must match that mask. The "all-PM-aware-put" value is
the lower bound on what the kernel can power-gate -- the always-on set
is the bitstream (FCB0, GPIO banks without PM actions, etc.) plus the
devicetree nodes that have no `PM_DEVICE` declaration.

## Build

```sh
cd $HOME/zephyrproject
west build -d /tmp/b_pm_gate -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/pm_gate_audit -- \
    -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
```

## Bench notes

The board must keep the bitstream's always-on peripherals live: the
sys clock controller needs `k_sleep` / `k_uptime_get` to work, so the
last print loop will appear frozen if the third `APB_CLKENABLE` value
ever drops to `0x00000001` (only FCB0). The dev board measurement is the
`boot_val & ~put_val` delta, not the absolute value.
