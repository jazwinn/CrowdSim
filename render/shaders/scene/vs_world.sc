$input a_position
$output v_wpos

// Static geometry transformed by the model matrix (ground, area markers).

#include <bgfx_shader.sh>

void main()
{
    vec4 wp = mul(u_model[0], vec4(a_position, 1.0));
    gl_Position = mul(u_viewProj, wp);
    v_wpos = wp.xyz;
}
