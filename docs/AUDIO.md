# Microphone: a Qualcomm WCD codec on SLIMbus into iOS's audio input

Proven on the Galaxy S10 Lite (SM8150, WCD9340 "Tavil" on SLIMbus), Tests 419-436: Voice Memos, the Camera app's
movies and AVAudioRecorder record from the phone's own microphones at 48 kHz. The test-by-test log is
`ports/r5q/wifi/WIFI-PLAN.md` (Tests 419-424) and `ports/r5q/wifi/CAMERA-PLAN.md` (Tests 431-433); the driver is
`ports/r5q/kernel-build/r5q-overlay/bsd/dev/redwood_adsp.c` (the SLIMbus link, the codec) and `redwood_mic.c` (the
record stream). Speaker playback through the ADSP came first (docs in the port's WIFI-PLAN, "sound"); this page
assumes the ADSP is running and the port already talks APR/QMI to it.

Phones of this generation (SDM845, SM8150, many SM8250) put the audio codec on **SLIMbus**: the codec's registers and
its sample data both go over it. Later ones (SM8250 onwards with LPASS macros, SoundWire) differ; check the device
tree for `qcom,slim-ngd` and `qcom,tavil-slim-pgd` / `qcom,tasha-slim-pgd`.

## The path

```
mic -> MIC BIAS n -> ADC n (codec) -> decimator (DEC6) -> CDC_IF TX6 mux -> SLIM TX port 6
  -> SLIMbus channel 134 (128 + port), 48 kHz 16-bit, hard isochronous
  -> the ADSP's AFE port SLIMBUS_0_TX (0x4001) -> an ASM record session -> shared memory
  -> the kernel's /dev/redwood-mic -> the iOS audio plug-in's input stream ("iPhone Microphone")
```

The SoC side of SLIMbus is an **NGD** (a satellite controller) fed by a BAM DMA engine: you send messages through
the BAM's TX pipe and read replies from its RX pipe. The bus itself (the framer, the channels) is owned by the
ADSP's SLIMbus manager; you ask it for things with Qualcomm's user messages.

## Bring-up order

1. **The NGD up and in the manager's books:** clocks, the BAM pipes (TX 4, RX 3 on SM8150), then
   REPORT_SATELLITE. Read back every RX word; log them all for the first tests.
2. **Find the codec:** release its reset (wcd9xxx_reset: low 20 ms, high 20 ms), then **ADDR_QUERY** for its two
   enumeration addresses (the codec and its interface device, e.g. `00:01:50:02:17:02` and `00:00:50:02:17:02`)
   until ADDR_REPLY gives a logical address (all-zero = not enumerated yet: ask again).
3. **Read the chip id** (REQUEST_VALUE, 4 bytes at value element 0x821 on Tavil) to prove register access.
4. **The codec's register defaults** (Tavil: the CPR defaults, `tavil_update_cpr_defaults`), MCLK, the capture
   route (as Android's mixer paths set it: `ADC MUX6 = AMIC`, `AMIC MUX6 = ADC1`, `CDC_IF TX6 MUX = DEC6`), the
   decimator and ADC gains.
5. **The channel:** the interface device's TX port registers, then CONNECT_SRC (port 6 -> channel 134),
   DEF_ACT_CHAN (48 kHz, 16 bits, hard isochronous, presence rate 0x83), RECONFIG_NOW. Each user message to the
   manager gets a GENERIC_ACK; bit 5 of its response byte = accepted.
6. **The DSP side:** the AFE port's SLIMbus config (the same channel), start the port, an ASM record session, and
   read buffers to it. Only now unmute: Tavil's driver unmutes the TX path 45 ms after the start and releases the
   TX hold / sets the HPF corner at 300 ms (delayed work); do the same, or the first part of every recording is
   silent or thumps.

`python tools/slimbus_msg.py` builds and decodes every one of these messages (`decode`, `write`, `read`,
`addr-query`); paste the bytes your driver logged into `decode`.

## Lessons (each one cost at least one test)

- **ADDR_QUERY is a user message of type 2.** The first byte is `0x4a` (length 10, "destination referred user"),
  code 0x0d, to the manager (0xff). Sent as type 5 the manager ignores it without an error, and the codec looks
  absent or dead (Test 419). `slimbus_msg.py decode` warns about exactly this.
- **Codec registers are paged.** Write the page (reg >> 8) to offset 0, then the offset (reg & 0xff), each a
  CHANGE_VALUE to value element 0x800 + offset. Cache the page; a failed write must forget it.
- **Barriers on the RX ring (Test 432).** Under camera DMA load the CPU read RX words before the BAM's write landed
  (a stale 0 read as a message). Pre-fill each RX word with a sentinel (0xffffffff), `dsb sy` after reading the
  BAM's offset and before reading the words, and retry a read once after 50 ms. Before: recordings at -91 dB
  (silent) whenever the camera streamed; after: clean.
- **Tear down in the exact reverse order (Test 433).** CHAN_CTRL remove, RECONFIG_NOW, DISCONNECT_PORT, and another
  RECONFIG_NOW for the disconnect; stop the DSP's AFE port before the SLIMbus channel and start it after. Missing
  the last RECONFIG_NOW left every second session silent.
- **Set the codec up at boot, not at the first recording (Test 424).** Done on demand, the first recording after a
  boot was empty for 4.8 s while the codec's defaults went over the bus.
- **iOS adds about 32 dB of its own input gain.** Set the analog/decimator gains low (r5q: `mic_gain=8`,
  `mic_dgain=0`), or speech clips.
- **Fix the microphone for the whole recording.** If iOS can switch the data source (bottom ADC1 + MIC BIAS1, top
  ADC3 + MIC BIAS3; `'imic'`/`'smic'`), read the choice once when the recording starts and use that same value for
  unmute, settle and teardown; a switch mid-recording otherwise mixes two paths or leaves one powered (Test 450).
- **Bound every retry that holds the bus lock.** A settle or unmute that fails must not be retried every 20 ms for
  the whole recording: each failed try with register dumps held the link for seconds and starved the DSP reads.

## On a new phone

1. `qcom,slim-ngd` in the live device tree: the NGD's registers, its BAM, the IRQ; the codec's node gives its
   enumeration addresses (`elemental-addr`) and the reset GPIO.
2. Android's `mixer_paths*.xml` for the capture route of the main microphone ("handset-mic").
3. The codec's Linux driver (`wcd934x.c`, `wcd9335.c`) for the register defaults and the delayed unmute.
4. Prove each step on the bus with a read-back before the next; `slimbus_msg.py decode` on every logged message.
