#ifndef VERSION_H
#define VERSION_H

/* Individual version components */
#define HANDLER_VERSION  0
#define HANDLER_REVISION 7
#define HANDLER_BUILD    1
#define HANDLER_DATE     "16.04.2026"
#define HANDLER_NAME     "Virtio9PFS-handler"

/* Helper macros for stringification */
#define STR(x) #x
#define XSTR(x) STR(x)

/*
 * Standard AmigaOS version string: $VER: name version.revision (date)
 * Combined using string literal concatenation.
 */
#define HANDLER_VERSION_STRING \
    HANDLER_NAME " " XSTR(HANDLER_VERSION) "." XSTR(HANDLER_REVISION) " (" HANDLER_DATE ")"

/* Full logging version string including build number */
#define VERSION_LOG_STRING \
    HANDLER_NAME " " XSTR(HANDLER_VERSION) "." XSTR(HANDLER_REVISION) "." XSTR(HANDLER_BUILD) " (" HANDLER_DATE ")"

#endif /* VERSION_H */
