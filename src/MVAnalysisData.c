#include "MVAnalysisData.h"


void scaleThSCD(int *thscd1, int *thscd2, const MVAnalysisData *ad, const char *filter_name, char *error, size_t error_size) {
    if (error_size) {
        if (error[0])
            return;
        error[0] = '\0';
    }

    int maxSAD = 8 * 8 * 255;

    if (*thscd1 > maxSAD) {
        snprintf(error, error_size, "%s: thscd1 can be at most %d.", filter_name, maxSAD);
        return;
    }

    // SCD thresholds
    int referenceBlockSize = 8 * 8;
    *thscd1 = *thscd1 * (ad->nBlkSizeX * ad->nBlkSizeY) / referenceBlockSize;
    if (ad->nMotionFlags & MOTION_USE_CHROMA_MOTION)
        *thscd1 += *thscd1 / (ad->xRatioUV * ad->yRatioUV) * 2;

    int pixelMax = (1 << ad->bitsPerSample) - 1;
    *thscd1 = (int)((double)*thscd1 * pixelMax / 255.0 + 0.5);

    *thscd2 = *thscd2 * ad->nBlkX * ad->nBlkY / 256;
}


void adataFromVectorClip(struct MVAnalysisData *ad, VSNodeRef *clip, const char *filter_name, const char *vector_name, const VSAPI *vsapi, char *error, size_t error_size) {
    if (error_size) {
        if (error[0])
            return;
        error[0] = '\0';
    }

    char errorMsg[1024];
    const VSFrameRef *evil = vsapi->getFrame(0, clip, errorMsg, 1024);
    if (!evil) {
        snprintf(error, error_size, "%s: Failed to retrieve first frame from %s. Error message: %s", filter_name, vector_name, errorMsg);
        return;
    }

    // XXX This really should be passed as a frame property.
    const MVAnalysisData *pAnalyseFilter = (const MVAnalysisData *)(vsapi->getReadPtr(evil, 0) + sizeof(int));

    *ad = *pAnalyseFilter;

    vsapi->freeFrame(evil);
}


void adataCheckSimilarity(const MVAnalysisData *ad1, const MVAnalysisData *ad2, const char *filter_name1, const char *filter_name2, const char *vector_name, char *error, size_t error_size) {
    if (error_size) {
        if (error[0])
            return;
        error[0] = '\0';
    }

    if (ad1->nWidth != ad2->nWidth)
        snprintf(error, error_size, "%s: %s and %s have different widths.", filter_name1, filter_name2, vector_name);

    if (ad1->nHeight != ad2->nHeight)
        snprintf(error, error_size, "%s: %s and %s have different heights.", filter_name1, filter_name2, vector_name);

    if (ad1->nBlkSizeX != ad2->nBlkSizeX || ad1->nBlkSizeY != ad2->nBlkSizeY)
        snprintf(error, error_size, "%s: %s and %s have different block sizes.", filter_name1, filter_name2, vector_name);

    if (ad1->nPel != ad2->nPel)
        snprintf(error, error_size, "%s: %s and %s have different pel precision.", filter_name1, filter_name2, vector_name);

    if (ad1->nOverlapX != ad2->nOverlapX || ad1->nOverlapY != ad2->nOverlapY)
        snprintf(error, error_size, "%s: %s and %s have different overlap.", filter_name1, filter_name2, vector_name);

    if (ad1->xRatioUV != ad2->xRatioUV)
        snprintf(error, error_size, "%s: %s and %s have different horizontal subsampling.", filter_name1, filter_name2, vector_name);

    if (ad1->yRatioUV != ad2->yRatioUV)
        snprintf(error, error_size, "%s: %s and %s have different vertical subsampling.", filter_name1, filter_name2, vector_name);

    if (ad1->bitsPerSample != ad2->bitsPerSample)
        snprintf(error, error_size, "%s: %s and %s have different bit depths.", filter_name1, filter_name2, vector_name);
}
