Texture2D g_Texture;
SamplerState g_Texture_sampler;

cbuffer PixelConstants
{
    float4 g_TintColor;
    float4 g_LightDirection;
    float4 g_AmbientColor;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 WorldNormal : TEXCOORD0;
    float3 WorldPosition : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

float4 main(PSInput input) : SV_TARGET
{
    const float4 texel = g_Texture.Sample(g_Texture_sampler, input.TexCoord);
    const float3 albedo = texel.rgb * g_TintColor.rgb;
    const float alpha = texel.a * g_TintColor.a;

    const float3 normal = normalize(input.WorldNormal);
    const float3 lightDir = normalize(-g_LightDirection.xyz);
    const float3 viewDir = normalize(float3(0.25, 0.55, -1.0));
    const float3 halfDir = normalize(lightDir + viewDir);

    const float ndotl = saturate(dot(normal, lightDir));
    const float hemi = normal.y * 0.5 + 0.5;
    const float specular = pow(saturate(dot(normal, halfDir)), 20.0) * 0.12;
    const float rim = pow(1.0 - saturate(dot(normal, viewDir)), 3.0) * 0.08;

    const float3 ambient = lerp(float3(0.12, 0.13, 0.16), g_AmbientColor.rgb, hemi);
    const float3 diffuse = albedo * (ambient + ndotl * 0.86);
    const float3 litColor = diffuse + specular.xxx + rim.xxx;

    return float4(litColor, alpha);
}
