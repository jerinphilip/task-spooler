/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <pwd.h>

#include "main.h"


char *joblistdump_headers() {
    char *line;

    line = malloc(600);
    snprintf(line, 600, "#!/bin/sh\n# - task spooler (ts) job dump\n"
                        "# This file has been created because a SIGTERM killed\n"
                        "# your queue server.\n"
                        "# The finished commands are listed first.\n"
                        "# The commands running or to be run are stored as you would\n"
                        "# probably run them. Take care - some quotes may have got"
                        " broken\n\n");

    return line;
}

char *joblist_headers(int max_slots, int busy_slots) {
    char *line;

    line = malloc(120);
    snprintf(line, 120, "%-4s %-15s %-10s %-20s %-8s %-6s %-5s %s [run=%i/%i]\n",
             "ID",
             "User",
             "State",
             "Output",
             "E-Level",
             "Time",
             "GPUs",
             "Command",
             busy_slots,
             max_slots);

    return line;
}

static int max(int a, int b) {
    return a > b ? a : b;
}

static const char *ofilename_shown(const struct Job *p) {
    const char *output_filename;

    if (p->state == SKIPPED) {
        output_filename = "(no output)";
    } else if (p->store_output) {
        if (p->state == QUEUED || p->state == ALLOCATING) {
            output_filename = "(file)";
        } else {
            if (p->output_filename == 0)
                /* This may happen due to concurrency
                 * problems */
                output_filename = "(...)";
            else
                output_filename = p->output_filename;
        }
    } else
        output_filename = "stdout";

    return output_filename;
}

static char *shorten(char *line, int len) {
    if (strlen(line) <= len)
        return line;
    else {
        char *newline = (char *) malloc((len + 1) * sizeof(char));
        snprintf(newline, len - 4, "%s", line);
        sprintf(newline, "%s...", newline);
        return newline;
    }
}

static char *print_noresult(const struct Job *p) {
    const char *jobstate;
    const char *output_filename;
    int maxlen;
    char *line;
    /* 20 chars should suffice for a string like "[int,int,..]&& " */
    char dependstr[20] = "";
    int cmd_len;
    struct passwd *pwd;
    if ((pwd = getpwuid((uid_t) p->uid)) == NULL)
        error("Cannot get username from uid");

    jobstate = jstate2string(p->state);
    output_filename = ofilename_shown(p);

    maxlen = 4 + 1 + 15 + 1 + 10 + 1 + max(20, strlen(output_filename)) + 1 + 8 + 1
             + 25 + 1 + 5 + 1 + strlen(p->command) + 20; /* 20 is the margin for errors */

    if (p->label)
        maxlen += 3 + strlen(p->label);
    if (p->do_depend) {
        maxlen += sizeof(dependstr);
        int pos = 0;
        if (p->depend_on[0] == -1)
            pos += snprintf(&dependstr[pos], sizeof(dependstr), "[ ");
        else
            pos += snprintf(&dependstr[pos], sizeof(dependstr), "[%i", p->depend_on[0]);

        for (int i = 1; i < p->depend_on_size; i++) {
            if (p->depend_on[i] == -1)
                pos += snprintf(&dependstr[pos], sizeof(dependstr), ", ");
            else
                pos += snprintf(&dependstr[pos], sizeof(dependstr), ",%i", p->depend_on[i]);
        }
        pos += snprintf(&dependstr[pos], sizeof(dependstr), "]&& ");
    }

    line = (char *) malloc(maxlen);
    if (line == NULL)
        error("Malloc for %i failed.\n", maxlen);

    cmd_len = max((strlen(p->command) + (term_width - maxlen)), 20);
    if (p->label)
        snprintf(line, maxlen, "%-4i %-15s %-10s %-20s %-8s %6s %-5d %s[%s]%s\n",
                 p->jobid,
                 pwd->pw_name,
                 jobstate,
                 output_filename,
                 "",
                 "",
                 p->num_gpus,
                 dependstr,
                 shorten(p->label, 20),
                 shorten(p->command, cmd_len));
    else
        snprintf(line, maxlen, "%-4i %-15s %-10s %-20s %-8s %6s %-5d %s%s\n",
                 p->jobid,
                 pwd->pw_name,
                 jobstate,
                 output_filename,
                 "",
                 "",
                 p->num_gpus,
                 dependstr,
                 shorten(p->command, cmd_len));

    return line;
}

static char *print_result(const struct Job *p) {
    const char *jobstate;
    int maxlen;
    char *line;
    const char *output_filename;
    /* 20 chars should suffice for a string like "[int,int,..]&& " */
    char dependstr[20] = "";
    float real_ms = p->result.real_ms;
    char *unit = "s";
    int cmd_len;
    struct passwd *pwd;
    if ((pwd = getpwuid((uid_t) p->uid)) == NULL)
        error("Cannot get username from uid");

    jobstate = jstate2string(p->state);
    output_filename = ofilename_shown(p);

    maxlen = 4 + 1 + 15 + 1 + 10 + 1 + max(20, strlen(output_filename)) + 1 + 8 + 1
             + 25 + 1 + 5 + 1 + strlen(p->command) + 20; /* 20 is the margin for errors */

    if (p->label)
        maxlen += 3 + strlen(p->label);
    if (p->do_depend) {
        maxlen += sizeof(dependstr);
        int pos = 0;
        if (p->depend_on[0] == -1)
            pos += snprintf(&dependstr[pos], sizeof(dependstr), "[ ");
        else
            pos += snprintf(&dependstr[pos], sizeof(dependstr), "[%i", p->depend_on[0]);

        for (int i = 1; i < p->depend_on_size; i++) {
            if (p->depend_on[i] == -1)
                pos += snprintf(&dependstr[pos], sizeof(dependstr), ", ");
            else
                pos += snprintf(&dependstr[pos], sizeof(dependstr), ",%i", p->depend_on[i]);
        }
        pos += snprintf(&dependstr[pos], sizeof(dependstr), "]&& ");
    }

    line = (char *) malloc(maxlen);
    if (line == NULL)
        error("Malloc for %i failed.\n", maxlen);

    if (real_ms > 60) {
        real_ms /= 60;
        unit = "m";

        if (real_ms > 60) {
            real_ms /= 60;
            unit = "h";

            if (real_ms > 24) {
                real_ms /= 24;
                unit = "d";
            }
        }
    }

    cmd_len = max((strlen(p->command) + (term_width - maxlen)), 20);
    if (p->label)
        snprintf(line, maxlen, "%-4i %-15s %-10s %-20s %-8i %5.2f%s %-5d %s[%s]%s\n",
                 p->jobid,
                 pwd->pw_name,
                 jobstate,
                 output_filename,
                 p->result.errorlevel,
                 real_ms,
                 unit,
                 p->num_gpus,
                 dependstr,
                 shorten(p->label, 20),
                 shorten(p->command, cmd_len));
    else
        snprintf(line, maxlen, "%-4i %-15s %-10s %-20s %-8i %5.2f%s %-5d %s%s\n",
                 p->jobid,
                 pwd->pw_name,
                 jobstate,
                 output_filename,
                 p->result.errorlevel,
                 real_ms,
                 unit,
                 p->num_gpus,
                 dependstr,
                 shorten(p->command, cmd_len));

    return line;
}

char *joblist_line(const struct Job *p) {
    char *line;

    if (p->state == FINISHED)
        line = print_result(p);
    else
        line = print_noresult(p);

    return line;
}

char *joblistdump_torun(const struct Job *p) {
    int maxlen;
    char *line;

    maxlen = 10 + strlen(p->command) + 20; /* 20 is the margin for errors */

    line = (char *) malloc(maxlen);
    if (line == NULL)
        error("Malloc for %i failed.\n", maxlen);

    snprintf(line, maxlen, "ts %s\n", p->command);

    return line;
}
