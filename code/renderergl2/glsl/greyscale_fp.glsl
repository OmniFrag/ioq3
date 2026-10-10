uniform sampler2D u_TextureMap;
uniform float     u_Greyscale;
uniform float     u_Gamma;
uniform float     u_Overbright;
uniform float     u_FrameScale;
varying vec2      var_TexCoords;

// From Rec. 709-1 4.2 "Derivation of luminance signal"
const vec3 LUMA = vec3(0.2125, 0.7154, 0.0721);

void main() {
    vec4 color = texture2D(u_TextureMap, var_TexCoords);
    if (u_Greyscale > 0.0) {
        float y = dot(color.rgb, LUMA);
        color.rgb = mix(color.rgb, vec3(y), clamp(u_Greyscale, 0.0, 1.0));
    }
    // brightness, as a gamma ramp would apply it (R_SetColorMappings'
    // table): to the color at Quake III's own scale, clamped as its
    // framebuffer would clamp it, then shifted up by overbright. The frame
    // holds that color (r_stockBlending), or in display units that color
    // already brightened by overbright, which u_FrameScale takes back
    color.rgb = clamp(color.rgb * u_FrameScale, 0.0, 1.0);
    if (u_Gamma != 1.0) {
        color.rgb = pow(color.rgb, vec3(1.0 / u_Gamma));
    }
    color.rgb *= u_Overbright;
    gl_FragColor = color;
}
