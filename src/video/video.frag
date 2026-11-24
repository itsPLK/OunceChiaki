// Adapted from Moonlight-Switch (XITRIX, GPL-3.0).
#version 450
#ifdef TEXEL_BUFFER_SAMPLING
layout(set = 0, binding = 1) uniform textureBuffer lumaPlane;
layout(set = 0, binding = 2) uniform textureBuffer chromaPlane;
#else
layout(set = 0, binding = 0, std430) readonly buffer Pixels { uint words[]; } pixels;
#endif
#ifdef INTERPOLATE
layout(set = 0, binding = 4, std430) readonly buffer Previous { uint words[]; } previous;
#endif
layout(push_constant) uniform Params
{
    uvec4 plane;
    uvec4 extra;
    vec4 region;
    vec4 red;
    vec4 green;
    vec4 blue;
#ifdef INTERPOLATE
    uvec2 previousChroma;
#endif
}
p;
layout(location = 0) in vec2 position;
layout(location = 0) out vec4 color;
float byteAt(uint address, bool older)
{
#ifdef TEXEL_BUFFER_SAMPLING
    return texelFetch(lumaPlane, int(address)).r;
#else
#ifdef INTERPOLATE
    uint word = older ? previous.words[address >> 2] : pixels.words[address >> 2];
#else
    uint word = pixels.words[address >> 2];
#endif
    return float((word >> ((address & 3) * 8)) & 255) / 255.0;
#endif
}
vec3 sampleAt(ivec2 pos, bool chroma, bool older)
{
    ivec2 size = ivec2(p.extra.zw) / (chroma ? 2 : 1);
    pos        = clamp(pos, ivec2(0), size - 1);
    uvec4 planes = p.plane;
#ifdef INTERPOLATE
    if (older)
        planes = uvec4(p.extra.xy, p.previousChroma);
#endif
    if (!chroma)
        return vec3(byteAt(planes.x + uint(pos.y) * planes.y + uint(pos.x), older), 0, 0);
    uint address = planes.z + uint(pos.y) * planes.w;
    if (p.red.w < 0.5)
#ifdef TEXEL_BUFFER_SAMPLING
        return vec3(0, texelFetch(chromaPlane, int((address + uint(pos.x) * 2) >> 1)).rg);
#else
        return vec3(0, byteAt(address + uint(pos.x) * 2, older), byteAt(address + uint(pos.x) * 2 + 1, older));
#endif
    return vec3(0, byteAt(address + uint(pos.x), older), byteAt(p.extra.x + uint(pos.y) * p.extra.y + uint(pos.x), older));
}
vec3 filtered(vec2 uv, bool chroma, bool older)
{
    vec2 q     = uv * vec2(p.extra.zw) / (chroma ? 2.0 : 1.0) - 0.5;
    ivec2 base = ivec2(floor(q));
    vec2 f     = fract(q);
    return mix(mix(sampleAt(base, chroma, older), sampleAt(base + ivec2(1, 0), chroma, older), f.x),
        mix(sampleAt(base + ivec2(0, 1), chroma, older), sampleAt(base + ivec2(1, 1), chroma, older), f.x), f.y);
}
#if defined(PREDICT) || defined(INTERPOLATE)
layout(set = 0, binding = 3, std430) readonly buffer Flow { vec2 motion[]; };
vec2 tileFlow(ivec2 tile, ivec2 tiles)
{
    tile = clamp(tile, ivec2(0), tiles - 1);
    return motion[tile.y * tiles.x + tile.x];
}
vec2 halfMotion(vec2 uv)
{
    // Vectors are current-to-previous pixel offsets. Sample the current image
    // half an offset toward the previous position to project the current picture half a period forward.
    ivec2 tiles = (ivec2(p.extra.zw) + 15) / 16;
    vec2 grid = uv * vec2(p.extra.zw) / 16.0 - 0.5;
    ivec2 tile = ivec2(floor(grid));
    vec2 f = fract(grid);
    vec2 v = mix(mix(tileFlow(tile, tiles), tileFlow(tile + ivec2(1, 0), tiles), f.x),
                 mix(tileFlow(tile + ivec2(0, 1), tiles), tileFlow(tile + 1, tiles), f.x), f.y);
    return 0.5 * v / vec2(p.extra.zw);
}
#endif
// Luma-only FSR 1 EASU, adapted from FsrEasuF in AMD FidelityFX Super
// Resolution 1.0 (ffx_fsr1.h): the stream is NV12, so the edge-adaptive kernel
// runs on full-resolution luma and the half-resolution chroma keeps the
// bilinear filter.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Taps around the 2x2 f g j k:
//    b c
//  e f g h
//  i j k l
//    n o
float lumaAt(ivec2 pos, bool older)
{
    return sampleAt(pos, false, older).x;
}
void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    // lA above, lB left, lC centre, lD right, lE below.
    float dirX = lD - lB;
    float lenX = clamp(abs(dirX) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 32768.0), 0.0, 1.0);
    dir.x += dirX * w;
    len += lenX * lenX * w;
    float dirY = lE - lA;
    float lenY = clamp(abs(dirY) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 32768.0), 0.0, 1.0);
    dir.y += dirY * w;
    len += lenY * lenY * w;
}
void easuTap(inout float aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, float c)
{
    vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;
    float d2 = min(dot(v, v), clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}
float easuLuma(vec2 uv, bool older)
{
    vec2 q = uv * vec2(p.extra.zw) - 0.5;
    ivec2 o = ivec2(floor(q));
    vec2 pp = fract(q);
    float b = lumaAt(o + ivec2(0, -1), older), c = lumaAt(o + ivec2(1, -1), older);
    float e = lumaAt(o + ivec2(-1, 0), older), f = lumaAt(o, older);
    float g = lumaAt(o + ivec2(1, 0), older), h = lumaAt(o + ivec2(2, 0), older);
    float i = lumaAt(o + ivec2(-1, 1), older), j = lumaAt(o + ivec2(0, 1), older);
    float k = lumaAt(o + ivec2(1, 1), older), l = lumaAt(o + ivec2(2, 1), older);
    float n = lumaAt(o + ivec2(0, 2), older), oo = lumaAt(o + ivec2(1, 2), older);
    // Direction and edge length, bilinearly weighted over the four centre texels.
    vec2 dir  = vec2(0);
    float len = 0.0;
    easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), b, e, f, g, j);
    easuSet(dir, len, pp.x * (1.0 - pp.y), c, f, g, h, k);
    easuSet(dir, len, (1.0 - pp.x) * pp.y, f, i, j, k, n);
    easuSet(dir, len, pp.x * pp.y, g, j, k, l, oo);
    float dirR = dot(dir, dir);
    bool zero  = dirR < 1.0 / 32768.0;
    dir = zero ? vec2(1, 0) : dir * inversesqrt(dirR);
    len = 0.5 * len;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + (1.0 / 4.0 - 0.04 - 0.5) * len;
    float clp = 1.0 / lob;
    float aC = 0.0, aW = 0.0;
    easuTap(aC, aW, vec2(0, -1) - pp, dir, len2, lob, clp, b);
    easuTap(aC, aW, vec2(1, -1) - pp, dir, len2, lob, clp, c);
    easuTap(aC, aW, vec2(-1, 1) - pp, dir, len2, lob, clp, i);
    easuTap(aC, aW, vec2(0, 1) - pp, dir, len2, lob, clp, j);
    easuTap(aC, aW, vec2(0, 0) - pp, dir, len2, lob, clp, f);
    easuTap(aC, aW, vec2(-1, 0) - pp, dir, len2, lob, clp, e);
    easuTap(aC, aW, vec2(1, 1) - pp, dir, len2, lob, clp, k);
    easuTap(aC, aW, vec2(2, 1) - pp, dir, len2, lob, clp, l);
    easuTap(aC, aW, vec2(2, 0) - pp, dir, len2, lob, clp, h);
    easuTap(aC, aW, vec2(1, 0) - pp, dir, len2, lob, clp, g);
    easuTap(aC, aW, vec2(1, 2) - pp, dir, len2, lob, clp, oo);
    easuTap(aC, aW, vec2(0, 2) - pp, dir, len2, lob, clp, n);
    // Deringing: stay within the four nearest texels.
    float lo = min(min(f, g), min(j, k)), hi = max(max(f, g), max(j, k));
    float bilinear = mix(mix(f, g, pp.x), mix(j, k, pp.x), pp.y);
    return aW > 0.0 ? clamp(aC / aW, lo, hi) : bilinear;
}
vec3 sampleYuv(vec2 uv, bool older)
{
    // blue.w is 1 when the picture is upscaled with EASU.
    vec3 luma = p.blue.w > 0.5 ? vec3(easuLuma(uv, older), 0, 0) : filtered(uv, false, older);
    return luma + filtered(uv, true, older);
}
void main()
{
    vec2 uv = (position - p.region.xy) * p.region.zw;
    if (any(lessThan(uv, vec2(0))) || any(greaterThan(uv, vec2(1))))
    {
        color = vec4(0, 0, 0, 1);
        return;
    }
#if defined(INTERPOLATE)
    // Flow points from B back to A. At the midpoint, A and B lie
    // half a displacement on either side of the output pixel.
    vec2 halfStep = halfMotion(uv);
    vec3 a = sampleYuv(uv + halfStep, true);
    vec3 b = sampleYuv(uv - halfStep, false);
    vec3 error = abs(a - b);
    // Hold the earlier image on poor matches, such as a cut or an occlusion.
    vec3 yuv = max(error.x, max(error.y, error.z)) > 0.12
        ? sampleYuv(uv, true) : mix(a, b, 0.5);
#elif defined(PREDICT)
    vec3 yuv = sampleYuv(uv + halfMotion(uv), false);
#else
    vec3 yuv = sampleYuv(uv, false);
#endif
    yuv -= vec3(p.green.w, 128.0 / 255.0, 128.0 / 255.0);
    color    = vec4(clamp(vec3(dot(p.red.xyz, yuv), dot(p.green.xyz, yuv), dot(p.blue.xyz, yuv)), 0, 1), 1);
}
