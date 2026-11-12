# Reverse-engineering helpers

Python reference decoders written while tracing `hg2sim.dll`. They validate each format against
every file in the game (exact byte counts) and are kept as executable documentation.
They read your own extracted game files; nothing from the game is stored here.

- `prj.py <simpack.prj> <simpack.map> <outdir>` — extract a PRJ archive
- `lzb.py '<Terrain/*.MR~>' [outdir]` — decompress `~` files
- `mdl.py` / `xb.py` — parse every `.M` / `.XB` in `$HG2_SIM` (an extracted simpack)
- `mission.py` — parse every mission in `$HG2_DATA/Mission`
- `mrm.py <file.MRM>` — dump a decompressed terrain

See `docs/FORMATS.md` for the format descriptions and the engine functions they come from.
