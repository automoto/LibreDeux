# Extraction

Use only a legally obtained compatible game copy. Put your Army of Two disc image
anywhere under `iso/` (any subfolder), then run:

```
python scripts\extract_iso.py
```

This auto-detects the first `*.iso` under `iso/` and extracts it to `game/` (producing
`game/default.xex`). To point at a specific image:

```
python scripts\extract_iso.py "iso\Army of Two (USA).iso"
```

The extractor is pure Python (standard library) and parses the Xbox 360 XDVDFS
filesystem directly. If it reports that the disc image may be encrypted, decrypt/redump it
or use a legal local extractor such as `extract-xiso`, then place the files in `game/` so
`game/default.xex` exists.
