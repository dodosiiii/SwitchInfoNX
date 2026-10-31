#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include <jpeglib.h>
#include <switch/services/ns.h>
#include <SDL2/SDL.h>
#include "game_icons.h"

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} GameJpegError;

static void game_jpeg_fail(j_common_ptr cinfo) {
    GameJpegError *err = (GameJpegError *)cinfo->err;
    longjmp(err->jump, 1);
}

SDL_Surface *game_icon_load(u64 application_id) {
    NsApplicationControlData *control = (NsApplicationControlData *)malloc(sizeof(*control));
    if (!control) return NULL;
    u64 actual_size = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage,
        application_id, control, sizeof(*control), &actual_size);
    if (R_FAILED(rc) || actual_size <= sizeof(control->nacp)) { free(control); return NULL; }

    size_t jpeg_size = actual_size - sizeof(control->nacp);
    if (jpeg_size > sizeof(control->icon)) jpeg_size = sizeof(control->icon);
    struct jpeg_decompress_struct decoder;
    GameJpegError error;
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = game_jpeg_fail;
    volatile bool decoder_created = false;
    SDL_Surface * volatile surface = NULL;
    if (setjmp(error.jump)) {
        if (decoder_created) jpeg_destroy_decompress(&decoder);
        if (surface) SDL_FreeSurface((SDL_Surface *)surface);
        free(control);
        return NULL;
    }

    jpeg_create_decompress(&decoder);
    decoder_created = true;
    jpeg_mem_src(&decoder, control->icon, (unsigned long)jpeg_size);
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&decoder); free(control); return NULL;
    }
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    if (!decoder.output_width || !decoder.output_height || decoder.output_width > 512 ||
        decoder.output_height > 512 || decoder.output_components != 3) {
        jpeg_destroy_decompress(&decoder); free(control); return NULL;
    }
    surface = SDL_CreateRGBSurfaceWithFormat(0, (int)decoder.output_width,
        (int)decoder.output_height, 24, SDL_PIXELFORMAT_RGB24);
    if (!surface) { jpeg_destroy_decompress(&decoder); free(control); return NULL; }
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW row = (JSAMPROW)((u8 *)((SDL_Surface *)surface)->pixels +
            decoder.output_scanline * ((SDL_Surface *)surface)->pitch);
        jpeg_read_scanlines(&decoder, &row, 1);
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    free(control);
    return (SDL_Surface *)surface;
}
