#pragma once

/* Encoder IDs. One public ID per codec: the encoder picks texture or system
 * memory input itself and reroutes to the internal ".raw" ID when it can't
 * use textures. */
#define QSV_ID_H264 "com.xaymar.knurbs.qsv.h264"
#define QSV_ID_HEVC "com.xaymar.knurbs.qsv.hevc"
#define QSV_ID_AV1 "com.xaymar.knurbs.qsv.av1"
#define QSV_ID_H264_RAW QSV_ID_H264 ".raw"
#define QSV_ID_HEVC_RAW QSV_ID_HEVC ".raw"
#define QSV_ID_AV1_RAW QSV_ID_AV1 ".raw"
