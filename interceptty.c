/*     interceptty.c
 *
 * This file is an adaptation of ttysnoops.c, from the ttysnoop-0.12d
 * package It was originally written by Carl Declerck, Ulrich
 * Callmeir, Carl Declerck, and Josh Bailey.  They deserve all of the
 * credit for the clever parts.  I, on the other hand, deserve all of
 * the blame for whatever I broke.  Please do not email the original
 * authors of ttysnoop about any problems with interceptty.
 *
 */

/* $Id: interceptty.c,v 7.12 2004/09/05 23:01:35 gifford Exp $ */

#include "config.h"

#include <sys/types.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <grp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <netdb.h>
#include <pwd.h>
#include <grp.h>

#include "bsd-openpty.h"
#include "common.h"

#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

#define DEFAULT_FRONTEND "/tmp/interceptty"

struct sockaddr_in inet_resolve(const char *sockname);


#define BUFF_SIZE	256

char buff[BUFF_SIZE];

char ttynam[TTYLEN+1] = "";
int ptyfd = -1;

int fdmax = 0;

char    *backend = NULL,
  *frontend = DEFAULT_FRONTEND,
  *settings = NULL,
  *outfilename = NULL,
  *opt_ptyname = NULL,
  *opt_ttyname = NULL;
int     verbose = 0,
  linebuff = 0,
  quiet = 0;
int     created_link = 0;
char    last_pty[TTYLEN] = "",
  last_tty[TTYLEN] = "";
pid_t child_pid = 0;
int please_die_now = 0;
int listenfd = 0;

mode_t frontend_mode = -1;
uid_t frontend_owner = -1;
gid_t frontend_group = -1;
uid_t switch_uid = -1;
gid_t switch_gid = -1;
char *switch_root = NULL;

int no_closedown = 0;

FILE *outfile;

/* find & open a pty to be used by the pty-master */

int find_ptyxx (char *ptyname)
{
  int fd, ttyfd;
  
  if (my_openpty(&fd,&ttyfd,ptyname) < 0)
    errorf("Couldn't openpty: %s\n",strerror(errno));

  if (stty_raw(ttyfd) != 0)
    errorf("Couldn't put pty into raw mode: %s\n",strerror(errno));
  /* Throw away the ttyfd.  We'll keep it open because it prevents
   * errors when the client disconnects, but we don't ever plan to
   * read or write any data, so we needn't remember it.
   */

  return fd;
}

/* Create the pty */
int create_pty (int *ptyfd, char *ttynam)
{
  char name[TTYLEN+1];

  if (opt_ptyname)
  { 
    if (strlen(opt_ptyname) > TTYLEN)
      errorf("Specified pty name is too long!");
                
    strcpy(name, opt_ptyname);
    if (opt_ttyname)
    {
      if (strlen(opt_ttyname) > TTYLEN)
        errorf("Specified tty name is too long!");
      strcpy(ttynam, opt_ttyname);
    }
    else if (strncmp(name,"/dev/pty",8) == 0)
    {
      /* Hacky, or heuristic? */
      strcpy(ttynam, name);
      ttynam[5] = 't';
    }
    else if (frontend)
    {
      errorf("A pty was specified with -p, but I couldn't figure out a tty to go with it.\nEither give me a tty with the -t switch, \nor else tell me not to create a front-end device by passing '-' as the front-device.");
    }
    else
    {
      ttynam[0]='\0';
    }
    *ptyfd = open(opt_ptyname,O_RDWR|O_NOCTTY);
  }
  else
  {
    *ptyfd = find_ptyxx(name);
    strcpy(ttynam, name);
  }
  if (*ptyfd < 0)
    errorf("can't open pty '%s'\n",name);

  return 1;
}		

/* do a graceful closedown */

void closedown (void)
{	
  if (no_closedown)
    return;
  stty_orig ();
  if (created_link)
    unlink(frontend);
  if (child_pid) 
  {
    if (verbose)
      fprintf(stderr,"Sending signal %d to child pid %ld\n",SIGTERM,(long)child_pid);
    kill(child_pid,SIGTERM);
  }
  if (verbose)
    fprintf(stderr,"closing down everything\n");
}

/* signal handlers */

void sighup (int sig)
{
  sig = sig;
  closedown ();
}

void sigpipe (int sig)
{
  sig = sig;
	
  signal (SIGPIPE, sigpipe);
}

void sigint(int sig)
{
  sig = sig;
  closedown();
  _exit(1);
}

void sigdeath(int sig)
{
  please_die_now=1;
}

void sigchld(int sig)
{
  child_pid = 0;
  sigdeath(sig);
}

int alldigits(const char *s)
{
  while(isdigit(*s))
    s++;
  return *s == '\0';
}

uid_t find_uid(const char *u)
{
  struct passwd *pw;

  if (alldigits(u))
    return atoi(u);
  
  if (!(pw = getpwnam(u)))
    errorf("Error finding user '%s': %s\n",u,strerror(errno));
  return pw->pw_uid;
}

gid_t find_gid(const char *g)
{
  struct group *gr;

  if (alldigits(g))
    return atoi(g);
  if (!(gr = getgrnam(g)))
    errorf("Error finding group '%s': %s\n",g,strerror(errno));
  return gr->gr_gid;
}

/* main program */

void dumpbuff(int dir, char *buf, int buflen)
{
  int i;
  int ic;
  
  for (i=0;i<buflen;i++)
  {
    if (dir)
    {
      fprintf(outfile, "> \t");
    }
    else
    {
      fprintf(outfile, "< ");
    }
    ic=(unsigned char)buf[i];
    fprintf(outfile, "0x%02x",ic);
    if ( (buf[i] >= 33) && (buf[i] <= 126) )
    {
      fprintf(outfile, " (%c)",buf[i]);
    }
    fprintf(outfile, "\n");
  }
  fflush(outfile);
}

/* Run stty on the given file descriptor with the given arguments */
int fstty(int fd, char *stty_args)
{
  int child_status;
  int pid;
  char *stty_cmd;
        
  stty_cmd = malloc(strlen(stty_args)+1+strlen("stty "));
  if (!stty_cmd)
    errorf("Couldn't malloc for stty_cmd: %s\n",strerror(errno));
  strcpy(stty_cmd,"stty ");
  strcat(stty_cmd,stty_args);
                
  if ((pid=fork()) == 0)
  {
    /* Child */
    if (dup2(fd,STDIN_FILENO) < 0)
      errorf("Couldn't dup2(%d,STDIN_FILENO=%d): %s\n",fd,STDIN_FILENO,strerror(errno));
    if (execlp("sh","sh","-c",stty_cmd,NULL) < 0)
      errorf("Couldn't exec stty command: %s\n",strerror(errno));
    /* Should never reach here. */
    exit(-1);
  }
  else if (pid == -1)
  {
    errorf("Couldn't fork: %s\n",strerror(errno));
  }
                
  free(stty_cmd);
  /* Parent */
  if (wait(&child_status) <= 0)
    errorf("Error waiting for forked stty process: '%s'\n",strerror(errno));
  if (!(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0) )
    errorf("stty %s failed\n",stty_args);

  return 0;
}
        

        
/*************************************
 * Set up backend device
 ************************************/

int setup_back_inet_socket(char *backend, int f[2])
{
  struct sockaddr_in sa;
  int fd;

  sa = inet_resolve(backend);
  if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 3)
    errorf("Couldn't open socket: %s\n",strerror(errno));

  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    errorf("Couldn't connect socket: %s\n", strerror(errno));

  return f[0]=f[1]=fd;
}

int setup_back_unix_socket(char *sockname, int f[2])
{
  int fd;
  struct sockaddr_un sa;
  
  if ((strlen(sockname)+1) > sizeof(sa.sun_path))
    errorf("Path name is too long for a Unix socket.\n");
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path,sockname);
  
  if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 3)
    errorf("Couldn't open socket: %s\n",strerror(errno));
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    errorf("Couldn't connect socket: %s\n", strerror(errno));

  return f[0]=f[1]=fd;
}


struct sockaddr_in inet_resolve(const char *sockname)
{
  struct sockaddr_in sa;
  char *hostname, *netport;
  struct hostent *he;
  
  if (strchr(sockname,':') == NULL)
    errorf("Internet hostname must be @host:port\n");

  if (!(hostname = strdup(sockname)))
    errorf("Couldn't dup string: %s\n",strerror(errno));

  netport = strchr(hostname,':');
  *netport='\0';
  netport++;
  
  sa.sin_family=AF_INET;
  
  if (!(he = gethostbyname(hostname)))
    errorf("Couldn't resolve name '%s': %s.\n",hostname,
	   (h_errno == HOST_NOT_FOUND) ? "Host not found" :
	   ((h_errno == NO_ADDRESS)||(h_errno == NO_DATA)) ? "No data available" :
	   (h_errno == NO_RECOVERY) ? "A non-recoverable name server error occured" :
	   (h_errno == TRY_AGAIN) ? "A temporary error occured." :
	   "An unknown error occured");

  memcpy(&(sa.sin_addr),he->h_addr,he->h_length);
      
#if 0
  if (!(se = getservbyname(netport)))
    errorf("Couldn't resolve port.\n");

  host_port=htons(se->s_port);
#endif
      
  if (!(sa.sin_port = htons(atoi(netport))))
    errorf("Couldn't figure out port number.\n");

  free(hostname);
  
  return sa;
}

int setup_back_tty(char *backend, int f[2])
{
  int serialfd;
  
  /* Open the serial port */
  serialfd = open(backend, O_RDWR | O_NOCTTY | O_SYNC | O_NOCTTY);
  if (serialfd < 0)
    errorf("error opening backend device '%s': %s\n",backend,strerror(errno));
  if (stty_raw(serialfd) != 0)
    errorf("Error putting serial device '%s' in raw mode: %s\n",backend,strerror(errno));
  
  /* Process settings from the -s switch */
  if (settings) {
    fstty(serialfd,settings);
  }

  return f[0]=f[1]=serialfd;
}

int setup_back_program(char *backend, int f[2])
{
  int sock[2];

  if (socketpair(PF_UNIX,SOCK_STREAM,0,sock) != 0)
    errorf("Couldn't create socket: %s\n",strerror(errno));

  /* Now run the program */
  switch (child_pid=fork()) {
    case -1:
      /* Error */
      errorf("Error in fork(): %s\n",strerror(errno));
    case 0:
      /* Child */
      if (close(sock[0]) != 0) {
	errorf("Error in close(sock[0]): %s\n",strerror(errno));
      }

      if (close(STDIN_FILENO) != 0) {
	errorf("Error in close(STDIN_FILENO): %s\n",strerror(errno));
      }
      if (dup2(sock[1],STDIN_FILENO) != STDIN_FILENO) {
	errorf("Error in dup2(sock[1],STDIN_FILENO): %s\n",strerror(errno));
      }

      if (close(STDOUT_FILENO) != 0) {
	errorf("Error in close(STDOUT_FILENO): %s\n",strerror(errno));
      }
      if (dup2(sock[1],STDOUT_FILENO) != STDOUT_FILENO) {
	errorf("Error in dup2(sock[1],STDOUT_FILENO): %s\n",strerror(errno));
      }
      
      if (close(sock[1]) != 0) {
	errorf("Error in close(sock[1]): %s\n",strerror(errno));
      }

      execl("/bin/sh","sh","-c",backend,NULL);
      /* Only returns if there is an error. */
      errorf("exec error: %s\n",strerror(errno));
  }
  /* Parent */
  return f[0]=f[1]=sock[0];
}

/* This can also do front fds */
int setup_back_fds(char *backend, int f[2])
{
  char *fd1_s,*fd2_s;

  if (!(fd1_s = strdup(backend)))
    errorf("Couldn't dup string: %s\n",strerror(errno));

  if ((fd2_s = strchr(fd1_s,',')) != 0) {
    *fd2_s='\0';
    fd2_s++;
  }
  else {
    fd2_s = fd1_s;
  }
  f[1]=atoi(fd2_s);
  return f[0]=atoi(fd1_s);
}


int setup_backend(int f[2])
{
  switch(backend[0]) {
    case '@':
      if (strchr(backend,'/')!=0)
        return setup_back_unix_socket(backend+1,f);
      else
        return setup_back_inet_socket(backend+1,f);
    case '!':
      return setup_back_program(backend+1,f);
    case '=':
      return setup_back_fds(backend+1,f);
    default:
      return setup_back_tty(backend,f);
  }
}


/* Continue in a child process, and wait for that process to exit.
 * When it does, fix permissions.
 * This is robust in the face of crashes, and will let us change
 * perms back even if we've dropped privs.
 */
void fix_perms_after_exit(char *ttynam, struct stat st)
{
  int exit_status;
  sigset_t sigmask;
  struct sigaction sigact;

  switch(fork())
  {
    case -1: /* error */ 
      errorf("Couldn't fork: %s\n",strerror(errno));
      break;
    case 0: /* child */
      return;
  }

  /* We've already registered the closedown function, but only want to
   * call it from the child. 
  */
  no_closedown = 1;

  /* Ignore these signals; the child will catch them and terminate. */
  sigemptyset(&sigmask);
  memset(&sigact,0,sizeof sigact);
  sigact.sa_handler = SIG_IGN;
  sigact.sa_mask = sigmask;
  sigaction(SIGHUP,&sigact,NULL);
  sigaction(SIGINT,&sigact,NULL);

  if (verbose)
    fprintf(stderr,"PID %d monitoring child process\n",getpid());

  /* Wait for everything else to finish */
  wait(&exit_status);
 
  if (chown(ttynam, st.st_uid, st.st_gid) < 0)
    errorf("Couldn't chown backend device to uid=%d, gid=%d: %s\n",st.st_uid,st.st_gid,strerror(errno));
  if (chmod(ttynam, st.st_mode & 07777) < 0)
    errorf("Couldn't set permissions on tty '%s': %s\n",strerror(errno));

  if (verbose)
    fprintf(stderr,"PID %d saw child process exit\n",getpid());
  /* Now exit the same way the child did */
  if (WIFEXITED(exit_status))
    exit(WEXITSTATUS(exit_status));
  else if (WIFSIGNALED(exit_status))
    kill(getpid(),WTERMSIG(exit_status));
  else
    exit(99); /* Should never happen */
}

/*************************************
 * Set up pty
 ************************************/
int setup_front_tty(char *frontend, int f[2])
{
  struct stat st;
  int ptyfd;

  /* Open the parent tty */
  create_pty(&ptyfd, ttynam);
  
  /* Now set permissions, owners, etc. */
  if (geteuid() == 0)
  {
    if ((frontend_mode == -1) && !strchr("@!=",backend[0]))
    {
      if (stat(backend, &st) < 0)
	errorf("Couldn't stat backend device '%s': %s\n",backend,strerror(errno));
      frontend_mode = st.st_mode;
      frontend_owner = st.st_uid;
      frontend_group = st.st_gid;
    }
    if (frontend_mode != -1) {
      /* Set up permissions on the pty slave */
      if (stat(ttynam, &st) < 0)
	errorf("Couldn't stat tty '%s': %s\n",ttynam,strerror(errno));

      fix_perms_after_exit(ttynam,st); /* This will create a monitor process */

      if (chown(ttynam, frontend_owner, frontend_group) < 0)
	errorf("Couldn't chown backend device to uid=%d, gid=%d: %s\n",st.st_uid,st.st_gid,strerror(errno));
      if (chmod(ttynam, frontend_mode & 07777) < 0)
	errorf("Couldn't set permissions on tty '%s': %s\n",strerror(errno));
    }
  }
  /* Now make the symlink */
  if (frontend)
  { 
    /* Unlink it in case it's there; if it fails, it
     * probably wasn't.
     */
    unlink(frontend);
    if (symlink(ttynam, frontend) < 0)
    {
      errorf("Couldn't symlink '%s' -> '%s': %s\n",frontend,ttynam,strerror(errno));
    }
    created_link = 1;
  }

  return f[0]=f[1]=ptyfd;
}

int setup_front_unix_socket(char *sockname, int f[2])
{
  struct sockaddr_un sa;
  int reuse_flag = 1;
  
  if ((strlen(sockname)+1) > sizeof(sa.sun_path))
    errorf("Path name is too long for a Unix socket.\n");
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path,sockname);
  
  if ((listenfd = socket(PF_UNIX, SOCK_STREAM, 0)) < 3)
    errorf("Couldn't open socket: %s\n",strerror(errno));
  if (setsockopt(listenfd,SOL_SOCKET, SO_REUSEADDR,&reuse_flag,sizeof(reuse_flag)) < 0)
    errorf("Couldn't make socket reusable: %s\n",strerror(errno));

  unlink(sockname); /* Just in case */

  if (bind(listenfd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    errorf("Couldn't bind to unix socket: %s\n",strerror(errno));
  if (listen(listenfd,5) < 0)
    errorf("Couldn't listen on unix socket: %s\n",strerror(errno));

  /* Arrange to have the socket deleted */
  frontend++;
  created_link=1;
  
  return f[0]=f[1]=accept(listenfd,NULL,NULL);
}

int setup_front_inet_socket(char *sockname, int f[2])
{
  struct sockaddr_in sa;
  int reuse_flag = 1;
  
  sa = inet_resolve(sockname);
  
  if ((listenfd = socket(PF_INET, SOCK_STREAM, 0)) < 3)
    errorf("Couldn't open socket: %s\n",strerror(errno));
  if (setsockopt(listenfd,SOL_SOCKET, SO_REUSEADDR,&reuse_flag,sizeof(reuse_flag)) < 0)
    errorf("Couldn't make socket reusable: %s\n",strerror(errno));
  if (bind(listenfd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    errorf("Couldn't bind to inet socket: %s\n",strerror(errno));
  if (listen(listenfd,5) < 0)
    errorf("Couldn't listen on unix socket: %s\n",strerror(errno));

  return f[0]=f[1]=accept(listenfd,NULL,NULL);
}

int setup_frontend(int f[2])
{
  if (frontend)
    switch (frontend[0])
    {
      case '@':
        if (strchr(frontend,'/')!=0)
          return setup_front_unix_socket(frontend+1,f);
        else
          return setup_front_inet_socket(frontend+1,f);
      case '=':
        return setup_back_fds(frontend+1,f);
    }

  return setup_front_tty(frontend,f);
}



/* Let me pass in device name, settings, new tty to create,
 * output file, verbose mode.
 */

void usage(char *name)
{
  fprintf(stderr,"Usage: %s [-V] [-qvl] [-s back-set] [-o output-file]\n"
                "       %*s [-p pty-dev] [-t tty-dev]\n"
                "       %*s [-m [pty-owner,[pty-group,]]pty-mode]\n"
                "       %*s [-u uid] [-g gid] [-/ chroot-dir]\n"
                "       %*s  back-device front-device\n"
"\tback-device\tUse back-device as the device to intercept\n"
           "\t\t/path\t\tTTY dev is at /path\n"
           "\t\t@/path\t\tSocket is at /path\n"
           "\t\t@host:port\tInet socket is at host:port\n"
           "\t\t!prog\t\tRun prog for backend\n"
           "\t\t=rfd[,wfd]\tUse file descriptors\n"

"\tfront-device\tUse front-device as the device applications connect to\n"
           "\t\t/path\t\tCreate symlink at /path\n"
           "\t\t@/path\t\tSocket at /path\n"
           "\t\t@host:port\tInet socket at host:port\n"
           "\t\t=rfd[,wfd]\tUse file descriptors\n"

"\t\t\t'-' to prevent creating a front-device.\n"
"\t\t\tDoesn't currently do anything.\n"
"\t-l\t\tLine-buffer output\n"
"\t-o output-file\tWrite intercepted data to output-file\n"
"\t-s back-stty\tUse given settings to set up back-device\n"
"\t\t\tThese settings are passed directly to stty(1).\n"
"\t-m pty-mode\tSpecify permissions for the new pty\n"
"\t\t\tFormat is [pty-owner,[pty-group,]]pty-mode]\n"
"\t-u uid\tSwitch to given uid after setting up (must be root)\n"
"\t-g gid\tSwitch to given gid after setting up (must be root)\n"
"-/ chroot-dir\tchroot(2) to given dir after setting up (must be root)\n"
"\t-q\t\tActivate quiet mode\n"
"\t-v\t\tActivate verbose mode\n"
"\t-V\t\tPrint version number then exit\n"
"\t-p pty-dev\tFull path to pty device for front-end (used internally)\n"
"\t-t tty-dev\tFull path to tty device for front-end (used externally)\n"
,name,
(int)strlen(name),"",
(int)strlen(name),"",
(int)strlen(name),"",
(int)strlen(name),"");
}

extern char *optarg;
extern int optind;

int main (int argc, char *argv[])
{
  fd_set readset;
  int n, sel;
  int c, errflg=0;
  int backfd[2], frontfd[2];
  struct sigaction sigact;
  sigset_t sigmask;
  char *scratch, *next_scratch;

  /* Set default options */
  outfile = stdout;

  /* Process options */
  while ((c = getopt(argc, argv, "Vlqvs:o:p:t:m:u:g:/:")) != EOF)
    switch (c) {
      case 'q':
	quiet=1;
	break;
      case 'v':
        verbose = 1;
        break;
      case 'l':
        linebuff = 1;
        break;
      case 'p':
        opt_ptyname = optarg;
        break;
      case 't':
        opt_ttyname = optarg;
        break;
      case 'm':
	/* mode for pty: [user,[group,]]mode */
	scratch = strdup(optarg);
	if ((next_scratch = strchr(scratch,',')) != NULL) {
	  /* Username */
	  *next_scratch = '\0';
	  next_scratch++;

	  frontend_owner = find_uid(scratch);

	  scratch = next_scratch;

	  if ((next_scratch = strchr(scratch,',')) != NULL)
	  {
	    /* Group */
	    *next_scratch = '\0';
	    next_scratch++;
	    
	    frontend_group = find_gid(scratch);

	    scratch = next_scratch;
	  }
	}
	frontend_mode = strtol(scratch,NULL,8);
	break;
      case 'u':
	switch_uid = find_uid(optarg);
	break;
      case 'g':
	switch_gid = find_gid(optarg);
	break;
      case '/':
	switch_root = strdup(optarg);
	break;
      case 's':
        settings = optarg;
        break;
      case 'o':
        outfilename=optarg;
        if ( (outfile = fopen(outfilename,"w")) == NULL)
          errorf("Couldn't open output file '%s' for write: %s\n",outfilename,strerror(errno));
        break;
      case 'V':
	puts(VERSION);
	exit(0);
      case '?':
      default:
        errflg++;
        break;
    }

  if (errflg || ((argc-optind) < 1) || ((argc-optind) > 2)) {
    usage(argv[0]);
    exit (2);
  }

  /* Process the two non-flag options */
  backend = argv[optind];
  if ((argc-optind) == 2)
    frontend = argv[optind+1];
        
  if (strcmp(frontend,"-") == 0)
    frontend = NULL;
  if (linebuff)
    setlinebuf(outfile);

  atexit (closedown);
  /* Do some initialization */
  stty_initstore();

  /* Setup backend */
  if (setup_backend(backfd) < 0)
    errorf ("select failed. errno = %d\n", errno);

  /* Setup frontend */
  if ((setup_frontend(frontfd)) < 0)
    errorf("setup_frontend failed: %s\n",strerror(errno));

  /* Drop privileges if we've been asked to */
  if (switch_root)
  {
    if (chroot(switch_root) != 0)
      errorf("chroot(%s) failed: %s\n",switch_root,strerror(errno));
  }
  if (switch_gid != -1)
  {
    if (setgroups(1,&switch_gid) == -1)
      errorf("setgroups(1,[%d]) failed: %s\n",switch_gid, strerror(errno));
    if (setregid(switch_gid, switch_gid) == -1)
      errorf("setregid(%d,%d) failed: %s\n",switch_gid,switch_gid);
    if (getgid() != switch_gid)
      errorf("setregid succeeded, but we're the wrong gid!");
    if (getegid() != switch_gid)
      errorf("setregid succeeded, but we're the wrong effective gid!");
  }
  if (switch_uid != -1)
  {
    if (setreuid(switch_uid, switch_uid) == -1)
      errorf("setreuid(%d,%d) failed: %s\n",switch_uid,switch_uid,strerror(errno));
    if (getuid() != switch_uid)
      errorf("setreuid succeeded, but we're the wrong uid!");
    if (geteuid() != switch_uid)
      errorf("setregid succeeded, but we're the wrong effective uid!");
  }

  
  /* calc (initial) max file descriptor to use in select() */
  fdmax = max(backfd[0], frontfd[0]);

  /* Set up signal handlers and such */
  sigemptyset(&sigmask);
  memset(&sigact,0,sizeof sigact);
  sigact.sa_handler = sigdeath;
  sigact.sa_mask = sigmask;

  sigaction(SIGHUP,&sigact,NULL);
  sigaction(SIGINT,&sigact,NULL);
  sigaction(SIGQUIT,&sigact,NULL);
  sigaction(SIGPIPE,&sigact,NULL);
  sigaction(SIGTERM,&sigact,NULL);

  sigact.sa_handler = sigchld;
  sigaction(SIGCHLD,&sigact,NULL);

  while (!please_die_now)
  {
    do
    {
      FD_ZERO (&readset);
      FD_SET (backfd[0], &readset);
      FD_SET (frontfd[0], &readset);
                        
      sel = select(fdmax + 1, &readset, NULL, NULL, NULL);
    }
    while (sel == -1 && errno == EINTR && !please_die_now);
    if (sel == -1 && errno != EINTR)
      errorf ("select failed. errno = %d\n", errno);
    else if (please_die_now)
      break;

    if (FD_ISSET(backfd[0], &readset))
    {
      if ((n = read(backfd[0], buff, BUFF_SIZE)) == 0)
      {
	/* Serial port has closed.  This doesn't really make sense for
	 * a real serial port, but for sockets and programs, probably
	 * we should just exit.
	 */
	if (!quiet)
	  errorf("Backend device was closed.\n");
	break;
      }
      else if (n < 0)
      {
	if ( (errno != EAGAIN) && (errno != EINTR) )
	{
	  errorf("Error reading from backend device: %s\n",strerror(errno));
	}
	break;
      }
      else
      {
	/* We should handle this better.  FIX */
        if (write (frontfd[1], buff, n) != n)
	  errorf("Error writing to frontend device: %s\n",strerror(errno));
        if (!quiet)
	  dumpbuff(1,buff,n);
      }
    }

    if (please_die_now)
      break;

    if (FD_ISSET(frontfd[0], &readset))
    {
      if ((n = read(frontfd[0], buff, BUFF_SIZE)) == 0)
      {
        if (listenfd)
        {
          if (close(frontfd[0]) < 0)
            errorf("Couldn't close old frontfd: %s\n",strerror(errno));
          if ((frontfd[0]=frontfd[1]=accept(listenfd,NULL,NULL)) < 0)
            errorf("Couldn't accept new socket connection: %s\n",strerror(errno));
        }
          
      }
      else if (n <= 0)
      {
	if ( (errno == EAGAIN) || (errno == EINTR) )
	{
	  /* No real error */
	}
	else
	{
	  errorf("Error reading from frontend device: %s\n",strerror(errno));
	}
      }
      else
      {
        if (write (backfd[1], buff, n) != n)
	  errorf("Error writing to backend device: %s\n",strerror(errno));
	if (!quiet)
	  dumpbuff(0,buff,n);
      }
    }

  }
  stty_orig();
  exit(0);
}
