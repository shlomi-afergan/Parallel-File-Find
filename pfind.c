/**based on functions from https://c-for-dummies.com/blog/?p=3252 and other pages in this blog.**/

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <dirent.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/stat.h>


/** ________________________ Structs Declaration ________________________*/

typedef struct Dir_Node{
    char* dir;
    struct Dir_Node *next;

}Dir_Node;

// ______________________________
typedef struct Dir_Queue{
    Dir_Node *first;
    Dir_Node *last;
    int size;
}Dir_Queue;

// ______________________________
typedef struct Waiting_Node{
    thrd_t curr_t;
    struct Waiting_Node *next;
    cnd_t personal_wait;
}Waiting_Node;

// ______________________________
typedef struct Waiting_Queue{
    Waiting_Node *first;
    Waiting_Node *last;
    int size;
}Waiting_Queue;

/** ________________________ Variables Declaration ________________________*/
Dir_Queue *dir_queue;
Waiting_Queue *waiting_queue;
thrd_t *Thread_arr;
atomic_int count_res;
int NUM_OF_THREADS;
char* search_term;
int ready_thrds;

/** ________________________ Mtx & Cnd Declaration ________________________*/
mtx_t dir_q_lock;
mtx_t waiting_q_lock;
mtx_t all_ready_mtx;

cnd_t all_ready_cnd;
cnd_t start_all;
cnd_t waiting_my_turn;
int No_search_errors;


/** ________________________ Arguments Validation Functions ________________________*/

void check_root_directory(int argc, char* path){
    int status;
    struct stat dir_stat;

    if(argc != 4){
        fprintf(stderr, "Error: 3 arguments are required.\n");
        exit(1);
    }
    status = stat(path, &dir_stat);
    if (status != 0) {
        fprintf(stderr, "Error: can't get stat of this path: %s\n", path);
        exit(1);
    }
    // should be a dir, not a file, and with R,X permissions:
    if (!S_ISDIR(dir_stat.st_mode) || (access(path, R_OK) != 0) || (access(path, X_OK)!= 0)){
        fprintf(stderr, "Error: can't open this path: %s\n", path);
        printf("Directory %s: Permission denied.\n", path);
        exit(1);
    }
}
void free_queues_and_Thread_array(){
    free(Thread_arr);
    free(dir_queue);
    free(waiting_queue);
}

// _______________________________________
void create_queues_and_Thread_array() {
    dir_queue = calloc(1, sizeof(Dir_Queue));
    waiting_queue = calloc(1, sizeof(Waiting_Queue));
    Thread_arr = calloc(NUM_OF_THREADS,sizeof(thrd_t));

    if (dir_queue == NULL || waiting_queue == NULL || Thread_arr == NULL) {
        free_queues_and_Thread_array();
        fprintf(stderr, "Error in memory allocation. \n");
        exit(1);
    }
}

// _______________________________________
long check_num_of_threads(char* num){
    char* end_ptr;
    NUM_OF_THREADS = strtol(num,&end_ptr,10);
    // return 0 on error:
    if (NUM_OF_THREADS <= 0){
        fprintf(stderr, "Error: Invalid number of threads.\n");
        exit(1);
    }
    return NUM_OF_THREADS;
}
/** ________________________ Auxiliary Functions ________________________*/
void init_all_mtx_cnd(){
    mtx_init(&dir_q_lock, mtx_plain);
    mtx_init(&waiting_q_lock, mtx_plain);
    mtx_init(&all_ready_mtx, mtx_plain);

    cnd_init(&all_ready_cnd);
    cnd_init(&start_all);
    cnd_init(&waiting_my_turn);
}

// _______________________________________
void destroy_all_mtx_cnd(){
    mtx_destroy(&dir_q_lock);
    mtx_destroy(&waiting_q_lock);
    mtx_destroy(&all_ready_mtx);

    cnd_destroy(&all_ready_cnd);
    cnd_destroy(&start_all);
    cnd_destroy(&waiting_my_turn);
}

// _______________________________________
Dir_Node* create_Dir_Node(char* source_dir){
    Dir_Node *new_dir = malloc(sizeof(Dir_Node));
    new_dir->dir = (char*)malloc(PATH_MAX * sizeof(char));
    strcpy(new_dir->dir, source_dir);
    new_dir->next = NULL;
    return new_dir;
}
// _______________________________________
Waiting_Node* create_Waiting_Node(long tid){
    Waiting_Node *t_node = malloc(sizeof(Waiting_Node));
    t_node->curr_t = Thread_arr[tid];
    t_node->next = NULL;
    cnd_init(&t_node->personal_wait);
    return t_node;
}

// _______________________________________
/** flag = 0 means dir_node.
    flag = 1 means waiting_node: */
void insert_to_queue(void* node, int flag){
//  dir_node:
    if (flag == 0) {
        if (dir_queue->first == NULL) {
            /* The queue is empty:*/
            dir_queue->first = dir_queue->last = (Dir_Node *) node;
        } else {
            dir_queue->last->next = (Dir_Node *) node;
            dir_queue->last = (Dir_Node *) node;
        }
        dir_queue->size++;
    }
//  waiting_node:
    else{
        if (waiting_queue->size == 0){
            /* The queue is empty:*/
            waiting_queue->first = waiting_queue->last = (Waiting_Node*) node;
        }
        else{
            waiting_queue->last->next = (Waiting_Node*) node;
            waiting_queue->last = (Waiting_Node*) node;
        }
        waiting_queue->size++;
    }
}
// _______________________________________
/** flag = 0 means dir_queue.
    flag = 1 means waiting_queue: */
int remove_first_node(int flag){
    Dir_Node *dir_node;

    // dir_queue case:
    if (flag == 0){
        /* if the queue is empty: */
        if(dir_queue->first == NULL){
            return -1;
        }
        dir_node = dir_queue->first;
        /* if there is 1 element in the queue: */
        if (dir_queue->first->next == NULL){
            dir_queue->first = dir_queue->last = NULL;
        }
        else{
            dir_queue->first = dir_queue->first->next;
        }
        free(dir_node);
        dir_queue->size--;
    }
    // waiting_queue case:
    else {
        if(waiting_queue->size == 0){
            return -1;
        }
//      thrd_node = waiting_queue->first;
        if (waiting_queue->size == 1){
            waiting_queue->first = waiting_queue->last = NULL;
        }
        else{
            waiting_queue->first = waiting_queue->first->next;
        }
        waiting_queue->size--;
    }
    return 0;
}

// _______________________________________
int check_valid_directory(char* path){
    if ((access(path, R_OK) != 0) || (access(path, X_OK) != 0)){
        printf("Directory %s: Permission denied.\n", path);
        return -1;
    }
    return 0;
}
// _______________________________________
DIR* open_dir(char *path){
    DIR *curr_dir = NULL;

    curr_dir = opendir(path);
    if (curr_dir == NULL){
        printf("Directory %s: Permission denied.\n", path);
        return NULL;
    }
    return curr_dir;
}
// _______________________________________
void check_this_dir(char* path) {
    Dir_Node *dir_node;
    int status = check_valid_directory(path);
    if (status == 0) {
//      Put the dir in dir_queue as a new task:
        dir_node = create_Dir_Node(path);
        mtx_lock(&dir_q_lock);
        insert_to_queue(dir_node, 0);
        mtx_unlock(&dir_q_lock);

//      A new dir has entered, so wake up the next thread to start working:
        mtx_lock(&waiting_q_lock);
        if(waiting_queue->size > 0) {
            cnd_signal(&waiting_queue->first->personal_wait);
        }
        mtx_unlock(&waiting_q_lock);
    }
}

// _______________________________________
void check_this_file(char* file_name, char* path){
    if (strstr(file_name, search_term) != NULL){
        printf("%s\n", path);
        count_res++;
    }
}

// _______________________________________
int scan_dir(char* root_path){
    int status;
    char *new_path = (char*) calloc(1,PATH_MAX * sizeof(char));
    struct dirent *curr_entry;
    struct stat entry_stat_buf;
    DIR *dir;
    dir = open_dir(root_path);
    if (dir != NULL) {
        // iterating the root_path:
        while ((curr_entry = readdir(dir)) != NULL) {
            sprintf(new_path, "%s/%s", root_path, curr_entry->d_name);
            status = stat(new_path, &entry_stat_buf);
            if (status != 0) {
                fprintf(stderr, "Error: can't get stat of this path: %s\n", new_path);
                No_search_errors = 1;
                continue;
            }
            if (S_ISDIR(entry_stat_buf.st_mode)) {
                if ((strcmp(curr_entry->d_name, ".") != 0) && (strcmp(curr_entry->d_name, "..") != 0)) {
                    check_this_dir(new_path);
                }
            } else {
                check_this_file(curr_entry->d_name, new_path);
            }
        }
        closedir(dir);
    }
    // The thread has finished the job, wakes the next thread in the queue:
    mtx_lock(&waiting_q_lock);
    if(waiting_queue->size > 0) {
        cnd_signal(&waiting_queue->first->personal_wait);
    }
    mtx_unlock(&waiting_q_lock);
    free(new_path);
    return 0;

}

// _______________________________________
int thrd_search(void *t){
    long tid = (long)t;
    int Today_Is_A_Good_Day = 1;
    Waiting_Node *t_node;
    char *dir_path;

    // Synchronizes all threads to be ready together:
    mtx_lock(&all_ready_mtx);

    // _________________________________________
    // Create and insert this thread node to the waiting_queue:
    mtx_lock(&waiting_q_lock);
    t_node = create_Waiting_Node(tid);
    mtx_unlock(&waiting_q_lock);
    ready_thrds++;
    // _________________________________________

    if(ready_thrds == NUM_OF_THREADS){
        // This is the last thread, so notify the main thread it's time to wake everyone up:
        cnd_signal(&all_ready_cnd);
    }
    else{
        // There are threads that haven't been initialized yet, so go to sleep for now:
        cnd_wait(&start_all, &all_ready_mtx);
    }
    mtx_unlock(&all_ready_mtx);

//  Now all threads are ready to start searching together.
//  Only the first thread in the waiting_queue (FIFO) can receive dir_path at any time:
    while(Today_Is_A_Good_Day){
        mtx_lock(&dir_q_lock);
//      __________________________________________
//      The thread is ready for a new task:
        mtx_lock(&waiting_q_lock);
        insert_to_queue(t_node,1);
        mtx_unlock(&waiting_q_lock);

        // the waiting queue:
        while (dir_queue->size == 0 || waiting_queue->first != t_node){

            // If all the work is done:
            if (dir_queue->size == 0 && waiting_queue->size == NUM_OF_THREADS) {
                // If this thread is not the first in the queue, it will wake up the first one to be deleted first, then will go to sleep.
                if (waiting_queue->first != t_node){
                    cnd_signal(&waiting_queue->first->personal_wait);
                }
                // Each time, the first thread in the queue deletes itself and calls the next one in the queue:
                else{
                    mtx_lock(&waiting_q_lock);
                    cnd_destroy(&waiting_queue->first->personal_wait);
                    remove_first_node(1);
                    NUM_OF_THREADS--;
                    if (waiting_queue->size > 0){
                        cnd_signal(&waiting_queue->first->personal_wait);
                    }
                    mtx_unlock(&waiting_q_lock);
                    mtx_unlock(&dir_q_lock);
                    free(t_node);
                    thrd_exit(0);
                }
            }
            // If there is still work but the thread is not the first, just go to sleep.
            cnd_wait(&t_node->personal_wait, &dir_q_lock);
        }
//      It's time to search:
        dir_path = dir_queue->first->dir;
        remove_first_node(0);
        mtx_lock(&waiting_q_lock);
        remove_first_node(1);
        mtx_unlock(&waiting_q_lock);
        mtx_unlock(&dir_q_lock);
        scan_dir(dir_path);
        free(dir_path);
    }
    return 0;
}
// _______________________________________
void run_threads(){
    long t;
    int status;
    init_all_mtx_cnd();

    mtx_lock(&all_ready_mtx);
    /* Creates all the threads and assigns them the tasks: */
    for (t = 0; t < NUM_OF_THREADS; ++t) {
        status = thrd_create(&Thread_arr[t], thrd_search, (void *)t);
        if (status != thrd_success) {
            fprintf(stderr,"ERROR in thread creating.\n");
            free_queues_and_Thread_array();
            exit(1);
        }
    }
    // The main waits for confirmation from the last created thread, then wakes everyone up:
    cnd_wait(&all_ready_cnd, &all_ready_mtx);
    cnd_broadcast(&start_all);
    mtx_unlock(&all_ready_mtx);

    /* Waiting for all threads to finish the task: */
    for (t = 0; t < NUM_OF_THREADS; ++t) {
        status = thrd_join(Thread_arr[t], NULL);
        if (status != thrd_success) {
            fprintf(stderr, "ERROR in thrd_join().\n");
            free_queues_and_Thread_array();
            exit(1);
        }
    }
    destroy_all_mtx_cnd();
}
/** ________________________ Main Function ________________________*/
int main(int argc, char *argv[]) {
    Dir_Node *root_node;
    No_search_errors = 0;
    count_res = 0;
    ready_thrds = 0;

    // check arguments' validity:
    check_root_directory(argc,argv[1]);
    search_term = argv[2];
    NUM_OF_THREADS = check_num_of_threads(argv[3]);

    create_queues_and_Thread_array();

    //create and insert the root dir:
    root_node = create_Dir_Node(argv[1]);
    insert_to_queue(root_node,0);               // 0 means 'dir' case

    run_threads();

    printf("Done searching, found %d files\n", count_res);

    free_queues_and_Thread_array();
    return No_search_errors;
}