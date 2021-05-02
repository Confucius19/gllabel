/*
 * zelbrium <zelbrium@gmail.com>, 2016-2020.
 *
 * This code is based on Will Dobbie's WebGL vector-based text rendering (2016).
 * It can be found here:
 * https://wdobbie.com/post/gpu-text-rendering-with-vector-textures/
 *
 * Dobbie's original code used a pre-generated bezier curve atlas generated
 * from a PDF. This GLLabel class allows for live text rendering based on
 * glyph curves exported from FreeType2.
 *
 * Text is rendered size-independently. This means you can scale, rotate,
 * or reposition text rendered using GLLabel without any loss of quality.
 * All that's required is a font file to load the text from. Currently, any TTF
 * font that does not use cubic beziers or make use of very detailed glyphs,
 * such as many Hanzi / Kanji characters, should work.
 */

#include <gllabel.hpp>
#include "vgrid.hpp"
#include "outline.hpp"
#include <set>
#include <fstream>
#include <iostream>
#include <string>
#include <glm/gtc/type_ptr.hpp>

#define sq(x) ((x)*(x))

static GLuint loadShaderProgram(const char* vPath, const char* fPath);

std::shared_ptr<GLFontManager> GLFontManager::singleton = nullptr;

static const char* kGlyphVertexShaderPath = "./shaders/glyphVertex.glsl";
static const char* kGlyphFragmentShaderPath = "./shaders/glyphFragment.glsl";

static const uint8_t kGridMaxSize = 20;
static const uint16_t kGridAtlasSize = 256; // Fits exactly 1024 8x8 grids
static const uint16_t kBezierAtlasSize = 256; // Fits around 700-1000 glyphs, depending on their curves
static const uint8_t kAtlasChannels = 4; // Must be 4 (RGBA), otherwise code breaks

GLLabel::GLLabel()
	: showingCaret(false), caretPosition(0), prevTime(0) {
	// this->lastColor = {0,0,0,255};
	this->manager = GLFontManager::GetFontManager();
	// this->lastFace = this->manager->GetDefaultFont();
	// this->manager->LoadASCII(this->lastFace);

	glGenBuffers(1, &this->vertBuffer);
	glGenBuffers(1, &this->caretBuffer);
}

GLLabel::~GLLabel() {
	glDeleteBuffers(1, &this->vertBuffer);
	glDeleteBuffers(1, &this->caretBuffer);
}

void GLLabel::InsertText(std::u32string text, size_t index, glm::vec4 color, FT_Face face) {
	if (index > this->text.size()) {
		index = this->text.size();
	}

	this->text.insert(index, text);
	this->glyphs.insert(this->glyphs.begin() + index, text.size(), nullptr);

	size_t prevCapacity = this->verts.capacity();
	GlyphVertex emptyVert{};
	this->verts.insert(this->verts.begin() + index * 6, text.size() * 6, emptyVert);

	glm::vec2 appendOffset(0, 0);
	if (index > 0) {
		appendOffset = this->verts[(index - 1) * 6].pos;
		if (this->glyphs[index - 1]) {
			appendOffset += -glm::vec2(this->glyphs[index - 1]->offset[0], this->glyphs[index - 1]->offset[1]) + glm::vec2(this->glyphs[index - 1]->advance, 0);
		}
	}
	glm::vec2 initialAppendOffset = appendOffset;

	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\r') {
			this->verts[(index + i) * 6].pos = appendOffset;
			continue;
		}
		else if (text[i] == '\n') {
			appendOffset.x = 0;
			appendOffset.y -= face->height;
			this->verts[(index + i) * 6].pos = appendOffset;
			continue;
		}
		else if (text[i] == '\t') {
			appendOffset.x += 2000;
			this->verts[(index + i) * 6].pos = appendOffset;
			continue;
		}

		GLFontManager::Glyph* glyph = this->manager->GetGlyphForCodepoint(face, text[i]);
		if (!glyph) {
			this->verts[(index + i) * 6].pos = appendOffset;
			continue;
		}

		GlyphVertex v[6]{}; // Insertion code depends on v[0] equaling appendOffset (therefore it is also set before continue;s above)
		v[0].pos = glm::vec2(0, 0);
		v[1].pos = glm::vec2(glyph->size[0], 0);
		v[2].pos = glm::vec2(0, glyph->size[1]);
		v[3].pos = glm::vec2(glyph->size[0], glyph->size[1]);
		v[4].pos = glm::vec2(0, glyph->size[1]);
		v[5].pos = glm::vec2(glyph->size[0], 0);
		for (unsigned int j = 0; j < 6; j++) {
			v[j].pos += appendOffset;
			v[j].pos[0] += glyph->offset[0];
			v[j].pos[1] += glyph->offset[1];

			v[j].color = { (uint8_t)(color.r * 255), (uint8_t)(color.g * 255), (uint8_t)(color.b * 255), (uint8_t)(color.a * 255) };

			// Encode both the bezier position and the norm coord into one int
			// This theoretically could overflow, but the atlas position will
			// never be over half the size of a uint16, so it's fine.
			unsigned int k = (j < 4) ? j : 6 - j;
			unsigned int normX = k & 1;
			unsigned int normY = k > 1;
			unsigned int norm = (normX << 1) + normY;
			v[j].data = (glyph->bezierAtlasPos[0] << 2) + norm;
			this->verts[(index + i) * 6 + j] = v[j];
		}

		appendOffset.x += glyph->advance;
		this->glyphs[index + i] = glyph;
	}

	// Shift everything after, if necessary
	glm::vec2 deltaAppend = appendOffset - initialAppendOffset;
	for (size_t i = index + text.size(); i < this->text.size(); i++) {
		// If a newline is reached and no change in the y has happened, all
		// glyphs which need to be moved have been moved.
		if (this->text[i] == '\n') {
			if (deltaAppend.y == 0) {
				break;
			}
			if (deltaAppend.x < 0) {
				deltaAppend.x = 0;
			}
		}

		for (unsigned int j = 0; j < 6; j++) {
			this->verts[i * 6 + j].pos += deltaAppend;
		}
	}

	glBindBuffer(GL_ARRAY_BUFFER, this->vertBuffer);

	if (this->verts.capacity() != prevCapacity) {
		// If the capacity changed, completely reupload the buffer
		glBufferData(GL_ARRAY_BUFFER, this->verts.capacity() * sizeof(GlyphVertex), NULL, GL_DYNAMIC_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, this->verts.size() * sizeof(GlyphVertex), &this->verts[0]);
	}
	else {
		// Otherwise only upload the changed parts
		glBufferSubData(GL_ARRAY_BUFFER,
			index * 6 * sizeof(GlyphVertex),
			(this->verts.size() - index * 6) * sizeof(GlyphVertex),
			&this->verts[index * 6]);
	}
	caretTime = 0;
}

void GLLabel::RemoveText(size_t index, size_t length) {
	if (index >= this->text.size()) {
		return;
	}
	if (index + length > this->text.size()) {
		length = this->text.size() - index;
	}

	glm::vec2 startOffset(0, 0);
	if (index > 0) {
		startOffset = this->verts[(index - 1) * 6].pos;
		if (this->glyphs[index - 1]) {
			startOffset += -glm::vec2(this->glyphs[index - 1]->offset[0], this->glyphs[index - 1]->offset[1]) + glm::vec2(this->glyphs[index - 1]->advance, 0);
		}
	}

	// Since all the glyphs between index-1 and index+length have been erased,
	// the end offset will be at index until it gets shifted back
	glm::vec2 endOffset(0, 0);
	// if (this->glyphs[index+length-1])
	// {
	endOffset = this->verts[index * 6].pos;
	if (this->glyphs[index + length - 1]) {
		endOffset += -glm::vec2(this->glyphs[index + length - 1]->offset[0], this->glyphs[index + length - 1]->offset[1]) + glm::vec2(this->glyphs[index + length - 1]->advance, 0);
	}
	// }

	this->text.erase(index, length);
	this->glyphs.erase(this->glyphs.begin() + index, this->glyphs.begin() + (index + length));
	this->verts.erase(this->verts.begin() + index * 6, this->verts.begin() + (index + length) * 6);

	glm::vec2 deltaOffset = endOffset - startOffset;
	// Shift everything after, if necessary
	for (size_t i = index; i < this->text.size(); i++) {
		if (this->text[i] == '\n') {
			deltaOffset.x = 0;
		}

		for (unsigned int j = 0; j < 6; j++) {
			this->verts[i * 6 + j].pos -= deltaOffset;
		}
	}

	glBindBuffer(GL_ARRAY_BUFFER, this->vertBuffer);
	if (this->verts.size() > 0) {
		glBufferSubData(GL_ARRAY_BUFFER,
			index * 6 * sizeof(GlyphVertex),
			(this->verts.size() - index * 6) * sizeof(GlyphVertex),
			&this->verts[index * 6]);
	}

	caretTime = 0;
}

void GLLabel::Render(float time, glm::mat4 transform) {
	float deltaTime = time - prevTime;
	this->caretTime += deltaTime;

	this->manager->UseGlyphShader();
	this->manager->UploadAtlases();
	this->manager->UseAtlasTextures(0); // TODO: Textures based on each glyph
	this->manager->SetShaderTransform(transform);

	glEnable(GL_BLEND);
	glBindBuffer(GL_ARRAY_BUFFER, this->vertBuffer);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLLabel::GlyphVertex), (void*)offsetof(GLLabel::GlyphVertex, pos));
	glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(GLLabel::GlyphVertex), (void*)offsetof(GLLabel::GlyphVertex, data));
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLLabel::GlyphVertex), (void*)offsetof(GLLabel::GlyphVertex, color));

	glDrawArrays(GL_TRIANGLES, 0, this->verts.size());

	if (this->showingCaret && !(((int)(this->caretTime * 3 / 2)) % 2)) {
		GLFontManager::Glyph* pipe = this->manager->GetGlyphForCodepoint(this->manager->GetDefaultFont(), '|');

		size_t index = this->caretPosition;

		glm::vec2 offset(0, 0);
		if (index > 0) {
			offset = this->verts[(index - 1) * 6].pos;
			if (this->glyphs[index - 1]) {
				offset += -glm::vec2(this->glyphs[index - 1]->offset[0], this->glyphs[index - 1]->offset[1]) + glm::vec2(this->glyphs[index - 1]->advance, 0);
			}
		}

		GlyphVertex x[6]{};
		x[0].pos = glm::vec2(0, 0);
		x[1].pos = glm::vec2(pipe->size[0], 0);
		x[2].pos = glm::vec2(0, pipe->size[1]);
		x[3].pos = glm::vec2(pipe->size[0], pipe->size[1]);
		x[4].pos = glm::vec2(0, pipe->size[1]);
		x[5].pos = glm::vec2(pipe->size[0], 0);
		for (unsigned int j = 0; j < 6; j++) {
			x[j].pos += offset;
			x[j].pos[0] += pipe->offset[0];
			x[j].pos[1] += pipe->offset[1];

			x[j].color = { 0,0,255,100 };

			// Encode both the bezier position and the norm coord into one int
			// This theoretically could overflow, but the atlas position will
			// never be over half the size of a uint16, so it's fine.
			unsigned int k = (j < 4) ? j : 6 - j;
			unsigned int normX = k & 1;
			unsigned int normY = k > 1;
			unsigned int norm = (normX << 1) + normY;
			x[j].data = (pipe->bezierAtlasPos[0] << 2) + norm;
			// this->verts[(index + i)*6 + j] = v[j];
		}

		glBindBuffer(GL_ARRAY_BUFFER, this->caretBuffer);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLLabel::GlyphVertex), (void*)offsetof(GLLabel::GlyphVertex, pos));
		glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(GLLabel::GlyphVertex), (void*)offsetof(GLLabel::GlyphVertex, data));
		glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLLabel::GlyphVertex), (void*)offsetof(GLLabel::GlyphVertex, color));

		glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(GlyphVertex), &x[0], GL_STREAM_DRAW);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisable(GL_BLEND);
	prevTime = time;
}


GLFontManager::GLFontManager() : defaultFace(nullptr) {
	if (FT_Init_FreeType(&this->ft) != FT_Err_Ok) {
		std::cerr << "Failed to load freetype\n";
	}

	this->glyphShader = loadShaderProgram(kGlyphVertexShaderPath, kGlyphFragmentShaderPath);
	this->uGridAtlas = glGetUniformLocation(glyphShader, "uGridAtlas");
	this->uGlyphData = glGetUniformLocation(glyphShader, "uGlyphData");
	this->uTransform = glGetUniformLocation(glyphShader, "uTransform");

	this->UseGlyphShader();
	glUniform1i(this->uGridAtlas, 0);
	glUniform1i(this->uGlyphData, 1);

	glm::mat4 iden = glm::mat4(1.0);
	glUniformMatrix4fv(this->uTransform, 1, GL_FALSE, glm::value_ptr(iden));
}

GLFontManager::~GLFontManager() {
	// TODO: Destroy atlases
	glDeleteProgram(this->glyphShader);
	FT_Done_FreeType(this->ft);
}

std::shared_ptr<GLFontManager> GLFontManager::GetFontManager() {
	if (!GLFontManager::singleton) {
		GLFontManager::singleton = std::shared_ptr<GLFontManager>(new GLFontManager());
	}
	return GLFontManager::singleton;
}

// TODO: FT_Faces don't get destroyed... FT_Done_FreeType cleans them eventually,
// but maybe use shared pointers?
FT_Face GLFontManager::GetFontFromPath(std::string fontPath) {
	FT_Face face;
	return FT_New_Face(this->ft, fontPath.c_str(), 0, &face) ? nullptr : face;
}

FT_Face GLFontManager::GetFontFromName(std::string fontName) {
	std::string path = fontName; // TODO
	return GLFontManager::GetFontFromPath(path);
}

FT_Face GLFontManager::GetDefaultFont() {
	// TODO
	if (!defaultFace) {
		defaultFace = GLFontManager::GetFontFromPath("fonts/LiberationSans-Regular.ttf");
	}
	return defaultFace;
}

GLFontManager::AtlasGroup* GLFontManager::GetOpenAtlasGroup() {
	if (this->atlases.size() == 0 || this->atlases[this->atlases.size() - 1].full) {
		AtlasGroup group{};
		group.glyphDataBuf = new uint8_t[sq(kBezierAtlasSize) * kAtlasChannels]();
		group.gridAtlas = new uint8_t[sq(kGridAtlasSize) * kAtlasChannels]();
		group.uploaded = true;

		// https://www.khronos.org/opengl/wiki/Buffer_Texture
		// TODO: Check GL_MAX_TEXTURE_BUFFER_SIZE
		glGenBuffers(1, &group.glyphDataBufId);
		glBindBuffer(GL_TEXTURE_BUFFER, group.glyphDataBufId);
		glGenTextures(1, &group.glyphDataBufTexId);
		glBindTexture(GL_TEXTURE_BUFFER, group.glyphDataBufTexId);
		glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, group.glyphDataBufId);

		glGenTextures(1, &group.gridAtlasId);
		glBindTexture(GL_TEXTURE_2D, group.gridAtlasId);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kGridAtlasSize, kGridAtlasSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, group.gridAtlas);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

		this->atlases.push_back(group);
	}

	return &this->atlases[this->atlases.size() - 1];
}

#pragma pack(push, 1)
struct bitmapdata {
	char magic[2];
	uint32_t size;
	uint16_t res1;
	uint16_t res2;
	uint32_t offset;

	uint32_t biSize;
	uint32_t width;
	uint32_t height;
	uint16_t planes;
	uint16_t bitCount;
	uint32_t compression;
	uint32_t imageSizeBytes;
	uint32_t xpelsPerMeter;
	uint32_t ypelsPerMeter;
	uint32_t clrUsed;
	uint32_t clrImportant;
};
#pragma pack(pop)

void writeBMP(const char* path, uint32_t width, uint32_t height, uint16_t channels, uint8_t* data) {
	FILE* f = fopen(path, "wb");

	bitmapdata head;
	head.magic[0] = 'B';
	head.magic[1] = 'M';
	head.size = sizeof(bitmapdata) + width * height * channels;
	head.res1 = 0;
	head.res2 = 0;
	head.offset = sizeof(bitmapdata);
	head.biSize = 40;
	head.width = width;
	head.height = height;
	head.planes = 1;
	head.bitCount = 8 * channels;
	head.compression = 0;
	head.imageSizeBytes = width * height * channels;
	head.xpelsPerMeter = 0;
	head.ypelsPerMeter = 0;
	head.clrUsed = 0;
	head.clrImportant = 0;

	fwrite(&head, sizeof(head), 1, f);
	fwrite(data, head.imageSizeBytes, 1, f);
	fclose(f);
}

// A bezier is written as 6 16-bit integers (12 bytes). Increments buffer by
// the number of bytes written (always 12). Coords are scaled from
// [0,glyphSize] to [0,UINT16_MAX].
void write_bezier_to_buffer(uint16_t** pbuffer, Bezier2* bezier, Vec2* glyphSize) {
	uint16_t* buffer = *pbuffer;
	buffer[0] = bezier->e0.x * UINT16_MAX / glyphSize->w;
	buffer[1] = bezier->e0.y * UINT16_MAX / glyphSize->h;
	buffer[2] = bezier->c.x * UINT16_MAX / glyphSize->w;
	buffer[3] = bezier->c.y * UINT16_MAX / glyphSize->h;
	buffer[4] = bezier->e1.x * UINT16_MAX / glyphSize->w;
	buffer[5] = bezier->e1.y * UINT16_MAX / glyphSize->h;
	*pbuffer += 6;
}

void write_glyph_data_to_buffer(
	uint8_t* buffer8,
	std::vector<Bezier2>& beziers,
	Vec2& glyphSize,
	uint16_t gridX,
	uint16_t gridY,
	uint16_t gridWidth,
	uint16_t gridHeight) {

	uint16_t* buffer = (uint16_t*)buffer8;
	buffer[0] = gridX;
	buffer[1] = gridY;
	buffer[2] = gridWidth;
	buffer[3] = gridHeight;
	buffer += 4;

	for (size_t i = 0; i < beziers.size(); i++) {
		write_bezier_to_buffer(&buffer, &beziers[i], &glyphSize);
	}
}

GLFontManager::Glyph* GLFontManager::GetGlyphForCodepoint(FT_Face face, uint32_t point) {
	auto faceIt = this->glyphs.find(face);
	if (faceIt != this->glyphs.end()) {
		auto glyphIt = faceIt->second.find(point);
		if (glyphIt != faceIt->second.end()) {
			return &glyphIt->second;
		}
	}

	AtlasGroup* atlas = this->GetOpenAtlasGroup();

	// Load the glyph. FT_LOAD_NO_SCALE implies that FreeType should not
	// render the glyph to a bitmap, and ensures that metrics and outline
	// points are represented in font units instead of em.
	FT_UInt glyphIndex = FT_Get_Char_Index(face, point);
	if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE)) {
		return nullptr;
	}

	FT_Pos glyphWidth = face->glyph->metrics.width;
	FT_Pos glyphHeight = face->glyph->metrics.height;
	uint8_t gridWidth = kGridMaxSize;
	uint8_t gridHeight = kGridMaxSize;

	std::vector<Bezier2> curves = GetBeziersForOutline(&face->glyph->outline);
	VGrid grid(curves, Vec2(glyphWidth, glyphHeight), gridWidth, gridHeight);

	// Although the data is represented as a 32bit texture, it's actually
	// two 16bit ints per pixel, each with an x and y coordinate for
	// the bezier. Every six 16bit ints (3 pixels) is a full bezier
	// Plus two pixels for grid position information
	uint16_t bezierPixelLength = 2 + curves.size() * 3;

	bool tooManyCurves = uint32_t(bezierPixelLength) > sq(uint32_t(kBezierAtlasSize));

	if (curves.size() == 0 || tooManyCurves) {
		if (tooManyCurves) {
			std::cerr << "WARN: Glyph " << point << " has too many curves\n";
		}

		GLFontManager::Glyph glyph{};
		glyph.bezierAtlasPos[1] = -1;
		glyph.size[0] = glyphWidth;
		glyph.size[1] = glyphHeight;
		glyph.offset[0] = face->glyph->metrics.horiBearingX;
		glyph.offset[1] = face->glyph->metrics.horiBearingY - glyphHeight;
		glyph.advance = face->glyph->metrics.horiAdvance;
		this->glyphs[face][point] = glyph;
		return &this->glyphs[face][point];
	}

	// Find an open position in the bezier atlas
	if (atlas->glyphDataBufOffset + bezierPixelLength > sq(kBezierAtlasSize)) {
		atlas->full = true;
		atlas->uploaded = false;
		atlas = this->GetOpenAtlasGroup();
	}

	// Find an open position in the grid atlas
	if (atlas->nextGridPos[0] + kGridMaxSize > kGridAtlasSize) {
		atlas->nextGridPos[1] += kGridMaxSize;
		atlas->nextGridPos[0] = 0;
		if (atlas->nextGridPos[1] >= kGridAtlasSize) {
			atlas->full = true;
			atlas->uploaded = false;
			atlas = this->GetOpenAtlasGroup(); // Should only ever happen once per glyph
		}
	}

	uint8_t* bezierData = atlas->glyphDataBuf + (atlas->glyphDataBufOffset * kAtlasChannels);

	Vec2 glyphSize(glyphWidth, glyphHeight);
	write_glyph_data_to_buffer(
		bezierData,
		curves,
		glyphSize,
		atlas->nextGridPos[0], // pos of grid within atlas?
		atlas->nextGridPos[1],
		kGridMaxSize, //size of vGrid
		kGridMaxSize);

	// TODO: Integrate with AtlasGroup / replace AtlasGroup
	VGridAtlas gridAtlas{};
	gridAtlas.data = atlas->gridAtlas;
	gridAtlas.width = kGridAtlasSize;
	gridAtlas.height = kGridAtlasSize;
	gridAtlas.depth = kAtlasChannels;
	gridAtlas.WriteVGridAt(grid, atlas->nextGridPos[0], atlas->nextGridPos[1]);

	GLFontManager::Glyph glyph{};
	glyph.bezierAtlasPos[0] = atlas->glyphDataBufOffset;
	glyph.bezierAtlasPos[1] = this->atlases.size() - 1;
	glyph.size[0] = glyphWidth;
	glyph.size[1] = glyphHeight;
	glyph.offset[0] = face->glyph->metrics.horiBearingX;
	glyph.offset[1] = face->glyph->metrics.horiBearingY - glyphHeight;
	glyph.advance = face->glyph->metrics.horiAdvance;
	this->glyphs[face][point] = glyph;

	atlas->glyphDataBufOffset += bezierPixelLength;
	atlas->nextGridPos[0] += kGridMaxSize;
	atlas->uploaded = false;

	// writeBMP("bezierAtlas.bmp", kBezierAtlasSize, kBezierAtlasSize, 4, atlas->glyphDataBuf);
	// writeBMP("gridAtlas.bmp", kGridAtlasSize, kGridAtlasSize, 4, atlas->gridAtlas);

	return &this->glyphs[face][point];
}

void GLFontManager::LoadASCII(FT_Face face) {
	if (!face) {
		return;
	}

	this->GetGlyphForCodepoint(face, 0);

	for (int i = 32; i < 128; i++) {
		this->GetGlyphForCodepoint(face, i);
	}
}

void GLFontManager::UploadAtlases() {
	for (size_t i = 0; i < this->atlases.size(); i++) {
		if (this->atlases[i].uploaded) {
			continue;
		}

		glBindBuffer(GL_TEXTURE_BUFFER, this->atlases[i].glyphDataBufId);
		glBufferData(GL_TEXTURE_BUFFER, sq(kBezierAtlasSize) * kAtlasChannels,
			this->atlases[i].glyphDataBuf, GL_STREAM_DRAW);

		glBindTexture(GL_TEXTURE_2D, this->atlases[i].gridAtlasId);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kGridAtlasSize, kGridAtlasSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, this->atlases[i].gridAtlas);

		atlases[i].uploaded = true;
	}
}

void GLFontManager::UseGlyphShader() {
	glUseProgram(this->glyphShader);
}

void GLFontManager::SetShaderTransform(glm::mat4 transform) {
	glUniformMatrix4fv(this->uTransform, 1, GL_FALSE, glm::value_ptr(transform));
}

void GLFontManager::UseAtlasTextures(uint16_t atlasIndex) {
	if (atlasIndex >= this->atlases.size()) {
		return;
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, this->atlases[atlasIndex].gridAtlasId);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_BUFFER, this->atlases[atlasIndex].glyphDataBufTexId);

}

static GLuint loadShaderProgram(const char* vertexPath, const char* fragPath) {
	//load vertex and fragment shaders from files
	std::ifstream vertexShaderFile(vertexPath, std::ios::in | std::ios::ate);
	if (!vertexShaderFile.is_open()) {
		std::cerr << "Failed to Open Vertex Shader File" << std::endl;
		return -1;
	}
	std::streampos size = vertexShaderFile.tellg();
	char* const vsCodeC = new char[size];
	vertexShaderFile.seekg(0, std::ios::beg);
	vertexShaderFile.read(vsCodeC, size);
	vertexShaderFile.close();

	std::ifstream fragmentShaderFile(fragPath, std::ios::in | std::ios::ate);
	if (!fragmentShaderFile.is_open()) {
		std::cerr << "Failed to Open Fragment Shader File" << std::endl;
		return -1;
	}
	size = fragmentShaderFile.tellg();
	char* const fsCodeC = new char[size];
	fragmentShaderFile.seekg(0, std::ios::beg);
	fragmentShaderFile.read(fsCodeC, size);
	fragmentShaderFile.close();

	// Compile vertex shader
	GLuint vertexShaderId = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vertexShaderId, 1, &vsCodeC, NULL);
	glCompileShader(vertexShaderId);

	GLint result = GL_FALSE;
	int infoLogLength = 0;
	glGetShaderiv(vertexShaderId, GL_COMPILE_STATUS, &result);
	glGetShaderiv(vertexShaderId, GL_INFO_LOG_LENGTH, &infoLogLength);
	if (infoLogLength > 1) {
		std::vector<char> infoLog(infoLogLength + 1);
		glGetShaderInfoLog(vertexShaderId, infoLogLength, NULL, &infoLog[0]);
		std::cerr << "[Vertex] " << &infoLog[0] << "\n";
	}
	if (!result) {
		return 0;
	}

	// Compile fragment shader
	GLuint fragmentShaderId = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fragmentShaderId, 1, &fsCodeC, NULL);
	glCompileShader(fragmentShaderId);

	result = GL_FALSE, infoLogLength = 0;
	glGetShaderiv(fragmentShaderId, GL_COMPILE_STATUS, &result);
	glGetShaderiv(fragmentShaderId, GL_INFO_LOG_LENGTH, &infoLogLength);
	if (infoLogLength > 1) {
		std::vector<char> infoLog(infoLogLength);
		glGetShaderInfoLog(fragmentShaderId, infoLogLength, NULL, &infoLog[0]);
		std::cerr << "[Fragment] " << &infoLog[0] << "\n";
	}
	if (!result) {
		return 0;
	}

	// Link the program
	GLuint programId = glCreateProgram();
	glAttachShader(programId, vertexShaderId);
	glAttachShader(programId, fragmentShaderId);
	glLinkProgram(programId);

	result = GL_FALSE, infoLogLength = 0;
	glGetProgramiv(programId, GL_LINK_STATUS, &result);
	glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &infoLogLength);
	if (infoLogLength > 1) {
		std::vector<char> infoLog(infoLogLength + 1);
		glGetProgramInfoLog(programId, infoLogLength, NULL, &infoLog[0]);
		std::cerr << "[Shader Linker] " << &infoLog[0] << "\n";
	}
	if (!result) {
		return 0;
	}

	glDetachShader(programId, vertexShaderId);
	glDetachShader(programId, fragmentShaderId);

	glDeleteShader(vertexShaderId);
	glDeleteShader(fragmentShaderId);
	delete vsCodeC;
	delete fsCodeC;

	return programId;
}
