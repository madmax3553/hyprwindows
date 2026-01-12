# Agent Guidelines for hyprwindows

This document outlines essential information for agents working on the `hyprwindows` codebase.

## Project Overview

`hyprwindows` is a C project designed to manage window properties based on user-defined rules, primarily interacting with the Hyprland window manager. It features a command-line interface (CLI) and a Terminal User Interface (TUI). JSON files are used for defining rules and application mappings.

## Essential Commands

The project uses `make` for building.

-   **Build**:
    ```bash
    make
    ```
    Compiles the source code and creates the `hyprwindows` executable.
-   **Clean**:
    ```bash
    make clean
    ```
    Removes compiled object files and the `hyprwindows` executable.
-   **Run TUI (Default)**:
    ```bash
    ./hyprwindows
    ```
    Runs the Terminal User Interface (TUI).
-   **Run TUI (Explicit)**:
    ```bash
    ./hyprwindows --tui
    ```
    Explicitly runs the Terminal User Interface (TUI).
-   **Summarize Rules**:
    ```bash
    ./hyprwindows summarize <rules.json|jsonc>
    ```
    Loads and summarizes the defined rules from a JSON file.
-   **Scan Dotfiles**:
    ```bash
    ./hyprwindows scan-dotfiles <dotfiles_dir> <rules.json|jsonc> [appmap.json]
    ```
    Scans a specified dotfiles directory and a rules file. It identifies applications with missing rules or overlapping rules, optionally using an `appmap.json` for application mapping.
-   **Active Windows**:
    ```bash
    ./hyprwindows active <rules.json|jsonc>
    ```
    Lists active windows and checks which rules apply to them. It can also suggest new rules based on the active windows.

## Code Organization and Structure

-   `src/`: Contains the core C source (`.c`) and header (`.h`) files for the application logic.
-   `third_party/`: Houses external dependencies, such as `jsmn.c` and `jsmn.h` for JSON parsing.
-   `data/`: Stores data files, currently including `appmap.json`.
-   The codebase follows a modular design, with header files defining data structures and function prototypes, and corresponding C files providing their implementations.
-   Custom data structures like `str_list` (dynamic array of strings) and `group_list` (dynamic array of `group_entry` structs) are used for internal data management.
-   An `outbuf` utility is implemented for efficient dynamic string output.

## Naming Conventions and Style Patterns

-   **Files**: Lowercase with `.c` and `.h` extensions (e.g., `main.c`, `rules.h`).
-   **Functions and Variables**: `snake_case` (e.g., `run_tui`, `ruleset_load_json`).
-   **Macros**: `CAPITAL_SNAKE_CASE`.
-   **Structures**: `struct name_of_struct`.
-   **Internal Functions**: Functions intended for internal use within a `.c` file are typically declared `static`.
-   **Memory Management**: Manual memory management using `malloc`, `realloc`, and `free` is consistently applied.

## Testing Approach and Patterns

-   No formal unit testing framework or dedicated test suite was observed.
-   Functional testing is primarily performed by executing the `hyprwindows` executable with various command-line arguments and observing the output.

## Important Gotchas or Non-Obvious Patterns

-   **Memory Management**: Due to manual memory management, it is crucial to ensure that all dynamically allocated memory is correctly freed to prevent memory leaks. Pay close attention to `_free` functions associated with data structures (e.g., `ruleset_free`, `list_free`).
-   **JSON Parsing**: The project uses the `jsmn` library for JSON parsing. Understanding its token-based approach is important when modifying or extending JSON-related logic.
-   **Rule Matching**: Rules are defined using regular expressions for `class`, `title`, `initialClass`, `initialTitle`, and `tag` fields. When constructing or modifying rule matching logic, be aware of regex syntax and potential need for escaping special characters (handled by `escape_regex` in suggestions).
-   **Hyprland Interaction**: The `hyprctl.c` and `hyprctl.h` files are responsible for communicating with the Hyprland window manager. Changes related to window management should be carefully considered in this context.
-   **Error Handling**: Errors are typically propagated via integer return codes (-1 for failure, 0 for success) and error messages are printed to `stderr` or written to the `outbuf`.
