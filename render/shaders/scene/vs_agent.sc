$input a_position, a_normal, i_data0, i_data1
$output v_normal, v_color0, v_wpos, v_local

// Instanced agent body.
//   i_data0 = (world x, world y, world z, heading radians; 0 faces +Z)
//   i_data1 = (r, g, b, horizontal scale); a negative scale lays the body down along
//             its heading (someone who has fallen)

#include <bgfx_shader.sh>

void main()
{
    float c = cos(i_data0.w);
    float s = sin(i_data0.w);

    float scale = abs(i_data1.w);
    vec3 p = vec3(a_position.x * scale, a_position.y, a_position.z * scale);
    vec3 nl = a_normal;
    if (i_data1.w < 0.0)
    {
        // Tip over forwards about the feet: up becomes the heading, lifted by the body's
        // half-depth so it rests on the ground.
        p  = vec3(p.x, 0.22 - p.z, p.y);
        nl = vec3(nl.x, -nl.z, nl.y);
    }
    // Rotate about +Y so local +Z maps to (sin h, 0, cos h).
    vec3 wp = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z) + i_data0.xyz;
    vec3 n  = vec3(c * nl.x + s * nl.z, nl.y, -s * nl.x + c * nl.z);

    gl_Position = mul(u_viewProj, vec4(wp, 1.0));
    v_normal = n;
    v_color0 = vec4(i_data1.xyz, 1.0);
    v_wpos = wp;
    v_local = a_position;
}
