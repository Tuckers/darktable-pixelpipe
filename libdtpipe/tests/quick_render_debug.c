#include <stdio.h>
#include <stdlib.h>
#include "dtpipe.h"

int main(int argc, char **argv) {
    if(argc < 2) { fprintf(stderr, "usage: %s <raf>\n", argv[0]); return 1; }
    fprintf(stderr, "init...\n"); fflush(stderr);
    dtpipe_init("./share/dtpipe");
    fprintf(stderr, "load...\n"); fflush(stderr);
    dt_image_t *img = dtpipe_load_raw(argv[1]);
    if(!img) { fprintf(stderr, "load failed\n"); return 1; }
    fprintf(stderr, "loaded: %dx%d\n", dtpipe_get_width(img), dtpipe_get_height(img)); fflush(stderr);
    fprintf(stderr, "create pipe...\n"); fflush(stderr);
    dt_pipe_t *pipe = dtpipe_create(img);
    if(!pipe) { fprintf(stderr, "create failed\n"); return 1; }
    fprintf(stderr, "pipe created\n"); fflush(stderr);
    fprintf(stderr, "render scale=0.05...\n"); fflush(stderr);
    dt_render_result_t *r = dtpipe_render(pipe, 0.05f);
    fprintf(stderr, "render returned: %s\n", r ? "OK" : "NULL"); fflush(stderr);
    if(r) fprintf(stderr, "size: %dx%d\n", r->width, r->height);
    return 0;
}
