# Where parts of this folder come from

- `sfo_make.py` writes the SFO pair with the key layout of a working
  background-app package. The layout was worked out by tailscale-vita (MIT,
  github.com/koalabuttz/tailscale-vita, revision `88a992c`,
  `spikes/06-bgapp/`), which took it from GrapheneCt's BGFTP (MIT). No file
  from either project is included, and BGFTP is not needed on the Vita.
- `assets/template.xml` is the minimal LiveArea template from the same
  tailscale-vita spike. The images are ours (`assets/make_assets.sh`).
- `shell_module.c` installs the hook GrapheneCt's LowMemMode plugin installs
  (github.com/GrapheneCt/LowMemMode): the same SceShell offsets per module
  NID and the same replacement function. The code around it is ours.
