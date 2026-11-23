#pragma once

/* Encoder IDs. One public ID per codec: the encoder picks texture or system
 * memory input itself and reroutes to the internal ".raw" ID when it can't
 * use textures. */
#define NVENC_ID_H264 "com.xaymar.knurbs.nvenc.h264"
#define NVENC_ID_HEVC "com.xaymar.knurbs.nvenc.hevc"
#define NVENC_ID_AV1 "com.xaymar.knurbs.nvenc.av1"
#define NVENC_ID_H264_RAW NVENC_ID_H264 ".raw"
#define NVENC_ID_HEVC_RAW NVENC_ID_HEVC ".raw"
#define NVENC_ID_AV1_RAW NVENC_ID_AV1 ".raw"
