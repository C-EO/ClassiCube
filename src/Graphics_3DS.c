#include "Core.h"
#if defined CC_BUILD_3DS
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Logger.h"
#include "Window.h"
#include <3ds.h>
#include <citro3d.h>

// autogenerated from the .v.pica files by Makefile
extern const u8  _3DS_coloured_shbin[];
extern const u32 _3DS_coloured_shbin_size;

extern const u8  _3DS_textured_shbin[];
extern const u32 _3DS_textured_shbin_size;

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// Current format and size of vertices
static int gfx_stride, gfx_format = -1;
	
	
/*########################################################################################################################*
*------------------------------------------------------Vertex shaders-----------------------------------------------------*
*#########################################################################################################################*/
#define UNI_MVP_MATRIX (1 << 0)
// cached uniforms (cached for multiple programs)
static C3D_Mtx _mvp;

static struct CCShader {
	DVLB_s* dvlb;
	shaderProgram_s program;
	int uniforms;     // which associated uniforms need to be resent to GPU
	int locations[1]; // location of uniforms (not constant)
};
static struct CCShader* gfx_activeShader;
static struct CCShader shaders[2];

static void Shader_Alloc(struct CCShader* shader, u8* binData, int binSize) {
	shader->dvlb = DVLB_ParseFile((u32*)binData, binSize);
	shaderProgramInit(&shader->program);
	shaderProgramSetVsh(&shader->program, &shader->dvlb->DVLE[0]);
	
	shader->locations[0] = shaderInstanceGetUniformLocation(shader->program.vertexShader, "MVP");
}

static void Shader_Free(struct CCShader* shader) {
	shaderProgramFree(&shader->program);
	DVLB_Free(shader->dvlb);
}

// Marks a uniform as changed on all programs
static void DirtyUniform(int uniform) {
	for (int i = 0; i < Array_Elems(shaders); i++) 
	{
		shaders[i].uniforms |= uniform;
	}
}

// Sends changed uniforms to the GPU for current program
static void ReloadUniforms(void) {
	struct CCShader* s = gfx_activeShader;
	if (!s) return; // NULL if context is lost

	if (s->uniforms & UNI_MVP_MATRIX) {
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s->locations[0], &_mvp);
		s->uniforms &= ~UNI_MVP_MATRIX;
	}
}
static void ReloadUniforms(void);

// Switches program to one that can render current vertex format and state
// Loads program and reloads uniforms if needed
static void SwitchProgram(void) {
	struct CCShader* shader;
	int index = 0;

	if (gfx_format == VERTEX_FORMAT_TEXTURED) index++;

	shader = &shaders[index];
	if (shader != gfx_activeShader) {
		gfx_activeShader = shader;
		C3D_BindProgram(&shader->program);
	}
	ReloadUniforms();
}


/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
static C3D_RenderTarget* target;

static void AllocShaders(void) {
	Shader_Alloc(&shaders[0], (u32*)_3DS_coloured_shbin, _3DS_coloured_shbin_size);
	Shader_Alloc(&shaders[1], (u32*)_3DS_textured_shbin, _3DS_textured_shbin_size);
}

static void FreeShaders(void) {
	for (int i = 0; i < Array_Elems(shaders); i++) {
		Shader_Free(&shaders[i]);
	}
}

static void SetDefaultState(void) {
	Gfx_SetFaceCulling(false);
	Gfx_SetAlphaTest(false);
	Gfx_SetDepthWrite(true);
}

static GfxResourceID white_square;
void Gfx_Create(void) {
	Gfx.MaxTexWidth  = 512;
	Gfx.MaxTexHeight = 512;
	Gfx.Created      = true;
	
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	SetDefaultState();
	InitDefaultResources();
	AllocShaders();
 	
	// 8x8 dummy white texture
	//  (textures must be at least 8x8, see C3D_TexInitWithParams source)
	struct Bitmap bmp;
	BitmapCol pixels[8*8];
	Mem_Set(pixels, 0xFF, sizeof(pixels));
	Bitmap_Init(bmp, 8, 8, pixels);
	white_square = Gfx_CreateTexture(&bmp, 0, false);
}

void Gfx_Free(void) { 
	FreeShaders();
	FreeDefaultResources(); 
	Gfx_DeleteTexture(&white_square);
}

cc_bool Gfx_TryRestoreContext(void) { return true; }
void Gfx_RestoreState(void) { }
void Gfx_FreeState(void) { }

/*########################################################################################################################*
*---------------------------------------------------------Textures--------------------------------------------------------*
*#########################################################################################################################*/
/*static inline cc_uint32 CalcZOrder(cc_uint32 x, cc_uint32 y) {
	// Simplified "Interleave bits by Binary Magic Numbers" from
	// http://graphics.stanford.edu/~seander/bithacks.html#InterleaveTableObvious
	// TODO: Simplify to array lookup?
    	x = (x | (x << 2)) & 0x33;
    	x = (x | (x << 1)) & 0x55;

    	y = (y | (y << 2)) & 0x33;
    	y = (y | (y << 1)) & 0x55;

    return x | (y << 1);
}*/
static inline cc_uint32 CalcZOrder(cc_uint32 a) {
	// Simplified "Interleave bits by Binary Magic Numbers" from
	// http://graphics.stanford.edu/~seander/bithacks.html#InterleaveTableObvious
	// TODO: Simplify to array lookup?
    	a = (a | (a << 2)) & 0x33;
    	a = (a | (a << 1)) & 0x55;
    	return a;
    	// equivalent to return (a & 1) | ((a & 2) << 1) | (a & 4) << 2;
    	//  but compiles to less instructions
}

// Pixels are arranged in a recursive Z-order curve / Morton offset
// They are arranged into 8x8 tiles, where each 8x8 tile is composed of
//  four 4x4 subtiles, which are in turn composed of four 2x2 subtiles
static void ToMortonTexture(C3D_Tex* tex, int originX, int originY, 
				struct Bitmap* bmp, int rowWidth) {
	unsigned int pixel, mortonX, mortonY;
	unsigned int dstX, dstY, tileX, tileY;
	
	int width = bmp->width, height = bmp->height;
	cc_uint32* dst = tex->data;
	cc_uint32* src = bmp->scan0;

	for (int y = 0; y < height; y++)
	{
		dstY    = tex->height - 1 - (y + originY);
		tileY   = dstY & ~0x07;
		mortonY = CalcZOrder(dstY & 0x07) << 1;

		for (int x = 0; x < width; x++)
		{
			dstX    = x + originX;
			tileX   = dstX & ~0x07;
			mortonX = CalcZOrder(dstX & 0x07);
			pixel   = src[x + (y * rowWidth)];

			dst[(mortonX | mortonY) + (tileX * 8) + (tileY * tex->width)] = pixel;
		}
	}
}


GfxResourceID Gfx_CreateTexture(struct Bitmap* bmp, cc_uint8 flags, cc_bool mipmaps) {
	C3D_Tex* tex = Mem_Alloc(1, sizeof(C3D_Tex), "GPU texture desc");
	bool success = C3D_TexInit(tex, bmp->width, bmp->height, GPU_RGBA8);
	//if (!success) Logger_Abort("Failed to create 3DS texture");
	
	ToMortonTexture(tex, 0, 0, bmp, bmp->width);
    	C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    	C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);
    	return tex;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
	C3D_Tex* tex = (C3D_Tex*)texId;
	ToMortonTexture(tex, x, y, part, rowWidth);
}

void Gfx_UpdateTexturePart(GfxResourceID texId, int x, int y, struct Bitmap* part, cc_bool mipmaps) {
	Gfx_UpdateTexture(texId, x, y, part, part->width, mipmaps);
}

void Gfx_DeleteTexture(GfxResourceID* texId) {
	C3D_Tex* tex = *texId;
	if (!tex) return;
	
	C3D_TexDelete(tex);
	Mem_Free(tex);
	*texId = NULL;
}

void Gfx_EnableMipmaps(void) { }
void Gfx_DisableMipmaps(void) { }

void Gfx_BindTexture(GfxResourceID texId) {
	C3D_Tex* tex  = (C3D_Tex*)texId;
	if (!tex) tex = white_square;
	
	C3D_TexBind(0, tex);
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetFaceCulling(cc_bool enabled) { 
	C3D_CullFace(enabled ? GPU_CULL_FRONT_CCW : GPU_CULL_NONE);
}

void Gfx_SetAlphaArgBlend(cc_bool enabled) { }

void Gfx_SetAlphaBlending(cc_bool enabled) { 
	if (enabled) {
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	} else {
		C3D_ColorLogicOp(GPU_LOGICOP_COPY);
	}
}

void Gfx_SetAlphaTest(cc_bool enabled) { 
	C3D_AlphaTest(enabled, GPU_GREATER, 0x7F);
}

void Gfx_DepthOnlyRendering(cc_bool depthOnly) {
	cc_bool enabled = !depthOnly;
	Gfx_SetColWriteMask(enabled, enabled, enabled, enabled);
}

static PackedCol clear_color;
void Gfx_ClearCol(PackedCol color) {
	// TODO find better way?
	clear_color = (PackedCol_R(color) << 24) | (PackedCol_G(color) << 16) | (PackedCol_B(color) << 8) | 0xFF;
}

static cc_bool depthTest, depthWrite;
static int colorWriteMask = GPU_WRITE_COLOR;

static void UpdateWriteState(void) {
	//C3D_EarlyDepthTest(true, GPU_EARLYDEPTH_GREATER, 0);
	//C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GREATER, 0);
	int writeMask = colorWriteMask;
	if (depthWrite) writeMask |= GPU_WRITE_DEPTH;
	C3D_DepthTest(depthTest, GPU_GEQUAL, writeMask);
}

void Gfx_SetDepthWrite(cc_bool enabled) {
	depthWrite = enabled;
	UpdateWriteState();
}
void Gfx_SetDepthTest(cc_bool enabled) { 
	depthTest = enabled;
	UpdateWriteState();
}

void Gfx_SetColWriteMask(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
	int mask = 0;
	if (r) mask |= GPU_WRITE_RED;
	if (g) mask |= GPU_WRITE_GREEN;
	if (b) mask |= GPU_WRITE_BLUE;
	if (a) mask |= GPU_WRITE_ALPHA;
	
	colorWriteMask = mask;
	UpdateWriteState();
}


/*########################################################################################################################*
*-----------------------------------------------------------Misc----------------------------------------------------------*
*#########################################################################################################################*/
cc_result Gfx_TakeScreenshot(struct Stream* output) {
	return ERR_NOT_SUPPORTED;
}

void Gfx_GetApiInfo(cc_string* info) {
	int pointerSize = sizeof(void*) * 8;

	String_Format1(info, "-- Using 3DS (%i bit) --\n", &pointerSize);
	String_Format2(info, "Max texture size: (%i, %i)\n", &Gfx.MaxTexWidth, &Gfx.MaxTexHeight);
}

void Gfx_SetFpsLimit(cc_bool vsync, float minFrameMs) {
	gfx_minFrameMs = minFrameMs;
	gfx_vsync      = vsync;
}

void Gfx_BeginFrame(void) {
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}
void Gfx_Clear(void) {
	//Platform_Log1("CLEAR: %i", &clear_color);
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, clear_color, 0);
	C3D_FrameDrawOn(target);
}

void Gfx_EndFrame(void) {
	C3D_FrameEnd(0);
	//gfxFlushBuffers();
	//gfxSwapBuffers();

	//Platform_LogConst("FRAME!");
	//if (gfx_vsync) gspWaitForVBlank();
	if (gfx_minFrameMs) LimitFPS();
}

void Gfx_OnWindowResize(void) { }


/*########################################################################################################################*
*----------------------------------------------------------Buffers--------------------------------------------------------*
*#########################################################################################################################*/
static cc_uint8* gfx_vertices;
static cc_uint16* gfx_indices;

static void* AllocBuffer(int count, int elemSize) {
	void* ptr = linearAlloc(count * elemSize);
	//cc_uintptr addr = ptr;
	//Platform_Log3("BUFFER CREATE: %i X %i = %x", &count, &elemSize, &addr);
	
	if (!ptr) Logger_Abort("Failed to allocate memory for buffer");
	return ptr;
}

static void FreeBuffer(GfxResourceID* buffer) {
	GfxResourceID ptr = *buffer;
	if (!ptr) return;
	linearFree(ptr);
	*buffer = 0;
}

GfxResourceID Gfx_CreateIb2(int count, Gfx_FillIBFunc fillFunc, void* obj) {
	void* ib = AllocBuffer(count, sizeof(cc_uint16));
	fillFunc(ib, count, obj);
	return ib;
}

void Gfx_BindIb(GfxResourceID ib)    { gfx_indices = ib; }

void Gfx_DeleteIb(GfxResourceID* ib) { FreeBuffer(ib); }


GfxResourceID Gfx_CreateVb(VertexFormat fmt, int count) {
	return AllocBuffer(count, strideSizes[fmt]);
}

void Gfx_BindVb(GfxResourceID vb) { 
	gfx_vertices = vb; // https://github.com/devkitPro/citro3d/issues/47
	// "Fyi the permutation specifies the order in which the attributes are stored in the buffer, LSB first. So 0x210 indicates attributes 0, 1 & 2."
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
  	BufInfo_Init(bufInfo);

	if (gfx_format == VERTEX_FORMAT_TEXTURED) {
		BufInfo_Add(bufInfo, vb, SIZEOF_VERTEX_TEXTURED, 3, 0x210);
	} else {
		BufInfo_Add(bufInfo, vb, SIZEOF_VERTEX_COLOURED, 2,  0x10);
	}
}

void Gfx_DeleteVb(GfxResourceID* vb) { FreeBuffer(vb); }

void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	return vb;
}

void Gfx_UnlockVb(GfxResourceID vb) { gfx_vertices = vb; }


GfxResourceID Gfx_CreateDynamicVb(VertexFormat fmt, int maxVertices)  {
	return AllocBuffer(maxVertices, strideSizes[fmt]);
}

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) { 
	return vb; 
}

void Gfx_UnlockDynamicVb(GfxResourceID vb) { gfx_vertices = vb; }

void Gfx_SetDynamicVbData(GfxResourceID vb, void* vertices, int vCount) {
	Mem_Copy(vb, vertices, vCount * gfx_stride);
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetFog(cc_bool enabled) {
}

void Gfx_SetFogCol(PackedCol color) {
}

void Gfx_SetFogDensity(float value) {
}

void Gfx_SetFogEnd(float value) {
}

void Gfx_SetFogMode(FogFunc func) {
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
static C3D_Mtx _view, _proj;

void Gfx_CalcOrthoMatrix(struct Matrix* matrix, float width, float height, float zNear, float zFar) {
	Mtx_OrthoTilt(matrix, 0.0f, width, height, 0.0f, zNear, zFar, true);
}

void Gfx_CalcPerspectiveMatrix(struct Matrix* matrix, float fov, float aspect, float zFar) {
	Mtx_PerspTilt(matrix, fov, aspect, 0.1f, zFar, true);
}

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (type == MATRIX_VIEW) {
		float* m  = (float*)matrix;
		// Transpose
		for(int i = 0; i < 4; i++)
		{
			_view.r[i].x = m[0  + i];
			_view.r[i].y = m[4  + i];
			_view.r[i].z = m[8  + i];
			_view.r[i].w = m[12 + i];
		}
	}
	if (type == MATRIX_PROJECTION) _proj = *((C3D_Mtx*)matrix);

	Mtx_Multiply(&_mvp, &_proj, &_view);
	DirtyUniform(UNI_MVP_MATRIX);
	ReloadUniforms();
}


/*void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (type == MATRIX_VIEW) _view = *matrix;
	
	// Provided projection matrix assumes landscape display, but 3DS framebuffer
	//  is a rotated portrait display, so need to swap pixel X/Y values to correct that
	// 
	// This can be done by rotating the projection matrix 90 degrees around Z axis
	// https://open.gl/transformations
	if (type == MATRIX_PROJECTION) {
		struct Matrix rot = Matrix_Identity;
		rot.row1.X =  0; rot.row1.Y = 1;
		rot.row2.X = -1; rot.row2.Y = 0;
		//Matrix_RotateZ(&rot, 90 * MATH_DEG2RAD);
		//Matrix_Mul(&_proj, &_proj, &rot); // TODO avoid Matrix_Mul ??
		Matrix_Mul(&_proj, matrix, &rot); // TODO avoid Matrix_Mul ?
	}

	UpdateMVP();
	DirtyUniform(UNI_MVP_MATRIX);
	ReloadUniforms();
}*/

void Gfx_LoadIdentityMatrix(MatrixType type) {
	Gfx_LoadMatrix(type, &Matrix_Identity);
}

void Gfx_EnableTextureOffset(float x, float y) {
}

void Gfx_DisableTextureOffset(void) {
}



/*########################################################################################################################*
*---------------------------------------------------------Drawing---------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Gfx_WarnIfNecessary(void) { return false; }
static void UpdateAttribFormat(VertexFormat fmt) {
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);         // in_pos
	AttrInfo_AddLoader(attrInfo, 1, GPU_UNSIGNED_BYTE, 4); // in_col
	if (fmt == VERTEX_FORMAT_TEXTURED) {
		AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 2); // in_tex
	}
}

static void UpdateTexEnv(VertexFormat fmt) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	
	if (fmt == VERTEX_FORMAT_TEXTURED) {
		// Configure the first fragment shading substage to blend the texture color with
  		// the vertex color (calculated by the vertex shader using a lighting algorithm)
  		// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight	
  		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
 		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
 	} else {
 		// Configure the first fragment shading substage to just pass through the vertex color
		// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
 	}
}

void Gfx_SetVertexFormat(VertexFormat fmt) {
	if (fmt == gfx_format) return;
	gfx_format = fmt;
	gfx_stride = strideSizes[fmt];
	
	SwitchProgram();
	UpdateAttribFormat(fmt);
	UpdateTexEnv(fmt);
}

void Gfx_DrawVb_Lines(int verticesCount) {
	/* TODO */
}

static void SetVertexBuffer(int startVertex) {
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
  	BufInfo_Init(bufInfo);

	if (gfx_format == VERTEX_FORMAT_TEXTURED) {
		BufInfo_Add(bufInfo, (struct VertexTextured*)gfx_vertices + startVertex, SIZEOF_VERTEX_TEXTURED, 3, 0x210);
	} else {
		BufInfo_Add(bufInfo, (struct VertexColoured*)gfx_vertices + startVertex, SIZEOF_VERTEX_COLOURED, 2,  0x10);
	}
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex) {
	SetVertexBuffer(startVertex);
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount), C3D_UNSIGNED_SHORT, gfx_indices);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
	SetVertexBuffer(0);
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount), C3D_UNSIGNED_SHORT, gfx_indices);
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex) {
	SetVertexBuffer(startVertex);
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount), C3D_UNSIGNED_SHORT, gfx_indices);
}


// this doesn't work properly, because (index buffer + offset) must be aligned to 16 bytes
/*void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex) {
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount), C3D_UNSIGNED_SHORT, gfx_indices + startVertex);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount), C3D_UNSIGNED_SHORT, gfx_indices);
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex) {
	C3D_DrawElements(GPU_TRIANGLES, ICOUNT(verticesCount), C3D_UNSIGNED_SHORT, gfx_indices + startVertex);
}*/
#endif