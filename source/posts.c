#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "posts.h"

Post posts[MAX_POSTS];
int post_count = 0;

/* ------------------------------------------------------------------ */
/* minimal string-aware JSON scanner                                  */
/*                                                                     */
/* Both APIs return brace-delimited objects, so we do not need a full   */
/* parser - but we do need to know which braces live inside string      */
/* literals. Tag lists routinely contain '{' and '}', and cutting an    */
/* object short at the first one silently drops every field after it.   */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Copy the string literal starting at p (which must point at '"') into
   out, unescaping the handful of escapes these APIs emit. Returns a
   pointer just past the closing quote. Pass out == NULL to skip. */
static const char *jstr(const char *p, char *out, int outsz)
{
    if (*p != '"') {
        if (out && outsz > 0)
            out[0] = 0;
        return p;
    }
    p++;
    int n = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' && p[1]) {
            p++;
            if (*p == 'n') c = '\n';
            else if (*p == 't') c = '\t';
            else if (*p == 'r') c = '\r';
            else c = (unsigned char)*p;
        }
        if (out && n < outsz - 1)
            out[n++] = (char)c;
        p++;
    }
    if (out && outsz > 0)
        out[n] = 0;
    if (*p == '"')
        p++;
    return p;
}

/* Advance past one JSON value of any type. */
static const char *jskip(const char *p)
{
    p = skip_ws(p);
    if (*p == '"')
        return jstr(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = jstr(p, NULL, 0);
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0)
                    return p + 1;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ')
        p++;
    return p;
}

/* Copy one JSON value into out as text, whether or not it is quoted.
   These APIs are not consistent: safebooru returns "directory":3921 as a
   bare number while konachan omits the field entirely. Accepting only
   quoted strings would leave the cursor parked on a number, and the next
   key check would then abort the whole object. */
static const char *jval(const char *p, char *out, int outsz)
{
    p = skip_ws(p);
    if (*p == '"')
        return jstr(p, out, outsz);
    if (!out || outsz <= 0)
        return jskip(p);
    int n = 0;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\n' && *p != '\r') {
        if (n < outsz - 1)
            out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return p;
}

/* JSON boolean. Both providers send it bare, but accept a quoted form
   too rather than silently treating "true" as false. */
static const char *jbool(const char *p, bool *out)
{
    char tmp[8];
    p = jval(p, tmp, sizeof(tmp));
    *out = (tmp[0] == 't' || tmp[0] == 'T' || tmp[0] == '1');
    return p;
}

/* id field: quoted on some providers, bare on others */
static const char *juint(const char *p, unsigned int *out)
{
    char tmp[24];
    p = jval(p, tmp, sizeof(tmp));
    *out = (unsigned int)strtoul(tmp, NULL, 10);
    return p;
}

/* Parse the object at ob into post. Returns a pointer past its closing
   brace, or ob itself if there is no object there. */
static const char *parse_object(const char *ob, Post *post)
{
    memset(post, 0, sizeof(*post));
    post->sample_ok = true; /* only an explicit false demotes it */
    if (*ob != '{')
        return ob;
    ob++;

    const char *p = ob;
    for (;;) {
        p = skip_ws(p);
        if (*p == '}')
            return p + 1;
        if (!*p)
            return p;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"')
            return p; /* malformed: stop before we loop forever */

        char key[48];
        p = jstr(p, key, sizeof(key));
        p = skip_ws(p);
        if (*p != ':')
            return p;
        p++;          /* step over the ':' onto the value */
        p = skip_ws(p);

        if (!strcmp(key, "id")) p = juint(p, &post->id);
        else if (!strcmp(key, "sample")) p = jbool(p, &post->sample_ok);
        else if (!strcmp(key, "has_sample")) p = jbool(p, &post->sample_ok);
        else if (!strcmp(key, "directory")) p = jval(p, post->directory, sizeof(post->directory));
        else if (!strcmp(key, "image")) p = jval(p, post->image, sizeof(post->image));
        else if (!strcmp(key, "preview_url")) p = jval(p, post->preview, sizeof(post->preview));
        else if (!strcmp(key, "sample_url")) p = jval(p, post->sample, sizeof(post->sample));
        else if (!strcmp(key, "file_url")) p = jval(p, post->file, sizeof(post->file));
        else p = jskip(p);
    }
}

int parse_posts(char *json)
{
    post_count = 0;
    const char *p = json;
    Post tmp;

    while (*p && post_count < MAX_POSTS) {
        const char *ob = strchr(p, '{');
        if (!ob)
            break;

        const char *end = parse_object(ob, &tmp);
        if (end <= ob) { /* unreadable: step past this brace and retry */
            p = ob + 1;
            continue;
        }

        if (tmp.image[0] || tmp.id) {
            posts[post_count++] = tmp;
            p = end; /* consumed */
        } else {
            /* No image and no id, so this is a container rather than a
               post - the API envelope the results are nested in. Keep
               looking from just inside it instead of skipping past the
               whole thing, or every post would be missed. */
            p = ob + 1;
        }
    }
    return post_count;
}
