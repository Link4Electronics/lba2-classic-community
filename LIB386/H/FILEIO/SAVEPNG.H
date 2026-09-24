// ──────────────────────────────────────────────────────────────────────────
// PNG Screenshot Support (using stb_image_write)
// ──────────────────────────────────────────────────────────────────────────
#ifndef SAVEPNG_H
#define SAVEPNG_H

#include <SYSTEM.H>

extern void SavePNG(char *filename, void *screen, S32 xres, S32 yres, void *ptrpalette);

/* Top-down 0x00RRGGBB pixels (no palette). Used for GPU-presented frames. */
extern void SavePNGRGB(char *filename, void *rgbPixels, S32 xres, S32 yres);

#endif // SAVEPNG_H
