/* desktop.h - the applications menu on Linux (.desktop files, freedesktop.org Desktop
 * Entry Specification): find a program by its menu name, read its command. */
#ifndef TICK_DESKTOP_H
#define TICK_DESKTOP_H

typedef struct {
    char name[256];             /* Name= (what the menu shows) */
    char exec[2048];            /* Exec=, with the file's escapes undone */
    char path[1024];            /* Path= (working folder), "" if none */
    char file[1024];            /* the .desktop file */
    char id[256];               /* desktop file id: "org.vendor.Soft" (no .desktop) */
    int terminal;               /* Terminal=true */
    int hidden;                 /* NoDisplay=true or Hidden=true */
} desktop_entry;

/* text of a .desktop file -> entry; 1 for an application with an Exec line */
int desktop_parse(const char *text, desktop_entry *d);
int desktop_load(const char *file, desktop_entry *d);
/* Exec -> argument list (field codes such as %f %U removed, quoting undone);
 * NULL-terminated, malloc'ed, free with desktop_free_argv; NULL if empty or invalid */
char **desktop_argv(const desktop_entry *d);
void desktop_free_argv(char **argv);
/* a .desktop file path, a desktop file id ("vendorsoft" / "vendorsoft.desktop") or a menu
 * name (case does not matter) -> entry; 1 if found */
int desktop_find(const char *what, desktop_entry *d);
/* all applications in the menu (not hidden), sorted by name; returns how many, *out malloc'ed */
int desktop_list(desktop_entry **out);
/* the first file of a text/uri-list (what a file manager drops: file:///path or
 * file://host/path, percent-encoded) -> local path; 1 if it is a local file URI */
int uri_list_first_path(const char *list, char *out, unsigned cap);

#endif
