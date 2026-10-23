attribute vec2 attr_Position;
attribute vec4 attr_TexCoord0;
attribute vec4 attr_Color;

// the 2D projection times the draw's transform (R_Transform2DMatrix)
uniform mat4   u_ModelViewProjectionMatrix;
// the vertex colors are premultiplied already
uniform int    u_VertexPremultiplied;
// the frame's light, as a picture's vertex colors get it
uniform float  u_ColorScale;

varying vec4   var_Color;
varying vec2   var_TexCoord;


void main()
{
	vec4 color = attr_Color;

	gl_Position = u_ModelViewProjectionMatrix * vec4(attr_Position, 0.0, 1.0);

	if (u_VertexPremultiplied == 0)
		color.rgb *= color.a;
	color.rgb *= u_ColorScale;

	var_Color = color;
	var_TexCoord = attr_TexCoord0.st;
}
