SteamVR Passthrough Plugin
---

This Unreal Engine 4 plugin adds SteamVR passthrough camera support using the OpenVR TrackedCamera API.

Example project: https://github.com/Rectus/UE4SteamVRPassthrough_Example

### Features ###

- Supports projects using either the native SteamVR plugin, or the OpenXR plugin when the SteamVR runtime is active.
- Supports Direct3D 11 texture sharing for the camera feed. 

### Note ###

This is a very early release. It has only been tested with the Valve Index.

Any feedback or issue reports are appreciated.

### Requirements ###

UE4 version 4.26 or 4.27 (the example project only supports 4.27).

SteamVR with a headset that exposes the passthrough camera images. Most Vive headsets and the Valve Index can do this. Oculus and WMR headsets are NOT supported.

If using OpenXR as the XR system, SteamVR needs to be the active runtime.

#### Headset compatibility ####

- Valve Index - Supported
- HTC Vive - Supported, driver only provides correct pose data in 60 Hz mode. 

The SteamVR settings UI will misconfigure the Vive camera if the frame rate is not set the right way. To correctly set it, click the right end of the slider instead of dragging it. The USB drivers may need to be reset if the camera is incorrectly configurred.


### Usage ###

Everything is controlled from a USteamVRPassthroughComponent.

The plugin supports rendering the passthrough in two different ways. 

1. An automaticly added post process render pass. This can either use a simple fullscreen shader optionally masked with a custom depth stencil, or with a full post process material.

2. Any scene material with manually set up UV transformation. In order for the transforms to be updated with minimal latency, the material paramters that pass the transformation matrices are registered with the USteamVRPassthroughComponent to be updated by the render thread.

Support for activating the passthrough while OpenXR or other XR systems are active can be toggled with the `vr.SteamVRPassthrough.AllowBackgroundRuntime` console variable.

Please see the example project for more information.
