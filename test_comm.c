/* test_comm.c - I'm just testing the basics here. */
#include <comp421/iolib.h>
#include <comp421/yalnix.h>
#include <comp421/filesystem.h>
#include "messagetypes.h"

/* External function defined in iolib.c */
extern int TestYFSCommunication();

int main(int argc, char *argv[]) {
    int status;
    (void)argc;
    (void)argv;
    
    TracePrintf(0, "Client: Starting communication test\n");
    
    status = TestYFSCommunication();
    if (status == ERROR) {
        TracePrintf(0, "Client: Communication test failed\n");
        Exit(1);
    }
    
    TracePrintf(0, "Client: Communication test succeeded\n");
    
    return 0;
}