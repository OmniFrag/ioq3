uniform sampler2D u_DiffuseMap;

// 0 no texture, 1 straight RGBA, 2 premultiplied RGBA, 3 coverage in red
uniform int    u_TextureMode;

#if defined(USE_PAINT)
// a gradient (paint2D_t) by the texture coordinates: -1 none, 0 linear,
// 1 radial, 2 conic; colors premultiplied, four floats a stop
uniform int    u_PaintKind;
uniform int    u_PaintRepeat;
uniform vec2   u_PaintP0;
uniform vec2   u_PaintP1;
uniform int    u_NumStops;
uniform float  u_StopPos[16];
uniform float  u_StopColor[64];
#endif

varying vec4   var_Color;
varying vec2   var_TexCoord;


#if defined(USE_PAINT)
vec4 StopColor(int i)
{
	return vec4(u_StopColor[i * 4], u_StopColor[i * 4 + 1], u_StopColor[i * 4 + 2], u_StopColor[i * 4 + 3]);
}

vec4 Paint(float t)
{
	float first = u_StopPos[0];
	float last = u_StopPos[u_NumStops - 1];
	vec4 color = StopColor(0);

	if (u_PaintRepeat != 0 && last > first)
		t = first + mod(t - first, last - first);

	for (int i = 1; i < 16; i++)
	{
		if (i >= u_NumStops || t <= u_StopPos[i - 1])
			break;

		if (t >= u_StopPos[i])
			color = StopColor(i);
		else
		{
			color = mix(StopColor(i - 1), StopColor(i), (t - u_StopPos[i - 1]) / (u_StopPos[i] - u_StopPos[i - 1]));
			break;
		}
	}

	return color;
}
#endif

void main()
{
	vec4 color = var_Color;

	if (u_TextureMode != 0)
	{
		vec4 texel = texture2D(u_DiffuseMap, var_TexCoord);

		if (u_TextureMode == 1)
			texel.rgb *= texel.a;
		else if (u_TextureMode == 3)
			texel = vec4(texel.r);

		color *= texel;
	}

#if defined(USE_PAINT)
	if (u_PaintKind >= 0)
	{
		vec2 d = var_TexCoord - u_PaintP0;
		float t;

		if (u_PaintKind == 0)
		{
			vec2 v = u_PaintP1 - u_PaintP0;
			t = dot(d, v) / dot(v, v);
		}
		else if (u_PaintKind == 1)
			t = length(d / u_PaintP1);
		else
			// clockwise from up, with y down
			t = fract((atan(d.x, -d.y) - u_PaintP1.x) / 6.28318530718);

		color *= Paint(t);
	}
#endif

	gl_FragColor = color;
}
