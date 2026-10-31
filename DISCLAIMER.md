# Disclaimer

## Not affiliated with Apple

This project is an independent, unofficial effort. It is **not affiliated
with, sponsored by, or endorsed by Apple Inc.** iPhone, iOS and other Apple
product names are trademarks of Apple Inc., registered in the U.S. and other
countries. They are used here only to say which hardware this software runs on.

## No Apple files included

This repository contains **no Apple firmware, no iOS files, no kernelcache,
no device-tree dumps and no Apple SDK files**. Drivers that need Apple
firmware (touch, Bluetooth, Wi-Fi, camera) expect you to extract it yourself
from an IPSW for, or from, a device you own. Do not redistribute those
extracted files.

## Reverse engineering

Hardware behaviour was studied for the sole purpose of **interoperability**:
making an independently written operating system (Linux) work on hardware the
author owns. The drivers and notes here are original work describing that
behaviour; they do not contain copied Apple code.

## No warranty — use at your own risk

This software is provided **"AS IS", WITHOUT WARRANTY OF ANY KIND**, express or
implied, as stated in the licences ([LICENSE](LICENSE),
[LICENSE-GPL-2.0](LICENSE-GPL-2.0), [patches/LICENSE.md](patches/LICENSE.md)).
In particular:

- It is experimental. A boot can hang and leave the phone unresponsive until
  you force it back into DFU mode.
- Low-level register access (the tools in `tools/`) can hang the phone or put
  hardware into odd states until the next power cycle.
- The optional **writable-flash step writes to the flash that holds your iOS
  data** using a work-in-progress driver. A bug, a wrong offset or a stale
  configuration can **corrupt iOS and lose all data on the phone**. Recovery is
  a full iOS restore.
- Charging and battery handling are not the same as iOS's. Keep an eye on the
  battery level and temperature.

**Use it only on a device you own** and whose data you have backed up or can
afford to lose. You are responsible for complying with the laws and any
agreements that apply to you.
