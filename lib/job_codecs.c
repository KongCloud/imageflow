#include <zlib.h>
#include <stdio.h>
#include <jpeglib.h>
#include "imageflow_private.h"
#include "job.h"
#include "lcms2.h"

typedef enum flow_job_png_decoder_stage {
    flow_job_png_decoder_stage_Null = 0,
    flow_job_png_decoder_stage_Failed,
    flow_job_png_decoder_stage_NotStarted,
    flow_job_png_decoder_stage_BeginRead,
    flow_job_png_decoder_stage_FinishRead,
} flow_job_png_decoder_stage;

typedef enum flow_job_color_profile_source {
    flow_job_color_profile_source_null,
    flow_job_color_profile_source_ICCP,
    flow_job_color_profile_source_ICCP_GRAY,
    flow_job_color_profile_source_GAMA_CHRM,

} flow_job_color_profile_source;

struct flow_job_png_decoder_state {
    flow_job_png_decoder_stage stage;
    png_structp png_ptr;
    png_infop info_ptr;
    png_size_t rowbytes;
    png_size_t w;
    png_size_t h;
    jmp_buf error_handler_jmp;
    int color_type, bit_depth;
    png_const_voidp file_bytes;
    png_size_t file_bytes_count;
    png_size_t file_bytes_read;
    png_bytep pixel_buffer;
    size_t pixel_buffer_size;
    png_bytepp pixel_buffer_row_pointers;
    flow_context* context;
    cmsHPROFILE color_profile;
    flow_job_color_profile_source color_profile_source;
    double gamma;
};

static bool flow_job_png_decoder_reset(flow_context* c, struct flow_job_png_decoder_state* state)
{
    if (state->stage == flow_job_png_decoder_stage_FinishRead) {
        FLOW_free(c, state->pixel_buffer);
    }
    if (state->stage == flow_job_png_decoder_stage_Null) {
        state->pixel_buffer_row_pointers = NULL;
        state->color_profile = NULL;
        state->info_ptr = NULL;
        state->png_ptr = NULL;
    } else {
        if (state->png_ptr != NULL || state->info_ptr != NULL) {
            png_destroy_read_struct(&state->png_ptr, &state->info_ptr, NULL);
        }
        if (state->color_profile != NULL) {
            cmsCloseProfile(state->color_profile);
            state->color_profile = NULL;
        }
        if (state->pixel_buffer_row_pointers != NULL) {
            FLOW_free(c, state->pixel_buffer_row_pointers);
            state->pixel_buffer_row_pointers = NULL;
        }
    }
    state->color_profile_source = flow_job_color_profile_source_null;
    state->rowbytes = 0;
    state->color_type = 0;
    state->bit_depth = 0;
    state->context = c;
    state->w = 0;
    state->h = 0;
    state->gamma = 0.45455;
    state->pixel_buffer = NULL;
    state->pixel_buffer_size = -1;
    state->file_bytes_read = 0;
    state->stage = flow_job_png_decoder_stage_NotStarted;
    return true;
}

static png_bytepp create_row_pointers(flow_context* c, void* buffer, size_t buffer_size, size_t stride, size_t height)
{
    if (buffer_size < stride * height) {
        FLOW_error(c, flow_status_Invalid_internal_state);
        return NULL;
    }
    png_bytepp rows = (png_bytepp)FLOW_malloc(c, sizeof(png_bytep) * height);
    if (rows == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        return NULL;
    }
    unsigned int y;
    for (y = 0; y < height; ++y) {
        rows[y] = (png_bytep)((uint8_t*)buffer + (stride * y));
    }
    return rows;
}

static void png_decoder_error_handler(png_structp png_ptr, png_const_charp msg)
{
    struct flow_job_png_decoder_state* state = (struct flow_job_png_decoder_state*)png_get_error_ptr(png_ptr);

    if (state == NULL) {
        exit(42);
        abort(); // WTF?
    }
    FLOW_CONTEXT_SET_LAST_ERROR(state->context, flow_status_Png_decoding_failed);

    // Dispose of everything
    flow_job_png_decoder_reset(state->context, state);
    state->stage = flow_job_png_decoder_stage_Failed;

    longjmp(state->error_handler_jmp, 1);
}

static void custom_read_data(png_structp png_ptr, png_bytep buffer, png_size_t bytes_requested)
{
    struct flow_job_png_decoder_state* state = (struct flow_job_png_decoder_state*)png_get_io_ptr(png_ptr);

    if (state == NULL) {
        png_error(png_ptr, "Read Error");
    }
    if (bytes_requested > state->file_bytes_count - state->file_bytes_read) {
        png_error(png_ptr, "Read beyond end of data requested");
    }
    size_t bytes_read = umin64(state->file_bytes_count - state->file_bytes_read, bytes_requested);
    memcpy(buffer, (const uint8_t*)state->file_bytes + state->file_bytes_read, bytes_read);
    state->file_bytes_read += bytes_read;
}

static bool png_decoder_load_color_profile(flow_context* c, struct flow_job_png_decoder_state* state)
{

    // Get gamma
    if (!png_get_valid(state->png_ptr, state->info_ptr, PNG_INFO_sRGB)) {
        png_get_gAMA(state->png_ptr, state->info_ptr, &state->gamma);
    }

    // We assume that the underlying buffer can be freed after opening the profile, per
    // http://www.littlecms.com/1/jpegemb.c

    png_bytep profile_buf;
    uint32_t profile_length;

    cmsHPROFILE profile = NULL;

    // Pre-transform color_type (prior to all pre-decode format transforms)
    int is_color_png = state->color_type & PNG_COLOR_MASK_COLOR;

    if (png_get_iCCP(state->png_ptr, state->info_ptr, &(png_charp){ 0 }, &(int){ 0 }, &profile_buf, &profile_length)) {
        // Decode the ICC profile from the buffer
        profile = cmsOpenProfileFromMem(profile_buf, profile_length);
        cmsColorSpaceSignature colorspace = cmsGetColorSpace(profile);

        if (colorspace == cmsSigRgbData && is_color_png) {
            state->color_profile_source = flow_job_color_profile_source_ICCP;
        } else {
            if (colorspace == cmsSigGrayData && !is_color_png) {
                // TODO: warn about this
                state->color_profile_source = flow_job_color_profile_source_ICCP_GRAY;
                ;
            }
            cmsCloseProfile(profile);
            profile = NULL;
        }
    }

    if (profile == NULL && is_color_png && !png_get_valid(state->png_ptr, state->info_ptr, PNG_INFO_sRGB)
        && png_get_valid(state->png_ptr, state->info_ptr, PNG_INFO_gAMA)
        && png_get_valid(state->png_ptr, state->info_ptr, PNG_INFO_cHRM)) {

        // Use cHRM and gAMA to build profile
        cmsCIExyY white_point;
        cmsCIExyYTRIPLE primaries;

        png_get_cHRM(state->png_ptr, state->info_ptr, &white_point.x, &white_point.y, &primaries.Red.x,
                     &primaries.Red.y, &primaries.Green.x, &primaries.Green.y, &primaries.Blue.x, &primaries.Blue.y);

        white_point.Y = primaries.Red.Y = primaries.Green.Y = primaries.Blue.Y = 1.0;

        cmsToneCurve* gamma_table[3];
        gamma_table[0] = gamma_table[1] = gamma_table[2] = cmsBuildGamma(NULL, 1 / state->gamma);

        profile = cmsCreateRGBProfile(&white_point, &primaries, gamma_table);

        cmsFreeToneCurve(gamma_table[0]);

        state->color_profile_source = flow_job_color_profile_source_GAMA_CHRM;
    }

    state->color_profile = profile;
    return true;
}

static bool transform_to_srgb(flow_context* c, cmsHPROFILE current_profile, flow_bitmap_bgra* frame)
{
    if (current_profile != NULL) {
        cmsHPROFILE target_profile = cmsCreate_sRGBProfile();
        if (target_profile == NULL) {
            FLOW_error(c, flow_status_Out_of_memory);
            return false;
        }
        cmsUInt32Number format = frame->fmt == flow_bgr24 ? TYPE_BGR_8
                                                          : (frame->fmt == flow_bgra32 ? TYPE_BGRA_8 : TYPE_GRAY_8);

        cmsHTRANSFORM transform
            = cmsCreateTransform(current_profile, format, target_profile, format, INTENT_PERCEPTUAL, 0);
        if (transform == NULL) {
            cmsCloseProfile(target_profile);
            FLOW_error(c, flow_status_Out_of_memory);
            return false;
        }
        for (unsigned int i = 0; i < frame->h; i++) {
            cmsDoTransform(transform, frame->pixels + (frame->stride * i), frame->pixels + (frame->stride * i),
                           frame->w);
        }

        cmsDeleteTransform(transform);
        cmsCloseProfile(target_profile);
    }
    return true;
}

static bool flow_job_png_decoder_BeginRead(flow_context* c, struct flow_job_png_decoder_state* state)
{
    if (state->stage != flow_job_png_decoder_stage_NotStarted) {
        FLOW_error(c, flow_status_Invalid_internal_state);
        return false;
    }
    if (!flow_job_png_decoder_reset(c, state)) {
        state->stage = flow_job_png_decoder_stage_Failed;
        FLOW_error_return(c);
    }
    state->stage = flow_job_png_decoder_stage_BeginRead;

    state->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, state, png_decoder_error_handler, NULL);
    if (state->png_ptr == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        flow_job_png_decoder_reset(c, state);
        state->stage = flow_job_png_decoder_stage_Failed;
        return false;
    }

    state->info_ptr = png_create_info_struct(state->png_ptr);
    if (state->info_ptr == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        flow_job_png_decoder_reset(c, state);
        state->stage = flow_job_png_decoder_stage_Failed;
        return false;
    }
    // Set up error continuation
    if (setjmp(state->error_handler_jmp)) {
        // Execution comes back to this point if an error happens
        // We assume that the handler already set the context error
        return false;
    }
    // Custom read function req.d - reading from memory
    png_set_read_fn(state->png_ptr, state, custom_read_data);

    // Read header and chunks
    png_read_info(state->png_ptr, state->info_ptr);

    png_uint_32 w, h;
    // Get dimensions and info
    png_get_IHDR(state->png_ptr, state->info_ptr, &w, &h, &state->bit_depth, &state->color_type, NULL, NULL, NULL);
    state->w = w;
    state->h = h;

    // Parse gamma and color profile info
    if (!png_decoder_load_color_profile(c, state)) {
        FLOW_add_to_callstack(c);
        flow_job_png_decoder_reset(c, state);
        state->stage = flow_job_png_decoder_stage_Failed;
        return false;
    }

    // Now we need to figure out how big our pixel buffer needs to be to hold the entire image.
    // We need to apply some normalization filters so we have fewer variants.

    /* expand palette images to RGB, low-bit-depth grayscale images to 8 bits,
    * transparency chunks to full alpha channel; strip 16-bit-per-sample
    * images to 8 bits per sample; and convert grayscale to RGB[A] */

    // Fill in the alpha channel with FFFF if missing.
    if (!(state->color_type & PNG_COLOR_MASK_ALPHA)) {
        png_set_expand(state->png_ptr);
        png_set_filler(state->png_ptr, 65535L, PNG_FILLER_AFTER);
    }

    // Drop to 8-bit per channel; we can't handle 16-bit yet.
    if (state->bit_depth == 16) {
        png_set_strip_16(state->png_ptr);
    }
    // Convert grayscale to RGB.
    if (!(state->color_type & PNG_COLOR_MASK_COLOR))
        png_set_gray_to_rgb(state->png_ptr);

    // We use BGRA, not RGBA
    png_set_bgr(state->png_ptr);
    // We don't want to think about interlacing. Let libpng fix that up.

    // Update our info based on these new settings.
    png_read_update_info(state->png_ptr, state->info_ptr);

    // Now we can access a stride that represents the post-transform data.
    state->rowbytes = png_get_rowbytes(state->png_ptr, state->info_ptr);

    if (png_get_channels(state->png_ptr, state->info_ptr) != 4) {
        FLOW_error(c, flow_status_Invalid_internal_state);
        return false; // Should always be 4
    }
    // We set this, but it's ignored and overwritten by existing callers
    state->pixel_buffer_size = state->rowbytes * state->h;

    return true;
}

static bool flow_job_png_decoder_FinishRead(flow_context* c, struct flow_job_png_decoder_state* state)
{
    if (state->stage != flow_job_png_decoder_stage_BeginRead) {
        FLOW_error(c, flow_status_Invalid_internal_state);
        return false;
    }
    // We let the caller create the buffer
    //    state->pixel_buffer =  (png_bytep)FLOW_calloc (c, state->pixel_buffer_size, sizeof(png_bytep));
    if (state->pixel_buffer == NULL) {
        flow_job_png_decoder_reset(c, state);
        state->stage = flow_job_png_decoder_stage_Failed;
        FLOW_error(c, flow_status_Out_of_memory);
        return false;
    }

    state->stage = flow_job_png_decoder_stage_FinishRead;
    if (setjmp(state->error_handler_jmp)) {
        // Execution comes back to this point if an error happens
        return false;
    }

    state->pixel_buffer_row_pointers
        = create_row_pointers(c, state->pixel_buffer, state->pixel_buffer_size, state->rowbytes, state->h);
    if (state->pixel_buffer_row_pointers == NULL) {
        flow_job_png_decoder_reset(c, state);
        state->stage = flow_job_png_decoder_stage_Failed;
        FLOW_error_return(c);
    }

    // The real work
    png_read_image(state->png_ptr, state->pixel_buffer_row_pointers);

    png_read_end(state->png_ptr, NULL);

    // Not sure if we should just call reset instead, or not...
    png_destroy_read_struct(&state->png_ptr, &state->info_ptr, NULL);

    return true;
}

static void* codec_aquire_decode_png_on_buffer(flow_context* c, struct flow_job* job,
                                               struct flow_job_resource_buffer* buffer)
{
    // flow_job_png_decoder_state
    if (buffer->codec_state == NULL) {
        struct flow_job_png_decoder_state* state
            = (struct flow_job_png_decoder_state*)FLOW_malloc(c, sizeof(struct flow_job_png_decoder_state));
        if (state == NULL) {
            FLOW_error(c, flow_status_Out_of_memory);
            return NULL;
        }
        state->stage = flow_job_png_decoder_stage_Null;

        if (!flow_job_png_decoder_reset(c, state)) {
            FLOW_add_to_callstack(c);
            return NULL;
        }
        state->file_bytes = buffer->buffer;
        state->file_bytes_count = buffer->buffer_size;

        buffer->codec_state = (void*)state;
    }
    return buffer->codec_state;
}

static bool png_get_info(flow_context* c, struct flow_job* job, void* codec_state,
                         struct decoder_frame_info* decoder_frame_info_ref)
{
    struct flow_job_png_decoder_state* state = (struct flow_job_png_decoder_state*)codec_state;
    if (state->stage < flow_job_png_decoder_stage_BeginRead) {
        if (!flow_job_png_decoder_BeginRead(c, state)) {
            FLOW_error_return(c);
        }
    }
    decoder_frame_info_ref->w = state->w;
    decoder_frame_info_ref->h = state->h;
    decoder_frame_info_ref->format = flow_bgra32;
    return true;
}

static bool png_read_frame(flow_context* c, struct flow_job* job, void* codec_state, flow_bitmap_bgra* canvas)
{
    struct flow_job_png_decoder_state* state = (struct flow_job_png_decoder_state*)codec_state;
    if (state->stage == flow_job_png_decoder_stage_BeginRead) {
        state->pixel_buffer = canvas->pixels;
        state->pixel_buffer_size = canvas->stride * canvas->h;
        if (!flow_job_png_decoder_FinishRead(c, state)) {
            FLOW_error_return(c);
        }

        if (!transform_to_srgb(c, state->color_profile, canvas)) {
            FLOW_error_return(c);
        }
        return true;
    } else {
        FLOW_error(c, flow_status_Invalid_internal_state);
        return false;
    }
}

static void png_write_data_callback(png_structp png_ptr, png_bytep data, png_size_t length)
{
    struct flow_job_png_encoder_state* p = (struct flow_job_png_encoder_state*)png_get_io_ptr(png_ptr);
    size_t nsize = p->size + length;

    /* allocate or grow buffer */
    if (p->buffer)
        p->buffer = (char*)FLOW_realloc(p->context, p->buffer, nsize);
    else
        p->buffer = (char*)FLOW_malloc(p->context, nsize);

    if (!p->buffer)
        png_error(png_ptr, "Write Error"); // TODO: comprehend png error handling

    /* copy new bytes to end of buffer */
    memcpy(p->buffer + p->size, data, length);
    p->size += length;
}

static void png_encoder_error_handler(png_structp png_ptr, png_const_charp msg)
{
    struct flow_job_png_encoder_state* state = (struct flow_job_png_encoder_state*)png_get_error_ptr(png_ptr);

    if (state == NULL) {
        exit(42);
        abort(); // WTF?
    }
    FLOW_CONTEXT_SET_LAST_ERROR(state->context, flow_status_Png_encoding_failed);

    longjmp(state->error_handler_jmp_buf, 1);
}

static void png_flush_nullop(png_structp png_ptr) {}

bool flow_bitmap_bgra_write_png(flow_context* c, struct flow_job* job, void* codec_state, flow_bitmap_bgra* frame)
{
    struct flow_job_png_encoder_state* state = (struct flow_job_png_encoder_state*)codec_state;
    state->buffer = NULL;
    state->size = 0;
    state->context = c;

    if (setjmp(state->error_handler_jmp_buf)) {
        // Execution comes back to this point if an error happens
        // We assume that the handler already set the context error
        return false;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, state, png_encoder_error_handler,
                                                  NULL); // makepng_error, makepng_warning);
    png_infop info_ptr = NULL;
    if (png_ptr == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        return false;
    }

    png_set_compression_level(png_ptr, Z_BEST_SPEED);
    png_set_text_compression_level(png_ptr, Z_DEFAULT_COMPRESSION);

    png_set_write_fn(png_ptr, state, png_write_data_callback, png_flush_nullop);

    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL)
        png_error(png_ptr, "OOM allocating info structure"); // TODO: comprehend png error handling
    {

        png_bytepp rows = create_row_pointers(c, frame->pixels, frame->stride * frame->h, frame->stride, frame->h);
        if (rows == NULL) {
            FLOW_add_to_callstack(c);
            return false;
        }
        // TODO: check rows for NULL

        png_set_rows(png_ptr, info_ptr, rows);

        png_set_IHDR(png_ptr, info_ptr, (png_uint_32)frame->w, (png_uint_32)frame->h, 8, PNG_COLOR_TYPE_RGB_ALPHA,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

        png_set_sRGB(png_ptr, info_ptr, PNG_sRGB_INTENT_PERCEPTUAL);

        png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_BGR, NULL);

        FLOW_free(c, rows);
        rows = NULL;
        png_destroy_write_struct(&png_ptr, &info_ptr);
        // Copy the final result to the output resource, if it exists.
        if (state->output_resource != NULL) {
            state->output_resource->buffer = state->buffer;
            state->output_resource->buffer_size = state->size;
        }
    }
    // TODO: maybe ? png_destroy_write_struct(&nv_ptr, &nv_info);
    return true;
}

static void* codec_aquire_encode_png_on_buffer(flow_context* c, struct flow_job* job,
                                               struct flow_job_resource_buffer* buffer)
{
    // flow_job_png_decoder_state
    if (buffer->codec_state == NULL) {
        struct flow_job_png_encoder_state* state
            = (struct flow_job_png_encoder_state*)FLOW_malloc(c, sizeof(struct flow_job_png_encoder_state));
        if (state == NULL) {
            FLOW_error(c, flow_status_Out_of_memory);
            return NULL;
        }
        state->buffer = NULL;
        state->size = 0;
        state->context = c;
        state->output_resource = buffer;

        buffer->codec_state = (void*)state;
    }
    return buffer->codec_state;
}

typedef enum flow_job_jpg_decoder_stage {
    flow_job_jpg_decoder_stage_Null = 0,
    flow_job_jpg_decoder_stage_Failed,
    flow_job_jpg_decoder_stage_NotStarted,
    flow_job_jpg_decoder_stage_BeginRead,
    flow_job_jpg_decoder_stage_FinishRead,
} flow_job_jpg_decoder_stage;

struct flow_job_jpg_decoder_state {

    struct jpeg_error_mgr error_mgr; // MUST be first
    flow_job_jpg_decoder_stage stage;
    struct jpeg_decompress_struct* cinfo;
    size_t row_stride;
    size_t w;
    size_t h;
    int channels;
    jmp_buf error_handler_jmp;
    void* file_bytes;
    size_t file_bytes_count;
    uint8_t* pixel_buffer;
    size_t pixel_buffer_size;
    uint8_t** pixel_buffer_row_pointers;
    flow_context* context;
    cmsHPROFILE color_profile;
    flow_job_color_profile_source color_profile_source;
    double gamma;
};
static bool flow_job_jpg_decoder_reset(flow_context* c, struct flow_job_jpg_decoder_state* state);

static void my_error_exit(j_common_ptr cinfo)
{
    /* cinfo->err really points to a my_error_mgr struct, so coerce pointer */
    struct flow_job_jpg_decoder_state* state = (struct flow_job_jpg_decoder_state*)cinfo->err;

    /* Always display the message. */
    /* We could postpone this until after returning, if we chose. */
    (*cinfo->err->output_message)(cinfo);

    flow_job_jpg_decoder_reset(state->context, state);
    state->stage = flow_job_jpg_decoder_stage_Failed;

    /* Return control to the setjmp point */
    longjmp(state->error_handler_jmp, 1);
}

static const JOCTET EOI_BUFFER[1] = { JPEG_EOI };
typedef struct my_source_mgr {
    struct jpeg_source_mgr pub;
    const JOCTET* data;
    size_t len;
} my_source_mgr;

static void my_init_source(j_decompress_ptr cinfo) {}

static boolean my_fill_input_buffer(j_decompress_ptr cinfo)
{
    my_source_mgr* src = (my_source_mgr*)cinfo->src;
    // No more data.  Probably an incomplete image;  just output EOI.
    src->pub.next_input_byte = EOI_BUFFER;
    src->pub.bytes_in_buffer = 1;
    return TRUE;
}
static void my_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
    my_source_mgr* src = (my_source_mgr*)cinfo->src;
    if ((long)src->pub.bytes_in_buffer < num_bytes) {
        // Skipping over all of remaining data;  output EOI.
        src->pub.next_input_byte = EOI_BUFFER;
        src->pub.bytes_in_buffer = 1;
    } else {
        // Skipping over only some of the remaining data.
        src->pub.next_input_byte += num_bytes;
        src->pub.bytes_in_buffer -= num_bytes;
    }
}
static void my_term_source(j_decompress_ptr cinfo) {}

static void my_set_source_mgr(j_decompress_ptr cinfo, const char* data, size_t len)
{
    my_source_mgr* src;
    if (cinfo->src == 0) { // if this is first time;  allocate memory
        cinfo->src = (struct jpeg_source_mgr*)(*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT,
                                                                         sizeof(my_source_mgr));
    }
    src = (my_source_mgr*)cinfo->src;
    src->pub.init_source = my_init_source;
    src->pub.fill_input_buffer = my_fill_input_buffer;
    src->pub.skip_input_data = my_skip_input_data;
    src->pub.resync_to_restart = jpeg_resync_to_restart; // default
    src->pub.term_source = my_term_source;
    // fill the buffers
    src->data = (const JOCTET*)data;
    src->len = len;
    src->pub.bytes_in_buffer = len;
    src->pub.next_input_byte = src->data;
}

static bool flow_job_jpg_decoder_BeginRead(flow_context* c, struct flow_job_jpg_decoder_state* state)
{
    if (state->stage != flow_job_jpg_decoder_stage_NotStarted) {
        FLOW_error(c, flow_status_Invalid_internal_state);
        return false;
    }
    if (!flow_job_jpg_decoder_reset(c, state)) {
        state->stage = flow_job_jpg_decoder_stage_Failed;
        FLOW_error_return(c);
    }
    state->stage = flow_job_jpg_decoder_stage_BeginRead;

    state->cinfo = (struct jpeg_decompress_struct*)FLOW_calloc(c, 1, sizeof(struct jpeg_decompress_struct));

    /* We set up the normal JPEG error routines, then override error_exit. */
    state->cinfo->err = jpeg_std_error(&state->error_mgr);
    state->error_mgr.error_exit = my_error_exit;

    if (state->cinfo == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        flow_job_jpg_decoder_reset(c, state);
        state->stage = flow_job_jpg_decoder_stage_Failed;
        return false;
    }
    /* Establish the setjmp return context for my_error_exit to use. */
    if (setjmp(state->error_handler_jmp)) {
        /* If we get here, the JPEG code has signaled an error.
         */
        if (state->stage != flow_job_jpg_decoder_stage_Failed) {
            exit(404); // This should never happen, my_error_exit should fix it.
        }
        return false;
    }
    /* Now we can initialize the JPEG decompression object. */
    jpeg_create_decompress(state->cinfo);

    // Set a source manager for reading from memory
    my_set_source_mgr(state->cinfo, (const char*)state->file_bytes, state->file_bytes_count);

    /* Step 3: read file parameters with jpeg_read_header() */

    (void)jpeg_read_header(state->cinfo, TRUE);
    /* We can ignore the return value from jpeg_read_header since
     *   (a) suspension is not possible with the stdio data source, and
     *   (b) we passed TRUE to reject a tables-only JPEG file as an error.
     * See libjpeg.txt for more info.
     */

    /* Step 4: set parameters for decompression */

    /* In this example, we don't need to change any of the defaults set by
     * jpeg_read_header(), so we do nothing here.
     */

    state->cinfo->out_color_space = JCS_EXT_BGRA;

    /* Step 5: Start decompressor */

    (void)jpeg_start_decompress(state->cinfo);

    /* We may need to do some setup of our own at this point before reading
 * the data.  After jpeg_start_decompress() we have the correct scaled
 * output image dimensions available, as well as the output colormap
 * if we asked for color quantization.
 * In this example, we need to make an output work buffer of the right size.
 */
    /* JSAMPLEs per row in output buffer */
    state->row_stride = state->cinfo->output_width * state->cinfo->output_components;
    state->w = state->cinfo->output_width;
    state->h = state->cinfo->output_height;
    state->channels = state->cinfo->output_components;
    state->gamma = state->cinfo->output_gamma;

    return true;
}

static bool flow_job_jpg_decoder_FinishRead(flow_context* c, struct flow_job_jpg_decoder_state* state)
{
    if (state->stage != flow_job_jpg_decoder_stage_BeginRead) {
        FLOW_error(c, flow_status_Invalid_internal_state);
        return false;
    }
    // We let the caller create the buffer
    //    state->pixel_buffer =  (jpg_bytep)FLOW_calloc (c, state->pixel_buffer_size, sizeof(jpg_bytep));
    if (state->pixel_buffer == NULL) {
        flow_job_jpg_decoder_reset(c, state);
        state->stage = flow_job_jpg_decoder_stage_Failed;
        FLOW_error(c, flow_status_Out_of_memory);
        return false;
    }

    state->stage = flow_job_jpg_decoder_stage_FinishRead;
    if (setjmp(state->error_handler_jmp)) {
        // Execution comes back to this point if an error happens
        return false;
    }

    state->pixel_buffer_row_pointers
        = create_row_pointers(c, state->pixel_buffer, state->pixel_buffer_size, state->row_stride, state->h);
    if (state->pixel_buffer_row_pointers == NULL) {
        flow_job_jpg_decoder_reset(c, state);
        state->stage = flow_job_jpg_decoder_stage_Failed;
        FLOW_error_return(c);
    }

    uint32_t scanlines_read = 0;
    /* Step 6: while (scan lines remain to be read) */
    /*           jpeg_read_scanlines(...); */

    /* Here we use the library's state variable cinfo.output_scanline as the
     * loop counter, so that we don't have to keep track ourselves.
     */
    while (state->cinfo->output_scanline < state->cinfo->output_height) {
        /* jpeg_read_scanlines expects an array of pointers to scanlines.
         * Here the array is only one element long, but you could ask for
         * more than one scanline at a time if that's more convenient.
         */
        scanlines_read = jpeg_read_scanlines(
            state->cinfo, &state->pixel_buffer_row_pointers[state->cinfo->output_scanline], state->h);
    }

    if (scanlines_read < 1) {
        return false;
    }
    /* Step 7: Finish decompression */

    (void)jpeg_finish_decompress(state->cinfo);
    /* We can ignore the return value since suspension is not possible
     * with the stdio data source.
     */

    jpeg_destroy_decompress(state->cinfo);
    FLOW_free(c, state->cinfo);
    state->cinfo = NULL;

    return true;
}

static bool flow_job_jpg_decoder_reset(flow_context* c, struct flow_job_jpg_decoder_state* state)
{
    if (state->stage == flow_job_jpg_decoder_stage_FinishRead) {
        FLOW_free(c, state->pixel_buffer);
    }
    if (state->stage == flow_job_jpg_decoder_stage_Null) {
        state->pixel_buffer_row_pointers = NULL;
        state->color_profile = NULL;
        state->cinfo = NULL;
    } else {

        if (state->cinfo != NULL) {
            jpeg_destroy_decompress(state->cinfo);
            FLOW_free(c, state->cinfo);
            state->cinfo = NULL;
        }
        memset(&state->error_mgr, 0, sizeof(struct jpeg_error_mgr));

        if (state->color_profile != NULL) {
            cmsCloseProfile(state->color_profile);
            state->color_profile = NULL;
        }
        if (state->pixel_buffer_row_pointers != NULL) {
            FLOW_free(c, state->pixel_buffer_row_pointers);
            state->pixel_buffer_row_pointers = NULL;
        }
    }
    state->color_profile_source = flow_job_color_profile_source_null;
    state->row_stride = 0;
    state->context = c;
    state->w = 0;
    state->h = 0;
    state->gamma = 0.45455;
    state->pixel_buffer = NULL;
    state->pixel_buffer_size = -1;
    state->channels = 0;
    state->stage = flow_job_jpg_decoder_stage_NotStarted;
    return true;
}

static void* codec_aquire_decode_jpg_on_buffer(flow_context* c, struct flow_job* job,
                                               struct flow_job_resource_buffer* buffer)
{
    // flow_job_jpg_decoder_state
    if (buffer->codec_state == NULL) {
        struct flow_job_jpg_decoder_state* state
            = (struct flow_job_jpg_decoder_state*)FLOW_malloc(c, sizeof(struct flow_job_jpg_decoder_state));
        if (state == NULL) {
            FLOW_error(c, flow_status_Out_of_memory);
            return NULL;
        }
        state->stage = flow_job_jpg_decoder_stage_Null;

        if (!flow_job_jpg_decoder_reset(c, state)) {
            FLOW_add_to_callstack(c);
            return NULL;
        }
        state->file_bytes = buffer->buffer;
        state->file_bytes_count = buffer->buffer_size;

        buffer->codec_state = (void*)state;
    }
    return buffer->codec_state;
}

static bool jpg_get_info(flow_context* c, struct flow_job* job, void* codec_state,
                         struct decoder_frame_info* decoder_frame_info_ref)
{
    struct flow_job_jpg_decoder_state* state = (struct flow_job_jpg_decoder_state*)codec_state;
    if (state->stage < flow_job_jpg_decoder_stage_BeginRead) {
        if (!flow_job_jpg_decoder_BeginRead(c, state)) {
            FLOW_error_return(c);
        }
    }
    decoder_frame_info_ref->w = state->w;
    decoder_frame_info_ref->h = state->h;
    decoder_frame_info_ref->format = flow_bgra32; // state->channels == 1 ? flow_gray8 : flow_bgr24;
    return true;
}

static bool jpg_read_frame(flow_context* c, struct flow_job* job, void* codec_state, flow_bitmap_bgra* canvas)
{
    struct flow_job_jpg_decoder_state* state = (struct flow_job_jpg_decoder_state*)codec_state;
    if (state->stage == flow_job_jpg_decoder_stage_BeginRead) {
        state->pixel_buffer = canvas->pixels;
        state->pixel_buffer_size = canvas->stride * canvas->h;
        if (!flow_job_jpg_decoder_FinishRead(c, state)) {
            FLOW_error_return(c);
        }

        if (!transform_to_srgb(c, state->color_profile, canvas)) {
            FLOW_error_return(c);
        }
        return true;
    } else {
        FLOW_error(c, flow_status_Invalid_internal_state);
        return false;
    }
}

// typedef bool (*codec_dispose_fn)(flow_context *c, struct flow_job * job, void * codec_state);

struct flow_job_codec_definition flow_job_codec_defs[] = {
    { .type = flow_job_codec_type_decode_png,
      .aquire_on_buffer = codec_aquire_decode_png_on_buffer,
      .get_frame_info = png_get_info,
      .read_frame = png_read_frame,
      .dispose = NULL,
      .name = "decode png" },
    { .type = flow_job_codec_type_encode_png,
      .aquire_on_buffer = codec_aquire_encode_png_on_buffer,
      .write_frame = flow_bitmap_bgra_write_png,
      .dispose = NULL,
      .name = "encode png" },
    { .type = flow_job_codec_type_decode_jpg,
      .aquire_on_buffer = codec_aquire_decode_jpg_on_buffer,
      .get_frame_info = jpg_get_info,
      .read_frame = jpg_read_frame,
      .dispose = NULL,
      .name = "decode jpeg" },
    //        {.type = flow_job_codec_type_encode_jpg,
    //                .aquire_on_buffer = codec_aquire_encode_jpg_on_buffer,
    //                .write_frame = flow_bitmap_bgra_write_jpg,
    //                .dispose = NULL,
    //                .name = "encode png"}
};

int32_t flow_job_codec_defs_count = sizeof(flow_job_codec_defs) / sizeof(struct flow_job_codec_definition);
struct flow_job_codec_definition* flow_job_get_codec_definition(flow_context* c, flow_job_codec_type type)
{
    int i = 0;
    for (i = 0; i < flow_job_codec_defs_count; i++) {
        if (flow_job_codec_defs[i].type == type)
            return &flow_job_codec_defs[i];
    }
    FLOW_error(c, flow_status_Not_implemented);
    return NULL;
}

uint8_t png_bytes[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

uint8_t jpeg_bytes_a[] = { 0xFF, 0xD8, 0xFF, 0xDB };
uint8_t jpeg_bytes_b[] = { 0xFF, 0xD8, 0xFF, 0xE0 };
uint8_t jpeg_bytes_c[] = { 0xFF, 0xD8, 0xFF, 0xE1 };

struct flow_job_codec_magic_bytes flow_job_codec_magic_bytes_defs[]
    = { {
          .codec_type = flow_job_codec_type_decode_png, .byte_count = 7, .bytes = (uint8_t*)&png_bytes,
        },
        {
          .codec_type = flow_job_codec_type_decode_jpg, .byte_count = 4, .bytes = (uint8_t*)&jpeg_bytes_a,

        },
        {
          .codec_type = flow_job_codec_type_decode_jpg, .byte_count = 4, .bytes = (uint8_t*)&jpeg_bytes_b,

        },
        {
          .codec_type = flow_job_codec_type_decode_jpg, .byte_count = 4, .bytes = (uint8_t*)&jpeg_bytes_c,

        } };
int32_t flow_job_codec_magic_bytes_defs_count = sizeof(flow_job_codec_magic_bytes_defs)
                                                / sizeof(struct flow_job_codec_magic_bytes);

flow_job_codec_type flow_job_codec_select(flow_context* c, struct flow_job* job, uint8_t* data, size_t data_bytes)
{
    int32_t series_ix = 0;
    for (series_ix = 0; series_ix < flow_job_codec_magic_bytes_defs_count; series_ix++) {
        struct flow_job_codec_magic_bytes* magic = &flow_job_codec_magic_bytes_defs[series_ix];
        if (data_bytes < magic->byte_count) {
            continue;
        }
        bool match = true;
        uint32_t i;
        for (i = 0; i < magic->byte_count; i++) {
            if (magic->bytes[i] != data[i]) {
                match = false;
                break;
            }
        }
        if (match)
            return magic->codec_type;
    }
    return flow_job_codec_type_null;
}

void* flow_job_acquire_decoder_over_buffer(flow_context* c, struct flow_job* job,
                                           struct flow_job_resource_buffer* buffer, flow_job_codec_type type)
{

    struct flow_job_codec_definition* def = flow_job_get_codec_definition(c, type);
    if (def == NULL) {
        FLOW_add_to_callstack(c);
        return NULL;
    }
    return def->aquire_on_buffer(c, job, buffer);
}

bool flow_job_decoder_get_frame_info(flow_context* c, struct flow_job* job, void* codec_state, flow_job_codec_type type,
                                     struct decoder_frame_info* decoder_frame_info_ref)
{
    struct flow_job_codec_definition* def = flow_job_get_codec_definition(c, type);
    if (def == NULL) {
        FLOW_error_return(c);
    }
    if (!def->get_frame_info(c, job, codec_state, decoder_frame_info_ref)) {
        FLOW_error_return(c);
    }
    return true;
}

bool flow_job_decoder_read_frame(flow_context* c, struct flow_job* job, void* codec_state, flow_job_codec_type type,
                                 flow_bitmap_bgra* canvas)
{
    struct flow_job_codec_definition* def = flow_job_get_codec_definition(c, type);
    if (def == NULL) {
        FLOW_error_return(c);
    }
    if (!def->read_frame(c, job, codec_state, canvas)) {
        FLOW_error_return(c);
    }
    return true;
}

// typedef bool (*codec_dispose_fn)(flow_context *c, struct flow_job * job, void * codec_state);