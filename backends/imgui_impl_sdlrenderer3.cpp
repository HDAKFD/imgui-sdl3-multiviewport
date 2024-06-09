// dear imgui: Renderer Backend for SDL_Renderer for SDL3
// (Requires: SDL 3.0.0+)

// Note how SDL_Renderer is an _optional_ component of SDL3.
// For a multi-platform app consider using e.g. SDL+DirectX on Windows and SDL+OpenGL on Linux/OSX.
// If your application will want to render any non trivial amount of graphics other than UI,
// please be aware that SDL_Renderer currently offers a limited graphic API to the end-user and
// it might be difficult to step out of those boundaries.

// Implemented features:
//  [X] Renderer: User texture binding. Use 'SDL_Texture*' as ImTextureID. Read the FAQ about ImTextureID!
//  [X] Renderer: Large meshes support (64k+ vertices) with 16-bit indices.
//  [X] Renderer: Multi-viewport support (multiple windows).
//    FIXME: Missing way to share textures betweens renderers: shttps://github.com/libsdl-org/SDL/issues/6742
//    FIXME: Missing way to specify a projection matrix, so our vertices in absolute coordinates are not displayed correctly in multi-viewports mode.

// You can copy and use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

// CHANGELOG
//  2024-XX-XX: Platform: Added support for multiple windows via the ImGuiPlatformIO interface. (#5835)
//  2024-05-14: *BREAKING CHANGE* ImGui_ImplSDLRenderer3_RenderDrawData() requires SDL_Renderer* passed as parameter.
//  2024-02-12: Amend to query SDL_RenderViewportSet() and restore viewport accordingly.
//  2023-05-30: Initial version.

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_sdlrenderer3.h"
#include <stdint.h>     // intptr_t

// Clang warnings with -Weverything
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"    // warning: implicit conversion changes signedness
#endif

// SDL
#include <SDL3/SDL.h>
#if !SDL_VERSION_ATLEAST(3,0,0)
#error This backend requires SDL 3.0.0+
#endif

// SDL_Renderer data
struct ImGui_ImplSDLRenderer3_Data
{
    SDL_Renderer*   Renderer;       // Main viewport's renderer
    SDL_Texture*    FontTexture;

    ImVector<ImVec2>    PosBuffer;  // Transformed pos buffer (for multi-viewports only)

    ImGui_ImplSDLRenderer3_Data()   { memset((void*)this, 0, sizeof(*this)); }
};

// Backend data stored in io.BackendRendererUserData to allow support for multiple Dear ImGui contexts
// It is STRONGLY preferred that you use docking branch with multi-viewports (== single Dear ImGui context + multiple windows) instead of multiple Dear ImGui contexts.
static ImGui_ImplSDLRenderer3_Data* ImGui_ImplSDLRenderer3_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplSDLRenderer3_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

// Forward Declarations
static void ImGui_ImplSDLRenderer3_InitPlatformInterface();
static void ImGui_ImplSDLRenderer3_ShutdownPlatformInterface();

// Functions
bool ImGui_ImplSDLRenderer3_Init(SDL_Renderer* renderer)
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    IM_ASSERT(renderer != nullptr && "SDL_Renderer not initialized!");

    // Setup backend capabilities flags
    ImGui_ImplSDLRenderer3_Data* bd = IM_NEW(ImGui_ImplSDLRenderer3_Data)();
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_sdlrenderer3";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;  // We can create multi-viewports on the Renderer side (optional)

    bd->Renderer = renderer;

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        ImGui_ImplSDLRenderer3_InitPlatformInterface();

    return true;
}

void ImGui_ImplSDLRenderer3_Shutdown()
{
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    ImGui_ImplSDLRenderer3_ShutdownPlatformInterface();
    ImGui_ImplSDLRenderer3_DestroyDeviceObjects();

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
    IM_DELETE(bd);
}

static void ImGui_ImplSDLRenderer3_SetupRenderState(SDL_Renderer* renderer)
{
	// Clear out any viewports and cliprect set by the user
    // FIXME: Technically speaking there are lots of other things we could backup/setup/restore during our render process.
	SDL_SetRenderViewport(renderer, nullptr);
	SDL_SetRenderClipRect(renderer, nullptr);
}

void ImGui_ImplSDLRenderer3_NewFrame()
{
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSDLRenderer3_Init()?");

    if (!bd->FontTexture)
    {    
        ImGuiIO& io = ImGui::GetIO();

        ImGui_ImplSDLRenderer3_CreateDeviceObjects();
        io.Fonts->SetTexID((ImTextureID)(intptr_t)bd->FontTexture);
    }
}

void ImGui_ImplSDLRenderer3_RenderDrawData(ImDrawData* draw_data, SDL_Renderer* renderer, SDL_Texture* texture)
{
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();

	// If there's a scale factor set by the user, use that instead
    // If the user has specified a scale factor to SDL_Renderer already via SDL_RenderSetScale(), SDL will scale whatever we pass
    // to SDL_RenderGeometryRaw() by that scale factor. In that case we don't want to be also scaling it ourselves here.
    float rsx = 1.0f;
	float rsy = 1.0f;
	SDL_GetRenderScale(renderer, &rsx, &rsy);
    ImVec2 render_scale;
	render_scale.x = (rsx == 1.0f) ? draw_data->FramebufferScale.x : 1.0f;
	render_scale.y = (rsy == 1.0f) ? draw_data->FramebufferScale.y : 1.0f;

	// Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
	int fb_width = (int)(draw_data->DisplaySize.x * render_scale.x);
	int fb_height = (int)(draw_data->DisplaySize.y * render_scale.y);
	if (fb_width == 0 || fb_height == 0)
		return;

    // Backup SDL_Renderer state that will be modified to restore it afterwards
    struct BackupSDLRendererState
    {
        SDL_Rect    Viewport;
        bool        ViewportEnabled;
        bool        ClipEnabled;
        SDL_Rect    ClipRect;
    };
    BackupSDLRendererState old = {};
    old.ViewportEnabled = SDL_RenderViewportSet(renderer) == SDL_TRUE;
    old.ClipEnabled = SDL_RenderClipEnabled(renderer) == SDL_TRUE;
    SDL_GetRenderViewport(renderer, &old.Viewport);
    SDL_GetRenderClipRect(renderer, &old.ClipRect);

	// Will project scissor/clipping rectangles into framebuffer space
	ImVec2 clip_off = draw_data->DisplayPos;         // (0,0) unless using multi-viewports
	ImVec2 clip_scale = render_scale;

    // Render command lists
    ImGui_ImplSDLRenderer3_SetupRenderState(renderer);
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        const ImDrawVert* vtx_buffer = cmd_list->VtxBuffer.Data;
        const ImDrawIdx* idx_buffer = cmd_list->IdxBuffer.Data;

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];

            if (pcmd->UserCallback)
            {
                // User callback, registered via ImDrawList::AddCallback()
                // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                    ImGui_ImplSDLRenderer3_SetupRenderState(renderer);
                else
                    pcmd->UserCallback(cmd_list, pcmd);
            }
            else
            {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
                ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);
                if (clip_min.x < 0.0f) { clip_min.x = 0.0f; }
                if (clip_min.y < 0.0f) { clip_min.y = 0.0f; }
                if (clip_max.x > (float)fb_width) { clip_max.x = (float)fb_width; }
                if (clip_max.y > (float)fb_height) { clip_max.y = (float)fb_height; }
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                SDL_Rect r = { (int)(clip_min.x), (int)(clip_min.y), (int)(clip_max.x - clip_min.x), (int)(clip_max.y - clip_min.y) };
                SDL_SetRenderClipRect(renderer, &r);

                const float* xy = (const float*)(const void*)((const char*)(vtx_buffer + pcmd->VtxOffset) + offsetof(ImDrawVert, pos));
                int xy_stride = (int)sizeof(ImDrawVert);
                const float* uv = (const float*)(const void*)((const char*)(vtx_buffer + pcmd->VtxOffset) + offsetof(ImDrawVert, uv));
                const SDL_Color* color = (const SDL_Color*)(const void*)((const char*)(vtx_buffer + pcmd->VtxOffset) + offsetof(ImDrawVert, col)); // SDL 2.0.19+

                // FIXME-OPT: Transform position manually
                // FIXME-OPT: Note that SDL_RenderGeometryRaw() does another transform for colors..
                if (draw_data->DisplayPos.x != 0.0f || draw_data->DisplayPos.y != 0.0f)
                {
                    const float off_x = draw_data->DisplayPos.x;
                    const float off_y = draw_data->DisplayPos.y;
                    bd->PosBuffer.resize(pcmd->ElemCount);
                    const ImDrawVert* p_in = vtx_buffer + pcmd->VtxOffset;
                    ImVec2* p_out = bd->PosBuffer.Data;
                    for (int remaining = pcmd->ElemCount; remaining > 0; remaining--, p_out++, p_in++)
                    {
                        p_out->x = p_in->pos.x - off_x;
                        p_out->y = p_in->pos.y - off_y;
                    }
                    xy = (const float*)bd->PosBuffer.Data;
                    xy_stride = (int)sizeof(ImVec2);
                }
                
                // Bind texture, Draw
                SDL_Texture* tex = (SDL_Texture*)pcmd->GetTexID();
    
                if (tex == bd->FontTexture)
                {
                    if (texture != nullptr)
                    {
                        tex = texture;
                    }
                }

                SDL_RenderGeometryRaw(renderer, tex,
                    xy, xy_stride,
                    color, (int)sizeof(ImDrawVert),
                    uv, (int)sizeof(ImDrawVert),
                    cmd_list->VtxBuffer.Size - pcmd->VtxOffset,
                    idx_buffer + pcmd->IdxOffset, pcmd->ElemCount, sizeof(ImDrawIdx));
            }
        }
    }

    // Restore modified SDL_Renderer state
    SDL_SetRenderViewport(renderer, old.ViewportEnabled ? &old.Viewport : nullptr);
    SDL_SetRenderClipRect(renderer, old.ClipEnabled ? &old.ClipRect : nullptr);
}

// Called by Init/NewFrame/Shutdown
bool ImGui_ImplSDLRenderer3_CreateFontsTexture(SDL_Renderer* renderer, SDL_Texture** texture)
{
    ImGuiIO& io = ImGui::GetIO();

    // Build texture atlas
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);   // Load as RGBA 32-bit (75% of the memory is wasted, but default font is so small) because it is more likely to be compatible with user's existing shaders. If your ImTextureId represent a higher-level concept than just a GL texture id, consider calling GetTexDataAsAlpha8() instead to save on GPU memory.

    // Upload texture to graphics system
    // (Bilinear sampling is required by default. Set 'io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines' or 'style.AntiAliasedLinesUseTex = false' to allow point/nearest sampling)
    *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
    if (*texture == nullptr)
    {
        SDL_Log("error creating texture");
        return false;
    }
    
    SDL_UpdateTexture(*texture, nullptr, pixels, 4 * width);
    SDL_SetTextureBlendMode(*texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(*texture, SDL_SCALEMODE_LINEAR);
    
    return true;
}

void ImGui_ImplSDLRenderer3_DestroyFontsTexture()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    if (bd->FontTexture)
    {
        io.Fonts->SetTexID(0);
        SDL_DestroyTexture(bd->FontTexture);
        bd->FontTexture = nullptr;
    }
}

bool ImGui_ImplSDLRenderer3_CreateDeviceObjects()
{
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();

    return ImGui_ImplSDLRenderer3_CreateFontsTexture(bd->Renderer, &bd->FontTexture);
}

void ImGui_ImplSDLRenderer3_DestroyDeviceObjects()
{
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
}

//--------------------------------------------------------------------------------------------------------
// MULTI-VIEWPORT / PLATFORM INTERFACE SUPPORT
// This is an _advanced_ and _optional_ feature, allowing the backend to create and handle multiple viewports simultaneously.
// If you are new to dear imgui or creating a new binding for dear imgui, it is recommended that you completely ignore this section first..
//--------------------------------------------------------------------------------------------------------

// Helper structure we store in the void* RendererUserData field of each ImGuiViewport to easily retrieve our backend data.
struct ImGui_ImplSDLRenderer3_ViewportData
{
    SDL_Renderer*                   Renderer;
    SDL_Texture*                    FontTexture;

    ImGui_ImplSDLRenderer3_ViewportData()   { Renderer = nullptr; FontTexture = nullptr; }
    ~ImGui_ImplSDLRenderer3_ViewportData() { IM_ASSERT(Renderer == nullptr); IM_ASSERT(FontTexture == nullptr); }
};

static void ImGui_ImplSDLRenderer3_CreateWindow(ImGuiViewport* viewport)
{
    //ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    ImGui_ImplSDLRenderer3_ViewportData* vd = IM_NEW(ImGui_ImplSDLRenderer3_ViewportData)();
    viewport->RendererUserData = vd;

    SDL_Window* window = (SDL_Window*)viewport->PlatformHandle;
    vd->Renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(vd->Renderer, 0);
    IM_ASSERT(vd->Renderer != NULL);

    ImGui_ImplSDLRenderer3_CreateFontsTexture(vd->Renderer, &vd->FontTexture);
}

static void ImGui_ImplSDLRenderer3_DestroyWindow(ImGuiViewport* viewport)
{
    // The main viewport (owned by the application) will always have RendererUserData == nullptr since we didn't create the data for it.
    if (ImGui_ImplSDLRenderer3_ViewportData* vd = (ImGui_ImplSDLRenderer3_ViewportData*)viewport->RendererUserData)
    {
        SDL_DestroyTexture(vd->FontTexture);
        SDL_DestroyRenderer(vd->Renderer);

        vd->FontTexture = nullptr;
        vd->Renderer = nullptr;
        IM_DELETE(vd);
    }

    viewport->RendererUserData = nullptr;
}

static void ImGui_ImplSDLRenderer3_RenderWindow(ImGuiViewport* viewport, void*)
{
    ImGui_ImplSDLRenderer3_ViewportData* vd = (ImGui_ImplSDLRenderer3_ViewportData*)viewport->RendererUserData;
    if (!(viewport->Flags & ImGuiViewportFlags_NoRendererClear))
    {
        ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
        SDL_SetRenderDrawColor(vd->Renderer, (Uint8)(clear_color.x * 255), (Uint8)(clear_color.y * 255), (Uint8)(clear_color.z * 255), (Uint8)(clear_color.w * 255));
        SDL_RenderClear(vd->Renderer);
    }

    ImGui_ImplSDLRenderer3_RenderDrawData(viewport->DrawData, vd->Renderer, vd->FontTexture);
}

static void ImGui_ImplSDLRenderer3_SwapBuffers(ImGuiViewport* viewport, void*)
{
    ImGui_ImplSDLRenderer3_ViewportData* vd = (ImGui_ImplSDLRenderer3_ViewportData*)viewport->RendererUserData;
    SDL_RenderPresent(vd->Renderer);
}

static void ImGui_ImplSDLRenderer3_InitPlatformInterface()
{
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Renderer_CreateWindow = ImGui_ImplSDLRenderer3_CreateWindow;
    platform_io.Renderer_DestroyWindow = ImGui_ImplSDLRenderer3_DestroyWindow;
    platform_io.Renderer_RenderWindow = ImGui_ImplSDLRenderer3_RenderWindow;
    platform_io.Renderer_SwapBuffers = ImGui_ImplSDLRenderer3_SwapBuffers;
}

static void ImGui_ImplSDLRenderer3_ShutdownPlatformInterface()
{
    ImGui::DestroyPlatformWindows();
}

//-----------------------------------------------------------------------------

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif // #ifndef IMGUI_DISABLE
