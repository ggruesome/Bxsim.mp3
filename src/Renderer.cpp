#include "Renderer.hpp"
#include "Platform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace
{
constexpr float Pi = 3.14159265358979f;

// Gaussian blur, 9 taps folded into 5 with linear filtering.
constexpr const char* BlurShader = R"(
uniform sampler2D source;
uniform vec2 direction;
void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec4 c = texture2D(source, uv) * 0.2270270270;
    c += texture2D(source, uv + direction * 1.3846153846) * 0.3162162162;
    c += texture2D(source, uv - direction * 1.3846153846) * 0.3162162162;
    c += texture2D(source, uv + direction * 3.2307692308) * 0.0702702703;
    c += texture2D(source, uv - direction * 3.2307692308) * 0.0702702703;
    gl_FragColor = c * gl_Color;
}
)";

// Shared GLSL helpers for the trippy modes.
constexpr const char* ShaderCommon = R"(
// Gradient noise with quintic fade: smooth everywhere, no visible grid.
// "Hash without sine": cheap, and identical on every graphics card.
vec2 hash2(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return -1.0 + 2.0 * fract((p3.xx + p3.yz) * p3.zy);
}
float noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = dot(hash2(i), f);
    float b = dot(hash2(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float c = dot(hash2(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float d = dot(hash2(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y); // about -0.7..0.7
}
float fbm(vec2 p)
{
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i)
    {
        v += a * noise(p);
        p = mat2(1.6, 1.2, -1.2, 1.6) * p + vec2(3.1, 1.7);
        a *= 0.5;
    }
    return v;
}
float fbm3(vec2 p)
{
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 3; ++i)
    {
        v += a * noise(p);
        p = mat2(1.6, 1.2, -1.2, 1.6) * p + vec2(3.1, 1.7);
        a *= 0.5;
    }
    return v;
}
vec3 hueRotate(vec3 c, float a)
{
    vec3 k = vec3(0.57735);
    float ca = cos(a);
    return c * ca + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - ca);
}
)";

// Liquid: a glossy, swirling fluid folded into a kaleidoscope.
// The surface is a height field: sine-turbulence swirls it, noise breaks it up,
// and its slope gives lighting so it reads as wet, shiny liquid.
constexpr const char* LiquidShader = R"(
uniform vec2 resolution;
uniform float flow;
uniform float time;
uniform float bass;
uniform float beat;
uniform float energy;
uniform float treble;
uniform float hueShift;
uniform sampler2D image;
uniform float hasImage;
uniform vec2 fitScale;     // screen (height units) -> image uv, whole picture visible
uniform float amount;      // 0 right after the picture opens, ramps to 1
uniform sampler2D audio;   // row 0 spectrum, row 1 waveform
uniform sampler2D previous; // last frame, for echo trails
uniform float echo;
uniform float warpAmount;  // settings: turbulence
uniform float zoom;        // settings: zoom
uniform float gloss;       // settings: gloss
uniform float kaleido;     // settings: kaleidoscope on/off

mat2 rot2(float a) { float c = cos(a), s = sin(a); return mat2(c, s, -s, c); }

vec3 imageAt(vec2 q)
{
    vec2 st = 0.5 + vec2(q.x, -q.y) * fitScale;
    float inside = step(0.0, st.x) * step(st.x, 1.0) * step(0.0, st.y) * step(st.y, 1.0);
    return texture2D(image, 1.0 - abs(1.0 - mod(st, 2.0))).rgb * mix(0.35, 1.0, max(inside, amount));
}

vec2 softAbs(vec2 v) { return sqrt(v * v + 0.0016); } // mirror fold without a hard crease

float surface(vec2 p, out vec2 warp)
{
    float t = flow * 0.18;
    // Layered sine turbulence: the classic smooth "swirling liquid" motion.
    for (int i = 1; i < 7; ++i)
    {
        float fi = float(i);
        p += vec2(0.65 / fi * sin(fi * p.y + t + 0.3 * fi),
                  0.55 / fi * cos(fi * p.x - t * 0.8 + 0.5 * fi)) * (0.85 + 0.35 * bass) * warpAmount;
    }
    // Slow noise so it never looks like a repeating pattern.
    vec2 n = vec2(fbm3(p * 0.6 + vec2(0.0, t * 0.3)), fbm3(p * 0.6 + vec2(5.2, -t * 0.25)));
    warp = n;
    p += 1.4 * n;
    return 0.5 + 0.85 * fbm(p * 0.9 + n * 1.5) + 0.12 * sin(p.x * 1.3 + p.y * 0.7);
}

void main()
{
    float px = 1.0 / resolution.y;
    vec2 screenUv = (gl_FragCoord.xy - 0.5 * resolution) * px;
    vec2 uv = mix(screenUv, softAbs(screenUv), kaleido);                 // 4-way kaleidoscope
    uv *= 1.0 - 0.06 * beat;          // breathe on the kick
    vec2 p = uv * 3.2 / zoom;

    vec2 warp;
    vec2 dummy;
    float h  = surface(p, warp);
    float e  = 2.0 * px * 3.2 / zoom;
    float hx = surface(p + vec2(e, 0.0), dummy);
    float hy = surface(p + vec2(0.0, e), dummy);
    vec2 grad = vec2(hx - h, hy - h) / e;

    vec3 deep   = vec3(0.00, 0.00, 0.035);
    vec3 indigo = vec3(0.13, 0.06, 0.60);
    vec3 violet = vec3(0.30, 0.22, 1.00);
    vec3 green  = vec3(0.08, 0.78, 0.48);

    vec3 col = mix(deep, indigo, smoothstep(0.2, 0.6, h));
    col = mix(col, violet, smoothstep(0.55, 0.85, h));

    // Green ink running through the folds; the highs feed it.
    float g = fbm3(p * 1.1 + warp * 3.0 + vec2(flow * 0.05, 0.0));
    float ink = smoothstep(0.04, 0.24, g) * smoothstep(0.78, 0.45, h);
    col = mix(col, green, clamp(ink * (0.9 + treble), 0.0, 1.0) * 0.85);

    // Lighting from the slope: soft diffuse plus a glossy highlight.
    vec3 n = normalize(vec3(-grad * 0.12, 1.0));
    vec3 light = normalize(vec3(-0.4, 0.5, 0.75));
    float diffuse = 0.6 + 0.4 * dot(n, light);
    float spec = pow(max(dot(reflect(-light, n), vec3(0.0, 0.0, 1.0)), 0.0), 20.0) * gloss;
    col *= diffuse;
    col += spec * vec3(0.5, 0.6, 1.0) * (0.45 + 0.6 * energy) * smoothstep(0.25, 0.75, h);

    // Faint ripple contours, anti-aliased by how fast the surface changes per pixel.
    float rings = h * 10.0 - flow * 0.25;
    float lineDist = abs(fract(rings) - 0.5) / max(length(grad) * 10.0 * px * 3.2, 1e-4);
    col += (1.0 - smoothstep(0.0, 1.5, lineDist)) * 0.05 * vec3(0.5, 0.6, 1.0) * smoothstep(0.35, 0.7, h);

    col *= smoothstep(0.1, 0.38, h + 0.25 * warp.x);   // dark pools
    col *= 0.85 + 0.3 * energy + 0.3 * beat;
    col = hueRotate(col, hueShift);

    // With a picture loaded, the picture itself becomes the liquid: it keeps its own colors but
    // gets lensed, twisted, folded into shifting symmetries, rippled by each frequency band,
    // split into RGB on kicks, lit like a wet surface and smeared into glowing echo trails.
    if (hasImage > 0.5)
    {
        float a = amount;
        vec2 q = screenUv;

        // Lens: bulges toward you with the bass and on every kick.
        q *= 1.0 - a * (0.3 * bass + 0.45 * beat) * (0.35 - dot(q, q));

        // Twist: a whirlpool around the center that the bass winds up.
        float r = length(q);
        q = rot2(a * (0.8 * sin(time * 0.21) + 2.6 * bass + 1.2 * beat) * exp(-r * 2.4)) * q;

        // Symmetry: every 9 seconds it folds into a different number of mirrored wedges.
        float phase = time / 9.0;
        float which = mod(floor(phase), 5.0);
        float n = which < 0.5 ? 1.0 : (which < 1.5 ? 2.0 : (which < 2.5 ? 3.0 : (which < 3.5 ? 4.0 : 6.0)));
        float within = fract(phase);
        float symmetry = a * kaleido * step(1.5, n) * smoothstep(0.0, 0.12, within) * smoothstep(1.0, 0.88, within);
        r = length(q);
        float seg = 3.14159265 / n;
        float folded = abs(mod(atan(q.y, q.x) + time * 0.05, 2.0 * seg) - seg);
        q = mix(q, r * vec2(cos(folded), sin(folded)), symmetry);

        // Liquid flow, plus ripples rolling outward where each frequency band lives.
        r = max(length(q), 1e-3);
        float band = texture2D(audio, vec2(clamp(r * 1.1, 0.0, 1.0), 0.25)).r;
        q += warp * (0.05 + 0.28 * energy + 0.1 * bass) * a;
        q += (q / r) * sin(r * 38.0 - flow * 4.0) * band * 0.025 * a;

        // RGB split along the radius.
        float split = (0.003 + 0.025 * beat + 0.01 * treble) * a;
        vec3 pic = vec3(imageAt(q * (1.0 + split)).r, imageAt(q).g, imageAt(q * (1.0 - split)).b);

        // Wet-surface lighting and a punchier, music-driven color.
        pic *= mix(1.0, 0.55 + 0.6 * diffuse, a);
        pic += spec * vec3(0.9, 0.92, 1.0) * (0.12 + 0.4 * energy) * a;
        float lum = dot(pic, vec3(0.299, 0.587, 0.114));
        pic = mix(vec3(lum), pic, 1.0 + 0.45 * a);
        pic = hueRotate(pic, a * 0.35 * sin(time * 0.11 + bass));
        pic *= 0.9 + 0.25 * energy + 0.35 * beat * a;

        // Echo: the previous frame, pulled slightly toward the center and turned, glows behind.
        vec2 sv = gl_FragCoord.xy / resolution - 0.5;
        vec2 pv = rot2(0.012 + 0.04 * beat) * sv * (0.975 - 0.03 * bass) + 0.5;
        vec3 prev = texture2D(previous, pv).rgb * 0.9;
        col = mix(pic, max(pic, prev), echo);
    }
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

// Dark Fantasy: a moonlit gothic castle. Tower heights follow the spectrum, windows flicker
// with the music, clouds drift past the moon, fog rolls along the cliffs. Lightning and bats
// are drawn on top on the CPU side. With a picture loaded, the picture becomes the scene.
constexpr const char* FantasyCommon = R"(
uniform vec2 resolution;
uniform float time;
uniform float bass;
uniform float beat;
uniform float energy;
uniform float treble;
uniform float flash;
uniform sampler2D audio;
uniform sampler2D image;
uniform float hasImage;
uniform vec2 fitScale;
uniform float moonSize;
uniform float castleHeight;
uniform float towerCount;
uniform float fogAmount;
uniform float cloudSpeed;
uniform float camZ;      // how far the camera has flown down the street
uniform float windowGain;
uniform float sway;

float h11(float n) { return fract(sin(n * 127.1) * 43758.5453); }
float h21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float band(float x) { return texture2D(audio, vec2(clamp(x, 0.0, 1.0), 0.25)).r; }

// Each star twinkles at its own speed and phase, and now and then flares up at its own random
// moment. Louder highs make more stars flare, but never all at once.
float starLight(vec2 cell, float t, float music)
{
    float h1 = h21(cell + 17.3);
    float h2 = h21(cell + 41.7);
    float h3 = h21(cell + 5.1);
    float steady = 0.2 + 0.3 * h3;
    float tw = 0.5 + 0.5 * sin(t * (0.6 + 2.6 * h1) + h2 * 6.2831);
    float period = 2.5 + 8.0 * h1;
    float x = (t + h2 * period) / period;
    float fires = step(h21(cell + floor(x) * 1.37 + 3.3), 0.15 + 0.5 * music);
    float local = (fract(x) - 0.5) * 9.0;
    return steady * (0.35 + 0.65 * tw) + fires * exp(-local * local) * 1.4;
}

// Jagged mountain ridges.
float ridge(float x, float seed)
{
    float v = 0.0;
    float a = 0.5;
    float f = 1.0;
    for (int i = 0; i < 4; ++i)
    {
        v += a * (1.0 - abs(noise(vec2(x * f, seed))) * 1.4);
        f *= 2.1;
        a *= 0.5;
    }
    return v;
}

// One row of towers. Returns 1 where a tower (or its spire) covers the pixel; collects moonlit
// edges in `rim` and lit windows in `glow`.
float towers(vec2 p, float y, float n, float span, float baseY, float hScale, float seed, float windows,
             float px, vec2 moonPos, inout float rim, inout vec3 glow)
{
    float hit = 0.0;
    for (int i = 0; i < 32; ++i)
    {
        if (float(i) >= n)
            break;
        float fi = float(i) + seed * 31.0;
        float t = (float(i) + 0.5) / n;
        float cx = (t - 0.5) * span + (h11(fi + 11.0) - 0.5) * span / n * 0.7;
        float w = span / n * (0.35 + 0.6 * h11(fi + 3.0));
        float center = exp(-(cx * 2.6) * (cx * 2.6));      // the cathedral in the middle stands tallest
        float height = (0.05 + 0.09 * h11(fi * 1.7) + 0.18 * center + 0.12 * band(t)) * hScale;
        float top = baseY + height;
        float dx = abs(p.x - cx);
        float capW = w * 0.62;                               // spire overhangs the tower a little
        float spire = w * (1.4 + 1.4 * center + 0.8 * h11(fi + 9.0));
        float slope = capW * (1.0 - (y - top) / spire);
        bool body = dx < w * 0.5 && y < top;
        bool roof = y >= top && dx < slope;
        if (!(body || roof))
            continue;
        hit = 1.0;
        // Moonlight catching the edge that faces the moon.
        if (sign(p.x - cx) == sign(moonPos.x - cx))
        {
            float edge = roof ? slope - dx : w * 0.5 - dx;
            float upper = roof ? 1.0 : smoothstep(top - 0.12, top, y);
            rim = max(rim, smoothstep(px * 2.5, 0.0, edge) * 0.35 * upper);
        }
        // Windows: thin arched slits that flicker with the music.
        if (body && windows > 0.0 && y < top - 0.012 && y > baseY + 0.008)
        {
            float rowH = 0.03;
            float row = floor((y - baseY) / rowH);
            float inRow = fract((y - baseY) / rowH);
            float cols = w > 0.045 ? 2.0 : 1.0;
            float cellX = (p.x - (cx - w * 0.5)) / w * cols;
            float key = fi * 13.0 + row * 7.0 + floor(cellX);
            float on = step(0.78 - 0.3 * energy - 0.2 * band(t), h11(key));
            float flick = 0.7 + 0.3 * sin(time * (5.0 + 9.0 * h11(key + 1.0)) + key);
            float arch = step(abs(fract(cellX) - 0.5), 0.13) * step(0.2, inRow) * step(inRow, 0.72);
            vec3 warm = mix(vec3(1.0, 0.55, 0.2), vec3(1.0, 0.32, 0.5), step(0.82, h11(key + 5.0)));
            glow += warm * arch * on * flick * (0.55 + 0.9 * band(t)) * windows;
        }
    }
    return hit;
}

)";

constexpr const char* FantasyShader = R"(
void main()
{
    vec2 uv = gl_FragCoord.xy / resolution;               // 0..1, y up
    float aspect = resolution.x / resolution.y;
    vec2 p = vec2((uv.x - 0.5) * aspect, uv.y);           // x centered, height units
    float px = 1.0 / resolution.y;
    vec2 moonPos = vec2(aspect * 0.24, 0.77);
    vec3 col;

    if (hasImage > 0.5)
    {
        // The picture as the scene: moonlit blue grade, breathing with the bass, lit by lightning.
        vec2 q = (p - vec2(0.0, 0.5)) * (1.0 - 0.03 * bass - 0.02 * beat);
        vec2 st = vec2(0.5) + vec2(q.x, -q.y) * fitScale;
        st = 1.0 - abs(1.0 - mod(st, 2.0));
        vec3 pic = texture2D(image, st).rgb;
        float l = dot(pic, vec3(0.299, 0.587, 0.114));
        col = mix(pic, vec3(0.25, 0.42, 1.0) * l * 1.3, 0.35) * (0.75 + 0.25 * energy);
        col += vec3(0.45, 0.55, 1.0) * flash * (0.25 + 0.6 * l);
        // Bright spots (windows, lamps) flicker with the music.
        col += pic * smoothstep(0.55, 0.9, l) * vec3(1.0, 0.6, 0.3) * (0.4 * beat + 0.2 * treble);
    }
    else
    {
        // ---- Sky ----
        col = mix(vec3(0.045, 0.07, 0.18), vec3(0.008, 0.012, 0.04), smoothstep(0.3, 1.0, uv.y));
        vec2 starCell = floor(gl_FragCoord.xy / 2.0);
        if (h21(starCell) > 0.9965)
            col += vec3(0.7, 0.78, 1.0) * starLight(starCell, time, treble) * smoothstep(0.45, 0.9, uv.y);

        // ---- Moon with a cold glow ----
        float radius = 0.075 * moonSize * (1.0 + 0.06 * bass);
        float d = length(p - moonPos);
        float crater = fbm(p * 16.0 + 3.0);
        col += vec3(0.2, 0.32, 0.8) * exp(-d * 5.5 / moonSize) * (0.45 + 0.45 * bass);
        vec3 moon = vec3(0.72, 0.8, 0.95) * (0.82 + 0.35 * crater);
        col = mix(col, moon, smoothstep(radius + px * 1.5, radius - px * 1.5, d));

        // ---- Storm clouds, dark, with moonlit and lightning-lit edges ----
        float drift = time * 0.025 * cloudSpeed;
        float cloud = fbm(vec2(p.x * 1.4 + drift, p.y * 3.2 - drift * 0.3)) + 0.5 * fbm(vec2(p.x * 3.1 - drift * 1.7, p.y * 5.0));
        float density = smoothstep(0.02, 0.38, cloud) * smoothstep(0.38, 0.62, uv.y);
        float lit = exp(-length(p - moonPos) * 2.6);
        float edgeLight = smoothstep(0.38, 0.1, cloud);      // thin parts glow
        vec3 cloudCol = mix(vec3(0.02, 0.03, 0.08), vec3(0.3, 0.42, 0.85), clamp(lit * (0.5 + edgeLight) + flash * 0.9, 0.0, 1.0));
        col = mix(col, cloudCol, density * 0.9);

        // ---- Jagged mountains: far and hazy, then nearer and darker ----
        float far = 0.33 + 0.2 * ridge(p.x * 1.1 + 7.0, 2.0) - 0.06;
        if (uv.y < far)
        {
            float shade = smoothstep(far - 0.06, far, uv.y);
            col = mix(vec3(0.05, 0.075, 0.18), vec3(0.14, 0.2, 0.42), shade * 0.6) + vec3(0.1, 0.14, 0.3) * flash;
        }
        float near = 0.25 + 0.14 * ridge(p.x * 2.0 - 3.0, 5.0) - 0.05;
        if (uv.y < near)
            col = vec3(0.025, 0.035, 0.085) + vec3(0.1, 0.15, 0.32) * smoothstep(near - 0.01, near, uv.y) * 0.45;
        col = mix(col, vec3(0.16, 0.22, 0.45), smoothstep(0.4, 0.22, uv.y) * 0.22 * fogAmount);

        // ---- The castle: a hazy back row and a darker front row ----
        float n = floor(towerCount);
        float span = min(1.1, aspect * 0.78);
        float ground = 0.2 + 0.012 * sin(p.x * 9.0);
        float wallTop = ground + 0.07 * castleHeight;

        float backRim = 0.0;
        vec3 backGlow = vec3(0.0);
        float back = towers(p, uv.y, floor(n * 0.7 + 0.5), span * 1.15, wallTop + 0.02, castleHeight * 1.05, 2.0, 0.35, px,
                            moonPos, backRim, backGlow);
        float frontRim = 0.0;
        vec3 frontGlow = vec3(0.0);
        float front = towers(p, uv.y, n, span, wallTop, castleHeight, 0.0, 1.0, px, moonPos, frontRim, frontGlow);
        bool wall = abs(p.x) < span * 0.55 && uv.y < wallTop + step(0.5, fract(p.x * 70.0)) * 0.007;

        float grainy = 0.85 + 0.15 * fbm3(p * 60.0);
        if (back > 0.5)
            col = mix(vec3(0.06, 0.08, 0.19), vec3(0.12, 0.17, 0.36), 0.25) * grainy + vec3(0.2, 0.28, 0.6) * backRim + backGlow;
        if (front > 0.5 || wall)
        {
            col = vec3(0.022, 0.026, 0.06) * grainy + vec3(0.22, 0.3, 0.62) * frontRim + frontGlow;
            col += vec3(0.25, 0.32, 0.7) * flash * 0.12;
        }
        // Rose window on the cathedral, pulsing with the bass.
        float rose = length(p - vec2(0.0, wallTop + 0.12 * castleHeight));
        float roseR = 0.016 * castleHeight;
        if (front > 0.5)
            col += vec3(1.0, 0.55, 0.25) * smoothstep(roseR, roseR * 0.6, rose) * (0.5 + 1.2 * bass);

        // ---- Cliffs in front ----
        float cliff = 0.1 + 0.05 * fbm3(vec2(p.x * 3.0, 9.0)) + 0.2 * smoothstep(0.35, 0.95, abs(uv.x - 0.5) * 2.0);
        if (uv.y < cliff)
            col = vec3(0.012, 0.016, 0.036) + vec3(0.08, 0.12, 0.26) * smoothstep(cliff - 0.006, cliff, uv.y) * 0.5;
    }

    // ---- Fog rolling along the bottom, brighter on kicks ----
    float f = fbm(vec2(p.x * 1.8 + time * 0.05, uv.y * 5.0 - time * 0.03));
    float fog = smoothstep(0.32, 0.0, uv.y) * (0.45 + 0.55 * f) * fogAmount;
    col = mix(col, vec3(0.28, 0.36, 0.62) * (0.8 + 0.4 * beat), clamp(fog * 0.65, 0.0, 1.0));

    col += vec3(0.2, 0.26, 0.5) * flash * 0.12 * smoothstep(0.3, 0.9, uv.y);   // the sky lights up for a moment
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

constexpr const char* Fantasy3DShader = R"(
// ---------------------------------------------------------------- 3D village fly-through
// Signed distance functions: each returns how far a point is from a shape.
float sdBox(vec3 p, vec3 b)
{
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}
float sdCylinder(vec3 p, float r, float h)
{
    vec2 d = abs(vec2(length(p.xz), p.y)) - vec2(r, h);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}
float sdCone(vec3 p, float r, float h)          // base radius r at y=0, tip at y=h
{
    vec2 q = vec2(length(p.xz), p.y);
    vec2 n = normalize(vec2(h, r));
    return max(dot(q - vec2(0.0, h), n), -q.y);
}
float sdPrism(vec3 p, float halfW, float h, float halfLen)   // gable roof, ridge along z
{
    vec2 n = normalize(vec2(h, halfW));
    return max(max(dot(vec2(abs(p.x), p.y - h), n), -p.y), abs(p.z) - halfLen);
}

// Everything about the building in the street cell around p (both sides of the street mirror).
// cell: which block along the street; side: -1 left, +1 right.
void lot(float cell, float side, out float height, out float front, out float halfW, out float kind, out float seed)
{
    seed = h11(cell * 7.13 + side * 3.1);
    float s2 = h11(cell * 3.7 + side * 9.2);
    // Each block's height also follows a slice of the spectrum, so the skyline breathes with the song.
    height = 3.4 + 3.8 * seed + 2.0 * band(fract(cell * 0.137 + (side > 0.0 ? 0.5 : 0.0)));
    front = 4.6 + 0.6 * s2;
    halfW = 2.2 + 0.4 * s2;
    kind = s2 < 0.2 ? 2.0 : (seed > 0.55 ? 1.0 : 0.0);  // 2 tower, 1 gable facing street, 0 ridge along street
}

// Returns (distance, material). Materials: 1 street, 2 wall, 3 roof, 4 lamp, 5 iron, 6 timber wall.
vec2 map(vec3 p)
{
    vec2 res = vec2(p.y, 1.0);
    float side = p.x < 0.0 ? -1.0 : 1.0;
    vec3 q = vec3(abs(p.x), p.y, p.z);
    float cell = floor(q.z / 6.0);
    float lz = q.z - cell * 6.0 - 3.0;

    float height, front, halfW, kind, seed;
    lot(cell, side, height, front, halfW, kind, seed);
    float material = seed > 0.35 ? 6.0 : 2.0;

    if (kind > 1.5)
    {
        vec3 tp = vec3(q.x - front - 1.7, q.y, lz);
        float tall = height + 3.0;
        float body = sdCylinder(tp - vec3(0.0, tall * 0.5, 0.0), 1.7, tall * 0.5);
        float roof = sdCone(tp - vec3(0.0, tall, 0.0), 2.1, 4.5 + 2.5 * seed);
        if (body < res.x) res = vec2(body, 2.0);
        if (roof < res.x) res = vec2(roof, 3.0);
    }
    else
    {
        vec3 bp = vec3(q.x - front - 2.2, q.y - height * 0.5, lz);
        float body = sdBox(bp, vec3(2.2, height * 0.5, halfW));
        vec3 rp = vec3(q.x - front - 2.2, q.y - height, lz);
        float roof = kind > 0.5 ? sdPrism(rp.zyx, halfW + 0.2, 2.6 + 1.4 * seed, 2.45)
                                : sdPrism(rp, 2.45, 2.0 + 1.4 * seed, halfW + 0.2);
        float chimney = sdBox(rp - vec3(1.0, 1.6, halfW * 0.45), vec3(0.28, 1.3, 0.28));
        if (body < res.x) res = vec2(body, material);
        if (min(roof, chimney) < res.x) res = vec2(min(roof, chimney), 3.0);
    }

    // Street lamps between the houses.
    float lampZ = q.z - floor(q.z / 6.0 + 0.5) * 6.0;
    float pole = sdBox(vec3(q.x - 3.95, q.y - 1.7, lampZ), vec3(0.05, 1.7, 0.05));
    float arm = sdBox(vec3(q.x - 3.75, q.y - 3.35, lampZ), vec3(0.22, 0.04, 0.04));
    float lamp = length(vec3(q.x - 3.55, q.y - 3.15, lampZ)) - 0.14;
    if (min(pole, arm) < res.x) res = vec2(min(pole, arm), 5.0);
    if (lamp < res.x) res = vec2(lamp, 4.0);
    return res;
}

vec3 normalAt(vec3 p, float t)
{
    vec2 e = vec2(0.002 * (1.0 + t * 0.05), -0.002 * (1.0 + t * 0.05));
    return normalize(e.xyy * map(p + e.xyy).x + e.yyx * map(p + e.yyx).x + e.yxy * map(p + e.yxy).x + e.xxx * map(p + e.xxx).x);
}

float softShadow(vec3 ro, vec3 rd)
{
    float res = 1.0;
    float t = 0.15;
    for (int i = 0; i < 14; ++i)
    {
        float h = map(ro + rd * t).x;
        res = min(res, 10.0 * h / t);
        t += clamp(h, 0.15, 1.6);
        if (res < 0.02 || t > 18.0)
            break;
    }
    return clamp(res, 0.0, 1.0);
}

vec3 lampColor() { return vec3(1.0, 0.62, 0.3) * (1.3 + 1.1 * beat + 0.4 * energy); }

void main()
{
    vec2 uv = gl_FragCoord.xy / resolution;
    vec2 sp = (gl_FragCoord.xy - 0.5 * resolution) / resolution.y;
    vec3 moonDir = normalize(vec3(-0.45, 0.42, 1.0));
    vec3 moonCol = vec3(0.42, 0.52, 0.88) * (1.0 + 2.2 * flash);

    // Camera gliding down the middle of the street, swaying gently.
    vec3 ro = vec3(0.6 * sin(time * 0.23) + sway, 1.75 + 0.12 * sin(time * 0.37), camZ);
    vec3 rd = normalize(vec3(sp.x, sp.y + 0.1, 1.45));
    float yaw = 0.12 * sin(time * 0.17) + sway * 0.08;
    rd.xz = mat2(cos(yaw), -sin(yaw), sin(yaw), cos(yaw)) * rd.xz;

    // March.
    float t = 0.05;
    vec2 hit = vec2(0.0);
    bool found = false;
    for (int i = 0; i < 72; ++i)
    {
        hit = map(ro + rd * t);
        if (hit.x < 0.002 * t)
        {
            found = true;
            break;
        }
        t += hit.x * 0.9;
        if (t > 95.0)
            break;
    }
    // Rays skimming the street can run out of steps before they "arrive". Treat that as a hit
    // instead of sky, and catch any remaining downward ray on the street plane, so the ground
    // continues smoothly into the fog with no visible edge.
    if (!found && t <= 95.0)
        found = true;
    if (!found && rd.y < -1e-4)
    {
        t = -ro.y / rd.y;
        hit = vec2(0.0, 1.0);
        found = true;
    }

    // ---- Sky: moon, clouds, the castle on the hill at the end of the road ----
    vec2 sky = rd.xy / max(rd.z, 0.05);
    vec3 col = mix(vec3(0.06, 0.09, 0.2), vec3(0.008, 0.012, 0.04), smoothstep(0.0, 0.8, sky.y));
    vec2 starCell = floor(gl_FragCoord.xy / 2.0);
    if (h21(starCell) > 0.997)
        col += vec3(0.7, 0.78, 1.0) * starLight(starCell, time, treble) * smoothstep(0.15, 0.5, sky.y);
    float md = dot(rd, moonDir);
    float moonR = 0.012 * moonSize * (1.0 + 0.06 * bass);
    col += vec3(0.2, 0.3, 0.7) * pow(max(md, 0.0), 60.0 / moonSize) * (0.6 + 0.5 * bass);
    col = mix(col, vec3(0.75, 0.82, 0.96) * (0.85 + 0.3 * fbm(sky * 40.0)), smoothstep(1.0 - moonR, 1.0 - moonR * 0.8, md));
    float drift = time * 0.02 * cloudSpeed;
    float cloud = fbm(vec2(sky.x * 1.3 + drift, sky.y * 3.0)) + 0.5 * fbm(vec2(sky.x * 2.8 - drift * 1.6, sky.y * 5.0));
    float density = smoothstep(0.02, 0.38, cloud) * smoothstep(0.08, 0.3, sky.y);
    float lit = pow(max(md, 0.0), 12.0);
    col = mix(col, mix(vec3(0.02, 0.03, 0.08), vec3(0.32, 0.44, 0.85), clamp(lit * 1.4 + flash * 0.8, 0.0, 1.0)), density * 0.9);
    // Distant mountains and the castle, hazy blue on the horizon.
    float far = 0.06 + 0.12 * ridge(sky.x * 1.3 + 7.0, 2.0) - 0.04;
    if (sky.y < far)
        col = mix(vec3(0.06, 0.09, 0.2), vec3(0.12, 0.17, 0.36), smoothstep(far - 0.05, far, sky.y) * 0.6);
    float castleRim = 0.0;
    vec3 castleGlow = vec3(0.0);
    float castle = towers(vec2(sky.x * 1.6, sky.y * 1.6), sky.y * 1.6, floor(towerCount), 0.95, 0.04, castleHeight * 0.55, 0.0, 0.8,
                          1.0 / resolution.y, vec2(-0.5, 0.4), castleRim, castleGlow);
    if (castle > 0.5)
        col = mix(vec3(0.05, 0.07, 0.15), vec3(0.09, 0.12, 0.26), 0.5) + vec3(0.2, 0.28, 0.6) * castleRim + castleGlow * 0.8;

    vec3 fogCol = mix(vec3(0.06, 0.08, 0.17), vec3(0.2, 0.27, 0.5), pow(max(md, 0.0), 6.0)) * (1.0 + flash);

    // ---- The street scene ----
    if (found)
    {
        vec3 p = ro + rd * t;
        vec3 n = normalAt(p, t);
        float mat = hit.y;
        float side = p.x < 0.0 ? -1.0 : 1.0;
        float cell = floor(p.z / 6.0);
        float lz = p.z - cell * 6.0 - 3.0;
        float height, front, halfW, kind, seed;
        lot(cell, side, height, front, halfW, kind, seed);

        vec3 albedo;
        vec3 emissive = vec3(0.0);
        float wet = 0.0;
        if (mat < 1.5)
        {
            // Wet cobblestones.
            vec2 c = p.xz * vec2(2.2, 1.6);
            c.x += floor(c.y) * 0.5;
            vec2 f = abs(fract(c) - 0.5);
            float stone = smoothstep(0.5, 0.38, max(f.x, f.y));
            albedo = vec3(0.1, 0.105, 0.12) * (0.55 + 0.45 * stone) * (0.85 + 0.3 * noise(p.xz * 3.0));
            wet = 0.6 * stone;
            n = normalize(n + vec3(noise(c * 2.0) * 0.08, 0.0, noise(c * 2.0 + 5.0) * 0.08));
        }
        else if (mat < 2.5 || mat > 5.5)
        {
            // Stone or half-timbered walls.
            vec2 face = abs(n.x) > 0.5 ? p.zy : p.xy;      // texture the wall in its own plane
            float grain = noise(face * 2.5) * 0.5 + noise(face * 7.0) * 0.25;
            if (mat > 5.5)
            {
                albedo = vec3(0.34, 0.31, 0.28) * (0.85 + 0.25 * grain);
                float beamY = step(abs(fract(p.y / 1.8) - 0.5), 0.05);
                float beamZ = step(abs(fract((lz + 3.0) / 1.5) - 0.5), 0.045);
                albedo = mix(albedo, vec3(0.07, 0.05, 0.04), max(beamY, beamZ));
            }
            else
            {
                vec2 brick = vec2(face.x * 1.4 + floor(face.y * 2.5) * 0.5, face.y * 2.5);
                float mortar = step(0.08, fract(brick.x)) * step(0.1, fract(brick.y));
                albedo = vec3(0.2, 0.21, 0.25) * (0.7 + 0.3 * mortar) * (0.85 + 0.3 * grain);
            }
            // Windows on walls that face the street; they light up and flicker with the music.
            bool facesStreet = abs(n.x) > 0.6 && n.x * side < 0.0;
            if (facesStreet && p.y > 0.9 && p.y < height - 0.6)
            {
                float row = floor((p.y - 0.9) / 1.8);
                float inRow = fract((p.y - 0.9) / 1.8);
                float span = kind > 1.5 ? 3.4 : halfW * 2.0;
                float colsN = kind > 1.5 ? 2.0 : 3.0;
                float wz = (lz + span * 0.5) / span * colsN;
                float inCol = fract(wz);
                float key = cell * 31.0 + side * 17.0 + row * 7.0 + floor(wz);
                float pane = step(abs(inCol - 0.5), 0.17) * step(abs(inRow - 0.48), 0.24);
                float frame = step(abs(inCol - 0.5), 0.22) * step(abs(inRow - 0.48), 0.3) - pane;
                float on = step(0.6 - 0.35 * energy - 0.25 * band(fract(key * 0.013)), h11(key));
                float flick = 0.75 + 0.25 * sin(time * (4.0 + 8.0 * h11(key + 2.0)) + key);
                vec3 warm = mix(vec3(1.0, 0.55, 0.22), vec3(1.0, 0.38, 0.45), step(0.85, h11(key + 4.0)));
                albedo = mix(albedo, vec3(0.04, 0.03, 0.03), frame);
                albedo = mix(albedo, vec3(0.02, 0.025, 0.04), pane);
                // Each window follows its own slice of the spectrum, and all of them punch on the kick.
                float level = band(fract(key * 0.013));
                emissive += warm * pane * on * flick * (0.25 + 2.6 * level * level + 1.4 * beat) * windowGain;
                wet = pane * 0.6;  // glass catches highlights
            }
        }
        else if (mat < 3.5)
        {
            // Slate roofs in rows of shingles.
            float rows = step(0.12, fract(p.y * 3.0 + noise(p.xz * 4.0) * 0.2));
            albedo = vec3(0.09, 0.095, 0.12) * (0.6 + 0.4 * rows) * (0.8 + 0.4 * noise(p.xz * 8.0));
            wet = 0.25;
        }
        else if (mat < 4.5)
        {
            albedo = vec3(0.0);
            emissive = lampColor() * 1.4;
        }
        else
        {
            albedo = vec3(0.03, 0.03, 0.035);
            wet = 0.4;
        }

        // Moonlight with soft shadows, plus a little sky light.
        float ao = clamp(map(p + n * 0.35).x / 0.35, 0.0, 1.0);
        float diff = max(dot(n, moonDir), 0.0);
        float shadow = diff > 0.0 ? softShadow(p + n * 0.02, moonDir) : 0.0;
        vec3 light = moonCol * diff * shadow + vec3(0.05, 0.07, 0.14) * (0.5 + 0.5 * n.y) * ao;

        // Warm light from the nearest street lamps on both sides.
        float lampCell = floor(p.z / 6.0 + 0.5) * 6.0;
        for (int k = -1; k <= 1; ++k)
        {
            for (int s = 0; s < 2; ++s)
            {
                vec3 lp = vec3(s == 0 ? -3.55 : 3.55, 3.1, lampCell + float(k) * 6.0);
                vec3 L = lp - p;
                float d2 = dot(L, L);
                light += lampColor() * max(dot(n, normalize(L)), 0.0) * 0.9 / (1.0 + d2 * 0.45) * ao;
            }
        }
        col = albedo * light + emissive;

        // Wet surfaces mirror the moon and lamps.
        vec3 refl = reflect(rd, n);
        col += wet * (moonCol * 1.6 * pow(max(dot(refl, moonDir), 0.0), 24.0) * shadow);
        col += wet * lampColor() * 0.25 * pow(max(refl.y, 0.0), 4.0) * smoothstep(1.0, 0.0, abs(abs(p.x) - 3.5) / 2.0);

        // Fog swallows the distance.
        // Fog swallows the distance; by the far end everything is fog, so the street never "ends".
        float fog = 1.0 - exp(-t * 0.035 * (0.6 + 0.4 * fogAmount));
        fog = max(fog, smoothstep(40.0, 90.0, t));
        col = mix(col, fogCol, fog);
    }
    else
    {
        // Rays that run out of steps far down the street (or below the horizon) land in the fog bank.
        float low = smoothstep(0.12, -0.03, sky.y);
        col = mix(col, fogCol, max(low, rd.y < 0.0 ? 1.0 : 0.0));
    }

    // Halos around lamps hanging in the misty air.
    float baseCell = floor(ro.z / 6.0 + 0.5) * 6.0;
    for (int k = 0; k < 9; ++k)
    {
        for (int s = 0; s < 2; ++s)
        {
            vec3 lp = vec3(s == 0 ? -3.55 : 3.55, 3.15, baseCell + float(k) * 6.0);
            float along = dot(lp - ro, rd);
            if (along < 0.2 || (found && along > t + 0.5))
                continue;
            vec3 closest = ro + rd * along;
            float d = length(closest - lp);
            col += lampColor() * 0.06 * exp(-d * d * 3.0) / (1.0 + along * 0.05) * fogAmount;
        }
    }

    // Low mist rolling over the cobbles.
    float mist = fbm(vec2(sp.x * 2.0 + time * 0.06, sp.y * 4.0 - time * 0.03));
    col = mix(col, vec3(0.2, 0.26, 0.46), smoothstep(0.0, -0.45, sp.y) * (0.35 + 0.4 * mist) * 0.5 * fogAmount);
    col += vec3(0.2, 0.26, 0.5) * flash * 0.08;

    // Alpha marks open sky (1) vs. buildings (0), so bats and lightning can stay behind the houses.
    gl_FragColor = vec4(max(col, 0.0), found ? 0.0 : 1.0);
}
)";

// Pop: a bright, flat cartoon sky (sunset gradient, flat two-tone clouds). With a picture, the
// picture is cel-shaded: flattened into a few bold colors with black ink outlines.
constexpr const char* PopShader = R"(
uniform vec2 resolution;
uniform float time;
uniform float bass;
uniform float beat;
uniform float cloudSpeed;
uniform sampler2D image;
uniform float hasImage;
uniform vec2 fitScale;
uniform float ink;
uniform vec4 warps[8];   // x, y (pixels), radius, strength: > 0 bulge around a starburst, < 0 ripple of a ring

// How far the pops push this pixel around.
vec2 popWarp(vec2 frag)
{
    vec2 offset = vec2(0.0);
    for (int i = 0; i < 8; ++i)
    {
        vec4 w = warps[i];
        if (w.w == 0.0)
            continue;
        vec2 d = frag - w.xy;
        float dist = length(d) + 1e-3;
        if (w.w > 0.0)
        {
            // Bulge: pull samples toward the center, so the picture swells out around the burst.
            float k = w.w * exp(-(dist * dist) / (w.z * w.z));
            offset -= d * k * 0.45;
        }
        else
        {
            // Ripple: a shock wave travelling with the ring.
            float band = (dist - w.z) / (resolution.y * 0.035);
            offset += (d / dist) * (-w.w) * exp(-band * band) * sin(band * 3.0) * resolution.y * 0.03;
        }
    }
    return offset;
}

vec3 pic(vec2 q)
{
    vec2 st = vec2(0.5) + vec2(q.x, -q.y) * fitScale;
    return texture2D(image, 1.0 - abs(1.0 - mod(st, 2.0))).rgb;
}
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
    vec2 frag = gl_FragCoord.xy + popWarp(gl_FragCoord.xy);
    vec2 uv = frag / resolution;
    float aspect = resolution.x / resolution.y;
    vec2 p = vec2((uv.x - 0.5) * aspect, uv.y - 0.5);
    float px = 1.0 / resolution.y;
    vec3 col;

    if (hasImage > 0.5)
    {
        vec2 q = p * (1.0 - 0.03 * bass - 0.03 * beat);
        vec3 c = pic(q);
        // Flatten into bold bands of color and push the saturation.
        c = floor(c * 4.0 + 0.5) / 4.0;
        c = clamp(mix(vec3(lum(c)), c, 1.5), 0.0, 1.0);
        // Ink outlines where the picture's brightness changes sharply.
        float e = px * (1.5 + ink);
        float gx = lum(pic(q + vec2(e, 0.0))) - lum(pic(q - vec2(e, 0.0)));
        float gy = lum(pic(q + vec2(0.0, e))) - lum(pic(q - vec2(0.0, e)));
        float edge = smoothstep(0.12, 0.25, length(vec2(gx, gy)));
        col = mix(c, vec3(0.05, 0.03, 0.08), edge);
    }
    else
    {
        // Sunset: orange at the bottom, magenta in the middle, violet at the top.
        float y = uv.y + 0.04 * sin(time * 0.3);
        col = mix(vec3(1.0, 0.58, 0.36), vec3(0.85, 0.3, 0.62), smoothstep(0.0, 0.5, y));
        col = mix(col, vec3(0.42, 0.16, 0.6), smoothstep(0.45, 1.0, y));
        col *= 1.0 + 0.08 * beat;

        // Flat two-tone clouds with clean edges.
        float drift = time * 0.03 * cloudSpeed;
        float c1 = fbm(vec2(p.x * 1.1 + drift, p.y * 2.4 + 3.0));
        float c2 = fbm(vec2(p.x * 1.7 - drift * 1.4, p.y * 3.2 + 9.0));
        float tone1 = smoothstep(0.06, 0.06 + px * 4.0, c1);
        float tone2 = smoothstep(0.2, 0.2 + px * 4.0, c2);
        col = mix(col, col * vec3(1.12, 1.0, 1.1) + vec3(0.06, 0.02, 0.08), tone1 * 0.55);
        col = mix(col, mix(col, vec3(1.0, 0.82, 0.92), 0.45), tone2 * 0.6);
    }
    gl_FragColor = vec4(col, 1.0);
}
)";

constexpr const char* DriveShader = R"(
// Night Drive: the view from the driver's seat on a highway at night. Perspective road with wet
// reflections, streetlights rushing past, traffic, a lit city skyline, rain on the windshield and
// a glowing dashboard. Everything is analytic 2D projection, so it's cheap enough for 60 fps.
uniform vec2 resolution;
uniform float time;
uniform float travel;      // meters driven
uniform float curveNow;    // how much the road bends ahead
uniform float wheelAngle;
uniform float beat;
uniform float energy;
uniform float speedNorm;   // 0..1 for the speedometer
uniform float rain;
uniform float traffic;     // number of cars
uniform float cityAmount;
uniform float lampGain;
uniform float dashOn;
uniform sampler2D audio;
uniform sampler2D image;
uniform float hasImage;
uniform float imageAspect;

const float CAM_H = 1.25;
const float F = 1.15;
float H = 0.43; // horizon height, set in main

float hash11(float n) { return fract(sin(n * 127.1) * 43758.5453); }
float hash21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float band(float x) { return texture2D(audio, vec2(clamp(x, 0.0, 1.0), 0.25)).r; }
float curveAt(float z) { return curveNow * z * z * 0.0016; }

// Each star twinkles at its own speed and phase, and now and then flares up at its own random
// moment. Louder highs make more stars flare, but never all at once.
float starLight(vec2 cell, float t, float music)
{
    float h1 = hash21(cell + 17.3);
    float h2 = hash21(cell + 41.7);
    float h3 = hash21(cell + 5.1);
    float steady = 0.2 + 0.3 * h3;
    float tw = 0.5 + 0.5 * sin(t * (0.6 + 2.6 * h1) + h2 * 6.2831);
    float period = 2.5 + 8.0 * h1;
    float x = (t + h2 * period) / period;
    float fires = step(hash21(cell + floor(x) * 1.37 + 3.3), 0.15 + 0.5 * music);
    float local = (fract(x) - 0.5) * 9.0;
    return steady * (0.35 + 0.65 * tw) + fires * exp(-local * local) * 1.4;
}

vec3 proj(vec3 w) { return vec3((w.x + curveAt(w.z)) * F / w.z, H + (w.y - CAM_H) * F / w.z, w.z); }
float lampZ(float k, float side) { float sp = 45.0; return sp * (k + 1.0) - mod(travel + side * 22.5, sp); }

// The two kinds of traffic: cars ahead going our way (tail lights), and oncoming (headlights).
void carAt(float i, out float lane, out float cz, out float oncoming)
{
    float hi = hash11(i * 17.3 + 1.0);
    float hj = hash11(i * 5.7 + 2.0);
    oncoming = mod(i, 2.0);
    if (oncoming > 0.5)
    {
        lane = hj < 0.5 ? -7.0 : -10.5;
        cz = mod(hi * 700.0 - travel * 1.9, 700.0) + 2.0;
    }
    else
    {
        lane = hj < 0.5 ? -3.5 : 0.0;
        cz = mod(hi * 600.0 - travel * 0.05 + time * 1.2, 600.0) + 20.0;
    }
}

vec3 haze(float y) { return mix(vec3(0.17, 0.08, 0.15), vec3(0.06, 0.04, 0.09), clamp(y * 3.0, 0.0, 1.0)); }

vec3 scene(vec2 p)
{
    float px = 1.0 / resolution.y;
    vec3 col;
    float y = p.y - H;
    bool ground = y < 0.0;
    float gz = 0.0;
    float wx = 0.0;
    float wet = 0.45 + 0.55 * rain;

    if (!ground)
    {
        // ---- Sky with a city glow on the horizon ----
        col = mix(vec3(0.14, 0.06, 0.17), vec3(0.012, 0.016, 0.05), smoothstep(0.0, 0.45, y));
        col += vec3(0.55, 0.25, 0.12) * exp(-y * 16.0) * 0.45;
        vec2 g = floor(p * 320.0);
        if (hash21(g) > 0.996)
            col += vec3(0.6, 0.65, 0.8) * starLight(g, time, energy) * smoothstep(0.15, 0.4, y) * 0.6 * (1.0 - rain * 0.8);

        // ---- City skyline: two layers of towers with windows that light up with the music ----
        for (int layer = 0; layer < 2; ++layer)
        {
            float fl = float(layer);
            float xs = p.x * (1.2 + 0.5 * fl) - curveNow * (0.12 + 0.1 * fl) + travel * (0.0003 + 0.0005 * fl) + fl * 3.7;
            float cols = 12.0 + 6.0 * fl;
            float cell = floor(xs * cols);
            float tall = (0.02 + 0.12 * pow(hash11(cell + fl * 50.0), 2.4)) * (1.0 - 0.35 * fl) * cityAmount;
            if (y < tall)
            {
                col = mix(vec3(0.03, 0.03, 0.055), vec3(0.06, 0.045, 0.08), fl);
                vec2 wg = vec2(fract(xs * cols) * 6.0, y * (170.0 - 40.0 * fl));
                vec2 wi = floor(wg);
                vec2 wf = fract(wg);
                float key = cell * 13.0 + wi.x * 7.0 + wi.y * 3.0 + fl * 101.0;
                float lvl = band(fract(key * 0.0137));
                float on = step(0.66 - 0.3 * lvl - 0.15 * energy, hash11(key));
                float pane = step(0.2, wf.x) * step(wf.x, 0.8) * step(0.3, wf.y) * step(wf.y, 0.75);
                vec3 warm = mix(vec3(1.0, 0.68, 0.32), vec3(0.6, 0.8, 1.0), step(0.72, hash11(key + 3.0)));
                col += warm * pane * on * (0.25 + 1.2 * lvl) * (0.7 - 0.3 * fl);
                // Red aircraft lights blinking on the tallest towers.
                if (tall > 0.09 && abs(fract(xs * cols) - 0.5) < 0.08 && abs(y - tall + 0.004) < 0.003)
                    col += vec3(1.0, 0.1, 0.05) * step(0.5, fract(time * 0.8 + cell * 0.37)) * 1.5;
            }
        }
        col = mix(col, haze(y), exp(-y * 40.0) * 0.6);
    }
    else
    {
        // ---- The road, in perspective ----
        gz = CAM_H * F / max(-y, 1e-4);
        wx = p.x * gz / F - curveAt(gz);
        float pw = gz / F * px * 1.2;

        vec3 albedo = vec3(0.06, 0.06, 0.07) * (0.85 + 0.3 * hash21(floor(vec2(wx * 6.0, (gz + travel) * 6.0))) * clamp(4.0 / gz, 0.0, 1.0));
        if (wx > 4.6 || wx < -13.2)
            albedo = vec3(0.02, 0.024, 0.02);

        float dash = step(fract((gz + travel) / 11.0), 0.4);
        dash = mix(dash, 0.4, smoothstep(40.0, 120.0, gz));
        float white = 0.0;
        float yellow = 0.0;
        // coverage of a painted line of half-width w at x
        #define LINE(x, w) clamp(((w) + pw - abs(wx - (x))) / (2.0 * pw), 0.0, 1.0) * min(1.0, (w) * 2.0 / pw)
        white += LINE(1.75, 0.08) + LINE(-12.25, 0.08);
        white += (LINE(-1.75, 0.07) + LINE(-8.75, 0.07)) * dash;
        yellow += LINE(-5.1, 0.06) + LINE(-5.4, 0.06);
        albedo = mix(albedo, vec3(0.8, 0.8, 0.75), clamp(white, 0.0, 1.0));
        albedo = mix(albedo, vec3(0.85, 0.62, 0.12), clamp(yellow, 0.0, 1.0));

        // Our own headlights, and pools of orange light under each streetlight.
        float beam = exp(-(wx * wx) / (3.0 + gz * 0.35)) * smoothstep(70.0, 6.0, gz) * smoothstep(2.0, 7.0, gz);
        vec3 light = vec3(0.05, 0.05, 0.08) + vec3(1.0, 0.94, 0.82) * beam * 1.1;
        for (int side = 0; side < 2; ++side)
        {
            float hx = side == 0 ? 4.2 : -12.2;
            for (int k = 0; k < 8; ++k)
            {
                float lz = lampZ(float(k), float(side));
                float dx = wx - hx;
                float dz = gz - lz;
                light += vec3(1.0, 0.55, 0.2) * exp(-(dx * dx + dz * dz) / 70.0) * 0.9 * lampGain * (0.85 + 0.3 * beat);
            }
        }
        col = albedo * light;

        // Wet asphalt: long shimmering reflections of every light.
        float shimmer = 0.7 + 0.3 * sin(gz * 1.7 + p.x * 40.0 + time * 3.0);
        vec3 refl = vec3(0.0);
        for (int side = 0; side < 2; ++side)
        {
            float hx = side == 0 ? 4.2 : -12.2;
            for (int k = 0; k < 8; ++k)
            {
                float lz = lampZ(float(k), float(side));
                if (lz < 2.0)
                    continue;
                vec3 m = proj(vec3(hx, -8.5, lz));
                float w = 0.003 + 0.35 / lz;
                float dx = p.x - m.x;
                float dy = (p.y - m.y) / (0.04 + 3.0 / lz);
                refl += vec3(1.0, 0.55, 0.22) * exp(-dx * dx / (w * w) - dy * dy) * lampGain * 0.55;
            }
        }
        for (int i = 0; i < 8; ++i)
        {
            if (float(i) >= traffic)
                break;
            float lane, cz, onc;
            carAt(float(i), lane, cz, onc);
            for (int s = -1; s <= 1; s += 2)
            {
                vec3 m = proj(vec3(lane + float(s) * (onc > 0.5 ? 0.65 : 0.7), -(onc > 0.5 ? 0.65 : 0.85), cz));
                float w = 0.002 + 0.25 / cz;
                float dx = p.x - m.x;
                float dy = (p.y - m.y) / (0.03 + 2.5 / cz);
                vec3 lc = onc > 0.5 ? vec3(1.0, 0.95, 0.85) * 1.2 : vec3(1.0, 0.1, 0.06) * (0.8 + 1.5 * beat);
                refl += lc * exp(-dx * dx / (w * w) - dy * dy) * 0.5;
            }
        }
        col += refl * wet * shimmer;
        col = mix(col, haze(0.0), 1.0 - exp(-gz * 0.006));
    }

    // ---- Billboards with your picture on the right side of the road ----
    if (hasImage > 0.5)
    {
        float bw = 9.0;
        float bh = clamp(bw / max(imageAspect, 0.1), 3.0, 9.0);
        for (int k = 1; k >= 0; --k)
        {
            float bz = 260.0 * (float(k) + 1.0) - mod(travel + 130.0, 260.0);
            if (bz < 3.0)
                continue;
            float c = curveAt(bz);
            float sx0 = (8.5 + c) * F / bz;
            float sx1 = (8.5 + bw + c) * F / bz;
            float sy0 = H + (5.0 - CAM_H) * F / bz;
            float sy1 = H + (5.0 + bh - CAM_H) * F / bz;
            float legW = 0.25 * F / bz;
            if (p.y < sy0 && p.y > H + (0.0 - CAM_H) * F / bz &&
                (abs(p.x - (sx0 + (sx1 - sx0) * 0.25)) < legW || abs(p.x - (sx0 + (sx1 - sx0) * 0.75)) < legW))
                col = vec3(0.02, 0.02, 0.025);
            if (p.x > sx0 && p.x < sx1 && p.y > sy0 && p.y < sy1)
            {
                vec2 st = vec2((p.x - sx0) / (sx1 - sx0), 1.0 - (p.y - sy0) / (sy1 - sy0));
                vec3 pic = texture2D(image, st).rgb * (0.75 + 0.35 * beat + 0.2 * energy);
                float frame = step(st.x, 0.02) + step(0.98, st.x) + step(st.y, 0.03) + step(0.97, st.y);
                col = mix(pic, vec3(0.03), clamp(frame, 0.0, 1.0));
                col = mix(col, haze(0.0), 1.0 - exp(-bz * 0.005));
            }
        }
    }

    // ---- Streetlight poles (far to near) and their glowing lamps ----
    vec3 glow = vec3(0.0);
    for (int side = 0; side < 2; ++side)
    {
        float poleX = side == 0 ? 6.0 : -14.2;
        float hx = side == 0 ? 4.2 : -12.2;
        for (int kk = 0; kk < 8; ++kk)
        {
            float k = 7.0 - float(kk);
            float lz = lampZ(k, float(side));
            if (lz < 0.6)
                continue;
            vec3 base = proj(vec3(poleX, 0.0, lz));
            vec3 top = proj(vec3(poleX, 8.8, lz));
            vec3 head = proj(vec3(hx, 8.5, lz));
            float pwid = 0.11 * F / lz;
            bool pole = abs(p.x - base.x) < pwid && p.y > base.y && p.y < top.y;
            bool arm = p.y > top.y - 0.09 * F / lz && p.y < top.y + 0.02 * F / lz &&
                       p.x > min(base.x, head.x) && p.x < max(base.x, head.x);
            if (pole || arm)
                col = vec3(0.025, 0.022, 0.03) + vec3(0.25, 0.13, 0.05) * smoothstep(top.y - 0.3, top.y, p.y) * 0.4;
            float d = length(p - head.xy);
            float core = smoothstep(0.32 * F / lz + px, 0.32 * F / lz * 0.4, d);
            float hr = min(0.006 + 1.6 / lz * (1.0 + rain), 0.08);
            glow += vec3(1.0, 0.62, 0.3) * (core * 2.0 + exp(-d * d / (hr * hr)) * 0.35) * lampGain * (0.9 + 0.4 * beat);
        }
    }

    // ---- Traffic ----
    for (int i = 0; i < 8; ++i)
    {
        if (float(i) >= traffic)
            break;
        float lane, cz, onc;
        carAt(float(i), lane, cz, onc);
        vec3 bl = proj(vec3(lane - 0.9, 0.25, cz));
        vec3 tr = proj(vec3(lane + 0.9, 1.45, cz));
        if (p.x > bl.x && p.x < tr.x && p.y > bl.y && p.y < tr.y)
        {
            float shape = smoothstep(tr.y, tr.y - (tr.y - bl.y) * 0.35, p.y);  // roofline
            float width = mix(0.78, 1.0, shape);
            float cx = (bl.x + tr.x) * 0.5;
            if (abs(p.x - cx) < (tr.x - bl.x) * 0.5 * width)
                col = mix(vec3(0.012, 0.012, 0.018), haze(0.0), 1.0 - exp(-cz * 0.006));
        }
        for (int s = -1; s <= 1; s += 2)
        {
            vec3 l = proj(vec3(lane + float(s) * (onc > 0.5 ? 0.65 : 0.7), onc > 0.5 ? 0.65 : 0.85, cz));
            vec2 d = p - l.xy;
            float r = (onc > 0.5 ? 0.16 : 0.11) * F / cz;
            float core = smoothstep(r + px, r * 0.3, length(d));
            float fog = exp(-cz * 0.004);
            if (onc > 0.5)
            {
                float hr = min(0.008 + 2.2 / cz * (1.0 + rain), 0.06);
                glow += vec3(1.0, 0.95, 0.85) * (core * 2.5 + exp(-dot(d, d) / (hr * hr)) * 0.6) * fog;
                // Anamorphic flare streak.
                glow += vec3(0.6, 0.75, 1.0) * exp(-d.y * d.y / 0.00002) * exp(-abs(d.x) * (6.0 + cz * 0.2)) * 0.5 * fog;
            }
            else
            {
                float hr = min(0.006 + 0.9 / cz * (1.0 + rain), 0.05);
                glow += vec3(1.0, 0.08, 0.05) * (core * 2.0 + exp(-dot(d, d) / (hr * hr)) * 0.5) * (0.7 + 1.6 * beat) * fog;
            }
        }
    }
    return col + glow;
}

mat2 rot(float a) { float c = cos(a), s = sin(a); return mat2(c, s, -s, c); }

float sdRoundBox(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / resolution;
    float aspect = resolution.x / resolution.y;
    vec2 p = vec2((uv.x - 0.5) * aspect, uv.y);
    float halfW = 0.5 * aspect;
    H = 0.43 - 0.007 * beat + 0.0012 * sin(time * 17.0) * speedNorm;

    // ---- Raindrops on the windshield bend the view behind them ----
    vec2 sp = p;
    float dropR = 0.0;
    vec2 dropD = vec2(0.0);
    float trail = 0.0;
    if (rain > 0.01)
    {
        // Beads: small drops sitting on the glass, slowly growing and fading.
        float cellSize = 0.04;
        vec2 g = p / cellSize;
        vec2 cell = floor(g);
        float h = hash21(cell);
        if (h < rain * 0.5)
        {
            float life = fract(time * 0.1 + h * 7.0);
            vec2 off = vec2(hash21(cell + 3.1), hash21(cell + 7.7)) - 0.5;
            float r = (0.1 + 0.16 * hash21(cell + 1.3)) * smoothstep(0.0, 0.1, life) * smoothstep(1.0, 0.9, life);
            vec2 d = fract(g) - 0.5 - off * 0.6;
            if (length(d) < r)
            {
                vec2 c = (cell + 0.5 + off * 0.6) * cellSize;
                sp = c - (p - c) * 6.0;
                dropR = r;
                dropD = d;
            }
        }

        // Runners: bigger drops that break loose and run down the glass, wiggling as they go
        // and leaving a wet trail behind them.
        float colW = 0.045;
        float column = floor(p.x / colW);
        float hc = hash11(column * 3.7 + 1.0);
        if (hc < rain * 0.95)
        {
            float speed = 0.12 + 0.25 * hash11(column + 9.0);
            float phase = fract(time * speed * 0.6 + hash11(column + 4.0));
            float headY = 1.15 - phase * 1.5;                 // starts above the glass, runs off the bottom
            float wiggle = 0.006 * sin(p.y * 38.0 + column) + 0.004 * sin(p.y * 91.0 + column * 2.0);
            float cx = (column + 0.3 + 0.4 * hash11(column + 2.0)) * colW + wiggle;
            float r = 0.008 + 0.006 * hash11(column + 5.0);
            vec2 d = vec2((p.x - cx) * 1.15, p.y - headY);
            float len = length(vec2(d.x, d.y * (d.y > 0.0 ? 0.75 : 1.0)));   // a little teardrop
            if (len < r)
            {
                vec2 c = vec2(cx, headY);
                sp = c - (p - c) * 7.0;
                dropR = r / cellSize;
                dropD = d / cellSize;
            }
            else if (p.y > headY && p.y < headY + 0.4 && abs(p.x - cx) < r * 0.55)
            {
                // The wet trail: a thin streak that bends the light and dries out further up.
                float fade = 1.0 - (p.y - headY) / 0.4;
                trail = fade * smoothstep(r * 0.55, 0.0, abs(p.x - cx));
                sp = p + vec2((p.x - cx) * 5.0 * fade, 0.004 * fade);
            }
        }
    }
    vec3 col = scene(sp);
    if (dropR > 0.0)
    {
        float l = length(dropD) / dropR;
        col *= 0.75 + 0.25 * smoothstep(1.0, 0.6, l);
        col += vec3(0.5) * smoothstep(0.35, 0.0, length(dropD / dropR - vec2(-0.35, 0.35)));
    }
    col += vec3(0.09, 0.09, 0.12) * trail;
    col += vec3(0.03, 0.025, 0.045) * rain;            // misty glass

    if (dashOn > 0.5)
    {
        // Light spilling into the car from streetlights we pass and oncoming headlights.
        float lampL = exp(-max(lampZ(0.0, 0.0), 0.0) * 0.16) + exp(-max(lampZ(0.0, 1.0), 0.0) * 0.22) * 0.5;
        float headL = 0.0;
        for (int i = 0; i < 8; ++i)
        {
            if (float(i) >= traffic)
                break;
            float lane, cz, onc;
            carAt(float(i), lane, cz, onc);
            headL += onc * exp(-cz * 0.08);
        }
        vec3 inLight = vec3(1.0, 0.55, 0.2) * lampL * lampGain * 0.6 + vec3(0.8, 0.85, 1.0) * headL * 0.5 + vec3(0.02, 0.022, 0.035);

        float gx = -halfW * 0.42;

        // The windshield opening: curved A-pillars, an arched roof line and a domed dashboard,
        // all blended with rounded corners (a smooth max of the four edges).
        float yy = uv.y;
        float pillar = halfW * (0.035 + 0.1 * yy + 0.16 * yy * yy);           // pillars bow inward toward the top
        float roofY = 0.975 - 0.085 * (p.x / halfW) * (p.x / halfW);
        float hx2 = (p.x - gx) / 0.13;
        float hood = 0.04 * exp(-hx2 * hx2);
        float dashTop = 0.175 + 0.06 * (1.0 - (p.x / halfW) * (p.x / halfW)) + hood;
        float k = 0.07;
        float eL = (-halfW + pillar) - p.x;
        float eR = p.x - (halfW - pillar);
        float eT = p.y - roofY;
        float eB = dashTop - p.y;
        // Smooth maximum: > 0 means the pixel is inside the car body, not glass.
        float body = max(max(eL, eR), max(eT, eB));
        float a = max(eL, eR);
        float b = max(eT, eB);
        float hk = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
        body = mix(a, b, hk) + k * hk * (1.0 - hk);
        bool isDash = eB > 0.0 && eB >= max(max(eL, eR), eT) - 0.02;
        if (body > 0.0 && !isDash)
        {
            col = vec3(0.012, 0.012, 0.016) + inLight * 0.12;
            col += inLight * 0.45 * smoothstep(0.008, 0.0, body);           // light catching the trim
        }

        // Rear-view mirror with headlights behind us.
        vec2 mp = p - vec2(halfW * 0.08, 0.875);
        float mirror = sdRoundBox(mp, vec2(0.13, 0.034), 0.018);
        if (abs(mp.x) < 0.008 && p.y > 0.9 && p.y < roofY)
            col = vec3(0.015);
        if (mirror < 0.0)
        {
            vec3 m = vec3(0.02, 0.02, 0.035);
            for (int s = -1; s <= 1; s += 2)
            {
                vec2 hp = vec2(float(s) * 0.03 + 0.02 * sin(time * 0.31), -0.005 + 0.003 * sin(time * 0.7));
                float d = length(mp - hp);
                m += vec3(1.0, 0.95, 0.85) * exp(-d * d / 0.00006) * (0.5 + 0.3 * sin(time * 0.23));
            }
            col = mirror > -0.006 ? vec3(0.01) : m;
        }

        // Dashboard.
        if (body > 0.0 && isDash)
        {
            col = vec3(0.015, 0.015, 0.02) * (0.9 + 0.2 * hash21(floor(gl_FragCoord.xy / 2.0)));
            col += (inLight * 0.9 + 0.03) * smoothstep(0.012, 0.0, body);   // rounded top edge catching light
            col *= 0.8 + 0.2 * smoothstep(0.0, 0.08, body);

            // Gauges: speedometer and a tach that follows the energy of the song.
            for (int j = 0; j < 2; ++j)
            {
                vec2 c = vec2(gx + (j == 0 ? -0.075 : 0.075), 0.17);
                vec2 d = p - c;
                float r = 0.055;
                float len = length(d);
                float ang = atan(d.x, d.y);
                if (len < r + 0.004)
                {
                    col = vec3(0.01, 0.025, 0.035);
                    float arc = smoothstep(0.003, 0.0, abs(len - r * 0.92)) * step(abs(ang), 2.35);
                    col += vec3(0.2, 0.8, 1.0) * arc * 0.9;
                    float tick = step(0.85, fract((ang + 2.35) / 4.7 * 10.0)) * step(len, r * 0.88) * step(r * 0.74, len) * step(abs(ang), 2.36);
                    col += vec3(0.6, 0.9, 1.0) * tick * 0.7;
                    float v = j == 0 ? speedNorm : clamp(0.25 + 0.6 * energy + 0.4 * beat, 0.0, 1.0);
                    float na = -2.35 + v * 4.7;
                    vec2 nd = vec2(sin(na), cos(na));
                    float t = clamp(dot(d, nd), 0.0, r * 0.82);
                    float needle = smoothstep(0.003, 0.0012, length(d - nd * t));
                    col = mix(col, vec3(1.0, 0.35, 0.1), needle);
                    col += vec3(1.0, 0.35, 0.1) * exp(-pow(length(d - nd * t) / 0.006, 2.0)) * 0.3;
                }
            }

            // Center screen (the song title and times are drawn on top of it as text).
            vec2 sc = vec2(halfW * 0.25, 0.12);
            vec2 sd = p - sc;
            float screen = sdRoundBox(sd, vec2(0.16, 0.062), 0.01);
            if (screen < 0.0)
            {
                col = vec3(0.012, 0.018, 0.032) + vec3(0.02, 0.04, 0.07) * smoothstep(-0.06, 0.06, sd.y);
                float x0 = sc.x - 0.148;
                float x1 = sc.x + 0.148;
                // Album art: your picture, or a glowing record icon.
                vec2 a = (p - vec2(x0, sc.y - 0.05)) / 0.1;
                if (a.x > 0.0 && a.x < 1.0 && a.y > 0.0 && a.y < 1.0)
                {
                    if (hasImage > 0.5)
                    {
                        vec2 st = a;
                        if (imageAspect > 1.0) st.x = 0.5 + (st.x - 0.5) / imageAspect;
                        else st.y = 0.5 + (st.y - 0.5) * imageAspect;
                        col = texture2D(image, vec2(st.x, 1.0 - st.y)).rgb * 0.95;
                    }
                    else
                    {
                        float rr = length(a - 0.5);
                        float spin = atan(a.y - 0.5, a.x - 0.5) + time * 2.0;
                        col = mix(vec3(0.04, 0.04, 0.06), vec3(0.08, 0.08, 0.1), step(0.5, fract(rr * 18.0)) * 0.4);
                        col *= step(rr, 0.48);
                        col += vec3(1.0, 0.3, 0.7) * step(rr, 0.15) * (0.7 + 0.3 * beat);
                        col += vec3(0.6) * smoothstep(0.02, 0.0, abs(spin - 6.2831 * floor(spin / 6.2831) - 1.0)) * step(0.17, rr) * step(rr, 0.46) * 0.15;
                    }
                }
                // Equalizer along the bottom, right of the art.
                float ex0 = x0 + 0.112;
                float bx = (p.x - ex0) / (x1 - ex0);
                float by = (p.y - (sc.y - 0.05)) / 0.032;
                if (bx > 0.0 && bx < 1.0 && by > 0.0 && by < 1.0)
                {
                    float bi = floor(bx * 26.0);
                    float level = band((bi + 0.5) / 26.0);
                    float barOn = step(0.15, fract(bx * 26.0)) * step(fract(bx * 26.0), 0.85) * step(by, 0.08 + level * 0.92);
                    col += mix(vec3(0.2, 0.9, 1.0), vec3(1.0, 0.25, 0.8), bx) * barOn * 1.1;
                }
                col += vec3(0.1, 0.3, 0.5) * smoothstep(-0.004, 0.0, screen) * 0.6;
            }
        }

        // Steering wheel, turning with the road.
        // rot(+a) on the sample point turns the drawn wheel clockwise, so a right bend turns it right.
        vec2 wd = rot(wheelAngle) * (p - vec2(gx, 0.02));
        float wr = length(wd);
        bool rim = abs(wr - 0.27) < 0.028;
        bool spoke = (abs(wd.y) < 0.028 && wr < 0.27 && abs(wd.x) > 0.06) || (abs(wd.x) < 0.03 && wd.y < 0.0 && wr < 0.27);
        bool hub = wr < 0.075;
        if (rim || spoke || hub)
        {
            col = vec3(0.016, 0.016, 0.02);
            float top = smoothstep(0.0, 0.25, wd.y / max(wr, 0.001));
            col += inLight * (0.35 * top + 0.08);
            if (rim)
                col += (inLight + 0.04) * smoothstep(0.01, 0.0, abs(wr - 0.27 - 0.018)) * top;
            if (hub)
                col += vec3(0.25, 0.3, 0.4) * smoothstep(0.003, 0.0, abs(wr - 0.035)) * 0.5;
        }
    }
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

constexpr const char* ForestShader = R"(
// Dark Forest: walking through dense woods at night with only a flashlight. Ray-marched trunks on
// a jittered grid (a path is kept clear down the middle), thick fog the beam cuts through, pages
// pinned to some trees, and static that crackles on the beat.
uniform vec2 resolution;
uniform float time;
uniform float walkZ;
uniform float beat;
uniform float energy;
uniform float bass;
uniform float flashlight;   // brightness setting
uniform float fogAmount;
uniform float staticAmount;
uniform float density;      // 0..1, how many cells hold a tree
uniform float pagesOn;
uniform float flicker;      // 0..1 how much the light is cutting out right now
uniform vec2 look;          // where the flashlight points (yaw, pitch)
uniform sampler2D image;
uniform float hasImage;
uniform float imageAspect;

float hash21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float hash11(float n) { return fract(sin(n * 127.1) * 43758.5453); }

const float CELL = 3.2;

// Tree in a grid cell: returns center (xz), radius; radius 0 means no tree.
vec3 treeIn(vec2 cell)
{
    float h = hash21(cell);
    vec2 jitter = vec2(hash21(cell + 3.7), hash21(cell + 9.1)) - 0.5;
    vec2 c = (cell + 0.5) * CELL + jitter * CELL * 0.55;
    float r = 0.18 + 0.22 * hash21(cell + 5.3);
    if (abs(c.x) < 1.6 || h > density)   // keep the path clear
        r = 0.0;
    return vec3(c, r);
}

float groundHeight(vec2 xz)
{
    return 0.12 * sin(xz.x * 0.7) * sin(xz.y * 0.5) + 0.05 * sin(xz.x * 2.3 + xz.y * 1.7);
}

// (distance, material) 1 ground, 2 trunk
vec2 map(vec3 p)
{
    vec2 res = vec2(p.y - groundHeight(p.xz), 1.0);
    vec2 cell = floor(p.xz / CELL);
    vec3 t = treeIn(cell);
    // Each tree stays inside its own cell, so only this cell's tree matters. The march itself
    // stops at the cell's edge (see main), which keeps it from jumping past a neighbor's tree.
    float d = 1e5;
    if (t.z > 0.0)
    {
        // Trunks taper a little and lean slightly.
        vec2 lean = vec2(hash21(cell + 1.1) - 0.5, hash21(cell + 2.2) - 0.5) * 0.06 * p.y;
        d = min(d, length(p.xz - t.xy - lean) - t.z * (1.0 - 0.02 * p.y));
    }
    if (d < res.x)
        res = vec2(d, 2.0);
    return res;
}

vec3 normalAt(vec3 p)
{
    vec2 e = vec2(0.003, -0.003);
    return normalize(e.xyy * map(p + e.xyy).x + e.yyx * map(p + e.yyx).x + e.yxy * map(p + e.yxy).x + e.xxx * map(p + e.xxx).x);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / resolution;
    vec2 sp = (gl_FragCoord.xy - 0.5 * resolution) / resolution.y;

    // Static tears: some lines shift sideways when the static spikes.
    float burst = staticAmount * (0.15 * energy + 0.85 * beat * beat);
    float line = floor(gl_FragCoord.y / 3.0);
    if (hash11(line + floor(time * 30.0)) < burst * 0.25)
        sp.x += (hash11(line * 1.3 + time) - 0.5) * 0.08 * burst;

    // Walking: slow steps with head bob, the view slowly looking around.
    float stride = walkZ * 1.4;
    vec3 ro = vec3(0.25 * sin(walkZ * 0.07), 1.62 + 0.035 * abs(sin(stride)), walkZ);
    float yaw = look.x;
    float pitch = look.y + 0.012 * sin(stride * 2.0);
    vec3 fwd = normalize(vec3(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch)));
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), fwd));
    vec3 up = cross(fwd, right);
    vec3 rd = normalize(fwd * 1.25 + right * sp.x + up * sp.y);

    // Flashlight: held a little low and to the right, aimed where we look, lagging slightly.
    vec3 lightPos = ro + right * 0.25 - up * 0.3;
    vec3 lightDir = normalize(fwd + right * 0.04 * sin(time * 0.9) + up * (0.03 * sin(time * 1.3) - 0.05));
    float power = flashlight * (1.0 - 0.85 * flicker) * (0.85 + 0.35 * beat);

    float t = 0.05;
    vec2 hit = vec2(0.0);
    bool found = false;
    // Rays only ever step to the edge of the cell they're in, measured along the ray. (Stopping
    // at the nearest edge in any direction made rays straight down the path, which runs along a
    // cell border, crawl in tiny steps and give up, leaving a smudge in the middle of the view.)
    vec2 invD = 1.0 / max(abs(rd.xz), vec2(1e-4));
    vec2 dirSign = sign(rd.xz) + step(rd.xz, vec2(0.0)) * step(vec2(0.0), rd.xz);
    for (int i = 0; i < 110; ++i)
    {
        vec3 p = ro + rd * t;
        hit = map(p);
        if (hit.x < 0.002 * t)
        {
            found = true;
            break;
        }
        vec2 cell = floor(p.xz / CELL);
        vec2 wall = (cell + step(0.0, dirSign)) * CELL;
        vec2 exitT = abs(wall - p.xz) * invD;
        t += min(hit.x, min(exitT.x, exitT.y) + 0.01);
        if (t > 45.0)
            break;
    }

    vec3 col = vec3(0.0);
    vec3 moon = vec3(0.05, 0.06, 0.09);
    if (found)
    {
        vec3 p = ro + rd * t;
        vec3 n = normalAt(p);
        vec3 albedo;
        float page = 0.0;
        vec3 pageCol = vec3(0.0);
        if (hit.y < 1.5)
        {
            // Forest floor: dirt, dead leaves and twigs.
            float leaves = hash21(floor(p.xz * 9.0));
            albedo = mix(vec3(0.06, 0.05, 0.035), vec3(0.11, 0.08, 0.05), leaves);
            albedo = mix(albedo, vec3(0.05, 0.07, 0.035), smoothstep(0.5, 0.9, hash21(floor(p.xz * 2.0))));
            albedo *= 0.75 + 0.5 * hash21(floor(p.xz * 40.0));
        }
        else
        {
            // Bark: vertical ridges.
            vec2 cell = floor(p.xz / CELL);
            vec3 tr = treeIn(cell);
            vec2 d = p.xz - tr.xy;
            float ang = atan(d.y, d.x);
            float ridge = 0.5 + 0.5 * sin(ang * 22.0 + sin(p.y * 3.0 + ang * 3.0) * 1.5);
            float knots = smoothstep(0.75, 1.0, hash21(floor(vec2(ang * 3.0, p.y * 1.5))));
            albedo = vec3(0.075, 0.062, 0.05) * (0.55 + 0.55 * ridge) * (0.75 + 0.5 * hash21(floor(vec2(ang * 9.0, p.y * 6.0)))) * (1.0 - 0.5 * knots);
            // Some trees have a page pinned on the side facing the path.
            if (pagesOn > 0.5 && hash21(cell + 13.0) < 0.1)
            {
                float face = tr.x > 0.0 ? 3.1416 : 0.0;          // the direction toward the path
                float da = ang - face;
                da = da - 6.2831 * floor((da + 3.1416) / 6.2831);
                float u = da * tr.z / 0.22 + 0.5;              // across the page
                float v = (p.y - 1.25) / 0.3;                   // up the page
                if (u > 0.0 && u < 1.0 && v > 0.0 && v < 1.0)
                {
                    page = 1.0;
                    if (hasImage > 0.5)
                    {
                        vec2 st = vec2(u, 1.0 - v);
                        pageCol = texture2D(image, st).rgb * 0.9;
                    }
                    else
                    {
                        // Paper with frantic scribbles.
                        float key = hash21(cell + 21.0);
                        float rows = floor(v * 9.0);
                        float stroke = abs(fract(v * 9.0) - 0.5 - 0.2 * sin(u * 30.0 + rows * 3.0 + key * 20.0));
                        float ink = smoothstep(0.12, 0.04, stroke) * step(0.08, u) * step(u, 0.92 - 0.3 * hash11(rows + key * 9.0));
                        float big = smoothstep(0.06, 0.02, abs(length(vec2(u - 0.5, (v - 0.55) * 1.4)) - 0.22 - 0.03 * sin(atan(v - 0.55, u - 0.5) * 7.0 + key * 10.0)));
                        pageCol = mix(vec3(0.82, 0.8, 0.72), vec3(0.08, 0.07, 0.07), max(ink * 0.85, big * step(0.5, key)));
                    }
                    albedo = pageCol;
                }
            }
        }

        // Flashlight: a cone with a bright hot spot, falling off with distance.
        vec3 L = lightPos - p;
        float dist = length(L);
        L /= dist;
        float cone = dot(-L, lightDir);
        float spot = smoothstep(0.86, 0.95, cone) * 0.75 + smoothstep(0.955, 0.985, cone) * 0.6;
        float diff = max(dot(n, L), 0.0);
        col = albedo * (moon * (0.5 + 0.5 * n.y) + vec3(1.0, 0.93, 0.8) * spot * diff * power * 14.0 / (1.0 + dist * dist * 0.08));
        col += pageCol * page * spot * power * 1.2 / (1.0 + dist * dist * 0.12);   // paper catches the light

        float fog = 1.0 - exp(-t * (0.045 + 0.03 * energy) * fogAmount);
        col = mix(col, vec3(0.008, 0.009, 0.013), fog);
    }
    else
    {
        // Canopy and sky: almost nothing.
        col = vec3(0.01, 0.012, 0.02) * (0.6 + 0.4 * uv.y);
    }

    // The beam itself, lit up in the fog.
    float beam = 0.0;
    float maxT = min(t, 16.0);
    for (int i = 0; i < 10; ++i)
    {
        float s = (float(i) + hash21(gl_FragCoord.xy + time)) / 10.0 * maxT;
        vec3 q = ro + rd * s;
        vec3 L = q - lightPos;
        float d = length(L);
        float cone = dot(L / d, lightDir);
        beam += smoothstep(0.88, 0.97, cone) / (1.0 + d * d * 0.08);
    }
    col += vec3(0.75, 0.75, 0.7) * beam / 10.0 * maxT * 0.012 * power * fogAmount;

    // Camcorder grit: grain, static, vignette.
    float grain = hash21(gl_FragCoord.xy + fract(time * 7.3) * 100.0);
    col += (grain - 0.5) * (0.03 + 0.3 * burst);
    col = mix(col, vec3(grain) * 0.6, clamp(burst * 0.3, 0.0, 0.6));
    vec2 v = uv - 0.5;
    col *= 1.0 - dot(v, v) * 1.6;
    col = max(col, 0.0);
    gl_FragColor = vec4(col, 1.0);
}
)";

// Hijack: the background of a hacked feed. A flowing neon field (or your picture, posterized)
// printed as big halftone dots, spray-paint strokes, a faint pixel grid and datamosh blocks.
constexpr const char* HijackShader = R"(
uniform vec2 resolution;
uniform float time;
uniform float bass;
uniform float beat;
uniform float energy;
uniform float palette;   // steps on big kicks: which neon color scheme
uniform float dots;      // halftone size
uniform sampler2D audio;
uniform sampler2D image;
uniform float hasImage;
uniform vec2 fitScale;

float h21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float band(float x) { return texture2D(audio, vec2(clamp(x, 0.0, 1.0), 0.25)).r; }

vec3 neon(float t, float pal)
{
    vec3 a, b, c, d;
    float k = mod(floor(pal), 3.0);
    if (k < 0.5)      { a = vec3(0.05, 0.0, 0.09); b = vec3(1.0, 0.1, 0.6);  c = vec3(0.0, 0.88, 1.0); d = vec3(0.95, 1.0, 0.18); }
    else if (k < 1.5) { a = vec3(0.0, 0.03, 0.07); b = vec3(0.55, 0.15, 1.0); c = vec3(1.0, 0.15, 0.45); d = vec3(0.25, 1.0, 0.6); }
    else              { a = vec3(0.07, 0.0, 0.03); b = vec3(1.0, 0.33, 0.0);  c = vec3(1.0, 0.1, 0.7);  d = vec3(0.1, 0.95, 1.0); }
    t = clamp(t, 0.0, 1.0);
    if (t < 0.33) return mix(a, b, t / 0.33);
    if (t < 0.66) return mix(b, c, (t - 0.33) / 0.33);
    return mix(c, d, (t - 0.66) / 0.34);
}

vec2 picUv(vec2 q) { return vec2(0.5) + vec2(q.x, -q.y) * fitScale; }

void main()
{
    vec2 frag = gl_FragCoord.xy;
    vec2 uv = frag / resolution;
    float aspect = resolution.x / resolution.y;
    vec2 p = vec2((uv.x - 0.5) * aspect, uv.y - 0.5);

    // Datamosh: a few chunky blocks get pixelated and slid around for a moment.
    float tick = floor(time * 8.0);
    vec2 blk = floor(p * 7.0);
    vec2 q = p;
    if (h21(blk + tick * 1.7) < 0.03 + 0.22 * beat)
    {
        float cs = 0.025 + 0.05 * h21(blk + 3.0);
        q = (floor(p / cs) + 0.5) * cs + (vec2(h21(blk + tick), h21(blk - tick)) - 0.5) * 0.1 * (0.3 + beat);
    }

    // The flowing neon field.
    float f = fbm(q * 1.5 + vec2(time * 0.06, -time * 0.045));
    f = f * 1.05 + 0.25 * sin(q.x * 2.6 + q.y * 1.9 + time * 0.5) + 0.3 * bass - 0.05;
    float tone = clamp(f, 0.0, 1.0);
    float onPic = 0.0;
    vec2 st = picUv(q);
    if (hasImage > 0.5 && st.x > 0.0 && st.x < 1.0 && st.y > 0.0 && st.y < 1.0)
    {
        vec3 c = texture2D(image, st).rgb;
        float l = dot(c, vec3(0.299, 0.587, 0.114));
        float stepped = floor(l * 4.0 + 0.5) / 4.0;              // posterized into a few flat tones
        tone = clamp(mix(l, stepped, 0.8) * (0.9 + 0.35 * bass), 0.0, 1.0);
        onPic = 1.0;
    }

    // Halftone print: a rotated grid of dots that grow with the tone.
    vec2 shifted = frag + (q - p) * resolution.y;
    float cell = max(3.0, resolution.y * 0.012 * dots);
    float ang = 0.52;
    vec2 g = mat2(cos(ang), sin(ang), -sin(ang), cos(ang)) * shifted / cell;
    vec2 gc = fract(g) - 0.5;
    float radius = sqrt(tone) * 0.66 + 0.05 * beat;
    float aa = 1.2 / cell;
    float dotMask = smoothstep(radius + aa, radius - aa, length(gc));
    vec3 ink = neon(tone, palette);
    vec3 col = neon(0.02, palette) + ink * (0.12 + 0.35 * onPic * tone);
    col = mix(col, ink * (1.0 + 0.25 * beat), dotMask);

    // Spray-paint strokes sweeping across, a little ragged at the edges.
    for (int i = 0; i < 2; ++i)
    {
        float fi = float(i);
        float y0 = 0.28 * sin(time * 0.13 + fi * 2.4) + (fi - 0.5) * 0.2;
        float d = abs(p.y - y0 - 0.09 * sin(p.x * 2.2 + time * 0.4 + fi * 3.0));
        d += 0.012 * noise(p * 55.0 + fi * 7.0);
        float w = 0.018 + 0.03 * band(0.1 + 0.4 * fi) + 0.01 * bass;
        float stroke = smoothstep(w, w - 0.006, d) * smoothstep(-0.2, 0.3, sin(p.x * 0.7 + time * 0.2 + fi * 4.0));
        col = mix(col, neon(0.45 + 0.4 * fi, palette + 1.0) * 1.15, stroke * (1.0 - 0.5 * onPic));
    }

    // Faint pixel grid and scanlines.
    vec2 gp = fract(frag / (resolution.y * 0.05));
    float grid = step(0.965, max(gp.x, gp.y));
    col += neon(0.7, palette) * grid * 0.05;
    col *= 0.9 + 0.1 * sin(frag.y * 3.1416);
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

// Hijack: tears up the finished frame (popups and text included): sliced rows, displaced blocks,
// RGB split and noise lines that get worse on the beat, and an inverted flash on big hits.
constexpr const char* HijackGlitchShader = R"(
uniform sampler2D source;
uniform vec2 resolution;
uniform float seed;
uniform float beat;
uniform float amount;
uniform float invert;
float h(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    float g = amount * (0.12 + beat);
    float row = floor(uv.y * 46.0);
    if (h(vec2(row, seed)) < 0.1 * g)
        uv.x += (h(vec2(row, seed + 7.0)) - 0.5) * 0.16 * g;
    vec2 b = floor(uv * vec2(14.0, 10.0));
    float swap = 0.0;
    if (h(b + seed * 3.1) < 0.045 * g)
    {
        uv += (vec2(h(b + seed), h(b - seed)) - 0.5) * 0.12;
        swap = 1.0;
    }
    float split = 0.0012 + 0.009 * g;
    vec3 col;
    col.r = texture2D(source, uv + vec2(split, 0.0)).r;
    col.g = texture2D(source, uv).g;
    col.b = texture2D(source, uv - vec2(split, 0.0)).b;
    if (swap > 0.5)
        col = col.brg;
    if (h(vec2(floor(uv.y * resolution.y * 0.5), seed)) > 1.0 - 0.008 * g)
        col = mix(col, vec3(1.0), 0.65);
    col = mix(col, 1.0 - col, invert);
    gl_FragColor = vec4(col, 1.0);
}
)";

// Rage: in the crowd at a rage show. Haze, red and white beams sweeping from the stage truss,
// an LED wall at the back, rows of jumping silhouettes lit from behind, strobes and heavy grain.
constexpr const char* RageShader = R"(
uniform vec2 resolution;
uniform float time;
uniform float bass;
uniform float beat;
uniform float energy;
uniform float strobe;      // 0..1 white strobe right now
uniform float flash;       // 0..1 blown-out camera flash
uniform float beamCount;
uniform float crowd;       // crowd density
uniform float faces;       // 1 = some of the crowd turn around with glowing faces
uniform float grain;
uniform float jump;        // crowd jump clock
uniform float sweep;       // beam sweep clock
uniform vec2 shake;
uniform float zoom;
uniform sampler2D audio;
uniform sampler2D image;
uniform float hasImage;
uniform float imageAspect;

float h11(float n) { return fract(sin(n * 127.1) * 43758.5453); }
float h21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float band(float x) { return texture2D(audio, vec2(clamp(x, 0.0, 1.0), 0.25)).r; }
mat2 rot(float a) { float c = cos(a), s = sin(a); return mat2(c, s, -s, c); }

float sdBox(vec2 p, vec2 b, float r) { vec2 d = abs(p) - b + r; return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r; }
float sdSeg(vec2 p, vec2 a, vec2 b, float r)
{
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

// One person in a row. Everyone gets their own look from their seed: head size and shape, a
// hairstyle (spikes, swinging dreads, a huge afro, a mohawk, a beanie, little horns, or bald),
// body width, a long neck now and then, and a pose (hands up, devil horns, phone up, headbanging).
// A few are turned around facing you with glowing eyes and a grin.
// em: 0 = nothing glowing here, 1 = phone screen, 2 = glowing face.
float person(vec2 p, float s, float seed, float lift, out float em)
{
    em = 0.0;
    vec2 q = (p - vec2(0.0, lift)) / s;
    float hs = 0.022 + 0.013 * h11(seed * 5.3);
    float neck = h11(seed * 8.1) > 0.86 ? 0.04 : 0.0;
    float bw = 0.032 + 0.024 * h11(seed * 2.9);
    float bang = sin(jump * 2.0 + seed * 3.0) * 0.014 * step(0.62, h11(seed * 4.4));   // headbangers
    vec2 hc = vec2(bang, 0.13 + neck + hs * 0.25);
    float ov = 0.85 + 0.45 * h11(seed * 6.6);                                           // round to egg-shaped heads
    float d = (length((q - hc) * vec2(1.0, 1.0 / ov)) - hs) * min(ov, 1.0);
    if (neck > 0.0)
        d = min(d, sdSeg(q, vec2(0.0, 0.1), hc, 0.008));
    d = min(d, sdBox(q - vec2(0.0, 0.055), vec2(bw, 0.06), 0.02));
    d = min(d, sdBox(q - vec2(0.0, -0.08), vec2(bw * 0.85, 0.09), 0.01));
    vec2 top = hc + vec2(0.0, hs * ov);

    float hair = floor(h11(seed * 9.7) * 7.0);
    if (hair < 0.5)
    {
        for (int i = 0; i < 5; ++i)   // spikes
        {
            float a = -1.15 + 0.575 * float(i);
            d = min(d, sdSeg(q, hc, hc + vec2(sin(a), cos(a)) * (hs * ov + 0.03 + 0.015 * h11(seed + float(i))), 0.0045));
        }
    }
    else if (hair < 1.5)
    {
        for (int i = 0; i < 5; ++i)   // dreads that swing as they jump
        {
            float o = (float(i) - 2.0) * hs * 0.45;
            vec2 a0 = hc + vec2(o, hs * 0.6);
            vec2 a1 = hc + vec2(o * 1.9 + 0.022 * sin(jump * 1.7 + float(i) + seed), -hs - 0.035);
            d = min(d, sdSeg(q, a0, a1, 0.0055));
        }
    }
    else if (hair < 2.5)
        d = min(d, length(q - hc - vec2(0.0, hs * 0.4)) - hs * 1.7);                   // huge afro
    else if (hair < 3.5)
        d = min(d, sdBox(q - top - vec2(0.0, 0.012), vec2(0.0055, 0.024), 0.003));     // mohawk
    else if (hair < 4.5)
    {
        d = min(d, sdBox(q - hc - vec2(0.0, hs * 0.55), vec2(hs * 1.05, hs * 0.6), hs * 0.4));   // beanie
        d = min(d, length(q - top - vec2(0.0, hs * 0.55)) - hs * 0.42);                         // with a pom
    }
    else if (hair < 5.5)
    {
        d = min(d, sdSeg(q, hc + vec2(-hs * 0.6, hs * 0.6), hc + vec2(-hs * 1.35, hs * 2.0), 0.0055));   // little horns
        d = min(d, sdSeg(q, hc + vec2(hs * 0.6, hs * 0.6), hc + vec2(hs * 1.35, hs * 2.0), 0.0055));
    }

    // Arms and hands.
    float pose = h11(seed * 3.1);
    float sway = 0.014 * sin(jump * 1.3 + seed * 6.0);
    float reach = 0.2 + 0.07 * h11(seed * 7.7);   // some have freakishly long arms
    if (pose > 0.32)
    {
        vec2 hand = vec2(0.055 + sway, reach + 0.03 * bass);
        d = min(d, sdSeg(q, vec2(0.03, 0.09), hand, 0.0085));
        if (pose > 0.88)
        {
            float phone = sdBox(q - hand - vec2(0.0, 0.018), vec2(0.008, 0.014), 0.002);   // phone up, recording
            d = min(d, phone);
            if (phone < -0.0018)
                em = 1.0;
        }
        else if (pose > 0.7)
        {
            d = min(d, sdSeg(q, hand, hand + vec2(-0.009, 0.024), 0.0032));   // devil horns
            d = min(d, sdSeg(q, hand, hand + vec2(0.009, 0.024), 0.0032));
        }
    }
    if (pose > 0.55 && pose <= 0.88)
        d = min(d, sdSeg(q, vec2(-0.03, 0.09), vec2(-0.06 - sway, reach - 0.01), 0.0085));

    // Turned around facing you: glowing eyes and a big grin.
    if (faces > 0.5 && h11(seed * 12.9) > 0.85 && length((q - hc) * vec2(1.0, 1.0 / ov)) < hs)
    {
        vec2 e = (q - hc) / vec2(hs, hs * ov);
        float big = 0.2 + 0.1 * h11(seed * 14.1);                     // mismatched eyes
        float eyes = min(length((e - vec2(-0.38, 0.18)) * vec2(1.0, 0.8)) - big,
                         length((e - vec2(0.38, 0.18)) * vec2(1.0, 0.8)) - (0.42 - big));
        float grin = max(abs(length(e - vec2(0.0, 0.35)) - 0.66) - 0.09, e.y + 0.05);
        float teeth = step(fract(e.x * 5.0 + 0.5), 0.18);
        if (eyes < 0.0 || (grin < 0.0 && teeth < 0.5))
            em = 2.0;
    }
    return d * s;
}

void main()
{
    vec2 frag = gl_FragCoord.xy;
    vec2 p = (frag - 0.5 * resolution) / resolution.y;
    p = rot(0.025 * sin(time * 0.6) + shake.x * 0.6) * (p / zoom) + shake;   // handheld camera

    float haze = fbm(p * 2.2 + vec2(time * 0.05, time * 0.02));
    vec3 red = vec3(1.0, 0.07, 0.04);
    vec3 col = vec3(0.012, 0.004, 0.004) + red * 0.03 * haze;

    // LED wall at the back of the stage.
    vec2 wallC = vec2(0.0, 0.12);
    vec2 wallH = vec2(0.46, 0.2);
    if (hasImage > 0.5)
    {
        float wallAspect = wallH.x / wallH.y;
        wallH = imageAspect > wallAspect ? vec2(wallH.x, wallH.x / imageAspect) : vec2(wallH.y * imageAspect, wallH.y);
    }
    vec2 wq = (p - wallC) / wallH;
    if (abs(wq.x) < 1.0 && abs(wq.y) < 1.0)
    {
        vec2 cells = vec2(64.0, 64.0 * wallH.y / wallH.x);
        vec2 cell = floor((wq * 0.5 + 0.5) * cells);
        vec2 cuv = (cell + 0.5) / cells;
        vec3 led;
        if (hasImage > 0.5)
        {
            vec3 c = texture2D(image, vec2(cuv.x, 1.0 - cuv.y)).rgb;
            float l = dot(c, vec3(0.299, 0.587, 0.114));
            led = mix(red * l * 1.6, vec3(1.0, 0.85, 0.8) * l, smoothstep(0.6, 1.0, l)) * (0.6 + 0.8 * bass);
        }
        else
        {
            // Huge pulsing bars from the spectrum.
            float v = band(abs(cuv.x - 0.5) * 1.8);
            float bar = step(cuv.y, 0.1 + v * 0.85);
            float ring = smoothstep(0.08, 0.0, abs(length((cuv - 0.5) * vec2(wallH.x / wallH.y, 1.0)) - fract(time * 0.5) * 1.2));
            led = red * (bar * 0.9 + ring * 0.6 * energy) + vec3(1.0) * bar * step(0.92, cuv.y + 0.85 - v * 0.85) * 0.4;
        }
        vec2 pix = fract((wq * 0.5 + 0.5) * cells) - 0.5;
        led *= smoothstep(0.5, 0.3, max(abs(pix.x), abs(pix.y)));   // gaps between LEDs
        col = mix(col, led, 0.95);
    }
    // Stage deck below the wall, catching the light.
    float deck = smoothstep(0.004, 0.0, abs(p.y + 0.11) - 0.012) * step(abs(p.x), 0.75);
    col += red * deck * (0.25 + 0.6 * bass);

    // Truss across the top with beams fanning out of it.
    float trussY = 0.39;
    col = mix(col, vec3(0.02), smoothstep(0.012, 0.006, abs(p.y - trussY)));
    vec3 beamLight = vec3(0.0);
    int n = int(beamCount);
    for (int i = 0; i < 14; ++i)
    {
        if (i >= n)
            break;
        float fi = float(i);
        float t = n > 1 ? fi / float(n - 1) : 0.5;
        vec2 src = vec2(mix(-0.72, 0.72, t), trussY);
        float down = mod(fi, 2.0);
        float base = down > 0.5 ? -1.5708 : 1.5708;
        float ang = base + (t - 0.5) * (down > 0.5 ? 1.2 : -1.6) + 0.55 * sin(sweep * (0.6 + 0.25 * h11(fi)) + fi * 1.7);
        vec2 dir = vec2(cos(ang), sin(ang));
        vec2 rel = p - src;
        float along = dot(rel, dir);
        float across = abs(dot(rel, vec2(-dir.y, dir.x)));
        float width = 0.004 + along * 0.07;
        float level = 0.35 + 1.8 * band(0.04 + 0.85 * t);
        float b = along > 0.0 ? smoothstep(width, 0.0, across) * exp(-along * 0.9) : 0.0;
        b *= 0.6 + 1.4 * haze;
        vec3 c = mod(fi, 3.0) < 0.5 ? vec3(1.0, 0.92, 0.88) : red;
        beamLight += c * b * level;
        beamLight += c * level * 0.012 / (dot(rel, rel) * 400.0 + 0.02);   // the fixture's hot spot
    }
    col += beamLight;
    col = mix(col, vec3(1.0, 0.97, 0.95), strobe * 0.9);

    // Stage light bleeding through the haze just above the crowd.
    col += red * (0.18 + 0.5 * bass + 0.4 * strobe) * exp(-abs(p.y + 0.08) * 9.0) * (0.5 + haze);

    // The crowd, back row first, each row in front of the last.
    float backlight = 0.5 + 1.6 * bass + 2.0 * strobe;
    for (int r = 0; r < 4; ++r)
    {
        float fr = float(r);
        float s = 0.5 + fr * 0.36;
        float baseY = -0.13 - fr * 0.12 - fr * fr * 0.03;
        float spacing = 0.1 * s / max(crowd, 0.2);
        float offset = h11(fr + 4.0) * spacing;
        float idx = floor((p.x - offset) / spacing);
        float rimW = 0.007 * s;
        float best = 1e5;
        float em = 0.0;
        vec2 bq = vec2(0.0);
        float bsc = s, bseed = 0.0, blift = 0.0;
        for (int k = -1; k <= 1; ++k)
        {
            float id = idx + float(k);
            float seed = id * 7.13 + fr * 31.0;
            float x = offset + (id + 0.5) * spacing + (h11(seed) - 0.5) * spacing * 0.5;
            float sc = s * (0.8 + 0.4 * h11(seed + 1.0));
            float lift = max(0.0, sin(jump + h11(seed + 2.0) * 6.2832)) * 0.035 * s * (0.25 + 1.2 * bass);
            vec2 q = p - vec2(x, baseY);
            float e;
            float dd = person(q, sc, seed, lift, e);
            if (dd < best)
            {
                best = dd;
                em = e;
                bq = q;
                bsc = sc;
                bseed = seed;
                blift = lift;
            }
        }
        if (r == 1)
        {
            // Now and then someone crowd-surfs across over everyone's hands.
            float sx = mod(time * 0.11, 4.0) - 2.0;
            vec2 q = p - vec2(sx, baseY + 0.23 * s + 0.01 * sin(time * 3.0));
            float a = 1.45 + 0.12 * sin(time * 1.7);
            q = mat2(cos(a), -sin(a), sin(a), cos(a)) * q;
            float e;
            float dd = person(q, s, 777.0, 0.0, e);
            if (dd < best)
            {
                best = dd;
                em = e;
                bq = q;
                bsc = s;
                bseed = 777.0;
                blift = 0.0;
            }
        }
        if (best < 0.0)
        {
            float e2;
            float above = person(bq + vec2(0.0, rimW), bsc, bseed, blift, e2);   // is light from above reaching this edge?
            float depth = 1.0 - fr / 3.0;   // 1 at the back, 0 at the front
            vec3 body = mix(vec3(0.004, 0.0, 0.0), vec3(0.09, 0.015, 0.012) + red * 0.08 * haze, 0.45 * depth);
            float rim = clamp(above / rimW, 0.0, 1.0);
            rim = max(rim, 0.25 * smoothstep(-0.004 * s, 0.0, best));
            col = body + (red * 0.9 + vec3(0.45) * strobe) * rim * backlight * (0.35 + 0.65 * depth);
            if (em > 1.5)
                col = mix(vec3(1.0, 0.95, 0.85), vec3(0.6, 1.0, 0.9), step(0.5, h11(bseed * 2.2))) * (0.9 + 0.8 * bass + 0.6 * beat);
            else if (em > 0.5)
                col = vec3(0.55, 0.7, 1.0) * (0.9 + 0.3 * sin(time * 9.0 + bseed));
        }
    }
    // Light through the haze in front of everyone.
    col += beamLight * 0.12 * (0.6 + haze);

    // Camera: blown-out flash, crushed blacks, red grade, heavy grain, vignette.
    col *= 1.0 + flash * 3.0;
    col = mix(col, vec3(1.0, 0.95, 0.92), flash * 0.6);
    col = max(col - 0.004, 0.0);
    col = col / (1.0 + col * 0.35);
    col = pow(col, vec3(0.95, 1.05, 1.08));
    float g = h21(frag + fract(time * 13.7) * 97.0) - 0.5;
    col += g * grain * (0.09 + 0.08 * beat);
    vec2 uv = frag / resolution - 0.5;
    col *= 1.0 - dot(uv, uv) * 1.5;
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

// Dystopia: flying slowly down a canyon of colossal black megacity towers in glowing smog. Window
// lights and light strips pulse with the music, warning lights blink on the rooftops, giant cold
// screens hang on the towers, and a colossal monolith with a glowing core stands on the horizon.
constexpr const char* DystopiaShader = R"(
uniform vec2 resolution;
uniform float time;
uniform float travel;
uniform float bass;
uniform float beat;
uniform float energy;
uniform float blink;       // rooftop lights flashing together on the beat
uniform float flash;       // red flash on the biggest hits
uniform float lights;      // window glow setting
uniform float fogAmount;
uniform float screens;
uniform vec2 sway;         // camera drift (x, y)
uniform float kickAge;     // seconds since the last kick (light waves run up the towers)
uniform sampler2D audio;
uniform sampler2D image;
uniform float hasImage;
uniform float imageAspect;

const vec2 CELL = vec2(11.0, 16.0);
uniform vec3 mainCol;     // the color scheme: main glow
uniform vec3 altCol;      // second, cold color
uniform vec3 warnCol;     // warning lights
uniform float smogK;      // how strongly the smog takes the main color

float h11(float n) { return fract(sin(n * 127.1) * 43758.5453); }
float h21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float band(float x) { return texture2D(audio, vec2(clamp(x, 0.0, 1.0), 0.25)).r; }

// The tower in a grid cell: center, half size, body height, crown height. h < 0 = no tower (the canyon).
void tower(vec2 cell, out vec2 c, out vec2 hs, out float h, out float h2, out float id)
{
    id = h21(cell);
    vec2 jit = (vec2(h21(cell + 1.3), h21(cell + 2.7)) - 0.5) * CELL * 0.1;
    c = (cell + 0.5) * CELL + jit;
    hs = vec2(2.8 + 1.6 * h21(cell + 4.1), 3.5 + 3.2 * h21(cell + 5.9));
    h = 22.0 + 105.0 * pow(h21(cell + 7.7), 1.4);
    h2 = h + 6.0 + 22.0 * h21(cell + 9.2);
    if (abs(c.x) < 7.0)
        h = -1.0;
}

float boxD(vec3 p, vec3 b)
{
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float towerD(vec3 p, vec2 cell)
{
    vec2 c, hs;
    float h, h2, id;
    tower(cell, c, hs, h, h2, id);
    if (h < 0.0)
        return 1e5;
    vec3 q = p - vec3(c.x, 0.0, c.y);
    float d = boxD(q - vec3(0.0, (h - 400.0) * 0.5, 0.0), vec3(hs.x, (h + 400.0) * 0.5, hs.y));
    d = min(d, boxD(q - vec3(0.0, (h + h2) * 0.5, 0.0), vec3(hs.x * 0.62, (h2 - h) * 0.5, hs.y * 0.62)));
    float ay = clamp(q.y, h2, h2 + 10.0);
    d = min(d, length(vec3(q.x, q.y - ay, q.z)) - 0.18);   // antenna
    return d;
}

vec3 skyCol(vec3 rd)
{
    vec3 c = mix(mainCol * 0.34 * smogK, vec3(0.006, 0.007, 0.009), smoothstep(-0.05, 0.5, rd.y));
    c = mix(mainCol * 0.53 * smogK, c, smoothstep(-0.45, -0.02, rd.y));   // the city glowing below
    return c * (1.0 + 0.6 * bass);
}

// Everything far away: the smog, the colossal monolith and the searchlights.
vec3 farView(vec3 rd)
{
    vec3 col = skyCol(rd);
    // Smog lit from below.
    vec2 cp = rd.xz / max(rd.y + 0.25, 0.05) * 2.0 + vec2(0.0, travel * 0.004 + time * 0.01);
    float smog = fbm(cp);
    col += mainCol * 0.12 * smogK * smog * smoothstep(0.6, 0.0, rd.y);

    // The colossal monolith far away on the horizon.
    float ax = atan(rd.x, rd.z) - 0.04;
    float ay = rd.y;
    float w = 0.07 + 0.04 * smoothstep(0.62, -0.1, ay);
    if (abs(ax) < w && ay < 0.65 && ay > -0.12)
    {
        vec3 m = vec3(0.005, 0.006, 0.007);
        m += mainCol * 0.5 * step(fract(ay * 45.0), 0.08) * step(abs(ax), w - 0.006) * (0.4 + bass);
        m += altCol * 0.8 * smoothstep(0.004, 0.0, abs(abs(ax) - w + 0.002));   // edge lights
        col = mix(m, col, 0.3 + 0.7 * smoothstep(0.02, -0.12, ay));   // its base sinks into the haze
    }
    float core = length(vec2(ax, (ay - 0.27) * 1.6));
    col += mainCol * (0.6 + 2.2 * bass + 1.5 * beat) * smoothstep(0.03, 0.0, abs(core - 0.075)) * step(abs(ax), w + 0.04);
    col += mainCol * (0.25 + bass) * 0.05 / (core * core * 40.0 + 0.05);
    col += warnCol * 3.0 * max(step(0.5, fract(time * 0.5)), blink) * smoothstep(0.006, 0.0, length(vec2(ax, ay - 0.67)));

    // Searchlights sweeping up out of the city.
    for (int i = 0; i < 4; ++i)
    {
        float fi = float(i);
        vec2 base = vec2(-0.9 + fi * 0.6 + 0.1 * sin(fi * 3.0), -0.08);
        float ang = 1.5708 + 0.5 * sin(time * (0.15 + 0.05 * fi) + fi * 2.0);
        vec2 dir = vec2(cos(ang), sin(ang));
        vec2 rel = vec2(ax, ay) - base;
        float along = dot(rel, dir);
        float across = abs(dot(rel, vec2(-dir.y, dir.x)));
        float beam = along > 0.0 ? smoothstep(0.004 + along * 0.05, 0.0, across) * exp(-along * 1.5) : 0.0;
        col += mix(altCol * 0.8, mainCol, step(0.5, h11(fi))) * beam * (0.18 + 0.4 * energy);
    }
    return col;
}

void main()
{
    vec2 frag = gl_FragCoord.xy;
    vec2 sp = (frag - 0.5 * resolution) / resolution.y;

    vec3 ro = vec3(sway.x, 34.0 + sway.y, travel);
    float yaw = 0.06 * sin(time * 0.05) + sway.x * 0.012;
    float pitch = 0.1;
    vec3 fwd = normalize(vec3(sin(yaw), pitch, cos(yaw)));
    float roll = -sway.x * 0.01;
    vec3 right = normalize(cross(vec3(sin(roll), cos(roll), 0.0), fwd));
    vec3 up = cross(fwd, right);
    vec3 rd = normalize(fwd * 1.15 + right * sp.x + up * sp.y);

    // March, stepping cell by cell so nothing gets skipped.
    vec2 invD = 1.0 / max(abs(rd.xz), vec2(1e-4));
    vec2 dirSign = sign(rd.xz) + step(rd.xz, vec2(0.0)) * step(vec2(0.0), rd.xz);
    float t = 0.2;
    bool hit = false;
    vec2 hitCell = vec2(0.0);
    vec2 lastCell = vec2(1e5);
    vec3 glow = vec3(0.0);
    for (int i = 0; i < 120; ++i)
    {
        vec3 p = ro + rd * t;
        vec2 cell = floor(p.xz / CELL);
        if (cell != lastCell)
        {
            // Rooftop warning light of this cell's tower, glowing in the smog.
            lastCell = cell;
            vec2 c, hs;
            float h, h2, id;
            tower(cell, c, hs, h, h2, id);
            if (h > 0.0)
            {
                vec3 L = vec3(c.x, h2 + 10.0, c.y);
                float s = dot(L - ro, rd);
                if (s > 0.0)
                {
                    float dm = length(ro + rd * s - L);
                    float on = max(step(0.55, fract(time * 0.7 + id)) * 0.7, blink);
                    glow += warnCol * on * 0.05 / (dm * dm * 0.2 + 0.003 * s + 0.01) * exp(-s * 0.006 * fogAmount);
                }
            }
        }
        float d = towerD(p, cell);
        if (d < 0.002 * t)
        {
            hit = true;
            hitCell = cell;
            break;
        }
        vec2 wall = (cell + step(0.0, dirSign)) * CELL;
        vec2 exitT = abs(wall - p.xz) * invD;
        t += min(d, min(exitT.x, exitT.y) + 0.02);
        if (t > 320.0)
            break;
    }

    vec3 col;
    if (hit)
    {
        vec3 p = ro + rd * t;
        vec2 e = vec2(0.01, 0.0);
        vec3 n = normalize(vec3(towerD(p + e.xyy, hitCell) - towerD(p - e.xyy, hitCell),
                                towerD(p + e.yxy, hitCell) - towerD(p - e.yxy, hitCell),
                                towerD(p + e.yyx, hitCell) - towerD(p - e.yyx, hitCell)));
        vec2 c, hs;
        float h, h2, id;
        tower(hitCell, c, hs, h, h2, id);
        vec3 q = p - vec3(c.x, 0.0, c.y);

        // Black glass and metal that reflects the red smog.
        vec3 r = reflect(rd, n);
        float fres = pow(1.0 - max(dot(-rd, n), 0.0), 3.0);
        col = vec3(0.012, 0.012, 0.016) + skyCol(r) * (0.12 + 0.55 * fres);
        col += mainCol * 0.12 * smogK * exp(-max(p.y + 10.0, 0.0) / 45.0);   // underglow from the city below

        if (abs(n.y) < 0.5 && q.y < h2)
        {
            float u = abs(n.x) > 0.5 ? q.z : q.x;
            // Windows: most dark, some red, a few cold white, pulsing with this tower's part of the spectrum.
            vec2 wc = vec2(u * 2.2, q.y * 1.5);
            vec2 wcell = floor(wc);
            vec2 wf = fract(wc);
            float win = step(0.18, wf.x) * step(wf.x, 0.72) * step(0.3, wf.y) * step(wf.y, 0.78);
            // Each tower section listens to its own part of the spectrum: louder means more windows
            // switch on and they burn brighter. On every kick a wave of light runs up the towers.
            float level = band(0.04 + 0.85 * fract(id * 7.31 + floor(q.y / 30.0) * 0.17));
            float on = step(h21(wcell + id * 91.0), (0.04 + 0.32 * level + 0.08 * beat) * lights);
            float pulse = 0.25 + 2.8 * level + 1.2 * beat;
            float front = kickAge * (90.0 + 40.0 * id);
            float wave = smoothstep(7.0, 0.0, abs(q.y + 20.0 - front)) * exp(-kickAge * 1.6) * step(h21(wcell + id * 17.0), 0.55);
            pulse = on * pulse + wave * 2.5;
            on = 1.0;
            float wk = h21(wcell + id * 3.0);
            vec3 wcol = wk < 0.6 ? mainCol * 1.2 : (wk < 0.88 ? altCol : vec3(0.85, 0.9, 1.0));
            col += wcol * win * on * pulse * min(lights, 1.5);
            // Bright strips wrapping the tower every few floors.
            float strip = step(fract(q.y / 26.0 + id), 0.012);
            col += mainCol * 2.2 * strip * (0.12 + 2.4 * bass + 2.0 * beat) * min(lights, 1.5);

            // Giant screens on towers facing the canyon.
            if (screens > 0.5 && h21(hitCell + 11.0) < 0.4 && abs(n.x) > 0.5 && n.x * c.x < 0.0)
            {
                float sh = 20.0 + 10.0 * h21(hitCell + 13.0);
                float sw = hs.y * 0.85;
                float sy0 = min(h * 0.4 + 8.0 * h21(hitCell + 12.0), h - sh - 2.0);
                vec2 suv = vec2(q.z / sw * 0.5 + 0.5, (q.y - sy0) / sh);
                if (n.x > 0.0)
                    suv.x = 1.0 - suv.x;
                if (suv.x > 0.0 && suv.x < 1.0 && suv.y > 0.0 && suv.y < 1.0)
                {
                    vec3 sc;
                    if (hasImage > 0.5)
                    {
                        // The picture is stretched to fill the whole screen.
                        vec3 ic = texture2D(image, vec2(suv.x, 1.0 - suv.y)).rgb;
                        float l = dot(ic, vec3(0.299, 0.587, 0.114));
                        sc = mix(mainCol * l * 1.4, vec3(0.9, 0.95, 1.0) * l, smoothstep(0.55, 1.0, l));
                    }
                    else
                    {
                        // Cold corporate feed: rows of glyphs scrolling over a pulsing bar graph.
                        float row = floor(suv.y * 12.0);
                        float colm = floor(suv.x * 26.0);
                        float glyph = step(h21(vec2(colm, row - floor(time * 1.5 + id * 9.0))), 0.45)
                                    * step(fract(suv.x * 26.0), 0.7) * step(fract(suv.y * 12.0), 0.6);
                        float bars = step(suv.y, band(suv.x * 0.9) * 0.8) * step(fract(suv.x * 40.0), 0.75);
                        float frameLine = step(min(min(suv.x, 1.0 - suv.x), min(suv.y, 1.0 - suv.y)), 0.02);
                        sc = altCol * glyph * 0.6 * step(0.45, suv.y) + mainCol * bars * 1.4 + vec3(0.8, 0.9, 1.0) * frameLine;
                    }
                    float scan = 0.85 + 0.15 * sin(suv.y * 300.0 + time * 8.0);
                    col = sc * scan * (1.1 + 0.5 * beat) + vec3(0.01);
                }
            }
        }
        // Rooftop light itself.
        float onTop = max(step(0.55, fract(time * 0.7 + id)) * 0.7, blink);
        col += warnCol * 6.0 * onTop * smoothstep(0.6, 0.0, length(q - vec3(0.0, h2 + 10.0, 0.0)));

        // Smog swallows distant towers; the monolith stays visible through it as a giant silhouette.
        float fog = 1.0 - exp(-t * 0.011 * fogAmount);
        col = mix(col, mix(skyCol(rd), farView(rd), fog * fog) * 0.92, fog);
    }
    else
        col = farView(rd);
    col += glow;

    // Red flash on the biggest hits, grain, vignette.
    col += mainCol * flash * 0.35;
    col = col / (1.0 + col * 0.25);
    float g = h21(frag + fract(time * 11.3) * 91.0) - 0.5;
    col += g * 0.035;
    vec2 uv = frag / resolution - 0.5;
    col *= 1.0 - dot(uv, uv) * 1.3;
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

// Draws a layer (bats, lightning) only where the 3D scene shows open sky.
constexpr const char* SkyMaskShader = R"(
uniform sampler2D source;
uniform sampler2D mask;
uniform float additive;
void main()
{
    vec4 c = texture2D(source, gl_TexCoord[0].xy);
    float sky = texture2D(mask, gl_TexCoord[0].xy).a;
    gl_FragColor = additive > 0.5 ? vec4(c.rgb * sky, 1.0) : vec4(c.rgb, c.a * sky);
}
)";

// Feedback: redraw last frame zoomed, rotated, warped, recolored and faded.
constexpr const char* FeedbackShader = R"(
uniform sampler2D source;
uniform vec2 resolution;
uniform float zoom;
uniform float angle;
uniform float fade;
uniform float hueShift;
uniform float mirror;
uniform float wobble;
uniform float time;
void main()
{
    float aspect = resolution.x / resolution.y;
    vec2 uv = gl_TexCoord[0].xy - 0.5;
    uv.x *= aspect;
    float c = cos(angle);
    float s = sin(angle);
    uv = vec2(c * uv.x - s * uv.y, s * uv.x + c * uv.y) / zoom;
    uv += wobble * vec2(sin(uv.y * 9.0 + time * 1.3), cos(uv.x * 9.0 - time * 1.1)) * 0.004;
    if (mirror > 0.5)
        uv = abs(uv);
    uv.x /= aspect;
    uv += 0.5;

    float inside = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);
    vec3 col = texture2D(source, uv).rgb;
    col = max(hueRotate(col, hueShift), 0.0) * fade * inside;
    gl_FragColor = vec4(col, 1.0);
}
)";

// Finishing pass for Liquid and Oscilloscope: screen glow, scanlines, grain and vignette at full resolution.
constexpr const char* FinishShader = R"(
uniform sampler2D source;
uniform float time;
uniform float scanlines;
uniform float grain;
uniform vec3 base;
void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec3 col = texture2D(source, uv).rgb;
    vec2 v = uv - 0.5;
    float vig = smoothstep(0.85, 0.2, length(v * vec2(1.0, 1.15)));
    col += base * vig;
    col *= 1.0 - scanlines + scanlines * (0.5 + 0.5 * sin(gl_FragCoord.y * 2.1));
    col += (fract(sin(dot(gl_FragCoord.xy + fract(time) * 91.0, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) * grain;
    col *= vig;
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

// The reacting picture. Each mode runs its own heavy effect on it; `amount` ramps from 0 to 1
// over the first few seconds after the picture opens, so it melts from recognizable into chaos.
// audio: a 256x2 texture, row 0 = spectrum bands, row 1 = waveform.
constexpr const char* ImageFxShader = R"(
uniform sampler2D image;
uniform sampler2D audio;
uniform vec2 resolution;
uniform vec2 fitScale;     // centered screen coords (height units) -> image uv, whole image visible
uniform float style;       // 0 glitch, 1 kaleidoscope, 2 wave smear, 3 swirl, 4 tunnel
uniform float amount;
uniform float time;
uniform float flow;
uniform float bass;
uniform float beat;
uniform float energy;
uniform float treble;
uniform float brightness;

float hash1(float n) { return fract(sin(n * 12.9898) * 43758.5453); }
float bandAt(float x) { return texture2D(audio, vec2(x, 0.25)).r; }
float waveAt(float x) { return texture2D(audio, vec2(x, 0.75)).r * 2.0 - 1.0; }
vec2 mirrorWrap(vec2 uv) { return 1.0 - abs(1.0 - mod(uv, 2.0)); } // fill the screen with mirrored copies
vec3 picture(vec2 q)
{
    vec2 uv = 0.5 + vec2(q.x, -q.y) * fitScale;
    // Outside the picture, mirrored copies fill the screen: dimmed at first so the picture reads as
    // fitted, then full brightness once the effect takes over.
    float inside = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);
    return texture2D(image, mirrorWrap(uv)).rgb * mix(0.35, 1.0, max(inside, amount));
}
vec3 hueRotate(vec3 c, float a)
{
    vec3 k = vec3(0.57735);
    float ca = cos(a);
    return c * ca + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - ca);
}
vec3 thermal(float t)
{
    t = fract(t);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}
mat2 rot(float a) { float c = cos(a), s = sin(a); return mat2(c, s, -s, c); }

void main()
{
    vec2 q = (gl_FragCoord.xy - 0.5 * resolution) / resolution.y;   // centered, height units
    vec2 st = gl_FragCoord.xy / resolution;
    float a = amount;
    vec3 col;

    if (style < 0.5)
    {
        // GLITCH: torn horizontal slices driven by the spectrum, blocky pixels and RGB tearing.
        float rows = 30.0 + 90.0 * treble;
        float row = floor(st.y * rows);
        float tick = floor(time * (6.0 + 18.0 * energy));
        float torn = step(0.62 - 0.45 * energy - 0.3 * beat, hash1(row * 1.7 + tick));
        q.x += (hash1(row + tick) - 0.5) * (0.08 + 0.6 * beat) * torn * a;
        q.x += (bandAt(fract(row * 0.137)) - 0.3) * 0.12 * sin(row * 2.3 + time) * a;
        float cells = mix(400.0, 24.0 + 60.0 * (1.0 - beat), a * (0.35 + 0.65 * step(0.5, hash1(row + tick * 3.1))));
        q = floor(q * cells) / cells;
        float split = (0.006 + 0.05 * beat + 0.02 * bandAt(0.1)) * a;
        col = vec3(picture(q + vec2(split, 0.0)).r, picture(q).g, picture(q - vec2(split, split * 0.5)).b);
        float levels = mix(64.0, 3.0 + 4.0 * (1.0 - energy), a);
        col = floor(col * levels) / levels;
        if (hash1(row * 3.3 + tick) > 1.0 - 0.25 * beat * a)
            col = 1.0 - col;                                   // flashes of inverted rows on kicks
        col = hueRotate(col, a * (time * 0.4 + bandAt(st.x) * 3.0));
    }
    else if (style < 1.5)
    {
        // KALEIDOSCOPE: the picture folded into a spinning mandala.
        float r = length(q);
        float ang = atan(q.y, q.x) + time * 0.15 + beat * 0.4;
        float n = max(1.0, floor(mix(1.0, 8.0, a)));
        float seg = 3.14159265 / n;
        float folded = abs(mod(ang, 2.0 * seg) - seg);
        float useFold = step(0.01, a);
        ang = mix(atan(q.y, q.x), folded, useFold);
        r = r * (1.0 - 0.35 * bass * a) + flow * 0.04 * a;
        r = mix(r, abs(fract(r * 1.4) - 0.5) * 1.6, a * 0.6);   // rings of repeats
        vec2 kq = r * vec2(cos(ang), sin(ang));
        col = picture(kq);
        col = hueRotate(col, a * (time * 0.3 + r * 4.0));
        col = mix(col, col * col * 2.2, a * 0.5);               // punchier contrast
    }
    else if (style < 2.5)
    {
        // WAVE SMEAR: the waveform drags the picture sideways; colors become a heat map.
        float w = waveAt(st.y);
        q.x += w * (0.08 + 0.4 * energy) * a;
        q.y += waveAt(fract(st.x * 2.0)) * (0.03 + 0.12 * energy) * a;
        q *= 1.0 - 0.15 * bass * a;
        vec3 p = picture(q);
        float lum = dot(p, vec3(0.299, 0.587, 0.114));
        vec3 heat = thermal(lum * 1.4 + time * 0.12 + bandAt(st.x) * 0.5);
        col = mix(p, heat, a * 0.85);
        col *= 0.75 + 0.5 * bandAt(st.x) * a;
    }
    else if (style < 3.5)
    {
        // SWIRL: the picture twisted around the center and solarized.
        float r = length(q);
        float twist = (3.0 + 9.0 * bass + 4.0 * beat) * exp(-r * 1.8) * sin(time * 0.35 + 1.0) * a;
        q = rot(twist + time * 0.1 * a) * q;
        q *= 1.0 + 0.25 * sin(r * 8.0 - flow) * a * energy;
        vec3 p = picture(q);
        vec3 sol = abs(sin(p * 3.14159 * (1.2 + 1.6 * energy + beat)));
        col = mix(p, sol, a * 0.9);
        col = hueRotate(col, a * time * 0.5);
    }
    else
    {
        // TUNNEL: the picture wrapped around the inside of an endless tube.
        float r = max(length(q), 1e-3);
        float ang = atan(q.y, q.x) / 3.14159265;
        vec2 tq = vec2(ang * 0.9 + time * 0.05, 0.3 / r + flow * 0.35);
        vec2 tunnelQ = (tq - 0.5) / fitScale;
        tunnelQ.y = -tunnelQ.y;
        col = picture(mix(q, tunnelQ, a));
        float depth = clamp(r * 2.2, 0.0, 1.0);
        col *= mix(1.0, depth, a);                              // fog toward the vanishing point
        col = hueRotate(col, a * (0.3 / r) * 0.6);
    }

    vec2 v = st - 0.5;
    col *= brightness * smoothstep(0.95, 0.3, length(v * vec2(1.0, 1.1)));
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

// Post-processing filters, applied to the finished frame of any mode (and to exports).
constexpr const char* FilterNames[] = {"None",   "VHS",    "CRT",          "Film",  "Noir",       "Thermal", "Neon edges",
                                       "Pixel",  "Chromatic", "Kaleidoscope", "Mirror", "Dream", "Duotone", "Halftone"};
constexpr int FilterTotal = static_cast<int>(sizeof(FilterNames) / sizeof(FilterNames[0]));

constexpr const char* FilterShader = R"(
uniform sampler2D source;
uniform vec2 resolution;
uniform float style;
uniform float time;
uniform float beat;
uniform float bass;
uniform float energy;

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
vec3 tex(vec2 uv) { return texture2D(source, clamp(uv, 0.0, 1.0)).rgb; }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float vignette(vec2 uv, float strength) { vec2 v = uv - 0.5; return 1.0 - dot(v, v) * strength; }
vec3 thermal(float t)
{
    t = clamp(t, 0.0, 1.0);
    vec3 a = mix(vec3(0.0, 0.0, 0.2), vec3(0.4, 0.0, 0.6), smoothstep(0.0, 0.25, t));
    a = mix(a, vec3(0.9, 0.1, 0.2), smoothstep(0.25, 0.5, t));
    a = mix(a, vec3(1.0, 0.7, 0.0), smoothstep(0.5, 0.75, t));
    return mix(a, vec3(1.0, 1.0, 0.9), smoothstep(0.75, 1.0, t));
}

void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec2 px = 1.0 / resolution;
    vec3 col = tex(uv);
    float grain = hash(gl_FragCoord.xy + fract(time * 13.7) * 100.0) - 0.5;

    if (style < 0.5)
    {
        // none
    }
    else if (style < 1.5)
    {
        // VHS: wobbling tape, torn lines on kicks, bleeding color, a rolling tracking band, noise.
        float line = floor(gl_FragCoord.y / 2.0);
        float tear = (hash(vec2(line, floor(time * 24.0))) - 0.5) * (0.002 + 0.012 * beat);
        vec2 u = uv + vec2(tear + sin(uv.y * 9.0 + time * 2.3) * 0.0015, 0.0);
        float bleed = px.x * (3.0 + 6.0 * bass);
        col = vec3(tex(u + vec2(bleed, 0.0)).r, tex(u).g, tex(u - vec2(bleed, 0.0)).b);
        float band = smoothstep(0.96, 1.0, fract(uv.y * 0.7 - time * 0.12));
        col += band * (hash(gl_FragCoord.xy + time) * 0.35);
        col += grain * 0.09;
        col *= 0.92 + 0.08 * sin(gl_FragCoord.y * 1.6);
        col = mix(vec3(luma(col)), col, 0.85) * vec3(1.05, 0.97, 1.08);
    }
    else if (style < 2.5)
    {
        // CRT: curved glass, RGB phosphor mask, scanlines, glow at the edges fading out.
        vec2 c = uv * 2.0 - 1.0;
        c *= 1.0 + 0.07 * dot(c, c);
        vec2 u = c * 0.5 + 0.5;
        if (u.x < 0.0 || u.x > 1.0 || u.y < 0.0 || u.y > 1.0)
            col = vec3(0.0);
        else
        {
            col = vec3(tex(u + vec2(px.x, 0.0)).r, tex(u).g, tex(u - vec2(px.x, 0.0)).b);
            float scan = 0.72 + 0.28 * sin(u.y * resolution.y * 3.14159);
            float m = mod(gl_FragCoord.x, 3.0);
            vec3 mask = m < 1.0 ? vec3(1.15, 0.85, 0.85) : (m < 2.0 ? vec3(0.85, 1.15, 0.85) : vec3(0.85, 0.85, 1.15));
            col *= scan * mask * (1.2 + 0.2 * beat);
            col *= smoothstep(1.25, 0.65, length(c));
        }
    }
    else if (style < 3.5)
    {
        // Film: warm grade, lifted blacks, grain, gate flicker, dust specks.
        col = pow(col, vec3(0.95, 1.0, 1.12)) * vec3(1.08, 1.0, 0.88);
        col = col * 0.92 + 0.04;
        col += grain * 0.07;
        col *= 0.96 + 0.04 * hash(vec2(floor(time * 24.0), 3.0));
        float dust = step(0.9993, hash(floor(gl_FragCoord.xy / 3.0) + floor(time * 24.0)));
        col = mix(col, vec3(0.9, 0.85, 0.75), dust * 0.6);
        col *= vignette(uv, 1.3);
    }
    else if (style < 4.5)
    {
        // Noir: punchy black and white with grain.
        float l = smoothstep(0.04, 0.8, luma(col) * (1.0 + 0.3 * beat));
        col = vec3(l) + grain * 0.08;
        col *= vignette(uv, 1.6);
    }
    else if (style < 5.5)
    {
        // Thermal camera.
        col = thermal(luma(col) * 1.3 + 0.08 * bass);
        col += grain * 0.03;
    }
    else if (style < 6.5)
    {
        // Neon edges: only the outlines, glowing in shifting neon.
        float tl = luma(tex(uv + px * vec2(-1.0, -1.0))), t = luma(tex(uv + px * vec2(0.0, -1.0))), tr = luma(tex(uv + px * vec2(1.0, -1.0)));
        float l = luma(tex(uv + px * vec2(-1.0, 0.0))), r = luma(tex(uv + px * vec2(1.0, 0.0)));
        float bl = luma(tex(uv + px * vec2(-1.0, 1.0))), b = luma(tex(uv + px * vec2(0.0, 1.0))), br = luma(tex(uv + px * vec2(1.0, 1.0)));
        float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
        float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);
        float edge = clamp(length(vec2(gx, gy)) * (2.5 + 2.0 * energy), 0.0, 1.0);
        vec3 neon = 0.5 + 0.5 * cos(6.2831 * (uv.x * 0.6 + uv.y * 0.3 + time * 0.12 + vec3(0.0, 0.33, 0.67)));
        col = neon * edge * (1.2 + beat) + col * 0.12;
    }
    else if (style < 7.5)
    {
        // Pixel: chunky pixels that get bigger on kicks, and a limited palette.
        float size = 6.0 + 10.0 * beat + 4.0 * bass;
        vec2 cell = (floor(uv * resolution / size) + 0.5) * size / resolution;
        col = floor(tex(cell) * 5.0 + 0.5) / 5.0;
    }
    else if (style < 8.5)
    {
        // Chromatic: red and blue pull apart from the center on every beat.
        vec2 d = uv - 0.5;
        float amount = 0.004 + 0.035 * beat + 0.012 * bass;
        vec2 z = d * (1.0 - 0.02 * beat) + 0.5;
        col = vec3(tex(z + d * amount).r, tex(z).g, tex(z - d * amount).b);
    }
    else if (style < 9.5)
    {
        // Kaleidoscope: the whole frame folded into 6 mirrored wedges.
        float aspect = resolution.x / resolution.y;
        vec2 c = (uv - 0.5) * vec2(aspect, 1.0);
        float r = length(c);
        float seg = 3.14159265 / 6.0;
        float a = abs(mod(atan(c.y, c.x) + time * 0.08, 2.0 * seg) - seg);
        c = r * vec2(cos(a), sin(a));
        col = tex(c / vec2(aspect, 1.0) + 0.5);
    }
    else if (style < 10.5)
    {
        // Mirror: the left half reflected onto the right.
        col = tex(vec2(0.5 - abs(uv.x - 0.5), uv.y));
    }
    else if (style < 11.5)
    {
        // Dream: soft glow and pastel colors.
        vec3 blur = vec3(0.0);
        for (int i = 0; i < 8; ++i)
        {
            float a = float(i) * 0.785398;
            blur += tex(uv + vec2(cos(a), sin(a)) * px * (6.0 + 6.0 * bass));
        }
        blur /= 8.0;
        col = 1.0 - (1.0 - col) * (1.0 - blur * 0.8);    // screen blend
        col = mix(col, vec3(luma(col)), 0.25) * vec3(1.02, 0.96, 1.08) + 0.03;
    }
    else if (style < 12.5)
    {
        // Duotone: deep purple shadows, hot pink to cyan highlights.
        float l = luma(col);
        vec3 lo = vec3(0.12, 0.02, 0.25);
        vec3 hi = mix(vec3(1.0, 0.2, 0.6), vec3(0.2, 0.95, 1.0), 0.5 + 0.5 * sin(time * 0.3));
        col = mix(lo, hi, smoothstep(0.05, 0.85, l));
    }
    else
    {
        // Halftone: printed-dot look, dots swell with the beat.
        float size = 7.0;
        vec2 p = uv * resolution;
        vec2 q = mat2(0.7071, -0.7071, 0.7071, 0.7071) * p / size;
        vec2 cellCenter = mat2(0.7071, 0.7071, -0.7071, 0.7071) * ((floor(q) + 0.5) * size);
        vec3 c = tex(cellCenter / resolution);
        float radius = sqrt(luma(c)) * 0.62 * (1.0 + 0.25 * beat);
        float dist = length(fract(q) - 0.5);
        col = c * (1.0 - smoothstep(radius - 0.06, radius + 0.06, dist)) * 1.3;
    }
    gl_FragColor = vec4(max(col, 0.0), 1.0);
}
)";

sf::Color hsv(float h, float s, float v, float a = 1.f)
{
    h = h - std::floor(h);
    s = std::clamp(s, 0.f, 1.f);
    v = std::clamp(v, 0.f, 1.f);
    const float i = std::floor(h * 6.f);
    const float f = h * 6.f - i;
    const float p = v * (1.f - s);
    const float q = v * (1.f - f * s);
    const float t = v * (1.f - (1.f - f) * s);
    float r = 0.f, g = 0.f, b = 0.f;
    switch (static_cast<int>(i) % 6)
    {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        default: r = v, g = p, b = q; break;
    }
    auto to8 = [](float x) { return static_cast<std::uint8_t>(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
    return {to8(r), to8(g), to8(b), to8(a)};
}

sf::Color scaled(sf::Color c, float k)
{
    auto s = [k](std::uint8_t x) { return static_cast<std::uint8_t>(std::clamp(static_cast<float>(x) * k, 0.f, 255.f)); };
    return {s(c.r), s(c.g), s(c.b), c.a};
}

sf::Color withAlpha(sf::Color c, float alpha)
{
    c.a = static_cast<std::uint8_t>(std::clamp(alpha, 0.f, 1.f) * 255.f);
    return c;
}

void quad(sf::VertexArray& out,
          sf::Vector2f a, sf::Vector2f b, sf::Vector2f c, sf::Vector2f d,
          sf::Color ca, sf::Color cb, sf::Color cc, sf::Color cd)
{
    out.append({a, ca});
    out.append({b, cb});
    out.append({c, cc});
    out.append({a, ca});
    out.append({c, cc});
    out.append({d, cd});
}

void rect(sf::VertexArray& out, float x0, float y0, float x1, float y1, sf::Color top, sf::Color bottom)
{
    quad(out, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, top, top, bottom, bottom);
}

// A line with real thickness (SFML's own lines are always 1px).
void thickLine(sf::VertexArray& out, const std::vector<sf::Vector2f>& pts, const std::vector<sf::Color>& colors,
               float thickness, bool closed)
{
    const std::size_t n = pts.size();
    if (n < 2)
        return;

    auto normalAt = [&](std::size_t i)
    {
        const sf::Vector2f prev = (i == 0) ? (closed ? pts[n - 1] : pts[0]) : pts[i - 1];
        const sf::Vector2f next = (i == n - 1) ? (closed ? pts[0] : pts[n - 1]) : pts[i + 1];
        sf::Vector2f d = next - prev;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1e-4f)
            return sf::Vector2f{0.f, 1.f};
        d /= len;
        return sf::Vector2f{-d.y, d.x};
    };

    const float half = thickness * 0.5f;
    const std::size_t segments = closed ? n : n - 1;
    for (std::size_t s = 0; s < segments; ++s)
    {
        const std::size_t i = s;
        const std::size_t j = (s + 1) % n;
        const sf::Vector2f ni = normalAt(i) * half;
        const sf::Vector2f nj = normalAt(j) * half;
        quad(out, pts[i] + ni, pts[j] + nj, pts[j] - nj, pts[i] - ni, colors[i], colors[j], colors[j], colors[i]);
    }
}

// Stretch or squeeze a list of values to n entries (for adjustable bar counts).
std::vector<float> resample(const std::vector<float>& src, std::size_t n)
{
    std::vector<float> out(n);
    if (src.empty() || n == 0)
        return out;
    for (std::size_t i = 0; i < n; ++i)
    {
        const float pos = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) * static_cast<float>(src.size() - 1) : 0.f;
        const auto i0 = static_cast<std::size_t>(pos);
        const std::size_t i1 = std::min(i0 + 1, src.size() - 1);
        const float f = pos - static_cast<float>(i0);
        out[i] = src[i0] * (1.f - f) + src[i1] * f;
    }
    return out;
}

std::string formatTime(float seconds)
{
    const int total = std::max(0, static_cast<int>(seconds));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}
} // namespace

const char* modeName(Mode mode)
{
    switch (mode)
    {
        case Mode::Bars: return "Bars";
        case Mode::Ring: return "Ring";
        case Mode::Scope: return "Scope";
        case Mode::XYScope: return "Lissajous";
        case Mode::Tunnel: return "Tunnel";
        case Mode::Liquid: return "Liquid";
        case Mode::Oscilloscope: return "Oscilloscope";
        case Mode::DarkFantasy: return "Dark Fantasy";
        case Mode::Pop: return "Pop";
        case Mode::NightDrive: return "Night Drive";
        case Mode::DarkForest: return "Dark Forest";
        case Mode::Hijack: return "Hijack";
        case Mode::Rage: return "Rage";
        case Mode::Dystopia: return "Dystopia";
        default: return "";
    }
}

float Renderer::cfg(const char* key) const
{
    if (m_settings)
        return m_settings->get(key);
    static const Settings defaults;
    return defaults.get(key);
}

int Renderer::filterCount() { return FilterTotal; }
const char* Renderer::filterName(int index) { return FilterNames[((index % FilterTotal) + FilterTotal) % FilterTotal]; }

Renderer::Renderer()
{
    m_particles.reserve(800);
}

void Renderer::compileShaders(const std::function<void(float, const char*)>& progress)
{
    // Compiling shaders is what takes a moment at startup, so report progress after each one.
    struct Job
    {
        sf::Shader* shader;
        std::string source;
        const char* label;
        int group; // 0 bloom, 1 modes, 2 filters
    };
    const std::vector<Job> jobs = {
        {&m_blur, BlurShader, "Glow", 0},
        {&m_finish, FinishShader, "Screen finish", 1},
        {&m_feedbackShader, std::string(ShaderCommon) + FeedbackShader, "Tunnel", 1},
        {&m_liquid, std::string(ShaderCommon) + LiquidShader, "Liquid", 1},
        {&m_imageShader, ImageFxShader, "Picture effects", 1},
        {&m_pop, std::string(ShaderCommon) + PopShader, "Pop", 1},
        {&m_fantasy, std::string(ShaderCommon) + FantasyCommon + FantasyShader, "Dark Fantasy", 1},
        {&m_fantasy3D, std::string(ShaderCommon) + FantasyCommon + Fantasy3DShader, "Dark Fantasy village", 1},
        {&m_skyMask, SkyMaskShader, "Sky", 1},
        {&m_drive, DriveShader, "Night Drive", 1},
        {&m_forest, ForestShader, "Dark Forest", 1},
        {&m_hijackBg, std::string(ShaderCommon) + HijackShader, "Hijack", 1},
        {&m_hijackGlitch, HijackGlitchShader, "Hijack glitch", 1},
        {&m_rage, std::string(ShaderCommon) + RageShader, "Rage", 1},
        {&m_dystopia, std::string(ShaderCommon) + DystopiaShader, "Dystopia", 1},
        {&m_filterShader, FilterShader, "Filters", 2},
    };
    const bool available = sf::Shader::isAvailable();
    bool ok[3] = {available, available, available};
    for (std::size_t i = 0; i < jobs.size(); ++i)
    {
        if (progress)
            progress(static_cast<float>(i) / static_cast<float>(jobs.size()), jobs[i].label);
        if (available && !jobs[i].shader->loadFromMemory(jobs[i].source, sf::Shader::Type::Fragment))
            ok[jobs[i].group] = false;
    }
    if (progress)
        progress(1.f, "Ready");
    m_bloomReady = ok[0];
    m_trippyReady = ok[1];
    m_filterReady = ok[2];
}

void Renderer::resize(sf::Vector2u size)
{
    size.x = std::max(size.x, 1u);
    size.y = std::max(size.y, 1u);
    m_size = {static_cast<float>(size.x), static_cast<float>(size.y)};

    const sf::Vector2u half{std::max(size.x / 2, 1u), std::max(size.y / 2, 1u)};
    sf::ContextSettings smoothEdges;
    smoothEdges.antiAliasingLevel = 4;
    const bool ok = m_scene.resize(size, smoothEdges) && m_blurA.resize(half) && m_blurB.resize(half);
    if (!ok)
        m_bloomReady = false;
    m_scene.setSmooth(true);
    if (!m_post.resize(size))
        m_filterReady = false;
    m_post.setSmooth(true);

    for (auto& feed : m_feed)
    {
        if (!feed.resize(size))
            m_trippyReady = false;
        feed.setSmooth(true);
        feed.clear(sf::Color::Black);
        feed.display();
    }
    m_blurA.setSmooth(true);
    m_blurB.setSmooth(true);

    // Liquid is smooth by nature, so it renders at up to ~0.5 megapixels and is scaled up.
    // That keeps it fast in fullscreen on any screen, even 4K.
    const float pixels = m_size.x * m_size.y;
    m_liquidScale = std::min(1.f, std::sqrt(500000.f / pixels));
    const sf::Vector2u liquidSize{std::max(1u, static_cast<unsigned>(m_size.x * m_liquidScale)),
                                  std::max(1u, static_cast<unsigned>(m_size.y * m_liquidScale))};
    {
        const float scale = std::min(1.f, std::sqrt(2100000.f / pixels));
        const sf::Vector2u fantasySize{std::max(1u, static_cast<unsigned>(m_size.x * scale)),
                                       std::max(1u, static_cast<unsigned>(m_size.y * scale))};
        if (!m_fantasyTarget.resize(fantasySize))
            m_trippyReady = false;
        m_fantasyTarget.setSmooth(true);
        // The 3D street is ray-marched, so it renders at up to ~0.6 megapixels and is scaled up.
        const float scale3D = std::min(1.f, std::sqrt(450000.f / pixels));
        const sf::Vector2u size3D{std::max(1u, static_cast<unsigned>(m_size.x * scale3D)),
                                  std::max(1u, static_cast<unsigned>(m_size.y * scale3D))};
        if (!m_fantasy3DTarget.resize(size3D))
            m_trippyReady = false;
        m_fantasy3DTarget.setSmooth(true);
        if (!m_overlayLayer.resize(size))
            m_trippyReady = false;
        m_overlayLayer.setSmooth(true);
    }
    for (auto& target : m_liquidTargets)
    {
        if (!target.resize(liquidSize))
            m_trippyReady = false;
        target.setSmooth(true);
        target.clear(sf::Color::Black);
        target.display();
    }
}

sf::Color Renderer::bandColor(float t, float brightness, float saturation) const
{
    return hsv(m_hue + t * 0.45f, saturation, brightness);
}

bool Renderer::setImage(const std::filesystem::path& path)
{
    sf::Image picture;
    if (!picture.loadFromFile(path))
        return false;
    sf::Texture texture;
    if (!texture.loadFromImage(picture))
        return false; // e.g. bigger than the graphics card allows
    texture.setSmooth(true);
    (void)texture.generateMipmap(); // keeps big photos clean when drawn small
    m_image = std::move(texture);
    m_hasImage = true;
    m_imageAge = 0.f;
    buildOutline(picture);
    return true;
}

float Renderer::imageAmount() const
{
    // Recognizable for a moment, then it melts into the effect over about three seconds.
    const float t = std::clamp((m_imageAge - 0.4f) / 3.f, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

sf::Glsl::Vec2 Renderer::imageFitScale() const
{
    // "Contain" fit: the whole picture is visible, centered (coordinates are in screen-height units).
    const sf::Vector2f imageSize(m_image.getSize());
    const float screenAspect = m_size.x / m_size.y;
    const float imageAspect = imageSize.x / std::max(imageSize.y, 1.f);
    const sf::Vector2f shown = imageAspect > screenAspect ? sf::Vector2f{screenAspect, screenAspect / imageAspect}
                                                          : sf::Vector2f{imageAspect, 1.f};
    return {1.f / shown.x, 1.f / shown.y};
}

void Renderer::updateAudioTexture(const Spectrum& spectrum)
{
    constexpr unsigned Width = 256;
    if (m_audioTexture.getSize().x != Width)
    {
        if (!m_audioTexture.resize({Width, 2}))
            return;
        m_audioTexture.setSmooth(true);
    }
    std::uint8_t pixels[Width * 2 * 4];
    const auto& bands = spectrum.bands();
    const auto& wave = spectrum.waveform();
    for (unsigned x = 0; x < Width; ++x)
    {
        const float b = bands[x * bands.size() / Width];
        const float w = wave[x * wave.size() / Width];
        const auto bv = static_cast<std::uint8_t>(std::clamp(b, 0.f, 1.f) * 255.f);
        const auto wv = static_cast<std::uint8_t>(std::clamp(0.5f + 0.5f * w * 1.5f, 0.f, 1.f) * 255.f);
        std::uint8_t* row0 = pixels + x * 4;
        std::uint8_t* row1 = pixels + (Width + x) * 4;
        row0[0] = row0[1] = row0[2] = bv;
        row1[0] = row1[1] = row1[2] = wv;
        row0[3] = row1[3] = 255;
    }
    m_audioTexture.update(pixels);
}

void Renderer::buildOutline(const sf::Image& picture)
{
    // Turn the picture into line drawings the oscilloscope beam can trace:
    // shrink it, find edges (Sobel), thin them, then walk along them to make strokes.
    m_outline.clear();
    const sf::Vector2u src = picture.getSize();
    if (src.x < 4 || src.y < 4)
        return;
    const float shrink = std::max(1.f, static_cast<float>(std::max(src.x, src.y)) / 220.f);
    const int w = std::max(8, static_cast<int>(static_cast<float>(src.x) / shrink));
    const int h = std::max(8, static_cast<int>(static_cast<float>(src.y) / shrink));

    std::vector<float> gray(static_cast<std::size_t>(w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            // Average a few source pixels per cell so detail doesn't alias.
            float sum = 0.f;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx)
                {
                    const auto px = std::min(src.x - 1, static_cast<unsigned>((static_cast<float>(x) + 0.25f + 0.5f * sx) * shrink));
                    const auto py = std::min(src.y - 1, static_cast<unsigned>((static_cast<float>(y) + 0.25f + 0.5f * sy) * shrink));
                    const sf::Color c = picture.getPixel({px, py});
                    sum += (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.f * (static_cast<float>(c.a) / 255.f);
                }
            gray[static_cast<std::size_t>(y * w + x)] = sum * 0.25f;
        }

    auto at = [&](const std::vector<float>& img, int x, int y)
    { return img[static_cast<std::size_t>(std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1))]; };

    // Light blur, then edge strength and direction.
    std::vector<float> blurred(gray.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            blurred[static_cast<std::size_t>(y * w + x)] =
                (at(gray, x - 1, y - 1) + 2 * at(gray, x, y - 1) + at(gray, x + 1, y - 1) + 2 * at(gray, x - 1, y) +
                 4 * at(gray, x, y) + 2 * at(gray, x + 1, y) + at(gray, x - 1, y + 1) + 2 * at(gray, x, y + 1) +
                 at(gray, x + 1, y + 1)) / 16.f;

    std::vector<float> mag(gray.size()), gx(gray.size()), gy(gray.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float dx = (at(blurred, x + 1, y - 1) + 2 * at(blurred, x + 1, y) + at(blurred, x + 1, y + 1)) -
                             (at(blurred, x - 1, y - 1) + 2 * at(blurred, x - 1, y) + at(blurred, x - 1, y + 1));
            const float dy = (at(blurred, x - 1, y + 1) + 2 * at(blurred, x, y + 1) + at(blurred, x + 1, y + 1)) -
                             (at(blurred, x - 1, y - 1) + 2 * at(blurred, x, y - 1) + at(blurred, x + 1, y - 1));
            const auto i = static_cast<std::size_t>(y * w + x);
            gx[i] = dx;
            gy[i] = dy;
            mag[i] = std::sqrt(dx * dx + dy * dy);
        }

    // Keep the strongest ~12% of edges.
    std::vector<float> sorted(mag);
    const auto cut = sorted.begin() + static_cast<long>(sorted.size() * 88 / 100);
    std::nth_element(sorted.begin(), cut, sorted.end());
    const float threshold = std::max(*cut, 0.08f);

    // Thin edges to one pixel by keeping only the peak across the edge (non-maximum suppression).
    std::vector<std::uint8_t> edge(gray.size(), 0);
    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x)
        {
            const auto i = static_cast<std::size_t>(y * w + x);
            if (mag[i] < threshold)
                continue;
            const float len = mag[i] + 1e-6f;
            const int ox = static_cast<int>(std::round(gx[i] / len));
            const int oy = static_cast<int>(std::round(gy[i] / len));
            if (mag[i] >= at(mag, x + ox, y + oy) && mag[i] >= at(mag, x - ox, y - oy))
                edge[i] = 1;
        }

    // Walk along connected edge pixels to build strokes.
    const float half = static_cast<float>(std::max(w, h)) * 0.5f;
    std::size_t totalPoints = 0;
    std::vector<std::vector<sf::Vector2f>> strokes;
    for (int y0 = 0; y0 < h; ++y0)
        for (int x0 = 0; x0 < w; ++x0)
        {
            if (!edge[static_cast<std::size_t>(y0 * w + x0)])
                continue;
            std::vector<sf::Vector2f> stroke;
            int x = x0, y = y0, lastDx = 1, lastDy = 0;
            while (true)
            {
                edge[static_cast<std::size_t>(y * w + x)] = 0;
                stroke.push_back({(static_cast<float>(x) - static_cast<float>(w) * 0.5f) / half,
                                  (static_cast<float>(y) - static_cast<float>(h) * 0.5f) / half});
                // Prefer continuing in the same direction for smooth strokes.
                int bestX = -1, bestY = -1, bestScore = -100;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (!dx && !dy)
                            continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || !edge[static_cast<std::size_t>(ny * w + nx)])
                            continue;
                        const int score = dx * lastDx + dy * lastDy;
                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestX = nx;
                            bestY = ny;
                        }
                    }
                if (bestX < 0)
                    break;
                lastDx = bestX - x;
                lastDy = bestY - y;
                x = bestX;
                y = bestY;
            }
            if (stroke.size() < 6)
                continue;
            // Smooth out pixel stair-steps so the laser draws curves.
            for (int pass = 0; pass < 2; ++pass)
                for (std::size_t k = 1; k + 1 < stroke.size(); ++k)
                    stroke[k] = (stroke[k - 1] + stroke[k] * 2.f + stroke[k + 1]) * 0.25f;
            std::vector<sf::Vector2f> thin;
            for (std::size_t k = 0; k < stroke.size(); k += 2)
                thin.push_back(stroke[k]);
            thin.push_back(stroke.back());
            totalPoints += thin.size();
            strokes.push_back(std::move(thin));
        }

    // Too busy for the beam? Keep the longest strokes.
    std::sort(strokes.begin(), strokes.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    std::size_t kept = 0;
    for (auto& s : strokes)
    {
        if (kept + s.size() > 7000)
            break;
        kept += s.size();
        m_outline.push_back(std::move(s));
    }
    (void)totalPoints;
}

void Renderer::drawImageBackground(sf::RenderTarget& target, const Spectrum& spectrum, Mode mode)
{
    float style = 0.f;
    switch (mode)
    {
        case Mode::Bars: style = 0.f; break;
        case Mode::Ring: style = 1.f; break;
        case Mode::Scope: style = 2.f; break;
        case Mode::XYScope: style = 3.f; break;
        case Mode::Tunnel: style = 4.f; break;
        default: break;
    }
    const float beat = spectrum.beat();
    const bool busyMode = mode == Mode::Tunnel || mode == Mode::XYScope;
    m_imageShader.setUniform("image", m_image);
    m_imageShader.setUniform("audio", m_audioTexture);
    m_imageShader.setUniform("resolution", sf::Glsl::Vec2{m_size.x, m_size.y});
    m_imageShader.setUniform("fitScale", imageFitScale());
    m_imageShader.setUniform("style", style);
    m_imageShader.setUniform("amount", imageAmount());
    m_imageShader.setUniform("time", m_time);
    m_imageShader.setUniform("flow", m_flow);
    m_imageShader.setUniform("bass", spectrum.bass());
    m_imageShader.setUniform("beat", beat);
    m_imageShader.setUniform("energy", spectrum.energy());
    m_imageShader.setUniform("treble", spectrum.treble());
    m_imageShader.setUniform("brightness", (0.55f + beat * 0.35f + spectrum.energy() * 0.15f) * (busyMode ? 0.7f : 1.f));
    sf::RectangleShape screen(m_size);
    target.draw(screen, sf::RenderStates(&m_imageShader));
}

void Renderer::draw(sf::RenderTarget& window, const Spectrum& spectrum, Mode mode, float time, float dt, const UiState& ui)
{
    m_time = time;
    m_hue = 0.62f + time * 0.012f; // a slow drift through the color wheel
    m_ringAngle += dt * cfg("ring.spin") * 0.6f;
    m_ringSpin = 0.12f * std::sin(time * 0.35f) + m_ringAngle; // gentle sway plus optional spin

    const bool shaderMode = mode == Mode::XYScope || mode == Mode::Tunnel || mode == Mode::Liquid ||
                            mode == Mode::Oscilloscope || mode == Mode::DarkFantasy || mode == Mode::Pop ||
                            mode == Mode::NightDrive || mode == Mode::DarkForest || mode == Mode::Hijack || mode == Mode::Rage || mode == Mode::Dystopia;
    if (shaderMode && !m_trippyReady)
        mode = Mode::Bars; // graphics card can't run the shaders

    if (mode != m_lastMode)
    {
        for (auto& feed : m_feed)
        {
            feed.clear(sf::Color::Black);
            feed.display();
        }
        m_lastMode = mode;
    }
    m_flow += dt * (0.35f + spectrum.energy() * 1.6f + spectrum.beat() * 2.f) * (mode == Mode::Liquid ? cfg("liquid.speed") : 1.f);
    if (m_hasImage)
        m_imageAge += dt;
    if (m_hasImage || mode == Mode::DarkFantasy || mode == Mode::NightDrive || mode == Mode::Hijack || mode == Mode::Rage || mode == Mode::Dystopia)
        updateAudioTexture(spectrum);

    // 1) The glowing part goes into an offscreen texture, drawn additively.
    m_scene.clear(sf::Color::Black);
    float bloom = 0.85f + spectrum.beat() * 0.9f;
    switch (mode)
    {
        case Mode::Bars:
        case Mode::Ring:
        case Mode::Scope:
        {
            sf::VertexArray verts(sf::PrimitiveType::Triangles);
            if (mode == Mode::Bars)
                drawBars(verts, spectrum);
            else if (mode == Mode::Ring)
                drawRing(verts, spectrum);
            else
                drawScope(verts, spectrum);
            updateParticles(verts, spectrum, mode, dt);
            m_scene.draw(verts, sf::RenderStates(sf::BlendAdd));
            break;
        }
        case Mode::Dystopia:
            drawDystopia(spectrum, dt);
            bloom = 0.8f + spectrum.beat() * 0.35f + m_dysFlash * 0.3f;
            break;
        case Mode::Rage:
            drawRage(spectrum, dt);
            bloom = 0.7f + spectrum.beat() * 0.5f + m_rageStrobe * 0.4f;
            break;
        case Mode::Hijack:
            drawHijack(spectrum, dt, ui);
            bloom = 0.3f + spectrum.beat() * 0.3f;
            break;
        case Mode::DarkForest:
            drawDarkForest(spectrum, dt);
            bloom = 0.35f + spectrum.beat() * 0.15f;
            break;
        case Mode::NightDrive:
            drawNightDrive(spectrum, dt, ui);
            bloom = 0.75f + spectrum.beat() * 0.35f;
            break;
        case Mode::Pop:
            drawPop(spectrum, dt);
            bloom = 0.f; // crisp cartoon, no glow
            break;
        case Mode::DarkFantasy:
            drawDarkFantasy(spectrum, dt);
            bloom = 0.45f + spectrum.beat() * 0.25f + m_flash * 0.12f;
            break;
        case Mode::Liquid:
            drawLiquid(spectrum);
            bloom = 0.15f + spectrum.beat() * 0.25f;
            break;
        case Mode::Oscilloscope:
        {
            drawFeedback(spectrum, mode, dt);
            sf::VertexArray grid(sf::PrimitiveType::Triangles);
            if (cfg("osc.grid") > 0.5f)
                drawGraticule(grid);
            m_scene.draw(grid, sf::RenderStates(sf::BlendAdd));
            bloom = 0.55f + spectrum.beat() * 0.2f;
            break;
        }
        default:
            drawFeedback(spectrum, mode, dt);
            bloom = (mode == Mode::XYScope ? 0.5f : 0.35f) + spectrum.beat() * 0.5f;
            break;
    }
    m_scene.display();

    // 2) Compose: background, the scene, then its bloom on top. With a filter on, compose into
    //    an offscreen frame first so the filter can process the finished picture.
    const bool filtering = m_filter > 0 && m_filterReady;
    if (filtering)
        m_post.clear(sf::Color::Black);
    sf::RenderTarget& out = filtering ? static_cast<sf::RenderTarget&>(m_post) : window;
    sf::Sprite sceneSprite(m_scene.getTexture());
    if (mode == Mode::Liquid || mode == Mode::Oscilloscope || mode == Mode::DarkFantasy || mode == Mode::Pop ||
        mode == Mode::NightDrive || mode == Mode::DarkForest || mode == Mode::Hijack || mode == Mode::Rage || mode == Mode::Dystopia)
    {
        // These two own the whole screen and get a CRT-style finish.
        const bool scope = mode == Mode::Oscilloscope;
        m_finish.setUniform("source", sf::Shader::CurrentTexture);
        m_finish.setUniform("time", m_time);
        m_finish.setUniform("scanlines", scope ? 0.14f : (mode == Mode::DarkFantasy || mode == Mode::Pop || mode == Mode::NightDrive || mode == Mode::DarkForest || mode == Mode::Rage || mode == Mode::Dystopia ? 0.f : (mode == Mode::Hijack ? 0.05f : 0.08f)));
        m_finish.setUniform("grain", mode == Mode::Pop ? 0.f : (scope ? 0.02f : 0.025f));
        m_finish.setUniform("base", scope ? sf::Glsl::Vec3{0.f, 0.035f, 0.014f} : sf::Glsl::Vec3{0.f, 0.f, 0.f});
        out.draw(sceneSprite, sf::RenderStates(&m_finish));
    }
    else
    {
        if (m_hasImage)
            drawImageBackground(out, spectrum, mode);
        else
            drawBackground(out, spectrum);
        out.draw(sceneSprite, sf::RenderStates(sf::BlendAdd));
    }
    applyBloom(out, bloom * cfg("global.glow"));

    if (filtering)
    {
        m_post.display();
        m_filterShader.setUniform("source", sf::Shader::CurrentTexture);
        m_filterShader.setUniform("resolution", sf::Glsl::Vec2{m_size.x, m_size.y});
        m_filterShader.setUniform("style", static_cast<float>(m_filter));
        m_filterShader.setUniform("time", m_time);
        m_filterShader.setUniform("beat", spectrum.beat());
        m_filterShader.setUniform("bass", spectrum.bass());
        m_filterShader.setUniform("energy", spectrum.energy());
        window.draw(sf::Sprite(m_post.getTexture()), sf::RenderStates(&m_filterShader));
    }

    // 3) Text and controls.
    drawOverlay(window, ui, mode);
}

void Renderer::drawBackground(sf::RenderTarget& target, const Spectrum& spectrum)
{
    const float pulse = spectrum.beat() * 0.05f + spectrum.energy() * 0.04f;
    const sf::Color top = hsv(m_hue + 0.05f, 0.65f, 0.09f + pulse);
    const sf::Color bottom = hsv(m_hue - 0.05f, 0.75f, 0.02f);
    sf::VertexArray bg(sf::PrimitiveType::Triangles);
    rect(bg, 0.f, 0.f, m_size.x, m_size.y, top, bottom);
    target.draw(bg);
}

void Renderer::drawBars(sf::VertexArray& out, const Spectrum& spectrum)
{
    const auto n = static_cast<std::size_t>(cfg("bars.count"));
    const std::vector<float> bands = resample(spectrum.bands(), n);
    const std::vector<float> peaks = resample(spectrum.peaks(), n);
    const bool showPeaks = cfg("bars.peaks") > 0.5f;
    const bool showReflection = cfg("bars.reflection") > 0.5f;

    const float left = m_size.x * 0.06f;
    const float right = m_size.x * 0.94f;
    const float floorY = m_size.y * 0.66f;
    const float maxHeight = m_size.y * 0.52f * cfg("bars.height");
    const float slot = (right - left) / static_cast<float>(n);
    const float barWidth = std::max(1.f, slot * cfg("bars.width"));

    for (std::size_t i = 0; i < n; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        const float v = bands[i];
        const float x0 = left + static_cast<float>(i) * slot + (slot - barWidth) * 0.5f;
        const float x1 = x0 + barWidth;
        const float top = floorY - std::max(2.f, v * maxHeight);

        const sf::Color cTop = bandColor(t, 0.55f + 0.45f * v, 0.8f);
        const sf::Color cBottom = bandColor(t, 0.16f, 0.95f);
        rect(out, x0, top, x1, floorY, cTop, cBottom);

        // Reflection on the "floor".
        if (showReflection)
        {
            const float reflection = (floorY - top) * 0.42f;
            rect(out, x0, floorY + 3.f, x1, floorY + 3.f + reflection, scaled(cBottom, 1.4f), sf::Color::Black);
        }

        // Peak cap.
        if (showPeaks)
        {
            const float py = floorY - peaks[i] * maxHeight - 7.f;
            rect(out, x0, py, x1, py + 3.f, bandColor(t, 1.f, 0.35f), bandColor(t, 0.9f, 0.45f));
        }
    }

    // Faint floor line.
    const sf::Color line = hsv(m_hue, 0.5f, 0.25f + spectrum.beat() * 0.3f);
    rect(out, left, floorY + 1.f, right, floorY + 2.f, line, line);
}

void Renderer::drawRing(sf::VertexArray& out, const Spectrum& spectrum)
{
    const auto n = static_cast<std::size_t>(cfg("ring.count"));
    const std::vector<float> bands = resample(spectrum.bands(), n);
    const std::vector<float> peaks = resample(spectrum.peaks(), n);

    const sf::Vector2f center = m_size * 0.5f;
    const float unit = std::min(m_size.x, m_size.y);
    const float base = unit * 0.17f * cfg("ring.size") * (1.f + 0.10f * spectrum.beat());
    const float maxLength = unit * 0.25f * cfg("ring.length");
    const float halfWidth = (Pi * base / static_cast<float>(n)) * 0.42f;

    // Bass at the top, mirrored left and right so it's symmetric.
    for (int side : {-1, 1})
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float v = bands[i];
            const float angle = -Pi * 0.5f + static_cast<float>(side) * ((static_cast<float>(i) + 0.5f) / static_cast<float>(n)) * Pi + m_ringSpin;
            const sf::Vector2f dir{std::cos(angle), std::sin(angle)};
            const sf::Vector2f perp{-dir.y, dir.x};

            const float r0 = base;
            const float r1 = base + 4.f + v * maxLength;
            const sf::Color cIn = bandColor(t, 0.25f, 0.95f);
            const sf::Color cOut = bandColor(t, 0.6f + 0.4f * v, 0.8f);
            quad(out,
                 center + dir * r0 + perp * halfWidth,
                 center + dir * r1 + perp * (halfWidth * 1.2f),
                 center + dir * r1 - perp * (halfWidth * 1.2f),
                 center + dir * r0 - perp * halfWidth,
                 cIn, cOut, cOut, cIn);

            // Peak dot.
            const float rp = base + 10.f + peaks[i] * maxLength;
            const float s = halfWidth * 0.9f;
            const sf::Vector2f p = center + dir * rp;
            const sf::Color cp = bandColor(t, 1.f, 0.35f);
            quad(out, p + perp * s + dir * s, p - perp * s + dir * s, p - perp * s - dir * s, p + perp * s - dir * s, cp, cp, cp, cp);
        }
    }

    // The raw waveform wrapped into a circle inside the ring.
    if (cfg("ring.wave") < 0.5f)
        return;
    const auto& wave = spectrum.waveform();
    constexpr std::size_t points = 200;
    std::vector<sf::Vector2f> pts(points);
    std::vector<sf::Color> colors(points);
    for (std::size_t k = 0; k < points; ++k)
    {
        const float a = static_cast<float>(k) / static_cast<float>(points) * 2.f * Pi - Pi * 0.5f;
        const float sample = std::clamp(wave[k * wave.size() / points] * 1.5f, -1.f, 1.f);
        const float r = base * 0.78f + sample * base * 0.3f;
        pts[k] = center + sf::Vector2f{std::cos(a), std::sin(a)} * r;
        colors[k] = bandColor(0.15f + 0.3f * std::abs(std::sin(a)), 0.9f, 0.55f);
    }
    thickLine(out, pts, colors, 2.5f, true);
}

void Renderer::drawScope(sf::VertexArray& out, const Spectrum& spectrum)
{
    // ---- Spectrum silhouette along the bottom, as a smooth curve ----
    std::vector<float> bands = spectrum.bands();
    const std::size_t n = bands.size();
    for (int pass = 0; pass < 2; ++pass) // soften neighbouring bars so the outline flows
    {
        std::vector<float> soft(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const float a = bands[i == 0 ? 0 : i - 1];
            const float b = bands[std::min(i + 1, n - 1)];
            soft[i] = (a + 2.f * bands[i] + b) * 0.25f;
        }
        bands.swap(soft);
    }

    const float baseY = m_size.y;
    const float height = m_size.y * 0.32f * cfg("scope.silhouetteHeight");
    const bool silhouette = cfg("scope.silhouette") > 0.5f;
    constexpr int Sub = 6; // curve points between each pair of bands
    std::vector<sf::Vector2f> edge;
    std::vector<sf::Color> edgeColors;
    edge.reserve(n * Sub);
    auto bandAt = [&](long i) { return bands[static_cast<std::size_t>(std::clamp<long>(i, 0, static_cast<long>(n) - 1))]; };
    for (std::size_t i = 0; i + 1 < n; ++i)
    {
        const long li = static_cast<long>(i);
        const float p0 = bandAt(li - 1), p1 = bandAt(li), p2 = bandAt(li + 1), p3 = bandAt(li + 2);
        for (int s = 0; s < Sub; ++s)
        {
            // Catmull-Rom spline through the band values.
            const float u = static_cast<float>(s) / Sub;
            const float v = 0.5f * ((2.f * p1) + (-p0 + p2) * u + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * u * u +
                                    (-p0 + 3.f * p1 - 3.f * p2 + p3) * u * u * u);
            const float t = (static_cast<float>(i) + u) / static_cast<float>(n - 1);
            edge.push_back({t * m_size.x, baseY - std::clamp(v, 0.f, 1.f) * height});
            edgeColors.push_back(bandColor(t, 0.8f, 0.6f));
        }
    }
    edge.push_back({m_size.x, baseY - bands.back() * height});
    edgeColors.push_back(bandColor(1.f, 0.8f, 0.6f));

    for (std::size_t i = 0; silhouette && i + 1 < edge.size(); ++i)
    {
        const float t0 = edge[i].x / m_size.x;
        const float t1 = edge[i + 1].x / m_size.x;
        quad(out, edge[i], edge[i + 1], {edge[i + 1].x, baseY}, {edge[i].x, baseY},
             bandColor(t0, 0.4f, 0.85f), bandColor(t1, 0.4f, 0.85f), sf::Color::Black, sf::Color::Black);
    }
    if (silhouette)
        thickLine(out, edge, edgeColors, 2.f, false);

    // ---- Oscilloscope line ----
    const auto& wave = spectrum.waveform();
    constexpr std::size_t Window = 640;
    if (wave.size() <= Window + 2)
        return;

    // Lock onto a rising zero crossing so the wave stands still instead of jittering.
    const std::size_t searchEnd = wave.size() - Window;
    const std::size_t preferred = searchEnd / 2;
    std::size_t start = preferred;
    std::size_t bestDistance = searchEnd;
    for (std::size_t i = 1; i < searchEnd; ++i)
    {
        if (wave[i - 1] < 0.f && wave[i] >= 0.f)
        {
            const std::size_t d = i > preferred ? i - preferred : preferred - i;
            if (d < bestDistance)
            {
                bestDistance = d;
                start = i;
            }
        }
    }

    // Zero-phase smoothing (forward + backward) takes off the hiss without shifting the wave.
    std::vector<float> s(wave.begin() + static_cast<long>(start), wave.begin() + static_cast<long>(start + Window));
    for (std::size_t i = 1; i < s.size(); ++i)
        s[i] += (s[i - 1] - s[i]) * 0.45f;
    for (std::size_t i = s.size() - 1; i-- > 0;)
        s[i] += (s[i + 1] - s[i]) * 0.45f;

    const float left = m_size.x * 0.04f;
    const float width = m_size.x * 0.92f;
    const float midY = m_size.y * 0.42f;
    const float amp = m_size.y * 0.26f * cfg("scope.height");
    const float thickness = cfg("scope.thickness");

    std::vector<sf::Vector2f> pts(Window), echo(Window);
    std::vector<sf::Color> core(Window), halo(Window), echoColors(Window);
    for (std::size_t k = 0; k < Window; ++k)
    {
        const float t = static_cast<float>(k) / static_cast<float>(Window - 1);
        const float sample = std::clamp(s[k] * 1.6f, -1.f, 1.f);
        const float taper = std::sin(t * Pi); // ease in and out at the edges
        pts[k] = {left + t * width, midY + sample * amp * taper};
        echo[k] = {left + t * width, midY - sample * amp * taper * 0.5f};
        core[k] = bandColor(t, 0.95f, 0.55f);
        halo[k] = bandColor(t, 0.16f, 0.9f);
        echoColors[k] = bandColor(t + 0.3f, 0.28f, 0.8f);
    }
    thickLine(out, echo, echoColors, 2.f, false);
    thickLine(out, pts, halo, thickness * 4.f, false); // soft glow under the line hides any stair-stepping
    thickLine(out, pts, core, thickness, false);
}

void Renderer::updateParticles(sf::VertexArray& out, const Spectrum& spectrum, Mode mode, float dt)
{
    std::uniform_real_distribution<float> uni(0.f, 1.f);

    // Spawn more on loud parts and kicks.
    const float rate = (6.f + spectrum.energy() * 60.f + spectrum.beat() * 260.f) * cfg("global.particles");
    m_spawnAccumulator += rate * dt;
    while (m_spawnAccumulator >= 1.f && m_particles.size() < 400)
    {
        m_spawnAccumulator -= 1.f;
        Particle p;
        p.maxLife = p.life = 1.f + uni(m_rng) * 1.6f;
        p.size = 1.f + uni(m_rng) * 2.5f;
        p.hue = uni(m_rng) * 0.45f;
        const float boost = 1.f + spectrum.beat() * 2.f;

        if (mode == Mode::Ring)
        {
            const float a = uni(m_rng) * 2.f * Pi;
            const float r = std::min(m_size.x, m_size.y) * 0.17f;
            const sf::Vector2f dir{std::cos(a), std::sin(a)};
            p.position = m_size * 0.5f + dir * r;
            p.velocity = dir * ((40.f + uni(m_rng) * 120.f) * boost);
        }
        else
        {
            const float y = (mode == Mode::Bars) ? m_size.y * 0.66f : m_size.y;
            p.position = {m_size.x * (0.06f + uni(m_rng) * 0.88f), y};
            p.velocity = {(uni(m_rng) - 0.5f) * 30.f, -(30.f + uni(m_rng) * 110.f) * boost};
        }
        m_particles.push_back(p);
    }
    m_spawnAccumulator = std::min(m_spawnAccumulator, 1.f);

    const float drag = std::exp(-0.5f * dt);
    for (auto& p : m_particles)
    {
        p.position += p.velocity * dt;
        p.velocity *= drag;
        p.life -= dt;
    }
    m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
                                     [](const Particle& p) { return p.life <= 0.f; }),
                      m_particles.end());

    for (const auto& p : m_particles)
    {
        const float fade = std::sin(std::clamp(p.life / p.maxLife, 0.f, 1.f) * Pi); // fade in and out
        const sf::Color c = bandColor(p.hue, 0.7f * fade, 0.5f);
        // Soft round dot: bright center fading to nothing at the rim.
        const float radius = p.size * 1.8f;
        constexpr int Segments = 10;
        for (int k = 0; k < Segments; ++k)
        {
            const float a0 = static_cast<float>(k) / Segments * 2.f * Pi;
            const float a1 = static_cast<float>(k + 1) / Segments * 2.f * Pi;
            out.append({p.position, c});
            out.append({p.position + sf::Vector2f{std::cos(a0), std::sin(a0)} * radius, sf::Color::Black});
            out.append({p.position + sf::Vector2f{std::cos(a1), std::sin(a1)} * radius, sf::Color::Black});
        }
    }
}

namespace
{
// Candy colors for the Pop mode.
const sf::Color PopPalette[] = {{255, 45, 145}, {255, 222, 40}, {60, 200, 255}, {150, 232, 60},
                                {255, 145, 35}, {255, 246, 222}, {140, 70, 230}, {255, 255, 255}};
constexpr int PopPaletteSize = 8;
const sf::Color Ink(28, 14, 34);

// A filled star/burst polygon as a triangle fan: alternating outer and inner radii.
void burst(sf::VertexArray& out, sf::Vector2f c, float outer, float inner, int spikes, float angle, sf::Color color,
           const std::vector<float>& jitter)
{
    const int n = spikes * 2;
    for (int i = 0; i < n; ++i)
    {
        auto radiusAt = [&](int k)
        {
            const float j = jitter.empty() ? 1.f : jitter[static_cast<std::size_t>(k % static_cast<int>(jitter.size()))];
            return (k % 2 == 0 ? outer * j : inner);
        };
        const float a0 = angle + static_cast<float>(i) / static_cast<float>(n) * 6.2831853f;
        const float a1 = angle + static_cast<float>(i + 1) / static_cast<float>(n) * 6.2831853f;
        out.append({c, color});
        out.append({c + sf::Vector2f{std::cos(a0), std::sin(a0)} * radiusAt(i), color});
        out.append({c + sf::Vector2f{std::cos(a1), std::sin(a1)} * radiusAt(i + 1), color});
    }
}

void disc(sf::VertexArray& out, sf::Vector2f c, float r, sf::Color color, int segments = 28)
{
    for (int i = 0; i < segments; ++i)
    {
        const float a0 = static_cast<float>(i) / static_cast<float>(segments) * 6.2831853f;
        const float a1 = static_cast<float>(i + 1) / static_cast<float>(segments) * 6.2831853f;
        out.append({c, color});
        out.append({c + sf::Vector2f{std::cos(a0), std::sin(a0)} * r, color});
        out.append({c + sf::Vector2f{std::cos(a1), std::sin(a1)} * r, color});
    }
}

void ring(sf::VertexArray& out, sf::Vector2f c, float r, float width, float squash, sf::Color color, int segments = 64)
{
    for (int i = 0; i < segments; ++i)
    {
        const float a0 = static_cast<float>(i) / static_cast<float>(segments) * 6.2831853f;
        const float a1 = static_cast<float>(i + 1) / static_cast<float>(segments) * 6.2831853f;
        const sf::Vector2f d0{std::cos(a0), std::sin(a0) * squash}, d1{std::cos(a1), std::sin(a1) * squash};
        quad(out, c + d0 * (r + width * 0.5f), c + d1 * (r + width * 0.5f), c + d1 * (r - width * 0.5f), c + d0 * (r - width * 0.5f),
             color, color, color, color);
    }
}
} // namespace

void Renderer::drawDarkForest(const Spectrum& spectrum, float dt)
{
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    const float quality = cfg("forest.quality");
    const float scale = std::min(1.f, std::sqrt(500000.f * quality * quality / (m_size.x * m_size.y)));
    const sf::Vector2u want{std::max(1u, static_cast<unsigned>(m_size.x * scale)),
                            std::max(1u, static_cast<unsigned>(m_size.y * scale))};
    if (m_forestTarget.getSize() != want && m_forestTarget.resize(want))
        m_forestTarget.setSmooth(true);

    const float beat = spectrum.beat();
    // Walking pace: a slow, nervous walk that speeds up with the bass.
    m_forestBass += (spectrum.bass() - m_forestBass) * (1.f - std::exp(-3.f * dt));
    m_forestZ += dt * cfg("forest.speed") * (1.1f + 1.8f * m_forestBass);

    // Looking around: every few seconds the view picks a new spot and turns there on a damped
    // spring, so it eases in and out instead of starting with a jerk. A big kick sometimes makes
    // it turn sooner, like you heard something, but the turn itself is still smooth.
    m_forestLookTimer -= dt;
    if (m_forestLookTimer <= 0.f || (beat > 0.8f && m_forestLookTimer < 1.5f && uni(m_rng) < 0.05f))
    {
        m_forestLookTarget = {(uni(m_rng) - 0.5f) * 0.8f, (uni(m_rng) - 0.55f) * 0.2f};
        m_forestLookTimer = 3.5f + uni(m_rng) * 4.f;
    }
    {
        const float k = 1.6f;                 // stiffness: how strongly it turns toward the target
        const float c = 2.f * std::sqrt(k);   // critically damped: no overshoot or wobble
        float left = std::min(dt, 0.1f);
        while (left > 0.f)
        {
            const float h = std::min(left, 1.f / 120.f);
            m_forestLookVel += ((m_forestLookTarget - m_forestLook) * k - m_forestLookVel * c) * h;
            m_forestLook += m_forestLookVel * h;
            left -= h;
        }
    }
    // A slow, gentle sway on top so it never sits perfectly still.
    const sf::Vector2f sway{0.035f * std::sin(m_time * 0.31f) + 0.02f * std::sin(m_time * 0.53f + 1.f),
                            0.012f * std::sin(m_time * 0.41f + 2.f)};

    // The flashlight flickers out on big hits now and then, and buzzes when it's quiet.
    m_forestFlicker *= std::exp(-9.f * dt);
    if (beat > 0.7f && uni(m_rng) < 0.12f * cfg("forest.static"))
        m_forestFlicker = 0.6f + 0.4f * uni(m_rng);
    if (uni(m_rng) < dt * 0.6f)
        m_forestFlicker = std::max(m_forestFlicker, 0.25f * uni(m_rng));

    const sf::Vector2f size(m_forestTarget.getSize());
    m_forest.setUniform("resolution", sf::Glsl::Vec2{size.x, size.y});
    m_forest.setUniform("time", m_time);
    m_forest.setUniform("walkZ", m_forestZ);
    m_forest.setUniform("beat", beat);
    m_forest.setUniform("energy", spectrum.energy());
    m_forest.setUniform("flashlight", cfg("forest.light"));
    m_forest.setUniform("fogAmount", cfg("forest.fog"));
    m_forest.setUniform("staticAmount", cfg("forest.static"));
    m_forest.setUniform("density", cfg("forest.trees"));
    m_forest.setUniform("pagesOn", cfg("forest.pages"));
    m_forest.setUniform("flicker", m_forestFlicker);
    m_forest.setUniform("look", sf::Glsl::Vec2{m_forestLook.x + sway.x, m_forestLook.y + sway.y});
    m_forest.setUniform("hasImage", m_hasImage ? 1.f : 0.f);
    if (m_hasImage)
    {
        const sf::Vector2f imageSize(m_image.getSize());
        m_forest.setUniform("image", m_image);
        m_forest.setUniform("imageAspect", imageSize.x / std::max(imageSize.y, 1.f));
    }
    m_forestTarget.clear(sf::Color::Black);
    m_forestTarget.draw(sf::RectangleShape(size), sf::RenderStates(&m_forest));
    m_forestTarget.display();
    sf::Sprite frame(m_forestTarget.getTexture());
    frame.setScale({m_size.x / size.x, m_size.y / size.y});
    m_scene.draw(frame);
}

namespace
{
// Pixel-art stickers, 12x12: '#' = sticker color, 'k' = black, 'w' = white, '.' = empty.
constexpr int HackIconCount = 10;
const char* const HackIcons[HackIconCount][12] = {
    {"...######...", ".##########.", ".##########.", "###kk##kk###", "###kk##kk###", "############",
     "############", "##k######k##", "###k####k###", ".###kkkk###.", ".##########.", "...######..."},   // smiley
    {"...######...", ".##########.", ".#k#k##k#k#.", "###k####k###", "##k#k##k#k##", "############",
     "############", "############", "###kkkkkk###", ".##########.", ".##########.", "...######..."},   // knocked out
    {"............", ".###....###.", "#####..#####", "#ww#########", "#w##########", "############",
     ".##########.", "..########..", "...######...", "....####....", ".....##.....", "............"},   // heart
    {"......####..", ".....####...", "....####....", "...####.....", "..########..", ".#########..",
     "....####....", "...####.....", "..####......", "..###.......", ".##.........", ".#.........."},   // bolt
    {"............", "............", "....####....", "..##wwww##..", ".#wwwkkwww#.", "#wwwkkkkwww#",
     "#wwwkkkkwww#", ".#wwwkkwww#.", "..##wwww##..", "....####....", "............", "............"},   // eye
    {"k...........", "kk..........", "k#k.........", "k##k........", "k###k.......", "k####k......",
     "k#####k.....", "k######k....", "k####kkkk...", "k#kk#k......", "kk..k#k.....", ".....kk....."},   // cursor
    {"##########..", "#kk####kk##.", "#kk####kk###", "#kkkkkkkk###", "############", "############",
     "#wwwwwwwwww#", "#wkkkkkkkkw#", "#wwwwwwwwww#", "#wkkkkkkkkw#", "#wwwwwwwwww#", "############"},   // floppy
    {".....##.....", ".....##.....", "....####....", "....####....", "..########..", "############",
     "############", "..########..", "....####....", "....####....", ".....##.....", ".....##....."},   // sparkle
    {"....kkkk....", "...k....k...", "...k....k...", "........k...", "........k...", ".##########.",
     ".##########.", ".####kk####.", ".####kk####.", ".#####k####.", ".##########.", ".##########."},   // open lock
    {"............", "....##......", "...###..#...", "..####...#..", "######.#..#.", "######..#.#.",
     "######..#.#.", "######.#..#.", "..####...#..", "...###..#...", "....##......", "............"},   // speaker
};

const sf::Color HackNeon[] = {{255, 40, 160}, {0, 230, 255}, {240, 255, 50}, {150, 70, 255},
                              {60, 255, 130}, {255, 90, 30}, {255, 255, 255}};
constexpr int HackNeonCount = 7;

const char* const HackWords[] = {"HIJACKED", "ACCESS GRANTED", "NO SIGNAL", "OVERRIDE", "NOPE", "LOUDER",
                                 "WAKE UP", "SYSTEM FAILURE", "404", "LIVE", "BASS.EXE", "ERROR",
                                 "TURN UP", "WE IN IT", "REBOOT", "?!?"};
constexpr int HackWordCount = 16;

struct HackMessage
{
    const char* title;
    const char* body;
};
const HackMessage HackMessages[] = {
    {"access.exe", "ACCESS GRANTED"},
    {"WARNING", "Too much bass detected"},
    {"readme.txt", "Your speakers belong to us now"},
    {"firewall.sys", "who is u"},
    {"notice", "Do not unplug"},
    {"volume.dll", "Volume: yes"},
    {"error 0x0BAD", "mb"},
    {"system", "Are you sure? (no)"},
    {"backdoor.bat", "Knock knock."},
    {"alert", "New signal found"},
};
constexpr int HackMessageCount = 10;
const char* const HackProgress[] = {"Uploading song...", "Decrypting beat...", "Rerouting signal...",
                                    "Downloading more bass...", "Bypassing firewall...", "Injecting virus..."};
constexpr int HackProgressCount = 6;
const char* const HackIconNames[] = {"lol.png", "rip.png", "luv.png", "zap.png", "watching.png",
                                     "click_me.png", "backup.png", "shiny.png", "unlocked.png", "loud.png"};

// One line for the terminal; tone picks its color.
std::string hackLine(std::mt19937& rng, const Spectrum& spectrum, int& tone)
{
    std::uniform_int_distribution<int> which(0, 15);
    std::uniform_int_distribution<int> byte(0, 255);
    char buf[128];
    tone = 0;
    switch (which(rng))
    {
        case 0: std::snprintf(buf, sizeof(buf), "> inject payload --target 0x%04X", byte(rng) * 256 + byte(rng)); tone = 1; break;
        case 1: std::snprintf(buf, sizeof(buf), "[ OK ] handshake 10.%d.%d.%d", byte(rng), byte(rng), byte(rng)); tone = 2; break;
        case 2: std::snprintf(buf, sizeof(buf), "bypass firewall ........ done"); tone = 2; break;
        case 3: std::snprintf(buf, sizeof(buf), "decrypt %08X%08X", byte(rng) * 16777 + byte(rng), byte(rng) * 65521 + byte(rng)); break;
        case 4: std::snprintf(buf, sizeof(buf), "> ping node_%02d   %d ms", byte(rng) % 64, 4 + byte(rng) % 90); tone = 1; break;
        case 5: std::snprintf(buf, sizeof(buf), "loading l00p.exe [%3d%%]", byte(rng) * 100 / 255); break;
        case 6: std::snprintf(buf, sizeof(buf), "!! trace detected -- spoofing"); tone = 3; break;
        case 7: std::snprintf(buf, sizeof(buf), "cat /dev/beat > /dev/speakers"); tone = 1; break;
        case 8: std::snprintf(buf, sizeof(buf), "sudo turn-it-up --max"); tone = 1; break;
        case 9: std::snprintf(buf, sizeof(buf), "rerouting signal ... %d hops", 3 + byte(rng) % 40); break;
        case 10:
            std::snprintf(buf, sizeof(buf), "%02X %02X %02X %02X %02X %02X %02X %02X", byte(rng), byte(rng), byte(rng),
                          byte(rng), byte(rng), byte(rng), byte(rng), byte(rng));
            break;
        case 11: std::snprintf(buf, sizeof(buf), "> override --all"); tone = 3; break;
        case 12:
            std::snprintf(buf, sizeof(buf), "bass %3d%%  kick %3d%%  nrg %3d%%",
                          static_cast<int>(std::clamp(spectrum.bass(), 0.f, 1.f) * 100.f),
                          static_cast<int>(std::clamp(spectrum.beat(), 0.f, 1.f) * 100.f),
                          static_cast<int>(std::clamp(spectrum.energy(), 0.f, 1.f) * 100.f));
            tone = 4;
            break;
        case 13: std::snprintf(buf, sizeof(buf), "upload chunk %d/%d", 1 + byte(rng) % 99, 100); break;
        case 14: std::snprintf(buf, sizeof(buf), "WARNING: they can hear you"); tone = 3; break;
        default:
        {
            std::string bits;
            for (int i = 0; i < 24; ++i)
                bits += (byte(rng) & 1) ? '1' : '0';
            return bits;
        }
    }
    return buf;
}

void hackBox(sf::VertexArray& out, const sf::Transform& tf, float x0, float y0, float x1, float y1, sf::Color c)
{
    quad(out, tf.transformPoint({x0, y0}), tf.transformPoint({x1, y0}), tf.transformPoint({x1, y1}),
         tf.transformPoint({x0, y1}), c, c, c, c);
}

// A pixel icon centered on the transform's origin. As a sticker it gets a white die-cut border and a hard shadow.
void hackIcon(sf::VertexArray& out, const sf::Transform& tf, int icon, float cell, sf::Color color, bool sticker)
{
    const char* const* rows = HackIcons[((icon % HackIconCount) + HackIconCount) % HackIconCount];
    const float half = 6.f * cell;
    auto each = [&](auto&& fn)
    {
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 12; ++x)
                if (rows[y][x] != '.')
                    fn(static_cast<float>(x) * cell - half, static_cast<float>(y) * cell - half, rows[y][x]);
    };
    if (sticker)
    {
        const float pad = cell * 1.1f;
        const float drop = cell * 0.9f;
        each([&](float x, float y, char) { hackBox(out, tf, x - pad + drop, y - pad + drop, x + cell + pad + drop, y + cell + pad + drop, sf::Color(0, 0, 0, 200)); });
        each([&](float x, float y, char) { hackBox(out, tf, x - pad, y - pad, x + cell + pad, y + cell + pad, sf::Color::White); });
    }
    each([&](float x, float y, char c)
         {
             const sf::Color col = c == 'k' ? sf::Color(12, 10, 18) : c == 'w' ? sf::Color::White : color;
             hackBox(out, tf, x - 0.3f, y - 0.3f, x + cell + 0.3f, y + cell + 0.3f, col);
         });
}

float easeOutBack(float t)
{
    t = std::clamp(t, 0.f, 1.f) - 1.f;
    return 1.f + t * t * (2.7f * t + 1.7f);
}
} // namespace

void Renderer::loadHijackFonts()
{
    if (m_hackFontsTried)
        return;
    m_hackFontsTried = true;
    for (const auto& path : platform::monoFontCandidates())
        if (m_monoFont.openFromFile(path))
        {
            m_hasMono = true;
            break;
        }
    for (const auto& path : platform::boldFontCandidates())
        if (m_boldFont.openFromFile(path))
        {
            m_hasBold = true;
            break;
        }
}

void Renderer::drawHijack(const Spectrum& spectrum, float dt, const UiState& ui)
{
    loadHijackFonts();
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    const sf::Font* mono = m_hasMono ? &m_monoFont : m_font;
    const sf::Font* bold = m_hasBold ? &m_boldFont : m_font;
    const float beat = spectrum.beat();
    const float energy = spectrum.energy();
    const float unit = std::min(m_size.x, m_size.y);
    const float glitch = cfg("hijack.glitch");
    auto neonPick = [&] { return HackNeon[static_cast<int>(uni(m_rng) * HackNeonCount) % HackNeonCount]; };

    const sf::Vector2u want{static_cast<unsigned>(m_size.x), static_cast<unsigned>(m_size.y)};
    if (m_hijackLayer.getSize() != want && m_hijackLayer.resize(want))
        m_hijackLayer.setSmooth(true);
    sf::RenderTexture& layer = m_hijackLayer;

    // ---- What the music sets off ----
    const float lastBeat = m_hackLastBeat;
    m_hackLastBeat = beat;
    auto crossed = [&](float level) { return beat > level && lastBeat <= level; };
    const bool kick = crossed(0.35f);
    const bool bigKick = crossed(0.6f);
    const bool hugeKick = crossed(0.82f);
    m_hackSeedTimer -= dt;
    if (m_hackSeedTimer <= 0.f || kick)
    {
        m_hackSeed = uni(m_rng) * 100.f;
        m_hackSeedTimer = 0.05f + 0.06f * uni(m_rng);
    }
    m_hackInvert *= std::exp(-16.f * dt);
    if (hugeKick && uni(m_rng) < 0.3f * glitch)
        m_hackInvert = 0.9f;
    if (bigKick && uni(m_rng) < 0.15f)
        m_hackPalette += 1.f;

    // Popups.
    const float popups = cfg("hijack.popups");
    m_hackPopupTimer -= dt * popups;
    const std::size_t maxPopups = static_cast<std::size_t>(5.f + 3.f * popups);
    auto spawnPopup = [&](int kind, sf::Vector2f at, float delay)
    {
        HackPopup p;
        p.kind = kind;
        const float w = unit * (kind == 4 ? 0.22f + 0.08f * uni(m_rng) : 0.36f + 0.16f * uni(m_rng));
        float h = w * 0.42f;
        if (kind == 1) h = w * 0.36f;
        if (kind == 2) h = w * 0.44f;
        if (kind == 3) h = w * 0.5f;
        if (kind == 4) h = w * 0.95f;
        if (kind == 5)
        {
            const sf::Vector2f is(m_image.getSize());
            h = w * std::clamp(is.y / std::max(is.x, 1.f), 0.5f, 1.3f) + unit * 0.034f;
        }
        p.size = {w, h};
        p.position = at.x >= 0.f ? at
                                 : sf::Vector2f{(m_size.x - w) * uni(m_rng), m_size.y * 0.04f + (m_size.y * 0.8f - h) * uni(m_rng)};
        p.age = -delay;
        p.life = 1.6f + 2.f * uni(m_rng);
        p.speed = 0.35f + 0.6f * uni(m_rng);
        p.icon = static_cast<int>(uni(m_rng) * HackIconCount) % HackIconCount;
        p.text = static_cast<int>(uni(m_rng) * 1000.f);
        p.bar = neonPick();
        do
            p.shadow = neonPick();
        while (p.shadow == p.bar);
        m_hackPopups.push_back(p);
    };
    if (popups > 0.f && m_hackPopups.size() < maxPopups && (m_hackPopupTimer <= 0.f || (kick && uni(m_rng) < 0.4f * popups)))
    {
        const float r = uni(m_rng);
        int kind = r < 0.25f ? 0 : r < 0.45f ? 1 : r < 0.63f ? 2 : r < 0.8f ? 3 : 4;
        if (m_hasImage && uni(m_rng) < 0.22f)
            kind = 5;
        spawnPopup(kind, {-1.f, -1.f}, 0.f);
        m_hackPopupTimer = 1.8f + 1.6f * uni(m_rng);
    }
    if (popups > 0.f && hugeKick && uni(m_rng) < 0.18f * popups && m_hackPopups.size() < maxPopups + 4)
    {
        // A cascade of the same error box, one after another.
        const sf::Vector2f start{m_size.x * (0.1f + 0.45f * uni(m_rng)), m_size.y * (0.08f + 0.3f * uni(m_rng))};
        const int text = static_cast<int>(uni(m_rng) * 1000.f);
        for (int i = 0; i < 5; ++i)
        {
            spawnPopup(2, start + sf::Vector2f{unit * 0.03f, unit * 0.03f} * static_cast<float>(i), 0.07f * static_cast<float>(i));
            m_hackPopups.back().text = text;
            m_hackPopups.back().life = 1.4f + 0.1f * static_cast<float>(i);
        }
    }
    for (auto& p : m_hackPopups)
    {
        p.age += dt;
        if (p.age > 0.f)
            p.progress = std::min(1.f, p.progress + dt * p.speed * (0.4f + energy + beat));
    }
    m_hackPopups.erase(std::remove_if(m_hackPopups.begin(), m_hackPopups.end(), [](const HackPopup& p) { return p.age > p.life; }),
                       m_hackPopups.end());

    // Stickers.
    const float stickers = cfg("hijack.stickers");
    m_hackStickerTimer -= dt * stickers * (1.f + 2.f * energy);
    const std::size_t maxStickers = static_cast<std::size_t>(8.f + 7.f * stickers);
    if (stickers > 0.f && m_hackStickers.size() < maxStickers && (m_hackStickerTimer <= 0.f || (kick && uni(m_rng) < 0.5f * stickers)))
    {
        HackSticker s;
        s.position = {m_size.x * (0.05f + 0.9f * uni(m_rng)), m_size.y * (0.08f + 0.78f * uni(m_rng))};
        s.velocity = {(uni(m_rng) - 0.5f) * unit * 0.04f, (uni(m_rng) - 0.5f) * unit * 0.03f};
        s.angle = (uni(m_rng) - 0.5f) * 40.f;
        s.spin = (uni(m_rng) - 0.5f) * 14.f;
        s.size = unit * (0.06f + 0.07f * uni(m_rng)) * (0.85f + 0.4f * beat);
        s.life = 2.2f + 3.f * uni(m_rng);
        s.icon = static_cast<int>(uni(m_rng) * HackIconCount) % HackIconCount;
        s.color = neonPick();
        if (s.color == sf::Color::White)
            s.color = HackNeon[0];
        if (m_hasImage && uni(m_rng) < 0.4f)
        {
            // A torn-out piece of your picture.
            const sf::Vector2f is(m_image.getSize());
            const float cw = is.x * (0.2f + 0.3f * uni(m_rng));
            const float ch = std::min(is.y, cw * (0.6f + 0.6f * uni(m_rng)));
            s.cut = sf::FloatRect({(is.x - cw) * uni(m_rng), (is.y - ch) * uni(m_rng)}, {cw, ch});
            s.icon = -1;
            s.size *= 1.6f;
        }
        m_hackStickers.push_back(s);
        m_hackStickerTimer = 0.8f + 1.2f * uni(m_rng);
    }
    for (auto& s : m_hackStickers)
    {
        s.age += dt;
        s.position += s.velocity * dt;
        s.angle += s.spin * dt;
    }
    m_hackStickers.erase(std::remove_if(m_hackStickers.begin(), m_hackStickers.end(), [](const HackSticker& s) { return s.age > s.life; }),
                         m_hackStickers.end());

    // Big words.
    m_hackWordLife = std::max(0.f, m_hackWordLife - dt);
    m_hackWordCooldown -= dt;
    if (cfg("hijack.words") > 0.5f && m_hackWordCooldown <= 0.f && ((bigKick && uni(m_rng) < 0.6f) || (kick && m_hackWordCooldown < -3.f)))
    {
        m_hackWord = HackWords[static_cast<int>(uni(m_rng) * HackWordCount) % HackWordCount];
        m_hackWordMax = m_hackWordLife = 0.6f + 0.6f * uni(m_rng);
        m_hackWordPos = {m_size.x * (0.3f + 0.4f * uni(m_rng)), m_size.y * (0.3f + 0.4f * uni(m_rng))};
        m_hackWordAngle = (uni(m_rng) - 0.5f) * 14.f;
        m_hackWordColor = uni(m_rng) < 0.5f ? sf::Color::White : HackNeon[2];
        m_hackWordCooldown = 1.3f + 2.f * uni(m_rng);
    }

    // Terminal typing.
    if (m_hackTyping.empty())
    {
        int tone = 0;
        m_hackTyping = hackLine(m_rng, spectrum, tone);
        const sf::Color tones[] = {{215, 215, 225}, {0, 230, 255}, {60, 255, 130}, {255, 60, 170}, {240, 255, 60}};
        m_hackTypingColor = tones[tone];
        m_hackTyped = 0.f;
    }
    m_hackTyped += dt * (25.f + 140.f * energy + 160.f * beat);
    if (m_hackTyped >= static_cast<float>(m_hackTyping.size()) + 4.f)
    {
        m_hackLines.emplace_back(m_hackTyping, m_hackTypingColor);
        if (m_hackLines.size() > 40)
            m_hackLines.erase(m_hackLines.begin());
        m_hackTyping.clear();
    }

    // ---- Background ----
    layer.clear(sf::Color::Black);
    m_hijackBg.setUniform("resolution", sf::Glsl::Vec2{m_size.x, m_size.y});
    m_hijackBg.setUniform("time", m_time);
    m_hijackBg.setUniform("bass", spectrum.bass());
    m_hijackBg.setUniform("beat", beat);
    m_hijackBg.setUniform("energy", energy);
    m_hijackBg.setUniform("palette", m_hackPalette);
    m_hijackBg.setUniform("dots", cfg("hijack.dots"));
    m_hijackBg.setUniform("audio", m_audioTexture);
    m_hijackBg.setUniform("hasImage", m_hasImage ? 1.f : 0.f);
    if (m_hasImage)
    {
        m_hijackBg.setUniform("image", m_image);
        m_hijackBg.setUniform("fitScale", imageFitScale());
    }
    layer.draw(sf::RectangleShape(m_size), sf::RenderStates(&m_hijackBg));

    auto makeText = [&](const sf::Font& font, const sf::String& s, float size, sf::Color color)
    {
        sf::Text t(font, s, static_cast<unsigned>(std::max(8.f, size)));
        t.setFillColor(color);
        return t;
    };
    // Fit a line (or two) of text into a box, centered at the transform's origin.
    auto fitText = [&](const sf::Font& font, std::string s, float maxW, float maxH, sf::Color color)
    {
        sf::Text t = makeText(font, s, maxH * 0.55f, color);
        float k = std::min({1.f, maxW / std::max(1.f, t.getLocalBounds().size.x), maxH / std::max(1.f, t.getLocalBounds().size.y)});
        const auto space = s.find(' ', s.size() / 3);
        if (k < 0.72f && space != std::string::npos)
        {
            s[space] = '\n';
            t = makeText(font, s, maxH * 0.36f, color);
            k = std::min({1.f, maxW / std::max(1.f, t.getLocalBounds().size.x), maxH / std::max(1.f, t.getLocalBounds().size.y)});
        }
        t.setCharacterSize(static_cast<unsigned>(std::max(8.f, static_cast<float>(t.getCharacterSize()) * k)));
        const sf::FloatRect b = t.getLocalBounds();
        t.setOrigin(b.position + b.size * 0.5f);
        return t;
    };

    // ---- Stickers ----
    for (const auto& s : m_hackStickers)
    {
        const float pop = easeOutBack(s.age / 0.22f) * (s.life - s.age < 0.2f ? (s.life - s.age) / 0.2f : 1.f);
        if (pop <= 0.01f)
            continue;
        sf::Transform tf;
        tf.translate(s.position).rotate(sf::degrees(s.angle)).scale({pop, pop});
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        if (s.icon >= 0)
        {
            hackIcon(va, tf, s.icon, s.size / 12.f, s.color, true);
            layer.draw(va);
        }
        else
        {
            const float w = s.size;
            const float h = w * s.cut.size.y / std::max(1.f, s.cut.size.x);
            const float border = unit * 0.008f;
            hackBox(va, tf, -w * 0.5f - border + border * 1.5f, -h * 0.5f - border + border * 1.5f, w * 0.5f + border * 2.5f, h * 0.5f + border * 2.5f, sf::Color(0, 0, 0, 200));
            hackBox(va, tf, -w * 0.5f - border, -h * 0.5f - border, w * 0.5f + border, h * 0.5f + border, sf::Color::White);
            layer.draw(va);
            sf::Sprite cut(m_image);
            cut.setTextureRect(sf::IntRect({static_cast<int>(s.cut.position.x), static_cast<int>(s.cut.position.y)},
                                           {static_cast<int>(s.cut.size.x), static_cast<int>(s.cut.size.y)}));
            cut.setScale({w / s.cut.size.x, h / s.cut.size.y});
            cut.setPosition({-w * 0.5f, -h * 0.5f});
            layer.draw(cut, sf::RenderStates(tf));
        }
    }

    // ---- Terminal ----
    if (cfg("hijack.terminal") > 0.5f && mono)
    {
        const float x0 = m_size.x * 0.03f;
        const float y0 = m_size.y * 0.07f;
        const float w = std::min(m_size.x * 0.36f, unit * 0.7f);
        const float h = m_size.y * 0.4f;
        const float fontSize = std::max(10.f, unit * 0.021f);
        const float lineH = fontSize * 1.3f;
        const float titleH = lineH * 1.25f;
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        const sf::Transform id;
        const sf::Color edge = HackNeon[static_cast<int>(m_hackPalette) % 2 == 0 ? 1 : 0];
        const float e = std::max(2.f, unit * 0.003f);
        hackBox(va, id, x0, y0 + titleH, x0 + w, y0 + h, sf::Color(6, 4, 12, 228));
        hackBox(va, id, x0 - e, y0 - e, x0 + w + e, y0 + titleH, edge);   // title strip and the frame around it
        hackBox(va, id, x0 - e, y0 + titleH, x0, y0 + h + e, edge);
        hackBox(va, id, x0 + w, y0 + titleH, x0 + w + e, y0 + h + e, edge);
        hackBox(va, id, x0, y0 + h, x0 + w, y0 + h + e, edge);
        layer.draw(va);
        sf::Text title = makeText(*mono, "term://node_07  [live]", fontSize * 0.9f, sf::Color(10, 8, 16));
        title.setPosition({std::round(x0 + fontSize * 0.5f), std::round(y0 + titleH * 0.12f)});
        layer.draw(title);

        const std::size_t charsFit = static_cast<std::size_t>(std::max(8.f, (w - fontSize) / (fontSize * 0.61f)));
        const int rows = std::max(1, static_cast<int>((h - titleH - lineH * 0.4f) / lineH));
        std::vector<std::pair<std::string, sf::Color>> shown;
        const int fromHistory = rows - 1;
        for (int i = std::max(0, static_cast<int>(m_hackLines.size()) - fromHistory); i < static_cast<int>(m_hackLines.size()); ++i)
            shown.push_back(m_hackLines[static_cast<std::size_t>(i)]);
        std::string typing = m_hackTyping.substr(0, std::min(m_hackTyping.size(), static_cast<std::size_t>(m_hackTyped)));
        const bool blink = std::fmod(m_time, 0.6f) < 0.3f;
        shown.emplace_back(typing + (blink ? "_" : " "), m_hackTypingColor);
        float y = y0 + titleH + lineH * 0.25f;
        for (auto& [line, color] : shown)
        {
            sf::Text t = makeText(*mono, line.substr(0, charsFit), fontSize, color);
            t.setPosition({std::round(x0 + fontSize * 0.5f), std::round(y)});
            layer.draw(t);
            y += lineH;
        }
    }

    // ---- Spectrum as stacked pixel blocks ----
    if (cfg("hijack.bars") > 0.5f)
    {
        constexpr std::size_t Columns = 32;
        const std::vector<float> bands = resample(spectrum.bands(), Columns);
        const std::vector<float> peaks = resample(spectrum.peaks(), Columns);
        const float left = m_size.x * 0.04f;
        const float slot = m_size.x * 0.92f / static_cast<float>(Columns);
        const float block = slot * 0.8f;
        const float rowH = std::min(block, m_size.y * 0.022f);
        const float bottom = m_size.y * 0.965f;
        constexpr int MaxRows = 11;
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        const sf::Transform id;
        const sf::Color low = HackNeon[1], mid = HackNeon[0], high = HackNeon[2];
        for (std::size_t i = 0; i < Columns; ++i)
        {
            const int n = static_cast<int>(std::round(std::clamp(bands[i], 0.f, 1.f) * MaxRows));
            const float x = left + slot * static_cast<float>(i) + (slot - block) * 0.5f;
            for (int k = 0; k < n; ++k)
            {
                const float t = static_cast<float>(k) / MaxRows;
                const sf::Color c = t < 0.45f ? low : t < 0.8f ? mid : high;
                const float yb = bottom - static_cast<float>(k) * rowH * 1.25f;
                hackBox(va, id, x + 2.f, yb - rowH + 2.f, x + block + 2.f, yb + 2.f, sf::Color(0, 0, 0, 170));
                hackBox(va, id, x, yb - rowH, x + block, yb, c);
            }
            const int pk = static_cast<int>(std::round(std::clamp(peaks[i], 0.f, 1.f) * MaxRows));
            if (pk > n)
            {
                const float yb = bottom - static_cast<float>(pk) * rowH * 1.25f;
                hackBox(va, id, x, yb - rowH * 0.5f, x + block, yb, sf::Color::White);
            }
        }
        layer.draw(va);
    }

    // ---- Popups ----
    for (std::size_t index = 0; index < m_hackPopups.size(); ++index)
    {
        const HackPopup& p = m_hackPopups[index];
        if (p.age <= 0.f)
            continue;
        const float left = p.life - p.age;
        if (left < 0.25f && std::fmod(p.age * 30.f, 2.f) < 1.f)
            continue; // glitches out before it vanishes
        const float pop = easeOutBack(p.age / 0.16f);
        const float w = p.size.x, h = p.size.y;
        const float tb = unit * 0.034f;
        sf::Transform tf;
        tf.translate(p.position + p.size * 0.5f).scale({pop, pop}).translate(-p.size * 0.5f);
        const bool dark = p.kind == 3;
        const sf::Color body = dark ? sf::Color(8, 6, 14) : sf::Color(236, 236, 242);
        const sf::Color ink = dark ? sf::Color(230, 230, 240) : sf::Color(14, 12, 22);
        const float outline = std::max(2.f, unit * 0.004f);
        const float drop = unit * 0.014f;
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        hackBox(va, tf, drop - outline, drop - outline, w + drop + outline, h + drop + outline, p.shadow);
        hackBox(va, tf, -outline, -outline, w + outline, h + outline, sf::Color(10, 8, 16));
        hackBox(va, tf, 0.f, 0.f, w, h, body);
        hackBox(va, tf, 0.f, 0.f, w, tb, p.bar);
        hackBox(va, tf, 0.f, tb - outline * 0.5f, w, tb + outline * 0.5f, sf::Color(10, 8, 16));
        // Close button.
        const float bx0 = w - tb + tb * 0.18f, by0 = tb * 0.18f, bs = tb * 0.64f;
        hackBox(va, tf, bx0, by0, bx0 + bs, by0 + bs, sf::Color(10, 8, 16));
        {
            const float m = bs * 0.25f, th = std::max(1.5f, bs * 0.12f);
            const sf::Vector2f a = {bx0 + m, by0 + m}, b = {bx0 + bs - m, by0 + bs - m};
            const sf::Vector2f c = {bx0 + bs - m, by0 + m}, d = {bx0 + m, by0 + bs - m};
            auto stroke = [&](sf::Vector2f s0, sf::Vector2f s1)
            {
                sf::Vector2f dir = s1 - s0;
                const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                const sf::Vector2f n = sf::Vector2f{-dir.y, dir.x} / len * th;
                quad(va, tf.transformPoint(s0 + n), tf.transformPoint(s1 + n), tf.transformPoint(s1 - n), tf.transformPoint(s0 - n),
                     sf::Color::White, sf::Color::White, sf::Color::White, sf::Color::White);
            };
            stroke(a, b);
            stroke(c, d);
        }
        const float cx = w * 0.5f, cy = tb + (h - tb) * 0.5f;
        const float pad = w * 0.06f;
        const HackMessage& msg = HackMessages[p.text % HackMessageCount];
        std::string titleText = msg.title;
        if (p.kind == 1) titleText = "upload.exe";
        if (p.kind == 3) titleText = "signal.exe";
        if (p.kind == 4) titleText = HackIconNames[p.icon];
        if (p.kind == 5) titleText = "screenshot.png";

        if (p.kind == 2)
        {
            // Warning triangle.
            const float ts = (h - tb) * 0.42f;
            const sf::Vector2f c0{pad + ts * 0.5f, tb + (h - tb) * 0.22f};
            const sf::Color y = HackNeon[2];
            va.append({tf.transformPoint({c0.x, c0.y}), y});
            va.append({tf.transformPoint({c0.x + ts * 0.58f, c0.y + ts}), y});
            va.append({tf.transformPoint({c0.x - ts * 0.58f, c0.y + ts}), y});
            hackBox(va, tf, c0.x - ts * 0.06f, c0.y + ts * 0.3f, c0.x + ts * 0.06f, c0.y + ts * 0.7f, sf::Color(10, 8, 16));
            hackBox(va, tf, c0.x - ts * 0.06f, c0.y + ts * 0.78f, c0.x + ts * 0.06f, c0.y + ts * 0.9f, sf::Color(10, 8, 16));
            // Buttons.
            const float bw = w * 0.2f, bh = (h - tb) * 0.22f, byy = h - bh - (h - tb) * 0.1f;
            for (int i = 0; i < 2; ++i)
            {
                const float bxx = w - pad - bw * static_cast<float>(2 - i) - pad * 0.5f * static_cast<float>(1 - i);
                hackBox(va, tf, bxx - outline, byy - outline, bxx + bw + outline, byy + bh + outline, sf::Color(10, 8, 16));
                hackBox(va, tf, bxx, byy, bxx + bw, byy + bh, i == 0 ? sf::Color(205, 205, 215) : p.bar);
            }
        }
        if (p.kind == 1)
        {
            const float bx = pad, by = tb + (h - tb) * 0.55f, bw = w - pad * 2.f, bh = (h - tb) * 0.24f;
            hackBox(va, tf, bx - outline, by - outline, bx + bw + outline, by + bh + outline, sf::Color(10, 8, 16));
            hackBox(va, tf, bx, by, bx + bw, by + bh, sf::Color(30, 28, 40));
            const int segs = 14;
            const int filled = static_cast<int>(p.progress * segs);
            const float sw = bw / segs;
            for (int i = 0; i < filled; ++i)
                hackBox(va, tf, bx + sw * static_cast<float>(i) + 2.f, by + 2.f, bx + sw * static_cast<float>(i + 1) - 2.f, by + bh - 2.f, p.bar == sf::Color::White ? HackNeon[1] : p.bar);
        }
        if (p.kind == 3)
        {
            const std::size_t n = 24;
            const std::vector<float> bands = resample(spectrum.bands(), n);
            const float bx = pad, bw = w - pad * 2.f, base = h - pad * 0.6f, top = tb + pad * 0.6f;
            const float sw = bw / static_cast<float>(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                const float v = std::clamp(bands[i], 0.f, 1.f);
                const sf::Color c = HackNeon[(i * 3 / n + static_cast<std::size_t>(m_hackPalette)) % 3];
                hackBox(va, tf, bx + sw * static_cast<float>(i) + 1.5f, base - (base - top) * (0.04f + 0.96f * v),
                        bx + sw * static_cast<float>(i + 1) - 1.5f, base, c);
            }
        }
        if (p.kind == 4)
        {
            sf::Transform it = tf;
            it.translate({cx, cy});
            hackIcon(va, it, p.icon, (h - tb) * 0.62f / 12.f, p.bar == sf::Color::White ? HackNeon[0] : p.bar, false);
        }
        layer.draw(va);

        if (p.kind == 5 && m_hasImage)
        {
            sf::Sprite pic(m_image);
            const sf::Vector2f is(m_image.getSize());
            const float aw = w - outline * 2.f, ah = h - tb - outline * 2.f;
            const float k = std::min(aw / is.x, ah / is.y);
            pic.setScale({k, k});
            pic.setPosition({cx - is.x * k * 0.5f, tb + outline + (ah - is.y * k) * 0.5f});
            if (beat > 0.6f)
                pic.setColor(p.bar);
            layer.draw(pic, sf::RenderStates(tf));
        }

        // Text.
        if (mono)
        {
            const bool lightBar = static_cast<int>(p.bar.r) + p.bar.g + p.bar.b > 380;
            sf::Text title = makeText(*mono, titleText, tb * 0.58f, lightBar ? sf::Color(10, 8, 16) : sf::Color::White);
            title.setPosition({std::round(tb * 0.3f), std::round(tb * 0.14f)});
            layer.draw(title, sf::RenderStates(tf));
        }
        sf::Transform center = tf;
        if (p.kind == 0 && bold)
        {
            center.translate({cx, cy});
            sf::Text t = fitText(*bold, msg.body, w - pad * 2.f, (h - tb) * 0.7f, ink);
            layer.draw(t, sf::RenderStates(center));
        }
        else if (p.kind == 1 && mono)
        {
            center.translate({cx, tb + (h - tb) * 0.3f});
            char pct[16];
            std::snprintf(pct, sizeof(pct), "  %d%%", static_cast<int>(p.progress * 100.f));
            sf::Text t = fitText(*mono, std::string(HackProgress[p.text % HackProgressCount]) + pct, w - pad * 2.f, (h - tb) * 0.3f, ink);
            layer.draw(t, sf::RenderStates(center));
        }
        else if (p.kind == 2 && mono)
        {
            const float ts = (h - tb) * 0.42f;
            center.translate({pad * 1.6f + ts + (w - pad * 2.6f - ts) * 0.5f, tb + (h - tb) * 0.36f});
            sf::Text t = fitText(*mono, msg.body, w - pad * 2.6f - ts, (h - tb) * 0.42f, ink);
            layer.draw(t, sf::RenderStates(center));
            const float bw = w * 0.2f, bh = (h - tb) * 0.22f, byy = h - bh - (h - tb) * 0.1f;
            const char* labels[2] = {"YES?", "YES"};
            for (int i = 0; i < 2; ++i)
            {
                const float bxx = w - pad - bw * static_cast<float>(2 - i) - pad * 0.5f * static_cast<float>(1 - i);
                sf::Transform bt = tf;
                bt.translate({bxx + bw * 0.5f, byy + bh * 0.5f});
                sf::Text lt = fitText(bold ? *bold : *mono, labels[i], bw * 0.7f, bh * 1.2f, sf::Color(10, 8, 16));
                layer.draw(lt, sf::RenderStates(bt));
            }
        }
    }

    // ---- Big word ----
    if (m_hackWordLife > 0.f && bold)
    {
        const float age = m_hackWordMax - m_hackWordLife;
        const bool visible = !(m_hackWordLife < 0.2f && std::fmod(age * 28.f, 2.f) < 1.f);
        if (visible)
        {
            const float pop = easeOutBack(age / 0.12f);
            sf::Text t = makeText(*bold, m_hackWord, unit * 0.14f, m_hackWordColor);
            const sf::FloatRect b = t.getLocalBounds();
            t.setOrigin(b.position + b.size * 0.5f);
            const sf::Vector2f jitter = beat > 0.4f ? sf::Vector2f{(uni(m_rng) - 0.5f) * unit * 0.012f, (uni(m_rng) - 0.5f) * unit * 0.006f}
                                                    : sf::Vector2f{};
            // Stretched wide, but never past the edges of the screen.
            const float stretch = std::min(1.3f, m_size.x * 0.86f / std::max(1.f, b.size.x + unit * 0.05f));
            const float squash = std::min(1.f, stretch / 1.3f * 1.15f);
            const float halfW = (b.size.x * 0.5f + unit * 0.025f) * stretch;
            sf::Vector2f at = m_hackWordPos;
            at.x = std::clamp(at.x, halfW + m_size.x * 0.02f, m_size.x * 0.98f - halfW);
            sf::Transform tf;
            tf.translate(at + jitter).rotate(sf::degrees(m_hackWordAngle)).scale({stretch * pop, squash * pop});
            // Black label behind it.
            sf::VertexArray va(sf::PrimitiveType::Triangles);
            const float px = b.size.x * 0.5f + unit * 0.02f, py = b.size.y * 0.5f + unit * 0.018f;
            hackBox(va, tf, -px - 4.f, -py - 4.f, px + 4.f, py + 4.f, HackNeon[(static_cast<int>(m_hackPalette) + 1) % 2 == 0 ? 0 : 1]);
            hackBox(va, tf, -px, -py, px, py, sf::Color(8, 6, 14));
            layer.draw(va);
            const float split = unit * (0.004f + 0.012f * beat * glitch);
            sf::Text c1 = t;
            c1.setFillColor(sf::Color(0, 230, 255, 220));
            sf::Transform t1 = tf;
            t1.translate({-split, 0.f});
            layer.draw(c1, sf::RenderStates(t1));
            sf::Text c2 = t;
            c2.setFillColor(sf::Color(255, 40, 160, 220));
            sf::Transform t2 = tf;
            t2.translate({split, 0.f});
            layer.draw(c2, sf::RenderStates(t2));
            layer.draw(t, sf::RenderStates(tf));
        }
    }

    // ---- Top-right status: live tag, song, upload bar ----
    if (mono)
    {
        const float fs = std::max(10.f, unit * 0.022f);
        const float right = m_size.x * 0.97f;
        float y = m_size.y * 0.05f;
        auto label = [&](const sf::String& s, sf::Color fg, sf::Color bg)
        {
            sf::Text t = makeText(*mono, s, fs, fg);
            const sf::FloatRect b = t.getLocalBounds();
            const float x = right - b.size.x - b.position.x - fs * 0.4f;
            sf::VertexArray va(sf::PrimitiveType::Triangles);
            hackBox(va, sf::Transform(), x - fs * 0.4f, y - fs * 0.15f, right, y + fs * 1.25f, bg);
            layer.draw(va);
            t.setPosition({std::round(x), std::round(y)});
            layer.draw(t);
            y += fs * 1.6f;
        };
        const bool dot = std::fmod(m_time, 1.f) < 0.6f;
        const std::string clock = ui.duration > 0.f ? formatTime(ui.position) + " / " + formatTime(ui.duration) : "--:--";
        label(std::string(dot ? "(o) " : "    ") + "LIVE  " + clock, sf::Color(10, 8, 16), HackNeon[0]);
        if (!ui.title.isEmpty())
        {
            sf::String name = ui.title;
            if (name.getSize() > 34)
                name = name.substring(0, 33) + "...";
            label(sf::String("> ") + name, sf::Color(235, 235, 245), sf::Color(8, 6, 14, 220));
        }
        const float progress = ui.duration > 0.f ? std::clamp(ui.position / ui.duration, 0.f, 1.f) : 0.f;
        std::string barText = "UPLOAD [";
        for (int i = 0; i < 16; ++i)
            barText += i < static_cast<int>(progress * 16.f) ? '#' : '.';
        char pct[16];
        std::snprintf(pct, sizeof(pct), "] %3d%%", static_cast<int>(progress * 100.f));
        label(barText + pct, sf::Color(10, 8, 16), HackNeon[1]);
    }
    layer.display();

    // ---- Tear the whole frame up ----
    m_hijackGlitch.setUniform("source", sf::Shader::CurrentTexture);
    m_hijackGlitch.setUniform("resolution", sf::Glsl::Vec2{m_size.x, m_size.y});
    m_hijackGlitch.setUniform("seed", m_hackSeed);
    m_hijackGlitch.setUniform("beat", beat);
    m_hijackGlitch.setUniform("amount", glitch);
    m_hijackGlitch.setUniform("invert", m_hackInvert);
    m_scene.draw(sf::Sprite(layer.getTexture()), sf::RenderStates(&m_hijackGlitch));
}

namespace
{
const char* const RageWords[] = {u8"MÖRE", u8"GÖ", u8"LÖUDËR", u8"HËY", u8"YÜP", u8"WÖAH", u8"JÜMP", u8"TÜRN ÜP", u8"NÖ SLËEP"};
constexpr int RageWordCount = 9;
} // namespace

void Renderer::drawRage(const Spectrum& spectrum, float dt)
{
    loadHijackFonts();
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    const float quality = cfg("rage.quality");
    const float scale = std::min(1.f, std::sqrt(650000.f * quality * quality / (m_size.x * m_size.y)));
    const sf::Vector2u want{std::max(1u, static_cast<unsigned>(m_size.x * scale)),
                            std::max(1u, static_cast<unsigned>(m_size.y * scale))};
    if (m_rageTarget.getSize() != want && m_rageTarget.resize(want))
        m_rageTarget.setSmooth(true);

    const float beat = spectrum.beat();
    const float bass = spectrum.bass();
    const float energy = spectrum.energy();
    const float unit = std::min(m_size.x, m_size.y);
    const float strobeAmount = cfg("rage.strobe");
    const float shakeAmount = cfg("rage.shake");
    const float lastBeat = m_rageLastBeat;
    m_rageLastBeat = beat;
    auto crossed = [&](float level) { return beat > level && lastBeat <= level; };
    const bool kick = crossed(0.35f);
    const bool bigKick = crossed(0.6f);
    const bool hugeKick = crossed(0.82f);

    // The crowd jumps faster with the bass; the lights sweep faster when it's loud.
    m_rageJump += dt * (3.5f + 7.f * bass + 4.f * beat);
    m_rageSweep += dt * (0.35f + 1.5f * energy + 2.f * beat);

    // Strobes: a hit on kicks, and rapid flicker when the track is going hard.
    m_rageStrobe *= std::exp(-24.f * dt);
    if (kick && uni(m_rng) < 0.55f * strobeAmount)
        m_rageStrobe = 1.f;
    if (energy > 0.45f && uni(m_rng) < dt * 14.f * strobeAmount * (energy - 0.45f) * 2.f)
        m_rageStrobe = std::max(m_rageStrobe, 0.75f);
    m_rageStrobe = std::min(m_rageStrobe, strobeAmount);

    // Camera: punches and shakes on hits, blows out on the biggest ones.
    m_rageFlash *= std::exp(-8.f * dt);
    if (hugeKick && uni(m_rng) < 0.6f * strobeAmount)
        m_rageFlash = 1.f;
    m_rageKickShake *= std::exp(-7.f * dt);
    if (kick)
        m_rageKickShake = std::max(m_rageKickShake, 0.5f + beat);
    const float a = uni(m_rng) * 6.2832f;
    m_rageShake = sf::Vector2f{std::cos(a), std::sin(a)} * (m_rageKickShake * 0.018f * shakeAmount) +
                  sf::Vector2f{0.006f * std::sin(m_time * 1.7f), 0.005f * std::sin(m_time * 2.3f + 1.f)} * shakeAmount;
    const float zoom = 1.f + (0.05f * beat + 0.03f * m_rageKickShake) * shakeAmount;
    {
        const sf::Vector2f size(m_rageTarget.getSize());
        m_rage.setUniform("resolution", sf::Glsl::Vec2{size.x, size.y});
        m_rage.setUniform("time", m_time);
        m_rage.setUniform("bass", bass);
        m_rage.setUniform("beat", beat);
        m_rage.setUniform("energy", energy);
        m_rage.setUniform("strobe", m_rageStrobe);
        m_rage.setUniform("flash", m_rageFlash);
        m_rage.setUniform("beamCount", cfg("rage.beams"));
        m_rage.setUniform("crowd", cfg("rage.crowd"));
        m_rage.setUniform("faces", cfg("rage.faces"));
        m_rage.setUniform("grain", cfg("rage.grain"));
        m_rage.setUniform("jump", m_rageJump);
        m_rage.setUniform("sweep", m_rageSweep);
        m_rage.setUniform("shake", sf::Glsl::Vec2{m_rageShake.x, m_rageShake.y});
        m_rage.setUniform("zoom", zoom);
        m_rage.setUniform("audio", m_audioTexture);
        m_rage.setUniform("hasImage", m_hasImage ? 1.f : 0.f);
        if (m_hasImage)
        {
            const sf::Vector2f is(m_image.getSize());
            m_rage.setUniform("image", m_image);
            m_rage.setUniform("imageAspect", is.x / std::max(is.y, 1.f));
        }
        m_rageTarget.clear(sf::Color::Black);
        m_rageTarget.draw(sf::RectangleShape(size), sf::RenderStates(&m_rage));
        m_rageTarget.display();
    }
    {
        const sf::Vector2f size(m_rageTarget.getSize());
        sf::Sprite frame(m_rageTarget.getTexture());
        frame.setScale({m_size.x / size.x, m_size.y / size.y});
        m_scene.draw(frame);
    }

    // ---- Bell sparkles on the high notes ----
    const float treble = spectrum.treble();
    m_rageTrebleAvg += (treble - m_rageTrebleAvg) * (1.f - std::exp(-2.f * dt));
    const float spike = std::max(0.f, treble - m_rageTrebleAvg * 1.12f);
    const float bells = cfg("rage.bells");
    m_rageGlintAcc += dt * bells * (1.2f + 90.f * spike + 4.f * treble);
    if (kick)
        m_rageGlintAcc += bells * 1.5f;
    while (m_rageGlintAcc >= 1.f && m_rageGlints.size() < 48)
    {
        m_rageGlintAcc -= 1.f;
        RageGlint g;
        g.position = {m_size.x * (0.05f + 0.9f * uni(m_rng)), m_size.y * (0.04f + 0.6f * uni(m_rng))};
        g.size = unit * (0.02f + 0.045f * uni(m_rng));
        g.life = 0.22f + 0.3f * uni(m_rng);
        g.angle = (uni(m_rng) - 0.5f) * 0.5f;
        m_rageGlints.push_back(g);
    }
    m_rageGlintAcc = std::min(m_rageGlintAcc, 1.f);
    sf::VertexArray glints(sf::PrimitiveType::Triangles);
    for (auto& g : m_rageGlints)
    {
        g.age += dt;
        const float env = std::sin(Pi * std::clamp(g.age / g.life, 0.f, 1.f));
        const float c = std::cos(g.angle), s = std::sin(g.angle);
        auto diamond = [&](float len, float wid, float rot, sf::Color col)
        {
            const float rc = std::cos(rot), rs = std::sin(rot);
            auto at = [&](float x, float y)
            {
                const float xr = x * rc - y * rs, yr = x * rs + y * rc;
                return g.position + sf::Vector2f{xr * c - yr * s, xr * s + yr * c};
            };
            quad(glints, at(0.f, -len), at(wid, 0.f), at(0.f, len), at(-wid, 0.f), col, col, col, col);
        };
        const float L = g.size * env;
        const sf::Color gold(255, 214, 160, static_cast<std::uint8_t>(200 * env));
        const sf::Color white(255, 255, 255, static_cast<std::uint8_t>(255 * env));
        diamond(L, L * 0.09f, 0.f, gold);
        diamond(L, L * 0.09f, Pi * 0.5f, gold);
        diamond(L * 0.45f, L * 0.06f, Pi * 0.25f, gold);
        diamond(L * 0.45f, L * 0.06f, -Pi * 0.25f, gold);
        diamond(L * 0.22f, L * 0.22f, 0.f, white);
    }
    m_rageGlints.erase(std::remove_if(m_rageGlints.begin(), m_rageGlints.end(), [](const RageGlint& g) { return g.age > g.life; }),
                       m_rageGlints.end());
    m_scene.draw(glints, sf::RenderStates(sf::BlendAdd));

    // ---- Drop words ----
    m_rageWordLife = std::max(0.f, m_rageWordLife - dt);
    m_rageWordCooldown -= dt;
    if (cfg("rage.words") > 0.5f && m_rageWordCooldown <= 0.f && ((bigKick && uni(m_rng) < 0.5f) || (kick && m_rageWordCooldown < -4.f)))
    {
        const std::string w = RageWords[static_cast<int>(uni(m_rng) * RageWordCount) % RageWordCount];
        m_rageWord = sf::String::fromUtf8(w.begin(), w.end());
        m_rageWordMax = m_rageWordLife = 0.5f + 0.45f * uni(m_rng);
        m_rageWordCooldown = 2.f + 3.f * uni(m_rng);
    }
    const sf::Font* bold = m_hasBold ? &m_boldFont : m_font;
    if (m_rageWordLife > 0.f && bold)
    {
        const float age = m_rageWordMax - m_rageWordLife;
        const float punch = 1.f + 0.35f * std::exp(-age * 18.f);
        const float fade = std::min(1.f, m_rageWordLife / 0.12f);
        sf::Text t(*bold, m_rageWord, static_cast<unsigned>(unit * 0.2f));
        const sf::FloatRect b = t.getLocalBounds();
        t.setOrigin(b.position + b.size * 0.5f);
        const float fit = std::min(1.f, m_size.x * 0.85f / std::max(1.f, b.size.x));
        const sf::Vector2f center = m_size * 0.5f + sf::Vector2f{0.f, -m_size.y * 0.06f};
        // Smeared red copies behind it, jittering every frame.
        for (int i = 0; i < 3; ++i)
        {
            sf::Text r = t;
            r.setFillColor(sf::Color(255, 20, 10, static_cast<std::uint8_t>(150 * fade)));
            r.setPosition(center + sf::Vector2f{(uni(m_rng) - 0.5f) * unit * 0.04f, (uni(m_rng) - 0.5f) * unit * 0.02f});
            r.setScale(sf::Vector2f{1.f + 0.08f * static_cast<float>(i), 1.25f + 0.1f * static_cast<float>(i)} * punch * fit);
            m_scene.draw(r, sf::RenderStates(sf::BlendAdd));
        }
        t.setFillColor(sf::Color(255, 250, 245, static_cast<std::uint8_t>(255 * fade)));
        t.setOutlineColor(sf::Color(0, 0, 0, static_cast<std::uint8_t>(220 * fade)));
        t.setOutlineThickness(unit * 0.004f);
        t.setPosition(center + m_rageShake * unit * 2.f);
        t.setScale(sf::Vector2f{1.f, 1.25f} * punch * fit);
        m_scene.draw(t);
    }
}

void Renderer::drawDystopia(const Spectrum& spectrum, float dt)
{
    loadHijackFonts();
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    const float quality = cfg("dys.quality");
    const float scale = std::min(1.f, std::sqrt(600000.f * quality * quality / (m_size.x * m_size.y)));
    const sf::Vector2u want{std::max(1u, static_cast<unsigned>(m_size.x * scale)),
                            std::max(1u, static_cast<unsigned>(m_size.y * scale))};
    if (m_dysTarget.getSize() != want && m_dysTarget.resize(want))
        m_dysTarget.setSmooth(true);

    const float beat = spectrum.beat();
    const float lastBeat = m_dysLastBeat;
    m_dysLastBeat = beat;
    const bool kick = beat > 0.35f && lastBeat <= 0.35f;
    const bool hugeKick = beat > 0.8f && lastBeat <= 0.8f;
    const float flashAmount = cfg("dys.flash");

    // A slow, heavy glide that surges with the bass.
    m_dysBass += (spectrum.bass() - m_dysBass) * (1.f - std::exp(-3.f * dt));
    m_dysTravel += dt * cfg("dys.speed") * (9.f + 16.f * m_dysBass);
    // Rooftop lights all flash together on kicks; the whole city flashes red on the biggest hits.
    m_dysBlink *= std::exp(-7.f * dt);
    if (kick)
        m_dysBlink = 1.f;
    m_dysFlash *= std::exp(-6.f * dt);
    if (hugeKick && uni(m_rng) < 0.7f * flashAmount)
        m_dysFlash = std::min(1.f, flashAmount);
    m_dysKickAge += dt;
    if (kick)
        m_dysKickAge = 0.f;

    const sf::Vector2f size(m_dysTarget.getSize());
    m_dystopia.setUniform("resolution", sf::Glsl::Vec2{size.x, size.y});
    m_dystopia.setUniform("time", m_time);
    m_dystopia.setUniform("travel", m_dysTravel);
    m_dystopia.setUniform("bass", m_dysBass);
    m_dystopia.setUniform("beat", beat);
    m_dystopia.setUniform("energy", spectrum.energy());
    m_dystopia.setUniform("blink", m_dysBlink);
    m_dystopia.setUniform("flash", m_dysFlash);
    // Color scheme.
    {
        auto vec = [](float r, float g, float b) { return sf::Glsl::Vec3{r, g, b}; };
        auto fromColor = [](sf::Color c) { return sf::Glsl::Vec3{c.r / 255.f, c.g / 255.f, c.b / 255.f}; };
        sf::Glsl::Vec3 main = vec(0.25f, 1.f, 0.4f), alt = vec(0.2f, 0.85f, 1.f), warn = vec(1.f, 0.08f, 0.05f);
        float smogK = 0.5f;
        switch (static_cast<int>(std::lround(cfg("dys.colors"))))
        {
            case 1:   // drifts through green, cyan and purple, and jumps on the biggest hits
                if (hugeKick)
                    m_dysHue += 0.17f + 0.2f * uni(m_rng);
                m_dysHue += dt * 0.012f;
                main = fromColor(hsv(0.36f + 0.22f * std::sin(m_dysHue * 6.2832f), 0.85f, 1.f));
                alt = fromColor(hsv(0.36f + 0.22f * std::sin(m_dysHue * 6.2832f) + 0.25f, 0.7f, 1.f));
                smogK = 0.55f;
                break;
            case 2:
                main = vec(0.1f, 0.85f, 1.f);
                alt = vec(1.f, 0.2f, 0.75f);
                smogK = 0.6f;
                break;
            case 3:
                main = vec(1.f, 0.06f, 0.03f);
                alt = vec(0.8f, 0.88f, 1.f);
                warn = vec(1.f, 0.06f, 0.03f);
                smogK = 1.f;
                break;
            default:
                break;
        }
        m_dysMain = sf::Color(static_cast<std::uint8_t>(main.x * 255.f), static_cast<std::uint8_t>(main.y * 255.f),
                              static_cast<std::uint8_t>(main.z * 255.f));
        m_dystopia.setUniform("mainCol", main);
        m_dystopia.setUniform("altCol", alt);
        m_dystopia.setUniform("warnCol", warn);
        m_dystopia.setUniform("smogK", smogK);
    }
    m_dystopia.setUniform("lights", cfg("dys.lights"));
    m_dystopia.setUniform("fogAmount", cfg("dys.fog"));
    m_dystopia.setUniform("screens", cfg("dys.screens"));
    m_dystopia.setUniform("sway", sf::Glsl::Vec2{2.2f * std::sin(m_time * 0.11f) + 0.8f * std::sin(m_time * 0.29f),
                                                 2.5f * std::sin(m_time * 0.07f + 1.f)});
    m_dystopia.setUniform("kickAge", m_dysKickAge);
    m_dystopia.setUniform("audio", m_audioTexture);
    m_dystopia.setUniform("hasImage", m_hasImage ? 1.f : 0.f);
    if (m_hasImage)
    {
        const sf::Vector2f is(m_image.getSize());
        m_dystopia.setUniform("image", m_image);
        m_dystopia.setUniform("imageAspect", is.x / std::max(is.y, 1.f));
    }
    m_dysTarget.clear(sf::Color::Black);
    m_dysTarget.draw(sf::RectangleShape(size), sf::RenderStates(&m_dystopia));
    m_dysTarget.display();
    sf::Sprite frame(m_dysTarget.getTexture());
    frame.setScale({m_size.x / size.x, m_size.y / size.y});
    m_scene.draw(frame);

    // ---- Surveillance HUD: thin corner brackets and cold status text ----
    const sf::Font* mono = m_hasMono ? &m_monoFont : m_font;
    if (cfg("dys.hud") < 0.5f || !mono)
        return;
    const float unit = std::min(m_size.x, m_size.y);
    const sf::Color cold(215, 225, 240, 150);
    const sf::Color red(255, 40, 30, 200);
    sf::VertexArray lines(sf::PrimitiveType::Triangles);
    const float m = unit * 0.045f, len = unit * 0.07f, th = std::max(1.f, unit * 0.0022f);
    auto bar = [&](float x0, float y0, float x1, float y1, sf::Color c) { rect(lines, x0, y0, x1, y1, c, c); };
    const float X0 = m, Y0 = m, X1 = m_size.x - m, Y1 = m_size.y - m;
    bar(X0, Y0, X0 + len, Y0 + th, cold); bar(X0, Y0, X0 + th, Y0 + len, cold);
    bar(X1 - len, Y0, X1, Y0 + th, cold); bar(X1 - th, Y0, X1, Y0 + len, cold);
    bar(X0, Y1 - th, X0 + len, Y1, cold); bar(X0, Y1 - len, X0 + th, Y1, cold);
    bar(X1 - len, Y1 - th, X1, Y1, cold); bar(X1 - th, Y1 - len, X1, Y1, cold);
    // Signal meter.
    const float energy = std::clamp(spectrum.energy() * 1.6f, 0.f, 1.f);
    const float mw = unit * 0.22f, mh = unit * 0.01f, mx = X1 - mw - unit * 0.02f, my = Y0 + unit * 0.07f;
    bar(mx, my, mx + mw, my + mh, sf::Color(215, 225, 240, 40));
    bar(mx, my, mx + mw * energy, my + mh, sf::Color(m_dysMain.r, m_dysMain.g, m_dysMain.b, 210));
    m_scene.draw(lines);

    const float fs = std::max(10.f, unit * 0.019f);
    auto text = [&](const std::string& s, float x, float y, sf::Color c, bool alignRight)
    {
        sf::Text t(*mono, s, static_cast<unsigned>(fs));
        t.setFillColor(c);
        const sf::FloatRect b = t.getLocalBounds();
        t.setPosition({std::round(alignRight ? x - b.size.x - b.position.x : x), std::round(y)});
        m_scene.draw(t);
    };
    const bool dot = std::fmod(m_time, 1.2f) < 0.7f;
    const float tx = X0 + unit * 0.02f, ty = Y0 + unit * 0.02f;
    text("SECTOR 09 // NIGHT CYCLE", tx, ty, cold, false);
    text("CURFEW IN EFFECT", tx, ty + fs * 1.5f, red, false);
    text(std::string(dot ? "(o) " : "    ") + "MONITORED", X1 - unit * 0.02f, ty, red, true);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "SIGNAL %03d", static_cast<int>(energy * 999.f));
    text(buf, X1 - unit * 0.02f, my + mh + fs * 0.5f, cold, true);
    std::snprintf(buf, sizeof(buf), "AIR QUALITY  CRITICAL      COMPLIANCE %.1f%%", 97.f + 2.9f * std::fabs(std::sin(m_time * 0.37f)));
    text(buf, tx, Y1 - unit * 0.02f - fs * 1.4f, cold, false);
    std::snprintf(buf, sizeof(buf), "ALT %04dM  //  GRID %05d", 340 + static_cast<int>(25.f * std::sin(m_time * 0.07f)),
                  static_cast<int>(m_dysTravel * 0.5f) % 100000);
    text(buf, X1 - unit * 0.02f, Y1 - unit * 0.02f - fs * 1.4f, cold, true);
}

void Renderer::drawNightDrive(const Spectrum& spectrum, float dt, const UiState& ui)
{
    // Render size follows the quality setting, then the frame is scaled up smoothly.
    const float quality = cfg("drive.quality");
    const float scale = std::min(1.f, std::sqrt(900000.f * quality * quality / (m_size.x * m_size.y)));
    const sf::Vector2u want{std::max(1u, static_cast<unsigned>(m_size.x * scale)),
                            std::max(1u, static_cast<unsigned>(m_size.y * scale))};
    if (m_driveTarget.getSize() != want && m_driveTarget.resize(want))
        m_driveTarget.setSmooth(true);

    // Speed: cruising, pushed harder by the bass.
    m_driveBass += (spectrum.bass() - m_driveBass) * (1.f - std::exp(-3.f * dt));
    const float speed = 28.f * cfg("drive.speed") * (0.75f + 0.6f * m_driveBass + 0.2f * spectrum.energy());
    m_driveTravel += dt * speed;
    const float curve = 0.55f * std::sin(m_driveTravel * 0.0021f) + 0.3f * std::sin(m_driveTravel * 0.0057f + 1.f);

    const sf::Vector2f size(m_driveTarget.getSize());
    m_drive.setUniform("resolution", sf::Glsl::Vec2{size.x, size.y});
    m_drive.setUniform("time", m_time);
    m_drive.setUniform("travel", m_driveTravel);
    m_drive.setUniform("curveNow", curve);
    // The wheel turns the way the road bends (right bend -> wheel turns right), a touch behind
    // the road so it feels like steering, with tiny corrections on top.
    m_wheel += (curve * 0.95f - m_wheel) * (1.f - std::exp(-2.5f * dt));
    m_drive.setUniform("wheelAngle", m_wheel + 0.02f * std::sin(m_time * 1.3f));
    m_drive.setUniform("beat", spectrum.beat());
    m_drive.setUniform("energy", spectrum.energy());
    m_drive.setUniform("speedNorm", std::clamp(speed / 60.f, 0.f, 1.f));
    m_drive.setUniform("rain", cfg("drive.rain"));
    m_drive.setUniform("traffic", cfg("drive.traffic"));
    m_drive.setUniform("cityAmount", cfg("drive.city"));
    m_drive.setUniform("lampGain", cfg("drive.lamps"));
    m_drive.setUniform("dashOn", cfg("drive.dash"));
    m_drive.setUniform("audio", m_audioTexture);
    m_drive.setUniform("hasImage", m_hasImage ? 1.f : 0.f);
    if (m_hasImage)
    {
        const sf::Vector2f imageSize(m_image.getSize());
        m_drive.setUniform("image", m_image);
        m_drive.setUniform("imageAspect", imageSize.x / std::max(imageSize.y, 1.f));
    }
    m_driveTarget.clear(sf::Color::Black);
    m_driveTarget.draw(sf::RectangleShape(size), sf::RenderStates(&m_drive));
    m_driveTarget.display();
    sf::Sprite frame(m_driveTarget.getTexture());
    frame.setScale({m_size.x / size.x, m_size.y / size.y});
    m_scene.draw(frame);

    if (cfg("drive.dash") > 0.5f)
        drawRadioText(ui);
}

void Renderer::drawRadioText(const UiState& ui)
{
    if (!m_font)
        return;
    // Same layout numbers as the screen in the shader (height units, y up from the bottom).
    const float aspect = m_size.x / m_size.y;
    const float halfW = 0.5f * aspect;
    auto toPixels = [&](float x, float y) { return sf::Vector2f{(x / aspect + 0.5f) * m_size.x, (1.f - y) * m_size.y}; };
    const float scX = halfW * 0.25f;
    const float scY = 0.12f;
    const sf::Vector2f topLeft = toPixels(scX - 0.148f + 0.112f, scY + 0.052f);
    const sf::Vector2f bottomRight = toPixels(scX + 0.148f, scY - 0.012f);
    const float areaW = bottomRight.x - topLeft.x;
    const float unit = m_size.y;
    if (areaW < 20.f)
        return;

    const sf::Color bright(215, 245, 255);
    const sf::Color dim(120, 175, 200);
    const auto titleSize = static_cast<unsigned>(std::max(9.f, unit * 0.021f));
    const auto smallSize = static_cast<unsigned>(std::max(8.f, unit * 0.015f));

    // Title: scrolls like a car radio when it's too long to fit.
    const sf::String title = ui.title.isEmpty() ? sf::String("No song") : ui.title;
    sf::Text text(*m_font, title, titleSize);
    text.setFillColor(bright);
    const float textW = text.getLocalBounds().size.x;
    const sf::Vector2u clip{static_cast<unsigned>(areaW), static_cast<unsigned>(titleSize * 1.6f)};
    if (m_radioText.getSize() != clip)
    {
        if (!m_radioText.resize(clip))
            return;
        m_radioText.setSmooth(true);
    }
    m_radioText.clear(sf::Color::Transparent);
    float offset = 0.f;
    if (textW > areaW)
    {
        const float gap = areaW * 0.4f;
        const float loop = textW + gap;
        offset = -std::fmod(m_time * unit * 0.05f, loop);
        sf::Text second = text;
        second.setPosition({std::round(offset + loop), 0.f});
        m_radioText.draw(second);
    }
    text.setPosition({std::round(offset), 0.f});
    m_radioText.draw(text);
    m_radioText.display();
    sf::Sprite titleSprite(m_radioText.getTexture());
    titleSprite.setPosition({std::round(topLeft.x), std::round(topLeft.y)});
    m_scene.draw(titleSprite);

    // Times and a progress bar.
    const float barY = topLeft.y + titleSize * 1.75f;
    const float fraction = ui.duration > 0.f ? std::clamp(ui.position / ui.duration, 0.f, 1.f) : 0.f;
    sf::RectangleShape track({areaW, std::max(2.f, unit * 0.003f)});
    track.setPosition({topLeft.x, std::round(barY)});
    track.setFillColor(sf::Color(255, 255, 255, 45));
    m_scene.draw(track);
    sf::RectangleShape fill({areaW * fraction, track.getSize().y});
    fill.setPosition(track.getPosition());
    fill.setFillColor(sf::Color(90, 220, 255));
    m_scene.draw(fill);

    sf::Text now(*m_font, formatTime(ui.position), smallSize);
    now.setFillColor(dim);
    now.setPosition({std::round(topLeft.x), std::round(barY + unit * 0.007f)});
    m_scene.draw(now);
    sf::Text total(*m_font, formatTime(ui.duration), smallSize);
    total.setFillColor(dim);
    total.setPosition({std::round(topLeft.x + areaW - total.getLocalBounds().size.x), std::round(barY + unit * 0.007f)});
    m_scene.draw(total);
}

void Renderer::drawPop(const Spectrum& spectrum, float dt)
{
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    const float beat = spectrum.beat();
    const float unit = std::min(m_size.x, m_size.y);
    const float ink = cfg("pop.outline") * unit / 700.f;

    // ---- Background ----
    m_pop.setUniform("resolution", sf::Glsl::Vec2{m_size.x, m_size.y});
    m_pop.setUniform("time", m_time);
    m_pop.setUniform("bass", spectrum.bass());
    m_pop.setUniform("beat", beat);
    m_pop.setUniform("cloudSpeed", cfg("pop.clouds"));
    m_pop.setUniform("hasImage", m_hasImage ? 1.f : 0.f);
    m_pop.setUniform("ink", cfg("pop.outline"));
    if (m_hasImage)
    {
        m_pop.setUniform("image", m_image);
        m_pop.setUniform("fitScale", imageFitScale());
    }
    // The biggest live starbursts and rings push the picture around them.
    {
        sf::Glsl::Vec4 warps[8];
        int count = 0;
        const float amount = cfg("pop.warp");
        for (auto it = m_pops.rbegin(); it != m_pops.rend() && count < 8; ++it)
        {
            const PopShape& s = *it;
            const float age = 1.f - s.life / s.maxLife;
            const float y = m_size.y - s.position.y; // shader pixels count up from the bottom
            if (s.kind == 0)
            {
                const float strength = (age < 0.15f ? age / 0.15f : 1.f - (age - 0.15f) / 0.85f) * 0.9f * amount;
                warps[count++] = {s.position.x, y, s.size * 1.6f, std::max(strength, 0.001f)};
            }
            else if (s.kind == 1)
            {
                const float r = s.size + age * unit * 0.75f;
                warps[count++] = {s.position.x, y, r, -(1.f - age) * amount};
            }
        }
        for (int i = count; i < 8; ++i)
            warps[i] = {0.f, 0.f, 1.f, 0.f};
        m_pop.setUniformArray("warps", warps, 8);
    }
    m_scene.draw(sf::RectangleShape(m_size), sf::RenderStates(&m_pop));

    // ---- Spawn shapes ----
    const bool kick = beat > 0.32f && m_popLastBeat <= 0.32f;
    m_popLastBeat = beat;
    auto pick = [&] { return PopPalette[static_cast<int>(uni(m_rng) * PopPaletteSize) % PopPaletteSize]; };

    if (kick && uni(m_rng) < cfg("pop.bursts"))
    {
        PopShape s;
        s.kind = 0;
        s.position = {m_size.x * (0.12f + 0.76f * uni(m_rng)), m_size.y * (0.12f + 0.5f * uni(m_rng))};
        s.size = unit * (0.08f + 0.1f * uni(m_rng)) * (0.7f + beat * 0.5f);
        s.maxLife = s.life = 0.9f + 0.5f * uni(m_rng);
        s.spin = (uni(m_rng) - 0.5f) * 2.f;
        s.angle = uni(m_rng) * 6.28f;
        s.colors[0] = pick(); s.colors[1] = pick(); s.colors[2] = pick();
        for (int k = 0; k < 14; ++k)
            s.jitter.push_back(0.75f + 0.5f * uni(m_rng));
        m_pops.push_back(s);
    }
    m_popRingTimer -= dt;
    if ((kick || m_popRingTimer <= 0.f) && cfg("pop.rings") > 0.f)
    {
        PopShape s;
        s.kind = 1;
        s.position = m_size * 0.5f + sf::Vector2f{(uni(m_rng) - 0.5f) * m_size.x * 0.3f, (uni(m_rng) - 0.5f) * m_size.y * 0.2f};
        s.size = unit * 0.02f;
        s.maxLife = s.life = 2.2f;
        s.squash = 0.45f + 0.5f * uni(m_rng);
        s.angle = uni(m_rng) * 3.14f;
        s.colors[0] = pick();
        m_pops.push_back(s);
        m_popRingTimer = 1.4f / cfg("pop.rings");
    }
    m_popConfetti += dt * cfg("pop.confetti") * (4.f + 22.f * spectrum.energy() + 40.f * beat);
    while (m_popConfetti >= 1.f && m_pops.size() < 400)
    {
        m_popConfetti -= 1.f;
        PopShape s;
        s.kind = uni(m_rng) < 0.25f ? 3 : 2;   // sparkle or confetti
        s.position = {m_size.x * uni(m_rng), -20.f};
        s.velocity = {(uni(m_rng) - 0.5f) * 60.f, 80.f + 140.f * uni(m_rng)};
        s.size = unit * (s.kind == 3 ? 0.014f + 0.016f * uni(m_rng) : 0.008f + 0.01f * uni(m_rng));
        s.maxLife = s.life = 8.f;
        s.spin = (uni(m_rng) - 0.5f) * 6.f;
        s.angle = uni(m_rng) * 6.28f;
        s.sides = 3 + static_cast<int>(uni(m_rng) * 3.f);
        s.colors[0] = pick();
        m_pops.push_back(s);
    }
    m_popConfetti = std::min(m_popConfetti, 1.f);

    // ---- Update and draw (outline first, then color, so every shape gets a bold ink line) ----
    sf::VertexArray back(sf::PrimitiveType::Triangles), front(sf::PrimitiveType::Triangles);
    for (auto& s : m_pops)
    {
        s.life -= dt;
        s.angle += s.spin * dt;
        s.position += s.velocity * dt;
        const float age = 1.f - s.life / s.maxLife;
        switch (s.kind)
        {
            case 0: // starburst: pops in, wobbles, shrinks away
            {
                const float pop = age < 0.15f ? age / 0.15f * 1.15f : 1.15f - 0.15f * std::min(1.f, (age - 0.15f) * 4.f);
                const float shrink = age > 0.7f ? 1.f - (age - 0.7f) / 0.3f : 1.f;
                const float r = s.size * pop * shrink * (1.f + 0.08f * beat);
                if (r <= 1.f)
                    break;
                burst(back, s.position, r + ink * 2.2f, r * 0.55f + ink * 2.2f, 14, s.angle, Ink, s.jitter);
                burst(front, s.position, r, r * 0.55f, 14, s.angle, s.colors[0], s.jitter);
                burst(front, s.position, r * 0.68f, r * 0.38f, 14, s.angle + 0.1f, s.colors[1], s.jitter);
                disc(front, s.position, r * 0.3f, s.colors[2]);
                break;
            }
            case 1: // ring: expands outward and fades by thinning
            {
                const float r = s.size + age * unit * 0.75f;
                const float w = unit * 0.03f * (1.f - age) + 1.f;
                const float squash = s.squash;
                ring(back, s.position, r, w + ink * 2.5f, squash, Ink);
                ring(front, s.position, r, w, squash, s.colors[0]);
                break;
            }
            case 2: // confetti polygon
            {
                const float r = s.size;
                std::vector<float> none;
                sf::VertexArray& b = back;
                for (int pass = 0; pass < 2; ++pass)
                {
                    const float rr = pass == 0 ? r + ink * 1.6f : r;
                    const sf::Color col = pass == 0 ? Ink : s.colors[0];
                    sf::VertexArray& target = pass == 0 ? b : front;
                    for (int k = 0; k < s.sides; ++k)
                    {
                        const float a0 = s.angle + static_cast<float>(k) / static_cast<float>(s.sides) * 6.2831853f;
                        const float a1 = s.angle + static_cast<float>(k + 1) / static_cast<float>(s.sides) * 6.2831853f;
                        target.append({s.position, col});
                        target.append({s.position + sf::Vector2f{std::cos(a0), std::sin(a0)} * rr, col});
                        target.append({s.position + sf::Vector2f{std::cos(a1), std::sin(a1)} * rr, col});
                    }
                }
                break;
            }
            default: // four-point sparkle that twinkles
            {
                const float tw = 0.6f + 0.4f * std::sin(m_time * 8.f + s.angle * 5.f);
                const float r = s.size * tw * (1.f + beat * 0.6f);
                std::vector<float> none;
                burst(back, s.position, r + ink * 1.8f, r * 0.28f + ink * 1.8f, 4, s.angle * 0.2f, Ink, none);
                burst(front, s.position, r, r * 0.28f, 4, s.angle * 0.2f, sf::Color(255, 252, 235), none);
                break;
            }
        }
    }
    m_pops.erase(std::remove_if(m_pops.begin(), m_pops.end(),
                                [&](const PopShape& s) { return s.life <= 0.f || s.position.y > m_size.y + 40.f; }),
                 m_pops.end());

    // ---- Candy spectrum arch along the bottom ----
    if (cfg("pop.archOn") < 0.5)
    {
        m_scene.draw(back);
        m_scene.draw(front);
        return;
    }
    const auto bars = resample(spectrum.bands(), 22);
    const sf::Vector2f base{m_size.x * 0.5f, m_size.y * 1.02f};
    const float inner = unit * 0.2f;
    for (std::size_t i = 0; i < bars.size(); ++i)
    {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(bars.size());
        const float a = 3.14159265f + t * 3.14159265f;
        const sf::Vector2f dir{std::cos(a), std::sin(a)};
        const sf::Vector2f perp{-dir.y, dir.x};
        const float len = unit * (0.03f + 0.32f * bars[i]) * cfg("pop.arch");
        const float half = inner * 3.14159f / static_cast<float>(bars.size()) * 0.3f;
        const sf::Vector2f a0 = base + dir * inner, a1 = base + dir * (inner + len);
        const sf::Color col = PopPalette[i % 5];
        quad(back, a0 + perp * (half + ink * 1.5f) - dir * ink, a1 + perp * (half + ink * 1.5f) + dir * ink * 1.5f,
             a1 - perp * (half + ink * 1.5f) + dir * ink * 1.5f, a0 - perp * (half + ink * 1.5f) - dir * ink, Ink, Ink, Ink, Ink);
        quad(front, a0 + perp * half, a1 + perp * half, a1 - perp * half, a0 - perp * half, col, col, col, col);
        disc(back, a1, half + ink * 1.5f, Ink, 16);
        disc(front, a1, half, col, 16);
    }
    disc(back, base, inner * 0.8f + ink * 1.5f, Ink, 48);
    disc(front, base, inner * 0.8f, sf::Color(255, 246, 222), 48);
    ring(front, base, inner * 0.55f, inner * 0.12f * (1.f + beat), 1.f, PopPalette[0], 48);

    m_scene.draw(back);
    m_scene.draw(front);
}

void Renderer::drawDarkFantasy(const Spectrum& spectrum, float dt)
{
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    const float beat = spectrum.beat();

    // ---- Lightning: big kicks can call down a bolt ----
    m_flash *= std::exp(-7.f * dt);
    m_lightningCooldown -= dt;
    const float lightning = cfg("fantasy.lightning");
    if (lightning > 0.f && beat > 0.55f && m_lightningCooldown <= 0.f && uni(m_rng) < 0.55f * lightning)
    {
        m_bolts.clear();
        // Midpoint displacement makes a jagged bolt; a couple of branches split off it.
        std::vector<sf::Vector2f> main{{m_size.x * (0.15f + 0.7f * uni(m_rng)), -10.f},
                                       {m_size.x * (0.15f + 0.7f * uni(m_rng)), m_size.y * (0.3f + 0.2f * uni(m_rng))}};
        for (int pass = 0; pass < 6; ++pass)
        {
            std::vector<sf::Vector2f> next;
            for (std::size_t i = 0; i + 1 < main.size(); ++i)
            {
                const sf::Vector2f a = main[i], b = main[i + 1];
                const float len = std::sqrt((b - a).x * (b - a).x + (b - a).y * (b - a).y);
                next.push_back(a);
                next.push_back((a + b) * 0.5f + sf::Vector2f{(uni(m_rng) - 0.5f) * len * 0.45f, 0.f});
            }
            next.push_back(main.back());
            main.swap(next);
        }
        m_bolts.push_back(main);
        for (int branch = 0; branch < 2; ++branch)
        {
            const std::size_t from = main.size() / 4 + static_cast<std::size_t>(uni(m_rng) * static_cast<float>(main.size() / 2));
            std::vector<sf::Vector2f> b{main[from]};
            sf::Vector2f dir{(uni(m_rng) - 0.5f) * 60.f, 25.f + uni(m_rng) * 20.f};
            for (int k = 0; k < 10; ++k)
            {
                b.push_back(b.back() + dir + sf::Vector2f{(uni(m_rng) - 0.5f) * 30.f, 0.f});
                dir *= 0.92f;
            }
            m_bolts.push_back(b);
        }
        m_boltLife = 0.35f;
        m_flash = 1.f;
        m_lightningCooldown = 0.9f + uni(m_rng) * 1.5f;
    }
    m_boltLife -= dt;

    // ---- The scene: a 3D fly-through of the village, or the loaded picture ----
    if (!m_hasImage)
    {
        // Render quality: how many pixels the 3D street is traced at before being scaled up.
        const float quality = cfg("fantasy.quality");
        const float scale3D = std::min(1.f, std::sqrt(450000.f * quality * quality / (m_size.x * m_size.y)));
        const sf::Vector2u want{std::max(1u, static_cast<unsigned>(m_size.x * scale3D)),
                                std::max(1u, static_cast<unsigned>(m_size.y * scale3D))};
        if (m_fantasy3DTarget.getSize() != want && m_fantasy3DTarget.resize(want))
            m_fantasy3DTarget.setSmooth(true);
    }
    sf::Shader& shader = m_hasImage ? m_fantasy : m_fantasy3D;
    sf::RenderTexture& target = m_hasImage ? m_fantasyTarget : m_fantasy3DTarget;
    m_camZ += dt * cfg("fantasy.speed") * (2.2f + 3.5f * spectrum.bass() + 2.f * beat);
    const sf::Vector2f size(target.getSize());
    shader.setUniform("resolution", sf::Glsl::Vec2{size.x, size.y});
    shader.setUniform("time", m_time);
    shader.setUniform("bass", spectrum.bass());
    shader.setUniform("beat", beat);
    shader.setUniform("energy", spectrum.energy());
    shader.setUniform("treble", spectrum.treble());
    shader.setUniform("flash", m_flash);
    shader.setUniform("audio", m_audioTexture);
    if (m_hasImage)
    {
        shader.setUniform("hasImage", 1.f);
        shader.setUniform("image", m_image);
        shader.setUniform("fitScale", imageFitScale());
    }
    else
    {
        shader.setUniform("camZ", m_camZ);
        shader.setUniform("windowGain", cfg("fantasy.windows"));
        shader.setUniform("sway", 0.f);
    }
    shader.setUniform("moonSize", cfg("fantasy.moon"));
    shader.setUniform("castleHeight", cfg("fantasy.castle"));
    shader.setUniform("towerCount", cfg("fantasy.towers"));
    shader.setUniform("fogAmount", cfg("fantasy.fog"));
    shader.setUniform("cloudSpeed", cfg("fantasy.clouds"));
    target.clear(sf::Color::Black);
    sf::RenderStates sceneStates(&shader);
    sceneStates.blendMode = sf::BlendNone; // keep the shader's alpha (sky mask) as-is
    target.draw(sf::RectangleShape(size), sceneStates);
    target.display();
    sf::Sprite scene(target.getTexture());
    scene.setScale({m_size.x / size.x, m_size.y / size.y});
    // Copy the colors but keep the frame opaque (the shader's alpha is only a sky mask).
    const sf::BlendMode copyColor(sf::BlendMode::Factor::One, sf::BlendMode::Factor::Zero, sf::BlendMode::Equation::Add,
                                  sf::BlendMode::Factor::Zero, sf::BlendMode::Factor::One, sf::BlendMode::Equation::Add);
    m_scene.draw(scene, sf::RenderStates(copyColor));

    // Bats and lightning go on a separate layer that is cut out by the sky mask.
    auto composite = [&](bool additive)
    {
        m_overlayLayer.display();
        m_skyMask.setUniform("source", sf::Shader::CurrentTexture);
        m_skyMask.setUniform("mask", target.getTexture());
        m_skyMask.setUniform("additive", additive ? 1.f : 0.f);
        sf::RenderStates states(&m_skyMask);
        states.blendMode = additive ? sf::BlendAdd : sf::BlendAlpha;
        m_scene.draw(sf::Sprite(m_overlayLayer.getTexture()), states);
    };

    // ---- Bats: a flock that flutters across the sky, scattering on kicks ----
    const float bats = cfg("fantasy.bats");
    m_batSpawn += dt * bats * (1.5f + 30.f * beat * beat);
    while (m_batSpawn >= 1.f && m_bats.size() < 160)
    {
        m_batSpawn -= 1.f;
        Bat bat;
        const bool fromLeft = uni(m_rng) < 0.5f;
        bat.position = {fromLeft ? -20.f : m_size.x + 20.f, m_size.y * (0.08f + 0.45f * uni(m_rng))};
        bat.velocity = {(fromLeft ? 1.f : -1.f) * (90.f + 160.f * uni(m_rng)), (uni(m_rng) - 0.6f) * 40.f};
        bat.size = 5.f + 9.f * uni(m_rng);
        bat.phase = uni(m_rng) * 10.f;
        m_bats.push_back(bat);
    }
    m_batSpawn = std::min(m_batSpawn, 1.f);

    sf::VertexArray wings(sf::PrimitiveType::Triangles);
    const sf::Color dark(4, 5, 12);
    for (auto& bat : m_bats)
    {
        bat.phase += dt * (14.f + 10.f * beat);
        bat.position += bat.velocity * dt * (1.f + beat * 0.8f);
        bat.position.y += std::sin(bat.phase * 0.35f) * 18.f * dt;
        const float flap = std::sin(bat.phase) * 0.7f;
        const float s = bat.size;
        const sf::Vector2f body = bat.position;
        const sf::Vector2f tipL = body + sf::Vector2f{-s, -s * flap};
        const sf::Vector2f tipR = body + sf::Vector2f{s, -s * flap};
        const sf::Vector2f down{0.f, s * 0.35f};
        wings.append({tipL, dark});
        wings.append({body, dark});
        wings.append({body + down, dark});
        wings.append({tipR, dark});
        wings.append({body, dark});
        wings.append({body + down, dark});
    }
    m_bats.erase(std::remove_if(m_bats.begin(), m_bats.end(),
                                [&](const Bat& b) { return b.position.x < -40.f || b.position.x > m_size.x + 40.f; }),
                 m_bats.end());
    m_overlayLayer.clear(sf::Color::Transparent);
    m_overlayLayer.draw(wings);
    composite(false);

    // ---- The bolt itself, flickering as it fades ----
    if (m_boltLife > 0.f && !m_bolts.empty())
    {
        const float flicker = (std::sin(m_time * 90.f) > -0.3f ? 1.f : 0.35f) * std::min(1.f, m_boltLife / 0.2f);
        sf::VertexArray bolt(sf::PrimitiveType::Triangles);
        for (std::size_t i = 0; i < m_bolts.size(); ++i)
        {
            const float width = i == 0 ? 1.f : 0.6f;
            const auto& pts = m_bolts[i];
            thickLine(bolt, pts, std::vector<sf::Color>(pts.size(), withAlpha(sf::Color(70, 110, 255), 0.55f * flicker)), 14.f * width, false);
            thickLine(bolt, pts, std::vector<sf::Color>(pts.size(), withAlpha(sf::Color(150, 185, 255), 0.8f * flicker)), 5.f * width, false);
            thickLine(bolt, pts, std::vector<sf::Color>(pts.size(), withAlpha(sf::Color(255, 255, 255), flicker)), 2.f * width, false);
        }
        m_overlayLayer.clear(sf::Color::Transparent);
        m_overlayLayer.draw(bolt, sf::RenderStates(sf::BlendAdd));
        composite(true);
    }
}

void Renderer::drawLiquid(const Spectrum& spectrum)
{
    sf::RenderTexture& current = m_liquidTargets[m_liquidIndex];
    const sf::RenderTexture& previous = m_liquidTargets[1 - m_liquidIndex];
    const sf::Vector2f target(current.getSize());

    m_liquid.setUniform("resolution", sf::Glsl::Vec2{target.x, target.y});
    m_liquid.setUniform("flow", m_flow);
    m_liquid.setUniform("bass", spectrum.bass());
    m_liquid.setUniform("beat", spectrum.beat());
    m_liquid.setUniform("energy", spectrum.energy());
    m_liquid.setUniform("treble", spectrum.treble());
    m_liquid.setUniform("hueShift", 0.35f * std::sin(m_time * 0.04f)); // wanders around the blues
    m_liquid.setUniform("hasImage", m_hasImage ? 1.f : 0.f);
    m_liquid.setUniform("warpAmount", cfg("liquid.warp"));
    m_liquid.setUniform("zoom", cfg("liquid.scale"));
    m_liquid.setUniform("gloss", cfg("liquid.gloss"));
    m_liquid.setUniform("kaleido", cfg("liquid.kaleido"));
    if (m_hasImage)
    {
        const float amount = imageAmount();
        m_liquid.setUniform("time", m_time);
        m_liquid.setUniform("image", m_image);
        m_liquid.setUniform("fitScale", imageFitScale());
        m_liquid.setUniform("amount", amount);
        m_liquid.setUniform("audio", m_audioTexture);
        m_liquid.setUniform("previous", previous.getTexture());
        m_liquid.setUniform("echo", amount * (0.45f + 0.35f * spectrum.energy()));
    }

    sf::RectangleShape screen(target);
    current.clear(sf::Color::Black);
    current.draw(screen, sf::RenderStates(&m_liquid));
    current.display();
    m_liquidIndex = 1 - m_liquidIndex;

    sf::Sprite up(current.getTexture());
    up.setScale({m_size.x / target.x, m_size.y / target.y});
    m_scene.draw(up);
}

void Renderer::drawFeedback(const Spectrum& spectrum, Mode mode, float dt)
{
    sf::RenderTexture& previous = m_feed[m_feedIndex];
    sf::RenderTexture& next = m_feed[1 - m_feedIndex];

    const float beat = spectrum.beat();
    m_feedbackShader.setUniform("source", sf::Shader::CurrentTexture);
    m_feedbackShader.setUniform("resolution", sf::Glsl::Vec2{m_size.x, m_size.y});
    m_feedbackShader.setUniform("time", m_time);
    if (mode == Mode::Tunnel)
    {
        // Zoom in on the last frame so everything rushes outward, mirrored into a kaleidoscope.
        const float swirl = std::sin(m_time * 0.21f) >= 0.f ? 1.f : -1.f;
        m_feedbackShader.setUniform("zoom", std::pow(2.f + spectrum.bass() * 3.f, dt * cfg("tunnel.speed")));
        m_feedbackShader.setUniform("angle", dt * (0.25f + beat * 2.f) * swirl * cfg("tunnel.spin"));
        m_feedbackShader.setUniform("fade", std::exp(-2.6f * std::pow(4.f, 0.5f - cfg("tunnel.trails")) * dt));
        m_feedbackShader.setUniform("hueShift", dt * 1.4f);
        m_feedbackShader.setUniform("mirror", cfg("tunnel.mirror"));
        m_feedbackShader.setUniform("wobble", 0.5f + beat);
    }
    else if (mode == Mode::Oscilloscope)
    {
        // Real phosphor: the trace just glows on and fades, nothing moves.
        m_feedbackShader.setUniform("zoom", 1.f);
        m_feedbackShader.setUniform("angle", 0.f);
        m_feedbackShader.setUniform("fade", std::exp(-9.f * std::pow(5.f, 0.4f - cfg("osc.persistence")) * dt));
        m_feedbackShader.setUniform("hueShift", 0.f);
        m_feedbackShader.setUniform("mirror", 0.f);
        m_feedbackShader.setUniform("wobble", 0.f);
    }
    else
    {
        // Phosphor trails that slowly expand, spin and change color.
        m_feedbackShader.setUniform("zoom", std::pow(1.25f, dt));
        m_feedbackShader.setUniform("angle", dt * (0.12f + beat * 0.8f) * cfg("liss.spin"));
        m_feedbackShader.setUniform("fade", std::exp(-5.f * std::pow(4.f, 0.5f - cfg("liss.trails")) * dt));
        m_feedbackShader.setUniform("hueShift", dt * 0.8f);
        m_feedbackShader.setUniform("mirror", 0.f);
        m_feedbackShader.setUniform("wobble", 1.f);
    }

    next.clear(sf::Color::Black);
    next.draw(sf::Sprite(previous.getTexture()), sf::RenderStates(&m_feedbackShader));

    sf::VertexArray fresh(sf::PrimitiveType::Triangles);
    if (mode == Mode::Tunnel)
        drawTunnelSeed(fresh, spectrum);
    else if (mode == Mode::Oscilloscope)
        drawOscilloscopeBeam(fresh, spectrum);
    else
        drawXYCurve(fresh, spectrum);
    // New content is added every frame, so scale it by frame time to look the same at 60 or 240 fps.
    const auto strength = static_cast<std::uint8_t>(std::clamp(dt * 60.f, 0.25f, 1.f) * 255.f);
    for (std::size_t i = 0; i < fresh.getVertexCount(); ++i)
        fresh[i].color.a = strength;
    next.draw(fresh, sf::RenderStates(sf::BlendAdd));

    next.display();
    m_feedIndex = 1 - m_feedIndex;

    m_scene.draw(sf::Sprite(next.getTexture()), sf::RenderStates(sf::BlendAdd));
}

void Renderer::drawXYCurve(sf::VertexArray& out, const Spectrum& spectrum)
{
    const auto& left = spectrum.left();
    const auto& right = spectrum.right();
    const std::size_t count = std::min(left.size(), right.size());
    constexpr std::size_t Delay = 12; // a small delay turns mono audio into loops instead of a flat line
    if (count <= Delay + 2)
        return;

    float peak = 0.f;
    for (std::size_t i = 0; i < count; ++i)
        peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
    const float wantedGain = std::clamp(0.6f / (peak + 1e-3f), 1.f, 8.f);
    m_xyGain += (wantedGain - m_xyGain) * 0.08f;

    const sf::Vector2f center = m_size * 0.5f;
    const float scale = std::min(m_size.x, m_size.y) * 0.42f * m_xyGain * cfg("liss.size");
    constexpr std::size_t Step = 2;
    const int copies = static_cast<int>(cfg("liss.copies"));
    const float thickness = cfg("liss.thickness");

    for (int copy = 0; copy < copies; ++copy) // extra copies, evenly turned, make it kaleidoscopic
    {
        const float turn = Pi * static_cast<float>(copy) / static_cast<float>(copies);
        const float ct = std::cos(turn), st = std::sin(turn);
        std::vector<sf::Vector2f> pts;
        std::vector<sf::Color> colors;
        pts.reserve(count / Step);
        colors.reserve(count / Step);
        // A gentle low-pass smooths out hiss so the trace draws silky loops, not spikes.
        float x = left[Delay];
        float y = right[0];
        for (std::size_t i = Delay; i < count; i += Step)
        {
            x += (left[i] - x) * 0.35f;
            y += (right[i - Delay] - y) * 0.35f;
            // Rotate 45 degrees: mono sits upright, stereo width spreads sideways.
            sf::Vector2f p{(x - y) * 0.7071f, -(x + y) * 0.7071f};
            p = {p.x * ct - p.y * st, p.x * st + p.y * ct};
            pts.push_back(center + p * scale);
            const float t = static_cast<float>(i) / static_cast<float>(count);
            colors.push_back(bandColor(t * 0.6f + static_cast<float>(copy) * 0.35f, 0.1f + 0.45f * t, 0.6f));
        }
        thickLine(out, pts, colors, thickness, false);
    }
}

void Renderer::drawTunnelSeed(sf::VertexArray& out, const Spectrum& spectrum)
{
    const auto& bands = spectrum.bands();
    const std::size_t n = bands.size();
    const sf::Vector2f center = m_size * 0.5f;
    const float unit = std::min(m_size.x, m_size.y);
    const float base = unit * 0.05f * cfg("tunnel.size") * (1.f + spectrum.beat() * 0.6f);
    const float maxLength = unit * 0.12f * cfg("tunnel.size");
    const float halfWidth = (Pi * base / static_cast<float>(n)) * 0.35f;

    for (int side : {-1, 1})
    {
        for (std::size_t i = 0; i < n; i += 2)
        {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float v = bands[i];
            const float angle = -Pi * 0.5f + static_cast<float>(side) * ((static_cast<float>(i) + 0.5f) / static_cast<float>(n)) * Pi;
            const sf::Vector2f dir{std::cos(angle), std::sin(angle)};
            const sf::Vector2f perp{-dir.y, dir.x};
            const float r1 = base + 2.f + v * maxLength;
            const sf::Color cIn = bandColor(t, 0.15f, 0.9f);
            const sf::Color cOut = bandColor(t, 0.35f + 0.55f * v, 0.75f);
            quad(out,
                 center + dir * base + perp * halfWidth,
                 center + dir * r1 + perp * (halfWidth * 1.6f),
                 center + dir * r1 - perp * (halfWidth * 1.6f),
                 center + dir * base - perp * halfWidth,
                 cIn, cOut, cOut, cIn);
        }
    }

    // Waveform ring in the middle.
    const auto& wave = spectrum.waveform();
    constexpr std::size_t points = 120;
    std::vector<sf::Vector2f> pts(points);
    std::vector<sf::Color> colors(points);
    for (std::size_t k = 0; k < points; ++k)
    {
        const float a = static_cast<float>(k) / static_cast<float>(points) * 2.f * Pi;
        const float sample = std::clamp(wave[k * wave.size() / points] * 2.f, -1.f, 1.f);
        const float r = base * (0.7f + sample * 0.35f);
        pts[k] = center + sf::Vector2f{std::cos(a), std::sin(a)} * r;
        colors[k] = bandColor(0.5f, 0.55f, 0.4f);
    }
    thickLine(out, pts, colors, 2.f, true);
}

sf::FloatRect Renderer::oscilloscopeScreen() const
{
    if (m_oscSweep)
    {
        // 10 x 8 divisions, like a bench scope.
        const float w = std::min(m_size.x * 0.86f, m_size.y * 0.80f * 1.25f) * cfg("osc.size");
        const float h = w * 0.8f;
        return {{(m_size.x - w) * 0.5f, (m_size.y - h) * 0.5f}, {w, h}};
    }
    // Square 8 x 8 for XY.
    const float side = std::min(m_size.x, m_size.y) * 0.82f * cfg("osc.size");
    return {{(m_size.x - side) * 0.5f, (m_size.y - side) * 0.5f}, {side, side}};
}

void Renderer::drawImageLaser(sf::VertexArray& out, const Spectrum& spectrum, sf::FloatRect screen)
{
    // The picture's outlines drawn by the beam, like oscilloscope music. The audio shakes every point,
    // the bass pumps the size, and the whole drawing spins in 3D and gets bent by the waveform.
    const auto& left = spectrum.left();
    const auto& right = spectrum.right();
    const auto& wave = spectrum.waveform();
    const float amount = imageAmount();
    const float energy = spectrum.energy();
    const float beat = spectrum.beat();

    const sf::Vector2f center = screen.position + screen.size * 0.5f;
    const float half = screen.size.x * 0.46f * (1.f + spectrum.bass() * 0.18f * amount + beat * 0.1f);
    const float yaw = amount * (std::sin(m_time * 0.45f) * 1.1f + beat * 0.5f);   // spin around the vertical axis
    const float roll = amount * std::sin(m_time * 0.23f) * 0.35f;
    const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
    const float cosRoll = std::cos(roll), sinRoll = std::sin(roll);
    const float shake = (0.01f + 0.09f * energy + 0.12f * beat) * amount;
    const float bend = (0.05f + 0.25f * energy) * amount;
    const float beamWidth = cfg("osc.thickness");

    std::size_t index = 0;
    for (const auto& stroke : m_outline)
    {
        std::vector<sf::Vector2f> pts;
        pts.reserve(stroke.size());
        for (const sf::Vector2f& p0 : stroke)
        {
            sf::Vector2f p = p0;
            const std::size_t k = (index++ * 7) % left.size();
            p.x += left[k] * shake;
            p.y += right[k] * shake;
            const auto w = static_cast<std::size_t>(std::clamp((p.x * 0.5f + 0.5f), 0.f, 1.f) * static_cast<float>(wave.size() - 1));
            p.y += wave[w] * bend;
            // 3D spin: rotate around Y, add a little perspective, then tilt.
            const float x3 = p.x * cosYaw;
            const float z3 = p.x * sinYaw;
            const float perspective = 1.f / (1.f + z3 * 0.35f);
            sf::Vector2f q{x3 * perspective, p.y * perspective};
            q = {q.x * cosRoll - q.y * sinRoll, q.x * sinRoll + q.y * cosRoll};
            pts.push_back({center.x + std::clamp(q.x * half, -screen.size.x * 0.5f, screen.size.x * 0.5f),
                           center.y + std::clamp(q.y * half, -screen.size.y * 0.5f, screen.size.y * 0.5f)});
        }
        const float flicker = std::min(1.f, (0.75f + 0.25f * std::sin(static_cast<float>(index) * 0.37f + m_time * 30.f * beat)) * cfg("osc.brightness"));
        const sf::Color core(static_cast<std::uint8_t>(120 * flicker), static_cast<std::uint8_t>(255 * flicker),
                             static_cast<std::uint8_t>(150 * flicker));
        const sf::Color halo(10, static_cast<std::uint8_t>(60 * flicker), 26);
        thickLine(out, pts, std::vector<sf::Color>(pts.size(), halo), beamWidth * 3.3f, false);
        thickLine(out, pts, std::vector<sf::Color>(pts.size(), core), beamWidth, false);
    }
}

void Renderer::drawGraticule(sf::VertexArray& out) const
{
    const sf::FloatRect screen = oscilloscopeScreen();
    const int divX = m_oscSweep ? 10 : 8;
    const int divY = 8;
    const sf::Color line(22, 70, 36);
    const sf::Color axis(34, 110, 55);
    const float x0 = screen.position.x, y0 = screen.position.y;
    const float x1 = x0 + screen.size.x, y1 = y0 + screen.size.y;

    for (int i = 0; i <= divX; ++i)
    {
        const float x = std::round(x0 + screen.size.x * static_cast<float>(i) / static_cast<float>(divX));
        rect(out, x, y0, x + 1.f, y1, line, line);
    }
    for (int j = 0; j <= divY; ++j)
    {
        const float y = std::round(y0 + screen.size.y * static_cast<float>(j) / static_cast<float>(divY));
        rect(out, x0, y, x1, y + 1.f, line, line);
    }

    // Fine tick marks along the center axes (5 per division).
    const float cx = std::round(x0 + screen.size.x * 0.5f);
    const float cy = std::round(y0 + screen.size.y * 0.5f);
    for (int i = 0; i <= divX * 5; ++i)
    {
        const float x = std::round(x0 + screen.size.x * static_cast<float>(i) / static_cast<float>(divX * 5));
        rect(out, x, cy - 3.f, x + 1.f, cy + 4.f, axis, axis);
    }
    for (int j = 0; j <= divY * 5; ++j)
    {
        const float y = std::round(y0 + screen.size.y * static_cast<float>(j) / static_cast<float>(divY * 5));
        rect(out, cx - 3.f, y, cx + 4.f, y + 1.f, axis, axis);
    }
}

void Renderer::drawOscilloscopeBeam(sf::VertexArray& out, const Spectrum& spectrum)
{
    const sf::FloatRect screen = oscilloscopeScreen();
    const sf::Vector2f center = screen.position + screen.size * 0.5f;

    if (!m_oscSweep && m_hasImage && !m_outline.empty())
    {
        drawImageLaser(out, spectrum, screen);
        return;
    }
    std::vector<sf::Vector2f> pts;

    if (!m_oscSweep)
    {
        // XY: left channel drives the beam sideways, right channel up and down.
        const auto& left = spectrum.left();
        const auto& right = spectrum.right();
        const std::size_t count = std::min(left.size(), right.size());
        float peak = 0.f;
        for (std::size_t i = 0; i < count; ++i)
            peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
        m_oscGain += (std::clamp(0.85f / (peak + 1e-3f), 1.f, 4.f) - m_oscGain) * 0.05f;

        const float half = screen.size.x * 0.5f * m_oscGain;
        pts.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            pts.push_back({center.x + std::clamp(left[i] * half, -screen.size.x * 0.5f, screen.size.x * 0.5f),
                           center.y - std::clamp(right[i] * half, -screen.size.y * 0.5f, screen.size.y * 0.5f)});
    }
    else
    {
        // Sweep: classic voltage-over-time trace, triggered on a rising zero crossing so it holds still.
        const auto& wave = spectrum.waveform();
        constexpr std::size_t Window = 768;
        if (wave.size() <= Window + 2)
            return;
        const std::size_t searchEnd = wave.size() - Window;
        std::size_t start = searchEnd / 2;
        for (std::size_t i = searchEnd / 4; i < searchEnd; ++i)
            if (wave[i - 1] < 0.f && wave[i] >= 0.f)
            {
                start = i;
                break;
            }
        float peak = 0.f;
        for (std::size_t i = start; i < start + Window; ++i)
            peak = std::max(peak, std::abs(wave[i]));
        m_oscGain += (std::clamp(0.8f / (peak + 1e-3f), 1.f, 4.f) - m_oscGain) * 0.05f;

        const float halfH = screen.size.y * 0.5f;
        pts.reserve(Window);
        for (std::size_t k = 0; k < Window; ++k)
        {
            const float t = static_cast<float>(k) / static_cast<float>(Window - 1);
            pts.push_back({screen.position.x + t * screen.size.x,
                           center.y - std::clamp(wave[start + k] * halfH * m_oscGain, -halfH, halfH)});
        }
    }
    if (pts.size() < 2)
        return;

    // Like a real CRT, the beam is bright where it moves slowly and faint where it races.
    std::vector<sf::Color> core(pts.size()), halo(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i)
    {
        const sf::Vector2f d = pts[std::min(i + 1, pts.size() - 1)] - pts[i == 0 ? 0 : i - 1];
        const float speed = std::sqrt(d.x * d.x + d.y * d.y) * 0.5f;
        const float intensity = std::clamp(1.8f / (1.f + speed * 0.3f) * cfg("osc.brightness"), 0.07f, 1.f);
        core[i] = sf::Color(static_cast<std::uint8_t>(120 * intensity), static_cast<std::uint8_t>(255 * intensity),
                            static_cast<std::uint8_t>(150 * intensity));
        halo[i] = sf::Color(static_cast<std::uint8_t>(10 * intensity), static_cast<std::uint8_t>(70 * intensity),
                            static_cast<std::uint8_t>(30 * intensity));
    }
    const float beamWidth = cfg("osc.thickness");
    thickLine(out, pts, halo, beamWidth * 3.75f, false);
    thickLine(out, pts, core, beamWidth, false);
}

void Renderer::applyBloom(sf::RenderTarget& target, float strength)
{
    if (!m_bloomReady)
        return;

    const sf::Vector2f halfSize{static_cast<float>(m_blurA.getSize().x), static_cast<float>(m_blurA.getSize().y)};

    // Downsample the scene to half size.
    m_blurA.clear(sf::Color::Black);
    sf::Sprite down(m_scene.getTexture());
    down.setScale({halfSize.x / m_size.x, halfSize.y / m_size.y});
    m_blurA.draw(down);
    m_blurA.display();

    // Blur back and forth, widening each pass.
    m_blur.setUniform("source", sf::Shader::CurrentTexture);
    for (int pass = 0; pass < 3; ++pass)
    {
        const float spread = 1.f + static_cast<float>(pass) * 1.5f;

        m_blur.setUniform("direction", sf::Glsl::Vec2{spread / halfSize.x, 0.f});
        m_blurB.clear(sf::Color::Black);
        m_blurB.draw(sf::Sprite(m_blurA.getTexture()), sf::RenderStates(&m_blur));
        m_blurB.display();

        m_blur.setUniform("direction", sf::Glsl::Vec2{0.f, spread / halfSize.y});
        m_blurA.clear(sf::Color::Black);
        m_blurA.draw(sf::Sprite(m_blurB.getTexture()), sf::RenderStates(&m_blur));
        m_blurA.display();
    }

    sf::Sprite glow(m_blurA.getTexture());
    glow.setScale({m_size.x / halfSize.x, m_size.y / halfSize.y});
    glow.setColor(withAlpha(sf::Color::White, std::min(strength, 1.f)));
    sf::RenderStates states(sf::BlendAdd);
    target.draw(glow, states);
    if (strength > 1.f) // extra punch on big kicks
    {
        glow.setColor(withAlpha(sf::Color::White, strength - 1.f));
        target.draw(glow, states);
    }
}

sf::FloatRect Renderer::seekBarHitBox() const
{
    const float margin = 32.f;
    return {{margin, m_size.y - 66.f}, {m_size.x - margin * 2.f, 24.f}};
}

float Renderer::seekFractionAt(float x) const
{
    const sf::FloatRect box = seekBarHitBox();
    return std::clamp((x - box.position.x) / box.size.x, 0.f, 1.f);
}

void Renderer::drawOverlay(sf::RenderTarget& target, const UiState& ui, Mode mode)
{
    if (!m_font)
        return;

    auto makeText = [&](const sf::String& str, unsigned size, sf::Color color)
    {
        sf::Text text(*m_font, str, size);
        text.setFillColor(color);
        return text;
    };
    auto centered = [&](sf::Text& text, float y)
    {
        const sf::FloatRect b = text.getLocalBounds();
        text.setPosition({std::round((m_size.x - b.size.x) * 0.5f - b.position.x), std::round(y)});
    };

    // Toast message (volume, errors, mode changes).
    if (ui.messageAlpha > 0.01f && !ui.message.isEmpty())
    {
        sf::Text msg = makeText(ui.message, 18, withAlpha(sf::Color::White, ui.messageAlpha));
        centered(msg, 22.f);
        target.draw(msg);
    }

    if (!ui.loaded)
    {
        const float pulse = 0.75f + 0.25f * std::sin(m_time * 2.f);
        sf::Text title = makeText(U"Bxsim", 64, withAlpha(sf::Color::White, 0.95f));
        centered(title, m_size.y * 0.5f - 90.f);
        target.draw(title);

        const sf::String hint = ui.loading ? sf::String(U"Loading…") : sf::String(U"Drop a song here, or press O to open one");
        sf::Text sub = makeText(hint, 22, withAlpha(sf::Color(200, 210, 255), pulse));
        centered(sub, m_size.y * 0.5f + 4.f);
        target.draw(sub);

        sf::Text formats = makeText(U"MP3 · WAV · OGG · FLAC", 15, withAlpha(sf::Color(150, 160, 190), 0.8f));
        centered(formats, m_size.y * 0.5f + 44.f);
        target.draw(formats);

        sf::Text picture = makeText(m_hasImage ? U"Image ready ✓" : U"Want a picture that reacts too? Drop an image or press I",
                                    15, withAlpha(sf::Color(150, 160, 190), 0.7f));
        centered(picture, m_size.y * 0.5f + 70.f);
        target.draw(picture);
        return;
    }

    if (ui.showFps)
    {
        sf::Text fps = makeText(std::to_string(static_cast<int>(std::round(ui.fps))) + " FPS", 14,
                                withAlpha(sf::Color(170, 255, 170), 0.9f));
        fps.setPosition({m_size.x - fps.getLocalBounds().size.x - 20.f, 18.f});
        target.draw(fps);
    }

    const float a = ui.uiAlpha;
    if (a <= 0.01f)
        return;

    // Song title and status.
    sf::Text title = makeText(ui.title, 24, withAlpha(sf::Color::White, a));
    title.setPosition({32.f, 24.f});
    target.draw(title);

    sf::String status = sf::String(modeName(mode));
    if (mode == Mode::Oscilloscope)
        status += m_oscSweep ? U"  ·  Sweep (Tab for XY)"
                             : (m_hasImage && !m_outline.empty() ? U"  ·  Image laser (Tab for sweep)" : U"  ·  XY (Tab for sweep)");
    if (!ui.playing)
        status += U"  ·  Paused";
    sf::Text sub = makeText(status, 15, withAlpha(sf::Color(170, 180, 210), a));
    sub.setPosition({33.f, 58.f});
    target.draw(sub);

    // Seek bar.
    const sf::FloatRect box = seekBarHitBox();
    const float barY = box.position.y + box.size.y * 0.5f - 2.f;
    const float fraction = ui.duration > 0.f ? std::clamp(ui.position / ui.duration, 0.f, 1.f) : 0.f;

    sf::RectangleShape track({box.size.x, 4.f});
    track.setPosition({box.position.x, barY});
    track.setFillColor(withAlpha(sf::Color::White, 0.18f * a));
    target.draw(track);

    sf::RectangleShape fill({box.size.x * fraction, 4.f});
    fill.setPosition({box.position.x, barY});
    fill.setFillColor(withAlpha(bandColor(0.2f, 1.f, 0.6f), a));
    target.draw(fill);

    sf::CircleShape knob(6.f);
    knob.setOrigin({6.f, 6.f});
    knob.setPosition({box.position.x + box.size.x * fraction, barY + 2.f});
    knob.setFillColor(withAlpha(sf::Color::White, a));
    target.draw(knob);

    // Times.
    sf::Text now = makeText(formatTime(ui.position), 14, withAlpha(sf::Color(200, 205, 225), a));
    now.setPosition({box.position.x, barY + 12.f});
    target.draw(now);

    sf::Text total = makeText(formatTime(ui.duration), 14, withAlpha(sf::Color(200, 205, 225), a));
    total.setPosition({box.position.x + box.size.x - total.getLocalBounds().size.x, barY + 12.f});
    target.draw(total);

    // Controls hint.
    sf::Text help = makeText(U"Space play/pause   ← → seek   ↑ ↓ volume   M / 0-9 / - = [ ] mode   S settings   G filter   A aspect   E export   N/P next/prev   O open   I image   F11 fullscreen",
                             13, withAlpha(sf::Color(140, 150, 180), a * 0.9f));
    const float helpWidth = help.getLocalBounds().size.x;
    if (helpWidth > m_size.x - 40.f) // narrow canvases (like 9:16) get a smaller hint line
        help.setScale({(m_size.x - 40.f) / helpWidth, (m_size.x - 40.f) / helpWidth});
    {
        const sf::FloatRect b = help.getGlobalBounds();
        help.setPosition({std::round(help.getPosition().x + (m_size.x - b.size.x) * 0.5f - b.position.x), std::round(m_size.y - 26.f)});
    }
    target.draw(help);
}
