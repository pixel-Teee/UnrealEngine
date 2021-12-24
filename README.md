rtx-dlss-4.27
=============
*A branch for practical optimizations and enhancements for ray tracing features in UE4.*

This branch captures enhancements to the DXR support found in UE 4.27. It attempts to demonstrate tweaks that might be desirable when looking at real-time performance of ray traced effects. While we work closely with Epic, we offer no guarantees that any of these will be integrated into core UE4. Most of these changes strive to maintain identical quality with the core release, but some offer additional compromises in image quality settings. Everything that produces a compromise is an optional feature that can be disabled. Below is a catalog of many of the offerings available. All optimizations were tested against content that has been made publicly available in the UE4 Marketplace. Typically, the testing was done by forcing ray tracing on for samples that were not originally built for ray tracing, so it should apply to problems commonly seen in today’s content. As optimizations or equivalent solution are adopted into mainline UE4, they are dropped from the RTX branches, so if an item present in RTX-4.26 is missing here, it is likely already be present in 4.27 core.

Direct Optimizations
====================

This class of optimizations faithfully implement the algorithms exactly as found in UE 4.27, but they apply transforms which can allow them to operate more efficiently. Generally, this class of optimizations are simply on by default as they have no impact on quality. Some may have configuration parameters, to control concerns like memory overhead.

### Low bounce count reflections specialization

Specialize the reflections shader with a compile-time constant number of bounces and loop unrolling. This enables better code generation and scheduling for the common case encountered in games. Presently, only the single bounce case is specialized. The measured gain for reflection costs on the RealisticRendering showcase sample is roughly 10%.

Commits:
  * 1df96f85285ddb5acd56fcbd30a0051191026441
  * 11098a483fa7725c029fea591d43c7ae4316eb14

### Translucency masking

This capability rasterizes primitives that wish to participate in ray traced translucency into the stencil buffer. By marking these locations, the ray generation shader knows where it can skip shooting rays. This frequently improves translucency costs by 30%, but obviously the results are very content dependent. It works with both normal ray traced translucency and hybrid translucency. This is controlled by the following CVar.

  r.RayTracing.Translucency.Mask [0/1]

Commits:
  * 62ea7be82f23ba55d0a33f63380223417020e7b8
  * f7880de8cefba358e2dee8d86f4ba8c0cecabf8e

### Optimized Instanced Static Mesh Culling

This optimization addresses efficiency tied to the handling of instanced static mesh (ISM) objects including foliage. In scenes with heavy ISM loads, it can substantially reduce CPU load.

Commit:
  * 05704a30341c8a53517922c64faba17d1744a1ba

### Auto instancing for ray traced static

Reduces the cost of SBT setup by identifying duplicate static mesh instances, and collapsing them into real instances rather than replicated nodes in the BVH. This is functionality similar to the raster instancing support.

  r.RayTracing.AutoInstance [0/1]
  
Commit:
  * 972befa7c4b24ec532f3c65d2834d236377152a4
  
### Solid angle culling for instanced static meshes 

Uses the projected solid angle of the bounding sphere to cull instances for instanced static meshes such as foliage. This produces a much more desirable result, as the culling is based on apparent size to the viewer as opposed to a distance.

  r.RayTracing.Geometry.InstancedStaticMeshes.CullAngle <float> - solid angle at which to cull in degrees, negative values mean to revert to dinstance culling

Commit:
  * 7e3eb27bff2081ac910354196e145e4c895ff4f7

Quality Tradeoffs
=================

These provide options to improve performance while making small compromises to image quality. They are similar in nature to many of the quality features already available in the engine. Most of these are configured to be on by default, but generally with a low threshold where the difference is hard identify for most content.

### Shadow Ray Scissoring and Denoising

This provides the option to increase performance by scissoring of the shadow rays to the screen space light scissor rectangle. This interacts with the shadow denoiser which might require access to data outside of the scissor rectangle. The default denoiser however also scissors the denoising to the screen space light rectangles, suggesting a low chance of artifacts. The following CVar allows to configure this.

  r.RayTracing.Shadow.Scissor [0/1/2]

Commits:
  * b7319c04434264c0a41d0a029f621932259e46e1

### Shadow Light Prioritization

This change adds a pair of CVars (r.RayTracing.Shadow.MaxLights and r.RayTracing.Shadow.MaxDenoisedLights) which allow the user to clamp the number of lights receiving ray traced shadows and the number being denoised. Setting either to -1 (the default) means that there is no maximum, preserving the original engine behavior. The MaxLights value is a soft cap on the number of ray traced shadowing lights, as it will not deny lights that need dynamic shadows as those are expected to be no worse than shadow maps. The clamp on the number of lights is applied after they undergo a priority sort to try to enforce the discarded lights to be the ones that are smaller on the screen or further away from the viewer. It is important to note that the lights still have any static shadowing, that they'd normally have under rasterization. These controls can help maintain performance with content where the casts ray traced shadow property has not been well curated on lights.

Commits:
  * b7319c04434264c0a41d0a029f621932259e46e1

#### Sharp Shadow Fallback

With this CVar enabled, shadows for area lights using ray tracing will fallback to sharp shadows rather than noisy shadows. Depending on the scene one or the other fallback may be more or less visually distracting.

  r.RayTracing.Shadow.FallbackToSharp [0/1]

Commits:
  * b7319c04434264c0a41d0a029f621932259e46e1

### Light Prioritization

This change selects lights for the ray traced light list based on a priority metric. Without this support, the first MAX_LIGHTS (256 in the current build) lights are selected to be used in ray traced effects with no provisions for whether they have any importance. This change attempts to select lights that are closer to the viewer and brighter among other criteria in an effort to select useful lights. Selecting a smaller set of relatively important lights can offer improved performance with equal or better visual quality than simply selecting the first 256 lights in the scene's list.

  r.RayTracing.Lighting.MaxLights - maximum number of lights to use for ray traced effects
    -1 - engine maximum and apply no priority (256 is engine max)
    <N> - allow N lights and select based on the priority heuristic
    default = 256 - select engine maximum number of lights via the priority heuristic

  r.RayTracing.Lighting.MaxShadowLights - maximum number of lights to cast shadows in ray traced effects (Lights not casting shadows are those ranked with a lower priority)
    <N> - allow N lights and select based on the priority heuristic
    default = 256 - select engine maximum number of lights via the priority heuristic

Additionally, the ranking heuristic can be tweaked by the following variables. They have been set to values known to work well for game content, but as it is a heuristic, nothing is completely fool-proof.

  r.RayTracing.Lighting.Priority.FrustumBoost - Prioritization boost given for RT lights touching frustum camera (0..inf)
    default = 0.5
  r.RayTracing.Lighting.Priority.AheadBoost - Prioritization boost given for RT light origins in cone ahead of camera (0..inf)
	  default = 1.0f
  r.RayTracing.Lighting.Priority.BehindBoost - Prioritization boost given for RT light origins in cone behind camera (0..inf)
	  default = 1.0f
  r.RayTracing.Lighting.Priority.DistPow - Exponent of light prioritization distance-weight damping
	  default = 2.0f (falloff with square of distance)
  r.RayTracing.Lighting.Priority.LumPow - Exponent of light prioritization luminance-weight damping
    default = 0.5f

Commits:
  * 2c7c9abf57401f7b94bcf301771f0aeb6367b06e

### Roughness Multiplier for Reflections

This enhancement allows trading smoother reflections for reduced reflection noise and improved GPU performance without making changes to the materials themselves.

During raytracing, once the roughness falls within the ray tracing threshold, the roughness can be multiplied by constant, e.g. zero. This results in smooth reflections, but also reduces the reflection noise and improves ray coherency and GPU performance. The surface area of the object which is reflected by the means of ray tracing remains the same.
The main strength of this approach is that it eliminates the need to adjust all materials to achieve a similar effect, thus saving content authoring effort.

The folling cvars can be used to tune this enhancement. They default to 1.0, i.e. regular behavior
  * r.RayTracing.Translucency.RoughnessMultiplier [0.0 ... 1.0]
  * r.RayTracing.Reflections.RoughnessMultiplier [0.0 ... 1.0]

Commits:
  * ceab0eb78ec8e8795a4132108cee3937b4e4584e

Enhanced Features
=================

These are features that are strictly enhancements to the base UE 4.27 release. They add new capabilities that may make your project better.

### Hybrid Translucency

This enhancement provides a feature that allows the mixing of raster and ray traced translucency. Today, ray traced translucency forces all translucency to be rendered via ray tracing. This will cause unsupported primitive types like Cascade particles to disappear. Further, the refraction behaviors often can interact in non-intuitive ways for content authored for rasterization. Hybrid translucency traces a number of layers to an off-screen surface, then composites the ray traced layers as part of normal raster translucency. It loses the OIT support and refraction of fully ray traced translucency, but it is no worse than raster in these areas while delivering the reflections and shading of ray traced translucency. This functionality requires enabling the hybrid rendering property for your project, as it permutes the base translucency shaders to enable the capability.

CVars controlling this functionality:
  * r.RayTracing.HybridTranslucencySupport - controls enabling shader support for hybrid translucency (tied to render property)
  * r.RayTracing.HybridTranslucency - controls whether to use hybrid translucency instead of regular ray tracing translucency (r.RayTracing.Translucency still controls whether translucency is ray traced at all)
  * r.RayTracing.HybridTranslucency.Layers - controls how many levels of overlapping ray traced translucency are tracked for hybrid
  * r.RayTracing.HybridTranslucency.DepthThreshold - separation distance at which geometry is considered a different layer of translucency. Units are in world space. (If this value is too small, you may not see the hybrid translucency applied, or z-fighting like artifacts, too big and layers placed one over another will errantly merge)

Commits:
  * 62ea7be82f23ba55d0a33f63380223417020e7b8
  * f7880de8cefba358e2dee8d86f4ba8c0cecabf8e

#### Half Resolution Translucency

This enhancement on hybrid translucency allows it to be rendered at a lower resolution by tracing every other line, then performing a smart rescaling at application time.

  * r.RayTracing.HybridTranslucency.HalfRes - whether to render the hybrid translucency at half resolution
    * 0 - full resolution
    * 1 - half resolution vertically (interleaved sampling)
    * 2 - half resolution checkerboard (4 tap reconstruction)
    * 3 - half resolution checkerboard (2 tap vertical reconstruction)

Commits:
  * 62ea7be82f23ba55d0a33f63380223417020e7b8
  * f7880de8cefba358e2dee8d86f4ba8c0cecabf8e


### Light Functions

The RTX branch adds light material function support to ray traced lighting effects (reflections and translucency). Up to 16 light function lights are supported simultaneously. Light function support can be disabled by the following CVar:

  r.RayTracing.LightFunction [0/1]

Commits:
  * adcb04a42e210abec1a87cccb35e9723cf995981

### Light Channel Masking

This branch adds support for the light channel masking functionality of UE4 to ray traced lighting effects like ray traced reflections and translucency.

  r.RayTracing.LightMask [0/1]

Commits:
  * adcb04a42e210abec1a87cccb35e9723cf995981

### Per-Light Shadow Casting

This branch adds a per-light shadowing flag to ray traced lights, so that the shadow casting property of lights in reflections and translucency will match what is seen in the main viewport. It can be controlled with the following CVar.

  r.RayTracing.Lighting.ObeyShadows [0/1] whether to apply the light's shadowing property to the ray traced version of the light seen in reflections and other ray traced effects

Commits:
  * 2c7c9abf57401f7b94bcf301771f0aeb6367b06e
  
### World Position Offset support for Instanced static meshes and foliage
  
Presently, standard UE4 ray tracing is unable to evaluate animations due to world position offset in instanced static meshes like foliage. This feature adds the ability to evaluate a subset of the animations and share them among the other instances. The result is an inexact approximation, but it provides motion to the reflections and shadows while keeping the added cost to a minimum.

  r.RayTracing.InstancedStaticMeshes.EvaluateWPO [-1/0/1]
  r.RayTracing.InstancedStaticMeshes.SimulationCount [1-256]
  
Evaluation of WPO for ISMs can be off (0), on (1), or selected per instance configured through the foliage tools. (per instance setting) Additionally, the maximum number of instances to simulate per group of instances allows for additional variation and fidelity in the simulated results. Finally, this change also adds the ability to hide certain foliage instance types from ray tracing completely. For instance, small underbrush and grass might be excluded.

Commits:
 * a368e60d3b6c87d935a1a77c82306c39482b6193
 
### Inexact shadow testing
   
Some objects in raster may not match their ray traced counterparts. This arises from features such as world position offset, which isn't evaluated for all ray tracing meshes or dithered LOD transitions, where only one LOD lives in the BVH. This can obviously result in distracting self-shadowing artifacts. To help hide these mismatch artifacts, it can be useful to apply a dithered offset to where the shadow ray begins. This results in testing a cloud of points floating above the object and produces a more stochastic answer to how much of the object is receiving self shadowing. To accomplish this, the scene is marked with a stencil mask of the potentially inexact geometry locations prior to executing ray traced shadow passes.

  r.RayTracing.Shadow.UseBiasForSkipWPOEval [0/1] - whether to rasterize the mask of inexact MaxBiasForInexactGeometry
  r.RayTracing.Shadow.MaxBiasForInexactGeometry <float> - Max offset in Unreal units

Commits:
  * dfca47f6023efce9a9d0d61ed37c33bb25e6bd74
  
### Sampled Direct Lighting (RTXDI/ReSTIRS)
     (beta)
     
  [Full Readme](Docs/RTXDI/README.md) 
     
  The RTX branches have long included enhancements to help manage the cost of direct lighting with ray traced shadows. These solutions operated at a global level attempting to select the best lights for the scene. RTXDI introduces support for making the light decisions on a per-pixel granularity. The Monte Carlo estimation techniques allow a single set of shading and denoising passes over the scene instead of the standard per light passes. This allows a scene to scale to hundreds or thousands of lights with little change in performance. Please note that the algorithm is highly dependent on the NVIDIA Real-Time Denoising (NRD) denoiser plug-in (also included in this branch), but it must be enabled per-project. 
  
    r.RayTracing.SampledDirectLighting [0/1] - whether to use the sampled lighting approach of RTXDI
    r.RayTracing.SampledLighting.Preset [medium/high/ultra] - configure quality/performance settings to curated levels
    r.RayTracing.SampledLighting.Denoiser [0/2] - which denoiser to use (0 - none, 2 - NRD plug-in ReLAX denoiser )
  
  Commits:
    * 3f72c3062581ec9f4d3bbd8384189fb85087b3b4

Debugging & Visualization Features
==================================

### BVH Visualization

BVH visualization allows a user to examine the layout of the static elements in the BVH to identify regions where extreme overlap may be occurring or volumes may be poorly fitting the underlying geometry. The feature is implemented as a showflag and is available through the menu in the editor. It is important to remember that this mode is only an approximation to what the underlying hardware actually sees and processes.

Visualization modes
  * VisualizeBVHComplexity - shows all volumes from the eye to the surface
  * VisualizeBVHOverlap - shows how many volumes overlap points visible in the world

CVars to configure the visualization
  * r.RayTracing.VisualizeBVH.ColorMap - Selects a color map encoding for visualizing the data
   * 0 - simple color ramp (default)
   * 1 - Jet-like encoding
   * 2 - Turbo-like encoding
   * 3 - Viridis-like
   * 4 - Plasma-like
   * 5 - Magma-like
   * 6 - Inferno-like
   * 7 - Grayscale
  * r.RayTracing.VisualizeBVH.Encoding - controls how data is mapped onto the color map
   * 0 - linear
   * 1 - alternate logarithmic
   * 2 - logarithmic (default)
  * r.RayTracing.VisualizeBVH.RangeMin - smallest value to visualize in the range (default 0.0)
  * r.RayTracing.VisualizeBVH.Range - largest value to visualize in the range (default 32.0)

Commits:
  * e288ed42c2b13c7a3ccd90a4ecbfbec07d3f7388
  * 61c209e095f7c9a56f3b2d5dcaa37523c73dd5ff
  * 1abf4f07b22ebbced0462093ac46a4bb080cc05a

### Ray Timing Visualization

Ray timing visualization provides modes to display the costs or ray tracing for individual samples in the scene. The visualization can be enabled for most ray tracing effects allowing the user to see the total cost of rays for different objects in the scene. The simplest and most intuitive version is enabling the visualization for just the debug rays view in the editor. This will measure the time to cast the ray from the eye to the surface, and evaluate the hit shader. If shading is enabled, it'll also time the cost of evaluating the shading of that sample. As with BVH visualization, this mode is controlled via a showflag which can be accessed through the editor menu, or via the show console command. All effects with timing enabled will be summed to compute the displayed value.

Visualization Modes
  * VisualizeRayTracingTiming

CVars to configure the visualization
  * r.RayTracing.VisualizeTiming.ColorMap - Selects a color map encoding for visualizing the data
   * 0 - simple color ramp (default)
   * 1 - Jet-like encoding
   * 2 - Turbo-like encoding
   * 3 - Viridis-like
   * 4 - Plasma-like
   * 5 - Magma-like
   * 6 - Inferno-like
   * 7 - Grayscale
  * r.RayTracing.VisualizeTiming.Encoding - controls how data is mapped onto the color map
   * 0 - linear (default)
   * 1 - logarithmic
   * 2 - exponential
  * r.RayTracing.VisualizeTiming.Range - largest value to visualize in the range (default 100,000.0)

CVars to enable visualization from different passes
  * r.RayTracing.Shadows.Timing - 0/1 (default 1)
  * r.RayTracing.AmbientOcclusion.Timing - 0/1 (default 1)
  * r.RayTracing.GlobalIllumination.Timing
   * 0 - Off (default)
	 * 1 - Shaded (passes shading the samples)
	 * 2 - Material gather (passes performing material gather for sorting)
	 * 3 - Final gather (pass doing the final gather for cached samples)
	 * 4 - All passes
  * r.RayTracing.Reflections.Timing
   * 0 - off
   * 1 - shaded rays
   * 2 - material gather
   * 3 - all (default)
  * r.RayTracing.Translucency.Timing - 0/1 (default 1)
  * r.RayTracing.SkyLight.Timing - 0/1 (default 1)

Commits:
  * ce5f0c929e7b36bfde53f95966af602410df1598

Miscellaneous Features & Fixes
==============================

### Add option to default translucent materials to non-shadowing

Base UE4 marks translucent materials as shadow casting for ray tracing. This behavior does not match the behavior used in other shadowing methods. The engine has a per-material flag to change the behavior, but this enhancement allows it to be globally changed to match the default used in other shadow techniques.

  r.RayTracing.ExcludeTranslucentsFromShadows [0/1]

Commits:
  * 7c100519bbdd043879e8b7a24d9c12759e434604
  
### Option to increase heap priority for BVH allocations

Base UE4 places all allocations at normal priority, leaving the OS memory manager to decide where to place pools of memory when residency is changed. Under high memory pressure, the heaps holding BLAS nodes for ray tracing can end up in system memory. This can dramatically increase the cost of acceleration structure builds, as well as increasing the cost of traversal during ray tracing. Enabling this feature marks the heaps containing BLAS data as high priority to encourage the OS to allocate them from video memory. 

  r.D3D12.RayTracingElevateASHeapPriority [0/1]
  
Commits:
  * 94d7f051391db4535a1a3aeaa27d2af98b54d5a3

