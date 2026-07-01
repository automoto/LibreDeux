# Dependencies

## ReXGlue SDK

The official ReXGlue SDK ships as the `tools/rexglue-sdk` git submodule, pinned to commit
`e8ce24f` (SDK `0.8.1.4`). Army of Two builds against vanilla ReXGlue.

Fetch it with the repo:

```
git clone --recurse-submodules <repo_url>
# or, if already cloned:
git submodule update --init --recursive
```

Build it once (see [building.md](building.md)). On Windows, `scripts/repair_sdk_symlinks.py`
first materializes the SDK's libmspack symlinks that Git checked out as text placeholders.
The SDK and this project are both built with `-mssse3`, the baseline Army of Two needs.

## Game Data

This project provides no game files. Put your own legal disc image in `iso/` and run
`python scripts/extract_iso.py` to produce `game/` (pure-Python XDVDFS extractor, standard
library only). See [extraction.md](extraction.md).
