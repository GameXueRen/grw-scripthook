/* Night-vision pixel shader, for substituting the engine's own HDRLighting_NV_ps.
 *
 * This is a transcription of the disassembly of that shader, captured from the
 * retail game (see .codebuddy/plans/nightvision-filter-feasibility_4c81ab07.md).
 * The original could not be edited in place: fxc disassembles DXBC but will not
 * take assembly back in, and d3dcompiler's D3DAssemble rejects the version
 * token. Its HLSL is not available either. So the instructions were read out of
 * the dump and rewritten here, and this file is the only place the night-vision
 * maths lives now.
 *
 * WHAT THE ORIGINAL DOES
 * ----------------------
 * cb5[13].y is the filter selector, an integer the engine picks from the
 * headgear the player is wearing. Three values are produced:
 *
 *   0   monochrome, then multiplied by (0.7, 1.3, 0.7) - a green boost - and
 *       the sample point is jittered by a per-pixel hash, which is the grain
 *       the default goggles show. This is what "night vision" looks like by
 *       default.
 *   1   monochrome, then mixed with a near-neutral (0.95, 1.05, 0.98) tint and
 *       no jitter at all. This is the black-and-white look that the Sonar
 *       Goggles / Splinter Cell headgear produce.
 *   2   monochrome, then a two-term ramp:
 *           g*g * (1.568628, 2.000000, 0.619608)
 *         + (g + 0.1) * (0.298039, 0.450980, 0.070588)
 *       which is yellow where the scene is bright and green where it is dark.
 *       This is the yellow-green "panoramic" look.
 *
 * THE TWO KNOBS
 * -------------
 * The look is chosen by remapping the mode selector:
 *
 *   NV_DEFAULT_MODE   which look every replaced mode gets
 *                     1 = black and white, 2 = yellow-green,
 *                     3 = sepia (a table look - see nv_looks.hlsl)
 *   NV_ALL_MODES      how far that reach goes
 *                     0 = only the mode the ordinary goggles use, so headgear
 *                         with a look of its own keeps it
 *                     1 = every mode, so the look stops depending on what is
 *                         equipped at all
 *
 *   /D=1 /D=0   ordinary goggles -> black and white
 *   /D=2 /D=0   ordinary goggles -> yellow-green
 *   /D=3 /D=0   ordinary goggles -> sepia
 *   /D=1 /D=1   everything -> black and white
 *   /D=2 /D=1   everything -> yellow-green
 *   /D=3 /D=1   everything -> sepia
 *
 * There is deliberately no build that leaves the modes as they are: "the game's
 * own look" is what the plugin's first row means by OFF, and it reaches that by
 * substituting nothing at all. A variant that folded mode 0 onto itself would
 * just be a copy of the original shader, which is the OFF row spelled differently.
 *
 * Mode 0's own sampling is deliberately NOT reproduced. While the selector
 * is 0 the original shifts the sample point radially and then throws the colour
 * away for the luminance of a five-tap blur whose taps are re-placed every frame
 * (cb5[10].xy). That was transcribed on 2026-10-19 and taken back out the same
 * day: it changes the brightness and the sharpness of EVERY look - throwing the
 * colour away is the whole point of it - and a measured comparison of two
 * consecutive frames found nothing in it that a still could see (1.5 grey levels
 * in a static, textured patch, which is JPEG quantisation). If the drifting scan
 * noise a player remembers is ever located, it is not in this branch.
 *
 * Everything else - the vignette, the ACES-ish tonemap, the gain, the two
 * texture lookups, the mode 2 ramp - is carried over unchanged. The per-pixel
 * jitter is multiplied by a literal 0.0 in this build (see the note in main), so
 * there is no hash to transcribe and nothing per-pixel is lost.
 *
 * THE ONE THING THIS DOES NOT OVERRIDE
 * ------------------------------------
 * cb5[13].x selects an entirely different branch - a pair of lookups - that
 * ignores the mode selector. The night-vision pass does not take it (the
 * selector demonstrably changes what is on screen, which it could not if that
 * branch were running), so it is transcribed for completeness and left alone.
 * If some future piece of gear is ever found to be immune to every row here,
 * that flag is the first thing to look at.
 *
 * ADDING A FILTER
 * ---------------
 * A new look means a new branch on `mode` plus another compile of this file,
 * not a new plugin: teach this file to produce it, build it as another variant
 * (see tools/embed_nv_filters.py), and add a row to the plugin's choice table.
 *
 * Two ways to teach it, and both are in use:
 *
 *   an expression   a few lines of maths on `g` - that is what modes 1 and 2
 *                   are, and it is the right shape for a look that is a tint
 *                   or a ramp
 *   a table         a 32-entry grey-axis curve plus one lerp, generated into
 *                   nv_looks.hlsl - that is what mode 3 is, and it is the only
 *                   shape that follows a grade with a colour cast that changes
 *                   across the range (blue shadows, warm highlights)
 *
 * A look may also ask for static grain. That is not part of the tone curve - it
 * is noise added to the final colour in main - so it travels as NV_GRAIN_MILLI,
 * one number the generated dispatch sets per look, and 0 preprocesses the whole
 * expression away so no other variant changes. Modes 18 and 19 are the two that
 * use it: the classic intensifier and the Splinter Cell one, which without it
 * look like clean screenshots of a green room.
 */

#ifndef NV_DEFAULT_MODE
#define NV_DEFAULT_MODE 1
#endif

#ifndef NV_ALL_MODES
#define NV_ALL_MODES 0
#endif

/* The white point of a table look: the `g` at which the table's last entry is
 * reached. Mode 1 puts white at 0.95, so a table look that is meant to keep the
 * brightness the player already knows has to land on its last sample at the
 * same g. This is the one number in here that is a judgement rather than a
 * transcription - raise it and the look darkens, lower it and it blows out -
 * so it is a define and not baked into the generated tables. */
#ifndef NV_G_WHITE
#define NV_G_WHITE 0.95
#endif

/* The three looks that are arithmetic rather than a table. `white` is the same
 * white point the tables get, and `t` is the luminance mapped onto it, so all
 * of these agree about where black and white are.
 *
 * They exist because the game's own twelve cover grading, not the two things a
 * night-vision player actually asks for: a colour that keeps dark adaptation
 * (long wavelength, no blue) and a cooler one for a screen that is too warm. */

/* Amber. Classic NVG: almost no blue, and no white point at all - an image
 * tube that has been given a colour like this stays that colour at full
 * brightness, which is what makes it recognisable. */
float3 LookAMBER(float g, float white) {
    float t = saturate(g / white);
    return saturate(pow(t, 0.85) * float3(1.10, 0.68, 0.26));
}

/* Cool blue, the other direction: green pulled down, blue pushed up. */
float3 LookCOOLBLUE(float g, float white) {
    float t = saturate(g / white);
    return saturate(pow(t, 1.05) * float3(0.62, 0.90, 1.25));
}

/* High-contrast green: the classic look with the contrast the game's own
 * green never had, and without its tint. Clips earlier, so shadows go black
 * and anything lit goes to full green. */
float3 LookCONTRAST(float g, float white) {
    float t = saturate(g / white);
    return saturate((t - 0.5) * 1.9 + 0.5) * float3(0.45, 1.35, 0.45);
}

/* Classic night vision: the P43 phosphor an image intensifier actually uses
 * (545 nm, which is why every NVG is green). Gain first, then contrast: a tube
 * lifts the bottom of the range - which is where a night scene lives, so a
 * curve that crushes it would show the player nothing - and then steepens what
 * it lifted. The pivot sits above the middle so the lift survives it. The top
 * drifts to near-white, because an intensifier being overdriven stops being
 * green. */
float3 LookCLASSICNV(float g, float white) {
    float t = saturate(g / white);
    float s = pow(t, 0.62);
    s = saturate((s - 0.55) * 1.30 + 0.55);
    float3 col = s * float3(0.60, 1.00, 0.52);
    col = lerp(col, float3(0.93, 1.00, 0.90), smoothstep(0.88, 1.00, s));
    return saturate(col);
}

/* Splinter Cell's green, the Chaos Theory one. What makes it recognisable is
 * not the tint but the tube: it never reaches black, so the whole image sits
 * high and looks soft, and the lights bloom into pale green rather than
 * clipping. The tint is a pea green, a little yellow, not the pure green the
 * classic look uses. */
float3 LookSPLINTERCELL(float g, float white) {
    float t = saturate(g / white);
    float s = 0.075 + 0.925 * pow(t, 0.90);
    s = s / (1.0 + 0.40 * s);          /* a soft shoulder, not a clip */
    s *= 1.50;                          /* and back to the brightness it had */
    float3 col = s * float3(0.56, 0.96, 0.38);
    col = lerp(col, float3(0.86, 1.00, 0.78), smoothstep(0.58, 1.00, saturate(s)));
    return saturate(col);
}

/* Per-pixel hash in [0,1). The engine's own look jitters its sample point
 * through one of these - that is the grain the retail goggles were supposed to
 * show - and the shipped shader multiplies it by a literal 0.0, so it has no
 * grain at all. A substituted look has none either, which is most of what makes
 * a screenshot read as "clean" instead of "through goggles". */
float NvHash(float2 p) {
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

/* The twelve Photo Mode grades, as 32-entry grey-axis tables, and the
 * compile-time dispatch that picks one. Generated; see the file header for
 * where those numbers come from. It references the five functions above. */
#include "nv_looks.hlsl"

/* How much grain this build adds, in thousandths of full scale. Generated by
 * the same dispatch: every look that predates it is 0, and the two that want
 * grain say so. Zero is not a small amount here - it removes the expression.
 * An integer, because the preprocessor cannot compare floats. */
#ifndef NV_GRAIN_MILLI
#define NV_GRAIN_MILLI 0
#endif


cbuffer CB1 : register(b1) { float4 cb1[161]; };
cbuffer CB2 : register(b2) { float4 cb2[48];  };
cbuffer CB5 : register(b5) { float4 cb5[14];  };

Texture2D    t0  : register(t0);
Texture2D    t1  : register(t1);
Texture2D    t2  : register(t2);
Texture2D    t6  : register(t6);
Texture2D    t7  : register(t7);
Texture2D    t8  : register(t8);

SamplerState s6  : register(s6);
SamplerState s7  : register(s7);
SamplerState s10 : register(s10);

float4 main(float4 v0 : SV_Position, float2 v1 : TEXCOORD0) : SV_Target
{
    /* The selector is an integer in a float4 slot, so it has to be read back
     * as bits. The engine tests it with ieq against l(1)/l(2), which only works
     * if the bits are compared.
     *
     * It is NOT remapped here any more. Which look this build substitutes is
     * decided before the shader is compiled (LookSelected, in nv_looks.hlsl),
     * and all this value does now is say whether the engine asked for one of
     * its own two looks - 1 and 2 - which a "ordinary goggles only" build
     * leaves alone. */
    int mode = asint(cb5[13].y);

    float2 uv = float2(v1.x * cb1[160].x, v1.y);

    /* Sampled before the branch in the original; it is the output alpha. */
    float alpha = t7.Sample(s10, uv).x;

    /* The sample point. The original shifts it radially while the selector
     * is 0, and then replaces the colour with the luminance of a five-tap blur
     * whose taps are re-placed every frame. Neither is reproduced here - see
     * the note at the top of this file for why, and for the measurement that
     * decided it. The per-pixel hash that used to scale that shift is
     * multiplied by a literal 0.0 in this build, so no hash is lost either. */
    float3 scene = t0.Sample(s10, uv).rgb;

    /* Original guard: x with an all-ones exponent (inf or NaN) becomes 65000
     * so the tonemap below cannot produce a black hole. */
    if ((asuint(scene.x) & 0x7f800000u) == 0x7f800000u)
        scene = 65000.0;

    if (asint(cb5[13].x) != 0)
    {
        /* The lookup-table path. The night-vision pass does not take it - it
         * ignores the selector entirely, and the selector demonstrably changes
         * what is on screen - but it is carried over so this file stands in for
         * the original in every case. Output is (t7.x, luma, t6.x, t7.x). */
        float  lut  = t6.Sample(s7, float2(v1.x * 10.0 - 0.5,
                                           v1.y * 10.0 - 0.5)).x;
        float3 wide = t8.Sample(s10, v1.xy).rgb;
        return float4(alpha,
                      dot(wide, float3(0.2126, 0.7152, 0.0722)),
                      lut,
                      alpha);
    }

    /* --- analytic path ---------------------------------------------------- */

    /* t1 is a one-pixel lookup sampled at the origin; it scales the scene. */
    float3 value = t1.Sample(s10, float2(0.0, 0.0)).x * scene;

    /* t2 is sampled in screen space and contributes luminance only. */
    float3 extra = t2.Sample(s6, uv).rgb;
    float  extraL = dot(cb5[0].y * 0.5 * extra, float3(0.39, 0.50, 0.11));
    value = value * cb2[47].y + extraL;

    /* Vignette. */
    float2 d = v1.xy - 0.5;
    value *= pow(max(1.0 - dot(d, d), 0.0), 5.2);

    value = max(value, 0.0);
    value = max(value - 0.004, 0.0);

    /* Filmic curve: (x*(3.2x+0.5)) / (x*(3.2x+0.5) + 1.06). */
    float3 curve = value * (3.2 * value + 0.5);
    value = curve / (curve + 1.06);

    value = pow(max(value, 1e-5), cb5[11].w);
    value = min(value, 1.0);
    value = saturate((value + cb5[11].y - 0.5) * cb5[11].z + 0.5);

    /* Everything past this point works on a single scalar, because every mode
     * this shader can reach is monochrome from here on. */
    float g = dot(value, float3(0.2126, 0.7152, 0.0722)) * 3.5;

    float3 gray = g.xxx;
    float3 tint = g.xxx;
    if (g > 0.95)
        tint = 1.0;

    /* Three-row bands of slightly different tints. Gated on cb5[0].z in the
     * original and normally inactive; kept so the two agree everywhere. */
    uint  row  = (uint)(v0.y + 0.5);       /* round_ni then ftou */
    uint  band = row / 3u;

    float3 bandA = tint * float3(0.329412, 1.098039, 0.996078);
    float3 bandB = tint * float3(0.996078, 0.431373, 0.996078);
    float3 bandC = tint * float3(0.996078, 1.098039, 0.329412);

    float3 picked = (band == 2u) ? bandC : tint;
    picked        = (band == 1u) ? bandB : picked;
    picked        = (band != 0u) ? picked : bandA;
    tint          = (asint(cb5[0].z) != 0) ? tint : picked;

    /* Everything the look needs: the luminance, plus the two colours modes 1
     * and 2 are built from, since an "ordinary goggles only" build has to be
     * able to hand those two back untouched. */
#if NV_GRAIN_MILLI > 0
    /* Grain goes on the final colour, not on the luminance: the tube's noise is
     * in the display, and adding it here is what makes it read as noise rather
     * than as banding in the shadows. */
    float3 col = LookSelected(mode, g, gray, tint);
    col += (NvHash(v0.xy) - 0.5) * (NV_GRAIN_MILLI * 0.001);
    return float4(col, alpha);
#else
    /* The same expression the shader ended with before grain existed, kept
     * verbatim rather than routed through a local: fxc allocates registers
     * differently for the two spellings, and this way a look that does not want
     * grain compiles to the very bytes it always did. */
    return float4(LookSelected(mode, g, gray, tint), alpha);
#endif
}
