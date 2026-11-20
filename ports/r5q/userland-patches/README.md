# Changes to the donor's userland (linux-source/userland/src)

The donor's userland lives outside this tree (IOS_WORK/linux-source/userland). Our changes to it are kept here as
patches against the donor's files; apply them with `patch -p1` in that userland directory.

| Patch | What it changes | Test |
|---|---|---|
| `init-var-hfs-t411.patch` | init mounts the HFS volume on `/dev/ufs_var` at `/private/var` (`var_hfs=1`). The skeleton keeps what an earlier boot left; var_store's archive seeds the volume once; `/tmp/t` is cleared before the write checks. | 411 |
| `var_store-hfs-t411.patch` | With `/private/var` on HFS, the end-of-run save becomes `sync()`, and `save` does nothing. | 411 |
| `camera_isp-exposure-t413.patch` | `isp_ae`: the whole frame's time before gain, a 1.25x gain floor (the IMX586's 1x frames are black), and two settle frames after each change. | 413 |

`pipeline/build_init.sh` builds init, var_store and libredwood-media.dylib from the patched source (wifi/STORAGE-PLAN.md,
wifi/CAMERA-PLAN.md).
