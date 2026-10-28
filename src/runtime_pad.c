/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Default keyboard layout (GOW3_KEY_MAP changes the buttons, see apply_button_map):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad,
 *   IJKL d-pad (I up, K down, J left, L right). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
#define MAX_GAMEPADS 8
static SDL_Gamepad *gamepads[MAX_GAMEPADS];
static int gamepad_count;
static uint64_t gamepad_scan_us;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Joysticks SDL has no gamepad mapping for are named once in the log (they cannot be read). */
static void report_unmapped_joysticks(void) {
    static SDL_JoystickID reported[16];
    static int reported_count;
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count;++i) {
        if (SDL_IsGamepad(ids[i])) continue;
        int seen=0;
        for (int j=0;j<reported_count;++j) seen|=reported[j]==ids[i];
        if (seen || reported_count==16) continue;
        reported[reported_count++]=ids[i];
        const char *name=SDL_GetJoystickNameForID(ids[i]);
        printf("Runtime: joystick without a gamepad mapping (not used): %s (USB %04x:%04x)\n",name ? name : "?",
               SDL_GetJoystickVendorForID(ids[i]),SDL_GetJoystickProductForID(ids[i]));
    }
    SDL_free(ids);
}
/* Opens every gamepad SDL knows about and returns how many are open; called under lock. All of
 * them are read: with only the first one, a second device listed before the pad in use (a
 * virtual pad, a wheel, a receiver) left that pad unread (issue #25). */
static int current_gamepads(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return 0;
    int open=0;
    for (int i=0;i<gamepad_count;++i) {
        if (SDL_GamepadConnected(gamepads[i])) { gamepads[open++]=gamepads[i]; continue; }
        printf("Runtime: gamepad disconnected: %s\n",SDL_GetGamepadName(gamepads[i]));
        SDL_CloseGamepad(gamepads[i]);
    }
    gamepad_count=open;
    /* New pads: looked for once a second (and at once while none is open). */
    const uint64_t now=now_us();
    if (gamepad_count && now-gamepad_scan_us<1000000u) return gamepad_count;
    gamepad_scan_us=now;
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    for (int i=0;ids && i<count && gamepad_count<MAX_GAMEPADS;++i) {
        if (SDL_GetGamepadFromID(ids[i])) continue;
        SDL_Gamepad *g=SDL_OpenGamepad(ids[i]);
        if (!g) { printf("Runtime: gamepad %s could not be opened: %s\n",SDL_GetGamepadNameForID(ids[i]),SDL_GetError()); continue; }
        gamepads[gamepad_count++]=g;
        ++connected_count;
        printf("Runtime: gamepad connected: %s (USB %04x:%04x, %d open)\n",SDL_GetGamepadName(g),
               SDL_GetGamepadVendor(g),SDL_GetGamepadProduct(g),gamepad_count);
    }
    SDL_free(ids);
    report_unmapped_joysticks();
    return gamepad_count;
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with GOW3_PAD_MAP (gamepad) and GOW3_KEY_MAP (keyboard), e.g.
 * GOW3_KEY_MAP="cross=Q,square=Left Shift" or GOW3_PAD_MAP="cross=b,circle=a": comma-separated
 * <ps4 button>=<SDL key or gamepad button name>. A mapped PS4 button loses its default sources.
 * Sticks, triggers and the overlay shortcuts are not remapped. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static ButtonMap key_map={{
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_LSHIFT,BTN_CIRCLE}, {SDL_SCANCODE_E,BTN_SQUARE},
    {SDL_SCANCODE_Q,BTN_TRIANGLE}, {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
    {SDL_SCANCODE_R,BTN_L2}, {SDL_SCANCODE_F,BTN_R2}, {SDL_SCANCODE_Z,BTN_L3}, {SDL_SCANCODE_C,BTN_R3},
    {SDL_SCANCODE_RETURN,BTN_OPTIONS},
    {SDL_SCANCODE_I,BTN_UP}, {SDL_SCANCODE_K,BTN_DOWN}, {SDL_SCANCODE_J,BTN_LEFT}, {SDL_SCANCODE_L,BTN_RIGHT},
},15};

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

/* Applies a GOW3_PAD_MAP/GOW3_KEY_MAP value; returns how many entries were used. Bad entries are
 * logged and skipped, so their PS4 button keeps its default. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        const int source=!*name ? -1 : keyboard ? (int)SDL_GetScancodeFromName(name) : (int)SDL_GetGamepadButtonFromString(name);
        if (!ps || source<=(keyboard ? (int)SDL_SCANCODE_UNKNOWN : (int)SDL_GAMEPAD_BUTTON_INVALID)) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        size_t kept=0;
        for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
        map->count=kept;
        if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        ++used;
    }
    return used;
}

static void load_button_maps(void) {
    static bool loaded;
    if (loaded) return;
    loaded=true;
    const int pad=apply_button_map(&pad_map,getenv("GOW3_PAD_MAP"),false);
    const int keys=apply_button_map(&key_map,getenv("GOW3_KEY_MAP"),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard buttons remapped\n",pad,keys);
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
/* The mapped buttons of all open gamepads. */
static uint32_t gamepad_buttons(void) {
    uint32_t buttons=0;
    for (int p=0;p<gamepad_count;++p)
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(gamepads[p],(SDL_GamepadButton)pad_map.entries[i].source)) buttons|=pad_map.entries[i].ps;
    return buttons;
}
/* Linux fork: both back grips held (Steam Deck L4+R4, SDL's paddles), the recording toggle. */
static int back_grips_down(void) {
    for (int p=0;p<gamepad_count;++p)
        if (SDL_GetGamepadButton(gamepads[p],SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) &&
            SDL_GetGamepadButton(gamepads[p],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)) return 1;
    return 0;
}
/* Linux fork: R4 alone (or F10) writes a marker into the log, so a player can flag the moment a
 * glitch shows ("MARK n"), with the frame position and, under gamescope, a screenshot in
 * <user dir>/marks/. Games started from Steam see Steam's virtual controller, which has no back
 * grips, so on a Steam Deck R4 is also read straight from the controller's hidraw device
 * (28de:1205; reports are delivered to every reader, Steam keeps its own). */
static volatile int deck_r4_down;
#ifdef __linux__
static void *deck_grip_thread(void *arg) {
    (void)arg;
    int fds[8], count=0;
    for (int n=0;n<32 && count<8;++n) {
        char path[96], line[256]; int match=0;
        snprintf(path,sizeof(path),"/sys/class/hidraw/hidraw%d/device/uevent",n);
        FILE *f=fopen(path,"r");
        if (!f) continue;
        while (fgets(line,sizeof(line),f)) if (strstr(line,"HID_ID=0003:000028DE:00001205")) match=1;
        fclose(f);
        if (!match) continue;
        snprintf(path,sizeof(path),"/dev/hidraw%d",n);
        const int fd=open(path,O_RDONLY);
        if (fd>=0) fds[count++]=fd;
    }
    if (!count) return NULL;
    printf("Runtime: R4 marks the log (read from %d Steam Deck controller device%s)\n",count,count>1 ? "s" : "");
    struct pollfd p[8];
    for (int i=0;i<count;++i) p[i]=(struct pollfd){fds[i],POLLIN,0};
    unsigned char buf[64];
    while (count) {
        if (poll(p,count,-1)<0) continue;
        for (int i=0;i<count;++i) {
            if (!(p[i].revents & POLLIN)) { if (p[i].revents & (POLLERR|POLLHUP|POLLNVAL)) p[i].fd=-1; continue; }
            const ssize_t r=read(p[i].fd,buf,sizeof(buf));
            if (r==64 && buf[0]==1 && buf[1]==0 && buf[2]==9 && buf[3]==64) {
                uint32_t high; memcpy(&high,buf+12,4);
                deck_r4_down=(high & 0x400)!=0; /* STEAMDECK_HBUTTON_R4 */
            }
        }
    }
    return NULL;
}
#endif
static int marker_down(void) {
    static int started;
    if (!started) {
        started=1;
#ifdef __linux__
        pthread_t t;
        if (!pthread_create(&t,NULL,deck_grip_thread,NULL)) pthread_detach(t);
#endif
    }
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (k && k[SDL_SCANCODE_F10]) return 1;
    if (deck_r4_down) return 1;
    for (int p=0;p<gamepad_count;++p)
        if (SDL_GetGamepadButton(gamepads[p],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) &&
            !SDL_GetGamepadButton(gamepads[p],SDL_GAMEPAD_BUTTON_LEFT_PADDLE1)) return 1;
    return 0;
}
static void marker_poll(void) {
    static int was_down, count;
    static uint64_t start;
    if (!start) start=now_us();
    const int down=marker_down();
    if (down && !was_down) {
        uint32_t segment, frame; gow3gpu_frame_position(&segment,&frame);
        time_t now=time(NULL); struct tm tm;
#ifdef _WIN32
        localtime_s(&tm,&now);
#else
        localtime_r(&now,&tm);
#endif
        ++count;
        printf("MARK %d: %02d:%02d:%02d, %.1f s into the session, segment %u frame %u (the player pressed R4/F10)\n",
               count,tm.tm_hour,tm.tm_min,tm.tm_sec,(now_us()-start)/1e6,segment,frame);
#ifndef _WIN32
        if (getenv("GAMESCOPE_WAYLAND_DISPLAY")) {
            char dir[4096], cmd[8400];
            snprintf(dir,sizeof(dir),"%s/marks",runtime_file_user_dir());
            mkdir(dir,0755);
            snprintf(cmd,sizeof(cmd),"env -u LD_LIBRARY_PATH -u LD_PRELOAD gamescopectl screenshot '%s/mark-%04d%02d%02d-%02d%02d%02d-%d.png' >/dev/null 2>&1 &",
                     dir,tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,count);
            if (system(cmd)) {}
        }
#endif
        fflush(stdout);
    }
    was_down=down;
}
/* GOW3_STICK_RANGE=<percent> (launcher: Stick range): that much tilt counts as a full push, for
 * sticks that never reach the edge (some hall-effect pads, issue #36). Default 100. */
static int16_t stick_range(int16_t v) {
    static int range;
    if (!range) {
        const char *env=getenv("GOW3_STICK_RANGE");
        range=env ? atoi(env) : 100;
        if (range<50 || range>100) range=100;
        if (range!=100) printf("Runtime: stick range %d%% (that much tilt is a full push)\n",range);
    }
    const int scaled=v*100/range;
    return (int16_t)(scaled<-32768 ? -32768 : scaled>32767 ? 32767 : scaled);
}
/* A stick of the open gamepad that pushes it furthest (an idle pad's drift does not cover it). */
static void gamepad_stick(SDL_GamepadAxis x_axis, SDL_GamepadAxis y_axis, uint8_t *x, uint8_t *y) {
    int64_t best=-1;
    for (int p=0;p<gamepad_count;++p) {
        const int16_t vx=SDL_GetGamepadAxis(gamepads[p],x_axis), vy=SDL_GetGamepadAxis(gamepads[p],y_axis);
        const int64_t d=(int64_t)vx*vx+(int64_t)vy*vy;
        if (d>best) { best=d; *x=axis(stick_range(vx)); *y=axis(stick_range(vy)); }
    }
}
static uint8_t gamepad_trigger(SDL_GamepadAxis which) {
    uint8_t value=0;
    for (int p=0;p<gamepad_count;++p) {
        const uint8_t v=trigger(SDL_GetGamepadAxis(gamepads[p],which));
        if (v>value) value=v;
    }
    return value;
}
static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    const int pads=current_gamepads();
    if (gow3gpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    load_button_maps();
    if (pads) {
        d->buttons=gamepad_buttons();
        const uint32_t mapped=d->buttons;
        gamepad_stick(SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,&d->left_x,&d->left_y);
        gamepad_stick(SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY,&d->right_x,&d->right_y);
        d->l2=gamepad_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER); d->r2=gamepad_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        for (int p=0;p<pads && !d->touch_count;++p) {
            SDL_Gamepad *g=gamepads[p];
            if (SDL_GetNumGamepadTouchpads(g)<=0) continue;
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (!k) return;
    for (size_t i=0;i<key_map.count;++i) if (k[key_map.entries[i].source]) d->buttons|=key_map.entries[i].ps;
    if (k[SDL_SCANCODE_TAB]) touch_click(d,0);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0); /* a remapped touchpad key */
    if (k[SDL_SCANCODE_BACKSPACE]) touch_click(d,1);
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,k[SDL_SCANCODE_A],k[SDL_SCANCODE_D]);
    key_axis(&d->left_y,k[SDL_SCANCODE_W],k[SDL_SCANCODE_S]);
    key_axis(&d->right_x,k[SDL_SCANCODE_LEFT],k[SDL_SCANCODE_RIGHT]);
    key_axis(&d->right_y,k[SDL_SCANCODE_UP],k[SDL_SCANCODE_DOWN]);
}

/* GOW3_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a GOW3_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("GOW3_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* GOW3_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* GOW3_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; GOW3_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in GOW3_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
/* Linux fork: F9 or the Steam Deck's back grips L4+R4 together start and stop a recording, also
 * without GOW3_PAD_RECORD: then each one goes to <user dir>/pad-recordings/route-<date>-<time>.txt,
 * for the pyro test bench to replay. Games started from Steam see Steam's virtual controller,
 * which hides the grips: bind L4 or R4 to the keyboard key F9 in the game's controller layout. */
/* Linux fork: GOW3_PAD_RECORD_AUTO=1 (the launcher's "Record my inputs for testing") records the
 * whole session from the first frame, frame-stamped ("# v2": segment, frame within it, then the
 * pad state), so the pyro test bench replays it frame for frame and re-synchronises after every
 * load (gow3gpu_frame_position). */
static int record_v2;
static void record_sample(const PadData *d) {
    static const char *env_path; static int checked, f9_was_down, sticks_were_down, auto_mode;
    static char path[4096];
    if (!checked) {
        env_path=getenv("GOW3_PAD_RECORD"); checked=1;
        const char *a=getenv("GOW3_PAD_RECORD_AUTO"); auto_mode=a && a[0]=='1';
    }
    if (!sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    const int sticks=back_grips_down();
    const int auto_start=auto_mode==1;
    if (auto_start) auto_mode=2;
    if (auto_start || (f9 && !f9_was_down) || (sticks && !sticks_were_down)) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else {
            if (env_path && *env_path) snprintf(path,sizeof(path),"%s",env_path);
            else {
                char dir[4096]; time_t now=time(NULL); struct tm tm;
                snprintf(dir,sizeof(dir),"%s/pad-recordings",runtime_file_user_dir());
#ifdef _WIN32
                localtime_s(&tm,&now); mkdir(dir);
#else
                localtime_r(&now,&tm); mkdir(dir,0755);
#endif
                snprintf(path,sizeof(path),"%s/route-%04d%02d%02d-%02d%02d%02d.txt",dir,tm.tm_year+1900,
                         tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec);
            }
            if ((record_file=fopen(path,"w"))) {
                record_start=now_us();
                memset(&record_last,0xff,sizeof(record_last));
                record_v2=auto_start;
                if (record_v2) fputs("# v2 segment frame buttons lx ly rx ry l2 r2\n",record_file);
                printf("Runtime: pad recording started (%s, F9 or L4+R4 stops)\n",path);
            }
        }
    }
    f9_was_down=f9;
    sticks_were_down=sticks;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    if (record_v2) {
        uint32_t segment, frame; gow3gpu_frame_position(&segment,&frame);
        fprintf(record_file,"%u %u %u %u %u %u %u %u %u\n",segment,frame,s.buttons,s.axes[0],s.axes[1],
                s.axes[2],s.axes[3],s.l2,s.r2);
    } else
        fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
                s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static uint32_t *replay_segment; /* Linux fork: v2 recordings, the segment of each sample (ms = frame) */
static int replay_v2;
static void replay_load(void) {
    static int loaded;
    if (loaded) return;
    loaded=1;
    const char *path=getenv("GOW3_PAD_REPLAY");
    FILE *f=path ? fopen(path,"r") : NULL;
    char line[256]; size_t cap=0;
    while (f && fgets(line,sizeof(line),f)) {
        if (!strncmp(line,"# v2",4)) { replay_v2=1; continue; }
        unsigned v[9]; PadSample s; uint32_t seg=0;
        if (replay_v2) {
            if (sscanf(line,"%u %u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8])!=9) continue;
            seg=v[0];
            s=(PadSample){v[1],v[2],{(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5],(uint8_t)v[6]},(uint8_t)v[7],(uint8_t)v[8]};
        } else {
            if (sscanf(line,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])!=8) continue;
            s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
        }
        if (replay_count==cap) {
            cap=cap ? cap*2 : 1024;
            PadSample *r=realloc(replay,cap*sizeof(*replay)); uint32_t *g=realloc(replay_segment,cap*sizeof(*g));
            if (!r || !g) { free(r); free(g); replay=NULL; replay_segment=NULL; replay_count=0; break; }
            replay=r; replay_segment=g;
        }
        replay_segment[replay_count]=seg; replay[replay_count++]=s;
    }
    if (f) fclose(f);
    printf("Runtime: pad replay of %zu samples from %s%s\n",replay_count,path ? path : "(unset)",
           replay_v2 ? " (frame-stamped, starts by itself)" : "");
    if (replay_v2 && replay_count) replay_armed=1;
}
static void replay_sample(PadData *d) {
    if (!replay_armed && getenv("GOW3_PAD_REPLAY")) replay_load();
    if (!replay_armed) return;
    if (replay_v2) {
        uint32_t segment, frame; gow3gpu_frame_position(&segment,&frame);
        while (replay_next<replay_count && (replay_segment[replay_next]<segment ||
               (replay_segment[replay_next]==segment && replay[replay_next].ms<=frame))) ++replay_next;
        if (!replay_next) return;
        if (replay_next==replay_count && replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        if (replay_armed==2) return;
        const PadSample *s=&replay[replay_next-1];
        d->buttons=s->buttons;
        d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
        d->l2=s->l2; d->r2=s->r2;
        return;
    }
    if (!replay_start) {
        replay_load();
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
/* Buttons of the gamepad and keyboard as mapped, even while the overlay holds the game neutral. */
static uint32_t host_buttons(void) {
    uint32_t buttons=0;
    load_button_maps();
    if (current_gamepads()) buttons|=gamepad_buttons();
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (k) for (size_t i=0;i<key_map.count;++i) if (k[key_map.entries[i].source]) buttons|=key_map.entries[i].ps;
    return buttons;
}
/* Buttons for the port's save list (sceSaveDataDialog), polled by the overlay every frame while
 * it is open: the game stops reading the pad during the dialog. Gamepad and keyboard as mapped,
 * scripted input, and the left stick as up/down. */
static uint32_t list_buttons(void) {
    pthread_mutex_lock(&lock);
    read_inject();
    uint32_t buttons=host_buttons()|injected.buttons;
    const int pads=current_gamepads();
    for (int p=0;p<pads;++p) {
        const int y=SDL_GetGamepadAxis(gamepads[p],SDL_GAMEPAD_AXIS_LEFTY);
        if (y<-16000) buttons|=BTN_UP;
        else if (y>16000) buttons|=BTN_DOWN;
    }
    if (injected.stick[1]>=0 && injected.stick[1]<64) buttons|=BTN_UP;
    else if (injected.stick[1]>192) buttons|=BTN_DOWN;
    pthread_mutex_unlock(&lock);
    return buttons;
}
static void sample(PadData *d) {
    sample_host(d);
    if (gow3gpu_overlay_captures_input()) { hold_after_capture=1; return; }
    record_sample(d);
    if (sdl_ready>0) marker_poll();
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) {
    pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock);
    gow3gpu_choice_set_input(list_buttons);
    return 0;
}
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    runtime_ue4_setres_poll();
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepads();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    const int pads=current_gamepads();
    for (int p=0;p<pads;++p) SDL_RumbleGamepad(gamepads[p],(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
/* scePadGetHandle: the handle scePadOpen returned for this user's standard port. */
static ABI int32_t pad_get_handle(int32_t user, int32_t type, int32_t index) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1 || (type!=0 && type!=2) || index) return ERR_INVALID_ARG;
    return opened ? PAD_HANDLE : (int32_t)0x80920008; /* SCE_PAD_ERROR_NO_HANDLE */
}
static const RuntimeExport exports[]={
    {"scePadGetHandle",pad_get_handle},
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) {
    printf("Runtime: pad reads=%zu, gamepads=%d (%s)\n",reads,gamepad_count,gamepad_count ? SDL_GetGamepadName(gamepads[0]) : "none");
}
