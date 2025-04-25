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
int *isblocktaken = NULL; // block free list. 1 indicates taken, 0 free. Includes boot block and inode blocks.
int dbcount = 0; // The number of data blocks in the file system.

// Function headers
int setupServer();
struct inode* readInode(int inumber);
void printisblocktaken();
void testInodeAllocation();
void testInodeDeallocation();
void testBlockAllocation();
void testBlockDeallocation();
void testSplitPath();
void test_path_resolution();
void test_lookup_in_directory();
int inInodeCache(int inodenum);
int addInodeToCache(struct inode *node, int nodenum);
int editInodeInCache(struct inode *node, int nodenum);
int deallocInodeInCache(int nodenum);
int inBlockCache(int bnum);
int editBlockInCache(int bnum, void *buff);
int addBlockToCache(int bnum, void *buff);
int deallocBlockInCache(int bnum);
char** pathStrToArray(char* path, int* num_components, int* is_absolute);
int lookup_in_directory(int dir_inode, char* component);

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
    testBlockDeallocation();
    testSplitPath();
    test_lookup_in_directory();
    test_path_resolution();
    
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
    isblocktaken = (int *)malloc(sizeof(int) * header->num_blocks);
    memset(isblocktaken, 0, sizeof(int) * header->num_blocks);
    // Mark used data blocks as used
    // Start with the datablocks that the inodes are in (and the boot block)
    for (int i = 0; i < (header->num_inodes / inodes_per_block) + 1; i++) {
        isblocktaken[i] = 1;
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
                    isblocktaken[node->direct[j]] = 1;
                }
            } else { // Case 2: we do have to worry about indirect blocks
                // Still mark the direct blocks
                for (int j = 0; j < NUM_DIRECT; j++) {
                    isblocktaken[node->direct[j]] = 1;
                }
                // And then also mark the layer of indirect blocks
                isblocktaken[node->indirect] = 1;
                int remainingblockcount = fileblockcount - NUM_DIRECT;

                // Cast the indirect block into an integer array, per doc spec
                void *indirect_buffer = malloc(BLOCKSIZE);
                ReadSector(node->indirect, indirect_buffer);
                int *indirect_blocks = (int *)indirect_buffer;

                for (int k = 0; k < remainingblockcount; k++) {
                    // iterate over the numbers in the array, and mark the blocks as taken.
                    if (indirect_blocks[k] > 0) {  // Make sure it's a valid block number lol
                        isblocktaken[indirect_blocks[k]] = 1;
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
    //printisblocktaken();

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
        if (!isblocktaken[i]) {
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

        WriteSector(newblocknum, buff);
        addBlockToCache(newblocknum, buff);
    }

    isblocktaken[newblocknum] = 1;

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

    isblocktaken[blocknum] = 0;

    return blocknum;
}

int inBlockCache(int bnum) {
    (void)bnum;
    return 0;
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

/* 
 * deallocates an inode and the data blocks in it. 
 * make sure that you don't deallocate an inode that's already free.
 * Not sure what would happen then. Probably nothing bad.
 */
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

        /* Free the direct and indirect blocks associated with the inode. */

        // Calculate number of blocks used by this file
        int fileblockcount = (blockbuff->size + BLOCKSIZE - 1) / BLOCKSIZE;
        
        // Free all direct blocks
        int direct_blocks_to_free = fileblockcount < NUM_DIRECT ? fileblockcount : NUM_DIRECT;
        for (int i = 0; i < direct_blocks_to_free; i++) {
            if (blockbuff->direct[i] > 0) {  // Make sure block number is valid
                deallocBlock(blockbuff[blockpos].direct[i]);
            }
        }
        
        // Handle indirect blocks if needed
        if (fileblockcount > NUM_DIRECT && blockbuff->indirect > 0) {
            // Read the indirect block
            void* indirect_buffer = malloc(BLOCKSIZE);
            ReadSector(blockbuff[blockpos].indirect, indirect_buffer);
            int* indirect_blocks = (int*)indirect_buffer;
            
            // Free each block pointed to by the indirect block
            int indirect_blocks_to_free = fileblockcount - NUM_DIRECT;
            for (int i = 0; i < indirect_blocks_to_free; i++) {
                if (indirect_blocks[i] > 0) {  // Make sure block number is valid
                    deallocBlock(indirect_blocks[i]);
                }
            }
            
            // Free the indirect block itself
            deallocBlock(blockbuff[blockpos].indirect);
            free(indirect_buffer);
        }

        // Change strictly the values that correspond to the specific inode we want to change
        blockbuff[blockpos].type = INODE_FREE;
        // I'm not resetting other values; that's your job when you allocate.

        WriteSector(block, buff);
        addInodeToCache(&blockbuff[blockpos], nodenum);
        free(buff);
    }

    isinodetaken[nodenum] = 0;

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






/* PATH HANDLING FUNCTIONS */

/*
 * This is the primary path handling function. Give it a path, and it should give you everything you want.
 * It doesn't do that yet. But it will. I promise.
 */
int resolve_path(char *path, int current_dir_inode, int *final_inode) {
    int is_absolute; // from pathstrtoarray; indicates if dealing with an absolute path
    int num_components; // also from pathstrtoarray
    char **components; // guess what? pathstrtoarray
    int working_inode; // note we're currently observing as we descend
    
    // Handle NULL path; too easy for a guy like me.
    if (path == NULL) {
        return ERROR;
    }
    
    // Empty path or just "/" -- error on first, root for second.
    if (path[0] == '\0') {
        return ERROR;
    }
    if (strcmp(path, "/") == 0) {
        *final_inode = ROOTINODE;
        return 0;  // Success
    }
    
    // Split path into components
    components = pathStrToArray(path, &num_components, &is_absolute);
    if (components == NULL) {
        // either the path is empty... or it's "/"; use abs flag for this case
        if (is_absolute) {
            *final_inode = ROOTINODE;
            return 0;  // Success
        }
        return ERROR;
    }
    
    // Start at root or current directory
    working_inode = is_absolute ? ROOTINODE : current_dir_inode;
    
    // Descend down components
    for (int i = 0; i < num_components; i++) {
        // Handle . and ..
        if (strcmp(components[i], ".") == 0) {
            // Current directory - do nothing(?)
            continue;
        } else if (strcmp(components[i], "..") == 0) {
            // Parent directory - lookup ".." in current directory
            // note that this isn't as hard as it seems, because every inode should have ".." on conception.
            int parent_inode = lookup_in_directory(working_inode, "..");
            if (parent_inode < 0) {
                // Free components and return ERROR. look how responsible I am at memory management
                for (int j = 0; j < num_components; j++) {
                    free(components[j]);
                }
                free(components);
                return ERROR;
            }
            working_inode = parent_inode;
        } else {
            // Regular component: look it up, get its inode, move on
            int next_inode = lookup_in_directory(working_inode, components[i]);
            if (next_inode < 0) {
                // Component not found
                for (int j = 0; j < num_components; j++) {
                    free(components[j]);
                }
                free(components);
                return ERROR;
            }
            
            // Get the inode to check if it's a directory (for all but the last component)
            if (i < num_components - 1) {
                struct inode *node = readInode(next_inode);
                if (node == NULL || node->type != INODE_DIRECTORY) {
                    // Not a directory but more components to process
                    if (node != NULL) {
                        free(node);
                    }
                    for (int j = 0; j < num_components; j++) {
                        free(components[j]);
                    }
                    free(components);
                    return ERROR;
                }
                free(node);
            }
            
            working_inode = next_inode;
        }
    }
    
    *final_inode = working_inode;
    
    // Free the components array
    for (int i = 0; i < num_components; i++) {
        free(components[i]);
    }
    free(components);
    
    return 0; // ez dub
}





/*
 * Splits a pathname into its component parts.
 * The goal is that I'll then be able to iterate over them
 */
char** pathStrToArray(char* path, int* num_components, int* is_absolute) {

    /* OK, here is the plan of attack:
     * 1) Check if path absolute
     * 2) Count the number of components
     * 3) Extract the components and stuff them in an array
     * 4) Return the array and the number counts
     * Note: I have completely changed this plan and it does not describe the code. sorry.
     */


    if (path == NULL || num_components == NULL || is_absolute == NULL) {
        return NULL;
    }
    
    // Check if path is absolute
    *is_absolute = (path[0] == '/');
    
    // Make a copy of the path since strtok modifies the string apparently.
    char* path_copy = strdup(path);
    if (path_copy == NULL) {
        return NULL;
    }
    
    // Count components for array allocation
    int count = 0;
    char* temp_copy = strdup(path_copy);
    char* token = strtok(temp_copy, "/");
    // I do not like this idiosyncratic function but could not find a better one
    // Keep tokenizing until we get the sum of the tokens.
    while (token != NULL) {
        count++;
        token = strtok(NULL, "/");
    }
    free(temp_copy);
    
    // This should also never happen
    if (count == 0) {
        free(path_copy);
        *num_components = 0;
        return NULL;
    }
    
    // Allocate the array of component strings
    char** components = (char**)malloc(count * sizeof(char*));
    if (components == NULL) {
        free(path_copy);
        return NULL;
    }
    
    // Tokenize the string again to fill the array lol
    // I rewrote this to try and do it in one pass but this seems impossible
    token = strtok(path_copy, "/");
    int i = 0;
    while (token != NULL && i < count) {
        components[i] = strdup(token);
        i++;
        token = strtok(NULL, "/");
    }
    
    free(path_copy);
    *num_components = count;
    return components;
}


/*
 * Looks up a component name in a directory. Should be easy, right?
 */
int lookup_in_directory(int dir_inode, char* component) {
    struct inode* dir = readInode(dir_inode);
    
    if (dir == NULL) {
        TracePrintf(1, "ERROR: Could not read directory inode %d\n", dir_inode);
        return ERROR;
    }
    
    if (dir->type != INODE_DIRECTORY) { // check it's a directory
        TracePrintf(1, "ERROR: Inode %d is not a directory\n", dir_inode);
        free(dir);
        return ERROR;
    }
    
    // Calculate number of directory entries we're dealing with (could be global probably?)
    int num_entries = dir->size / sizeof(struct dir_entry);
    int entries_per_block = BLOCKSIZE / sizeof(struct dir_entry);
    
    // Block read buffer
    void* block_buffer = malloc(BLOCKSIZE);
    if (block_buffer == NULL) {
        TracePrintf(1, "ERROR: Failed to allocate memory for block buffer\n");
        free(dir);
        return ERROR;
    }
    
    int result = -1;  // will contain inode result
    
    // Calculate how many data blocksto be read
    int num_blocks = (num_entries + entries_per_block - 1) / entries_per_block;
    int blocks_read = 0;
    
    // Scan direct blocks
    for (int i = 0; i < NUM_DIRECT && blocks_read < num_blocks; i++) {
        if (dir->direct[i] == 0) {
            continue;
            // this shouldn't happen really
        }
        
        // Read block
        if (ReadSector(dir->direct[i], block_buffer) != 0) {
            TracePrintf(1, "ERROR: Failed to read directory block\n");
            free(block_buffer);
            free(dir);
            return ERROR;
        }
        
        // block is filled with dir entries, so this cast should be safe
        struct dir_entry* entries = (struct dir_entry*)block_buffer;
        
        // Calculate entries in this block (might be partial for last block?)
        int entries_this_block = (blocks_read == num_blocks - 1) ? 
                               (num_entries - blocks_read * entries_per_block) : 
                               entries_per_block; // I'm getting good at this notation, everyone clap
        
        // For a given (direct) block
        for (int j = 0; j < entries_this_block; j++) {
            // Skip free entries
            if (entries[j].inum == 0) {
                continue;
            }
            
            // Corrected name comparison (accounts for hitting nul char right, I think)
            int match = 1;
            int k;
            
            // Compare characters until we reach a null or DIRNAMELEN
            for (k = 0; k < DIRNAMELEN; k++) {
                // At end of component it's a match
                if (component[k] == '\0') {
                    if (entries[j].name[k] != '\0') {
                        match = 0;  // I lied: not a match if directory entry has more characters
                    }
                    break;
                }
                
                // when directory entry name has a null, but component has more chars
                if (entries[j].name[k] == '\0') {
                    match = 0;
                    break;
                }
                
                // just a character mismatch
                if (entries[j].name[k] != component[k]) {
                    match = 0;
                    break;
                }
            }
            
            // If we reached DIRNAMELEN but component has more characters, also not a match
            if (k == DIRNAMELEN && component[k] != '\0') {
                match = 0;
            }
            
            // for once I will be consistent about freeing memory.
            if (match) {
                result = entries[j].inum;
                free(block_buffer);
                free(dir);
                return result;
            }
        }
        
        blocks_read++;
    }
    
    // If directory has more blocks in indirect block, read those too, I hope this works
    if (blocks_read < num_blocks && dir->indirect != 0) {
        void* indirect_buffer = malloc(BLOCKSIZE);
        if (indirect_buffer == NULL) {
            TracePrintf(1, "ERROR: Failed to allocate memory for indirect block\n");
            free(block_buffer);
            free(dir);
            return ERROR;
        }
        
        // Read indirect block
        if (ReadSector(dir->indirect, indirect_buffer) != 0) {
            TracePrintf(1, "ERROR: Failed to read indirect block\n");
            free(indirect_buffer);
            free(block_buffer);
            free(dir);
            return ERROR;
        }
        
        // it's just an int array
        int* indirect_blocks = (int*)indirect_buffer;
        
        // so scan through the list
        for (int i = 0; blocks_read < num_blocks; i++) {
            if (indirect_blocks[i] == 0) {
                continue;  // Skip empty blocks
                // I have no idea why this would happen; call it paranoia
            }
            
            // Read the corresponding block
            if (ReadSector(indirect_blocks[i], block_buffer) != 0) {
                TracePrintf(1, "ERROR: Failed to read directory block\n");
                free(indirect_buffer);
                free(block_buffer);
                free(dir);
                return ERROR;
            }

            // we're now doing stuff similar to direct blocks, but I am rushed and lazy
            // so I'm copy-pasting instead of some modular solution. sorry.
            
            // Cast buffer to directory entries (this is now akin to direct blocks)
            struct dir_entry* entries = (struct dir_entry*)block_buffer;
            
            // Calculate entries in this block (might be partial for last block)
            int entries_this_block = (blocks_read == num_blocks - 1) ? 
                                   (num_entries - blocks_read * entries_per_block) : 
                                   entries_per_block;
            
            // Search entries in this block
            for (int j = 0; j < entries_this_block; j++) {
                if (entries[j].inum == 0) {
                    continue;
                }
                
                int match = 1;
                int k;
                
                for (k = 0; k < DIRNAMELEN; k++) {
                    if (entries[j].name[k] != '\0') {
                        match = 0;  // I lied: not a match if directory entry has more characters
                    }
                    break;
                    
                    if (entries[j].name[k] == '\0') {
                        match = 0;
                        break;
                    }
                    
                    if (entries[j].name[k] != component[k]) {
                        match = 0;
                        break;
                    }
                }
                
                if (k == DIRNAMELEN && component[k] != '\0') {
                    match = 0;
                }
                
                if (match) {
                    result = entries[j].inum;
                    free(indirect_buffer);
                    free(block_buffer);
                    free(dir);
                    return result;
                }
            }
            
            blocks_read++;
        }
        
        free(indirect_buffer);
    }
    
    free(block_buffer);
    free(dir);
    return result;  // -1 if not found
}







/* INTERNAL TEST FUNCTIONS */

void printisblocktaken() {
    for(int i = 0; i < header->num_blocks; i++) {
        TracePrintf(5, "printisblocktaken: db %d has value %d.\n", i, isblocktaken[i]);
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
        if (!isblocktaken[i]) {
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
        if (!isblocktaken[i]) {
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
    
    if (isblocktaken[allocatedBlock] != 1) {
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

void testBlockDeallocation() {
    // Allocate a block first
    void *testBuffer = malloc(SECTORSIZE);
    memset(testBuffer, 'A', SECTORSIZE);
    
    int allocatedBlock = allocBlock(testBuffer);
    free(testBuffer);
    
    if (allocatedBlock == -1) {
        TracePrintf(0, "TEST: Block allocation failed!\n");
        return;
    }
    
    // Count free blocks before deallocation
    int freeBlocksBefore = 0;
    for (int i = 1; i < header->num_blocks; i++) {
        if (!isblocktaken[i]) {
            freeBlocksBefore++;
        }
    }
    
    // Deallocate the block
    deallocBlock(allocatedBlock);
    
    // Count free blocks after deallocation
    int freeBlocksAfter = 0;
    for (int i = 1; i < header->num_blocks; i++) {
        if (!isblocktaken[i]) {
            freeBlocksAfter++;
        }
    }
    
    // Verify we have one more free block
    if (freeBlocksAfter != freeBlocksBefore + 1) {
        TracePrintf(0, "TEST FAILED: Free block count mismatch after deallocation!\n");
    } else {
        TracePrintf(0, "TEST PASSED: Block properly deallocated\n");
    }
    
    // Verify the block is marked as free
    if (isblocktaken[allocatedBlock] != 0) {
        TracePrintf(0, "TEST FAILED: Deallocated block still marked as taken!\n");
    } else {
        TracePrintf(0, "TEST PASSED: Deallocated block marked as free\n");
    }
}

void testSplitPath() {
    struct test_case {
        char* path;
        int expected_components;
        int expected_absolute;
        char** expected_parts;
    };
    
    // Define test cases
    // Add to this however you want
    struct test_case tests[] = {
        {"/home/user/docs", 3, 1, (char*[]){"home", "user", "docs"}},
        {"home/user/docs", 3, 0, (char*[]){"home", "user", "docs"}},
        {"/home//user///docs", 3, 1, (char*[]){"home", "user", "docs"}},
        {"/", 0, 1, NULL},
        {"", 0, 0, NULL},
        {"////", 0, 1, NULL},
        {"/home/user/", 2, 1, (char*[]){"home", "user"}},
        {"./test", 2, 0, (char*[]){".", "test"}}
    };
    
    // Dont' touch this stuff
    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int tests_passed = 0;
    
    TracePrintf(1, "Starting pathStrToArray tests...\n");
    
    for (int i = 0; i < num_tests; i++) {
        int num_components;
        int is_absolute;
        
        TracePrintf(1, "Test %d: path=\"%s\"\n", i + 1, tests[i].path);
        
        char** components = pathStrToArray(tests[i].path, &num_components, &is_absolute);
        
        // Check component count
        if (num_components != tests[i].expected_components) {
            TracePrintf(1, "  FAILED: Expected %d components, got %d\n", 
                   tests[i].expected_components, num_components);
            goto cleanup;
        }
        
        // Check absolute flag
        if (is_absolute != tests[i].expected_absolute) {
            TracePrintf(1, "  FAILED: Expected is_absolute=%d, got %d\n", 
                   tests[i].expected_absolute, is_absolute);
            goto cleanup;
        }
        
        // Check components
        int components_match = 1;
        for (int j = 0; j < num_components; j++) {
            if (strcmp(components[j], tests[i].expected_parts[j]) != 0) {
                TracePrintf(1, "  FAILED: Component %d expected \"%s\", got \"%s\"\n", 
                       j, tests[i].expected_parts[j], components[j]);
                components_match = 0;
                break;
            }
        }
        
        if (!components_match) {
            goto cleanup;
        }
        
        TracePrintf(1, "  PASSED\n");
        tests_passed++;
        
    cleanup:
        // Free memory
        if (components) {
            for (int j = 0; j < num_components; j++) {
                free(components[j]);
            }
            free(components);
        }
    }
    
    TracePrintf(1, "Tests completed: %d/%d passed\n", tests_passed, num_tests);
}

/**
 * Tests the lookup_in_directory function
 */
void test_lookup_in_directory() {
    TracePrintf(0, "Starting lookup_in_directory tests...\n");
    
    // First, we need to create a test directory to search in
    struct inode dir_inode;
    int dir_inode_num;
    void* block_buffer;
    struct dir_entry* entries;
    int block_num;
    
    // Initialize the directory inode
    memset(&dir_inode, 0, sizeof(struct inode));
    dir_inode.type = INODE_DIRECTORY;
    dir_inode.nlink = 1;
    dir_inode.size = 5 * sizeof(struct dir_entry); // Space for 5 entries
    
    // Allocate a block for directory entries
    block_buffer = malloc(BLOCKSIZE);
    if (block_buffer == NULL) {
        TracePrintf(0, "TEST FAILED: Could not allocate memory\n");
        return;
    }
    memset(block_buffer, 0, BLOCKSIZE);
    
    // Create directory entries in the block
    entries = (struct dir_entry*)block_buffer;
    
    // Entry 1: "."
    entries[0].inum = 42; // We'll pretend this directory has inode 42
    strcpy(entries[0].name, ".");
    
    // Entry 2: ".."
    entries[1].inum = 1; // Parent is root
    strcpy(entries[1].name, "..");
    
    // Entry 3: "testfile"
    entries[2].inum = 100;
    strcpy(entries[2].name, "testfile");
    
    // Entry 4: "longnamefoldr" (exactly DIRNAMELEN - 1 chars + null)
    entries[3].inum = 101;
    strcpy(entries[3].name, "longnamefoldr");
    
    // Entry 5: Free entry (inum = 0)
    entries[4].inum = 0;
    strcpy(entries[4].name, "deleted");
    
    // Allocate the block on disk
    block_num = allocBlock(block_buffer);
    if (block_num == -1) {
        TracePrintf(0, "TEST FAILED: Could not allocate block\n");
        free(block_buffer);
        return;
    }
    
    // Set the block number in the inode
    dir_inode.direct[0] = block_num;
    
    // Allocate the inode
    dir_inode_num = allocInode(&dir_inode);
    if (dir_inode_num == -1) {
        TracePrintf(0, "TEST FAILED: Could not allocate inode\n");
        deallocBlock(block_num);
        free(block_buffer);
        return;
    }
    
    TracePrintf(0, "Test directory created with inode %d and block %d\n", 
               dir_inode_num, block_num);
    
    // Now test lookup_in_directory
    struct test_case {
        char* component;
        int expected_result;
    };
    
    struct test_case tests[] = {
        {".", 42},                    // Test case 1: Current directory
        {"..", 1},                    // Test case 2: Parent directory
        {"testfile", 100},            // Test case 3: Regular file
        {"longnamefoldr", 101},       // Test case 4: Long name (DIRNAMELEN-1)
        {"deleted", -1},              // Test case 5: Deleted entry
        {"nonexistent", -1},          // Test case 6: Nonexistent entry
        {"testfil", -1},              // Test case 7: Prefix of existing name
        {"testfiles", -1},            // Test case 8: Existing name with extra chars
    };
    
    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int tests_passed = 0;
    
    for (int i = 0; i < num_tests; i++) {
        int result = lookup_in_directory(dir_inode_num, tests[i].component);
        
        TracePrintf(0, "Test %d: Looking up \"%s\" - ", i + 1, tests[i].component);
        
        if (result == tests[i].expected_result) {
            TracePrintf(0, "PASSED (got %d as expected)\n", result);
            tests_passed++;
        } else {
            TracePrintf(0, "FAILED (expected %d, got %d)\n", 
                       tests[i].expected_result, result);
        }
    }
    
    // Test edge case: Test with component exactly DIRNAMELEN chars long
    char long_name[DIRNAMELEN + 1];
    memset(long_name, 'a', DIRNAMELEN);
    long_name[DIRNAMELEN] = '\0';
    
    TracePrintf(0, "Test %d: Looking up name of exactly DIRNAMELEN length - ", 
               num_tests + 1);
    int result = lookup_in_directory(dir_inode_num, long_name);
    if (result == -1) {
        TracePrintf(0, "PASSED (got -1 as expected)\n");
        tests_passed++;
    } else {
        TracePrintf(0, "FAILED (expected -1, got %d)\n", result);
    }
    
    // Test edge case: Non-directory inode
    struct inode file_inode;
    int file_inode_num;
    
    memset(&file_inode, 0, sizeof(struct inode));
    file_inode.type = INODE_REGULAR;
    file_inode_num = allocInode(&file_inode);
    
    if (file_inode_num != -1) {
        TracePrintf(0, "Test %d: Lookup in non-directory inode - ", 
                   num_tests + 2);
        result = lookup_in_directory(file_inode_num, "anything");
        if (result == ERROR) {
            TracePrintf(0, "PASSED (got ERROR as expected)\n");
            tests_passed++;
        } else {
            TracePrintf(0, "FAILED (expected ERROR, got %d)\n", result);
        }
        
        deallocInode(file_inode_num);
    }
    
    // Clean up
    deallocInode(dir_inode_num);
    free(block_buffer);
    
    TracePrintf(0, "Lookup tests completed: %d/%d passed\n", 
               tests_passed, num_tests + 2);
}

void test_path_resolution() {
    TracePrintf(0, "Starting path resolution tests...\n");
    
    // Create a test directory structure
    // (You may want to reuse your test_lookup_in_directory setup)
    
    // Test cases
    struct test_case {
        char* path;
        int current_dir;  // Starting directory
        int expected_result;
        int expected_error;  // 0 for success, ERROR for failure
    };
    
    struct test_case tests[] = {
        // Basic cases
        {"/", ROOTINODE, ROOTINODE, 0},
        {".", ROOTINODE, ROOTINODE, 0},
        {"..", ROOTINODE, ROOTINODE, 0},  // Root is its own parent
        
        // Add more tests based on your test directory structure
        // For example, if you create files/directories in setup
    };
    
    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int tests_passed = 0;
    
    for (int i = 0; i < num_tests; i++) {
        int result_inode;
        int status = resolve_path(tests[i].path, tests[i].current_dir, &result_inode);
        
        TracePrintf(0, "Test %d: Path \"%s\" from dir %d - ", 
                   i + 1, tests[i].path, tests[i].current_dir);
        
        if (status == tests[i].expected_error) {
            if (status == 0 && result_inode == tests[i].expected_result) {
                TracePrintf(0, "PASSED (got inode %d as expected)\n", result_inode);
                tests_passed++;
            } else if (status != 0) {
                TracePrintf(0, "PASSED (got expected error)\n");
                tests_passed++;
            } else {
                TracePrintf(0, "FAILED (got inode %d, expected %d)\n", 
                           result_inode, tests[i].expected_result);
            }
        } else {
            TracePrintf(0, "FAILED (got status %d, expected %d)\n", 
                       status, tests[i].expected_error);
        }
    }
    
    TracePrintf(0, "Path resolution tests completed: %d/%d passed\n", 
               tests_passed, num_tests);
}