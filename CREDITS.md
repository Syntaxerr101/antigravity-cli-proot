# Credits and licence notes

**Dev:** Simo — `tg:syn74x_3rr`

## Attribution chain

```
google-antigravity/antigravity-cli          the official upstream CLI
        |
        v
wallentx/antigravity-cli-termux              native Termux standalone port
        |
        v
antigravity-cli-proot                       this package (PRoot-only)
```

### google-antigravity/antigravity-cli

- Repository: https://github.com/google-antigravity/antigravity-cli
- Author: Google
- Contribution here: the engine binary that is downloaded at install time and
  patched. The engine is not redistributed in this package; the installer
  fetches it from Google's official release manifest and verifies it against
  the published SHA-512 checksum.

### wallentx/antigravity-cli-termux

- Repository: https://github.com/wallentx/antigravity-cli-termux
- Author: wallentx
- Contribution here: the relocatable twin-binary layout (a small C bootstrapper
  next to the patched engine), the VA39 memory patch rules, the self-update
  mechanism and the bootstrapper source this package starts from.

### This package

- `src/agy_proot.c` is the PRoot-only derivative of that bootstrapper. The
  Termux specific code paths were removed and the rest adjusted for a
  glibc-only target.
- `install.sh` is new: it detects the host, can bootstrap a PRoot userland
  from native Termux, installs the build requirements, downloads and verifies
  the engine, builds the pair and installs it transactionally.
- `src/patch_va39.py` is the reference implementation of the VA39 patcher, kept
  here so the patch rules can be audited independently of the C implementation.

### modded-ubuntu/modded-ubuntu

- Repository: https://github.com/modded-ubuntu/modded-ubuntu
- Licence: Apache-2.0
- Contribution here: the PRoot Ubuntu userland this package was tested
  against. It registers its container with `proot-distro` under the name
  `ubuntu`, which is the container the installer looks for. It builds on the
  official Ubuntu rootfs provided by
  [termux/proot-distro](https://github.com/termux/proot-distro), which is
  credited by that project.

## Licence

Neither the upstream repository nor the native Termux port ships a licence
file. The upstream repository declares no licence, and the native Termux port
does not add one either.

For the same reason no licence is imposed on the files in this package. The
code is provided as-is, without any warranty, for use with the Antigravity
CLI. The redistribution terms of the upstream engine apply to the engine
itself and are unaffected by this package: the engine is not included here, it
is downloaded from Google's own release service at install time.

If you publish a derivative of this package, keep this attribution chain
intact.
