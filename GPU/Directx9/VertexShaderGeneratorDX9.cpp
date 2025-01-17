// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <stdio.h>
#include <locale.h>

#if defined(_WIN32) && defined(_DEBUG)
#include "Common/CommonWindows.h"
#endif

#include "base/stringutil.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#include "Core/Config.h"

#include "GPU/Directx9/VertexShaderGeneratorDX9.h"
#include "GPU/Common/VertexDecoderCommon.h"

#undef WRITE

#define WRITE p+=sprintf

namespace DX9 {

static const char * const boneWeightAttrDecl[9] = {	
	"#ERROR#",
	"float  a_w1:TEXCOORD1;\n",
	"float2 a_w1:TEXCOORD1;\n",
	"float3 a_w1:TEXCOORD1;\n",
	"float4 a_w1:TEXCOORD1;\n",
	"float4 a_w1:TEXCOORD1;\n float  a_w2:TEXCOORD2;\n",
	"float4 a_w1:TEXCOORD1;\n float2 a_w2:TEXCOORD2;\n",
	"float4 a_w1:TEXCOORD1;\n float3 a_w2:TEXCOORD2;\n",
	"float4 a_w1:TEXCOORD1;\n float4 a_w2:TEXCOORD2;\n",
};

enum DoLightComputation {
	LIGHT_OFF,
	LIGHT_SHADE,
	LIGHT_FULL,
};

void GenerateVertexShaderDX9(const ShaderID &id, char *buffer) {
	char *p = buffer;
	const u32 vertType = gstate.vertType;

	bool isModeThrough = id.Bit(VS_BIT_IS_THROUGH);
	bool lmode = id.Bit(VS_BIT_LMODE) && !isModeThrough;  // TODO: Different expression than in shaderIDgen
	bool doTexture = id.Bit(VS_BIT_DO_TEXTURE);
	bool doTextureProjection = id.Bit(VS_BIT_DO_TEXTURE_PROJ);

	GETexMapMode uvGenMode = static_cast<GETexMapMode>(id.Bits(VS_BIT_UVGEN_MODE, 2));

	// this is only valid for some settings of uvGenMode
	GETexProjMapMode uvProjMode = static_cast<GETexProjMapMode>(id.Bits(VS_BIT_UVPROJ_MODE, 2));
	bool doShadeMapping = uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP;
	bool doFlatShading = id.Bit(VS_BIT_FLATSHADE);

	bool useHWTransform = id.Bit(VS_BIT_USE_HW_TRANSFORM);
	bool hasColor = id.Bit(VS_BIT_HAS_COLOR) || !useHWTransform;
	bool hasNormal = id.Bit(VS_BIT_HAS_NORMAL) && useHWTransform;
	bool hasTexcoord = id.Bit(VS_BIT_HAS_TEXCOORD) || !useHWTransform;
	bool enableFog = id.Bit(VS_BIT_ENABLE_FOG);
	bool throughmode = id.Bit(VS_BIT_IS_THROUGH);
	bool flipNormal = id.Bit(VS_BIT_NORM_REVERSE);
	int ls0 = id.Bits(VS_BIT_LS0, 2);
	int ls1 = id.Bits(VS_BIT_LS1, 2);
	bool enableBones = id.Bit(VS_BIT_ENABLE_BONES);
	bool enableLighting = id.Bit(VS_BIT_LIGHTING_ENABLE);
	int matUpdate = id.Bits(VS_BIT_MATERIAL_UPDATE, 3);

	bool prescale = g_Config.bPrescaleUV && !throughmode && (uvGenMode == GE_TEXMAP_TEXTURE_COORDS || uvGenMode == GE_TEXMAP_UNKNOWN);

	DoLightComputation doLight[4] = { LIGHT_OFF, LIGHT_OFF, LIGHT_OFF, LIGHT_OFF };
	if (useHWTransform) {
		int shadeLight0 = doShadeMapping ? ls0 : -1;
		int shadeLight1 = doShadeMapping ? ls1 : -1;
		for (int i = 0; i < 4; i++) {
			if (i == shadeLight0 || i == shadeLight1)
				doLight[i] = LIGHT_SHADE;
			if (id.Bit(VS_BIT_LIGHTING_ENABLE) && id.Bit(VS_BIT_LIGHT0_ENABLE + i))
				doLight[i] = LIGHT_FULL;
		}
	}

	int numBoneWeights = 0;
	int boneWeightScale = id.Bits(VS_BIT_WEIGHT_FMTSCALE, 2);
	if (enableBones) {
		numBoneWeights = 1 + id.Bits(VS_BIT_BONES, 3);
	}
	int texFmtScale = id.Bits(VS_BIT_TEXCOORD_FMTSCALE, 2);

	WRITE(p, "#pragma warning( disable : 3571 )\n");

	if (isModeThrough)	{
		WRITE(p, "float4x4 u_proj_through : register(c%i);\n", CONST_VS_PROJ_THROUGH);
	} else {
		WRITE(p, "float4x4 u_proj : register(c%i);\n", CONST_VS_PROJ);
		// Add all the uniforms we'll need to transform properly.
	}

	if (enableFog) {
		WRITE(p, "float2 u_fogcoef : register(c%i);\n", CONST_VS_FOGCOEF);
	}
	if (useHWTransform || !hasColor)
		WRITE(p, "float4 u_matambientalpha : register(c%i);\n", CONST_VS_MATAMBIENTALPHA);  // matambient + matalpha

	if (useHWTransform) {
		// When transforming by hardware, we need a great deal more uniforms...
		WRITE(p, "float4x3 u_world : register(c%i);\n", CONST_VS_WORLD);
		WRITE(p, "float4x3 u_view : register(c%i);\n", CONST_VS_VIEW);
		if (doTextureProjection)
			WRITE(p, "float4x3 u_texmtx : register(c%i);\n", CONST_VS_TEXMTX);
		if (enableBones) {
#ifdef USE_BONE_ARRAY
			WRITE(p, "float4x3 u_bone[%i] : register(c%i);\n", numBones, CONST_VS_BONE0);
#else
			for (int i = 0; i < numBoneWeights; i++) {
				WRITE(p, "float4x3 u_bone%i : register(c%i);\n", i, CONST_VS_BONE0 + i * 3);
			}
#endif
		}
		if (doTexture) {
			WRITE(p, "float4 u_uvscaleoffset : register(c%i);\n", CONST_VS_UVSCALEOFFSET);
		}
		for (int i = 0; i < 4; i++) {
			if (doLight[i] != LIGHT_OFF) {
				// This is needed for shade mapping
				WRITE(p, "float3 u_lightpos%i : register(c%i);\n", i, CONST_VS_LIGHTPOS + i);
			}
			if (doLight[i] == LIGHT_FULL) {
				GELightType type = static_cast<GELightType>(id.Bits(VS_BIT_LIGHT0_TYPE + 4 * i, 2));
				GELightComputation comp = static_cast<GELightComputation>(id.Bits(VS_BIT_LIGHT0_COMP + 4 * i, 2));

				if (type != GE_LIGHTTYPE_DIRECTIONAL)
					WRITE(p, "float3 u_lightatt%i : register(c%i);\n", i, CONST_VS_LIGHTATT + i);

				if (type == GE_LIGHTTYPE_SPOT || type == GE_LIGHTTYPE_UNKNOWN) { 
					WRITE(p, "float3 u_lightdir%i : register(c%i);\n", i, CONST_VS_LIGHTDIR + i);
					WRITE(p, "float u_lightangle%i : register(c%i);\n", i, CONST_VS_LIGHTANGLE + i);
					WRITE(p, "float u_lightspotCoef%i : register(c%i);\n", i, CONST_VS_LIGHTSPOTCOEF + i);
				}
				WRITE(p, "float3 u_lightambient%i : register(c%i);\n", i, CONST_VS_LIGHTAMBIENT + i);
				WRITE(p, "float3 u_lightdiffuse%i : register(c%i);\n", i, CONST_VS_LIGHTDIFFUSE + i);

				if (comp != GE_LIGHTCOMP_ONLYDIFFUSE) {
					WRITE(p, "float3 u_lightspecular%i : register(c%i);\n", i, CONST_VS_LIGHTSPECULAR + i);
				}
			}
		}
		if (enableLighting) {
			WRITE(p, "float4 u_ambient : register(c%i);\n", CONST_VS_AMBIENT);
			if ((gstate.materialupdate & 2) == 0 || !hasColor)
				WRITE(p, "float3 u_matdiffuse : register(c%i);\n", CONST_VS_MATDIFFUSE);
			// if ((gstate.materialupdate & 4) == 0)
			WRITE(p, "float4 u_matspecular : register(c%i);\n", CONST_VS_MATSPECULAR);  // Specular coef is contained in alpha
			WRITE(p, "float3 u_matemissive : register(c%i);\n", CONST_VS_MATEMISSIVE);
		}
	}

	if (!isModeThrough && gstate_c.Supports(GPU_ROUND_DEPTH_TO_16BIT)) {
		WRITE(p, "float4 u_depthRange : register(c%i);\n", CONST_VS_DEPTHRANGE);
	}

	// And the "varyings".
	if (useHWTransform) {
		WRITE(p, "struct VS_IN {                              \n");
		if (enableBones) {
			WRITE(p, "%s", boneWeightAttrDecl[numBoneWeights]);
		}
		if (doTexture && hasTexcoord) {
			WRITE(p, "  float2 texcoord : TEXCOORD0;\n");
		}
		if (hasColor)  {
			WRITE(p, "  float4 color0 : COLOR0;\n");
		}
		if (hasNormal) {
			WRITE(p, "  float3 normal : NORMAL;\n");
		}
		WRITE(p, "  float3 position : POSITION;\n");
		WRITE(p, "};\n");
		
	} else {
		WRITE(p, "struct VS_IN {\n");
		WRITE(p, "  float4 position : POSITION;\n");
		if (doTexture && hasTexcoord) {
			if (doTextureProjection && !throughmode)
				WRITE(p, "  float3 texcoord : TEXCOORD0;\n");
			else
				WRITE(p, "  float2 texcoord : TEXCOORD0;\n");
		}
		if (hasColor) {
			WRITE(p, "  float4 color0 : COLOR0;\n");
		}
		// only software transform supplies color1 as vertex data
		if (lmode) {
			WRITE(p, "  float4 color1 : COLOR1;\n");
		}
		WRITE(p, "};\n");
	}

	WRITE(p, "struct VS_OUT {\n");
	WRITE(p, "  float4 gl_Position   : POSITION;\n");
	if (doTexture) {
		if (doTextureProjection)
			WRITE(p, "  float3 v_texcoord: TEXCOORD0;\n");
		else
			WRITE(p, "  float2 v_texcoord: TEXCOORD0;\n");
	}
	WRITE(p, "  float4 v_color0    : COLOR0;\n");
	if (lmode)
		WRITE(p, "  float3 v_color1    : COLOR1;\n");

	if (enableFog) {
		WRITE(p, "  float2 v_fogdepth: TEXCOORD1;\n");
	}
	WRITE(p, "};\n");

	// Confirmed: Through mode gets through exactly the same in GL and D3D in Phantasy Star: Text is 38023.0 in the test scene.

	if (!isModeThrough && gstate_c.Supports(GPU_ROUND_DEPTH_TO_16BIT)) {
		// Apply the projection and viewport to get the Z buffer value, floor to integer, undo the viewport and projection.
		// The Z range in D3D is different but we compensate for that using parameters.
		WRITE(p, "\nfloat4 depthRoundZVP(float4 v) {\n");
		WRITE(p, "  float z = v.z / v.w;\n");
		WRITE(p, "  z = (z * u_depthRange.x + u_depthRange.y);\n");
		WRITE(p, "  z = floor(z);\n");
		WRITE(p, "  z = (z - u_depthRange.z) * u_depthRange.w;\n");
		WRITE(p, "  return float4(v.x, v.y, z * v.w, v.w);\n");
		WRITE(p, "}\n\n");
	}

	WRITE(p, "VS_OUT main(VS_IN In) {\n");
	WRITE(p, "  VS_OUT Out = (VS_OUT)0;							   \n");  
	if (!useHWTransform) {
		// Simple pass-through of vertex data to fragment shader
		if (doTexture) {
			if (doTextureProjection) {
				if (throughmode) {
					WRITE(p, "  Out.v_texcoord = float3(In.texcoord.x, In.texcoord.y, 1.0);\n");
				} else {
					WRITE(p, "  Out.v_texcoord = In.texcoord;\n");
				}
			} else {
				WRITE(p, "  Out.v_texcoord = In.texcoord.xy;\n");
			}
		}
		if (hasColor) {
			WRITE(p, "  Out.v_color0 = In.color0;\n");
			if (lmode)
				WRITE(p, "  Out.v_color1 = In.color1.rgb;\n");
		} else {
			WRITE(p, "  Out.v_color0 = In.u_matambientalpha;\n");
			if (lmode)
				WRITE(p, "  Out.v_color1 = float3(0.0);\n");
		}
		if (enableFog) {
			WRITE(p, "  Out.v_fogdepth.x = In.position.w;\n");
		}
		if (gstate.isModeThrough())	{
			WRITE(p, "  Out.gl_Position = mul(float4(In.position.xyz, 1.0), u_proj_through);\n");
		} else {
			if (gstate_c.Supports(GPU_ROUND_DEPTH_TO_16BIT)) {
				WRITE(p, "  Out.gl_Position = depthRoundZVP(mul(float4(In.position.xyz, 1.0), u_proj));\n");
			} else {
				WRITE(p, "  Out.gl_Position = mul(float4(In.position.xyz, 1.0), u_proj);\n");
			}
		}
	}  else {
		// Step 1: World Transform / Skinning
		if (!enableBones) {
			// No skinning, just standard T&L.
			WRITE(p, "  float3 worldpos = mul(float4(In.position.xyz, 1.0), u_world);\n");
			if (hasNormal)
				WRITE(p, "  float3 worldnormal = normalize(	mul(float4(%sIn.normal, 0.0), u_world));\n", flipNormal ? "-" : "");
			else
				WRITE(p, "  float3 worldnormal = float3(0.0, 0.0, 1.0);\n");
		} else {
			static const char * const boneWeightAttr[8] = {
				"a_w1.x", "a_w1.y", "a_w1.z", "a_w1.w",
				"a_w2.x", "a_w2.y", "a_w2.z", "a_w2.w",
			};

#if defined(USE_FOR_LOOP) && defined(USE_BONE_ARRAY)

			// To loop through the weights, we unfortunately need to put them in a float array.
			// GLSL ES sucks - no way to directly initialize an array!
			switch (numBoneWeights) {
			case 1: WRITE(p, "  float w[1]; w[0] = a_w1;\n"); break;
			case 2: WRITE(p, "  float w[2]; w[0] = a_w1.x; w[1] = a_w1.y;\n"); break;
			case 3: WRITE(p, "  float w[3]; w[0] = a_w1.x; w[1] = a_w1.y; w[2] = a_w1.z;\n"); break;
			case 4: WRITE(p, "  float w[4]; w[0] = a_w1.x; w[1] = a_w1.y; w[2] = a_w1.z; w[3] = a_w1.w;\n"); break;
			case 5: WRITE(p, "  float w[5]; w[0] = a_w1.x; w[1] = a_w1.y; w[2] = a_w1.z; w[3] = a_w1.w; w[4] = a_w2;\n"); break;
			case 6: WRITE(p, "  float w[6]; w[0] = a_w1.x; w[1] = a_w1.y; w[2] = a_w1.z; w[3] = a_w1.w; w[4] = a_w2.x; w[5] = a_w2.y;\n"); break;
			case 7: WRITE(p, "  float w[7]; w[0] = a_w1.x; w[1] = a_w1.y; w[2] = a_w1.z; w[3] = a_w1.w; w[4] = a_w2.x; w[5] = a_w2.y; w[6] = a_w2.z;\n"); break;
			case 8: WRITE(p, "  float w[8]; w[0] = a_w1.x; w[1] = a_w1.y; w[2] = a_w1.z; w[3] = a_w1.w; w[4] = a_w2.x; w[5] = a_w2.y; w[6] = a_w2.z; w[7] = a_w2.w;\n"); break;
			}

			WRITE(p, "  mat4 skinMatrix = w[0] * u_bone[0];\n");
			if (numBoneWeights > 1) {
				WRITE(p, "  for (int i = 1; i < %i; i++) {\n", numBoneWeights);
				WRITE(p, "    skinMatrix += w[i] * u_bone[i];\n");
				WRITE(p, "  }\n");
			}

#else

#ifdef USE_BONE_ARRAY
			if (numBoneWeights == 1)
				WRITE(p, "  float4x3 skinMatrix = a_w1 * u_bone[0]");
			else
				WRITE(p, "  float4x3 skinMatrix = a_w1.x * u_bone[0]");
			for (int i = 1; i < numBoneWeights; i++) {
				const char *weightAttr = boneWeightAttr[i];
				// workaround for "cant do .x of scalar" issue
				if (numBoneWeights == 1 && i == 0) weightAttr = "a_w1";
				if (numBoneWeights == 5 && i == 4) weightAttr = "a_w2";
				WRITE(p, " + %s * u_bone[%i]", weightAttr, i);
			}
#else
			// Uncomment this to screw up bone shaders to check the vertex shader software fallback
			// WRITE(p, "THIS SHOULD ERROR! #error");
			if (numBoneWeights == 1)
				WRITE(p, "  float4x3 skinMatrix = mul(In.a_w1, u_bone0)");
			else
				WRITE(p, "  float4x3 skinMatrix = mul(In.a_w1.x, u_bone0)");
			for (int i = 1; i < numBoneWeights; i++) {
				const char *weightAttr = boneWeightAttr[i];
				// workaround for "cant do .x of scalar" issue
				if (numBoneWeights == 1 && i == 0) weightAttr = "a_w1";
				if (numBoneWeights == 5 && i == 4) weightAttr = "a_w2";
				WRITE(p, " + mul(In.%s, u_bone%i)", weightAttr, i);
			}
#endif

#endif
			WRITE(p, ";\n");

			// Trying to simplify this results in bugs in LBP...
			WRITE(p, "  float3 skinnedpos = mul(float4(In.position.xyz, 1.0), skinMatrix);\n");
			WRITE(p, "  float3 worldpos = mul(float4(skinnedpos, 1.0), u_world);\n");

			if (hasNormal) {
				WRITE(p, "  float3 skinnednormal = mul(float4(%sIn.normal, 0.0), skinMatrix);\n", flipNormal ? "-" : "");
			} else {
				WRITE(p, "  float3 skinnednormal = mul(float4(0.0, 0.0, %s1.0, 0.0), skinMatrix);\n", flipNormal ? "-" : "");
			}
			WRITE(p, "  float3 worldnormal = normalize(mul(float4(skinnednormal, 0.0), u_world));\n");
		}

		WRITE(p, "  float4 viewPos = float4(mul(float4(worldpos, 1.0), u_view), 1.0);\n");

		// Final view and projection transforms.
		if (gstate_c.Supports(GPU_ROUND_DEPTH_TO_16BIT)) {
			WRITE(p, "  Out.gl_Position = depthRoundZVP(mul(viewPos, u_proj));\n");
		} else {
			WRITE(p, "  Out.gl_Position = mul(viewPos, u_proj);\n");
		}

		// TODO: Declare variables for dots for shade mapping if needed.

		const char *ambientStr = (gstate.materialupdate & 1) && hasColor ? "In.color0" : "u_matambientalpha";
		const char *diffuseStr = (gstate.materialupdate & 2) && hasColor ? "In.color0.rgb" : "u_matdiffuse";
		const char *specularStr = (gstate.materialupdate & 4) && hasColor ? "In.color0.rgb" : "u_matspecular.rgb";

		bool diffuseIsZero = true;
		bool specularIsZero = true;
		bool distanceNeeded = false;

		if (enableLighting) {
			WRITE(p, "  float4 lightSum0 = u_ambient * %s + float4(u_matemissive, 0.0);\n", ambientStr);

			for (int i = 0; i < 4; i++) {
				GELightType type = static_cast<GELightType>(id.Bits(VS_BIT_LIGHT0_TYPE + 4 * i, 2));
				GELightComputation comp = static_cast<GELightComputation>(id.Bits(VS_BIT_LIGHT0_COMP + 4 * i, 2));
				if (doLight[i] != LIGHT_FULL)
					continue;
				diffuseIsZero = false;
				if (comp != GE_LIGHTCOMP_ONLYDIFFUSE)
					specularIsZero = false;
				if (type != GE_LIGHTTYPE_DIRECTIONAL)
					distanceNeeded = true;
			}

			if (!specularIsZero) {
				WRITE(p, "  float3 lightSum1 = 0;\n");
			}
			if (!diffuseIsZero) {
				WRITE(p, "  float3 toLight;\n");
				WRITE(p, "  float3 diffuse;\n");
			}
			if (distanceNeeded) {
				WRITE(p, "  float distance;\n");
				WRITE(p, "  float lightScale;\n");
			}
		}

		// Calculate lights if needed. If shade mapping is enabled, lights may need to be
		// at least partially calculated.
		for (int i = 0; i < 4; i++) {
			if (doLight[i] != LIGHT_FULL)
				continue;

			GELightType type = static_cast<GELightType>(id.Bits(VS_BIT_LIGHT0_TYPE + 4 * i, 2));
			GELightComputation comp = static_cast<GELightComputation>(id.Bits(VS_BIT_LIGHT0_COMP + 4 * i, 2));

			if (type == GE_LIGHTTYPE_DIRECTIONAL) {
				// We prenormalize light positions for directional lights.
				WRITE(p, "  toLight = u_lightpos%i;\n", i);
			} else {
				WRITE(p, "  toLight = u_lightpos%i - worldpos;\n", i);
				WRITE(p, "  distance = length(toLight);\n");
				WRITE(p, "  toLight /= distance;\n");
			}

			bool doSpecular = comp != GE_LIGHTCOMP_ONLYDIFFUSE;
			bool poweredDiffuse = comp == GE_LIGHTCOMP_BOTHWITHPOWDIFFUSE;

			if (poweredDiffuse) {
				WRITE(p, "  float dot%i = pow(dot(toLight, worldnormal), u_matspecular.a);\n", i);
				// TODO: Somehow the NaN check from GLES seems unnecessary here?
				// If it returned 0, it'd be wrong, so that's strange.
			} else {
				WRITE(p, "  float dot%i = dot(toLight, worldnormal);\n", i);
			}

			const char *timesLightScale = " * lightScale";

			// Attenuation
			switch (type) {
			case GE_LIGHTTYPE_DIRECTIONAL:
				timesLightScale = "";
				break;
			case GE_LIGHTTYPE_POINT:
				WRITE(p, "  lightScale = clamp(1.0 / dot(u_lightatt%i, float3(1.0, distance, distance*distance)), 0.0, 1.0);\n", i);
				break;
			case GE_LIGHTTYPE_SPOT:
			case GE_LIGHTTYPE_UNKNOWN:
				WRITE(p, "  float angle%i = dot(normalize(u_lightdir%i), toLight);\n", i, i);
				WRITE(p, "  if (angle%i >= u_lightangle%i) {\n", i, i);
				WRITE(p, "    lightScale = clamp(1.0 / dot(u_lightatt%i, float3(1.0, distance, distance*distance)), 0.0, 1.0) * pow(angle%i, u_lightspotCoef%i);\n", i, i, i);
				WRITE(p, "  } else {\n");
				WRITE(p, "    lightScale = 0.0;\n");
				WRITE(p, "  }\n");
				break;
			default:
				// ILLEGAL
				break;
			}

			WRITE(p, "  diffuse = (u_lightdiffuse%i * %s) * max(dot%i, 0.0);\n", i, diffuseStr, i);
			if (doSpecular) {
				WRITE(p, "  dot%i = dot(normalize(toLight + float3(0.0, 0.0, 1.0)), worldnormal);\n", i);
				WRITE(p, "  if (dot%i > 0.0)\n", i);
				WRITE(p, "    lightSum1 += u_lightspecular%i * %s * (pow(dot%i, u_matspecular.a) %s);\n", i, specularStr, i, timesLightScale);
			}
			WRITE(p, "  lightSum0.rgb += (u_lightambient%i * %s.rgb + diffuse)%s;\n", i, ambientStr, timesLightScale);
		}

		if (enableLighting) {
			// Sum up ambient, emissive here.
			if (lmode) {
				WRITE(p, "  Out.v_color0 = clamp(lightSum0, 0.0, 1.0);\n");
				// v_color1 only exists when lmode = 1.
				if (specularIsZero) {
					WRITE(p, "  Out.v_color1 = float3(0, 0, 0);\n");
				} else {
					WRITE(p, "  Out.v_color1 = clamp(lightSum1, 0.0, 1.0);\n");
				}
			} else {
				if (specularIsZero) {
					WRITE(p, "  Out.v_color0 = clamp(lightSum0, 0.0, 1.0);\n");
				} else {
					WRITE(p, "  Out.v_color0 = clamp(clamp(lightSum0, 0.0, 1.0) + float4(lightSum1, 0.0), 0.0, 1.0);\n");
				}
			}
		} else {
			// Lighting doesn't affect color.
			if (hasColor) {
				WRITE(p, "  Out.v_color0 = In.color0;\n");
			} else {
				WRITE(p, "  Out.v_color0 = u_matambientalpha;\n");
			}
			if (lmode)
				WRITE(p, "  Out.v_color1 = float3(0, 0, 0);\n");
		}

		// Step 3: UV generation
		if (doTexture) {
			switch (uvGenMode) {
			case GE_TEXMAP_TEXTURE_COORDS:  // Scale-offset. Easy.
			case GE_TEXMAP_UNKNOWN: // Not sure what this is, but Riviera uses it.  Treating as coords works.
				if (prescale) {
					if (hasTexcoord) {
						WRITE(p, "  Out.v_texcoord = In.texcoord * u_uvscaleoffset.xy;\n");
					} else {
						WRITE(p, "  Out.v_texcoord = float2(0.0, 0.0);\n");
					}
				} else {
					if (hasTexcoord) {
						WRITE(p, "  Out.v_texcoord = In.texcoord * u_uvscaleoffset.xy + u_uvscaleoffset.zw;\n");
					} else {
						WRITE(p, "  Out.v_texcoord = u_uvscaleoffset.zw;\n");
					}
				}
				break;

			case GE_TEXMAP_TEXTURE_MATRIX:  // Projection mapping.
				{
					std::string temp_tc;
					switch (uvProjMode) {
					case GE_PROJMAP_POSITION:  // Use model space XYZ as source
						temp_tc = "float4(In.position.xyz, 1.0)";
						break;
					case GE_PROJMAP_UV:  // Use unscaled UV as source
						{
							if (hasTexcoord) {
								temp_tc = StringFromFormat("float4(In.texcoord.xy, 0.0, 1.0)");
							} else {
								temp_tc = "float4(0.0, 0.0, 0.0, 1.0)";
							}
						}
						break;
					case GE_PROJMAP_NORMALIZED_NORMAL:  // Use normalized transformed normal as source
						if (hasNormal)
							temp_tc = flipNormal ? "float4(normalize(-In.normal), 1.0)" : "float4(normalize(In.normal), 1.0)";
						else
							temp_tc = "float4(0.0, 0.0, 1.0, 1.0)";
						break;
					case GE_PROJMAP_NORMAL:  // Use non-normalized transformed normal as source
						if (hasNormal)
							temp_tc =  flipNormal ? "float4(-In.normal, 1.0)" : "float4(In.normal, 1.0)";
						else
							temp_tc = "float4(0.0, 0.0, 1.0, 1.0)";
						break;
					}
					// Transform by texture matrix. XYZ as we are doing projection mapping.
					WRITE(p, "  Out.v_texcoord.xyz = mul(%s,u_texmtx) * float3(u_uvscaleoffset.xy, 1.0);\n", temp_tc.c_str());
				}
				break;

			case GE_TEXMAP_ENVIRONMENT_MAP:  // Shade mapping - use dots from light sources.
				WRITE(p, "  Out.v_texcoord.xy = u_uvscaleoffset.xy * float2(1.0 + dot(normalize(u_lightpos%i), worldnormal), 1.0 + dot(normalize(u_lightpos%i), worldnormal)) * 0.5;\n", ls0, ls1);
				break;

			default:
				// ILLEGAL
				break;
			}
		}

		// Compute fogdepth
		if (enableFog)
			WRITE(p, "  Out.v_fogdepth.x = (viewPos.z + u_fogcoef.x) * u_fogcoef.y;\n");
	}

	WRITE(p, "  return Out;\n");
	WRITE(p, "}\n");
}

};
