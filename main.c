#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>
#include <ppu-types.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <rsx/rsx.h>
#include <io/pad.h>

static gcmContextData *ctx;
static u32 W=1280, H=720, *fb[2], fboff[2], *back;
static int cur=0;
static volatile int running=1;
static void cb(u64 s,u64 p,void*u){ if(s==SYSUTIL_EXIT_GAME) running=0; }

// --- DESENHO 2D BASE ---
static void rect(int x,int y,int w,int h,u32 c){
  int x0=x<0?0:x, y0=y<0?0:y, x1=x+w, y1=y+h;
  if(x1>(int)W)x1=W; if(y1>(int)H)y1=H;
  for(int yy=y0;yy<y1;yy++){ u32 *p=back+yy*W; for(int xx=x0;xx<x1;xx++) p[xx]=c; }
}
static void circle(int cx,int cy,int r,u32 c){
  for(int y=-r;y<=r;y++) for(int x=-r;x<=r;x++) if(x*x+y*y<=r*r) rect(cx+x,cy+y,1,1,c);
}
// --- FONTE ---
typedef struct{char c; const char *p;} Glyph;
static const Glyph glyphs[]={
 {'0',"###" "#.#" "#.#" "#.#" "###"},{'1',".#." "##." ".#." ".#." "###"},{'2',"###" "..#" "###" "#.." "###"},{'3',"###" "..#" "###" "..#" "###"},{'4',"#.#" "#.#" "###" "..#" "..#"},{'5',"###" "#.." "###" "..#" "###"},{'6',"###" "#.." "###" "#.#" "###"},{'7',"###" "..#" "..#" "..#" "..#"},{'8',"###" "#.#" "###" "#.#" "###"},{'9',"###" "#.#" "###" "..#" "###"},
 {'A',".#." "#.#" "###" "#.#" "#.#"},{'B',"##." "#.#" "##." "#.#" "##."},{'C',"###" "#.." "#.." "#.." "###"},{'D',"##." "#.#" "#.#" "#.#" "##."},{'E',"###" "#.." "###" "#.." "###"},{'F',"###" "#.." "###" "#.." "#.."},{'G',"###" "#.." "#.#" "#.#" "###"},{'H',"#.#" "#.#" "###" "#.#" "#.#"},{'I',"###" ".#." ".#." ".#." "###"},{'L',"#.." "#.." "#.." "#.." "###"},{'M',"#.#" "###" "###" "#.#" "#.#"},{'N',"#.#" "###" "###" "#.#" "#.#"},{'O',"###" "#.#" "#.#" "#.#" "###"},{'P',"###" "#.#" "###" "#.." "#.."},{'R',"##." "#.#" "##." "#.#" "#.#"},{'S',"###" "#.." "###" "..#" "###"},{'T',"###" ".#." ".#." ".#." ".#."},{'U',"#.#" "#.#" "#.#" "#.#" "###"},{'V',"#.#" "#.#" "#.#" "#.#" ".#."},{'X',"#.#" "#.#" ".#." "#.#" "#.#"},{'Y',"#.#" "#.#" ".#." ".#." ".#."},{'!',"###" ".#." ".#." "..." ".#."},{':',"..." ".#." "..." ".#." "..."},{'.',"..." "..." "..." "..." ".#."},{'-',"..." "..." "###" "..." "..."},{' ',"..." "..." "..." "..." "..."} };
static const char *find_g(char c){ for(unsigned i=0;i<sizeof(glyphs)/sizeof(glyphs[0]);i++) if(glyphs[i].c==c) return glyphs[i].p; return NULL; }
static void text(const char *s,int x,int y,int sc,u32 c){ for(;*s;s++){ const char *g=find_g(*s); if(g) for(int r=0;r<5;r++) for(int k=0;k<3;k++) if(g[r*3+k]=='#') rect(x+k*sc,y+r*sc,sc,sc,c); x+=4*sc; } }
static void textC(const char *s,int y,int sc,u32 c){ int w=strlen(s)*4*sc; text(s,(W-w)/2,y,sc,c); }

// --- 3D ---
static float camX=0, camZ=100, camY=3;
static int proj(float wx,float wy,float wz,int *sx,int *sy,float *scale){
  float dx=wx-camX, dy=wy-camY, dz=wz-camZ;
  if(dz>-1) dz=-1;
  float dist=-dz;
  *scale=500.0f/dist;
  *sx=(int)(640 + dx * (*scale));
  *sy=(int)(360 - dy * (*scale));
  return dist<200;
}
// Caixa 3D = jogador / boneca simplificada
static void drawBox3D(float x,float z,float w,float h,float d,u32 c){
  int sx,sy; float sc;
  if(!proj(x,0,z,&sx,&sy,&sc)) return;
  int bw=(int)(w*sc); int bh=(int)(h*sc);
  // sombra
  rect(sx-bw/2, sy, bw, 6, 0x00000000);
  // corpo
  rect(sx-bw/2, sy-bh, bw, bh, c);
  // cabeca
  int hw=bw*0.6f, hh=bh*0.25f;
  rect(sx-hw/2, sy-bh-hh, hw, hh, 0x00FFD0A0);
  // cabelo
  rect(sx-hw/2, sy-bh-hh, hw, hh/3, 0x00000000);
  // numero
  rect(sx-bw/4, sy-bh+ bh/3, bw/2, bh/4, 0x00FFFFFF);
}

int main(){
  void *host=memalign(1024*1024, 32*1024*1024);
  rsxInit(&ctx, 0x100000, 32*1024*1024, host);
  videoConfiguration vc; memset(&vc,0,sizeof(vc));
  vc.resolution=VIDEO_RESOLUTION_720; vc.format=VIDEO_BUFFER_FORMAT_XRGB; vc.aspect=VIDEO_ASPECT_AUTO; vc.pitch=1280*4;
  videoConfigure(VIDEO_PRIMARY,&vc,NULL,0);
  gcmSetFlipMode(GCM_FLIP_VSYNC);
  for(int i=0;i<2;i++){ fb[i]=(u32*)rsxMemalign(64,1280*720*4); gcmAddressToOffset(fb[i],&fboff[i]); gcmSetDisplayBuffer(i,fboff[i],1280*4,1280,720); }
  back=(u32*)memalign(64,1280*720*4);
  ioPadInit(7);
  sysUtilRegisterCallback(0,cb,NULL);

  int state=0; // 0 menu, 1 jogando, 2 morreu, 3 venceu
  float playerZ=100, playerX=0;
  float npcZ[6]={95,92,97,90,93,96}; float npcX[6]={-4,-2,2,4,-6,6}; int npcAlive[6]={1,1,1,1,1,1};
  int light=0, lightTimer=0, lightPhase=0, frame=0, shake=0, timeLeft=60*60;
  int dollTurn=0; // 0 de costas, 1 de frente

  while(running){
    sysUtilCheckCallback(); frame++;
    padInfo info; padData d; memset(&d,0,sizeof(d));
    ioPadGetInfo(&info); if(info.status[0]) ioPadGetData(0,&d);
    int ax=d.ANA_L_H-128, ay=d.ANA_L_V-128;
    int run=d.BTN_R2, start=d.BTN_START, cross=d.BTN_CROSS;

    if(state==0 && start){ state=1; playerZ=100; playerX=0; light=0; lightTimer=0; lightPhase=0; timeLeft=60*60; for(int i=0;i<6;i++){ npcAlive[i]=1; npcZ[i]=90+rand()%10; } }
    if((state==2||state==3) && cross){ state=0; }

    if(state==1){
      timeLeft--; if(timeLeft<=0) state=2;
      lightTimer++;
      if(lightPhase==0){ light=0; dollTurn=0; if(lightTimer>220){ lightPhase=1; lightTimer=0; } }
      else if(lightPhase==1){ if(lightTimer>45){ lightPhase=2; lightTimer=0; light=1; dollTurn=1; } }
      else { light=1; dollTurn=1; if(lightTimer>180){ lightPhase=0; lightTimer=0; } }

      float speed = run ? 0.35f : 0.15f;
      int moving=0;
      if(ay<-20){ playerZ-=speed; moving=1; }
      if(ay>20){ playerZ+=speed; moving=1; }
      if(ax>20){ playerX+=speed*0.6f; moving=1; }
      if(ax<-20){ playerX-=speed*0.6f; moving=1; }
      if(playerX<-8) playerX=-8; if(playerX>8) playerX=8;
      if(playerZ<0) playerZ=0; if(playerZ>100) playerZ=100;

      if(light==1 && moving){ state=2; shake=25; }
      for(int i=0;i<6;i++) if(npcAlive[i]){
        if(light==0){ npcZ[i]-=0.12f; if(npcZ[i]<2) npcZ[i]=2; }
        else { if(rand()%200<3) npcAlive[i]=0; }
      }
      if(playerZ<=2){ state=3; }
    }
    if(shake>0) shake--;
    int shx=0, shy=0; if(shake>0){ shx=rand()%10-5; shy=rand()%10-5; }

    camX=playerX; camZ=playerZ+6; camY=3.5f;

    // CEU
    for(int y=0;y<360;y++) for(int x=0;x<1280;x++) back[y*1280+x]=0x0087CEEB;
    // CHAO - grama 3D
    for(int y=360;y<720;y++) for(int x=0;x<1280;x++) back[y*1280+x]=0x0030A030;
    // LINHAS DO CHAO - da sensacao de 3D
    for(int i=0;i<20;i++){
      float z=playerZ - i*5;
      if(z<0) continue;
      int sx,sy; float sc; if(proj(0,0,z,&sx,&sy,&sc)) rect(0,sy+shy,1280,3,0x00FFFFFF);
    }
    // LINHA DE CHEGADA
    { int sx,sy; float sc; if(proj(0,0,0,&sx,&sy,&sc)) rect(0,sy+shy,1280,10,0x00FFFF00); }
    // LINHA DE SAIDA
    { int sx,sy; float sc; if(proj(0,0,100,&sx,&sy,&sc)) rect(0,sy+shy,1280,6,0x00FFFFFF); }

    // ARVORES 3D
    for(int i=0;i<6;i++){
      float tx=-12 + i*5; float tz=playerZ - 10 - i*8;
      int sx,sy; float sc; if(proj(tx,0,tz,&sx,&sy,&sc)){
        int tw=(int)(2*sc), th=(int)(8*sc);
        rect(sx-tw/2, sy-th, tw, th, 0x00804000);
        circle(sx, sy-th, (int)(3*sc), 0x00008000);
      }
    }

    // BONECA GIGANTE 3D - la na frente!
    {
      int sx,sy; float sc; if(proj(0,0,0,&sx,&sy,&sc)){
        int bw=(int)(6*sc), bh=(int)(14*sc);
        // vestido
        rect(sx-bw/2+shx, sy-bh+shy, bw, bh, 0x00FFA500);
        rect(sx-bw/2+shx, sy-bh+shy, bw, bh/5, 0x00FFFFFF);
        // cabeca gigante
        int hr=(int)(4*sc);
        circle(sx+shx, sy-bh-hr+shy, hr, 0x00FFD0A0);
        rect(sx-hr+shx, sy-bh-hr*2+shy, hr*2, hr/2, 0x00000000);
        if(dollTurn==1){
          // de frente - olho vermelho
          circle(sx-hr/2+shx, sy-bh-hr+shy, hr/4, 0x00FF0000);
          circle(sx+hr/2+shx, sy-bh-hr+shy, hr/4, 0x00FF0000);
        } else {
          // de costas - cabelo
          rect(sx-hr+shx, sy-bh-hr*2+shy, hr*2, hr*2, 0x00000000);
        }
      }
    }

    // NPCs 3D
    for(int i=0;i<6;i++){
      if(npcAlive[i]) drawBox3D(npcX[i], npcZ[i], 1.2f, 3.0f, 1.0f, 0x00108040);
      else {
        int sx,sy; float sc; if(proj(npcX[i],0,npcZ[i],&sx,&sy,&sc)) rect(sx-20,sy,40,8,0x00FF0000);
      }
    }
    // JOGADOR 3D - voce!
    drawBox3D(playerX, playerZ, 1.4f, 3.2f, 1.0f, 0x000000FF);
    // Seta em cima de voce
    { int sx,sy; float sc; if(proj(playerX,0,playerZ,&sx,&sy,&sc)) { rect(sx-15+shx, sy-180+shy, 30, 12, 0x00FFFF00); text("VOCE", sx-25+shx, sy-210+shy, 3, 0x00FFFF00); } }

    // HUD
    if(state==1){
      if(light==0){ circle(640,50,20,0x0000FF00); textC("LUZ VERDE - CORRE!", 90, 6, 0x0000FF00); }
      else { circle(640,50,20,0x00FF0000); textC("LUZ VERMELHA - PARA!", 90, 6, 0x00FF0000); }
      char buf[64]; snprintf(buf,sizeof(buf),"TEMPO %d",timeLeft/60); text(buf,20,20,5,0x00FFFFFF);
      text("L3 ANDAR  R2 CORRER",20,680,4,0x00FFFFFF);
    }
    if(state==0){ rect(200,180,880,360,0x00000000); rect(210,190,860,340,0x00FF1493); textC("ROUND 6",230,12,0x00FFFFFF); textC("BATATINHA FRITA 3D",320,8,0x00FFFF00); textC("L3 ANDAR  R2 CORRER",400,6,0x00FFFFFF); textC("VERDE CORRE  VERMELHO PARA",440,6,0x00FF0000); textC("APERTE START",500,7,0x00000000); }
    if(state==2){ rect(250,200,780,300,0x00000000); textC("ELIMINADO!",250,12,0x00FF0000); textC("VOCE SE MEXEU NO VERMELHO!",350,6,0x00FFFFFF); textC("APERTE X P/ TENTAR DE NOVO",420,6,0x00FFFF00); }
    if(state==3){ rect(250,200,780,300,0x00000000); textC("VOCE VENCEU!",250,12,0x0000FF00); textC("45 BILHOES DE WON!",350,7,0x00FFFF00); textC("APERTE X P/ JOGAR DE NOVO",430,6,0x00FFFFFF); }

    memcpy(fb[cur],back,1280*720*4);
    gcmSetFlip(ctx,cur); rsxFlushBuffer(ctx); gcmSetWaitFlip(ctx); cur^=1;
    usleep(16000);
  }
  return 0;
}