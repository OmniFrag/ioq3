uniform sampler2D u_DiffuseMap;
uniform sampler2D u_MaskMap;

// the UI's layers' passes (tr_ui2d_layers.c), on premultiplied colors: 0
// the texture times u_Color, 1 a color matrix, on colors in display units,
// which the 2D draws at u_ColorScale (tr.frameLight), 2 a blur along
// u_BlurDirection, 3 its alpha in u_Color, for a shadow, 4 the texture
// times u_MaskMap's coverage, placed by u_MaskTransform and clear around it
uniform int    u_FilterMode;
uniform vec4   u_Color;
uniform vec2   u_InvTexRes;
uniform mat4   u_ColorMatrix;
uniform vec4   u_ColorOffset;
uniform float  u_ColorScale;
uniform vec2   u_BlurDirection;
uniform int    u_BlurRadius;
uniform float  u_BlurWeights[13];
uniform vec4   u_MaskTransform;
uniform vec4   u_MaskChannel;		// which of u_MaskMap's holds its coverage

varying vec2   var_TexCoord;


void main()
{
	vec4 texel = texture2D(u_DiffuseMap, var_TexCoord);

	if (u_FilterMode == 1)
	{
		vec4 c = vec4(texel.a > 0.0 ? texel.rgb / (texel.a * u_ColorScale) : vec3(0.0), texel.a);
		vec4 o = clamp(u_ColorMatrix * c + u_ColorOffset, 0.0, 1.0);

		gl_FragColor = vec4(o.rgb * (o.a * u_ColorScale), o.a);
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
	else if (u_FilterMode == 4)
	{
		vec2 m = var_TexCoord * u_MaskTransform.xy + u_MaskTransform.zw;
		vec2 inside = step(vec2(0.0), m) * step(m, vec2(1.0));

		gl_FragColor = texel * dot(texture2D(u_MaskMap, m), u_MaskChannel) * inside.x * inside.y;
	}
	else
		gl_FragColor = texel * u_Color;
}
