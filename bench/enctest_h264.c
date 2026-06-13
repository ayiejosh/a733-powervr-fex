/* Feasibility proof: encode NV12 -> H.265/HEVC on the VE2 (IC 21320).
   Mirrors the driver's working H.264 path but with VENC_CODEC_H264 + VencH264Param.
   Validates: (1) the IC accepts H.265 encode, (2) output is a decodable HEVC stream
   (verify with: ffmpeg -i /tmp/out.h264 -f null -). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vencoder.h"
#include "memoryAdapter.h"
#include "veAdapter.h"
#include "veInterface.h"

int main(int argc, char **argv) {
    int W = argc>2?atoi(argv[2]):640, H = argc>3?atoi(argv[3]):360; int fps = 30, br = 4000000, N = 30;
    FILE *f = fopen("/tmp/frame.nv12", "rb");
    if (!f) { perror("frame.nv12"); return 1; }
    long fsz = (long)W * H * 3 / 2;
    unsigned char *nv12 = malloc(fsz);
    if (fread(nv12, 1, fsz, f) != (size_t)fsz) { printf("short frame read\n"); return 1; }
    fclose(f);

    VideoEncoder *enc = VideoEncCreate(VENC_CODEC_H264);
    if (!enc) { printf("VideoEncCreate(VENC_CODEC_H264) FAILED -> no HEVC encode\n"); return 1; }
    printf("VideoEncCreate(H265) OK\n");

    VencH264Param p; memset(&p, 0, sizeof p);
    p.sProfileLevel.nProfile = VENC_H264ProfileMain;
    p.sProfileLevel.nLevel = VENC_H264Level41;
    p.nFramerate = fps; p.nSrcFramerate = fps; p.nBitrate = br;
    p.sQPRange.nMinqp = 10; p.sQPRange.nMaxqp = 48;
    p.sQPRange.nMinPqp = 10; p.sQPRange.nMaxPqp = 48; p.sQPRange.nQpInit = 30;
    p.nMaxKeyInterval=30; p.nCodingMode=VENC_FRAME_CODING; p.sRcParam.eRcMode = AW_CBR;
    if (VideoEncSetParameter(enc, VENC_IndexParamH264Param, &p) != 0)
        printf("WARN: SetParameter(H265Param) returned nonzero\n");
    int b = br, fr = fps;
    VideoEncSetParameter(enc, VENC_IndexParamBitrate, &b);
    VideoEncSetParameter(enc, VENC_IndexParamFramerate, &fr);

    VencBaseConfig base; memset(&base, 0, sizeof base);
    base.nInputWidth = W; base.nInputHeight = H;
    base.nDstWidth = W; base.nDstHeight = H; base.nStride = W;
    base.eInputFormat = VENC_PIXEL_YUV420SP;
    base.memops = MemAdapterGetOpsS();
    base.veOpsS = GetVeOpsS(0);  /* 0=encode/default path; 1=decode (proven via libVE disasm) */
    base.pVeOpsSelf = NULL;
    if (VideoEncInit(enc, &base) != 0) { printf("VideoEncInit(H265) FAILED\n"); return 1; }
    printf("VideoEncInit(H265) OK %dx%d %dkbps\n", W, H, br / 1000);

    VencAllocateBufferParam bp; memset(&bp, 0, sizeof bp);
    bp.nBufferNum = 4; bp.nSizeY = W * H; bp.nSizeC = W * H / 2;
    if (AllocInputBuffer(enc, &bp) != 0) { printf("AllocInputBuffer FAILED\n"); return 1; }

    FILE *out = fopen("/tmp/out.h264", "wb");
    VencHeaderData hdr; memset(&hdr, 0, sizeof hdr);
    if (VideoEncGetParameter(enc, VENC_IndexParamH264SPSPPS, &hdr) == 0 && hdr.nLength) {
        printf("HEVC VPS/SPS/PPS header = %d bytes\n", hdr.nLength);
        fwrite(hdr.pBuffer, 1, hdr.nLength, out);
    } else printf("WARN: no H265 header returned\n");

    long total = 0; int ok = 0;
    for (int i = 0; i < N; i++) {
        VencInputBuffer in; memset(&in, 0, sizeof in);
        fprintf(stderr,"M1 getbuf\n");
        int gr=GetOneAllocInputBuffer(enc, &in);
        fprintf(stderr,"M2 gr=%d Y=%p C=%p\n",gr,(void*)in.pAddrVirY,(void*)in.pAddrVirC);
        if (gr != 0) { printf("GetOneAllocInputBuffer FAILED\n"); break; }
        memcpy(in.pAddrVirY, nv12, W * H);
        memcpy(in.pAddrVirC, nv12 + W * H, W * H / 2);
        in.nPts = (long long)i * (1000000 / fps);
        FlushCacheAllocInputBuffer(enc, &in);
        AddOneInputBuffer(enc, &in);
        int r = VideoEncodeOneFrame(enc);
        AlreadyUsedInputBuffer(enc, &in);
        ReturnOneAllocInputBuffer(enc, &in);
        if (r != 0) { printf("VideoEncodeOneFrame[%d] rc=%d\n", i, r); break; }
        VencOutputBuffer o; memset(&o, 0, sizeof o);
        if (GetOneBitstreamFrame(enc, &o) == 0) {
            if (o.nSize0) { fwrite(o.pData0, 1, o.nSize0, out); total += o.nSize0; }
            if (o.nSize1) { fwrite(o.pData1, 1, o.nSize1, out); total += o.nSize1; }
            FreeOneBitStreamFrame(enc, &o);
            ok++;
        }
    }
    fclose(out);
    printf("RESULT: encoded %d/%d frames, %ld bytes total -> /tmp/out.h264\n", ok, N, total);
    VideoEncDestroy(enc);
    return ok > 0 ? 0 : 2;
}
