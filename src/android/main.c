#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

#include <android/native_activity.h>

#include <GLES2/gl2.h>
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <EGL/egl.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "audio.c"

static void loadfonts(void);
static void freefonts(void);
#include "gl.c"
#include "freetype.c"


static volatile _Bool destroy, focused;

static _Bool shift;

static          ANativeActivity *activity;
static          ANativeWindow   *window;
static volatile ANativeWindow   *windowN;
static          AInputQueue     *inputQueue;
static volatile AInputQueue     *inputQueueNew;

static volatile ARect rect;
static volatile _Bool _redraw;

const char* internalPath[UTOX_FILE_NAME_LENGTH];

static int pipefd[2];
typedef struct {
    uint32_t msg;
    uint16_t param1, param2;
    void *data;
} PIPING;

void postmessage(uint32_t msg, uint16_t param1, uint16_t param2, void *data)
{
    PIPING piping = {
        .msg = msg,
        .param1 = param1,
        .param2 = param2,
        .data = data
    };

    write(pipefd[1], &piping, sizeof(PIPING));
}

void init_ptt(void){ settings.push_to_talk = 0; /* android is unsupported */ }
_Bool check_ptt_key(void){ return 1; /* android is unsupported */ }
void exit_ptt(void){ settings.push_to_talk = 0; /* android is unsupported */ }

void image_set_filter(UTOX_NATIVE_IMAGE *image, uint8_t filter){ /* Unsupported on android */ }
void image_set_scale(UTOX_NATIVE_IMAGE *image, double scale){ /* Unsupported on android */ }

void draw_image(const UTOX_NATIVE_IMAGE *data, int x, int y, uint32_t width, uint32_t height, uint32_t imgx, uint32_t imgy)
{
    GLuint texture = data;

    makequad(&quads[0], x - imgx, y - imgy, x + width, y + height);

    glBindTexture(GL_TEXTURE_2D, texture);

    float one[] = {1.0, 1.0, 1.0};
    float zero[] = {0.0, 0.0, 0.0};
    glUniform3fv(k, 1, one);
    glUniform3fv(k2, 1, zero);

    glDrawQuads(0, 1);

    glUniform3fv(k2, 1, one);
}


/* this function is no longer used, but it might contain handy information for creating the newer image draw functions
   on android. Currently drawing images is unsupported.
    void drawimage(UTOX_NATIVE_IMAGE data, int x, int y, int width, int height, int maxwidth, _Bool zoom, double position)
    {
        GLuint texture = data;

        if(!zoom && width > maxwidth) {
            makequad(&quads[0], x, y, x + maxwidth, y + (height * maxwidth / width));
        } else {
            makequad(&quads[0], x - (int)((double)(width - maxwidth) * position), y, x + width, y + height);
        }

        glBindTexture(GL_TEXTURE_2D, texture);

        float one[] = {1.0, 1.0, 1.0};
        float zero[] = {0.0, 0.0, 0.0};
        glUniform3fv(k, 1, one);
        glUniform3fv(k2, 1, zero);

        glDrawQuads(0, 1);

        glUniform3fv(k2, 1, one);
    }
*/

void thread(void func(void*), void *args)
{
    pthread_t thread_temp;
    pthread_create(&thread_temp, NULL, (void*(*)(void*))func, args);
}

void yieldcpu(uint32_t ms)
{
    usleep(1000 * ms);
}

uint64_t get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    return ((uint64_t)ts.tv_sec * (1000 * 1000 * 1000)) + (uint64_t)ts.tv_nsec;
}


/* These functions aren't support on Andorid HELP?
 * TODO: fix these! */
void copy(int value){ /* Unsupported on android */ }
void paste(void){ /* Unsupported on android */ }
void openurl(char_t *str){ /* Unsupported on android */ }
void openfilesend(void){ /* Unsupported on android */ }
void openfileavatar(void){ /* Unsupported on android */ }
void savefiledata(MSG_FILE *file){ /* Unsupported on android */ }
void setselection(char_t *data, uint16_t length){ /* Unsupported on android */ }
void edit_will_deactivate(void){ /* Unsupported on android */ }

UTOX_NATIVE_IMAGE *decode_image_rgb(const UTOX_IMAGE data, size_t size, uint16_t *w, uint16_t *h, _Bool keep_alpha)
{
    unsigned width, height, bpp;
    uint8_t *out = stbi_load_from_memory(data, size, &width, &height, &bpp, 3);
    //free(data);

    if (out == NULL || width == 0 || height == 0) {
        return 0;
    }

    *w = width;
    *h = height;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, out);
    free(out);

    return texture;
}

void image_free(UTOX_NATIVE_IMAGE *image)
{
    GLuint texture = image;
    glDeleteTextures(1, &texture);
}

void* loadsavedata(uint32_t *len)
{
    return file_raw("/data/data/tox.utox/files/tox_save", len);
}

void writesavedata(void *data, uint32_t len)
{
    debug("Trying to save data (android)\n");
    FILE *file;
    file = fopen("/data/data/tox.utox/files/tox_save", "wb");
    if(file) {
        fwrite(data, len, 1, file);
        fclose(file);
        debug("Saved data\n");
    } else {
        debug("fopen failed\n");
    }
}

int datapath(uint8_t *dest){
    return 0;
}

/** Takes data from µTox and saves it, just how the OS likes it saved! */
size_t native_save_data(const uint8_t *name, size_t name_length, const uint8_t *data, size_t length, _Bool append) {
    uint8_t path[UTOX_FILE_NAME_LENGTH];
    FILE *file;
    size_t offset = 0;

    snprintf((char*)path, UTOX_FILE_NAME_LENGTH, ANDROID_INTERNAL_SAVE);

    // mkdir((char*)path, 0700);

    snprintf((char*)path + strlen((const char*)path), UTOX_FILE_NAME_LENGTH - strlen((const char*)path), "%s", name);

    if (append) {
        file = fopen((const char*)path, "ab");
    } else {
        file = fopen((const char*)path, "wb");
    }

    if (file) {
        offset = ftello(file);
        fwrite(data, length, 1, file);
        fflush(file);
        fclose(file);

        if (append) {
            return offset;
        }

        return 1;
    } else {
        debug("NATIVE:\tUnable to open %s to write save\n", path);
        return 0;
    }

    return 0;
}

/** Takes data from µTox and loads it up! */
uint8_t *native_load_data(const uint8_t *name, size_t name_length, size_t *out_size){
    uint8_t path[UTOX_FILE_NAME_LENGTH];
    uint8_t *data;

    snprintf((char*)path, UTOX_FILE_NAME_LENGTH, ANDROID_INTERNAL_SAVE);

    if (strlen((const char*)path) + name_length >= UTOX_FILE_NAME_LENGTH){
        debug("NATIVE:\tLoad directory name too long\n");
        return 0;
    } else {
        snprintf((char*)path + strlen((const char*)path), UTOX_FILE_NAME_LENGTH - strlen((const char*)path), "%s", name);
    }


    FILE *file = fopen((const char*)path, "rb");
    if (!file) {
        //debug("NATIVE:\tUnable to open/read %s\n", path);
        if (out_size) {*out_size = 0;}
        return NULL;
    }


    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    data = calloc(size + 1, 1); // needed for the ending null byte
    if (!data) {
        fclose(file);
        if (out_size) {*out_size = 0;}
        return NULL;
    } else {
        fseek(file, 0, SEEK_SET);

        if(fread(data, size, 1, file) != 1) {
            debug("NATIVE:\tRead error on %s\n", path);
            fclose(file);
            free(data);
            if (out_size) {*out_size = 0;}
            return NULL;
        }

    fclose(file);
    }

    if (out_size) {*out_size = size;}
    return data;
}

/** native_load_data_log
 *
 *  reads records from the log file of a friend
 *
 * returns each MSG in the order they were stored, to a max of `count`
 * after skipping `skip` records
 */
FILE *native_load_data_logfile(uint32_t friend_number) {
    FRIEND *f = &friend[friend_number];
    uint8_t hex[TOX_PUBLIC_KEY_SIZE * 2];
    uint8_t path[UTOX_FILE_NAME_LENGTH];

    cid_to_string(hex, f->cid);

    snprintf((char*)path, UTOX_FILE_NAME_LENGTH, ANDROID_INTERNAL_SAVE);

    if (strlen((const char*)path) + sizeof(hex) >= UTOX_FILE_NAME_LENGTH){
        debug("NATIVE:\tLoad directory name too long\n");
        return 0;
    } else {
        snprintf((char*)path + strlen((const char*)path), UTOX_FILE_NAME_LENGTH - strlen((const char*)path),
                 "%.*s.new.txt", (int)sizeof(hex), (char*)hex);
    }

    FILE *file = fopen((const char*)path, "rb+");
    if (!file) {
        return NULL;
    }

    return file;
}

void native_select_dir_ft(uint32_t fid, MSG_FILE *file)
{
    return; /* TODO unsupported on android */
    //fall back to working dir
    char *path = malloc(file->name_length + 1);
    memcpy(path, file->name, file->name_length);
    path[file->name_length] = 0;

    postmessage_toxcore(TOX_FILE_ACCEPT, fid, file->filenumber, path);
}

void native_autoselect_dir_ft(uint32_t fid, FILE_TRANSFER *file) {
    return; /* TODO unsupported on android */
    /* TODO: maybe do something different here? */
    char *path = malloc(file->name_length + 1);
    memcpy(path, file->name, file->name_length);
    path[file->name_length] = 0;
    postmessage_toxcore(TOX_FILE_ACCEPT, fid, file->file_number, path);
}



void flush_file(FILE *file)
{
    fflush(file);
    int fd = fileno(file);
    fsync(fd);
}

int ch_mod(uint8_t *file){
    /* You're probably looking for ./xlib as android isn't working when this was written. */
    return -1;
}

int file_lock(FILE *file, uint64_t start, size_t length){
    // Unsupported on android
    return 0;
}

_Bool native_video_init(void *handle) { return 0; /* Unsupported on android */}
void native_video_close(void *handle) { /* Unsupported on android */ }
_Bool native_video_startread(void) { return 1; /* Unsupported on android */}
_Bool native_video_endread(void) { return 1; /* Unsupported on android */}
int native_video_getframe(uint8_t *y, uint8_t *u, uint8_t *v, uint16_t width, uint16_t height) { return 0; /* Unsupported on android */}
int file_unlock(FILE *file, uint64_t start, size_t length) { return 0; /* Unsupported on android */}

void setscale_fonts(void) {
    freefonts();
    loadfonts();
}


void setscale(void) {
    if(window) {
        svg_draw(0);
    }
    setscale_fonts();
}

void notify(char_t *title, uint16_t title_length, char_t *msg, uint16_t msg_length, FRIEND *f){ /* Unsupported on android */ }
void desktopgrab(_Bool video){ /* Unsupported on android */ }
void video_frame(uint32_t id, uint8_t *img_data, uint16_t width, uint16_t height, _Bool resize){ /* Unsupported on android */ }
void video_begin(uint32_t id, char_t *name, uint16_t name_length, uint16_t width, uint16_t height){ /* Unsupported on android */ }
void video_end(uint32_t id){ /* Unsupported on android */ }
uint16_t native_video_detect(void) { return 0; /* Unsupported on android */ }
_Bool video_init(void *handle) { return 0; /* Unsupported on android */  }
void video_close(void *handle){ /* Unsupported on android */ }
_Bool video_startread(void){ return 1; /* Unsupported on android */ }
_Bool video_endread(void){ return 1; /* Unsupported on android */ }
int video_getframe(uint8_t *y, uint8_t *u, uint8_t *v, uint16_t width, uint16_t height){ return 0; /* Unsupported on android */ }

/* gl initialization with EGL */
static _Bool init_display(void) {
    const EGLint attrib_list[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_NONE
    };

    EGLint numConfigs;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);

    EGLint format;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);

    ANativeWindow_setBuffersGeometry(window, 0, 0, format);
    surface = eglCreateWindowSurface(display, config, window, NULL);
    context = eglCreateContext(display, config, NULL, attrib_list);

    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
        return 0;
    }

    int32_t w, h;
    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);

    settings.window_width = w;
    settings.window_height = h;

    return gl_init();
}

static uint32_t getkeychar(int32_t key) /* get a character from an android keycode */{
    #define MAP(x,y) case AKEYCODE_##x: return y
    #define MAPS(x,y,z) case AKEYCODE_##x: return ((shift) ? z : y)
    #define MAPC(x) case AKEYCODE_##x: return (#x[0] + ((shift) ? 0 : ('a' - 'A')))
    #define MAPN(x,y) case AKEYCODE_##x: return ((shift) ? y : #x[0])

    switch(key) {
        MAP(ENTER, KEY_RETURN);
        MAP(DEL, KEY_BACK);
        MAP(DPAD_LEFT, KEY_LEFT);
        MAP(DPAD_RIGHT, KEY_RIGHT);
        MAP(DPAD_UP, KEY_UP);
        MAP(DPAD_DOWN, KEY_DOWN);

        MAP(SPACE, ' ');

        MAPS(MINUS, '-', '_');
        MAPS(EQUALS, '=', '+');
        MAPS(LEFT_BRACKET, '[', '{');
        MAPS(RIGHT_BRACKET, ']', '}');
        MAPS(BACKSLASH, '\\', '|');
        MAPS(SEMICOLON, ';', ':');
        MAPS(APOSTROPHE, '\'', '\"');
        MAPS(COMMA, ',', '<');
        MAPS(PERIOD, '.', '>');
        MAPS(SLASH, '/', '?');
        MAPS(GRAVE, '`', '~');

        MAP(AT, '@');
        MAP(STAR, '*');
        MAP(PLUS, '+');

        MAPC(A);
        MAPC(B);
        MAPC(C);
        MAPC(D);
        MAPC(E);
        MAPC(F);
        MAPC(G);
        MAPC(H);
        MAPC(I);
        MAPC(J);
        MAPC(K);
        MAPC(L);
        MAPC(M);
        MAPC(N);
        MAPC(O);
        MAPC(P);
        MAPC(Q);
        MAPC(R);
        MAPC(S);
        MAPC(T);
        MAPC(U);
        MAPC(V);
        MAPC(W);
        MAPC(X);
        MAPC(Y);
        MAPC(Z);
        MAPN(0,')');
        MAPN(1,'!');
        MAPN(2,'@');
        MAPN(3,'#');
        MAPN(4,'$');
        MAPN(5,'%');
        MAPN(6,'^');
        MAPN(7,'&');
        MAPN(8,'*');
        MAPN(9,'(');

    default: {
        debug("un-mapped %u", key);
        break;
    }
    }
    return 0;

    #undef MAP
    #undef MAPC
}

void redraw(void) {
    _redraw = 1;
}

void force_redraw(void) {
    redraw();
}

void update_tray(void) { /* Unsupported on android */ }

void config_osdefaults(UTOX_SAVE *r) { /* Unsupported on android */ }

void utox_android_redraw_window() {
    int32_t new_width, new_height;
    eglQuerySurface(display, surface, EGL_WIDTH, &new_width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &new_height);

    if(new_width != settings.window_width || new_height != settings.window_height) {
        settings.window_width  = new_width;
        settings.window_height = new_height;

        float vec[4];
        vec[0] = -(float)settings.window_width / 2.0;
        vec[1] = -(float)settings.window_height / 2.0;
        vec[2] = 2.0 / (float)settings.window_width;
        vec[3] = -2.0 / (float)settings.window_height;
        glUniform4fv(matrix, 1, vec);

        ui_size(settings.window_width, settings.window_height);

        glViewport(0, 0, settings.window_width, settings.window_height);

        _redraw = 1;
    }

    if(_redraw) {
        _redraw = 0;
        panel_draw(&panel_root, 0, 0, settings.window_width, settings.window_height);
    }
}

int lx = 0, ly = 0;
uint64_t p_last_down;
_Bool p_down, already_up;
static void utox_andoid_input (AInputQueue *in_queue, AInputEvent *event) {
    if (AInputQueue_preDispatchEvent(inputQueue, event) == 0) {
        int32_t handled = 1;

        int32_t type = AInputEvent_getType(event);
        if (type == AINPUT_EVENT_TYPE_MOTION) {
            int32_t action        = AMotionEvent_getAction(event);
            int32_t pointer_index = ((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            int32_t action_bits   = (action & AMOTION_EVENT_ACTION_MASK);

            float x = AMotionEvent_getX(event, pointer_index);
            float y = AMotionEvent_getY(event, pointer_index);

            switch (action_bits) {
                case AMOTION_EVENT_ACTION_DOWN:
                case AMOTION_EVENT_ACTION_POINTER_DOWN: {
                    lx = x;
                    ly = y;
                    panel_mmove(&panel_root, 0, 0, settings.window_width, settings.window_height, x, y, 0, 0);
                    panel_mdown(&panel_root);
                    //pointer[pointer_index].down = true;
                    //pointer[pointer_index].x = x;
                    //pointer[pointer_index].y = y;

                   // pointerinput2(pointer_index);

                    already_up = 0;
                    debug("down %f %f, %u\n", x, y, pointer_index);
                    p_down = 1;
                    p_last_down = get_time();
                    break;
                }

                case AMOTION_EVENT_ACTION_UP:
                case AMOTION_EVENT_ACTION_POINTER_UP: {
                    //panel_mmove(&panel_root, 0, 0, width, height, x, y, 0);
                    if (!already_up) {
                        panel_mup(&panel_root);
                        panel_mleave(&panel_root);
                    }
                    //pointer[pointer_index].down = false;
                    //pointer[pointer_index].x = x;
                    //pointer[pointer_index].y = y;

                    //pointerinput(pointer_index);

                    debug("up %f %f, %u\n", x, y, pointer_index);
                    p_down = 0;
                    break;
                }

                case AMOTION_EVENT_ACTION_MOVE: {
                    panel_mmove(&panel_root, 0, 0, settings.window_width, settings.window_height, x, y, x - lx, y - ly);
                    if (lx != (int)x || ly != (int)y) {
                        p_down = 0;
                        lx = x;
                        ly = y;
                        debug("move %f %f, %u\n", x, y, pointer_index);
                    }
                    //pointer[pointer_index].x = x;
                    //pointer[pointer_index].y = y;

                    break;
                }
            }
        } else if(type == AINPUT_EVENT_TYPE_KEY) {
            int32_t action = AMotionEvent_getAction(event);
            int32_t key = AKeyEvent_getKeyCode(event);

            if(action == AKEY_EVENT_ACTION_DOWN) {
                switch(key) {
                    case AKEYCODE_VOLUME_UP:
                    case AKEYCODE_VOLUME_DOWN: {
                        handled = 0;
                        break;
                    }

                    case AKEYCODE_MENU: {
                        //open menu
                        break;
                    }

                    case AKEYCODE_SHIFT_LEFT:
                    case AKEYCODE_SHIFT_RIGHT: {
                        shift = 1;
                        break;
                    }

                    case AKEYCODE_BACK: {
                        //ANativeActivity_finish(activity);
                        break;
                    }


                    default: {
                        uint32_t c = getkeychar(key);
                        if(c != 0) {
                            if(edit_active()) {
                                //debug("%u\n", c);
                                edit_char(c, 0, 0);
                            }
                            //inputchar(c);
                        }
                        break;
                    }
                }
            } else if(action == AKEY_EVENT_ACTION_UP) {
                if(key == AKEYCODE_SHIFT_LEFT || key == AKEYCODE_SHIFT_RIGHT) {
                    shift = 0;
                }
            }
        }
        AInputQueue_finishEvent(inputQueue, event, handled);
    }
}

static void android_main(struct android_app* state){
    bool theme_was_set_on_argv;
    int8_t should_launch_at_startup;
    int8_t set_show_window;
    bool no_updater;

    parse_args(NULL, NULL, &theme_was_set_on_argv, &should_launch_at_startup, &set_show_window, &no_updater);

    if (should_launch_at_startup == 1 || should_launch_at_startup == -1) {
        debug("Start on boot not supported on this OS!\n");
    }

    if (set_show_window == 1 || set_show_window == -1) {
        debug("Showing/hiding windows not supported on this OS!\n");
    }

    if (no_updater == true) {
        debug("Disabling the updater is not supported on this OS.\n");
    }

    // Make sure glue isn't stripped
    // ANativeActivity* nativeActivity = state->activity;
    // internalPath = nativeActivity->internalDataPath;

    pipe(pipefd);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    LANG = DEFAULT_LANG;
    dropdown_language.selected = dropdown_language.over = LANG;

    UTOX_SAVE *save = config_load();
    theme_load(THEME_DEFAULT);

    utox_init();

    thread(toxcore_thread, NULL);

    initfonts();

    dropdown_dpi.selected = dropdown_dpi.over = 20;
    ui_set_scale(26);

    while(!tox_thread_init) {
        yieldcpu(1);
    }
    /*
    createEngine();
    createAudioRecorder(global_av);
    startRecording();
    */

    /* Code has been changed, this probably should be moved! */
    list_start();

    while(!destroy) {
        if (p_down && (p_last_down + 500 * 1000 * 1000) < get_time()) {
            panel_mup(&panel_root);
            panel_mright(&panel_root);
            p_down = 0;
            already_up = 1;
        }

        inputQueue = (AInputQueue*)inputQueueNew;
        if(inputQueue != NULL) {
            AInputEvent *event = NULL;
            while(AInputQueue_hasEvents(inputQueue) && AInputQueue_getEvent(inputQueue, &event) >= 0) {
                utox_andoid_input(inputQueue, event);
            }
        }

        int rlen, len;
        PIPING piping;
        while((len = read(pipefd[0], (void*)&piping, sizeof(PIPING))) > 0) {
            debug("%u %u\n", len, sizeof(PIPING));
            while(len != sizeof(PIPING)) {
                if((rlen = read(pipefd[0], (void*)&piping + len, sizeof(PIPING) - len)) > 0) {
                    len += rlen;
                }
            }

            tox_message(piping.msg, piping.param1, piping.param2, piping.data);
        }

        ANativeWindow *win = (ANativeWindow*)windowN;
        if(win != window) { //new window
            if(window != NULL) {
                freefonts();
                //eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroyContext(display, context);
                eglDestroySurface(display, surface);
                eglTerminate(display);
            }

            window = win;

            if (window != NULL) {
                if(!init_display()) {
                    ANativeActivity_finish(activity);
                    break;
                }
            }
        }

        if(window != NULL && focused) {
            utox_android_redraw_window();
        }

        usleep(1000);
    }

    debug("ANDROID DESTROYED\n");
}

void showkeyboard(_Bool show) {
    JavaVM* vm = activity->vm;
    JNIEnv* env = activity->env;

    JavaVMAttachArgs lJavaVMAttachArgs;
    lJavaVMAttachArgs.version = JNI_VERSION_1_6;
    lJavaVMAttachArgs.name = "NativeThread";
    lJavaVMAttachArgs.group = NULL;

    (*vm)->AttachCurrentThread(vm, &env,&lJavaVMAttachArgs); //error check

    jobject lNativeActivity     = activity->clazz;
    jclass  ClassNativeActivity = (*env)->GetObjectClass(env, lNativeActivity);

    jclass   ClassInputMethodManager = (*env)->FindClass(env, "android/view/inputmethod/InputMethodManager");
    jfieldID fid                     = (*env)->GetFieldID(env, ClassNativeActivity, "mIMM", "Landroid/view/inputmethod/InputMethodManager;");
    jobject  lInputMethodManager     = (*env)->GetObjectField(env, lNativeActivity, fid);

    jmethodID MethodGetWindow    = (*env)->GetMethodID(env, ClassNativeActivity, "getWindow", "()Landroid/view/Window;");
    jobject   lWindow            = (*env)->CallObjectMethod(env, lNativeActivity, MethodGetWindow);
    jclass    ClassWindow        = (*env)->FindClass(env, "android/view/Window");
    jmethodID MethodGetDecorView = (*env)->GetMethodID(env, ClassWindow, "getDecorView", "()Landroid/view/View;");
    jobject   lDecorView         = (*env)->CallObjectMethod(env, lWindow, MethodGetDecorView);


    if (show) {
        jmethodID MethodShowSoftInput = (*env)->GetMethodID(env, ClassInputMethodManager, "showSoftInput", "(Landroid/view/View;I)Z");
        jboolean  lResult             = (*env)->CallBooleanMethod(env, lInputMethodManager, MethodShowSoftInput, lDecorView, 0);
        utox_android_redraw_window();
    } else {
        jclass    ClassView            = (*env)->FindClass(env, "android/view/View");
        jmethodID MethodGetWindowToken = (*env)->GetMethodID(env, ClassView, "getWindowToken", "()Landroid/os/IBinder;");
        jobject   lBinder              = (*env)->CallObjectMethod(env, lDecorView, MethodGetWindowToken);

        jmethodID MethodHideSoftInput = (*env)->GetMethodID(env, ClassInputMethodManager, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z");
        jboolean  lRes                = (*env)->CallBooleanMethod(env, lInputMethodManager, MethodHideSoftInput, lBinder, 0);
    }

    /*jmethodID MethodToggle = (*env)->GetMethodID(env, ClassInputMethodManager, "toggleSoftInput", "(II)V");
    (*env)->CallVoidMethod(env, lInputMethodManager, MethodToggle, 0, 0);*/

    (*vm)->DetachCurrentThread(vm);
}

static void onDestroy(ANativeActivity* act)
{
    destroy = 1;
}

static void onNativeWindowCreated(ANativeActivity* act, ANativeWindow* win)
{
    windowN = win;
}

static void onNativeWindowDestroyed(ANativeActivity* act, ANativeWindow* win)
{
    windowN = NULL;
}

static void onWindowFocusChanged(ANativeActivity* act, int focus)
{
    focused = (focus != 0);
}

static void onInputQueueCreated(ANativeActivity* act, AInputQueue* queue)
{
    inputQueueNew = queue;
}

static void onInputQueueDestroyed(ANativeActivity* act, AInputQueue* queue)
{
    inputQueueNew = NULL;
}

static void onContentRectChanged(ANativeActivity* activity, const ARect* r)
{
    rect = *r;
    debug("rect: %u %u %u %u\n", rect.left, rect.right, rect.top, rect.bottom);

    utox_window_baseline = rect.bottom;
    _redraw = 1;
}

__attribute__ ((externally_visible)) void ANativeActivity_onCreate(ANativeActivity* act, void* savedState, size_t savedStateSize)
{
    if(!act) {
        return;
    }
    activity = act;

    //Add callbacks here (find them in android/native_activity.h)
    act->callbacks->onDestroy = onDestroy;
    act->callbacks->onNativeWindowCreated = onNativeWindowCreated;
    act->callbacks->onNativeWindowDestroyed = onNativeWindowDestroyed;
    act->callbacks->onWindowFocusChanged = onWindowFocusChanged;
    act->callbacks->onInputQueueCreated = onInputQueueCreated;
    act->callbacks->onInputQueueDestroyed = onInputQueueDestroyed;
    act->callbacks->onContentRectChanged = onContentRectChanged;

    //start main thread (android_main)
    pthread_t thread;
    pthread_attr_t myattr;
    pthread_attr_init(&myattr);
    pthread_attr_setdetachstate(&myattr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &myattr, (void*(*)(void*))android_main, NULL);
}

void launch_at_startup(int is_launch_at_startup)
{
}
