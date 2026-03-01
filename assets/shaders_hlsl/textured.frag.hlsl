Texture2D g_Texture;
SamplerState g_Texture_sampler;

cbuffer PixelConstants
{
    float4 g_TintColor;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    return g_Texture.Sample(g_Texture_sampler, input.TexCoord) * g_TintColor;
}
