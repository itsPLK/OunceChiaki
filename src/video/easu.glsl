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
// The including shader defines float lumaAt(ivec2) (clamped to the picture).
// q is the output pixel's position in source texels, minus 0.5.
// Taps around the 2x2 f g j k:
//    b c
//  e f g h
//  i j k l
//    n o
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
float easuLuma(vec2 q)
{
    ivec2 o = ivec2(floor(q));
    vec2 pp = fract(q);
    float b = lumaAt(o + ivec2(0, -1)), c = lumaAt(o + ivec2(1, -1));
    float e = lumaAt(o + ivec2(-1, 0)), f = lumaAt(o);
    float g = lumaAt(o + ivec2(1, 0)), h = lumaAt(o + ivec2(2, 0));
    float i = lumaAt(o + ivec2(-1, 1)), j = lumaAt(o + ivec2(0, 1));
    float k = lumaAt(o + ivec2(1, 1)), l = lumaAt(o + ivec2(2, 1));
    float n = lumaAt(o + ivec2(0, 2)), oo = lumaAt(o + ivec2(1, 2));
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
