// DCT calculation with fftw (real)
// Copyright(c)2006 A.G.Balakhnin aka Fizick
// See legal notice in Copying.txt for more information

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .

#ifndef __MV_DCTFFTW__
#define __MV_DCTFFTW__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <fftw3.h>

typedef struct DCTFFTW {
    int sizex;
    int sizey;
    int dctmode;
    int bitsPerSample;

    float *fSrc;
    fftwf_plan dctplan;
    float *fSrcDCT;

    int dctshift;
    int dctshift0;
} DCTFFTW;


void dctInit(DCTFFTW *dct, int sizex, int sizey, int dctmode, int bitsPerSample);

void dctDeinit(DCTFFTW *dct);

void dctBytes2D(DCTFFTW *dct, const uint8_t *srcp, int src_pitch, uint8_t *dctp, int dct_pitch);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
