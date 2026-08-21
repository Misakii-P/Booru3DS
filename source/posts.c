#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "posts.h"

Post posts[MAX_POSTS];
int post_count = 0;

static int jget(const char *obj, const char *key, char *out, int outsz)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(obj, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    while (*p == ' ')
        p++;
    int n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n < outsz - 1) {
            if (*p == '\\' && p[1])
                p++;
            out[n++] = *p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && n < outsz - 1)
            out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

int parse_posts(char *json)
{
    post_count = 0;
    const char *p = json;
    static char obj[4096];

    while (*p && post_count < MAX_POSTS) {
        const char *ob = strchr(p, '{');
        if (!ob)
            break;
        const char *cb = strchr(ob, '}');
        if (!cb)
            break;

        int len = cb - ob + 1;
        if (len > (int)sizeof(obj) - 1)
            len = sizeof(obj) - 1;
        memcpy(obj, ob, len);
        obj[len] = 0;

        Post *post = &posts[post_count];
        memset(post, 0, sizeof(*post));

        char val[192];
        if (jget(obj, "id", val, sizeof(val)))
            post->id = (unsigned int)strtoul(val, NULL, 10);
        jget(obj, "directory", post->directory, sizeof(post->directory));
        jget(obj, "image", post->image, sizeof(post->image));
        jget(obj, "preview_url", post->preview, sizeof(post->preview));
        jget(obj, "sample_url", post->sample, sizeof(post->sample));
        jget(obj, "file_url", post->file, sizeof(post->file));
        if (!jget(obj, "tags", post->tags, sizeof(post->tags)))
            post->tags[0] = 0;

        if (post->image[0] || post->id)
            post_count++;

        p = cb + 1;
    }
    return post_count;
}
