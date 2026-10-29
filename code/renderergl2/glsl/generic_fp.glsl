uniform sampler2D u_DiffuseMap;

uniform int       u_AlphaTest;

varying vec2      var_DiffuseTex;

varying vec4      var_Color;

#if defined(USE_DLIGHT)
// the dlights on the surface, two vec4s each: the origin, in the model's
// space, and 1 / radius; the color. A stage's light takes them: a
// lightmap's (u_DlightTo 1) or its vertex color's (2), so the stages after
// it apply the surface's colors to them as to the lightmap

// the position and the origins at high precision where a fragment's float
// is medium: their difference in world units, thousands, would step by 2-8
#if defined(GL_ES) && defined(GL_FRAGMENT_PRECISION_HIGH)
#define DLIGHT_PRECISION highp
#else
#define DLIGHT_PRECISION
#endif

uniform int       u_NumDlights;
uniform DLIGHT_PRECISION vec4 u_Dlights[2 * MAX_GENERIC_DLIGHTS];
uniform int       u_DlightTo;

varying DLIGHT_PRECISION vec3 var_Position;
varying vec3      var_Normal;

// lightall's, with which ForwardDlight lights the other surfaces
float CalcLightAttenuation(float point, float normDist)
{
	// zero light at 1.0, approximating q3 style
	// also don't attenuate directional light
	float attenuation = (0.5 * normDist - 1.5) * point + 1.0;

	// clamp attenuation
	attenuation = clamp(attenuation, 0.0, 1.0);

	return attenuation;
}

vec3 CalcDlights()
{
	vec3 N = normalize(!gl_FrontFacing ? var_Normal : -var_Normal);
	vec3 light = vec3(0.0);

	for (int i = 0; i < MAX_GENERIC_DLIGHTS; i++)
	{
		if (i >= u_NumDlights)
			break;

		// in radii, so the square stays in range at medium precision
		vec3 L = (u_Dlights[2 * i].xyz - var_Position) * u_Dlights[2 * i].w;
		// not infinite at the light's origin, where 0 * inf would be NaN
		float invLength = inversesqrt(max(dot(L, L), 1e-4));
		float attenuation = CalcLightAttenuation(1.0, invLength * invLength);
		float NL = clamp(dot(N, L) * invLength, 0.0, 1.0);
		light += u_Dlights[2 * i + 1].rgb * (attenuation * NL);
	}

	return light;
}
#endif


void main()
{
	vec4 color  = texture2D(u_DiffuseMap, var_DiffuseTex);

	float alpha = color.a * var_Color.a;
	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}
	
#if defined(USE_DLIGHT)
	vec3 light = CalcDlights();
	if (u_DlightTo == 1)
		gl_FragColor.rgb = (color.rgb + light) * var_Color.rgb;
	else // beside a vertex color, scaled as rgbGen vertex scales that light
		gl_FragColor.rgb = color.rgb * (var_Color.rgb + light * DLIGHT_VERTEX_SCALE);
#else
	gl_FragColor.rgb = color.rgb * var_Color.rgb;
#endif
	gl_FragColor.a = alpha;
}
