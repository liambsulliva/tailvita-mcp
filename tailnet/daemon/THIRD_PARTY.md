# Third-party material in this folder

- `sfo_make.py`, `reference/bgftp_param.sfo`, `reference/bgftp_param2.sfo` and
  `assets/` come from tailscale-vita (MIT, github.com/koalabuttz/tailscale-vita,
  revision `88a992c`: `scripts/sfo_make.py` and `spikes/06-bgapp/`). The SFO
  pair and the LiveArea images there were taken from GrapheneCt's BGFTP v3.24
  (MIT). The images are placeholders.
- `lowmem.c` installs the hook GrapheneCt's LowMemMode plugin installs
  (github.com/GrapheneCt/LowMemMode): the same SceShell offsets per module
  NID and the same replacement function. The code around it is ours.
