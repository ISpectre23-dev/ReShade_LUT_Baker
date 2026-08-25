// ReShade LUT Baker companion preview shader.
//
// The CUBE file must use a 0..1 input domain and its LUT_3D_SIZE must match
// LUT_BAKER_CUBE_SIZE. Define LUT_BAKER_CUBE_FILENAME as the quoted basename
// of a specific export, add its directory to ReShade's texture search paths,
// then reload effects.

#ifndef LUT_BAKER_CUBE_FILENAME
#define LUT_BAKER_CUBE_FILENAME "LUT_Name.cube"
#endif

#ifndef LUT_BAKER_CUBE_SIZE
#define LUT_BAKER_CUBE_SIZE 64
#endif

#if LUT_BAKER_CUBE_SIZE < 2
#error "LUT_BAKER_CUBE_SIZE must be at least 2."
#endif

#include "ReShade.fxh"

texture3D LUTBakerVolume < source = LUT_BAKER_CUBE_FILENAME; >
{
	Width = LUT_BAKER_CUBE_SIZE;
	Height = LUT_BAKER_CUBE_SIZE;
	Depth = LUT_BAKER_CUBE_SIZE;
	MipLevels = 1;
	Format = RGBA32F;
};

sampler3D LUTBakerSampler
{
	Texture = LUTBakerVolume;
	AddressU = CLAMP;
	AddressV = CLAMP;
	AddressW = CLAMP;
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = POINT;
	SRGBTexture = false;
};

uniform int LUTBakerInterpolation <
	ui_type = "combo";
	ui_label = "Interpolation";
	ui_items = "Tetrahedral\0Trilinear\0";
	ui_tooltip = "Tetrahedral uses four LUT samples. Trilinear uses the GPU's native 3D texture filtering.";
> = 0;

uniform int LUTBakerPreviewMode <
	ui_type = "combo";
	ui_label = "Preview mode";
	ui_items = "Apply LUT\0Split: Input | LUT\0Absolute difference\0";
	ui_tooltip = "Apply the LUT, compare the unmodified input on the left with the LUT on the right, or display abs(input - LUT).";
> = 0;

uniform float LUTBakerSplitPosition <
	ui_type = "slider";
	ui_label = "Split position";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.01;
	ui_tooltip = "Horizontal position of the Input | LUT split.";
> = 0.5;

uniform float LUTBakerDifferenceGain <
	ui_type = "slider";
	ui_label = "Difference gain";
	ui_min = 1.0;
	ui_max = 32.0;
	ui_step = 0.25;
	ui_tooltip = "Multiplies the absolute-difference preview. A value of 1 shows the unscaled difference.";
> = 1.0;

float3 LUTBakerFetch(int3 coordinate)
{
	return tex3Dfetch(LUTBakerSampler, coordinate).rgb;
}

float3 LUTBakerSampleTrilinear(float3 color)
{
	const float size = float(LUT_BAKER_CUBE_SIZE);
	const float3 uvw = (saturate(color) * (size - 1.0) + 0.5) / size;
	return tex3D(LUTBakerSampler, uvw).rgb;
}

float3 LUTBakerSampleTetrahedral(float3 color)
{
	const float3 position = saturate(color) * float(LUT_BAKER_CUBE_SIZE - 1);
	const int max_base = LUT_BAKER_CUBE_SIZE - 2;
	const int3 base = min(int3(floor(position)), int3(max_base, max_base, max_base));
	const float3 fraction = position - float3(base);

	const int3 x = int3(1, 0, 0);
	const int3 y = int3(0, 1, 0);
	const int3 z = int3(0, 0, 1);
	const int3 xyz = int3(1, 1, 1);

	const float3 c000 = LUTBakerFetch(base);
	const float3 c111 = LUTBakerFetch(base + xyz);

	if (fraction.x >= fraction.y)
	{
		if (fraction.y >= fraction.z) // x >= y >= z: 000 -> 100 -> 110 -> 111
		{
			const float3 c100 = LUTBakerFetch(base + x);
			const float3 c110 = LUTBakerFetch(base + x + y);
			return c000
				+ fraction.x * (c100 - c000)
				+ fraction.y * (c110 - c100)
				+ fraction.z * (c111 - c110);
		}

		if (fraction.x >= fraction.z) // x >= z >= y: 000 -> 100 -> 101 -> 111
		{
			const float3 c100 = LUTBakerFetch(base + x);
			const float3 c101 = LUTBakerFetch(base + x + z);
			return c000
				+ fraction.x * (c100 - c000)
				+ fraction.z * (c101 - c100)
				+ fraction.y * (c111 - c101);
		}

		// z >= x >= y: 000 -> 001 -> 101 -> 111
		const float3 c001 = LUTBakerFetch(base + z);
		const float3 c101 = LUTBakerFetch(base + x + z);
		return c000
			+ fraction.z * (c001 - c000)
			+ fraction.x * (c101 - c001)
			+ fraction.y * (c111 - c101);
	}

	if (fraction.x >= fraction.z) // y >= x >= z: 000 -> 010 -> 110 -> 111
	{
		const float3 c010 = LUTBakerFetch(base + y);
		const float3 c110 = LUTBakerFetch(base + x + y);
		return c000
			+ fraction.y * (c010 - c000)
			+ fraction.x * (c110 - c010)
			+ fraction.z * (c111 - c110);
	}

	if (fraction.y >= fraction.z) // y >= z >= x: 000 -> 010 -> 011 -> 111
	{
		const float3 c010 = LUTBakerFetch(base + y);
		const float3 c011 = LUTBakerFetch(base + y + z);
		return c000
			+ fraction.y * (c010 - c000)
			+ fraction.z * (c011 - c010)
			+ fraction.x * (c111 - c011);
	}

	// z >= y >= x: 000 -> 001 -> 011 -> 111
	const float3 c001 = LUTBakerFetch(base + z);
	const float3 c011 = LUTBakerFetch(base + y + z);
	return c000
		+ fraction.z * (c001 - c000)
		+ fraction.y * (c011 - c001)
		+ fraction.x * (c111 - c011);
}

float4 LUTBakerPreviewPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	const float4 input = tex2D(ReShade::BackBuffer, texcoord);
	const float3 graded = LUTBakerInterpolation == 0
		? LUTBakerSampleTetrahedral(input.rgb)
		: LUTBakerSampleTrilinear(input.rgb);

	float3 output = graded;

	if (LUTBakerPreviewMode == 1)
	{
		const float split_x = saturate(LUTBakerSplitPosition) * float(BUFFER_WIDTH);
		output = position.x < split_x ? input.rgb : graded;

		// One-screen-pixel box-filtered divider, centered on the exact split.
		const float divider_coverage = saturate(1.0 - abs(position.x - split_x));
		output = lerp(output, float3(1.0, 1.0, 1.0), divider_coverage);
	}
	else if (LUTBakerPreviewMode == 2)
		output = abs(input.rgb - graded) * LUTBakerDifferenceGain;

	return float4(output, input.a);
}

technique ReShadeLUTPreview <
	ui_label = "ReShade LUT Preview";
	ui_tooltip = "Loads the explicitly configured ReShade LUT Baker CUBE file as a native FP32 3D texture.";
>
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = LUTBakerPreviewPS;
	}
}
