// Welcome to server land. Population: you

#include <comp421/yalnix.h>
#include <comp421/filesystem.h>
#include <string.h>
#include "messagetypes.h"

// The main loop that runs the server
// As of now, it's set only for basic messages.
int main(int argc, char *argv[]) {
    int status;
    struct test_msg msg;
    int sender_pid;
    
    TracePrintf(0, "YFS: Server starting\n");
    
    // Register as the file server w/ new kernel calls
    status = Register(FILE_SERVER);
    if (status == ERROR) {
        TracePrintf(0, "YFS: Failed to register as FILE_SERVER\n");
        Exit(1);
    }
    
    // Fork and execute first client if it's in the arguments
    if (argc > 1) {
        int child_pid = Fork();
        if (child_pid == 0) {
            // Child process
            Exec(argv[1], &argv[1]);
            // According to the docs, we can assume this next bit never happens.
            // But I'm paranoid.
            Exit(1);
        } else if (child_pid == ERROR) {
            TracePrintf(0, "YFS: Failed to fork first client\n");
        }
    }
    
    // Non-child server loop
    while (1) {
        TracePrintf(1, "YFS: Waiting for a message...\n");
        
        /* Receive a message */
        sender_pid = Receive(&msg);
        if (sender_pid == ERROR) {
            TracePrintf(0, "YFS: Error receiving message\n");
            continue;
        }
        
        TracePrintf(1, "YFS: Received message type %d from PID %d\n", 
                   msg.type, sender_pid);
        
        /* Process the message based on its type */
        // This will presumably get much bigger?
        switch (msg.type) {
            case TEST_MSG:
                // Just echo back the message for now
                // It says to be careful about using strcpy, but I believe
                // this isn't one of those cases.
                strcpy(msg.data, "Hello from YFS server!");
                break;
                
            default:
                TracePrintf(0, "YFS: Unknown message type %d\n", msg.type);
                strcpy(msg.data, "Unknown message type");
                break;
        }
        
        // Send reply back to the client
        msg.pid = 0; /* Server PID */
        status = Reply(&msg, sender_pid);
        if (status == ERROR) {
            TracePrintf(0, "YFS: Error replying to client %d\n", sender_pid);
        }
    }
    
    return 0; // This shouldn't happen
}