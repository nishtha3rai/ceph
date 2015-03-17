// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "gtest/gtest.h"
#ifndef GTEST_IS_THREADSAFE
#error "!GTEST_IS_THREADSAFE"
#endif

#include "include/cephfs/libcephfs.h"
#include <errno.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/xattr.h>

#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <sys/mman.h>

#ifdef __linux__
#include <limits.h>
#endif

// Startup common: create and mount ceph fs
#define STARTUP_CEPH() do {				\
    ASSERT_EQ(0, ceph_create(&cmount, NULL));		\
    ASSERT_EQ(0, ceph_conf_parse_env(cmount, NULL));	\
    ASSERT_EQ(0, ceph_conf_read_file(cmount, NULL));	\
    ASSERT_EQ(0, ceph_mount(cmount, NULL));		\
  } while(0)

// Cleanup common: unmount and release ceph fs
#define CLEANUP_CEPH() do {			\
    ASSERT_EQ(0, ceph_unmount(cmount));		\
    ASSERT_EQ(0, ceph_release(cmount));		\
  } while(0)

static const mode_t fileMode = S_IRWXU | S_IRWXG | S_IRWXO;

// Default wait time for normal and "slow" operations
static const long waitMs = 1 * 1000;
static const long waitSlowMs = 10 * 1000;

// Get the absolute struct timespec reference from now + 'ms' milliseconds
static const struct timespec* abstime(struct timespec &ts, long ms) {
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    abort();
  }
  ts.tv_nsec += ms * 1000000;
  ts.tv_sec += ts.tv_nsec / 1000000000;
  ts.tv_nsec %= 1000000000;
  return &ts;
}

/* Basic locking */

TEST(LibCephFS, BasicLocking) {
  struct ceph_mount_info *cmount = NULL;
  STARTUP_CEPH();

  char c_file[1024];
  sprintf(c_file, "/flock_test_%d", getpid());
  const int fd = ceph_open(cmount, c_file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 

  // Lock exclusively twice
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, 42));
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, 43));
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, 44));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, 42));

  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, 43));
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, 44));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, 43));

  // Lock shared three times
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH, 42));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH, 43));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH, 44));
  // And then attempt to lock exclusively
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, 45));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, 42));
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, 45));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, 44));
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, 45));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, 43));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, 45));
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, 42));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, 45));

  // Lock shared with upgrade to exclusive (POSIX) 
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH, 42));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, 42));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, 42));

  // Lock exclusive with downgrade to shared (POSIX) 
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, 42));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH, 42));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, 42));

  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}

/* Locking in different threads */

// Used by ConcurrentLocking test
struct str_ConcurrentLocking {
  const char *file;
  struct ceph_mount_info *cmount;  // !NULL if shared
  sem_t sem;
  sem_t semReply;
};

// Wakeup main (for (N) steps)
#define PING_MAIN() ASSERT_EQ(0, sem_post(&s.sem))
// Wait for main to wake us up (for (RN) steps)
#define WAIT_MAIN() \
  ASSERT_EQ(0, sem_timedwait(&s.semReply, abstime(ts, waitSlowMs)))

// Wakeup worker (for (RN) steps)
#define PING_WORKER() ASSERT_EQ(0, sem_post(&s.semReply))
// Wait for worker to wake us up (for (N) steps)
#define WAIT_WORKER() \
  ASSERT_EQ(0, sem_timedwait(&s.sem, abstime(ts, waitSlowMs)))
// Worker shall not wake us up (for (N) steps)
#define NOT_WAIT_WORKER() \
  ASSERT_EQ(-1, sem_timedwait(&s.sem, abstime(ts, waitMs)))

// Do twice an operation
#define TWICE(EXPR) do {			\
    EXPR;					\
    EXPR;					\
  } while(0)

/* Locking in different threads */

// Used by ConcurrentLocking test
static void thread_ConcurrentLocking(str_ConcurrentLocking& s) {
  struct ceph_mount_info *const cmount = s.cmount;
  struct timespec ts;

  const int fd = ceph_open(cmount, s.file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 
  PING_MAIN(); // (1)

  ASSERT_EQ(-EWOULDBLOCK,
	    ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, pthread_self()));
  PING_MAIN(); // (2)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, pthread_self()));
  PING_MAIN(); // (3)

  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));
  PING_MAIN(); // (4)

  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH, pthread_self()));
  PING_MAIN(); // (5)

  WAIT_MAIN(); // (R1)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));
  PING_MAIN(); // (6)

  WAIT_MAIN(); // (R2)
  PING_MAIN(); // (7)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, pthread_self()));
  PING_MAIN(); // (8)

  WAIT_MAIN(); // (R3)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));
  PING_MAIN(); // (9)
}

// Used by ConcurrentLocking test
static void* thread_ConcurrentLocking_(void *arg) {
  str_ConcurrentLocking *const s =
    reinterpret_cast<str_ConcurrentLocking*>(arg);
  thread_ConcurrentLocking(*s);
  return NULL;
}

TEST(LibCephFS, ConcurrentLocking) {
  const pid_t mypid = getpid();
  struct ceph_mount_info *cmount;
  STARTUP_CEPH();

  char c_file[1024];
  sprintf(c_file, "/flock_test_%d", mypid);
  const int fd = ceph_open(cmount, c_file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 

  // Lock
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, pthread_self()));

  // Start locker thread
  pthread_t thread;
  struct timespec ts;
  str_ConcurrentLocking s = { c_file, cmount };
  ASSERT_EQ(0, sem_init(&s.sem, 0, 0));
  ASSERT_EQ(0, sem_init(&s.semReply, 0, 0));
  ASSERT_EQ(0, pthread_create(&thread, NULL, thread_ConcurrentLocking_, &s));
  // Synchronization point with thread (failure: thread is dead)
  WAIT_WORKER(); // (1)

  WAIT_WORKER(); // (2)
  // Shall not have lock immediately
  NOT_WAIT_WORKER(); // (3)

  // Unlock
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));

  // Shall have lock
  // Synchronization point with thread (failure: thread is dead)
  WAIT_WORKER(); // (3)

  // Synchronization point with thread (failure: thread is dead)
  WAIT_WORKER(); // (4)

  // Wait for thread to share lock
  WAIT_WORKER(); // (5)
  ASSERT_EQ(-EWOULDBLOCK,
	    ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, pthread_self()));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, pthread_self()));

  // Wake up thread to unlock shared lock
  PING_WORKER(); // (R1)
  WAIT_WORKER(); // (6)

  // Now we can lock exclusively
  // Upgrade to exclusive lock (as per POSIX)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, pthread_self()));

  // Wake up thread to lock shared lock
  PING_WORKER(); // (R2)

  WAIT_WORKER(); // (7)
  // Shall not have lock immediately
  NOT_WAIT_WORKER(); // (8)

  // Release lock ; thread will get it
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));
  WAIT_WORKER(); // (8)

  // We no longer have the lock
  ASSERT_EQ(-EWOULDBLOCK,
	    ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, pthread_self()));
  ASSERT_EQ(-EWOULDBLOCK,
	    ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, pthread_self()));

  // Wake up thread to unlock exclusive lock
  PING_WORKER(); // (R3)
  WAIT_WORKER(); // (9)

  // We can lock it again
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, pthread_self()));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));

  // Cleanup
  void *retval = (void*) (uintptr_t) -1;
  ASSERT_EQ(0, pthread_join(thread, &retval));
  ASSERT_EQ(NULL, retval);
  ASSERT_EQ(0, sem_destroy(&s.sem));
  ASSERT_EQ(0, sem_destroy(&s.semReply));
  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}

TEST(LibCephFS, ThreesomeLocking) {
  const pid_t mypid = getpid();
  struct ceph_mount_info *cmount;
  STARTUP_CEPH();

  char c_file[1024];
  sprintf(c_file, "/flock_test_%d", mypid);
  const int fd = ceph_open(cmount, c_file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 

  // Lock
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, pthread_self()));

  // Start locker thread
  pthread_t thread[2];
  struct timespec ts;
  str_ConcurrentLocking s = { c_file, cmount };
  ASSERT_EQ(0, sem_init(&s.sem, 0, 0));
  ASSERT_EQ(0, sem_init(&s.semReply, 0, 0));
  ASSERT_EQ(0, pthread_create(&thread[0], NULL, thread_ConcurrentLocking_, &s));
  ASSERT_EQ(0, pthread_create(&thread[1], NULL, thread_ConcurrentLocking_, &s));
  // Synchronization point with thread (failure: thread is dead)
  TWICE(WAIT_WORKER()); // (1)

  TWICE(WAIT_WORKER()); // (2)
  // Shall not have lock immediately
  NOT_WAIT_WORKER(); // (3)

  // Unlock
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));

  // Shall have lock
  TWICE(// Synchronization point with thread (failure: thread is dead)
	WAIT_WORKER(); // (3)
	
	// Synchronization point with thread (failure: thread is dead)
	WAIT_WORKER()); // (4)
  
  // Wait for thread to share lock
  TWICE(WAIT_WORKER()); // (5)
  ASSERT_EQ(-EWOULDBLOCK,
	    ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, pthread_self()));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, pthread_self()));

  // Wake up thread to unlock shared lock
  TWICE(PING_WORKER(); // (R1)
	WAIT_WORKER()); // (6)

  // Now we can lock exclusively
  // Upgrade to exclusive lock (as per POSIX)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, pthread_self()));

  TWICE(  // Wake up thread to lock shared lock
	PING_WORKER(); // (R2)
	WAIT_WORKER()); // (7)

  // Shall not have lock immediately
  NOT_WAIT_WORKER(); // (8)
  
  // Release lock ; thread will get it
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));
  TWICE(WAIT_WORKER(); // (8)
	
	// We no longer have the lock
	ASSERT_EQ(-EWOULDBLOCK,
		  ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, pthread_self()));
	ASSERT_EQ(-EWOULDBLOCK,
		  ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, pthread_self()));
	
	// Wake up thread to unlock exclusive lock
	PING_WORKER(); // (R3)
	WAIT_WORKER(); // (9)
	);
  
  // We can lock it again
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, pthread_self()));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, pthread_self()));

  // Cleanup
  void *retval = (void*) (uintptr_t) -1;
  ASSERT_EQ(0, pthread_join(thread[0], &retval));
  ASSERT_EQ(NULL, retval);
  ASSERT_EQ(0, pthread_join(thread[1], &retval));
  ASSERT_EQ(NULL, retval);
  ASSERT_EQ(0, sem_destroy(&s.sem));
  ASSERT_EQ(0, sem_destroy(&s.semReply));
  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}

/* Locking in different processes */

// Used by ConcurrentLocking test
static void process_ConcurrentLocking(str_ConcurrentLocking& s) {
  const pid_t mypid = getpid();

  PING_MAIN(); // (1)

  struct ceph_mount_info *cmount = NULL;
  struct timespec ts;

  STARTUP_CEPH();
  s.cmount = cmount;

  const int fd = ceph_open(cmount, s.file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 
  WAIT_MAIN(); // (R0)

  ASSERT_EQ(-EWOULDBLOCK,
	    ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, mypid));
  PING_MAIN(); // (2)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, mypid));
  PING_MAIN(); // (3)

  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));
  PING_MAIN(); // (4)

  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH, mypid));
  PING_MAIN(); // (5)

  WAIT_MAIN(); // (R1)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));
  PING_MAIN(); // (6)

  WAIT_MAIN(); // (R2)
  PING_MAIN(); // (7)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, mypid));
  PING_MAIN(); // (8)

  WAIT_MAIN(); // (R3)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));
  PING_MAIN(); // (9)

  CLEANUP_CEPH();

  ASSERT_EQ(0, sem_destroy(&s.sem));
  ASSERT_EQ(0, sem_destroy(&s.semReply));
  exit(EXIT_SUCCESS);
}

TEST(LibCephFS, InterProcessLocking) {
  // Process synchronization
  char c_file[1024];
  const pid_t mypid = getpid();
  sprintf(c_file, "/flock_test_%d", mypid);

  // Note: the semaphores MUST be on a shared memory segment
  str_ConcurrentLocking *const shs =
    reinterpret_cast<str_ConcurrentLocking*>
    (mmap(0, sizeof(*shs), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
	  -1, 0));
  str_ConcurrentLocking &s = *shs;
  s.file = c_file;
  ASSERT_EQ(0, sem_init(&s.sem, 1, 0));
  ASSERT_EQ(0, sem_init(&s.semReply, 1, 0));

  // Start locker process
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    process_ConcurrentLocking(s);
    exit(EXIT_FAILURE);
  }

  struct timespec ts;
  struct ceph_mount_info *cmount;
  STARTUP_CEPH();

  const int fd = ceph_open(cmount, c_file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 

  // Lock
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, mypid));

  // Synchronization point with process (failure: process is dead)
  WAIT_WORKER(); // (1)
  PING_WORKER(); // (R0)

  WAIT_WORKER(); // (2)
  // Shall not have lock immediately
  NOT_WAIT_WORKER(); // (3)

  // Unlock
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));

  // Shall have lock
  // Synchronization point with process (failure: process is dead)
  WAIT_WORKER(); // (3)

  // Synchronization point with process (failure: process is dead)
  WAIT_WORKER(); // (4)

  // Wait for process to share lock
  WAIT_WORKER(); // (5)
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, mypid));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, mypid));

  // Wake up process to unlock shared lock
  PING_WORKER(); // (R1)
  WAIT_WORKER(); // (6)

  // Now we can lock exclusively
  // Upgrade to exclusive lock (as per POSIX)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, mypid));

  // Wake up process to lock shared lock
  PING_WORKER(); // (R2)

  WAIT_WORKER(); // (7)
  // Shall not have lock immediately
  NOT_WAIT_WORKER(); // (8)

  // Release lock ; process will get it
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));
  WAIT_WORKER(); // (8)

  // We no longer have the lock
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, mypid));
  ASSERT_EQ(-EWOULDBLOCK, ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, mypid));

  // Wake up process to unlock exclusive lock
  PING_WORKER(); // (R3)
  WAIT_WORKER(); // (9)

  // We can lock it again
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, mypid));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));

  // Wait pid
  int status;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_EQ(EXIT_SUCCESS, status);

  // Cleanup
  ASSERT_EQ(0, sem_destroy(&s.sem));
  ASSERT_EQ(0, sem_destroy(&s.semReply));
  ASSERT_EQ(0, munmap(shs, sizeof(*shs)));
  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}

TEST(LibCephFS, ThreesomeInterProcessLocking) {
  // Process synchronization
  char c_file[1024];
  const pid_t mypid = getpid();
  sprintf(c_file, "/flock_test_%d", mypid);

  // Note: the semaphores MUST be on a shared memory segment
  str_ConcurrentLocking *const shs =
    reinterpret_cast<str_ConcurrentLocking*>
    (mmap(0, sizeof(*shs), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
	  -1, 0));
  str_ConcurrentLocking &s = *shs;
  s.file = c_file;
  ASSERT_EQ(0, sem_init(&s.sem, 1, 0));
  ASSERT_EQ(0, sem_init(&s.semReply, 1, 0));

  // Start locker processes
  pid_t pid[2];
  pid[0] = fork();
  ASSERT_GE(pid[0], 0);
  if (pid[0] == 0) {
    process_ConcurrentLocking(s);
    exit(EXIT_FAILURE);
  }
  pid[1] = fork();
  ASSERT_GE(pid[1], 0);
  if (pid[1] == 0) {
    process_ConcurrentLocking(s);
    exit(EXIT_FAILURE);
  }

  struct timespec ts;
  struct ceph_mount_info *cmount;
  STARTUP_CEPH();

  const int fd = ceph_open(cmount, c_file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 

  // Lock
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, mypid));

  // Synchronization point with process (failure: process is dead)
  TWICE(WAIT_WORKER()); // (1)
  TWICE(PING_WORKER()); // (R0)

  TWICE(WAIT_WORKER()); // (2)
  // Shall not have lock immediately
  NOT_WAIT_WORKER(); // (3)

  // Unlock
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));

  // Shall have lock
  TWICE(// Synchronization point with process (failure: process is dead)
	WAIT_WORKER(); // (3)
	
	// Synchronization point with process (failure: process is dead)
	WAIT_WORKER()); // (4)
  
  // Wait for process to share lock
  TWICE(WAIT_WORKER()); // (5)
  ASSERT_EQ(-EWOULDBLOCK,
	    ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, mypid));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, mypid));

  // Wake up process to unlock shared lock
  TWICE(PING_WORKER(); // (R1)
	WAIT_WORKER()); // (6)

  // Now we can lock exclusively
  // Upgrade to exclusive lock (as per POSIX)
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX, mypid));

  TWICE(  // Wake up process to lock shared lock
	PING_WORKER(); // (R2)
	WAIT_WORKER()); // (7)

  // Shall not have lock immediately
  NOT_WAIT_WORKER(); // (8)
  
  // Release lock ; process will get it
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));
  TWICE(WAIT_WORKER(); // (8)
	
	// We no longer have the lock
	ASSERT_EQ(-EWOULDBLOCK,
		  ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, mypid));
	ASSERT_EQ(-EWOULDBLOCK,
		  ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, mypid));
	
	// Wake up process to unlock exclusive lock
	PING_WORKER(); // (R3)
	WAIT_WORKER(); // (9)
	);
  
  // We can lock it again
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, mypid));
  ASSERT_EQ(0, ceph_flock(cmount, fd, LOCK_UN, mypid));

  // Wait pids
  int status;
  ASSERT_EQ(pid[0], waitpid(pid[0], &status, 0));
  ASSERT_EQ(EXIT_SUCCESS, status);
  ASSERT_EQ(pid[1], waitpid(pid[1], &status, 0));
  ASSERT_EQ(EXIT_SUCCESS, status);

  // Cleanup
  ASSERT_EQ(0, sem_destroy(&s.sem));
  ASSERT_EQ(0, sem_destroy(&s.semReply));
  ASSERT_EQ(0, munmap(shs, sizeof(*shs)));
  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}