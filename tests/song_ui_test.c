/* SPDX-License-Identifier: GPL-3.0-only */
/* Real song screen renderer and interaction code, with a framebuffer and
 * panel/flash doubles. No device access. Writes a 240x240 PPM preview. */
#define FELUCCA_ARRANGER 1
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>
static uint16_t screen[240*240];
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i,j;
    assert(x+w<=240 && y+h<=240);
    for(j=0;j<h;j++) for(i=0;i<w;i++) screen[(y+j)*240+x+i]=p[j*w+i];
}
#include "../firmware/src/gfx.c"
enum {B_FX,B_SCL,B_ENV,B_LFO,B_EDIT,B_GLO,B_HOME,B_SAVE,B_ARP,B_SEQ,B_PLAY,B_REC,B_OCTDN,B_OCTUP,NB};
enum {EN_SELECT,EN_ALGO,EN_PRESET,EN_K1,EN_K2,EN_K3,EN_K4,NE};
static struct {uint8_t btn[NB];} panel;
static struct {uint8_t home,page,force,msg_t;char msg[24];} ui;
static int32_t enc[NE];
static uint32_t ready, scene_saves, order_saves, loads;
static uint32_t arrangement_ready(void) {return ready;}
static void arrangement_apply(uint32_t ei) {(void)ei;}
static void arrangement_apply_scene(uint32_t scene) {(void)scene;}
static const page_t *cur_page(void) {return &PAGES[ui.page];}
static int project_used(uint32_t i) {return (ready>>i)&1u;}
static void project_save(uint32_t i) {ready|=1u<<i;scene_saves++;}
static void project_load(uint32_t i) {(void)i;loads++;}
static void settings_save(void) {}
static void arrangement_save(void) {order_saves++;}
static void ui_message(const char *s) {str_cpy(ui.msg,s,sizeof ui.msg);ui.msg_t=40;}
static void go_home(void) {ui.home=1;}
static void open_family(uint32_t f) {(void)f;ui.home=1;}
static int32_t panel_enc(uint32_t i) {int32_t s=enc[i];enc[i]=0;return s;}
static void song_backup(void) {}
static void song_restore(void) {}
#include "../firmware/src/ui_song.c"
static void press(uint32_t b) {song_screen_input(1u<<panel.btn[b],0);}

int main(int argc,char **argv)
{
    uint32_t i, k;
    arr_defaults(&arrangement);
    for(i=0;i<NB;i++) panel.btn[i]=(uint8_t)i;
    for(i=0;i<NPAGES;i++) if(PAGES[i].scope==SC_SONG) ui.page=(uint8_t)i;
    assert(on_song_page());
    press(B_SAVE); assert(order_saves==1 && !scene_saves);
    press(B_REC); assert(scene_saves==1 && ready==1);
    press(B_REC); assert(scene_saves==1);           /* replacement needs confirmation */
    press(B_REC); assert(scene_saves==2);
    press(B_REC); fm1_ms+=3001; press(B_REC); assert(scene_saves==2);
    enc[EN_K2]=1;song_screen_input(0,0);             /* navigating sub-field cancels confirmation */
    assert(song_sub==1);                             /* sub moved to track 1 field */
    enc[EN_K3]=1;song_screen_input(0,0);             /* change pattern for track 1: A -> B */
    assert(arrangement.entry[0].track[0]==1);
    press(B_REC);assert(scene_saves==3 && ready==3);
    song.playing=1;
    press(B_REC);press(B_SAVE);press(B_OCTUP);
    enc[EN_K3]=20;song_screen_input(0,0);
    assert(scene_saves==3 && order_saves==1 && !loads && arrangement.entry[0].bars==4);
    song.playing=0;transport_req=0;
    press(B_OCTDN);assert(arrangement_enabled);
    press(B_PLAY);assert(!transport_req);           /* later scenes still empty */
    ready=15;press(B_PLAY);assert(transport_req==1);
    transport_req=0;press(B_OCTUP);assert(loads==0);   /* load: a second press confirms */
    press(B_OCTUP);assert(loads==1);
    enc[EN_K4]=99;song_screen_input(0,0);assert(arrangement.count==16);
    enc[EN_K1]=99;song_screen_input(0,0);assert(song_cursor==15);
    enc[EN_K4]=-99;song_screen_input(0,0);assert(arrangement.count==1 && song_cursor==0);

    /* Per-track: ENC1 navigates fields, ENC2 (EN_K3) edits pattern or bars */
    {
        uint32_t sv;
        arr_defaults(&arrangement); ready=15; ui.msg_t=0; song_cursor=0; song_sub=0;
        song.playing=0; transport_req=0;
        /* ENC2 (EN_K3) on bars row (sub=0) changes bars */
        enc[EN_K3]=3; song_screen_input(0,0);
        assert(arrangement.entry[0].bars==7);          /* default 4 + 3 */
        /* ENC1 (EN_K2) navigates to track 1 sub-field */
        enc[EN_K2]=1; song_screen_input(0,0);
        assert(song_sub==1);
        /* ENC2 changes pattern for track 1: A(0) -> B(1) */
        enc[EN_K3]=1; song_screen_input(0,0);
        assert(arrangement.entry[0].track[0]==1);
        /* ENC2 large positive: clamps at MUTE */
        enc[EN_K3]=99; song_screen_input(0,0);
        assert(arrangement.entry[0].track[0]==ARR_MUTE);
        /* ENC2 large negative: clamps at A(0) */
        enc[EN_K3]=-99; song_screen_input(0,0);
        assert(arrangement.entry[0].track[0]==0);
        /* Navigate to track 2 */
        enc[EN_K2]=1; song_screen_input(0,0);
        assert(song_sub==2);
        enc[EN_K3]=2; song_screen_input(0,0);          /* track 2: A -> C */
        assert(arrangement.entry[0].track[1]==2);
        /* REC on a muted track shows an error and does not save */
        enc[EN_K2]=-1; song_screen_input(0,0);         /* back to track 1 sub */
        assert(song_sub==1);
        enc[EN_K3]=99; song_screen_input(0,0);          /* set track 1 to MUTE */
        assert(arrangement.entry[0].track[0]==ARR_MUTE);
        sv=scene_saves; ui.msg_t=0;
        press(B_REC); assert(ui.msg_t>0 && scene_saves==sv);
        /* ENC1 clamps at 0 (bars row) when going below */
        enc[EN_K2]=-99; song_screen_input(0,0);
        assert(song_sub==0);
        /* ENC1 clamps at ARR_TRACKS (=4) when going above */
        enc[EN_K2]=99; song_screen_input(0,0);
        assert(song_sub==ARR_TRACKS);
    }
    puts("song UI: per-track pattern editing, mute, field navigation PASS");

    arr_defaults(&arrangement);ready=15;ui.msg_t=0;ui.force=1;
    for(k=0;k<ARR_TRACKS;k++) arrangement.entry[0].track[k]=0; arrangement.entry[0].bars=4;
    for(k=0;k<ARR_TRACKS;k++) arrangement.entry[1].track[k]=1; arrangement.entry[1].bars=8;
    for(k=0;k<ARR_TRACKS;k++) arrangement.entry[2].track[k]=2; arrangement.entry[2].bars=8;
    for(k=0;k<ARR_TRACKS;k++) arrangement.entry[3].track[k]=3; arrangement.entry[3].bars=4;
    palette_set(2);song_screen_draw();
    if(argc>1) {
        FILE *f=fopen(argv[1],"wb");assert(f);
        fprintf(f,"P6\n240 240\n255\n");
        for(i=0;i<240*240;i++) {
            uint16_t p=swap16(screen[i]);
            uint8_t rgb[3]={(uint8_t)((p>>11)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
            fwrite(rgb,1,3,f);
        }
        fclose(f);
    }
    puts("song UI: independent plan/scene save, overwrite confirmation, stopped-only changes, missing scene, bounds PASS");
    return 0;
}
