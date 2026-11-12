$input a_position, a_normal, a_color0, a_texcoord2, a_texcoord3, i_data0, i_data1, i_data2, i_data3
$output v_normal, v_color0, v_wpos

// Animated crowd character: skinned from a bone-matrix vertex animation texture.
//   s_bones  RGBA32F, 3 texels per joint per row (top rows of the skinning matrix)
//   i_data0 = (world x, y, z, heading; 0 faces +Z)
//   i_data1 = (shirt r, g, b, uniform scale)
//   i_data2 = (clip A row 0, clip A row 1, clip A fraction, A->B blend)
//   i_data3 = (clip B row 0, clip B row 1, clip B fraction, per-agent hash)
// a_texcoord2 = joint indices / 255, a_texcoord3 = joint weights (both normalized bytes).
// Vertex colour alpha selects the material: 1 shirt, 2/3 trousers, 1/3 skin, ~0.16 hair, 0 fixed.

#include <bgfx_shader.sh>

SAMPLER2D(s_bones, 0);

mat4 boneAt(float row, float joint)
{
    ivec2 c = ivec2(int(joint) * 3, int(row));
    vec4 r0 = texelFetch(s_bones, c, 0);
    vec4 r1 = texelFetch(s_bones, c + ivec2(1, 0), 0);
    vec4 r2 = texelFetch(s_bones, c + ivec2(2, 0), 0);
    return mtxFromRows(r0, r1, r2, vec4(0.0, 0.0, 0.0, 1.0));
}

// Vertex inputs are not globals in every backend: pass the instance data in.
mat4 animatedBone(float joint, vec4 clipA, vec4 clipB)
{
    mat4 a = boneAt(clipA.x, joint) * (1.0 - clipA.z) + boneAt(clipA.y, joint) * clipA.z;
    mat4 b = boneAt(clipB.x, joint) * (1.0 - clipB.z) + boneAt(clipB.y, joint) * clipB.z;
    return a * (1.0 - clipA.w) + b * clipA.w;
}

vec3 trousersColor(float h)
{
    float k = fract(h * 3.71);
    if (k < 0.25) return vec3(0.16, 0.18, 0.27);
    if (k < 0.50) return vec3(0.20, 0.20, 0.22);
    if (k < 0.75) return vec3(0.24, 0.32, 0.47);
    return vec3(0.52, 0.47, 0.36);
}

vec3 skinColor(float h)
{
    return mix(vec3(0.96, 0.80, 0.68), vec3(0.42, 0.28, 0.20), fract(h * 7.13));
}

vec3 hairColor(float h)
{
    float k = fract(h * 5.31);
    if (k < 0.45) return vec3(0.07, 0.06, 0.05);
    if (k < 0.75) return vec3(0.28, 0.18, 0.11);
    if (k < 0.92) return vec3(0.72, 0.58, 0.36);
    return vec3(0.62, 0.62, 0.64);
}

void main()
{
    float j0 = floor(a_texcoord2.x * 255.0 + 0.5);
    float j1 = floor(a_texcoord2.y * 255.0 + 0.5);
    mat4 skin = animatedBone(j0, i_data2, i_data3) * a_texcoord3.x + animatedBone(j1, i_data2, i_data3) * a_texcoord3.y;
    vec3 p = mul(skin, vec4(a_position, 1.0)).xyz * i_data1.w;
    vec3 n = mul(skin, vec4(a_normal, 0.0)).xyz;

    float c = cos(i_data0.w);
    float s = sin(i_data0.w);
    vec3 wp = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z) + i_data0.xyz;
    vec3 wn = vec3(c * n.x + s * n.z, n.y, -s * n.x + c * n.z);

    float m = a_color0.a;
    float h = i_data3.w;
    vec3 base = a_color0.rgb;
    if (m > 0.9) base = i_data1.xyz;
    else if (m > 0.5) base = trousersColor(h);
    else if (m > 0.25) base = skinColor(h);
    else if (m > 0.1) base = hairColor(h);

    gl_Position = mul(u_viewProj, vec4(wp, 1.0));
    v_normal = wn;
    v_color0 = vec4(base, 1.0);
    v_wpos = wp;
}
