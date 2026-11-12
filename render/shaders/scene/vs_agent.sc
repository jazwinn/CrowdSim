$input a_position, a_normal, i_data0, i_data1
$output v_normal, v_color0, v_wpos, v_local

// Instanced agent body.
//   i_data0 = (world x, world y, world z, heading radians; 0 faces +Z)
//   i_data1 = (r, g, b, horizontal scale)

#include <bgfx_shader.sh>

void main()
{
    float c = cos(i_data0.w);
    float s = sin(i_data0.w);

    vec3 p = vec3(a_position.x * i_data1.w, a_position.y, a_position.z * i_data1.w);
    // Rotate about +Y so local +Z maps to (sin h, 0, cos h).
    vec3 wp = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z) + i_data0.xyz;
    vec3 n  = vec3(c * a_normal.x + s * a_normal.z, a_normal.y, -s * a_normal.x + c * a_normal.z);

    gl_Position = mul(u_viewProj, vec4(wp, 1.0));
    v_normal = n;
    v_color0 = vec4(i_data1.xyz, 1.0);
    v_wpos = wp;
    v_local = a_position;
}
