/* Standalone proof: drive the VE2 hardware decoder (libvdecoder) directly.
   Decodes an H.264 Annex-B elementary stream to NV12. This validates the cedar
   decode loop in isolation before we wrap it in a VAAPI VLD driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vdecoder.h"
#include "memoryAdapter.h"
#include "veAdapter.h"
#include "veInterface.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/test.h264";
    int codec = (argc > 2 && !strcmp(argv[2], "h265")) ? VIDEO_CODEC_FORMAT_H265
                                                        : VIDEO_CODEC_FORMAT_H264;
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(sz);
    if (fread(data, 1, sz, f) != (size_t)sz) { printf("short read\n"); return 1; }
    fclose(f);
    printf("loaded %ld bytes from %s (codec=0x%x)\n", sz, path, codec);

    int W = argc > 3 ? atoi(argv[3]) : 640;
    int H = argc > 4 ? atoi(argv[4]) : 360;
    VideoStreamInfo si; memset(&si, 0, sizeof si);
    si.eCodecFormat = codec;
    si.bIsFramePackage = 0;            /* stream package: decoder finds NAL boundaries */
    si.nWidth = W; si.nHeight = H; si.nFrameRate = 30;

    VConfig vc; memset(&vc, 0, sizeof vc);
    vc.eOutputPixelFormat = PIXEL_FORMAT_NV12;
    vc.nFrameBufferNum = 8;
    vc.bCalledByOmxFlag = 1; vc.bIsSoftDecoderFlag = getenv("SOFT")?1:0;
    vc.memops = MemAdapterGetOpsS();
    vc.veOpsS = GetVeOpsS(0);
    vc.pVeOpsSelf = NULL;

    VideoDecoder *dec = CreateVideoDecoder();
    if (!dec) { printf("CreateVideoDecoder FAILED\n"); return 1; }
    int r = InitializeVideoDecoder(dec, &si, &vc);
    printf("InitializeVideoDecoder rc=%d\n", r);
    if (r != 0) { DestroyVideoDecoder(dec); return 1; }

    long off = 0; int frames = 0, saved = 0;
    while (off < sz) {
        int chunk = 65536; if (off + chunk > sz) chunk = sz - off;
        char *buf = NULL, *ring = NULL; int bufsz = 0, ringsz = 0;
        r = RequestVideoStreamBuffer(dec, chunk, &buf, &bufsz, &ring, &ringsz, 0);
        if (r != 0 || !buf) { printf("RequestVideoStreamBuffer rc=%d bufsz=%d\n", r, bufsz); break; }
        int n1 = chunk < bufsz ? chunk : bufsz;
        memcpy(buf, data + off, n1);
        if (chunk > n1 && ring) memcpy(ring, data + off + n1, chunk - n1);
        VideoStreamDataInfo di; memset(&di, 0, sizeof di);
        di.pData = buf; di.nLength = chunk; di.nPts = off;
        di.bIsFirstPart = 1; di.bIsLastPart = 1; di.bValid = 1;
        SubmitVideoStreamData(dec, &di, 0);
        off += chunk;
        r = DecodeVideoStream(dec, off >= sz ? 1 : 0, 0, 0, 0);
        VideoPicture *pic;
        while ((pic = RequestPicture(dec, 0)) != NULL) {
            frames++;
            if (!saved) {
                printf("FIRST FRAME: %dx%d stride=%d fmt=%d Y=%p UV=%p buffd=%d\n",
                       pic->nWidth, pic->nHeight, pic->nLineStride, pic->ePixelFormat,
                       (void*)pic->pData0, (void*)pic->pData1, pic->nBufFd);
                FILE *o = fopen("/tmp/dec_frame.nv12", "wb");
                if (o && pic->pData0) {
                    fwrite(pic->pData0, 1, (size_t)pic->nLineStride * pic->nHeight, o);
                    if (pic->pData1) fwrite(pic->pData1, 1, (size_t)pic->nLineStride * pic->nHeight / 2, o);
                    fclose(o);
                }
                saved = 1;
            }
            ReturnPicture(dec, pic);
        }
    }
    DecodeVideoStream(dec, 1, 0, 0, 0);
    VideoPicture *pic;
    while ((pic = RequestPicture(dec, 0)) != NULL) { frames++; ReturnPicture(dec, pic); }
    printf("TOTAL decoded frames=%d\n", frames);
    DestroyVideoDecoder(dec);
    return frames > 0 ? 0 : 2;
}
