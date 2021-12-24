![NVIDIA Logo](../Images/nv-logo.png)

RTX Direct Illumination and NVIDIA Real-Time Denoiser (ReLAX)
=============================================================
*Best Practices with Tips and Tricks*

# Summary

Have you ever struggled with managing the number of shadow casting lights in Unreal to make your
performance target? Tuning the number of lights can be a tedious task, made even harder in the
context of ray tracing where each light has an additional property of whether to cast ray-traced shadows. 

The new NVIDIA-curated RTXDI technology for sampling shadowed lights in the NvRTX branch offers a
solution that drastically reduces the cost of additional shadowed lights. RTXDI works by selecting,
on a per-pixel basis, which lights need to be evaluated, then sending the signal of all lighting
through a single denoiser pass. By having only a single lighting pass and a single denoising pass,
the cost difference between 1 light and 1000 lights is virtually non-existent. The RTXDI technology
integrated into this branch is a derivative of the RTXDI SDK. It has just been tuned and adapted to UE4.

It is important to note that the RTXDI support here should still be considered beta, as it is
currently undergoing testing against additional scenarios. You can expect it to continue to improve
in the weeks to come. Further, it is worth noting that RTXDI might not be the best solution for all
lighting scenarios. RTXDI pays a fixed amount of overhead in selecting lights to process and maintaining
temporal history. In simple lighting scenarios, like with just a single directional light, RTXDI may
be more costly than the standard rendering pipeline with ray-traced shadows. It may, however, produce
better penumbras through the use of this temporal history. RTXDI truly shines in three categories.
First, it robustly handles a very large number of area lights without the linear cost scaling seen
in standard lighting. Second, it provides consistent performance, in that lighting performance won’t
tank in certain locations. Finally, it offers controls to scale cost versus quality, so that you
can have a quality slider to trade-off performance versus noise or softness in the image. 


# Enabling and Using the RTXDI Enhancements

First, you need to have ray tracing enabled in your project and be running with the D3D12 RHI. Second,
you should enable the NRD denoising plugin included in the branch, as this provides the single-pass
denoiser that RTXDI relies on. You can run it without denoising for simple experimental purposes, but
you'll obviously want denoising for final results. (The RTXDI path will warn you if you fail to enable
the plugin)  Next, simply enable the RTXDI code by toggling the console variable
r.RayTracing.SampledDirectLighting to 1. Now, all shadowing lights will utilize the sampled lighting
system. You'll probably notice a quick flicker, as the system relies on temporal feedback.

By default, all lights casting shadows will utilize RTXDI, as the most efficient solution is to use
that single pass for everything. Different light types can be excluded from RTXDI, much like ray traced
shadows can exclude lights by type. The console variables `r.RayTracing.SampledLighting.Light.<Type>`
enable this filtering by type. It enables the user to filter out a particular light type if they don't
like the way things look. The most common case might be to exclude directional lights to exclude the sun.
The reasons one might consider such a tweak would be to balance noise in the penumbra versus some ghosting
showing up in a moving shadow. There are other settings to tune these sorts of artifacts, but this extra
option lets you deal with situations where lights are disagreeing on the best balance of settings.
Additionally, this same mechanism supports the exclusion of lights with light function materials via the
console variable `r.RayTracing.SampledLighting.Light.FunctionLights`. This might be useful if you experience
aliasing in the light function or rely heavily on the light function's impact on particle lighting. The
way that the light material function is sampled changes somewhat with RTXDI to support the sparsity of
execution.

Several quality knobs exist for RTXDI. The simplest way of tweaking the quality at present is to use the
command `r.RayTracing.SampledLighting.Preset` to apply one of the standard preset quality levels. We presently
offer "medium", "high", and "ultra" settings. These are quite similar to the settings used by the RTXDI SDK
sample, so those familiar with it can map the settings pretty much directly. Importantly, the settings
that the preset command impacts are independently configurable as well.

## Options configured through Preset

`r.RayTracing.SampledLighting.Spatial.ApplyApproxVisibility`   [0/1]

`r.RayTracing.SampledLighting.Temporal.ApplyApproxVisibility`  [0/1]

Together these control the bias correction mode for gathering light samples. Enabling approximate visibility
uses ray tracing to check whether a candidate light sample is in shadow while applying resampling. The
temporal and spatial resampling passes offer independent control, but in almost all cases, the right
solution is to keep them set to the same value. Enabling these will reduce bias in the image. In particular
it can reduce bias that is darkening a scene somewhat compared to the ground truth as it more accurately
rejects occluded lights. These default to off in medium quality but on in high and ultra quality.

`r.RayTracing.SampledLighting.InitialSamples`  [1-N]

This is the number of lights tested per pixel when starting the sampled lighting pass. The more tested,
the higher quality of the selected light. Each light tested requires additional evaluation cost, so fewer
initial samples means more performance. Medium and high both default to 4, with ultra defaulting to 8. 

`r.RayTracing.SampledLighting.Spatial.Samples`  [1-16]

This is how many samples to test when applying spatial resampling to try to find higher quality light samples.
The more taken, the higher the quality of the estimate, and the more taken, the higher the execution cost.
The number of samples is presently limited to a maximum of 16. For the presets, medium and high both use just
one spatial sample, and ultra uses 4.

`r.RayTracing.SampledLighting.Spatial.SamplesBoost`  [1-16]

This is how many spatial samples to test when a pixel determines it has bad temporal history. Events like
objects that just became visible due to disocclusion are covered by this property. It helps the signal more
quickly converge to a good result to avoid issues like a noisy trail following a disocclusion. The default
for medium quality is 8, with high and ultra using the maximum of 16.

## Other quality options

In addition to these basic settings to tweak quality tied to the preset parameters there are a couple
others of note.

`r.RayTracing.SampledLighting.BoilingFilterStrength`  [0.0 ... 1.0]

The boiling filter helps to avoid exceptional samples that attempt to take over the image. When this
happens, the artifact generally looks like an odd highlight that grows to cover a large part of the
screen. The boiling filter resolves this by killing off samples that appear to be statistically out
of the norm. The filter strength of 0.35 has been shown to work well in testing. Lower values can improve
the quality of samples by not mistakenly discarding samples that might actually be good, but at a higher
risk of a runaway sample. The values here range from 0.0 to 1.0 with expected desirable values in the
range of 0.2 to 0.35. It is highly recommended that the default value not be changed unless you encounter
a problem.

`r.RayTracing.SampledLighting.NumReservoirs`  [-1/1-N]

The entire RTXDI algorithm relies on tracking a set of light samples and their probabilities. These
combined quantities are referred to as reservoirs, and they are maintained per pixel with temporal
history. Having more reservoirs reduces the noise in the signal and improves its quality, but obviously
there is substantial added cost for each additional reservoir. Typically, one reservoir per pixel produces
good quality; however, we have seen cases where the extra stability of a second reservoir is useful. This
is most apparent when rendering at a reduced resolution with upscaling like DLSS or TAAU where stability can
suffer somewhat. This is easily understandable as RTXDI is reducing the number of lighting samples and DLSS
is reducing the number of rendered samples. To make this easy, we added an auto-selection mode of -1 as the
default. When the number of reservoirs is set to -1, it will select the number of reservoirs based on the
image scaling. Care should be taken with high numbers of reservoirs as each additional one consumes roughly
64 MB of video memory when rendering at 1080p.

`r.RayTracing.SampledLighting.MinReservoirs`  [1-N]

This is the minimum number of reservoirs selected when using auto-selection to determine the number of
reservoirs. It represents the number that will be used when no rescaling such as DLSS is applied.

`r.RayTracing.SampledLighting.MaxReservoirs`  [1-N]

This is the maximum number of reservoirs selected when auto-selection to determine the number of
reservoirs. It represents the number that will be used when rescaling such as DLSS is applied.

## Other options

There are several additional advanced debugging and tuning options available, but the vast majority
of users will never care to look at them. One option which can be handy for users trying to understand
any possible artifacts is the ability to disable the denoiser.

`r.RayTracing.SampledLighting.Denoiser`  [0/2]

Setting this console variable to 0 will show the RTXDI result without denoising applied. It can
be helpful for understanding whether an artifact arises from the lighting in RTXDI, or as a result
of the denoising pass. The default value for the denoiser is 2, meaning to use the ReLAX denoiser from
the NRD plug-in. To match contention, 1 is intended to use the UE4 built-in denoiser; however, no such
implementation presently functions.

# NVIDIA Real-time Denoiser using ReLAX

[Full NRD Plug-in Readme](../../Engine/Plugins/Runtime/Nvidia/NRD/README.md) 

Like most ray tracing effects, RTXDI is dependent on denoising to produce a clean image. The solution
integrated into NvRTX is the ReLAX denoiser from the “NRD” NVIDIA Real-time Denoisers SDK. It uses the
denoiser plug-in interface like other UE4 denoisers, so a project desiring to use it needs to enable
this plug-in. Unlike other denoiser systems in UE4, this particular one has no working built-in fallback,
so it is either using the plug-in or foregoing denoising entirely.

ReLAX contains a very large number of tunable options, the defaults should be reasonable for most users.
Almost all of the relevant console variables fall in the namespace `r.NRD.ReLAX`, making them easy to find.
There is one option ( `r.NRD.DenoisingRange` ) which may frequently be tuned between projects, as it is tied
to the scale of the world. The denoising range is used to restrict how far the denoiser applies, so that
it isn’t processing sky pixels. The default value is 100,000 units or 1 km based on standard conventions.
Large outdoor environments will likely need to increase this limit. Anything beyond the limit will not
just lose denoising, but it will receive no lighting from RTXDI.

One other aspect potentially worth tuning is the temporal responsiveness. The integration in UE4 has
default settings optimized for responsiveness. In our testing, this felt like the best configuration for
games to avoid ghosting on a fast moving shadow. The bias toward responsiveness can potentially reduce
the temporal stability of the image, like a flicker on a thin feature in a shadow. While we have yet to
observe any major occurrence of this, it is possible in theory. Adjusting the ReLAX parameter
`r.NRD.Relax.History.DiffuseFastMaxAccumulatedFrameNum` is the most effective way to tweak these behaviors
in our experiments. The default of zero will give you the most responsive signal, while increasing it
to a value like 4 may maximize the temporal stability.

# General Operation

RTXDI is derived from an algorithm known as [ReSTIRS](https://developer.nvidia.com/blog/rendering-millions-of-dynamics-lights-in-realtime/).
It works by statistically selecting which lights to render on a per-pixel basis and keeping a temporal
history of the importance of the lights selected. It starts by randomly selecting a small number of
initial candidate samples and computing weights based on how they likely contribute. Ultimately, one
sample is selected from the bunch, then compared against the sample from the last frame. Next, the pixel
searches a few nearby pixels looking for better samples. Each step improves the quality of the sample
selected, and by feeding the temporal history from one frame to the next, the quality continues to improve.
Once the light samples are selected, shadow rays are cast and shading is performed. This produces a noisy
image which the included denoiser ReLAX processes to produce a final shading result representing all the
sampled lights. 
