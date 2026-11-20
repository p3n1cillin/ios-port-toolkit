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
