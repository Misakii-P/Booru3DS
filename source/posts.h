#pragma once
#include <stddef.h>

/* both APIs cap limit= at 100 per request */
#define MAX_POSTS 100

typedef struct {
    unsigned int id;
    char directory[64];
    char image[128];
    char tags[192];
    char preview[256];
    char sample[256];
    char file[256];
} Post;

extern Post posts[MAX_POSTS];
extern int post_count;

int parse_posts(char *json);
