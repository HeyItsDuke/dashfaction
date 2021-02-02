#pragma once

#include "../rf/bmpman.h"
#include "../rf/gr/gr.h"

void gr_apply_patch();
void gr_render_fps_counter();
int gr_font_get_default();
void gr_font_set_default(int font_id);
bool gr_render_to_texture(int bm_handle);
void gr_render_to_back_buffer();
void gr_delete_texture(int bm_handle);
bool gr_is_texture_format_supported(rf::BmFormat format);
void gr_bitmap_stretched_float(int bitmap_handle, float x, float y, float w, float h, float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::GrMode mode);

template<typename F>
void gr_font_run_with_default(int font_id, F fun)
{
    int old_font = gr_font_get_default();
    gr_font_set_default(font_id);
    fun();
    gr_font_set_default(old_font);
}
