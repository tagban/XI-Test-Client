#pragma once

// Water. A translucent sheet at the height each collision cell records, tinted
// by the zone's own lighting so it belongs to the time of day.

namespace mh
{
inline constexpr const char* kWaterShader = R"(
struct Uniforms {
    viewProjection : mat4x4<f32>,
    lightDirection : vec4<f32>,
    ambient : vec4<f32>,
    sunlight : vec4<f32>,
    fogColour : vec4<f32>,
    fogRange : vec4<f32>,
    eye : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var waterTexture : texture_2d<f32>;
@group(0) @binding(2) var waterSampler : sampler;

struct WaterOut {
    @builtin(position) clipPosition : vec4<f32>,
    @location(0) worldPosition : vec3<f32>,
    @location(1) uv : vec2<f32>,
};

@vertex
fn vertexMain(@location(0) position : vec3<f32>,
              @location(1) normal : vec3<f32>,
              @location(2) uv : vec2<f32>) -> WaterOut {
    var out : WaterOut;
    out.clipPosition = uniforms.viewProjection * vec4<f32>(position, 1.0);
    out.worldPosition = position;
    out.uv = uv;
    return out;
}

@fragment
fn fragmentMain(in : WaterOut) -> @location(0) vec4<f32> {
    let time = uniforms.eye.w;

    // Two layers, which is how FFXI builds it. effect kaw1 and ike1 are pure
    // white - average RGB (255,255,255) - so they are a foam and ripple sheet,
    // not a water colour. Used as the base they read as wet concrete.
    //
    // The tint is deliberately light. effect umna measures (12,15,29), but that
    // is the sea; taken literally for a river, with the bed hidden behind an
    // opaque sheet, it renders as a black void.
    // Greener and darker than it was, from a side-by-side against a retail
    // client standing in the same spot in Windurst Waters: theirs is a deep
    // green-teal that reads as depth, ours was a pale blue-grey that read as a
    // slab laid on the ground.
    // A river or pond, or the sea. Which one the zone has is decided by the
    // ripple sheet it ships - umi and sea sheets are the sea - and passed in
    // fogRange.z, which the zone pass leaves unused. Retail's harbour at
    // Port Bastok is close to black with the dusk sky on it; a green-teal
    // river tint made it a grey-green slab.
    let sea = uniforms.fogRange.z;

    // The ripple sheet, sampled twice drifting at different speeds and angles so
    // it does not read as one sheet sliding.
    //
    // The sea drifts faster than a river, which is backwards from how water
    // behaves and right for how it looks. What matters is movement against the
    // texture, and a sea lays that texture out far coarser: Valkurm's repeats
    // fifteen times across the whole bay, so at a canal's pace one ripple
    // takes a minute and a half to cross a single tile and the water reads as
    // painted on. A canal repeats it every few yalms and needs no help.
    let drift = mix(1.0, 6.0, sea);
    let a = textureSample(waterTexture, waterSampler, in.uv + vec2<f32>(time * 0.011, time * 0.007) * drift);
    let b = textureSample(waterTexture, waterSampler, in.uv * 0.63 + vec2<f32>(time * -0.008, time * 0.013) * drift);
    let foam = clamp(a.a * 0.7 + b.a * 0.55, 0.0, 1.0);

    // The sea's colour is the sheet's own. umi0 measured straight from the DAT
    // is a bright cyan (69, 217, 255), drawn translucent - the whole reason
    // Valkurm's bay reads blue-green. The shader used to throw that away and
    // paint a hardcoded tint, which came out the wrong hue (a dark teal-green).
    // Read it back off the texture the sea actually ships. A river keeps a
    // painted tint: its sheets are white foam with no colour of their own.
    let texColour = (a.rgb + b.rgb) * 0.5;
    let body = mix(vec3<f32>(0.09, 0.20, 0.17), texColour, sea);

    // Ambient is clamped: components are scaled 0..128, so it can exceed 1, and
    // letting that through once turned the river white.
    let ambient = min(uniforms.ambient.rgb, vec3<f32>(1.0, 1.0, 1.0));
    var colour = body * (0.55 + 0.45 * ambient);
    // The foam was most of why it looked washed out: at 0.40 a bright ripple
    // sheet lifted the whole surface toward grey, which is a lake in overcast
    // daylight rather than a canal.
    colour = colour + ambient * foam * mix(0.16, 0.07, sea);

    // The sky on the water. There is no reflection pass; the fog colour is
    // the horizon's, which is what a flat sheet mostly mirrors, and how much
    // of it shows depends on how flat the view is - straight down sees the
    // bed, along the surface sees the sky. That grazing brightening is most
    // of what makes retail's harbour read as water rather than as tar.
    let toEye = normalize(uniforms.eye.xyz - in.worldPosition);
    let facing = clamp(toEye.y, 0.0, 1.0);
    let fresnel = pow(1.0 - facing, 3.0);
    colour = mix(colour, uniforms.fogColour.rgb * mix(0.55, 0.85, sea), fresnel * mix(0.45, 0.7, sea));

    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    // Clear enough to see the bed through it, which is most of what makes water
    // read as water rather than as a coloured lid. Valkurm's shallow bay shows
    // its fish and its sand: a deep sea hides its bed only because the bed is
    // too far down and too dark to show, not because the surface is a lid. So
    // the sea is translucent too, held opaque enough that its teal survives the
    // bright sand under it rather than washing back to grey.
    let alpha = clamp(mix(0.50, 0.64, sea) + foam * 0.22 + fresnel * 0.1 * sea, 0.0, 0.9);
    return vec4<f32>(colour, alpha);
}
)";
} // namespace mh
