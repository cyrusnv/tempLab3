/* iolib.c - Note that this is Cyrus's basic test implementation —probably should not be final */
#include <comp421/iolib.h>
#include <comp421/yalnix.h>
#include <comp421/filesystem.h>
#include <string.h>
#include "messagetypes.h"

// Test some communication, yeah?
int TestYFSCommunication() {
    struct test_msg msg;
    int status;
    
    // Put a message in our struct
    msg.type = TEST_MSG;
    msg.pid = GetPid();
    strcpy(msg.data, "Hello from client!");
    
    // Send the message to the server
    // Make the PID negative!!
    status = Send(&msg, -FILE_SERVER);
    if (status == ERROR) {
        return ERROR;
    }
    
    TracePrintf(0, "Client: Received reply with data: '%s'\n", msg.data);
    
    /* At this point, the message has been overwritten with the reply */
    /* Return 0 for success */
    return 0;
}

// Actual functions are not my job and we'll sort it out soon.