# Licences of the patches in `patches/`

The patches in `patches/` modify other projects, so each one is licensed the
same way as the project it applies to.

| Patches | Applies to | Licence | Full text |
|---|---|---|---|
| Kernel patches (`patches/kernel/*.patch`, 138) | Linux: Hoolock Linux, `ans1` branch at `ed8528f48` | **GPL-2.0-only** (SPDX: `GPL-2.0-only`), the same as the Linux kernel | [LICENSE-GPL-2.0](../LICENSE-GPL-2.0) |
| m1n1 patches (`patches/m1n1/*.patch`, 2) | m1n1: Hoolock's m1n1, `idevice` branch at `d5a10ac52` | **MIT** (SPDX: `MIT`), the same as m1n1 | see below |

New source files added by a kernel patch carry their own SPDX header, and that
header applies to that file. Most are `GPL-2.0-only`. The exceptions are all
GPL-2.0-compatible: the new DT binding schemas are `GPL-2.0-only OR
BSD-2-Clause` (the kernel's rule for bindings),
`drivers/misc/s2idle-wake-timer.c` is `GPL-2.0`, and the uapi header
`include/uapi/linux/apple-isp-t7000.h` is `GPL-2.0-only WITH
Linux-syscall-note`.

Code the patches modify keeps its original copyright and licence; the patches
only add changes under the same terms.

## MIT License (m1n1 patches)

```
Copyright (c) 2026 ITSMEFRTRUST

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## GNU GPL version 2 (kernel patches)

The full text is in [LICENSE-GPL-2.0](../LICENSE-GPL-2.0) (fetched unmodified
from <https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>). The Linux
kernel's own `COPYING` and `LICENSES/` directory also apply, including the
Linux syscall note where relevant.
