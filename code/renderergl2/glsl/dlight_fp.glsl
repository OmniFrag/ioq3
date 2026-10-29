uniform int       u_AlphaTest;

#if defined(USE_PER_PIXEL)
varying vec3      var_LightDir;
varying vec3      var_Normal;
#else
uniform sampler2D u_DiffuseMap;

varying vec2      var_Tex1;
#endif
varying vec4      var_Color;

#if defined(USE_PER_PIXEL)
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
#endif

void main()
{
#if defined(USE_PER_PIXEL)
	// as lightall lights a surface with a dlight (USE_LIGHT_VECTOR): full
	// out to the radius over the square root of 3, none from the radius on,
	// and by the angle the light meets the surface at. var_LightDir is in
	// radii, so r^2 / d^2 is one over its square. The light is the alpha
	// too, so the alpha test drops what it doesn't reach, as the projected
	// shader drops what its vertex color doesn't
	// (not infinite at the light's origin, where 0 * inf would be NaN)
	float invLength = inversesqrt(max(dot(var_LightDir, var_LightDir), 1e-4));
	float attenuation = CalcLightAttenuation(1.0, invLength * invLength);
	vec3 N = normalize(!gl_FrontFacing ? var_Normal : -var_Normal);
	float NL = clamp(dot(N, var_LightDir) * invLength, 0.0, 1.0);
	vec4 color = vec4(attenuation * NL);
#else
	vec4 color = texture2D(u_DiffuseMap, var_Tex1);
#endif

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
	
	gl_FragColor.rgb = color.rgb * var_Color.rgb;
	gl_FragColor.a = alpha;
}
