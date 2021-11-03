/*
    Task Spooler - a task queue system for the unix user
    Copyright (C) 2007-2009  Lluís Batlle i Rossell

    Please find the license in the provided COPYING file.
*/
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <signal.h>
#include <time.h>

#include "main.h"

static void c_end_of_job(const struct Result *res);

static void c_wait_job_send();

static void c_wait_running_job_send();

static void shuffle(int *array, size_t n);

static struct Msg initialize_message();

static void require_root();

char *build_command_string() {
    int size;
    int i;
    int num;
    char **array;
    char *commandstring;

    size = 0;
    num = command_line.command.num;
    array = command_line.command.array;

    /* Count bytes needed */
    for (i = 0; i < num; ++i) {
        /* The '1' is for spaces, and at the last i,
         * for the null character */
        size = size + strlen(array[i]) + 1;
    }

    /* Alloc */
    commandstring = (char *) malloc(size);
    if (commandstring == NULL)
        error("Error in malloc for commandstring");

    /* Build the command */
    strcpy(commandstring, array[0]);
    for (i = 1; i < num; ++i) {
        strcat(commandstring, " ");
        strcat(commandstring, array[i]);
    }

    return commandstring;
}

void c_new_job() {
    struct Msg m = initialize_message();
    char *new_command;
    char *myenv;

    m.type = NEWJOB;

    new_command = build_command_string();

    myenv = get_environment();

    /* global */
    m.u.newjob.command_size = strlen(new_command) + 1; /* add null */
    if (myenv)
        m.u.newjob.env_size = strlen(myenv) + 1; /* add null */
    else
        m.u.newjob.env_size = 0;
    if (command_line.label)
        m.u.newjob.label_size = strlen(command_line.label) + 1; /* add null */
    else
        m.u.newjob.label_size = 0;
    m.u.newjob.store_output = command_line.store_output;
    m.u.newjob.do_depend = command_line.do_depend;
    m.u.newjob.should_keep_finished = command_line.should_keep_finished;
    m.u.newjob.command_size = strlen(new_command) + 1; /* add null */
    m.u.newjob.wait_enqueuing = command_line.wait_enqueuing;
    m.u.newjob.num_slots = command_line.num_slots;
    m.u.newjob.gpus = command_line.gpus;
    m.u.newjob.wait_free_gpus = command_line.wait_free_gpus;

    /* Send the message */
    send_msg(server_socket, &m);

    /* send dependencies */
    if (command_line.do_depend)
        send_ints(server_socket, command_line.depend_on, command_line.depend_on_size);

    /* Send the command */
    send_bytes(server_socket, new_command, m.u.newjob.command_size);

    /* Send the label */
    send_bytes(server_socket, command_line.label, m.u.newjob.label_size);

    /* Send the environment */
    send_bytes(server_socket, myenv, m.u.newjob.env_size);

    free(new_command);
    free(myenv);
}

int c_wait_newjob_ok() {
    struct Msg m = initialize_message();
    int res;

    res = recv_msg(server_socket, &m);
    if (res == -1)
        error("Error in wait_newjob_ok");
    if (m.type == NEWJOB_NOK) {
        fprintf(stderr, "Error, queue full\n");
        exit(EXITCODE_QUEUE_FULL);
    }
    if (m.type != NEWJOB_OK)
        error("Error getting the newjob_ok");

    return m.u.jobid;
}

int c_wait_server_commands() {
    struct Msg m = initialize_message();
    int res;

    while (1) {
        res = recv_msg(server_socket, &m);
        if (res == -1)
            error("Error in wait_server_commands");

        if (res == 0)
            break;
        if (res != sizeof(m))
            error("Error in wait_server_commands");
        if (m.type == RUNJOB) {
            struct Result result;
            result.skipped = 0;
            if (command_line.do_depend && command_line.require_elevel && m.u.last_errorlevel != 0) {
                result.errorlevel = -1;
                result.user_ms = 0.;
                result.system_ms = 0.;
                result.real_ms = 0.;
                result.skipped = 1;
                c_send_runjob_ok(0, -1);
            } else {
                if (command_line.gpus) {
                    if (command_line.gpu_nums) {
                        char tmp[50];
                        strcpy(tmp, "CUDA_VISIBLE_DEVICES=");
                        strcat(tmp, command_line.gpu_nums);
                        putenv(tmp);
                    } else {
                        int numFree;
                        int *freeGpuList = getFreeGpuList(&numFree);
                        if ((command_line.gpus > numFree)) {
                            result.errorlevel = -1;
                            result.user_ms = 0.;
                            result.system_ms = 0.;
                            result.real_ms = 0.;
                            result.skipped = 1;
                            c_send_runjob_ok(0, -1);
                        } else {
                            char tmp[50];
                            strcpy(tmp, "CUDA_VISIBLE_DEVICES=");
                            shuffle(freeGpuList, numFree);
                            for (int i = 0; i < command_line.gpus; i++) {
                                char tmp2[5];
                                sprintf(tmp2, "%d", freeGpuList[i]);
                                strcat(tmp, tmp2);
                                if (i < command_line.gpus - 1)
                                    strcat(tmp, ",");
                            }
                            putenv(tmp);
                        }
                        free(freeGpuList);
                    }
                } else {
                    putenv("CUDA_VISIBLE_DEVICES=-1");
                }

                run_job(&result);
            }

            c_end_of_job(&result);
            return result.errorlevel;
        } else if (m.type == REMINDER) {
            sleep(m.u.gpu_wait_time);
            c_send_reminder();
        }
    }
    return -1;
}

void c_wait_server_lines() {
    struct Msg m = initialize_message();
    int res;

    while (1) {
        res = recv_msg(server_socket, &m);
        if (res == -1)
            error("Error in wait_server_lines");

        if (res == 0)
            break;
        if (res != sizeof(m))
            error("Error in wait_server_lines 2");
        if (m.type == LIST_LINE) {
            char *buffer;
            buffer = (char *) malloc(m.u.size);
            recv_bytes(server_socket, buffer, m.u.size);
            printf("%s", buffer);
            free(buffer);
        }
    }
}

void c_list_jobs() {
    struct Msg m = initialize_message();

    m.type = LIST;
    m.u.term_width = term_width;
    send_msg(server_socket, &m);
}

/* Exits if wrong */
void c_check_version() {
    struct Msg m = initialize_message();
    int res;

    m.type = GET_VERSION;
    /* Double send, so an old ts will answer for sure at least once */
    send_msg(server_socket, &m);
    send_msg(server_socket, &m);

    /* Set up a 2 second timeout to receive the
    version msg. */

    res = recv_msg(server_socket, &m);
    if (res == -1)
        error("Error calling recv_msg in c_check_version");
    if (m.type != VERSION || m.u.version != PROTOCOL_VERSION) {
        printf("Wrong server version. Received %i, expecting %i\n",
               m.u.version, PROTOCOL_VERSION);

        error("Wrong server version. Received %i, expecting %i",
              m.u.version, PROTOCOL_VERSION);
    }

    /* Receive also the 2nd send_msg if we got the right version */
    res = recv_msg(server_socket, &m);
    if (res == -1)
        error("Error calling the 2nd recv_msg in c_check_version");
}

void c_show_info() {
    struct Msg m = initialize_message();
    int res;

    m.type = INFO;
    m.u.jobid = command_line.jobid;

    send_msg(server_socket, &m);

    while (1) {
        res = recv_msg(server_socket, &m);
        if (res == -1)
            error("Error in wait_server_lines");

        if (res == 0)
            break;
        if (res != sizeof(m))
            error("Error in wait_server_lines 2");
        if (m.type == INFO_DATA) {
            char *buffer;
            enum {
                DSIZE = 1000
            };

            /* We're going to output data using the stdout fd */
            fflush(stdout);
            buffer = (char *) malloc(DSIZE);
            do {
                res = recv(server_socket, buffer, DSIZE, 0);
                if (res > 0)
                    write(1, buffer, res);
            } while (res > 0);
            free(buffer);
        }
    }
}

void c_show_last_id() {
    struct Msg m = initialize_message();
    int res;

    m.type = LAST_ID;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_output_file");

    switch (m.type) {
        case LAST_ID:
            printf("%d\n", m.u.jobid);
        default:
            warning("Wrong internal message in get_output_file line size");
    }
}

void c_send_runjob_ok(const char *ofname, int pid) {
    struct Msg m = initialize_message();

    /* Prepare the message */
    m.type = RUNJOB_OK;
    if (ofname) /* ofname == 0, skipped execution */
        m.u.output.store_output = command_line.store_output;
    else
        m.u.output.store_output = 0;
    m.u.output.pid = pid;
    if (m.u.output.store_output)
        m.u.output.ofilename_size = strlen(ofname) + 1;
    else
        m.u.output.ofilename_size = 0;

    send_msg(server_socket, &m);

    /* Send the filename */
    if (command_line.store_output)
        send_bytes(server_socket, ofname, m.u.output.ofilename_size);
}

static void c_end_of_job(const struct Result *res) {
    struct Msg m = initialize_message();

    m.type = ENDJOB;
    m.u.result = *res; /* struct copy */

    send_msg(server_socket, &m);
}

void c_shutdown_server() {
    require_root();
    struct Msg m = initialize_message();

    m.type = KILL_SERVER;
    send_msg(server_socket, &m);
}

void c_clear_finished() {
    struct Msg m = initialize_message();

    m.type = CLEAR_FINISHED;
    send_msg(server_socket, &m);
}

static char *get_output_file(int *pid) {
    struct Msg m = initialize_message();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = ASK_OUTPUT;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_output_file");
    switch (m.type) {
        case ANSWER_OUTPUT:
            if (m.u.output.store_output) {
                /* Receive the output file name */
                string = 0;
                if (m.u.output.ofilename_size > 0) {
                    string = (char *) malloc(m.u.output.ofilename_size);
                    recv_bytes(server_socket, string, m.u.output.ofilename_size);
                }
                *pid = m.u.output.pid;
                return string;
            }
            *pid = m.u.output.pid;
            return 0;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in get_output_file line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in get_output_file line size");
    }
    /* This will never be reached */
    return 0;
}

int c_tail() {
    char *str;
    int pid;
    str = get_output_file(&pid);
    if (str == 0) {
        fprintf(stderr, "The output is not stored. Cannot tail.\n");
        exit(-1);
    }

    c_wait_running_job_send();

    return tail_file(str, 10 /* Last lines to show */);
}

int c_cat() {
    char *str;
    int pid;
    str = get_output_file(&pid);
    if (str == 0) {
        fprintf(stderr, "The output is not stored. Cannot cat.\n");
        exit(-1);
    }
    c_wait_running_job_send();

    return tail_file(str, -1 /* All the lines */);
}

void c_show_output_file() {
    char *str;
    int pid;
    /* This will exit if there is any error */
    str = get_output_file(&pid);
    if (str == 0) {
        fprintf(stderr, "The output is not stored.\n");
        exit(-1);
    }
    printf("%s\n", str);
    free(str);
}

void c_show_pid() {
    int pid;
    /* This will exit if there is any error */
    get_output_file(&pid);
    printf("%i\n", pid);
}

void c_kill_job() {
    int pid = 0;
    /* This will exit if there is any error */
    get_output_file(&pid);

    if (pid == -1 || pid == 0) {
        fprintf(stderr, "Error: strange PID received: %i\n", pid);
        exit(-1);
    }

    /* Send SIGTERM to the process group, as pid is for process group */
    kill(-pid, SIGTERM);
}

void c_kill_all_jobs() {
    struct Msg m = initialize_message();
    int res;

    /* Send the request */
    m.type = KILL_ALL;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in kill_all");
    switch (m.type) {
        case COUNT_RUNNING:
            for (int i = 0; i < m.u.count_running; ++i) {
                int pid;
                res = recv(server_socket, &pid, sizeof(int), 0);
                if (res != sizeof(int))
                    error("Error in receiving PID kill_all");
                kill(-pid, SIGTERM);
            }
            return;
        default:
            warning("Wrong internal message in kill_all");
    }
}

void c_remove_job() {
    struct Msg m = initialize_message();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = REMOVEJOB;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in remove_job");
    switch (m.type) {
        case REMOVEJOB_OK:
            return;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in remove_job");
    }
    /* This will never be reached */
}

int c_wait_job_recv() {
    struct Msg m = initialize_message();
    int res;
    char *string = 0;

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in wait_job");
    switch (m.type) {
        case WAITJOB_OK:
            return m.u.result.errorlevel;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in wait_job - line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in c_wait_job");
    }
    /* This will never be reached */
    return -1;
}

static void c_wait_job_send() {
    struct Msg m = initialize_message();

    /* Send the request */
    m.type = WAITJOB;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);
}

static void c_wait_running_job_send() {
    struct Msg m = initialize_message();

    /* Send the request */
    m.type = WAIT_RUNNING_JOB;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);
}

/* Returns the errorlevel */
int c_wait_job() {
    c_wait_job_send();
    return c_wait_job_recv();
}

/* Returns the errorlevel */
int c_wait_running_job() {
    c_wait_running_job_send();
    return c_wait_job_recv();
}

void c_send_max_slots(int max_slots) {
    struct Msg m = initialize_message();

    /* Send the request */
    m.type = SET_MAX_SLOTS;
    m.u.max_slots = command_line.max_slots;
    send_msg(server_socket, &m);
}

void c_get_max_slots() {
    struct Msg m = initialize_message();
    int res;

    /* Send the request */
    m.type = GET_MAX_SLOTS;
    m.u.max_slots = command_line.max_slots;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in move_urgent");
    switch (m.type) {
        case GET_MAX_SLOTS_OK:
            printf("%i\n", m.u.max_slots);
            return;
        default:
            warning("Wrong internal message in get_max_slots");
    }
}

void c_move_urgent() {
    struct Msg m = initialize_message();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = URGENT;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in move_urgent");
    switch (m.type) {
        case URGENT_OK:
            return;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in move_urgent - line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in move_urgent");
    }
    /* This will never be reached */
    return;
}

void c_get_state() {
    struct Msg m = initialize_message();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = GET_STATE;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_state - line size");
    switch (m.type) {
        case ANSWER_STATE:
            printf("%s\n", jstate2string(m.u.state));
            return;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in get_state - line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in get_state");
    }
    /* This will never be reached */
    return;
}

void c_swap_jobs() {
    struct Msg m = initialize_message();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = SWAP_JOBS;
    m.u.swap.jobid1 = command_line.jobid;
    m.u.swap.jobid2 = command_line.jobid2;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in swap_jobs");
    switch (m.type) {
        case SWAP_JOBS_OK:
            return;
            /* WILL NOT GO FURTHER */
        case LIST_LINE: /* Only ONE line accepted */
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in swap_jobs - line size");
            fprintf(stderr, "Error in the request: %s",
                    string);
            free(string);
            exit(-1);
            /* WILL NOT GO FURTHER */
        default:
            warning("Wrong internal message in swap_jobs");
    }
    /* This will never be reached */
    return;
}

void c_get_count_running() {
    struct Msg m = initialize_message();
    int res;

    /* Send the request */
    m.type = COUNT_RUNNING;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in count_running - line size");

    switch (m.type) {
        case COUNT_RUNNING:
            printf("%i\n", m.u.count_running);
            return;
        default:
            warning("Wrong internal message in count_running");
    }

    /* This will never be reached */
    return;
}

void c_show_label() {
    struct Msg m = initialize_message();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = GET_LABEL;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_label");

    switch (m.type) {
        case LIST_LINE:
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in get_label - line size");

            printf("%s", string);
            free(string);
            return;
        default:
            warning("Wrong internal message in get_label");
    }

    /* This will never be reached */
    return;
}

void c_get_gpu_wait_time() {
    struct Msg m = initialize_message();
    int res;

    m.type = GET_GPU_WAIT_TIME;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in get_gpu_wait_time");

    if (m.type == GET_GPU_WAIT_TIME)
        printf("%d\n", m.u.gpu_wait_time);
    else
        warning("Wrong internal message in get_gpu_wait_time");
}

void c_set_gpu_wait_time() {
    struct Msg m = initialize_message();

    m.type = SET_GPU_WAIT_TIME;
    m.u.gpu_wait_time = command_line.gpu_wait_time;
    send_msg(server_socket, &m);
}

void c_send_reminder() {
    struct Msg m = initialize_message();
    m.type = REMINDER;
    send_msg(server_socket, &m);
}

void c_show_cmd() {
    struct Msg m = initialize_message();
    int res;
    char *string = 0;

    /* Send the request */
    m.type = GET_CMD;
    m.u.jobid = command_line.jobid;
    send_msg(server_socket, &m);

    /* Receive the answer */
    res = recv_msg(server_socket, &m);
    if (res != sizeof(m))
        error("Error in show_cmd");

    switch (m.type) {
        case LIST_LINE:
            string = (char *) malloc(m.u.size);
            res = recv_bytes(server_socket, string, m.u.size);
            if (res != m.u.size)
                error("Error in show_cmd - line size");

            printf("%s", string);
            free(string);
            return;
        default:
            warning("Wrong internal message in show_cmd");
    }
}

static void shuffle(int *array, size_t n) {
    if (n > 1) {
        size_t i;
        srand(time(NULL));
        for (i = 0; i < n - 1; i++) {
            size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
            int t = array[j];
            array[j] = array[i];
            array[i] = t;
        }
    }
}

static struct Msg initialize_message() {
    return (struct Msg) {.uid = (int) getuid()};
}

static void require_root() {
    if (geteuid() != 0)
        error("Not enough permission to perform the action");
}
