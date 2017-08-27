/*
**  Hardpoly renderer
**  Copyright (c) 2016 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include <stdlib.h>
#include "r_utility.h"
#include "hardpolyrenderer.h"
#include "v_palette.h"
#include "v_video.h"
#include "m_png.h"
#include "textures/textures.h"
#include "r_data/voxels.h"
#include "p_setup.h"
#include "r_utility.h"
#include "d_player.h"
#include "gl/system/gl_system.h"
#include "gl/system/gl_swframebuffer.h"
#include "gl/data/gl_data.h"
#include "po_man.h"
#include "r_data/r_interpolate.h"
#include "p_effect.h"
#include "g_levellocals.h"

HardpolyRenderer::HardpolyRenderer()
{
	mContext = std::make_shared<GPUContext>();
}

HardpolyRenderer::~HardpolyRenderer()
{
}

void HardpolyRenderer::Begin()
{
	mFrameUniformsDirty = true;
	mContext->Begin();
	SetupFramebuffer();
	CompileShaders();
	CreateSamplers();
	mDrawBatcher.NextFrame();
}

void HardpolyRenderer::End()
{
	mDrawBatcher.Flush(this);
	mContext->SetViewport(0, 0, screen->GetWidth(), screen->GetHeight());
	mContext->End();

	auto swframebuffer = static_cast<OpenGLSWFrameBuffer*>(screen);
	swframebuffer->SetViewFB(mSceneFB->Handle());
}

void HardpolyRenderer::ClearBuffers(DCanvas *canvas)
{
	mDrawBatcher.Flush(this);
	mContext->ClearColorBuffer(0, 0.5f, 0.5f, 0.2f, 1.0f);
	mContext->ClearColorBuffer(1, 0.0f, 0.0f, 0.0f, 0.0f);
	mContext->ClearColorBuffer(2, 1.0f, 0.0f, 0.0f, 0.0f);
	mContext->ClearDepthStencilBuffer(1.0f, 0);
}

void HardpolyRenderer::SetViewport(int x, int y, int width, int height, DCanvas *canvas)
{
	mDrawBatcher.Flush(this);
	mContext->SetViewport(x, y, width, height);
}

void HardpolyRenderer::DrawArray(const PolyDrawArgs &drawargs)
{
	if (!drawargs.WriteColor())
		return;

	bool ccw = drawargs.FaceCullCCW();
	int vcount = drawargs.VertexCount();
	if (vcount < 3)
		return;

	mDrawBatcher.GetVertices(this, vcount);

	DrawRun run;
	run.Texture = drawargs.Texture();
	if (!run.Texture)
	{
		run.Pixels = drawargs.TexturePixels();
		run.PixelsWidth = drawargs.TextureWidth();
		run.PixelsHeight = drawargs.TextureHeight();
	}
	run.Translation = drawargs.Translation();
	run.DrawMode = drawargs.DrawMode();
	run.BaseColormap = drawargs.BaseColormap();
	run.Uniforms.AlphaTest = 0.5f;
	run.Uniforms.Light = drawargs.Light();
	if (drawargs.FixedLight())
		run.Uniforms.Light = -run.Uniforms.Light - 1;
	run.Uniforms.Mode = GetSamplerMode(drawargs.BlendMode());
	run.Uniforms.FillColor.X = RPART(drawargs.Color()) / 255.0f;
	run.Uniforms.FillColor.Y = GPART(drawargs.Color()) / 255.0f;
	run.Uniforms.FillColor.Z = BPART(drawargs.Color()) / 255.0f;
	run.Uniforms.FillColor.W = drawargs.Color();
	run.BlendMode = drawargs.BlendMode();
	run.SrcAlpha = drawargs.SrcAlpha();
	run.DestAlpha = drawargs.DestAlpha();
	run.DepthTest = drawargs.DepthTest();
	run.WriteDepth = drawargs.WriteDepth();
	run.Start = mDrawBatcher.mNextVertex;
	run.NumVertices = vcount;

	memcpy(mDrawBatcher.mVertices + mDrawBatcher.mNextVertex, drawargs.Vertices(), sizeof(TriVertex) * vcount);
	mDrawBatcher.mNextVertex += run.NumVertices;
	mDrawBatcher.mCurrentBatch->DrawRuns.push_back(run);
}

void HardpolyRenderer::UpdateFrameUniforms()
{
	if (mFrameUniformsDirty)
	{
		mCurrentFrameUniforms = (mCurrentFrameUniforms + 1) % 3;

		if (!mFrameUniforms[mCurrentFrameUniforms])
			mFrameUniforms[mCurrentFrameUniforms] = std::make_shared<GPUUniformBuffer>(nullptr, (int)sizeof(FrameUniforms));

		FrameUniforms frameUniforms;
		frameUniforms.WorldToView = worldToView;
		frameUniforms.ViewToProjection = viewToClip;
		frameUniforms.GlobVis = R_GetGlobVis(PolyRenderer::Instance()->Viewwindow, r_visibility);

		mFrameUniforms[mCurrentFrameUniforms]->Upload(&frameUniforms, (int)sizeof(FrameUniforms));

		mFrameUniformsDirty = false;
	}
}

void HardpolyRenderer::DrawRect(const RectDrawArgs &args)
{
	UpdateFrameUniforms();

	if (!mScreenQuad)
	{
		Vec2f quad[4] =
		{
			{ 0.0f, 0.0f },
			{ 1.0f, 0.0f },
			{ 0.0f, 1.0f },
			{ 1.0f, 1.0f },
		};
		mScreenQuadVertexBuffer = std::make_shared<GPUVertexBuffer>(quad, (int)(sizeof(Vec2f) * 4));
		std::vector<GPUVertexAttributeDesc> desc =
		{
			{ 0, 2, GPUVertexAttributeType::Float, false, 0, 0, mScreenQuadVertexBuffer }
		};
		mScreenQuad = std::make_shared<GPUVertexArray>(desc);
	}

	if (!mRectUniforms)
	{
		mRectUniforms = std::make_shared<GPUUniformBuffer>(nullptr, (int)sizeof(RectUniforms));
	}

	RectUniforms uniforms;
	uniforms.x0 = args.X0() / (float)screen->GetWidth() * 2.0f - 1.0f;
	uniforms.x1 = args.X1() / (float)screen->GetWidth() * 2.0f - 1.0f;
	uniforms.y0 = args.Y0() / (float)screen->GetHeight() * 2.0f - 1.0f;
	uniforms.y1 = args.Y1() / (float)screen->GetHeight() * 2.0f - 1.0f;
	uniforms.u0 = args.U0();
	uniforms.v0 = args.V0();
	uniforms.u1 = args.U1();
	uniforms.v1 = args.V1();
	uniforms.Light = args.Light();
	mRectUniforms->Upload(&uniforms, (int)sizeof(RectUniforms));

	mContext->SetVertexArray(mScreenQuad);
	mContext->SetProgram(mRectProgram);

	int loc = glGetUniformLocation(mRectProgram->Handle(), "DiffuseTexture");
	if (loc != -1)
		glUniform1i(loc, 0);

	loc = glGetUniformLocation(mRectProgram->Handle(), "BasecolormapTexture");
	if (loc != -1)
		glUniform1i(loc, 1);

	loc = glGetUniformLocation(mRectProgram->Handle(), "TranslationTexture");
	if (loc != -1)
		glUniform1i(loc, 2);

	mContext->SetUniforms(0, mFrameUniforms[mCurrentFrameUniforms]);
	mContext->SetUniforms(1, mRectUniforms);
	mContext->SetSampler(0, mSamplerNearest);
	mContext->SetSampler(1, mSamplerNearest);
	mContext->SetTexture(0, GetTexturePal(args.Texture()));
	mContext->SetTexture(1, GetColormapTexture(args.BaseColormap()));

	mContext->Draw(GPUDrawMode::TriangleStrip, 0, 4);

	mContext->SetTexture(0, nullptr);
	mContext->SetTexture(1, nullptr);
	mContext->SetSampler(0, nullptr);
	mContext->SetSampler(1, nullptr);
	mContext->SetUniforms(0, nullptr);
	mContext->SetUniforms(1, nullptr);
	mContext->SetVertexArray(nullptr);
	mContext->SetProgram(nullptr);
}

void HardpolyRenderer::RenderBatch(DrawBatch *batch)
{
	UpdateFrameUniforms();

	if (!mFaceUniforms)
	{
		mFaceUniforms = std::make_shared<GPUUniformBuffer>(nullptr, (int)sizeof(FaceUniforms));
	}

	mContext->SetVertexArray(batch->VertexArray);
	mContext->SetProgram(mOpaqueProgram);
	mContext->SetUniforms(0, mFrameUniforms[mCurrentFrameUniforms]);
	mContext->SetUniforms(1, mFaceUniforms);

	int loc = glGetUniformLocation(mOpaqueProgram->Handle(), "DiffuseTexture");
	if (loc != -1)
		glUniform1i(loc, 0);

	loc = glGetUniformLocation(mOpaqueProgram->Handle(), "BasecolormapTexture");
	if (loc != -1)
		glUniform1i(loc, 1);

	loc = glGetUniformLocation(mOpaqueProgram->Handle(), "TranslationTexture");
	if (loc != -1)
		glUniform1i(loc, 2);

	//glEnable(GL_STENCIL_TEST);
	//glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	//glStencilFunc(GL_ALWAYS, 1, 0xffffffff);

	mContext->SetSampler(0, mSamplerNearest);
	mContext->SetSampler(1, mSamplerNearest);
	mContext->SetSampler(2, mSamplerNearest);
	for (const auto &run : batch->DrawRuns)
	{
		mFaceUniforms->Upload(&run.Uniforms, (int)sizeof(FaceUniforms));

		//glDepthFunc(run.DepthTest ? GL_LESS : GL_ALWAYS);
		//glDepthMask(run.WriteDepth ? GL_TRUE : GL_FALSE);

		BlendSetterFunc blendSetter = GetBlendSetter(run.BlendMode);
		(*this.*blendSetter)(run.SrcAlpha, run.DestAlpha);

		if (run.Texture)
			mContext->SetTexture(0, GetTexturePal(run.Texture));
		else if (run.Pixels)
			mContext->SetTexture(0, GetEngineTexturePal(run.Pixels, run.PixelsWidth, run.PixelsHeight));

		mContext->SetTexture(1, GetColormapTexture(run.BaseColormap));

		if (run.Translation)
			mContext->SetTexture(2, GetTranslationTexture(run.Translation));

		switch (run.DrawMode)
		{
		case PolyDrawMode::Triangles:
			mContext->Draw(GPUDrawMode::Triangles, run.Start, run.NumVertices);
			break;
		case PolyDrawMode::TriangleStrip:
			mContext->Draw(GPUDrawMode::TriangleStrip, run.Start, run.NumVertices);
			break;
		case PolyDrawMode::TriangleFan:
			mContext->Draw(GPUDrawMode::TriangleFan, run.Start, run.NumVertices);
			break;
		}
	}
	mContext->SetTexture(0, nullptr);
	mContext->SetTexture(1, nullptr);
	mContext->SetTexture(2, nullptr);
	mContext->SetSampler(0, nullptr);
	mContext->SetSampler(1, nullptr);
	mContext->SetSampler(2, nullptr);

	//glDisable(GL_STENCIL_TEST);

	mContext->SetUniforms(0, nullptr);
	mContext->SetUniforms(1, nullptr);
	mContext->SetVertexArray(nullptr);
	mContext->SetProgram(nullptr);
}

void HardpolyRenderer::SetupFramebuffer()
{
	int width = screen->GetWidth();
	int height = screen->GetHeight();
	if (!mSceneFB || mAlbedoBuffer->Width() != width || mAlbedoBuffer->Height() != height)
	{
		mSceneFB.reset();
		mAlbedoBuffer.reset();
		mDepthStencilBuffer.reset();
		mNormalBuffer.reset();
		mSpriteDepthBuffer.reset();

		mAlbedoBuffer = std::make_shared<GPUTexture2D>(width, height, false, 0, GPUPixelFormat::RGBA16f);
		mNormalBuffer = std::make_shared<GPUTexture2D>(width, height, false, 0, GPUPixelFormat::RGBA16f);
		mDepthStencilBuffer = std::make_shared<GPUTexture2D>(width, height, false, 0, GPUPixelFormat::Depth24_Stencil8);
		mSpriteDepthBuffer = std::make_shared<GPUTexture2D>(width, height, false, 0, GPUPixelFormat::R32f);

		std::vector<std::shared_ptr<GPUTexture2D>> colorbuffers = { mAlbedoBuffer, mNormalBuffer, mSpriteDepthBuffer };
		mSceneFB = std::make_shared<GPUFrameBuffer>(colorbuffers, mDepthStencilBuffer);

		std::vector<std::shared_ptr<GPUTexture2D>> translucentcolorbuffers = { mAlbedoBuffer, mNormalBuffer };
		mTranslucentFB = std::make_shared<GPUFrameBuffer>(translucentcolorbuffers, mDepthStencilBuffer);
	}

	mContext->SetFrameBuffer(mSceneFB);

	GLenum drawbuffers[3] = { GL_COLOR_ATTACHMENT0 + 0, GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0 + 2 };
	glDrawBuffers(3, drawbuffers);
}

void HardpolyRenderer::CreateSamplers()
{
	if (!mSamplerNearest)
	{
		mSamplerLinear = std::make_shared<GPUSampler>(GPUSampleMode::Linear, GPUSampleMode::Nearest, GPUMipmapMode::None, GPUWrapMode::Repeat, GPUWrapMode::Repeat);
		mSamplerNearest = std::make_shared<GPUSampler>(GPUSampleMode::Nearest, GPUSampleMode::Nearest, GPUMipmapMode::None, GPUWrapMode::Repeat, GPUWrapMode::Repeat);
	}
}

std::shared_ptr<GPUTexture2D> HardpolyRenderer::GetTranslationTexture(const uint8_t *translation)
{
	auto &texture = mTranslationTextures[translation];
	if (!texture)
	{
		texture = std::make_shared<GPUTexture2D>(256, 1, false, 0, GPUPixelFormat::R8, translation);
	}
	return texture;
}

std::shared_ptr<GPUTexture2D> HardpolyRenderer::GetEngineTexturePal(const uint8_t *src, int width, int height)
{
	auto &texture = mEngineTextures[src];
	if (!texture)
	{
		std::vector<uint8_t> pixels;
		if (src)
		{
			pixels.resize(width * height);
			uint8_t *dest = pixels.data();
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					dest[x + y * width] = src[y + x * height];
				}
			}
		}
		else
		{
			width = 1;
			height = 1;
			pixels.push_back(0);
		}

		texture = std::make_shared<GPUTexture2D>(width, height, false, 0, GPUPixelFormat::R8, pixels.data());
	}
	return texture;
}

std::shared_ptr<GPUTexture2D> HardpolyRenderer::GetColormapTexture(const uint8_t *basecolormap)
{
	auto &texture = mColormaps[basecolormap];
	if (!texture)
	{
		uint32_t rgbacolormap[256 * NUMCOLORMAPS];
		for (int i = 0; i < 256 * NUMCOLORMAPS; i++)
		{
			const auto &entry = GPalette.BaseColors[basecolormap[i]];
			uint32_t red = entry.r;
			uint32_t green = entry.g;
			uint32_t blue = entry.b;
			uint32_t alpha = 255;
			rgbacolormap[i] = red | (green << 8) | (blue << 16) | (alpha << 24);
		}
		texture = std::make_shared<GPUTexture2D>(256, NUMCOLORMAPS, false, 0, GPUPixelFormat::RGBA8, rgbacolormap);
	}
	return texture;
}

std::shared_ptr<GPUTexture2D> HardpolyRenderer::GetTextureBgra(FTexture *ztexture)
{
	auto &texture = mTextures[ztexture];
	if (!texture)
	{
		int width, height;
		bool mipmap;
		std::vector<uint32_t> pixels;
		if (ztexture)
		{
			width = ztexture->GetWidth();
			height = ztexture->GetHeight();
			mipmap = true;

			pixels.resize(width * height);
			const uint32_t *src = ztexture->GetPixelsBgra();
			uint32_t *dest = pixels.data();
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					uint32_t pixel = src[y + x * height];
					uint32_t red = RPART(pixel);
					uint32_t green = GPART(pixel);
					uint32_t blue = BPART(pixel);
					uint32_t alpha = APART(pixel);
					dest[x + y * width] = red | (green << 8) | (blue << 16) | (alpha << 24);
				}
			}
		}
		else
		{
			width = 1;
			height = 1;
			mipmap = false;
			pixels.push_back(0xff00ffff);
		}

		texture = std::make_shared<GPUTexture2D>(width, height, mipmap, 0, GPUPixelFormat::RGBA8, pixels.data());
	}
	return texture;
}

std::shared_ptr<GPUTexture2D> HardpolyRenderer::GetTexturePal(FTexture *ztexture)
{
	auto &texture = mTextures[ztexture];
	if (!texture)
	{
		int width, height;
		std::vector<uint8_t> pixels;
		if (ztexture)
		{
			width = ztexture->GetWidth();
			height = ztexture->GetHeight();

			pixels.resize(width * height);
			const uint8_t *src = ztexture->GetPixels();
			uint8_t *dest = pixels.data();
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					dest[x + y * width] = src[y + x * height];
				}
			}
		}
		else
		{
			width = 1;
			height = 1;
			pixels.push_back(0);
		}

		texture = std::make_shared<GPUTexture2D>(width, height, false, 0, GPUPixelFormat::R8, pixels.data());
	}
	return texture;
}

void HardpolyRenderer::CompileShaders()
{
	if (!mOpaqueProgram)
	{
		mOpaqueProgram = std::make_shared<GPUProgram>();

		mOpaqueProgram->Compile(GPUShaderType::Vertex, "vertex", R"(
				layout(std140) uniform FrameUniforms
				{
					mat4 WorldToView;
					mat4 ViewToProjection;
					float GlobVis;
				};

				in vec4 Position;
				in vec4 Texcoord;
				out vec2 UV;
				out vec3 PositionInView;

				void main()
				{
					vec4 posInView = WorldToView * vec4(Position.xyz, 1.0);
					PositionInView = posInView.xyz;
					gl_Position = ViewToProjection * posInView;

					UV = Texcoord.xy;
				}
			)");
		mOpaqueProgram->Compile(GPUShaderType::Fragment, "fragment", R"(
				layout(std140) uniform FrameUniforms
				{
					mat4 WorldToView;
					mat4 ViewToProjection;
					float GlobVis;
				};

				layout(std140) uniform FaceUniforms
				{
					float Light;
					float AlphaTest;
					int Mode;
					int Padding;
					vec4 FillColor;
				};

				in vec2 UV;
				in vec3 PositionInView;
				out vec4 FragColor;
				uniform sampler2D DiffuseTexture;
				uniform sampler2D BasecolormapTexture;
				uniform sampler2D TranslationTexture;
				
				float SoftwareLight()
				{
					float z = -PositionInView.z;
					float vis = GlobVis / z;
					float shade = 64.0 - (Light + 12.0) * 32.0/128.0;
					float lightscale = clamp((shade - min(24.0, vis)) / 32.0, 0.0, 31.0/32.0);
					return 1.0 - lightscale;
				}

				int SoftwareLightPal()
				{
					if (Light < 0)
						return 31 - int((-1.0 - Light) * 31.0 / 255.0 + 0.5);

					float z = -PositionInView.z;
					float vis = GlobVis / z;
					float shade = 64.0 - (Light + 12.0) * 32.0/128.0;
					float lightscale = clamp((shade - min(24.0, vis)), 0.0, 31.0);
					return int(lightscale);
				}

				int SampleFg()
				{
					return int(texture(DiffuseTexture, UV).r * 255.0 + 0.5);
				}

				vec4 LightShadePal(int fg)
				{
					return texelFetch(BasecolormapTexture, ivec2(fg, SoftwareLightPal()), 0);
				}

				int Translate(int fg)
				{
					return int(texelFetch(TranslationTexture, ivec2(fg, 0), 0).r * 255.0 + 0.5);
				}

				int FillColorPal()
				{
					return int(FillColor.a);
				}

				void TextureSampler()
				{
					int fg = SampleFg();
					if (fg == 0) discard;
					FragColor = LightShadePal(fg);
					FragColor.rgb *= FragColor.a;
				}

				void TranslatedSampler()
				{
					int fg = SampleFg();
					if (fg == 0) discard;

					FragColor = LightShadePal(Translate(fg));
					FragColor.rgb *= FragColor.a;
				}

				void ShadedSampler()
				{
					FragColor = LightShadePal(FillColorPal()) * texture(DiffuseTexture, UV).r;
				}

				void StencilSampler()
				{
					float alpha = (SampleFg() != 0) ? 1.0 : 0.0;
					FragColor = LightShadePal(FillColorPal()) * alpha;
				}

				void FillSampler()
				{
					FragColor = LightShadePal(FillColorPal());
				}

				void SkycapSampler()
				{
					vec4 capcolor = LightShadePal(FillColorPal());

					int fg = SampleFg();
					vec4 skycolor = LightShadePal(fg);

					float startFade = 4.0; // How fast it should fade out
					float alphaTop = clamp(UV.y * startFade, 0.0, 1.0);
					float alphaBottom = clamp((2.0 - UV.y) * startFade, 0.0, 1.0);
					float alpha = min(alphaTop, alphaBottom);

					FragColor = mix(capcolor, skycolor, alpha);
				}

				void FuzzSampler()
				{
					float alpha = (SampleFg() != 0) ? 1.0 : 0.0;
					FragColor = LightShadePal(FillColorPal()) * alpha;
				}

				void FogBoundarySampler()
				{
					FragColor = LightShadePal(FillColorPal());
				}
				
				void main()
				{
					switch (Mode)
					{
					case 0: TextureSampler(); break;
					case 1: TranslatedSampler(); break;
					case 2: ShadedSampler(); break;
					case 3: StencilSampler(); break;
					case 4: FillSampler(); break;
					case 5: SkycapSampler(); break;
					case 6: FuzzSampler(); break;
					case 7: FogBoundarySampler(); break;
					}
				}
			)");

		mOpaqueProgram->SetAttribLocation("Position", 0);
		mOpaqueProgram->SetAttribLocation("UV", 1);
		mOpaqueProgram->SetFragOutput("FragColor", 0);
		mOpaqueProgram->Link("program");
		mOpaqueProgram->SetUniformBlock("FrameUniforms", 0);
		mOpaqueProgram->SetUniformBlock("FaceUniforms", 1);
	}

	if (!mRectProgram)
	{
		mRectProgram = std::make_shared<GPUProgram>();

		mRectProgram->Compile(GPUShaderType::Vertex, "vertex", R"(
				layout(std140) uniform RectUniforms
				{
					float X0, Y0, U0, V0;
					float X1, Y1, U1, V1;
					float Light;
				};

				in vec4 Position;
				out vec2 UV;

				void main()
				{
					gl_Position.x = mix(X0, X1, Position.x);
					gl_Position.y = mix(Y0, Y1, Position.y);
					gl_Position.z = -1.0;
					gl_Position.w = 1.0;
					UV.x = mix(U0, U1, Position.x);
					UV.y = mix(V0, V1, Position.y);
				}
			)");
		mRectProgram->Compile(GPUShaderType::Fragment, "fragment", R"(
				layout(std140) uniform RectUniforms
				{
					float X0, Y0, U0, V0;
					float X1, Y1, U1, V1;
					float Light;
				};

				in vec2 UV;
				out vec4 FragColor;
				uniform sampler2D DiffuseTexture;
				uniform sampler2D BasecolormapTexture;
				
				void main()
				{
					int shade = 31 - int(Light * 31.0 / 255.0 + 0.5);
					int fg = int(texture(DiffuseTexture, UV).r * 255.0 + 0.5);
					if (fg == 0) discard;
					FragColor = texelFetch(BasecolormapTexture, ivec2(fg, shade), 0);
				}
			)");

		mRectProgram->SetAttribLocation("Position", 0);
		mRectProgram->SetAttribLocation("UV", 1);
		mRectProgram->SetFragOutput("FragColor", 0);
		mRectProgram->Link("program");
		mRectProgram->SetUniformBlock("FrameUniforms", 0);
		mRectProgram->SetUniformBlock("RectUniforms", 1);
	}

	if (!mStencilProgram)
	{
		mStencilProgram = std::make_shared<GPUProgram>();

		mStencilProgram->Compile(GPUShaderType::Vertex, "vertex", R"(
				layout(std140) uniform FrameUniforms
				{
					mat4 WorldToView;
					mat4 ViewToProjection;
					float GlobVis;
				};

				in vec4 Position;

				void main()
				{
					vec4 posInView = WorldToView * Position;
					gl_Position = ViewToProjection * posInView;
				}
			)");
		mStencilProgram->Compile(GPUShaderType::Fragment, "fragment", R"(
				out vec4 FragColor;
				void main()
				{
					FragColor = vec4(1.0);
				}
			)");

		mStencilProgram->SetAttribLocation("Position", 0);
		mStencilProgram->SetFragOutput("FragColor", 0);
		mStencilProgram->SetFragOutput("FragNormal", 1);
		mStencilProgram->Link("program");
	}
}

int HardpolyRenderer::GetSamplerMode(TriBlendMode triblend)
{
	enum { Texture, Translated, Shaded, Stencil, Fill, Skycap, Fuzz, FogBoundary };
	static int modes[] =
	{
		Texture,         // TextureOpaque
		Texture,         // TextureMasked
		Texture,         // TextureAdd
		Texture,         // TextureSub
		Texture,         // TextureRevSub
		Texture,         // TextureAddSrcColor
		Translated,      // TranslatedOpaque
		Translated,      // TranslatedMasked
		Translated,      // TranslatedAdd
		Translated,      // TranslatedSub
		Translated,      // TranslatedRevSub
		Translated,      // TranslatedAddSrcColor
		Shaded,          // Shaded
		Shaded,          // AddShaded
		Stencil,         // Stencil
		Stencil,         // AddStencil
		Fill,            // FillOpaque
		Fill,            // FillAdd
		Fill,            // FillSub
		Fill,            // FillRevSub
		Fill,            // FillAddSrcColor
		Skycap,          // Skycap
		Fuzz,            // Fuzz
		FogBoundary,     // FogBoundary
	};
	return modes[(int)triblend];
}

HardpolyRenderer::BlendSetterFunc HardpolyRenderer::GetBlendSetter(TriBlendMode triblend)
{
	static BlendSetterFunc blendsetters[] =
	{
		&HardpolyRenderer::SetOpaqueBlend,         // TextureOpaque
		&HardpolyRenderer::SetMaskedBlend,         // TextureMasked
		&HardpolyRenderer::SetAddClampBlend,       // TextureAdd
		&HardpolyRenderer::SetSubClampBlend,       // TextureSub
		&HardpolyRenderer::SetRevSubClampBlend,    // TextureRevSub
		&HardpolyRenderer::SetAddSrcColorBlend,    // TextureAddSrcColor
		&HardpolyRenderer::SetOpaqueBlend,         // TranslatedOpaque
		&HardpolyRenderer::SetMaskedBlend,         // TranslatedMasked
		&HardpolyRenderer::SetAddClampBlend,       // TranslatedAdd
		&HardpolyRenderer::SetSubClampBlend,       // TranslatedSub
		&HardpolyRenderer::SetRevSubClampBlend,    // TranslatedRevSub
		&HardpolyRenderer::SetAddSrcColorBlend,    // TranslatedAddSrcColor
		&HardpolyRenderer::SetShadedBlend,         // Shaded
		&HardpolyRenderer::SetAddClampShadedBlend, // AddShaded
		&HardpolyRenderer::SetShadedBlend,         // Stencil
		&HardpolyRenderer::SetAddClampShadedBlend, // AddStencil
		&HardpolyRenderer::SetOpaqueBlend,         // FillOpaque
		&HardpolyRenderer::SetAddClampBlend,       // FillAdd
		&HardpolyRenderer::SetSubClampBlend,       // FillSub
		&HardpolyRenderer::SetRevSubClampBlend,    // FillRevSub
		&HardpolyRenderer::SetAddSrcColorBlend,    // FillAddSrcColor
		&HardpolyRenderer::SetOpaqueBlend,         // Skycap
		&HardpolyRenderer::SetShadedBlend,         // Fuzz
		&HardpolyRenderer::SetOpaqueBlend          // FogBoundary
	};
	return blendsetters[(int)triblend];
}

void HardpolyRenderer::SetOpaqueBlend(int srcalpha, int destalpha)
{
	glDisable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ZERO);
}

void HardpolyRenderer::SetMaskedBlend(int srcalpha, int destalpha)
{
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void HardpolyRenderer::SetAlphaBlendFunc(int srcalpha, int destalpha)
{
	int srcblend;
	if (srcalpha == 0.0f)
		srcblend = GL_ZERO;
	else if (srcalpha == 1.0f)
		srcblend = GL_ONE;
	else
		srcblend = GL_CONSTANT_ALPHA;

	int destblend;
	if (destalpha == 0.0f)
		destblend = GL_ZERO;
	else if (destalpha == 1.0f)
		destblend = GL_ONE;
	else if (srcalpha + destalpha >= 255)
		destblend = GL_ONE_MINUS_CONSTANT_ALPHA;
	else
		destblend = GL_CONSTANT_COLOR;

	glBlendColor(destalpha / 256.0f, destalpha / 256.0f, destalpha / 256.0f, srcalpha / 256.0f);
	glBlendFunc(srcblend, destblend);
}

void HardpolyRenderer::SetAddClampBlend(int srcalpha, int destalpha)
{
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	SetAlphaBlendFunc(srcalpha, destalpha);
}

void HardpolyRenderer::SetSubClampBlend(int srcalpha, int destalpha)
{
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_SUBTRACT);
	SetAlphaBlendFunc(srcalpha, destalpha);
}

void HardpolyRenderer::SetRevSubClampBlend(int srcalpha, int destalpha)
{
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
	SetAlphaBlendFunc(srcalpha, destalpha);
}

void HardpolyRenderer::SetAddSrcColorBlend(int srcalpha, int destalpha)
{
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
}

void HardpolyRenderer::SetShadedBlend(int srcalpha, int destalpha)
{
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void HardpolyRenderer::SetAddClampShadedBlend(int srcalpha, int destalpha)
{
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE);
}

/////////////////////////////////////////////////////////////////////////////

void DrawBatcher::GetVertices(HardpolyRenderer *hardpoly, int numVertices)
{
	if (mNextVertex + numVertices > MaxVertices)
	{
		Flush(hardpoly);
	}

	if (!mVertices)
	{
		if (mNextBatch == mCurrentFrameBatches.size())
		{
			auto newBatch = std::unique_ptr<DrawBatch>(new DrawBatch());
			mCurrentFrameBatches.push_back(std::move(newBatch));
		}

		mCurrentBatch = mCurrentFrameBatches[mNextBatch++].get();
		mCurrentBatch->DrawRuns.clear();

		if (!mCurrentBatch->Vertices)
		{
			mCurrentBatch->Vertices = std::make_shared<GPUVertexBuffer>(nullptr, MaxVertices * (int)sizeof(TriVertex));

			std::vector<GPUVertexAttributeDesc> attributes =
			{
				{ 0, 4, GPUVertexAttributeType::Float, false, sizeof(TriVertex), offsetof(TriVertex, x), mCurrentBatch->Vertices },
				{ 1, 2, GPUVertexAttributeType::Float, false, sizeof(TriVertex), offsetof(TriVertex, u), mCurrentBatch->Vertices }
			};

			mCurrentBatch->VertexArray = std::make_shared<GPUVertexArray>(attributes);
		}

		mVertices = (TriVertex*)mCurrentBatch->Vertices->MapWriteOnly();
	}
}

void DrawBatcher::Flush(HardpolyRenderer *hardpoly)
{
	if (mVertices)
	{
		mCurrentBatch->Vertices->Unmap();
		mVertices = nullptr;
	}

	if (mCurrentBatch)
	{
		if (!mCurrentBatch->DrawRuns.empty())
			hardpoly->RenderBatch(mCurrentBatch);
	}

	mNextVertex = 0;
	mCurrentBatch = nullptr;
}

void DrawBatcher::NextFrame()
{
	mCurrentFrameBatches.swap(mLastFrameBatches);
	mNextBatch = 0;
}