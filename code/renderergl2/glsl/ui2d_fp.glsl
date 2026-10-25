uniform sampler2D u_DiffuseMap;

// 0 no texture, 1 straight RGBA, 2 premultiplied RGBA, 3 coverage in red
uniform int    u_TextureMode;

#if defined(USE_PAINT)
// a paint (paint2D_t) by the texture coordinates: -1 none, 0 linear, 1
// radial, 2 conic, 3 a rounded box, 4 a multi-channel distance field in
// the texture; gradients' colors premultiplied, four floats a stop
uniform int    u_PaintKind;
uniform int    u_PaintRepeat;
uniform vec2   u_PaintP0;
uniform vec2   u_PaintP1;
uniform int    u_NumStops;
uniform float  u_StopPos[16];
uniform float  u_StopColor[64];
uniform vec4   u_PaintBox;		// x, y, width, height
uniform vec4   u_PaintRadii;	// top left, top right, bottom right, bottom left
uniform float  u_PaintFeather;
uniform float  u_MsdfRange;		// the field's range in texels
uniform vec2   u_MsdfSourceTexel;	// a premultiplied raster's texel the glyph stands in for, in texels; 0 for none
uniform vec2   u_TextureSize;
#endif

varying vec4   var_Color;
varying vec2   var_TexCoord;


#if defined(USE_PAINT)
vec4 StopColor(int i)
{
	return vec4(u_StopColor[i * 4], u_StopColor[i * 4 + 1], u_StopColor[i * 4 + 2], u_StopColor[i * 4 + 3]);
}

vec4 Gradient(float t)
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

// how much of a rounded box covers this pixel: its signed distance,
// anti-aliased over a pixel, or softened over the feather
float RoundedBox(vec2 p)
{
	vec2 halfSize = u_PaintBox.zw * 0.5;
	vec2 q = p - (u_PaintBox.xy + halfSize);
	// a corner no rounder than the box allows
	float r = min(q.x < 0.0 ? (q.y < 0.0 ? u_PaintRadii.x : u_PaintRadii.w) : (q.y < 0.0 ? u_PaintRadii.y : u_PaintRadii.z),
		min(halfSize.x, halfSize.y));
	vec2 e = abs(q) - halfSize + r;
	float d = length(max(e, 0.0)) + min(max(e.x, e.y), 0.0) - r;
	float soft = max(fwidth(d), u_PaintFeather);

	return clamp(0.5 - d / soft, 0.0, 1.0);
}

// a multi-channel distance field's distance: the median of its channels,
// 0.5 on the outline, more inside
float Median(vec4 texel)
{
	return max(min(texel.r, texel.g), min(max(texel.r, texel.g), texel.b));
}

// how much of a glyph the field covers at a distance d: its range scaled
// to the pixels it's drawn at along the narrower axis, so a glyph stretched
// unevenly (a console's 8x16 cell of a square design) stays smooth across
// its squeezed edges
float Msdf(float d)
{
	vec2 unitRange = vec2(u_MsdfRange) / u_TextureSize;
	vec2 screenRange = unitRange / fwidth(var_TexCoord);
	float pixelRange = max(min(screenRange.x, screenRange.y), 1.0);

	return clamp(pixelRange * (d - 0.5) + 0.5, 0.0, 1.0);
}
#endif

void main()
{
	vec4 color = var_Color;

#if defined(USE_PAINT)
	if (u_PaintKind == 4)
	{
		vec4 texel = texture2D(u_DiffuseMap, var_TexCoord);

		// a raster with premultiplied colors drawn as straight alpha, which
		// the glyph stands in for, draws its color times its coverage again:
		// darker toward the outline, from full one of its texels inside to
		// half on it. From the field's true distance (alpha), round at
		// corners as the raster's coverage is, the texel's width across the
		// outline from the field's own slope, so it doesn't shimmer as the
		// screen's would
		if (u_MsdfSourceTexel.x > 0.0)
		{
			vec2 one = 1.0 / u_TextureSize;
			vec2 normal = vec2(texture2D(u_DiffuseMap, var_TexCoord + vec2(one.x, 0.0)).a - texel.a,
				texture2D(u_DiffuseMap, var_TexCoord + vec2(0.0, one.y)).a - texel.a);
			float slope = dot(normal, normal);
			float width = slope > 1e-8 ? length(u_MsdfSourceTexel * normal) * inversesqrt(slope)
				: 0.5 * (u_MsdfSourceTexel.x + u_MsdfSourceTexel.y);

			color.rgb *= smoothstep(-width, width, (texel.a - 0.5) * u_MsdfRange);
		}

		gl_FragColor = color * Msdf(Median(texel));
		return;
	}
#endif

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
	if (u_PaintKind == 3)
		color *= RoundedBox(var_TexCoord);
	else if (u_PaintKind >= 0)
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

		color *= Gradient(t);
	}
#endif

	gl_FragColor = color;
}
