// Mutual exclusion lock.
struct spinlock {
  uint locked; // Is the lock held?

  // For debugging:
  char *name;      // Name of lock.
  struct cpu *cpu; // The cpu holding the lock.
};

// Reader-writer spin lock with writer priority.
struct rwspinlock {
  uint readers;         // Number of readers holding the lock.
  uint writer;          // Non-zero if a writer is holding the lock.
  uint pending_writers; // Number of writers waiting to acquire.
};
