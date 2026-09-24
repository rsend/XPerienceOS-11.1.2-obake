
/*
 * Motorola's optional GPU ALTM implementation constructs a legacy
 * GraphicBuffer in storage that is too small for Nougat's GraphicBuffer.
 * Disable the complete ALTM lifecycle at its C entry points so neither the
 * incompatible initialization nor a later execute/deinit path can run.
 * ALTM callers treat zero as success and can continue without this optional
 * tone-mapping stage.
 */
extern "C" int altm_init(int width, int height) {
    (void) width;
    (void) height;
    return 0;
}

extern "C" int altm_execute(void* frame_proc) {
    (void) frame_proc;
    return 0;
}

extern "C" int altm_deinit() {
    return 0;
}
