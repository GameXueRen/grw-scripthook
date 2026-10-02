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
 *   NV_DEFAULT_MODE   which of the two looks every replaced mode gets
 *                     1 = black and white, 2 = yellow-green
 *   NV_ALL_MODES      how far that reach goes
 *                     0 = only the mode the ordinary goggles use, so headgear
 *                         with a look of its own keeps it
 *                     1 = every mode, so the look stops depending on what is
 *                         equipped at all
 *
 *   /D=1 /D=0   ordinary goggles -> black and white
 *   /D=2 /D=0   ordinary goggles -> yellow-green
 *   /D=1 /D=1   everything -> black and white
 *   /D=2 /D=1   everything -> yellow-green
 *
 * There is deliberately no build that leaves the modes as they are: "the game's
 * own look" is what the plugin's first row means by OFF, and it reaches that by
 * substituting nothing at all. A fifth variant that folded mode 0 onto itself
 * would be a copy of the original shader, and mode 0 is not just a colour
 * choice - it swaps the scene colour for a blurred luminance and shifts the
 * sample point radially - so reproducing it would have meant transcribing a
 * branch nothing can reach. Dropping that variant dropped the branch with it.
 *
 * Everything else - the vignette, the ACES-ish tonemap, the gain, the two
 * texture lookups, the mode 2 ramp - is carried over unchanged. Because the
 * per-pixel jitter is multiplied by a literal 0.0 in this build (see the note
 * on the sample point below) there is no hash to transcribe.
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
 */

#ifndef NV_DEFAULT_MODE
#define NV_DEFAULT_MODE 1
#endif

#ifndef NV_ALL_MODES
#define NV_ALL_MODES 0
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
     * if the bits are compared. */
    int mode = asint(cb5[13].y);
#if NV_ALL_MODES
    /* Every mode, so the look stops depending on what is equipped. */
    mode = NV_DEFAULT_MODE;
#else
    /* Only the ordinary goggles; headgear with a look of its own keeps it. */
    if (mode == 0)
        mode = NV_DEFAULT_MODE;
#endif

    /* The sample point. The original offsets it by a small radial amount while
     * the selector is 0 (movc r1.zw, cb5[13].yyyy, l(0,0,0,0), r1.zzzw), and
     * the per-pixel hash that fed that offset is dead in this build - the
     * disassembly multiplies it by a literal 0.0. No build here leaves a mode
     * as 0, so that offset is not transcribed: it belongs to the branch that
     * draws the game's own look, and that look is reached by substituting
     * nothing rather than by a variant of this shader. */
    float2 uv = float2(v1.x * cb1[160].x, v1.y);

    /* Sampled before the branch in the original; it is the output alpha. */
    float alpha = t7.Sample(s10, uv).x;

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
        float lut   = t6.Sample(s7, float2(v1.x * 10.0 - 0.5,
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

    /* The mix that lands on (0.95, 1.05, 0.98) - a hair of green. */
    float3 mono = lerp(gray, tint, 0.35) * float3(0.95, 1.05, 0.98);

    /* The mode 2 ramp: yellow when bright, green when dark. */
    float3 ramp = (g * g) * float3(1.568628, 2.000000, 0.619608)
                + (g + 0.1) * float3(0.298039, 0.450980, 0.070588);

    float3 rgb = (mode == 2) ? ramp : tint;
    rgb        = (mode == 1) ? mono : rgb;

    return float4(rgb, alpha);
}
