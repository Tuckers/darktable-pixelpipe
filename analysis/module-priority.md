# IOP Module Priority Classification

## Overview

Classification of darktable's 92 IOP modules into priority tiers for the pixelpipe extraction project.

---

## Tier 1 - Essential (Pipeline Core)

These modules are **required** for basic RAW → RGB conversion. The pipeline cannot function without them.

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `rawprepare` | RAW data preparation, black/white point | Yes | First in pipeline |
| `temperature` | White balance | Yes | Critical for color accuracy |
| `highlights` | Highlight reconstruction | Yes | Handles clipped highlights |
| `demosaic` | Bayer pattern interpolation | Yes | Core RAW → RGB conversion |
| `colorin` | Input color profile (camera → working space) | Yes | Color management |
| `colorout` | Output color profile (working → display/export) | Yes | Color management |
| `gamma` | Final gamma encoding | No | Output encoding |
| `finalscale` | Final output scaling | Yes | Helper module |

**Total: 8 modules**

---

## Tier 2 - Core Creative (Essential Editing)

The 20 most-used editing modules for a professional workflow. These cover exposure, tone mapping, color grading, and essential corrections.

### Exposure & Tone

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `exposure` | Exposure adjustment, black point | Yes | Most-used module |
| `filmicrgb` | Scene-referred tone mapping | Yes | Modern workflow standard |
| `sigmoid` | Alternative tone mapping | Yes | Simpler than filmic |
| `basecurve` | Display-referred tone curve | Yes | Legacy but still used |
| `tonecurve` | RGB/Lab tone curves | Yes | Classic adjustment |
| `levels` | Black/white/gamma levels | Yes | Quick adjustments |
| `rgblevels` | Per-channel levels | Yes | Color correction |

### Color

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `colorbalancergb` | Modern color grading (lift/gamma/gain) | Yes | Primary color tool |
| `channelmixerrgb` | Channel mixing, color calibration | Yes | Complex but powerful |
| `colorzones` | Hue/saturation/lightness by zone | Yes | Selective color |
| `vibrance` | Intelligent saturation boost | Yes | Simple enhancement |
| `velvia` | Film-like saturation | Yes | Creative effect |

### Detail & Sharpening

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `sharpen` | Unsharp mask sharpening | Yes | Essential |
| `diffuse` | Diffuse/sharpen (wavelets) | Yes | Advanced sharpening |
| `denoiseprofile` | Profiled noise reduction | Yes | Primary denoiser |
| `nlmeans` | Non-local means denoising | Yes | Alternative denoiser |
| `bilat` | Local contrast (bilateral) | Yes | Clarity/local contrast |

### Geometry

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `crop` | Crop and aspect ratio | Yes | Essential framing |
| `flip` | Orientation correction | Yes | Simple rotation |
| `clipping` | Crop with rotation/keystone | Yes | More advanced cropping |

**Total: 20 modules**

---

## Tier 3 - Specialized (Nice to Have)

Useful for specific workflows but not essential for MVP.

### Color & Tone

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `colorchecker` | Color chart calibration | Yes | Studio work |
| `colorequal` | Color equalizer | Yes | Advanced color |
| `lut3d` | 3D LUT application | Yes | Film emulation |
| `colorbalance` | Legacy color balance | Yes | Older version |
| `channelmixer` | Legacy channel mixer | Yes | Older version |
| `colorcorrection` | Shadows/highlights split toning | Yes | Creative |
| `splittoning` | Split toning effect | Yes | Creative |
| `monochrome` | B&W conversion | Yes | B&W workflow |
| `colormapping` | Color mapping between images | Yes | Specialized |
| `negadoctor` | Negative film inversion | Yes | Film scanning |
| `invert` | Simple inversion | Yes | Film scanning |

### Tone & Contrast

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `toneequal` | Tone equalizer (zone system) | No | Advanced tonal |
| `shadhi` | Shadows and highlights | Yes | Useful but dated |
| `atrous` | Wavelets (contrast/denoise) | Yes | Older approach |
| `globaltonemap` | HDR tone mapping | Yes | HDR workflow |
| `lowlight` | Low light vision simulation | Yes | Specialized |
| `zonesystem` | Zone system adjustment | Yes | B&W workflow |
| `rgbcurve` | RGB curves | Yes | Alternative to tonecurve |

### Detail & Correction

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `cacorrect` | Chromatic aberration (auto) | No | Lens correction |
| `cacorrectrgb` | Chromatic aberration (manual) | No | Lens correction |
| `defringe` | Purple fringing removal | No | Lens artifacts |
| `hotpixels` | Hot pixel removal | No | Sensor defects |
| `rawdenoise` | RAW-level denoising | No | Alternative denoiser |
| `hazeremoval` | Dehaze | Yes | Atmospheric correction |
| `highpass` | High-pass filter | Yes | Texture enhancement |
| `lowpass` | Gaussian blur | Yes | Smoothing |
| `equalizer` | Wavelets equalizer | No | Legacy |

### Geometry & Transform

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `ashift` | Perspective correction | Yes | Architectural |
| `liquify` | Warp/liquify | Yes | Retouching |
| `rotatepixels` | Sub-pixel rotation | No | FUJI X-Trans |
| `scalepixels` | Non-square pixel correction | No | Specialized |
| `retouch` | Clone/heal/fill | Yes | Retouching |
| `spots` | Spot removal (legacy) | No | Simple retouching |

### Effects & Output

| Module | Purpose | OpenCL | Notes |
|--------|---------|--------|-------|
| `vignette` | Vignette effect | Yes | Creative |
| `graduatednd` | Graduated ND filter | Yes | Landscape |
| `borders` | Add borders | Yes | Presentation |
| `grain` | Film grain simulation | No | Creative |
| `bloom` | Bloom/glow effect | Yes | Creative |
| `soften` | Orton/soft focus | Yes | Creative |
| `blurs` | Various blur effects | Yes | Creative |
| `colorize` | Solid color overlay | Yes | Creative |
| `watermark` | Watermark overlay | No | Export |

**Total: ~40 modules**

---

## Tier 4 - Skip (Low Priority)

Rarely used, deprecated, or very specialized modules. Skip for MVP.

| Module | Purpose | Notes |
|--------|---------|-------|
| `agx` | AgX tone mapping | Experimental |
| `basicadj` | Basic adjustments | Redundant with other modules |
| `clahe` | CLAHE contrast | Limited use |
| `colisa` | Contrast/lightness/saturation | Superseded |
| `colorcontrast` | A/B color contrast | Niche |
| `colorreconstruction` | Highlight color reconstruction | Niche |
| `colortransfer` | Color transfer between images | Niche |
| `dither` | Output dithering | Export-specific |
| `enlargecanvas` | Enlarge canvas | Niche |
| `filmic` | Original filmic (v1-v4) | Superseded by filmicrgb |
| `overexposed` | Overexposure indicator | Preview helper |
| `rawoverexposed` | RAW overexposure indicator | Preview helper |
| `overlay` | Image overlay | Specialized |
| `primaries` | Custom color primaries | Expert only |
| `profile_gamma` | Linear/log gamma | Legacy |
| `rasterfile` | Overlay raster file | Specialized |
| `relight` | Fill light (deprecated) | Superseded |
| `censorize` | Pixelate/blur regions | Novelty |
| `useless` | Test module | Developer only |
| `mask_manager` | Mask management | GUI helper |

### Helper Files (Not Modules)

| File | Purpose |
|------|---------|
| `ashift_lsd.c` | Line segment detector for ashift |
| `ashift_nmsimplex.c` | Optimization for ashift |

**Total: ~20 modules to skip**

---

## Implementation Order

### Phase 1: Minimal Pipeline
1. rawprepare
2. demosaic
3. colorin
4. exposure
5. colorout
6. gamma

### Phase 2: Basic Workflow
7. temperature (white balance)
8. highlights
9. filmicrgb or sigmoid
10. crop/flip

### Phase 3: Color Grading
11. colorbalancergb
12. vibrance
13. levels/rgblevels

### Phase 4: Detail
14. sharpen
15. denoiseprofile
16. bilat

### Phase 5+: Additional Modules
Continue adding Tier 2 and Tier 3 modules based on user demand.

---

## Summary

| Tier | Count | Priority |
|------|-------|----------|
| Tier 1 - Essential | 8 | Must have |
| Tier 2 - Core Creative | 20 | Should have |
| Tier 3 - Specialized | ~40 | Nice to have |
| Tier 4 - Skip | ~20 | Skip for MVP |
| Helper files | 4 | Dependencies only |
| **Total** | 92 | |
