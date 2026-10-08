# Memory and heap measurement

Connect USB Serial at 115200 and run:

```text
memory
```

The command reports:

- current/minimum internal heap,
- largest allocatable internal block,
- PSRAM total/free/minimum/largest block,
- minimum free stack reported for the main loop task.

For a meaningful measurement, reboot first so minimum counters start from a fresh boot, then exercise heavy simultaneous workloads such as Web, dual-CAN recording, DBC loading and BLE.

This does not measure independent AsyncTCP/BLE task stacks. Hardware measurements are not replaced by build or simulation results.
