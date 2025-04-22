// Welcome to server land. Population: you

#include <comp421/yalnix.h>
#include <comp421/filesystem.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "messagetypes.h"

// Global variables
struct fs_header *header;
int inodes_per_block = BLOCKSIZE / INODESIZE; // note that this is -1 for the first block due to the header.
int *isinodetaken = NULL; // inode free list. 1 indicates taken, 0 free. Note that there is nothing in the 0th position, inodes begin at 1.
int *isdbtaken = NULL; // data block free list. 1 indicates taken, 0 free. Note that there is nothing in the 0th position, inodes begin at 1.
int dbcount = 0; // The number of data blocks in the file system.

// Function headers
int setupServer();
struct inode* readInode(int inumber);
void printisdbtaken();



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

    // Now that startup is beginning, and assuming the file system is formatted, so some initial setup
    setupServer();
    
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


// reads the initial sectors as required to set up the servers.
int setupServer() {

    // Alright, Cyrus. Time to think. What do you need to do on this initial read to get
    // a sense of the file system?

    // 1) Know how many inodes there are
    // 2) Know the structure of the fs_header
    // 3) Know how many free inodes you have (47)
    // 4) Set up your lists for keeping track of inodes
    // 5) Set up tracking of your free data blocks

    // Start by just stuffing all of this into your global header block
    void *b1buff = malloc(SECTORSIZE);
    // Should error handle here later
    ReadSector(1, b1buff);
    // Cast the header into the global variable.
    header = (struct fs_header*)b1buff;
    // This test completes.
    TracePrintf(5, "SETUP: header value test, number of inodes is %d\n", header->num_inodes);

    // A basic test of readInode, no longer needed.
    // struct inode *firstnode = readInode(1);
    // TracePrintf(5, "SETUP: readinode test, inode type is %d\n", firstnode->type);
    // firstnode->type = 1;
    // TracePrintf(5, "SETUP: second readinode test, inode type is %d\n", firstnode->type);

    // Set up the free inode list
    isinodetaken = (int *)malloc(sizeof(int) * header->num_inodes);
    // Note that the list needs to begin at 1, since there is no inode 0.
    for (int i = 1; i < header->num_inodes; i++) {
        // this implies that I need a read inode function, huh
        struct inode *node = readInode(i);
        if (node->type == INODE_FREE) {
            isinodetaken[i] = 0;
        } else {
            isinodetaken[i] = 1;
        }
        TracePrintf(5, "SETUP: free inode list loop, inode type is %d\n", node->type);
    }

    // Set up the free data block list
    dbcount = header->num_blocks - header->num_inodes;
    isdbtaken = (int *)malloc(sizeof(int) * dbcount);
    memset(isdbtaken, 0, sizeof(int) * dbcount); // initialize all value to zero... part of debugging...
    // Mark used data blocks as used
    // Start with the datablocks that the inodes are in (and the boot block)
    for (int i = 0; i < (header->num_inodes / inodes_per_block) + 1; i++) {
        isdbtaken[i] = 1;
    }
    // Now, iterate over the inodes themselves (again) and update their marked data blocks
    // (I understand that this extra iteration is not necessary, but do not care.)
    for (int i = 1; i < header->num_inodes; i++) {
        struct inode *node = readInode(i);
        if (node->size > 0) { //if the inode points to something taking up space
            int fileblockcount = (int)ceil(node->size / BLOCKSIZE); // Round up for the last partially-filled block
            // Case 1: we do not have to worry about indirect blocks
            if (fileblockcount <= (NUM_DIRECT)) {
                for (int j = 0; j < fileblockcount; j++) {
                    isdbtaken[node->direct[j]] = 1;
                }
            } else { // Case 2: we do have to worry about indirect blocks
                // Still mark the direct blocks
                for (int j = 0; j < NUM_DIRECT; j++) {
                    isdbtaken[node->direct[j]] = 1;
                }
                // And then also mark the layer of indirect blocks
                isdbtaken[node->indirect] = 1;
                int remainingblockcount = fileblockcount - NUM_DIRECT;

                // Cast the indirect block into an integer array, per doc spec
                void *indirect_buffer = malloc(BLOCKSIZE);
                ReadSector(node->indirect, indirect_buffer);
                int *indirect_blocks = (int *)indirect_buffer;

                for (int k = 0; k < remainingblockcount; k++) {
                    // iterate over the numbers in the array, and mark the blocks as taken.
                    if (indirect_blocks[k] > 0) {  // Make sure it's a valid block number lol
                        isdbtaken[indirect_blocks[k]] = 1;
                    }
                }
            }
        }
    }

    printisdbtaken();

    return 1;
}


/* INODE MANAGEMENT FUNCTIONS */

struct inode* readInode(int inumber) {
    if (inumber == 0) {
        TracePrintf(1, "WARNING: READINODE CALLED WITH INUMBER 0\n");
        // the first valid inode number is 1.
    }

    // Find the block in which the inode lives, that way you can read properly.
    int block = (inumber / inodes_per_block) + 1;
    int pos_in_block = inumber % inodes_per_block;

    void *buff = malloc(SECTORSIZE);
    ReadSector(block, buff);
    struct inode *blockbuff = (struct inode*)buff;

    struct inode *return_node = malloc(sizeof(struct inode));

    memcpy(return_node, &blockbuff[pos_in_block], sizeof(struct inode));
    free(buff);

    return return_node;
}

/* INTERNAL TEST FUNCTIONS */

void printisdbtaken() {
    for(int i = 0; i < dbcount; i++) {
        TracePrintf(5, "printisdbtaken: db %d has value %d.\n", i, isdbtaken[i]);
    }
}