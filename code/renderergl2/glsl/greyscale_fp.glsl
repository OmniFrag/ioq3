uniform sampler2D u_TextureMap;
uniform float     u_Greyscale;
uniform float     u_Gamma;
varying vec2      var_TexCoords;

// From Rec. 709-1 4.2 "Derivation of luminance signal"
const vec3 LUMA = vec3(0.2125, 0.7154, 0.0721);

void main() {
    vec4 color = texture2D(u_TextureMap, var_TexCoords);
    if (u_Greyscale > 0.0) {
        float y = dot(color.rgb, LUMA);
        color.rgb = mix(color.rgb, vec3(y), clamp(u_Greyscale, 0.0, 1.0));
    }
    // brightness, as a gamma ramp would apply it to the displayed colour
    // (R_SetColorMappings' table)
    if (u_Gamma != 1.0) {
        color.rgb = pow(clamp(color.rgb, 0.0, 1.0), vec3(1.0 / u_Gamma));
    }
    gl_FragColor = color;
}
