$input a_position, a_normal, a_color0
$output v_normal, v_color0, v_wpos

// Static world-space geometry with per-vertex colour (environment meshes).

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
    v_normal = a_normal;
    v_color0 = a_color0;
    v_wpos = a_position;
}
