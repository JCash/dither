#include <stdio.h>
#include <stdint.h>
#include <memory.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <math.h>

// https://en.wikipedia.org/wiki/Ordered_dithering
// https://bartwronski.com/2016/10/30/dithering-part-three-real-world-2d-quantization-dithering/
// https://blog.demofox.org/2017/10/31/animating-noise-for-integration-over-time/
// http://loopit.dk/banding_in_games.pdf + https://www.shadertoy.com/view/MslGR8
// https://ubm-twvideo01.s3.amazonaws.com/o1/vault/gdc2016/Presentations/Gjoel_Svendsen_Rendering_of_Inside.pdf

static void computeBayerThresholdMap(int n, float* M)
{
    if (n == 4)
    {
        const float m[] = {
             0,  8,  2, 10,
            12,  4, 14,  6,
             3, 11,  1,  9,
            15,  7, 13,  5
        };
        memcpy(M, m, sizeof(float) * n * n);
    }
    else if (n == 8)
    {
        const float m[] = {
             0,  32,  8, 40,  2, 34, 10, 42,
             48, 16, 56, 24, 50, 18, 58, 26,
             12, 44,  4, 36, 14, 46,  6, 38,
             60, 28, 52, 20, 62, 30, 54, 22,
              3, 35, 11, 43,  1, 33,  9, 41,
             51, 19, 59, 27, 49, 17, 57, 25,
             15, 47,  7, 39, 13, 45,  5, 37,
             63, 31, 55, 23, 61, 29, 53, 21
        };
        memcpy(M, m, sizeof(float) * n * n);
    }
    float div = 1.0f / (n*n);
    for (int i = 0; i < n*n; ++i)
    {
        M[i] = M[i] * div - 0.5f;
    }
}

static void printM(int n, const float* M)
{
    for (int y = 0; y < n; ++y)
    {
        for (int x = 0; x < n; ++x)
        {
            printf("%5.3f  ", M[y * n + x]);
        }
        printf("\n");
    }
    printf("\n");
}


static void RGB565ToRGBA8888(const uint16_t* data, const uint32_t width, const uint32_t height, uint8_t* color_rgba)
{
    for(uint32_t i = 0; i < width*height; ++i)
    {
        const uint16_t c = *(data++);
        uint32_t r5 = (c>>11) & 0x1f; // [0,31]
        uint32_t g6 = (c>>5) & 0x3f; // [0,63]
        uint32_t b5 = (c>>0) & 0x1f; // [0,31]
        // Map to range [0,255]
        *(color_rgba++) = (r5 * 255 + 15) / 31;
        *(color_rgba++) = (g6 * 255 + 31) / 63;
        *(color_rgba++) = (b5 * 255 + 15) / 31;
        *(color_rgba++) = 255;
    }
}

static void RGBA8888ToRGB565(const uint8_t* data, const uint32_t width, const uint32_t height, uint16_t* color_rgb)
{
    for(uint32_t i = 0; i < width*height; ++i)
    {
        uint8_t red = data[0];
        uint8_t green = data[1];
        uint8_t blue = data[2];
        uint16_t r = ((red >> 3) & 0x1f) << 11;
        uint16_t g = ((green >> 2) & 0x3f) << 5;
        uint16_t b = ((blue >> 3) & 0x1f);

        *(color_rgb++) = (r | g | b);
        data += 4;
    }
}

static void RGBA4444ToRGBA8888(const uint16_t* data, const uint32_t width, const uint32_t height, uint8_t* color_rgba)
{
    for(uint32_t i = 0; i < width*height; ++i)
    {
        const uint16_t c = *(data++);
        // Range [0,15]
        uint32_t r4 = (c>>12) & 0xf;
        uint32_t g4 = (c>>8) & 0xf;
        uint32_t b4 = (c>>4) & 0xf;
        uint32_t a4 = (c>>0) & 0xf;
        // Map to range [0,255]
        *(color_rgba++) = (r4 * 255 + 7) / 15;
        *(color_rgba++) = (g4 * 255 + 7) / 15;
        *(color_rgba++) = (b4 * 255 + 7) / 15;
        *(color_rgba++) = (a4 * 255 + 7) / 15;
    }
}

static void RGBA8888ToRGBA4444(const uint8_t* data, const uint32_t width, const uint32_t height, uint16_t* color_rgba)
{
    for(uint32_t i = 0; i < width*height; ++i)
    {
        uint16_t r = (data[0] >> 4) << 12;
        uint16_t g = (data[1] >> 4) << 8;
        uint16_t b = (data[2] >> 4) << 4;
        uint16_t a = (data[3] >> 4) << 0;
        *(color_rgba++) = (r | g | b | a);
        data += 4;
    }
}

static inline float clamp255(float v)
{
    if (v < 0)
        v = 0.0f;
    else if (v > 255)
        v = 255.0f;
    return v;
}

//https://en.wikipedia.org/wiki/Ordered_dithering
static float getM(int n, const float* M, uint32_t x, uint32_t y)
{
    x = x % n;
    y = y % n;
    return M[y * n + x];
}

static inline uint8_t ditherBayerValue(uint8_t uv, float m, float r)
{
    float v = ((float)uv) + r * m;
    v = clamp255(v);
    return (uint8_t)round(v);
}

static void ditherBayer(uint8_t* data, uint32_t width, uint32_t height, int n, float* M)
{
    const float r = 255.0f / n;

    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            float m = getM(n, M, x, y);
            data[0] = ditherBayerValue(data[0], m, r);
            data[1] = ditherBayerValue(data[1], m, r);
            data[2] = ditherBayerValue(data[2], m, r);
            data[3] = ditherBayerValue(data[3], m, r);
            data+=4;
        }
    }
}


// #define BITS_PER_PIXEL 4
// #define BPP_MUL (256 / ((1 << BITS_PER_PIXEL) - 1))
// #define BPP_BIAS (BPP_MUL / 2)
// #define BPP_CLIP (255 - BPP_BIAS)

// // https://github.com/rec0de/vsmp-zero/blob/e14c2cfc815bf38204d5e2d191f246e50e6f72e5/dither.c

// static unsigned char nearestPaletteColor(unsigned char pixel) {
//     if(pixel > BPP_CLIP)
//         return 255;
//     return ((pixel + BPP_BIAS) / BPP_MUL) * BPP_MUL;
// }


static inline float fract(float v)
{
    float f = v - (int32_t)v;
    return f;
}

//note: returns [-intensity;intensity[, magnitude of 2x intensity
//note: from "NEXT GENERATION POST PROCESSING IN CALL OF DUTY: ADVANCED WARFARE"
//      http://advances.realtimerendering.com/s2014/index.html
static inline float InterleavedGradientNoise(float u, float v)
{
    const float magic[3] = { 0.06711056f, 0.00583715f, 52.9829189f };
    return fract( magic[2] * fract( u * magic[0] + v * magic[1] ) );
}

static inline uint8_t addNoise(uint8_t v_in, int8_t noise)
{
    int16_t v = v_in + noise;
    if (v > 255)
        v = 255;
    else if(v < 0)
        v = 0;
    return v;
}


static void ditherInterleavedGradientRGBA4444(uint8_t* data, uint32_t width, uint32_t height)
{
    // Since we are going to convert this data to rgba4444 we the minimal value for a
    // color change is 2^8 / 2^4 = 16
    uint8_t bpp_mul = 16;
    uint8_t bpp_bias = bpp_mul / 2;

    float oowidth = 1.0f / width;
    float ooheight = 1.0f / height;
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            float rnd = InterleavedGradientNoise(x, y);
            data[0] = addNoise(data[0], rnd * bpp_mul - bpp_bias);
            data[1] = addNoise(data[1], (1.0f - rnd) * bpp_mul - bpp_bias); // As seen in the shadertoy by Mikkel Gjoel
            //data[1] = addNoise(data[1], rnd * bpp_mul - bpp_bias);
            data[2] = addNoise(data[2], rnd * bpp_mul - bpp_bias);
            data[3] = addNoise(data[3], rnd * bpp_mul - bpp_bias);
            data+=4;
        }
    }
}

static void ditherInterleavedGradientRGBx565(uint8_t* data, uint32_t width, uint32_t height)
{
    // Since we are going to convert this data to rgba4444 we the minimal value for a color change is
    uint8_t bpp_mul_5 = 8; // (1<<8)/(1<<5)
    uint8_t bpp_bias_5 = bpp_mul_5 / 2;
    uint8_t bpp_mul_6 = 4; // (1<<8)/(1<<6)
    uint8_t bpp_bias_6 = bpp_mul_6 / 2;

    float oowidth = 1.0f / width;
    float ooheight = 1.0f / height;
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            float rnd = InterleavedGradientNoise(x, y);
            data[0] = addNoise(data[0], rnd * bpp_mul_5 - bpp_bias_5);
            data[1] = addNoise(data[1], (1.0f - rnd) * bpp_mul_6 - bpp_bias_6); // As seen in the shadertoy by Mikkel Gjoel
            //data[1] = addNoise(data[1], rnd * bpp_mul - bpp_bias);
            data[2] = addNoise(data[2], rnd * bpp_mul_5 - bpp_bias_5);
            // the
            data+=4;
        }
    }
}


int main(int argc, char const *argv[])
{
    const char* path = 0;
    if (argc > 1)
    {
        path = argv[1];
    }

    if (!path) {
        fprintf(stderr, "You must supply an image path");
        return 1;
    }

    int width, height, numchannels;
    uint8_t* image_input = stbi_load(path, &width, &height, &numchannels, 0);
    if (!image_input) {
        fprintf(stderr, "Failed to load '%s'", path);
        return 1;
    }

    // int N = 8;
    // float* M = (float*)malloc(sizeof(float) * N * N);

    // computeBayerThresholdMap(N, M);

    // printf("M:\n");
    // printM(N, M);

    //ditherBayer(image_input, width, height, N, M);

    uint8_t* image_output_16bit = (uint8_t*)malloc(width*height*2);
    uint8_t* image_output_32bit = (uint8_t*)malloc(width*height*4);

    if (numchannels == 4)
    {
        ditherInterleavedGradientRGBA4444(image_input, width, height);

        RGBA8888ToRGBA4444(image_input, width, height, (uint16_t*)image_output_16bit);

        RGBA4444ToRGBA8888((uint16_t*)image_output_16bit, width, height, image_output_32bit);
    }
    else if (numchannels == 3)
    {
        // make it into rgba8888 since that's what out functions operate on
        uint8_t* image_input8888 = (uint8_t*)malloc(width*height*4);
        for (int i = 0; i < width*height; ++i)
        {
            image_input8888[i*4+0] = image_input[i*3 + 0];
            image_input8888[i*4+1] = image_input[i*3 + 1];
            image_input8888[i*4+2] = image_input[i*3 + 2];
            image_input8888[i*4+3] = 255;
        }
        free(image_input);
        image_input = image_input8888;

        ditherInterleavedGradientRGBx565(image_input, width, height);

        RGBA8888ToRGB565(image_input, width, height, (uint16_t*)image_output_16bit);

        RGB565ToRGBA8888((uint16_t*)image_output_16bit, width, height, image_output_32bit);
    }

    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s.dither.png", path);
    stbi_write_png(buffer, width, height, 4, image_output_32bit, width*4);
    printf("Wrote '%s'\n", buffer);

    //free(M);
    free(image_input);
    free(image_output_16bit);
    free(image_output_32bit);
    return 0;
}