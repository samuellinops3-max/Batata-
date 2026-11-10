/*
 * Batatinha Frita 1, 3 - versao 2 (3D por software)
 *
 * Voce e uma crianca olhando um guarda de costas, do fundo de um campo.
 *  - Fase "andando" (guarda de costas): aperte X para dar um passo.
 *  - Fase "batatinha frita!" (guarda virado com olhos abertos): se estiver
 *    segurando X, voce se mexeu e volta para o inicio.
 *  - Chegar perto do guarda = vitoria. Aperte X para jogar de novo.
 *
 * Renderizacao: rasterizador em CPU com z-buffer, sem shaders. A cena e
 * desenhada numa resolucao interna pequena (RW x RH) e ampliada para o
 * framebuffer do RSX.
 *
 * ATENCAO: as partes de video (gfx_*) e de controle (input_*) foram escritas
 * de memoria. Se o build falhar ou o X nao responder, compare com o main.c
 * do Cobra Arena, que roda no PS3, e copie as chamadas exatas.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <rsx/rsx.h>
#include <io/pad.h>

#define HOST_SIZE   (32 * 1024 * 1024)
#define HOST_ALIGN  (1024 * 1024)
#define CB_SIZE     (1024 * 1024)

/* resolucao interna do renderizador por software */
#define RW     320
#define RH     180
#define FOCAL  220.0f

#define WALK_FRAMES   150   /* fase de andar (aprox. 2,5 s a 60 fps) */
#define FREEZE_FRAMES 120   /* fase de congelar (aprox. 2 s) */
#define LOSE_FRAMES   75    /* flash ao perder */
#define STEP          0.5f  /* distancia de cada passo */
#define GOAL_Z        6.2f  /* distancia do guarda para vencer */
#define GUARD_Z       7.0f  /* posicao do guarda */

/* cores no formato 0x00RRGGBB */
#define C_SKY_WALK   0x6ec6ff
#define C_SKY_FREEZE 0xff6a5a
#define C_SKY_WIN    0xffcc33
#define C_SKY_LOSE   0x000000
#define C_GROUND     0x3a9a3a
#define C_PLAYER     0x2c6fd6
#define C_SKIN       0xf2c08a
#define C_GUARD      0x333333
#define C_GUARD_HEAD_BACK   0xbfbfbf
#define C_GUARD_HEAD_FACE   0xff3b3b
#define C_EYE        0x111111
#define C_POTATO     0xd9a441

/* ---------- sistema ---------- */

static volatile int running = 1;

static void sysutil_cb(u64 status, u64 param, void *userdata)
{
	(void)param;
	(void)userdata;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

/* ---------- buffers de render ---------- */

typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, inv; } Scr;

static u32   cbuf[RW * RH];   /* cor final de cada pixel interno */
static float zbuf[RW * RH];   /* 1/z de cada pixel (maior = mais perto) */

static float camX, camY, camZ;

/* ---------- utilidades ---------- */

static float fminf3(float a, float b, float c)
{
	float m = a;
	if (b < m) m = b;
	if (c < m) m = c;
	return m;
}

static float fmaxf3(float a, float b, float c)
{
	float m = a;
	if (b > m) m = b;
	if (c > m) m = c;
	return m;
}

static float clampf(float v, float lo, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/* escurece ou clareia uma cor. pct 100 = igual */
static u32 shade(u32 c, u32 pct)
{
	u32 r = ((c >> 16) & 0xff) * pct / 100;
	u32 g = ((c >> 8) & 0xff) * pct / 100;
	u32 b = (c & 0xff) * pct / 100;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return (r << 16) | (g << 8) | b;
}

/* ---------- projecao ---------- */

/* camera fixa, olhando para +Z. retorna 0 se o ponto esta perto demais */
static int project(Vec3 w, Scr *o)
{
	float vz = w.z - camZ;
	float k;
	if (vz < 0.2f)
		return 0;
	k = 1.0f / vz;
	o->x = RW * 0.5f + (w.x - camX) * k * FOCAL;
	o->y = RH * 0.5f - (w.y - camY) * k * FOCAL;
	o->inv = k;
	return 1;
}

/* ---------- rasterizacao ---------- */

static void fill_tri(Scr a, Scr b, Scr c, u32 color)
{
	float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
	float minx, maxx, miny, maxy;
	int x0, x1, y0, y1, x, y;

	if (den > -1e-6f && den < 1e-6f)
		return;

	minx = clampf(fminf3(a.x, b.x, c.x), 0, RW - 1);
	maxx = clampf(fmaxf3(a.x, b.x, c.x), 0, RW - 1);
	miny = clampf(fminf3(a.y, b.y, c.y), 0, RH - 1);
	maxy = clampf(fmaxf3(a.y, b.y, c.y), 0, RH - 1);

	x0 = (int)minx; x1 = (int)maxx;
	y0 = (int)miny; y1 = (int)maxy;

	for (y = y0; y <= y1; y++) {
		float py = y + 0.5f;
		for (x = x0; x <= x1; x++) {
			float px = x + 0.5f;
			float l0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / den;
			float l1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / den;
			float l2 = 1.0f - l0 - l1;
			float inv;
			int idx;

			if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f)
				continue;

			inv = l0 * a.inv + l1 * b.inv + l2 * c.inv;
			idx = y * RW + x;
			if (inv > zbuf[idx]) {
				zbuf[idx] = inv;
				cbuf[idx] = color;
			}
		}
	}
}

/* desenha uma caixa alinhada aos eixos, com sombreamento por face */
static void draw_box(Vec3 mn, Vec3 mx, u32 color)
{
	/* indice do canto: bit0 = x, bit1 = y, bit2 = z */
	static const int faces[6][4] = {
		{0, 1, 3, 2},  /* frente (z minimo) */
		{4, 5, 7, 6},  /* tras (z maximo) */
		{0, 4, 5, 1},  /* baixo */
		{2, 3, 7, 6},  /* topo */
		{0, 2, 6, 4},  /* esquerda */
		{1, 3, 7, 5}   /* direita */
	};
	static const u32 shadePct[6] = {100, 55, 60, 125, 80, 80};
	Vec3 w[8];
	Scr s[8];
	int i, f;

	for (i = 0; i < 8; i++) {
		w[i].x = (i & 1) ? mx.x : mn.x;
		w[i].y = (i & 2) ? mx.y : mn.y;
		w[i].z = (i & 4) ? mx.z : mn.z;
		if (!project(w[i], &s[i]))
			return; /* perto demais da camera: pula a caixa */
	}

	for (f = 0; f < 6; f++) {
		u32 c = shade(color, shadePct[f]);
		const int *q = faces[f];
		fill_tri(s[q[0]], s[q[1]], s[q[2]], c);
		fill_tri(s[q[0]], s[q[2]], s[q[3]], c);
	}
}

/* ---------- cena ---------- */

static void draw_scene(int facingPlayer, float playerZ, float bob, u32 skyColor)
{
	int i;

	/* limpa cor e profundidade */
	for (i = 0; i < RW * RH; i++) {
		cbuf[i] = skyColor;
		zbuf[i] = 0.0f;
	}

	/* camera atras do jogador, na altura dos olhos */
	camX = 0.0f;
	camY = 2.2f;
	camZ = playerZ - 6.0f;

	/* chao */
	{
		Vec3 mn = {-40.0f, -0.5f, -5.0f};
		Vec3 mx = {40.0f, 0.0f, 90.0f};
		draw_box(mn, mx, C_GROUND);
	}

	/* batatas espalhadas para dar profundidade */
	for (i = 0; i < 6; i++) {
		float z = 2.0f + i * 1.5f;
		Vec3 a1 = {-3.4f, 0.0f, z};
		Vec3 b1 = {-2.8f, 0.6f, z + 0.6f};
		Vec3 a2 = {2.8f, 0.0f, z};
		Vec3 b2 = {3.4f, 0.6f, z + 0.6f};
		draw_box(a1, b1, C_POTATO);
		draw_box(a2, b2, C_POTATO);
	}

	/* jogador */
	{
		Vec3 bmn = {-0.35f, 0.6f, playerZ - 0.2f};
		Vec3 bmx = {0.35f, 1.5f, playerZ + 0.2f};
		Vec3 hmn = {-0.28f, 1.5f + bob, playerZ - 0.22f};
		Vec3 hmx = {0.28f, 2.1f + bob, playerZ + 0.22f};
		draw_box(bmn, bmx, C_PLAYER);
		draw_box(hmn, hmx, C_SKIN);
	}

	/* guarda */
	{
		Vec3 bmn = {-0.6f, 0.0f, GUARD_Z - 0.4f};
		Vec3 bmx = {0.6f, 1.8f, GUARD_Z + 0.4f};
		Vec3 hmn = {-0.45f, 1.8f, GUARD_Z - 0.3f};
		Vec3 hmx = {0.45f, 2.8f, GUARD_Z + 0.3f};
		draw_box(bmn, bmx, C_GUARD);
		draw_box(hmn, hmx, facingPlayer ? C_GUARD_HEAD_FACE : C_GUARD_HEAD_BACK);

		if (facingPlayer) {
			/* olhos na frente do rosto (lado do jogador, z menor) */
			Vec3 e1a = {-0.35f, 2.2f, GUARD_Z - 0.5f};
			Vec3 e1b = {-0.10f, 2.5f, GUARD_Z - 0.3f};
			Vec3 e2a = {0.10f, 2.2f, GUARD_Z - 0.5f};
			Vec3 e2b = {0.35f, 2.5f, GUARD_Z - 0.3f};
			draw_box(e1a, e1b, C_EYE);
			draw_box(e2a, e2b, C_EYE);
		}
	}
}

/* ---------- video (RSX) ---------- */

static gcmContextData *context;
static void *host_addr;
static u32 *fb[2];
static u32 fb_offset[2];
static u32 fb_pitch;
static u32 fb_width, fb_height;
static int fb_index = 0;

static void gfx_init(void)
{
	videoState state;
	videoResolution res;
	videoConfiguration vconfig;
	u32 buf_size;
	int i;

	videoGetState(0, 0, &state);
	videoGetResolution(state.displayMode.resolutionId, &res);

	fb_width  = res.width;
	fb_height = res.height;
	fb_pitch  = res.width * sizeof(u32);

	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolutionId = state.displayMode.resolutionId;
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
		/* CORRIGIDO: rsxAddressToOffset devolve status e grava o offset no 2o argumento */
		rsxAddressToOffset(fb[i], &fb_offset[i]);
		gcmSetDisplayBuffer(i, fb_offset[i], fb_pitch, fb_width, fb_height);
	}
}

/* copia o buffer interno para o framebuffer, ampliado e centralizado */
static void present(void)
{
	u32 *dst = fb[fb_index];
	int s = (int)(fb_width / RW);
	int sy = (int)(fb_height / RH);
	int ox, oy, x, y;

	if (sy < s)
		s = sy;
	if (s < 1)
		s = 1;

	ox = ((int)fb_width - RW * s) / 2;
	oy = ((int)fb_height - RH * s) / 2;

	for (y = 0; y < (int)fb_height; y++) {
		int iy = (y - oy) / s;
		for (x = 0; x < (int)fb_width; x++) {
			int ix = (x - ox) / s;
			if (x < ox || y < oy || ix >= RW || iy >= RH || ix < 0 || iy < 0)
				dst[y * fb_width + x] = 0x00000000;
			else
				dst[y * fb_width + x] = cbuf[iy * RW + ix];
		}
	}
}

static void gfx_flip(void)
{
	present();

	gcmSetWaitFlip(context);
	gcmSetFlip(context, fb_index);
	rsxFlushBuffer(context);

	fb_index ^= 1;

	while (gcmGetFlipStatus() != 0)
		usleep(200);
	gcmResetFlipStatus();
}

/* ---------- controle ---------- */

static int cross_held;
static int cross_prev;

static void input_init(void)
{
	ioPadInit(7);
}

static void input_update(void)
{
	padData data;
	int now;

	ioPadGetData(0, &data);
	now = (data.len > 0) && (data.button[2] & 0x4000); /* X (CROSS) */

	cross_prev = cross_held;
	cross_held = now;
}

/* X acabou de ser apertado (borda de subida) */
static int cross_pressed(void)
{
	return cross_held && !cross_prev;
}

/* ---------- jogo ---------- */

typedef enum {
	PHASE_WALK,
	PHASE_FREEZE
} phase_t;

static phase_t phase;
static int phase_timer;
static float playerZ;
static int lose_timer;
static int won;
static int anim;

static void game_reset(void)
{
	phase = PHASE_WALK;
	phase_timer = WALK_FRAMES;
	playerZ = 0.0f;
	lose_timer = 0;
	won = 0;
}

static void game_update(void)
{
	anim++;

	if (won) {
		if (cross_pressed())
			game_reset();
		return;
	}

	if (lose_timer > 0) {
		lose_timer--;
		return;
	}

	if (phase == PHASE_WALK) {
		if (cross_pressed()) {
			playerZ += STEP;
			if (playerZ >= GOAL_Z) {
				won = 1;
				return;
			}
		}
	} else { /* PHASE_FREEZE: segurar X significa que voce se mexeu */
		if (cross_held) {
			playerZ = 0.0f;
			lose_timer = LOSE_FRAMES;
			phase = PHASE_WALK;
			phase_timer = WALK_FRAMES;
			return;
		}
	}

	phase_timer--;
	if (phase_timer <= 0) {
		if (phase == PHASE_WALK) {
			phase = PHASE_FREEZE;
			phase_timer = FREEZE_FRAMES;
		} else {
			phase = PHASE_WALK;
			phase_timer = WALK_FRAMES;
		}
	}
}

static void game_render(void)
{
	int facing = (phase == PHASE_FREEZE);
	float bob = 0.0f;
	u32 sky = C_SKY_WALK;

	if (won) {
		sky = C_SKY_WIN;
	} else if (lose_timer > 0) {
		sky = C_SKY_LOSE;
	} else if (facing) {
		sky = C_SKY_FREEZE;
	} else {
		/* balanco da cabeca enquanto anda */
		bob = fabsf(sinf(anim * 0.25f)) * 0.08f;
	}

	draw_scene(facing, playerZ, bob, sky);
}

/* ---------- main ---------- */

int main(void)
{
	/* CORRIGIDO: o nome certo e sysUtilRegisterCallback */
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);

	gfx_init();
	input_init();
	game_reset();

	while (running) {
		sysUtilCheckCallback();
		input_update();
		game_update();
		game_render();
		gfx_flip();
	}

	return 0;
}