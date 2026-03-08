#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#define COMPUTER_BAD_MOVE_CHANCE 5

#define WIN_W   1024
#define WIN_H    768
#define CARD_W    71
#define CARD_H   101
#define CARD_RAD   6

#define SUIT_HEARTS   0
#define SUIT_DIAMONDS 1
#define SUIT_CLUBS    2
#define SUIT_SPADES   3
#define RANK_JOKER   14
#define NUM_SUITS     4
#define DECK_SIZE    54

static const char *RANK_STR[] = {
    "","A","2","3","4","5","6","7","8","9","10","J","Q","K","A","JK"
};
/* proper UTF-8 suit symbols */
static const char *SUIT_STR[] = { "\xe2\x99\xa5", "\xe2\x99\xa6", "\xe2\x99\xa3", "\xe2\x99\xa0" };

typedef struct { int rank; int suit; } Card;

#define MAX_HAND 54
typedef struct { Card cards[MAX_HAND]; int n; } Hand;

typedef enum {
    GS_PLAYER_TURN,
    GS_COMPUTER_TURN,
    GS_ACE_DIALOG,
    GS_SKIP_HUMAN,
    GS_GAME_OVER
} GameState;

static Card      deck[DECK_SIZE];
static int       deck_top;
static Card      discard[DECK_SIZE];
static int       discard_top;
static Hand      player_hand, computer_hand;
static int       requested_suit;
static GameState state;
static int       show_computer;
static char      msg[256];
static int       pending_skip;   /* rounds to skip for next player */
static int       pending_draw;    /* cards forced to draw for next player */
static int       player_scroll  = 0;  /* index of leftmost visible card */
static int       computer_scroll= 0;

/* macao state */
static int       player_said_macao  = 0; /* 1 = player already said it */
static int       computer_said_macao= 0; /* 1 = computer said it */
static time_t    macao_deadline     = 0; /* unix time by which player must say macao */
static int       macao_window_open  = 0; /* 1 = 5s window is running */
static int       show_call_macao_btn= 0; /* 1 = show "Call Macao!" button on computer */
static int       show_hints         = 0; /* 1 = highlight playable cards */

/* drag state */
static int  drag_active    = 0;
static int  drag_card_idx  = -1; /* index in player_hand */
static int  drag_x         = 0;  /* current mouse pos */
static int  drag_y         = 0;
static int  drag_ox        = 0;  /* offset from card top-left when grabbed */
static int  drag_oy        = 0;

/* X11 */
static Display    *dpy;
static Window      win;
static GC          gc;
static int         scr;
static Colormap    cmap;
static Visual     *visual;
static Pixmap      back_buf;
static XftDraw    *xft_back;

/* Xft */
static XftDraw    *xft_draw;
static XftFont    *font_sm, *font_md, *font_lg;

/* colours as XRenderColor (for Xft) and unsigned long (for Xlib fills) */
typedef struct { unsigned long xlib; XftColor xft; } Color;

static Color COL_BG, COL_FELT, COL_CARD_FACE, COL_CARD_BACK,
             COL_RED, COL_BLACK, COL_GOLD, COL_GREEN_HI,
             COL_BLUE_HI, COL_GRAY, COL_WHITE, COL_SHADOW,
             COL_BTN, COL_BTN_HI;

static Color mkcolor(int r, int g, int b)
{
    Color c;
    XColor xc;
    xc.red   = (unsigned short)(r << 8);
    xc.green = (unsigned short)(g << 8);
    xc.blue  = (unsigned short)(b << 8);
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cmap, &xc);
    c.xlib = xc.pixel;

    XRenderColor rc;
    rc.red   = (unsigned short)(r * 257);
    rc.green = (unsigned short)(g * 257);
    rc.blue  = (unsigned short)(b * 257);
    rc.alpha = 0xffff;
    XftColorAllocValue(dpy, visual, cmap, &rc, &c.xft);
    return c;
}

static Color mkcolor_lazy(int r, int g, int b)
{
    /* for one-off colours inside render – only xlib part needed */
    Color c;
    XColor xc;
    xc.red   = (unsigned short)(r << 8);
    xc.green = (unsigned short)(g << 8);
    xc.blue  = (unsigned short)(b << 8);
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cmap, &xc);
    c.xlib = xc.pixel;
    XRenderColor rc;
    rc.red=(unsigned short)(r*257); rc.green=(unsigned short)(g*257);
    rc.blue=(unsigned short)(b*257); rc.alpha=0xffff;
    XftColorAllocValue(dpy,visual,cmap,&rc,&c.xft);
    return c;
}

static void init_colors(void)
{
    COL_BG        = mkcolor(34,  85,  34);
    COL_FELT      = mkcolor(20,  100, 20);
    COL_CARD_FACE = mkcolor(255, 252, 240);
    COL_CARD_BACK = mkcolor(30,  60,  160);
    COL_RED       = mkcolor(210, 30,  30);
    COL_BLACK     = mkcolor(15,  15,  15);
    COL_GOLD      = mkcolor(220, 180, 40);
    COL_GREEN_HI  = mkcolor(80,  200, 80);
    COL_BLUE_HI   = mkcolor(80,  130, 230);
    COL_GRAY      = mkcolor(130, 130, 130);
    COL_WHITE     = mkcolor(255, 255, 255);
    COL_SHADOW    = mkcolor(0,   0,   0);
    COL_BTN       = mkcolor(50,  50,  120);
    COL_BTN_HI    = mkcolor(80,  80,  200);
}

static XftFont *load_xft_font(int size, int bold)
{
    char pat[64];
    snprintf(pat, sizeof pat, "DejaVu Sans%s:size=%d:charset=2665", bold ? " Bold" : "", size);
    XftFont *f = XftFontOpenName(dpy, scr, pat);
    if (!f) { snprintf(pat, sizeof pat, "FreeSans:size=%d", size); f = XftFontOpenName(dpy, scr, pat); }
    if (!f) f = XftFontOpenName(dpy, scr, "fixed");
    return f;
}

/* ── draw helpers ────────────────────────────────────────────────────── */
static void fill_rect(int x, int y, int w, int h, unsigned long c)
{
    XSetForeground(dpy, gc, c);
    XFillRectangle(dpy, back_buf, gc, x, y, (unsigned)w, (unsigned)h);
}

static void fill_rounded(int x, int y, int w, int h, int r, unsigned long c)
{
    XSetForeground(dpy, gc, c);
    XFillRectangle(dpy, back_buf, gc, x+r, y,   (unsigned)(w-2*r), (unsigned)h);
    XFillRectangle(dpy, back_buf, gc, x,   y+r, (unsigned)w,       (unsigned)(h-2*r));
    XFillArc(dpy, back_buf, gc, x,       y,       2*r, 2*r, 90*64,  90*64);
    XFillArc(dpy, back_buf, gc, x+w-2*r, y,       2*r, 2*r, 0,      90*64);
    XFillArc(dpy, back_buf, gc, x,       y+h-2*r, 2*r, 2*r, 180*64, 90*64);
    XFillArc(dpy, back_buf, gc, x+w-2*r, y+h-2*r, 2*r, 2*r, 270*64, 90*64);
}

/* UTF-8 string via Xft */
static void draw_str(int x, int y, const char *s, Color *c, XftFont *f)
{
    XftDrawStringUtf8(xft_back, &c->xft, f, x, y, (const FcChar8 *)s, (int)strlen(s));
}

static int text_width(XftFont *f, const char *s)
{
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, (int)strlen(s), &ext);
    return ext.xOff;
}

/* ── deck/game logic (unchanged from original) ───────────────────────── */
static int is_red_suit(int s){ return s==SUIT_HEARTS||s==SUIT_DIAMONDS; }

static void build_deck(void)
{
    int i=0;
    /* 2..13 (2 through King) for all four suits */
    for(int s=0;s<NUM_SUITS;s++)
        for(int r=2;r<=13;r++){
            deck[i].rank=r; deck[i].suit=s; i++;
        }
    /* Aces as rank=1 */
    for(int s=0;s<NUM_SUITS;s++){
        deck[i].rank=1; deck[i].suit=s; i++;
    }
    /* two jokers */
    deck[i].rank=RANK_JOKER; deck[i].suit=SUIT_HEARTS; i++;
    deck[i].rank=RANK_JOKER; deck[i].suit=SUIT_SPADES; i++;
}

static void shuffle(Card *arr, int n)
{
    for(int i=n-1;i>0;i--){int j=rand()%(i+1);Card t=arr[i];arr[i]=arr[j];arr[j]=t;}
}

static void reset_deck_from_discard(void)
{
    if(discard_top <= 1) return;

    Card last_played = discard[discard_top - 1];

    /* all cards except the last played one become the new draw deck */
    deck_top = discard_top - 1;
    for(int i = 0; i < deck_top; i++)
        deck[i] = discard[i];
    shuffle(deck, deck_top);

    /* discard restarted with just the one card that was on top */
    discard[0]  = last_played;
    discard_top = 1;

    snprintf(msg, sizeof msg, "Deck reshuffled: %d cards back in play.", deck_top);
}

static int cards_available(void)
{
    return deck_top + (discard_top > 1 ? discard_top - 1 : 0);
}

static Card draw_card(void)
{
    if(deck_top==0) reset_deck_from_discard();
    if(deck_top==0){
        /* truly no cards anywhere — return a placeholder, caller must check */
        Card empty = {0, 0};
        return empty;
    }
    return deck[--deck_top];
}

static void deal(void)
{
    player_hand.n=0; computer_hand.n=0;
    for(int i=0;i<5;i++){
        { Card _c=draw_card(); if(_c.rank||_c.suit) player_hand.cards[player_hand.n++]=_c; }
        if(computer_hand.n<MAX_HAND) computer_hand.cards[computer_hand.n++]=draw_card();
    }
    Card c;
    do{ c=draw_card(); }while(c.rank==RANK_JOKER||c.rank==1||c.rank==2||c.rank==3||c.rank==4||c.rank==7);
    discard[0]=c; discard_top=1;
}

static Card top_discard(void){ return discard[discard_top-1]; }

static int can_play(Card c, Card top, int req)
{
    /* 7 can always counter a pending effect */
    if(c.rank==7 && (pending_skip>0 || pending_draw>0)) return 1;

    /* when there is a pending draw, handle 2s, 3s, and Jokers */
    if(pending_draw>0){
        if(c.rank==7) return 1;

        /* If top is a 2 or 3 */
        if(top.rank==2 || top.rank==3) {
            /* 2s and 3s can stack on each other regardless of color */
            if (c.rank == 2 || c.rank == 3) return 1;
            /* A Joker stacked on a 2 or 3 must match its color */
            if (c.rank == RANK_JOKER) return is_red_suit(c.suit) == is_red_suit(top.suit);
            return 0;
        }

        /* If top is a Joker */
        if(top.rank==RANK_JOKER) {
            /* Any Joker can be stacked on any other Joker (e.g., Black on Red) */
            if (c.rank == RANK_JOKER) return 1;
            /* 2s and 3s stacked on a Joker must match the Joker's color */
            if (c.rank == 2 || c.rank == 3) return is_red_suit(c.suit) == is_red_suit(top.suit);
            return 0;
        }
        return 0;
    }
    /* when there is a pending skip, only 4 or 7 allowed */
    if(pending_skip>0){
        return c.rank==4||c.rank==7;
    }
    if(c.rank==RANK_JOKER){
        if(req>=0) return is_red_suit(c.suit)==is_red_suit(req);
        if(top.rank==RANK_JOKER) return 1; /* Allow any Joker on any Joker */
        return is_red_suit(c.suit)==is_red_suit(top.suit);
    }
    if(top.rank==RANK_JOKER){
        if(req>=0) return c.suit==req;
        /* (Joker-on-Joker is already caught by the block above) */
        return is_red_suit(c.suit)==is_red_suit(top.suit);
    }
    if(req>=0) return c.suit==req||c.rank==top.rank;
    return c.suit==top.suit||c.rank==top.rank;
}

static void remove_from_hand(Hand *h, int idx)
{
    for(int i=idx;i<h->n-1;i++) h->cards[i]=h->cards[i+1];
    h->n--;
}

static void play_card_from(Hand *h, int idx, int is_player)
{
    Card c=h->cards[idx];
    discard[discard_top++]=c;
    remove_from_hand(h,idx);
    requested_suit=-1;
    const char *who=is_player?"You":"Computer";
    const char *opp=is_player?"Computer":"You";
    if(c.rank==RANK_JOKER){
        int draw_n = is_red_suit(c.suit)?10:5;
        pending_draw += draw_n;
        snprintf(msg,sizeof msg,"%s played %s Joker! %s must draw %d total.",
                 who,is_red_suit(c.suit)?"Red":"Black",opp,pending_draw);
    } else if(c.rank==2){
        pending_draw += 2;
        snprintf(msg,sizeof msg,"%s played 2! %s must draw %d total.",who,opp,pending_draw);
    } else if(c.rank==3){
        pending_draw += 3;
        snprintf(msg,sizeof msg,"%s played 3! %s must draw %d total.",who,opp,pending_draw);
    } else if(c.rank==4){
        pending_skip += 1;
        snprintf(msg,sizeof msg,"%s played 4! %s skips %d turn(s).",who,opp,pending_skip);
    } else if(c.rank==7){
        if(pending_draw>0)
            snprintf(msg,sizeof msg,"%s played 7 - draw obligation cancelled!",who);
        else if(pending_skip>0)
            snprintf(msg,sizeof msg,"%s played 7 - skip cancelled!",who);
        else
            snprintf(msg,sizeof msg,"%s played 7.",who);
        pending_draw=0;
        pending_skip=0;
    } else if(c.rank==1){
        if(is_player){ state=GS_ACE_DIALOG; snprintf(msg,sizeof msg,"You played an Ace - choose a suit!"); }
        else{
            int cnt[4]={0};
            for(int i=0;i<h->n;i++) if(h->cards[i].rank!=RANK_JOKER) cnt[h->cards[i].suit]++;
            int best=0;
            for(int s=1;s<4;s++) if(cnt[s]>cnt[best]) best=s;
            requested_suit=best;
            snprintf(msg,sizeof msg,"Computer played Ace - requests %s!",SUIT_STR[best]);
        }
    } else {
        snprintf(msg,sizeof msg,"%s played %s%s",who,RANK_STR[c.rank],SUIT_STR[c.suit]);
    }
}

/* ── card drawing ────────────────────────────────────────────────────── */
static void draw_card_back(int x, int y)
{
    fill_rounded(x+3,y+3,CARD_W,CARD_H,CARD_RAD,COL_SHADOW.xlib);
    fill_rounded(x,y,CARD_W,CARD_H,CARD_RAD,COL_CARD_BACK.xlib);
    XSetForeground(dpy,gc,mkcolor_lazy(50,80,190).xlib);
    for(int i=0;i<CARD_W;i+=8) XDrawLine(dpy,back_buf,gc,x+i,y,x,y+i);
    for(int i=0;i<CARD_H;i+=8) XDrawLine(dpy,back_buf,gc,x+CARD_W,y+i,x+CARD_W-i,y+CARD_H);
    XSetForeground(dpy,gc,COL_WHITE.xlib);
    fill_rounded(x+4,y+4,CARD_W-8,CARD_H-8,4,mkcolor_lazy(40,70,180).xlib);
    XSetForeground(dpy,gc,COL_WHITE.xlib);
    XDrawRectangle(dpy,back_buf,gc,x,y,CARD_W,CARD_H);
}

static void draw_card_face(int x, int y, Card c, int highlighted)
{
    fill_rounded(x+3,y+3,CARD_W,CARD_H,CARD_RAD,COL_SHADOW.xlib);
    fill_rounded(x,y,CARD_W,CARD_H,CARD_RAD,COL_CARD_FACE.xlib);

    int red = (c.suit==SUIT_HEARTS||c.suit==SUIT_DIAMONDS||
               (c.rank==RANK_JOKER&&is_red_suit(c.suit)));
    Color *suit_col = red ? &COL_RED : &COL_BLACK;

    if(c.rank==RANK_JOKER){
        int red_joker = is_red_suit(c.suit);
        unsigned long c1 = red_joker ? mkcolor_lazy(210,30,30).xlib   : mkcolor_lazy(30,30,30).xlib;
        unsigned long c2 = red_joker ? mkcolor_lazy(255,210,210).xlib : mkcolor_lazy(200,200,200).xlib;
        fill_rounded(x+4,y+4,CARD_W-8,CARD_H-8,4,c2);
        XPoint tri[3] = {{(short)(x+4),(short)(y+4)},
                         {(short)(x+CARD_W-4),(short)(y+4)},
                         {(short)(x+4),(short)(y+CARD_H-4)}};
        XSetForeground(dpy,gc,c1);
        XFillPolygon(dpy,back_buf,gc,tri,3,Nonconvex,CoordModeOrigin);
        Color col_dark  = red_joker ? mkcolor_lazy(130,0,0)    : mkcolor_lazy(20,20,20);
        //Color col_light = mkcolor_lazy(255,255,255);
        int cx2 = x + CARD_W/2;
        /* hat peaks — compressed into top 30px */
        int hb = y+30; /* hat brim y */
        XPoint lp[3] = {{(short)(cx2-16),(short)(hb)},
                        {(short)(cx2-24),(short)(y+10)},
                        {(short)(cx2-5),(short)(hb)}};
        XSetForeground(dpy,gc,c1);
        XFillPolygon(dpy,back_buf,gc,lp,3,Nonconvex,CoordModeOrigin);
        XPoint mp[3] = {{(short)(cx2-8),(short)(hb)},
                        {(short)(cx2),  (short)(y+4)},
                        {(short)(cx2+8),(short)(hb)}};
        XSetForeground(dpy,gc,c2);
        XFillPolygon(dpy,back_buf,gc,mp,3,Nonconvex,CoordModeOrigin);
        XPoint rp[3] = {{(short)(cx2+5),(short)(hb)},
                        {(short)(cx2+24),(short)(y+10)},
                        {(short)(cx2+16),(short)(hb)}};
        XSetForeground(dpy,gc,c1);
        XFillPolygon(dpy,back_buf,gc,rp,3,Nonconvex,CoordModeOrigin);
        /* hat brim */
        XSetForeground(dpy,gc,col_dark.xlib);
        XFillRectangle(dpy,back_buf,gc,(unsigned)(cx2-18),(unsigned)(hb),36,4);
        /* bells */
        XSetForeground(dpy,gc,mkcolor_lazy(220,180,0).xlib);
        XFillArc(dpy,back_buf,gc,cx2-27,y+6, 8,8,0,360*64);
        XFillArc(dpy,back_buf,gc,cx2-4, y+1, 8,8,0,360*64);
        XFillArc(dpy,back_buf,gc,cx2+19,y+6, 8,8,0,360*64);
        XSetForeground(dpy,gc,col_dark.xlib);
        XFillArc(dpy,back_buf,gc,cx2-25,y+9, 3,3,0,360*64);
        XFillArc(dpy,back_buf,gc,cx2-1, y+4, 3,3,0,360*64);
        XFillArc(dpy,back_buf,gc,cx2+21,y+9, 3,3,0,360*64);
        /* face */
        int fy = hb+4;
        XSetForeground(dpy,gc,mkcolor_lazy(255,220,170).xlib);
        XFillArc(dpy,back_buf,gc,cx2-11,fy,22,20,0,360*64);
        XSetForeground(dpy,gc,col_dark.xlib);
        XFillArc(dpy,back_buf,gc,cx2-8, fy+5, 4,4,0,360*64);
        XFillArc(dpy,back_buf,gc,cx2+4, fy+5, 4,4,0,360*64);
        XDrawArc(dpy,back_buf,gc,cx2-5, fy+11,10,6,200*64,140*64);
        /* collar */
        int col_y = fy+22;
        for(int ci=0;ci<5;ci++){
            XSetForeground(dpy,gc,ci%2==0?c1:c2);
            XFillArc(dpy,back_buf,gc,cx2-12+ci*6,col_y,8,6,0,360*64);
        }
        /* body */
        int body_y = col_y+8;
        XSetForeground(dpy,gc,c1);
        XFillRectangle(dpy,back_buf,gc,(unsigned)(cx2-12),(unsigned)(body_y),12,20);
        XSetForeground(dpy,gc,c2);
        XFillRectangle(dpy,back_buf,gc,(unsigned)(cx2),(unsigned)(body_y),12,20);
        XPoint dia[4]={{(short)(cx2),     (short)(body_y+3)},
                       {(short)(cx2+7),   (short)(body_y+10)},
                       {(short)(cx2),     (short)(body_y+17)},
                       {(short)(cx2-7),   (short)(body_y+10)}};
        XSetForeground(dpy,gc,mkcolor_lazy(220,180,0).xlib);
        XFillPolygon(dpy,back_buf,gc,dia,4,Nonconvex,CoordModeOrigin);
        /* legs */
        int leg_y = body_y+20;
        XSetForeground(dpy,gc,c1);
        XFillRectangle(dpy,back_buf,gc,(unsigned)(cx2-11),(unsigned)(leg_y),9,12);
        XSetForeground(dpy,gc,c2);
        XFillRectangle(dpy,back_buf,gc,(unsigned)(cx2+2),(unsigned)(leg_y),9,12);
        /* shoes — keep within card */
        XSetForeground(dpy,gc,col_dark.xlib);
        XFillArc(dpy,back_buf,gc,cx2-14,leg_y+10,12,6,0,360*64);
        XFillArc(dpy,back_buf,gc,cx2+2, leg_y+10,12,6,0,360*64);
        /* corner labels */
        //draw_str(x+3,y+13,"J",&col_light,font_md);
        //draw_str(x+3,y+24,red_joker?"\xe2\x99\xa5":"\xe2\x99\xa0",&col_light,font_sm);
        //draw_str(x+CARD_W-12,y+CARD_H-4,"J",&col_light,font_sm);
    } else {
        const char *rs=RANK_STR[c.rank];
        const char *ss=SUIT_STR[c.suit];
        draw_str(x+4,          y+14,        rs, suit_col, font_md);
        draw_str(x+4,          y+26,        ss, suit_col, font_sm);
        int csx=x+CARD_W/2-6, csy=y+CARD_H/2+8;
        draw_str(csx, csy, ss, suit_col, font_lg);
        draw_str(x+CARD_W-18,  y+CARD_H-4,  rs, suit_col, font_sm);
    }

    unsigned long border = highlighted ? COL_GOLD.xlib : COL_GRAY.xlib;
    XSetForeground(dpy,gc,border);
    for(int i=0;i<(highlighted?2:1);i++)
        XDrawRectangle(dpy,back_buf,gc,x+i,y+i,CARD_W-2*i,CARD_H-2*i);
}

/* ── button ──────────────────────────────────────────────────────────── */
typedef struct { int x,y,w,h; const char *label; } Button;

static void draw_button(Button b, int hover)
{
    fill_rounded(b.x,b.y,b.w,b.h,6,(hover?COL_BTN_HI:COL_BTN).xlib);
    int tw=text_width(font_sm,b.label);
    draw_str(b.x+(b.w-tw)/2, b.y+b.h/2+5, b.label, &COL_WHITE, font_sm);
}

static int in_button(Button b, int mx, int my)
{
    return mx>=b.x&&mx<=b.x+b.w&&my>=b.y&&my<=b.y+b.h;
}

/* ── layout ──────────────────────────────────────────────────────────── */
#define PLAYER_Y    (WIN_H - CARD_H - 30)
#define COMPUTER_Y  50
#define DISCARD_X   (WIN_W/2 - CARD_W/2)
#define DISCARD_Y   (WIN_H/2 - CARD_H/2 - 20)

static Button btn_draw   = {WIN_W/2+120, WIN_H/2-20, 110, 36, "Draw Card"};
static Button btn_toggle = {WIN_W-160,   WIN_H-50,   140, 34, "Show CPU Cards"};
static Button btn_hints  = {20,          WIN_H-50,    90, 34, "Hints: OFF"};
static Button btn_suits[4];

/* ── render ──────────────────────────────────────────────────────────── */
static void draw_msg_wrapped(const char *text, int x, int y, int max_w, Color *c, XftFont *f)
{
    char buf[512];
    strncpy(buf, text, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    char *words[128];
    int nw = 0;
    char *p = buf;
    while(*p && nw < 128){
        while(*p == ' ') p++;
        if(!*p) break;
        words[nw++] = p;
        while(*p && *p != ' ') p++;
        if(*p) *p++ = '\0';
    }
    char line[512] = "";
    int line_y = y;
    int line_h = f->ascent + f->descent + 2;
    for(int i = 0; i < nw; i++){
        char test[512];
        if(line[0]) snprintf(test, sizeof test, "%s %s", line, words[i]);
        else        snprintf(test, sizeof test, "%s", words[i]);
        if(text_width(f, test) <= max_w){
            strcpy(line, test);
        } else {
            if(line[0]) draw_str(x, line_y, line, c, f);
            line_y += line_h;
            strcpy(line, words[i]);
        }
    }
    if(line[0]) draw_str(x, line_y, line, c, f);
}

static void render(int mx, int my)
{
    fill_rect(0,0,WIN_W,WIN_H,COL_BG.xlib);
    fill_rounded(20,10,WIN_W-40,WIN_H-20,20,COL_FELT.xlib);

    /* title */
    {
        int tw=text_width(font_lg,"MACAO");
        draw_str(WIN_W/2-tw/2, 24, "MACAO", &COL_GOLD, font_lg);
    }

    /* status — word wrapped */
    draw_msg_wrapped(msg, 20, WIN_H/2-72, WIN_W/2-80, &COL_WHITE, font_sm);

    /* discard */
    draw_str(DISCARD_X, DISCARD_Y-14, "Last played:", &COL_GOLD, font_sm);
    draw_card_face(DISCARD_X, DISCARD_Y, top_discard(), 0);

    if(requested_suit>=0){
        char rs[32];
        snprintf(rs,sizeof rs,"Requested: %s",SUIT_STR[requested_suit]);
        Color *rc=is_red_suit(requested_suit)?&COL_RED:&COL_BLACK;
        fill_rounded(DISCARD_X,DISCARD_Y+CARD_H+8,150,24,6,COL_WHITE.xlib);
        draw_str(DISCARD_X+6,DISCARD_Y+CARD_H+24,rs,rc,font_sm);
    }

    /* deck */
    {
        int dx=WIN_W/2-CARD_W/2-CARD_W-30, dy=DISCARD_Y;
        draw_str(dx,dy-14,"Deck:",&COL_GOLD,font_sm);
        if(deck_top>0){
            draw_card_back(dx,dy);
            char dbuf[16]; snprintf(dbuf,sizeof dbuf,"%d",deck_top);
            draw_str(dx+CARD_W/2-8,dy+CARD_H+16,dbuf,&COL_WHITE,font_sm);
        } else {
            fill_rounded(dx,dy,CARD_W,CARD_H,CARD_RAD,mkcolor_lazy(40,40,40).xlib);
            draw_str(dx+10,dy+CARD_H/2+5,"Empty",&COL_GRAY,font_sm);
        }
    }

    /* turn indicator */
    {
        const char *ts=
            state==GS_PLAYER_TURN  ?"YOUR TURN":
            state==GS_COMPUTER_TURN?"COMPUTER'S TURN":
            state==GS_ACE_DIALOG   ?"CHOOSE SUIT":
            state==GS_SKIP_HUMAN   ?(pending_draw>0?"DRAW CARDS OR COUNTER":(pending_skip>0?"SKIP TURN - play 4/7 or accept":"YOU SKIP THIS TURN")):
            state==GS_GAME_OVER    ?"GAME OVER":"";
        Color *tc=(state==GS_PLAYER_TURN||state==GS_ACE_DIALOG)?&COL_GREEN_HI:&COL_BLUE_HI;
        int tw=text_width(font_md,ts);
        fill_rounded(WIN_W/2-tw/2-10,WIN_H/2-CARD_H/2-50,tw+20,26,6,COL_SHADOW.xlib);
        draw_str(WIN_W/2-tw/2,WIN_H/2-CARD_H/2-30,ts,tc,font_md);
    }

    /* computer hand — scrollable */
    {
        int n=computer_hand.n;
        int max_vis=(WIN_W-80)/(CARD_W+4);
        if(n<=max_vis) computer_scroll=0;
        else if(computer_scroll>n-max_vis) computer_scroll=n-max_vis;
        if(computer_scroll<0) computer_scroll=0;
        int vis=n<max_vis?n:max_vis;
        int total_w=vis*(CARD_W+4)-4;
        int sx=(WIN_W-total_w)/2;
        char cbuf[64]; snprintf(cbuf,sizeof cbuf,"Computer (%d cards)%s:",n,n>max_vis?" [scroll < >]":"");
        draw_str(sx,COMPUTER_Y-4,cbuf,&COL_GOLD,font_sm);
        for(int i=0;i<vis;i++){
            int ci=computer_scroll+i;
            int cx=sx+i*(CARD_W+4);
            if(show_computer) draw_card_face(cx,COMPUTER_Y,computer_hand.cards[ci],0);
            else              draw_card_back(cx,COMPUTER_Y);
        }
        /* nav arrows */
        if(computer_scroll>0){
            fill_rounded(30,COMPUTER_Y+CARD_H/2-14,28,28,6,COL_BTN.xlib);
            draw_str(38,COMPUTER_Y+CARD_H/2+6,"<",&COL_WHITE,font_md);
        }
        if(computer_scroll+max_vis<n){
            fill_rounded(WIN_W-58,COMPUTER_Y+CARD_H/2-14,28,28,6,COL_BTN.xlib);
            draw_str(WIN_W-50,COMPUTER_Y+CARD_H/2+6,">",&COL_WHITE,font_md);
        }
    }

    /* player hand — scrollable */
    {
        int n=player_hand.n;
        int max_vis=(WIN_W-80)/(CARD_W+4);
        if(n<=max_vis) player_scroll=0;
        else if(player_scroll>n-max_vis) player_scroll=n-max_vis;
        if(player_scroll<0) player_scroll=0;
        int vis=n<max_vis?n:max_vis;
        int total_w=vis*(CARD_W+4)-4;
        int sx=(WIN_W-total_w)/2;
        char pbuf[64];
        snprintf(pbuf,sizeof pbuf,"Your hand — %d cards%s (click to play):",n,n>max_vis?" [scroll < >]":"");
        draw_str(sx,PLAYER_Y-16,pbuf,&COL_GOLD,font_sm);
        Card top=top_discard();
        for(int i=0;i<vis;i++){
            int ci=player_scroll+i;
            int cx=sx+i*(CARD_W+4);
            int playable=show_hints&&(state==GS_PLAYER_TURN||(state==GS_SKIP_HUMAN&&(pending_draw>0||pending_skip>0)))&&can_play(player_hand.cards[ci],top,requested_suit);
            int hover=(mx>=cx&&mx<=cx+CARD_W&&my>=PLAYER_Y&&my<=PLAYER_Y+CARD_H);
            int dy=(hover&&playable)?PLAYER_Y-10:PLAYER_Y;
            draw_card_face(cx,dy,player_hand.cards[ci],playable);
        }
        /* nav arrows */
        if(player_scroll>0){
            fill_rounded(30,PLAYER_Y+CARD_H/2-14,28,28,6,COL_BTN.xlib);
            draw_str(38,PLAYER_Y+CARD_H/2+6,"<",&COL_WHITE,font_md);
        }
        if(player_scroll+max_vis<n){
            fill_rounded(WIN_W-58,PLAYER_Y+CARD_H/2-14,28,28,6,COL_BTN.xlib);
            draw_str(WIN_W-50,PLAYER_Y+CARD_H/2+6,">",&COL_WHITE,font_md);
        }
    }

    if(state==GS_PLAYER_TURN||(state==GS_SKIP_HUMAN&&pending_draw>0))
        draw_button(btn_draw, in_button(btn_draw,mx,my));
    if(state==GS_SKIP_HUMAN&&pending_skip>0){
        Button btn_skip={WIN_W/2+120,WIN_H/2+20,130,40,"Accept Skip"};
        draw_button(btn_skip, in_button(btn_skip,mx,my));
    }
    draw_button(btn_toggle, in_button(btn_toggle,mx,my));
    btn_hints.label = show_hints ? "Hints: ON" : "Hints: OFF";
    draw_button(btn_hints, in_button(btn_hints,mx,my));

    /* ace dialog */
    if(state==GS_ACE_DIALOG){
        int dx=WIN_W/2-180, dy=WIN_H/2-60;
        fill_rounded(dx-10,dy-30,380,180,12,mkcolor_lazy(20,20,60).xlib);
        XSetForeground(dpy,gc,COL_GOLD.xlib);
        XDrawRectangle(dpy,back_buf,gc,dx-10,dy-30,380,180);
        draw_str(dx+40,dy-10,"Choose the requested suit:",&COL_WHITE,font_md);
        /* suit symbols big enough to see clearly */
        static const char *snames[]={"Hearts \xe2\x99\xa5","Diamonds \xe2\x99\xa6","Clubs \xe2\x99\xa3","Spades \xe2\x99\xa0"};
        for(int s=0;s<4;s++){
            btn_suits[s].x=dx+s*88; btn_suits[s].y=dy+20;
            btn_suits[s].w=80; btn_suits[s].h=60; btn_suits[s].label=snames[s];
            Color *bc=(s<2)?&COL_RED:&COL_BLACK;
            int hov=in_button(btn_suits[s],mx,my);
            fill_rounded(btn_suits[s].x,btn_suits[s].y,80,60,8,
                         hov?mkcolor_lazy(200,200,200).xlib:COL_WHITE.xlib);
            draw_str(btn_suits[s].x+4,btn_suits[s].y+36,snames[s],bc,font_sm);
        }
    }

    /* game over */
    if(state==GS_GAME_OVER){
        int gx=WIN_W/2-160, gy=WIN_H/2-50;
        fill_rounded(gx,gy,320,120,16,COL_SHADOW.xlib);
        XSetForeground(dpy,gc,COL_GOLD.xlib);
        XDrawRectangle(dpy,back_buf,gc,gx,gy,320,120);
        int tw=text_width(font_md,msg);
        draw_str(gx+(320-tw)/2,gy+45,msg,&COL_GOLD,font_md);
        Button br={gx+80,gy+65,160,36,"Play Again"};
        draw_button(br,in_button(br,mx,my));
    }

    /* floating dragged card */
    if(drag_active && drag_card_idx >= 0){
        int fx = drag_x - drag_ox;
        int fy = drag_y - drag_oy;
        /* drop zone highlight: discard pile area + margin */
        int dz_x=DISCARD_X-12, dz_y=DISCARD_Y-12, dz_w=CARD_W+24, dz_h=CARD_H+24;
        XSetForeground(dpy,gc,mkcolor_lazy(255,255,100).xlib);
        XSetLineAttributes(dpy,gc,2,LineSolid,CapButt,JoinMiter);
        XDrawRectangle(dpy,back_buf,gc,(unsigned)dz_x,(unsigned)dz_y,(unsigned)dz_w,(unsigned)dz_h);
        XSetLineAttributes(dpy,gc,1,LineSolid,CapButt,JoinMiter);
        /* shadow */
        fill_rounded(fx+6, fy+6, CARD_W, CARD_H, CARD_RAD, mkcolor_lazy(0,0,0).xlib);
        draw_card_face(fx, fy, player_hand.cards[drag_card_idx], 1);
    }
    /* ── macao UI ── */
    if(macao_window_open && !player_said_macao && player_hand.n==1){
        int secs = (int)(macao_deadline - time(NULL));
        if(secs<0) secs=0;
        char tbuf[32]; snprintf(tbuf,sizeof tbuf,"SAY MACAO! (%ds)",secs);
        Button btn_mac={WIN_W/2-90, WIN_H/2-80, 180, 44, tbuf};
        /* flash red when <2s */
        unsigned long bcol = secs<=2 ? mkcolor_lazy(200,20,20).xlib : mkcolor_lazy(160,0,120).xlib;
        fill_rounded(btn_mac.x+2,btn_mac.y+2,btn_mac.w,btn_mac.h,8,COL_SHADOW.xlib);
        fill_rounded(btn_mac.x,btn_mac.y,btn_mac.w,btn_mac.h,8,bcol);
        Color cw=mkcolor_lazy(255,255,255);
        int tw=text_width(font_md,tbuf);
        draw_str(btn_mac.x+(btn_mac.w-tw)/2, btn_mac.y+btn_mac.h/2+6, tbuf, &cw, font_md);
    }
    if(show_call_macao_btn && computer_hand.n==1){
        Button btn_call={WIN_W/2-90, COMPUTER_Y+CARD_H+30, 180, 44, "Call Macao!"};
        fill_rounded(btn_call.x+2,btn_call.y+2,btn_call.w,btn_call.h,8,COL_SHADOW.xlib);
        fill_rounded(btn_call.x,btn_call.y,btn_call.w,btn_call.h,8,mkcolor_lazy(180,120,0).xlib);
        Color cw=mkcolor_lazy(255,255,255);
        int tw=text_width(font_md,btn_call.label);
        draw_str(btn_call.x+(btn_call.w-tw)/2, btn_call.y+btn_call.h/2+6, btn_call.label, &cw, font_md);
    }

    /* blit back buffer to window in one shot — eliminates flicker */
    XCopyArea(dpy,back_buf,win,gc,0,0,WIN_W,WIN_H,0,0);
    XFlush(dpy);
}

/* ── helper: force a hand to draw N cards ───────────────────────────── */
static void force_draw(Hand *h, int n, int is_player)
{
    int max_vis = (WIN_W-80)/(CARD_W+4);
    int drawn   = 0;

    /* pass 1: draw from current deck (may trigger one reshuffle internally) */
    while(drawn < n && h->n < MAX_HAND){
        if(deck_top == 0){
            /* try reshuffle now */
            if(discard_top <= 1) break;  /* nothing to reshuffle yet */
            reset_deck_from_discard();
            if(deck_top == 0) break;     /* still empty after reshuffle */
        }
        Card c = draw_card();
        if(c.rank == 0 && c.suit == 0) break;
        h->cards[h->n++] = c;
        drawn++;
    }

    /* pass 2: if still short, try one more reshuffle and continue */
    if(drawn < n && deck_top == 0 && discard_top > 1){
        reset_deck_from_discard();
        while(drawn < n && h->n < MAX_HAND && deck_top > 0){
            Card c = draw_card();
            if(c.rank == 0 && c.suit == 0) break;
            h->cards[h->n++] = c;
            drawn++;
        }
    }

    if(drawn < n){
        /* couldn't fulfill full penalty — report and clear pending */
        snprintf(msg, sizeof msg,
            "%s drew %d of %d forced cards — no more cards available. Penalty cleared.",
            is_player ? "You" : "Computer", drawn, n);
        pending_draw = 0;
    }

    /* auto-scroll player to rightmost card after forced draw */
    if(is_player){
        int excess = h->n - max_vis;
        player_scroll = excess > 0 ? excess : 0;
    }
}


/* ── macao check — called after any card is played ─────────────────── */
static void check_macao_after_play(int is_player)
{
    if(is_player && player_hand.n==1){
        /* player has 1 card — open 5s window */
        player_said_macao = 0;
        macao_window_open  = 1;
        macao_deadline     = time(NULL) + 3;
        snprintf(msg, sizeof msg, "You have 1 card! Say MACAO within 5 seconds!");
    }
    if(!is_player && computer_hand.n==1){
        computer_said_macao = 0;
        show_call_macao_btn = 0;
        if((rand()%100) < 90){
            computer_said_macao = 1;
            snprintf(msg, sizeof msg, "Computer says: MACAO!");
        } else {
            /* computer forgot — show button for player to call it out */
            show_call_macao_btn = 1;
            snprintf(msg, sizeof msg, "Computer has 1 card... did it say macao?");
        }
    }
}

/* ── tick macao timer — called every frame ─────────────────────────── */
static void tick_macao(void)
{
    if(!macao_window_open) return;
    if(player_hand.n != 1){ macao_window_open=0; return; } /* hand changed */
    if(time(NULL) >= macao_deadline){
        macao_window_open = 0;
        if(!player_said_macao){
            /* player missed the window — 50/50 computer notices */
            if((rand()%100) < 50){
                int n = player_hand.n;
                for(int i=0;i<5;i++){
                    if(cards_available()==0) break;
                    Card _c=draw_card();
                    if((_c.rank||_c.suit)&&player_hand.n<MAX_HAND) player_hand.cards[player_hand.n++]=_c;
                }
                int max_vis=(WIN_W-80)/(CARD_W+4);
                int excess=player_hand.n-max_vis;
                player_scroll=excess>0?excess:0;
                snprintf(msg, sizeof msg,
                    "You forgot to say MACAO! Computer noticed — you draw 5 cards! (%d->%d)",
                    n, player_hand.n);
            } else {
                snprintf(msg, sizeof msg, "You forgot to say MACAO! Computer didn't notice this time.");
            }
        }
    }
}

/* ── computer turn ───────────────────────────────────────────────────── */
static void do_computer_turn(void)
{
    /* handle pending skip */
    if(pending_skip>0){
        pending_skip--;
        snprintf(msg,sizeof msg,"Computer skips a turn (%d remaining).",pending_skip);
        state=GS_PLAYER_TURN;
        return;
    }
    /* handle pending draw obligation */
    if(pending_draw>0){
        Card top=top_discard();
        /* check if computer can counter with 2,3,joker or cancel with 7 */
        int playable[MAX_HAND],np=0;
        for(int i=0;i<computer_hand.n;i++)
            if(can_play(computer_hand.cards[i],top,requested_suit)) playable[np++]=i;
        if(np>0){
            play_card_from(&computer_hand,playable[rand()%np],0);
        } else {
            force_draw(&computer_hand,pending_draw,0);
            snprintf(msg,sizeof msg,"Computer drew %d card(s) (forced).",pending_draw);
            pending_draw=0;
        }
        if(computer_hand.n==0){ state=GS_GAME_OVER; snprintf(msg,sizeof msg,"Computer wins!"); return; }
        if(pending_skip>0){ state=GS_SKIP_HUMAN; return; }
        if(pending_draw>0){ state=GS_SKIP_HUMAN; return; } /* player must now draw */
        state=GS_PLAYER_TURN;
        return;
    }
    Card top=top_discard();
    int lazy=(rand()%100)<COMPUTER_BAD_MOVE_CHANCE;
    int playable[MAX_HAND],np=0;
    for(int i=0;i<computer_hand.n;i++)
        if(can_play(computer_hand.cards[i],top,requested_suit)) playable[np++]=i;
    if(np==0||lazy){
        if(cards_available()==0)
            snprintf(msg,sizeof msg,"Computer cannot play and deck is empty.");
        else{
            Card c=draw_card();
            if(computer_hand.n<MAX_HAND) computer_hand.cards[computer_hand.n++]=c;
            snprintf(msg,sizeof msg,"Computer drew a card.");
        }
    } else {
        int choice=playable[rand()%np];
        play_card_from(&computer_hand,choice,0);
    }
    if(computer_hand.n==0){ state=GS_GAME_OVER; snprintf(msg,sizeof msg,"Computer wins!"); return; }
    check_macao_after_play(0);
    if(pending_skip>0){ state=GS_SKIP_HUMAN; return; }
    if(pending_draw>0){ state=GS_SKIP_HUMAN; return; }
    state=GS_PLAYER_TURN;
}

/* ── new game ────────────────────────────────────────────────────────── */
static void new_game(void)
{
    build_deck();
    shuffle(deck,DECK_SIZE);
    deck_top=DECK_SIZE; discard_top=0;
    requested_suit=-1; pending_skip=0; pending_draw=0; show_computer=0;
    player_said_macao=0; computer_said_macao=0; macao_deadline=0; macao_window_open=0; show_call_macao_btn=0; show_hints=0;
    deal();
    state=GS_PLAYER_TURN;
    player_scroll=0; computer_scroll=0;
    snprintf(msg,sizeof msg,"Game started! Your turn.");
}

/* ── handle press (ButtonPress) ─────────────────────────────────────── */
static void handle_press(int mx, int my)
{
    if(state==GS_GAME_OVER){
        Button br={WIN_W/2-80,WIN_H/2+15,160,36,"Play Again"};
        if(in_button(br,mx,my)) new_game();
        return;
    }
    if(in_button(btn_toggle,mx,my)){ show_computer=!show_computer; return; }
    if(in_button(btn_hints,mx,my)){ show_hints=!show_hints; return; }

    /* macao buttons */
    if(macao_window_open && !player_said_macao && player_hand.n==1){
        Button btn_mac={WIN_W/2-90, WIN_H/2-80, 180, 44, ""};
        if(in_button(btn_mac,mx,my)){
            player_said_macao=1;
            macao_window_open=0;
            snprintf(msg,sizeof msg,"You said MACAO!");
            return;
        }
    }
    if(show_call_macao_btn && computer_hand.n==1){
        Button btn_call={WIN_W/2-90, COMPUTER_Y+CARD_H+30, 180, 44, ""};
        if(in_button(btn_call,mx,my)){
            show_call_macao_btn=0;
            /* computer forgot macao — it draws 5 */
            for(int i=0;i<5;i++){
                if(cards_available()==0) break;
                Card _c=draw_card();
                if((_c.rank||_c.suit)&&computer_hand.n<MAX_HAND) computer_hand.cards[computer_hand.n++]=_c;
            }
            snprintf(msg,sizeof msg,"You caught it! Computer forgot MACAO and draws 5 cards!");
            return;
        }
    }
    if(state==GS_ACE_DIALOG){
        for(int s=0;s<4;s++){
            if(in_button(btn_suits[s],mx,my)){
                requested_suit=s;
                state=GS_COMPUTER_TURN;
                snprintf(msg,sizeof msg,"You requested %s - computer's turn.",SUIT_STR[s]);
            }
        }
        return;
    }
    if(state==GS_SKIP_HUMAN){
        if(pending_skip>0){
            /* check Accept Skip button */
            Button btn_skip={WIN_W/2+120,WIN_H/2+20,130,40,"Accept Skip"};
            if(in_button(btn_skip,mx,my)){
                pending_skip--;
                snprintf(msg,sizeof msg,"You skipped your turn (%d skip(s) remaining).",pending_skip);
                state=GS_COMPUTER_TURN;
                return;
            }
            /* otherwise fall through — player may play a 4 or 7 */
        } else if(pending_draw>0){
            /* fall through — player may play a counter or click Draw */
        } else {
            state=GS_COMPUTER_TURN;
            snprintf(msg,sizeof msg,"You skipped your turn.");
            return;
        }
    }
    if(state!=GS_PLAYER_TURN && state!=GS_SKIP_HUMAN) return;

    if(in_button(btn_draw,mx,my)){
        if(cards_available()==0){ snprintf(msg,sizeof msg,"No cards left to draw!"); return; }
        if(pending_draw>0){
            int n=pending_draw; pending_draw=0;
            force_draw(&player_hand,n,1);
            snprintf(msg,sizeof msg,"You drew %d card(s) (forced).",n);
        } else {
            Card c=draw_card();
            if(player_hand.n<MAX_HAND) player_hand.cards[player_hand.n++]=c;
            /* auto-scroll to show new card */
            {int max_vis=(WIN_W-80)/(CARD_W+4);
             int excess=player_hand.n-max_vis;
             player_scroll=excess>0?excess:0;}
            snprintf(msg,sizeof msg,"You drew %s%s.",RANK_STR[c.rank],c.rank==RANK_JOKER?"":SUIT_STR[c.suit]);
        }
        state=GS_COMPUTER_TURN;
        return;
    }

    /* player scroll arrows */
    {
        int n=player_hand.n;
        int max_vis=(WIN_W-80)/(CARD_W+4);
        if(mx>=30&&mx<=58&&my>=PLAYER_Y+CARD_H/2-14&&my<=PLAYER_Y+CARD_H/2+14){
            if(player_scroll>0) player_scroll--;
            return;
        }
        if(mx>=WIN_W-58&&mx<=WIN_W-30&&my>=PLAYER_Y+CARD_H/2-14&&my<=PLAYER_Y+CARD_H/2+14){
            if(player_scroll+max_vis<n) player_scroll++;
            return;
        }
        /* pick up card for drag */
        if(state==GS_PLAYER_TURN||state==GS_SKIP_HUMAN){
            int vis=n<max_vis?n:max_vis;
            int total_w=vis*(CARD_W+4)-4;
            int sx=(WIN_W-total_w)/2;
            for(int i=0;i<vis;i++){
                int ci=player_scroll+i;
                int cx=sx+i*(CARD_W+4);
                if(mx>=cx&&mx<=cx+CARD_W&&my>=PLAYER_Y-10&&my<=PLAYER_Y+CARD_H){
                    drag_active   = 1;
                    drag_card_idx = ci;
                    drag_x        = mx;
                    drag_y        = my;
                    drag_ox       = mx - cx;
                    drag_oy       = my - PLAYER_Y;
                    return;
                }
            }
        }
    }
}

/* ── handle release (ButtonRelease) ─────────────────────────────────── */
static void handle_release(int mx, int my)
{
    if(!drag_active){ return; }
    drag_active = 0;
    int ci = drag_card_idx;
    drag_card_idx = -1;

    if(state!=GS_PLAYER_TURN && state!=GS_SKIP_HUMAN) return;

    /* compute overlap between dropped card rect and discard zone */
    int cx = mx - drag_ox;
    int cy = my - drag_oy;
    /* card rect */
    int c_x1=cx, c_y1=cy, c_x2=cx+CARD_W, c_y2=cy+CARD_H;
    /* discard zone rect */
    int d_x1=DISCARD_X, d_y1=DISCARD_Y, d_x2=DISCARD_X+CARD_W, d_y2=DISCARD_Y+CARD_H;
    /* intersection */
    int ix1=c_x1>d_x1?c_x1:d_x1, iy1=c_y1>d_y1?c_y1:d_y1;
    int ix2=c_x2<d_x2?c_x2:d_x2, iy2=c_y2<d_y2?c_y2:d_y2;
    int inter = (ix2>ix1&&iy2>iy1) ? (ix2-ix1)*(iy2-iy1) : 0;
    int card_area = CARD_W * CARD_H;
    /* valid drop: >35% of card overlaps discard zone */
    if(inter * 100 / card_area < 35) return; /* snap back — no play */

    Card top=top_discard();
    if(!can_play(player_hand.cards[ci],top,requested_suit)){
        snprintf(msg,sizeof msg,"Can't play that card!");
        return;
    }
    play_card_from(&player_hand,ci,1);
    {int max_vis=(WIN_W-80)/(CARD_W+4);
     if(player_hand.n<=max_vis) player_scroll=0;
     else if(player_scroll>player_hand.n-max_vis) player_scroll=player_hand.n-max_vis;}
    if(player_hand.n==0){ state=GS_GAME_OVER; snprintf(msg,sizeof msg,"You win! Congratulations!"); return; }
    check_macao_after_play(1);
    if(state!=GS_ACE_DIALOG) state=GS_COMPUTER_TURN;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void)
{
    srand((unsigned)time(NULL));

    dpy=XOpenDisplay(NULL);
    if(!dpy){ fprintf(stderr,"Cannot open display\n"); return 1; }
    scr    = DefaultScreen(dpy);
    visual = DefaultVisual(dpy,scr);
    cmap   = DefaultColormap(dpy,scr);

    win=XCreateSimpleWindow(dpy,RootWindow(dpy,scr),
                             100,100,WIN_W,WIN_H,2,
                             BlackPixel(dpy,scr),BlackPixel(dpy,scr));
    XStoreName(dpy,win,"Macao");
    XSelectInput(dpy,win,ExposureMask|KeyPressMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask);
    XMapWindow(dpy,win);

    gc=XCreateGC(dpy,win,0,NULL);
    back_buf=XCreatePixmap(dpy,win,WIN_W,WIN_H,(unsigned)DefaultDepth(dpy,scr));
    init_colors();

    xft_draw=XftDrawCreate(dpy,win,visual,cmap);
    if(!xft_draw){ fprintf(stderr,"XftDrawCreate failed\n"); return 1; }
    xft_back=XftDrawCreate(dpy,back_buf,visual,cmap);
    if(!xft_back){ fprintf(stderr,"XftDrawCreate back failed\n"); return 1; }

    font_sm=load_xft_font(11,0);
    font_md=load_xft_font(13,1);
    font_lg=load_xft_font(16,1);

    new_game();

    int mx=0,my=0,cpu_tick=0;
    XEvent e;
    for(;;){
        while(XPending(dpy)){
            XNextEvent(dpy,&e);
            if(e.type==Expose)       render(mx,my);
            if(e.type==MotionNotify){ mx=e.xmotion.x; my=e.xmotion.y; if(drag_active){drag_x=mx;drag_y=my;} render(mx,my); }
            if(e.type==ButtonPress){
                handle_press(e.xbutton.x, e.xbutton.y);
                render(mx,my);
            }
            if(e.type==ButtonRelease){
                handle_release(e.xbutton.x, e.xbutton.y);
                render(mx,my);
            }
            if(e.type==KeyPress){
                KeySym ks=XLookupKeysym(&e.xkey,0);
                if(ks==XK_q||ks==XK_Escape) goto done;
                if(ks==XK_n){ new_game(); render(mx,my); }
            }
        }
        if(state==GS_COMPUTER_TURN){
            struct timespec ts={0,30000000};
            nanosleep(&ts,NULL);
            if(++cpu_tick>20){ cpu_tick=0; do_computer_turn(); render(mx,my); }
        } else {
            cpu_tick=0;
            tick_macao();
            struct timespec ts={0,16000000};
            nanosleep(&ts,NULL);
        }
    }
done:
    XftDrawDestroy(xft_back);
    XftDrawDestroy(xft_draw);
    XFreePixmap(dpy,back_buf);
    XftFontClose(dpy,font_sm);
    XftFontClose(dpy,font_md);
    XftFontClose(dpy,font_lg);
    XFreeGC(dpy,gc);
    XCloseDisplay(dpy);
    return 0;
}