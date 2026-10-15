# PAIR MASTER: USB Host Mode Implementation Plan

The core idea: in PAIR MASTER mode the FM-1 stops being a USB device and becomes a USB host. The MUSB controller that already runs the device stack can also run in host mode — same SIE, different CSR bits and a few extra operations. The SLAVE stays exactly as it is.

**Hardware assumption to verify first**: The MASTER must supply VBUS (5 V) to the cable. Check whether the PCB has a VBUS enable circuit (a P-FET or load switch from the battery/supply rail to the USB VBUS pin). If it does, find its GPIO. If it doesn't, a powered USB-OTG adapter or a cable that carries VBUS from the SLAVE's own power will be needed — but the software is the same either way.

---

## Step 1 — HAL audit and host-mode register mapping

**Goal**: Confirm that DEVCTL, host-mode IntrUSB bits, and VBUS control are reachable, with no code changes yet.

**Prompt**:

> Read `firmware/hal/fm1_usb.h` and `firmware/src/usb.c`. The existing SIE interface (`sie_wr(r, v)` / `sie_rd(r)`) uses MUSB register byte offsets directly. In standard MUSB:
>
> | Register | Offset | Purpose |
> |----------|--------|---------|
> | DevCtl   | 0x60   | Session control, mode read-back |
> | IntrUSB  | 0x06   | Already in `S_INTRUSB`; in host mode bits 4=Conn, 5=Discon, 6=SessionReq, 7=VBusError join the existing 0=Susp/1=Resume/2=Reset |
> | IntrUSBE | 0x0B   | Interrupt enables — same register, already `S_INTRUSBE` |
>
> Do the following **without changing any code**:
> 1. Confirm that `sie_wr(0x60, v)` and `sie_rd(0x60)` would reach DevCtl — trace `fm1_usb_sie_wr_start` / `fm1_usb_sie_rd_start` in the HAL to verify the register address is just passed through as the high byte of `FM1_USB_CON1`.
> 2. Search the schematic signal names in the source (`FM1_USB_IO_CON0`, `0x51000`) for a bit that controls VBUS output — in typical chips it's a bit in the IO controller that asserts a GPIO which enables a load switch. Document which bit it is (or note if absent).
> 3. Check whether `FM1_USB_IO_CON0 |= 0x40u` (the D+ pull-up for device mode) can simply be inverted to `0x80u` for a D- pull-down (host mode sensing), or whether host-side pull-downs are handled automatically by the MUSB PHY when DevCtl.Session is set.
>
> Report findings as a short table: register/bit → offset/mask → confirmed reachable yes/no → notes.

---

## Step 2 — HAL host-mode additions

**Goal**: Add `fm1_usb_host_attach()`, `fm1_usb_host_off()`, and VBUS control to the HAL. No firmware logic yet — just the lowest-level pin/register operations.

**Prompt**:

> Edit `firmware/hal/fm1_usb.h`. Add the following after `fm1_usb_off()`:
>
> ```c
> /* MUSB DevCtl (offset 0x60): session, mode read-back */
> #define S_DEVCTL 0x60u
>
> /* Host attach: remove the D+ device pull-up, then start a session.
>  * The MUSB PHY asserts its own pull-downs when Session is set in host mode.
>  * Call fm1_usb_reset() + fm1_delay_ms(25) first (same as device attach). */
> FM1_INLINE void fm1_usb_host_attach(void *ep0)
> {
>     uint32_t n;
>     FM1_CLK_CON1 &= ~3u;
>     FM1_USB_EP0_ADR = (uint32_t)(uintptr_t)ep0;
>     for (n = 1; n <= 3u; n++)
>         FM1_USB_EP_RADR(n) = (uint32_t)(uintptr_t)ep0;
>     FM1_USB_EP4_RADR = (uint32_t)(uintptr_t)ep0;
>     FM1_USB_IO_CON0 |= (1u << 10) | (1u << 9);
>     FM1_USB_IO_CON0 &= ~(1u << 11);
>     FM1_USB_IO_CON0 |= 1u << 12;
>     FM1_USB_CON0 &= ~0x4035u;
>     FM1_USB_CON1 = 0;
>     FM1_USB_CON0 |= 1u;
>     FM1_USB_IO_CON0 &= ~0xF0u;   /* no D+/D- pull from pad side; PHY handles it */
>     FM1_USB_CON0 |= 0x3Cu;
> }
>
> /* If the board has a VBUS enable GPIO, fill this in from Step 1. */
> FM1_INLINE void fm1_usb_vbus_en(int on) { (void)on; }   /* TODO: board-specific */
> ```
>
> Key difference from `fm1_usb_attach()`: no `FM1_USB_IO_CON0 |= 0x40u` (D+ pull-up). The MUSB PHY in host mode self-asserts pull-downs via DevCtl.Session. Keep `fm1_usb_attach()` unchanged.

---

## Step 3 — Host session & connection state machine

**Goal**: Implement `usb_host_start()` and `usb_host_poll()`. By the end of this step the MASTER can detect a device plugged in and has issued a bus reset.

**Prompt**:

> In `firmware/src/usb.c`, add a host state enum and struct after the existing `usb` struct:
>
> ```c
> enum { HS_OFF, HS_SESSION, HS_CONNECTED, HS_RESET, HS_RESET_WAIT, HS_ENUM, HS_RUNNING, HS_ERROR };
> static struct {
>     uint8_t  state;
>     uint8_t  dev_addr;
>     uint8_t  ep_midi_out;   /* slave's MIDI bulk-OUT endpoint address */
>     uint8_t  ep_uac_in;     /* slave's UAC isochronous-IN endpoint address */
>     uint32_t reset_ms;
> } host;
> ```
>
> Add `usb_host_start()` (called when PAIR MASTER is selected):
>
> ```c
> static void usb_host_start(void)
> {
>     host.state = HS_OFF;
>     host.dev_addr = 0;
>     host.ep_midi_out = 0;
>     host.ep_uac_in   = 0;
>     fm1_usb_off();
>     fm1_usb_reset();
>     fm1_delay_ms(25);
>     fm1_usb_vbus_en(1);
>     fm1_usb_host_attach(ep0buf);
>     sie_wr(S_INTRUSBE, 0xF7u);   /* enable Conn/Discon/VBusError + existing bits */
>     sie_wr(S_INTRTX1E, 0x01u);
>     sie_wr(S_INTRRX1E, 0x00u);
>     fm1_usb_ep_enable(0x1Fu);
>     sie_wr(S_DEVCTL, 0x01u);     /* Session=1: become A-device host */
>     usb.up = 1;
>     usb.config = 0;
>     host.state = HS_SESSION;
> }
> ```
>
> Add `usb_host_poll()` (called at 2 kHz from TIMER5 ISR instead of `usb_poll()` when in MASTER mode):
>
> - `iu & 0x10u` (Conn): start bus reset — `sie_wr(S_POWER, 0x08u)`, record `reset_ms`, set `state = HS_RESET`
> - `iu & 0x20u` (Discon): `host.state = HS_SESSION`, clear endpoints
> - `iu & 0x80u` (VBusError): `host.state = HS_ERROR`
> - In `HS_RESET`: after 55 ms — clear Reset (`sie_wr(S_POWER, 0)`), `state = HS_RESET_WAIT`
> - In `HS_RESET_WAIT`: after 10 ms → `state = HS_ENUM`
>
> In the existing `usb_poll()` entry point, gate at the top: `if (usb_pair == 1u) { usb_host_poll(); return; }`.

---

## Step 4 — Host EP0 control transfers (enumeration foundation)

**Goal**: Implement blocking polled host control transfers — SETUP → optional DATA-IN → STATUS — used only during enumeration.

**Prompt**:

> In `firmware/src/usb.c`, add `host_ctrl()`. In host mode the EP0 CSR0 bits differ from device mode:
>
> | Bit | Host meaning |
> |-----|-------------|
> | 0   | RxPktRdy — IN data arrived |
> | 1   | TxPktRdy — set to send |
> | 2   | RxStall / error |
> | 3   | SetupPkt — set together with TxPktRdy to mark as SETUP |
> | 4   | Error (no response) |
> | 5   | ReqPkt — issue an IN token |
> | 6   | StatusPkt — handshake phase |
> | 7   | NAKTimeout |
>
> ```c
> static int host_ctrl(uint8_t addr, const uint8_t *setup,
>                      uint8_t *in_buf, uint16_t in_len)
> {
>     uint32_t n, i, t0 = fm1_ms;
>     sie_wr(S_FADDR, addr);
>     sie_wr(S_INDEX, 0);
>     for (i = 0; i < 8u; i++) ep0buf[i] = setup[i];
>     fm1_usb_ep0_send(ep0buf, 8);
>     sie_wr(S_CSR0, 0x0Au);                   /* TxPktRdy | SetupPkt */
>     while (fm1_ms - t0 < 100u) {
>         sie_wr(S_INDEX, 0);
>         if (!(sie_rd(S_CSR0) & 0x02u)) break;
>     }
>     if (!in_buf || !in_len) goto status_out;
>     n = 0;
>     while (n < in_len && fm1_ms - t0 < 500u) {
>         sie_wr(S_INDEX, 0);
>         sie_wr(S_CSR0, 0x20u);               /* ReqPkt */
>         while (fm1_ms - t0 < 500u) {
>             uint32_t c = sie_rd(S_CSR0);
>             if (c & 0x14u) return -1;
>             if (c & 0x01u) break;
>         }
>         { uint32_t cnt = sie_rd(S_COUNT0);
>           if (cnt > in_len - n) cnt = in_len - n;
>           fm1_usb_rx_sync();
>           for (i = 0; i < cnt; i++) in_buf[n + i] = ep0buf[i];
>           n += cnt;
>           sie_wr(S_CSR0, 0);
>           if (cnt < 8u) break; }
>     }
> status_out:
>     sie_wr(S_INDEX, 0);
>     sie_wr(S_CSR0, 0x40u);                   /* StatusPkt */
>     while (fm1_ms - t0 < 200u) {
>         if (!(sie_rd(S_CSR0) & 0x40u)) break;
>     }
>     return in_buf ? (int)n : 0;
> }
> ```
>
> Note: if `ep0buf` isn't DMA-filled in host mode, fall back to byte-by-byte SIE FIFO reads — verify on first hardware test with a logic analyzer.

---

## Step 5 — Device enumeration: find MIDI and UAC endpoints

**Goal**: After bus reset, run GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION / SET_INTERFACE to fully enumerate the slave and locate its MIDI bulk-OUT and UAC isochronous-IN endpoint addresses.

**Prompt**:

> In `firmware/src/usb.c`, add `host_enum()`. Called from `usb_host_poll()` when `host.state == HS_ENUM`. Sets `host.state = HS_RUNNING` on success, `HS_ERROR` on failure.
>
> Sequence:
>
> 1. **GET_DESCRIPTOR (device, addr=0)** with `wLength=18` — note `bMaxPacketSize0` at byte 7.
> 2. **SET_ADDRESS 1** — `{0x00,0x05,0x01,0x00,0x00,0x00,0x00,0x00}`, no DATA stage. Use `addr=1` from here on.
> 3. **GET_DESCRIPTOR (config, addr=1)** — first with `wLength=9` to read `wTotalLength` (bytes 2-3), then re-issue with that length into a `uint8_t buf[256]` (cap at 256).
> 4. **Parse config descriptor** — walk the descriptor list:
>    - MIDI bulk-OUT: class=1, subclass=3, endpoint `(addr & 0x80)==0`, `(attr & 3)==2`. Store in `host.ep_midi_out`.
>    - UAC isochronous-IN: class=1, subclass=2, endpoint `(addr & 0x80)==0x80`, `(attr & 3)==1`. Store in `host.ep_uac_in` and note the interface number.
> 5. **SET_CONFIGURATION 1** — `{0x00,0x09,0x01,0x00,0x00,0x00,0x00,0x00}`.
> 6. **SET_INTERFACE (alt 1)** for the UAC AS interface found in step 4 — `{0x01,0x0B,0x01,0x00,if_num,0x00,0x00,0x00}`.
>
> Hard-code fallback for FM-1 slave: if the walk found nothing, set `ep_midi_out = 0x01` and `ep_uac_in = 0x84` (the FM-1's known values from `usb.c` lines 210 and 224).

---

## Step 6 — MIDI clock output to the slave

**Goal**: Forward F8/FA/FC USB-MIDI packets from the `mo` ring to the slave's MIDI bulk-OUT endpoint.

**Prompt**:

> In `firmware/src/usb.c`, add `host_ep_midi_tx()`:
>
> ```c
> static void host_ep_midi_tx(void)
> {
>     uint32_t csr, n = 0;
>     if (host.state != HS_RUNNING || !host.ep_midi_out)
>         return;
>     if (mo_w == mo_r)
>         return;
>     sie_wr(S_INDEX, 1);
>     csr = sie_rd(S_TXCSR1);
>     if (csr & 0x01u)
>         return;                              /* previous packet still in flight */
>     while (mo_r != mo_w && n < 16u) {
>         uint32_t p = midi_out_q[mo_r % MQ];
>         mo_r++;
>         ep1tx[n * 4u]     = (uint8_t)(p);
>         ep1tx[n * 4u + 1] = (uint8_t)(p >> 8);
>         ep1tx[n * 4u + 2] = (uint8_t)(p >> 16);
>         ep1tx[n * 4u + 3] = (uint8_t)(p >> 24);
>         n++;
>     }
>     sie_wr(S_FADDR, host.dev_addr);
>     fm1_usb_ep_send(1, ep1tx, n * 4u);
>     sie_wr(S_TXCSR1, 0x01u);
> }
> ```
>
> Call from `usb_host_poll()` when `host.state == HS_RUNNING`.
>
> In `firmware/src/seq.c`, add the clock output that feeds the `mo` ring:
> - `seq_reset_tracks()`: add `clk_out_pulse = 0` after `clk_beat = 0`
> - `seq_start()`: if `usb_pair == 1u`, call `midi_out_event(0x0Fu | 0xFA00u)` (START)
> - `seq_stop()`: if `usb_pair == 1u`, call `midi_out_event(0x0Fu | 0xFC00u)` (STOP)
> - `events_block()`: after advancing `clk_pos`, if `song.playing && usb_pair == 1u`:
>   ```c
>   uint32_t now_p = clk_beat * 24u + clk_pos / (BEAT_U / 24u);
>   while (clk_out_pulse < now_p) {
>       clk_out_pulse++;
>       midi_out_event(0x0Fu | 0xF800u);   /* F8: clock */
>   }
>   ```

---

## Step 7 — UAC audio receive from the slave

**Goal**: Issue isochronous IN tokens to the slave's UAC endpoint every ~0.5 ms, fill `uai_ring[]`, and mix into the headphone output.

**Prompt**:

> In `firmware/src/usb.c`, add the UAC IN ring and state (after the existing `ua_ring`):
>
> ```c
> #define UAI_N 256u
> static uint32_t uai_ring[UAI_N];
> static volatile uint32_t uai_w, uai_r;
> static uint8_t ep4rx[UAC_MAXP + 4] __attribute__((aligned(4)));
> static struct { uint8_t flowing; } uai;
> ```
>
> Add `host_ep_uac_rx()`, called from `usb_host_poll()` on every tick once running:
>
> ```c
> static void host_ep_uac_rx(void)
> {
>     uint32_t csr, n, i, w;
>     if (host.state != HS_RUNNING || !host.ep_uac_in)
>         return;
>     sie_wr(S_INDEX, 4);
>     csr = sie_rd(S_RXCSR1);
>     if (csr & 0x01u) {                       /* RxPktRdy: data from previous IN token */
>         n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
>         if (n > sizeof ep4rx) n = (uint32_t)sizeof ep4rx;
>         FM1_USB_EP4_RADR = (uint32_t)(uintptr_t)ep4rx;
>         FM1_USB_EP_CNT(4) = 0;
>         fm1_usb_rx_sync();
>         w = uai_w;
>         for (i = 0; i + 3u < n; i += 4u) {
>             uint32_t fr = (uint32_t)ep4rx[i]   | (uint32_t)ep4rx[i+1] << 8 |
>                           (uint32_t)ep4rx[i+2] << 16 | (uint32_t)ep4rx[i+3] << 24;
>             if (UAI_N - (w - uai_r) > 0u)
>                 uai_ring[w++ & (UAI_N - 1u)] = fr;
>         }
>         RING_PUBLISH();
>         uai_w = w;
>         uai.flowing = 1;
>         sie_wr(S_RXCSR1, csr & ~0x01u);
>     }
>     /* Issue next IN token */
>     sie_wr(S_INDEX, 4);
>     sie_wr(S_RXCSR2, 0x40u);                 /* isochronous mode */
>     sie_wr(S_RXMAXP, UAC_MAXP >> 3);
>     sie_wr(S_FADDR, host.dev_addr);
>     sie_wr(S_RXCSR1, 0x20u);                 /* ReqPkt */
> }
> ```
>
> In `firmware/src/audio.c`, in `audio_block()` after `mix_block(out, n)`:
>
> ```c
> if (usb_pair == 1u && uai.flowing) {
>     static const int16_t GAIN[8] = {0, 181, 256, 323, 362, 512, 724, 1448};  /* Q9 */
>     int16_t g = GAIN[usb_pair_gain & 7u];
>     uint32_t r = uai_r, fill = uai_w - r, k = fill < n ? fill : n, i;
>     for (i = 0; i < k; i++) {
>         uint32_t fr = uai_ring[(r + i) & (UAI_N - 1u)];
>         out[2u * i]     += (int32_t)(int16_t)(fr & 0xFFFFu) * g >> 9;
>         out[2u * i + 1u] += (int32_t)(int16_t)(fr >> 16)    * g >> 9;
>     }
>     uai_r = r + k;
> }
> ```
>
> Buffer tuning (on real hardware): monitor `uai_w - uai_r` at render time; target 44–220 frames in hand. If drop-outs occur, increase `UAI_N` to 512 or issue the ReqPkt every other poll instead of every poll.

---

## Step 8 — Integration, PAIR MODE menu, and status display

**Goal**: Wire everything together — menu settings, mode switching, SLAVE auto-sync, and a status indicator.

**Prompt**:

> **Mode switching** (call from `mi_set()` in `ui_menu.c` when `MI_PAIR` changes):
> - `usb_pair` → 1 (MASTER): call `usb_host_start()`; device is now a USB host, normal USB-MIDI/UAC to computer suspended.
> - `usb_pair` → 0 or 2 (away from MASTER): call `usb_detach()` then `usb_start()` to resume device mode.
> - Gate on `!song.playing` to avoid audio glitches (or add a short mute).
>
> **SYSTEM menu** (`firmware/src/ui_menu.c`):
> - Add `MI_PAIR` (K3) and `MI_PAIR_GAIN` (K4) after `MI_ABOUT` in the enum and `MS_FIRST`.
> - `PAIR_NAME[3] = {"OFF", "MASTER", "SLAVE"}`.
> - `PAIR_GAIN_NAME[8] = {"MUTE", "-6", "-3", "-1", "+0", "+3", "+6", "+12"}` — show dimmed when not in MASTER mode.
> - Store in `lights_word()` at bits 21-22 (pair, 2 bits) and 23-25 (gain, 3 bits).
> - Fix the footer: only render when the section has ≤ 3 rows (`if (MS_FIRST[sec+1u] - MS_FIRST[sec] <= 3u)`).
>
> **SLAVE auto-sync** (`firmware/src/seq.c`):
> - `mclk_on()`: if `usb_pair == 2` return `mclk.alive && fm1_ms - mclk.last_ms < 500u` (bypass G_SYNC check).
> - `mclk_event()`: if `usb_pair == 1` return immediately (MASTER ignores incoming clock); if `usb_pair == 2` accept only `src == 1` (USB) regardless of G_SYNC.
>
> **Status indicator** (`firmware/src/ui_draw.c` or wherever the top status bar is drawn):
> - `usb_pair == 0`: nothing extra.
> - `usb_pair == 1` (MASTER): show "PAIR M" near the BPM readout.
>   - `host.state == HS_RUNNING`: `C_HI`
>   - `host.state == HS_SESSION / HS_RESET / HS_RESET_WAIT / HS_ENUM`: `C_AMB` (connecting)
>   - `host.state == HS_ERROR`: `C_DIM`
> - `usb_pair == 2` (SLAVE): show "PAIR S" — `C_HI` when `mclk.alive`, `C_DIM` otherwise.
