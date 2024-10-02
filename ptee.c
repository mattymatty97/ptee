/*
 *  ptee © 2024 by mattymatty97 is licensed under CC BY 4.0. To view a copy of this license, visit https://creativecommons.org/licenses/by/4.0/
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include "pthread.h"
#include <sys/wait.h>
#include <semaphore.h>


//--------------CONSTANTS-------------------

static struct option long_options[] = {
        {"input-pipe",  required_argument, 0, 'i'},
        {"output-pipe", required_argument, 0, 'o'},
        {"error-pipe",  required_argument, 0, 'e'}
};

int openPipe(const char *pipeName, int mode);

_Noreturn void *readInput(void *params);

_Noreturn void *readOutput(void *params);


int main(int argc, char **argv) {
    int c;

    char *inputPipeName = NULL;
    char *outputPipeName = NULL;
    char *errorPipeName = NULL;

    int option_index = 0;
    while ((c = getopt_long(argc, argv, "+:i:o:e:", long_options, &option_index)) != -1) {
        switch (c) {
            case 0:
                /* If this option set a flag, do nothing else now. */
                if (long_options[option_index].flag != 0)
                    break;
                printf("option %s", long_options[option_index].name);
                if (optarg)
                    printf(" with arg %s", optarg);
                printf("\n");
                break;

            case 'i':
                inputPipeName = strdup(optarg);
                break;

            case 'o':
                outputPipeName = strdup(optarg);
                break;

            case 'e':
                errorPipeName = strdup(optarg);
                break;

            default :
                /* unknown flag ignore for now. */
                break;
        }
    }

    if (argc <= optind) {
        fprintf(stderr, "Missing Required parameters\n");
        fprintf(stderr, "Usage: ptee -i [inputPipe] -o [outputPipe] -e [errorPipe] [command to execute]\n");
        fprintf(stderr, "Or: ptee -i [inputPipe] -o [outputPipe] -e [errorPipe] -- [command to execute]\n");
        exit(EXIT_FAILURE);
    }

    //ignore broken pipes
    signal(SIGPIPE, SIG_IGN);

    int inputfd, outputfd, errorfd;


    //create input pipe if missing!
    if (inputPipeName != NULL) {
        if (access(inputPipeName, F_OK)) {
            mkfifo(inputPipeName, 0666);
        }

        inputfd = openPipe(inputPipeName, O_RDONLY | O_NONBLOCK);

    }

    if (outputPipeName != NULL) {
        //create output pipe if missing!
        if (access(outputPipeName, F_OK)) {
            mkfifo(outputPipeName, 0666);
        }

        int tmpfd = openPipe(outputPipeName, O_RDONLY | O_NONBLOCK);

        outputfd = openPipe(outputPipeName, O_WRONLY | O_NONBLOCK);

        close(tmpfd);
    }

    if (errorPipeName != NULL) {
        //create error pipe if missing!
        if (access(errorPipeName, F_OK)) {
            mkfifo(errorPipeName, 0666);
        }

        int tmpfd = openPipe(errorPipeName, O_RDONLY | O_NONBLOCK);

        errorfd = openPipe(errorPipeName, O_WRONLY | O_NONBLOCK);

        close(tmpfd);
    }

    int stdinFd[2];
    int stdoutFd[2];
    int stderrFd[2];

    if (inputPipeName != NULL) {
        if (pipe(stdinFd)) {
            fprintf(stderr, "process pipe 1\n");
            exit(EXIT_FAILURE);
        }
    }

    if (outputPipeName != NULL) {
        if (pipe(stdoutFd)) {
            fprintf(stderr, "process pipe 2\n");
            exit(EXIT_FAILURE);
        }
    }

    if (errorPipeName != NULL) {
        if (pipe(stderrFd)) {
            fprintf(stderr, "process pipe 3\n");
            exit(EXIT_FAILURE);
        }
    }

    char sem_name[5 + 22];
    sem_t *sem;

    sprintf(sem_name,"ptee-%d", getpid());

    /* initialize semaphores for shared processes */
    sem = sem_open (sem_name, O_CREAT | O_EXCL, 0644, 0);
    /* name of semaphore is "pSem", semaphore is reached using this name */

    fprintf(stderr, "Starting %s with params: ", argv[optind]);
    int index;
    for (index = optind + 1; index < argc; index++)
        fprintf(stderr, "\"%s\" ", argv[index]);
    fprintf(stderr, "\n");

    pid_t pid = fork();

    if (pid < 0) {
        /* cleanup semaphores */
        sem_unlink (sem_name);
        sem_close(sem);
        /* unlink prevents the semaphore existing forever */
        /* if a crash occurs during the execution         */
        perror("fork failed");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {
        //child process
        //receive notice of parent death
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int altErr = dup(STDERR_FILENO);

        //use our pipes

        if (inputPipeName != NULL) {
            dup2(stdinFd[0], 0);
            close(stdinFd[1]);
            close(inputfd);
        }
        if (outputPipeName != NULL) {
            dup2(stdoutFd[1], 1);
            close(stdoutFd[0]);
            close(outputfd);
        }
        if (errorPipeName != NULL) {
            dup2(stderrFd[1], 2);
            close(stderrFd[0]);
            close(errorfd);
        }

        sem_wait(sem);

        if (execvp(argv[optind], &argv[optind]) == -1) {
            perror("Error");
            exit(1);
        }
        dprintf(altErr, "Bad Fork!");
        exit(EXIT_FAILURE);
    }

    if (inputPipeName != NULL) {
        close(stdinFd[0]);
    }

    if (outputPipeName != NULL) {
        close(stdoutFd[1]);
    }

    if (errorPipeName != NULL) {
        close(stderrFd[1]);
    }

    pthread_t inputThread[2];


    if (inputPipeName != NULL) {
        int input1params[2] = {STDIN_FILENO, stdinFd[1]};
        if (pthread_create(&inputThread[0], NULL, readInput, input1params)) {
            /* cleanup semaphores */
            sem_unlink (sem_name);
            sem_close(sem);
            /* unlink prevents the semaphore existing forever */
            /* if a crash occurs during the execution         */
            perror("Reader Thread 1 Failed");
            exit(EXIT_FAILURE);
        }

        int input2params[2] = {inputfd, stdinFd[1]};
        if (pthread_create(&inputThread[1], NULL, readInput, input2params)) {
            /* cleanup semaphores */
            sem_unlink (sem_name);
            sem_close(sem);
            /* unlink prevents the semaphore existing forever */
            /* if a crash occurs during the execution         */
            perror("Reader Thread 2 Failed");
            exit(EXIT_FAILURE);
        }
    }

    pthread_t outputThread;

    if (outputPipeName != NULL) {
        int outputParams[3] = {stdoutFd[0], STDOUT_FILENO, outputfd};
        if (pthread_create(&outputThread, NULL, readOutput, outputParams)) {
            /* cleanup semaphores */
            sem_unlink (sem_name);
            sem_close(sem);
            /* unlink prevents the semaphore existing forever */
            /* if a crash occurs during the execution         */
            perror("Output Thread Failed");
            exit(EXIT_FAILURE);
        }
    }


    pthread_t errorThread;

    if (errorPipeName != NULL) {
        int errorParams[3] = {stderrFd[0], STDERR_FILENO, errorfd};
        if (pthread_create(&errorThread, NULL, readOutput, errorParams)) {
            /* cleanup semaphores */
            sem_unlink (sem_name);
            sem_close(sem);
            /* unlink prevents the semaphore existing forever */
            /* if a crash occurs during the execution         */
            perror("Error Thread Failed");
            exit(EXIT_FAILURE);
        }
    }

    //let the fork process start
    sem_post(sem);

    int retcode;
    if (waitpid(pid,&retcode, WUNTRACED) < 0) {
        /* cleanup semaphores */
        sem_unlink (sem_name);
        sem_close(sem);
        /* unlink prevents the semaphore existing forever */
        /* if a crash occurs during the execution         */
        perror("Waitpid failed");
        exit(EXIT_FAILURE);
    }
    /* cleanup semaphores */
    sem_unlink (sem_name);
    sem_close(sem);
    /* unlink prevents the semaphore existing forever */
    /* if a crash occurs during the execution         */
    exit(WEXITSTATUS(retcode));
}

int openPipe(const char *pipeName, int mode) {
    int fd = open(pipeName, mode);
    if (-1 == fd) {
        fprintf(stderr, "%s:", pipeName);
        perror("readfd: open()");
        exit(EXIT_FAILURE);
    }

    struct stat status;

    if (-1 == fstat(fd, &status)) {
        fprintf(stderr, "%s:", pipeName);
        perror("fstat");
        close(fd);
        exit(EXIT_FAILURE);
    }

    if (!S_ISFIFO(status.st_mode)) {
        fprintf(stderr, "%s: in not a fifo!\n", pipeName);
        close(fd);
        exit(EXIT_FAILURE);
    }

    return fd;
}

_Noreturn void *readInput(void *params) {
    int readFd = ((int *) params)[0];
    int writeFd = ((int *) params)[1];
    char buffer[BUFSIZ];
    ssize_t bytes;
    while (1) {
        bytes = read(readFd, buffer, sizeof(buffer));
        if (bytes < 0 && errno == EINTR)
            continue;

        (void) write(writeFd, buffer, bytes);
    }
}

_Noreturn void *readOutput(void *params) {
    int readFd = ((int *) params)[0];
    int write1Fd = ((int *) params)[1];
    int write2Fd = ((int *) params)[2];
    char buffer[BUFSIZ];
    ssize_t bytes;
    while (1) {
        bytes = read(readFd, buffer, sizeof(buffer));
        if (bytes < 0 && errno == EINTR)
            continue;

        (void) write(write1Fd, buffer, bytes);
        (void) write(write2Fd, buffer, bytes);
    }
}
