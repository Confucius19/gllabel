#version 330 core
uniform samplerBuffer uGlyphData;
uniform mat4 uTransform;

layout(location = 0) in vec2 vPosition;
layout(location = 1) in uint vData;
layout(location = 2) in vec4 vColor;

out vec4 oColor;
flat out uint glyphDataOffset;
flat out ivec4 oGridRect;
out vec2 oNormCoord;

float ushortFromVec2(vec2 v)
{
	//the vec2 v was a uint16 on cpu memory
	//the gpu bitshift
	return (v.y * 65535.0 + v.x * 255.0);
}

ivec2 vec2FromPixel(uint offset)
{
	// RGBA* ->4*8 = 32 bits??
	vec4 pixel = texelFetch(uGlyphData, int(offset));
	return ivec2(ushortFromVec2(pixel.xy), ushortFromVec2(pixel.zw));
}

void main()
{
	oColor = vColor;
	glyphDataOffset = vData >> 2u;
	oNormCoord = vec2((vData & 2u) >> 1, vData & 1u);
	oGridRect = ivec4(vec2FromPixel(glyphDataOffset), vec2FromPixel(glyphDataOffset + 1u));
	//oGridRect.xy is origin in the grid atlas
	//oGridRect.zw is size of the grid
	gl_Position = uTransform*vec4(vPosition, 0.0, 1.0);
}