/*
 * alarm_cond.c
 *
 * This is an enhancement to the alarm_mutex.c program, which
 * used only a mutex to synchronize access to the shared alarm
 * list. This version adds a condition variable. The alarm
 * thread waits on this condition variable, with a timeout that
 * corresponds to the earliest timer request. If the main thread
 * enters an earlier timeout, it signals the condition variable
 * so that the alarm thread will wake up and process the earlier
 * timeout first, requeueing the later request.
 */
#include <pthread.h>
#include <time.h>
#include "errors.h"
// Added libraries (Arthi S.)
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
//Added libraries (Hien L.)
#include <semaphore.h>

/*
 * The "alarm" structure now contains the time_t (time since the
 * Epoch, in seconds) for each alarm, so that they can be
 * sorted. Storing the requested number of seconds would not be
 * enough, since the "alarm thread" cannot tell how long it has
 * been on the list.
 */
typedef struct alarm_tag {
    struct alarm_tag    *link;
    int                 seconds;
    time_t              time;   /* seconds from EPOCH */
    char                message[128];
    int                 alarm_id;
    int                 group_id;
    int                 active;
} alarm_t;

typedef struct group_t {
    int                 group_id;
    pthread_t           thread_id;
    struct              group_t *next;
}group_t;


// Global synchronization objects
pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;

// Shared alarm list and current alarm for condition signaling
alarm_t *alarm_list = NULL;
group_t *group_threads = NULL;
time_t current_alarm = 0;

//Semaphores for Synchronization (Reader-Writer problem)
sem_t rw_mutex;     //Writer lock
sem_t mutex;        //Reader count lock     
int reader_count = 0;

//Function for Readers-Writers Synchronization
void start_read(){
    sem_wait(&mutex);
    reader_count++;
    
    if(reader_count == 1) {
        sem_wait(&rw_mutex);
    }
    sem_post(&mutex);
}

void stop_read() {
    sem_wait(&mutex);
    reader_count--;
    if(reader_count == 0){
        sem_post(&rw_mutex);
    }
    sem_post(&mutex);
}

void start_write() {
    sem_wait(&rw_mutex);
}

void stop_write(){
    sem_post(&rw_mutex);
}

//Function Declarations
void cancel_alarm(int alarm_id);
void suspend_alarm(int alarm_id);
void reactivate_alarm(int alarm_id);
void view_alarms();

//Helper functions for alarm list manipulation
alarm_t *find_alarm(int alarm_id);
void remove_alarm(int alarm_id);

/*
 * Insert alarm entry on list, in order.
 */
void alarm_insert (alarm_t *alarm){
    int status;
    alarm_t **last = &alarm_list, *next = *last;

    /*
     * LOCKING PROTOCOL:
     * 
     * This routine requires that the caller have locked the
     * alarm_mutex!
     */

    // Traverse the list to find the correct insertion point
    while (next != NULL && next -> time < alarm -> time) {
        last = &next -> link;
        next = next -> link;
    }
    alarm -> link = next;
    *last = alarm;

    // Signal the alarm thread if necessary
    if (current_alarm == 0 || alarm -> time < current_alarm){
        current_alarm = alarm -> time;
        status = pthread_cond_signal(&alarm_cond);
        if (status != 0) {
            err_abort(status, "Signal cond");
        }
    }
}

/*
 * The alarm thread's start routine. (Arthi S.)
 */
void *alarm_thread (void *arg)
{
    alarm_t *alarm;
    struct timespec cond_time;
    time_t now;
    int status, expired;

    /*
     * Loop forever, processing commands. The alarm thread will
     * be disintegrated when the process exits. Lock the mutex
     * at the start -- it will be unlocked during condition
     * waits, so the main thread can insert alarms.
     */
    status = pthread_mutex_lock (&alarm_mutex);
    if (status != 0) {
        err_abort (status, "Lock mutex");
    }

    while (1) {
        /*
         * If the alarm list is empty, wait until an alarm is
         * added. Setting current_alarm to 0 informs the insert
         * routine that the thread is not busy.
         */
        // Reset the current alarm and wait for new alarms if the list is empty
        current_alarm = 0;
        while (alarm_list == NULL) {
            status = pthread_cond_wait(&alarm_cond, &alarm_mutex);
            if (status != 0) {
                err_abort (status, "Wait on cond");
            }
        }

        // Get the first alarm from the list
        alarm = alarm_list;
        alarm_list = alarm->link;

        // Wait for the alarm's time
        now = time (NULL);
        expired = 0; // Initialize expired

        if (alarm -> time > now) {
            cond_time.tv_sec = alarm->time;
            cond_time.tv_nsec = 0;
            current_alarm = alarm->time;

            while (current_alarm == alarm->time) {
                status = pthread_cond_timedwait (&alarm_cond, &alarm_mutex, &cond_time);
                
                if (status == ETIMEDOUT) {
                    expired = 1;
                    break;
                }
                if (status != 0) {
                    err_abort (status, "Cond timedwait");
                }
            }

            // Reinsert the alarm if it was not expired
            if (!expired) {
                alarm_insert (alarm);
            }
        } else {
            expired = 1;    // Alarm already expired
        }

        // Print alarm message
        if (expired) {
            printf ("(%d) %s\n", alarm->seconds, alarm->message);
            free (alarm);
        }
    }
}

// Arthi S.
void *alarm_group_display_creation(void *arg) {
    int status;

    while(1) {
        status = pthread_mutex_lock(&alarm_mutex);
        if (status != 0) {
            err_abort(status, "Lock mutex");  
        }

        // Traverse list and create display threads for the new groups
        alarm_t *current = alarm_list;
        while (current != NULL) {
            // Display message
            printf("Group(%d) alarm ready: %s\n", current -> group_id, current -> message);
            current = current -> link;
        }

        status = pthread_mutex_unlock(&alarm_mutex);
        if (status != 0) {
            err_abort(status, "Unlock mutex");
        }

        sleep(1);   // Preiodically recheck the alarm list
    }
}

void *display_alarm_thread(void *arg) {
    int group_id = *((int *) arg);
    free(arg);

    while(1) {
        start_read();
        alarm_t *current = alarm_list;
        
        while(current != NULL) {
            if(current->group_id == group_id && current->active == 1){
                printf("Alarm (%d) Printed by Display Alarm Thread %ld at %ld: Group(%d) %s\n", current->alarm_id, pthread_self(), time(NULL), current->group_id, current->message);
                sleep(current->seconds);
            }
            current = current->link;
        }
        stop_read();
        sleep(1);
    }
    return NULL;
}

void *alarm_group_display_creation_thread(void *arg) {
    while(1){
        start_read();
        alarm_t *current = alarm_list;

        while(current != NULL){
            int group_id = current->group_id;

            //Check if a thread for this group exists
            group_t *group = group_threads;
            int exists = 0;

            while(group != NULL){
                if(group->group_id == group_id) {
                    exists = 1;
                    break;
                }
                group = group->next;
            }

            if(exists == 0){
                //Create a new thread for this group
                pthread_t new_thread;
                int *group_id_ptr = malloc(sizeof(int));
                *group_id_ptr = group_id;
                pthread_create(&new_thread, NULL, display_alarm_thread, group_id_ptr);

                //Add to group_threads list
                group_t *new_group = malloc(sizeof(group_t));
                new_group->group_id = group_id;
                new_group->thread_id = new_thread;
                new_group->next = group_threads;
                group_threads = new_group;

                printf("Alarm Group Display Creation Thread Created New Display Alarm Thread %ld for Alarm (%d) at %ld: Group(%d) %s\n", new_thread, current->alarm_id, time(NULL), group_id, current->message);
            }
            current = current->link;
        }
        stop_read();
        sleep(1); //Periodic Check
    }
    return NULL;
}

void *alarm_group_display_removal(void*arg) {
    while(1){
        start_read();
        group_t *current_group = group_threads;
        group_t *prev_group = NULL;

        while (current_group != NULL){
            int group_id = current_group->group_id;
            int found = 0;

            alarm_t *current_alarm = alarm_list;
            while(current_alarm != NULL){
                if(current_alarm->group_id == group_id){
                    found = 1;
                    break;
                }
            }
            current_alarm = current_alarm->link;

            if(found == 0) {
                pthread_cancel(current_group->thread_id);
                printf("No More Alarm in Group(%d) Alarm Removal Thread Has Removed Display Alarm Thread %ld at %ld: Group(%d)\n", group_id, current_group->thread_id, time(NULL), group_id);
                
                if(prev_group != NULL) {
                    prev_group->next = current_group->next;
                }else {
                    group_threads = current_group->next;
                }

                free(current_group);
                
                if(prev_group != NULL) {
                    current_group = prev_group->next;
                }else {
                    current_group = group_threads;
                }
            } else {
                prev_group = current_group;
                current_group = current_group->next;
            }
        }
        stop_read();
        sleep(1); //Periodic Check
    }
    return NULL;
}


int main (int argc, char *argv[])
{
    int status;
    char line[128]; // Input buffer for user commands
    alarm_t *alarm;
    pthread_t alarm_handler_thread, display_creation_thread, display_removal_thread, display_thread;

    //Initiate semaphores
    sem_init(&mutex, 0, 1);
    sem_init(&rw_mutex, 0, 1);

    // Create the alarm handler thread
    status = pthread_create (&alarm_handler_thread, NULL, alarm_thread, NULL);
    if (status != 0) {
        err_abort (status, "Create alarm thread");
    }

    // Create the alarm group display creation thread
    status = pthread_create(&display_creation_thread, NULL, alarm_group_display_creation_thread, NULL);
    if (status != 0) {
        err_abort(status, "Create alarm group display thread");
    }

    //Create the alarm group display removal thread
    status = pthread_create (&display_removal_thread, NULL, alarm_group_display_removal, NULL);
    if (status != 0) {
        err_abort(status, "Remove alarm group display thread");
    }

    //Create the alarm group display thread
    status = pthread_create (&display_thread, NULL, display_alarm_thread, NULL);
    if (status != 0) {
        err_abort(status, "Remove alarm group display thread");
    }

    // Main loop to handle user commands
    while (1) {
        printf ("Alarm> ");
        if (fgets (line, sizeof(line), stdin) == NULL) exit (0);    // Exit on EOF
        if (strlen (line) <= 1) continue;   // Ignore empty input

        char command[16];
        int alarm_id, group_id, seconds;
        char message[128];
        
        // Parse the input as a command
        if (sscanf(line, "%15s", command) == 1) {
            if (sscanf(line, "Start_Alarm(%d): Group(%d) %d %[^\n]", &alarm_id, &group_id, &seconds, message) == 4) {
                // // Parse Start_Alarm command
                // if (sscanf(line, "%*s(%d): Group(%d) %d %[^\n]", &alarm_id, &group_id, &seconds, message) != 4){
                //     fprintf(stderr, "Bad Start_Alarm command format\n");
                //     continue;
                // }
                
                // Create and initialize a new alarm
                alarm = (alarm_t*)malloc (sizeof (alarm_t));
                if (alarm == NULL) {
                    errno_abort ("Allocate alarm");
                }
                alarm -> alarm_id = alarm_id;
                alarm -> group_id = group_id;
                alarm -> seconds = seconds;
                alarm -> time = time(NULL) + seconds;
                strncpy(alarm -> message, message, sizeof(alarm -> message) - 1);
                alarm -> message[sizeof(alarm -> message) - 1] = '\0';

                // Insert the alarm into the list
                status = pthread_mutex_lock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Lock mutex");
                }
                alarm_insert(alarm);
                status = pthread_mutex_unlock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Unlock mutex");
                }
            } else if (sscanf(line, "Change_Alarm(%d): Group(%d) %d %[^\n]", &alarm_id, &group_id, &seconds, message) == 4) {
                // // Parse Change_Alarm command
                // if (sscanf(line, "%*s(%d): Group(%d) %[^\n]", &alarm_id, &group_id, &seconds, message) != 4) {
                //     fprintf(stderr, "Invalid Change_Alarm command format\n");
                //     continue;
                // }

                // Modify an existing alarm
                status = pthread_mutex_lock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Lock mutex");
                }
                alarm_t *current = alarm_list;

                while (current != NULL) {
                    if (current->alarm_id == alarm_id) { // Match using alarm_id
                        current->group_id = group_id;
                        current->seconds = seconds;
                        current->time = time(NULL) + seconds;
                        strncpy(current->message, message, sizeof(current->message) - 1);
                        current->message[sizeof(current->message) - 1] = '\0';
                        printf("Alarm(%d) updated successfully\n", alarm_id);
                        break;
                    }
                    current = current->link;
                }
                if (current == NULL) {
                    fprintf(stderr, "Alarm(%d) not found\n", alarm_id);
                }
                status = pthread_mutex_unlock(&alarm_mutex);

                if (status != 0) {
                    err_abort(status, "Unlock mutex");
                }
            } else if (sscanf(line, "Cancel_Alarm(%d)", &alarm_id) == 1) {
                status = pthread_mutex_lock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Lock mutex");
                }
                
                cancel_alarm(alarm_id);
                
                status = pthread_mutex_unlock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Unlock mutex");
                }
            } else if (sscanf(line, "Suspend_Alarm(%d)", &alarm_id) == 1) {
                status = pthread_mutex_lock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Lock mutex");
                }

                suspend_alarm(alarm_id);

                status = pthread_mutex_unlock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Unlock mutex");
                }
            } else if (sscanf(line, "Reactivate_Alarm(%d)", &alarm_id) == 1) {
                status = pthread_mutex_lock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Lock mutex");
                }

                reactivate_alarm(alarm_id);

                status = pthread_mutex_unlock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Unlock mutex");
                }
            } else if (strcmp(line, "View_Alarms\n") == 0){
                status = pthread_mutex_lock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Lock mutex");
                }

                //view_alarms();

                status = pthread_mutex_unlock(&alarm_mutex);
                if (status != 0) {
                    err_abort(status, "Unlock mutex");
                }
            } else {
                fprintf(stderr, "Invalid request format\n");
            }
        }
    }
    return 0;
}

//Remove alarm from the list
void cancel_alarm(int alarm_id) {
    remove_alarm(alarm_id);
    alarm_t *alarm = find_alarm(alarm_id);
    printf("Alarm(%d) Canceled at %ld: %d %s\n", alarm->alarm_id, time(NULL), alarm->seconds, alarm->message);
}

//Suspend alarm in the list
void suspend_alarm(int alarm_id) {
    alarm_t *node = find_alarm(alarm_id);
    if(node && node->active == 1){
        node->active = 0;
        printf("Alarm(%d) Suspended at %ld: %d %s\n", node->alarm_id, time(NULL), node->seconds, node->message);
    }else {
        printf("Alarm(%d) Not Found or Already Suspended\n", alarm_id);
    }
}

//Reactivate alarm in the list
void reactivate_alarm(int alarm_id) {
    alarm_t *node = find_alarm(alarm_id);
    if(node && node->active == 0){
        node->active = 1;
        printf("Alarm(%d) Reactivated at %ld: %d %s\n", node->alarm_id, time(NULL), node->seconds, node->message);
    }else {
        printf("Alarm(%d) Not Found or Already Active\n", alarm_id);
    }
}

//Helper function to find alarm
alarm_t *find_alarm(int alarm_id) {
    alarm_t *current = alarm_list;

    /*
     * LOCKING PROTOCOL:
     * 
     * This routine requires that the caller have locked the
     * alarm_mutex!
     */

    // Traverse the list to find the alarm
    while (current != NULL) {
        if(current->alarm_id == alarm_id){
            return current; //Found the alarm
        }
        current = current->link; //Move to the next alarm
    }

    //If no alarm with the given ID is found, return NULL
    return NULL;
}

//Helper function to remove alarm
void remove_alarm(int alarm_id){
    int status;
    alarm_t **last = &alarm_list, *next = *last;
    
    /*
     * LOCKING PROTOCOL:
     * 
     * This routine requires that the caller have locked the
     * alarm_mutex!
     */

    // Traverse the list to remove the alarm
    while (next != NULL && next->alarm_id != alarm_id) {
        last = &next -> link;
        next = next -> link;
    }

    //If alarm is found, remove it from the list
    if(next != NULL){
        *last = next->link; //Skip over the alarm being removed
        free(next); //free the memory associated with the removed alarm
    }else {
        //Handle case where no alarm with th given ID exists
        printf("Alarm with ID %d not found.\n", alarm_id);
    }
}