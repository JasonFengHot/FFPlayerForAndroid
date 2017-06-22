/*
    SDL_android_main.c, placed in the public domain by Sam Lantinga  3/13/14
*/

#ifdef __ANDROID__

/* Include the SDL main definition header */
#include "SDL_main.h"

/*******************************************************************************
                 Functions called by JNI
*******************************************************************************/
#include <jni.h>

#include "SDL.h"
#include "ffplayer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called before SDL_main() to initialize JNI bindings in SDL library */
extern void SDL_Android_Init(JNIEnv *env, jclass cls);

/* This prototype is needed to prevent a warning about the missing prototype for global function below */
JNIEXPORT int JNICALL
Java_org_libsdl_app_SDLActivity_nativeInit(JNIEnv *env, jclass cls, jobjectArray array);

typedef struct Sprite {
    SDL_Texture *texture;
    Uint16 w;
    Uint16 h;
} Sprite;

/* Adapted from SDL's testspriteminimal.c */
Sprite LoadSprite(const char *file, SDL_Renderer *renderer) {
    Sprite result;
    result.texture = NULL;
    result.w = 0;
    result.h = 0;

    SDL_Surface *temp;

    /* Load the sprite image */
    temp = SDL_LoadBMP(file);
    if (temp == NULL) {
        fprintf(stderr, "Couldn't load %s: %s\n", file, SDL_GetError());
        return result;
    }
    result.w = temp->w;
    result.h = temp->h;

    /* Create texture from the image */
    result.texture = SDL_CreateTextureFromSurface(renderer, temp);
    if (!result.texture) {
        fprintf(stderr, "Couldn't create texture: %s\n", SDL_GetError());
        SDL_FreeSurface(temp);
        return result;
    }
    SDL_FreeSurface(temp);

    return result;
}

void draw(SDL_Window *window, SDL_Renderer *renderer, const Sprite sprite) {
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    SDL_Rect destRect = {w / 2 - sprite.w / 2, h / 2 - sprite.h / 2, sprite.w, sprite.h};
    /* Blit the sprite onto the screen */
    SDL_RenderCopy(renderer, sprite.texture, NULL, &destRect);
}


int SDL_main(int argc, char *argv[]) {

    SDL_Window *window;
    SDL_Renderer *renderer;

    if (SDL_CreateWindowAndRenderer(0, 0, 0, &window, &renderer) < 0)
        exit(2);

    Sprite sprite = LoadSprite("/sdcard/image.bmp", renderer);
    if (sprite.texture == NULL)
        exit(2);

    /* Main render loop */
    Uint8 done = 0;
    SDL_Event event;
    while (!done) {
        /* Check for events */
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || event.type == SDL_KEYDOWN ||
                event.type == SDL_FINGERDOWN) {
                done = 1;
            }
        }


        /* Draw a gray background */
        SDL_SetRenderDrawColor(renderer, 0xA0, 0xA0, 0xA0, 0xFF);
        SDL_RenderClear(renderer);

        draw(window, renderer, sprite);

        /* Update the screen! */
        SDL_RenderPresent(renderer);

        SDL_Delay(1000000);
    }

    exit(0);


    // A dummy main that is necessary to compile SDL2 statically.
    return 0;
}

/* Start up the SDL app */
JNIEXPORT int JNICALL
Java_org_libsdl_app_SDLActivity_nativeInit(JNIEnv *env, jclass cls, jobjectArray array) {
    int i;
    int argc;
    int status;
    int len;
    char **argv;

    /* This interface could expand with ABI negotiation, callbacks, etc. */
    SDL_Android_Init(env, cls);

    SDL_SetMainReady();

    /* Prepare the arguments. */

    len = (env)->GetArrayLength(array);
    argv = SDL_stack_alloc(char*, 1 + len + 1);
    argc = 0;
    /* Use the name "app_process" so PHYSFS_platformCalcBaseDir() works.
       https://bitbucket.org/MartinFelis/love-android-sdl2/issue/23/release-build-crash-on-start
     */
    argv[argc++] = SDL_strdup("app_process");
    for (i = 0; i < len; ++i) {
        const char *utf;
        char *arg = NULL;
        jstring string = (jstring) (env)->GetObjectArrayElement(array, i);
        if (string) {
            utf = (env)->GetStringUTFChars(string, 0);
            if (utf) {
                arg = SDL_strdup(utf);
                (env)->ReleaseStringUTFChars(string, utf);
            }
            (env)->DeleteLocalRef(string);
        }
        if (!arg) {
            arg = SDL_strdup("");
        }
        argv[argc++] = arg;
    }
    argv[argc] = NULL;


    /* Run the application. */

//    status = SDL_main(argc, argv);

    char *path1 = "/sdcard/Daisy/chinesemovie_0.mp4";
    char *path2 = "http://114.80.117.173/qyrrs?url=http%3A%2F%2Fvdata.tvxio.com%2Fcdn%2F0%2F1f%2F20170323%2Fdf845611c7418f9c80a999eee6e8ae%2FH264%2Fhigh%2Fslice%2F14858_1.m3u8&quality=high&sn=go_grf7qt3h&clipid=1785703&sid=a4d3146858734a9c8fe35625e515f3723&sign=abfda0ef5177de62bbe610935e7147d3&ne=1&clip=1";
    ffplay(path2);

    /* Release the arguments. */

    for (i = 0; i < argc; ++i) {
        SDL_free(argv[i]);
    }
    SDL_stack_free(argv);
    /* Do not issue an exit or the whole application will terminate instead of just the SDL thread */
    /* exit(status); */

    return status;
}

#ifdef __cplusplus
}
#endif
#endif /* __ANDROID__ */

/* vi: set ts=4 sw=4 expandtab: */
