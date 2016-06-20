// Make a motion compensate temporal denoiser
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


#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <VapourSynth.h>
#include <VSHelper.h>

#include "Bullshit.h"
#include "CopyCode.h"
#include "CommonFunctions.h"
#include "MaskFun.h"
#include "MVAnalysisData.h"
#include "Overlap.h"
#include "SimpleResize.h"


typedef struct MVBlockFPSData {
    VSNodeRef *node;
    VSVideoInfo vi;
    const VSVideoInfo *oldvi;
    const VSVideoInfo *supervi;

    VSNodeRef *super;
    VSNodeRef *mvbw;
    VSNodeRef *mvfw;

    int64_t num, den;
    int mode;
    double ml;
    int blend;
    int thscd1, thscd2;
    int isse;

    MVAnalysisData mvbw_data;
    MVAnalysisData mvfw_data;

    int nSuperHPad;
    int nSuperVPad;
    int nSuperPel;
    int nSuperModeYUV;
    int nSuperLevels;

    int nWidthUV;
    int nHeightUV;
    int nPitchY;
    int nPitchUV;
    int nWidthP;
    int nHeightP;
    int nWidthPUV;
    int nHeightPUV;
    int nBlkXP;
    int nBlkYP;

    SimpleResize upsizer;
    SimpleResize upsizerUV;

    int64_t fa, fb;

    int dstTempPitch;
    int dstTempPitchUV;
    int nBlkPitch;

    OverlapWindows *OverWins;
    OverlapWindows *OverWinsUV;

    OverlapsFunction OVERSLUMA;
    OverlapsFunction OVERSCHROMA;
    ToPixelsFunction ToPixels;
} MVBlockFPSData;


static void VS_CC mvblockfpsInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    (void)in;
    (void)out;
    (void)core;
    MVBlockFPSData *d = (MVBlockFPSData *)*instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}


static void MultMasks(uint8_t *smallmaskF, uint8_t *smallmaskB, uint8_t *smallmaskO, int nBlkX, int nBlkY) {
    for (int j = 0; j < nBlkY; j++) {
        for (int i = 0; i < nBlkX; i++)
            smallmaskO[i] = (smallmaskF[i] * smallmaskB[i]) / 255;
        smallmaskF += nBlkX;
        smallmaskB += nBlkX;
        smallmaskO += nBlkX;
    }
}


#define MEDIAN(PixelType) \
static inline PixelType MEDIAN_##PixelType(PixelType a, PixelType b, PixelType c) { \
    PixelType mn = VSMIN(a, b); \
    PixelType mx = VSMAX(a, b); \
    PixelType m = VSMIN(mx, c); \
    m = VSMAX(mn, m); \
    return m; \
}

MEDIAN(uint8_t)
MEDIAN(uint16_t)


#define RealResultBlock(PixelType) \
static void RealResultBlock_##PixelType(uint8_t *pDst, int dst_pitch, const uint8_t *pMCB, int MCB_pitch, const uint8_t *pMCF, int MCF_pitch, \
                            const uint8_t *pRef, int ref_pitch, const uint8_t *pSrc, int src_pitch, uint8_t *maskB, int mask_pitch, uint8_t *maskF, \
                            uint8_t *pOcc, int nBlkSizeX, int nBlkSizeY, int time256, int mode) { \
    if (mode == 0) { \
        for (int h = 0; h < nBlkSizeY; h++) { \
            for (int w = 0; w < nBlkSizeX; w++) { \
                const PixelType *pMCB_ = (const PixelType *)pMCB; \
                const PixelType *pMCF_ = (const PixelType *)pMCF; \
                PixelType *pDst_ = (PixelType *)pDst; \
 \
                int mca = (pMCB_[w] * time256 + pMCF_[w] * (256 - time256)) >> 8; /* MC fetched average */ \
                pDst_[w] = mca; \
            } \
            pDst += dst_pitch; \
            pMCB += MCB_pitch; \
            pMCF += MCF_pitch; \
        } \
    } else if (mode == 1) { \
        for (int h = 0; h < nBlkSizeY; h++) { \
            for (int w = 0; w < nBlkSizeX; w++) { \
                const PixelType *pMCB_ = (const PixelType *)pMCB; \
                const PixelType *pMCF_ = (const PixelType *)pMCF; \
                const PixelType *pRef_ = (const PixelType *)pRef; \
                const PixelType *pSrc_ = (const PixelType *)pSrc; \
                PixelType *pDst_ = (PixelType *)pDst; \
 \
                int mca = (pMCB_[w] * time256 + pMCF_[w] * (256 - time256)) >> 8; /* MC fetched average */ \
                int sta = MEDIAN_##PixelType(pRef_[w], pSrc_[w], mca);             /* static median */ \
                pDst_[w] = sta; \
            } \
            pDst += dst_pitch; \
            pMCB += MCB_pitch; \
            pMCF += MCF_pitch; \
            pRef += ref_pitch; \
            pSrc += src_pitch; \
        } \
    } else if (mode == 2) { \
        for (int h = 0; h < nBlkSizeY; h++) { \
            for (int w = 0; w < nBlkSizeX; w++) { \
                const PixelType *pMCB_ = (const PixelType *)pMCB; \
                const PixelType *pMCF_ = (const PixelType *)pMCF; \
                const PixelType *pRef_ = (const PixelType *)pRef; \
                const PixelType *pSrc_ = (const PixelType *)pSrc; \
                PixelType *pDst_ = (PixelType *)pDst; \
 \
                int avg = (pRef_[w] * time256 + pSrc_[w] * (256 - time256)) >> 8; /* simple temporal non-MC average */ \
                int dyn = MEDIAN_##PixelType(avg, pMCB_[w], pMCF_[w]);             /* dynamic median */ \
                pDst_[w] = dyn; \
            } \
            pDst += dst_pitch; \
            pMCB += MCB_pitch; \
            pMCF += MCF_pitch; \
            pRef += ref_pitch; \
            pSrc += src_pitch; \
        } \
    } else if (mode == 3 || mode == 6) { \
        for (int h = 0; h < nBlkSizeY; h++) { \
            for (int w = 0; w < nBlkSizeX; w++) { \
                const PixelType *pMCB_ = (const PixelType *)pMCB; \
                const PixelType *pMCF_ = (const PixelType *)pMCF; \
                PixelType *pDst_ = (PixelType *)pDst; \
 \
                pDst_[w] = (((maskB[w] * pMCF_[w] + (255 - maskB[w]) * pMCB_[w] + 255) >> 8) * time256 + \
                            ((maskF[w] * pMCB_[w] + (255 - maskF[w]) * pMCF_[w] + 255) >> 8) * (256 - time256)) >> \
                           8; \
            } \
            pDst += dst_pitch; \
            pMCB += MCB_pitch; \
            pMCF += MCF_pitch; \
            maskB += mask_pitch; \
            maskF += mask_pitch; \
        } \
    } else if (mode == 4 || mode == 7) { \
        for (int h = 0; h < nBlkSizeY; h++) { \
            for (int w = 0; w < nBlkSizeX; w++) { \
                const PixelType *pMCB_ = (const PixelType *)pMCB; \
                const PixelType *pMCF_ = (const PixelType *)pMCF; \
                const PixelType *pRef_ = (const PixelType *)pRef; \
                const PixelType *pSrc_ = (const PixelType *)pSrc; \
                PixelType *pDst_ = (PixelType *)pDst; \
 \
                int f = (maskF[w] * pMCB_[w] + (255 - maskF[w]) * pMCF_[w] + 255) >> 8; \
                int b = (maskB[w] * pMCF_[w] + (255 - maskB[w]) * pMCB_[w] + 255) >> 8; \
                int avg = (pRef_[w] * time256 + pSrc_[w] * (256 - time256) + 255) >> 8; /* simple temporal non-MC average */ \
                int m = (b * time256 + f * (256 - time256)) >> 8; \
                pDst_[w] = (avg * pOcc[w] + m * (255 - pOcc[w]) + 255) >> 8; \
            } \
            pDst += dst_pitch; \
            pMCB += MCB_pitch; \
            pMCF += MCF_pitch; \
            pRef += ref_pitch; \
            pSrc += src_pitch; \
            maskB += mask_pitch; \
            maskF += mask_pitch; \
            pOcc += mask_pitch; \
        } \
    } else if (mode == 5 || mode == 8) { \
        for (int h = 0; h < nBlkSizeY; h++) { \
            for (int w = 0; w < nBlkSizeX; w++) { \
                PixelType *pDst_ = (PixelType *)pDst; \
 \
                pDst_[w] = pOcc[w]; \
            } \
            pDst += dst_pitch; \
            pOcc += mask_pitch; \
        } \
    } \
}

RealResultBlock(uint8_t)
RealResultBlock(uint16_t)


static void ResultBlock(uint8_t *pDst, int dst_pitch, const uint8_t *pMCB, int MCB_pitch, const uint8_t *pMCF, int MCF_pitch,
                        const uint8_t *pRef, int ref_pitch, const uint8_t *pSrc, int src_pitch, uint8_t *maskB, int mask_pitch, uint8_t *maskF,
                        uint8_t *pOcc, int nBlkSizeX, int nBlkSizeY, int time256, int mode, int bitsPerSample) {
    if (bitsPerSample == 8)
        RealResultBlock_uint8_t(pDst, dst_pitch, pMCB, MCB_pitch, pMCF, MCF_pitch, pRef, ref_pitch, pSrc, src_pitch, maskB, mask_pitch, maskF, pOcc, nBlkSizeX, nBlkSizeY, time256, mode);
    else
        RealResultBlock_uint16_t(pDst, dst_pitch, pMCB, MCB_pitch, pMCF, MCF_pitch, pRef, ref_pitch, pSrc, src_pitch, maskB, mask_pitch, maskF, pOcc, nBlkSizeX, nBlkSizeY, time256, mode);
}


static const VSFrameRef *VS_CC mvblockfpsGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;

    MVBlockFPSData *d = (MVBlockFPSData *)*instanceData;

    if (activationReason == arInitial) {
        int off = d->mvbw_data.nDeltaFrame; // integer offset of reference frame

        int nleft = (int)(n * d->fa / d->fb);
        int nright = nleft + off;

        int time256 = (int)(((double)n * d->fa / d->fb - nleft) * 256 + 0.5);
        if (off > 1)
            time256 = time256 / off;

        if (time256 == 0) {
            vsapi->requestFrameFilter(VSMIN(nleft, d->oldvi->numFrames - 1), d->node, frameCtx);
            return 0;
        } else if (time256 == 256) {
            vsapi->requestFrameFilter(VSMIN(nright, d->oldvi->numFrames - 1), d->node, frameCtx);
            return 0;
        }

        if (nleft < d->oldvi->numFrames && nright < d->oldvi->numFrames) { // for the good estimation case
            vsapi->requestFrameFilter(nright, d->mvfw, frameCtx);  // requests nleft, nleft + off
            vsapi->requestFrameFilter(nleft, d->mvbw, frameCtx);   // requests nleft, nleft + off

            vsapi->requestFrameFilter(nleft, d->super, frameCtx);
            vsapi->requestFrameFilter(nright, d->super, frameCtx);
        }

        vsapi->requestFrameFilter(VSMIN(nleft, d->oldvi->numFrames - 1), d->node, frameCtx);

        if (d->blend)
            vsapi->requestFrameFilter(VSMIN(nright, d->oldvi->numFrames - 1), d->node, frameCtx);

    } else if (activationReason == arAllFramesReady) {
        int nleft = (int)(n * d->fa / d->fb);
        // intermediate product may be very large! Now I know how to multiply int64
        int time256 = (int)(((double)n * d->fa / d->fb - nleft) * 256 + 0.5);

        int off = d->mvbw_data.nDeltaFrame; // integer offset of reference frame
        // usually off must be = 1
        if (off > 1)
            time256 = time256 / off;

        int nright = nleft + off;

        if (time256 == 0)
            return vsapi->getFrameFilter(VSMIN(nleft, d->oldvi->numFrames - 1), d->node, frameCtx); // simply left
        else if (time256 == 256)
            return vsapi->getFrameFilter(VSMIN(nright, d->oldvi->numFrames - 1), d->node, frameCtx); // simply right

        FakeGroupOfPlanes fgopF, fgopB;

        fgopInit(&fgopF, &d->mvfw_data);
        fgopInit(&fgopB, &d->mvbw_data);

        int isUsableF = 0;
        int isUsableB = 0;

        if (nleft < d->oldvi->numFrames && nright < d->oldvi->numFrames) {
            // forward from current to next
            const VSFrameRef *mvF = vsapi->getFrameFilter(nright, d->mvfw, frameCtx);
            const VSMap *mvprops = vsapi->getFramePropsRO(mvF);
            fgopUpdate(&fgopF, (const int *)vsapi->propGetData(mvprops, prop_MVTools_vectors, 0, NULL));
            isUsableF = fgopIsUsable(&fgopF, d->thscd1, d->thscd2);
            vsapi->freeFrame(mvF);

            // backward from next to current
            const VSFrameRef *mvB = vsapi->getFrameFilter(nleft, d->mvbw, frameCtx);
            mvprops = vsapi->getFramePropsRO(mvB);
            fgopUpdate(&fgopB, (const int *)vsapi->propGetData(mvprops, prop_MVTools_vectors, 0, NULL));
            isUsableB = fgopIsUsable(&fgopB, d->thscd1, d->thscd2);
            vsapi->freeFrame(mvB);
        }

        const int nWidth = d->mvbw_data.nWidth;
        const int nHeight = d->mvbw_data.nHeight;
        const int nWidthUV = d->nWidthUV;
        const int nHeightUV = d->nHeightUV;
        const int nHeightP = d->nHeightP;
        const int nHeightPUV = d->nHeightPUV;
        const int mode = d->mode;
        const double ml = d->ml;
        const int blend = d->blend;
        const int isse = d->isse;
        const int xRatioUV = d->mvbw_data.xRatioUV;
        const int yRatioUV = d->mvbw_data.yRatioUV;
        const int nBlkX = d->mvbw_data.nBlkX;
        const int nBlkY = d->mvbw_data.nBlkY;
        const int nBlkSizeX = d->mvbw_data.nBlkSizeX;
        const int nBlkSizeY = d->mvbw_data.nBlkSizeY;
        const int nOverlapX = d->mvbw_data.nOverlapX;
        const int nOverlapY = d->mvbw_data.nOverlapY;
        const int nPel = d->mvbw_data.nPel;
        const int nPitchY = d->nPitchY;
        const int nPitchUV = d->nPitchUV;
        const int nBlkXP = d->nBlkXP;
        const int nBlkYP = d->nBlkYP;
        const int nWidth_B = nBlkX * (nBlkSizeX - nOverlapX) + nOverlapX;
        const int nHeight_B = nBlkY * (nBlkSizeY - nOverlapY) + nOverlapY;
        SimpleResize *upsizer = &d->upsizer;
        SimpleResize *upsizerUV = &d->upsizerUV;

        const int nSuperHPad = d->nSuperHPad;
        const int nSuperVPad = d->nSuperVPad;
        const int nSuperModeYUV = d->nSuperModeYUV;
        const int nSuperLevels = d->nSuperLevels;
        const int nSuperPel = d->nSuperPel;

        const int bitsPerSample = d->supervi->format->bitsPerSample;
        const int bytesPerSample = d->supervi->format->bytesPerSample;


        if (isUsableB && isUsableF) {
            uint8_t *pDst[3] = { NULL };
            const uint8_t *pRef[3] = { NULL };
            const uint8_t *pSrc[3] = { NULL };
            int nDstPitches[3] = { 0 };
            int nRefPitches[3] = { 0 };
            int nSrcPitches[3] = { 0 };

            // If both are usable, that means both nleft and nright are less than oldvi->numFrames. Thus there is no need to check nleft and nright here.
            const VSFrameRef *src = vsapi->getFrameFilter(nleft, d->super, frameCtx);
            const VSFrameRef *ref = vsapi->getFrameFilter(nright, d->super, frameCtx); //  right frame for  compensation
            VSFrameRef *dst = vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height, src, core);

            for (int i = 0; i < d->supervi->format->numPlanes; i++) {
                pDst[i] = vsapi->getWritePtr(dst, i);
                pRef[i] = vsapi->getReadPtr(ref, i);
                pSrc[i] = vsapi->getReadPtr(src, i);
                nDstPitches[i] = vsapi->getStride(dst, i);
                nRefPitches[i] = vsapi->getStride(ref, i);
                nSrcPitches[i] = vsapi->getStride(src, i);
            }

            MVGroupOfFrames pRefBGOF, pRefFGOF;

            mvgofInit(&pRefBGOF, nSuperLevels, nWidth, nHeight, nSuperPel, nSuperHPad, nSuperVPad, nSuperModeYUV, isse, xRatioUV, yRatioUV, d->supervi->format->bitsPerSample);
            mvgofInit(&pRefFGOF, nSuperLevels, nWidth, nHeight, nSuperPel, nSuperHPad, nSuperVPad, nSuperModeYUV, isse, xRatioUV, yRatioUV, d->supervi->format->bitsPerSample);

            mvgofUpdate(&pRefBGOF, (uint8_t **)pRef, nRefPitches);
            mvgofUpdate(&pRefFGOF, (uint8_t **)pSrc, nSrcPitches);

            MVPlane **pPlanesB = pRefBGOF.frames[0]->planes;
            MVPlane **pPlanesF = pRefFGOF.frames[0]->planes;


            uint8_t *MaskFullYB = (uint8_t *)malloc(nHeightP * nPitchY);
            uint8_t *MaskFullYF = (uint8_t *)malloc(nHeightP * nPitchY);
            uint8_t *MaskOccY = (uint8_t *)malloc(nHeightP * nPitchY);

            uint8_t *MaskFullUVB = NULL;
            uint8_t *MaskFullUVF = NULL;
            uint8_t *MaskOccUV = NULL;
            if (nSuperModeYUV & UVPLANES) {
                MaskFullUVB = (uint8_t *)malloc(nHeightPUV * nPitchUV);
                MaskFullUVF = (uint8_t *)malloc(nHeightPUV * nPitchUV);
                MaskOccUV = (uint8_t *)malloc(nHeightPUV * nPitchUV);
            }


            uint8_t *smallMaskB = NULL;
            uint8_t *smallMaskF = NULL;
            uint8_t *smallMaskO = NULL;

            memset(MaskFullYB, 0, nHeightP * nPitchY);
            memset(MaskFullYF, 0, nHeightP * nPitchY);

            int blocks = nBlkX * nBlkY;

            if (mode >= 3 && mode <= 8) {
                smallMaskB = (uint8_t *)malloc(nBlkXP * nBlkYP);
                smallMaskF = (uint8_t *)malloc(nBlkXP * nBlkYP);
                smallMaskO = (uint8_t *)malloc(nBlkXP * nBlkYP);


                if (mode <= 5) {
                    MakeVectorOcclusionMaskTime(&fgopF, 0, nBlkX, nBlkY, ml, 1.0, nPel, smallMaskF, nBlkXP, time256, nBlkSizeX - nOverlapX, nBlkSizeY - nOverlapY);
                    MakeVectorOcclusionMaskTime(&fgopB, 1, nBlkX, nBlkY, ml, 1.0, nPel, smallMaskB, nBlkXP, 256 - time256, nBlkSizeX - nOverlapX, nBlkSizeY - nOverlapY);
                } else { // 6 to 8
                    MakeSADMaskTime(&fgopF, nBlkX, nBlkY, 4.0 / (ml * nBlkSizeX * nBlkSizeY), 1.0, nPel, smallMaskF, nBlkXP, time256, nBlkSizeX - nOverlapX, nBlkSizeY - nOverlapY);
                    MakeSADMaskTime(&fgopB, nBlkX, nBlkY, 4.0 / (ml * nBlkSizeX * nBlkSizeY), 1.0, nPel, smallMaskB, nBlkXP, 256 - time256, nBlkSizeX - nOverlapX, nBlkSizeY - nOverlapY);
                }

                if (nBlkXP > nBlkX) // fill right
                    for (int j = 0; j < nBlkY; j++) {
                        smallMaskF[j * nBlkXP + nBlkX] = smallMaskF[j * nBlkXP + nBlkX - 1];
                        smallMaskB[j * nBlkXP + nBlkX] = smallMaskB[j * nBlkXP + nBlkX - 1];
                    }
                if (nBlkYP > nBlkY) // fill bottom
                    for (int i = 0; i < nBlkXP; i++) {
                        smallMaskF[nBlkXP * nBlkY + i] = smallMaskF[nBlkXP * (nBlkY - 1) + i];
                        smallMaskB[nBlkXP * nBlkY + i] = smallMaskB[nBlkXP * (nBlkY - 1) + i];
                    }


                // upsize small mask to full frame size
                simpleResize(upsizer, MaskFullYF, nPitchY, smallMaskF, nBlkXP);
                simpleResize(upsizer, MaskFullYB, nPitchY, smallMaskB, nBlkXP);

                if (nSuperModeYUV & UVPLANES) {
                    simpleResize(upsizerUV, MaskFullUVF, nPitchUV, smallMaskF, nBlkXP);
                    simpleResize(upsizerUV, MaskFullUVB, nPitchUV, smallMaskB, nBlkXP);
                }
            }

            if (mode == 4 || mode == 5 || mode == 7 || mode == 8) {
                // make final (both directions) occlusion mask
                MultMasks(smallMaskF, smallMaskB, smallMaskO, nBlkXP, nBlkYP);
                // upsize small mask to full frame size
                simpleResize(upsizer, MaskOccY, nPitchY, smallMaskO, nBlkXP);
                if (nSuperModeYUV & UVPLANES)
                    simpleResize(upsizerUV, MaskOccUV, nPitchUV, smallMaskO, nBlkXP);
            }

            // pointers
            uint8_t *pMaskFullYB = MaskFullYB;
            uint8_t *pMaskFullYF = MaskFullYF;
            uint8_t *pMaskFullUVB = MaskFullUVB;
            uint8_t *pMaskFullUVF = MaskFullUVF;
            uint8_t *pMaskOccY = MaskOccY;
            uint8_t *pMaskOccUV = MaskOccUV;

            pSrc[0] += nSuperHPad * bytesPerSample + nSrcPitches[0] * nSuperVPad; // add offset source in super
            pRef[0] += nSuperHPad * bytesPerSample + nRefPitches[0] * nSuperVPad;
            if (nSuperModeYUV & UVPLANES) {
                // XXX Seriously, how can this be right for anything that isn't 4:2:0?
                pSrc[1] += (nSuperHPad >> 1) * bytesPerSample + nSrcPitches[1] * (nSuperVPad >> 1);
                pSrc[2] += (nSuperHPad >> 1) * bytesPerSample + nSrcPitches[2] * (nSuperVPad >> 1);
                pRef[1] += (nSuperHPad >> 1) * bytesPerSample + nRefPitches[1] * (nSuperVPad >> 1);
                pRef[2] += (nSuperHPad >> 1) * bytesPerSample + nRefPitches[2] * (nSuperVPad >> 1);
            }

            if (nOverlapX == 0 && nOverlapY == 0) {
                // fetch image blocks
                for (int i = 0; i < blocks; i++) {
                    const FakeBlockData *blockB = fgopGetBlock(&fgopB, 0, i);
                    const FakeBlockData *blockF = fgopGetBlock(&fgopF, 0, i);

                    // luma
                    ResultBlock(pDst[0], nDstPitches[0],
                            mvpGetPointer(pPlanesB[0], blockB->x * nPel + ((blockB->vector.x * (256 - time256)) >> 8), blockB->y * nPel + ((blockB->vector.y * (256 - time256)) >> 8)),
                            pPlanesB[0]->nPitch,
                            mvpGetPointer(pPlanesF[0], blockF->x * nPel + ((blockF->vector.x * time256) >> 8), blockF->y * nPel + ((blockF->vector.y * time256) >> 8)),
                            pPlanesF[0]->nPitch,
                            pRef[0], nRefPitches[0],
                            pSrc[0], nSrcPitches[0],
                            pMaskFullYB, nPitchY,
                            pMaskFullYF, pMaskOccY,
                            nBlkSizeX, nBlkSizeY, time256, mode, bitsPerSample);
                    if (nSuperModeYUV & UVPLANES) {
                        // chroma u
                        ResultBlock(pDst[1], nDstPitches[1],
                                mvpGetPointer(pPlanesB[1], (blockB->x * nPel + ((blockB->vector.x * (256 - time256)) >> 8)) / xRatioUV, (blockB->y * nPel + ((blockB->vector.y * (256 - time256)) >> 8)) / yRatioUV),
                                pPlanesB[1]->nPitch,
                                mvpGetPointer(pPlanesF[1], (blockF->x * nPel + ((blockF->vector.x * time256) >> 8)) / xRatioUV, (blockF->y * nPel + ((blockF->vector.y * time256) >> 8)) / yRatioUV),
                                pPlanesF[1]->nPitch,
                                pRef[1], nRefPitches[1],
                                pSrc[1], nSrcPitches[1],
                                pMaskFullUVB, nPitchUV,
                                pMaskFullUVF, pMaskOccUV,
                                nBlkSizeX / xRatioUV, nBlkSizeY / yRatioUV, time256, mode, bitsPerSample);
                        // chroma v
                        ResultBlock(pDst[2], nDstPitches[2],
                                mvpGetPointer(pPlanesB[2], (blockB->x * nPel + ((blockB->vector.x * (256 - time256)) >> 8)) / xRatioUV, (blockB->y * nPel + ((blockB->vector.y * (256 - time256)) >> 8)) / yRatioUV),
                                pPlanesB[2]->nPitch,
                                mvpGetPointer(pPlanesF[2], (blockF->x * nPel + ((blockF->vector.x * time256) >> 8)) / xRatioUV, (blockF->y * nPel + ((blockF->vector.y * time256) >> 8)) / yRatioUV),
                                pPlanesF[2]->nPitch,
                                pRef[2], nRefPitches[2],
                                pSrc[2], nSrcPitches[2],
                                pMaskFullUVB, nPitchUV,
                                pMaskFullUVF, pMaskOccUV,
                                nBlkSizeX / xRatioUV, nBlkSizeY / yRatioUV, time256, mode, bitsPerSample);
                    }


                    // update pDsts
                    pDst[0] += nBlkSizeX * bytesPerSample;
                    pRef[0] += nBlkSizeX * bytesPerSample;
                    pSrc[0] += nBlkSizeX * bytesPerSample;
                    pMaskFullYB += nBlkSizeX;
                    pMaskFullYF += nBlkSizeX;
                    pMaskOccY += nBlkSizeX;
                    if (nSuperModeYUV & UVPLANES) {
                        pDst[1] += nBlkSizeX / xRatioUV * bytesPerSample;
                        pDst[2] += nBlkSizeX / xRatioUV * bytesPerSample;
                        pRef[1] += nBlkSizeX / xRatioUV * bytesPerSample;
                        pRef[2] += nBlkSizeX / xRatioUV * bytesPerSample;
                        pSrc[1] += nBlkSizeX / xRatioUV * bytesPerSample;
                        pSrc[2] += nBlkSizeX / xRatioUV * bytesPerSample;
                        pMaskFullUVB += nBlkSizeX / xRatioUV;
                        pMaskFullUVF += nBlkSizeX / xRatioUV;
                        pMaskOccUV += nBlkSizeX / xRatioUV;
                    }


                    if (!((i + 1) % nBlkX)) {
                        // blend rest right with time weight
                        Blend(pDst[0], pSrc[0], pRef[0], nBlkSizeY, nWidth - nBlkSizeX * nBlkX, nDstPitches[0], nSrcPitches[0], nRefPitches[0], time256, bitsPerSample);

                        pDst[0] += nBlkSizeY * nDstPitches[0] - nBlkSizeX * nBlkX * bytesPerSample;
                        pRef[0] += nBlkSizeY * nRefPitches[0] - nBlkSizeX * nBlkX * bytesPerSample;
                        pSrc[0] += nBlkSizeY * nSrcPitches[0] - nBlkSizeX * nBlkX * bytesPerSample;
                        pMaskFullYB += nBlkSizeY * nPitchY - nBlkSizeX * nBlkX;
                        pMaskFullYF += nBlkSizeY * nPitchY - nBlkSizeX * nBlkX;
                        pMaskOccY += nBlkSizeY * nPitchY - nBlkSizeX * nBlkX;

                        if (nSuperModeYUV & UVPLANES) {
                            Blend(pDst[1], pSrc[1], pRef[1], nBlkSizeY / yRatioUV, nWidthUV - (nBlkSizeX / xRatioUV) * nBlkX, nDstPitches[1], nSrcPitches[1], nRefPitches[1], time256, bitsPerSample);
                            Blend(pDst[2], pSrc[2], pRef[2], nBlkSizeY / yRatioUV, nWidthUV - (nBlkSizeX / xRatioUV) * nBlkX, nDstPitches[2], nSrcPitches[2], nRefPitches[2], time256, bitsPerSample);

                            pDst[1] += (nBlkSizeY / yRatioUV) * nDstPitches[1] - (nBlkSizeX / xRatioUV) * nBlkX * bytesPerSample;
                            pDst[2] += (nBlkSizeY / yRatioUV) * nDstPitches[2] - (nBlkSizeX / xRatioUV) * nBlkX * bytesPerSample;
                            pRef[1] += (nBlkSizeY / yRatioUV) * nRefPitches[1] - (nBlkSizeX / xRatioUV) * nBlkX * bytesPerSample;
                            pRef[2] += (nBlkSizeY / yRatioUV) * nRefPitches[2] - (nBlkSizeX / xRatioUV) * nBlkX * bytesPerSample;
                            pSrc[1] += (nBlkSizeY / yRatioUV) * nSrcPitches[1] - (nBlkSizeX / xRatioUV) * nBlkX * bytesPerSample;
                            pSrc[2] += (nBlkSizeY / yRatioUV) * nSrcPitches[2] - (nBlkSizeX / xRatioUV) * nBlkX * bytesPerSample;
                            pMaskFullUVB += (nBlkSizeY / yRatioUV) * nPitchUV - (nBlkSizeX / xRatioUV) * nBlkX;
                            pMaskFullUVF += (nBlkSizeY / yRatioUV) * nPitchUV - (nBlkSizeX / xRatioUV) * nBlkX;
                            pMaskOccUV += (nBlkSizeY / yRatioUV) * nPitchUV - (nBlkSizeX / xRatioUV) * nBlkX;
                        }
                    }
                }
                // blend rest bottom with time weight
                Blend(pDst[0], pSrc[0], pRef[0], nHeight - nBlkSizeY * nBlkY, nWidth, nDstPitches[0], nSrcPitches[0], nRefPitches[0], time256, bitsPerSample);
                if (nSuperModeYUV & UVPLANES) {
                    Blend(pDst[1], pSrc[1], pRef[1], nHeightUV - (nBlkSizeY / yRatioUV) * nBlkY, nWidthUV, nDstPitches[1], nSrcPitches[1], nRefPitches[1], time256, bitsPerSample);
                    Blend(pDst[2], pSrc[2], pRef[2], nHeightUV - (nBlkSizeY / yRatioUV) * nBlkY, nWidthUV, nDstPitches[2], nSrcPitches[2], nRefPitches[2], time256, bitsPerSample);
                }
            } else { // overlap
                // blend rest right with time weight
                Blend(pDst[0] + nWidth_B * bytesPerSample,
                      pSrc[0] + nWidth_B * bytesPerSample,
                      pRef[0] + nWidth_B * bytesPerSample,
                      nHeight_B,
                      nWidth - nWidth_B,
                      nDstPitches[0], nSrcPitches[0], nRefPitches[0], time256, bitsPerSample);
                if (nSuperModeYUV & UVPLANES) {
                    Blend(pDst[1] + nWidth_B / xRatioUV * bytesPerSample,
                          pSrc[1] + nWidth_B / xRatioUV * bytesPerSample,
                          pRef[1] + nWidth_B / xRatioUV * bytesPerSample,
                          nHeight_B / yRatioUV,
                          nWidthUV - nWidth_B / xRatioUV,
                          nDstPitches[1], nSrcPitches[1], nRefPitches[1], time256, bitsPerSample);
                    Blend(pDst[2] + nWidth_B / xRatioUV * bytesPerSample,
                          pSrc[2] + nWidth_B / xRatioUV * bytesPerSample,
                          pRef[2] + nWidth_B / xRatioUV * bytesPerSample,
                          nHeight_B / yRatioUV,
                          nWidthUV - nWidth_B / xRatioUV,
                          nDstPitches[2], nSrcPitches[2], nRefPitches[2], time256, bitsPerSample);
                }

                // blend rest bottom with time weight
                Blend(pDst[0] + nHeight_B * nDstPitches[0],
                      pSrc[0] + nHeight_B * nSrcPitches[0],
                      pRef[0] + nHeight_B * nRefPitches[0],
                      nHeight - nHeight_B,
                      nWidth,
                      nDstPitches[0], nSrcPitches[0], nRefPitches[0], time256, bitsPerSample);
                if (nSuperModeYUV & UVPLANES) {
                    Blend(pDst[1] + nDstPitches[1] * nHeight_B / yRatioUV,
                          pSrc[1] + nSrcPitches[1] * nHeight_B / yRatioUV,
                          pRef[1] + nRefPitches[1] * nHeight_B / yRatioUV,
                          nHeightUV - nHeight_B / yRatioUV,
                          nWidthUV,
                          nDstPitches[1], nSrcPitches[1], nRefPitches[1], time256, bitsPerSample);
                    Blend(pDst[2] + nDstPitches[2] * nHeight_B / yRatioUV,
                          pSrc[2] + nSrcPitches[2] * nHeight_B / yRatioUV,
                          pRef[2] + nRefPitches[2] * nHeight_B / yRatioUV,
                          nHeightUV - nHeight_B / yRatioUV,
                          nWidthUV,
                          nDstPitches[2], nSrcPitches[2], nRefPitches[2], time256, bitsPerSample);
                }

                int dstTempPitch = d->dstTempPitch;
                int dstTempPitchUV = d->dstTempPitchUV;
                int nBlkPitch = d->nBlkPitch;

                uint8_t *DstTemp = (uint8_t *)malloc(dstTempPitch * nHeight);
                uint8_t *DstTempU = NULL;
                uint8_t *DstTempV = NULL;
                if (nSuperModeYUV & UVPLANES) {
                    DstTempU = (uint8_t *)malloc(dstTempPitchUV * nHeightUV);
                    DstTempV = (uint8_t *)malloc(dstTempPitchUV * nHeightUV);
                }

                memset(DstTemp, 0, nHeight_B * dstTempPitch);
                if (nSuperModeYUV & UVPLANES) {
                    memset(DstTempU, 0, nHeight_B / yRatioUV * dstTempPitchUV);
                    memset(DstTempV, 0, nHeight_B / yRatioUV * dstTempPitchUV);
                }

                uint8_t *pDstTemp = DstTemp;
                uint8_t *pDstTempU = DstTempU;
                uint8_t *pDstTempV = DstTempV;

                uint8_t *TmpBlock = (uint8_t *)malloc(nBlkSizeY * nBlkPitch);

                for (int by = 0; by < nBlkY; by++) {
                    int wby = ((by + nBlkY - 3) / (nBlkY - 2)) * 3;
                    int xx = 0;
                    int xxUV = 0;
                    for (int bx = 0; bx < nBlkX; bx++) {
                        // select window
                        int wbx = (bx + nBlkX - 3) / (nBlkX - 2);
                        int16_t *winOver = overGetWindow(d->OverWins, wby + wbx);
                        int16_t *winOverUV = NULL;
                        if (nSuperModeYUV & UVPLANES)
                            winOverUV = overGetWindow(d->OverWinsUV, wby + wbx);

                        int i = by * nBlkX + bx;

                        const FakeBlockData *blockB = fgopGetBlock(&fgopB, 0, i);
                        const FakeBlockData *blockF = fgopGetBlock(&fgopF, 0, i);

                        // firstly calculate result block and write it to temporary place, not to dst
                        ResultBlock(TmpBlock, nBlkPitch,
                                    mvpGetPointer(pPlanesB[0], blockB->x * nPel + ((blockB->vector.x * (256 - time256)) >> 8), blockB->y * nPel + ((blockB->vector.y * (256 - time256)) >> 8)),
                                    pPlanesB[0]->nPitch,
                                    mvpGetPointer(pPlanesF[0], blockF->x * nPel + ((blockF->vector.x * time256) >> 8), blockF->y * nPel + ((blockF->vector.y * time256) >> 8)),
                                    pPlanesF[0]->nPitch,
                                    pRef[0] + xx * bytesPerSample, nRefPitches[0],
                                    pSrc[0] + xx * bytesPerSample, nSrcPitches[0],
                                    pMaskFullYB + xx, nPitchY,
                                    pMaskFullYF + xx, pMaskOccY + xx,
                                    nBlkSizeX, nBlkSizeY, time256, mode, bitsPerSample);
                        d->OVERSLUMA(pDstTemp + xx * bytesPerSample * 2, dstTempPitch, TmpBlock, nBlkPitch, winOver, nBlkSizeX);

                        if (nSuperModeYUV & UVPLANES) {
                            ResultBlock(TmpBlock, nBlkPitch,
                                        mvpGetPointer(pPlanesB[1], (blockB->x * nPel + ((blockB->vector.x * (256 - time256)) >> 8)) / xRatioUV, (blockB->y * nPel + ((blockB->vector.y * (256 - time256)) >> 8)) / yRatioUV),
                                        pPlanesB[1]->nPitch,
                                        mvpGetPointer(pPlanesF[1], (blockF->x * nPel + ((blockF->vector.x * time256) >> 8)) / xRatioUV, (blockF->y * nPel + ((blockF->vector.y * time256) >> 8)) / yRatioUV),
                                        pPlanesF[1]->nPitch,
                                        pRef[1] + xxUV * bytesPerSample, nRefPitches[1],
                                        pSrc[1] + xxUV * bytesPerSample, nSrcPitches[1],
                                        pMaskFullUVB + xxUV, nPitchUV,
                                        pMaskFullUVF + xxUV, pMaskOccUV + xxUV,
                                        nBlkSizeX / xRatioUV, nBlkSizeY / yRatioUV, time256, mode, bitsPerSample);
                            d->OVERSCHROMA(pDstTempU + xxUV * bytesPerSample * 2, dstTempPitchUV, TmpBlock, nBlkPitch, winOverUV, nBlkSizeX / xRatioUV);

                            ResultBlock(TmpBlock, nBlkPitch,
                                        mvpGetPointer(pPlanesB[2], (blockB->x * nPel + ((blockB->vector.x * (256 - time256)) >> 8)) / xRatioUV, (blockB->y * nPel + ((blockB->vector.y * (256 - time256)) >> 8)) / yRatioUV),
                                        pPlanesB[2]->nPitch,
                                        mvpGetPointer(pPlanesF[2], (blockF->x * nPel + ((blockF->vector.x * time256) >> 8)) / xRatioUV, (blockF->y * nPel + ((blockF->vector.y * time256) >> 8)) / yRatioUV),
                                        pPlanesF[2]->nPitch,
                                        pRef[2] + xxUV * bytesPerSample, nRefPitches[2],
                                        pSrc[2] + xxUV * bytesPerSample, nSrcPitches[2],
                                        pMaskFullUVB + xxUV, nPitchUV,
                                        pMaskFullUVF + xxUV, pMaskOccUV + xxUV,
                                        nBlkSizeX / xRatioUV, nBlkSizeY / yRatioUV, time256, mode, bitsPerSample);
                            d->OVERSCHROMA(pDstTempV + xxUV * bytesPerSample * 2, dstTempPitchUV, TmpBlock, nBlkPitch, winOverUV, nBlkSizeX / xRatioUV);
                        }

                        xx += (nBlkSizeX - nOverlapX);
                        xxUV += (nBlkSizeX - nOverlapX) / xRatioUV;
                    }

                    pDstTemp += dstTempPitch * (nBlkSizeY - nOverlapY);
                    pDstTempU += dstTempPitchUV * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pDstTempV += dstTempPitchUV * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pDst[0] += nDstPitches[0] * (nBlkSizeY - nOverlapY);
                    pDst[1] += nDstPitches[1] * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pDst[2] += nDstPitches[2] * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pRef[0] += nRefPitches[0] * (nBlkSizeY - nOverlapY);
                    pRef[1] += nRefPitches[1] * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pRef[2] += nRefPitches[2] * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pSrc[0] += nSrcPitches[0] * (nBlkSizeY - nOverlapY);
                    pSrc[1] += nSrcPitches[1] * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pSrc[2] += nSrcPitches[2] * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pMaskFullYB += nPitchY * (nBlkSizeY - nOverlapY);
                    pMaskFullUVB += nPitchUV * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pMaskFullYF += nPitchY * (nBlkSizeY - nOverlapY);
                    pMaskFullUVF += nPitchUV * (nBlkSizeY - nOverlapY) / yRatioUV;
                    pMaskOccY += nPitchY * (nBlkSizeY - nOverlapY);
                    pMaskOccUV += nPitchUV * (nBlkSizeY - nOverlapY) / yRatioUV;
                }

                for (int i = 0; i < d->supervi->format->numPlanes; i++)
                    pDst[i] = vsapi->getWritePtr(dst, i);

                d->ToPixels(pDst[0], nDstPitches[0], DstTemp, dstTempPitch, nWidth_B, nHeight_B, bitsPerSample);
                if (nSuperModeYUV & UVPLANES) {
                    d->ToPixels(pDst[1], nDstPitches[1], DstTempU, dstTempPitchUV, nWidth_B / xRatioUV, nHeight_B / yRatioUV, bitsPerSample);
                    d->ToPixels(pDst[2], nDstPitches[2], DstTempV, dstTempPitchUV, nWidth_B / xRatioUV, nHeight_B / yRatioUV, bitsPerSample);
                }

                free(TmpBlock);
                free(DstTemp);
                if (nSuperModeYUV & UVPLANES) {
                    free(DstTempU);
                    free(DstTempV);
                }
            }


            free(MaskFullYB);
            free(MaskFullYF);
            free(MaskOccY);
            if (nSuperModeYUV & UVPLANES) {
                free(MaskFullUVB);
                free(MaskFullUVF);
                free(MaskOccUV);
            }
            if (smallMaskB) {
                free(smallMaskB);
                free(smallMaskF);
                free(smallMaskO);
            }

            mvgofDeinit(&pRefBGOF);
            mvgofDeinit(&pRefFGOF);

            vsapi->freeFrame(src);
            vsapi->freeFrame(ref);

            fgopDeinit(&fgopF);
            fgopDeinit(&fgopB);

            return dst;
        } else { // poor estimation
            fgopDeinit(&fgopF);
            fgopDeinit(&fgopB);

            const VSFrameRef *src = vsapi->getFrameFilter(VSMIN(nleft, d->oldvi->numFrames - 1), d->node, frameCtx);

            if (blend) { //let's blend src with ref frames like ConvertFPS
                uint8_t *pDst[3];
                const uint8_t *pRef[3], *pSrc[3];
                int nDstPitches[3], nRefPitches[3], nSrcPitches[3];

                const VSFrameRef *ref = vsapi->getFrameFilter(VSMIN(nright, d->oldvi->numFrames - 1), d->node, frameCtx);

                VSFrameRef *dst = vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height, src, core);

                for (int i = 0; i < d->vi.format->numPlanes; i++) {
                    pDst[i] = vsapi->getWritePtr(dst, i);
                    pRef[i] = vsapi->getReadPtr(ref, i);
                    pSrc[i] = vsapi->getReadPtr(src, i);
                    nDstPitches[i] = vsapi->getStride(dst, i);
                    nRefPitches[i] = vsapi->getStride(ref, i);
                    nSrcPitches[i] = vsapi->getStride(src, i);
                }

                // blend with time weight
                Blend(pDst[0], pSrc[0], pRef[0], nHeight, nWidth, nDstPitches[0], nSrcPitches[0], nRefPitches[0], time256, bitsPerSample);
                if (nSuperModeYUV & UVPLANES) {
                    Blend(pDst[1], pSrc[1], pRef[1], nHeightUV, nWidthUV, nDstPitches[1], nSrcPitches[1], nRefPitches[1], time256, bitsPerSample);
                    Blend(pDst[2], pSrc[2], pRef[2], nHeightUV, nWidthUV, nDstPitches[2], nSrcPitches[2], nRefPitches[2], time256, bitsPerSample);
                }

                vsapi->freeFrame(src);
                vsapi->freeFrame(ref);

                return dst;
            } else {
                return src; // like ChangeFPS
            }
        }
    }

    return 0;
}


static void VS_CC mvblockfpsFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;

    MVBlockFPSData *d = (MVBlockFPSData *)instanceData;

    simpleDeinit(&d->upsizer);
    if (d->nSuperModeYUV & UVPLANES)
        simpleDeinit(&d->upsizerUV);

    if (d->mvbw_data.nOverlapX || d->mvbw_data.nOverlapY) {
        overDeinit(d->OverWins);
        free(d->OverWins);
        if (d->nSuperModeYUV & UVPLANES) {
            overDeinit(d->OverWinsUV);
            free(d->OverWinsUV);
        }
    }

    vsapi->freeNode(d->super);
    vsapi->freeNode(d->mvfw);
    vsapi->freeNode(d->mvbw);
    vsapi->freeNode(d->node);
    free(d);
}


static inline void setFPS(VSVideoInfo *vi, int64_t num, int64_t den) {
    if (num <= 0 || den <= 0) {
        vi->fpsNum = 0;
        vi->fpsDen = 1;
    } else {
        int64_t x = num;
        int64_t y = den;
        while (y) {
            int64_t t = x % y;
            x = y;
            y = t;
        }
        vi->fpsNum = num / x;
        vi->fpsDen = den / x;
    }
}


static void selectFunctions(MVBlockFPSData *d) {
    const int xRatioUV = d->mvbw_data.xRatioUV;
    const int yRatioUV = d->mvbw_data.yRatioUV;
    const int nBlkSizeX = d->mvbw_data.nBlkSizeX;
    const int nBlkSizeY = d->mvbw_data.nBlkSizeY;

    OverlapsFunction overs[33][33];

    if (d->vi.format->bitsPerSample == 8) {
        overs[2][2] = mvtools_overlaps_2x2_uint16_t_uint8_t_c;
        overs[2][4] = mvtools_overlaps_2x4_uint16_t_uint8_t_c;
        overs[4][2] = mvtools_overlaps_4x2_uint16_t_uint8_t_c;
        overs[4][4] = mvtools_overlaps_4x4_uint16_t_uint8_t_c;
        overs[4][8] = mvtools_overlaps_4x8_uint16_t_uint8_t_c;
        overs[8][1] = mvtools_overlaps_8x1_uint16_t_uint8_t_c;
        overs[8][2] = mvtools_overlaps_8x2_uint16_t_uint8_t_c;
        overs[8][4] = mvtools_overlaps_8x4_uint16_t_uint8_t_c;
        overs[8][8] = mvtools_overlaps_8x8_uint16_t_uint8_t_c;
        overs[8][16] = mvtools_overlaps_8x16_uint16_t_uint8_t_c;
        overs[16][1] = mvtools_overlaps_16x1_uint16_t_uint8_t_c;
        overs[16][2] = mvtools_overlaps_16x2_uint16_t_uint8_t_c;
        overs[16][4] = mvtools_overlaps_16x4_uint16_t_uint8_t_c;
        overs[16][8] = mvtools_overlaps_16x8_uint16_t_uint8_t_c;
        overs[16][16] = mvtools_overlaps_16x16_uint16_t_uint8_t_c;
        overs[16][32] = mvtools_overlaps_16x32_uint16_t_uint8_t_c;
        overs[32][8] = mvtools_overlaps_32x8_uint16_t_uint8_t_c;
        overs[32][16] = mvtools_overlaps_32x16_uint16_t_uint8_t_c;
        overs[32][32] = mvtools_overlaps_32x32_uint16_t_uint8_t_c;

        d->ToPixels = ToPixels_uint16_t_uint8_t;

        if (d->isse) {
#if defined(MVTOOLS_X86)
            overs[4][2] = mvtools_overlaps_4x2_sse2;
            overs[4][4] = mvtools_overlaps_4x4_sse2;
            overs[4][8] = mvtools_overlaps_4x8_sse2;
            overs[8][1] = mvtools_overlaps_8x1_sse2;
            overs[8][2] = mvtools_overlaps_8x2_sse2;
            overs[8][4] = mvtools_overlaps_8x4_sse2;
            overs[8][8] = mvtools_overlaps_8x8_sse2;
            overs[8][16] = mvtools_overlaps_8x16_sse2;
            overs[16][1] = mvtools_overlaps_16x1_sse2;
            overs[16][2] = mvtools_overlaps_16x2_sse2;
            overs[16][4] = mvtools_overlaps_16x4_sse2;
            overs[16][8] = mvtools_overlaps_16x8_sse2;
            overs[16][16] = mvtools_overlaps_16x16_sse2;
            overs[16][32] = mvtools_overlaps_16x32_sse2;
            overs[32][8] = mvtools_overlaps_32x8_sse2;
            overs[32][16] = mvtools_overlaps_32x16_sse2;
            overs[32][32] = mvtools_overlaps_32x32_sse2;
#endif
        }
    } else {
        overs[2][2] = mvtools_overlaps_2x2_uint32_t_uint16_t_c;
        overs[2][4] = mvtools_overlaps_2x4_uint32_t_uint16_t_c;
        overs[4][2] = mvtools_overlaps_4x2_uint32_t_uint16_t_c;
        overs[4][4] = mvtools_overlaps_4x4_uint32_t_uint16_t_c;
        overs[4][8] = mvtools_overlaps_4x8_uint32_t_uint16_t_c;
        overs[8][1] = mvtools_overlaps_8x1_uint32_t_uint16_t_c;
        overs[8][2] = mvtools_overlaps_8x2_uint32_t_uint16_t_c;
        overs[8][4] = mvtools_overlaps_8x4_uint32_t_uint16_t_c;
        overs[8][8] = mvtools_overlaps_8x8_uint32_t_uint16_t_c;
        overs[8][16] = mvtools_overlaps_8x16_uint32_t_uint16_t_c;
        overs[16][1] = mvtools_overlaps_16x1_uint32_t_uint16_t_c;
        overs[16][2] = mvtools_overlaps_16x2_uint32_t_uint16_t_c;
        overs[16][4] = mvtools_overlaps_16x4_uint32_t_uint16_t_c;
        overs[16][8] = mvtools_overlaps_16x8_uint32_t_uint16_t_c;
        overs[16][16] = mvtools_overlaps_16x16_uint32_t_uint16_t_c;
        overs[16][32] = mvtools_overlaps_16x32_uint32_t_uint16_t_c;
        overs[32][8] = mvtools_overlaps_32x8_uint32_t_uint16_t_c;
        overs[32][16] = mvtools_overlaps_32x16_uint32_t_uint16_t_c;
        overs[32][32] = mvtools_overlaps_32x32_uint32_t_uint16_t_c;

        d->ToPixels = ToPixels_uint32_t_uint16_t;
    }

    d->OVERSLUMA = overs[nBlkSizeX][nBlkSizeY];
    d->OVERSCHROMA = overs[nBlkSizeX / xRatioUV][nBlkSizeY / yRatioUV];
}


static void VS_CC mvblockfpsCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;

    MVBlockFPSData d;
    MVBlockFPSData *data;

    int err;

    d.num = vsapi->propGetInt(in, "num", 0, &err);
    if (err)
        d.num = 25;

    d.den = vsapi->propGetInt(in, "den", 0, &err);
    if (err)
        d.den = 1;

    d.mode = int64ToIntS(vsapi->propGetInt(in, "mode", 0, &err));
    if (err)
        d.mode = 3;

    d.ml = vsapi->propGetInt(in, "ml", 0, &err);
    if (err)
        d.ml = 100.0;

    d.blend = !!vsapi->propGetInt(in, "blend", 0, &err);
    if (err)
        d.blend = 1;

    d.thscd1 = int64ToIntS(vsapi->propGetInt(in, "thscd1", 0, &err));
    if (err)
        d.thscd1 = MV_DEFAULT_SCD1;

    d.thscd2 = int64ToIntS(vsapi->propGetInt(in, "thscd2", 0, &err));
    if (err)
        d.thscd2 = MV_DEFAULT_SCD2;

    d.isse = !!vsapi->propGetInt(in, "isse", 0, &err);
    if (err)
        d.isse = 1;


    if (d.mode < 0 || d.mode > 8) {
        vsapi->setError(out, "BlockFPS: mode must be between 0 and 8 (inclusive).");
        return;
    }


    d.super = vsapi->propGetNode(in, "super", 0, NULL);

#define ERROR_SIZE 1024
    char errorMsg[ERROR_SIZE] = "BlockFPS: failed to retrieve first frame from super clip. Error message: ";
    size_t errorLen = strlen(errorMsg);
    const VSFrameRef *evil = vsapi->getFrame(0, d.super, errorMsg + errorLen, ERROR_SIZE - errorLen);
#undef ERROR_SIZE
    if (!evil) {
        vsapi->setError(out, errorMsg);
        vsapi->freeNode(d.super);
        return;
    }
    const VSMap *props = vsapi->getFramePropsRO(evil);
    int evil_err[6];
    int nHeightS = int64ToIntS(vsapi->propGetInt(props, "Super_height", 0, &evil_err[0]));
    d.nSuperHPad = int64ToIntS(vsapi->propGetInt(props, "Super_hpad", 0, &evil_err[1]));
    d.nSuperVPad = int64ToIntS(vsapi->propGetInt(props, "Super_vpad", 0, &evil_err[2]));
    d.nSuperPel = int64ToIntS(vsapi->propGetInt(props, "Super_pel", 0, &evil_err[3]));
    d.nSuperModeYUV = int64ToIntS(vsapi->propGetInt(props, "Super_modeyuv", 0, &evil_err[4]));
    d.nSuperLevels = int64ToIntS(vsapi->propGetInt(props, "Super_levels", 0, &evil_err[5]));
    vsapi->freeFrame(evil);

    for (int i = 0; i < 6; i++)
        if (evil_err[i]) {
            vsapi->setError(out, "BlockFPS: required properties not found in first frame of super clip. Maybe clip didn't come from mv.Super? Was the first frame trimmed away?");
            vsapi->freeNode(d.super);
            return;
        }


    d.mvbw = vsapi->propGetNode(in, "mvbw", 0, NULL);
    d.mvfw = vsapi->propGetNode(in, "mvfw", 0, NULL);

    // There is another variable called "error" a bit lower.
    {
#define ERROR_SIZE 512
        char error[ERROR_SIZE + 1] = { 0 };
        const char *filter_name = "BlockFPS";

        adataFromVectorClip(&d.mvbw_data, d.mvbw, filter_name, "mvbw", vsapi, error, ERROR_SIZE);
        adataFromVectorClip(&d.mvfw_data, d.mvfw, filter_name, "mvfw", vsapi, error, ERROR_SIZE);

        scaleThSCD(&d.thscd1, &d.thscd2, &d.mvbw_data, filter_name, error, ERROR_SIZE);

        adataCheckSimilarity(&d.mvbw_data, &d.mvfw_data, filter_name, "mvbw", "mvfw", error, ERROR_SIZE);
#undef ERROR_SIZE

        if (error[0]) {
            vsapi->setError(out, error);

            vsapi->freeNode(d.super);
            vsapi->freeNode(d.mvfw);
            vsapi->freeNode(d.mvbw);
            return;
        }
    }


    if (d.mvbw_data.nDeltaFrame <= 0 || d.mvfw_data.nDeltaFrame <= 0) {
        vsapi->setError(out, "BlockFPS: cannot use motion vectors with absolute frame references.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.mvfw);
        vsapi->freeNode(d.mvbw);
        return;
    }

    // XXX Alternatively, use both clips' delta as offsets in GetFrame.
    if (d.mvbw_data.nDeltaFrame != d.mvfw_data.nDeltaFrame) {
        vsapi->setError(out, "BlockFPS: mvbw and mvfw must be generated with the same delta.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.mvfw);
        vsapi->freeNode(d.mvbw);
        return;
    }

    // Make sure the motion vector clips are correct.
    if (!d.mvbw_data.isBackward || d.mvfw_data.isBackward) {
        if (!d.mvbw_data.isBackward)
            vsapi->setError(out, "BlockFPS: mvbw must be generated with isb=True.");
        else
            vsapi->setError(out, "BlockFPS: mvfw must be generated with isb=False.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.mvfw);
        vsapi->freeNode(d.mvbw);
        return;
    }


    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.oldvi = vsapi->getVideoInfo(d.node);
    d.vi = *d.oldvi;


    if (d.vi.fpsNum == 0 || d.vi.fpsDen == 0) {
        vsapi->setError(out, "BlockFPS: The input clip must have a frame rate. Invoke AssumeFPS if necessary.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.mvfw);
        vsapi->freeNode(d.mvbw);
        vsapi->freeNode(d.node);
        return;
    }

    int64_t numeratorOld = d.vi.fpsNum;
    int64_t denominatorOld = d.vi.fpsDen;
    int64_t numerator, denominator;

    if (d.num != 0 && d.den != 0) {
        numerator = d.num;
        denominator = d.den;
    } else {
        numerator = numeratorOld * 2; // double fps by default
        denominator = denominatorOld;
    }

    //  safe for big numbers since v2.1
    d.fa = denominator * numeratorOld;
    d.fb = numerator * denominatorOld;
    int64_t fgcd = gcd(d.fa, d.fb); // general common divisor
    d.fa /= fgcd;
    d.fb /= fgcd;

    setFPS(&d.vi, numerator, denominator);

    d.vi.numFrames = (int)(1 + (d.vi.numFrames - 1) * d.fb / d.fa);


    d.supervi = vsapi->getVideoInfo(d.super);
    int nSuperWidth = d.supervi->width;

    if (d.mvbw_data.nHeight != nHeightS ||
        d.mvbw_data.nWidth != nSuperWidth - d.nSuperHPad * 2 ||
        d.mvbw_data.nWidth != d.vi.width ||
        d.mvbw_data.nHeight != d.vi.height ||
        d.mvbw_data.nPel != d.nSuperPel) {
        vsapi->setError(out, "BlockFPS: wrong source or super clip frame size.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.mvfw);
        vsapi->freeNode(d.mvbw);
        vsapi->freeNode(d.node);
        return;
    }

    if (!isConstantFormat(&d.vi) || d.vi.format->bitsPerSample > 16 || d.vi.format->sampleType != stInteger || d.vi.format->subSamplingW > 1 || d.vi.format->subSamplingH > 1 || (d.vi.format->colorFamily != cmYUV && d.vi.format->colorFamily != cmGray)) {
        vsapi->setError(out, "BlockFPS: input clip must be GRAY, 420, 422, 440, or 444, up to 16 bits, with constant dimensions.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.mvfw);
        vsapi->freeNode(d.mvbw);
        vsapi->freeNode(d.node);
        return;
    }

    if (d.vi.format->bitsPerSample > 8)
        d.isse = 0;


    d.nBlkXP = (d.mvbw_data.nBlkX * (d.mvbw_data.nBlkSizeX - d.mvbw_data.nOverlapX) + d.mvbw_data.nOverlapX < d.mvbw_data.nWidth) ? d.mvbw_data.nBlkX + 1 : d.mvbw_data.nBlkX;
    d.nBlkYP = (d.mvbw_data.nBlkY * (d.mvbw_data.nBlkSizeY - d.mvbw_data.nOverlapY) + d.mvbw_data.nOverlapY < d.mvbw_data.nHeight) ? d.mvbw_data.nBlkY + 1 : d.mvbw_data.nBlkY;
    d.nWidthP = d.nBlkXP * (d.mvbw_data.nBlkSizeX - d.mvbw_data.nOverlapX) + d.mvbw_data.nOverlapX;
    d.nHeightP = d.nBlkYP * (d.mvbw_data.nBlkSizeY - d.mvbw_data.nOverlapY) + d.mvbw_data.nOverlapY;

    d.nWidthPUV = d.nWidthP / d.mvbw_data.xRatioUV;
    d.nHeightPUV = d.nHeightP / d.mvbw_data.yRatioUV;
    d.nHeightUV = d.mvbw_data.nHeight / d.mvbw_data.yRatioUV;
    d.nWidthUV = d.mvbw_data.nWidth / d.mvbw_data.xRatioUV;

    d.nPitchY = (d.nWidthP + 15) & (~15);
    d.nPitchUV = (d.nWidthPUV + 15) & (~15);


    simpleInit(&d.upsizer, d.nWidthP, d.nHeightP, d.nBlkXP, d.nBlkYP);
    if (d.nSuperModeYUV & UVPLANES)
        simpleInit(&d.upsizerUV, d.nWidthPUV, d.nHeightPUV, d.nBlkXP, d.nBlkYP);

    if (d.mvbw_data.nOverlapX || d.mvbw_data.nOverlapY) {
        d.OverWins = (OverlapWindows *)malloc(sizeof(OverlapWindows));
        overInit(d.OverWins, d.mvbw_data.nBlkSizeX, d.mvbw_data.nBlkSizeY, d.mvbw_data.nOverlapX, d.mvbw_data.nOverlapY);
        if (d.nSuperModeYUV & UVPLANES) {
            d.OverWinsUV = (OverlapWindows *)malloc(sizeof(OverlapWindows));
            overInit(d.OverWinsUV, d.mvbw_data.nBlkSizeX / d.mvbw_data.xRatioUV, d.mvbw_data.nBlkSizeY / d.mvbw_data.yRatioUV, d.mvbw_data.nOverlapX / d.mvbw_data.xRatioUV, d.mvbw_data.nOverlapY / d.mvbw_data.yRatioUV);
        }
    }

    d.dstTempPitch = ((d.mvbw_data.nWidth + 15) / 16) * 16 * d.vi.format->bytesPerSample * 2;
    d.dstTempPitchUV = (((d.mvbw_data.nWidth / d.mvbw_data.xRatioUV) + 15) / 16) * 16 * d.vi.format->bytesPerSample * 2;
    d.nBlkPitch = ((d.mvbw_data.nBlkSizeX + 15) & (~15)) * d.vi.format->bytesPerSample;


    selectFunctions(&d);


    data = (MVBlockFPSData *)malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "BlockFPS", mvblockfpsInit, mvblockfpsGetFrame, mvblockfpsFree, fmParallel, 0, data, core);

    // AssumeFPS sets the _DurationNum and _DurationDen properties.
    VSNodeRef *node = vsapi->propGetNode(out, "clip", 0, NULL);
    VSMap *args = vsapi->createMap();
    vsapi->propSetNode(args, "clip", node, paReplace);
    vsapi->freeNode(node);
    vsapi->propSetInt(args, "fpsnum", d.vi.fpsNum, paReplace);
    vsapi->propSetInt(args, "fpsden", d.vi.fpsDen, paReplace);
    VSPlugin *stdPlugin = vsapi->getPluginById("com.vapoursynth.std", core);
    VSMap *ret = vsapi->invoke(stdPlugin, "AssumeFPS", args);
    if (vsapi->getError(ret)) {
#define ERROR_SIZE 512
        char error_msg[ERROR_SIZE + 1] = { 0 };
        snprintf(error_msg, ERROR_SIZE, "BlockFPS: Failed to invoke AssumeFPS. Error message: %s", vsapi->getError(ret));
#undef ERROR_SIZE
        vsapi->setError(out, error_msg);

        vsapi->freeMap(args);
        vsapi->freeMap(ret);
        return;
    }
    node = vsapi->propGetNode(ret, "clip", 0, NULL);
    vsapi->freeMap(ret);
    vsapi->clearMap(args);
    vsapi->propSetNode(args, "clip", node, paReplace);
    vsapi->freeNode(node);
    ret = vsapi->invoke(stdPlugin, "Cache", args);
    vsapi->freeMap(args);
    if (vsapi->getError(ret)) {
#define ERROR_SIZE 512
        char error_msg[ERROR_SIZE + 1] = { 0 };
        snprintf(error_msg, ERROR_SIZE, "BlockFPS: Failed to invoke Cache. Error message: %s", vsapi->getError(ret));
#undef ERROR_SIZE
        vsapi->setError(out, error_msg);

        vsapi->freeMap(ret);
        return;
    }
    node = vsapi->propGetNode(ret, "clip", 0, NULL);
    vsapi->freeMap(ret);
    vsapi->propSetNode(out, "clip", node, paReplace);
    vsapi->freeNode(node);
}


void mvblockfpsRegister(VSRegisterFunction registerFunc, VSPlugin *plugin) {
    registerFunc("BlockFPS",
                 "clip:clip;"
                 "super:clip;"
                 "mvbw:clip;"
                 "mvfw:clip;"
                 "num:int:opt;"
                 "den:int:opt;"
                 "mode:int:opt;"
                 "ml:float:opt;"
                 "blend:int:opt;"
                 "thscd1:int:opt;"
                 "thscd2:int:opt;"
                 "isse:int:opt;",
                 mvblockfpsCreate, 0, plugin);
}
