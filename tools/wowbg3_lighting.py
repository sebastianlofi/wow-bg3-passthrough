#!/usr/bin/env python3
"""wowbg3_lighting.py OUT.lsx: the lighting and atmosphere resources of the levels of our own.

Written from the numbers below, not copied from a BG3 resource. What they are for:

- Lighting (two resources with the same numbers, LTN_WOWBG3_WoWSun and LTN_WOWBG3_WoWSun_B: the mod's WowSky.lua writes
  WoW's sun into the one that is not active and switches the level's zone to it, which is when BG3 applies a
  lighting's values). No fog, no moon, no clouds, no cloud shadows: WoW draws its own sky, air and weather, and BG3
  lights only its own characters and effects. The sun lights by sky scattering (SunIntensity 0, ScatteringSunIntensity
  the strength, set at run time with the colour and the direction); the sky's own light is the ambient. Shadows: four
  cascades to 45 m.
- Atmosphere (ATM_WOWBG3_Neutral): no grading (contrast and saturation 1, 6500 K, no LUT, no vignette, no lens
  flare), bloom as BG3 has it, the eye kept between EV 11.5 and 12.5, BG3's own rim-light setup for characters
  (named by its id), no environment effects.

A node is (name, [(attribute, type, value)...], [children...]).
"""
import sys

LIGHTINGS = (('a7d3c1e9-5f2b-4c8e-9d16-3b0e7f4a2c58', 'LTN_WOWBG3_WoWSun'), ('c4f81b27-3a6d-4e09-8b52-7d1e9a0c6f34', 'LTN_WOWBG3_WoWSun_B'))

LIGHTING = \
('Resource', [
    ('ID', 'FixedString', '@ID@'), ('Name', 'LSString', '@NAME@'),
], [
    ('Lighting', [
        ('TimelineFogOverride', 'bool', 'False'),
    ], [
        ('Fog', [
            ('Phase', 'float', '0.5'), ('RenderDistance', 'float', '1000'),
        ], [
            ('FogLayer0', [
                ('Albedo', 'fvec3', '1 1 1'), ('Density0', 'float', '0'), ('Density1', 'float', '0'),
                ('Enabled', 'bool', 'False'), ('Height0', 'float', '0'), ('Height1', 'float', '100'),
                ('NoiseCoverage', 'float', '0'), ('NoiseFrequency', 'fvec3', '0.01 0.01 0.01'),
                ('NoiseRotation', 'fvec3', '0 0 0'), ('NoiseWind', 'fvec3', '0 0 0'),
            ], []),
            ('FogLayer1', [
                ('Albedo', 'fvec3', '1 1 1'), ('Density0', 'float', '0'), ('Density1', 'float', '0'),
                ('Enabled', 'bool', 'False'), ('Height0', 'float', '0'), ('Height1', 'float', '100'),
                ('NoiseCoverage', 'float', '0'), ('NoiseFrequency', 'fvec3', '0.01 0.01 0.01'),
                ('NoiseRotation', 'fvec3', '0 0 0'), ('NoiseWind', 'fvec3', '0 0 0'),
            ], []),
        ]),
        ('Moon', [
            ('AlbedoTexResourceGUID', 'FixedString', ''), ('CastLightEnabled', 'bool', 'False'),
            ('Color', 'fvec3', '1 1 1'), ('Distance', 'float', '38440'), ('Earthshine', 'float', '0'),
            ('Enabled', 'bool', 'False'), ('Intensity', 'float', '0'), ('Kelvin', 'float', '6500'),
            ('MoonGlare', 'float', '0'), ('NormalTexResourceGUID', 'FixedString', ''), ('Pitch', 'float', '0'),
            ('Radius', 'float', '1737.4'), ('TearsAlbedoTexResourceGUID', 'FixedString', ''),
            ('TearsNormalTexResourceGUID', 'FixedString', ''), ('TearsRotate', 'float', '0'),
            ('TearsScale', 'float', '1'), ('UseTemperature', 'bool', 'False'), ('Yaw', 'float', '0'),
        ], []),
        ('SSAOSettings', [
            ('Bias', 'float', '0'), ('DirectLightInfluence', 'float', '0.5'), ('Enabled', 'bool', 'True'),
            ('Intensity', 'float', '1.25'), ('Radius', 'float', '1'),
        ], []),
        ('SkyLight', [
            ('CirrusCloudsAmount', 'float', '0'), ('CirrusCloudsColor', 'fvec3', '1 1 1'),
            ('CirrusCloudsEnabled', 'bool', 'False'), ('CirrusCloudsIntensity', 'float', '0'),
            ('Color', 'fvec3', '0.5294118 0.7921569 0.9411765'), ('Intensity', 'float', '2000'),
            ('Kelvin', 'float', '6500'), ('PhysicalModel', 'int32', '2'), ('ProcStarsAmount', 'float', '0'),
            ('ProcStarsEnabled', 'bool', 'False'), ('ProcStarsIntensity', 'float', '0'),
            ('ProcStarsSaturation', 'fvec2', '0 0'), ('ProcStarsShimmer', 'float', '0'),
            ('RotateSkydomeEnabled', 'bool', 'True'), ('ScatteringEnabled', 'bool', 'True'),
            ('ScatteringIntensity', 'float', '6000'), ('ScatteringSunColor', 'fvec3', '1 1 1'),
            ('ScatteringSunIntensity', 'float', '100000'), ('SkydomeEnabled', 'bool', 'False'),
            ('SkydomeTex', 'FixedString', '00000000-0000-0000-0000-000000000000'),
            ('UseTemperature', 'bool', 'False'),
        ], []),
        ('Sun', [
            ('CascadeCount', 'uint8', '4'), ('CascadeSpread', 'float', '0.86'), ('Kelvin', 'float', '6500'),
            ('LightDistance', 'float', '200'), ('LightSize', 'float', '0.53'),
            ('LocalCoverageEnabled', 'bool', 'False'), ('LocalCoverageScalar', 'float', '0'),
            ('Pitch', 'float', '140'), ('ScatteringIntensityScale', 'float', '2'), ('ShadowBias', 'float', '0.03'),
            ('ShadowEnabled', 'bool', 'True'), ('ShadowFade', 'float', '0.1'), ('ShadowFarPlane', 'float', '45'),
            ('ShadowNearPlane', 'float', '0.1'), ('ShadowObscurity', 'uint8', '1'), ('SunColor', 'fvec3', '1 1 1'),
            ('SunIntensity', 'float', '0'), ('SunlightObscurity', 'uint8', '0'), ('UseTemperature', 'bool', 'False'),
            ('Yaw', 'float', '240'),
        ], [
            ('CoverageSettings', [
                ('EndHeight', 'float', '4000'), ('HorizonDistance', 'float', '150'), ('Offset', 'fvec2', '0 0'),
                ('StartHeight', 'float', '50'), ('TexResourceGUID', 'FixedString', ''),
            ], []),
        ]),
        ('TimelineFog', [
            ('Phase', 'float', '0.5'), ('RenderDistance', 'float', '1000'),
        ], [
            ('FogLayer0', [
                ('Albedo', 'fvec3', '1 1 1'), ('Density0', 'float', '0'), ('Density1', 'float', '0'),
                ('Enabled', 'bool', 'False'), ('Height0', 'float', '0'), ('Height1', 'float', '100'),
                ('NoiseCoverage', 'float', '0'), ('NoiseFrequency', 'fvec3', '0.01 0.01 0.01'),
                ('NoiseRotation', 'fvec3', '0 0 0'), ('NoiseWind', 'fvec3', '0 0 0'),
            ], []),
            ('FogLayer1', [
                ('Albedo', 'fvec3', '1 1 1'), ('Density0', 'float', '0'), ('Density1', 'float', '0'),
                ('Enabled', 'bool', 'False'), ('Height0', 'float', '0'), ('Height1', 'float', '100'),
                ('NoiseCoverage', 'float', '0'), ('NoiseFrequency', 'fvec3', '0.01 0.01 0.01'),
                ('NoiseRotation', 'fvec3', '0 0 0'), ('NoiseWind', 'fvec3', '0 0 0'),
            ], []),
        ]),
        ('VolumetricCloudsSettings', [
            ('AmbientLightFactor', 'float', '0'), ('BackwardScatteringG', 'float', '-0.5'),
            ('BaseColor', 'fvec3', '1 1 1'), ('BottomFade', 'float', '0.20000002'), ('ConeRadius', 'float', '0.3'),
            ('CoverageEndDistance', 'float', '1'), ('CoverageStartDistance', 'float', '1'),
            ('CoverageWindSpeed', 'float', '0'), ('DarkOutline', 'float', '0.3'), ('Density', 'float', '0'),
            ('DetailScale', 'float', '8'), ('Distortion', 'float', '0.45000002'),
            ('DistortionScale', 'float', '0.6'), ('Enabled', 'bool', 'False'), ('ErosionEdgeSize', 'float', '0.6'),
            ('ForwardScatteringG', 'float', '0.69'), ('HorizonFade', 'float', '0'), ('Intensity', 'float', '0'),
            ('LODDistance', 'float', '0.3'), ('RainCoverageMaxInfluence', 'float', '3'),
            ('SampleScalar', 'float', '0.79999995'), ('SampleThreshold', 'float', '0.01'),
            ('ShadowFactor', 'float', '0'), ('SunLightFactor', 'float', '0'),
            ('SunRayLength', 'float', '1.4901161E-09'), ('TopColor', 'fvec3', '1 1 1'),
        ], [
            ('CoverageSettings', [
                ('EndHeight', 'float', '3297.297'), ('HorizonDistance', 'float', '34000'),
                ('Offset', 'fvec2', '0 0'), ('StartHeight', 'float', '1100'), ('TexResourceGUID', 'FixedString', ''),
            ], []),
        ]),
    ]),
])

ATMOSPHERE = \
('Resource', [
    ('ID', 'FixedString', '5e2c8a71-4b3d-4f19-9c6e-2a7d1f80b4c5'), ('Name', 'LSString', 'ATM_WOWBG3_Neutral'),
], [
    ('Atmosphere', [
        ('CharacterLightSetup', 'guid', 'fbccfd23-60a5-4394-89e2-070d91387a10'),
        ('ClothMainWindFrequency', 'float', '3'), ('ClothMainWindSpeed', 'float', '3'),
        ('ClothMaxWindDirectionOffset', 'float', '45'), ('ClothWindDirectionOffsetFrequency', 'float', '1'),
        ('ClothWindSpeed', 'float', '2'), ('ClothWindVariance', 'float', '0'),
        ('EnvironmentEffect', 'FixedString', ''), ('EnvironmentEffect1', 'FixedString', ''),
        ('EnvironmentEffect2', 'FixedString', ''), ('EnvironmentEffect3', 'FixedString', ''),
        ('EnvironmentEffectEnabled', 'bool', 'False'), ('EnvironmentEffectEnabled1', 'bool', 'False'),
        ('EnvironmentEffectEnabled2', 'bool', 'False'), ('EnvironmentEffectEnabled3', 'bool', 'False'),
        ('EnvironmentEffectEnabledForTimeline', 'bool', 'True'),
        ('EnvironmentEffectEnabledForTimeline1', 'bool', 'True'),
        ('EnvironmentEffectEnabledForTimeline2', 'bool', 'True'),
        ('EnvironmentEffectEnabledForTimeline3', 'bool', 'True'), ('EnvironmentEffectGlobalEnabled', 'bool', 'True'),
        ('EnvironmentEffectOffset', 'float', '0'), ('EnvironmentEffectOffset1', 'float', '0'),
        ('EnvironmentEffectOffset2', 'float', '0'), ('EnvironmentEffectOffset3', 'float', '0'),
        ('InheritanceFlags', 'uint32', '0'), ('LocalLightSourceColor', 'fvec3', '1 1 1'),
        ('LocalLightSourceEnabled', 'bool', 'True'), ('LocalLightSourceIntensity', 'float', '0'),
        ('LocalLightSourceOverrideSettings', 'bool', 'True'),
        ('TimelineAutomaticLightingDefaultSetup', 'guid', '00000000-0000-0000-0000-000000000000'),
        ('TimelineAutomaticLightingDisableFlip', 'bool', 'False'), ('WindDirection', 'float', '27'),
        ('WindSpeed', 'float', '2'),
    ], [
        ('Camera', [
            ('FarPlane', 'float', '20000'), ('NearPlane', 'float', '0.1'),
        ], []),
        ('PostProcessing', [
            ('Amount', 'float', '0.5'), ('Bloom', 'bool', 'True'), ('DOFAperature', 'float', '13.11'),
            ('DOFFocalDistance', 'float', '3'), ('DOFNearOnly', 'bool', 'True'), ('GodRays', 'bool', 'True'),
            ('GodRaysPower', 'float', '1'), ('GodRaysRayIntensity', 'float', '0.01'),
            ('GodRaysThreshold', 'float', '0.2'), ('GradingLut', 'FixedString', ''),
            ('GradingLutEnabled', 'bool', 'False'), ('LensFlareChromaticDistortion', 'float', '0.89'),
            ('LensFlareEnabled', 'bool', 'False'), ('LensFlareGhostDispersal', 'float', '0.12'),
            ('LensFlareHaloWidth', 'float', '0.69'), ('LensFlareIntensity', 'float', '0.01'),
            ('LensFlareTreshold', 'float', '4.67'), ('Threshold', 'float', '4'),
            ('ToneMapHighlightsFixAmount', 'float', '0.6'), ('Vignette', 'bool', 'False'),
            ('VignetteColor', 'fvec3', '0 0 0'), ('VignetteIntensity', 'float', '0.36'),
            ('VignettePower', 'float', '7.43'), ('WhiteBalanceTemperature', 'float', '6500'),
            ('WhiteBalanceTint', 'float', '0'),
        ], [
            ('ExposureSettings', [
            ], [
                ('Object', [
                    ('Exposure', 'bool', 'True'), ('ExposureCompensation', 'float', '0.96'),
                    ('ExposureMax', 'float', '12.5'), ('ExposureMaxPercentile', 'float', '0.95'),
                    ('ExposureMeteringMode', 'int32', '0'), ('ExposureMin', 'float', '11.5'),
                    ('ExposureMinPercentile', 'float', '0.05'),
                ], []),
            ]),
            ('ExposureSettings', [
            ], [
                ('Object', [
                    ('Exposure', 'bool', 'True'), ('ExposureCompensation', 'float', '-0.5'),
                    ('ExposureMax', 'float', '12'), ('ExposureMaxPercentile', 'float', '0.95'),
                    ('ExposureMeteringMode', 'int32', '0'), ('ExposureMin', 'float', '10.5'),
                    ('ExposureMinPercentile', 'float', '0.05'),
                ], []),
            ]),
            ('NewColorCorrection', [
                ('Contrast', 'float', '1'), ('Highlights', 'float', '0'), ('Hue', 'float', '0'),
                ('LutApplyMode', 'int32', '1'), ('LutBlendFactors', 'fvec2', '1 0.5'), ('Pivot', 'float', '0'),
                ('Saturation', 'float', '1'), ('Shadows', 'float', '0'),
            ], [
                ('DarkRange', [
                    ('ExposureShift', 'float', '0'), ('Saturation', 'float', '1'), ('XYOffset', 'fvec2', '0 0'),
                ], []),
                ('GlobalRange', [
                    ('ExposureShift', 'float', '0'), ('Saturation', 'float', '1'), ('XYOffset', 'fvec2', '0 0'),
                ], []),
                ('HighlightRange', [
                    ('ExposureShift', 'float', '0'), ('Saturation', 'float', '1'), ('XYOffset', 'fvec2', '0 0'),
                ], []),
                ('MidtonesRange', [
                    ('ExposureShift', 'float', '0'), ('Saturation', 'float', '1'), ('XYOffset', 'fvec2', '0 0'),
                ], []),
                ('ShadowRange', [
                    ('ExposureShift', 'float', '0'), ('Saturation', 'float', '1'), ('XYOffset', 'fvec2', '0 0'),
                ], []),
                ('SpecularRange', [
                    ('ExposureShift', 'float', '0'), ('Saturation', 'float', '1'), ('XYOffset', 'fvec2', '0 0'),
                ], []),
            ]),
        ]),
    ]),
    ('Labels', [
        ('Object', 'FixedString', 'WOWBG3'),
    ], []),
])


def esc(v):
    return v.replace('&', '&amp;').replace('"', '&quot;').replace('<', '&lt;')


def node_xml(n, depth, subst):
    pad = '\t' * depth
    name, attrs, kids = n
    out = pad + '<node id="%s">\n' % name
    for a, t, v in attrs:
        out += pad + '\t<attribute id="%s" type="%s" value="%s" />\n' % (a, t, esc(subst.get(v, v)))
    if kids:
        out += pad + '\t<children>\n' + ''.join(node_xml(k, depth + 2, subst) for k in kids) + pad + '\t</children>\n'
    return out + pad + '</node>\n'


def bank_lsx():
    """The resource bank file (Public/<mod>/Content/Assets/Atmospheres/[PAK]_Atmospheres/_merged) as LSX text."""
    def region(name, resources):
        return ('\t<region id="%s">\n\t\t<node id="%s">\n\t\t\t<children>\n' % (name, name) + ''.join(resources)
                + '\t\t\t</children>\n\t\t</node>\n\t</region>\n')
    return ('<?xml version="1.0" encoding="utf-8"?>\n<save>\n'
            '\t<version major="4" minor="0" revision="9" build="0" lslib_meta="v1,bswap_guids,lsf_adjacency" />\n'
            + region('AtmosphereBank', [node_xml(ATMOSPHERE, 4, {})])
            + region('LightingBank', [node_xml(LIGHTING, 4, {'@ID@': i, '@NAME@': n}) for i, n in LIGHTINGS])
            + '</save>\n')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    open(sys.argv[1], 'w', encoding='utf-8').write(bank_lsx())
