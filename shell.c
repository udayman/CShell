#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>


#define MAXARGS 81
#define MAXLINE 81
#define MAXJOBS 6

struct job
{
    int jid;
    pid_t pid;
    char status; 
    char cmdline[MAXLINE]; /*Cmd line*/
};

struct job* new_job(int jid, pid_t pid, char status, char *cmd) 
{
    struct job *j = malloc(sizeof(struct job));
    if (j == NULL) 
    {
        perror("malloc");
        exit(1);
    }

    j->jid = jid;
    j->pid = pid;
    j->status = status;
    strcpy(j->cmdline, cmd);
    return j;
}

/* Function prototypes */
int builtin_command(int argc, char **argv);
int getnextjid();
int add_new_bg(struct job *value);
int getjob_pid(pid_t pid);
int getjob_jid(int jid);
void sigint_handler();
void sigtstp_handler();
void sigchld_handler();
void sigquit_handler();
void quitshell();
int parseline(char *buf, int *narg, char **argv, char *input, char *output);
void eval(char *cmdline);
void block_on_fg(pid_t pid);
void kill_job(int index);


/*global*/
struct job *jobs[MAXJOBS]; /*Allow for Max 6 jobs (1 in foreground 5 in background)*/
int job_cnt = 0;

/*helper functions for getting various information*/

/*get the next available jid*/
int getnextjid()
{
    int i;
    int max = 0;
    for (i = 1; i < MAXJOBS; i++) 
    {
        if (jobs[i] != NULL) 
        {
            if (max < jobs[i]->jid) 
            {
                max = jobs[i]->jid;
            }
        }
    }
    return max+1;
}

/*add the job to background section at a free index */
int add_new_bg(struct job *value)
{
    int i;
    for (i = 1; i < MAXJOBS; i++) 
    {
        if (jobs[i] == NULL) 
        {
            value->jid = getnextjid();
            jobs[i] = value;
            job_cnt++;
            /*printf("add_new_job i=%d jid=%d cnt=%d\n", i, value->jid, job_cnt);*/
            return 0;
        }
    }

    return -1;
}

/*get background job index with pid*/
int getjob_pid(pid_t pid)
{
    int i;
    for (i = 1; i < MAXJOBS; i++) 
    {
        if (jobs[i] != NULL) 
        {
            if (jobs[i]->pid == pid) 
            {
                return i;
            }
        }
    }
    return -1;
}

/*get background job index with jid*/
int getjob_jid(int jid)
{
    int i;
    for (i = 1; i < MAXJOBS; i++) 
    {
        if (jobs[i] != NULL) 
        {
            if (jobs[i]->jid == jid) 
            {
                return i;
            }
        }
    }
    return -1;
}


/*handler functions*/

/*ctrl-c check index 0 for fg if there is fg, kill with sigint and delete index 0*/
void sigint_handler()
{
    if (jobs[0] != NULL) 
    {
        kill(jobs[0]->pid, SIGINT);
    }
    
}

/*ctrl-z check index 0 for fg if there is fg, fg gets added to bg delete jobs[0]*/
void sigtstp_handler()
{
    struct job *j = jobs[0];

    if (j != NULL) 
    {
        if (job_cnt >= MAXJOBS-1) 
        {
            printf("jobs reached max, cannot put fg to bg\n");
            return;
        }
        kill(j->pid, SIGTSTP);
    }
    
}

/*when child terminates, make sure it is not stopped, remove rest from our array */
void sigchld_handler()
{
    pid_t cpid;
    int index, stat;
    struct job *j;

    
    while ((cpid = waitpid(-1, &stat, WNOHANG | WUNTRACED)) > 0) 
    {
        index = getjob_pid(cpid);
        /*printf("sigchld %d %d SIG%d index:%d\n", cpid, stat, WTERMSIG(stat), index);*/
        if (index < 0)
            continue;

        j = jobs[index];
        if (WIFEXITED(stat) || WIFSIGNALED(stat)) 
        {
            /*printf("someone TERMd a bg process %d\n", cpid);*/
            free(j);
            jobs[index] = NULL;
            job_cnt--;
        }
        else if (WIFSTOPPED(stat)) 
        {
            j->status = 's';
            /*printf("someone STOPd a bg process %d\n", cpid);*/
        }
        else if (WIFCONTINUED(stat)) 
        {
            j->status = 'b';
            /*printf("someone CONTd a bg process %d\n", cpid);*/
        }
    }
    /*printf("sigchld done\n");*/
}

/*quitting on signal*/
void sigquit_handler(int sig)
{
    /*printf("Ok, bye\n");*/

    exit(0);
}

/*to quit, we deallocate all and end*/
void quitshell()
{
    int i;

    /*remove all background*/
    for (i = 1; i < MAXJOBS; i++) 
    {
        if (jobs[i] != NULL) 
        {
            kill_job(i);
        }

    }
    exit(0);
}

void eval(char *cmdline)
{
    char *argv[MAXARGS]; /*argument list to pass to execve*/
    char buf[MAXLINE]; /*holds modified cmd*/
    int is_bg; /*fg or bg?*/
    pid_t pid; /*Process id*/
    int argc;

    /*declare file strings here*/
    char inputfile[MAXLINE] = "";
    char outputfile[MAXLINE] = "";

    strcpy(buf, cmdline);
    is_bg = parseline(buf, &argc, argv, inputfile, outputfile);
    if (argv[0] == NULL) 
    {
        return; /* Ignore empty lines*/
    }

    if (!builtin_command(argc, argv)) 
    {
        int status;
        struct job *j;

        if (is_bg && job_cnt >= (MAXJOBS - 1))
        {
            printf("%s: max jobs reached in shell.\n", argv[0]);
            return;
        }

        if ((pid = fork()) == 0) 
        {
            /* each child becomes a pg, so to get signal for it and any of its children */
            setpgid(0,0);
            //handle io redirection
            if (inputfile[0] != '\0')
            {
                mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;
                int inFileID = open (inputfile, O_RDONLY, mode);
                dup2(inFileID, STDIN_FILENO);

            }

            if (outputfile[0] != '\0')
            {
                mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;
                int outFileID = open (outputfile, O_CREAT|O_WRONLY|O_TRUNC, mode);
                dup2(outFileID, STDOUT_FILENO);
            }

            if (execvp(argv[0], argv) < 0) 
            {
                if (execv(argv[0], argv) < 0)
                {
                    printf("%s: Command not found.\n", argv[0]);
                    exit(0);
                }
            }
        }

        if (pid < 0) 
        {
            printf("fork error\n");
            return;
        }

        j = new_job(0, pid, 'f', cmdline);
        if (is_bg) 
        {
            j->status = 'b';
            add_new_bg(j);
        }

        else 
        {  /* fg */
            jobs[0] = j;
        }

        if (!is_bg) {
            block_on_fg(pid);
        }
    }
}

/*parseline function returns true if bg-able cmd*/
int parseline(char *buf, int *narg, char **argv, char *inputfile, char *outputfile)
{
    int argc; /*number of args*/

    int len = strcspn(buf, "\n");

    buf[len] = '\0';
    int bg = 0;

    len = len - 1;
    while (len >= 0)
    {
        if (buf[len] == '&')
        {
            buf[len] = '\0';
            bg = 1;
            break;
        }
        else if (buf[len] != '\t' && buf[len] != ' ')
        {
            break;
        }
        len--;
    }

    int i = 0;
    int j = 0;
    int k = 0;

    while (buf[i] != '\0')
    {
        if (buf[i] == '<')
        {
            buf[i] = '\0'; //ignore command part after >/<
            //handle spaces after i
            i++;
            if (buf[i] == '\0')
            {
                break;
            }

            while (buf[i] == '\t' || buf[i] == ' ')
            {
                i++;
            }

            if (buf[i] == '\0')
            {
                break;
            }

            //now file descriptor, take it in inputfile
            while (buf[i] != '\t' && buf[i] != ' ' && buf[i] != '\0')
            {
                inputfile[j] = buf[i];
                i++;
                j++;
            }

            if (buf[i] == '\0')
            {
                break;
            }
        }

        if (buf[i] == '>')
        {
            buf[i] = '\0'; //same as before
            //handle spaces after i
            i++;
            if (buf[i] == '\0')
            {
                break;
            }

            while (buf[i] == '\t' || buf[i] == ' ')
            {
                i++;
            }
            
            if (buf[i] == '\0')
            {
                break;
            }

            //now file descriptor, take it in inputfile
            while (buf[i] != '\t' && buf[i] != ' ' && buf[i] != '\0')
            {
                outputfile[k] = buf[i];
                i++;
                k++;
            }

            if (buf[i] == '\0')
            {
                break;
            }
        }

        i++;
    }

    char *word = strtok(buf, " \t");

    /*Build the argv list*/
    argc = 0;
    while (word != NULL) 
    {
        argv[argc++] = word;
        word = strtok(NULL, " \t");
    }

    argv[argc] = NULL;
    if (argc == 0) 
    { /*ignore blank line*/
        return 1;
    }
    
    *narg = argc;
    return bg;
}

void block_on_fg(pid_t pid)
{
    struct job *j = jobs[0];
    int stat, ret;

    ret = waitpid(pid, &stat, WUNTRACED);
    if (ret < 0) 
    {    
        printf("waitfg: waitpid %d error\n", pid);
    }

    else 
    {
        if (WIFEXITED(stat)) 
        {   /* fg either done, or kill stop sent via sigstop*/
            free(j);
            jobs[0] = NULL;
        }
        else 
        {
            if (WIFSTOPPED(stat))
            {
                j->status = 's';
                if(add_new_bg(j) == 0) 
                {
                    jobs[0] = NULL;
                }
            }
        }
        /*printf("block on fg pid:%d done\n", pid);*/
    }
}

/* making it foreground means, move to 0th index, block till job finished */
void fg_job(int index) 
{
    struct job *j = jobs[index];

    jobs[0] = j;
    jobs[index] = NULL;
    job_cnt--;
    /* let  's' run, 'b' are running already */
    if (j->status == 's') 
    {
        kill(j->pid, SIGCONT);
    }

    j->status = 'f';
    block_on_fg(j->pid);
}

void bg_job(int index) 
{
    struct job *j = jobs[index];

    if (j->status == 's') 
    {
        kill(j->pid, SIGCONT);
        j->status = 'b';
    }
}

void kill_job(int index) 
{
    struct job *j = jobs[index];

    if (j->status == 's')
    {
        kill(j->pid, SIGCONT);
    }
    kill(j->pid, SIGINT);
}

/*builtin commands*/
int builtin_command(int argc, char **argv)
{
    struct job *j;
    int i, cnt = 0;
    char *bcmd = argv[0];

    if (!strcmp(bcmd, "quit")) 
    {
        quitshell();    // program exits here
    }

    if (!strcmp(bcmd, "jobs")) 
    {
        for (i = 1; i < MAXJOBS; i++) 
        {
            j = jobs[i];
            if (j == NULL) continue;
            cnt++;

            printf("[%d] (%d) %s %s", j->jid, j->pid, j->status == 's' ? "Stopped" : "Running", j->cmdline);
        }

        if (cnt != job_cnt) 
        {
            printf("BUG: cnt %d job_cnt %d\n", cnt, job_cnt);
            exit(1);
        }
        return 1;
    }

    if (!strcmp(bcmd, "fg") || !strcmp(bcmd, "bg") || !strcmp(bcmd, "kill")) 
    {
        int stat, pid, jid;

        if (argc != 2 || argv[1] == NULL) 
        {
            printf("usage: %s use percent job or pid\n", bcmd);
            return 1;
        }

        if (argv[1][0] == '%') 
        {
            sscanf(argv[1]+1, "%d", &jid);
            i = getjob_jid(jid);
        }
        else 
        {
            sscanf(argv[1], "%d", &pid);
            i = getjob_pid(pid);
        }

        if (i == -1) 
        {
            printf("no matching job or pid. Run \"jobs\" to be sure\n");
            return 1;
        }

        if (!strcmp(bcmd, "fg")) 
        {
            fg_job(i);
        }

        else if (!strcmp(bcmd, "bg")) 
        {
            bg_job(i);
        }

        else if (!strcmp(bcmd, "kill")) 
        {
            kill_job(i);
        }
        return 1;
    }

    if (!strcmp(argv[0], "&")) 
    {    /* not really a command */
        return 1;
    }

    return 0;
}

int main()
{
    char cmdline[MAXLINE]; /*Cmd line*/
    memset(jobs, 0, sizeof(jobs)); /*creating jobs with null*/
   
    /*define signals here*/
    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, sigtstp_handler);
    signal(SIGCHLD, sigchld_handler);
    signal(SIGQUIT, sigquit_handler);   //TBR

    while (1) 
    {
        printf("prompt> ");

        fgets(cmdline, MAXLINE, stdin);
        
        if (feof(stdin)) 
        {
            quitshell();
        }

        /*Evaluate*/
        eval(cmdline);
        
    }
}
