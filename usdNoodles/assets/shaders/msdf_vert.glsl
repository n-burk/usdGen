#version 330 core

// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in float aNodeIndex;

uniform mat4 uProjection;
// RG32F texture, capacity x 1 texels. sampler2D keeps the transform path
// available on OpenGL ES 3.0, which has no core buffer textures.
uniform sampler2D uNodeTransforms;

out vec2 vTexCoord;

void main() {
    vec3 transformedPos = aPosition;

    // Negative indices skip the fetch (UI overlays that shouldn't move).
    if (aNodeIndex >= 0.0) {
        vec2 offset = texelFetch(uNodeTransforms, ivec2(int(aNodeIndex), 0), 0).rg;
        transformedPos = vec3(aPosition.xy + offset, aPosition.z);
    }

    gl_Position = uProjection * vec4(transformedPos, 1.0);
    vTexCoord = aTexCoord;
}
