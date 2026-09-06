#pragma once

// The effect shader: the zone shader with a scrolling texture.
//
// Generator-placed meshes - the fountain's jets and flames, a waterfall's
// sheet - are ordinary geometry whose texture the game slides along over
// time, at a rate the generator gives (op 0x28). Everything else here is the
// zone shader's: the same vertex layout and placement matrices, the same
// lighting and fog. The lighting is only half applied, because a flame lit by
// the night's ambient came out black.

namespace mh
{
inline constexpr const char* kEffectShader = R"(
struct Uniforms {
    viewProjection : mat4x4<f32>,
    lightDirection : vec4<f32>,
    ambient : vec4<f32>,
    sunlight : vec4<f32>,
    fogColour : vec4<f32>,
    fogRange : vec4<f32>,
    eye : vec4<f32>,
};

// x, y: texture scroll in uv per second. z: how much of the zone's lighting
// applies, 0 for self-lit. w: 1 for a sky object - placed relative to the
// eye rather than the world, and never fogged; 0 for scenery.
struct Effect {
    scroll : vec4<f32>,
    // A shoreline wave, from the curves its generator names, evaluated on
    // this frame. x, y: an offset in uv - a position, not a rate, so it can
    // run up the sheet and back. z: opacity. w: unused. (0, 0, 1, 0) for
    // everything that is not a wave.
    wave : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var zoneTexture : texture_2d<f32>;
@group(0) @binding(2) var zoneSampler : sampler;
@group(0) @binding(3) var<uniform> effect : Effect;

struct VertexOut {
    @builtin(position) clipPosition : vec4<f32>,
    @location(0) normal : vec3<f32>,
    @location(1) uv : vec2<f32>,
    @location(2) worldPosition : vec3<f32>,
    @location(3) colour : vec4<f32>,
};

@vertex
fn vertexMain(@location(0) position : vec3<f32>,
              @location(1) normal : vec3<f32>,
              @location(2) uv : vec2<f32>,
              @location(7) colour : vec4<f32>,
              @location(3) m0 : vec4<f32>,
              @location(4) m1 : vec4<f32>,
              @location(5) m2 : vec4<f32>,
              @location(6) m3 : vec4<f32>) -> VertexOut {
    let model = mat4x4<f32>(m0, m1, m2, m3);

    var out : VertexOut;
    let world = model * vec4<f32>(position, 1.0) + vec4<f32>(uniforms.eye.xyz, 0.0) * effect.scroll.w;
    out.clipPosition = uniforms.viewProjection * world;
    out.normal = (model * vec4<f32>(normal, 0.0)).xyz;
    // eye.w carries the animation clock, in seconds.
    //
    // A wave (wave.w carries its z stretch, always at least one) tiles the
    // foam texture along the strip: the geometry is stretched in z below the
    // model's own uv, so without repeating the v the one copy of the texture
    // smears the length of the strip. Multiplying v by the stretch repeats it
    // that many times, which is what turns one sheet into a run of foam bands.
    var localUv = uv;
    if (effect.wave.w > 0.5) {
        localUv.y = uv.y * effect.wave.w;
    }
    out.uv = localUv + effect.scroll.xy * uniforms.eye.w + effect.wave.xy;
    out.worldPosition = world.xyz;
    out.colour = colour;
    return out;
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    let n = normalize(in.normal);
    let lambert = abs(dot(n, normalize(uniforms.lightDirection.xyz)));
    let sampled = textureSample(zoneTexture, zoneSampler, in.uv);
    let alpha = clamp(4.0 * in.colour.a * sampled.a, 0.0, 1.0) * effect.wave.z;

    let light = uniforms.ambient.rgb + uniforms.sunlight.rgb * lambert;
    // Vertex colour at half scale, 0x80 meaning full - a sprite frame's
    // 0x808080 leaves the sheet's colour alone, a darker one dims it.
    let tinted = sampled.rgb * min(in.colour.rgb * 2.0, vec3<f32>(1.0, 1.0, 1.0));
    var colour = mix(tinted, tinted * light, effect.scroll.z);

    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0) * (1.0 - effect.scroll.w);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    // A shoreline wave is a sheet of foam lying flat on the water, and the
    // lighting above cannot be used on it. Its generator scales the placement
    // by (6, 0, 1) - the zero flattens the model into a sheet, and it flattens
    // the normals with it, so `lambert` comes out zero and every term that
    // depends on it goes to black. Drawn that way the foam darkened the sea
    // instead of whitening it: measured against the same frame without it, it
    // took a shoreline from (148, 150, 132) down to (115, 117, 102).
    //
    // So the sheet takes the ambient and nothing else, which is what a flat
    // white surface under an open sky gets anyway.
    if (effect.wave.w > 0.5) {
        let ambient = min(uniforms.ambient.rgb, vec3<f32>(1.0, 1.0, 1.0));
        var foam = sampled.rgb * min(in.colour.rgb * 2.0, vec3<f32>(1.0, 1.0, 1.0)) * (0.45 + 0.55 * ambient);
        foam = mix(foam, uniforms.fogColour.rgb, fog);
        return vec4<f32>(foam, alpha);
    }
    // The wet-sand wash (wave.w < 0): a dark opacity layer that fades over the
    // sand on the wave's own clock, not part of the water. wave.z is its
    // opacity curve - it swells as the wave runs up and clears as it draws
    // back - and scroll.z carries how heavy the darkening gets. Shaped by the
    // sheet's own texture alpha so it reads as the wash's pattern spreading
    // over the sand rather than a flat rectangle darkening.
    if (effect.wave.w < -0.5) {
        let shape = 0.35 + 0.65 * sampled.a;
        let washAlpha = clamp(shape * effect.wave.z * effect.scroll.z, 0.0, 0.85);
        let wet = vec3<f32>(0.05, 0.04, 0.03);
        return vec4<f32>(wet, washAlpha);
    }
    return vec4<f32>(colour, alpha);
}
)";
} // namespace mh
