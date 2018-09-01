#if !defined __TIMEF__
#define __TIMEF__

struct timespec * timespec_diff(struct timespec *start, struct timespec *stop, struct timespec *result);
uint64_t timespec_to_uint64(struct timespec *input) ;

#endif
