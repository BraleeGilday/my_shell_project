#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jobs.h"
#include "params.h"
#include "parser.h"
#include "wait.h"

int
wait_on_fg_pgid(pid_t const pgid)
{
  if (pgid < 0) return -1;
  jid_t const jid = jobs_get_jid(pgid);
  if (jid < 0) return -1;

  /* Make sure the foreground group is running */
  /* BGDID send the "continue" signal to the process group 'pgid'
   * XXX review kill(2)
   */
  if (kill(-pgid, SIGCONT) < 0) return -1;
    // I think it's correct to have -pgid; 
    // If pid is less than -1, then sig is sent to every process in the
    //   process group whose ID is -pid (Linux manpage)
  
  // BG added; double check this 11/21
  pid_t terminal_pgid = tcgetpgrp(STDIN_FILENO);
  if (terminal_pgid < 0)  return -1;

  if (is_interactive) {
    /* BGDID make 'pgid' the foreground process group
     * XXX review tcsetpgrp(3) */
    if (tcsetpgrp(STDIN_FILENO, pgid) < 0) return -1; 
  }

  /* XXX From this point on, all exit paths must account for setting bigshell
   * back to the foreground process group--no naked return statements */
  int retval = 0;

  /* XXX Notice here we loop until ECHILD and we use the status of
   * the last child process that terminated (in the previous iteration).
   * Consider a pipeline,
   *        cmd1 | cmd2 | cmd3
   *
   * We will loop exactly 4 times, once for each child process, and a
   * fourth time to see ECHILD.
   */
  for (;;) {
    /* Wait on ALL processes in the process group 'pgid' */
    int status;
    pid_t res = waitpid(/*BGDID*/-pgid, &status, /*BGDID*/ WUNTRACED);
      /* waitpid(): on success, returns the process ID of the child whose state has changed; 
       * On error, -1 is returned. bg
       * used to wait for state changes in a child of the calling process, and obtain information
       *  about the child whose state has changed. A state change is considered to be: 
       * the child terminated; the child was stopped by a signal; or the child was resumed by a signal.
       * 
       * The waitpid() system call suspends execution of the calling process until a child specified 
       * by pid argument has changed state. 
       * 
       * This call to waitpid() waits for any child process whose 
       * process group ID is equal to that of the calling process because of the options argument, 0.
      */
    if (res < 0) {
      /* Error occurred (some errors are ok, see below)
       *
       * XXX status may have a garbage value, use last_status from the
       * previous loop iteration */
      if (errno == ECHILD) {
        /* No unwaited-for children. The job is done! */
        errno = 0;
        if (jobs_get_status(jid, &status) < 0) goto err;

        if (WIFEXITED(status)) {
          // That is, if the child terminated normally -BG
          /* BGDID set params.status to the correct value */
          params.status = WEXITSTATUS(status); // returns the exit status of the child; macro should only be employed if WIFEXITED returned true
        } else if (WIFSIGNALED(status)) {
          // That is, if the child process was terminated by a signal -BG
          /* BGDID set params.status to the correct value */
          params.status = (128 + (WTERMSIG(status)));   // Double check I'm supposed to add to 128, yes
            // returns the number of the signal that caused the child process to terminate. This macro should only be employed if WIFSIGNALED returned true.
        }

        /* BGDID remove the job for this group from the job list
         *  see jobs.h
         */
        if (jobs_remove_jid(jid) < 0) goto err;

        goto out;
      }
      goto err; /* An actual error occurred */
    }
    assert(res > 0);
    /* status is valid */

    /* Record status for reporting later when we see ECHILD */
    if (jobs_set_status(jid, status) < 0) goto err;

    /* BGDID to handle case where a child process is stopped
     *  The entire process group is placed in the background (how is that being taken care of??)
     */
    if (WIFSTOPPED(status)) {
      fprintf(stderr, "[%jd] Stopped\n", (intmax_t)jid);
      goto out;
    }

    /* A child exited, but others remain. Loop! */
  }

out:
  if (0) {
  err:
    retval = -1;
  }

  if (is_interactive) {
    /* BGDID make bigshell the foreground process group again
     * XXX review tcsetpgrp(3)
     *
     * Note: this will cause bigshell to receive a SIGTTOU signal.
     *       You need to also finish signal.c to have full functionality here.
     *       Otherwise you bigshell will get stopped.
     */

    if (tcsetpgrp(STDIN_FILENO, terminal_pgid) < 0) return -1;
                  // Or should I use 0?
  }
  return retval;
}

/* XXX DO NOT MODIFY XXX */
int
wait_on_fg_job(jid_t jid)
{
  pid_t pgid = jobs_get_pgid(jid);
  if (pgid < 0) return -1;
  return wait_on_fg_pgid(pgid);
}

int
wait_on_bg_jobs()
{
  size_t job_count = jobs_get_joblist_size();
  struct job const *jobs = jobs_get_joblist();
  for (size_t i = 0; i < job_count; ++i) {
    pid_t pgid = jobs[i].pgid;
    jid_t jid = jobs[i].jid;
    for (;;) {
      /* BGDID: Modify the following line to wait for process group
       * XXX make sure to do a nonblocking wait!
       */
      int status;
      pid_t pid = waitpid(-pgid, &status, WNOHANG);
        /* 
        * waitpid(): on success, returns the process ID of the child whose state has changed; 
        * if WNOHANG was specified and one or more child(ren) specified by pid exist, 
        * but have not yet changed state, then 0 is returned. 
        * On error, -1 is returned.
        */
      if (pid == 0) {
        /* Unwaited children that haven't exited */
        break;
      } else if (pid < 0) {
        /* Error -- some errors are ok though! */
        if (errno == ECHILD) {
          /* No children -- print saved exit status */
          errno = 0;
          if (jobs_get_status(jid, &status) < 0) return -1;
          if (WIFEXITED(status)) {
            fprintf(stderr, "[%jd] Done\n", (intmax_t)jid);
          } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[%jd] Terminated\n", (intmax_t)jid);
          }
          jobs_remove_pgid(pgid);
          job_count = jobs_get_joblist_size();
          jobs = jobs_get_joblist();
          break;
        }
        return -1; /* Other errors are not ok */
      }

      /* Record status for reporting later when we see ECHILD */
      if (jobs_set_status(jid, status) < 0) return -1;

      /* Handle case where a process in the group is stopped */
      if (WIFSTOPPED(status)) {
        fprintf(stderr, "[%jd] Stopped\n", (intmax_t)jid);
        break;
      }
    }
  }
  return 0;
}
