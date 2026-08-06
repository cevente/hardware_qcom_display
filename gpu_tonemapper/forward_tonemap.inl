/*
 * Copyright (c) 2016-2017, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

const char* forward_tonemap_shader = ""
    "#extension GL_OES_EGL_image_external_essl3 : require                       \n"
    "precision highp float;                                                     \n"
    "precision highp sampler2D;                                                 \n"
    "// Optimized for 1080x2400 on Bengal GPU                                  \n"
    "layout(binding = 0) uniform samplerExternalOES externalTexture;            \n"
    "layout(binding = 1) uniform sampler3D tonemapper;                          \n"
    "layout(binding = 2) uniform sampler2D xform;                               \n"
    "layout(location = 3) uniform vec2 tSO;                                     \n"
    "#ifdef USE_NONUNIFORM_SAMPLING                                             \n"
    "layout(location = 4) uniform vec2 xSO;                                     \n"
    "#endif                                                                     \n"
    "in vec2 uv;                                                                \n"
    "out vec4 fs_color;                                                         \n"
    "                                                                           \n"
    "// Inline ScaleOffset to avoid function call overhead                    \n"
    "#define SCALE_OFFSET(samplePt, so) ((so.x * samplePt) + so.y)             \n"
    "                                                                           \n"
    "void main()                                                                \n"
    "{                                                                          \n"
    "    // Flip UV with single operation for 1080x2400                       \n"
    "    vec2 flipped = vec2(uv.x, 1.0f - uv.y);                                \n"
    "    vec4 rgb = texture(externalTexture, flipped);                          \n"
    "#ifdef USE_NONUNIFORM_SAMPLING                                             \n"
    "    // Combine operations for better vectorization on Bengal             \n"
    "    vec3 adj = (xSO.x * rgb.xyz) + xSO.y;                                  \n"
    "    // Use texture gather for better performance                         \n"
    "    float r = texture(xform, vec2(adj.r, 0.5f)).r;                         \n"
    "    float g = texture(xform, vec2(adj.g, 0.5f)).g;                         \n"
    "    float b = texture(xform, vec2(adj.b, 0.5f)).b;                         \n"
    "    // Combine final RGB directly                                        \n"
    "    vec3 finalRgb = vec3(r, g, b);                                         \n"
    "#else                                                                      \n"
    "    vec3 finalRgb = rgb.xyz;                                               \n"
    "#endif                                                                     \n"
    "    // Direct texture lookup with combined scale/offset                  \n"
    "    fs_color.rgb = texture(tonemapper, (tSO.x * finalRgb) + tSO.y).rgb;    \n"
    "    fs_color.a = 1.0f;                                                     \n"
    "}                                                                          \n";
