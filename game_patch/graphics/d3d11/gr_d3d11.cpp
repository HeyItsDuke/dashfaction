#include <cassert>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../../rf/gr/gr.h"
#include "../../rf/file/file.h"
#include "../../rf/os/os.h"
#include "../../bmpman/bmpman.h"
#include "gr_d3d11.h"

using namespace rf;

static HMODULE d3d11_lib;
static std::optional<D3D11Renderer> d3d11_renderer;

void D3D11Renderer::window_active()
{
    if (gr::screen.window_mode == gr::FULLSCREEN) {
        xlog::warn("gr_d3d11_window_active SetFullscreenState");
        HRESULT hr = swap_chain_->SetFullscreenState(TRUE, nullptr);
        check_hr(hr, "SetFullscreenState");
    }
}

void D3D11Renderer::window_inactive()
{
    if (gr::screen.window_mode == gr::FULLSCREEN) {
        xlog::warn("gr_d3d11_window_inactive SetFullscreenState");
        HRESULT hr = swap_chain_->SetFullscreenState(FALSE, nullptr);
        check_hr(hr, "SetFullscreenState");
    }
}

void D3D11Renderer::init_device(HWND hwnd, HMODULE d3d11_lib)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = gr::screen.window_mode == gr::FULLSCREEN ? 2 : 1;
    sd.BufferDesc.Width = gr::screen.max_w;
    sd.BufferDesc.Height = gr::screen.max_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    //sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Numerator = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = gr::screen.window_mode == gr::WINDOWED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    auto pD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        reinterpret_cast<void(*)()>(GetProcAddress(d3d11_lib, "D3D11CreateDeviceAndSwapChain")));
    if (!pD3D11CreateDeviceAndSwapChain) {
        xlog::error("Failed to find D3D11CreateDeviceAndSwapChain procedure");
        abort();
    }

    // D3D_FEATURE_LEVEL feature_levels[] = {
    //     D3D_FEATURE_LEVEL_9_1,
    //     D3D_FEATURE_LEVEL_9_2,
    //     D3D_FEATURE_LEVEL_9_3,
    //     D3D_FEATURE_LEVEL_10_0,
    //     D3D_FEATURE_LEVEL_10_1,
    //     D3D_FEATURE_LEVEL_11_0,
    //     D3D_FEATURE_LEVEL_11_1
    // };

    DWORD flags = 0;
//#ifndef NDEBUG
    // Requires Windows 10 SDK
    //flags |= D3D11_CREATE_DEVICE_DEBUG;
//#endif
    D3D_FEATURE_LEVEL feature_level_supported;
    HRESULT hr = pD3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        feature_levels,
        std::size(feature_levels),
        // nullptr,
        // 0,
        D3D11_SDK_VERSION,
        &sd,
        &swap_chain_,
        &device_,
        &feature_level_supported,
        &context_
    );
    check_hr(hr, "D3D11CreateDeviceAndSwapChain");

    void gr_d3d11_init_error(ID3D11Device* device);
    gr_d3d11_init_error(device_);

    xlog::info("D3D11 feature level: 0x%x", feature_level_supported);
}

void D3D11Renderer::init_back_buffer()
{
    // Get a pointer to the back buffer
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_ID3D11Texture2D, reinterpret_cast<LPVOID*>(&back_buffer));
    check_hr(hr, "GetBuffer");

    // Create a render-target view
    hr = device_->CreateRenderTargetView(back_buffer, NULL, &back_buffer_view_);
    check_hr(hr, "CreateRenderTargetView");
}

void D3D11Renderer::init_depth_stencil_buffer()
{
    D3D11_TEXTURE2D_DESC depth_stencil_desc;
    ZeroMemory(&depth_stencil_desc, sizeof(depth_stencil_desc));
    depth_stencil_desc.Width = gr::screen.max_w;
    depth_stencil_desc.Height = gr::screen.max_h;
    depth_stencil_desc.MipLevels = 1;
    depth_stencil_desc.ArraySize = 1;
    depth_stencil_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_stencil_desc.SampleDesc.Count = 1;
    depth_stencil_desc.SampleDesc.Quality = 0;
    depth_stencil_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_stencil_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depth_stencil_desc.CPUAccessFlags = 0;
    depth_stencil_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> depth_stencil;
    HRESULT hr = device_->CreateTexture2D(&depth_stencil_desc, nullptr, &depth_stencil);
    check_hr(hr, "CreateTexture2D");

    hr = device_->CreateDepthStencilView(depth_stencil, nullptr, &depth_stencil_buffer_view_);
    check_hr(hr, "CreateDepthStencilView");
}

void D3D11Renderer::init_vertex_shader()
{
    rf::File file;
    if (file.open("default_vs.bin") != 0) {
        xlog::error("Cannot open vertex shader file");
        return;
    }
    int size = file.size();
    auto shader_data = std::make_unique<std::byte[]>(size);
    int bytes_read = file.read(shader_data.get(), size);
    xlog::info("Vertex shader size %d file size %d first byte %02x", bytes_read, size,
        static_cast<ubyte>(shader_data.get()[0]));
    HRESULT hr = device_->CreateVertexShader(shader_data.get(), size, nullptr, &vertex_shader_);
    check_hr(hr, "CreateVertexShader");

    D3D11_INPUT_ELEMENT_DESC desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        // { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    xlog::info("layout size %d", std::size(desc));
    hr = device_->CreateInputLayout(
        desc,
        std::size(desc),
        shader_data.get(),
        size,
        &input_layout_
    );
    check_hr(hr, "CreateInputLayout");

    context_->IASetInputLayout(input_layout_);
    context_->VSSetShader(vertex_shader_, nullptr, 0);
}

void D3D11Renderer::init_pixel_shader()
{
    rf::File file;
    if (file.open("default_ps.bin") != 0) {
        xlog::error("Cannot open pixel shader file");
        return;
    }
    int size = file.size();
    auto shader_data = std::make_unique<std::byte[]>(size);
    file.read(shader_data.get(), size);
    HRESULT hr = device_->CreatePixelShader(shader_data.get(), size, nullptr, &pixel_shader_);
    check_hr(hr, "CreatePixelShader");

    context_->PSSetShader(pixel_shader_, nullptr, 0);
}

D3D11Renderer::D3D11Renderer(HWND hwnd, HMODULE d3d11_lib)
{
    init_device(hwnd, d3d11_lib);
    init_back_buffer();
    init_depth_stencil_buffer();
    init_vertex_shader();
    init_pixel_shader();

    state_manager_.emplace(device_);
    texture_manager_.emplace(device_);

    render_context_.emplace(device_, context_, state_manager_.value(), texture_manager_.value());
    render_context_->set_render_target(back_buffer_view_, depth_stencil_buffer_view_);
    batch_manager_.emplace(device_, context_, render_context_.value());

    //gr::screen.mode = GR_DIRECT3D11;
    gr::screen.depthbuffer_type = gr::DEPTHBUFFER_Z;

    // TODO: move
    batch_manager_->bind_buffers();
}

D3D11Renderer::~D3D11Renderer()
{
    if (context_) {
        context_->ClearState();
    }
    if (gr::screen.window_mode == gr::FULLSCREEN) {
        swap_chain_->SetFullscreenState(FALSE, nullptr);
    }
}

void D3D11Renderer::bitmap(int bitmap_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y, gr::Mode mode)
{
    //xlog::info("gr_d3d11_bitmap");
    //gr_d3d11_set_texture(0, bitmap_handle);
    gr::screen.current_texture_1 = bitmap_handle;
    int bm_w, bm_h;
    bm::get_dimensions(bitmap_handle, &bm_w, &bm_h);
    gr::Vertex verts[4];
    gr::Vertex* verts_ptrs[] = {
        &verts[0],
        &verts[1],
        &verts[2],
        &verts[3],
    };
    float sx_left = gr::screen.offset_x + x;
    float sx_right = gr::screen.offset_x + x + w;
    float sy_top = gr::screen.offset_y + y;
    float sy_bottom = gr::screen.offset_y + y + h;
    float u_left = static_cast<float>(sx) / bm_w * (flip_x ? -1.0f : 1.0f);
    float u_right = static_cast<float>(sx + sw) / bm_w * (flip_x ? -1.0f : 1.0f);
    float v_top = static_cast<float>(sy) / bm_h * (flip_y ? -1.0f : 1.0f);
    float v_bottom = static_cast<float>(sy + sh) / bm_h * (flip_y ? -1.0f : 1.0f);
    verts[0].sx = sx_left;
    verts[0].sy = sy_top;
    verts[0].sw = 0.0f;
    verts[0].u1 = u_left;
    verts[0].v1 = v_top;
    verts[1].sx = sx_right;
    verts[1].sy = sy_top;
    verts[1].sw = 0.0f;
    verts[1].u1 = u_right;
    verts[1].v1 = v_top;
    verts[2].sx = sx_right;
    verts[2].sy = sy_bottom;
    verts[2].sw = 0.0f;
    verts[2].u1 = u_right;
    verts[2].v1 = v_bottom;
    verts[3].sx = sx_left;
    verts[3].sy = sy_bottom;
    verts[3].sw = 0.0f;
    verts[3].u1 = u_left;
    verts[3].v1 = v_bottom;
    batch_manager_->tmapper(std::size(verts_ptrs), verts_ptrs, 0, mode);
}

void D3D11Renderer::clear()
{
    float clear_color[4] = {
        gr::screen.current_color.red / 255.0f,
        gr::screen.current_color.green / 255.0f,
        gr::screen.current_color.blue / 255.0f,
        1.0f,
    };
    context_->ClearRenderTargetView(back_buffer_view_, clear_color);
}

void D3D11Renderer::zbuffer_clear()
{
    if (gr::screen.depthbuffer_type != gr::DEPTHBUFFER_NONE) {
        float depth = gr::screen.depthbuffer_type == gr::DEPTHBUFFER_Z ? 0.0f : 1.0f;
        context_->ClearDepthStencilView(depth_stencil_buffer_view_, D3D11_CLEAR_DEPTH, depth, 0);
    }
}

void D3D11Renderer::set_clip()
{
    batch_manager_->flush();
    D3D11_VIEWPORT vp;
    vp.TopLeftX = gr::screen.clip_left + gr::screen.offset_x;
    vp.TopLeftY = gr::screen.clip_top + gr::screen.offset_y;
    vp.Width = gr::screen.clip_width;
    vp.Height = gr::screen.clip_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
}

void D3D11Renderer::flip()
{
    //xlog::info("gr_d3d11_flip");
    batch_manager_->flush();
    HRESULT hr = swap_chain_->Present(0, 0);
    check_hr(hr, "Present");
}

int D3D11Renderer::lock(int bm_handle, int section, rf::gr::LockInfo *lock, int mode)
{
    return texture_manager_->lock(bm_handle, section, lock, mode);
}

void D3D11Renderer::unlock(rf::gr::LockInfo *lock)
{
    texture_manager_->unlock(lock);
}

void D3D11Renderer::tmapper(int nv, rf::gr::Vertex **vertices, int tmap_flags, rf::gr::Mode mode)
{
    batch_manager_->tmapper(nv, vertices, tmap_flags, mode);
}

HRESULT D3D11Renderer::get_device_removed_reason()
{
    return device_ ? device_->GetDeviceRemovedReason() : E_FAIL;
}

void gr_d3d11_msg_handler(UINT msg, WPARAM w_param, LPARAM l_param)
{
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (w_param) {
            xlog::warn("active %x %lx", w_param, l_param);
            d3d11_renderer->window_active();
        }
        else {
            xlog::warn("inactive %x %lx", w_param, l_param);
            d3d11_renderer->window_inactive();
        }
    }
}

void gr_d3d11_flip()
{
    d3d11_renderer->flip();
}

void gr_d3d11_close()
{
    xlog::info("gr_d3d11_close");
    d3d11_renderer.reset();
    FreeLibrary(d3d11_lib);
}

void gr_d3d11_init(HWND hwnd)
{
    xlog::info("gr_d3d11_init");
    d3d11_lib = LoadLibraryW(L"d3d11.dll");
    if (!d3d11_lib) {
        RF_DEBUG_ERROR("Failed to load d3d11.dll");
    }

    d3d11_renderer.emplace(hwnd, d3d11_lib);
    os_add_msg_handler(gr_d3d11_msg_handler);
}

void gr_d3d11_clear()
{
    d3d11_renderer->clear();
}

void gr_d3d11_bitmap(int bitmap_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y, gr::Mode mode)
{
    d3d11_renderer->bitmap(bitmap_handle, x, y, w, h, sx, sy, sw, sh, flip_x, flip_y, mode);
}

void gr_d3d11_set_clip()
{
    d3d11_renderer->set_clip();
}

void gr_d3d11_zbuffer_clear()
{
    d3d11_renderer->zbuffer_clear();
}

void gr_d3d11_tmapper(int nv, gr::Vertex **vertices, int tmap_flags, gr::Mode mode)
{
    d3d11_renderer->tmapper(nv, vertices, tmap_flags, mode);
}

int gr_d3d11_lock(int bm_handle, int section, gr::LockInfo *lock, int mode)
{
    return d3d11_renderer->lock(bm_handle, section, lock, mode);
}

void gr_d3d11_unlock(gr::LockInfo *lock)
{
    d3d11_renderer->unlock(lock);
}

HRESULT gr_d3d11_get_device_removed_reason()
{
    return d3d11_renderer ? d3d11_renderer->get_device_removed_reason() : E_FAIL;
}

void gr_d3d11_apply_patch()
{
    // AsmWriter{0x00545960}.jmp(gr_d3d11_init);

    // write_mem<ubyte>(0x0050CBE0 + 6, GR_DIRECT3D11);
    // AsmWriter{0x0050CBE9}.call(gr_d3d11_close);

    // write_mem<ubyte>(0x0050CE2A + 6, GR_DIRECT3D11);
    // AsmWriter{0x0050CE33}.jmp(gr_d3d11_flip);

    // write_mem<ubyte>(0x0050DF80 + 6, GR_DIRECT3D11);
    // AsmWriter{0x0050DF9D}.call(gr_d3d11_tmapper);

    using namespace asm_regs;
    AsmWriter{0x00520A90}.ret(); // bink_play
    AsmWriter{0x00544FC0}.jmp(gr_d3d11_flip); // gr_d3d_flip
    AsmWriter{0x00545230}.jmp(gr_d3d11_close); // gr_d3d_close
    AsmWriter{0x00545960}.jmp(gr_d3d11_init); // gr_d3d_init
    AsmWriter{0x00546730}.ret(); // gr_d3d_read_backbuffer
    AsmWriter{0x005468C0}.ret(); // gr_d3d_set_fog
    AsmWriter{0x00546A00}.mov(al, 1).ret(); // gr_d3d_is_mode_supported
    //AsmWriter{0x00546A40}.ret(); // gr_d3d_setup_frustum
    //AsmWriter{0x00546F60}.ret(); // gr_d3d_change_frustum
    //AsmWriter{0x00547150}.ret(); // gr_d3d_setup_3d
    //AsmWriter{0x005473F0}.ret(); // gr_d3d_start_instance
    //AsmWriter{0x00547540}.ret(); // gr_d3d_stop_instance
    //AsmWriter{0x005477A0}.ret(); // gr_d3d_project_vertex
    //AsmWriter{0x005478F0}.ret(); // gr_d3d_is_normal_facing
    //AsmWriter{0x00547960}.ret(); // gr_d3d_is_normal_facing_plane
    //AsmWriter{0x005479B0}.ret(); // gr_d3d_get_apparent_distance_from_camera
    //AsmWriter{0x005479D0}.ret(); // gr_d3d_screen_coords_from_world_coords
    AsmWriter{0x00547A60}.ret(); // gr_d3d_update_gamma_ramp
    AsmWriter{0x00547AC0}.ret(); // gr_d3d_set_texture_mip_filter
    AsmWriter{0x00550820}.ret(); // gr_d3d_page_in
    AsmWriter{0x005508C0}.jmp(gr_d3d11_clear); // gr_d3d_clear
    AsmWriter{0x00550980}.jmp(gr_d3d11_zbuffer_clear); // gr_d3d_zbuffer_clear
    AsmWriter{0x00550A30}.jmp(gr_d3d11_set_clip); // gr_d3d_set_clip
    AsmWriter{0x00550AA0}.jmp(gr_d3d11_bitmap); // gr_d3d_bitmap
    AsmWriter{0x00551450}.ret(); // gr_d3d_flush_after_color_change
    AsmWriter{0x00551460}.ret(); // gr_d3d_line
    AsmWriter{0x00551900}.jmp(gr_d3d11_tmapper); // gr_d3d_tmapper
    //AsmWriter{0x00553C60}.ret(); // gr_d3d_render_movable_solid - uses gr_d3d_render_face_list
    //AsmWriter{0x00553EE0}.ret(); // gr_d3d_vfx - uses gr_poly
    //AsmWriter{0x00554BF0}.ret(); // gr_d3d_vfx_facing - uses gr_d3d_3d_bitmap_angle, gr_d3d_render_volumetric_light
    //AsmWriter{0x00555080}.ret(); // gr_d3d_vfx_glow - uses gr_d3d_3d_bitmap_angle
    AsmWriter{0x00555100}.ret(); // gr_d3d_line_vertex
    //AsmWriter{0x005551E0}.ret(); // gr_d3d_line_vec - uses gr_d3d_line_vertex
    //AsmWriter{0x00555790}.ret(); // gr_d3d_3d_bitmap - uses gr_poly
    //AsmWriter{0x00555AC0}.ret(); // gr_d3d_3d_bitmap_angle - uses gr_poly
    //AsmWriter{0x00555B20}.ret(); // gr_d3d_3d_bitmap_angle_wh - uses gr_poly
    //AsmWriter{0x00555B80}.ret(); // gr_d3d_render_volumetric_light - uses gr_poly
    //AsmWriter{0x00555DC0}.ret(); // gr_d3d_laser - uses gr_tmapper
    //AsmWriter{0x005563F0}.ret(); // gr_d3d_cylinder - uses gr_line
    //AsmWriter{0x005565D0}.ret(); // gr_d3d_cone - uses gr_line
    //AsmWriter{0x005566E0}.ret(); // gr_d3d_sphere - uses gr_line
    //AsmWriter{0x00556AB0}.ret(); // gr_d3d_chain - uses gr_poly
    //AsmWriter{0x00556F50}.ret(); // gr_d3d_line_directed - uses gr_line_vertex
    //AsmWriter{0x005571F0}.ret(); // gr_d3d_line_arrow - uses gr_line_vertex
    //AsmWriter{0x00557460}.ret(); // gr_d3d_render_particle_sys_particle - uses gr_poly, gr_3d_bitmap_angle
    //AsmWriter{0x00557D40}.ret(); // gr_d3d_render_bolts - uses gr_poly, gr_line
    //AsmWriter{0x00558320}.ret(); // gr_d3d_render_geomod_debris - uses gr_poly
    //AsmWriter{0x00558450}.ret(); // gr_d3d_render_glass_shard - uses gr_poly
    AsmWriter{0x00558550}.ret(); // gr_d3d_render_face_wireframe
    //AsmWriter{0x005585F0}.ret(); // gr_d3d_render_weapon_tracer - uses gr_poly
    //AsmWriter{0x005587C0}.ret(); // gr_d3d_poly - uses gr_d3d_tmapper
    AsmWriter{0x00558920}.ret(); // gr_d3d_render_geometry_wireframe
    AsmWriter{0x00558960}.ret(); // gr_d3d_render_geometry_in_editor
    AsmWriter{0x00558C40}.ret(); // gr_d3d_render_sel_face_in_editor
    //AsmWriter{0x00558D40}.ret(); // gr_d3d_world_poly - uses gr_d3d_poly
    //AsmWriter{0x00558E30}.ret(); // gr_d3d_3d_bitmap_stretched_square - uses gr_d3d_world_poly
    //AsmWriter{0x005590F0}.ret(); // gr_d3d_rod - uses gr_d3d_world_poly
    AsmWriter{0x005596C0}.ret(); // gr_d3d_render_face_list_colored
    AsmWriter{0x0055B520}.ret(); // gr_d3d_texture_save_cache
    AsmWriter{0x0055B550}.ret(); // gr_d3d_texture_flush_cache
    AsmWriter{0x0055CDC0}.ret(); // gr_d3d_mark_texture_dirty
    AsmWriter{0x0055CE00}.jmp(gr_d3d11_lock); // gr_d3d_lock
    AsmWriter{0x0055CF60}.jmp(gr_d3d11_unlock); // gr_d3d_unlock
    AsmWriter{0x0055CFA0}.ret(); // gr_d3d_get_texel
    AsmWriter{0x0055D160}.ret(); // gr_d3d_texture_add_ref
    AsmWriter{0x0055D190}.ret(); // gr_d3d_texture_remove_ref
    AsmWriter{0x0055F5E0}.ret(); // gr_d3d_render_static_solid
    AsmWriter{0x00561650}.ret(); // gr_d3d_render_face_list
    AsmWriter{0x0052FA40}.ret(); // gr_d3d_render_vif_mesh
}
