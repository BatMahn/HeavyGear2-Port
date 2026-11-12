# Indeo 5 decoder

The Intel Indeo Video 5 decoder used for the game's movies. `indeo5.c`, `indeo5data.h`, `ivi.c`, `ivi.h`,
`ivi_dsp.c` and `ivi_dsp.h` come from FFmpeg n6.1 (`libavcodec/`), unchanged apart from their `#include`
lines (which now point at `ffshim.h`) and the removal of the FFCodec registration block. `ffshim.h` / `ffshim.c`
provide the small parts of libavutil/libavcodec they need (a little-endian bit reader, VLC tables,
memory helpers) and the `indeo5dec.h` C interface.

All of these files are licensed under the GNU Lesser General Public License, version 2.1 or later; see
`COPYING.LGPLv2.1`. The FFmpeg copyright notices are kept at the top of each original file.
