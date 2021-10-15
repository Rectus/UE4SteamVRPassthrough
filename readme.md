This Unreal Engine 4 plugin adds SteamVR passthrough camera support using the OpenVR TrackedCamera API.

Supports Direct3D 11 shared textures for the camera feed. 

### Note ###

This is a very early release. It has only been tested with the Valve Index.
Please report any issues.

### Requirements ###

UE4 version 4.26 or 4.27 (the example project only supports 4.27)

SteamVR with a headset that exposes the passthrough camera images (most Vive headests and the Valve Index).

The SteamVR XR plugin needs to be active.


### Usage ###

Everything is controlled from a USteamVRPassthroughComponent.

The plugin supports rendering the passthrough in two different ways. 

1. An automaticly added post process render pass. This can either use a simple fullscreen shader optionally masked with a custom depth stencil, or with a full post process material.

2. Any scene material with manually set up UV transformation. In order for the transforms to be updated with minimal latency, the material paramters that pass the transformation matrices are registered with the USteamVRPassthroughComponent to be updated by the render thread.

Please see the example project for more information.