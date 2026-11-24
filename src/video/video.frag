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
    uvec2 previousChroma;
    uvec2 upscaledSize;
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
// Luma upscaled once per picture by easu.comp (FSR 1 EASU), packed four
// 8-bit samples to a word, upscaledSize.x by upscaledSize.y.
layout(set = 0, binding = 5, std430) readonly buffer Upscaled { uint words[]; } upscaled;
#ifdef INTERPOLATE
layout(set = 0, binding = 6, std430) readonly buffer UpscaledPrevious { uint words[]; } upscaledPrevious;
#endif
float upscaledAt(ivec2 pos, bool older)
{
    pos = clamp(pos, ivec2(0), ivec2(p.upscaledSize) - 1);
    uint index = uint(pos.y) * ((p.upscaledSize.x + 3u) / 4u) + uint(pos.x >> 2);
#ifdef INTERPOLATE
    uint word = older ? upscaledPrevious.words[index] : upscaled.words[index];
#else
    uint word = upscaled.words[index];
#endif
    return float((word >> ((uint(pos.x) & 3u) * 8u)) & 255u) / 255.0;
}
float upscaledLuma(vec2 uv, bool older)
{
    // At the picture's own pixel centres this is a single sample; generated
    // frames sample between them along the motion.
    vec2 q     = uv * vec2(p.upscaledSize) - 0.5;
    ivec2 base = ivec2(floor(q));
    vec2 f     = fract(q);
    return mix(mix(upscaledAt(base, older), upscaledAt(base + ivec2(1, 0), older), f.x),
        mix(upscaledAt(base + ivec2(0, 1), older), upscaledAt(base + ivec2(1, 1), older), f.x), f.y);
}
vec3 sampleYuv(vec2 uv, bool older)
{
    // blue.w is 1 when the upscaled luma is bound.
    vec3 luma = p.blue.w > 0.5 ? vec3(upscaledLuma(uv, older), 0, 0) : filtered(uv, false, older);
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
