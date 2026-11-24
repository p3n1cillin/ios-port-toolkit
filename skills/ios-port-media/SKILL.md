---
name: ios-port-media
description: Bring up or debug the camera (sensors, supplies, autofocus, OIS), the microphone (a Qualcomm WCD codec on SLIMbus) and video recording with sound in a native iOS port. Use when the Camera app's picture is dark, black, noisy or never focuses, when a recording is silent, when movies never reach Photos or the picture lags the sound, or when starting the camera or microphone on a new phone.
---

# Camera, microphone and video

Each half has its own page. Read it before changing anything:
- camera: `docs/CAMERA.md` (the path, what to gather, the bring-up order, lessons from Tests 267-449);
- microphone: `docs/AUDIO.md` (the SLIMbus path, the message formats, lessons from Tests 419-450).

## First, from the live device tree (no phone writes)

```
python tools/cam_map.py live.dts          # every CCI master's pins; each sensor / lens motor / OIS / EEPROM:
                                          # 7-bit address, bus, GPIOs, supplies and the PMIC behind each
python tools/pmic_ldo.py dts live.dts     # the register value for every supply on an I2C PMIC
```

`cam_map.py` warns when a sensor's lens motor or OIS sits on another bus (the S10 Lite's do: GPIO33/34, not the
sensor's 17/18). Don't conclude "no autofocus" from a scan of the sensor's bus.

## After every boot that touched a supply or the codec

```
python tools/pmic_ldo.py log <boot log> --dts live.dts   # every "LDOn 0xNN" decoded; wrong ones flagged
python tools/slimbus_msg.py decode <bytes>               # any SLIMbus message the driver logged
python tools/av_check.py MOVIE.MOV --fps 30              # a recording: fps, A/V sync, gaps, unfinished file
python tools/bootargs_budget.py phone-args.txt --reserve <loader + payload bytes>   # before adding a boot-arg
```

## Symptom -> cause (S10 Lite)

| What you see | It was | Test |
|---|---|---|
| Dim scenes black until the sensor warms up; exposure/PLL/line-length changes don't help | a supply written at the wrong voltage (S2MPB03 LDO3 0x80 = 0.70 V, not 1.8 V); `pmic_ldo.py log` | 425-427 |
| Raw frame shows a dark ramp or purple streaks after white balance | the sensor's black clamp broken by a too-long line length | 396-398 |
| No lens motor found on the sensor's bus | it is on another CCI master; `cam_map.py` | 428 |
| Codec never answers on SLIMbus | ADDR_QUERY sent with the wrong message type (must be 0x4a) | 419 |
| First recording after boot silent for seconds | codec defaults sent at the first recording; do it at boot | 424 |
| Recordings silent (-91 dB) only while the camera streams | RX ring read without barriers or sentinels under DMA load | 431-432 |
| Every second recording silent | teardown not the exact reverse; a missing RECONFIG_NOW after DISCONNECT_PORT | 433 |
| Movies stay in `DCIM/.MISC/Incoming`, never in Photos; picture shorter than sound | the software encoder below real time (HEVC ~5 fps); H.264 (OpenH264) + NEON ISP | 430-433 |
| Speech clips | iOS adds ~32 dB; lower the codec gains | 421-424 |
| A boot-arg "does nothing" | the boot line is full and the loader cut the end; `bootargs_budget.py` | 449 |

## Rules that hold everywhere

- Prove iOS's side with a synthetic source first (`cam_fake=1`; a tone for audio), then the hardware.
- One step per test, a log line after each, and a read-back of every register write you depend on.
- Vendor sensor module files, tuning and tables derived from them stay local: never publish them.
- Supplies and PMIC writes: follow `ios-port-safety`. Camera and codec supplies on a small I2C PMIC are the port's
  own writes; decode them before the first boot, not after.
