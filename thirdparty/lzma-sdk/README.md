# LZMA SDK 26.06, its 7z and xz decoders

The C files `librecomp/src/rom_archive.cpp` needs from Igor Pavlov's LZMA SDK 26.06 (`lzma2606.7z`, from
https://www.7-zip.org/sdk.html or the 7-Zip releases at https://github.com/ip7z/7zip), unchanged: the 7z archive
reader with its LZMA, LZMA2, PPMd, BCJ, BCJ2 and delta decoders, and the xz stream decoder. The LZMA SDK is in the
public domain.

Built single-threaded (`Z7_ST`) with PPMd (`Z7_PPMD_SUPPORT`), and only with `RECOMP_ROM_ARCHIVES` (librecomp's
CMakeLists.txt). To move to a newer SDK, copy the same files from its `C/` folder over these.
