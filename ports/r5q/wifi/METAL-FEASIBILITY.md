# Metal on the S10 Lite under iOS: what it would take (2026-11-20, desk work)

Metal is what Maps' map (VectorKit), live wallpapers and many apps draw with. On this port there is no Metal device:
`MTLCreateSystemDefaultDevice()` returns nil, and apps that need Metal show nothing or quit. Everything else draws
through the port's OpenGL ES path (redwood-gl in userland, the Adreno 640 in the kernel: redwood_adreno.c).

## How Metal finds its GPU driver (Metal.framework, the 20F66 shared cache)

- `MTLCreateSystemDefaultDevice` matches the IOKit service **`IOAcceleratorES`** (`IOServiceGetMatchingServices`),
  reads **`MetalPluginName`** off it, opens `/System/Library/Extensions/<name>.bundle`, and instantiates the class
  named by **`MetalPluginClassName`** ("MTLCreateSystemDefaultDevice doesn't know what to do with more than one
  device"). The compiler side has its own plugin path (`MTLCodeGenServiceSetPluginPath`, "Could not load compiler
  plugin at %s").
- **This port already publishes an `IOAcceleratorES`** (iokit/Kernel/MiniIOMobileFramebuffer.cpp, for the OpenGL
  path), and with boot-arg `metal=1` it accepts exactly those two properties from user space
  (`IORegistryEntrySetCFProperties`), an empty string removing them. So the entry point exists: a process could point
  Metal at a bundle of ours.

## Why that is not enough (the donor's findings, kept in MiniIOMobileFramebuffer.cpp's comments)

- Metal's checks on the device object are **asserts, not refusals**: a device that is not complete aborts the
  process that asked ("Assertion failed: ([self conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion
  _limits.maxColorAttachments > 0"). An aborting daemon is restarted by launchd, so a half-made device makes a loop.
  That is why the properties are only set on purpose (`metal=1`), never at boot.

## What a working Metal device needs

1. **A device class** conforming to `MTLDevice` and Apple's private `MTLDeviceSPI`, with real limits and feature
   sets (the GPU family of an A11 for this iOS image).
2. **Its object model:** command queues and buffers, render/compute/blit encoders, buffers, textures, samplers,
   heaps, fences and events, drawables shared with CoreAnimation (IOSurface-backed, as the GL path already does).
3. **Shaders: the hard part.** Apps ship Metal shaders compiled to AIR (LLVM bitcode in `.metallib` files).
   Running them on the Adreno needs an AIR -> GLSL ES (or SPIR-V) translator, plus Metal's argument buffers,
   resource bindings and semantics mapped onto OpenGL ES 3.2 or onto the Adreno directly. This is the scale of
   projects like MoltenVK, in the other direction.
4. **Pipeline state** (render/compute pipeline descriptors, depth/stencil, blending, vertex descriptors) and
   **synchronisation** mapped onto the port's GL command stream.

## Verdict

- The plugin mechanism is open (the `IOAcceleratorES` service and `metal=1` switch exist); no Apple code needs to
  be broken into.
- A Metal implementation over the port's OpenGL ES path is a large project: weeks to months, mostly the AIR shader
  translator. It is feasible in principle (no secret keys, unlike FairPlay) but not a test-sized step.
- Cheaper ways to reach the missing features first: per app, a GL fallback where one exists (not for VectorKit in
  iOS 16, which is Metal-only); and live wallpapers left static.
- **First concrete steps, when taken up:** (1) a minimal device that satisfies Metal's asserts (MTLDeviceSPI, limits)
  and returns working buffers and textures but refuses pipelines cleanly, tried with `metal=1` in a single test
  process (never system-wide); (2) a survey of the AIR in the apps that matter (VectorKit's metallibs: which
  shader features they use) to size the translator.

## Tests 392 (2026-11-20): the donor's Metal driver on this phone, and why Maps cannot use it

- **The donor already built most of a Metal driver**: `userland/src/redwoodmetal.m` (6,300 lines: RedwoodMetalDevice
  over the kernel's RedwoodGPU service, command queues/buffers, compute and render encoders, textures, samplers,
  drawables for CAMetalLayer) and an AIR -> NIR -> ir3 shader compiler in Mesa (`tools/mesa/air`, docs/air.md: 321
  of 456 system shader modules compile; 30 run on the Adreno with checked results). This port had none of it built.
- **Built here:** Mesa 25.0.7 with `AIR=1 tools/mk-mesa.sh` -> `libredwoodair.dylib` and `air_run`; the driver as
  `libredwoodmetal.dylib` and the `RedwoodMetal.bundle` executable; `metal_drv_test`, `metal_props`, `metal_caps`.
  Root test392 = the cellular root's settings + the driver in /redwood/lib (the system's
  /System/Library/Extensions/RedwoodMetal.bundle symlink points there).
- **On this phone (kernel t389):** `metal_drv_test`: buffers made and read back; Apple's AV1SW.metallib loaded, **all
  11 compute functions compiled for the Adreno**; command queue, 16 textures, a sampler, command buffer and compute
  encoder made (its one dispatch was refused for a texture-format mismatch the test itself chose). With `metal=1` and
  `metal_props on`: **`MTLCreateSystemDefaultDevice()` returns RedwoodMetalDevice** ("Adreno ... chip 0x6040001")
  and answers the capability queries (`metal_caps`; it ends on Metal's own assert for a linear depth format, a
  question the tool asks).
- **Maps with that device:** Maps runs (no crash) but the map stays black: VectorKit logs "Compiler error for
  library from bundle 'StandardLibrary': no default library was found" and "Failed to create library for function
  'GridShader' / 'PolygonFillShader' / ...". **VectorKit's shaders exist on iOS 16.5 only as Apple-GPU machine code:**
  `iphoneos.pipelinelib/pipelines.g4p .. g15p` (one archive per Apple GPU, `LPPM` containers, no bitcode, no LZFSE)
  with an empty `functions` file -- no AIR to translate. So **the map in Maps cannot be drawn by any non-Apple GPU
  from the files Apple ships**, whatever the driver; that would take translating AGX machine code, which is not a
  realistic project. Metal discovery stays off in the standard boot (it is only on with `metal=1` + `metal_props
  on`, and gone at reboot).
- What the driver *can* serve: Metal code that ships AIR (`.metallib` with bitcode -- CoreImage's and other system
  libraries, apps built from source); MSL compiled at run time would also need Apple's front end to hand AIR over.

## Tests 399-400 (2026-11-20): THE WALLPAPERS -- lock screen and home screen, through the port's Metal driver

- **Why they were black, in two layers.**
  1. PosterBoard and the poster extensions draw posters through CoreAnimation's Metal renderer, and there was no
     Metal device. Unlike Maps (Apple-GPU code only), their shaders ship as AIR bitcode, which the driver compiles:
     QuartzCore default.metallib (141 functions), PhotosUICore (4), PridePoster (8), CoreImage (58).
  2. With a device, still black. CollectionsPoster logged "No compatible wallpaper found on disk (wallpaperIdentifier:
     '7400' ...)" and skipped the iOS 16 collection as empty. WallpaperKit picks a variant by its "logical screen
     class", built by an internal function (+0x11c60) from `"%.0fw-%.0fh@%@x~%@"` with [UIScreen mainScreen]'s bounds
     and -scale. This port runs at 2x (`fbscale=2`): 375w-812h@2x~iphone. Apple ships the iPhone X's variants as @3x
     only. The files were all there (/Library/Wallpaper, 21 MB).
- **libr5q-poster.dylib** (wifi/inject_poster.m) is injected into PosterBoard and the poster extensions:
  CollectionsPoster, PhotosPosterProvider, GradientPosterExtension, EmojiPosterExtension, PridePosterExtension,
  UnityPosterExtension, ExtragalacticPoster, LegacyPoster, WeatherPoster, AegirPoster.
  - **A Metal device in that process only.** It opens /redwood/lib/RedwoodMetal.bundle, calls
    `-initWithAcceleratorPort:` on IOAcceleratorES, and hands the device to Metal's `MTLAddDevice` (unslid
    0x181a1875c), as metal_drv_test does.
    - Before that it calls `MTLCreateSystemDefaultDevice()` once. That creates Metal's `_deviceArray`; in an extension
      at load time MTLAddDevice otherwise asserts "_deviceArray != NULL" (SIGABRT; found with libr5q-crash). The
      global is also checked before adding, so a failure means no device, not an abort.
    - System-wide discovery stays off. backboardd and SpringBoard keep their GL road.
  - **@3x for the extensions.** `-[UIScreen scale]` answers 3 for 2 in the extensions; PosterBoard and SpringBoard are
    excluded. That is the iPhone X's real scale: the shipped variants match, and the layers render at 3x and are
    scaled for the display. `-[WKDefaultWallpaperManager deviceLogicalScreenClass]` is wrapped too, at WallpaperKit's
    load (dyld add-image callback), but on its own it did not help: the class is kept in an ivar.
- **Result (Test 400: kernel t398, root test400 = test395 + the Metal driver files + libr5q-poster; a clean boot):**
  - the lock screen shows the iOS 16 default wallpaper;
  - after `phone.py unlock` (a 200 ms swipe: 300-400 ms ones don't unlock) the home screen has it behind the icons;
  - apps afterwards; 55% memory free; CollectionsPoster 31 MB, PosterBoard 21 MB.
- build_root_app.sh builds libr5q-poster and injects it. It adds the Metal driver after build.sh from
  port/build/metal: libredwoodmetal.dylib, libredwoodair.dylib, RedwoodMetal (the bundle's executable, a Mach-O
  bundle, not the dylib), RedwoodMetal-Info.plist. All local builds, not in the repository.
- Tools from this: `toolkit/tools/ios/dsc_global_writers.py` (who stores to a library's static).
