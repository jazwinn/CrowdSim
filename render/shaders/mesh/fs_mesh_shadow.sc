$input v_normal, v_color0, v_wpos

// Depth-only pass into the shadow map. bgfx requires the fragment inputs to match
// the vertex outputs, so each vertex shader family has its own depth shader.

#include <bgfx_shader.sh>

void main()
{
    gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
}
