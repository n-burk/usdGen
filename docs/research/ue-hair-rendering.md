# Unreal Engine 5 strand hair (HairStrands / Groom) — implementation reference

Target: bringing a custom USD/Hydra hair viewport renderer to visual parity with UE5's Groom "Strands" look.

## 0. Provenance and how to read this

Almost everything below is quoted **verbatim from UE source**, pulled from a public third‑party mirror of Epic's repo, `chenyong2github/UnrealEngine`, branch `5.3` (UE 5.3) with a few 4.26 comparisons from branch `master`. Raw URL base:

```
https://raw.githubusercontent.com/chenyong2github/UnrealEngine/5.3/Engine/...
https://raw.githubusercontent.com/chenyong2github/UnrealEngine/master/Engine/...   (UE 4.26.2)
```

Epic's own repo (`https://github.com/EpicGames/UnrealEngine`) requires an Epic-linked GitHub account; the mirror is the reachable copy. **No public mirror of 5.4–5.7 was found**, so treat the code as "UE 5.3 exact". The BSDF itself has been essentially frozen since 4.22 — the 4.26 → 5.3 diff of `HairBsdf.ush` is only NaN-guards (`abs()` wrappers) and a variable rename. Deltas in 5.4+ that I could *not* verify are flagged.

Secondary sources (marked inline): Brian Karis, *Physically Based Hair Shading in Unreal*, SIGGRAPH 2016 PBS course — https://blog.selfshadow.com/publications/s2016-shading-course/karis/s2016_pbs_epic_hair.pdf (the PDF's slides are images; the **speaker notes are text** and are quoted below); Epic docs on dev.epicgames.com; Chinese pipeline breakdowns.

File map (all under `Engine/Shaders/Private/`):

| Concern | File |
|---|---|
| BSDF | `HairBsdf.ush`, `HairShadingCommon.ush`, `ShadingModels.ush` (`HairBxDF`) |
| Geometry expansion | `HairStrands/HairStrandsVertexFactory.ush`, `HairStrandsVertexFactoryCommon.ush`, `HairStrandsPack.ush` |
| Visibility | `HairStrands/HairStrandsVisibility{VS,PS,Common.ush,CommonStruct.ush,Compaction,RasterCompute,ComposeSubPixelPS,Utils.ush}` |
| Deep shadow | `HairStrands/HairStrandsDeepShadow{VS,PS,Common.ush,Mask}` |
| Voxels | `HairStrands/HairStrandsVoxelPage{Common.ush,Traversal.ush}`, `HairStrandsVoxelRasterCompute.usf` |
| Transmittance / dual scattering | `HairStrands/HairStrandsDeepTransmittance{Common.ush,DualScattering.ush,Mask.usf}`, `HairStrands/HairStrandsCommon.ush`, `HairStrands/HairStrandsLUT.usf` |
| Environment | `HairStrands/HairStrandsEnvironmentLighting.usf`, `...Common.ush`, `HairStrandsEnvironmentAO.usf` |
| Attributes | `HairStrands/HairStrandsAttributeCommon.ush`, `HairStrandsAttributeTemplate.ush` |
| Path tracer | `PathTracing/Material/PathTracingHair.ush` |

C++ side: `Engine/Source/Runtime/Renderer/Private/HairStrands/*.cpp` (renderer), `Engine/Plugins/Runtime/HairStrands/Source/HairStrandsCore/*` (asset/component).

---

## 1. The shading model

### 1.1 The idea (Karis 2016)

Marschner's factorization: `S = Σ_p M_p(θ) · N_p(φ) / cos²θ_d`, with p = R (0), TT (1), TRT (2). M is *longitudinal* (scattering down the length of the fiber), N is *azimuthal* (radially around it). From the speaker notes of the SIGGRAPH deck:

> "The R path is white due to not passing through the interior of the fiber. The TRT is colored due to passing through the fiber twice. They are separated from one another due to the tilted cuticle scales."

> "One of the improvements that Weta's model had over Marschner's is an energy conserving longitudinal scattering function. We use a Gaussian instead… 2 symbols that are good to explain here are beta and alpha. Beta is based on the roughness of the fiber. Alpha is based on the tilt of the cuticle scales."

> On TRT: "Marschner solved this and glints with root finding for h which has multiple solutions which means multiple lobes. Weta solved it with heavy numerical integration. Pixar solved it with very large look up tables. **I chose a single simple lobe.** h, the offset of the path from the fiber center, for the purposes of Fresnel I use a constant. In the absorption term all use of h or the modified index of refraction are gone, replaced by a constant. The distribution I again started with the logistic function and approximated it with a Gaussian in terms of cos phi."

> On the `s_TT` width: "The formula for sTT was too expensive for us for too little of an improvement. Instead of using Pixar's solution for s we pick the constant **0.35**. From there we approximate the logistic with a Gaussian in terms of cos phi to avoid inverse trig."

> On the modified IOR: "If we fix the original index of refraction at **1.55** which is the IOR for human hair, this is a very accurate approximation… in terms of cos theta d the function looked very much like 1/x. Adding a linear term made the fit much better."

> On the multiple-scattering hack: "It feels wrong to present this part in a physically based shading course but instead consider this a call to arms… We used ideas from the Agni's Philosophy demo. We don't author a normal and use this fake normal instead… The rest of the scattering approximation is a wrapped Lambert, and an absorption based on the direct light path length through the hair volume. That path length is derived from the exponential shadow value. **This is all a giant artistic hack and not physically based in the slightest.** It was derived by looking at photos, not ground truth renders."

> On shadows: "As mentioned already I rely on shadows for the shading model. **It is absolutely vital to the look** which is easy to see by disabling them. Shadow maps are PCF filtered with an exponential falloff instead of a hard comparison. The result is volumetric looking without any provided normals."

> On IBL / area lights: "Increase the roughness as an approximation of a wider area light source. We don't have shadowing from shadow maps so we need to artificially shadow paths that would likely be blocked by a volume of hair. These are primarily those that are coming from the opposite side."

### 1.2 `HairShading` — the production BSDF, verbatim

`Engine/Shaders/Private/HairBsdf.ush` (UE 5.3). **This is the single most important artifact for parity.**

```hlsl
float Hair_g(float B, float Theta)
{
	return exp(-0.5 * Pow2(Theta) / (B * B)) / (sqrt(2 * PI) * B);
}

float Hair_F(float CosTheta)
{
	const float n = 1.55;
	const float F0 = Pow2((1 - n) / (1 + n));
	return F0 + (1 - F0) * Pow5(1 - CosTheta);
}
```

`F0 = ((1-1.55)/(1+1.55))^2 = 0.0465...` — Schlick with hair's IOR.

```hlsl
float3 HairShading( FGBufferData GBuffer, float3 L, float3 V, half3 N, float Shadow,
                    FHairTransmittanceData HairTransmittance, float InBacklit, float Area, uint2 Random )
{
	// to prevent NaN with decals
	float ClampedRoughness = clamp(GBuffer.Roughness, 1/255.0f, 1.0f);

	const float Backlit	= min(InBacklit, HairTransmittance.bUseBacklit ? GBuffer.CustomData.z : 1);

	// N is the vector parallel to hair pointing toward root

	const float VoL       = dot(V,L);
	const float SinThetaL = clamp(dot(N,L), -1.f, 1.f);
	const float SinThetaV = clamp(dot(N,V), -1.f, 1.f);
	float CosThetaD = cos( 0.5 * abs( asinFast( SinThetaV ) - asinFast( SinThetaL ) ) );

	const float3 Lp = L - SinThetaL * N;
	const float3 Vp = V - SinThetaV * N;
	const float CosPhi = dot(Lp,Vp) * rsqrt( dot(Lp,Lp) * dot(Vp,Vp) + 1e-4 );
	const float CosHalfPhi = sqrt( saturate( 0.5 + 0.5 * CosPhi ) );

	float n = 1.55;
	//float n_prime = sqrt( n*n - 1 + Pow2( CosThetaD ) ) / CosThetaD;
	float n_prime = 1.19 / CosThetaD + 0.36 * CosThetaD;

	float Shift = 0.035;
	float Alpha[] =
	{
		-Shift * 2,
		Shift,
		Shift * 4,
	};
	float B[] =
	{
		Area + Pow2(ClampedRoughness),
		Area + Pow2(ClampedRoughness) / 2,
		Area + Pow2(ClampedRoughness) * 2,
	};

	float3 S = 0;
	if (HairTransmittance.ScatteringComponent & HAIR_COMPONENT_R)
	{
		const float sa = sin(Alpha[0]);
		const float ca = cos(Alpha[0]);
		float ShiftR = 2 * sa * (ca * CosHalfPhi * sqrt(1 - SinThetaV * SinThetaV) + sa * SinThetaV);
		float BScale = HairTransmittance.bUseSeparableR ? sqrt(2.0) * CosHalfPhi : 1;
		float Mp = Hair_g(B[0] * BScale, SinThetaL + SinThetaV - ShiftR);
		float Np = 0.25 * CosHalfPhi;
		float Fp = Hair_F(sqrt(saturate(0.5 + 0.5 * VoL)));
		S += Mp * Np * Fp * (GBuffer.Specular * 2) * lerp(1, Backlit, saturate(-VoL));
	}

	// TT
	if (HairTransmittance.ScatteringComponent & HAIR_COMPONENT_TT)
	{
		float Mp = Hair_g( B[1], SinThetaL + SinThetaV - Alpha[1] );

		float a = 1 / n_prime;
		//float h = CosHalfPhi * rsqrt( 1 + a*a - 2*a * sqrt( 0.5 - 0.5 * CosPhi ) );
		//float h = CosHalfPhi * ( ( 1 - Pow2( CosHalfPhi ) ) * a + 1 );
		float h = CosHalfPhi * ( 1 + a * ( 0.6 - 0.8 * CosPhi ) );

		float f = Hair_F( CosThetaD * sqrt( saturate( 1 - h*h ) ) );
		float Fp = Pow2(1 - f);
		float3 Tp = 0;
		if (HairTransmittance.bUseLegacyAbsorption)
		{
			Tp = pow(abs(GBuffer.BaseColor), 0.5 * sqrt(1 - Pow2(h * a)) / CosThetaD);
		}
		else
		{
			// Compute absorption color which would match user intent after multiple scattering
			const float3 AbsorptionColor = HairColorToAbsorption(GBuffer.BaseColor);
			Tp = exp(-AbsorptionColor * 2 * abs(1 - Pow2(h * a) / CosThetaD));
		}

		//float s = 0.35;
		//float Np = exp( (Phi - PI) / s ) / ( s * Pow2( 1 + exp( (Phi - PI) / s ) ) );
		//float Np = 0.71 * exp( -1.65 * Pow2(Phi - PI) );
		float Np = exp( -3.65 * CosPhi - 3.98 );

		S += Mp * Np * Fp * Tp * Backlit;
	}

	// TRT
	if (HairTransmittance.ScatteringComponent & HAIR_COMPONENT_TRT)
	{
		float Mp = Hair_g( B[2], SinThetaL + SinThetaV - Alpha[2] );

		//float h = 0.75;
		float f = Hair_F( CosThetaD * 0.5 );
		float Fp = Pow2(1 - f) * f;
		//float3 Tp = pow( GBuffer.BaseColor, 1.6 / CosThetaD );
		float3 Tp = pow(abs(GBuffer.BaseColor), 0.8 / CosThetaD );

		//float s = 0.15;
		//float Np = 0.75 * exp( Phi / s ) / ( s * Pow2( 1 + exp( Phi / s ) ) );
		float Np = exp( 17 * CosPhi - 16.78 );

		S += Mp * Np * Fp * Tp;
	}

	if (HairTransmittance.ScatteringComponent & HAIR_COMPONENT_MULTISCATTER)
	{
		S  = EvaluateHairMultipleScattering(HairTransmittance, ClampedRoughness, S);
		S += KajiyaKayDiffuseAttenuation(GBuffer, L, V, N, Shadow);
	}

	S = -min(-S, 0.0);   // clamp negatives, NaN-safe
	return S;
}
```

Key facts to reproduce exactly:

* **`N` is not a normal.** It is the **hair tangent, pointing toward the root**. `SinThetaL = dot(N,L)`, `SinThetaV = dot(N,V)`. The Gaussian argument is `SinThetaL + SinThetaV - Alpha[p]`, i.e. the model works in *sin θ* space, not θ — a deliberate cheapness (Karis' `M_p` is a Gaussian in `sinθ_i + sinθ_o − α`, not in `θ_h − α`).
* **Cuticle shift `Shift = 0.035` rad** (≈2.0°), lobe shifts `α = {−0.07, +0.035, +0.14}` rad = `{−2°, +2°, +8°}`.
* **Longitudinal widths** `B = {β², β²/2, β²·2}` where `β = Roughness` (clamped to `[1/255, 1]`), i.e. **R uses `Roughness²`, TT is half, TRT is double**. `Area` is added, not multiplied (see §5).
* **R's azimuthal term is `0.25·cos(φ/2)`** with the Weta azimuthal-dependent longitudinal widening `B_R *= sqrt(2)·cos(φ/2)` when `bUseSeparableR` (default **true**; strands path sets it true as well). Plus the off-axis shift `ShiftR`.
* **R's Fresnel uses the half-angle-ish term** `F(sqrt(0.5 + 0.5·V·L))` — this is `F(cos(½ acos(V·L)))`.
* **`Specular` scales R only, as `Specular * 2`.** Material default Specular = 0.5 ⇒ multiplier 1.0.
* **TT azimuthal is a Gaussian in `cosφ`:** `N_TT = exp(−3.65·cosφ − 3.98)`. At φ=π (straight through) `cosφ=−1` ⇒ `exp(−0.33) = 0.719`; at φ=0 ⇒ `exp(−7.63) = 4.8e−4`.
* **TRT azimuthal:** `N_TRT = exp(17·cosφ − 16.78)`. At φ=0 ⇒ `exp(0.22)=1.246`; falls off extremely fast — this is the tight secondary highlight.
* **TRT is brutally simplified**: `h` gone, `F(cosθ_d · 0.5)` (a constant `h≈0.866`), absorption `Tp = BaseColor^(0.8/cosθ_d)`.
* **TT `h` approximation:** `h = cos(φ/2)·(1 + a·(0.6 − 0.8·cosφ))` with `a = 1/n'`.
* **Modified IOR approximation:** `n' ≈ 1.19/cosθ_d + 0.36·cosθ_d` (replaces `sqrt(n²−1+cos²θ_d)/cosθ_d`).
* **Two TT absorption models.** `bUseLegacyAbsorption` (default `true` for deferred/cards; **`false` for strands**, see `InitHairStrandsTransmittanceData`) picks `pow(BaseColor, 0.5·sqrt(1−(h·a)²)/cosθ_d)` vs the Chiang-style `exp(−σ_a · 2·|1 − (h·a)²/cosθ_d|)` where `σ_a = HairColorToAbsorption(BaseColor)`. The legacy one is the SIGGRAPH-2016 formula; the new one is "absorption color which would match user intent after multiple scattering". Selected by the `HAIR_COMPONENT_TT_MODEL` bit, driven by `r.HairStrands.Components.TTModel` (default **0** ⇒ legacy).
* `S = -min(-S, 0.0)` is a NaN-and-negative clamp; do the same or you get fireflies.

`Pow2/Pow5/Square` are `x*x`, `x^5`, `x*x`. `asinFast` is UE's polynomial asin.

### 1.3 The "Scatter" fake multiple-scattering term

```hlsl
float3 KajiyaKayDiffuseAttenuation(FGBufferData GBuffer, float3 L, float3 V, half3 N, float Shadow)
{
	// Use soft Kajiya Kay diffuse attenuation
	float KajiyaDiffuse = 1 - abs(dot(N, L));

	float3 FakeNormal = normalize(V - N * dot(V, N));
	//N = normalize( DiffuseN + FakeNormal * 2 );
	N = FakeNormal;

	// Hack approximation for multiple scattering.
	float MinValue = 0.0001f;
	float Wrap = 1;
	float NoL = saturate((dot(N, L) + Wrap) / Square(1 + Wrap));
	float DiffuseScatter = (1 / PI) * lerp(NoL, KajiyaDiffuse, 0.33) * GBuffer.Metallic;
	float Luma = Luminance(GBuffer.BaseColor);
	float3 BaseOverLuma = abs(GBuffer.BaseColor / max(Luma, MinValue));
	float3 ScatterTint = Shadow < 1 ? pow(BaseOverLuma, 1 - Shadow) : 1;
	return sqrt(abs(GBuffer.BaseColor)) * DiffuseScatter * ScatterTint;
}

float3 EvaluateHairMultipleScattering(const FHairTransmittanceData TransmittanceData, const float Roughness, const float3 Fs)
{
	return TransmittanceData.GlobalScattering * (Fs + TransmittanceData.LocalScattering) * TransmittanceData.OpaqueVisibility;
}
```

* **`GBuffer.Metallic` IS the material's `Scatter` input.** Confirmed in `MaterialAttributeDefinitionMap.cpp`:
  ```cpp
  case MP_Metallic:
      CustomPinNames.Add({ MSM_Hair, "Scatter" });
  case MP_Normal:
      CustomPinNames.Add({ MSM_Hair, "Tangent" });
  case MP_CustomData0:
      CustomPinNames.Add({ MSM_Hair, "Backlit" });
  ```
* `FakeNormal = normalize(V − N(V·N))` — the vector perpendicular to the tangent in the view plane. This is Karis' "fake normal" from Agni's Philosophy.
* Wrapped Lambert with `Wrap = 1` ⇒ `NoL = saturate((N·L + 1)/4)`.
* `ScatterTint = (BaseColor/luma)^(1−Shadow)` — the deeper into shadow, the more saturated, emulating longer absorption paths. `Shadow` here is `FShadowTerms.TransmissionShadow` (the "exponential shadow value" Karis mentions).
* The final colour is `sqrt(BaseColor)` — a gamma-ish square root, not linear.
* **Important**: on the **strands** path this term is dead. `HairSampleToGBufferData` (`HairStrandsVisibilityUtils.ush`) sets `Out.Metallic = 0; // Scattering;`, so `DiffuseScatter == 0`. Strands get real dual scattering (§4.4) instead. The Kajiya term is live for **cards/meshes and any mesh using the Hair shading model** in the deferred path.

### 1.4 `FHairTransmittanceData` and component flags

`HairShadingCommon.ush`:

```hlsl
#define HAIR_COMPONENT_R			0x1u
#define HAIR_COMPONENT_TT			0x2u
#define HAIR_COMPONENT_TRT			0x4u
#define HAIR_COMPONENT_LS			0x8u
#define HAIR_COMPONENT_GS			0x10u
#define HAIR_COMPONENT_MULTISCATTER	0x20u
#define HAIR_COMPONENT_TT_MODEL  	0x40u

struct FHairTransmittanceData
{
	bool bUseLegacyAbsorption;
	bool bUseSeparableR;
	bool bUseBacklit;

	float  OpaqueVisibility;
	float3 LocalScattering;
	float3 GlobalScattering;

	uint ScatteringComponent;
};

FHairTransmittanceData InitHairTransmittanceData(bool bMultipleScatterEnable = true)
{
	FHairTransmittanceData o;
	o.bUseLegacyAbsorption = true;
	o.bUseSeparableR = true;
	o.bUseBacklit = false;
	o.OpaqueVisibility = 1;
	o.LocalScattering = 0;
	o.GlobalScattering = 1;
	o.ScatteringComponent = HAIR_COMPONENT_R | HAIR_COMPONENT_TT | HAIR_COMPONENT_TRT
	                      | (bMultipleScatterEnable ? HAIR_COMPONENT_MULTISCATTER : 0);
	return o;
}

FHairTransmittanceData InitHairStrandsTransmittanceData(bool bMultipleScatterEnable = false)
{
	FHairTransmittanceData o = InitHairTransmittanceData(bMultipleScatterEnable);
	o.bUseLegacyAbsorption = false;
	o.bUseBacklit = true;
	return o;
}
```

Component toggles (all default 1 except TTModel) live in `HairStrandsUtils.cpp`: `r.HairStrands.Components.{R,TT,TRT,LocalScattering,GlobalScattering,TTModel}`.

### 1.5 Absorption / melanin helpers (`HairShadingCommon.ush`)

These are the Chiang 2016 mapping, and they are what a "hair colour" material function uses.

```hlsl
// Reference: A Practical and Controllable Hair and Fur Model for Production Path Tracing.
float3 HairAbsorptionToColor(float3 A, float B=0.3f)
{
	const float b2 = B * B;  const float b3 = B * b2;
	const float b4 = b2 * b2; const float b5 = B * b4;
	const float D = (5.969f - 0.215f * B + 2.532f * b2 - 10.73f * b3 + 5.574f * b4 + 0.245f * b5);
	return exp(-sqrt(A) * D);
}

float3 HairColorToAbsorption(float3 C, float B = 0.3f)
{
	... same D ...
	return Pow2(log(C) / D);
}

// Reference: An Energy-Conserving Hair Reflectance Model  (adapted for [0..1] range)
float3 GetHairColorFromMelanin(float InMelanin, float InRedness, float3 InDyeColor)
{
	InMelanin = saturate(InMelanin);
	InRedness = saturate(InRedness);
	const float Melanin		= -log(max(1 - InMelanin, 0.0001f));
	const float Eumelanin 	= Melanin * (1 - InRedness);
	const float Pheomelanin = Melanin * InRedness;

	const float3 DyeAbsorption = HairColorToAbsorption(saturate(InDyeColor));
	const float3 Absorption = Eumelanin * float3(0.506f, 0.841f, 1.653f)
	                        + Pheomelanin * float3(0.343f, 0.733f, 1.924f);

	return HairAbsorptionToColor(Absorption + DyeAbsorption);
}
```

The `B = 0.3` default is the azimuthal roughness assumed by the fit; `D(0.3) = 5.969 − 0.0645 + 0.2279 − 0.2897 + 0.0452 + 0.0006 = 5.888`.

Melanin absorption coefficients are d'Eon 2011's: eumelanin `(0.506, 0.841, 1.653)`, pheomelanin `(0.343, 0.733, 1.924)`.

### 1.6 Hair material inputs, pin names and defaults

Shading model `SHADINGMODELID_HAIR`. Pins (from `MaterialAttributeDefinitionMap.cpp`, with the engine-wide defaults registered in the same file):

| UE pin (hair label) | Underlying property | Default | Used as |
|---|---|---|---|
| Base Color | `MP_BaseColor` | `(0,0,0)` | absorption/tint in TT & TRT, tint in Kajiya term |
| **Scatter** | `MP_Metallic` | `0` | `GBuffer.Metallic`, the fake-MS strength (deferred/cards only) |
| Specular | `MP_Specular` | `0.5` | `Specular * 2` multiplies **R only** |
| Roughness | `MP_Roughness` | `0.5` | `β`; `B = {β², β²/2, 2β²}` |
| **Tangent** | `MP_Normal` | — | `N` in the BSDF, i.e. the fibre direction toward the root |
| **Backlit** | `MP_CustomData0` | `1` | scales TT fully; scales R by `lerp(1, Backlit, saturate(-VoL))` |
| Emissive Color | `MP_EmissiveColor` | `(0,0,0)` | added at composition, never packed into the sample |
| Ambient Occlusion | `MP_AmbientOcclusion` | `1` | per-point AO attribute path |
| World Position Offset, Pixel Depth Offset | — | — | standard |

`FShadowTerms` → `HairBxDF` in `ShadingModels.ush`:

```hlsl
FDirectLighting HairBxDF(FGBufferData GBuffer, half3 N, half3 V, half3 L, float Falloff, half NoL, FAreaLight AreaLight, FShadowTerms Shadow)
{
	const float3 BsdfValue = HairShading(GBuffer, L, V, N, Shadow.TransmissionShadow, Shadow.HairTransmittance, 1, 0, uint2(0, 0));

	FDirectLighting Lighting;
	Lighting.Diffuse = 0;
	Lighting.Specular = 0;
	Lighting.Transmission = AreaLight.FalloffColor * Falloff * BsdfValue;
	return Lighting;
}
```

Note: **`InBacklit = 1` and `Area = 0` for punctual/area lights.** All hair energy lands in `Lighting.Transmission`, and there is **no `NoL` cosine factor** — the BSDF already integrates over the fibre cross-section.

### 1.7 The reference (ground-truth) implementation

`HairBsdf.ush` ships the exact model behind `#define HAIR_REFERENCE 0`, useful as a validation target. It contains the d'Eon 2011 energy-conserving `M_p`:

```hlsl
float LongitudinalScattering(float B, float SinThetaL, float SinThetaV)
{
	float v = B * B;
	float CosThetaL2 = 1 - SinThetaL * SinThetaL;
	float CosThetaV2 = 1 - SinThetaV * SinThetaV;
	float Mp = 0;
	if (v < 0.1)
	{
		float a = sqrt(CosThetaL2 * CosThetaV2) / v;
		float b = -SinThetaL * SinThetaV / v;
		float logI0a = a > 12 ? a + 0.5 * (-log(2 * PI) + log(1 / a) + 0.125 / a) : log(I0(a));
		Mp = exp(logI0a + b - rcp(v) + 0.6931 + log(0.5 / v));
	}
	else
	{
		Mp = rcp(exp(2 / v) * v - v) * exp((1 - SinThetaL * SinThetaV) / v) * I0(sqrt(CosThetaL2 * CosThetaV2) / v);
	}
	return Mp;
}
```

plus `I0` (Abramowitz–Stegun rational fit), a wrapped `GaussianDetector` (`Σ_{k=-4..4} Hair_g(Bp, Phi − 2πk)`), Marschner's `Omega(p,h) = 2p·γ_t − 2γ_i + pπ`, and a 16-sample Monte-Carlo azimuthal integral. The reference `Attenuation()` shows the absorption UE approximates:

```hlsl
	float3 Sigma_ae = { 0.419, 0.697, 1.37 };
	float3 Sigma_ap = { 0.187, 0.4, 1.05 };
	float3 ua = -0.25 * log(Color);
	float3 ua_prime = ua / HairTemp.CosThetaT;
	float f = Hair_F(HairTemp.CosThetaD * sqrt(1 - h * h));		// (14)
	float3 T = exp(-2 * ua_prime * cos(yt));
	if (p == 1) A = Pow2(1 - f) * T;		// (13)
	else        A = Pow2(1 - f) * f * T * T;	// (13)
```

---

## 2. Geometry: control points → screen quads

### 2.1 Packed control point

`HairStrandsPack.ush`:

```hlsl
struct FHairControlPoint
{
	float3 Position;
	float  WorldRadius;
	float  UCoord;
	uint   Type;
};

FHairControlPoint UnpackHairControlPoint(
	uint4 InPackedData, float3 InVF_PositionOffset=0, float InVF_Radius=1,
	float InVF_RootScale=1, float InVF_TipScale=1)
{
	const uint PackedAlpha	= asuint(InPackedData.a);

	FHairControlPoint Out = (FHairControlPoint)0;
	Out.Position	= f16tof32(InPackedData.xyz) + InVF_PositionOffset;
	Out.UCoord		= ((PackedAlpha & 0xFF00) >> 8) / 255.f;
	Out.WorldRadius	= ((PackedAlpha & 0x00FC) >> 2) / 63.f;
	Out.Type		= PackedAlpha & 0x3;

	Out.WorldRadius *= InVF_Radius * lerp(InVF_RootScale, InVF_TipScale, Out.UCoord);

	return Out;
}
```

So one control point is **64 bits**: three fp16 positions (relative to a per-instance offset) + one packed uint: `[15:8]` U coordinate (8 bits, 0=root → 1=tip), `[7:2]` normalised radius (**only 6 bits**, 0..63/63), `[1:0]` type.

```hlsl
#define HAIR_CONTROLPOINT_INSIDE 0
#define HAIR_CONTROLPOINT_START	 1
#define HAIR_CONTROLPOINT_END	 2
```

**Radius model:** `WorldRadius = normalizedRadius · HairWidth/2-ish · lerp(RootScale, TipScale, U)`. `HairStrandsVF_Radius` is the per-instance `HairWidth` scalar (`FHairGroupDesc::HairWidth`, default **0.01 cm = 100 µm**), `RootScale`/`TipScale` default **1.0** each. The per-point normalised radius comes from the imported `groom_width` attribute (Alembic), normalised against the group's max radius at build time.

### 2.2 Quad expansion (view-aligned ribbons)

`HairStrandsVertexFactory.ush`. Three topologies exist:

```hlsl
FVertexInfo GetVertexInfo(FVertexFactoryInput Input)
{
	uint VertexId = Input.VertexId;
	FVertexInfo VertexInfo;
#if RAYHITGROUPSHADER
	uint BaseIndex    = VertexId / 4;
	VertexInfo.IsTip  = (Input.TriangleId % 2) == 0 ? Input.TriangleVertexId != 2 : Input.TriangleVertexId == 0;
#elif USE_HAIR_TRIANGLE_STRIP
	uint QuadIndex = VertexId % 2;
	uint BaseIndex	= VertexId / 2;
	VertexInfo.IsTip		= 0;
	VertexInfo.IsLeft		= QuadIndex == 0 ? 1 : 0;
#else
	uint QuadIndex	= VertexId % 6;
	uint BaseIndex	= VertexId / 6;
	VertexInfo.IsTip		= QuadIndex == 0 || QuadIndex == 2 || QuadIndex == 4 ? 0 : 1;
	VertexInfo.IsLeft		= QuadIndex == 0 || QuadIndex == 1 || QuadIndex == 5 ? 1 : 0;
#endif
	VertexInfo.HairControlPointId = BaseIndex;
	VertexInfo.VertexIndex = BaseIndex + VertexInfo.IsTip;
	...
}
```

**Default HW path: 6 vertices (2 triangles) per control point**, i.e. one quad per *segment*, addressed by `VertexId/6`, so the draw is `PointCount * 6` non-indexed vertices. There is also a triangle-strip path (2 verts per point).

Segment ends are cut with a degenerate quad rather than an index buffer:

```hlsl
	// Create a degenerated quad the end of each strand to cut between each strands
	const bool bIsInvalidQuad = (bInvalidJointVertex && Out.Type == HAIR_CONTROLPOINT_START && VertexInfo.IsTip == 1)
	                         || (bInvalidJointVertex && Out.Type == HAIR_CONTROLPOINT_END   && VertexInfo.IsTip == 0)
	                         || VertexInfo.bForceInvalidQuad;
	Out.Position = bIsInvalidQuad ? float3(INFINITE_FLOAT, INFINITE_FLOAT, INFINITE_FLOAT) : Out.Position;
```

The expansion itself:

```hlsl
float4 ComputeViewAlignedWorldPosition(FVertexFactoryInput Input, float3 WorldTangent, float4 WorldPosition, float WorldStrandRadius, FHairViewInfo HairViewInfo)
{
	FVertexInfo VertexInfo = GetVertexInfo(Input);

	// Minimal radius to snap the strand to a sample/pixel center (to avoid aliasing)
	const float DistanceToCamera = length(HairViewInfo.TranslatedWorldCameraOrigin - WorldPosition.xyz);
	const float MinStrandHairRadius = HairViewInfo.bIsOrthoView ? HairViewInfo.RadiusAtDepth1 : (DistanceToCamera * HairViewInfo.RadiusAtDepth1);
	const float3 ViewDir = -HairViewInfo.ViewForward;
	const float3 Right = normalize(cross(WorldTangent, ViewDir));
	const float3 OutWorldPosition = WorldPosition.xyz + (VertexInfo.IsLeft ? -Right : Right) * max(WorldStrandRadius, MinStrandHairRadius);

	return float4(OutWorldPosition, 1);
}
```

Notes for parity:
* `ViewDir = -ViewForward` — a **constant** per view, **not** per-vertex `cameraPos − P`. The ribbon is aligned to the view *plane*, not to the eye vector. This matters: it makes the ribbon width uniform across the screen and avoids twisting.
* Half-width used is `max(WorldRadius, MinStrandHairRadius)` — a hard `max`, not a lerp.
* `WorldTangent` comes from a precomputed tangent buffer (`HairStrandsTangent.usf`), not from the VS.

### 2.3 Stable rasterization / minimum pixel width

This is the *single most important* anti-aliasing mechanism, and the "UE look" of thin hair depends on it.

`HairStrandsUtils.cpp`:

```cpp
static float GStrandHairRasterizationScale = 0.5f;        // r.HairStrands.RasterizationScale
static float GStrandHairStableRasterizationScale = 1.0f;  // r.HairStrands.StableRasterizationScale
static float GStrandHairVelocityRasterizationScale = 1.5f;// r.HairStrands.VelocityRasterizationScale
static float GStrandHairShadowRasterizationScale = 1.0f;  // r.HairStrands.ShadowRasterizationScale

float SampleCountToSubPixelSize(uint32 SamplePerPixelCount)
{
	float Scale = 1;
	switch (SamplePerPixelCount)
	{
	case 1: Scale = 1.f; break;
	case 2: Scale = 8.f / 16.f; break;
	case 4: Scale = 8.f / 16.f; break;
	case 8: Scale = 4.f / 16.f; break;
	}
	return Scale;
}

FMinHairRadiusAtDepth1 ComputeMinStrandRadiusAtDepth1(
	const FIntPoint& Resolution, const float FOV, const uint32 SampleCount,
	const float OverrideStrandHairRasterizationScale)
{
	auto InternalMinRadiusAtDepth1 = [Resolution, FOV, SampleCount](float RasterizationScale)
	{
		const float DiameterToRadius = 0.5f;
		const float SubPixelScale = SampleCountToSubPixelSize(SampleCount);
		const float vFOV = FMath::DegreesToRadians(FOV);
		const float StrandDiameterAtDepth1 = FMath::Tan(vFOV * 0.5f) / (0.5f * Resolution.Y) * SubPixelScale;
		return DiameterToRadius * RasterizationScale * StrandDiameterAtDepth1;
	};

	FMinHairRadiusAtDepth1 Out;
	// Scales strand to covers a bit more than a pixel and insure at least one sample point is hit
	const float PrimaryRasterizationScale  = Override > 0 ? Override : GStrandHairRasterizationScale;
	const float VelocityRasterizationScale = Override > 0 ? Override : GStrandHairVelocityRasterizationScale;
	const float StableRasterizationScale   = FMath::Max(1.f, GStrandHairStableRasterizationScale);
	Out.Primary  = InternalMinRadiusAtDepth1(PrimaryRasterizationScale);
	Out.Velocity = InternalMinRadiusAtDepth1(VelocityRasterizationScale);
	Out.Stable   = InternalMinRadiusAtDepth1(StableRasterizationScale);
	return Out;
}
```

In closed form: **minimum world-space half-width at unit depth**

```
RadiusAtDepth1 = 0.5 * RasterizationScale * tan(vFOV/2) / (0.5 * ResolutionY) * SubPixelScale
MinWorldHalfWidth(P) = RadiusAtDepth1 * distance(camera, P)      (perspective)
                     = RadiusAtDepth1                            (ortho)
```

* Default **`RasterizationScale = 0.5`** with **8×MSAA** (`SubPixelScale = 4/16 = 0.25`) ⇒ the strand is snapped to **0.5 · 0.25 = 1/8 of a pixel diameter**, i.e. one MSAA sample.
* `bUseStableRasterization` (Groom "Use Stable Rasterization", default **false**) switches to `Stable` (scale ≥ 1.0), making thin hair a full pixel wide. Selected in-shader by:
  ```hlsl
  Info.RadiusAtDepth1Primary = bUseScableRasterization ? ViewHairRenderInfo.y : ViewHairRenderInfo.x;
  ```
* Source comment: *"For no AA without TAA, a good value is: 1.325f (Empirical)"*. If your renderer has no temporal AA, use ≈1.325 for `RasterizationScale`.
* Shadow/DOM views use their own `RadiusAtDepth1 = SphereRadius / min(ShadowRes.X, ShadowRes.Y)` scaled by the shadow/stable scale.
* Voxelization uses `max(WorldRadius, HairViewInfo.RadiusAtDepth1)` **without** the distance multiply.

### 2.4 Coverage compensation

Widening the geometry must be paid back in alpha, or thin hair goes opaque. `HairStrandsVisibilityPS.usf`:

```hlsl
	float Coverage = 1;
	{
		bool bUseStableRasterization = UseStableRasterization();
		FHairRenderInfo HairRenderInfo = GetHairRenderInfo(ResolvedView.HairRenderInfo, ResolvedView.HairRenderInfoBits, bUseStableRasterization);
		const float SceneDepth = ConvertFromDeviceZ(SvPosition.z); // Linear depth in world unit
		const float PixelRadius = HairRenderInfo.bIsOrthoView ? HairRenderInfo.RadiusAtDepth1Primary : SceneDepth * HairRenderInfo.RadiusAtDepth1Primary;
		const float StrandRealRadius = WorldStrandRadius;
		Coverage = saturate(StrandRealRadius / max(StrandRealRadius, PixelRadius) * HairVisibilityPass_HairCoverageScale);
	}
```

**`Coverage = saturate( r / max(r, PixelRadius) * CoverageScale )`** — i.e. the fraction of the widened footprint the real fibre actually occupies. The exact same expression is used in the material pass (`HairStrandsMaterialCommon.ush:186`) and, structurally, by the DOM and voxel passes. This is the whole anti-aliasing strategy: *widen geometry, attenuate alpha*.

`HairCoverageScale` is the LOD thickness compensation (§2.6).

Transmittance/hair-count render modes emit:
```hlsl
#if HAIR_RENDER_MODE == RENDER_MODE_TRANSMITTANCE
	OutColor0 = saturate(1.0f - Coverage);
#elif HAIR_RENDER_MODE == RENDER_MODE_TRANSMITTANCE_AND_HAIRCOUNT
	OutColor0 = saturate(1.0f - Coverage);
	OutColor1 = float2(Coverage, 1);      // accumulated radius-ratio, accumulated count
#endif
```
(`OutColor0` multiplicative-blended, `OutColor1` additive.)

### 2.5 Compute-rasterizer coverage

`HairStrandsVisibilityRasterCompute.usf` uses the analogous per-sample form:
```hlsl
	const uint HairCount = min(Rad, 0.5f) * 2.0f * 1000.0f * CoverageScale;
```
i.e. `coverage = min(radiusInPixels, 0.5) * 2`, fixed point ×1000.

### 2.6 LOD, decimation and thickness compensation

**Discrete LOD (UE4-style, still present).** `FHairLODSettings` (`GroomAssetInterpolation.h`) defaults:

```cpp
float CurveDecimation = 1;      // fraction of curves kept
float VertexDecimation = 1;     // fraction of points kept per curve
float AngularThreshold = 1.f;   // degrees, for curve simplification
float ScreenSize = 1;
float ThicknessScale = 1;       // <- radius compensation for this LOD
bool  bVisible = true;
EGroomGeometryType GeometryType = EGroomGeometryType::Strands;   // Strands | Cards | Meshes
EGroomBindingType  BindingType  = EGroomBindingType::Skinning;
```

Per-cluster, the GPU stores per-LOD vertex counts *and* radius scales, and a **fractional LOD interpolates both**:

```hlsl
FHairClusterLOD GetHairClusterLOD(FHairClusterInfo InClusterInfo, StructuredBuffer<FPackedHairClusterLODInfo> InClusterLODInfoBuffer, float LOD)
{
	const uint iLOD = clamp(floor(LOD), 0, InClusterInfo.LODCount-1);
	FHairClusterLODInfo ClusterLODInfo = UnpackHairClusterLODInfo(InClusterLODInfoBuffer[InClusterInfo.LODInfoOffset + iLOD]);
	const float S = LOD - iLOD;
	FHairClusterLOD Out;
	Out.VertexOffset = ClusterLODInfo.VertexOffset;
	Out.VertexCount  = lerp(ClusterLODInfo.VertexCount0, ClusterLODInfo.VertexCount1, S);
	Out.RadiusScale  = lerp(ClusterLODInfo.RadiusScale0, ClusterLODInfo.RadiusScale1, S);
	Out.LOD = LOD;
	return Out;
}
```
That `RadiusScale` lands in `CullingRadiusScaleBuffer` and is applied in `GetVertexPosition` as `HairStrandsVF.Radius * VertexInfo.LodRadiusScale` — **strands get thicker as they get fewer**. A `LodRadiusScale <= 0` forces a degenerate quad (culled curve).

**Auto LOD / continuous LOD (UE5).** `GroomManager.cpp`:

```cpp
static float ComputeActiveCurveCoverageScale(uint32 InAvailableCurveCount, uint32 InRestCurveCount)
{
	// Compensate lost in curve by a coverage scale increase
	const float CurveRatio = InAvailableCurveCount / float(InRestCurveCount);
	return 1.f/FMath::Max(CurveRatio, 0.01f);
}

static uint32 ComputeActiveCurveCount(float InScreenSize, uint32 InCurveCount, uint32 InClusterCount)
{
	const float Power = FMath::Max(0.1f, GHairStrands_AutoLOD_Scale);          // r.HairStrands.AutoLOD.Scale = 1
	const float ScreenSizeBias = FMath::Clamp(GHairStrands_AutoLOD_Bias, 0.f, 1.f); // r.HairStrands.AutoLOD.Bias = 0
	uint32 OutCurveCount = InCurveCount * FMath::Pow(FMath::Clamp(InScreenSize + ScreenSizeBias, 0.f, 1.0f), Power);
	// Ensure there is at least 1 curve per cluster
	OutCurveCount = FMath::Max(InClusterCount, OutCurveCount);
	return FMath::Clamp(OutCurveCount, 1, InCurveCount);
}
```

So: **curve count ∝ screenSize^AutoLODScale**, and the lost opacity is bought back by **scaling coverage by `1/curveRatio`** (clamped at ×100) rather than by thickening. `ContinuousLODCoverageScale` is what feeds `HairVisibilityPass_HairCoverageScale` / `MaterialPass_HairCoverageScale` above. `ScreenSize` here is `ComputeBoundsScreenSize` clamped to `[0,1]`.

Two different compensations, then: **discrete LOD thickens the strands; continuous LOD boosts the alpha.** If you only implement one, implement the alpha one — it preserves silhouette width.

Culled clusters are **not dropped**, they are pinned to the lowest LOD so their positions and AABBs stay valid for voxelization:
```hlsl
		// Always force clusters to be flagged as (forced) visible. If a cluster is not visible, its lowest LOD will be selected.
		bool bForceVisible = bIsHairGroupVisible == 1;
```

**Cards / meshes** are separate geometry types selected *per LOD entry* by screen size only (no decimation params). They go through `HairCardsVertexFactory.ush` and the ordinary deferred GBuffer path with `SHADINGMODELID_HAIR` — which is exactly why the Kajiya "Scatter" term is alive for cards and dead for strands.

### 2.7 Guide → render-strand interpolation

`HairStrandsPack.ush` / `HairStrandsInterpolation.usf`:

```hlsl
struct FGuideData
{
	uint3  VertexIndices;
	float3 VertexLerps;
	float3 VertexWeights;
};

FGuideData UnpackGuideData(uint In)      // single-guide mode: 24-bit index + 8-bit lerp
{
	Out.VertexWeights	= float3(1, 0, 0);
	Out.VertexIndices.x	= In & 0xFFFFFF;
	Out.VertexLerps.x	= UnpackR8(In >> 24);
	return Out;
}

FGuideData UnpackGuideData(uint4 In)     // 3-guide mode
{
	Out.VertexIndices.x = (In.x & 0xFFFF) | (((In.z      ) & 0xFF) << 16);
	Out.VertexIndices.y = (In.x >> 16   ) | (((In.z >> 8 ) & 0xFF) << 16);
	Out.VertexIndices.z = (In.y & 0xFFFF) | (((In.z >> 16) & 0xFF) << 16);
	Out.VertexWeights	= float3(UnpackR8(In.y>>16), UnpackR8(In.y>>24), 0);
	Out.VertexWeights.z = saturate(1.0f - (Out.VertexWeights.x + Out.VertexWeights.y));
	Out.VertexLerps.x	= UnpackR8(In.w);
	Out.VertexLerps.y	= UnpackR8(In.w >> 8);
	Out.VertexLerps.z	= UnpackR8(In.w >> 16);
	return Out;
}
```

Up to **3 guides per render point**, each a 24-bit guide-*vertex* index plus an 8-bit lerp along that guide segment; two 8-bit weights, third derived. Guide assignment metric weights at build time (`GroomBuilder.cpp`): `r.HairStrands.InterpolationMetric.Distance = 1`, `.Angle = 0`, `.Length = 0`, `.AngleAttenuation = 5`. Import defaults (`GroomAssetInterpolation.cpp`): `HairToGuideDensity = 0.1`, quality `High`, distance metric `Parametric`, `bRandomizeGuide = false`, `bUseUniqueGuide = false`, rigged guides 10 curves × 4 points.

---

## 3. Visibility and anti-aliasing

### 3.1 Render modes

`HairStrandsVisibilityCommon.ush` (5.3):

```hlsl
#define RENDER_MODE_TRANSMITTANCE 0
#define RENDER_MODE_PPLL 1
#define RENDER_MODE_MSAA_VISIBILITY 2
#define RENDER_MODE_TRANSMITTANCE_AND_HAIRCOUNT 3
#define RENDER_MODE_COMPUTE_RASTER 4
```

The three primary paths and their defaults (`HairStrandsVisibility.cpp`):

| Path | cvar | Default |
|---|---|---|
| MSAA visibility buffer | `r.HairStrands.Visibility.MSAA.SamplePerPixel` | **8** |
| PPLL (per-pixel linked list) | `r.HairStrands.Visibility.PPLL` | 0 (off); `PPLL.SamplePerPixel` **16** |
| Compute/software raster | `r.HairStrands.Visibility.ComputeRaster` | 0 in 5.3 (experimental); `Compute.SamplePerPixel` **1** |

Node-buffer sizing: `MSAA.MeanSamplePerPixel = 0.75`, `Compute.MeanSamplePerPixel = 0.75`, `PPLL.MeanSamplePerPixel = 1` (allocation = `W·H·SamplePerPixel·Mean`).

**MSAA path** is the shipping default: render strand quads into an MSAA `R32_UINT` visibility target containing only IDs, then compact.

```hlsl
uint PackHairVisControlPointMaterialId(uint ControlPointId, uint MaterialId)
{
	return (ControlPointId & 0x00FFFFFF) | ((MaterialId & 0xFF) << 24);
}
uint PackHairVisDepthCoverage(float InDepth, uint InCoverage8bits)
{
	return (uint(InDepth * 0x00FFFFFF) << 8) | min(0xFFu, InCoverage8bits);
}
```
⇒ **24-bit control-point id + 8-bit material id**, and for the compute/PPLL paths a second word of **24-bit depth + 8-bit coverage**.

Node addressing after compaction:
```hlsl
// * Max 67,108,864 total nodes ... * Max 63 nodes per pixel
// 26bits for offset | 6 bits for count (max 63 nodes)
uint EncodeNodeDesc(const FNodeDesc Desc) { return (Desc.Offset & 0x03FFFFFF) | ((Desc.Count & 0x3F) << 26); }
```

### 3.2 The shaded sample

`FHairSample` / `FPackedHairSample` — 5 dwords (20 bytes) per shaded fragment:

```hlsl
	Out.Tangent_Coverage8bit =
		((0xFF & uint(T.x * 255)))      |
		((0xFF & uint(T.y * 255)) << 8) |
		((0xFF & uint(T.z * 255)) << 16)|
		(min(uint(0xFF), In.Coverage8bit) << 24);

	// ControlPointId is on 28bits | MacroGroupId is on 4bits
	Out.ControlPointID_MacroGroupID = ((In.ControlPointId & 0x0FFFFFFF)) | ((In.MacroGroupId & 0xF) << 28);

	Out.Depth					= In.Depth;                                                  // 32bits float
	Out.BaseColor_Roughness		= float4ToUint(float4(sqrt(In.BaseColor), In.Roughness));     // 32bits uint
	Out.Specular_LightChannels_Backlit =
		clamp(uint(In.Specular * 0xFF), 0, 0xFF) |
		((In.LightChannelMask & 0x7) << 8)  |
		((In.bScatterSceneLighting ? 1 : 0) << 12) |
		(clamp(uint(In.Backlit * 0xFF), 0, 0xFF) << 16);
	// Emissive is not packed/unpacked
```

Note **BaseColor is stored gamma-2** (`sqrt` on write, squared on read) because it only gets 8 bits per channel. Tangent is octahedral-free, just 8:8:8 `N*0.5+0.5`. Emissive rides in a separate texture.

The GBuffer view of a sample, for feeding the BSDF:

```hlsl
FGBufferData HairSampleToGBufferData(in FHairSample In, in float InDualScatteringRoughnessOverride=0)
{
	FGBufferData Out = (FGBufferData)0;
	Out.ShadingModelID = SHADINGMODELID_HAIR;
	Out.WorldNormal = In.Tangent;
	Out.BaseColor = In.BaseColor;
	Out.Roughness = In.Roughness;
	Out.Specular = In.Specular;
	Out.Metallic = 0; // Scattering;
	Out.Depth = ConvertFromDeviceZ(In.Depth);
	Out.GBufferAO = 1;
	Out.CustomData = float4(InDualScatteringRoughnessOverride, 0, In.Backlit, 0);
	Out.IndirectIrradiance = 1000000;
	...
}
```

`CustomData.x` = dual-scattering roughness override (`r.HairStrands.DualScatteringRoughness`, default 0 = off), `.z` = Backlit.

### 3.3 Compaction, coverage and transmittance

`HairStrandsVisibilityCompaction.usf`:

* **MSAA path**: gather the (up to 8) samples, deduplicate by control-point id, and set each node's `Coverage8bit = To8bitCoverage(count_i / ValidSampleCount)` — a *normalised weight*, summing to 1 across the pixel's nodes. Then the pixel's overall coverage comes from a separate full-screen transmittance render:
  ```hlsl
  PixelCoverage = TransmittanceToCoverage(ViewTransmittanceTexture.Load(uint3(PixelCoord, 0)), CoverageThreshold);
  ```
* **PPLL path**: sort by depth, then compute ordered transmittance explicitly:
  ```hlsl
  SortedCoverage[i] = TotalSortedTransmittance * Coverage;
  TotalSortedTransmittance *= 1.0f - Coverage;
  ValidPixelSampleTotalCoverage += SortedCoverage[i];
  ...
  const float PatchedCoverage8bit = To8bitCoverage(SortedCoverage[j] / float(ValidPixelSampleTotalCoverage));
  ...
  PixelCoverage = TransmittanceToCoverage(TotalTransmittance, CoverageThreshold);
  ```
* The conversion itself:
  ```hlsl
  float TransmittanceToCoverage(float InTransmittance, float InCoverageThreashold)
  {
      return saturate(min(1 - InTransmittance, 1) / InCoverageThreashold);
  }
  ```
  with `r.HairStrands.Visibility.FullCoverageThreshold = 0.98` — pixels ≥98% covered snap to fully opaque. **This little threshold matters a lot**: without it a mass of hair never reaches alpha 1 and you see the background through it.

The per-pixel "categorization" texture (UE's hair coverage texture):

```hlsl
struct FCategorizationData { uint TotalCoverage8bit; uint SampleCount; float PixelCoverage; float ClosestDepth; };
uint4 EncodeCategorizationData(FCategorizationData Data)
{
	const uint ComponentX = min(Data.TotalCoverage8bit, uint(0xFF)) | ((Data.SampleCount & 0xFF)<<8);
	return uint4(ComponentX, f32tof16(Data.PixelCoverage), ClosestDepthLow, ClosestDepthHigh);
}
```

There is also a hair-count → coverage transfer LUT path (`HairStrandsCoverage.usf`, `r.HairStrands.Visibility.UseCoverageMappping`, default **0**):
```hlsl
	const float RemapScale = 2; // Remap the radius from [0..0.5] -> [0..1], i.e. the mapping used by the LUT
	const float HairCount = Raw.y;
	const float HairRadius = saturate(Raw.x / HairCount * RemapScale);
	const float Coverage = HairCoverageLUT.SampleLevel(LinearSampler, float2(HairRadius, (HairCount+0.5f)/LUT_HairCount), 0);
	OutputTexture[PixelCoord] = saturate(1-Coverage);
```

### 3.4 Deferred material and lighting passes

The compacted node list is shaded as a **square 2D grid**, not as screen pixels:

```hlsl
	OutNodeCount = HairVisibilityNodeCount.Load(uint3(0, 0, 0));
	OutResolution.x = ceil(sqrt(OutNodeCount));
	OutResolution.y = OutResolution.x;
```
and read back with `Coord = uint2(LocalOffset % Resolution, LocalOffset / Resolution)`. Lighting sample format: `r.HairStrands.LightSampleFormat = 1` ⇒ `PF_FloatR11G11B10` (0 ⇒ `PF_FloatRGBA`).

Material compaction thresholds: `r.HairStrands.MaterialCompaction.DepthThreshold = 1.0` cm, `.TangentThreshold = 10` degrees — adjacent samples within 1 cm and 10° are merged before shading.

### 3.5 Composition into scene colour

`HairStrandsVisibilityComposeSubPixelPS.usf`:

```hlsl
	const FCategorizationData CatData = DecodeCategorizationData(HairCategorizationTexture.Load(PixelCoord));
	const float PixelCoverage = min(CatData.PixelCoverage, 1);
	if (PixelCoverage == 0) discard;

	const float3 ClosetPointWorldPosition = SvPositionToWorld(float4(Input.Position.xy, CatData.ClosestDepth, 1.0));
	const float4 Volumetric = EvaluateVolumetric(ClosetPointWorldPosition);

	float3 LocalAccColor = 0;
	for (uint SampleIt = 0; SampleIt < NodeDesc.Count; SampleIt++) { ... LocalAccColor += LightingSample.rgb + EmissiveSample.rgb; }
	OutColor.rgb = LocalAccColor * Volumetric.a + Volumetric.rgb;
	OutColor.rgb *= PixelCoverage;
	OutColor.a = PixelCoverage;
	OutDepth = CatData.ClosestDepth;
```

So: sum the per-node lit colours (already coverage-weighted by the normalised `Coverage8bit`), apply fog/aerial perspective **once, at the closest sample's depth**, then alpha-blend over scene colour with `PixelCoverage`. Depth written is the **closest** sample's. `r.HairStrands.ComposeAfterTranslucency = 1` (0: before translucents, 1: after translucent but before separate-translucent, 2: after all, 3: …).

DOF depth is a coverage-weighted blend (`r.HairStrands.DOFDepth = 1`):
```hlsl
	const float OutDeviceZ = lerp(SceneDeviceZ, HairDeviceZ, PixelCoverage);
```

`r.HairStrands.WriteGBufferData = 1` writes hair tangent/shading-model into GBuffer A/B so SSAO/SSR/decals see something coherent.

### 3.6 TAA / TSR interaction

* Velocity filtering per pixel, `r.HairStrands.VelocityType = 1` (closest).
* `r.HairStrands.VelocityRasterizationScale = 1.5` — hair is rasterised **thicker under motion** ("Tuned based on heavy motion example (e.g., head shaking)"), so the velocity buffer doesn't get holes.
* A "fast resolve" stencil pass marks fast-moving hair so TAA rejects history:
  ```hlsl
  bool NeedFastResolve(float2 InEncodedVelocity, float InVelocityThreshold)
  {
      const float2 Velocity = DecodeVelocityFromTexture(float4(InEncodedVelocity, 0.0, 0.0)).xy;
      const float VelocityMagnitude = sqrt(dot(Velocity, Velocity));
      return InEncodedVelocity.x > 0 && VelocityMagnitude > InVelocityThreshold;
  }
  ```
  with `r.HairStrands.VelocityThreshold = 1` pixel.
* `r.HairStrands.Visibility.WriteVelocityCoverageThreshold = 0` (write velocity for every hair pixel).

---

## 4. Shadowing and transmittance

### 4.1 Deep opacity maps (DOM)

Two textures per light, in an atlas: a **front-depth** map (`PF_DepthStencil`) and a **4-layer** DOM (`PF_FloatRGBA`), both `r.HairStrands.DeepShadow.Resolution = 2048` per slot (`DeepShadow.MinResolution = 64`, `AABBScale = 1.0`, `MaxFrustumAngle = 90°`).

Layer depths are an exponential ramp in clip space (`HairStrandsTransmittance.cpp`):

```cpp
FVector4f ComputeDeepShadowLayerDepths(float LayerDistribution)
{
	// LayerDistribution in [0..1]
	// Exponent in [1 .. 6.2]
	// Default LayerDistribution is 0.5, which is mapped onto exponent=3.1, making the last layer at depth 0.5f in clip space
	const float Exponent = FMath::Clamp(LayerDistribution, 0.f, 1.f) * 5.2f + 1;
	FVector4f Depths;
	Depths.X = FMath::Pow(0.2f, Exponent);
	Depths.Y = FMath::Pow(0.4f, Exponent);
	Depths.Z = FMath::Pow(0.6f, Exponent);
	Depths.W = FMath::Pow(0.8f, Exponent);
	return Depths;
}
```
At the default `LayerDistribution = 0.5` ⇒ exponent 3.1: layer depths ≈ **(0.0072, 0.0617, 0.2046, 0.4855)** of the clip-space depth range.

DOM accumulation is a pure additive splat of coverage into whichever layers the fragment is in front of:

```hlsl
void MainDom(in float4 SvPosition : SV_Position, in float HairCoverage : CUSTOM_COVERAGE, out float4 OutColor : SV_Target0)
{
	const uint2 PixelCoord = uint2(floor(SvPosition.xy));
	const float FrontDepth = DeepRasterPass.FrontDepthTexture.Load(uint3(PixelCoord, 0));
	const float DistanceToFrontDepth = GetDomDistanceToFrontDepth(FrontDepth, SvPosition.z);
	OutColor = ComputeDOMWeights(DistanceToFrontDepth, DeepRasterPass.LayerDepths) * HairCoverage;
}
```
with `ComputeDOMWeights` = a 4-vector of 0/1 ("is this fragment closer than layer k"). So DOM channel k accumulates **the number of hairs (coverage-weighted) between the front surface and layer k** — a cumulative histogram.

Lookup interpolates the histogram piecewise-linearly:

```hlsl
float ComputeHairCount(float4 DomValue, float DistanceToFrontDepth, float4 InLayerDepths)
{
	float OutCount = 0;
	if (DistanceToFrontDepth < InLayerDepths[0])
		OutCount = lerp(          0, DomValue[0], InterpolateCount(DistanceToFrontDepth, 0, InLayerDepths[0]));
	else if (DistanceToFrontDepth < InLayerDepths[1])
		OutCount = lerp(DomValue[0], DomValue[1], InterpolateCount(DistanceToFrontDepth, InLayerDepths[0], InLayerDepths[1]));
	else if (DistanceToFrontDepth < InLayerDepths[2]) ...
	else if (DistanceToFrontDepth < InLayerDepths[3]) ...
	else OutCount = DomValue[3];
	return OutCount;
}
```

Filtering kernels (`r.HairStrands.DeepShadow.KernelType`, default **2**):

```
0: linear, 1: PCF 2x2, 2: PCF 6x4 (default), 3: PCSS, 4: PCF 6x6 accurate
```
`SampleDOM_PCF` is a 3×3 grid of `SampleDOM_PCF2x2` taps at ±2 texel offsets, averaged. PCSS uses a 16-tap Poisson disk, 5 occluder-search taps, and `r.HairStrands.DeepShadow.KernelAperture = 1` degree.

Density compensation for sparse grooms: `r.HairStrands.DeepShadow.DensityScale = 2` ("Set density scale for compensating the lack of hair fiber in an asset"), depth bias `r.HairStrands.DeepShadow.DepthBiasScale = 0.05`.

```hlsl
	Out.HairCount  = HairCount * Settings.DeepShadowDensityScale;
	Out.Visibility = 1;
```

Shadow **onto opaque geometry** is a separate mask pass, `r.HairStrands.DeepShadow.ShadowMaskPassType = 1` (one pass for all groups), `ShadowMaskKernelType = 4` ("Gaussian8 with transmittance"). UE5 also routes hair through Virtual Shadow Maps via `EVirtualShadowMapProjectionInputType::HairStrands`.

### 4.2 Virtual voxel volume

Used for every light that has no DOM, plus environment lighting, AO, RT shadows and Lumen.

```hlsl
#define VOXEL_HAIR_MASK		0x00FFFFFF
#define VOXEL_OPAQUE_MASK	0xFF000000
#define VOXEL_OPAQUE_ADD	0xFF000000
#define VOXEL_OPAQUE_SHIFT	24

float GetVoxelDensityFixPointScale() { return 1000.f; }

float GetInternalVoxelHairCount(uint RawDensity)
{
	return (RawDensity & VOXEL_HAIR_MASK) / GetVoxelDensityFixPointScale();
}
float GetInternalVoxelOpaqueVisibility(uint RawDensity)
{
	const uint Raw = (RawDensity & VOXEL_OPAQUE_MASK) >> VOXEL_OPAQUE_SHIFT;
	return 1.f - saturate(Raw / 255.f);
}
```

**One `uint` per voxel: bits [23:0] = hair count × 1000, bits [31:24] = opaque occlusion.** Both accumulate with a single `InterlockedAdd` (opaque geometry injects `0xFF000000`).

Voxel rasterisation: `InterlockedAdd(DensityTexture[PageVoxelCoord], HairCoverage * 1000)`.

Structure / defaults (`HairStrandsVoxelization.cpp`):

```
r.HairStrands.Voxelization                              = 1
r.HairStrands.Voxelization.Virtual                      = 1
r.HairStrands.Voxelization.Virtual.VoxelWorldSize       = 0.3      // cm
r.HairStrands.Voxelization.Virtual.VoxelPageResolution  = 32       // 32^3 voxels per page
r.HairStrands.Voxelization.Virtual.VoxelPageCountPerDim = 14       // 14^3 = 2744 pages -> 448^3 page texture
r.HairStrands.Voxelization.Virtual.Adaptive             = 1
r.HairStrands.Voxelization.Virtual.Adaptive.CorrectionSpeed     = 0.1
r.HairStrands.Voxelization.Virtual.Adaptive.CorrectionThreshold = 0.90
r.HairStrands.Voxelization.Virtual.Jitter               = 1
r.HairStrands.Voxelization.DensityScale                 = 2.0      // "arbitraty"
r.HairStrands.Voxelization.DensityScale.{AO,Shadow,Transmittance,Environment,Raytracing} = -1  (inherit)
r.HairStrands.Voxelization.DepthBiasScale.Light         = 3.0
r.HairStrands.Voxelization.DepthBiasScale.Shadow        = 2.0
r.HairStrands.Voxelization.DepthBiasScale.Transmittance = 3.0
r.HairStrands.Voxelization.DepthBiasScale.Environment   = 1.8
r.HairStrands.Voxelization.Raymarching.SteppingScale    = 1.15
r.HairStrands.Voxelization.InjectOpaqueDepth            = 1
r.HairStrands.Voxelization.InjectOpaque.BiasCount       = 3        // voxels
r.HairStrands.Voxelization.InjectOpaque.MarkCount        = 6        // voxels
r.HairStrands.Voxelization.VoxelSizeInPixel             = 1        // (HairStrandsCluster.cpp)
```

Ray march (`HairStrandsVoxelPageTraversal.ush`), the `LINEAR_MIPMAP` variant:

```hlsl
	const float3 D = normalize(E - O) * InCommonDesc.VoxelWorldSize;
	const float MaxStep = float(min(ceil(OELength / InCommonDesc.VoxelWorldSize), 1024u));
	const float DeltaWorld = OELength / float(MaxStep);
	float StepScale = 1.0f;
	for (float StepIt = 0.0f; StepIt < MaxStep; StepIt += StepScale)
	{
		const float SteppingWorldSize = max(DeltaWorld * StepScale, InSettings.TanConeAngle * StepIt * InCommonDesc.VoxelWorldSize);
		const float3 HitP = O + StepIt * D + InSettings.JitterScale * RandomStepJitter * SteppingWorldSize * 0.5f;
		...
		HairCountScale = SteppingWorldSize * MipLevelDistanceScale;
		MipLevel = log2(SteppingWorldSize * MipLevelDistanceScale);
		const FHairTraversalResult StepResult = GetHairVirtualVoxelDensity(VoxelPageCoord, InPageTexture, uint(MipLevel), InSettings.DensityScale * HairCountScale, InSettings.bCastShadow);
		Acc(Out, StepResult);
		...
		StepScale = min(float(InCommonDesc.PageResolution), StepScale * InSettings.SteppingScale);
	}
```
Accumulation: `HairCount += `, `Visibility = min(...)`. Early-out when `Visibility == 0` or `HairCount > CountThreshold`. Note the **geometrically growing step** (×1.15 each step, capped at one page) and the mip-level tied to step size — it's an optical-depth integral, not a voxel count.

### 4.3 Transmittance mask

The per-sample, per-light result of either DOM or voxel traversal:

```hlsl
struct FHairTransmittanceMask { float HairCount; float Visibility; };

uint PackTransmittanceMask(FHairTransmittanceMask In)
{
	return min(uint(In.HairCount * 1000), uint(0x00FFFFFF)) | (min(uint(In.Visibility * 0xFF), uint(0xFF)) << 24);
}
```
24-bit fixed-point hair count (×1000) + 8-bit opaque visibility. Directional lights use `VoxelShadowMaxDistance = 100000.0; // 1 km shadow distance`.

### 4.4 Dual scattering (Zinke 2008), as implemented

`HairStrandsDeepTransmittanceDualScattering.ush` — reproduce this, it is where most of the "volume" in UE hair comes from.

```hlsl
float Hair_g2(float Variance, float Theta)
{
	const float A = 1.f;                       // note: NOT normalized
	return A * exp(-0.5 * Pow2(Theta) / Variance);
}

FHairTransmittanceData ComputeDualScatteringTerms(
	const FHairTransmittanceMask TransmittanceMask,
	const FHairAverageScattering AverageScattering,
	float Roughness, const float Backlit,
	const float3 V, const float3 L, const float3 T,
	const uint HairComponents)
{
	const float SinThetaL = clamp(dot(T, L), -1, 1);
	const float SinThetaV = clamp(dot(T, V), -1, 1);
	const float MaxAverageScatteringValue = 0.99f;

	// Straight implementation of the dual scattering paper
	const float3 af	 = min(MaxAverageScatteringValue.xxx, AverageScattering.A_front);
	const float3 af2 = Pow2(af);
	const float3 ab  = min(MaxAverageScatteringValue.xxx, AverageScattering.A_back);
	const float3 ab2 = Pow2(ab);
	const float3 OneMinusAf2 = 1 - af2;

	const float3 A1 = ab * af2 / OneMinusAf2;
	const float3 A3 = ab * ab2 * af2 / (OneMinusAf2*Pow2(OneMinusAf2));
	const float3 Ab = A1 + A3;

	// Add a min/max roughness for dual scattering ... adhoc
	Roughness = clamp(Roughness, 0.18f, 0.6f);
	const float Beta_R	 = Pow2( Roughness );
	const float Beta_TT	 = Pow2( Roughness / 2 );
	const float Beta_TRT = Pow2( Roughness * 2 );

	const float Shift     = 0.035;
	const float Shift_R   =-0.035*2;
	const float Shift_TT  = 0.035;
	const float Shift_TRT = 0.035*4;

	// Average density factor (This is the constant used in the original paper)
	const float df = 0.7f;
	const float db = 0.7f;

	// Always shift the hair count by one to remove self-occlusion/shadow aliasing
	const float HairCount = max(0, TransmittanceMask.HairCount - 1);

	const float3 af_weights = af / (af.r + af.g + af.b);
	const float3 Beta_f  = dot(float3(Beta_R, Beta_TT, Beta_TRT), af_weights);
	const float3 Beta_f2 = Beta_f*Beta_f;
	const float3 sigma_f2 = Beta_f2 * max(1.f, HairCount);

	const float Theta_d = asin(SinThetaL) + asin(SinThetaV);
	const float Theta_h = Theta_d * 0.5f;

	// Global scattering spread 'Sf'
	float3 Sf = float3(Hair_g2(sigma_f2.r, Theta_h), Hair_g2(sigma_f2.g, Theta_h), Hair_g2(sigma_f2.b, Theta_h)) / PI;
	const float3 Tf = pow(AverageScattering.A_front, HairCount);

	const float3 shift_f = dot(float3(Shift_R, Shift_TT, Shift_TRT), af_weights);
	const float3 shift_b = shift_f;
	const float3 delta_b = shift_b * (1 - 2*ab2 / Pow2(1 - af2)) * shift_f * (2 * Pow2(1 - af2) + 4*af2*ab2)/Pow3(1-af2);

	const float3 ab_weights = ab / (ab.r + ab.g + ab.b);
	const float3 Beta_b  = dot(float3(Beta_R, Beta_TT, Beta_TRT), ab_weights);
	const float3 Beta_b2 = Beta_b * Beta_b;

	const float3 sigma_b = (1 + db*af2) * (ab*sqrt(2*Beta_f2 + Beta_b2) + ab*ab2*sqrt(2*Beta_f2 + Beta_b2)) / (ab + ab*ab2*(2*Beta_f + 3*Beta_b));
	const float3 sigma_b2 = sigma_b * sigma_b;

	// Local scattering Spread 'Sb'
	float3 Sb = float3(	Hair_g2(sigma_f2.r + sigma_b2.r, Theta_h - delta_b.r),
						Hair_g2(sigma_f2.g + sigma_b2.g, Theta_h - delta_b.g),
						Hair_g2(sigma_f2.b + sigma_b2.b, Theta_h - delta_b.b)) / PI;

	const float3 GlobalScattering = lerp(1, Tf * Sf * df, saturate(HairCount));
	const float3 LocalScattering  = 2 * Ab * Sb * db;

	FHairTransmittanceData Out = InitHairStrandsTransmittanceData();
	Out.ScatteringComponent = HairComponents | HAIR_COMPONENT_MULTISCATTER;
	Out.GlobalScattering = (HairComponents & HAIR_COMPONENT_GS) > 0 ? GlobalScattering : 1;
	Out.LocalScattering  = (HairComponents & HAIR_COMPONENT_LS) > 0 ? LocalScattering : 0;
	Out.bUseLegacyAbsorption = (HairComponents & HAIR_COMPONENT_TT_MODEL) == 0;
	Out.OpaqueVisibility = TransmittanceMask.Visibility;
	return Out;
	// Final computation is done in ShadingModels.ush with the following formula
	// GlobalScattering * (Fs + LocalScattering) * TransmittanceMask.OpaqueVisibility;
}
```

Practical notes:
* `Roughness` is **clamped to [0.18, 0.6]** for dual scattering only — "Min/lower bound helps with BSDF being too narrow and causing some fireflies, Max/upper bound helps against 'too-flat' look".
* `HairCount − 1` shift is what removes the self-shadow term of the strand you're shading.
* `df = db = 0.7`.
* `Hair_g2` is **unnormalised** (`A = 1`), then divided by π.
* Transmittance for the non-shaded case is just `pow(A_front, HairCount) * Visibility`:
  ```hlsl
  float3 GetHairTransmittanceOnly(FHairTransmittanceMask TransmittanceMask, FGBufferData GBuffer, float SinLightAngle, ...)
  {
      const float HairCount = max(0.f, TransmittanceMask.HairCount - 1.f);
      const FHairAverageScattering AbsorptionData = SampleHairLUT(..., GBuffer.BaseColor, GBuffer.Roughness, SinLightAngle);
      return pow(AbsorptionData.A_front, HairCount) * TransmittanceMask.Visibility;
  }
  ```

### 4.5 The scattering LUTs

`A_front` / `A_back` come from a precomputed **3D LUT** indexed by `(|sin(viewAngle)|, roughness, sqrt(absorption))`:

```hlsl
float3 FromLinearAbsorption(float3 In) { return sqrt(In);  }
float3 ToLinearAbsorption(float3 In)   { return In*In; }

FHairAverageScattering SampleHairLUT(Texture3D<float4> LUTTexture, SamplerState LUTSampler, float3 InAbsorption, float Roughness, float SinViewAngle)
{
	const float3 RemappedAbsorption = FromLinearAbsorption(InAbsorption);
	const float2 LUTValue_R = LUTTexture.SampleLevel(LUTSampler, float3(saturate(abs(SinViewAngle)), saturate(Roughness), saturate(RemappedAbsorption.x)), 0).xy;
	... G, B ...
	Output.A_front = float3(LUTValue_R.x, LUTValue_G.x, LUTValue_B.x);
	Output.A_back  = float3(LUTValue_R.y, LUTValue_G.y, LUTValue_B.y);
	return Output;
}
```
Three separate 1-channel lookups (one per colour channel) into the same LUT — the LUT is monochrome-parameterised by absorption.

LUT dimensions (`HairStrandsLUT.cpp`): `IncidentAngleCount = 64`, `RoughnessCount = 64`, `AbsorptionCount = 16`, `SampleCountScale = 1`.

How it is built (`HairStrandsLUT.usf`) — **you can reproduce this offline**:

```hlsl
	const float SinAngle   	= saturate(float(PixelCoord.x+0.5f) / ThetaCount);
	const float Roughness  	= saturate(float(PixelCoord.y+0.5f) / RoughnessCount);
	const float Absorption 	= saturate(float(PixelCoord.z+0.5f) / AbsorptionCount);
	const float CosAngle 	= sqrt(1-SinAngle*SinAngle);

	FGBufferData GBufferData;
	GBufferData.Specular  	= 0.5f;
	GBufferData.BaseColor	= ToLinearAbsorption(Absorption.xxx);   // Perceptual absorption
	GBufferData.Metallic	= 0;                                    // This disable the fake multiple scattering
	GBufferData.Roughness 	= Roughness;
	GBufferData.CustomData  = float4(0, 0, 1, 0);                   // Backlit

	const uint LocalThetaSampleCount = max(1u, SampleCountScale * lerp(128, 64, Roughness));
	const uint LocalPhiSampleCount   = max(1u, SampleCountScale * lerp(128, 32, Roughness));

	const float Area = 0;
	const float Backlit = 1;
	const float3 N = float3(0,0,1); // N is the vector parallel to hair pointing toward root
	const float3 V = float3(CosAngle, 0, SinAngle);
	...
		const float4 SampleDirection = UniformSampleSphere(u.yx);
		const float3 BSDFValue = HairShading(GBufferData, L, JitteredV, N, 1, TransmittanceData, Backlit, Area, 0);
		// back hemisphere (R / TRT) is on the positive side of X; front hemisphere (TT) on the negative side
		const bool bIsBackHemisphere = SampleDirection.x > 0;
	...
	const float HemisphereFactor = 0.5f;
	OutputColor[PixelCoord] = float4(
		saturate(FrontHemisphereOutput / FrontHemisphereCount * HemisphereFactor),
		saturate(BackHemisphereOutput  / BackHemisphereCount  * HemisphereFactor), 0, 1);
```

There is a **second LUT**, `HairAverageEnergy`, holding per-lobe directional albedo `A_R, A_TT, A_TRT`, built the same way but with `ScatteringComponent` forced to one lobe at a time. It is used by the environment integrator.

---

## 5. Environment / sky lighting, RT, Lumen, path tracer

### 5.1 Sky lighting on hair

`HairStrandsEnvironmentLighting.usf`, four permutations:

```hlsl
#define INTEGRATION_SCENECOLOR 0
#define INTEGRATION_ADHOC 1
#define INTEGRATION_UNIFORM 2
#define INTEGRATION_SH 3
```
`r.HairStrands.SkyLighting.IntegrationType = 2` (Uniform) by default, `SampleCount = 16`, `ConeAngle = 3`, `DistanceThreshold = 10`, `TransmissionDensityScale = 10`, `UseViewHairCount = 1`.

The file's own caveat:
```hlsl
// * This version of environement lighting is very much in WIP state ...
// * There is a few fudges factor as well (roughness mapping, TT density, reflected vector orientations, ...)
```

**Roughness widening onto GGX** (`HairStrandsEnvironmentLightingCommon.ush`) — this is Karis' "increase the roughness as an approximation of a wider area light source", fitted:

```hlsl
float HairToGGXRoughness_R(float R_Roughness)
{
	const float X = saturate(R_Roughness) - 0.51f;
	return clamp(X, 0.1f, 1);
}

float HairToGGXRoughness_TRT(float R_Roughness)
{
	const float X = saturate(R_Roughness);
	const float X2 = X*X, X3 = X2*X, X4 = X2*X2;
	const float A = -0.207704f, B = 6.95267f, C = -10.1783f, D = 4.24751f, F = 3.37235f, G = -1.96947f;
	return (A * X + B * X2 + C * X3 + D * X4) / (G * X + F * X2);
}

float HairToGGXRoughness_TT(float TT_Roughness)
{
	const float X = saturate(TT_Roughness);
	const float X2 = X*X, X3 = X2*X;
	const float A = 0.70505f, B = -0.117104f, C = 0.0259047f;
	return A * X + B * X2 + C * X3;
}
```
The multiple-scattering lobe uses `HairToGGXRoughness_TT(Roughness) * 2` (`FudgeScale = 2`).

The cheap shared helper, used everywhere a single directional-albedo estimate is needed:

```hlsl
float3 EvaluateEnvHair(FGBufferData GBuffer, float3 V, float3 N, inout float3 OutL)
{
	const float Shadow = 1;
	const float Backlit = 0;
	const float Area = 0.2f;
	// Evaluate the hair BSDF for a imaginary reflected direction, and uses it a measure of the directional albedo
	OutL = normalize(V - N * dot(V, N));
	FHairTransmittanceData TransmittanceData = InitHairTransmittanceData(true);
	if (ShouldUseHairComplexTransmittance(GBuffer)) { TransmittanceData = EvaluateDualScattering(GBuffer, V, OutL.xyz); }
	return min(1.f, 2 * PI * HairShading(GBuffer, OutL, V, N, Shadow, TransmittanceData, Backlit, Area, Random));
}
```
Note **`Area = 0.2`** here — that is the only place the `Area` roughness widening is actually used (it is added to `B[p]`, i.e. to `β²`, so it is a *variance* offset).

The ADHOC integrator builds a transmittance-weighted "unoccluded normal" by shooting N spherical rays through the voxel volume, then blends the reflection vector 60/40 back toward the Kajiya fake normal:

```hlsl
		float3 N = UnoccludedN;
		R = 2 * dot( V, N ) * N - V;
		{
			N = GBuffer.WorldNormal;                       // == hair tangent
			N = normalize(V - N * dot(V, N));
			float3 R_b = 2 * dot(V, N) * N - V;
			const float alpha = 0.4f;
			R = normalize(lerp(R_b, R, alpha));
		}
```

The TT (see-through) term is evaluated with `L = -V` and can be driven by the screen-space hair-count texture instead of voxels:
```hlsl
			if (bHairUseViewHairCount)
			{
				Result.HairCount = HairCountTexture.Load(uint3(SvPosition.xy, 0)) * max(TransmissionDensityScaleFactor, 0);
				Result.Visibility = 1;
			}
			FHairAverageEnergy Energy = SampleHairEnergyLUT(HairEnergyLUTTexture, HairLUTSampler, GBuffer.BaseColor, GBuffer.Roughness, SinViewAngle);
			ColorTT.rgb *= Energy.A_TT * TransmittanceMask.Transmittance * TransmittanceMask.Visibility * Backlit;
```

Final: `Color = ColorR + ColorTT + ColorMS`. And crucially:
```hlsl
// Disable static lighting, as hair don't have any diffused lighting baked,
// and rely solely on specular information (i.e. reflection capture)
#undef ALLOW_STATIC_LIGHTING
#define ALLOW_STATIC_LIGHTING 0
```

`INTEGRATION_SCENECOLOR` is the "Scatter Scene Lighting" path for vellus/short hair: divide scene colour by pixel coverage to recover the unoccluded background, scale by `A_front`, re-multiply by coverage. Gated per-sample by `bScatterSceneLighting` (Groom asset flag, default **false**), globally `r.HairStrands.ScatterSceneLighting = 1`.

### 5.2 Hair AO

`HairStrandsEnvironmentAO.usf`, cosine-hemisphere voxel cone tracing from the *opaque* GBuffer (darkens skin under hair), defaults `r.HairStrands.SkyAO = 1`, `SkyAO.SampleCount = 4`, `SkyAO.DistanceThreshold = 10`:

```hlsl
		const float StepVisibility = saturate(1 - Result.HairCount) * Result.Visibility;
		BentNormal += SampleL * StepVisibility;
		Visibility += StepVisibility;
	}
	Visibility /= SampleCount;
	const float AO = 1 - (1 - pow(saturate(Visibility), AO_Power)) * AO_Intensity;
	return saturate(AO);
```

### 5.3 Ray tracing / Lumen

* **Hair is not in the BVH for shadows/sky/Lumen.** `HairStrandsRaytracing.ush` ray-marches the same voxel volume, with `DistanceThreshold = 1000` cm and `CoverageThreshold = 0.995`. `r.RayTracing.Shadows.HairOcclusionThreshold = 1`, `r.RayTracing.Sky.HairOcclusionThreshold = 1` ("number of hair that need to be crossed, before casting occlusion").
* **Actual RT geometry exists** for the path tracer and opt-in effects (`r.HairStrands.Raytracing = 1`, per-asset `bUseHairRaytracingGeometry` default **false**). UE 4.26 emitted **4 triangles per segment** (a cross of two quads); UE 5.3 emits either **procedural AABBs** (`RaytracingProceduralSplits` per segment, with a surface-area heuristic that collapses back to one AABB when splitting doesn't pay) or a **4-sided prism** (8 triangles/segment). Radius used is `HairStrandsVF.Radius * HairStrandsVF.RaytracingRadiusScale` — `FHairShadowSettings::HairRaytracingRadiusScale`, default **1.0**. Dead segments are marked with `NaN` so DXR drops them.
* **Lumen** does not put strands through screen probes; `HairStrandsEnvironmentLighting` remains the sole indirect path, with `GatherRadiance()` resolving to Lumen probes when enabled. VSMs support hair via a hair-specific projection input type.

### 5.4 Path tracer: a *different*, more exact BSDF

`PathTracing/Material/PathTracingHair.ush` implements **Chiang et al. 2016** (Disney's practical hair model) — **not** `HairShading`.

```hlsl
#define PATH_TRACER_HAIR_SHADING_MODEL	1 // 0: zinke diffuse (a simple, cheap model for performance testing)
                                          // 1: chiang principled hair (a sophisticated model with specular reflection/transmission)
```

Parameterisation — note the **mapping from UE's material pins**:

```hlsl
FHairData PrepareHairData(float3 V, float3 Color, float LongitudinalRoughness, float AzimuthalRoughness, float h, float Specular)
{
	float Bm = max(LongitudinalRoughness, 0.01);
	...
	// Chiang et al. - Eq(7)
	Result.v0 = Pow2(0.726 * Bm + 0.812 * Bm2 + 3.7 * Bm20);
	Result.v1 = 0.25 * Result.v0;
	Result.v2 = 4.00 * Result.v0;
	Result.v3 = Result.v2;

	const float SqrtPiOver8 = 0.626657069;
	float Bn = max(AzimuthalRoughness, 0.01);
	// Chiang et al. - Eq(8) + Eq(12)
	Result.s = SqrtPiOver8 * (0.265 * Bn + 1.194 * Bn2 + 5.372 * Bn22);

	// Specular to Eta
	float F0 = Specular * 0.08;
	float F90 = saturate(Specular * (50.0 * 0.08));
	float Eta = (1 + sqrt(F0)) / (1 - sqrt(F0));
	...
	// TODO: should this angle be exposed as a parameter?  Using value from HairBsdf.ush for now
	float ScaleAngle = 0.035; // about 2 degrees
	...
	Result.SinThetaO.x = SinThetaO * CosScale.y - CosThetaO * SinScale.y; 	// R: -2*angle
	Result.SinThetaO.y = SinThetaO * CosScale.x + CosThetaO * SinScale.x;	// TT: +angle
	Result.SinThetaO.z = SinThetaO * CosScale.z + CosThetaO * SinScale.z;	// TRT: +4*angle
	Result.SinThetaO.w = SinThetaO;	// TR*T: 0 angle
	...
	// Chiang et al. - Eq(9)
	float3 SigmaA = Pow2(log(max(Color, 1e-4)) / ((((((0.245 * Bn) + 5.574) * Bn - 10.73) * Bn + 2.532) * Bn - 0.215) * Bn + 5.969));
	// Zinke 2007 - Eq(20)
	float3 T = exp(-SigmaA * (2 * sqrt(CosGammaT2 / CosThetaT2)));
	// d'Eon et al. - Eq(12) + Eq(13)
	Result.A0 = F;
	Result.A1 = Pow2(1 - F) * T;
	Result.A2 = Result.A1 * F * T;
	// Chiang et al. - Eq(6)   (the compressed TR^nT residual lobe)
	Result.A3 = Result.A2 * F * T * rcp(1.00001 - F * T);
}
```
and in `Hair_SampleMaterial`:
```hlsl
	const float h = 2 * Payload.GetHairPrimitiveUV().y - 1; // remap back to [-1,1]
	const float LongitudinalRoughness = Payload.GetHairLongitudinalRoughness();   // = Roughness
	const float AzimuthalRoughness = lerp(1.0, 0.1, Payload.GetHairAzimuthalRoughness()); // = Scatter (Metallic)
```

So in the path tracer: **Roughness → longitudinal roughness; the Scatter pin → azimuthal roughness (remapped 1.0 → 0.1); Specular → IOR via `F0 = 0.08·Specular`; `h` comes from the V coordinate across the ribbon.** Four lobes (R, TT, TRT, and a compressed residual TR^nT). `Mp` uses a numerically-stable `LogI0` fit, `Np` a trimmed logistic. The path tracer therefore **will not match the raster view exactly** — expect a different, more saturated, more physically-complete look.

---

## 6. Per-strand / per-point attributes

Storage (`HairStrandsAttributeCommon.ush`) — attributes are split into **per-curve** and **per-point** byte-address buffers with per-attribute offsets:

```hlsl
#define HAIR_CURVE_ATTRIBUTE_STRIDE_ROOTUV		4
#define HAIR_CURVE_ATTRIBUTE_STRIDE_SEED		1
#define HAIR_CURVE_ATTRIBUTE_STRIDE_LENGTH		2
#define HAIR_CURVE_ATTRIBUTE_STRIDE_CLUMPID		2
#define HAIR_CURVE_ATTRIBUTE_STRIDE_CLUMPID3	8

#define HAIR_POINT_ATTRIBUTE_STRIDE_COLOR		4
#define HAIR_POINT_ATTRIBUTE_STRIDE_ROUGHNESS	1
#define HAIR_POINT_ATTRIBUTE_STRIDE_AO			1
```

Accessors (`HairStrandsAttributeTemplate.ush`) — these *are* the Hair Attributes material node:

| Material node output | Accessor | Semantics |
|---|---|---|
| U / V | `GetHairStrandsUV(cpId, segmentUV)` | **U = 0 at root, 1 at tip**, lerped from the packed per-CP `UCoord`; V = across the ribbon |
| Length | `GetHairStrandsDimensions().x` | `CurveLength * U` — distance along the curve, cm |
| Radius | `GetHairStrandsDimensions().y` | world radius at this point (after HairWidth/root/tip scaling) |
| Root UV | `GetHairStrandsRootUV(cpId)` | UV of the binding surface at the follicle; returns 0 if absent |
| Seed | `GetHairStrandsSeed(cpId)` | 8-bit per-curve random, **constant along the curve**, 0..1 |
| Clump ID | `GetHairStrandsClumpID(cpId)` | 16-bit, or a 3-level `uint3` variant |
| BaseColor | `GetHairStrandsColor(cpId, u)` | per-point, lerped along the segment; 0 if absent |
| Roughness | `GetHairStrandsRoughness(cpId, u)` | per-point, lerped |
| AO | per-curve AO | |
| Tangent | from the tangent buffer | |
| Group Index | per instance | |
| Depth / Coverage / AtlasUVs / AuxiliaryData | cards & meshes only | |

Alembic attribute names the importer looks for (`HairAttributes.cpp`):

```cpp
	const FName Vertex::Color("groom_color");
	const FName Vertex::Roughness("groom_roughness");
	const FName Vertex::AO("groom_ao");
	const FName Vertex::Width("groom_width");

	const FName Strand::GroupID("groom_group_id");
	const FName Strand::Guide("groom_guide");
	const FName Strand::ID("groom_id");
	const FName Strand::ClumpID("groom_clumpid");
	const FName Strand::RootUV("groom_root_uv");
	const FName Strand::VertexCount("vertexcount");
	const FName Strand::ClosestGuides("groom_closest_guides");
	const FName Strand::GuideWeights("groom_guide_weights");
	const FName Strand::BasisType("groom_basis_type");
	const FName Strand::CurveType("groom_curve_type");
	const FName Strand::Knots("groom_knots");
```
plus groom-level `groom_color`, `groom_roughness`, `groom_width`, `groom_version_major/minor`, `groom_tool`, `groom_properties`. There are exactly 7 optional attributes (`EHairAttribute::Count == 7`): RootUV, ClumpID, StrandID, PrecomputedGuideWeights, Color, Roughness, AO; flags include `HasRootUDIM` and `HasMultipleClumpIDs`.

**This name set is directly relevant for a USD implementation** — a USD groom that wants UE parity should carry `groom_width`, `groom_root_uv`, `groom_clumpid`, `groom_color`, `groom_roughness`, `groom_ao` as primvars with matching interpolation.

---

## 7. Defaults reference

### 7.1 Groom asset per-group defaults (`GroomAssetRendering.cpp`)

```cpp
FHairGeometrySettings::FHairGeometrySettings()
{
	HairWidth = 0.01;      // centimeters  -> 100 micrometres
	HairRootScale = 1;
	HairTipScale = 1;
}
FHairShadowSettings::FHairShadowSettings()
{
	HairShadowDensity = 1;
	HairRaytracingRadiusScale = 1;
	bVoxelize = true;
	bUseHairRaytracingGeometry = false;
}
FHairAdvancedRenderingSettings::FHairAdvancedRenderingSettings()
{
	bUseStableRasterization = false;
	bScatterSceneLighting = false;
}
```
LOD entry defaults: `CurveDecimation = 1`, `VertexDecimation = 1`, `AngularThreshold = 1°`, `ScreenSize = 1`, `ThicknessScale = 1`, `bVisible = true`, `GeometryType = Strands`, `BindingType = Skinning`.
Interpolation defaults: `HairToGuideDensity = 0.1`, quality `High`, `InterpolationDistance = Parametric`, `bRandomizeGuide = false`, `bUseUniqueGuide = false`, `GuideType = Imported`, `RiggedGuideNumCurves = 10`, `RiggedGuideNumPoints = 4`.

### 7.2 `r.HairStrands.*` console variables (UE 5.3, from source)

Extracted directly from `FAutoConsoleVariableRef` registrations. Values are engine defaults.

**Global toggles** (`HairStrandsInterface.cpp`, `GroomAsset.cpp`, `GroomManager.cpp`)

| CVar | Default | Note |
|---|---|---|
| `r.HairStrands.Strands` | 1 | enable strand rendering |
| `r.HairStrands.Cards` | 1 | |
| `r.HairStrands.Meshes` | 1 | |
| `r.HairStrands.Binding` | 1 | attach to skeletal meshes |
| `r.HairStrands.Simulation` | 1 | |
| `r.HairStrands.Raytracing` | 1 | opt-in per asset |
| `r.HairStrands.ContinuousDecimationReordering` | 0 | experimental continuous LOD |
| `r.HairStrands.Shadow.CastShadowWhenNonVisible` | 1 | |
| `r.HairStrands.Visibility.NonVisibleShadowCasting.CullDistance` | 2000 | |
| `r.HairStrands.MinLOD` | 0 | |
| `r.HairStrands.MaxSimulatedLOD` | -1 | |
| `r.HairStrands.UseCardsInsteadOfStrands` | 0 | |
| `r.HairStrands.AutoLOD.Force` | 0 | |
| `r.HairStrands.AutoLOD.Bias` | 0.0 | screen-size bias |
| `r.HairStrands.AutoLOD.Scale` | 1.0 | exponent on screen size |
| `r.HairStrands.Streaming` | 0 | `Streaming.CurvePage = 2048` |
| `r.HairStrands.BoundMode` | 0 | |

*Note:* `r.HairStrands.LODMode` (default 1 = Auto) is a **5.4+** cvar; in 5.3 the equivalent is the per-asset `bAutoLOD` plus `AutoLOD.Force`.

**BSDF components** (`HairStrandsUtils.cpp`)

| CVar | Default |
|---|---|
| `r.HairStrands.Components.R` | 1 |
| `r.HairStrands.Components.TT` | 1 |
| `r.HairStrands.Components.TRT` | 1 |
| `r.HairStrands.Components.LocalScattering` | 1 |
| `r.HairStrands.Components.GlobalScattering` | 1 |
| `r.HairStrands.Components.TTModel` | 0 (0 = legacy `pow()` absorption) |
| `r.HairStrands.DualScatteringRoughness` | 0 (0 = no override) |

**Rasterization / geometry** (`HairStrandsUtils.cpp`)

| CVar | Default | Note |
|---|---|---|
| `r.HairStrands.RasterizationScale` | **0.5** | "For no AA without TAA, a good value is: 1.325f (Empirical)" |
| `r.HairStrands.StableRasterizationScale` | **1.0** | clamped to ≥1 |
| `r.HairStrands.VelocityRasterizationScale` | **1.5** | |
| `r.HairStrands.ShadowRasterizationScale` | 1.0 | |
| `r.HairStrands.DeepShadow.AABBScale` | 1.0 | |
| `r.HairStrands.DeepShadow.MaxFrustumAngle` | 90° | |
| `r.HairStrands.RectLightingOptim` | 1 | |
| `r.HairStrands.ComposeAfterTranslucency` | 1 | |

**Visibility** (`HairStrandsVisibility.cpp`)

| CVar | Default |
|---|---|
| `r.HairStrands.Visibility.MSAA.SamplePerPixel` | **8** |
| `r.HairStrands.Visibility.MSAA.MeanSamplePerPixel` | 0.75 |
| `r.HairStrands.Visibility.PPLL` | 0 |
| `r.HairStrands.Visibility.PPLL.SamplePerPixel` | 16 |
| `r.HairStrands.Visibility.PPLL.MeanSamplePerPixel` | 1 |
| `r.HairStrands.Visibility.ComputeRaster` | 0 |
| `r.HairStrands.Visibility.ComputeRaster.ContinuousLOD` | 1 |
| `r.HairStrands.Visibility.Compute.SamplePerPixel` | 1 |
| `r.HairStrands.Visibility.Compute.MeanSamplePerPixel` | 0.75 |
| `r.HairStrands.Visibility.ComputeRaster.TileSize` | 32 |
| `r.HairStrands.Visibility.ComputeRaster.NumBinners` | 32 |
| `r.HairStrands.Visibility.ComputeRaster.NumRasterizers` | 256 |
| `r.HairStrands.Visibility.ComputeRaster.MaxTiles` | 65536 |
| `r.HairStrands.Visibility.FullCoverageThreshold` | **0.98** |
| `r.HairStrands.Visibility.UseCoverageMappping` | 0 |
| `r.HairStrands.Visibility.SortByDepth` | 0 |
| `r.HairStrands.Visibility.HairCount.DistanceThreshold` | 30.0 |
| `r.HairStrands.Visibility.WriteVelocityCoverageThreshold` | 0 |
| `r.HairStrands.MaterialCompaction.DepthThreshold` | 1.0 cm |
| `r.HairStrands.MaterialCompaction.TangentThreshold` | 10° |
| `r.HairStrands.LightSampleFormat` | 1 (R11G11B10) |
| `r.HairStrands.VelocityType` | 1 (closest) |
| `r.HairStrands.VelocityThreshold` | 1 px |
| `r.HairStrands.DOFDepth` | 1 |
| `r.HairStrands.WriteGBufferData` | 1 |
| `r.HairStrands.PathTracing.InvalidationThreshold` | 0.05 cm |

**Deep shadow / transmittance** (`HairStrandsDeepShadow.cpp`, `HairStrandsTransmittance.cpp`)

| CVar | Default |
|---|---|
| `r.HairStrands.DeepShadow.Resolution` | **2048** |
| `r.HairStrands.DeepShadow.MinResolution` | 64 |
| `r.HairStrands.DeepShadow.GPUDriven` | 1 |
| `r.HairStrands.DeepShadow.DensityScale` | **2** |
| `r.HairStrands.DeepShadow.DepthBiasScale` | 0.05 |
| `r.HairStrands.DeepShadow.KernelType` | **2** (PCF 6×4) |
| `r.HairStrands.DeepShadow.KernelAperture` | 1° |
| `r.HairStrands.DeepShadow.ShadowMaskKernelType` | 4 (Gaussian8 + transmittance) |
| `r.HairStrands.DeepShadow.ShadowMaskPassType` | 1 |
| `r.HairStrands.DeepShadow.MipTraversal` | 1 |
| `r.HairStrands.DeepShadow.RandomType` | 2 |
| `r.HairStrands.DeepShadow.SuperSampling` | 0 |
| `r.HairStrands.DeepShadow.InjectVoxelDepth` | 0 |

**Voxelization** — see the table in §4.2.

**Environment** (`HairStrandsEnvironment.cpp`)

| CVar | Default |
|---|---|
| `r.HairStrands.SkyLighting` | 1 |
| `r.HairStrands.SkyLighting.IntegrationType` | **2** |
| `r.HairStrands.SkyLighting.SampleCount` | 16 |
| `r.HairStrands.SkyLighting.ConeAngle` | 3 |
| `r.HairStrands.SkyLighting.DistanceThreshold` | 10 |
| `r.HairStrands.SkyLighting.TransmissionDensityScale` | 10 |
| `r.HairStrands.SkyLighting.UseViewHairCount` | 1 |
| `r.HairStrands.SkyAO` | 1 |
| `r.HairStrands.SkyAO.SampleCount` | 4 |
| `r.HairStrands.SkyAO.DistanceThreshold` | 10 |
| `r.HairStrands.ScatterSceneLighting` | 1 |

**LUTs** (`HairStrandsLUT.cpp`): `HairLUT.IncidentAngleCount = 64`, `HairLUT.RoughnessCount = 64`, `HairLUT.AbsorptionCount = 16`, `HairLUT.SampleCountScale = 1`.

---

## 8. Prioritized parity plan

### Tier 0 — without these it will not read as "UE hair" at all

1. **`HairShading()` verbatim (R + TT + TRT).** Port §1.2 literally, including `Shift = 0.035`, `α = {−2Shift, Shift, 4Shift}`, `B = {β², β²/2, 2β²}`, the `sinθ_L + sinθ_V − α` Gaussian argument, `n' = 1.19/cosθ_d + 0.36·cosθ_d`, `N_TT = exp(−3.65cosφ − 3.98)`, `N_TRT = exp(17cosφ − 16.78)`, `Specular*2` on R only, `Backlit` on TT and on back-facing R. Do **not** substitute a "correct" Marschner — the constants are the look.
2. **Tangent-as-normal convention.** `N` = fibre tangent pointing to the **root**. Getting the sign wrong flips the specular shift and kills the R/TRT separation.
3. **View-aligned ribbon expansion with a minimum pixel width, plus coverage compensation.** `halfWidth = max(WorldRadius, RadiusAtDepth1 * distToCamera)` and `coverage = r / max(r, pixelRadius)`. This is the entire AA strategy; without it thin hair either aliases into sparkle or disappears. Use the §2.3 formula with `RasterizationScale ≈ 1.325` if you have no TAA, `0.5` with 8× MSAA.
4. **Order-independent accumulation with a coverage threshold.** Per-pixel sorted or MSAA-resolved samples, normalised per-sample weights, `PixelCoverage = saturate((1 − T)/0.98)`, composite as `color*coverage` over the background with `alpha = coverage`, depth = closest sample.
5. **Some shadow / self-occlusion term.** Karis: *"It is absolutely vital to the look."* Minimum viable: a hair-count along the light ray, then `Transmittance = pow(A_front, max(HairCount−1, 0))`. Even a crude screen-space or single-DOM-layer count beats none.

### Tier 1 — the difference between "a hair shader" and "UE hair"

6. **Dual scattering (§4.4) with the `A_front`/`A_back` LUT (§4.5).** `S_final = GlobalScattering·(S_single + LocalScattering)·OpaqueVisibility`. The LUT is cheap to bake offline (64×64×16 RGBA16F, generated by Monte-Carlo integrating your own `HairShading`). Include the `Roughness ∈ [0.18, 0.6]` clamp, the `HairCount − 1` shift and `df = db = 0.7`. This is where dark hair stops looking like black wire and blond hair stops looking like white plastic.
7. **Deep opacity maps, 4 layers, exponent-3.1 layer spacing (§4.1)**, or a voxel density volume. DOM is simpler and is what produces UE's soft volumetric self-shadowing; use the PCF 6×4 kernel (nine 2×2 bilinear taps at ±2 texels) and `DensityScale = 2`.
8. **Root/tip radius taper + `HairWidth` in centimetres (default 0.01 cm).** `radius = normRadius · HairWidth · lerp(RootScale, TipScale, U)`.
9. **Per-strand attributes: U along the strand (root=0), per-curve Seed, Root UV.** Nearly every production groom material varies colour/roughness by `Seed` and `RootUV` and darkens the root with `U`. Without these the groom is uniform and reads as fake.
10. **The `Scatter` (Kajiya-Kay) term for card/mesh geometry** if you support proxies, since UE's cards rely on it entirely.

### Tier 2 — visible polish

11. **Environment lighting with the GGX roughness remaps (§5.1)** — `HairToGGXRoughness_{R,TT,TRT}` plus the `×2` fudge on the MS lobe, the `Area = 0.2` variance offset, and the fake-normal reflection vector. Ambient-lit hair without this looks flat and grey.
12. **LOD thickness/coverage compensation** — `coverageScale = 1/curveRatio` when decimating curves, and `RadiusScale` lerp when using discrete LODs. Otherwise hair visibly thins out as you pull back.
13. **Velocity widening (×1.5) and TAA history rejection** for moving hair.
14. **`HairColorToAbsorption` / `GetHairColorFromMelanin`** as authoring helpers, and the non-legacy TT absorption (`exp(−σ_a·…)`) behind a flag.
15. **Hair AO onto opaque geometry** (voxel cone trace), which is what makes the scalp/forehead sit under the hair rather than glow.

### Tier 3 — nice to have / probably skip for a viewport

16. Compute rasterizer, PPLL, tile classification, HZB cluster culling — pure performance, no look change.
17. Path-tracer-grade Chiang BSDF (§5.4) — only if you want an offline reference lane; it will *not* match the raster look.
18. `bScatterSceneLighting` scene-colour path (vellus/peach fuzz).
19. Groom cache, RBF/skinning binding, Niagara XPBD simulation.

### Numbers to hard-code first

```
IOR n                    = 1.55        -> F0 = 0.0465
cuticle Shift            = 0.035 rad   -> alpha = {-0.070, +0.035, +0.140}
B (variance)             = {R^2, R^2/2, R^2*2}, R = clamp(Roughness, 1/255, 1)
R:   N = 0.25*cos(phi/2),  B *= sqrt(2)*cos(phi/2),  weight = Specular*2
TT:  N = exp(-3.65*cosPhi - 3.98),  Tp = BaseColor^(0.5*sqrt(1-(h/n')^2)/cosThetaD)
TRT: N = exp(17*cosPhi - 16.78),    Tp = BaseColor^(0.8/cosThetaD),  F = Fr(0.5*cosThetaD)
n'                       = 1.19/cosThetaD + 0.36*cosThetaD
dual scattering          df = db = 0.7, roughness clamp [0.18, 0.6], HairCount-1
coverage threshold       0.98
HairWidth                0.01 cm, root/tip scale 1.0
RasterizationScale       0.5 (8x MSAA) / 1.325 (no TAA)
DOM layers               4, depths = {0.2,0.4,0.6,0.8}^3.1
voxel                    0.3 cm, 32^3 pages, density fixed-point x1000, DensityScale 2
material defaults        BaseColor 0, Scatter 0, Specular 0.5, Roughness 0.5, Backlit 1
```

---

## 9. Source URLs

* UE mirror (5.3): `https://raw.githubusercontent.com/chenyong2github/UnrealEngine/5.3/Engine/Shaders/Private/HairBsdf.ush` — and the same base for `HairShadingCommon.ush`, `ShadingModels.ush`, `HairStrands/*.ush|usf`, `PathTracing/Material/PathTracingHair.ush`.
* UE mirror (4.26): same host, branch `master`.
* Renderer C++: `.../5.3/Engine/Source/Runtime/Renderer/Private/HairStrands/{HairStrandsUtils,HairStrandsVisibility,HairStrandsDeepShadow,HairStrandsVoxelization,HairStrandsTransmittance,HairStrandsEnvironment,HairStrandsLUT,HairStrandsComposition,HairStrandsCluster,HairStrandsInterface}.cpp`
* Groom plugin: `.../5.3/Engine/Plugins/Runtime/HairStrands/Source/HairStrandsCore/{Public,Private}/Groom*.{h,cpp}`, `HairAttributes.cpp`
* Material pin names/defaults: `.../5.3/Engine/Source/Runtime/Engine/Private/Materials/MaterialAttributeDefinitionMap.cpp`
* Karis, *Physically Based Hair Shading in Unreal*, SIGGRAPH 2016: https://blog.selfshadow.com/publications/s2016-shading-course/karis/s2016_pbs_epic_hair.pdf (course index: https://blog.selfshadow.com/publications/s2016-shading-course/)
* Epic docs: https://dev.epicgames.com/documentation/en-us/unreal-engine/hair-rendering-and-simulation-in-unreal-engine , https://dev.epicgames.com/documentation/unreal-engine/groom-materials-in-unreal-engine , https://dev.epicgames.com/documentation/unreal-engine/setting-up-level-of-detail-for-grooms-in-unreal-engine , https://dev.epicgames.com/documentation/unreal-engine/groom-scalability-and-performance-with-unreal-engine
* Underlying papers: Marschner et al. 2003 *Light Scattering from Human Hair Fibers*; d'Eon et al. 2011 *An Energy-Conserving Hair Reflectance Model*; Zinke et al. 2008 *Dual Scattering Approximation for Fast Multiple Scattering in Hair*; Chiang et al. 2016 *A Practical and Controllable Hair and Fur Model for Production Path Tracing* (https://media.disneyanimation.com/uploads/production/publication_asset/152/asset/eurographics2016Fur_Smaller.pdf); Pekelis et al. 2015 *A Data-Driven Light Scattering Model for Hair* (cited in `HairBsdf.ush`'s header).

## 10. Known gaps

* No public mirror of **UE 5.4–5.7** was reachable, so 5.4+ deltas are unverified. Known-by-name additions: `r.HairStrands.LODMode`, `r.HairStrands.RaytracingProceduralSplits`, `r.HairStrands.Strands.BulkData.AsyncLoading`. The BSDF itself is very unlikely to have changed.
* The engine-content groom **master material** (`MF_*` melanin/dye helpers used by MetaHuman) is binary content, not source; the math it wraps is `GetHairColorFromMelanin` above.
* Physics/simulation numeric defaults (XPBD substeps, stiffness) are not published and were not extracted — irrelevant for a viewport renderer.
* `r.HairStrands.DeepShadow.KernelType` docs on third-party cvar sites list a slightly different 0–4 mapping than the source string; the source string quoted in §4.1 is authoritative for 5.3.
