$input v_wpos

#include <bgfx_shader.sh>

uniform vec4 u_cameraPos;  // xyz = camera position, w = fog density

// Anti-aliased grid line intensity for a given spacing.
float gridLine(vec2 p, float spacing, float width)
{
    vec2 coord = p / spacing;
    vec2 deriv = fwidth(coord);
    vec2 g = abs(fract(coord - 0.5) - 0.5) / max(deriv * width, vec2(1e-4, 1e-4));
    return 1.0 - min(min(g.x, g.y), 1.0);
}

void main()
{
    vec2 p = v_wpos.xz;
    vec3 color = vec3(0.235, 0.247, 0.267);
    color = mix(color, vec3(0.30, 0.31, 0.33), gridLine(p, 1.0, 1.0) * 0.6);
    color = mix(color, vec3(0.40, 0.41, 0.44), gridLine(p, 10.0, 1.5));

    // Axis lines: X red, Z blue.
    vec2 axis = abs(p) / max(fwidth(p) * 2.0, vec2(1e-4, 1e-4));
    color = mix(color, vec3(0.75, 0.30, 0.30), 1.0 - min(axis.y, 1.0));
    color = mix(color, vec3(0.30, 0.45, 0.80), 1.0 - min(axis.x, 1.0));

    float dist = length(v_wpos - u_cameraPos.xyz);
    float fog = 1.0 - exp(-dist * u_cameraPos.w);
    color = mix(color, vec3(0.149, 0.165, 0.188), clamp(fog, 0.0, 1.0));
    gl_FragColor = vec4(color, 1.0);
}
