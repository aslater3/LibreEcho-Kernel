# ARM32 TinyALSA audio tools

These static ARM32 hard-float utilities are built from TinyALSA commit
`e43025bbf702eb7dd8edd48c1eb50530c60f1de8`.

The local utility patch does three things required by the MT8163 Amazon PCM
driver:

- explicitly calls `pcm_prepare()` before the first transfer;
- makes `tinyplay` return failure when a write fails; and
- maps 24-bit `tinycap` capture to packed `PCM_FORMAT_S24_3LE`; and
- makes `tinycap` return failure when no frames are captured; and
- includes `tinymix` for inspecting and enabling the Echo amplifier controls.

The binaries are static ELF32 ARM EABI5 hard-float executables with no dynamic
section:

| file | size | SHA-256 |
| --- | ---: | --- |
| `tinyplay` | 485796 | `3464771e2a94c03fc77ac5c1794fc5a15075277c5da2364660acc2e80a219c33` |
| `tinycap` | 434740 | `3709bd23a2cc231441c1176e9aee48f9cfd1c5e2da09b3c2fcd5908095de81ab` |
| `tinymix` | 440884 | `7f1d9974d571588f33162adbdecd2f5f4ce192bdc4dd2dce94adf66ecb744983` |
