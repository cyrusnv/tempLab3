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
void testInodeAllocation();
void testInodeDeallocation();
void testBlockAllocation();
void testBlockDeallocation();
int inInodeCache(int inodenum);
int addInodeToCache(struct inode *node, int nodenum);
int editInodeInCache(struct inode *node, int nodenum);
int deallocInodeInCache(int nodenum);
int inBlockCache(int bnum);
int editBlockInCache(int bnum, void *buff);
int addBlockToCache(int bnum, void *buff);
int deallocBlockInCache(int bnum);

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
    testInodeAllocation();
    testInodeDeallocation();
    testBlockAllocation();
    
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
    isdbtaken = (int *)malloc(sizeof(int) * header->num_blocks);
    memset(isdbtaken, 0, sizeof(int) * header->num_blocks);
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
            int fileblockcount = (node->size + BLOCKSIZE - 1) / BLOCKSIZE; // Round up for the last partially-filled block
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

    // Testing why block 7 is not free, but block six is. It's per the root node spec. We're fine.
    // struct inode* root = readInode(ROOTINODE); // ROOTINODE is defined as 1
    // TracePrintf(0, "Root inode: type=%d, size=%d, direct[0]=%d\n", 
    //        root->type, root->size, root->direct[0]);
    // free(root);

    // Testing post-setup.
    //printisdbtaken();

    return 1;
}








/* BLOCK MANAGEMENT FUNCTIONS */

/* 
 * Note that this function does not check that the buffer is the
 * correct size.
 */
int allocBlock(void *buff) {

    int newblocknum = -1;

    // Find a free block
    for (int i = 1; i < header->num_blocks; i++) {
        if (!isdbtaken[i]) {
            newblocknum = i;
            break;
        }
    }

    if (newblocknum == -1) {
        TracePrintf(1, "WARNING: NO MORE FREE BLOCKS.\n");
        return -1;
    }

    if (inBlockCache(newblocknum)) {
        editBlockInCache(newblocknum, buff);
    } else {
        // int block = blockFromInode(newblocknum);
        // int blockpos = inodePosInBlock(newblocknum);

        if (sizeof(buff) != SECTORSIZE) {
            TracePrintf(1, "WARNING: BLOCKALLOC BUFFER SIZE WRONG.\n");
        }

        WriteSector(newblocknum, buff);
        addBlockToCache(newblocknum, buff);
    }

    isdbtaken[newblocknum] = 1;

    return newblocknum;
}

int deallocBlock(int blocknum) {

    // I guess we should validate the block number.
    if (blocknum <= 0 || blocknum >= header->num_blocks) {
        TracePrintf(1, "WARNING: ATTEMPTING TO DEALLOC INVALID BLOCK NUMBER %d\n", blocknum);
    }

    if (inBlockCache(blocknum)) {
        deallocBlockInCache(blocknum);
    }

    isdbtaken[blocknum] = 0;

    return blocknum;
}

int inBlockCache(int bnum) {
    (void)bnum;
    return -1;
}

int editBlockInCache(int bnum, void *buff) {
    (void)bnum;
    (void)buff;
    return -1;
}

int addBlockToCache(int bnum, void *buff) {
    (void)bnum;
    (void)buff;
    return -1;
}

int deallocBlockInCache(int bnum) {
    (void)bnum;
    return -1;
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

int blockFromInode(int inodenum) {
    return (inodenum / inodes_per_block) + 1;
}

int inodePosInBlock(int inodenum) {
    return inodenum % inodes_per_block;
}

int allocInode(struct inode *newnode) {
    // Alright, here's the plan for this guy:
    // 1) Pick a free inode (linear scan, this will be efficient enough)
    // 2) Check if it's in the cache (dummy helper)
    // 3) If it is in the cache, edit it in the cache and mark it as dirty
    // 4) If it is not, do the whole read bit, write bit, put it in the cache
    // 5) Either way, once you're done, mark it as free.

    int newinodenum = -1;

    // Find a free inode
    for (int i = 1; i < header->num_inodes; i++) {
        if (!isinodetaken[i]) {
            newinodenum = i;
            break;
        }
    }

    if (newinodenum == -1) {
        TracePrintf(1, "WARNING: NO MORE FREE INODES.");
        return -1;
    }

    if (inInodeCache(newinodenum)) {
        editInodeInCache(newnode, newinodenum);
    } else {
        int block = blockFromInode(newinodenum);
        int blockpos = inodePosInBlock(newinodenum);

        void *buff = malloc(SECTORSIZE);
        ReadSector(block, buff);
        struct inode *blockbuff = (struct inode*)buff;

        // Change strictly the values that correspond to the specific inode we want to change
        blockbuff[blockpos].type = newnode->type;
        blockbuff[blockpos].nlink = newnode->nlink;
        blockbuff[blockpos].reuse = blockbuff[blockpos].reuse + 1;
        blockbuff[blockpos].size = newnode->size;
        for (int i = 0; i < NUM_DIRECT; i++) {
            blockbuff[blockpos].direct[i] = newnode->direct[i];
        }
        blockbuff[blockpos].indirect = newnode->indirect;

        WriteSector(block, buff);
        addInodeToCache(&blockbuff[blockpos], newinodenum);
        free(buff);
    }

    isinodetaken[newinodenum] = 1;

    return newinodenum;
}

int deallocInode(int nodenum) {
    // Hopefully, this looks very similar.

    if (inInodeCache(nodenum)) {
        deallocInodeInCache(nodenum);
    } else {
        int block = blockFromInode(nodenum);
        int blockpos = inodePosInBlock(nodenum);

        void *buff = malloc(SECTORSIZE);
        ReadSector(block, buff);
        struct inode *blockbuff = (struct inode*)buff;

        // Change strictly the values that correspond to the specific inode we want to change
        blockbuff[blockpos].type = INODE_FREE;
        // I'm not resetting other values; that's your job when you allocate.

        WriteSector(block, buff);
        addInodeToCache(&blockbuff[blockpos], nodenum);
        free(buff);
    }

    isinodetaken[nodenum] = 0;

    // HUGE TODO: YOU NEED TO DEALLOC DATA BLOCKS ASSOCIATED WITH INODE!

    return nodenum;
}

/* CACHE MANAGEMENT FUNCTIONS */

// Dummy function that, for now, always returns false
int inInodeCache(int inodenum) {
    (void)inodenum;
    return 0;
}

// Dummy function that, for now, should never be hit
int addInodeToCache(struct inode *node, int nodenum) {
    (void)node;
    (void)nodenum;
    return -1;
}

// Dummy function that, for now, should never be hit
int editInodeInCache(struct inode *node, int nodenum) {
    (void)node;
    (void)nodenum;
    return -1;
}

int deallocInodeInCache(int nodenum) {
    (void)nodenum;
    return -1;
}









/* INTERNAL TEST FUNCTIONS */

void printisdbtaken() {
    for(int i = 0; i < header->num_blocks; i++) {
        TracePrintf(5, "printisdbtaken: db %d has value %d.\n", i, isdbtaken[i]);
    }
}

// Testing inode allocation. This had better extend to data blocks, otherwise I've wasted way too much
// time on this.
void testInodeAllocation() {
    // Count free inodes before allocation
    int freeInodesBefore = 0;
    for (int i = 1; i < header->num_inodes; i++) {
        if (!isinodetaken[i]) {
            freeInodesBefore++;
        }
    }
    
    TracePrintf(0, "TEST: Free inodes before allocation: %d\n", freeInodesBefore);
    
    // Create a new inode to allocate (empty normal file)
    struct inode newNode;
    memset(&newNode, 0, sizeof(struct inode));
    newNode.type = INODE_REGULAR;
    newNode.nlink = 1;
    newNode.size = 0;
    
    // allocate
    int allocatedInum = allocInode(&newNode);
    
    if (allocatedInum == -1) {
        TracePrintf(0, "TEST: Inode allocation failed!\n");
        return;
    }
    
    TracePrintf(0, "TEST: Allocated inode number: %d\n", allocatedInum);
    
    // Count free inodes after allocation
    int freeInodesAfter = 0;
    for (int i = 1; i < header->num_inodes; i++) {
        if (!isinodetaken[i]) {
            freeInodesAfter++;
        }
    }
    
    TracePrintf(0, "TEST: Free inodes after allocation: %d\n", freeInodesAfter);
    
    // make sure we're only off by one
    if (freeInodesBefore != freeInodesAfter + 1) {
        TracePrintf(0, "TEST FAILED: Free inode count mismatch!\n");
    } else {
        TracePrintf(0, "TEST PASSED: Free inode count updated correctly\n");
    }
    
    if (isinodetaken[allocatedInum] != 1) {
        TracePrintf(0, "TEST FAILED: Allocated inode not marked as taken!\n");
    } else {
        TracePrintf(0, "TEST PASSED: Allocated inode marked as taken\n");
    }
    
    // read the inode back and verify contents
    struct inode *readNode = readInode(allocatedInum);
    
    if (readNode->type != INODE_REGULAR) {
        TracePrintf(0, "TEST FAILED: Inode type not set correctly! Expected %d, got %d\n", 
                   INODE_REGULAR, readNode->type);
    } else {
        TracePrintf(0, "TEST PASSED: Inode type set correctly\n");
    }
    
    if (readNode->nlink != 1) {
        TracePrintf(0, "TEST FAILED: Inode nlink not set correctly!\n");
    } else {
        TracePrintf(0, "TEST PASSED: Inode nlink set correctly\n");
    }
    
    // Verify reuse count was incremented (need to know original value for this to work ig)
    TracePrintf(0, "TEST INFO: Reuse count is now: %d\n", readNode->reuse);
    
    free(readNode);
}

// Testing inode deallocation. Hopefully this flows from the last guy.
void testInodeDeallocation() {
    // TODO: MAKE SURE THIS ALSO GETS RID OF DATA BLOCKS


    // Count free inodes before allocation
    int freeInodesBefore = 0;
    for (int i = 1; i < header->num_inodes; i++) {
        if (!isinodetaken[i]) {
            freeInodesBefore++;
        }
    }


    // Create a new inode to allocate (empty normal file)
    struct inode newNode;
    memset(&newNode, 0, sizeof(struct inode));
    newNode.type = INODE_REGULAR;
    newNode.nlink = 1;
    newNode.size = 0;
    
    // allocate
    int allocatedInum = allocInode(&newNode);
    
    // deallocate
    int deallocatedInum = deallocInode(allocatedInum);

    if (deallocatedInum != allocatedInum) {
        TracePrintf(5, "DEALLOCATION TEST: NODE NUMBERS DON'T MATCH\n");
    }

    // Count free inodes after deallocation
    int freeInodesAfter = 0;
    for (int i = 1; i < header->num_inodes; i++) {
        if (!isinodetaken[i]) {
            freeInodesAfter++;
        }
    }
    // Make sure before/after are the same
    if (freeInodesBefore != freeInodesAfter) {
        TracePrintf(5, "DEALLOCATION TEST: ALLOCATED INODE COUNTS DON'T MATCH\n");
    }

    // Check the type of the new inode
    if (readInode(deallocatedInum)->type != INODE_FREE) {
        TracePrintf(5, "DEALLOCATION TEST: DEALLOCATED INODE DOES NOT HAVE RIGHT TYPE");
    } else {
        TracePrintf(5, "DEALLOCATION TEST: I think it works :)\n");
    }
    
}

void testBlockAllocation() {
    // More or less the exact same test as inodes, now with blocks

    // Count free blocks before allocation
    int freeBlocksBefore = 0;
    for (int i = 1; i < header->num_blocks; i++) {
        if (!isdbtaken[i]) {
            freeBlocksBefore++;
        }
    }
    
    TracePrintf(0, "TEST: Free blocks before allocation: %d\n", freeBlocksBefore);
    
    // Create a test buffer to write
    void *testBuffer = malloc(SECTORSIZE);
    if (testBuffer == NULL) {
        TracePrintf(0, "TEST: Failed to allocate test buffer memory\n");
        return;
    }
    memset(testBuffer, 'A', SECTORSIZE); // Fill buffer with 'A's, which is the grade I want
    
    // Allocate the block
    int allocatedBlock = allocBlock(testBuffer);
    
    if (allocatedBlock == -1) {
        TracePrintf(0, "TEST: Block allocation failed!\n");
        free(testBuffer);
        return;
    }
    
    TracePrintf(0, "TEST: Allocated block number: %d\n", allocatedBlock);
    
    // Count free blocks after allocation
    int freeBlocksAfter = 0;
    for (int i = 1; i < header->num_blocks; i++) {
        if (!isdbtaken[i]) {
            freeBlocksAfter++;
        }
    }
    
    TracePrintf(0, "TEST: Free blocks after allocation: %d\n", freeBlocksAfter);
    
    // Make sure we're only off by one
    if (freeBlocksBefore != freeBlocksAfter + 1) {
        TracePrintf(0, "TEST FAILED: Free block count mismatch!\n");
    } else {
        TracePrintf(0, "TEST PASSED: Free block count updated correctly\n");
    }
    
    if (isdbtaken[allocatedBlock] != 1) {
        TracePrintf(0, "TEST FAILED: Allocated block not marked as taken!\n");
    } else {
        TracePrintf(0, "TEST PASSED: Allocated block marked as taken\n");
    }
    
    // Read the block back and verify contents
    void *readBuffer = malloc(SECTORSIZE);
    if (readBuffer == NULL) {
        TracePrintf(0, "TEST: Failed to allocate read buffer memory\n");
        free(testBuffer);
        return;
    }
    
    ReadSector(allocatedBlock, readBuffer);
    
    // Check if the data matches
    if (((char*)readBuffer)[0] != 'A') {
        TracePrintf(0, "TEST FAILED: Block content doesn't match what we wrote! Expected 'A', got '%c'\n", 
                   ((char*)readBuffer)[0]);
    } else {
        TracePrintf(0, "TEST PASSED: Block content matches what we wrote\n");
    }
    
    // Free memory
    free(testBuffer);
    free(readBuffer);
}