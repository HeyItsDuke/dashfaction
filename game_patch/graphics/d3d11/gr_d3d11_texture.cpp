#include <cstring>
#include "gr_d3d11.h"
#include "gr_d3d11_texture.h"
#include "../../bmpman/bmpman.h"

using namespace rf;

namespace df::gr::d3d11
{
    TextureManager::TextureManager(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> device_context) :
        device_{std::move(device)}, device_context_{std::move(device_context)}
    {
        texture_cache_.reserve(512);
    }

    TextureManager::Texture TextureManager::create_texture(int bm_handle, bm::Format fmt, int w, int h, ubyte* bits, ubyte* pal, bool staging)
    {
        auto [dxgi_format, supported_fmt] = get_supported_texture_format(fmt);
        CD3D11_TEXTURE2D_DESC desc{
            dxgi_format,
            static_cast<UINT>(w),
            static_cast<UINT>(h),
            1, // arraySize
            1, // mipLevels
            staging ? 0u : static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE),
            staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT,
            staging ? D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE : 0u,
        };
        // TODO: mapmaps?

        D3D11_SUBRESOURCE_DATA subres_data{nullptr, 0, 0};
        std::unique_ptr<ubyte[]> converted_bits;
        D3D11_SUBRESOURCE_DATA* subres_data_ptr = nullptr;
        if (bits) {
            if (supported_fmt != fmt) {
                xlog::trace("Converting texture %d -> %d", fmt, supported_fmt);
                converted_bits = std::make_unique<ubyte[]>(w * h * 4);
                ::bm_convert_format(converted_bits.get(), supported_fmt, bits, fmt, w, h,
                    w * bm_bytes_per_pixel(supported_fmt), w * bm_bytes_per_pixel(fmt), pal);
                subres_data.pSysMem = converted_bits.get();
                subres_data.SysMemPitch = w * bm_bytes_per_pixel(supported_fmt);
            }
            else {
                xlog::trace("Creating texture without conversion: format %d", fmt);
                subres_data.pSysMem = bits;
                subres_data.SysMemPitch = w * bm_bytes_per_pixel(fmt);
            }
            subres_data_ptr = &subres_data;
        }
        else {
            xlog::trace("Creating uninitialized texture");
        }

        ComPtr<ID3D11Texture2D> d3d_texture;
        HRESULT hr = device_->CreateTexture2D(&desc, subres_data_ptr, &d3d_texture);
        check_hr(hr, "CreateTexture2D cpu");

        if (staging) {
            return {bm_handle, desc.Format, d3d_texture};
        }
        else {
            Texture texture{bm_handle, desc.Format, {}};
            texture.gpu_texture = d3d_texture;
            return texture;
        }
    }

    TextureManager::Texture TextureManager::create_render_target(int bm_handle, int w, int h)
    {
        CD3D11_TEXTURE2D_DESC desc{
            DXGI_FORMAT_R8G8B8A8_UNORM,
            static_cast<UINT>(w),
            static_cast<UINT>(h),
            1, // arraySize
            1, // mipLevels
            D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,
        };
        ComPtr<ID3D11Texture2D> gpu_texture;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &gpu_texture);
        check_hr(hr, "CreateTexture2D render target");

        ComPtr<ID3D11RenderTargetView> render_target_view;
        hr = device_->CreateRenderTargetView(gpu_texture, nullptr, &render_target_view);
        check_hr(hr, "CreateRenderTargetView");

        return {bm_handle, desc.Format, std::move(gpu_texture), std::move(render_target_view)};
    }

    TextureManager::Texture TextureManager::load_texture(int bm_handle, bool staging)
    {
        xlog::info("Creating texture for bitmap %s handle %d format %d", bm::get_filename(bm_handle), bm_handle, bm::get_format(bm_handle));

        int w = 0, h = 0;
        bm::get_dimensions(bm_handle, &w, &h);
        if (w <= 0 || h <= 0) {
            xlog::warn("Bad bitmap dimensions: handle %d", bm_handle);
            return {};
        }

        bm::Format fmt = bm::get_format(bm_handle);
        if (fmt == bm::FORMAT_RENDER_TARGET) {
            xlog::info("Creating render target: handle %d", bm_handle);
            return create_render_target(bm_handle, w, h);
        }

        if (bm::get_type(bm_handle) == bm::TYPE_USER) {
            xlog::trace("Creating user bitmap texture: handle %d", bm_handle);
            auto texture = create_texture(bm_handle, fmt, w, h, nullptr, nullptr, staging);
            return texture;
        }

        ubyte* bm_bits = nullptr;
        ubyte* bm_pal = nullptr;
        xlog::trace("Locking bitmap");
        fmt = bm::lock(bm_handle, &bm_bits, &bm_pal);
        if (fmt == bm::FORMAT_NONE || bm_bits == nullptr || bm_pal != nullptr) {
            xlog::warn("Bitmap lock failed or unsupported bitmap");
            return {};
        }

        xlog::trace("Creating normal texture: handle %d", bm_handle);
        auto texture = create_texture(bm_handle, fmt, w, h, bm_bits, bm_pal, staging);

        xlog::trace("Unlocking bitmap");
        bm::unlock(bm_handle);

        xlog::trace("Texture created");
        return texture;
    }

    TextureManager::Texture& TextureManager::get_or_load_texture(int bm_handle, bool staging)
    {
        // Note: bm_index will change for each animation frame but bm_handle will stay the same
        int bm_index = rf::bm::get_cache_slot(bm_handle);
        auto it = texture_cache_.find(bm_index);
        if (it != texture_cache_.end()) {
            return it->second;
        }

        auto insert_result = texture_cache_.insert({bm_index, std::move(load_texture(bm_handle, staging))});
        return insert_result.first->second;
    }

    ID3D11ShaderResourceView* TextureManager::lookup_texture(int bm_handle)
    {
        if (bm_handle < 0) {
            return nullptr;
        }
        Texture& texture = get_or_load_texture(bm_handle, false);
        if (!texture.texture_view) {
            xlog::trace("Creating GPU texture for %s", bm::get_filename(bm_handle));
        }
        return texture.get_or_create_texture_view(device_, device_context_);
    }

    ID3D11RenderTargetView* TextureManager::lookup_render_target(int bm_handle)
    {
        if (bm_handle < 0) {
            return nullptr;
        }
        Texture& texture = get_or_load_texture(bm_handle, false);
        return texture.render_target_view;
    }

    void TextureManager::save_cache()
    {
        for (auto& e : texture_cache_) {
            Texture& texture = e.second;
            ++texture.save_cache_count;
        }
    }

    void TextureManager::flush_cache(bool force)
    {
        xlog::info("Flushing texture cache");
        if (force) {
            xlog::info("Flushing all textures");
            texture_cache_.clear();
        }
        else {
            auto it = texture_cache_.begin();
            while (it != texture_cache_.end()) {
                Texture& texture = it->second;
                if (texture.save_cache_count > 0) {
                    --texture.save_cache_count;
                }
                else if (texture.ref_count <= 0) {
                    xlog::trace("Flushing texture: handle %d", texture.bm_handle);
                    it = texture_cache_.erase(it);
                    continue;
                }
                ++it;
            }
        }
    }

    void TextureManager::add_ref(int bm_handle)
    {
        page_in(bm_handle);
        int bm_index = rf::bm::get_cache_slot(bm_handle);
        auto it = texture_cache_.find(bm_index);
        if (it != texture_cache_.end()) {
            Texture& texture = it->second;
            ++texture.ref_count;
        }
    }

    void TextureManager::remove_ref(int bm_handle)
    {
        int bm_index = rf::bm::get_cache_slot(bm_handle);
        auto it = texture_cache_.find(bm_index);
        if (it != texture_cache_.end()) {
            Texture& texture = it->second;
            assert(texture.ref_count > 0);
            --texture.ref_count;
            if (texture.ref_count <= 0) {
                xlog::trace("Flushing texture after ref removal: handle %d", texture.bm_handle);
                texture_cache_.erase(it);
            }
        }
    }

    void TextureManager::remove(int bm_handle)
    {
        int bm_index = rf::bm::get_cache_slot(bm_handle);
        texture_cache_.erase(bm_index);
    }

    std::pair<DXGI_FORMAT, bm::Format> TextureManager::determine_supported_texture_format(bm::Format fmt)
    {
        std::vector<std::pair<DXGI_FORMAT, bm::Format>> candidates;
        switch (fmt) {
            case bm::FORMAT_565_RGB:
                candidates.emplace_back(DXGI_FORMAT_B5G6R5_UNORM, bm::FORMAT_565_RGB);
                candidates.emplace_back(DXGI_FORMAT_B8G8R8X8_UNORM, bm::FORMAT_8888_ARGB);
                break;
            case bm::FORMAT_1555_ARGB:
                candidates.emplace_back(DXGI_FORMAT_B5G5R5A1_UNORM, bm::FORMAT_1555_ARGB);
                candidates.emplace_back(DXGI_FORMAT_B8G8R8A8_UNORM, bm::FORMAT_8888_ARGB);
                break;
            case bm::FORMAT_4444_ARGB:
                candidates.emplace_back(DXGI_FORMAT_B4G4R4A4_UNORM, bm::FORMAT_4444_ARGB);
                candidates.emplace_back(DXGI_FORMAT_B8G8R8A8_UNORM, bm::FORMAT_8888_ARGB);
                break;
            // Formats below should always be supported
            case bm::FORMAT_888_RGB: return {DXGI_FORMAT_B8G8R8X8_UNORM, bm::FORMAT_8888_ARGB};
            case bm::FORMAT_8888_ARGB: return {DXGI_FORMAT_B8G8R8A8_UNORM, fmt};
            case bm::FORMAT_DXT1: return {DXGI_FORMAT_BC1_UNORM, fmt};
            case bm::FORMAT_DXT3: return {DXGI_FORMAT_BC2_UNORM, fmt};
            case bm::FORMAT_DXT5: return {DXGI_FORMAT_BC3_UNORM, fmt};
            // Other formats
            default: return {DXGI_FORMAT_B8G8R8A8_UNORM, bm::FORMAT_8888_ARGB};
        }

        unsigned format_support = 0;
        for (auto& p: candidates) {
            HRESULT hr = device_->CheckFormatSupport(p.first, &format_support);
            check_hr(hr, "CheckFormatSupport");
            if (format_support & D3D11_FORMAT_SUPPORT_TEXTURE2D) {
                return p;
            }
        }
        // We shouldn't get here (last candidate should always be supported)
        assert(false);
        return {DXGI_FORMAT_B8G8R8A8_UNORM, bm::FORMAT_8888_ARGB};
    }

    std::pair<DXGI_FORMAT, bm::Format> TextureManager::get_supported_texture_format(rf::bm::Format fmt)
    {
        auto it = supported_texture_format_cache_.find(fmt);
        if (it != supported_texture_format_cache_.end()) {
            return it->second;
        }
        std::pair<DXGI_FORMAT, bm::Format> p = determine_supported_texture_format(fmt);
        supported_texture_format_cache_.insert({fmt, p});
        return p;
    }

    rf::bm::Format TextureManager::get_bm_format(DXGI_FORMAT dxgi_fmt)
    {
        switch (dxgi_fmt) {
            case DXGI_FORMAT_B5G6R5_UNORM: return bm::FORMAT_565_RGB;
            case DXGI_FORMAT_B5G5R5A1_UNORM: return bm::FORMAT_1555_ARGB;
            case DXGI_FORMAT_B4G4R4A4_UNORM: return bm::FORMAT_4444_ARGB;
            case DXGI_FORMAT_B8G8R8A8_UNORM: return bm::FORMAT_8888_ARGB;
            case DXGI_FORMAT_B8G8R8X8_UNORM: return bm::FORMAT_8888_ARGB;
            case DXGI_FORMAT_BC1_UNORM: return bm::FORMAT_DXT1;
            case DXGI_FORMAT_BC2_UNORM: return bm::FORMAT_DXT3;
            case DXGI_FORMAT_BC3_UNORM: return bm::FORMAT_DXT5;
            default: return bm::FORMAT_NONE;
        }
    }

    static D3D11_MAP convert_lock_mode_to_map_type(gr::LockMode mode)
    {
        switch (mode) {
            case gr::LOCK_READ_ONLY:
                return D3D11_MAP_READ;
            case gr::LOCK_READ_ONLY_WRITE:
                return D3D11_MAP_READ_WRITE;
            case gr::LOCK_WRITE_ONLY:
                return D3D11_MAP_WRITE;
            default:
                xlog::warn("Invalid lock mode: %d", mode);
                assert(false);
                return D3D11_MAP_READ_WRITE;
        }
    }

    bool TextureManager::lock(int bm_handle, int section, gr::LockInfo *lock)
    {
        if (section != 0) {
            return false;
        }

        int w, h;
        bm::get_dimensions(bm_handle, &w, &h);
        if (w == 0 || h == 0) {
            return false;
        }

        Texture& texture = get_or_load_texture(bm_handle, true);
        ID3D11Texture2D* staging_texture = texture.get_or_create_staging_texture(device_, device_context_,
            lock->mode != gr::LOCK_WRITE_ONLY);
        if (!staging_texture) {
            xlog::warn("Attempted to lock texture without staging resource %d", texture.bm_handle);
            return false;
        }

        D3D11_TEXTURE2D_DESC desc;
        staging_texture->GetDesc(&desc);

        D3D11_MAPPED_SUBRESOURCE mapped_texture;
        D3D11_MAP map_type = convert_lock_mode_to_map_type(lock->mode);
        HRESULT hr = device_context_->Map(staging_texture, 0, map_type, 0, &mapped_texture);
        check_hr(hr, "Map texture");

        lock->bm_handle = bm_handle;
        lock->section = section;
        lock->format = get_bm_format(desc.Format);
        lock->data = reinterpret_cast<rf::ubyte*>(mapped_texture.pData);
        lock->w = w;
        lock->h = h;
        lock->stride_in_bytes = mapped_texture.RowPitch;
        // Note: lock->mode is set by gr_lock
        xlog::trace("locked texture: handle %d format %d size %dx%d data %p", lock->bm_handle, lock->format, lock->w, lock->h, lock->data);
        return true;
    }

    void TextureManager::unlock(gr::LockInfo *lock)
    {
        xlog::trace("unlocking texture: handle %d format %d size %dx%d data %p", lock->bm_handle, lock->format, lock->w, lock->h, lock->data);
        Texture& texture = get_or_load_texture(lock->bm_handle, true);
        if (texture.cpu_texture) {
            device_context_->Unmap(texture.cpu_texture, 0);
            if (lock->mode != rf::gr::LOCK_READ_ONLY && texture.gpu_texture) {
                device_context_->CopySubresourceRegion(texture.gpu_texture, 0, 0, 0, 0, texture.cpu_texture, 0, nullptr);
            }
        }
    }

    void TextureManager::get_texel(
        [[maybe_unused]] int bm_handle,
        [[maybe_unused]] float u,
        [[maybe_unused]] float v,
        rf::gr::Color *clr)
    {
        const auto& it = texture_cache_.find(bm_handle);
        if (it != texture_cache_.end()) {
            Texture& texture = it->second;
            D3D11_MAPPED_SUBRESOURCE mapped_texture;
            HRESULT hr = device_context_->Map(texture.cpu_texture, 0, D3D11_MAP_READ, 0, &mapped_texture);
            check_hr(hr, "Map texture");

            // TODO: convert color

            device_context_->Unmap(texture.cpu_texture, 0);
        }
        clr->set(255, 255, 255, 255);
    }

    void TextureManager::Texture::init_gpu_texture(ID3D11Device* device, ID3D11DeviceContext* device_context)
    {
        if (!gpu_texture) {
            if (!cpu_texture) {
                xlog::warn("Both GPU and CPU textures are missing");
                return;
            }

            D3D11_TEXTURE2D_DESC cpu_desc;
            cpu_texture->GetDesc(&cpu_desc);

            CD3D11_TEXTURE2D_DESC desc{
                cpu_desc.Format,
                cpu_desc.Width,
                cpu_desc.Height,
                cpu_desc.ArraySize,
                cpu_desc.MipLevels,
            };
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &gpu_texture);
            check_hr(hr, "CreateTexture2D gpu");

            // Copy only first level
            // TODO: mapmaps?
            device_context->CopyResource(gpu_texture, cpu_texture);
        }

        HRESULT hr = device->CreateShaderResourceView(gpu_texture, nullptr, &texture_view);
        check_hr(hr, "CreateShaderResourceView");

        xlog::trace("Created GPU texture");
    }

    void TextureManager::Texture::init_cpu_texture(ID3D11Device* device, ID3D11DeviceContext* device_context, bool copy_from_gpu)
    {
        if (!gpu_texture) {
            xlog::warn("Both GPU and CPU textures are missing");
            return;
        }

        D3D11_TEXTURE2D_DESC gpu_desc;
        gpu_texture->GetDesc(&gpu_desc);

        CD3D11_TEXTURE2D_DESC desc{
            gpu_desc.Format,
            gpu_desc.Width,
            gpu_desc.Height,
            gpu_desc.ArraySize,
            gpu_desc.MipLevels,
            0u,
            D3D11_USAGE_STAGING,
            D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,
        };
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &cpu_texture);
        check_hr(hr, "CreateTexture2D staging");

        if (copy_from_gpu) {
            device_context->CopyResource(cpu_texture, gpu_texture);
        }
    }

    rf::bm::Format TextureManager::read_back_buffer(ID3D11Texture2D* back_buffer, int x, int y, int w, int h, rf::ubyte* data)
    {
        D3D11_TEXTURE2D_DESC gpu_desc;
        back_buffer->GetDesc(&gpu_desc);
        bm::Format bm_format = get_bm_format(gpu_desc.Format);
        if (data) {
            // TODO: use ResolveSubresource if multi-sampling is enabled
            if (!back_buffer_staging_texture_) {
                CD3D11_TEXTURE2D_DESC cpu_desc{
                    gpu_desc.Format,
                    gpu_desc.Width,
                    gpu_desc.Height,
                    1, // arraySize
                    1, // mipLevels
                    0, // bindFlags
                    D3D11_USAGE_STAGING,
                    D3D11_CPU_ACCESS_READ,
                };
                HRESULT hr = device_->CreateTexture2D(&cpu_desc, nullptr, &back_buffer_staging_texture_);
                check_hr(hr, "CreateTexture2D back buffer staging texture");
            }
            device_context_->CopySubresourceRegion(back_buffer_staging_texture_, 0, 0, 0, 0, back_buffer, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped_res;
            HRESULT hr = device_context_->Map(back_buffer_staging_texture_, 0, D3D11_MAP_READ, 0, &mapped_res);
            check_hr(hr, "Map back buffer staging texture");
            ubyte* src_ptr = reinterpret_cast<ubyte*>(mapped_res.pData);
            ubyte* dst_ptr = data;

            src_ptr += y * mapped_res.RowPitch;
            int bytes_per_pixel = bm_bytes_per_pixel(bm_format);
            int rows_left = h;
            while (rows_left > 0) {
                int bytes_to_copy = w * bytes_per_pixel;
                std::memcpy(dst_ptr, src_ptr + x * bytes_per_pixel, bytes_to_copy);
                dst_ptr += bytes_to_copy;
                src_ptr += mapped_res.RowPitch;
                --rows_left;
            }
            device_context_->Unmap(back_buffer_staging_texture_, 0);
        }
        return bm_format;
    }
}