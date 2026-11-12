$input v_normal, v_color0, v_wpos

#include <bgfx_shader.sh>
#include "shadow.sh"

uniform vec4 u_lightDir;   // xyz = direction towards the light
uniform vec4 u_cameraPos;  // xyz = camera position, w = fog density

void main()
{
    vec3 n = normalize(v_normal);
    float diffuse = max(dot(n, normalize(u_lightDir.xyz)), 0.0) * shadowVisibility(v_wpos, n);
    // Vertex colours already carry baked face shading; add a gentle light on top.
    vec3 color = v_color0.rgb * (0.55 + 0.55 * diffuse);

    float dist = length(v_wpos - u_cameraPos.xyz);
    float fog = 1.0 - exp(-dist * u_cameraPos.w);
    color = mix(color, vec3(0.149, 0.165, 0.188), clamp(fog, 0.0, 1.0));
    gl_FragColor = vec4(color, 1.0);
}
