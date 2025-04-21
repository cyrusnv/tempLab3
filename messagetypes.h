#ifndef YFS_PROTOCOL_H
#define YFS_PROTOCOL_H

// According to the Internet, this is how we can test 
// struct sizes easily.
#include <assert.h>

// This is how we'll define all of our message types, I think
typedef enum {
    TEST_MSG = 1,
} msg_type_t;

struct test_msg {
    int type;
    int pid;
    char data[24];
};

// Not sure if/how exactly this will work.
// This isn't working; I'll try another way of testing sizes later
//static_assert(sizeof(struct test_msg) == 32, "your structs gotta be 32 bytes man");

#endif