/* Copyright (C) 2026-present PortareOS */
/*
 * hakuX HLE GPU — a fast ETC2 / EAC block encoder, for GPUs without BC
 * (Mali, PowerVR): a DXT texture's blocks are decoded and encoded again as
 * ETC2 of the same size, instead of A8R8G8B8 at four to eight times the
 * bytes (pb/pbres.c). ETC1's modes only (individual and differential base
 * colours, one modifier table per half): every block decodes on any ETC2
 * sampler. Pure: no state, any thread.
 *
 * A block is 16 texels, row-major (y * 4 + x), each 0xAARRGGBB.
 */
#ifndef HW_XBOX_HLE_TEX_ETC2_ENC_H
#define HW_XBOX_HLE_TEX_ETC2_ENC_H

#include <stdbool.h>
#include <stdint.h>

/* ETC2 RGB (VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK): 8 bytes; the alpha ignored */
void hle_etc2_rgb_block(const uint32_t px[16], uint8_t out[8]);
/* ETC2 RGB with punch-through alpha (VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK): 8
 * bytes; a texel with alpha < 128 is transparent (its colour 0 too) */
void hle_etc2_rgba1_block(const uint32_t px[16], uint8_t out[8]);
/* EAC alpha (the first 8 bytes of VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK) */
void hle_eac_alpha_block(const uint32_t px[16], uint8_t out[8]);

#endif
