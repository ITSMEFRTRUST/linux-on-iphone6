# SmartIO (SIO) protocol on the A8 / T7000 (iPhone7,2, iOS 12.5.5)

This is static RE of `kc.macho`. It covers the AppleA7IOP V1 mailbox, the AppleT7000SmartIO kext and the SIO firmware embedded in the kernelcache.

- **[C] CONFIRMED** means it was seen in code or data. The evidence is a function or instruction address.
- **[G] GUESSED** means it was inferred.

Address conventions:
- `0xfffffff0…` is a kernelcache VA.
- `fw+0x…` is an offset into the SIO firmware image. Thumb-2 disassembly: `tools/sio-fw-thumb.s`, made from `tools/sio-fw.bin`.

Helpers: `tools/kc.py` reads VAs and pointers. The tag rule is real VA = `(raw & 0xffffffffff) | 0xffffff0000000000`.

> **Annotator caveat:** in `annot.py`, a chained `add xN, xN, #imm` after `adrp+add` is resolved against the adrp page, not the running value. So `&0xfffffff0075da010`-style annotations on the second `add` are wrong. The real value is (page + first add) + second add.

---

## 0. Big picture

```
 AP (Linux)                      SIO IOP (ARMv7 Cortex-A class, runs fw from AP DRAM)
 ─────────                       ─────────────────────────────────────────────────────
 mailbox @0x20ae00000  ──64-bit msgs──►  EP0 Control / EP1-3 debug / EP 0x14..0x67 DMA channels
 shared DRAM tables (phys pages):        "Shim" DMA adapters live INSIDE each peripheral's 16 KB
   segment table (12-byte slots)         MMIO window (+0x1000/+0x1800 regs, +0x2000/+0x3000 data)
   response table (16 B/ep/tag)          and pull PCM from DRAM over AXI into the MCA FIFO.
   panic buffer
```

- **Not RTKit.** It uses the classic A7IOP mailbox: 64-bit messages and no management endpoint.
- **No DART.** All addresses passed to SIO are 4 KB page numbers of AP physical addresses, up to 44 bits ([C] fw+0x2068: `lsls r0,r1,#12; lsrs r1,r1,#20`).
- `AppleSmartIO::start CHIPID:0x%04x` is not a handshake. It is an `IOLog` of the constant `0x7000` (`mov w8,#0x7000` at 0xfffffff008473cbc) [C].

---

## 1. AppleA7IOP V1 mailbox (reg[0] = phys 0x20ae00000, size 0x2000)

The SIO node uses compatible `iop,s5l8960x`, which selects `AppleA7IOPV1`. Its vtable is at 0xfffffff007549e48 (alloc 0xfffffff0080a194c).

DT `sio` node:
- `interrupts = [149 148 151 150]`, `interrupt-parent` = AIC
- `clock-gates = [90 4]`, `power-gates = [90]`
- `force-timebase-sync` and `ignore-gating` are present
- There is **no** `segment-ranges` and **no** `auto-boot`

| Offset | Name | Use | Ev. |
|---|---|---|---|
| 0x008 / 0x00c | FW_REMAP_PA lo/hi | Physical address of the firmware buffer (`md->getPhysicalSegment`) | [C] 0x80a1ce4–d00 |
| 0x010 / 0x014 | FW_REMAP_IOPVA lo/hi | IOP-side base. Always 0, so firmware runs at IOP address 0 | [C] 0x80a1cb4 |
| 0x018 / 0x01c | FW_REMAP_SIZE | `len & ~0xfff`. iOS writes 0x78000 (seg len 0x787d0), then 0 | [C] 0x80a1cd0 |
| 0x020 | FW_REMAP_EN | Write 1 | [C] 0x80a1d08 |
| 0x028 | CPU_CTRL | Bit 4 (0x10) = RUN. Read-modify-write: set to start, clear to stop | [C] 0x80a1d38 |
| 0x80c / 0x810 / 0x818 | Timebase sync | Clear bit 1 of 0x80c, write `mach_absolute_time` lo→0x810 and hi→0x818, set bit 1 | [C] 0x80a1da4; name [G] |
| 0x884 / 0x8a4 | Doorbell / NMI | `ringDoorbell(1)`: 0x884=0x11, 0x8a4=1. Not used at boot | [C]; meaning [G] |
| 0xb88 | Status | Bit 0 polled against `a7iop_enable_timeout` | [C]; meaning [G] |
| 0x1000 | MAILBOX_SET | startCPU writes 0x1111 | [C]; IRQ mask-set [G] |
| 0x1004 | MAILBOX_CLR | startCPU writes 0x0001, then 0x1000 | [C]; unmask inbox-empty (bit 0) and outbox-not-empty (bit 12) [G] |
| 0x1008 | INBOX_CTRL (AP→IOP) | Bit 16 FULL, bit 17 EMPTY, bits 18/19 under/overflow | [C] 0x80a1a10–54 |
| 0x1010 | INBOX data | **One 64-bit store** sends a message | [C] 0x80a19cc |
| 0x1020 | OUTBOX_CTRL (IOP→AP) | Bit 0 ENABLE (set by startCPU), bit 17 EMPTY, bits 18/19 errors | [C] 0x80a1b48, 0x80a1ad4 |
| 0x1038 | OUTBOX data | **One 64-bit load** pops a message | [C] 0x80a19b8 |

IRQs: AIC 151 is outbox-not-empty, the RX path, `_outboxHandler` at 0x80a0198. AIC 148 is inbox-empty, used for TX flow control. 149 and 150 are unused [C for which source is wired; the 149/150 meaning is G].

`AppleSmartIO::_doorbellAction` (0xfffffff008473e70) drains with `getMailbox()` until it is empty. It dispatches each message to `endpoint[EP]` at `this+0xb8+EP*8` [C].

### Message format (64-bit, little-endian) [C]

```
bits  7:0   EP     endpoint index (0..0x81)
bits 15:8   TAG    0 = unsolicited/async; 1..32 = reply to a synchronous command (cmd-pool index+1)
bits 23:16  OP     <0x65 = AP->SIO request, >=0x65 = SIO->AP
bits 31:24  PARAM
bits 63:32  DATA
```

Evidence:
- `AppleSmartIO::sendMessage` (0xfffffff008474b6c) packs the bytes.
- The decode in `dumpLogs` (0xfffffff0084747c4) and the "AP->SIO / SIO->AP" `cmp op,#0x65` at 0xfffffff0084747f0.
- The tag is assigned in `synchronousMessage` (0xfffffff008476d18–d34: `TAG = cmd->index + 1`, `EP = endpoint->index`).
- `sendMessage` refuses to send unless `_state(+0x4e0) == 1` ("non-receptive state") [C].

---

## 2. Firmware

- **Location [C]:** the whole `__DATA.__firmware` section.
  - VA 0xfffffff008995000, **file offset 0x1991000, size 0x787d0**.
  - `_initFirmware` (0xfffffff00847780c) does `IOMemoryDescriptor::withAddressRange(*(0xfffffff00897cd58)=0xfffffff008995000, 0x787d0, kIODirectionInOut)`, wraps it in an IODMACommand and hands the descriptor to `startCPU`.
- **Format:** raw ARMv7 image. It has no header and no magic.
  - fw+0x0 is an ARM vector table: `0xea00040e` = `b 0x1040` (reset), then `b .` ×7.
  - The code is mostly Thumb-2 with an LPAE MMU (fault strings mention "Long Desc Format", DFSR/IFSR).
  - Code and data occupy fw+0x0..0xb7cf. The rest, up to 0x787d0, is zero: BSS, heap and stacks. The firmware runs in place in AP DRAM through the remap window.
  - Tag string `"KF-SIO"` is at fw+0x9b60.
- **Linux load [C for mechanism, G for "copy is fine"]:**
  1. Allocate ≥0x78000 bytes physically contiguous and 4 KB aligned. Allocating the full 0x79000 is safer.
  2. Copy file[0x1991000 .. +0x787d0] into it, zero-filled.
  3. Program the remap registers (§1).

  It uses no DART (DT has no `segment-ranges`; `_dartMapFirmware` 0x80a0958 is skipped) [C].

- **Boot sequence [C]:**
  1. `AppleSmartIO::start` (0xfffffff008473c1c) calls `setDoorbellAction` (vt+0x550), then `startCPU(_firmware)` (vt+0x538).
  2. `AppleA7IOP::startCPU` (0xfffffff00809fd04) then runs:
     - power and clock on
     - remap registers 0x08..0x20
     - mask/unmask 0x1000/0x1004
     - OUTBOX_CTRL |= 1
     - timebase sync
     - CPU_CTRL |= 0x10
  3. Firmware init (fw+0x2a96..0x2ad2) runs its constructors. It then sends **`{EP 0, TAG 0, OP 0x6d, PARAM 3, DATA 0}`** (`movt r0,#0x36d` → `0x00000000_036d0000`) through its outbox writer at fw+0x681e.
  4. On the AP, `AppleSmartIOControl::_asyncMessage` (0xfffffff00847535c) handles OP 0x6d and asserts `PARAM == SMARTIO_PROTOCOL_VERSION (3)`. It then calls `notifyOSActive` (0xfffffff0084740e8), which sets `_state = 1` and wakes the 5 s wait ("timeout waiting for IOP to start", 0xfffffff008474058, `commandSleep` 5000 ms).
  5. `notifyOSActive` then does the following in order:
     1. `setupPanicBuffer`
     2. `setupPerfBuffer`
     3. JConsole/Trace, only with boot-args
     4. publishes the `sio-dma` nub
     5. `AppleSmartIODMAController::start` registers the segment and response tables

  There is no AP→SIO reply to the hello.

---

## 3. SmartIO protocol

### Endpoints [C]

The name table is at 0xfffffff00897c948 (0x82 entries). `endpointForHandle` is at 0xfffffff008474890.

| EP | Name | EP | Name |
|---|---|---|---|
| 0x00 | Control | 0x28/0x29 | MCA0_TX/RX |
| 0x01 | JConsole | 0x2a/0x2b | MCA1_TX/RX |
| 0x02 | Trace | **0x2c/0x2d** | **MCA2_TX/RX** (speaker) |
| 0x03 | Perf | 0x2e/0x2f, 0x30/0x31 | MCA3, MCA4 |
| 0x13 | AES | 0x3c..0x4d | UART0..8 TX/RX |
| 0x14..0x1b | SPI0..3 TX/RX | 0x50..0x59 | MCA_SLAVE0..4 TX/RX |
| 0x5a/0x5b | PDM0 TX/RX | 0x64..0x67 | DPA0/1 TX/RX |

EPs 1–3 come from the jump table at 0xfffffff008474b40. 0x13 is AES. Every other EP becomes an `AppleSmartIODMA` instance (0xfffffff008478f10). Even EP numbers are TX (memory→device) and odd are RX.

### Opcodes

**AP → SIO**

| OP | Name | EP | PARAM / DATA | Evidence |
|---|---|---|---|---|
| 1 | NOP / ping | DMA | Firmware just ACKs | fw table fw+0x34c4 → fw+0x34f0 [C] |
| 2 | (Get?)Param | DMA | Same handler as 3 | fw [C]; name [G] |
| 3 | SetParam | any | PARAM = param id, DATA = value | `AppleSmartIOEndpoint::setParam` 0xfffffff0084775a8, `AppleSmartIODMA::setParam` 0xfffffff00847a558 [C] |
| 4 | Suspend | Control | — | `AppleSmartIOControl::suspend` 0xfffffff008475610 (`strb #4 → +0x72`) [C] |
| 5 | DMA Configure | DMA | DATA = segment-table handle of a config block | `_configureDMA` 0xfffffff00847a114 (`strh #5`) [C] |
| 6 | DMA Submit buffer | DMA | TAG = command, DATA = segment-table handle of the buffer descriptor | `_startDMACommand` 0xfffffff008479408 (`strh #6` @…98f4) [C] |
| 7 | DMA Query | DMA | DATA = AP sequence counter | `_queryDMA` 0xfffffff008479a68 [C] |
| 8 | DMA Stop | DMA | DATA = 0 | `_stopDMA` 0xfffffff008479c48 [C] |
| 9 / 10 | DMA Pause / Resume | DMA | State 3→4 / 4→3. Unused by iOS | fw+0x35e0 / fw+0x3688 [C] |
| 0xb / 0xc | AES op / disable key | AES | — | 0xfffffff00847bd8c / …b698 [C] |
| 0x10 | Perf snapshot | Perf | — | user client 0xfffffff0084750b4 [C] |

**SIO → AP**

| OP | Name | Meaning | Evidence |
|---|---|---|---|
| 0x65 | ACK | Reply with the same EP and TAG. DATA = the opcode being acked | fw+0x7c5a; every AP sync path checks `op == 0x65` [C] |
| 0x66 | ERROR | Synchronous error reply (same TAG) or async error (TAG 0). PARAM = code (2 unsupported op, 3 bad param/config, 4 TAG==0 on submit, 5 bad state with DATA = state). `_asyncAction` logs "unexpected asynchronous error" | fw+0x7ccc, AP 0xfffffff008479298 [C]; code names [G] |
| 0x68 | DMA notify | **PARAM bit 7 set** = buffer TAG complete, DATA = bytes transferred. **PARAM bit 7 clear** = channel went idle (after Stop, or ran dry) | Filter 0xfffffff00847a65c and `_asyncAction` 0xfffffff00847922c [C] |
| 0x69 | Query reply | PARAM = TAG of the active buffer, DATA = progress | fw+0x36b6, AP 0xfffffff008479ba8 [C] |
| 0x6a | SIO panic | AP prints the panic buffer and panics | 0xfffffff0084754a4 [C] |
| 0x6d | Hello / OS active | PARAM = protocol version 3 | 0xfffffff008475408 [C] |
| 0x6e / 0x6f | Trace messages | — | 0xfffffff008475970 [C] |

If a 0x68 completion arrives with DATA ≠ the submitted length, the AP flags the command `0xe00002eb` (0xfffffff00847a72c) [C].

### SetParam IDs (OP 3)

Control (EP 0). The firmware table is at fw+0x2054. The AP sends:
- **1** = segment table page (PA>>12), **2** = segment table slot count (0xaaa). Sent from `AppleSmartIODMAController::start` 0xfffffff008477d6c [C].
- **0xb** = response table page, **0xc** = response entry count (0x1040). Same function, …7d9c [C].
- **0xf** = panic buffer page, **0x10** = panic size (0x4000). `setupPanicBuffer` 0xfffffff0084754f4 [C].
- 3 / 4: a page and size pair the firmware accepts (fw+0x207e) but iOS never sends [C]. Purpose [G].

Other endpoints:
- JConsole EP: 8 / 9 (0x20000)
- Trace EP: 5 / 6 (0x2000) and 0x11 / 0x12 (0x4000)
- Perf EP: 0xd (page) [C] and 0xe (size) [G]

DMA EP:
- **0xa** = frame size, a byte. It comes from `setFrameSize` through the gated action 0xfffffff00847a104 and is also copied into config +0x18 [C]. AE2 never calls it [C, per AE2 analysis].

### Shared memory structures [C]

All three are allocated by `AppleSmartIODMAController::start` (0xfffffff008477a4c) through `AppleSmartIO` vt+0x550, which calls IOSlaveProcessor 0x809eb1c. Each is page-aligned.

**Segment table:** 0x7ff8 bytes = 0xaaa slots × 12 bytes.
- `getBuffer(n)` (0xfffffff008478ca4) allocates **n+6 consecutive slots**. That is a 0x48-byte header followed by n segment entries `{u64 pa; u32 len}`.
- The entry layout comes from the `genIOVMSegments` callback at 0xfffffff00847a3c8.
- The DATA field of OP 5 and OP 6 is the starting slot index.

**Response table:** 0x10400 bytes = 0x1040 entries × 16 bytes.
- The entry for (EP, TAG) is at `base + ((EP*32 + TAG) - 1) * 16`. That gives COMMANDS_PER_ENDPOINT = 32 and kMaxEndpoint = 0x82 (0xfffffff00847955c–95b4).
- On completion the AP copies the 16 bytes into the command (`_notifyCommand` 0xfffffff00847a770).
- Contents: firmware-written completion info, probably a timestamp and count [G].

**Panic buffer:** 0x4000 bytes of text, printed on OP 0x6a.

#### Config block (OP 5)

The header sits at slot h. The firmware copies 32 bytes from `seg[h]` (fw+0x39e0, with a cache clean+invalidate first).

| Off | Field | Source | Firmware use |
|---|---|---|---|
| 0x00 | XFER | `dma-channels`[1] | → shim reg +0x00 (fw+0x4956, fw+0x1608) |
| 0x04 | TIMEOUT | [2] | → shim reg +0x04 |
| 0x08 | FIFO | [3] | Bytes of shim SRAM to allocate. Must be ≠0 (fw+0x490e) |
| 0x0c | TRIGGER | [4] | Must be ≤ FIFO ("trigger > fifo") |
| 0x10 | LIMIT | `min(setFIFODepth arg, FIFO)`, or 1 if the arg is 0 (0xfffffff00847a0c4) | Must be ≤ FIFO ("limit > fifo") |
| 0x14 | [5] | blob | — |
| 0x18 | frame size | `setFrameSize` | — |
| 0x1c | [6] | blob | — |
| 0x38 | style | 0 for config | — |

The AP logs this as `"Shim Configured: EP XFER TIMEOUT FIFO TRIGGER LIMIT"` (0xfffffff00847a364).

#### Buffer descriptor (OP 6)

- Slot h, with n+6 slots reserved.
- Header +0x38 = style: bit 18 of the memory descriptor's `_flags` (0xfffffff00847989c) [C]. Meaning [G]; use 0.
- Header +0x3c = segment count.
- Header bytes 0x00..0x37 are not written by the AP [C]. Zero them.
- Segments start at slot h+6.

### `dma-channels` blob [C]

There are 4 entries of 8×u32, selected by the config index. `_getConfiguration` (0xfffffff008477ec4) does `blob + idx*32` and checks that the length is a multiple of 0x20.

mca2:

```
idx0: EP 0x2c (MCA2_TX)  XFER 0x21 TIMEOUT 0 FIFO 0xcc TRIGGER 0xac 0 0 0
idx1: EP 0x2d (MCA2_RX)  XFER 0x21 ...
idx2: EP 0x2c (MCA2_TX)  XFER 0x22 ...
idx3: EP 0x2d (MCA2_RX)  XFER 0x22 ...
```

AE2MCA selects the entry by sample width (0xfffffff0086e74c0 / …74e0):
- TX: `setDMAConfig(TX, bits ≤ 16 ? 0 : 2)`
- RX: `setDMAConfig(RX, bits ≤ 16 ? 1 : 3)`

So in XFER, 0x21 is 16-bit and 0x22 is 32-bit. The low nibble is probably the access width code; 0x20 is unknown [G].

FIFO (0xcc = 204 bytes) and TRIGGER (0xac = 172) are bytes of SIO shim SRAM. The audio shim pool is 0x400 bytes (fw table fw+0xa8ec[2]) [C for values, G for units].

### DMA channel state machine

Firmware states at fw `ep+0xa0`, from the strings at fw+0xa138: 0 Uninit, 1 Exception, 2 Inactive, 3 Active, 4 Paused [G for numbering; transitions C].
- Configure is rejected while Active.
- Submit is rejected in state 1.
- Pause needs state 3; Resume needs state 4.

AP-side `_channel_state` (+0x2308) [C]:
- 0 IDLE
- 1 RUNNING (set on submit)
- 2 STOPPING (set by Stop)
- 3 STOPPED (set by 0x68 with PARAM bit 7 clear)
- back to IDLE

---

## 4. Minimal Linux sequence (pseudo-C)

```c
#define SIO      0x20ae00000   /* ioremap 0x2000 */
#define MCA2     0x20a0a8000   /* ioremap 0x4000; i2s-switch reg 0x20a002008 */
#define EP_CTRL  0x00
#define EP_MCA2TX 0x2c
static inline u64 MSG(u8 ep,u8 tag,u8 op,u8 param,u32 data)
{ return ep | tag<<8 | (u64)op<<16 | (u64)param<<24 | (u64)data<<32; }

/* ---- mailbox ---- */
void sio_send(u64 m){ while (readl(sio+0x1008) & BIT(16)) cpu_relax(); writeq(m, sio+0x1010); }
irqreturn_t sio_irq(int irq, void *d)              /* AIC 151 */
{ while (!(readl(sio+0x1020) & BIT(17))) sio_dispatch(readq(sio+0x1038)); return IRQ_HANDLED; }
/* sio_dispatch: tag!=0 -> complete pending sync cmd (expect op 0x65, else 0x66 err=PARAM);
                 ep0 op 0x6d param 3 -> complete(&hello);  ep0 op 0x6a -> dump panic buf;
                 ep 0x2c op 0x68: (param&0x80) ? buffer_done(tag, data /*bytes*/) : channel_idle(); */

/* sync request: tag 1..32 per EP; reply carries same EP/TAG */
int sio_call(u8 ep,u8 op,u8 param,u32 data){ tag=alloc_tag(ep); sio_send(MSG(ep,tag,op,param,data));
  wait reply; return reply.op==0x65 ? 0 : -EIO; }
#define setparam(ep,id,val) sio_call(ep, 3, id, val)

/* ---- 1. boot ---- */
pmgr_enable(SIO_gates /*clock-gates 90,4; power-gate 90*/); pmgr_enable(MCA2 gate 66);
fw = dma_alloc_coherent(0x79000, &fw_pa);                 /* contiguous, 4K aligned */
memcpy(fw, kc + 0x1991000, 0x787d0);                      /* rest zero (BSS) */
writel(lower_32_bits(fw_pa), sio+0x08); writel(upper_32_bits(fw_pa), sio+0x0c);
writel(0, sio+0x10); writel(0, sio+0x14);                 /* IOP VA 0 */
writel(0x78000, sio+0x18); writel(0, sio+0x1c); writel(1, sio+0x20);
writel(0x1111, sio+0x1000); writel(0x0001, sio+0x1004); writel(0x1000, sio+0x1004);
request_irq(AIC151, sio_irq);
writel(readl(sio+0x1020) | 1, sio+0x1020);                /* outbox enable */
t = arch_timer_read_counter();                            /* force-timebase-sync */
writel(readl(sio+0x80c) & ~2, sio+0x80c); writel(t, sio+0x810); writel(t>>32, sio+0x818);
writel(readl(sio+0x80c) | 2, sio+0x80c);
writel(readl(sio+0x28) | 0x10, sio+0x28);                 /* RUN */
wait_for_completion_timeout(&hello, 5*HZ);                /* EP0 OP 0x6d PARAM 3 */

/* ---- 2. shared tables (non-cacheable / coherent, phys < 2^44) ---- */
panic = dma_alloc_coherent(0x4000);  setparam(EP_CTRL,0x0f, panic_pa>>12); setparam(EP_CTRL,0x10,0x4000);
seg   = dma_alloc_coherent(0x8000);  setparam(EP_CTRL,0x01, seg_pa>>12);   setparam(EP_CTRL,0x02,0xaaa);
resp  = dma_alloc_coherent(0x10400); setparam(EP_CTRL,0x0b, resp_pa>>12);  setparam(EP_CTRL,0x0c,0x1040);

/* ---- 3. MCA2 register setup (AE2 v1 order; see AE2 notes below), then configure TX ---- */
u32 *cfg = seg + h*12;   memset(cfg, 0, 0x48);            /* h = slot from your slot allocator */
cfg[0]=0x21 /*16-bit; 0x22 for 32-bit*/; cfg[1]=0; cfg[2]=0xcc; cfg[3]=0xac;
cfg[4]=fifo_depth /* <=0xcc, AE2: roundup4(ch*a/b)*bytes_per_sample */; cfg[5]=0; cfg[6]=0; cfg[7]=0;
cfg[0x38/4]=0;  wmb();
sio_call(EP_MCA2TX, 5, 0, h);                             /* expect 0x65; then free slots */

/* ---- 4. stream: keep >=2 buffers in flight ---- */
void submit(int tag /*1..32*/, dma_addr_t pa, u32 len, int s /*slot*/){
  u8 *d = seg + s*12; memset(d,0,0x48);
  *(u32*)(d+0x38) = 0;  *(u32*)(d+0x3c) = 1;                     /* style, nseg */
  *(u64*)(d+0x48) = pa; *(u32*)(d+0x50) = len;                   /* seg[0] at slot s+6 */
  wmb(); sio_call(EP_MCA2TX, 6, 0, s);                           /* ACK = queued */
}
/* AE2 order: submit first, then MCA2 TX enable: reg 0x18 |= 1, then reg 0x0c |= 2 */
/* completion IRQ: EP 0x2c OP 0x68 PARAM&0x80 TAG=t DATA=bytes -> refill period t, resubmit;
   16-byte status at resp + ((0x2c*32 + t) - 1)*16 */

/* ---- 5. stop ---- */
writel(0, mca2+0x18); writel(1, mca2+0x44); while (readl(mca2+0x44) & 1);   /* TX off + flush */
sio_call(EP_MCA2TX, 8, 0, 0);   /* ACK, then async OP 0x68 PARAM 0 = idle */
```

AE2MCA notes for MCA2 v1 TX bring-up. From `AppleAE2MCA` (start 0xfffffff0086e729c, startIIS 0x86e802c, stop 0x86e85b0) [C]:
1. Write `0x44 = 3` and `0x48 = 1`, then poll both until they clear.
2. Write 0x10, 0x1c and 0x30 (=0xd).
3. Write 0x08 = `((32-rxbits)<<8) | (32-txbits)`.
4. SIO `setDMAConfig` / `setFIFODepth`.
5. Write `0x20 = ((burst-1)<<16) | burst`, then 0x34, 0x14, and 0x24/0x38 slot masks (0xffffffff).
6. Write 0x18 / 0x2c with bit 0 clear, then 0x10, then `0x40 = 0xb0003000` (W1C status), then 0x0c.
7. Start the DMA.
8. Set 0x18 bit 0 and `0x0c |= 2`.

There is one completion per submitted buffer. The MCA IRQ (AIC 171) only reports errors and under/overflow (0x40 bits 31/29/28/13/12).

---

## 5. Can the CPU feed the MCA FIFO directly (PIO)?

What is confirmed [C]:
- AE2MCA never touches a FIFO data register.
- The MCA IRQ has no FIFO-level or watermark bit.
- AE2 never reads or writes offsets 0xac or 0xcc. Those blob values are SIO shim sizes, not register offsets.

What the firmware shows [C]:
- The shim constructor (fw+0x7f22, args `(idx, dir, type)`) computes `block = type_base[type] + idx<<14`. The type bases are at fw+0xa8e0: SPI 0x0a080000, UART 0x0a0b0000, **MCA 0x0a06c000**.
- `shim+0x10 = block + (dir==TX ? 0x1000 : 0x1800)` is the shim control registers. `shim+0x14 = block + (dir==TX ? 0x2000 : 0x3000)`.
- MCA2_TX is constructed with idx 0xf, dir 1, type 2 (fw+0x2634). That gives **0x0a0a8000**, the mca2 block.

On the AP that means:
- **MCA2 TX shim registers = 0x20a0a9000**: +0x0 XFER, +0x4 TIMEOUT, +0x8 `(fifo_off<<14 & ~0xffff) | fifo_bytes>>2`, +0xc trigger, +0x10 enable. Written at fw+0x15e6 [C].
- **Aperture = 0x20a0aa000** (RX: 0x20a0a9800 / 0x20a0ab000).

The AE2 analysis also found non-zero registers at mca2+0x1000 and +0x1800 in the power-on dump. That fits this layout.

**GUESS:** the shim is a per-peripheral DMA adapter. Its AXI master (the "Shim0/Shim1 machine check AXIW/AXIR" strings) pulls from DRAM, and +0x2000 is the peripheral TX FIFO data port. A PIO path is plausible: program the shim registers yourself (or leave them disabled) and write 32-bit samples to 0x20a0aa000 at a rate paced by a timer.

Nothing in the code proves the AP can write the aperture or that the FIFO accepts unpaced writes. Treat it as an experiment, not a known path.

---

## 6. Remaining unknowns

1. Whether the remap PA must be physical. It almost certainly is, since there is no DART for SIO and the firmware treats DATA<<12 as PA.
2. Whether a fresh copy of the firmware boots, or iBoot/XNU leaves state the firmware relies on. Chained-fixup corruption of `__firmware` is unlikely: the vector table is intact.
3. The exact bit meanings of 0x1000/0x1004, 0xb88 and 0x884/0x8a4.
4. The semantics of XFER (0x21/0x22) and of TIMEOUT 0.
5. The meaning of LIMIT versus AE2's fifo-depth formula. On the AP, LIMIT = `min(arg, 0xcc)`.
6. The contents of the 16-byte response entry. The style bit (header +0x38) and its circular/loop semantics; iOS uses discrete buffers.
7. Whether the firmware needs Control param 3/4 or Trace/Perf for normal operation. iOS always sends Perf (0xd, plus 0xe [G]) on boot; Trace/JConsole only with boot-args.
8. MCA FIFO PIO via the +0x2000 aperture is untested.
9. Cache coherency of the SIO's AXI reads. Use coherent/non-cacheable buffers or flush.

---

## Shim registers (2026-10-17)

Source: `data_firmware_segment.bin` (its first 0xb7d0 bytes match `tools/sio-fw.bin`), disassembled in `tools/sio-fw-thumb.s`. **Address map [C]:** SIO address X = AP PA `0x200000000 + X`. The firmware's shim bases (SPI 0x0a080000, MCA 0x0a06c000, + idx<<14) are the same numbers as the DT `reg` values (mca2 = 0xa0a8000). The firmware never builds an MCA core-register address (it never touches 0x00..0x48). It touches only shim +0x00..0x18, the aperture, and the wrapper at 0x0a000000/0x0a001000/0x0a100000.

**Shim object [C]:** ctor fw+0x7f22 `(obj, idx, dir, type)`, called for MCA2_TX at fw+0x2622 (idx 0xf, dir 1, type 2). vtable fw+0xa874: [0] program fw+0x15e6, [1] TX data IRQ fw+0x4cb6, [2] RX data IRQ fw+0x574a, [3] "Timeout!" panic fw+0x65ee, [4] "Unavailable" fw+0x6604, [5] dump fw+0x6616. The global per-shim index (0..0x11) is also the IRQ bit number, so MCA2 = bit 15.

### Per-shim registers, MCA2 TX = 0x20a0a9000 (RX = 0x20a0a9800)
| off | name | value / semantics | ev. |
|---|---|---|---|
| +0x00 | XFER | config[0] (0x21 = 16-bit, 0x22 = 32-bit) | [C] fw+0x1608 |
| +0x04 | TIMEOUT | config[1] (0). Timeout IRQ → firmware panic "Shim %s(%d) Timeout!" | [C] fw+0x161e, 0x65ee |
| +0x08 | FIFO window | **`(fifo_bytes/4)<<16 \| (sram_off_bytes/4)`**. This corrects §5, which had the fields swapped. The offset comes from a bump allocator over a 0x400-byte pool per type and direction (MCA TX counter @fw RAM 0x701d4, RX @0x701d8, starting at 0). FIFO must be a multiple of 4 | [C] fw+0x484a–0x48a4 |
| +0x0c | TRIGGER | TX: `TRIG > FIFO−TRIG ? TRIG : min(FIFO−TRIG, …)`, which gives **0xac** for mca2. RX: `min(TRIG, LIMIT)`. The value is rewritten at runtime: when idle on start it is set to 0xac (TX) / 1 (RX) (fw+0x4b32); when out of buffers it is set to FIFO size (fw+0x540c) | [C] fw+0x496a–0x49c0 |
| +0x10 | ENABLE | Program sequence: write 0, read back 0x0a000000 twice (posted-write flush), write 0, write XFER/FIFO/TRIG/TIMEOUT, write **1**. At boot every shim gets 0→1 | [C] fw+0x15e6–0x1622; "enable/reset" meaning [G] |
| +0x14 | IRQ_EN | 1 = data IRQ on, 0 = off. Paired with the global mask below | [C] fw+0x1904/0x1936 |
| +0x18 | COUNT | **[8:0]**: TX = free bytes, RX = filled bytes. Upper bits unknown | [C] fw+0x4b94, 0x5442 (copy size = COUNT − reserve) |

There is **no data port in the register block**. Data goes to the **aperture: TX 0x20a0aa000, RX 0x20a0ab000** (obj+0x14). It is a write-only FIFO port, so it reads back 0.

### Wrapper (shared, AP 0x20a000000 page) [C addresses, G names]
- 0x20a001000/04 = data-IRQ MASK tx/rx. Init writes 0xffffffff to both (fw+0x1638).
- 0x20a001008/0c = per-shim disable (tx/rx, write 1<<idx). 0x20a001010/14 = per-shim enable.
- 0x20a001018/1c = MASKED status tx/rx; the handler dispatches per bit (fw+0x1722).
- 0x2..1040/44/58/5c/60/64 = "Shim DMA unavailable" MASK/MASKED/RAW. 0x2..1080/84 init 0xffffffff, 0x2..1098/9c timeout status.
- Init also writes **0x20a100004 = 0x03000102 and 0x20a100800 = 0x118** (fw+0x1652–0x1666). "Shim0"/"Shim1" AXI machine-check logs are at 0x20a000004.. and 0x20a100008...

### How samples reach the MCA [C]
There is **no DMA engine.** The SIO Cortex-A **CPU memcpys** the PCM. The TX IRQ handler (fw+0x4cb6 → fw+0x5422; fw+0x4dc0 when config[5]≠0) works like this:
1. It reads free = COUNT[8:0] − reserve. Reserve = obj+0x2c0 = `min(FIFO−LIMIT, TRIG−4)`.
2. It cleans the D-cache over the source.
3. It copies `min(free, remaining)` bytes from the buffer's DRAM VA to the aperture. The copy is `ldm/stm` 32-byte bursts to aperture+0x00..0x1c with no writeback, with strh/strb used for unaligned heads and tails (fw+0x4fd0–0x5190).
4. When no buffer is queued it writes zeros from the zero block at fw+0xb224 (underrun fill, fw+0x4ff2).

The data IRQ is level-type "free ≥ TRIGGER". The shim then drains its SRAM FIFO into the MCA TX FIFO in hardware. **No MCA FIFO data register is exposed to software**; the push from shim to MCA is internal. AE2 and SIO never address one [C negative].

### Verdict: the AP can drive this directly [G, strong]
The shim is a plain FIFO with a write port. SIO only does programmed I/O (PIO) to it, so the AP can do exactly what SIO does with no SIO firmware. The pace is 96 KB/s for 48 kHz 16-bit mono (TX slot mask 1). An 0xcc FIFO holds about 2 ms; up to 0x1fc bytes is possible (9-bit COUNT), plus the MCA's own 0x80-deep FIFO. So use an hrtimer or tight poll at ≤1–2 ms, or a larger FIFO. The AP probably cannot get the shim IRQ, since the wrapper IRQ goes to SIO.

Minimal sequence to try:
```c
pmgr_on(mca2 0x20e020140=0xf); pmgr_on(SIO gate 90 /*wrapper+shim SRAM likely in SIO domain*/);  /* SIO CPU NOT started */
w(0x20a100004,0x03000102); w(0x20a100800,0x118);                 /* fw init [C], need [G] */
w(0x20a001008,1<<15);                                            /* mask MCA2 TX data IRQ */
/* MCA2 core config per §4 AE2 order (reset 0x44/0x48, 0x08,0x20,0x14,0x24,0x18&~1,0x10,0x40,0x0c) */
S=0x20a0a9000; w(S+0x14,0); w(S+0x10,0); r(S+0x10);
w(S+0x00,0x21); w(S+0x04,0); w(S+0x08,0x00330000 /*0xcc B @ off 0*/); w(S+0x0c,0xac); w(S+0x10,1);
while (free=r(S+0x18)&0x1ff) push 32-bit words to 0x20a0aa000..+0x1c;   /* prefill */
w(MCA2+0x18, txctl|1); w(MCA2+0x0c, r(MCA2+0x0c)|2);              /* start */
loop: n=r(S+0x18)&0x1ff; write n bytes (multiple of 4) to aperture; check MCA2+0x40 bit12 (TX underrun).
```
Checks: COUNT should drop as words are written and the MCA should then drain it. If COUNT stays at 0, the shim is unclocked (try gate 90 / SIO PS) or 0x20a100xxx needs setup. The live reads at +0x1000 (`32 … 00210042 40 … 020208f6`) show ENABLE=0, so earlier aperture writes were dropped as expected. Those values are stale or reset values, not iOS ones [G].

## Shim link (2026-10-17)

Re-audit of every MMIO store in `data_firmware_segment.bin` (all `movt #0xa00/#0xa10/#0xae0/#0xe02/#0x8100`, every `mov.w #0x0a000000/#0x0e000000`, and all literal words 0x0a…/0x0e… in the data area fw+0xa7e8..0xa930). **[C] The complete list of SIO-fw writes that touch the wrapper/shim/peripheral space:**
- Boot, fw+0x1626..0x1666: `0x20a001000=0xffffffff` · `0x20a001004=0xffffffff` · `0x20a001080=0xffffffff` · `0x20a001084=0xffffffff` · `0x20a001040=0` · `0x20a001044=0` · `0x20a100004=0x03000102` · `0x20a100800=0x118`. Then, for each of the 18 TX and 18 RX shims, program (vt[0], fw+0x15e6): `+0x10=0`, read 0x20a000000 ×2, `+0x10=0`, then `+0x10=1` (config regs are skipped while no FIFO is allocated). Then IOP-local IRQs 0/1/2 are unmasked at 0x20ae00a80 (the SIO's own AIC, not the data path).
- Configure (op 5 → fw+0x48c2 → fw+0x46ea): IRQ off (`0x20a001008/0c = 1<<idx`, `+0x14=0`, fw+0x1904), then vt[0] = `+0x10=0`, flush, `+0x00=XFER`, `+0x08=(FIFO/4)<<16|off/4`, `+0x0c=TRIG`, `+0x04=TIMEOUT`, `+0x10=1`.
- Start/submit (op 6 → fw+0x33d6 → fw+0x4b1c): `+0x0c = 0xac` (TX) / 1 (RX), then IRQ on (fw+0x4a9c → 0x1936): **`0x20a001010/14 = 1<<idx` and `+0x14 = 1`**.
- Runtime: data IRQ fw+0x4cb6/0x5422 reads `+0x18`, PIO `stm` to aperture; out of buffers fw+0x5402: IRQ off, `+0x0c = FIFO`, IRQ on; idle fw+0x53f2: IRQ off.
- Nothing else. No request-select or routing register exists, and nothing is written to a peripheral's core registers, a clock, or a fabric register. The 0x20a108000 block plus PMGR `0x20e0201e8` (|=0xf, poll [7:4]==0xf) belong to the AES0 driver (fw+0x80e8, 0x8940), not to audio. The type tables at fw+0xa8e0..0xa90c hold the bases, the pool sizes (SPI 0x200, UART 0x100, **MCA 0x400 shared by MCA0–4 TX**) and the bump counters.
- Wrapper map [C from printf arg order, fw+0x1876]: blocks of 0x40: data at 0x1000 (MASK tx/rx 0x1000/04, per-bit off 0x1008/0c, per-bit on 0x1010/14, MASKED 0x1018/1c), **unavailable at 0x1040 (MASK 0x40/44, MASKED 0x58/5c, RAW 0x60/64)**, timeout at 0x1080 (MASK 0x80/84, status 0x98/9c). AXI machine check: Shim0 = 0x20a000004/08/0c (status/AXIW/AXIR), Shim1 = 0x20a100008/0c/10.

**Answer to "is the count fake / does SIO's bus master move data?" [C]:** no. COUNT is hardware. The AP's stores move it with the SIO CPU halted. The fw only reads it (`free = COUNT[8:0] − reserve`, fw+0x5442; `in_fifo = FIFO − COUNT`, fw+0x4b94), and it counts "fully drained" underruns (fw+0x545a). After the `stm` the fw does nothing that could push data. The aperture is SRAM, not the MCA FIFO: the RX aperture at power-on reads random SRAM, and there is one 0x400 B pool for all MCA TX. The SRAM→MCA move is done by a hardware engine ("Shim0/1 AXI" master), started by the peripheral's request.

### Ranked things to try (both directions are dead ⇒ look for a common gate)
1. **`0x20a0a9014 = 1` (TX) and `0x20a0a9814 = 1` (RX)** together with `0x20a001010 = 0x8000`, `0x20a001014 = 0x8000`. [C that the fw does this at every start (fw+0x195a); GUESS that +0x14 gates the request/engine, not just the IRQ.] The test sequence leaves +0x14 = 0 in both directions, which is the only start-time write still missing.
2. **Order as iOS does:** MCA `0x44=3; 0x48=1` (poll), `0x10=0`, `0x1c=0xd`, `0x30=0xd` **first**, *then* program the shim (+0x10 0→1 …), then the rest of the MCA config. [C for the order (AE2 → op 5 → fw+0x46ea); GUESS that the MCA soft reset also resets the co-located shim/request logic.]
3. Wrapper init exactly as the fw does it: `0x20a001000/04 = 0xffffffff`, `0x20a001080/84 = 0xffffffff`, `0x20a001040/44 = 0`. [C values. GUESS that 0x1000 may be a per-shim enable rather than only a mask.]
4. MCA `0x10 = 1` (iOS writes 1, or 3) before `0x40 = 0xb0003000`; it may be the request enable. [GUESS]
5. Burst: the MCA requests in bursts of `0x20[15:0]` words. With `0x20 = 0x00170018` the TX side may wait for 24 words (48 B at 16-bit) in the shim. Prefill the whole 0xcc B, or try `0x20 = 0x007f0004`, which is the reset depth with burst 4 as in the AE2 config sequence. [GUESS; it does not explain RX]
6. `XFER 0x22` (32-bit) instead of 0x21. UART/SPI use XFER 0, so 0x20 is MCA-specific. [GUESS]

**Diagnostics to read after each try:** `0x20a001060` bit15 (UNAVAILABLE RAW: if set, the MCA's request reaches the shim while it holds no data, so the request link works); `0x20a001098`; AXI logs `0x20a000004..0c` and `0x20a100008..10` (non-zero means the engine tried and faulted); MCA `0x04` (FIFO level?); MCA `0x40` bit12 (TX underflow, which proves the MCA consumes); `0x20a000000` (value of the flush read).
