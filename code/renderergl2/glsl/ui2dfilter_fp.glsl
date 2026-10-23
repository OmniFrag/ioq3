uniform sampler2D u_DiffuseMap;

// the UI's layers' passes (tr_ui2d_layers.c), on premultiplied colors: 0
// the texture times u_Color, 1 a color matrix, 2 a blur along
// u_BlurDirection, 3 its alpha in u_Color, for a shadow
uniform int    u_FilterMode;
uniform vec4   u_Color;
uniform vec2   u_InvTexRes;
uniform mat4   u_ColorMatrix;
uniform vec4   u_ColorOffset;
uniform vec2   u_BlurDirection;
uniform int    u_BlurRadius;
uniform float  u_BlurWeights[13];

varying vec2   var_TexCoord;


void main()
{
	vec4 texel = texture2D(u_DiffuseMap, var_TexCoord);

	if (u_FilterMode == 1)
	{
		vec4 c = vec4(texel.a > 0.0 ? texel.rgb / texel.a : vec3(0.0), texel.a);
		vec4 o = clamp(u_ColorMatrix * c + u_ColorOffset, 0.0, 1.0);

		gl_FragColor = vec4(o.rgb * o.a, o.a);
	}
	else if (u_FilterMode == 2)
	{
		vec2 step = u_BlurDirection * u_InvTexRes;
		vec4 sum = texel * u_BlurWeights[0];

		for (int i = 1; i < 13; i++)
		{
			if (i > u_BlurRadius)
				break;
			sum += (texture2D(u_DiffuseMap, var_TexCoord + step * float(i)) +
				texture2D(u_DiffuseMap, var_TexCoord - step * float(i))) * u_BlurWeights[i];
		}
		gl_FragColor = sum;
	}
	else if (u_FilterMode == 3)
		gl_FragColor = u_Color * texel.a;
	else
		gl_FragColor = texel * u_Color;
}
