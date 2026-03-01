cbuffer PixelConstants
{
    float4 g_TintColor;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 Color : COLOR0;
};

float4 main(PSInput input) : SV_TARGET
{
    return float4(input.Color, 1.0) * g_TintColor;
}
