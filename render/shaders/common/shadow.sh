// Directional-light shadow map lookup shared by every lit fragment shader.
//   u_lightMtx     world -> shadow-map texture space (xy in [0,1], z = depth)
//   u_shadowParams x = texel size, y = depth bias, z = 1 if enabled, w = normal offset (m)

uniform mat4 u_lightMtx;
uniform vec4 u_shadowParams;
SAMPLER2DSHADOW(s_shadowMap, 1);

float shadowVisibility(vec3 wpos, vec3 n)
{
    if (u_shadowParams.z < 0.5)
    {
        return 1.0;
    }
    vec4 sc = mul(u_lightMtx, vec4(wpos + n * u_shadowParams.w, 1.0));
    vec3 c = sc.xyz / sc.w;
    if (c.x <= 0.0 || c.x >= 1.0 || c.y <= 0.0 || c.y >= 1.0 || c.z >= 1.0)
    {
        return 1.0;
    }
    float t = u_shadowParams.x;
    float z = c.z - u_shadowParams.y;
    // 5-tap PCF (each tap is itself bilinear-filtered by the comparison sampler).
    float v = shadow2D(s_shadowMap, vec3(c.xy, z));
    v += shadow2D(s_shadowMap, vec3(c.xy + vec2(-t, -t), z));
    v += shadow2D(s_shadowMap, vec3(c.xy + vec2( t, -t), z));
    v += shadow2D(s_shadowMap, vec3(c.xy + vec2(-t,  t), z));
    v += shadow2D(s_shadowMap, vec3(c.xy + vec2( t,  t), z));
    return v * 0.2;
}
