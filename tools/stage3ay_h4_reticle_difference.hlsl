Texture2D fullHud : register(t0);
Texture2D hudWithoutReticle : register(t1);
SamplerState linearClamp : register(s0);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target
{
    float4 fullPixel = fullHud.Sample(linearClamp, input.uv);
    float4 withoutPixel = hudWithoutReticle.Sample(linearClamp, input.uv);
    float4 delta = abs(fullPixel - withoutPixel);
    float coverage = max(max(delta.r, delta.g), max(delta.b, delta.a));
    if (coverage < (0.5 / 255.0))
        return 0.0;
    return float4(delta.rgb, coverage);
}
