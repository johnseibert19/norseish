#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/*
 * Program: norseish - A custom shell program
 * This program implements a simple shell that allows users to:
 * 1. Navigate directories using the "cd" command.
 * 2. Execute external commands.
 * 3. Exit the shell gracefully using the "exit" command.
 */

// Method: cd
// Purpose: Changes the current working directory to the specified path.
// Input: A string (path) representing the directory to change to.
// Behavior: If the operation fails, an error message is displayed.

void cd(char *path) {
    if (chdir(path) != 0) {
        perror("cd");
    }
}

/*
 * Method: main
 * Purpose: Acts as the entry point for the program and manages the shell's functionality.
 *          Continuously prompts the user for input, processes built-in commands (e.g., "cd" and "exit"),
 *          and handles external commands by creating child processes.
 * Behavior:
 *  - Displays the shell prompt.
 *  - Reads and tokenizes user input into a command and its arguments.
 *  - Executes "exit" to terminate the shell or "cd" to change directories.
 *  - For other commands, forks a child process to execute the command.
 * Input: None
 * Output: Executes commands or displays error messages for invalid operations.
 */
int main() {
    char command[256];
    char *args[10];
    char *token;

    while (1) {
        printf("norseish> "); // (Nor)folk(Sei)bert(Sh)ell
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }

        command[strcspn(command, "\n")] = '\0';

        int i = 0;
        token = strtok(command, " ");
        while (token != NULL && i < 10) {
            args[i++] = token;
            token = strtok(NULL, " ");
        }
        args[i] = NULL;

        if (strcmp(args[0], "exit") == 0) {
            printf("Exiting shell. Goodbye!\n");
            break;
        }

        if (strcmp(args[0], "cd") == 0) {
            if (args[1] == NULL) {
                fprintf(stderr, "cd: missing argument\n");
            } else {
                cd(args[1]);
            }
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                execvp(args[0], args);
                perror("execvp");
                exit(1);
            } else if (pid > 0) {
                wait(NULL);
            } else {
                perror("fork");
            }
        }
    }

    return 0;
}
