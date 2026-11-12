$input v_normal, v_color0, v_wpos, v_local

#include <bgfx_shader.sh>
#include "shadow.sh"

uniform vec4 u_lightDir;   // xyz = direction towards the light
uniform vec4 u_cameraPos;  // xyz = camera position, w = fog density

void main()
{
    vec3 n = normalize(v_normal);
    vec3 l = normalize(u_lightDir.xyz);
    float diffuse = max(dot(n, l), 0.0) * shadowVisibility(v_wpos, n);
    float hemi = 0.5 + 0.5 * n.y;

    vec3 base = v_color0.rgb;
    // A light band across the front of the head shows which way the agent faces.
    if (v_local.z > 0.0 && v_local.y > 1.38 && v_local.y < 1.52) {
        base = mix(base, vec3(1.0, 1.0, 1.0), 0.65);
    }

    vec3 color = base * (0.25 + 0.30 * hemi + 0.65 * diffuse);

    float dist = length(v_wpos - u_cameraPos.xyz);
    float fog = 1.0 - exp(-dist * u_cameraPos.w);
    color = mix(color, vec3(0.149, 0.165, 0.188), clamp(fog, 0.0, 1.0));
    gl_FragColor = vec4(color, 1.0);
}
