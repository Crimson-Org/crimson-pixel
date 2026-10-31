#pragma once

/* The ICC profile layout, shared by reading and writing profiles. Numbers are big endian and signatures are four
 * characters. A 128 byte header, whose input and output colour spaces sit at 16 and 20, is followed by a tag count
 * and twelve byte tag entries of signature, offset and size. Every tag starts with its type and four unused bytes. */
#define ICC_HEADER_SIZE 128
#define ICC_SPACE_IN_AT 16
#define ICC_SPACE_OUT_AT 20
#define ICC_NUMBER_BYTES 4
#define ICC_SIGNATURE_BYTES 4
#define ICC_TAG_ENTRY_SIZE 12
#define TAG_ENTRY_OFFSET 4
#define TAG_ENTRY_SIZE 8
#define ICC_TAG_HEAD 8
/* Tags are padded to four bytes */
#define ICC_ALIGNMENT 4
/* Fixed point numbers have sixteen bits after the point, or eight in a gamma */
#define ICC_FIXED_ONE 65536.0
#define ICC_GAMMA_FIXED_ONE 256.0

/* Lookups work on three channels in and out */
#define LUT_CHANNELS 3
/* An mAB tag gives its channels in and out, then the offsets of its matrix, its M curves and its colour table */
#define MAB_INPUT_CHANNELS 8
#define MAB_OUTPUT_CHANNELS 9
#define MAB_MATRIX_OFFSET 16
#define MAB_M_CURVES_OFFSET 20
#define MAB_CLUT_OFFSET 24
#define MAB_HEADER_SIZE 32
/* A matrix is three by three with an offset for each row */
#define ICC_MATRIX_VALUES 12
/* A colour table gives the grid size of each channel in sixteen places, then how many bytes each value takes */
#define CLUT_PRECISION 16
#define CLUT_HEADER_SIZE 20
#define LUT_VALUE_BYTES 2
