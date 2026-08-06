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

const char* rgba_inverse_tonemap_shader = ""
    "#extension GL_OES_EGL_image_external_essl3 : require                                               \n"
    "precision highp float;                                                                             \n"
    "precision highp sampler2D;                                                                         \n"
    "// Optimized for 1080x2400 on Bengal GPU                                                          \n"
    "layout(binding = 0) uniform samplerExternalOES externalTexture;                                    \n"
    "layout(binding = 1) uniform sampler3D tonemapper;                                                  \n"
    "layout(binding = 2) uniform sampler2D xform;                                                       \n"
    "layout(location = 3) uniform vec2 tSO;                                                             \n"
    "#if defined(USE_NONUNIFORM_SAMPLING)                                                               \n"
    "layout(location = 4) uniform vec2 xSO;                                                             \n"
    "#endif                                                                                             \n"
    "in vec2 uv;                                                                                        \n"
    "out vec4 fs_color;                                                                                 \n"
    "                                                                                                   \n"
    "// Inline ScaleOffset for performance                                                             \n"
    "#define SCALE_OFFSET(samplePt, so) ((so.x * samplePt) + so.y)                                     \n"
    "                                                                                                   \n"
    "void main()                                                                                        \n"
    "{                                                                                                  \n"
    "    vec2 flipped = vec2(uv.x, 1.0f - uv.y);                                                        \n"
    "    vec4 rgb_premulalpha = texture(externalTexture, flipped);                                      \n"
    "    fs_color = rgb_premulalpha;                                                                    \n"
    "    if( rgb_premulalpha.a > 0.0 ) {                                                                \n"
    "        vec3 rgb = rgb_premulalpha.rgb / rgb_premulalpha.a;                                        \n"
    "#if defined(USE_NONUNIFORM_SAMPLING)                                                               \n"
    "        // Optimized vector operations for Bengal                                                \n"
    "        vec3 adj = (xSO.x * rgb.xyz) + xSO.y;                                                      \n"
    "        float r = texture(xform, vec2(adj.r, 0.5f)).r;                                             \n"
    "        float g = texture(xform, vec2(adj.g, 0.5f)).g;                                             \n"
    "        float b = texture(xform, vec2(adj.b, 0.5f)).b;                                             \n"
    "        vec3 finalRgb = vec3(r, g, b);                                                             \n"
    "#else                                                                                              \n"
    "        vec3 finalRgb = rgb;                                                                       \n"
    "#endif                                                                                             \n"
    "        // Combine texture lookup with alpha                                                     \n"
    "        fs_color.rgb = texture(tonemapper, (tSO.x * finalRgb) + tSO.y).rgb * rgb_premulalpha.a;    \n"
    "        fs_color.a = rgb_premulalpha.a;                                                            \n"
    "    }                                                                                              \n"
    "}                                                                                                  \n";
