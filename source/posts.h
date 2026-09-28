#pragma once
#include <stddef.h>
#include <stdbool.h>

/* both APIs cap limit= at 100 per request */
#define MAX_POSTS 100

typedef struct {
    unsigned int id;
    /* safebooru sets sample:false for ~2/3 of posts yet still fills
       sample_url - with a link to the FULL original. Trusting sample_url
       unconditionally means downloading multi-megabyte originals to show
       on a 400x240 screen, which is what made the top view fail with
       "http 200" and starved the save path of heap. Konachan has no
       sample key at all, so default to true and only safebooru demotes. */
    bool sample_ok;
    char directory[64];
    char image[128];
    char preview[256];
    char sample[256];
    char file[256];
} Post;

extern Post posts[MAX_POSTS];
extern int post_count;

int parse_posts(char *json);
