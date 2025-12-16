#include "ascii_art.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <glob.h>
#include <pthread.h>
#include <sys/ioctl.h>

/**
 * @file shell.c
 * @brief Implementation of the Norseish shell, a custom Unix command-line interpreter.
 *
 * This file contains the source code for the Norseish shell, a Unix-like shell
 * that provides an interactive command-line interface for users to execute
 * programs, manage files, and interact with the operating system.
 *
 * Key Features:
 * * Command History: Stores and allows users to recall previously entered commands.
 * Inline Command Completion: Provides suggestions and auto-completion
 * as the user types commands.
 * Pathname Expansion (Globbing): Supports wildcard characters (*, ?)
 * to specify multiple files in a single command.
 * Input/Output Redirection: Allows users to redirect standard input,
 * standard output, and standard error streams.
 * Piping: Enables chaining multiple commands together, where the
 * output of one command becomes the input of the next.
 * Background Execution: Supports running commands in the background,
 * allowing users to continue using the shell while commands execute.
 * Delayed Command Execution: Schedules commands to be executed at a
 * specified time in the future.
 * Signal Handling: Implements robust signal handling for 'SIGINT',
 * 'SIGCHLD', and other signals to ensure proper shell behavior.
 * Built-in Commands: Includes implementations of common shell built-in
 * commands such as 'cd' (change directory).
 * Dynamic Memory Management: Uses 'malloc', 'realloc', and 'free'
 * to efficiently manage memory for command arguments, file paths,
 * and other data structures.
 * Terminal Interaction: Utilizes 'termios' functions for advanced
 * terminal control, including disabling input buffering and echoing.
 * Multi-threading: Uses POSIX threads for delayed command execution.
 *
 * The shell is designed to be a powerful and user-friendly alternative to
 * traditional Unix shells, with a focus on interactive features and
 * efficient command processing.
 *
 * @author John Seibert
 * @author Jack Norfolk
 *
 * @version 1.0
 * @date April 29, 2025
 * @section Testing
 *
 * **Commands Tested:**
 * ls
 * pwd
 * cd
 * mkdir
 * echo
 * cat
 * grep
 * find
 * sleep
 *  ./myprogram (execution of a user-created program)
 * sleep 10 & (background execution)
 * cat < input.txt > output.txt (input/output redirection)
 * ls -l | grep "myfile" (piping)
 * delay 10 echo thoughts (delayed execution)
 * ls * (globbing)
 *
 * @section Sources
 *
 * * 'termios' functions ('tcgetattr', 'tcsetattr'):
 * * https://man7.org/linux/man-pages/man3/termios.3.html
 * * 'ioctl' with 'TIOCGWINSZ':
 * * https://man7.org/linux/man-pages/man2/ioctl.2.html
 * * https://stackoverflow.com/questions/1022957/how-to-get-the-terminal-size-in-characters-in-c
 * * 'glob':
 * * https://man7.org/linux/man-pages/man3/glob.3.html
 * * 'disown':
 * * https://man7.org/linux/man-pages/man1/disown.1.html
 *
 */

#define MAX_HISTORY 100
#define MAX_COMMAND_LENGTH 256
#define INITIAL_COMPLETIONS_SIZE 20
#define MAX_ARGS 25
#define MAX_ALIASES 100

typedef struct {
    time_t scheduled_time;
    char command[MAX_COMMAND_LENGTH];
} DelayedCommand;

#define MAX_DELAYED_COMMANDS 100
DelayedCommand delayed_commands[MAX_DELAYED_COMMANDS];
int delayed_command_count = 0;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
pthread_t delayed_commands_thread;

void disableInputBuffering(struct termios *oldt);
void restoreInputBuffering(struct termios *oldt);
int isExecutable(const char *filepath);
void displayInlineCompletions(const char *prompt, const char *buf, const char *completion);
char **generateCompletions(const char *buf, int pos, int *count);
int readLine(const char *prompt, char *buf, int bufsize);
void titleScreen();
void cd(char *path);
void removeQuotes(char *str);
void executeCommand(char **args, int background);
void handlePipes(char **args, int num_commands, int background);
void addToHistory(const char *command);
void displayHistory();
int disownProcess(pid_t pid);
int expandWildcards(char **args, char ***expanded_args);
void *processDelayedCommands(void *arg);
void addDelayedCommand(time_t scheduled_time, const char *command);
void executeDelayedCommand(char *command);

char history[MAX_HISTORY][MAX_COMMAND_LENGTH];
int history_count = 0;

/**
 * @brief Retrieves the current width of the terminal window.
 *
 * This function uses the 'ioctl' system call with the 'TIOCGWINSZ' request to
 * get the current terminal window size. The width, in columns, is then extracted
 * from the returned 'struct winsize'.
 *
 * @return An integer representing the width of the terminal window in columns.
 *
 * @see https://man7.org/linux/man-pages/man2/ioctl.2.html
 * @see https://stackoverflow.com/questions/1022957/how-to-get-the-terminal-size-in-characters-in-c
 */
int get_terminal_width() {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_col;
}

/**
 * @brief Disables the terminal's input buffering (canonical mode) and echoing.
 *
 * This function modifies the terminal attributes associated with the standard
 * input ('STDIN_FILENO'). It first retrieves the current terminal settings
 * and stores them in the 'oldt' structure. Then, it creates a new 'termios'
 * structure based on the old settings and disables the 'ICANON' (canonical
 * input) and 'ECHO' flags. Disabling canonical mode allows the program to
 * receive input character by character, without waiting for a newline. Disabling
 * echoing prevents the typed characters from being displayed on the terminal.
 * Finally, it applies these new settings to the terminal.
 *
 * @param oldt A pointer to a 'struct termios' where the original terminal
 * attributes will be stored before modification. This is typically used later
 * to restore the terminal to its original state.
 *
 * @note If either 'tcgetattr' (to get the current settings) or 'tcsetattr'
 * (to set the new settings) fails, an error message is printed to the standard
 * error stream using 'perror', and the function will return without fully
 * disabling the input buffering.
 * @see https://man7.org/linux/man-pages/man3/termios.3.html
 */
void disableInputBuffering(struct termios *oldt) {
    struct termios newt;
    if (tcgetattr(STDIN_FILENO, oldt) != 0) {
        perror("tcgetattr");
        return;
    }
    newt = *oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) != 0) {
        perror("tcsetattr");
    }
}

/**
 * @brief Restores the terminal's input buffering settings.
 *
 * This function takes a pointer to a 'struct termios' that presumably holds
 * the original terminal settings. It then uses the 'tcsetattr' system call
 * to apply these saved settings to the standard input file descriptor
 * ('STDIN_FILENO'). This is typically used to revert the terminal to its
 * normal behavior after modifications like disabling canonical mode or echoing.
 *
 * @param oldt A pointer to a 'struct termios' containing the original terminal
 * attributes to be restored.
 *
 * @note If the 'tcsetattr' call fails, an error message is printed to the
 * standard error stream using 'perror'.
 * @see https://man7.org/linux/man-pages/man3/termios.3.html
 */
void restoreInputBuffering(struct termios *oldt) {
    if (tcsetattr(STDIN_FILENO, TCSANOW, oldt) != 0) {
        perror("tcsetattr");
    }
}

/**
 * @brief Checks if a given file path refers to an executable file.
 *
 * This function uses the 'stat' system call to retrieve information about the
 * file specified by 'filepath'. It then checks if the file has execute permissions
 * for the owner (user) and if it is not a directory.
 *
 * @param filepath A pointer to a null-terminated string representing the path
 * to the file to check.
 *
 * @return 1 if the file exists, is executable by the owner, and is not a
 * directory; otherwise, it returns 0.
 * 
 * @see https://man7.org/linux/man-pages/man2/stat.2.html
 */
int isExecutable(const char *filepath) {
    struct stat sb;
    if (stat(filepath, &sb) == 0) {
        return (sb.st_mode & S_IXUSR) && !S_ISDIR(sb.st_mode); 
        // Second statement (above) checks if it is not a directory.
    }
    return 0;
}

/**
 * @brief Displays inline command completion in the terminal.
 *
 * This function takes the current prompt, the user's input buffer, and a
 * potential completion string as input. It then prints a line to the terminal
 * that shows the prompt, the current buffer, and the suggested completion
 * displayed in a highlighted style (white and bold). The portion of the
 * completion that matches the current buffer is not re-printed.
 *
 * @param prompt A pointer to a null-terminated string representing the shell prompt.
 * @param buf A pointer to a null-terminated string containing the user's current input.
 * @param completion A pointer to a null-terminated string representing the
 * potential completion for the input buffer.
 *
 * @details The function uses ANSI escape codes to:
 * - '\33[2K': Clear the current line.
 * - '\r': Move the cursor to the beginning of the line.
 * - '\033[37m\033[1m': Set the text color to white and make it bold.
 * - '\033[0m': Reset the text formatting to default.
 *
 * It prints the prompt, the buffer, and then the part of the completion
 * string that comes *after* the content of the buffer. This provides
 * an inline suggestion to the user.
 */
void displayInlineCompletions(const char *prompt, const char *buf, const char *completion) {
    printf("\33[2K\r%s%s\033[37m\033[1m%s\033[0m", prompt, buf, completion + strlen(buf));
    fflush(stdout);
}

/**
 * @brief Generates a dynamically allocated array of command completions based on the current input buffer.
 *
 * This function analyzes the input buffer 'buf' up to the cursor position 'pos' to
 * provide filename and executable command completions. It dynamically allocates
 * memory for the array of completion strings and the strings themselves.
 *
 * The function works as follows:
 * 1. It determines the directory to search within. If the input buffer contains a
 * slash ('/'), it uses the directory path before the last slash. Otherwise,
 * it searches in the current directory ('.').
 * 2. It extracts the prefix to match against. This is the part of the input
 * buffer after the last slash (or the entire buffer if no slash exists).
 * 3. It opens the determined directory and reads its contents. For each entry
 * that starts with the extracted prefix and is not "." or "..", it adds the
 * entry's name (or the full path if a directory was specified) to the
 * 'completions' array. If the entry is a directory, a trailing slash is added.
 * 4. If the first word of the input buffer is being completed (no slashes), the
 * function also searches through the directories listed in the 'PATH'
 * environment variable for executable files that match the prefix. Duplicate
 * completions are avoided.
 * 5. The 'completions' array is dynamically resized as needed using 'realloc' to
 * accommodate more completions.
 * 6. The number of generated completions is stored in the integer pointed to by 'count'.
 *
 * @param buf A pointer to a null-terminated string representing the current input buffer.
 * @param pos The current cursor position within the input buffer. This is used to
 * determine the prefix for completion.
 * @param count A pointer to an integer where the number of generated completions
 * will be stored.
 *
 * @return A dynamically allocated null-terminated array of character pointers, where each
 * pointer points to a string representing a completion. Returns 'NULL' if
 * memory allocation fails at any point. It is the caller's responsibility to
 * free the memory allocated for this array and the individual completion strings
 * when they are no longer needed.
 * @see https://man7.org/linux/man-pages/man3/opendir.3.html
 * @see https://man7.org/linux/man-pages/man3/readdir.3.html
 * @see https://man7.org/linux/man-pages/man3/strdup.3.html
 * @see https://man7.org/linux/man-pages/man2/stat.2.html
 * @see https://man7.org/linux/man-pages/man3/realloc.3.html
 * @see https://man7.org/linux/man-pages/man3/strtok.3.html
 */
char **generateCompletions(const char *buf, int pos, int *count) {
    *count = 0;
    char *last_slash = strrchr(buf, '/');
    char dirname[MAX_COMMAND_LENGTH];
    char prefix[MAX_COMMAND_LENGTH];
    char **completions = malloc(INITIAL_COMPLETIONS_SIZE * sizeof(char *));
    int completions_capacity = INITIAL_COMPLETIONS_SIZE;

    if (completions == NULL) {
        perror("malloc");
        return NULL;
    }

    if (last_slash == NULL) {
        strcpy(dirname, ".");
        strncpy(prefix, buf, pos);
        prefix[pos] = '\0';
    } else {
        strncpy(dirname, buf, last_slash - buf);
        dirname[last_slash - buf] = '\0';
        strcpy(prefix, last_slash + 1);
    }

    DIR *d;
    struct dirent *ent;
    if ((d = opendir(dirname)) != NULL) {
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(prefix, ent->d_name, strlen(prefix)) == 0 
            && strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                char *completion;
                if (last_slash == NULL) {
                    completion = strdup(ent->d_name);
                } else {
                    completion = malloc(strlen(dirname) + 1 + strlen(ent->d_name) + 1); 
                    // +1 for '/', +1 for null terminator
                    if (completion == NULL) {
                        perror("malloc");
                        break;
                    }
                    strcpy(completion, dirname);
                    strcat(completion, "/");
                    strcat(completion, ent->d_name);
                }

                if (completion == NULL) {
                    perror("strdup/malloc");
                    continue;
                }

                struct stat sb;
                char fullpath[MAX_COMMAND_LENGTH * 2];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", dirname, ent->d_name);
                if (stat(fullpath, &sb) == 0 && S_ISDIR(sb.st_mode)) {
                    char *tmp = realloc(completion, strlen(completion) + 2);
                    if (tmp != NULL) {
                        completion = tmp;
                        strcat(completion, "/");
                    }
                }

                if (*count >= completions_capacity) {
                    completions_capacity *= 2;
                    char **tmp = realloc(completions, completions_capacity * sizeof(char *));
                    if (tmp == NULL) {
                        perror("realloc");
                        free(completion);
                        for (int i = 0; i < *count; i++) {
                            free(completions[i]);
                        }
                        free(completions);
                        closedir(d);
                        return NULL;
                    }
                    completions = tmp;
                }
                completions[*count] = completion;
                (*count)++;
            }
        }
        closedir(d);
    }

    // Add executables from PATH if the first word is being completed
    if (last_slash == NULL) {
        char *path_env = getenv("PATH");
        if (path_env != NULL) {
            char *path = strdup(path_env);
            if (path == NULL) {
                perror("strdup");
                return completions;
            }
            char *dir = strtok(path, ":");
            while (dir != NULL) {
                DIR *path_d;
                struct dirent *path_ent;
                if ((path_d = opendir(dir)) != NULL) {
                    while ((path_ent = readdir(path_d)) != NULL) {
                        if (path_ent->d_type == DT_REG 
                            && strncmp(prefix, path_ent->d_name, strlen(prefix)) == 0) {
                            char filepath[MAX_COMMAND_LENGTH * 2];
                            snprintf(filepath, sizeof(filepath), "%s/%s", dir, path_ent->d_name);
                            if (isExecutable(filepath)) {
                                char *completion = strdup(path_ent->d_name);
                                if (completion == NULL) {
                                    perror("strdup");
                                    continue;
                                }
                                int found = 0;
                                for (int i = 0; i < *count; i++) {
                                    if (strcmp(completions[i], completion) == 0) {
                                        free(completion);
                                        found = 1;
                                        break;
                                    }
                                }
                                if (!found) {
                                    if (*count >= completions_capacity) {
                                        completions_capacity *= 2;
                                        char **tmp = realloc(completions, completions_capacity * sizeof(char *));
                                        if (tmp == NULL) {
                                            perror("realloc");
                                            free(completion);
                                            for (int i = 0; i < *count; i++) {
                                                free(completions[i]);
                                            }
                                            free(completions);
                                            closedir(path_d);
                                            free(path);
                                            return NULL;
                                        }
                                        completions = tmp;
                                    }
                                    completions[*count] = completion;
                                    (*count)++;
                                }
                            }
                        }
                    }
                    closedir(path_d);
                }
                dir = strtok(NULL, ":");
            }
            free(path);
        }
    }
    return completions;
}

/**
 * @brief Reads a line of input from the user, supporting command history (using up/down arrows) and Vim-like tab completion.
 *
 * This function displays the given 'prompt' and reads user input into the provided
 * buffer 'buf' up to a maximum size of 'bufsize'. It enables non-canonical input
 * mode to process keystrokes immediately. The function supports the following
 * features:
 * - Tab Completion: Pressing the Tab key triggers command and filename
 * completion based on the current input. It uses the 'generateCompletions'
 * function to get a list of possible completions and 'displayInlineCompletions'
 * to show the suggestions. Subsequent Tab presses cycle through the completions.
 * - Command History: The Up and Down arrow keys allow navigation through
 * the command history (stored in the global 'history' array).
 * - Left/Right Arrows: Move the cursor within the input buffer. When
 * completions are active, these keys cycle through the suggestions.
 * - Backspace: Deletes the character to the left of the cursor.
 * - Enter/Return: Terminates input and returns the length of the entered line.
 * - Escape sequences: Handles arrow key inputs.
 *
 * The function temporarily disables input buffering and echoing using
 * 'disableInputBuffering' and restores the original settings using
 * 'restoreInputBuffering' before returning. It dynamically allocates memory
 * for the completion suggestions and frees it before exiting the loop or when
 * new completions are generated.
 *
 * @param prompt A pointer to a null-terminated string to be displayed as the input prompt.
 * @param buf A pointer to a character buffer where the user's input will be stored.
 * @param bufsize The maximum size of the input buffer to prevent overflow.
 *
 * @return The number of characters read into the buffer (excluding the null terminator).
 * Returns -1 on error (though the current implementation doesn't explicitly return -1).
 * 
 * @see https://man7.org/linux/man-pages/man3/termios.3.html
 */
int readLine(const char *prompt, char *buf, int bufsize) {
    struct termios oldt;
    disableInputBuffering(&oldt);

    int pos = 0;
    int history_index = history_count;
    char **completions = NULL;
    int completion_count = 0;
    int completion_index = -1;
    int original_prefix_length = 0;

    buf[0] = '\0'; // Initialize the buffer

    while (1) {
        printf("\33[2K\r%s%s", prompt, buf); // Use combined format string
        fflush(stdout);

        int c = getchar();
        if (c == 9) { // Tab
            if (completions != NULL) {
                for (int i = 0; i < completion_count; i++) {
                    free(completions[i]);
                }
                free(completions);
                completions = NULL;
                completion_count = 0;
                completion_index = -1;
            }
            completions = generateCompletions(buf, pos, &completion_count);
            if (completions != NULL && completion_count > 0) {
                completion_index = (completion_index + 1) % completion_count;
                displayInlineCompletions(prompt, buf, completions[completion_index]);
                original_prefix_length = pos;
                strncpy(buf, completions[completion_index], bufsize - 1);
                buf[bufsize - 1] = '\0';
                pos = strlen(buf);
            } else {
                printf("\a");
                fflush(stdout);
            }
            continue;
        } else if (c == 27) { // Escape
            int c1 = getchar();
            if (c1 == '[') {
                int c2 = getchar();
                if (c2 == 'A') { // Up Arrow
                    if (history_index > 0) {
                        history_index--;
                        strncpy(buf, history[history_index], bufsize - 1);
                        buf[bufsize - 1] = '\0';
                        pos = strlen(buf);
                        original_prefix_length = 0;
                        if (completions != NULL) {
                            for (int i = 0; i < completion_count; i++) free(completions[i]);
                            free(completions);
                            completions = NULL;
                            completion_count = 0;
                            completion_index = -1;
                        }
                    }
                } else if (c2 == 'B') { // Down Arrow
                    if (history_index < history_count) {
                        if (history_index < history_count - 1) {
                            history_index++;
                            strncpy(buf, history[history_index], bufsize - 1);
                            buf[bufsize - 1] = '\0';
                        } else {
                            buf[0] = '\0';
                            pos = 0;
                        }
                        pos = strlen(buf);
                        original_prefix_length = 0;
                        if (completions != NULL) {
                            for (int i = 0; i < completion_count; i++) free(completions[i]);
                            free(completions);
                            completions = NULL;
                            completion_count = 0;
                            completion_index = -1;
                        }
                    }
                } else if (c2 == 'D') { // Left Arrow
                    if (completions != NULL && completion_count > 0) {
                        completion_index = (completion_index - 1 + completion_count) % completion_count;
                        displayInlineCompletions(prompt, buf, completions[completion_index]);
                        strncpy(buf, completions[completion_index], bufsize - 1);
                        buf[bufsize - 1] = '\0';
                        pos = strlen(buf);
                    } else {
                        if (pos > 0) {
                            pos--;
                            printf("\b");
                            fflush(stdout);
                        }
                    }
                } else if (c2 == 'C') { // Right Arrow
                    if (completions != NULL && completion_count > 0) {
                        completion_index = (completion_index + 1) % completion_count;
                        displayInlineCompletions(prompt, buf, completions[completion_index]);
                        strncpy(buf, completions[completion_index], bufsize - 1);
                        buf[bufsize - 1] = '\0';
                        pos = strlen(buf);
                    } else {
                        if (pos < strlen(buf)) {
                            pos++;
                            printf("\033[C");
                            fflush(stdout);
                        }
                    }
                }
            }
            continue;
        } else if (c == '\n' || c == '\r') {
            putchar('\n');
            buf[pos] = '\0';
            original_prefix_length = 0;
            if (completions != NULL) {
                for (int i = 0; i < completion_count; i++) free(completions[i]);
                free(completions);
            }
            break;
        } else if (c == 127 || c == 8) { // Backspace and Ctrl+Backspace
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                printf("\b \b");
                fflush(stdout);
                if (pos < original_prefix_length) {
                    original_prefix_length = pos;
                    if (completions != NULL) {
                        for (int i = 0; i < completion_count; i++) free(completions[i]);
                        free(completions);
                        completions = NULL;
                        completion_count = 0;
                        completion_index = -1;
                    }
                }
            }
            continue;
        } else {
            if (pos < bufsize - 1) {
                buf[pos++] = c;
                buf[pos] = '\0';
                putchar(c);
                fflush(stdout);
                if (original_prefix_length > 0 && pos > original_prefix_length) {
                    original_prefix_length = pos;
                    if (completions != NULL) {
                        for (int i = 0; i < completion_count; i++) free(completions[i]);
                        free(completions);
                        completions = NULL;
                        completion_count = 0;
                        completion_index = -1;
                    }
                }
            }
        }
    }
    if (completions != NULL) {
        for (int i = 0; i < completion_count; i++) free(completions[i]);
        free(completions);
    }
    restoreInputBuffering(&oldt);
    return pos;
}


/**
 * @brief Displays the title screen animation and waits for user input.
 *
 * This function clears the terminal screen, prints an ASCII art title for the
 * shell along with creator information, and then displays a simple
 * animation (of Pacman eating pellets). It sets the terminal to raw mode to capture 
 * single key presses without requiring the Enter key. After a key is pressed, it restores 
 * the original terminal settings and clears the screen again.
 *
 * The animation is controlled by 'frame_delay' and 'animation_cycles', and the
 * starting row for the Pac-Man animation is defined by 'pacman_start_row'.
 * The function relies on another function, 'get_frame()', to retrieve the
 * ASCII art frames for the animation.
 *
 * @note This function modifies the terminal settings temporarily. It's crucial
 * that the original settings are restored before the program exits to avoid
 * leaving the terminal in an unusable state.
 * @note usleep is the same functioning command as sleep from Java, specified in microseconds
 */
void titleScreen() {
    struct termios oldt, newt;
    const double frame_delay = 0.25;       // Faster delay for eating illusion
    const int animation_cycles = 6;         // Number of eating cycles
    const int pacman_start_row = 1;       // Row to start printing Pac-Man

    // Save current terminal settings
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) {
        perror("tcgetattr");
        return;
    }

    // Set terminal to raw mode
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echoing
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) != 0) {
        perror("tcsetattr");
        return;
    }

    // Clear the screen
    printf("\033[2J\033[H");

    // Print the title
    printf("\033[33m"
           " NN   NN OOOOOO RRRRRR SSSSSS EEEEEE  IIIII  SSSSSS HH  HH\n"
           " NNN  NN OO  OO RR  RR SS     EE        I    SS     HH  HH\n"
           " NNNN NN OO  OO RRRRRR  SSSS  EEEEE     I     SSSS  HHHHHH\n"
           " NN NNNN OO  OO RR RR      SS EE        I        SS HH  HH\n"
           " NN  NNN OOOOOO RR  RR SSSSSS EEEEEE  IIIII  SSSSSS HH  HH\n"
           "\033[0m\n\n");

    printf("\033[36m"
           "Welcome to the Norseish Shell\n"
           "Created by John Seibert and Jack Norfolk\n"
           "Press any key to continue!\n"
           "\033[0m");
    fflush(stdout);

    printf("\033[%dB", pacman_start_row);

    // "Pac-man" animation
    for (int i = 0; i < animation_cycles * 2; i++) {
        const char *ascii_art = get_frame(i % 2);
        if (ascii_art != NULL) {
            // Print the frame
            printf("%s\n", ascii_art);
            fflush(stdout);
            usleep((int)(frame_delay * 1000000));

            // Move cursor up to overwrite
            printf("\033[%dA", animation_cycles * 2 - 1);

        } else {
            printf("Error: Could not retrieve artwork!\n");
            break;
        }
    }

    fflush(stdout);

    // Wait for a single character input without pressing enter
    getchar();

    // Restore the original terminal settings
    if (tcsetattr(STDIN_FILENO, TCSANOW, &oldt) != 0) {
        perror("tcsetattr");
    }
    printf("\033[2J\033[H"); // clear screen
}

/**
 * @brief Changes the current working directory.
 *
 * This function changes the current working directory to the specified 'path'.
 * It uses the 'chdir' system call to perform the directory change.
 * If the directory change is successful, it prints a message to the standard
 * output. If it fails, it prints an error message to the standard error stream
 * using 'perror'.
 *
 * @param path A pointer to a null-terminated string representing the path to
 * the directory to change to.
 * @see https://man7.org/linux/man-pages/man2/chdir.2.html
 */
void cd(char *path) {
    if (chdir(path) != 0) {
        perror("cd");
    }
}

/**
 * @brief Removes double quotes from a string.
 *
 * This function processes an input string and removes all occurrences of the
 * double quote character ('"'). It modifies the input string in-place. This
 * is particularly useful for handling the output of commands like 'echo "message"'
 * when piped to other commands like 'wc', where the quotes might interfere with
 * accurate length calculations.
 *
 * @param str A pointer to a null-terminated character buffer. This buffer will
 * be modified directly by the function to remove the double quotes.
 *
 * @note The function modifies the input string in-place. Ensure that the
 * memory pointed to by 'str' is writable and large enough to accommodate the
 * string after the removal of quotes (which will always be shorter or the same
 * length).
 * @see https://man7.org/linux/man-pages/man2/fork.2.html
 * @see https://man7.org/linux/man-pages/man3/execvp.3.html
 * @see https://man7.org/linux/man-pages/man2/wait.2.html
 */
void removeQuotes(char *str) {
    char *src = str;
    char *dest = str;
    while (*src) {
        if (*src != '"') {
            *dest++ = *src;
        }
        src++;
    }
    *dest = '\0';
}

/**
 * @brief Executes a command with its arguments, handling both foreground and background 
 * execution, as well as input/output redirection.
 *
 * This function forks a child process to execute the specified command. In the
 * child process, it restores the default signal handlers for 'SIGINT', 'SIGQUIT',
 * 'SIGTSTP', and 'SIGCHLD'. It then checks for input redirection ('<'), output
 * redirection ('>'), and append redirection ('>>') in the command arguments. If
 * any of these are found, it opens the corresponding file and uses 'dup2' to
 * redirect the standard input or standard output of the child process. Finally,
 * it executes the command using 'execvp'. If 'execvp' fails, an error message
 * is printed.
 *
 * In the parent process, if the 'background' flag is false (foreground execution),
 * it waits for the child process to complete and retrieves its exit status or
 * termination signal. If the 'background' flag is true, it prints the process ID
 * of the child process and then attempts to detach it using the 'disownProcess'
 * function (which is assumed to be defined elsewhere).
 *
 * @param args A null-terminated array of character pointers representing the
 * command and its arguments (e.g., {"ls", "-l", NULL}). The first element
 * ('args[0]') should be the command to execute. Redirection operators ("<", ">",
 * ">>") and their corresponding filenames can be included in this array.
 * @param background An integer flag. If non-zero, the command is executed in
 * the background; otherwise, it's executed in the foreground and the parent
 * waits for its completion.
 *
 * @note This function assumes that the 'disownProcess' function is defined
 * elsewhere and handles the detachment of background processes. It also handles
 * basic input and output redirection but does not support more complex scenarios
 * like pipes or multiple redirections in a single command. Error handling is
 * performed using 'perror' and exiting the child process on failure.
 * 
 * @see https://man7.org/linux/man-pages/man2/pipe.2.html
 * @see https://man7.org/linux/man-pages/man2/fork.2.html
 * @see https://man7.org/linux/man-pages/man2/dup2.html
 * @see https://man7.org/linux/man-pages/man3/execvp.3.html
 * @see https://man7.org/linux/man-pages/man2/wait.2.html
 */
void executeCommand(char **args, int background) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        // Restore default signal handlers for child process.  This is important
        // for proper job control.  The parent shell might have set signal handlers
        // that we don't want the child process to inherit.
        signal(SIGINT, SIG_DFL);  // Restore default behavior for Ctrl+C
        signal(SIGQUIT, SIG_DFL); // Restore default behavior for Ctrl+backslash
        signal(SIGTSTP, SIG_DFL); // Restore default behavior for Ctrl+Z
        signal(SIGCHLD, SIG_DFL);

        int inPos = -1, outPos = -1, appendPos = -1;
        for (int j = 0; j < MAX_ARGS - 1 && args[j] != NULL; j++) {
            if (strcmp(args[j], "<") == 0) inPos = j;
            if (strcmp(args[j], ">") == 0) outPos = j;
            if (strcmp(args[j], ">>") == 0) appendPos = j;
        }
        // Input redirection
        if (inPos != -1 && args[inPos + 1] != NULL) {
            FILE *inFile = fopen(args[inPos + 1], "r");
            if (inFile == NULL) {
                perror("fopen");
                exit(1);
            }
            dup2(fileno(inFile), STDIN_FILENO);
            fclose(inFile);
            args[inPos] = NULL;
        }
        // Output redirection
        if (outPos != -1 && args[outPos + 1] != NULL) {
            FILE *outFile = fopen(args[outPos + 1], "w");
            if (outFile == NULL) {
                perror("fopen");
                exit(1);
            }
            dup2(fileno(outFile), STDOUT_FILENO);
            fclose(outFile);
            args[outPos] = NULL;
        }
        // Append redirection
        if (appendPos != -1 && args[appendPos + 1] != NULL) {
            FILE *appendFile = fopen(args[appendPos + 1], "a");
            if (appendFile == NULL) {
                perror("fopen");
                exit(1);
            }
            dup2(fileno(appendFile), STDOUT_FILENO);
            fclose(appendFile);
            args[appendPos] = NULL;
        }

        execvp(args[0], args);
        perror("execvp"); // Only gets here if execvp fails
        exit(1);
    } else if (pid > 0) {
        // Parent process
        if (!background) {
            // Wait for foreground process to complete
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status)) {
                // printf("Child process exited with status %d\n", WEXITSTATUS(status)); //debugging
            } else if (WIFSIGNALED(status)) {
                // printf("Child process terminated by signal %d\n", WTERMSIG(status)); //debugging
            }
        } else {
            // Print the PID of the background process
            printf("[Background] Process ID: %d\n", pid);
            // Detach the process using signal handling.
            if (disownProcess(pid) != 0) {
                perror("disownProcess");
            }
        }
    } else {
        perror("fork");
        exit(1); // Exit on fork error.
    }
}

/**
 * @brief Disowns a process, allowing it to continue running after the shell exits.
 *
 * This function disowns the process with the given process ID ('pid') by
 * creating a new process and making the original process a child of the
 * new process.  The new process then calls setsid to detach from the
 * terminal.
 *
 * @param pid The process ID of the process to disown.
 *
 * @return 0 on success, -1 on error.
 * @see https://man7.org/linux/man-pages/man1/disown.1.html
 * @see https://www.man7.org/linux/man-pages/man2/sigaction.2.html
 */
int disownProcess(pid_t pid) {
    // Set the signal handler for SIGCHLD to ignore the child process.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDSTOP; // Important: Don't get notified on STOP/CONTINUE
    if (sigaction(SIGCHLD, &sa, NULL) != 0) {
        perror("sigaction");
        return -1;
    }
    return 0;
}

/**
 * @brief Handles the execution of commands connected by pipes.
 *
 * This function takes an array of arguments representing a pipeline of commands
 * and executes them by creating a series of child processes connected by pipes.
 * It iterates through the arguments, identifying the pipe symbols ("|") to
 * determine the individual commands in the pipeline. For each command, it
 * creates a child process, sets up the necessary pipe file descriptors,
 * and executes the command using 'execvp'. The parent process waits for all
 * child processes to complete, unless the last command is to be run in the
 * background.
 *
 * @param args A null-terminated array of character pointers representing the
 * commands and their arguments, including pipe symbols ("|").
 * @param num_commands The number of commands in the pipeline.
 * @param background An integer flag indicating whether the last command in the
 * pipeline should be executed in the background (1) or foreground (0).
 *
 * @note This function uses 'pipe', 'fork', 'dup2', 'close', 'execvp', and
 * 'waitpid' system calls.  It handles errors during pipe creation,
 * process creation, and execution.
 * @note dup2 is functionality same as dup in C, but user specifies file descriptor
 */
void handlePipes(char **args, int num_commands, int background) {
    int pipefd[2 * (num_commands - 1)];
    for (int i = 0; i < num_commands - 1; i++) {
        if (pipe(pipefd + i * 2) < 0) {
            perror("pipe");
            exit(1);
        }
    }

    int arg_index = 0;
    for (int i = 0; i < num_commands; i++) {
        char *command_args[MAX_ARGS];
        int current_arg = 0;
        while (args[arg_index] != NULL && strcmp(args[arg_index], "|") != 0) {
            command_args[current_arg++] = args[arg_index++];
        }
        command_args[current_arg] = NULL; // Null-terminate arguments for the current command
        if (args[arg_index] != NULL && strcmp(args[arg_index], "|") == 0) {
            arg_index++; // Move past the pipe symbol
        }

        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            // Set up input from the previous pipe, if not the first command
            if (i > 0) {
                dup2(pipefd[(i - 1) * 2], STDIN_FILENO);
            }

            // Set up output to the next pipe, if not the last command
            if (i < num_commands - 1) {
                dup2(pipefd[i * 2 + 1], STDOUT_FILENO);
            }

            // Close all pipe ends in the child
            for (int k = 0; k < 2 * (num_commands - 1); k++) {
                close(pipefd[k]);
            }

            if (command_args[0] != NULL) {
                execvp(command_args[0], command_args);
                perror("execvp");
            }
        exit(1);
        } else if (pid < 0) {
            perror("fork");
            exit(1);
        }
    }

    // Close all pipe ends in the parent
    for (int i = 0; i < 2 * (num_commands - 1); i++) {
        close(pipefd[i]);
    }

    // Wait for all child processes if not in the background
    if (!background) {
        for (int i = 0; i < num_commands; i++) {
            wait(NULL);
        }
    }
}

/**
 * @brief Adds a command to the command history.
 *
 * This function adds the given 'command' string to the global 'history' array.
 * The command is added to the end of the history, and the 'history_count'
 * is incremented. If the history is full (reaches 'MAX_HISTORY'), the oldest
 * command is overwritten.
 *
 * @param command A pointer to a null-terminated string representing the
 * command to add to the history.
 */
void addToHistory(const char *command) {
    if (history_count < MAX_HISTORY) {
        strcpy(history[history_count], command);
        history_count++;
    } else {
        // Wrap around the history buffer
        strcpy(history[history_count % MAX_HISTORY], command);
        history_count++;
    }
}

/**
 * @brief Displays the command history.
 *
 * This function iterates through the stored command history and prints each
 * command along with its index number to the standard output. If the history
 * contains more than 10 commands, it will only display the last 10 entries.
 */
void displayHistory() {
    int start = 0;
    if (history_count > 10) {
        start = history_count - 10;
    }
    for (int i = start; i < history_count; i++) {
        printf("  %d  %s\n", i + 1, history[i]);
    }
}

/**
 * @brief Expands wildcards in command arguments using globbing.
 *
 * This function takes an array of command arguments ('args') and expands any
 * arguments that contain wildcard characters (*, ?, []) using the 'glob'
 * function.  It dynamically allocates memory for the expanded arguments.
 *
 * @param args A null-terminated array of character pointers representing the
 * command arguments.
 * @param expanded_args A pointer to a pointer to an array of character pointers.
 * This will be updated to point to the dynamically allocated array of expanded
 * arguments.  It is the caller's responsibility to free this memory.
 *
 * @return The number of expanded arguments, or -1 on error.
 * @see https://man7.org/linux/man-pages/man3/glob.3.html
 * @see https://linux.die.net/man/3/globfree
 */
int expandWildcards(char **args, char ***expanded_args) {
    glob_t glob_result;
    size_t num_expanded = 0;
    *expanded_args = NULL;

    for (int i = 0; args[i] != NULL; i++) {
        // Check if the argument contains any wildcard characters
        if (strchr(args[i], '*') != NULL || strchr(args[i], '?') != NULL || strchr(args[i], '[') != NULL) {
            // Perform globbing
            int ret = glob(args[i], GLOB_NOCHECK | GLOB_TILDE, NULL, &glob_result);
            if (ret == 0) {
                // Success
                for (size_t j = 0; j < glob_result.gl_pathc; j++) {
                    // Add each expanded argument to the result
                    char *new_arg = strdup(glob_result.gl_pathv[j]);
                    if (new_arg == NULL) {
                        perror("strdup");
                        // Clean up already expanded arguments
                        for (size_t k = 0; k < num_expanded; k++) {
                            free((*expanded_args)[k]);
                        }
                        free(*expanded_args);
                        globfree(&glob_result);
                        return -1;
                    }
                    // Dynamically resize the expanded arguments array
                    char **temp_expanded_args = realloc(*expanded_args, (num_expanded + 1) * sizeof(char *));
                    if (temp_expanded_args == NULL) {
                        perror("realloc");
                        free(new_arg);
                        for (size_t k = 0; k < num_expanded; k++) {
                            free((*expanded_args)[k]);
                        }
                        free(*expanded_args);
                        globfree(&glob_result);
                        return -1;
                    }
                    *expanded_args = temp_expanded_args;
                    (*expanded_args)[num_expanded++] = new_arg;
                }
            } else if (ret == GLOB_NOMATCH) {
                // No matches found, keep the original argument
                char *new_arg = strdup(args[i]);
                if (new_arg == NULL)
                {
                    perror("strdup");
                    return -1;
                }
                char **temp_expanded_args = realloc(*expanded_args, (num_expanded + 1) * sizeof(char *));
                    if (temp_expanded_args == NULL) {
                        perror("realloc");
                        free(new_arg);
                        for (size_t k = 0; k < num_expanded; k++) {
                            free((*expanded_args)[k]);
                        }
                        free(*expanded_args);
                        return -1;
                    }
                *expanded_args = temp_expanded_args;
                (*expanded_args)[num_expanded++] = new_arg;

            } else {
                // Error during globbing
                fprintf(stderr, "glob error: %d\n", ret);
                globfree(&glob_result);
                return -1;
            }
            globfree(&glob_result);
        } else {
            // No wildcard detected. We copy the original argument
            char *new_arg = strdup(args[i]);
             if (new_arg == NULL)
                {
                    perror("strdup");
                    return -1;
                }
            char **temp_expanded_args = realloc(*expanded_args, (num_expanded + 1) * sizeof(char *));
            if (temp_expanded_args == NULL) {
                perror("realloc");
                free(new_arg);
                 for (size_t k = 0; k < num_expanded; k++) {
                        free((*expanded_args)[k]);
                    }
                free(*expanded_args);
                return -1;
            }
            *expanded_args = temp_expanded_args;
            (*expanded_args)[num_expanded++] = new_arg;
        }
    }

    char **temp_expanded_args = realloc(*expanded_args, (num_expanded + 1) * sizeof(char *));
    if (temp_expanded_args == NULL)
    {
         perror("realloc");
         for (size_t k = 0; k < num_expanded; k++) {
                free((*expanded_args)[k]);
            }
        free(*expanded_args);
        return -1;
    }
    *expanded_args = temp_expanded_args;
    (*expanded_args)[num_expanded] = NULL;
    return num_expanded;
}


/**
 * @brief Thread function to process delayed commands.
 *
 * This function is executed by a separate thread. It continuously checks the
 * 'delayed_commands' queue for commands that are ready to be executed
 * (i.e., their scheduled time has arrived).  It uses a mutex and condition
 * variable for synchronization.
 *
 * @param arg A pointer to void, which is unused in this implementation.
 *
 * @return A pointer to void.  Returns NULL.
 * @see https://man7.org/linux/man-pages/man3/pthread_create.3.html
 * @see https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3.html
 * @see https://man7.org/linux/man-pages/man3/pthread_cond_wait.3.html
 * @see https://man7.org/linux/man-pages/man3/pthread_mutex_unlock.3.html
 * @see https://man7.org/linux/man-pages/man3/pthread_self.3.html
 */
void *processDelayedCommands(void *arg) {
    while (1) {
        pthread_mutex_lock(&queue_mutex);
        // If the queue is empty, wait until signaled.
        while (delayed_command_count == 0) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        time_t now = time(NULL);
        // Find the command with the earliest scheduled time.
        int earliest_index = -1;
        time_t earliest_time = now + 1;
        for (int i = 0; i < delayed_command_count; i++) {
            if (delayed_commands[i].scheduled_time <= now) {
                earliest_index = i;
                break; // Execute the first command that is ready
            }
            if (delayed_commands[i].scheduled_time < earliest_time) {
                earliest_time = delayed_commands[i].scheduled_time;
                earliest_index = i;
            }
        }
        // If a command is ready to execute, execute it.
        if (earliest_index != -1) {
            DelayedCommand command_to_execute = delayed_commands[earliest_index];
            // Remove the command from the queue.
            if (earliest_index == 0) {
                // If it's the first element, shift the array.
                for (int i = 0; i < delayed_command_count - 1; i++) {
                    delayed_commands[i] = delayed_commands[i + 1];
                }
            } else if (earliest_index == delayed_command_count - 1) {
            // Do nothing.
            } else {
                for (int i = earliest_index; i < delayed_command_count - 1; i++) {
                    delayed_commands[i] = delayed_commands[i + 1];
                }
            }
            delayed_command_count--;
            pthread_mutex_unlock(&queue_mutex);
            // Execute command; make sure this is done outside the mutex lock.
            executeDelayedCommand(command_to_execute.command);
        } else {
            // Calculate the time to wait until the next command is ready.
            time_t now = time(NULL);
            time_t time_to_wait = earliest_time - now;
            if (time_to_wait > 0) {
                // Use a timed wait.
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += time_to_wait;
                pthread_cond_timedwait(&queue_cond, &queue_mutex, &ts);
            }
            pthread_mutex_unlock(&queue_mutex);
        }
    }
    return NULL;
}
    

/**
 * @brief Adds a command to the delayed command queue.
 *
 * This function adds a command to the 'delayed_commands' queue to be executed
 * at a later time. It stores the command and its scheduled execution time.
 * It uses a mutex to protect access to the queue and signals a condition
 * variable to wake up the thread that processes the queue.
 *
 * @param scheduled_time The time at which the command should be executed,
 * represented as a 'time_t' value.
 * @param command A pointer to a null-terminated string representing the
 * command to be executed.
 * @see https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3.html
 * @see https://man7.org/linux/man-pages/man3/pthread_mutex_unlock.3.html
 * @see https://man7.org/linux/man-pages/man3/pthread_cond_signal.3.html
 */
void addDelayedCommand(time_t scheduled_time, const char *command) {
    pthread_mutex_lock(&queue_mutex);
    if (delayed_command_count < MAX_DELAYED_COMMANDS) {
        // Find the correct position to insert the new command, maintaining sorted order.
        int i;
        for (i = 0; i < delayed_command_count; i++) {
            if (scheduled_time < delayed_commands[i].scheduled_time) {
                break;
            }
        }
        // Shift existing commands to make space for the new one.
        for (int j = delayed_command_count; j > i; j--) {
            delayed_commands[j] = delayed_commands[j - 1];
        }
        delayed_commands[i].scheduled_time = scheduled_time;
        strcpy(delayed_commands[i].command, command);
        delayed_command_count++;
        // Signal the worker thread that a new command has been added.
        pthread_cond_signal(&queue_cond);
    } else {
        fprintf(stderr, "Delayed command queue is full.\n");
    }
    pthread_mutex_unlock(&queue_mutex);
}

/**
 * @brief Executes a delayed command.
 *
 * This function executes the given command using the 'system' function.
 * It also adds the command to the command history.
 *
 * @param command A pointer to a null-terminated string representing the
 * command to execute.
 * @see https://man7.org/linux/man-pages/man3/system.3.html
 */
void executeDelayedCommand(char *command) {
        char *args[MAX_ARGS];
        char *token;
        int i = 0;
        token = strtok(command, " ");
        while (token != NULL && i < MAX_ARGS - 1) {
             args[i++] = token;
             token = strtok(NULL, " ");
        }
        args[i] = NULL;
        // Check for background execution
         int background = 0;
         if (i > 0 && strcmp(args[i - 1], "&") == 0) {
             background = 1;
         i--; // Remove '&' from arguments
         args[i] = NULL;
         }

         int numCommands = 1;
         for (int j = 0; j < i; j++) {
             if (strcmp(args[j], "|") == 0)
                 numCommands++;
             }
         if (numCommands > 1) {
             handlePipes(args, numCommands, background);
         } else {
         executeCommand(args, background);
         }
    }    

/**
 * @brief The main entry point for the Norseish shell.
 *
 * This function initializes the shell, displays a welcome message,
 * and enters the main loop for processing user commands.
 * It handles built-in commands such as 'exit', 'cd', 'history', and 'delay',
 * as well as external commands, pipes, and background execution.
 * Signal handling is set up to ignore interrupt, quit, and stop signals,
 * and to handle child process termination. A separate thread is created
 * to process delayed commands.
 *
 * @return 0 if the shell exits normally.
 */
int main() {
    titleScreen();
    char command[MAX_COMMAND_LENGTH];
    char *args[MAX_ARGS];
    char *token;
    printf("Welcome to John and Jack's Seashell.\n");
    printf("Type 'exit' to leave the shell.\n");

    // Signal handling for the shell process itself.
    signal(SIGINT, SIG_IGN); // Ignore Ctrl+C
    signal(SIGQUIT, SIG_IGN); // ignore Ctrl+backslash
    signal(SIGTSTP, SIG_IGN); // Ignore Ctrl+Z
    signal(SIGCHLD, SIG_DFL); // Clean up zombie processes

    if (pthread_create(&delayed_commands_thread, NULL, processDelayedCommands, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }

    while (1) {
        if (readLine("Norseish> ", command, sizeof(command)) <= 0) {
            printf("\n");
            break;
        }

        if (command[0] == '\0') {
            continue;
        }

        addToHistory(command);

        removeQuotes(command);

        // Tokenize the command string into arguments
        int i = 0;
        token = strtok(command, " ");
        while (token != NULL && i < MAX_ARGS - 1) {
            args[i++] = token;
            token = strtok(NULL, " ");
        }
        args[i] = NULL;

        int background = 0;
        if (i > 0 && strcmp(args[i - 1], "&") == 0) {
            background = 1;
            i--;
            args[i] = NULL;
        }

        // Handling of empty commands
        if (i == 0) {
            continue;
        }

        // Delayed commands (main logic)
        if (strcmp(args[0], "delay") == 0) {
            if (i < 3) {
                fprintf(stderr, "Usage: delay <seconds> <command>\n");
                continue;
            }
            int delay_seconds = atoi(args[1]);
            if (delay_seconds <= 0) {
                fprintf(stderr, "delay: Invalid number of seconds\n");
                continue;
            }
            
            char delayed_command[MAX_COMMAND_LENGTH];
            strcpy(delayed_command, args[2]);
            for (int j = 3; j < i; j++) {
                strcat(delayed_command, " ");
                strcat(delayed_command, args[j]);
            }
            time_t scheduled_time = time(NULL) + delay_seconds;
            addDelayedCommand(scheduled_time, delayed_command);
            continue;
        }

        // History command
        if (strcmp(args[0], "history") == 0) {
            displayHistory();
            continue;
        }

        // Expand wildcards
        char **expanded_args = NULL;
        int num_expanded_args = expandWildcards(args, &expanded_args);
        if (num_expanded_args < 0) {
            fprintf(stderr, "Error: Wildcard expansion failed.\n");
            continue;
        }

        // exit command
        if (strcmp(expanded_args[0], "exit") == 0) {
            for (int j = 0; j < num_expanded_args; j++) {
                free(expanded_args[j]);
            }
            free(expanded_args);
            if (pthread_cancel(delayed_commands_thread) != 0) {
                perror("pthread_cancel");
            }
            // Join the delayed commands thread to ensure it has terminated
            if (pthread_join(delayed_commands_thread, NULL) != 0) {
                perror("pthread_join");
            }
            pthread_mutex_destroy(&queue_mutex);
            pthread_cond_destroy(&queue_cond);
            printf("Thank you for using the shell!\n");
            break;
        }

        // cd command
        if (strcmp(expanded_args[0], "cd") == 0) {
            char *targetDir = NULL;
            char expandedPath[MAX_COMMAND_LENGTH];
            if (expanded_args[1] == NULL) {
                fprintf(stderr, "cd: missing argument\n");
                // Free allocated memory
                for (int j = 0; j < num_expanded_args; j++) {
                    free(expanded_args[j]);
                }
                free(expanded_args);
                continue; // Continue the loop, don't execute
            } else if (strcmp(expanded_args[1], "~") == 0) {
                targetDir = getenv("HOME");
                if (targetDir == NULL) {
                    fprintf(stderr, "cd: Your HOME environment is not set!\n");
                    // Free allocated memory
                    for (int j = 0; j < num_expanded_args; j++) {
                        free(expanded_args[j]);
                    }
                    free(expanded_args);
                    continue;
                }
            } else if (expanded_args[1][0] == '~') {
                char *home = getenv("HOME");
                if (home == NULL) {
                    fprintf(stderr, "cd: HOME environment variable not set\n");
                    // Free allocated memory
                    for (int j = 0; j < num_expanded_args; j++) {
                        free(expanded_args[j]);
                    }
                    free(expanded_args);
                    continue;
                } else {
                    snprintf(expandedPath, sizeof(expandedPath), "%s%s", home, expanded_args[1] + 1);
                    targetDir = expandedPath;
                }
            } else {
                targetDir = expanded_args[1];
            }
            if (targetDir != NULL) {
                cd(targetDir);
            }
            for (int j = 0; j < num_expanded_args; j++) {
                free(expanded_args[j]);
            }
            free(expanded_args);
            continue; // Go to the next iteration of the loop
        }

        int numCommands = 1;
        for (int j = 0; j < num_expanded_args; j++) {
            if (strcmp(expanded_args[j], "|") == 0)
                numCommands++;
        }

        // Handle pipes
        if (numCommands > 1) {
            handlePipes(expanded_args, numCommands, background);
            // Free allocated memory
            for (int j = 0; j < num_expanded_args; j++) {
                free(expanded_args[j]);
            }
            free(expanded_args);
            continue;
        }

        executeCommand(expanded_args, background);

        for (int j = 0; j < num_expanded_args; j++) {
            free(expanded_args[j]);
        }
        free(expanded_args);
    }

    pthread_mutex_destroy(&queue_mutex);
    pthread_cond_destroy(&queue_cond);
    return 0;
}