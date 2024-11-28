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
    char                message[64];
} alarm_t;

// Global synchronization objects
pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;

// Shared alarm list and current alarm for condition signaling
alarm_t *alarm_list = NULL;
time_t current_alarm = 0;

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
        expired = 0 // Initialize expired

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

int main (int argc, char *argv[])
{
    int status;
    char line[128]; // Input buffer for user commands
    alarm_t *alarm;
    pthread_t alarm_handler_thread, display_creation_thread;

    // Create the alarm handler thread
    status = pthread_create (&alarm_handler_thread, NULL, alarm_thread, NULL);
    if (status != 0) {
        err_abort (status, "Create alarm thread");
    }

    // Create the alarm group display creation thread
    status = pthread_create(&display_creation_thread, NULL, alarm_group_display_creation, NULL);
    if (status != 0) {
        err_abort(status, "Create alarm group display thread");
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
            if (strcmp(command, "Start_Alarm") == 0) {
                // Parse Start_Alarm command
                if (sscanf(line, "%*s(%d): Group(%d) %d %[^\n]", &alarm_id, &group_id, &seconds, message) != 4){
                    fprintf(stderr, "Bad Start_Alarm command format\n");
                    continue;
                }
                
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
            } else if (strcmp(command, "Change_Alarm") == 0) {
                // Parse Change_Alarm command
                if (sscanf(line, "%*s(%d): Group(%d) %[^\n]", &alarm_id, &group_id, &seconds, message) != 4) {
                    fprintf(stderr, "Invalid Change_Alarm command format\n");
                    continue;
                }

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
            } else {
                fprintf(stderr, "Unknown command: %s\n", command);
            }
        }
    }
    return 0;
}
