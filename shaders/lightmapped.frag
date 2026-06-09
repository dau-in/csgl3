// halflife.glsl - goldsrc ubershader
#include "common.glsl"
#include "brush_common.glsl"

uniform sampler2D u_texture;
uniform sampler2D u_lightmap;

#if defined(DETAIL)
uniform sampler2D u_detail;
uniform vec2 u_detailScale;
#endif

in vec3 fragPosition;
in vec4 texCoord;

flat in vec4 f_lightmapWeights;
flat in float f_lightmapWidth;

in float f_fogFactor;

out vec4 fragColor;

// made this the fuck up, completely wrong and based on nothing
#if defined(HAS_DLIGHTS)
vec3 AddLight(vec3 pos, float invRadius, vec3 color)
{
    float dist = distance(pos, fragPosition);
    float attenuation = max(0.0, 1.0 - dist * invRadius);
    return color * attenuation * 0.3; // wtf is 0.3? no idea
}
#endif

vec3 Brighten(vec3 f)
{
    vec3 a = (f / k_brighten) * 0.125;
    vec3 b = (f - k_brighten) / (1.0 - k_brighten) * 0.875 + 0.125;
    return mix(a, b, step(k_brighten, f));
}

vec3 ApplyBrightness(vec3 value)
{
    value = pow(value, vec3(k_lightgamma));

#if !defined(OVERBRIGHT)
    value = min(value * 2.0, 1.0);
#endif

    value *= max(k_brightness, 1.0);
    value = Brighten(value);
    value = pow(value, vec3(1.0 / k_gamma));

    value = min(value, 1.0);

#if defined(OVERBRIGHT)
    value *= (255.0 / 192.0);
#endif

    return value;
}

void main()
{
    // discard might turn off early z, so we have it as a shader variant
    vec4 diffuse = texture(u_texture, texCoord.xy);
#if defined(ALPHA_TEST)
    if (diffuse.a < 0.25)
    {
        discard;
        return;
    }
#endif

#if defined(DETAIL)
    vec3 detail = texture(u_detail, texCoord.xy * u_detailScale).rgb;
    diffuse.rgb *= detail * 2.0;
#endif

    vec3 lightmap = f_lightmapWeights[0] * texture(u_lightmap, texCoord.zw).rgb;

#if defined(MULTI_STYLE)
    vec2 uvOffset = vec2(f_lightmapWidth, 0.0);
    lightmap += f_lightmapWeights[1] * texture(u_lightmap, texCoord.zw + uvOffset * 1.0).rgb;
    lightmap += f_lightmapWeights[2] * texture(u_lightmap, texCoord.zw + uvOffset * 2.0).rgb;
    lightmap += f_lightmapWeights[3] * texture(u_lightmap, texCoord.zw + uvOffset * 3.0).rgb;
#endif

#if defined(HAS_DLIGHTS)
    for (int i = 0; i < MAX_SHADER_LIGHTS; i++)
    {
        lightmap += AddLight(lightPositions[i].xyz,
            lightPositions[i].w,
            lightColors[i].rgb);
    }
#endif

    lightmap = ApplyBrightness(lightmap);
    diffuse.rgb *= lightmap;

    diffuse.rgb = mix(fogColor.rgb, diffuse.rgb, f_fogFactor);

    fragColor = diffuse;
}
