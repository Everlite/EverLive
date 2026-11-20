# FidelityFX Optical Flow — vendored sources

Copied unmodified from the AMD FidelityFX SDK, tag `v1.1.4`, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` (2025-05-08), MIT licence (`LICENSE.txt`).

| Here | There |
|---|---|
| `ffx_core*.h`, `ffx_common_types.h` | `sdk/include/FidelityFX/gpu/` |
| `opticalflow/*.h` | `sdk/include/FidelityFX/gpu/opticalflow/` (HLSL callbacks and the CMake fragment left out) |
| `spd/ffx_spd.h` | `sdk/include/FidelityFX/gpu/spd/` |
| `passes/*.glsl` | `sdk/src/backends/vk/shaders/opticalflow/` |

Technique version: FidelityFX Optical Flow 1.1.2 (`FFX_OPTICALFLOW_VERSION_*` in the SDK's host header).
Only the colour buffer goes in; block motion vectors (8x8, `rg16i`, `prev = cur + v`) and a scene
change detector come out. The host-side sequencing of the seven passes is ported to C in
`src/framegen.c`.

Local edits, every line of them marked `afmf-linux:`:

- `opticalflow/ffx_opticalflow_compute_optical_flow_v5.h`: each block's SAD at the vector
  predicted by the coarser level (zero at the coarsest) and at rest are taken before the search.
  A block whose SAD at rest is at or under the specialization constant `afmfStaticBlockSad`
  (`AFMF_STATIC_BLOCK_SAD`, 0 = the SDK's behaviour) did not move: it stores the zero vector and
  skips the search. A block whose predicted vector is at or under it and at least twice as good
  as rest keeps that vector and skips the search; without the second test a textureless block
  keeps whatever the coarser level handed it, since there every candidate matches equally well.
  The search's winner is kept only when it too is at least twice as good as rest, at every level
  (the SDK dropped it at level 0 alone, and only when rest matched at least as well), so a block
  with no detail to match stores the zero vector instead of noise; the SDK's level-0 zero-vector
  fallback only computes its sum at level 0. The cross-subgroup sum and minimum combine
  `gl_NumSubgroups` partials through shared memory instead of exactly two of 32 lanes, so the
  result is uniform across the group on every subgroup size (ANV picks 8 or 16).
- `opticalflow/ffx_opticalflow_prepare_luma.h`: the HDR luma is clamped to [0, 1] before it
  is stored as a byte, and the perceived-luminance cube root never sees a negative input
  (scRGB carries components outside the Rec.709 gamut). The SDK stored `fY * 255` as is, so a
  pixel brighter than the peak wrapped to a dark value.
- `opticalflow/ffx_opticalflow_callbacks_glsl.h`: `StoreOpticalFlow` drops a write outside the
  flow image. The search dispatches one group per 2x2 block neighbourhood, so a level whose size
  is not a multiple of 16 has a trailing row or column of blocks that do not exist.
- `opticalflow/ffx_opticalflow_compute_scd_divergence.h`: the last six steps of the two
  shared-memory reductions have a group barrier each; the SDK ran them without one, which only
  holds when the first 32 lanes are a single subgroup.
- `opticalflow/ffx_opticalflow_common.h`, `..._v5.h` and
  `passes/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl`: with `AFMF_SAD_INT16=1`
  (a second build of the search pass, used on devices with `shaderInt16`) the SAD runs on packed
  16-bit byte pairs instead of one byte at a time. Without the define the SDK's code compiles
  unchanged.

Everything else is as vendored.
