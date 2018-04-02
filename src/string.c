#include "string.h"

// You must free the result if result is non-NULL.
char *str_replace(const char *subject, const char *search, const char *replace) {
    char *result; // the return string
    const char *ins;    // the next insert point
    char *tmp;    // varies
    int len_rep;  // length of rep (the string to remove)
    int len_with; // length of with (the string to replace rep with)
    int len_front; // distance between rep and end of last rep
    int count;    // number of replacements

                  // sanity checks and initialization
    if (!subject || !search) {
        return NULL;
    }
    len_rep = strlen(search);
    if (len_rep == 0) {
        return NULL; // empty rep causes infinite loop during count
    }
    if (!replace) {
        replace = "";
    }
    len_with = strlen(replace);

    // count the number of replacements needed
    ins = subject;
    for (count = 0; (tmp = strstr(ins, search)); ++count) {
        ins = tmp + len_rep;
    }

    tmp = result = malloc(strlen(subject) + (len_with - len_rep) * count + 1);

    if (! result) {
        return NULL;
    }

    // first time through the loop, all the variable are set correctly
    // from here on,
    //    tmp points to the end of the result string
    //    ins points to the next occurrence of rep in orig
    //    orig points to the remainder of orig after "end of rep"
    while (count--) {
        ins = strstr(subject, search);
        len_front = ins - subject;
        tmp = strncpy(tmp, subject, len_front) + len_front;
        tmp = strcpy(tmp, replace) + len_with;
        subject += len_front + len_rep; // move to next "end of rep"
    }

    strcpy(tmp, subject);

    return result;
}
