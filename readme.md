SteamVR Passthrough Plugin
---

This Unreal Engine 4 and 5 plugin adds SteamVR passthrough camera support using the OpenVR TrackedCamera API.

[Example project](https://github.com/Rectus/UE4SteamVRPassthrough_Example)

[Unreal Engine 5 branch](../../tree/ue5)

### Features ###

- Supports projects using either the native SteamVR plugin, or the OpenXR plugin when the SteamVR runtime is active.
- Supports Direct3D 11 texture sharing for the camera feed. 

### Note ###

This is a very early release. It has only been fully tested with a few headsets.

Any feedback or issue reports are appreciated.

Direct texture sharing of the camera stream is only supported when using Direct3D 11. When using other RHIs, the image data is copied on the CPU, which may take up significant time on the render thread.

### Requirements ###

UE4 version 4.26 or 4.27 (the example project only supports 4.27).

Windows or Linux (Linux support not tested).

SteamVR with a headset that exposes the passthrough camera images. Most Vive headsets and the Valve Index can do this. Oculus and WMR headsets are NOT supported.

If using OpenXR as the XR system, SteamVR needs to be the active runtime.

#### Headset compatibility ####

- Valve Index - Supported
- HTC Vive - Supported, driver only provides correct pose data in 60 Hz mode.
- HTC Vive Pro - Supported but not fully verified
- HTC Vive Pro 2 - Supported but not fully verified
- Other HTC headsets - Unknown
- Windows Mixed Reality headsets - Unsupported, no passthrough support in driver
- Oculus/Meta headsets - Unsupported, no passthrough support in driver

The SteamVR settings UI will misconfigure the original HTC Vive camera if the frame rate is not set the right way. To correctly set it, click the right end of the slider instead of dragging it. The USB drivers may need to be reset if the camera is incorrectly configurred.


### Usage ###

Everything is controlled from a USteamVRPassthroughComponent.

The plugin supports rendering the passthrough in three different ways.

1. An automaticly added post process render pass, using a simple shader that draws the camera output over the screen. The shader supports compositing the output with the scene in two ways: 
	- Stenciling the output with the Custom Depth Stencil (no MSAA). 
	- Blending based on the scene alpha channel (supports MSAA, requires the setting "Enable alpha channel support in post processing" to be set to "allow through tonemapping").

2. An automaticly added post process render pass, using a post process material. The camera frames are automatically passed to the `CameraTexture` parameter and the UVs for transforming the frames are passed to the first two UV channels.

3. Any scene material with manually set up UV transformation. In order for the transforms to be updated with minimal latency, the material paramters that pass the transformation matrices are registered with the USteamVRPassthroughComponent to be updated by the render thread.

Support for activating the passthrough while OpenXR or other XR systems are active can be toggled with the `vr.SteamVRPassthrough.AllowBackgroundRuntime` console variable.

Please see the example project for more information.
