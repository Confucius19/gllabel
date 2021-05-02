// This shader slightly modified from source code by Will Dobbie.
// See dobbieText.cpp for more info.

#version 330 core
precision highp float;

#define numSS 4
#define pi 3.1415926535897932384626433832795
#define kPixelWindowSize 1.0

uniform sampler2D uGridAtlas;
uniform samplerBuffer uGlyphData;


in vec4 oColor;

flat in uint glyphDataOffset;

//oGridRect.xy is origin in the grid atlas
//oGridRect.zw is size of the grid
flat in ivec4 oGridRect;
//float between [0,1] that indicates where it is on the glyph's quad
in vec2 oNormCoord;

layout(location = 0) out vec4 outColor;

float positionAt(float p0, float p1, float p2, float t)
{
	float mt = 1.0 - t;
	return mt*mt*p0 + 2.0*t*mt*p1 + t*t*p2;
}

float tangentAt(float p0, float p1, float p2, float t)
{
	return 2.0 * (1.0-t) * (p1 - p0) + 2.0 * t * (p2 - p1);
}

bool almostEqual(float a, float b)
{
	return abs(a-b) < 1e-5;
}

float normalizedUshortFromVec2(vec2 v)
{
	return (v.y * 65280.0 + v.x * 255.0) / 65536.0;
}

vec4 getPixelByOffset(int offset)
{
	return texelFetch(uGlyphData, offset);
}

void fetchBezier(int coordIndex, out vec2 p[3])
{
	for (int i=0; i<3; i++) {
		vec4 pixel = getPixelByOffset(int(glyphDataOffset) + 2 + coordIndex*3 + i);
		p[i] = vec2(normalizedUshortFromVec2(pixel.xy), normalizedUshortFromVec2(pixel.zw)) - oNormCoord;
	}
}

int getAxisIntersections(float p0, float p1, float p2, out vec2 t)
{
	if (almostEqual(p0, 2.0*p1 - p2)) {
		t[0] = 0.5 * (p2 - 2.0*p1) / (p2 - p1);
		return 1;
	}

	float sqrtTerm = p1*p1 - p0*p2;
	if (sqrtTerm < 0.0) return 0;
	sqrtTerm = sqrt(sqrtTerm);
	float denom = p0 - 2.0*p1 + p2;
	t[0] = (p0 - p1 + sqrtTerm) / denom;
	t[1] = (p0 - p1 - sqrtTerm) / denom;
	return 2;
}

float integrateWindow(float x)
{
	float xsq = x*x;
	return sign(x) * (0.5 * xsq*xsq - xsq) + 0.5;  // parabolic window
	//return 0.5 * (1.0 - sign(x) * xsq);          // box window
}

//https://web.ma.utexas.edu/users/ysulyma/matrix/
//matrix rotates [1,0] to the difference between the vectors
mat2 getUnitLineMatrix(vec2 b1, vec2 b2)
{
	vec2 V = b2 - b1;
	float normV = length(V);
	V = V / (normV*normV);

	return mat2(V.x, -V.y, V.y, V.x);
}

ivec2 normalizedCoordToIntegerCell(vec2 ncoord)
{
	//oGridRect.zw is the width and height of the grid
	return clamp(ivec2(ncoord * oGridRect.zw), ivec2(0), oGridRect.zw - 1);
}

void updateClosestCrossing(in vec2 porig[3], mat2 M, inout float closest, ivec2 integerCell)
{
	vec2 p[3];
	for (int i=0; i<3; i++) {
		p[i] = M * porig[i];
	}

	vec2 t;
	int numT = getAxisIntersections(p[0].y, p[1].y, p[2].y, t);

	for (int i=0; i<2; i++) {
		if (i == numT) {
			break;
		}

		if (t[i] > 0.0 && t[i] < 1.0) {
			float posx = positionAt(p[0].x, p[1].x, p[2].x, t[i]);
			vec2 op = vec2(positionAt(porig[0].x, porig[1].x, porig[2].x, t[i]),
			               positionAt(porig[0].y, porig[1].y, porig[2].y, t[i]));
			op += oNormCoord;

			bool sameCell = normalizedCoordToIntegerCell(op) == integerCell;

			//if (posx > 0.0 && posx < 1.0 && posx < abs(closest)) {
			if (sameCell && abs(posx) < abs(closest)) {
				float derivy = tangentAt(p[0].y, p[1].y, p[2].y, t[i]);
				closest = (derivy < 0.0) ? -posx : posx;
			}
		}
	}
}

mat2 inverse(mat2 m)
{
	return mat2(m[1][1],-m[0][1], -m[1][0], m[0][0])
		/ (m[0][0]*m[1][1] - m[0][1]*m[1][0]);
}

void main()
{
	ivec2 integerCell = normalizedCoordToIntegerCell(oNormCoord);
	//oGridRect.xy bottom left of grid, 
	//pointer to cell that contains 0-4 beziers
	ivec2 indicesCoord = ivec2(oGridRect.xy + integerCell);
	vec2 cellMid = (integerCell + 0.5) / oGridRect.zw;

	//dF/dx of oNormCoord is units of pixels^-1 
	//the of the mat2 columns represent the normalized size of the pixel window 
	//this matrix should be diagonal and the final product could 
	//map normalized size -> pixel size
	mat2 initrot = inverse(mat2(dFdx(oNormCoord) * kPixelWindowSize, dFdy(oNormCoord) * kPixelWindowSize));

	// the angle to increment for each sample
	float theta = pi/float(numSS);

// note this is column major ordering
	mat2 rotM = mat2(cos(theta), sin(theta), -sin(theta), cos(theta)); 

	//fetch the indices into bezier array and bitshift left twice
	ivec4 indices1 = ivec4(texelFetch(uGridAtlas, indicesCoord, 0) * 255.0);

	// The mid-inside flag is encoded by the order of the beziers indices.
	// See write_vgrid_cell_to_buffer() for details.
	bool midInside = indices1[0] > indices1[1];

	float midClosest = midInside ? -2.0 : 2.0;

	float firstIntersection[numSS];
	for (int ss=0; ss<numSS; ss++) {
		firstIntersection[ss] = 2.0;
	}

	float percent = 0.0;

	mat2 midTransform = getUnitLineMatrix(oNormCoord, cellMid);

	for (int bezierIndex=0; bezierIndex<4; bezierIndex++) {
		int coordIndex;

		//if (bezierIndex < 4) {
			coordIndex = indices1[bezierIndex];
		//} else {
		//	 if (!moreThanFourIndices) break;
		//	 coordIndex = indices2[bezierIndex-4];
		//}

		// Indices 0 and 1 are both "no bezier" -- see
		// write_vgrid_cell_to_buffer() for why.
		if (coordIndex < 2) {
			continue;
		}

		vec2 p[3];
		fetchBezier(coordIndex-2, p);

		updateClosestCrossing(p, midTransform, midClosest, integerCell);

		// Transform p so fragment in glyph space is a unit circle
		for (int i=0; i<3; i++) {
			p[i] = initrot * p[i];
		}

		// Iterate through angles
		for (int ss=0; ss<numSS; ss++) {
			vec2 t;
			int numT = getAxisIntersections(p[0].x, p[1].x, p[2].x, t);

			for (int tindex=0; tindex<2; tindex++) {
				if (tindex == numT) break;

				if (t[tindex] > 0.0 && t[tindex] <= 1.0) {

					float derivx = tangentAt(p[0].x, p[1].x, p[2].x, t[tindex]);
					float posy = positionAt(p[0].y, p[1].y, p[2].y, t[tindex]);

					if (posy > -1.0 && posy < 1.0) {
						// Note: whether to add or subtract in the next statement is determined
						// by which convention the path uses: moving from the bezier start to end,
						// is the inside to the right or left?
						// The wrong operation will give buggy looking results, not a simple inverse.
						float delta = integrateWindow(posy);
						percent = percent + (derivx < 0.0 ? delta : -delta);

						float intersectDist = posy + 1.0;
						if (intersectDist < abs(firstIntersection[ss])) {
							firstIntersection[ss] = derivx < 0.0 ? -intersectDist : intersectDist;
						}
					}
				}
			}

			if (ss+1<numSS) {
				for (int i=0; i<3; i++) {
					p[i] = rotM * p[i];
				}
			}
		} // ss
	}

	bool midVal = midClosest < 0.0;

	// Add contribution from rays that started inside
	for (int ss=0; ss<numSS; ss++) {
		if ((firstIntersection[ss] >= 2.0 && midVal) || (firstIntersection[ss] > 0.0 && abs(firstIntersection[ss]) < 2.0)) {
			percent = percent + 1.0 /*integrateWindow(-1.0)*/;
		}
	}

	percent = percent / float(numSS);
	outColor = oColor;
	outColor.a *= percent;
}