---
name: ios-port-next-test
description: Make the next hardware test (Test N+1) for the S10 Lite or any profiled phone from a small spec instead of copying the previous test's scripts - write the spec, generate, stage, build/check, bundle, add the runner case, then run only with authorization and record the result. Use whenever a new test number is about to be built.
---

# Making test N+1

The old way was `genNN.py`: copy test N-1's six scripts and patch strings. The generator does the same from
a spec plus the device profile, and it reproduces Tests 66, 67 and 68 byte for byte
(`python toolkit/tools/selftest.py`).

## Steps

1. **Spec.** Copy `toolkit/tests/<codename>/test69.example.json` to `test<N>.json`, extending the last test.
   List only what changes:
   - `number`, `previous`, `firmware_number`
   - `artefacts.kernel` and `artefacts.firmware_fd`: the kernel and FD this test runs
   - `payload.cmdline`: boot-args. `{timed}` is the run length and `%d` is the epoch the firmware overwrites
   - `loader.flags` and `loader.comment`, if the loader changes
   - `notes`: the previous test's result in the owner's words, plus what this test changes
   - `start_blocked_reason`
   - `runner.markers`: lines of markers in log order. Placeholders: `{N}` `{timed}` `{panel_last_row}`
     `{system_bytes}` `{system_hex}` `{resident_hex}`
   - `runner.comment` and `runner.timeout`
2. **Generate** into a scratch folder:
   `python toolkit/generator/gen_test.py --device toolkit/devices/<codename>.json --spec toolkit/tests/<codename>/test<N>.json --out <scratch>/gen<N>`
   It refuses, and lists what is missing, if the profile or the private overlay lacks a value.
3. **Review** the output: `NEXT-test<N>.md`, the payload items in `build_test<N>_shim.py`, the loader flags in
   `port/xnuloader/build-test<N>.sh`, and the markers.
4. **Stage:** `python <scratch>/gen<N>/stage_test<N>.py`. It copies the FD, kernel, root, the `port/test<N>` dir
   and the scripts into the work tree. It registers `--test<N>` in build_loader_linux.py. It refuses to
   overwrite another test's files.
5. **Build and check** in WSL with `bash work/ios-r5q/check_test<N>.sh`. That runs the loader build, the shim
   build, and the Unicorn shim test with all cases passing.
6. **Bundle** with `bash work/ios-r5q/bundle_test<N>.sh`. The guarded bundle refuses to overwrite a historical
   one, and stays start-blocked.
7. **Runner.** Paste `runner/markers-test<N>.ps1.txt` into `run_stage_test_auto.ps1` above the previous case.
   Raise `ValidateRange` to N. Add N to `$iosRuns` if the test takes the system image over USB. On other phones,
   use `toolkit/runner/run_test.py` with `runner/test<N>.json` instead.
8. **Run.** Load `ios-port-safety` first. Run only with the owner's OK for this run, or a standing authorization
   recorded with it. Lift the start guard for this one run and record who authorized it. Then run
   `run_stage_test_auto.ps1 -Number N` (S10 Lite). Exit codes:
   - 0: confirmed
   - 2: physical access needed
   - 3: baseline not verified
   - 4: markers unproven (read the receipt; often a marker order or anchor issue, not a failure)
9. **Record.** Add a short entry to `work/ios-r5q/SESSION.md` with the result, the evidence, the restore and
   baseline state, and what comes next. Update PHONE-STATE.json. Put the start guard back on the bundle.

## Other phones

- **Recovery-flashing phones (Samsung):** use the same flow. Run `run_test.py` with the generated
  `runner/test<N>.json`.
- **Phones with `fastboot boot`:** the test image is the stock boot image with BootShim as its kernel:
  `tools/bootimg.py repack stock.img --kernel <shim+fd+payload> -o test.img`, for any header version.
  `runner/fastboot_boot.py` RAM-boots it, and there is no restore. The `qcom-userland` templates' packager is
  Samsung-recovery shaped. For a fastboot-boot phone, package with `bootimg.py`; the donor's `mkxnuimg.py`
  does the same for redwood.

## Tips from Tests 1-69

- From firmware fulldbg23 (Test 69 on), `SYSTEM.APF` can come from a FAT32 microSD card instead of the USB copy.
  Write the card once with `work/ios-r5q/prepare_sdcard.py <drive>:`. A test that uses the card needs no USB
  host session, so drop `usb_system_image` from the spec's runner section, and its markers change to the
  firmware's file-system load lines.
- The userland root volume is built with `REST=1`, the donor's phone recipe.

- Shared markers may already be in the previous boot's log. The anchor (the third marker) must be this test's own.
- The full firmware ends with a cold PS_HOLD reset that wipes last_kmsg. The log slot in recovery is the log then.
- Change one thing per test when a step is uncertain. The owner may ask for "one big test" when steps are well
  understood (Test 68).
