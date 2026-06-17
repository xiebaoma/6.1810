struct stat;

#define SBRK_ERROR ((char *)-1)

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
char* sbrklazy(int);
char* sys_sbrk(int, int);
int sleep(int);
int pause(int);
int uptime(void);
int symlink(const char*, const char*);
void *mmap(void *, int, int, int, int, int);
int munmap(void *, int);
int interpose(int, const char *);
int kpgtbl(void);
int sigalarm(int, void (*handler)());
int sigreturn(void);
int rwlktest(int);
int bind(int);
int unbind(int);
int send(int, int, int, char *, int);
int recv(int, uint *, ushort *, char *, int);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void fprintf(int, const char*, ...) __attribute__ ((format (printf, 2, 3)));
void printf(const char*, ...) __attribute__ ((format (printf, 1, 2)));
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);

// umalloc.c
void* malloc(uint);
void free(void*);
