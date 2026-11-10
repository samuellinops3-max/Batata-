static void gfx_init(void)
{
	videoState state;
	videoResolution res;
	videoConfiguration vconfig;
	u32 buf_size;
	int i;

	videoGetState(0, 0, &state);
	videoGetResolution(state.displayMode.resolution, &res);

	fb_width  = res.width;
	fb_height = res.height;
	fb_pitch  = res.width * sizeof(u32);

	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolutionId = state.displayMode.resolution;
	vconfig.format       = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch        = fb_pitch;
	vconfig.aspect       = VIDEO_ASPECT_AUTO;
	videoConfigure(0, &vconfig, NULL, 0);
	videoGetState(0, 0, &state);

	host_addr = memalign(HOST_ALIGN, HOST_SIZE);
	rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr);

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	buf_size = fb_pitch * fb_height;
	for (i = 0; i < 2; i++) {
		fb[i] = rsxMemalign(64, buf_size);
		rsxAddressToOffset(fb[i], &fb_offset[i]);
		gcmSetDisplayBuffer(i, fb_offset[i], fb_pitch, fb_width, fb_height);
	}
}